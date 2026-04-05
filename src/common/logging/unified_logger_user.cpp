// unified_logger_user.cpp - User-mode logging implementation
#include "unified_logger.h"
#include <stdarg.h>
#include <time.h>
#include <fstream>
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

// Lock-free ring buffer for log messages.
// Writers claim a slot via fetch_add; reader drains slots the writer has committed.
// Slot state: 0 = empty, 1 = being written, 2 = committed/readable.
struct LockFreeLogSlot {
    std::atomic<int> state{0};  // 0=empty, 1=writing, 2=ready
    char message[1024];
};

static constexpr size_t LF_RING_SIZE = 4096;  // Must be power of 2

// User-mode logger with file output and lock-free ring buffer on the hot path
class UserModeLogger {
public:
    UserModeLogger() : running_(false), logFile_(nullptr), writeHead_(0) {
        for (size_t i = 0; i < LF_RING_SIZE; i++) {
            ring_[i].state.store(0, std::memory_order_relaxed);
        }
    }
    
    ~UserModeLogger() {
        Shutdown();
    }
    
    bool Initialize(const char* logFilePath, UCHAR minLevel, ULONG categories) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        minLevel_ = minLevel;
        categories_ = categories;
        
        // Open log file
        if (logFilePath) {
            logFile_ = fopen(logFilePath, "a");
            if (!logFile_) {
                fprintf(stderr, "[Logger] Failed to open log file: %s\n", logFilePath);
                return false;
            }
            
            // Write header
            time_t now = time(nullptr);
            fprintf(logFile_, "\n========================================\n");
            fprintf(logFile_, "KVM-Drivers Log Started: %s", ctime(&now));
            fprintf(logFile_, "========================================\n\n");
            fflush(logFile_);
        }
        
        // Start background writer thread
        running_ = true;
        writerThread_ = std::thread(&UserModeLogger::WriterLoop, this);
        
        return true;
    }
    
    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        
        cv_.notify_all();
        
        if (writerThread_.joinable()) {
            writerThread_.join();
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        if (logFile_) {
            time_t now = time(nullptr);
            fprintf(logFile_, "\n========================================\n");
            fprintf(logFile_, "Log Ended: %s", ctime(&now));
            fprintf(logFile_, "========================================\n");
            fclose(logFile_);
            logFile_ = nullptr;
        }
    }
    
    void Log(UCHAR level, ULONG category, const char* component, 
             const char* function, int line, const char* format, ...) {
        // Check level/category
        if (level > minLevel_ || !(category & categories_)) {
            return;
        }
        
        // Build message
        char message[MAX_LOG_MESSAGE_LENGTH];
        va_list args;
        va_start(args, format);
        vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        
        // Get timestamp
        time_t now = time(nullptr);
        struct tm timeinfo;
        localtime_s(&timeinfo, &now);
        
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);
        
        // Format full log line
        char logLine[1024];
        snprintf(logLine, sizeof(logLine), "[%s] [%s] [%s] [%s:%d] %s\n",
            timestamp,
            LevelToString(level),
            component,
            function,
            line,
            message);
        
        // Lock-free ring buffer write: atomically claim a slot, write, then commit.
        // Falls back to drop (with counter) if ring is full rather than blocking.
        size_t slot = writeHead_.fetch_add(1, std::memory_order_relaxed) & (LF_RING_SIZE - 1);
        int expected = 0;
        if (ring_[slot].state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
            strncpy(ring_[slot].message, logLine, sizeof(ring_[slot].message) - 1);
            ring_[slot].message[sizeof(ring_[slot].message) - 1] = '\0';
            ring_[slot].state.store(2, std::memory_order_release);  // Mark ready for reader
            cv_.notify_one();
        } else {
            // Slot still held by a previous write - ring full, drop and count
            InterlockedIncrement64((LONGLONG*)&droppedMessages_);
        }
        
        // Always output to debugger immediately (lock-free, no queue needed)
        OutputDebugStringA(logLine);
    }
    
    void GetStats(uint64_t* total, uint64_t* errors, uint64_t* warnings) {
        if (total)   *total   = totalMessages_.load(std::memory_order_relaxed);
        if (errors)  *errors  = errorsLogged_.load(std::memory_order_relaxed);
        if (warnings)*warnings = warningsLogged_.load(std::memory_order_relaxed);
    }
    
    void SetMinLevel(UCHAR level) {
        std::lock_guard<std::mutex> lock(mutex_);
        minLevel_ = level;
    }

