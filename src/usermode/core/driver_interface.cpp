// driver_interface.cpp - Communication with kernel drivers and SendInput fallback
#include "driver_interface.h"
#include <iostream>
#include <Windows.h>

// Fallback to SendInput when drivers not loaded
bool UseSendInputFallback = true;

DriverInterface::DriverInterface() 
    : keyboardHandle(INVALID_HANDLE_VALUE)
    , mouseHandle(INVALID_HANDLE_VALUE)
    , controllerHandle(INVALID_HANDLE_VALUE)
    , displayHandle(INVALID_HANDLE_VALUE)
    , useDriverInjection(false) {
}

DriverInterface::~DriverInterface() {
    Disconnect();
}

bool DriverInterface::Initialize() {
    // Try to open driver handles first
    keyboardHandle = CreateFile(
        L"\\\\.\\vhidkb",
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    mouseHandle = CreateFile(
        L"\\\\.\\vhidmouse",
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    controllerHandle = CreateFile(
        L"\\\\.\\vxinput",
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    displayHandle = CreateFile(
        L"\\\\.\\vdisplay",
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    // Determine if we can use driver injection
    useDriverInjection = (keyboardHandle != INVALID_HANDLE_VALUE && mouseHandle != INVALID_HANDLE_VALUE);

    // Log which drivers are available
    if (keyboardHandle != INVALID_HANDLE_VALUE) {
        std::wcout << L"[DriverInterface] Keyboard driver connected" << std::endl;
    } else if (UseSendInputFallback) {
        std::wcout << L"[DriverInterface] Keyboard driver not available, will use SendInput fallback" << std::endl;
    }
    
    if (mouseHandle != INVALID_HANDLE_VALUE) {
        std::wcout << L"[DriverInterface] Mouse driver connected" << std::endl;
    } else if (UseSendInputFallback) {
        std::wcout << L"[DriverInterface] Mouse driver not available, will use SendInput fallback" << std::endl;
    }
    
    if (controllerHandle != INVALID_HANDLE_VALUE) {
        std::wcout << L"[DriverInterface] Controller driver connected" << std::endl;
    }
    
    if (displayHandle != INVALID_HANDLE_VALUE) {
        std::wcout << L"[DriverInterface] Display driver connected" << std::endl;
    }

    return true;
}

void DriverInterface::Disconnect() {
    if (keyboardHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(keyboardHandle);
        keyboardHandle = INVALID_HANDLE_VALUE;
    }
    if (mouseHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(mouseHandle);
        mouseHandle = INVALID_HANDLE_VALUE;
    }
    if (controllerHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(controllerHandle);
        controllerHandle = INVALID_HANDLE_VALUE;
    }
    if (displayHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(displayHandle);
        displayHandle = INVALID_HANDLE_VALUE;
    }
    useDriverInjection = false;
}

// Convert HID key code to virtual key code
WORD HidToVirtualKey(UCHAR hidKeyCode) {
    // HID Usage Table to Windows Virtual Key mapping
    // This is a simplified mapping - full mapping would be more comprehensive
    static const WORD hidToVk[] = {
        0, 0, 0, 0,              // 0x00-0x03: Reserved
        'A', 'B', 'C', 'D',      // 0x04-0x07: a,b,c,d
        'E', 'F', 'G', 'H',      // 0x08-0x0B: e,f,g,h
        'I', 'J', 'K', 'L',      // 0x0C-0x0F: i,j,k,l
        'M', 'N', 'O', 'P',      // 0x10-0x13: m,n,o,p
        'Q', 'R', 'S', 'T',      // 0x14-0x17: q,r,s,t
        'U', 'V', 'W', 'X',      // 0x18-0x1B: u,v,w,x
        'Y', 'Z', '1', '2',      // 0x1C-0x1F: y,z,1,2
        '3', '4', '5', '6',      // 0x20-0x23: 3,4,5,6
        '7', '8', '9', '0',      // 0x24-0x27: 7,8,9,0
        VK_RETURN, VK_ESCAPE, VK_BACK, VK_TAB,  // 0x28-0x2B: Enter,Esc,Backspace,Tab
        VK_SPACE, 0xBD, 0xBB, 0xDB,  // 0x2C-0x2F: Space,-,=,[
        0xDD, 0xDC, 0xBA, 0xDE,  // 0x30-0x33: ],\,;,',
        0xC0, 0xBC, 0xBE, 0xBF,  // 0x34-0x37: `,,,.,/
        VK_CAPITAL, VK_F1, VK_F2, VK_F3,  // 0x38-0x3B: CapsLock,F1-F3
        VK_F4, VK_F5, VK_F6, VK_F7,      // 0x3C-0x3F: F4-F7
        VK_F8, VK_F9, VK_F10, VK_F11,    // 0x40-0x43: F8-F11
        VK_F12, VK_PRINT, VK_SCROLL, VK_PAUSE,  // 0x44-0x47: F12,PrtSc,Scroll,Pause
        VK_INSERT, VK_HOME, VK_PRIOR, VK_DELETE, // 0x48-0x4B: Insert,Home,PageUp,Delete
        VK_END, VK_NEXT, VK_RIGHT, VK_LEFT,      // 0x4C-0x4F: End,PageDown,Right,Left
        VK_DOWN, VK_UP                           // 0x50-0x51: Down,Up
    };
    
    if (hidKeyCode < sizeof(hidToVk) / sizeof(hidToVk[0])) {
        return hidToVk[hidKeyCode];
    }
    
    // Extended keys
    switch (hidKeyCode) {
        case 0xE0: return VK_LCONTROL;
        case 0xE1: return VK_LSHIFT;
        case 0xE2: return VK_LMENU;  // Left Alt
        case 0xE3: return VK_LWIN;
        case 0xE4: return VK_RCONTROL;
        case 0xE5: return VK_RSHIFT;
        case 0xE6: return VK_RMENU;  // Right Alt
        case 0xE7: return VK_RWIN;
    }
    
    return 0;
}

// Map HID modifiers to SendInput flags
void MapHidModifiers(UCHAR hidModifiers, INPUT& input) {
    if (hidModifiers & VKB_MOD_LEFT_CTRL)  input.ki.dwFlags |= 0;
    if (hidModifiers & VKB_MOD_LEFT_SHIFT) input.ki.dwFlags |= 0;
    if (hidModifiers & VKB_MOD_LEFT_ALT)    input.ki.dwFlags |= 0;
    if (hidModifiers & VKB_MOD_LEFT_GUI)    input.ki.dwFlags |= 0;
}

bool SendKeyWithModifiers(WORD vkCode, UCHAR modifiers, bool keyUp) {
    INPUT inputs[8] = {};
    int inputCount = 0;
    
    // Press modifier keys first (if not keyUp)
    if (!keyUp) {
        if (modifiers & VKB_MOD_LEFT_CTRL) {
            inputs[inputCount].type = INPUT_KEYBOARD;
            inputs[inputCount].ki.wVk = VK_LCONTROL;
            inputCount++;
        }
        if (modifiers & VKB_MOD_LEFT_SHIFT) {
            inputs[inputCount].type = INPUT_KEYBOARD;
            inputs[inputCount].ki.wVk = VK_LSHIFT;
            inputCount++;
        }
        if (modifiers & VKB_MOD_LEFT_ALT) {
            inputs[inputCount].type = INPUT_KEYBOARD;
            inputs[inputCount].ki.wVk = VK_LMENU;
            inputCount++;
        }
        if (modifiers & VKB_MOD_LEFT_GUI) {
            inputs[inputCount].type = INPUT_KEYBOARD;
            inputs[inputCount].ki.wVk = VK_LWIN;
            inputCount++;
        }
    }
    
    // Main key
    inputs[inputCount].type = INPUT_KEYBOARD;
    inputs[inputCount].ki.wVk = vkCode;
    if (keyUp) {
        inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    inputCount++;
    
    // Release modifier keys (if keyUp)
    if (keyUp) {
        if (modifiers & VKB_MOD_LEFT_GUI) {
            inputs[inputCount].type = INPUT_KEYBOARD;
            inputs[inputCount].ki.wVk = VK_LWIN;
            inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
            inputCount++;
        }
        if (modifiers & VKB_MOD_LEFT_ALT) {
            inputs[inputCount].type = INPUT_KEYBOARD;
            inputs[inputCount].ki.wVk = VK_LMENU;
            inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
            inputCount++;
        }
        if (modifiers & VKB_MOD_LEFT_SHIFT) {
            inputs[inputCount].type = INPUT_KEYBOARD;
            inputs[inputCount].ki.wVk = VK_LSHIFT;
            inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
            inputCount++;
        }
        if (modifiers & VKB_MOD_LEFT_CTRL) {
            inputs[inputCount].type = INPUT_KEYBOARD;
            inputs[inputCount].ki.wVk = VK_LCONTROL;
            inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
            inputCount++;
        }
    }
    
    UINT result = SendInput(inputCount, inputs, sizeof(INPUT));
    return (result == (UINT)inputCount);
}

bool DriverInterface::InjectKeyDown(UCHAR keyCode, UCHAR modifiers) {
    // Try driver first if available
    if (useDriverInjection && keyboardHandle != INVALID_HANDLE_VALUE) {
        VKB_INPUT_REPORT report = {};
        report.ModifierKeys = modifiers;
        report.KeyCodes[0] = keyCode;

        DWORD bytesReturned;
        BOOL result = DeviceIoControl(
            keyboardHandle,
            IOCTL_VKB_INJECT_KEYDOWN,
            &report,
            sizeof(report),
            NULL,
            0,
            &bytesReturned,
            NULL
        );
        
        if (result) return true;
        // Fall through to SendInput if driver fails
    }
    
    // Fallback to SendInput
    if (UseSendInputFallback) {
        WORD vkCode = HidToVirtualKey(keyCode);
        if (vkCode != 0) {
            return SendKeyWithModifiers(vkCode, modifiers, false);
        }
    }
    
    return false;
}

bool DriverInterface::InjectKeyUp(UCHAR keyCode, UCHAR modifiers) {
    // Try driver first if available
    if (useDriverInjection && keyboardHandle != INVALID_HANDLE_VALUE) {
        VKB_INPUT_REPORT report = {};
        report.ModifierKeys = modifiers;
        report.KeyCodes[0] = keyCode;

        DWORD bytesReturned;
        BOOL result = DeviceIoControl(
            keyboardHandle,
            IOCTL_VKB_INJECT_KEYUP,
            &report,
            sizeof(report),
            NULL,
            0,
            &bytesReturned,
            NULL
        );
        
        if (result) return true;
    }
    
    // Fallback to SendInput
    if (UseSendInputFallback) {
        WORD vkCode = HidToVirtualKey(keyCode);
        if (vkCode != 0) {
            return SendKeyWithModifiers(vkCode, modifiers, true);
        }
    }
    
    return false;
}

bool DriverInterface::InjectKeyCombo(const std::vector<std::pair<UCHAR, UCHAR>>& keys) {
    // Press all keys
    for (const auto& key : keys) {
        if (!InjectKeyDown(key.first, key.second)) {
            return false;
        }
    }
    
    // Small delay to ensure keys register
    Sleep(50);
    
    // Release all keys in reverse order
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        if (!InjectKeyUp(it->first, it->second)) {
            return false;
        }
    }
    
    return true;
}

bool DriverInterface::InjectMouseMove(LONG x, LONG y, bool absolute) {
    // Try driver first if available
    if (useDriverInjection && mouseHandle != INVALID_HANDLE_VALUE) {
        DWORD ioctl = absolute ? IOCTL_VMOUSE_MOVE_ABSOLUTE : IOCTL_VMOUSE_MOVE_RELATIVE;
        
        if (absolute) {
            VMOUSE_ABSOLUTE_DATA data = {};
            data.X = x;
            data.Y = y;
            data.ScreenWidth = GetSystemMetrics(SM_CXSCREEN);
            data.ScreenHeight = GetSystemMetrics(SM_CYSCREEN);
            
            DWORD bytesReturned;
            BOOL result = DeviceIoControl(mouseHandle, ioctl, &data, sizeof(data), NULL, 0, &bytesReturned, NULL);
            if (result) return true;
        } else {
            VMOUSE_MOVE_DATA data = {};
            data.X = x;
            data.Y = y;
            
            DWORD bytesReturned;
            BOOL result = DeviceIoControl(mouseHandle, ioctl, &data, sizeof(data), NULL, 0, &bytesReturned, NULL);
            if (result) return true;
        }
    }
    
    // Fallback to SendInput
    if (UseSendInputFallback) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        
        if (absolute) {
            // Convert to absolute coordinates (0-65535)
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);
            input.mi.dx = (x * 65535) / (screenWidth - 1);
            input.mi.dy = (y * 65535) / (screenHeight - 1);
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        } else {
            input.mi.dx = x;
            input.mi.dy = y;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
        }
        
        return SendInput(1, &input, sizeof(INPUT)) == 1;
    }
    
    return false;
}

bool DriverInterface::InjectMouseButton(UCHAR button, bool pressed) {
    // Try driver first if available
    if (useDriverInjection && mouseHandle != INVALID_HANDLE_VALUE) {
        VMOUSE_BUTTON_DATA data = {};
        data.Button = button;
        data.Pressed = pressed;

        DWORD bytesReturned;
        BOOL result = DeviceIoControl(
            mouseHandle,
            IOCTL_VMOUSE_BUTTON,
            &data,
            sizeof(data),
            NULL,
            0,
            &bytesReturned,
            NULL
        );
        
        if (result) return true;
    }
    
    // Fallback to SendInput
    if (UseSendInputFallback) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        
        switch (button) {
            case VMOUSE_BUTTON_LEFT:
                input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                break;
            case VMOUSE_BUTTON_RIGHT:
                input.mi.dwFlags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                break;
            case VMOUSE_BUTTON_MIDDLE:
                input.mi.dwFlags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                break;
            case VMOUSE_BUTTON_X1:
            case VMOUSE_BUTTON_X2:
                // X buttons need XBUTTON1/XBUTTON2 constants
                input.mi.dwFlags = pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
                input.mi.mouseData = (button == VMOUSE_BUTTON_X1) ? XBUTTON1 : XBUTTON2;
                break;
            default:
                return false;
        }
        
        return SendInput(1, &input, sizeof(INPUT)) == 1;
    }
    
    return false;
}

bool DriverInterface::InjectMouseScroll(int vertical, int horizontal) {
    // Try driver first if available
    if (useDriverInjection && mouseHandle != INVALID_HANDLE_VALUE) {
        VMOUSE_SCROLL_DATA data = {};
        data.Vertical = vertical;
        data.Horizontal = horizontal;

        DWORD bytesReturned;
        BOOL result = DeviceIoControl(
            mouseHandle,
            IOCTL_VMOUSE_SCROLL,
            &data,
            sizeof(data),
            NULL,
            0,
            &bytesReturned,
            NULL
        );
        
        if (result) return true;
    }
    
    // Fallback to SendInput
    if (UseSendInputFallback) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = vertical * WHEEL_DELTA;
        
        return SendInput(1, &input, sizeof(INPUT)) == 1;
    }
    
    return false;
}

bool DriverInterface::InjectControllerReport(const XUSB_REPORT& report) {
    if (controllerHandle == INVALID_HANDLE_VALUE) return false;

    // TODO: Send XUSB report via IOCTL
    UNREFERENCED_PARAMETER(report);
    return false;
}

bool DriverInterface::IsDriverInjectionAvailable() const {
    return useDriverInjection;
}
