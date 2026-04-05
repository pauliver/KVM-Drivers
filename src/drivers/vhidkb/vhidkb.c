/*
 * Virtual HID Keyboard Driver (vhidkb.sys)
 * 
 * This driver acts as an upper filter on the HID keyboard stack,
 * allowing injection of keyboard input that is indistinguishable
 * from physical keyboard input.
 * 
 * Author: KVM-Drivers Team
 * License: [To be determined]
 */

#include "vhidkb.h"
#include "vhidkb_ioctl.h"

#pragma comment(lib, "vhfkm.lib")

// Standard HID boot-protocol keyboard report descriptor (8-byte reports)
static const UCHAR s_KbdDescriptor[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0xE0,       //   Usage Min (Left Ctrl)
    0x29, 0xE7,       //   Usage Max (Right GUI)
    0x15, 0x00,       //   Logical Min (0)
    0x25, 0x01,       //   Logical Max (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data, Variable, Absolute) — modifier byte
    0x95, 0x01,       //   Report Count (1)
    0x75, 0x08,       //   Report Size (8)
    0x81, 0x01,       //   Input (Constant) — reserved byte
    0x95, 0x06,       //   Report Count (6)
    0x75, 0x08,       //   Report Size (8)
    0x15, 0x00,       //   Logical Min (0)
    0x25, 0x65,       //   Logical Max (101)
    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0x00,       //   Usage Min (0)
    0x29, 0x65,       //   Usage Max (101)
    0x81, 0x00,       //   Input (Data, Array, Absolute) — key codes
    0xC0              // End Collection
};

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, vhidkbEvtDeviceAdd)
#pragma alloc_text (PAGE, vhidkbEvtDriverContextCleanup)
#endif

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;

    // Initialize WPP tracing
    WPP_INIT_TRACING(DriverObject, RegistryPath);

    // Register cleanup callback
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = vhidkbEvtDriverContextCleanup;

    // Initialize driver config
    WDF_DRIVER_CONFIG_INIT(&config, vhidkbEvtDeviceAdd);

    // Create the driver object
    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE
    );

    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: WdfDriverCreate failed 0x%x\n", status));
        WPP_CLEANUP(DriverObject);
        return status;
    }

    KdPrint(("vhidkb: Driver loaded successfully\n"));
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    return status;
}

NTSTATUS vhidkbEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    UNREFERENCED_PARAMETER(Driver);

    NTSTATUS status;
    WDFDEVICE device;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    PDEVICE_CONTEXT deviceContext;

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    // Stand-alone HID source (not a filter) — VHF creates its own child device
    WDF_OBJECT_ATTRIBUTES devAttr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttr, DEVICE_CONTEXT);
    devAttr.EvtCleanupCallback = vhidkbEvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &devAttr, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: WdfDeviceCreate failed 0x%x\n", status));
        return status;
    }

    deviceContext = vhidkbGetDeviceContext(device);
    RtlZeroMemory(deviceContext, sizeof(DEVICE_CONTEXT));
    deviceContext->Device = device;
    deviceContext->VhfHandle = NULL;

    // Create default I/O queue
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = vhidkbEvtIoDeviceControl;
    queueConfig.EvtIoStop = vhidkbEvtIoStop;

    status = WdfIoQueueCreate(
        device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue
    );

    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    WdfObjectSetDefaultIoQueue(device, queue);

    // ── Create VHF virtual HID keyboard source ──────────────────────────────
    VHF_CONFIG vhfConfig;
    VHF_CONFIG_INIT(&vhfConfig,
        WdfDeviceWdmGetDeviceObject(device),
        sizeof(s_KbdDescriptor),
        s_KbdDescriptor);

    // Keyboard HID device attributes (VID/PID match generic keyboard)
    vhfConfig.HidDeviceAttributes.Size          = sizeof(HID_DEVICE_ATTRIBUTES);
    vhfConfig.HidDeviceAttributes.VendorID       = 0x045E;  // Microsoft
    vhfConfig.HidDeviceAttributes.ProductID      = 0x0750;  // KVM virtual keyboard
    vhfConfig.HidDeviceAttributes.VersionNumber  = 0x0100;

    status = VhfCreate(&vhfConfig, &deviceContext->VhfHandle);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: VhfCreate failed 0x%x\n", status));
        return status;
    }

    status = VhfStart(deviceContext->VhfHandle);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: VhfStart failed 0x%x\n", status));
        VhfDelete(deviceContext->VhfHandle, TRUE);
        deviceContext->VhfHandle = NULL;
        return status;
    }

    // Expose \Device\vhidkb / \DosDevices\vhidkb so user-mode can open \\.\vhidkb
    {
        DECLARE_CONST_UNICODE_STRING(symLink, L"\\DosDevices\\vhidkb");
        NTSTATUS slStatus = WdfDeviceCreateSymbolicLink(device, &symLink);
        if (!NT_SUCCESS(slStatus)) {
            KdPrint(("vhidkb: WdfDeviceCreateSymbolicLink failed 0x%x (non-fatal)\n", slStatus));
        }
    }

    KdPrint(("vhidkb: VHF virtual keyboard started\n"));
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    return STATUS_SUCCESS;
}

