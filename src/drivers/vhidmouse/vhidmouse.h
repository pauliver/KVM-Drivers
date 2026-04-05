#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>

#define POOL_TAG 'mouV'

// Mouse context structure — one instance per WDFDEVICE
typedef struct _MOUSE_CONTEXT {
    WDFDEVICE Device;
    VHFHANDLE VhfHandle;        // VHF virtual mouse source handle
    KSPIN_LOCK ButtonLock;      // Protects ButtonState under parallel dispatch
    UCHAR ButtonState;          // Current 5-button bitmask (persists across reports)
} MOUSE_CONTEXT, *PMOUSE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MOUSE_CONTEXT, vhidmouseGetContext)

// Driver callbacks
NTSTATUS vhidmouseEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
);

VOID vhidmouseEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
);

VOID vhidmouseEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
);

VOID vhidmouseEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
);

// Mouse injection functions
NTSTATUS vhidmouseInjectRelativeMove(
    _In_ PMOUSE_CONTEXT Context,
    _In_ WDFREQUEST Request
);

NTSTATUS vhidmouseInjectAbsoluteMove(
    _In_ PMOUSE_CONTEXT Context,
    _In_ WDFREQUEST Request
);

NTSTATUS vhidmouseInjectButton(
    _In_ PMOUSE_CONTEXT Context,
    _In_ WDFREQUEST Request
);

NTSTATUS vhidmouseInjectScroll(
    _In_ PMOUSE_CONTEXT Context,
    _In_ WDFREQUEST Request
);

NTSTATUS vhidmouseReset(
    _In_ PMOUSE_CONTEXT Context
);
