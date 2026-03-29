#pragma once
#include <ntddk.h>
#include <wdf.h>

#define POOL_TAG 'xniV'

// IOCTL codes
#define FILE_DEVICE_VXINPUT 0x8001
#define IOCTL_VXINPUT_CREATE_CONTROLLER     CTL_CODE(FILE_DEVICE_VXINPUT, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VXINPUT_REMOVE_CONTROLLER   CTL_CODE(FILE_DEVICE_VXINPUT, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VXINPUT_SUBMIT_REPORT       CTL_CODE(FILE_DEVICE_VXINPUT, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VXINPUT_GET_RUMBLE          CTL_CODE(FILE_DEVICE_VXINPUT, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VXINPUT_GET_CONTROLLER_COUNT CTL_CODE(FILE_DEVICE_VXINPUT, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Xbox 360 input report (XUSB format)
typedef struct _XUSB_REPORT {
    BYTE bReportId;      // 0x00
    BYTE bSize;          // sizeof(XUSB_REPORT) = 20
    USHORT wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
    // Padding to match XUSB protocol
    ULONG dwPaddingUnused;
} XUSB_REPORT, *PXUSB_REPORT;

// Digital button masks
#define XUSB_GAMEPAD_DPAD_UP        0x0001
#define XUSB_GAMEPAD_DPAD_DOWN      0x0002
#define XUSB_GAMEPAD_DPAD_LEFT      0x0004
#define XUSB_GAMEPAD_DPAD_RIGHT     0x0008
#define XUSB_GAMEPAD_START          0x0010
#define XUSB_GAMEPAD_BACK           0x0020
#define XUSB_GAMEPAD_LEFT_THUMB     0x0040
#define XUSB_GAMEPAD_RIGHT_THUMB    0x0080
#define XUSB_GAMEPAD_LEFT_SHOULDER  0x0100
#define XUSB_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XUSB_GAMEPAD_A              0x1000
#define XUSB_GAMEPAD_B              0x2000
#define XUSB_GAMEPAD_X              0x4000
#define XUSB_GAMEPAD_Y              0x8000

// Rumble state (output from games)
typedef struct _XUSB_RUMBLE_STATE {
    BYTE bLeftTriggerMotor;
    BYTE bRightTriggerMotor;
    BYTE bLeftMotor;
    BYTE bRightMotor;
} XUSB_RUMBLE_STATE, *PXUSB_RUMBLE_STATE;

// Controller creation info
typedef struct _XUSB_CONTROLLER_INFO {
    ULONG UserIndex;        // Player number (0-3)
    HANDLE ControllerHandle; // Out: Handle to controller context
} XUSB_CONTROLLER_INFO, *PXUSB_CONTROLLER_INFO;

// Bus context (one per driver)
typedef struct _VXINPUT_BUS_CONTEXT {
    WDFDEVICE BusDevice;
    LIST_ENTRY ControllerList;
    KSPIN_LOCK ControllerListLock;
    ULONG ControllerCount;
    ULONG NextControllerIndex;
} VXINPUT_BUS_CONTEXT, *PVXINPUT_BUS_CONTEXT;

// Controller context (one per virtual gamepad)
typedef struct _VXINPUT_CONTROLLER_CONTEXT {
    LIST_ENTRY ListEntry;
    WDFDEVICE ControllerDevice;
    PVXINPUT_BUS_CONTEXT BusContext;
    ULONG ControllerIndex;
    ULONG UserIndex;
    BOOLEAN IsActive;
    XUSB_REPORT CurrentReport;
    XUSB_RUMBLE_STATE RumbleState;
} VXINPUT_CONTROLLER_CONTEXT, *PVXINPUT_CONTROLLER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VXINPUT_BUS_CONTEXT, vxinputGetBusContext)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VXINPUT_CONTROLLER_CONTEXT, vxinputGetControllerContext)

// Driver callbacks
NTSTATUS vxinputEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit);
VOID vxinputEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
);

// Controller management
NTSTATUS vxinputCreateController(
    _In_ PVXINPUT_BUS_CONTEXT BusContext,
    _In_ PXUSB_CONTROLLER_INFO ControllerInfo,
    _Out_ PVXINPUT_CONTROLLER_CONTEXT* ControllerContext
);
NTSTATUS vxinputRemoveController(_In_ PVXINPUT_CONTROLLER_CONTEXT ControllerContext);
NTSTATUS vxinputSubmitReport(_In_ PVXINPUT_CONTROLLER_CONTEXT ControllerContext, _In_ PXUSB_REPORT Report);
PVXINPUT_CONTROLLER_CONTEXT vxinputGetControllerByIndex(
    _In_ PVXINPUT_BUS_CONTEXT BusContext,
    _In_ ULONG Index
);
VOID vxinputCleanupControllers(_In_ PVXINPUT_BUS_CONTEXT BusContext);

// Legacy structure for backward compatibility
typedef struct _CONTROLLER_CONTEXT {
    WDFDEVICE Device;
    XUSB_REPORT CurrentReport;
    ULONG ControllerIndex;
} CONTROLLER_CONTEXT, *PCONTROLLER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONTROLLER_CONTEXT, vxinputGetContext)
