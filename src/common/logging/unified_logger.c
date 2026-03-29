// unified_logger.c - Unified logging implementation for kernel mode
#include "unified_logger.h"

// Static logger instance for drivers
static PLOGGER_CONTEXT g_DriverLogger = NULL;

// Forward declarations
static VOID LoggerWriteToDebugger(PLOG_ENTRY Entry);
static VOID LoggerWriteToEtw(PLOG_ENTRY Entry);
static const CHAR* GetLevelString(UCHAR Level);
static const CHAR* GetCategoryString(ULONG Category);

// Initialize logger
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS LoggerInitialize(
    _In_ PLOGGER_CONTEXT Context,
    _In_ UCHAR MinLevel,
    _In_ ULONG Categories
)
{
    if (!Context) {
        return STATUS_INVALID_PARAMETER;
    }
    
    RtlZeroMemory(Context, sizeof(LOGGER_CONTEXT));
    
    Context->MinLevel = MinLevel;
    Context->ActiveCategories = Categories;
    Context->LogToDebugger = TRUE;
    Context->LogToFile = FALSE;  // File logging not supported in kernel
    
    KeInitializeSpinLock(&Context->BufferLock);
    
    // Store as global for driver use
    g_DriverLogger = Context;
    
    LOG_INFO(Context, LOG_CATEGORY_GENERAL, "Logger", "Unified logger initialized (level=%d)", MinLevel);
    
    return STATUS_SUCCESS;
}

// Shutdown logger
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID LoggerShutdown(
    _In_ PLOGGER_CONTEXT Context
)
{
    if (!Context) {
        return;
    }
    
    LOG_INFO(Context, LOG_CATEGORY_GENERAL, "Logger", "Logger shutting down. Stats: total=%llu, errors=%llu, warnings=%llu",
        Context->TotalMessagesLogged,
        Context->ErrorsLogged,
        Context->WarningsLogged);
    
    g_DriverLogger = NULL;
}

// Core logging function
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
)
{
    if (!Context) {
        return;
    }
    
    // Check level and category
    if (Level > Context->MinLevel) {
        return;
    }
    
    if (!(Category & Context->ActiveCategories)) {
        return;
    }
    
    // Build log entry
    LOG_ENTRY entry;
    RtlZeroMemory(&entry, sizeof(entry));
    
    KeQuerySystemTime(&entry.Timestamp);
    entry.ThreadId = (ULONG)(ULONG_PTR)PsGetCurrentThreadId();
    entry.ProcessId = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    entry.Level = Level;
    entry.Category = Category;
    
    // Copy strings safely
    if (Component) {
        strncpy(entry.Component, Component, sizeof(entry.Component) - 1);
    }
    if (Function) {
        strncpy(entry.Function, Function, sizeof(entry.Function) - 1);
    }
    entry.Line = Line;
    
    // Format message
    va_list args;
    va_start(args, Format);
    vsnprintf(entry.Message, sizeof(entry.Message) - 1, Format, args);
    va_end(args);
    
    // Update statistics
    InterlockedIncrement64((LONG64*)&Context->TotalMessagesLogged);
    if (Level == LOG_LEVEL_ERROR) {
        InterlockedIncrement64((LONG64*)&Context->ErrorsLogged);
    } else if (Level == LOG_LEVEL_WARNING) {
        InterlockedIncrement64((LONG64*)&Context->WarningsLogged);
    }
    
    // Write to ring buffer
    KIRQL oldIrql;
    KeAcquireSpinLock(&Context->BufferLock, &oldIrql);
    
    ULONG index = InterlockedIncrement((LONG*)&Context->WriteIndex) % MAX_LOG_BUFFER_ENTRIES;
    RtlCopyMemory(&Context->Buffer[index], &entry, sizeof(LOG_ENTRY));
    
    KeReleaseSpinLock(&Context->BufferLock, oldIrql);
    
    // Output to debugger
    if (Context->LogToDebugger) {
        LoggerWriteToDebugger(&entry);
    }
    
    // Output to ETW (for advanced analysis)
    LoggerWriteToEtw(&entry);
}

// Write to kernel debugger
static VOID LoggerWriteToDebugger(PLOG_ENTRY Entry)
{
    if (!Entry) {
        return;
    }
    
    // Format: [TIMESTAMP] [LEVEL] [COMPONENT] Function: Message
    KdPrint(("[%s] [%s] [%s] %s:%d %s\n",
        GetLevelString(Entry->Level),
        GetCategoryString(Entry->Category),
        Entry->Component,
        Entry->Function,
        Entry->Line,
        Entry->Message));
}

// Write to ETW (placeholder for actual ETW implementation)
static VOID LoggerWriteToEtw(PLOG_ENTRY Entry)
{
    // TODO: Implement ETW event writing
    // This would use TraceEvents() macros or EtwWrite()
    UNREFERENCED_PARAMETER(Entry);
}

