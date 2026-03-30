# Building and Testing KVM-Drivers

## Prerequisites

| Component | Version | Required For |
|-----------|---------|--------------|
| Windows | 10/11 64-bit | Building, running drivers |
| Visual Studio | 2022 Community+ | Compilation |
| Windows Driver Kit | Windows 11 WDK | Kernel drivers |
| Windows SDK | 10.0.22621.0+ | Headers, libraries |
| .NET 8 SDK | 8.0.x | Tray application |
| Git | 2.35+ | Source control |

### Installing Prerequisites

**Visual Studio 2022 Workloads:**
- Desktop development with C++
- Windows Driver development

**WDK Installation:**
```powershell
# Download from Microsoft and install
# https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
```

## Building

### Quick Build (Command Line)

```powershell
cd P:\KVM-Drivers
.\scripts\build.bat Release all
```

### Build Options

| Command | Description |
|---------|-------------|
| `build.bat Release all` | Build everything (Release) |
| `build.bat Debug all` | Build everything (Debug) |
| `build.bat Release drivers` | Build kernel drivers only |
| `build.bat Release usermode` | Build applications only |
| `build.bat Release tests` | Build test suite only |
| `build.bat clean` | Clean all build artifacts |

### Visual Studio Build

1. Open `KVM-Drivers.sln` in Visual Studio 2022
2. Select Configuration: `Release` or `Debug`
3. Select Platform: `x64`
4. Build → Build Solution (Ctrl+Shift+B)

### Build Output Structure

```
build\Release\
├── drivers\             # Kernel drivers (.sys, .inf)
│   ├── vhidkb.sys
│   ├── vhidmouse.sys
│   ├── vxinput.sys
│   └── vdisplay.dll
├── bin\                 # Applications
│   ├── KVMService.exe
│   ├── KVMTray.exe
│   └── remote.dll
├── tests\               # Test executables
│   ├── test_keyboard.exe
│   ├── test_mouse.exe
│   ├── test_harness.exe
│   └── stress_test.exe
└── logs\                # Build logs
    ├── vhidkb.log
    └── ...
```

### User-Data Paths (Runtime)

```
%LOCALAPPDATA%\KVM-Drivers\
├── settings.json              # Persisted tray settings
├── trusted_clients.txt        # Approved remote client IPs + expiry
├── audit_log.csv              # Per-connection ETW audit trail
└── pending_approvals\         # File-IPC between C++ servers and C# tray
    ├── <UUID>.request         # Written by C++ server for new clients
    └── <UUID>.result          # Written by C# tray with decision
```

## Installing Drivers

**⚠️ WARNING: Requires Administrator privileges**

### Install All Drivers

```powershell
# As Administrator
.\scripts\install.bat Release
```

### Install Individual Drivers

```powershell
# Keyboard only
pnputil /add-driver build\Release\drivers\vhidkb.inf /install

# Mouse only
pnputil /add-driver build\Release\drivers\vhidmouse.inf /install

# Controller only
pnputil /add-driver build\Release\drivers\vxinput.inf /install

# Display driver (IDD)
regsvr32 build\Release\drivers\vdisplay.dll
```

### Uninstall Drivers

```powershell
.\scripts\uninstall.bat
```

### Verify Installation

```powershell
# Check installed drivers
pnputil /enum-devices /class HIDClass

# Check in Device Manager
# - Virtual HID Keyboard
# - Virtual HID Mouse
# - Virtual Xbox Controller
# - Virtual Display Monitor
```

## Testing

### Automated Test Harness

```powershell
cd build\Release
tests\test_harness.exe
```

**Test Coverage:**
- Keyboard single key injection
- Keyboard modifier combos (Ctrl+C, etc.)
- Mouse relative/absolute movement
- Mouse button clicks
- Mouse scroll wheel
- Controller XUSB reports

### Manual Keyboard Test

```powershell
cd build\Release
tests\test_keyboard.exe
```

Menu options:
1. Inject 'A' key
2. Inject 'Enter' key
3. Inject Ctrl+C combo
4. Reset keyboard state
5. Exit

### Manual Mouse Test

```powershell
cd build\Release
tests\test_mouse.exe
```

