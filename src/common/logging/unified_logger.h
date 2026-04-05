// unified_logger.h - Cross-platform logging interface for kernel and user-mode
// Provides consistent logging across all components with ETW integration

#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#else
#include <windows.h>
#include <stdio.h>
#endif

// Log levels
#define LOG_LEVEL_FATAL       0
#define LOG_LEVEL_ERROR       1
#define LOG_LEVEL_WARNING     2
#define LOG_LEVEL_INFO        3
#define LOG_LEVEL_DEBUG       4
#define LOG_LEVEL_TRACE       5

// Log categories
#define LOG_CATEGORY_GENERAL    0x00000001
#define LOG_CATEGORY_DRIVER     0x00000002
#define LOG_CATEGORY_IO         0x00000004
#define LOG_CATEGORY_MEMORY     0x00000008
#define LOG_CATEGORY_NETWORK    0x00000010
#define LOG_CATEGORY_SECURITY   0x00000020
#define LOG_CATEGORY_PERFORMANCE 0x00000040
#define LOG_CATEGORY_ALL        0xFFFFFFFF

// Pool tags for memory tracking
#define LOGGER_POOL_TAG 'golK'
#define LOGGER_MSG_TAG  'gslK'

// Maximum log message length
#define MAX_LOG_MESSAGE_LENGTH 512
#define MAX_LOG_BUFFER_ENTRIES 1024

// Log entry structure
typedef struct _LOG_ENTRY {
    LARGE_INTEGER Timestamp;
    ULONG ThreadId;
    ULONG ProcessId;
    UCHAR Level;
    ULONG Category;
    CHAR Component[32];
    CHAR Function[64];
    ULONG Line;
    CHAR Message[MAX_LOG_MESSAGE_LENGTH];
} LOG_ENTRY, *PLOG_ENTRY;

// Logger context
typedef struct _LOGGER_CONTEXT {
    // Lock-free ring buffer: each writer atomically claims a slot via InterlockedIncrement.
    // No spinlock needed - writers get unique slots, readers snapshot WriteIndex first.
    LOG_ENTRY Buffer[MAX_LOG_BUFFER_ENTRIES];
    volatile LONG WriteIndex;   // Claimed atomically; slot index = (WriteIndex-1) % SIZE
    ULONG ReadIndex;            // Only used by single-threaded readers
    
    // Settings
    UCHAR MinLevel;
    ULONG ActiveCategories;
    BOOLEAN LogToDebugger;
    BOOLEAN LogToFile;
    
    // File logging (user-mode only)
    #ifndef _KERNEL_MODE
    HANDLE LogFileHandle;
    #endif
    
    // Statistics
    ULONG64 TotalMessagesLogged;
    ULONG64 MessagesDropped;
    ULONG64 ErrorsLogged;
    ULONG64 WarningsLogged;
} LOGGER_CONTEXT, *PLOGGER_CONTEXT;