private:
    std::mutex mutex_;                    // Protects logFile_, settings
    std::condition_variable cv_;
    std::thread writerThread_;
    std::atomic<bool> running_;

    // Lock-free ring buffer
    LockFreeLogSlot ring_[LF_RING_SIZE];
    std::atomic<size_t> writeHead_;

    FILE* logFile_;
    UCHAR minLevel_;
    ULONG categories_;

    std::atomic<uint64_t> totalMessages_{0};
    std::atomic<uint64_t> errorsLogged_{0};
    std::atomic<uint64_t> warningsLogged_{0};
    volatile LONGLONG droppedMessages_ = 0;

    // Background thread drains the ring buffer to disk
    void WriterLoop() {
        size_t readHead = 0;
        while (running_ || HasPendingSlots(readHead)) {
            // Wait up to 1s for new entries
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(100), [this, readHead] {
                    return !running_ || HasPendingSlots(readHead);
                });
            }

            // Drain all committed slots
            int flushed = 0;
            for (int i = 0; i < (int)LF_RING_SIZE; i++) {
                size_t slot = readHead & (LF_RING_SIZE - 1);
                if (ring_[slot].state.load(std::memory_order_acquire) == 2) {
                    if (logFile_) {
                        fputs(ring_[slot].message, logFile_);
                    }
                    ring_[slot].state.store(0, std::memory_order_release);  // Mark free
                    readHead++;
                    flushed++;
                } else {
                    break;  // Stop at first non-ready slot to preserve ordering
                }
            }

            if (logFile_ && flushed > 0) {
                fflush(logFile_);
            }
        }
    }

    bool HasPendingSlots(size_t readHead) const {
        return ring_[readHead & (LF_RING_SIZE - 1)].state.load(std::memory_order_acquire) == 2;
    }
    
    const char* LevelToString(UCHAR level) {
        switch (level) {
        case LOG_LEVEL_FATAL:   return "FATAL";
        case LOG_LEVEL_ERROR:   return "ERROR";
        case LOG_LEVEL_WARNING: return "WARN";
        case LOG_LEVEL_INFO:    return "INFO";
        case LOG_LEVEL_DEBUG:   return "DEBUG";
        case LOG_LEVEL_TRACE:   return "TRACE";
        default:                return "UNKNOWN";
        }
    }
};

// Global logger instance
static UserModeLogger g_UserLogger;

// C interface functions
extern "C" {

__declspec(dllexport) BOOL UserLogger_Initialize(const char* logFile, int minLevel, unsigned int categories) {
    return g_UserLogger.Initialize(logFile, (UCHAR)minLevel, (ULONG)categories);
}

__declspec(dllexport) void UserLogger_Shutdown() {
    g_UserLogger.Shutdown();
}

__declspec(dllexport) void UserLogger_Log(int level, unsigned int category, const char* component,
                                          const char* function, int line, const char* format, ...) {
    char message[MAX_LOG_MESSAGE_LENGTH];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    g_UserLogger.Log((UCHAR)level, (ULONG)category, component, function, line, "%s", message);
}

__declspec(dllexport) void UserLogger_SetLevel(int level) {
    g_UserLogger.SetMinLevel((UCHAR)level);
}

__declspec(dllexport) void UserLogger_GetStats(unsigned long long* total, unsigned long long* errors, 
                                                unsigned long long* warnings) {
    g_UserLogger.GetStats(total, errors, warnings);
}

} // extern "C"

// ── Bridge: unified_logger.h interface → UserModeLogger ──────────────────────
// These satisfy callers of LoggerInitialize/LoggerLog/LoggerShutdown.
extern "C" {

void LoggerInitialize(PLOGGER_CONTEXT /*ctx*/, UCHAR minLevel, ULONG categories) {
    g_UserLogger.Initialize(nullptr, minLevel, categories);
}

void LoggerShutdown(PLOGGER_CONTEXT /*ctx*/) {
    g_UserLogger.Shutdown();
}

void LoggerLog(PLOGGER_CONTEXT /*ctx*/, UCHAR level, ULONG /*category*/,
               const CHAR* component, const CHAR* function, ULONG line,
               const CHAR* format, ...) {
    char message[MAX_LOG_MESSAGE_LENGTH];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    g_UserLogger.Log(level, 0, component, function, (int)line, "%s", message);
}

ULONG LoggerGetRecentEntries(PLOGGER_CONTEXT /*ctx*/, PLOG_ENTRY /*entries*/, ULONG /*max*/) {
    return 0;
}

void LoggerSetLevel(PLOGGER_CONTEXT /*ctx*/, UCHAR newLevel) {
    g_UserLogger.SetMinLevel(newLevel);
}

} // extern "C"
