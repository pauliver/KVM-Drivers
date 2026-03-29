/*
 * Virtual Xbox Controller Driver (vxinput.sys)
 * 
 * Virtual bus driver that creates XInput-compatible gamepad devices.
 * Implements XUSB protocol for maximum game compatibility.
 */

#include "vxinput.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, vxinputEvtDeviceAdd)
#pragma alloc_text (PAGE, vxinputEvtIoDeviceControl)
#endif

// Global bus context
static PVXINPUT_BUS_CONTEXT g_BusContext = NULL;

// XUSB Report Descriptor for Xbox 360 controller
UCHAR G_XusbReportDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x15, 0x00,        //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  // Logical Maximum (65535)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x02,        //     Input (Data, Var, Abs)
    0xC0,              //   End Collection
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x32,        //     Usage (Z)
    0x09, 0x35,        //     Usage (Rz)
    0x15, 0x00,        //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  // Logical Maximum (65535)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x02,        //     Input (Data, Var, Abs)
    0xC0,              //   End Collection
    0x05, 0x02,        //   Usage Page (Simulation Controls)
    0x09, 0xC5,        //   Usage (Brake)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data, Var, Abs)
    0x09, 0xC4,        //   Usage (Accelerator)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data, Var, Abs)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x0E,        //   Usage Maximum (14)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0E,        //   Report Count (14)
    0x81, 0x02,        //   Input (Data, Var, Abs)
    0x75, 0x06,        //   Report Size (6)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const)
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x39,        //   Usage (Hat Switch)
    0x15, 0x01,        //   Logical Minimum (1)
    0x25, 0x08,        //   Logical Maximum (8)
    0x35, 0x00,        //   Physical Minimum (0)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x42,        //   Input (Data, Var, Abs, Null)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const)
    0xC0               // End Collection
};

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;

    WPP_INIT_TRACING(DriverObject, RegistryPath);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    WDF_DRIVER_CONFIG_INIT(&config, vxinputEvtDeviceAdd);

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE
    );

    if (!NT_SUCCESS(status)) {
        KdPrint(("vxinput: Driver creation failed 0x%x\n", status));
        WPP_CLEANUP(DriverObject);
        return status;
    }

    KdPrint(("vxinput: Driver loaded\n"));
    return status;
}

NTSTATUS vxinputEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    UNREFERENCED_PARAMETER(Driver);
    NTSTATUS status;
    WDFDEVICE device;
    PVXINPUT_BUS_CONTEXT busContext;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    WDF_OBJECT_ATTRIBUTES attributes;

    PAGED_CODE();

    // Create bus device
    status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vxinput: WdfDeviceCreate failed 0x%x\n", status));
        return status;
    }

    // Allocate bus context
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VXINPUT_BUS_CONTEXT);
    status = WdfObjectAllocateContext(device, &attributes, (PVOID*)&busContext);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vxinput: WdfObjectAllocateContext failed 0x%x\n", status));
        return status;
    }

    // Initialize bus context
    RtlZeroMemory(busContext, sizeof(VXINPUT_BUS_CONTEXT));
    busContext->BusDevice = device;
    KeInitializeSpinLock(&busContext->ControllerListLock);
    InitializeListHead(&busContext->ControllerList);
    busContext->NextControllerIndex = 1;

    // Create default I/O queue
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = vxinputEvtIoDeviceControl;

    status = WdfIoQueueCreate(
        device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue
    );

    if (!NT_SUCCESS(status)) {
        KdPrint(("vxinput: WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    WdfObjectSetDefaultIoQueue(device, queue);
    g_BusContext = busContext;

    KdPrint(("vxinput: Virtual bus created\n"));
    return STATUS_SUCCESS;
}

// Create a new virtual controller
NTSTATUS vxinputCreateController(
    _In_ PVXINPUT_BUS_CONTEXT BusContext,
    _In_ PXUSB_CONTROLLER_INFO ControllerInfo,
    _Out_ PVXINPUT_CONTROLLER_CONTEXT* ControllerContext
)
{
    NTSTATUS status;
    WDFDEVICE controllerDevice;
    PVXINPUT_CONTROLLER_CONTEXT context;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_DEVICE_PNP_CAPABILITIES pnpCaps;
    WDF_DEVICE_STATE deviceState;

    KdPrint(("vxinput: Creating controller %d\n", ControllerInfo->UserIndex));

    // Allocate PDO device init
    PWDFDEVICE_INIT pdoInit = WdfPdoInitAllocate(BusContext->BusDevice);
    if (!pdoInit) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Set device IDs
    DECLARE_CONST_UNICODE_STRING(deviceId, L"USB\\VID_045E&PID_028E");  // Xbox 360 controller
    DECLARE_CONST_UNICODE_STRING(hardwareId, L"USB\\VID_045E&PID_028E&REV_0114");
    DECLARE_CONST_UNICODE_STRING(compatId, L"USB\\Class_FF&SubClass_5D&Prot_01");
    DECLARE_CONST_UNICODE_STRING(instanceId, L"0");

    status = WdfPdoInitAssignDeviceID(pdoInit, &deviceId);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(pdoInit);
        return status;
    }

    status = WdfPdoInitAddHardwareID(pdoInit, &hardwareId);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(pdoInit);
        return status;
    }

    status = WdfPdoInitAddCompatibleID(pdoInit, &compatId);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(pdoInit);
        return status;
    }

    status = WdfPdoInitAssignInstanceID(pdoInit, &instanceId);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(pdoInit);
        return status;
    }

    // Create PDO
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VXINPUT_CONTROLLER_CONTEXT);
    status = WdfDeviceCreate(&pdoInit, &attributes, &controllerDevice);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(pdoInit);
        return status;
    }

    // Initialize controller context
    context = vxinputGetControllerContext(controllerDevice);
    RtlZeroMemory(context, sizeof(VXINPUT_CONTROLLER_CONTEXT));
    context->ControllerDevice = controllerDevice;
    context->BusContext = BusContext;
    context->ControllerIndex = BusContext->NextControllerIndex++;
    context->UserIndex = ControllerInfo->UserIndex;
    context->IsActive = TRUE;
    
    // Initialize default report
    RtlZeroMemory(&context->CurrentReport, sizeof(XUSB_REPORT));
    context->CurrentReport.bSize = sizeof(XUSB_REPORT);
    context->CurrentReport.bReportId = 0x00;

    // Initialize rumble state
    context->RumbleState.bLeftTriggerMotor = 0;
    context->RumbleState.bRightTriggerMotor = 0;

    // Add to controller list
    KIRQL oldIrql;
    KeAcquireSpinLock(&BusContext->ControllerListLock, &oldIrql);
    InsertTailList(&BusContext->ControllerList, &context->ListEntry);
    BusContext->ControllerCount++;
    KeReleaseSpinLock(&BusContext->ControllerListLock, oldIrql);

    // Set device state to started
    WDF_DEVICE_STATE_INIT(&deviceState);
    deviceState.Removed = WdfFalse;
    WdfDeviceSetDeviceState(controllerDevice, &deviceState);

    KdPrint(("vxinput: Controller %d created (PDO: %p)\n", context->ControllerIndex, controllerDevice));

    *ControllerContext = context;
    return STATUS_SUCCESS;
}

