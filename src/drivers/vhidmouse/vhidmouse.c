/*
 * Virtual HID Mouse Driver (vhidmouse.sys)
 *
 * VHF-based standalone virtual HID mouse.  Each IOCTL builds an 8-byte HID
 * input report and submits it via VhfReadReportSubmit — identical to the
 * vhidkb.c keyboard approach.
 *
 * Report layout (matches G_MouseReportDescriptor below):
 *   Byte 0   : buttons (5 bits) + 3 padding bits
 *   Bytes 1-2: X delta (16-bit signed LE)
 *   Bytes 3-4: Y delta (16-bit signed LE)
 *   Byte 5   : vertical wheel (8-bit signed)
 *   Byte 6   : horizontal pan (8-bit signed)  [7 bytes total]
 *
 * IOCTL_VMOUSE_MOVE_ABSOLUTE returns STATUS_NOT_SUPPORTED so user-mode
 * driver_interface.cpp falls through to SendInput (MOUSEEVENTF_ABSOLUTE).
 */

#include "vhidmouse.h"
#include "vhidmouse_ioctl.h"

#pragma comment(lib, "vhfkm.lib")

// HID report descriptor: relative 5-button mouse with V/H scroll
static const UCHAR s_MouseDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)

    // 5 buttons
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Min (1)
    0x29, 0x05,        //     Usage Max (5)
    0x15, 0x00,        //     Logical Min (0)
    0x25, 0x01,        //     Logical Max (1)
    0x95, 0x05,        //     Report Count (5)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data, Var, Abs)
    // 3 padding bits
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x03,        //     Report Size (3)
    0x81, 0x03,        //     Input (Const, Var, Abs)

    // X, Y relative axes (16-bit signed)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x16, 0x01, 0x80,  //     Logical Min (-32767)
    0x26, 0xFF, 0x7F,  //     Logical Max ( 32767)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x06,        //     Input (Data, Var, Rel)

    // Vertical wheel (8-bit signed)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Min (-127)
    0x25, 0x7F,        //     Logical Max ( 127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data, Var, Rel)

    // Horizontal pan (8-bit signed, AC Pan)
    0x05, 0x0C,        //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,  //     Usage (AC Pan)
    0x15, 0x81,        //     Logical Min (-127)
    0x25, 0x7F,        //     Logical Max ( 127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data, Var, Rel)

    0xC0,              //   End Collection
    0xC0               // End Collection
};

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, vhidmouseEvtDeviceAdd)
#pragma alloc_text (PAGE, vhidmouseEvtDriverContextCleanup)
#endif

// ── Submit a 7-byte mouse HID report via VHF ─────────────────────────────────
static NTSTATUS vhidmouseSubmitReport(
    _In_ PMOUSE_CONTEXT Ctx,
    _In_ SHORT X,
    _In_ SHORT Y,
    _In_ CHAR  Wheel,
    _In_ CHAR  Pan
)
{
    if (!Ctx || !Ctx->VhfHandle) return STATUS_DEVICE_NOT_READY;

    UCHAR report[7];
    report[0] = Ctx->ButtonState & 0x1F;  // 5 buttons (lower 5 bits)
    report[1] = (UCHAR)( X & 0xFF);
    report[2] = (UCHAR)((X >> 8) & 0xFF);
    report[3] = (UCHAR)( Y & 0xFF);
    report[4] = (UCHAR)((Y >> 8) & 0xFF);
    report[5] = (UCHAR)Wheel;
    report[6] = (UCHAR)Pan;

    HID_XFER_PACKET pkt;
    pkt.reportBuffer    = report;
    pkt.reportBufferLen = sizeof(report);
    pkt.reportId        = 0;

    NTSTATUS st = VhfReadReportSubmit(Ctx->VhfHandle, &pkt);
    if (!NT_SUCCESS(st)) {
        KdPrint(("vhidmouse: VhfReadReportSubmit failed 0x%x\n", st));
    }
    return st;
}

