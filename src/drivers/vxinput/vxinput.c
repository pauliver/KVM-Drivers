/*
 * Virtual Xbox Controller Driver (vxinput.sys)
 * 
 * Virtual bus driver that creates XInput-compatible gamepad devices.
 * Compatible with ViGEmBus approach for maximum game compatibility.
 */

#include "vxinput.h"

#pragma comment(lib, "vhfkm.lib")

// HID gamepad descriptor — matches Xbox 360 layout for XInput recognition
// Report ID 1, 13 bytes: [buttons(2)][ltrig][rtrig][LX(2)][LY(2)][RX(2)][RY(2)]
static const UCHAR s_GamepadDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)

    // 16 digital buttons
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Min (Button 1)
    0x29, 0x10,        //   Usage Max (Button 16)
    0x15, 0x00,        //   Logical Min (0)
    0x25, 0x01,        //   Logical Max (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x10,        //   Report Count (16)
    0x81, 0x02,        //   Input (Data,Var,Abs)

    // Left trigger (8-bit, 0-255)
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x32,        //   Usage (Z)
    0x15, 0x00,        //   Logical Min (0)
    0x26, 0xFF, 0x00,  //   Logical Max (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data,Var,Abs)

    // Right trigger (8-bit, 0-255)
    0x09, 0x35,        //   Usage (Rz)
    0x81, 0x02,        //   Input (Data,Var,Abs)

    // Left stick X/Y (16-bit signed, -32768..32767)
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    0x16, 0x00, 0x80,  //   Logical Min (-32768)
    0x26, 0xFF, 0x7F,  //   Logical Max (32767)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data,Var,Abs)

    // Right stick X/Y
    0x09, 0x33,        //   Usage (Rx)
    0x09, 0x34,        //   Usage (Ry)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data,Var,Abs)

    0xC0               // End Collection
};

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
    devAttr.EvtCleanupCallback = vxinputEvtDeviceCleanup;  // Release VHF handles on removal
    status = WdfDeviceCreate(&DeviceInit, &devAttr, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vxinput: WdfDeviceCreate failed 0x%x\n", status));
        return status;
    }

    // Initialise bus context (RtlZeroMemory already zeros VhfHandle[4] array)
    busCtx = vxinputGetBusContext(device);
    RtlZeroMemory(busCtx, sizeof(VXINPUT_BUS_CONTEXT));
    busCtx->BusDevice = device;
    InitializeListHead(&busCtx->ControllerList);
    KeInitializeSpinLock(&busCtx->ControllerListLock);

    // Default I/O queue for IOCTL handling
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = vxinputEvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vxinput: WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    // Symbolic link so user-mode can open \\.\vxinput
    {
        DECLARE_CONST_UNICODE_STRING(devName, L"\\Device\\vxinput");
        DECLARE_CONST_UNICODE_STRING(symLink, L"\\DosDevices\\vxinput");
        (void)WdfDeviceCreateSymbolicLink(device, &symLink);
        UNREFERENCED_PARAMETER(devName);
    }

    // Slots are created on demand via IOCTL_VXINPUT_CREATE_CONTROLLER.
    // No VHF handles created here at device-add time.
    for (int i = 0; i < VXINPUT_MAX_CONTROLLERS; i++)
        busCtx->VhfHandle[i] = NULL;

    KdPrint(("vxinput: Device added, %d controller slots ready\n", VXINPUT_MAX_CONTROLLERS));
    return STATUS_SUCCESS;
}