// Function prototypes
#ifdef __cplusplus
extern "C" {
#endif

#ifdef _KERNEL_MODE
// ── Kernel-mode prototypes (with SAL/IRQL annotations) ───────────────────────
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS LoggerInitialize(
    _In_ PLOGGER_CONTEXT Context,
    _In_ UCHAR MinLevel,
    _In_ ULONG Categories
);
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID LoggerShutdown(_In_ PLOGGER_CONTEXT Context);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID LoggerLog(
    _In_ PLOGGER_CONTEXT Context,
    _In_ UCHAR Level,
    _In_ ULONG Category,
    _In_z_ const CHAR* Component,
    _In_z_ const CHAR* Function,
    _In_ ULONG Line,
    _In_z_ const CHAR* Format,
    ...
);
_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG LoggerGetRecentEntries(
    _In_ PLOGGER_CONTEXT Context,
    _Out_writes_(MaxEntries) PLOG_ENTRY Entries,
    _In_ ULONG MaxEntries
);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID LoggerSetLevel(_In_ PLOGGER_CONTEXT Context, _In_ UCHAR NewLevel);
#else
// ── User-mode Logger*  (context-based, bridges to global singleton) ───────────
void LoggerInitialize(PLOGGER_CONTEXT Context, UCHAR MinLevel, ULONG Categories);
void LoggerShutdown(PLOGGER_CONTEXT Context);
void LoggerLog(PLOGGER_CONTEXT Context, UCHAR Level, ULONG Category,
    const CHAR* Component, const CHAR* Function, ULONG Line,
    const CHAR* Format, ...);
ULONG LoggerGetRecentEntries(PLOGGER_CONTEXT Context, PLOG_ENTRY Entries, ULONG MaxEntries);
void LoggerSetLevel(PLOGGER_CONTEXT Context, UCHAR NewLevel);

#ifdef __cplusplus
extern "C" {
#endif
// ── UserLogger_*  process-level API (call from service.cpp / main) ────────────
// UserLogger_Initialize MUST be called at process start with a real file path.
// All other logging (LOG_INFO, LoggerLog, etc.) routes to the same global log.
BOOL UserLogger_Initialize(const char* logFile, int minLevel, unsigned int categories);
void UserLogger_Shutdown(void);
void UserLogger_FlushSync(void);   // safe to call from crash / SEH handler
void UserLogger_SetLevel(int level);
void UserLogger_GetStats(unsigned long long* total, unsigned long long* errors,
                         unsigned long long* warnings, unsigned long long* dropped);
void UserLogger_Log(int level, unsigned int category, const char* component,
                    const char* function, int line, const char* format, ...);
#ifdef __cplusplus
}
#endif

// ── KVM_LOG_* macros — no context needed, use global singleton ────────────────
#define KVM_LOG_FATAL(comp, fmt, ...)  UserLogger_Log(LOG_LEVEL_FATAL,   LOG_CATEGORY_ALL, comp, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define KVM_LOG_ERROR(comp, fmt, ...)  UserLogger_Log(LOG_LEVEL_ERROR,   LOG_CATEGORY_ALL, comp, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define KVM_LOG_WARN(comp,  fmt, ...)  UserLogger_Log(LOG_LEVEL_WARNING, LOG_CATEGORY_ALL, comp, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define KVM_LOG_INFO(comp,  fmt, ...)  UserLogger_Log(LOG_LEVEL_INFO,    LOG_CATEGORY_ALL, comp, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define KVM_LOG_DEBUG(comp, fmt, ...)  UserLogger_Log(LOG_LEVEL_DEBUG,   LOG_CATEGORY_ALL, comp, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define KVM_LOG_TRACE(comp, fmt, ...)  UserLogger_Log(LOG_LEVEL_TRACE,   LOG_CATEGORY_ALL, comp, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)

#endif // !_KERNEL_MODE

// Convenience macros
#define LOG_FATAL(logger, cat, comp, fmt, ...) \
    LoggerLog(logger, LOG_LEVEL_FATAL, cat, comp, __FUNCTION__, __LINE__, fmt, __VA_ARGS__)

#define LOG_ERROR(logger, cat, comp, fmt, ...) \
    LoggerLog(logger, LOG_LEVEL_ERROR, cat, comp, __FUNCTION__, __LINE__, fmt, __VA_ARGS__)

#define LOG_WARNING(logger, cat, comp, fmt, ...) \
    LoggerLog(logger, LOG_LEVEL_WARNING, cat, comp, __FUNCTION__, __LINE__, fmt, __VA_ARGS__)

#define LOG_INFO(logger, cat, comp, fmt, ...) \
    LoggerLog(logger, LOG_LEVEL_INFO, cat, comp, __FUNCTION__, __LINE__, fmt, __VA_ARGS__)

#define LOG_DEBUG(logger, cat, comp, fmt, ...) \
    LoggerLog(logger, LOG_LEVEL_DEBUG, cat, comp, __FUNCTION__, __LINE__, fmt, __VA_ARGS__)

#define LOG_TRACE(logger, cat, comp, fmt, ...) \
    LoggerLog(logger, LOG_LEVEL_TRACE, cat, comp, __FUNCTION__, __LINE__, fmt, __VA_ARGS__)

// Memory tracking functions
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID LoggerTrackAllocation(
    _In_ PLOGGER_CONTEXT Context,
    _In_ PVOID Address,
    _In_ SIZE_T Size,
    _In_ ULONG PoolTag,
    _In_z_ const CHAR* Function,
    _In_ ULONG Line
);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID LoggerTrackFree(
    _In_ PLOGGER_CONTEXT Context,
    _In_ PVOID Address,
    _In_ ULONG PoolTag
);

// Get memory statistics
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID LoggerGetMemoryStats(
    _In_ PLOGGER_CONTEXT Context,
    _Out_ PULONG64 TotalAllocated,
    _Out_ PULONG64 TotalFreed,
    _Out_ PULONG64 CurrentAllocated,
    _Out_ PULONG AllocationCount
);

#ifdef __cplusplus
}
#endif