// ── Device cleanup: release VHF handle ───────────────────────────────────────
static VOID vhidmouseEvtDeviceContextCleanup(_In_ WDFOBJECT Object)
{
    PMOUSE_CONTEXT ctx = vhidmouseGetContext((WDFDEVICE)Object);
    if (ctx && ctx->VhfHandle) {
        VhfDelete(ctx->VhfHandle, TRUE);
        ctx->VhfHandle = NULL;
        KdPrint(("vhidmouse: VHF handle released\n"));
    }
}

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;

    WPP_INIT_TRACING(DriverObject, RegistryPath);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = vhidmouseEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config, vhidmouseEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidmouse: WdfDriverCreate failed 0x%x\n", status));
        WPP_CLEANUP(DriverObject);
        return status;
    }

    KdPrint(("vhidmouse: Driver loaded\n"));
    return STATUS_SUCCESS;
}

NTSTATUS vhidmouseEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    NTSTATUS status;
    WDFDEVICE device;
    PMOUSE_CONTEXT ctx;
    WDF_IO_QUEUE_CONFIG queueConfig;

    // Stand-alone VHF source: NOT a filter
    WDF_OBJECT_ATTRIBUTES devAttr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttr, MOUSE_CONTEXT);
    devAttr.EvtCleanupCallback = vhidmouseEvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &devAttr, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidmouse: WdfDeviceCreate failed 0x%x\n", status));
        return status;
    }

    ctx = vhidmouseGetContext(device);
    RtlZeroMemory(ctx, sizeof(MOUSE_CONTEXT));
    ctx->Device = device;
    KeInitializeSpinLock(&ctx->ButtonLock);

    // Default I/O queue (parallel)
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = vhidmouseEvtIoDeviceControl;
    queueConfig.EvtIoStop          = vhidmouseEvtIoStop;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidmouse: WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    // Create VHF virtual mouse
    VHF_CONFIG vhfCfg;
    VHF_CONFIG_INIT(&vhfCfg,
        WdfDeviceWdmGetDeviceObject(device),
        sizeof(s_MouseDescriptor),
        s_MouseDescriptor);
    vhfCfg.HidDeviceAttributes.Size         = sizeof(HID_DEVICE_ATTRIBUTES);
    vhfCfg.HidDeviceAttributes.VendorID      = 0x045E;  // Microsoft
    vhfCfg.HidDeviceAttributes.ProductID     = 0x0750;  // KVM virtual mouse
    vhfCfg.HidDeviceAttributes.VersionNumber = 0x0100;

    status = VhfCreate(&vhfCfg, &ctx->VhfHandle);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidmouse: VhfCreate failed 0x%x\n", status));
        return status;
    }

    status = VhfStart(ctx->VhfHandle);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidmouse: VhfStart failed 0x%x\n", status));
        VhfDelete(ctx->VhfHandle, TRUE);
        ctx->VhfHandle = NULL;
        return status;
    }

    // Expose \\.\vhidmouse for user-mode CreateFile
    {
        DECLARE_CONST_UNICODE_STRING(symLink, L"\\DosDevices\\vhidmouse");
        NTSTATUS slStatus = WdfDeviceCreateSymbolicLink(device, &symLink);
        if (!NT_SUCCESS(slStatus)) {
            KdPrint(("vhidmouse: WdfDeviceCreateSymbolicLink failed 0x%x (non-fatal)\n", slStatus));
        }
    }

    KdPrint(("vhidmouse: VHF virtual mouse started\n"));
    return STATUS_SUCCESS;
}

