// unified_logger_user.cpp - User-mode logging implementation
//
// Design: lock-free ring buffer on the write hot-path + single background
// writer thread that drains to disk.  Writers never block waiting for I/O.
// Reader skips past any slot still being written (rare writer stall), so a
// slow writer cannot head-of-line-block the rest of the log.
//
// Thread-safety:
//   - minLevel_ / categories_  are std::atomic  → safe to read without lock
//   - logFile_                 protected by fileMutex_ (only writer thread touches it)
//   - ring_[] slots            atomic state field; each slot claimed by exactly one writer
//   - writeHead_               std::atomic<size_t> fetch_add; wraps modulo ring size
//
#include "unified_logger.h"
#include <stdarg.h>
#include <time.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstring>

// ── Ring buffer slot ─────────────────────────────────────────────────────────
// State machine per slot:  FREE(0) → WRITING(1) → READY(2) → FREE(0)
static constexpr size_t LF_RING_SIZE  = 8192;   // power-of-two; ~8 MB footprint
static constexpr size_t LF_MSG_SIZE   = 512;    // max chars per formatted line

struct LockFreeLogSlot {
    std::atomic<int> state{0};
    char message[LF_MSG_SIZE];
};

// ── Logger implementation ────────────────────────────────────────────────────
class UserModeLogger {
public:
    UserModeLogger()
        : running_(false)
        , logFile_(nullptr)
        , writeHead_(0)
        , readHead_(0)
        , minLevel_(LOG_LEVEL_INFO)
        , categories_(LOG_CATEGORY_ALL)
        , totalMessages_(0)
        , errorsLogged_(0)
        , warningsLogged_(0)
        , droppedMessages_(0)
    {
        for (size_t i = 0; i < LF_RING_SIZE; i++)
            ring_[i].state.store(0, std::memory_order_relaxed);
    }

    ~UserModeLogger() { Shutdown(); }

    // Open the log file and start the background writer.
    // Safe to call more than once — subsequent calls reopen the file (rotating).
    bool Initialize(const char* logFilePath, UCHAR minLevel, ULONG categories) {
        minLevel_.store(minLevel,    std::memory_order_relaxed);
        categories_.store(categories, std::memory_order_relaxed);

        if (logFilePath && *logFilePath) {
            std::lock_guard<std::mutex> lk(fileMutex_);
            if (logFile_) { fclose(logFile_); logFile_ = nullptr; }
            logFile_ = fopen(logFilePath, "a");
            if (!logFile_) {
                // Can't open file — keep running in OutputDebugString-only mode
                char err[256];
                snprintf(err, sizeof(err),
                    "[KVMLogger] WARNING: cannot open log file '%s' (errno %d). "
                    "Logging to OutputDebugString only.\n", logFilePath, errno);
                OutputDebugStringA(err);
            } else {
                // Write a session header so adjacent runs are clearly separated
                SYSTEMTIME st;
                GetLocalTime(&st);
                fprintf(logFile_,
                    "\n"
                    "==================================================================\n"
                    "KVM-Drivers service started  %04d-%02d-%02d %02d:%02d:%02d\n"
                    "==================================================================\n",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
                fflush(logFile_);
            }
        }

        // Start writer thread once
        bool alreadyRunning = running_.exchange(true);
        if (!alreadyRunning) {
            writerThread_ = std::thread(&UserModeLogger::WriterLoop, this);
        }
        return true;
    }

    // Flush all pending log entries and close the file.
    void Shutdown() {
        if (!running_.exchange(false)) return;  // already shut down
        cv_.notify_all();
        if (writerThread_.joinable()) writerThread_.join();

        std::lock_guard<std::mutex> lk(fileMutex_);
        if (logFile_) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(logFile_,
                "==================================================================\n"
                "KVM-Drivers service stopped  %04d-%02d-%02d %02d:%02d:%02d\n"
                "Dropped messages: %lld\n"
                "==================================================================\n\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond,
                (long long)droppedMessages_.load());
            fclose(logFile_);
            logFile_ = nullptr;
        }
    }

    // FlushSync — called from the crash handler to drain the ring before exit.
    void FlushSync() {
        DrainRing(true);
        std::lock_guard<std::mutex> lk(fileMutex_);
        if (logFile_) fflush(logFile_);
    }