// ── VHF cleanup: called when WDFDEVICE is being destroyed ────────────────────
VOID vhidkbEvtDeviceContextCleanup(_In_ WDFOBJECT Object)
{
    PDEVICE_CONTEXT ctx = vhidkbGetDeviceContext((WDFDEVICE)Object);
    if (ctx && ctx->VhfHandle) {
        VhfDelete(ctx->VhfHandle, TRUE);
        ctx->VhfHandle = NULL;
        KdPrint(("vhidkb: VHF handle released\n"));
    }
}

VOID vhidkbEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    NTSTATUS status = STATUS_SUCCESS;
    WDFDEVICE device;
    PDEVICE_CONTEXT deviceContext;

    device = WdfIoQueueGetDevice(Queue);
    deviceContext = vhidkbGetDeviceContext(device);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, 
        "vhidkbEvtIoDeviceControl: IOCTL 0x%x", IoControlCode);

    switch (IoControlCode) {
    case IOCTL_VKB_INJECT_KEYDOWN:
        status = vhidkbInjectKeyDown(deviceContext, Request);
        break;

    case IOCTL_VKB_INJECT_KEYUP:
        status = vhidkbInjectKeyUp(deviceContext, Request);
        break;

    case IOCTL_VKB_INJECT_COMBO:
        status = vhidkbInjectKeyCombo(deviceContext, Request);
        break;

    case IOCTL_VKB_RESET:
        status = vhidkbReset(deviceContext);
        break;

    default:
        // Standalone VHF device — no lower driver to forward to.
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}

VOID vhidkbEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(ActionFlags);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "%!FUNC! Entry");

    if (ActionFlags & WdfRequestStopActionSuspend) {
        WdfRequestStopAcknowledge(Request, FALSE);
    } else if (ActionFlags & WdfRequestStopActionPurge) {
        WdfRequestCancelSentRequest(Request);
    }
}

VOID vhidkbEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
)
{
    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)DriverObject));
}

// Key injection implementations with HID report generation
NTSTATUS vhidkbSendHidReport(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ UCHAR ModifierKeys,
    _In_ UCHAR* KeyCodes,
    _In_ UCHAR KeyCount
)
{
    UNREFERENCED_PARAMETER(DeviceContext);
    
    // Build HID boot keyboard report (8 bytes)
    // [Modifier][Reserved][Key0][Key1][Key2][Key3][Key4][Key5]
    UCHAR report[8] = {0};
    report[0] = ModifierKeys;
    report[1] = 0; // Reserved
    
    // Copy up to 6 key codes
    UCHAR keysToCopy = (KeyCount > 6) ? 6 : KeyCount;
    for (UCHAR i = 0; i < keysToCopy; i++) {
        report[2 + i] = KeyCodes[i];
    }
    
    // Log the constructed report for diagnostic purposes
    KdPrint(("vhidkb: HID Report: [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
        report[0], report[1], report[2], report[3],
        report[4], report[5], report[6], report[7]));

    // Submit the 8-byte HID report to the VHF virtual keyboard device.
    // The HID class driver above VHF will deliver it to all consumers
    // (e.g., the focused application, win32k, etc.).
    if (!DeviceContext || !DeviceContext->VhfHandle) {
        // VHF not yet started — fall back gracefully (driver_interface.cpp
        // will retry via SendInput when IOCTL returns failure).
        return STATUS_DEVICE_NOT_READY;
    }

    HID_XFER_PACKET packet;
    packet.reportBuffer    = report;
    packet.reportBufferLen = 8;    // Always 8 bytes for boot-protocol keyboard
    packet.reportId        = 0;    // Report ID 0 (no report IDs in descriptor)

    NTSTATUS st = VhfReadReportSubmit(DeviceContext->VhfHandle, &packet);
    if (!NT_SUCCESS(st)) {
        KdPrint(("vhidkb: VhfReadReportSubmit failed 0x%x\n", st));
    }
    return st;
}

