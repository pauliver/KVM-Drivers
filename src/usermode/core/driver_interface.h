#pragma once
#include <windows.h>
#include <vector>
#include "../../../drivers/vhidkb/vhidkb_ioctl.h"
#include "../../../drivers/vhidmouse/vhidmouse_ioctl.h"
#include "../../../drivers/vxinput/vxinput.h"

// C++ interface for driver communication with SendInput fallback
class DriverInterface {
public:
    DriverInterface();
    ~DriverInterface();

    bool Initialize();
    void Disconnect();

    // Check if using driver or SendInput
    bool IsDriverInjectionAvailable() const;

    // Keyboard
    bool InjectKeyDown(UCHAR keyCode, UCHAR modifiers = 0);
    bool InjectKeyUp(UCHAR keyCode, UCHAR modifiers = 0);
    bool InjectKeyCombo(const std::vector<std::pair<UCHAR, UCHAR>>& keys);

    // Mouse
    bool InjectMouseMove(LONG x, LONG y, bool absolute = false);
    bool InjectMouseButton(UCHAR button, bool pressed);
    bool InjectMouseScroll(int vertical, int horizontal = 0);

    // Controller
    bool InjectControllerReport(const XUSB_REPORT& report);
    bool SetControllerRumble(UCHAR leftMotor, UCHAR rightMotor);

    // Display
    bool CaptureFrame(void** frameData, size_t* size);
    bool GetDisplayInfo(UINT* width, UINT* height, UINT* refreshRate);

private:
    HANDLE keyboardHandle;
    HANDLE mouseHandle;
    HANDLE controllerHandle;
    HANDLE displayHandle;
    bool useDriverInjection;
};

// HID to Virtual Key conversion helper
WORD HidToVirtualKey(UCHAR hidKeyCode);
bool SendKeyWithModifiers(WORD vkCode, UCHAR modifiers, bool keyUp);
