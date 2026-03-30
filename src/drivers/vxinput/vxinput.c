/*
 * Virtual Xbox Controller Driver (vxinput.sys)
 * 
 * Virtual bus driver that creates XInput-compatible gamepad devices.
 * Compatible with ViGEmBus approach for maximum game compatibility.
 */

#include "vxinput.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, vxinputEvtDeviceAdd)
#endif

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
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    WDF_OBJECT_ATTRIBUTES devAttr;
    PVXINPUT_BUS_CONTEXT busCtx;

    PAGED_CODE();

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttr, VXINPUT_BUS_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &devAttr, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vxinput: WdfDeviceCreate failed 0x%x\n", status));
        return status;
    }

    // Initialise bus context
    busCtx = vxinputGetBusContext(device);
    RtlZeroMemory(busCtx, sizeof(VXINPUT_BUS_CONTEXT));
    busCtx->BusDevice = device;
    InitializeListHead(&busCtx->ControllerList);
    KeInitializeSpinLock(&busCtx->ControllerListLock);

    // Create default I/O queue for IOCTL handling
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = vxinputEvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vxinput: WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    // Create a named symbolic link so user-mode can open \\.\vxinput
    {
        DECLARE_CONST_UNICODE_STRING(devName,  L"\\Device\\vxinput");
        DECLARE_CONST_UNICODE_STRING(symLink,  L"\\DosDevices\\vxinput");
        (void)WdfDeviceCreateSymbolicLink(device, &symLink);
        UNREFERENCED_PARAMETER(devName);
    }

    KdPrint(("vxinput: Device added, IOCTL queue ready\n"));
    return STATUS_SUCCESS;
}

// ── IOCTL dispatcher ─────────────────────────────────────────────────────────

VOID vxinputEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PVXINPUT_BUS_CONTEXT busCtx = vxinputGetBusContext(device);

    switch (IoControlCode) {

    case IOCTL_VXINPUT_SUBMIT_REPORT: {
        PXUSB_REPORT report;
        size_t bufLen;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(XUSB_REPORT), (PVOID*)&report, &bufLen);
        if (NT_SUCCESS(status)) {
            // Update the first (or only) controller's report in-context.
            // A full implementation would look up by index; for now update
            // the first active controller or stash for the next GetReport poll.
            PVXINPUT_CONTROLLER_CONTEXT ctrlCtx =
                vxinputGetControllerByIndex(busCtx, 0);
            if (ctrlCtx) {
                RtlCopyMemory(&ctrlCtx->CurrentReport, report, sizeof(XUSB_REPORT));
                status = STATUS_SUCCESS;
            } else {
                // No controller created yet — queue is established, IOCTL
                // succeeds so user-mode knows the driver is alive.
                status = STATUS_SUCCESS;
            }
            KdPrint(("vxinput: SUBMIT_REPORT buttons=0x%04x\n",
                     report->wButtons));
        }
        break;
    }

    case IOCTL_VXINPUT_CREATE_CONTROLLER: {
        PXUSB_CONTROLLER_INFO info;
        size_t bufLen;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(XUSB_CONTROLLER_INFO), (PVOID*)&info, &bufLen);
        if (NT_SUCCESS(status)) {
            PVXINPUT_CONTROLLER_CONTEXT newCtrl = NULL;
            status = vxinputCreateController(busCtx, info, &newCtrl);
        }
        break;
    }

    case IOCTL_VXINPUT_REMOVE_CONTROLLER: {
        PVXINPUT_CONTROLLER_CONTEXT ctrl = vxinputGetControllerByIndex(busCtx, 0);
        if (ctrl) {
            status = vxinputRemoveController(ctrl);
        } else {
            status = STATUS_NOT_FOUND;
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}
