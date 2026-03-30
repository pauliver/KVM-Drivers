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
    WDF_OBJECT_ATTRIBUTES attributes;

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    // Configure device as a filter driver
    WdfFdoInitSetFilter(DeviceInit);

    // Create device
    status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: WdfDeviceCreate failed 0x%x\n", status));
        return status;
    }

    // Set device context
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    status = WdfObjectAllocateContext(device, &attributes, (PVOID*)&deviceContext);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: WdfObjectAllocateContext failed 0x%x\n", status));
        return status;
    }

    // Initialize device context
    RtlZeroMemory(deviceContext, sizeof(DEVICE_CONTEXT));
    deviceContext->Device = device;

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

    // Set queue context
    WdfObjectSetDefaultIoQueue(device, queue);

    KdPrint(("vhidkb: Device added successfully\n"));
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");

    return STATUS_SUCCESS;
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
        // Pass through to lower driver
        WdfRequestFormatRequestUsingCurrentStackLocation(Request);
        WdfRequestSend(Request, WdfDeviceGetIoTarget(device), WDF_NO_SEND_OPTIONS);
        return;
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

    // Kernel-mode HID injection via pending read queue is not yet implemented.
    // Returning STATUS_NOT_IMPLEMENTED causes driver_interface.cpp to fall back
    // to the SendInput path, which correctly injects the key event.
    // When the VHF (Virtual HID Framework) path is implemented, remove this
    // and complete pending HID read IRPs with the report data above.
    UNREFERENCED_PARAMETER(DeviceContext);
    return STATUS_NOT_IMPLEMENTED;
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

    // Update device state
    DeviceContext->CurrentModifierKeys = inputReport->ModifierKeys;
    for (int i = 0; i < VKB_MAX_KEYS; i++) {
        DeviceContext->CurrentKeyCodes[i] = inputReport->KeyCodes[i];
    }

    // Send HID report
    status = vhidkbSendHidReport(DeviceContext, 
        inputReport->ModifierKeys, 
        inputReport->KeyCodes, 
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
    UNREFERENCED_PARAMETER(Request);

    // Clear all key codes (send empty report)
    UCHAR emptyKeys[VKB_MAX_KEYS] = {0};
    
    NTSTATUS status = vhidkbSendHidReport(DeviceContext, 
        DeviceContext->CurrentModifierKeys, 
        emptyKeys, 
        VKB_MAX_KEYS);

    // Clear state
    for (int i = 0; i < VKB_MAX_KEYS; i++) {
        DeviceContext->CurrentKeyCodes[i] = 0;
    }

    KdPrint(("vhidkb: InjectKeyUp - All keys released\n"));
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