Menu options:
1. Move cursor right 100px
2. Move cursor left 100px
3. Left click
4. Scroll up 3 clicks
5. Move to screen center
6. Exit

### VNC Server Test

```powershell
# Start VNC server
cd build\Release\bin
KVMService.exe --vnc-port 5900

# Connect with any VNC client to localhost:5900
# RFB 3.8: offers VNCAuth (if password set) or None
# AnonTLS: enabled via Settings → VNC Security → Enable AnonTLS
```

### Stress Tests

```powershell
cd build\Release\tests

# Quick smoke test (1 hour)
stress_test.exe --hours 1 --output results_1h.json

# Full 72-hour stability test
stress_test.exe --72h --output results_72h.json

# Options
#  --hours N               Duration in hours (default 12)
#  --rate N                Events/second (default 100)
#  --72h                   72h preset at 60 events/sec
#  --single                Single thread instead of keyboard+mouse+controller
#  --no-watchdog           Disable hang detection
#  --watchdog-timeout N    Seconds before watchdog fires (default 30)
#  --output FILE           Write JSON result file
```

**Passing criteria**: RESULT: PASSED (error rate < 0.1%, no watchdog fires, no driver reconnects)

### WebSocket Protocol Test

```powershell
# Connect via wscat or similar
wscat -c ws://localhost:8443
> {"jsonrpc":"2.0","method":"system.ping","id":1}
```

## Running the System

### Start Core Service

```powershell
# As Administrator (for driver access)
cd build\Release\bin
KVMService.exe
```

Service options:
```
KVMService.exe [options]
  --daemon          Run as Windows service
  --vnc-port N      VNC server port (default: 5900)
  --ws-port N       WebSocket port (default: 8443)
  --install         Install as Windows service
  --uninstall       Uninstall Windows service
```

### Start System Tray Application

```powershell
cd build\Release\bin
KVMTray.exe
```

Or run from Visual Studio with F5 (set `tray` as startup project).

## Troubleshooting

### Build Failures

| Error | Solution |
|-------|----------|
| "WDK not found" | Install Windows 11 WDK, check `C:\Program Files (x86)\Windows Kits\10` |
| "VS not found" | Run `vswhere` to verify installation, install VS 2022 |
| "signtool failed" | Disable driver signing test mode: `bcdedit /set testsigning on` |
| "Link errors" | Ensure WDK version matches SDK version |

### Driver Installation Failures

| Error | Solution |
|-------|----------|
| "Access denied" | Run PowerShell/Command Prompt as Administrator |
| "Driver not signed" | Enable test signing: `bcdedit /set testsigning on` |
| "Device not found" | Check Device Manager → View → Show hidden devices |
| "Driver failed to start" | Check Event Viewer → Windows Logs → System |

### Driver Not Working

```powershell
# 1. Check driver is loaded
Get-Process | Where-Object {$_.ProcessName -like "*vhid*"}

# 2. Check driver handles are accessible
$kb = Get-Item "\\.\vhidkb" -ErrorAction SilentlyContinue
if ($kb) { "Keyboard driver accessible" } else { "Keyboard driver NOT accessible" }

# 3. Verify in Device Manager
#    - Should show "Virtual HID Keyboard" under HID devices
#    - No yellow warning triangles

# 4. Test with SendInput fallback
cd build\Release\tests
test_harness.exe  # Will show if using driver or SendInput fallback
```

### Test Failures

| Symptom | Cause | Solution |
|-----------|-------|----------|
| Tests inject input but nothing happens | Driver not loaded, using SendInput | Check `test_results.log` for injection mode |
| Keyboard tests fail | Wrong window focus | Ensure you're testing in a text editor like Notepad |
| Mouse tests erratic | Screen scaling | Set display scaling to 100% for tests |
| Controller not detected | No game running | Launch a game or Steam Big Picture |

### Network/Protocol Issues