    // ── Hot-path log write ──────────────────────────────────────────────────
    void Log(UCHAR level, ULONG category,
             const char* component, const char* function, int line,
             const char* format, ...) {

        // Fast level/category filter — both fields are atomic, no lock needed
        if (level > minLevel_.load(std::memory_order_relaxed)) return;
        if (!(category & categories_.load(std::memory_order_relaxed))) return;

        // ── Build the formatted line ────────────────────────────────────────
        // Millisecond timestamp
        SYSTEMTIME st;
        GetLocalTime(&st);

        // Thread ID for multi-thread correlation
        DWORD tid = GetCurrentThreadId();

        // Format caller's message
        char userMsg[LF_MSG_SIZE];
        va_list ap;
        va_start(ap, format);
        vsnprintf(userMsg, sizeof(userMsg), format, ap);
        va_end(ap);

        // Full line:  [date time.ms] [LEVEL] [TID] [component] [func:line] message
        char logLine[LF_MSG_SIZE + 128];
        int len = snprintf(logLine, sizeof(logLine),
            "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%-5s] [T%05lu] [%-14s] [%s:%d] %s\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            LevelToString(level),
            (unsigned long)tid,
            component,
            function,
            line,
            userMsg);
        if (len < 0 || len >= (int)sizeof(logLine))
            len = (int)sizeof(logLine) - 1;
        logLine[len] = '\0';

        // ── Update counters (atomic, no lock) ───────────────────────────────
        totalMessages_.fetch_add(1, std::memory_order_relaxed);
        if (level == LOG_LEVEL_ERROR || level == LOG_LEVEL_FATAL)
            errorsLogged_.fetch_add(1, std::memory_order_relaxed);
        else if (level == LOG_LEVEL_WARNING)
            warningsLogged_.fetch_add(1, std::memory_order_relaxed);

        // ── Claim a ring slot and write ─────────────────────────────────────
        // fetch_add wraps naturally via the bitmask on the index
        size_t idx = writeHead_.fetch_add(1, std::memory_order_relaxed) & (LF_RING_SIZE - 1);
        int expected = 0;
        if (ring_[idx].state.compare_exchange_strong(
                expected, 1,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            memcpy(ring_[idx].message, logLine, (size_t)len + 1);
            ring_[idx].state.store(2, std::memory_order_release);
            cv_.notify_one();
        } else {
            // Slot still occupied — ring full, drop this message
            droppedMessages_.fetch_add(1, std::memory_order_relaxed);
        }

#ifdef _DEBUG
        // In debug builds only: mirror to debugger output (may block if debugger busy)
        OutputDebugStringA(logLine);
#endif
    }

    void SetMinLevel(UCHAR level)  { minLevel_.store(level,    std::memory_order_relaxed); }
    void SetCategories(ULONG cats) { categories_.store(cats,   std::memory_order_relaxed); }

    void GetStats(uint64_t* total, uint64_t* errors, uint64_t* warnings, uint64_t* dropped) {
        if (total)   *total   = totalMessages_.load(std::memory_order_relaxed);
        if (errors)  *errors  = errorsLogged_.load(std::memory_order_relaxed);
        if (warnings)*warnings = warningsLogged_.load(std::memory_order_relaxed);
        if (dropped) *dropped = droppedMessages_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool>    running_;
    std::mutex           fileMutex_;   // protects logFile_ (writer thread + Shutdown/FlushSync)
    std::condition_variable cv_;
    std::thread          writerThread_;
    FILE*                logFile_;

    LockFreeLogSlot      ring_[LF_RING_SIZE];
    std::atomic<size_t>  writeHead_;
    size_t               readHead_;    // only touched by WriterLoop

    std::atomic<UCHAR>   minLevel_;
    std::atomic<ULONG>   categories_;

    std::atomic<uint64_t> totalMessages_;
    std::atomic<uint64_t> errorsLogged_;
    std::atomic<uint64_t> warningsLogged_;
    std::atomic<uint64_t> droppedMessages_;

    // ── Background writer loop ──────────────────────────────────────────────
    // Drains committed ring slots to disk.  Crucially, skips any slot that is
    // still in WRITING state rather than blocking — the slow writer will be
    // picked up on the next iteration.  This eliminates head-of-line blocking.
    void WriterLoop() {
        while (running_.load(std::memory_order_relaxed) || HasAnyReady()) {
            {
                std::unique_lock<std::mutex> lk(fileMutex_);
                // Wake up when notified or after 50 ms (for slow trickle)
                cv_.wait_for(lk, std::chrono::milliseconds(50),
                    [this] { return !running_ || HasAnyReady(); });
            }
            DrainRing(false);
        }
        // Final drain after running_ → false
        DrainRing(true);
    }

    // Drain up to one full ring's worth of ready slots.
    // If exhaustive==true, spin-wait on WRITING slots so nothing is left.
    void DrainRing(bool exhaustive) {
        int flushed = 0;
        size_t scanned = 0;

        while (scanned < LF_RING_SIZE) {
            size_t idx = readHead_ & (LF_RING_SIZE - 1);
            int s = ring_[idx].state.load(std::memory_order_acquire);

            if (s == 2) {                           // READY — drain it
                std::lock_guard<std::mutex> lk(fileMutex_);
                if (logFile_) fputs(ring_[idx].message, logFile_);
                ring_[idx].state.store(0, std::memory_order_release);
                readHead_++;
                flushed++;
                scanned = 0;  // reset: we may have lapped a WRITING slot earlier
            } else if (s == 1 && exhaustive) {      // WRITING — spin briefly
                SwitchToThread();
                // don't advance scanned so we retry this slot
            } else {
                readHead_++;                         // FREE or WRITING (non-exhaustive): skip
                scanned++;
            }
        }

        if (flushed > 0) {
            std::lock_guard<std::mutex> lk(fileMutex_);
            if (logFile_) fflush(logFile_);
        }
    }

    bool HasAnyReady() const {
        // Quick scan of a window around the read head
        for (size_t i = 0; i < 64; i++) {
            if (ring_[(readHead_ + i) & (LF_RING_SIZE - 1)].state.load(
                    std::memory_order_relaxed) == 2)
                return true;
        }
        return false;
    }

    static const char* LevelToString(UCHAR level) {
        switch (level) {
        case LOG_LEVEL_FATAL:   return "FATAL";
        case LOG_LEVEL_ERROR:   return "ERROR";
        case LOG_LEVEL_WARNING: return "WARN";
        case LOG_LEVEL_INFO:    return "INFO";
        case LOG_LEVEL_DEBUG:   return "DEBUG";
        case LOG_LEVEL_TRACE:   return "TRACE";
        default:                return "?????";
        }
    }
};

// ── Global singleton ─────────────────────────────────────────────────────────
static UserModeLogger g_UserLogger;

// ── C public API (UserLogger_* prefix, called from service.cpp / main) ───────
extern "C" {

BOOL UserLogger_Initialize(const char* logFile, int minLevel, unsigned int categories) {
    return g_UserLogger.Initialize(logFile, (UCHAR)minLevel, (ULONG)categories) ? TRUE : FALSE;
}

void UserLogger_Shutdown() {
    g_UserLogger.Shutdown();
}

void UserLogger_FlushSync() {
    g_UserLogger.FlushSync();
}

void UserLogger_Log(int level, unsigned int category, const char* component,
                    const char* function, int line, const char* format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    g_UserLogger.Log((UCHAR)level, (ULONG)category, component, function, line, "%s", message);
}

void UserLogger_SetLevel(int level) {
    g_UserLogger.SetMinLevel((UCHAR)level);
}

void UserLogger_GetStats(unsigned long long* total, unsigned long long* errors,
                         unsigned long long* warnings, unsigned long long* dropped) {
    g_UserLogger.GetStats(total, errors, warnings, dropped);
}

} // extern "C"

// ── Bridge: unified_logger.h Logger* interface → g_UserLogger ────────────────
// Callers that use LOGGER_CONTEXT* (websocket_server_async, etc.) route here.
extern "C" {

void LoggerInitialize(PLOGGER_CONTEXT /*ctx*/, UCHAR minLevel, ULONG categories) {
    // If the logger is already running (initialized with a file path by service.cpp)
    // just update level/categories; don't re-initialize (would stomp the file).
    g_UserLogger.SetMinLevel(minLevel);
    g_UserLogger.SetCategories(categories);
    // Ensure the writer thread is running even if no file was opened yet
    g_UserLogger.Initialize(nullptr, minLevel, categories);
}

void LoggerShutdown(PLOGGER_CONTEXT /*ctx*/) {
    // Only the process-level shutdown (UserLogger_Shutdown) should fully stop
    // the writer thread — component-level calls just update state.
    // No-op here so that destruction order doesn't prematurely close the log.
}

void LoggerLog(PLOGGER_CONTEXT /*ctx*/, UCHAR level, ULONG category,
               const CHAR* component, const CHAR* function, ULONG line,
               const CHAR* format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    g_UserLogger.Log(level, category, component, function, (int)line, "%s", message);
}

ULONG LoggerGetRecentEntries(PLOGGER_CONTEXT /*ctx*/, PLOG_ENTRY /*entries*/, ULONG /*max*/) {
    return 0;
}

void LoggerSetLevel(PLOGGER_CONTEXT /*ctx*/, UCHAR newLevel) {
    g_UserLogger.SetMinLevel(newLevel);
}

} // extern "C"
