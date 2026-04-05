// performance_monitor.h - Performance monitoring and hitch detection for drivers
#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// Performance thresholds (in microseconds)
#define PERF_THRESHOLD_WARNING_US   1000    // 1ms - warning
#define PERF_THRESHOLD_CRITICAL_US  5000    // 5ms - critical hitch
#define PERF_THRESHOLD_HANG_US      50000   // 50ms - potential hang

// Categories to monitor
#define PERF_CATEGORY_IOCTL         0x00000001
#define PERF_CATEGORY_INPUT_INJECT 0x00000002
#define PERF_CATEGORY_HID_REPORT    0x00000004
#define PERF_CATEGORY_DPC           0x00000008
#define PERF_CATEGORY_ISR           0x00000010
#define PERF_CATEGORY_PNP           0x00000020
#define PERF_CATEGORY_POWER         0x00000040
#define PERF_CATEGORY_ALL           0xFFFFFFFF

// Performance event entry
typedef struct _PERF_EVENT {
    LARGE_INTEGER Timestamp;
    ULONG Category;
    UCHAR Level;  // 0=OK, 1=Warning, 2=Critical, 3=Hang
    ULONGLONG DurationUs;
    ULONGLONG DurationCycles;  // For precise measurement
    CHAR Operation[64];
    CHAR Function[64];
    ULONG Line;
    PVOID Context;  // Device object or context
} PERF_EVENT, *PPERF_EVENT;

// Performance statistics per category
typedef struct _PERF_STATS {
    ULONGLONG TotalCalls;
    ULONGLONG TotalDurationUs;
    ULONGLONG MinDurationUs;
    ULONGLONG MaxDurationUs;
    ULONGLONG WarningCount;
    ULONGLONG CriticalCount;
    ULONGLONG HangCount;
    ULONGLONG LastDurationUs;
    LARGE_INTEGER LastTimestamp;
} PERF_STATS, *PPERF_STATS;

#ifdef _KERNEL_MODE

// ── Kernel-mode monitor context ──────────────────────────────────────────────
typedef struct _PERF_MONITOR_CONTEXT {
    PERF_EVENT Events[256];
    ULONG WriteIndex;
    KSPIN_LOCK BufferLock;
    PERF_STATS Stats[32];
    BOOLEAN Enabled;
    ULONG ActiveCategories;
    ULONGLONG WarningThresholdUs;
    ULONGLONG CriticalThresholdUs;
    KTIMER HitchDetectionTimer;
    KDPC HitchDetectionDpc;
    BOOLEAN HitchTimerRunning;
    VOID (*LogCallback)(PPERF_EVENT Event);
} PERF_MONITOR_CONTEXT, *PPERF_MONITOR_CONTEXT;

// Kernel prototypes
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS PerfMonitorInitialize(_Out_ PPERF_MONITOR_CONTEXT Context,
    _In_ ULONG Categories, _In_ ULONGLONG WarningUs, _In_ ULONGLONG CriticalUs);
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID PerfMonitorShutdown(_In_ PPERF_MONITOR_CONTEXT Context);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PerfMonitorStart(_In_ PPERF_MONITOR_CONTEXT Context, _In_ ULONG Category,
    _In_z_ const CHAR* Op, _In_z_ const CHAR* Fn, _In_ ULONG Line, _Out_ PULONGLONG StartCycles);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PerfMonitorEnd(_In_ PPERF_MONITOR_CONTEXT Context, _In_ ULONG Category,
    _In_z_ const CHAR* Op, _In_z_ const CHAR* Fn, _In_ ULONG Line,
    _In_ ULONGLONG StartCycles, _In_opt_ PVOID DevCtx);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PerfMonitorGetStats(_In_ PPERF_MONITOR_CONTEXT Context, _In_ ULONG Category,
    _Out_ PPERF_STATS Stats);
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN PerfMonitorHasIssues(_In_ PPERF_MONITOR_CONTEXT Context, _In_ ULONG Category,
    _In_ ULONGLONG WarnCount, _Out_opt_ PPERF_STATS Worst);
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID PerfMonitorGenerateReport(_In_ PPERF_MONITOR_CONTEXT Context,
    _Out_writes_(Max) PPERF_EVENT Events, _In_ ULONG Max, _Out_ PULONG Count);
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID PerfMonitorStartHitchDetection(_In_ PPERF_MONITOR_CONTEXT Context, _In_ ULONG IntervalMs);
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID PerfMonitorStopHitchDetection(_In_ PPERF_MONITOR_CONTEXT Context);
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN PerfMonitorIsResponsive(_In_ PPERF_MONITOR_CONTEXT Context, _In_ ULONGLONG MaxUs);

#else // User-mode ─────────────────────────────────────────────────────────────

typedef struct _PERF_MONITOR_CONTEXT {
    PERF_EVENT    Events[256];
    ULONG         WriteIndex;
    CRITICAL_SECTION BufferLock;
    PERF_STATS    Stats[32];
    BOOL          Enabled;
    ULONG         ActiveCategories;
    ULONGLONG     WarningThresholdUs;
    ULONGLONG     CriticalThresholdUs;
} PERF_MONITOR_CONTEXT, *PPERF_MONITOR_CONTEXT;

// User-mode inline stubs — performance monitoring is a no-op outside the kernel.
static inline void PerfMonitorInitialize(PPERF_MONITOR_CONTEXT ctx,
    ULONG cats, ULONGLONG warnUs, ULONGLONG critUs) {
    if (!ctx) return;
    ZeroMemory(ctx, sizeof(*ctx));
    InitializeCriticalSection(&ctx->BufferLock);
    ctx->Enabled = TRUE;
    ctx->ActiveCategories = cats;
    ctx->WarningThresholdUs  = warnUs;
    ctx->CriticalThresholdUs = critUs;
}
static inline void PerfMonitorShutdown(PPERF_MONITOR_CONTEXT ctx) {
    if (ctx && ctx->Enabled) { DeleteCriticalSection(&ctx->BufferLock); ctx->Enabled = FALSE; }
}
static inline void PerfMonitorStart(PPERF_MONITOR_CONTEXT, ULONG, const char*,
    const char*, ULONG line, ULONGLONG* start) {
    if (start) QueryPerformanceCounter((LARGE_INTEGER*)start);
    (void)line;
}
static inline void PerfMonitorEnd(PPERF_MONITOR_CONTEXT, ULONG, const char*,
    const char*, ULONG, ULONGLONG, void*) {}
static inline void PerfMonitorGetStats(PPERF_MONITOR_CONTEXT, ULONG, PERF_STATS*) {}
static inline BOOL PerfMonitorHasIssues(PPERF_MONITOR_CONTEXT, ULONG, ULONGLONG, PERF_STATS*) { return FALSE; }
static inline void PerfMonitorGenerateReport(PPERF_MONITOR_CONTEXT, PERF_EVENT*, ULONG, ULONG* count) { if (count) *count = 0; }
static inline void PerfMonitorStartHitchDetection(PPERF_MONITOR_CONTEXT, ULONG) {}
static inline void PerfMonitorStopHitchDetection(PPERF_MONITOR_CONTEXT) {}
static inline BOOL PerfMonitorIsResponsive(PPERF_MONITOR_CONTEXT, ULONGLONG) { return TRUE; }

#endif // _KERNEL_MODE

// Quick macro for scoped timing (both modes — kernel uses defer, user mode fires manually)
#define PERF_MONITOR_SCOPE(ctx, cat, op) \
    ULONGLONG _perfStart; \
    PerfMonitorStart(ctx, cat, op, __FUNCTION__, __LINE__, &_perfStart);
