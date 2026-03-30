/*
 * vhidkb_hid.c - HID Minidriver Implementation for Virtual Keyboard
 * 
 * This implements the HID minidriver callbacks to register with the Windows HID class driver
 * and submit actual HID reports that appear as real keyboard input.
 */

#include "vhidkb.h"
#include "vhidkb_ioctl.h"
#include <hidport.h>

// HID Report Descriptor for a standard boot keyboard
// This describes an 8-byte input report (modifier, reserved, 6 keycodes)
// and a 1-byte output report (LEDs)
UCHAR G_ReportDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0xE0,        //   Usage Minimum (224) - Left Ctrl
    0x29, 0xE7,        //   Usage Maximum (231) - Right GUI
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Var, Abs) - Modifier keys
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const) - Reserved byte
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (1 - Num Lock)
    0x29, 0x05,        //   Usage Maximum (5 - Kana)
    0x91, 0x02,        //   Output (Data, Var, Abs) - LED report
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Const) - LED padding
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x00,  //   Usage Maximum (255)
    0x81, 0x00,        //   Input (Data, Array) - Keycodes[6]
    0xC0               // End Collection
};

#define G_REPORT_DESCRIPTOR_SIZE sizeof(G_ReportDescriptor)

// HID device extension structure
typedef struct _HID_DEVICE_EXTENSION {
    PHID_DEVICE_CONTEXT HidContext;
    WDFDEVICE Device;
    UCHAR InputReport[8];      // Current input report
    UCHAR OutputReport[1];     // LED output report
    WDFQUEUE PendingReportQueue;  // Queue for pending read requests
} HID_DEVICE_EXTENSION, *PHID_DEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HID_DEVICE_EXTENSION, vhidkbGetHidExtension)

// Forward declarations
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD vhidkbEvtDeviceAdd;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT vhidkbEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP vhidkbEvtSelfManagedIoCleanup;

// HID minidriver callbacks
NTSTATUS vhidkbHidGetDeviceAttributes(
    _In_ PHID_DEVICE_CONTEXT Context,
    _Out_ PHID_DEVICE_ATTRIBUTES Attributes
);

NTSTATUS vhidkbHidGetReportDescriptor(
    _In_ PHID_DEVICE_CONTEXT Context,
    _Out_writes_bytes_to_(DescriptorLength, *ActualDescriptorLength) UCHAR* Descriptor,
    _In_ ULONG DescriptorLength,
    _Out_ ULONG* ActualDescriptorLength
);

NTSTATUS vhidkbHidReadReport(
    _In_ PHID_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _Out_writes_bytes_to_(ReportLength, *ActualReportLength) UCHAR* Report,
    _In_ ULONG ReportLength,
    _Out_ ULONG* ActualReportLength
);

NTSTATUS vhidkbHidWriteReport(
    _In_ PHID_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _In_reads_bytes_(ReportLength) UCHAR* Report,
    _In_ ULONG ReportLength
);

NTSTATUS vhidkbHidQueryReportType(
    _In_ PHID_DEVICE_CONTEXT Context,
    _In_ UCHAR ReportType,
    _Out_ PHIDP_REPORT_IDS ReportIds
);

// HID minidriver registration data
HID_MINIDRIVER_REGISTRATION G_HidMinidriverRegistration = {
    .Revision = HID_REVISION,
    .DriverObject = NULL,  // Set during DriverEntry
    .RegistryPath = NULL,  // Set during DriverEntry
    .DeviceExtensionSize = sizeof(HID_DEVICE_EXTENSION),
    .ContextExtensionSize = sizeof(HID_DEVICE_CONTEXT),
    .Callbacks = {
        .Size = sizeof(HID_MINIDRIVER_CALLBACKS),
        .EvtHidDeviceGetDeviceAttributes = vhidkbHidGetDeviceAttributes,
        .EvtHidDeviceGetReportDescriptor = vhidkbHidGetReportDescriptor,
        .EvtHidDeviceReadReport = vhidkbHidReadReport,
        .EvtHidDeviceWriteReport = vhidkbHidWriteReport,
        .EvtHidDeviceQueryReportType = vhidkbHidQueryReportType,
    },
    .Attributes = {
        .Size = sizeof(HID_DEVICE_ATTRIBUTES),
        .VendorID = 0x5A63,   // "Zc" - KVM
        .ProductID = 0x0001,
        .VersionNumber = 0x0101,
    }
};

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    KdPrint(("vhidkb: DriverEntry\n"));

    // Initialize WDF driver config
    WDF_DRIVER_CONFIG_INIT(&config, vhidkbEvtDeviceAdd);

    // Create WDF driver
    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE
    );

    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: WdfDriverCreate failed 0x%x\n", status));
        return status;
    }

    // Register as HID minidriver
    G_HidMinidriverRegistration.DriverObject = DriverObject;
    G_HidMinidriverRegistration.RegistryPath = RegistryPath;

    status = HidRegisterMinidriver(&G_HidMinidriverRegistration);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: HidRegisterMinidriver failed 0x%x\n", status));
        return status;
    }

    KdPrint(("vhidkb: Registered as HID minidriver\n"));

    return STATUS_SUCCESS;
}