| Symptom | Solution |
|---------|----------|
| "Cannot bind to port" | Check if another service uses 5900/8443: `netstat -ano \| findstr 5900` |
| TLS handshake fails | Check certificate store, ensure TLS 1.3 enabled |
| VNC authentication fails | Reset VNC password in tray app Settings → VNC Server |
| WebSocket disconnects | Check firewall rules for port 8443 |
| VNC AnonTLS fails | Ensure Settings → VNC Security → Enable AnonTLS is checked; cert is auto-created |
| Connection approval dialog doesn't appear | Ensure KVMTray.exe is running; check `%LOCALAPPDATA%\KVM-Drivers\pending_approvals\` |
| Remote client rejected silently | Check IP allowlist in Settings — empty = allow all |
| Trusted client not remembered | Check `%LOCALAPPDATA%\KVM-Drivers\trusted_clients.txt` exists and is not expired |

### Diagnostics Tab

The tray app has a built-in **Diagnostics** tab:
1. Click **Run Health Checks** — checks WDF runtime, port availability, driver accessibility, disk space, pending reboots
2. Select a failed row and click **Repair Selected** — attempts pnputil-based driver reinstall
3. **Export Audit Log** — saves `audit_log.csv` with all connection history
4. Connection Audit Log table shows real-time events from ETW

## Development

### Project Structure

```
KVM-Drivers/
├── src/
│   ├── common/                     # Shared C++ headers
│   │   ├── adaptive_quality.h      # 5-tier FPS scaler
│   │   ├── rate_limiter.h          # Token bucket + connection tracker
│   │   ├── connection_security.h   # ETW audit, cert pinning, TOFU gate
│   │   ├── logging/                # Lock-free unified logger
│   │   └── performance/            # Hitch detection, latency tracking
│   ├── drivers/                    # Kernel drivers
│   │   ├── vhidkb/                # Virtual HID Keyboard
│   │   ├── vhidmouse/             # Virtual HID Mouse
│   │   ├── vxinput/               # Virtual Xbox Controller
│   │   └── vdisplay/              # Virtual Display (IDD)
│   ├── usermode/
│   │   ├── core/                  # KVMService, DriverInterface
│   │   ├── remote/
│   │   │   ├── native/            # WebSocket (sync + async)
│   │   │   └── vnc/               # VNC server + TLS wrapper
│   │   ├── encoding/              # NVENC, AMF, QSV encoders
│   │   └── automation/            # YAML engine, C++ framework, C# wrapper
│   └── tray/                       # WPF System Tray App
│       ├── ConnectionApprovalDialog.*  # TOFU approval UI
│       ├── ConnectionApprovalManager.cs # Polls pending_approvals dir
│       ├── DiagnosticsEngine.cs        # Health checks, self-repair
│       └── SettingsManager.cs          # Persists AppSettings
├── tests/
│   ├── stress_test.cpp              # 72-hour capable stress tester
│   ├── test_keyboard.cpp
│   ├── test_mouse.cpp
│   └── test_harness.cpp
├── docs/                             # Guides and audit reports
├── scripts/                          # Build/install scripts
├── CONTRIBUTING.md
├── LICENSE
└── build/                            # Build output (generated)
```

### Adding New IOCTL Commands

1. Define IOCTL in `src/drivers/<driver>/<driver>_ioctl.h`:
```c
#define IOCTL_MY_NEW_COMMAND CTL_CODE(FILE_DEVICE_VHIDKB, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
```

2. Add handler in `src/drivers/<driver>/<driver>.c`:
```c
case IOCTL_MY_NEW_COMMAND:
    status = MyNewCommandHandler(deviceContext, Request);
    break;
```

3. Update user-mode interface in `src/usermode/core/driver_interface.cpp`:
```cpp
bool DriverInterface::MyNewCommand(Parameters params) {
    // Build IOCTL and send
}
```

### Debugging Drivers

```powershell
# Enable kernel debugging (requires debug machine setup)
bcdedit /debug on
bcdedit /dbgsettings serial debugport:1 baudrate:115200

# Use WinDbg for kernel debugging
# Or use DebugView for driver output
Dbgview.exe  # Capture DbgPrint/KdPrint output
```

### Performance Testing

```powershell
# Measure input latency
$start = Get-Date
# Inject key
$end = Get-Date
($end - $start).TotalMilliseconds

# Profile encoding performance
KVMService.exe --benchmark-encoding
```

## CI/CD Integration

See `.github/workflows/build.yml` for GitHub Actions configuration.

Build matrix:
- Windows Server 2019/2022
- Debug and Release configurations
- Automated test execution