VOID vhidmouseEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);

    NTSTATUS status;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PMOUSE_CONTEXT ctx = vhidmouseGetContext(device);

    KdPrint(("vhidmouse: IOCTL 0x%X inLen=%zu\n", IoControlCode, InputBufferLength));

    switch (IoControlCode) {

    case IOCTL_VMOUSE_MOVE_RELATIVE: {
        PVMOUSE_MOVE_DATA data;
        size_t sz;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(VMOUSE_MOVE_DATA), (PVOID*)&data, &sz);
        if (NT_SUCCESS(status)) {
            status = vhidmouseSubmitReport(ctx, (SHORT)data->X, (SHORT)data->Y, 0, 0);
            KdPrint(("vhidmouse: RelMove(%d,%d) %s\n", data->X, data->Y, NT_SUCCESS(status) ? "OK" : "FAIL"));
        }
        break;
    }

    case IOCTL_VMOUSE_MOVE_ABSOLUTE:
        // The HID descriptor uses relative axes; absolute positioning is handled
        // by the user-mode SendInput fallback (MOUSEEVENTF_ABSOLUTE).
        status = STATUS_NOT_SUPPORTED;
        break;

    case IOCTL_VMOUSE_BUTTON: {
        PVMOUSE_BUTTON_DATA data;
        size_t sz;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(VMOUSE_BUTTON_DATA), (PVOID*)&data, &sz);
        if (NT_SUCCESS(status)) {
            UCHAR mask = 0;
            switch (data->Button) {
            case VMOUSE_BUTTON_LEFT:   mask = 0x01; break;
            case VMOUSE_BUTTON_RIGHT:  mask = 0x02; break;
            case VMOUSE_BUTTON_MIDDLE: mask = 0x04; break;
            case VMOUSE_BUTTON_X1:     mask = 0x08; break;
            case VMOUSE_BUTTON_X2:     mask = 0x10; break;
            default:                   break;
            }
            if (mask) {
                KIRQL oldIrql;
                KeAcquireSpinLock(&ctx->ButtonLock, &oldIrql);
                if (data->Pressed) ctx->ButtonState |=  mask;
                else               ctx->ButtonState &= ~mask;
                KeReleaseSpinLock(&ctx->ButtonLock, oldIrql);
            }
            // Send a zero-delta report so the button change is delivered
            status = vhidmouseSubmitReport(ctx, 0, 0, 0, 0);
            KdPrint(("vhidmouse: Button %d %s state=0x%02x %s\n",
                data->Button, data->Pressed ? "down" : "up", ctx->ButtonState,
                NT_SUCCESS(status) ? "OK" : "FAIL"));
        }
        break;
    }

    case IOCTL_VMOUSE_SCROLL: {
        PVMOUSE_SCROLL_DATA data;
        size_t sz;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(VMOUSE_SCROLL_DATA), (PVOID*)&data, &sz);
        if (NT_SUCCESS(status)) {
            CHAR vWheel = (data->Vertical   >  127) ?  127 :
                          (data->Vertical   < -127) ? -127 : (CHAR)data->Vertical;
            CHAR hPan   = (data->Horizontal >  127) ?  127 :
                          (data->Horizontal < -127) ? -127 : (CHAR)data->Horizontal;
            status = vhidmouseSubmitReport(ctx, 0, 0, vWheel, hPan);
            KdPrint(("vhidmouse: Scroll V=%d H=%d %s\n", data->Vertical, data->Horizontal,
                NT_SUCCESS(status) ? "OK" : "FAIL"));
        }
        break;
    }

    case IOCTL_VMOUSE_RESET: {
        KIRQL oldIrql;
        KeAcquireSpinLock(&ctx->ButtonLock, &oldIrql);
        ctx->ButtonState = 0;
        KeReleaseSpinLock(&ctx->ButtonLock, oldIrql);
        status = vhidmouseSubmitReport(ctx, 0, 0, 0, 0);
        KdPrint(("vhidmouse: Reset %s\n", NT_SUCCESS(status) ? "OK" : "FAIL"));
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}

VOID vhidmouseEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
{
    UNREFERENCED_PARAMETER(Queue);
    if (ActionFlags & WdfRequestStopActionSuspend) {
        WdfRequestStopAcknowledge(Request, FALSE);
    } else if (ActionFlags & WdfRequestStopActionPurge) {
        WdfRequestCancelSentRequest(Request);
    }
}

VOID vhidmouseEvtDriverContextCleanup(_In_ WDFOBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    PAGED_CODE();
    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)DriverObject));
}

// Legacy stubs kept for header compatibility
NTSTATUS vhidmouseInjectRelativeMove(_In_ PMOUSE_CONTEXT C, _In_ WDFREQUEST R) {
    UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(R);
    return STATUS_NOT_IMPLEMENTED;
}
NTSTATUS vhidmouseInjectAbsoluteMove(_In_ PMOUSE_CONTEXT C, _In_ WDFREQUEST R) {
    UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(R);
    return STATUS_NOT_SUPPORTED;
}
NTSTATUS vhidmouseInjectButton(_In_ PMOUSE_CONTEXT C, _In_ WDFREQUEST R) {
    UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(R);
    return STATUS_NOT_IMPLEMENTED;
}
NTSTATUS vhidmouseInjectScroll(_In_ PMOUSE_CONTEXT C, _In_ WDFREQUEST R) {
    UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(R);
    return STATUS_NOT_IMPLEMENTED;
}
NTSTATUS vhidmouseReset(_In_ PMOUSE_CONTEXT C) {
    UNREFERENCED_PARAMETER(C);
    return STATUS_NOT_IMPLEMENTED;
}