NTSTATUS vhidkbEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    UNREFERENCED_PARAMETER(Driver);

    NTSTATUS status;
    WDFDEVICE device;
    PHID_DEVICE_EXTENSION hidExtension;
    PHID_DEVICE_CONTEXT hidContext;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    WDF_OBJECT_ATTRIBUTES attributes;

    KdPrint(("vhidkb: vhidkbEvtDeviceAdd\n"));

    // Initialize as HID device
    status = HidPnpDeviceInit(DeviceInit);
    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: HidPnpDeviceInit failed 0x%x\n", status));
        return status;
    }

    // Create device
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, HID_DEVICE_EXTENSION);
    status = HidDeviceCreate(
        DeviceInit,
        &attributes,
        &G_HidMinidriverRegistration,
        &device
    );

    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: HidDeviceCreate failed 0x%x\n", status));
        return status;
    }

    // Get HID extension
    hidExtension = vhidkbGetHidExtension(device);
    hidExtension->Device = device;
    RtlZeroMemory(hidExtension->InputReport, sizeof(hidExtension->InputReport));
    hidExtension->OutputReport[0] = 0;

    // Get HID context
    hidContext = HidContextFromDevice(device);
    hidExtension->HidContext = hidContext;

    // Create I/O queue for IOCTLs
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = vhidkbEvtIoDeviceControl;

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

    // Create queue for pending report reads
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(
        device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &hidExtension->PendingReportQueue
    );

    if (!NT_SUCCESS(status)) {
        KdPrint(("vhidkb: PendingReportQueue creation failed 0x%x\n", status));
        return status;
    }

    KdPrint(("vhidkb: Device created successfully\n"));

    return STATUS_SUCCESS;
}

// HID callback: Return device attributes
NTSTATUS vhidkbHidGetDeviceAttributes(
    _In_ PHID_DEVICE_CONTEXT Context,
    _Out_ PHID_DEVICE_ATTRIBUTES Attributes
)
{
    UNREFERENCED_PARAMETER(Context);

    KdPrint(("vhidkb: vhidkbHidGetDeviceAttributes\n"));

    RtlCopyMemory(Attributes, &G_HidMinidriverRegistration.Attributes, sizeof(HID_DEVICE_ATTRIBUTES));

    return STATUS_SUCCESS;
}