// Remove a controller
NTSTATUS vxinputRemoveController(
    _In_ PVXINPUT_CONTROLLER_CONTEXT ControllerContext
)
{
    PVXINPUT_BUS_CONTEXT busContext = ControllerContext->BusContext;
    KIRQL oldIrql;

    KdPrint(("vxinput: Removing controller %d\n", ControllerContext->ControllerIndex));

    KeAcquireSpinLock(&busContext->ControllerListLock, &oldIrql);
    RemoveEntryList(&ControllerContext->ListEntry);
    busContext->ControllerCount--;
    KeReleaseSpinLock(&busContext->ControllerListLock, oldIrql);

    ControllerContext->IsActive = FALSE;

    // Mark PDO for removal
    WdfDeviceSetRemoved(ControllerContext->ControllerDevice);

    return STATUS_SUCCESS;
}

// Submit XUSB report
NTSTATUS vxinputSubmitReport(
    _In_ PVXINPUT_CONTROLLER_CONTEXT ControllerContext,
    _In_ PXUSB_REPORT Report
)
{
    if (!ControllerContext || !ControllerContext->IsActive) {
        return STATUS_DEVICE_NOT_READY;
    }

    // Store the report
    RtlCopyMemory(&ControllerContext->CurrentReport, Report, sizeof(XUSB_REPORT));

    KdPrint(("vxinput: Report submitted for controller %d (buttons: 0x%04X)\n",
        ControllerContext->ControllerIndex, Report->wButtons));

    return STATUS_SUCCESS;
}

