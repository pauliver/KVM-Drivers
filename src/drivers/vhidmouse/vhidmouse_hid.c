// vhidmouse_hid.c - HID Minidriver Implementation for Virtual Mouse
// Registers with Windows HID class driver for actual mouse input injection

#include "vhidmouse.h"
#include <hidport.h>

// HID Report Descriptor for standard 5-button mouse with wheel
// Input: X, Y, Wheel, Pan, 5 buttons
// Output: None (no LEDs on mouse)
UCHAR G_MouseReportDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    
    // Buttons (5 buttons)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x05,        //     Usage Maximum (5)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x05,        //     Report Count (5)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data, Var, Abs)
    
    // Button padding (3 bits)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x03,        //     Report Size (3)
    0x81, 0x03,        //     Input (Const, Var, Abs)
    
    // X and Y axis (16-bit signed relative)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x16, 0x01, 0x80,  //     Logical Minimum (-32767)
    0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x06,        //     Input (Data, Var, Rel)
    
    // Wheel (8-bit signed relative)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data, Var, Rel)
    
    // Horizontal wheel (Pan)
    0x05, 0x0C,        //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,  //     Usage (AC Pan)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data, Var, Rel)
    
    0xC0,              //   End Collection
    0xC0               // End Collection
};

#define G_MOUSE_REPORT_DESC_SIZE sizeof(G_MouseReportDescriptor)

// HID device extension
typedef struct _MOUSE_HID_EXTENSION {
    PHID_DEVICE_CONTEXT HidContext;
    WDFDEVICE Device;
    UCHAR InputReport[7];  // [Buttons+Padding][X][Y][Wheel][Pan] = 1+2+2+1+1 = 7 bytes
    WDFQUEUE PendingReportQueue;
} MOUSE_HID_EXTENSION, *PMOUSE_HID_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MOUSE_HID_EXTENSION, vhidmouseGetHidExtension)

// Forward declarations
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD vhidmouseEvtDeviceAdd;

// HID minidriver callbacks
NTSTATUS vhidmouseHidGetDeviceAttributes(_In_ PHID_DEVICE_CONTEXT Context, _Out_ PHID_DEVICE_ATTRIBUTES Attributes);
NTSTATUS vhidmouseHidGetReportDescriptor(_In_ PHID_DEVICE_CONTEXT Context, _Out_ UCHAR* Descriptor, _In_ ULONG Length, _Out_ ULONG* ActualLength);
NTSTATUS vhidmouseHidReadReport(_In_ PHID_DEVICE_CONTEXT Context, _In_ WDFREQUEST Request, _Out_ UCHAR* Report, _In_ ULONG Length, _Out_ ULONG* ActualLength);
NTSTATUS vhidmouseHidQueryReportType(_In_ PHID_DEVICE_CONTEXT Context, _In_ UCHAR ReportType, _Out_ PHIDP_REPORT_IDS ReportIds);

// HID minidriver registration
HID_MINIDRIVER_REGISTRATION G_MouseHidMinidriver = {
    .Revision = HID_REVISION,
    .DriverObject = NULL,
    .RegistryPath = NULL,
    .DeviceExtensionSize = sizeof(MOUSE_HID_EXTENSION),
    .ContextExtensionSize = sizeof(HID_DEVICE_CONTEXT),
    .Callbacks = {
        .Size = sizeof(HID_MINIDRIVER_CALLBACKS),
        .EvtHidDeviceGetDeviceAttributes = vhidmouseHidGetDeviceAttributes,
        .EvtHidDeviceGetReportDescriptor = vhidmouseHidGetReportDescriptor,
        .EvtHidDeviceReadReport = vhidmouseHidReadReport,
        .EvtHidDeviceQueryReportType = vhidmouseHidQueryReportType,
    },
    .Attributes = {
        .Size = sizeof(HID_DEVICE_ATTRIBUTES),
        .VendorID = 0x5A63,   // KVM
        .ProductID = 0x0002,
        .VersionNumber = 0x0101,
    }
};

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath) {
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;

    KdPrint(("vhidmouse: DriverEntry\n"));

    WDF_DRIVER_CONFIG_INIT(&config, vhidmouseEvtDeviceAdd);
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;

    G_MouseHidMinidriver.DriverObject = DriverObject;
    G_MouseHidMinidriver.RegistryPath = RegistryPath;

    status = HidRegisterMinidriver(&G_MouseHidMinidriver);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidmouse: HidRegisterMinidriver failed 0x%x\n", status));
        return status;
    }

    KdPrint(("vhidmouse: Registered as HID minidriver\n"));
    return STATUS_SUCCESS;
}