// HID callback: Return report descriptor
NTSTATUS vhidkbHidGetReportDescriptor(
    _In_ PHID_DEVICE_CONTEXT Context,
    _Out_writes_bytes_to_(DescriptorLength, *ActualDescriptorLength) UCHAR* Descriptor,
    _In_ ULONG DescriptorLength,
    _Out_ ULONG* ActualDescriptorLength
)
{
    UNREFERENCED_PARAMETER(Context);

    KdPrint(("vhidkb: vhidkbHidGetReportDescriptor\n"));

    if (DescriptorLength < G_REPORT_DESCRIPTOR_SIZE) {
        *ActualDescriptorLength = G_REPORT_DESCRIPTOR_SIZE;
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(Descriptor, G_ReportDescriptor, G_REPORT_DESCRIPTOR_SIZE);
    *ActualDescriptorLength = G_REPORT_DESCRIPTOR_SIZE;

    return STATUS_SUCCESS;
}

// HID callback: Handle read report request (from HID class driver)
NTSTATUS vhidkbHidReadReport(
    _In_ PHID_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _Out_writes_bytes_to_(ReportLength, *ActualReportLength) UCHAR* Report,
    _In_ ULONG ReportLength,
    _Out_ ULONG* ActualReportLength
)
{
    PHID_DEVICE_EXTENSION hidExtension;

    UNREFERENCED_PARAMETER(Context);

    hidExtension = vhidkbGetHidExtension(WdfObjectContextGetObject(Context));

    KdPrint(("vhidkb: vhidkbHidReadReport\n"));

    if (ReportLength < sizeof(hidExtension->InputReport)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    // Copy current input report
    RtlCopyMemory(Report, hidExtension->InputReport, sizeof(hidExtension->InputReport));
    *ActualReportLength = sizeof(hidExtension->InputReport);

    // Complete the request
    WdfRequestComplete(Request, STATUS_SUCCESS);

    return STATUS_SUCCESS;
}

// HID callback: Handle write report request (LEDs from HID class driver)
NTSTATUS vhidkbHidWriteReport(
    _In_ PHID_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _In_reads_bytes_(ReportLength) UCHAR* Report,
    _In_ ULONG ReportLength
)
{
    PHID_DEVICE_EXTENSION hidExtension;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Request);

    hidExtension = vhidkbGetHidExtension(WdfObjectContextGetObject(Context));

    KdPrint(("vhidkb: vhidkbHidWriteReport - LEDs: 0x%x\n", Report[0]));

    if (ReportLength >= sizeof(hidExtension->OutputReport)) {
        hidExtension->OutputReport[0] = Report[0];
    }

    return STATUS_SUCCESS;
}

// HID callback: Query report type
NTSTATUS vhidkbHidQueryReportType(
    _In_ PHID_DEVICE_CONTEXT Context,
    _In_ UCHAR ReportType,
    _Out_ PHIDP_REPORT_IDS ReportIds
)
{
    UNREFERENCED_PARAMETER(Context);

    KdPrint(("vhidkb: vhidkbHidQueryReportType - Type: %d\n", ReportType));

    switch (ReportType) {
    case HidP_Input:
        ReportIds->ReportID = 0;
        ReportIds->InputLength = 8;   // 8-byte input report
        ReportIds->OutputLength = 0;
        ReportIds->FeatureLength = 0;
        break;

    case HidP_Output:
        ReportIds->ReportID = 0;
        ReportIds->InputLength = 0;
        ReportIds->OutputLength = 1;   // 1-byte output report
        ReportIds->FeatureLength = 0;
        break;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

// Submit HID report to the HID class driver
NTSTATUS vhidkbSubmitHidReport(
    _In_ WDFDEVICE Device,
    _In_ UCHAR ModifierKeys,
    _In_ UCHAR* KeyCodes,
    _In_ UCHAR KeyCount
)
{
    PHID_DEVICE_EXTENSION hidExtension;
    UCHAR report[8];

    hidExtension = vhidkbGetHidExtension(Device);

    // Build boot keyboard report
    report[0] = ModifierKeys;
    report[1] = 0;  // Reserved
    
    UCHAR keysToCopy = (KeyCount > 6) ? 6 : KeyCount;
    for (UCHAR i = 0; i < keysToCopy; i++) {
        report[2 + i] = KeyCodes[i];
    }
    for (UCHAR i = keysToCopy; i < 6; i++) {
        report[2 + i] = 0;
    }

    // Store the report
    RtlCopyMemory(hidExtension->InputReport, report, sizeof(report));

    KdPrint(("vhidkb: Submitting report [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
        report[0], report[1], report[2], report[3],
        report[4], report[5], report[6], report[7]));

    // Notify HID class driver that report is available
    // This causes the HID class driver to call our vhidkbHidReadReport callback
    HidNotifyPresence(Device, TRUE);

    return STATUS_SUCCESS;
}

// Validate IOCTL buffer with comprehensive checks
static NTSTATUS ValidateIoctlBuffer(
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
        KdPrint(("vhidkb: Buffer retrieval failed: 0x%X\n", status));
        return status;
    }
    
    if (*ActualSize < ExpectedSize) {
        KdPrint(("vhidkb: Buffer too small: %zu < %zu\n", *ActualSize, ExpectedSize));
        return STATUS_BUFFER_TOO_SMALL;
    }
    
    // Validate buffer is readable
    __try {
        volatile UCHAR test = ((PUCHAR)*Buffer)[0];
        UNREFERENCED_PARAMETER(test);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("vhidkb: Buffer access violation\n"));
        return STATUS_ACCESS_VIOLATION;
    }
    
    return STATUS_SUCCESS;
}

// IOCTL handler for key injection
VOID vhidkbEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);

    NTSTATUS status = STATUS_SUCCESS;
    WDFDEVICE device;
    PVKB_INPUT_REPORT inputReport;
    size_t bufferSize;

    device = WdfIoQueueGetDevice(Queue);

    // Log IOCTL for debugging
    KdPrint(("vhidkb: IOCTL 0x%X, InputLen=%zu\n", IoControlCode, InputBufferLength));

    switch (IoControlCode) {
    case IOCTL_VKB_INJECT_KEYDOWN:
        // Validate input buffer size first
        if (InputBufferLength < sizeof(VKB_INPUT_REPORT)) {
            KdPrint(("vhidkb: KEYDOWN buffer too small: %zu < %zu\n", 
                InputBufferLength, sizeof(VKB_INPUT_REPORT)));
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        
        status = ValidateIoctlBuffer(Request, sizeof(VKB_INPUT_REPORT), 
            (PVOID*)&inputReport, &bufferSize);
        if (NT_SUCCESS(status)) {
            status = vhidkbSubmitHidReport(
                device,
                inputReport->ModifierKeys,
                inputReport->KeyCodes,
                VKB_MAX_KEYS
            );
            if (NT_SUCCESS(status)) {
                KdPrint(("vhidkb: Key injected successfully\n"));
            } else {
                KdPrint(("vhidkb: Key injection failed: 0x%X\n", status));
            }
        }
        break;

    case IOCTL_VKB_INJECT_KEYUP:
        // Send empty report (all keys released)
        {
            UCHAR emptyKeys[VKB_MAX_KEYS] = {0};
            status = vhidkbSubmitHidReport(device, 0, emptyKeys, 0);
        }
        break;

    case IOCTL_VKB_RESET:
        // Send empty report
        {
            UCHAR emptyKeys[VKB_MAX_KEYS] = {0};
            status = vhidkbSubmitHidReport(device, 0, emptyKeys, 0);
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}
