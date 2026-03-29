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

// User-mode logger with file output and thread-safe queue
class UserModeLogger {
public:
    UserModeLogger() : running_(false), logFile_(nullptr) {}
    
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
        
        // Add to queue
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (logQueue_.size() < 10000) {  // Prevent unbounded growth
                logQueue_.push(logLine);
            }
        }
        
        cv_.notify_one();
        
        // Also output to debugger/console
        OutputDebugStringA(logLine);
        printf("%s", logLine);
    }
    
    void GetStats(uint64_t* total, uint64_t* errors, uint64_t* warnings) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (total) *total = totalMessages_;
        if (errors) *errors = errorsLogged_;
        if (warnings) *warnings = warningsLogged_;
    }
    
    void SetMinLevel(UCHAR level) {
        std::lock_guard<std::mutex> lock(mutex_);
        minLevel_ = level;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::string> logQueue_;
    std::thread writerThread_;
    std::atomic<bool> running_;
    
    FILE* logFile_;
    UCHAR minLevel_;
    ULONG categories_;
    
    uint64_t totalMessages_ = 0;
    uint64_t errorsLogged_ = 0;
    uint64_t warningsLogged_ = 0;
    
    void WriterLoop() {
        while (running_) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this] { 
                return !logQueue_.empty() || !running_; 
            });
            
            // Process queue
            while (!logQueue_.empty()) {
                std::string msg = logQueue_.front();
                logQueue_.pop();
                
                if (logFile_) {
                    fprintf(logFile_, "%s", msg.c_str());
                }
            }
            
            if (logFile_) {
                fflush(logFile_);
            }
        }
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