NTSTATUS vhidmouseEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit) {
    NTSTATUS status;
    WDFDEVICE device;
    PMOUSE_HID_EXTENSION hidExt;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES attributes;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    status = HidPnpDeviceInit(DeviceInit);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MOUSE_HID_EXTENSION);
    status = HidDeviceCreate(DeviceInit, &attributes, &G_MouseHidMinidriver, &device);
    if (!NT_SUCCESS(status)) return status;

    hidExt = vhidmouseGetHidExtension(device);
    hidExt->Device = device;
    hidExt->HidContext = HidContextFromDevice(device);
    RtlZeroMemory(hidExt->InputReport, sizeof(hidExt->InputReport));

    // Create I/O queue for IOCTLs
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = vhidmouseEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) return status;

    // Manual queue for pending reads
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &hidExt->PendingReportQueue);

    KdPrint(("vhidmouse: Device created\n"));
    return STATUS_SUCCESS;
}

// HID callback: Device attributes
NTSTATUS vhidmouseHidGetDeviceAttributes(_In_ PHID_DEVICE_CONTEXT Context, _Out_ PHID_DEVICE_ATTRIBUTES Attributes) {
    UNREFERENCED_PARAMETER(Context);
    RtlCopyMemory(Attributes, &G_MouseHidMinidriver.Attributes, sizeof(HID_DEVICE_ATTRIBUTES));
    return STATUS_SUCCESS;
}