// Get recent log entries
_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG LoggerGetRecentEntries(
    _In_ PLOGGER_CONTEXT Context,
    _Out_writes_(MaxEntries) PLOG_ENTRY Entries,
    _In_ ULONG MaxEntries
)
{
    if (!Context || !Entries || MaxEntries == 0) {
        return 0;
    }
    
    KIRQL oldIrql;
    KeAcquireSpinLock(&Context->BufferLock, &oldIrql);
    
    ULONG count = 0;
    ULONG current = Context->WriteIndex;
    
    while (count < MaxEntries && count < MAX_LOG_BUFFER_ENTRIES) {
        ULONG index = (current + MAX_LOG_BUFFER_ENTRIES - count - 1) % MAX_LOG_BUFFER_ENTRIES;
        
        if (Context->Buffer[index].Timestamp.QuadPart != 0) {
            RtlCopyMemory(&Entries[count], &Context->Buffer[index], sizeof(LOG_ENTRY));
            count++;
        } else {
            break;  // Empty entry
        }
    }
    
    KeReleaseSpinLock(&Context->BufferLock, oldIrql);
    
    return count;
}

// Set log level
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID LoggerSetLevel(
    _In_ PLOGGER_CONTEXT Context,
    _In_ UCHAR NewLevel
)
{
    if (Context) {
        Context->MinLevel = NewLevel;
    }
}

// Memory tracking - allocation
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID LoggerTrackAllocation(
    _In_ PLOGGER_CONTEXT Context,
    _In_ PVOID Address,
    _In_ SIZE_T Size,
    _In_ ULONG PoolTag,
    _In_z_ const CHAR* Function,
    _In_ ULONG Line
)
{
    if (!Context || !Address) {
        return;
    }
    
    LOG_DEBUG(Context, LOG_CATEGORY_MEMORY, "MemTrack",
        "ALLOC: addr=%p size=%zu tag=%c%c%c%c func=%s:%d",
        Address,
        Size,
        (CHAR)(PoolTag & 0xFF),
        (CHAR)((PoolTag >> 8) & 0xFF),
        (CHAR)((PoolTag >> 16) & 0xFF),
        (CHAR)((PoolTag >> 24) & 0xFF),
        Function,
        Line);
}

// Memory tracking - free
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID LoggerTrackFree(
    _In_ PLOGGER_CONTEXT Context,
    _In_ PVOID Address,
    _In_ ULONG PoolTag
)
{
    if (!Context || !Address) {
        return;
    }
    
    LOG_DEBUG(Context, LOG_CATEGORY_MEMORY, "MemTrack",
        "FREE: addr=%p tag=%c%c%c%c",
        Address,
        (CHAR)(PoolTag & 0xFF),
        (CHAR)((PoolTag >> 8) & 0xFF),
        (CHAR)((PoolTag >> 16) & 0xFF),
        (CHAR)((PoolTag >> 24) & 0xFF));
}

// Get level string
static const CHAR* GetLevelString(UCHAR Level)
{
    switch (Level) {
    case LOG_LEVEL_FATAL:   return "FATAL";
    case LOG_LEVEL_ERROR:   return "ERROR";
    case LOG_LEVEL_WARNING: return "WARN";
    case LOG_LEVEL_INFO:    return "INFO";
    case LOG_LEVEL_DEBUG:   return "DEBUG";
    case LOG_LEVEL_TRACE:   return "TRACE";
    default:                return "UNKNOWN";
    }
}

// Get category string
static const CHAR* GetCategoryString(ULONG Category)
{
    switch (Category) {
    case LOG_CATEGORY_GENERAL:     return "GENERAL";
    case LOG_CATEGORY_DRIVER:      return "DRIVER";
    case LOG_CATEGORY_IO:          return "IO";
    case LOG_CATEGORY_MEMORY:      return "MEMORY";
    case LOG_CATEGORY_NETWORK:     return "NETWORK";
    case LOG_CATEGORY_SECURITY:    return "SECURITY";
    case LOG_CATEGORY_PERFORMANCE: return "PERF";
    default:                       return "OTHER";
    }
}

// Get statistics
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID LoggerGetStats(
    _In_ PLOGGER_CONTEXT Context,
    _Out_opt_ PULONG64 TotalMessages,
    _Out_opt_ PULONG64 Errors,
    _Out_opt_ PULONG64 Warnings,
    _Out_opt_ PULONG64 Dropped
)
{
    if (!Context) {
        return;
    }
    
    if (TotalMessages) {
        *TotalMessages = Context->TotalMessagesLogged;
    }
    if (Errors) {
        *Errors = Context->ErrorsLogged;
    }
    if (Warnings) {
        *Warnings = Context->WarningsLogged;
    }
    if (Dropped) {
        *Dropped = Context->MessagesDropped;
    }
}