// Tear-down all active VHF slots when the device is removed
static VOID vxinputEvtDeviceCleanup(_In_ WDFOBJECT Object)
{
    PVXINPUT_BUS_CONTEXT ctx =
        vxinputGetBusContext((WDFDEVICE)Object);
    if (!ctx) return;
    for (int i = 0; i < VXINPUT_MAX_CONTROLLERS; i++) {
        if (ctx->VhfHandle[i]) {
            VhfDelete(ctx->VhfHandle[i], TRUE);
            ctx->VhfHandle[i] = NULL;
        }
    }
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
        // bReportId encodes the player slot (0-3). Build a 13-byte HID report
        // and submit it to the corresponding VHF handle.
        PXUSB_REPORT report;
        size_t bufLen;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(XUSB_REPORT), (PVOID*)&report, &bufLen);
        if (NT_SUCCESS(status)) {
            ULONG slot = report->bReportId & (VXINPUT_MAX_CONTROLLERS - 1);
            RtlCopyMemory(&busCtx->LastReport[slot], report, sizeof(XUSB_REPORT));

            if (busCtx->VhfHandle[slot]) {
                UCHAR hidReport[13];
                hidReport[0]  = 0x01;
                hidReport[1]  = (UCHAR)(report->wButtons & 0xFF);
                hidReport[2]  = (UCHAR)(report->wButtons >> 8);
                hidReport[3]  = report->bLeftTrigger;
                hidReport[4]  = report->bRightTrigger;
                hidReport[5]  = (UCHAR)( report->sThumbLX & 0xFF);
                hidReport[6]  = (UCHAR)((report->sThumbLX >> 8) & 0xFF);
                hidReport[7]  = (UCHAR)( report->sThumbLY & 0xFF);
                hidReport[8]  = (UCHAR)((report->sThumbLY >> 8) & 0xFF);
                hidReport[9]  = (UCHAR)( report->sThumbRX & 0xFF);
                hidReport[10] = (UCHAR)((report->sThumbRX >> 8) & 0xFF);
                hidReport[11] = (UCHAR)( report->sThumbRY & 0xFF);
                hidReport[12] = (UCHAR)((report->sThumbRY >> 8) & 0xFF);

                HID_XFER_PACKET pkt;
                pkt.reportBuffer    = hidReport;
                pkt.reportBufferLen = sizeof(hidReport);
                pkt.reportId        = 0x01;

                status = VhfReadReportSubmit(busCtx->VhfHandle[slot], &pkt);
                if (!NT_SUCCESS(status))
                    KdPrint(("vxinput: slot %lu VhfReadReportSubmit failed 0x%x\n", slot, status));
            } else {
                // Slot not yet created — silently accept and cache
                status = STATUS_SUCCESS;
            }
            KdPrint(("vxinput: SUBMIT slot=%lu buttons=0x%04x\n", slot, report->wButtons));
        }
        break;
    }

    case IOCTL_VXINPUT_CREATE_CONTROLLER: {
        // Create a VHF virtual gamepad for the requested slot (UserIndex 0-3).
        PXUSB_CONTROLLER_INFO info;
        size_t bufLen;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(XUSB_CONTROLLER_INFO), (PVOID*)&info, &bufLen);
        if (!NT_SUCCESS(status)) break;

        ULONG slot = info->UserIndex & (VXINPUT_MAX_CONTROLLERS - 1);
        if (busCtx->VhfHandle[slot]) {
            // Already created for this slot
            status = STATUS_SUCCESS;
            break;
        }

        VHF_CONFIG vhfCfg;
        VHF_CONFIG_INIT(&vhfCfg,
            WdfDeviceWdmGetDeviceObject(device),
            sizeof(s_GamepadDescriptor),
            s_GamepadDescriptor);
        vhfCfg.HidDeviceAttributes.Size          = sizeof(HID_DEVICE_ATTRIBUTES);
        vhfCfg.HidDeviceAttributes.VendorID      = 0x045E;
        vhfCfg.HidDeviceAttributes.ProductID     = 0x028E;
        vhfCfg.HidDeviceAttributes.VersionNumber = (USHORT)(0x0114 + slot);

        status = VhfCreate(&vhfCfg, &busCtx->VhfHandle[slot]);
        if (!NT_SUCCESS(status)) {
            KdPrint(("vxinput: VhfCreate slot %lu failed 0x%x\n", slot, status));
            break;
        }
        status = VhfStart(busCtx->VhfHandle[slot]);
        if (!NT_SUCCESS(status)) {
            KdPrint(("vxinput: VhfStart slot %lu failed 0x%x\n", slot, status));
            VhfDelete(busCtx->VhfHandle[slot], TRUE);
            busCtx->VhfHandle[slot] = NULL;
            break;
        }
        KdPrint(("vxinput: slot %lu VHF created\n", slot));
        break;
    }

    case IOCTL_VXINPUT_REMOVE_CONTROLLER: {
        // UserIndex in XUSB_CONTROLLER_INFO selects which slot to remove.
        PXUSB_CONTROLLER_INFO info;
        size_t bufLen;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(XUSB_CONTROLLER_INFO), (PVOID*)&info, &bufLen);
        if (!NT_SUCCESS(status)) break;

        ULONG slot = info->UserIndex & (VXINPUT_MAX_CONTROLLERS - 1);
        if (busCtx->VhfHandle[slot]) {
            VhfDelete(busCtx->VhfHandle[slot], TRUE);
            busCtx->VhfHandle[slot] = NULL;
            KdPrint(("vxinput: slot %lu VHF removed\n", slot));
        }
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_VXINPUT_SET_RUMBLE: {
        // Input: XUSB_RUMBLE_STATE (4 bytes). Slot encoded in bLeftTriggerMotor's
        // upper nibble for legacy compat, or use bReportId-less slot=0 default.
        // For simplicity, accept a 4-byte XUSB_RUMBLE_STATE and store for slot 0.
        PXUSB_RUMBLE_STATE rumble;
        size_t bufLen;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(XUSB_RUMBLE_STATE), (PVOID*)&rumble, &bufLen);
        if (NT_SUCCESS(status)) {
            // Store rumble state in slot 0 (single-slot path; per-slot via automation)
            RtlCopyMemory(&busCtx->RumbleState[0], rumble, sizeof(XUSB_RUMBLE_STATE));
            KdPrint(("vxinput: SET_RUMBLE L=%u R=%u\n",
                (ULONG)rumble->bLeftMotor, (ULONG)rumble->bRightMotor));
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}