NTSTATUS vhidkbInjectKeyDown(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ WDFREQUEST Request
)
{
    PVKB_INPUT_REPORT inputReport;
    size_t bufferSize;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(VKB_INPUT_REPORT), (PVOID*)&inputReport, &bufferSize);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: WdfRequestRetrieveInputBuffer failed 0x%x\n", status));
        return status;
    }

    // Update modifier state (always reflects current modifier mask)
    DeviceContext->CurrentModifierKeys = inputReport->ModifierKeys;

    // Add pressed key to the first empty slot in the held-key array.
    // This preserves other simultaneously-held keys rather than overwriting them.
    // (A key already present is not duplicated.)
    UCHAR newKey = inputReport->KeyCodes[0];
    if (newKey != 0) {
        BOOLEAN alreadyHeld = FALSE;
        int emptySlot = -1;
        for (int i = 0; i < VKB_MAX_KEYS; i++) {
            if (DeviceContext->CurrentKeyCodes[i] == newKey) {
                alreadyHeld = TRUE;
                break;
            }
            if (emptySlot < 0 && DeviceContext->CurrentKeyCodes[i] == 0) {
                emptySlot = i;
            }
        }
        if (!alreadyHeld && emptySlot >= 0) {
            DeviceContext->CurrentKeyCodes[emptySlot] = newKey;
        }
    }

    // Send HID report reflecting all currently-held keys
    status = vhidkbSendHidReport(DeviceContext,
        DeviceContext->CurrentModifierKeys,
        DeviceContext->CurrentKeyCodes,
        VKB_MAX_KEYS);

    KdPrint(("vhidkb: InjectKeyDown - Modifier: 0x%x, Key: 0x%x\n", 
        inputReport->ModifierKeys, inputReport->KeyCodes[0]));

    return status;
}

NTSTATUS vhidkbInjectKeyUp(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ WDFREQUEST Request
)
{
    PVKB_INPUT_REPORT inputReport;
    size_t bufferSize;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(
        Request, sizeof(VKB_INPUT_REPORT), (PVOID*)&inputReport, &bufferSize);
    if (!NT_SUCCESS(status)) {
        // No buffer (e.g. IOCTL_VKB_RESET path) — clear everything
        UCHAR emptyKeys[VKB_MAX_KEYS] = {0};
        DeviceContext->CurrentModifierKeys = 0;
        for (int i = 0; i < VKB_MAX_KEYS; i++) DeviceContext->CurrentKeyCodes[i] = 0;
        return vhidkbSendHidReport(DeviceContext, 0, emptyKeys, 0);
    }

    // Remove only the specific key that was released from the held-key array.
    // Other simultaneously-held keys remain, so e.g. releasing C while holding
    // Ctrl does not also release Ctrl.
    UCHAR releasedKey = inputReport->KeyCodes[0];
    if (releasedKey != 0) {
        for (int i = 0; i < VKB_MAX_KEYS; i++) {
            if (DeviceContext->CurrentKeyCodes[i] == releasedKey) {
                DeviceContext->CurrentKeyCodes[i] = 0;
                break;
            }
        }
    }

    // Modifier state comes from the client-reported value (reflects which modifiers
    // are STILL held after this release, not just the released key's modifiers).
    DeviceContext->CurrentModifierKeys = inputReport->ModifierKeys;

    // Re-send updated state: released key is now absent; held keys remain.
    status = vhidkbSendHidReport(DeviceContext,
        DeviceContext->CurrentModifierKeys,
        DeviceContext->CurrentKeyCodes,
        VKB_MAX_KEYS);

    KdPrint(("vhidkb: InjectKeyUp - released key=0x%x mods=0x%x\n",
        releasedKey, DeviceContext->CurrentModifierKeys));
    return status;
}

NTSTATUS vhidkbInjectKeyCombo(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ WDFREQUEST Request
)
{
    PVKB_KEY_COMBO combo;
    size_t bufferSize;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(VKB_KEY_COMBO), (PVOID*)&combo, &bufferSize);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Process each key in the combo
    for (ULONG i = 0; i < combo->Count && i < VKB_MAX_KEYS; i++) {
        UCHAR keys[1] = { combo->Keys[i].KeyCode };
        
        if (combo->Keys[i].KeyUp) {
            // Key up
            UCHAR emptyKeys[VKB_MAX_KEYS] = {0};
            vhidkbSendHidReport(DeviceContext, combo->Keys[i].ModifierKeys, emptyKeys, 0);
        } else {
            // Key down
            vhidkbSendHidReport(DeviceContext, combo->Keys[i].ModifierKeys, keys, 1);
            
            // Hold duration
            if (combo->Keys[i].DurationMs > 0) {
                // In a real implementation, we'd use a timer
                // For now, just note it
            }
        }
    }

    KdPrint(("vhidkb: InjectKeyCombo with %u keys\n", combo->Count));
    return STATUS_SUCCESS;
}

NTSTATUS vhidkbReset(_In_ PDEVICE_CONTEXT DeviceContext)
{
    UCHAR emptyKeys[VKB_MAX_KEYS] = {0};
    
    // Send empty report to release all keys
    vhidkbSendHidReport(DeviceContext, 0, emptyKeys, 0);
    
    // Clear state
    DeviceContext->CurrentModifierKeys = 0;
    for (int i = 0; i < VKB_MAX_KEYS; i++) {
        DeviceContext->CurrentKeyCodes[i] = 0;
    }
    
    KdPrint(("vhidkb: Reset - All keys released\n"));
    return STATUS_SUCCESS;
}