// HID callback: Report descriptor
NTSTATUS vhidmouseHidGetReportDescriptor(_In_ PHID_DEVICE_CONTEXT Context, _Out_ UCHAR* Descriptor, 
    _In_ ULONG Length, _Out_ ULONG* ActualLength) {
    UNREFERENCED_PARAMETER(Context);
    if (Length < G_MOUSE_REPORT_DESC_SIZE) {
        *ActualLength = G_MOUSE_REPORT_DESC_SIZE;
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(Descriptor, G_MouseReportDescriptor, G_MOUSE_REPORT_DESC_SIZE);
    *ActualLength = G_MOUSE_REPORT_DESC_SIZE;
    return STATUS_SUCCESS;
}

// HID callback: Read report
NTSTATUS vhidmouseHidReadReport(_In_ PHID_DEVICE_CONTEXT Context, _In_ WDFREQUEST Request,
    _Out_ UCHAR* Report, _In_ ULONG Length, _Out_ ULONG* ActualLength) {
    PMOUSE_HID_EXTENSION hidExt;
    hidExt = vhidmouseGetHidExtension(WdfObjectContextGetObject(Context));

    if (Length < sizeof(hidExt->InputReport)) return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Report, hidExt->InputReport, sizeof(hidExt->InputReport));
    *ActualLength = sizeof(hidExt->InputReport);
    
    // Clear relative values (only buttons persist)
    hidExt->InputReport[1] = 0; // X low
    hidExt->InputReport[2] = 0; // X high
    hidExt->InputReport[3] = 0; // Y low
    hidExt->InputReport[4] = 0; // Y high
    hidExt->InputReport[5] = 0; // Wheel
    hidExt->InputReport[6] = 0; // Pan

    WdfRequestComplete(Request, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

// HID callback: Query report type
NTSTATUS vhidmouseHidQueryReportType(_In_ PHID_DEVICE_CONTEXT Context, _In_ UCHAR ReportType, _Out_ PHIDP_REPORT_IDS ReportIds) {
    UNREFERENCED_PARAMETER(Context);
    if (ReportType == HidP_Input) {
        ReportIds->ReportID = 0;
        ReportIds->InputLength = 7;
        ReportIds->OutputLength = 0;
        ReportIds->FeatureLength = 0;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

// Submit mouse report to HID stack
NTSTATUS vhidmouseSubmitHidReport(_In_ WDFDEVICE Device, _In_ SHORT X, _In_ SHORT Y, 
    _In_ UCHAR Buttons, _In_ CHAR Wheel, _In_ CHAR Pan) {
    PMOUSE_HID_EXTENSION hidExt = vhidmouseGetHidExtension(Device);

    // Build report: [Buttons][X][Y][Wheel][Pan]
    hidExt->InputReport[0] = Buttons & 0x1F;  // 5 buttons + 3 padding bits
    hidExt->InputReport[1] = (UCHAR)(X & 0xFF);
    hidExt->InputReport[2] = (UCHAR)((X >> 8) & 0xFF);
    hidExt->InputReport[3] = (UCHAR)(Y & 0xFF);
    hidExt->InputReport[4] = (UCHAR)((Y >> 8) & 0xFF);
    hidExt->InputReport[5] = (UCHAR)Wheel;
    hidExt->InputReport[6] = (UCHAR)Pan;

    KdPrint(("vhidmouse: Report [%02x %d %d %d %d]\n", Buttons, X, Y, Wheel, Pan));

    HidNotifyPresence(Device, TRUE);
    return STATUS_SUCCESS;
}

// Validate IOCTL buffer with comprehensive checks
static NTSTATUS ValidateMouseIoctlBuffer(
    _In_ WDFREQUEST Request,
    _In_ size_t ExpectedSize,
    _Out_ PVOID* Buffer,
    _Out_ size_t* ActualSize
)
{
    NTSTATUS status;
    
    if (ExpectedSize == 0) {
        *Buffer = NULL;
        *ActualSize = 0;
        return STATUS_SUCCESS;
    }
    
    status = WdfRequestRetrieveInputBuffer(Request, ExpectedSize, Buffer, ActualSize);
    
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidmouse: Buffer retrieval failed: 0x%X\n", status));
        return status;
    }
    
    if (*ActualSize < ExpectedSize) {
        KdPrint(("vhidmouse: Buffer too small: %zu < %zu\n", *ActualSize, ExpectedSize));
        return STATUS_BUFFER_TOO_SMALL;
    }
    
    return STATUS_SUCCESS;
}

// IOCTL handlers
VOID vhidmouseEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength, _In_ size_t InputBufferLength, _In_ ULONG IoControlCode) {
    UNREFERENCED_PARAMETER(OutputBufferLength);

    NTSTATUS status = STATUS_SUCCESS;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);

    // Log IOCTL for debugging
    KdPrint(("vhidmouse: IOCTL 0x%X, InputLen=%zu\n", IoControlCode, InputBufferLength));

    switch (IoControlCode) {
    case IOCTL_VMOUSE_MOVE_RELATIVE: {
        PVMOUSE_MOVE_DATA data;
        size_t size;
        if (InputBufferLength < sizeof(VMOUSE_MOVE_DATA)) {
            KdPrint(("vhidmouse: MOVE_REL buffer too small\n"));
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = ValidateMouseIoctlBuffer(Request, sizeof(VMOUSE_MOVE_DATA), (PVOID*)&data, &size);
        if (NT_SUCCESS(status)) {
            status = vhidmouseSubmitHidReport(device, (SHORT)data->X, (SHORT)data->Y, 0, 0, 0);
            KdPrint(("vhidmouse: Move rel (%d,%d) %s\n", data->X, data->Y, 
                NT_SUCCESS(status) ? "OK" : "FAIL"));
        }
        break;
    }

    case IOCTL_VMOUSE_MOVE_ABSOLUTE: {
        PVMOUSE_ABSOLUTE_DATA data;
        size_t size;
        if (InputBufferLength < sizeof(VMOUSE_ABSOLUTE_DATA)) {
            KdPrint(("vhidmouse: MOVE_ABS buffer too small\n"));
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = ValidateMouseIoctlBuffer(Request, sizeof(VMOUSE_ABSOLUTE_DATA), (PVOID*)&data, &size);
        if (NT_SUCCESS(status)) {
            SHORT relX = (SHORT)(data->X / 10);
            SHORT relY = (SHORT)(data->Y / 10);
            status = vhidmouseSubmitHidReport(device, relX, relY, 0, 0, 0);
            KdPrint(("vhidmouse: Move abs (%d,%d) %s\n", data->X, data->Y,
                NT_SUCCESS(status) ? "OK" : "FAIL"));
        }
        break;
    }

    case IOCTL_VMOUSE_BUTTON: {
        PVMOUSE_BUTTON_DATA data;
        size_t size;
        static UCHAR currentButtons = 0;
        if (InputBufferLength < sizeof(VMOUSE_BUTTON_DATA)) {
            KdPrint(("vhidmouse: BUTTON buffer too small\n"));
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = ValidateMouseIoctlBuffer(Request, sizeof(VMOUSE_BUTTON_DATA), (PVOID*)&data, &size);
        if (NT_SUCCESS(status)) {
            UCHAR buttonMask = 0;
            switch (data->Button) {
            case VMOUSE_BUTTON_LEFT: buttonMask = 0x01; break;
            case VMOUSE_BUTTON_RIGHT: buttonMask = 0x02; break;
            case VMOUSE_BUTTON_MIDDLE: buttonMask = 0x04; break;
            case VMOUSE_BUTTON_X1: buttonMask = 0x08; break;
            case VMOUSE_BUTTON_X2: buttonMask = 0x10; break;
            }
            if (data->Pressed) currentButtons |= buttonMask;
            else currentButtons &= ~buttonMask;
            vhidmouseSubmitHidReport(device, 0, 0, currentButtons, 0, 0);
        }
        break;
    }

    case IOCTL_VMOUSE_SCROLL: {
        PVMOUSE_SCROLL_DATA data;
        size_t size;
        if (InputBufferLength < sizeof(VMOUSE_SCROLL_DATA)) {
            KdPrint(("vhidmouse: SCROLL buffer too small\n"));
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = ValidateMouseIoctlBuffer(Request, sizeof(VMOUSE_SCROLL_DATA), (PVOID*)&data, &size);
        if (NT_SUCCESS(status)) {
            status = vhidmouseSubmitHidReport(device, 0, 0, 0, (CHAR)data->Vertical, (CHAR)data->Horizontal);
            KdPrint(("vhidmouse: Scroll (%d,%d) %s\n", data->Vertical, data->Horizontal,
                NT_SUCCESS(status) ? "OK" : "FAIL"));
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}