// IOCTL handlers
VOID vxinputEvtIoDeviceControl(
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
    PVXINPUT_BUS_CONTEXT busContext;

    device = WdfIoQueueGetDevice(Queue);
    busContext = vxinputGetBusContext(device);

    switch (IoControlCode) {
    case IOCTL_VXINPUT_CREATE_CONTROLLER: {
        PXUSB_CONTROLLER_INFO controllerInfo;
        size_t bufferSize;
        PVXINPUT_CONTROLLER_CONTEXT controllerContext = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(XUSB_CONTROLLER_INFO), (PVOID*)&controllerInfo, &bufferSize);
        if (NT_SUCCESS(status)) {
            status = vxinputCreateController(busContext, controllerInfo, &controllerContext);
            if (NT_SUCCESS(status)) {
                // Return controller handle
                PXUSB_CONTROLLER_INFO outputInfo;
                status = WdfRequestRetrieveOutputBuffer(Request, sizeof(XUSB_CONTROLLER_INFO), (PVOID*)&outputInfo, &bufferSize);
                if (NT_SUCCESS(status)) {
                    outputInfo->UserIndex = controllerContext->ControllerIndex;
                    outputInfo->ControllerHandle = (HANDLE)controllerContext;
                    WdfRequestSetInformation(Request, sizeof(XUSB_CONTROLLER_INFO));
                }
            }
        }
        break;
    }

    case IOCTL_VXINPUT_REMOVE_CONTROLLER: {
        PVXINPUT_CONTROLLER_CONTEXT controllerContext;
        size_t bufferSize;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(PVOID), (PVOID*)&controllerContext, &bufferSize);
        if (NT_SUCCESS(status)) {
            status = vxinputRemoveController(controllerContext);
        }
        break;
    }

    case IOCTL_VXINPUT_SUBMIT_REPORT: {
        PXUSB_REPORT report;
        size_t bufferSize;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(XUSB_REPORT), (PVOID*)&report, &bufferSize);
        if (NT_SUCCESS(status)) {
            // Controller handle is in the request context or we search by index
            // For now, use the first active controller
            PVXINPUT_CONTROLLER_CONTEXT controller = NULL;
            PLIST_ENTRY entry;
            KIRQL oldIrql;

            KeAcquireSpinLock(&busContext->ControllerListLock, &oldIrql);
            if (!IsListEmpty(&busContext->ControllerList)) {
                entry = busContext->ControllerList.Flink;
                controller = CONTAINING_RECORD(entry, VXINPUT_CONTROLLER_CONTEXT, ListEntry);
            }
            KeReleaseSpinLock(&busContext->ControllerListLock, oldIrql);

            if (controller) {
                status = vxinputSubmitReport(controller, report);
            } else {
                status = STATUS_DEVICE_NOT_CONNECTED;
            }
        }
        break;
    }

    case IOCTL_VXINPUT_GET_RUMBLE: {
        // Return rumble state for a controller
        PVXINPUT_CONTROLLER_CONTEXT controllerContext;
        size_t bufferSize;
        PXUSB_RUMBLE_STATE rumbleState;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(PVOID), (PVOID*)&controllerContext, &bufferSize);
        if (NT_SUCCESS(status)) {
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(XUSB_RUMBLE_STATE), (PVOID*)&rumbleState, &bufferSize);
            if (NT_SUCCESS(status)) {
                RtlCopyMemory(rumbleState, &controllerContext->RumbleState, sizeof(XUSB_RUMBLE_STATE));
                WdfRequestSetInformation(Request, sizeof(XUSB_RUMBLE_STATE));
            }
        }
        break;
    }

    case IOCTL_VXINPUT_GET_CONTROLLER_COUNT: {
        PULONG count;
        size_t bufferSize;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), (PVOID*)&count, &bufferSize);
        if (NT_SUCCESS(status)) {
            KIRQL oldIrql;
            KeAcquireSpinLock(&busContext->ControllerListLock, &oldIrql);
            *count = busContext->ControllerCount;
            KeReleaseSpinLock(&busContext->ControllerListLock, oldIrql);
            WdfRequestSetInformation(Request, sizeof(ULONG));
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}

// Get controller by index
PVXINPUT_CONTROLLER_CONTEXT vxinputGetControllerByIndex(
    _In_ PVXINPUT_BUS_CONTEXT BusContext,
    _In_ ULONG Index
)
{
    PVXINPUT_CONTROLLER_CONTEXT controller = NULL;
    PLIST_ENTRY entry;
    KIRQL oldIrql;

    KeAcquireSpinLock(&BusContext->ControllerListLock, &oldIrql);
    
    for (entry = BusContext->ControllerList.Flink; entry != &BusContext->ControllerList; entry = entry->Flink) {
        PVXINPUT_CONTROLLER_CONTEXT ctx = CONTAINING_RECORD(entry, VXINPUT_CONTROLLER_CONTEXT, ListEntry);
        if (ctx->ControllerIndex == Index && ctx->IsActive) {
            controller = ctx;
            break;
        }
    }

    KeReleaseSpinLock(&BusContext->ControllerListLock, oldIrql);
    return controller;
}

// Cleanup all controllers on driver unload
VOID vxinputCleanupControllers(
    _In_ PVXINPUT_BUS_CONTEXT BusContext
)
{
    PLIST_ENTRY entry;
    KIRQL oldIrql;

    KeAcquireSpinLock(&BusContext->ControllerListLock, &oldIrql);

    while (!IsListEmpty(&BusContext->ControllerList)) {
        entry = RemoveHeadList(&BusContext->ControllerList);
        PVXINPUT_CONTROLLER_CONTEXT controller = CONTAINING_RECORD(entry, VXINPUT_CONTROLLER_CONTEXT, ListEntry);
        
        controller->IsActive = FALSE;
        WdfDeviceSetRemoved(controller->ControllerDevice);
    }

    BusContext->ControllerCount = 0;

    KeReleaseSpinLock(&BusContext->ControllerListLock, oldIrql);
}
