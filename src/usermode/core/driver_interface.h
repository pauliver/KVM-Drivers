#pragma once
#include <windows.h>
#include <vector>
#include <mutex>
#include <atomic>
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

    // Keyboard (HID usage codes — used by the web client)
    bool InjectKeyDown(UCHAR keyCode, UCHAR modifiers = 0);
    bool InjectKeyUp(UCHAR keyCode, UCHAR modifiers = 0);
    bool InjectKeyCombo(const std::vector<std::pair<UCHAR, UCHAR>>& keys);
    // Windows Virtual Key injection (used by VNC: X11 keysyms are already mapped to VK codes)
    // Goes straight to SendInput — the vhidkb driver speaks HID usage codes, not VK codes.
    bool InjectVirtualKey(WORD vk, bool keyUp, bool extended = false);

    // Mouse
    bool InjectMouseMove(LONG x, LONG y, bool absolute = false);
    bool InjectMouseButton(UCHAR button, bool pressed);
    bool InjectMouseScroll(int vertical, int horizontal = 0);

    // Controller
    bool InjectControllerReport(const XUSB_REPORT& report);  // legacy: injects to slot 0
    bool InjectControllerReportSlot(int slot, const XUSB_REPORT& report);
    int  ClaimControllerSlot();                               // returns 0-3 or -1 if full
    void ReleaseControllerSlot(int slot);
    bool IsControllerSlotClaimed(int slot) const;
    bool SetControllerRumble(UCHAR leftMotor, UCHAR rightMotor);

    // Display
    bool CaptureFrame(void** frameData, size_t* size);
    bool GetDisplayInfo(UINT* width, UINT* height, UINT* refreshRate);
    bool SetDisplayResolution(int width, int height);

private:
    HANDLE keyboardHandle;
    HANDLE mouseHandle;
    HANDLE controllerHandle;         // single-slot legacy handle (slot 0)
    HANDLE displayHandle;
    std::atomic<bool> useDriverInjection;
    mutable std::mutex handleMutex_;  // Protects all HANDLE members
    bool   controllerSlotClaimed_[4] = {};  // which slots are currently in use
};

// HID to Virtual Key conversion helper
WORD HidToVirtualKey(UCHAR hidKeyCode);
bool SendKeyWithModifiers(WORD vkCode, UCHAR modifiers, bool keyUp);
