# KVM-Drivers Architecture

System structure, component responsibilities, runtime topology, data flow, and inter-process communication.

---

## Contents

- [System Overview](#system-overview)
- [Component Map](#component-map)
- [Kernel Drivers](#kernel-drivers)
- [KVMService — Core Daemon](#kvmservice--core-daemon)
  - [Startup Modes](#startup-modes)
  - [Protocol Servers](#protocol-servers)
  - [DriverInterface Abstraction](#driverinterface-abstraction)
- [KVMTray — Management UI](#kvmtray--management-ui)
  - [Initialization Sequence](#initialization-sequence)
  - [Connection Approval Gate (TOFU)](#connection-approval-gate-tofu)
- [Automation Framework](#automation-framework)
- [Network Topology](#network-topology)
- [Authentication Flow](#authentication-flow)
- [Inter-Process Communication](#inter-process-communication)
- [Adaptive Quality System](#adaptive-quality-system)
- [Logging and Observability](#logging-and-observability)

---

## System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        REMOTE CLIENTS                           │
│  Browser (ws://)   VNC Client (RFB 3.8)   Custom WS / wscat    │
└──────────┬──────────────────┬───────────────────┬──────────────┘
           │ :8443            │ :5900             │ :8443
           │                  │                   │
┌──────────▼──────────────────▼───────────────────▼──────────────┐
│                       KVMService.exe                            │
│  ┌─────────────────┐ ┌──────────────┐ ┌────────────────────┐   │
│  │  AsyncWebSocket │ │  VNCServer   │ │   HTTP Server      │   │
│  │  Server (8443)  │ │  (5900)      │ │   (8080)           │   │
│  │  JSON-RPC 2.0   │ │  RFB 3.8     │ │   index.html only  │   │
│  └────────┬────────┘ └──────┬───────┘ └──────────┬─────────┘   │
│           │                 │                     │             │
│  ┌────────▼─────────────────▼─────────────────────▼──────────┐  │
│  │                    DriverInterface                         │  │
│  │   Opens \\.\vhidkb  \\.\vhidmouse  \\.\vxinput  vdisplay  │  │
│  │   SendInput fallback when any handle is unavailable        │  │
│  └────────────────────────┬───────────────────────────────────┘  │
└───────────────────────────┼─────────────────────────────────────┘
                            │ DeviceIoControl (IOCTLs)
┌───────────────────────────▼─────────────────────────────────────┐
│                    KERNEL MODE                                  │
│  ┌──────────┐  ┌────────────┐  ┌──────────┐  ┌─────────────┐   │
│  │ vhidkb   │  │ vhidmouse  │  │ vxinput  │  │  vdisplay   │   │
│  │  .sys    │  │  .sys      │  │  .sys    │  │  (IDD DLL)  │   │
│  │  VHF HID │  │  VHF HID   │  │  VHF HID │  │  WDDM IDD  │   │
│  └────┬─────┘  └─────┬──────┘  └────┬─────┘  └──────┬──────┘   │
└───────┼──────────────┼──────────────┼───────────────┼──────────┘
        │              │              │               │
┌───────▼──────────────▼──────────────▼───────────────▼──────────┐
│                   WINDOWS HID / INPUT STACK                     │
│   HID class driver → keyboard/mouse events → Win32 input queue  │
│   XInput stack → XInput API → DirectInput API                   │
│   Display adapter → virtual monitor → DXGI → D3D               │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                      KVMTray.exe  (WPF)                         │
│  ┌────────────────┐ ┌───────────────────┐ ┌─────────────────┐   │
│  │  Tray icon +   │ │ ConnectionApproval│ │ DiagnosticsEngine│  │
│  │  Main window   │ │ Manager (polling) │ │ health checks    │  │
│  └────────────────┘ └──────────┬────────┘ └─────────────────┘   │
└──────────────────────────────  │  ──────────────────────────────┘
                        File IPC │ (pending_approvals\*.request/result)
                   ┌─────────────▼──────────────┐
                   │  %PROGRAMDATA%\KVM-Drivers\  │
                   │  settings.json               │
                   │  trusted_clients.txt          │
                   │  audit_log.csv                │
                   │  pending_approvals\           │
                   └──────────────────────────────┘
```

---

## Component Map

| Component | Binary | Language | Runs as |
|-----------|--------|----------|---------|
| Virtual HID Keyboard | `vhidkb.sys` | C (WDF/VHF) | Kernel — boot/demand |
| Virtual HID Mouse | `vhidmouse.sys` | C (WDF/VHF) | Kernel — boot/demand |
| Virtual Xbox Controller | `vxinput.sys` | C (WDF/VHF) | Kernel — boot/demand |
| Virtual Display (IDD) | `vdisplay.dll` | C++ (WDDM IDD) | Kernel session — display subsystem |
| Core service + servers | `KVMService.exe` | C++ | User-mode Windows Service (`NT AUTHORITY\LocalService`) |
| Tray application | `KVMTray.exe` | C# .NET 8 (WPF) | User-mode, interactive desktop |
| Automation framework | `kvm_automation.dll` | C++ | Loaded by host process |
| .NET automation wrapper | `AutomationFramework.cs` | C# | P/Invoke over `kvm_automation.dll` |

---

## Kernel Drivers

All four kernel drivers follow the **Windows Driver Framework (WDF)** model and use the **Virtual HID Framework (VHF)** for input devices and the **Indirect Display Driver (IDD)** model for the display.

### vhidkb — Virtual HID Keyboard

| Detail | Value |
|--------|-------|
| Device node | `HID\VID_045E&PID_0750` (Microsoft HID keyboard) |
| HID descriptor | Boot-protocol keyboard (6-key rollover, standard 104-key layout) |
| VHF model | `VhfCreate` + `VhfReadReportSubmit` |
| IOCTL interface | `\\.\vhidkb` |
| Key IOCTLs | `IOCTL_VKB_INJECT_KEYDOWN`, `IOCTL_VKB_INJECT_KEYUP`, `IOCTL_VKB_RESET` |
| Fallback | If VHF unavailable: falls through to `SendInput` in user-mode |

### vhidmouse — Virtual HID Mouse

| Detail | Value |
|--------|-------|
| Device node | `HID\VID_045E&PID_0750` (Microsoft HID mouse) |
| Capabilities | Relative movement, absolute positioning, 5 buttons, vertical + horizontal scroll |
| IOCTL interface | `\\.\vhidmouse` |
| Key IOCTLs | `IOCTL_VMOUSE_MOVE_RELATIVE`, `IOCTL_VMOUSE_MOVE_ABSOLUTE`, `IOCTL_VMOUSE_BUTTON`, `IOCTL_VMOUSE_SCROLL` |

### vxinput — Virtual Xbox Controller

| Detail | Value |
|--------|-------|
| Device node | `HID\VID_045E&PID_028E` (Xbox 360 controller — XInput compatible) |
| Report format | 13-byte XUSB report → HID gamepad report via `VhfReadReportSubmit` |
| Slots | Up to 4 simultaneous controllers (player 1–4) |
| IOCTL interface | `\\.\vxinput` |
| Key IOCTLs | `IOCTL_VXINPUT_CREATE_CONTROLLER`, `IOCTL_VXINPUT_REMOVE_CONTROLLER`, `IOCTL_VXINPUT_SUBMIT_REPORT`, `IOCTL_VXINPUT_SET_RUMBLE`, `IOCTL_VXINPUT_GET_RUMBLE`, `IOCTL_VXINPUT_GET_CONTROLLER_COUNT` |
| Visible to | DirectInput, XInput, Steam Input, Windows gamepad API |

### vdisplay — Virtual Display (IDD)

| Detail | Value |
|--------|-------|
| Model | WDDM Indirect Display Driver (IDD) |
| Capabilities | Multi-resolution, DXGI surface publication |
| Shared texture | `FinishFrameProcessing` publishes a DXGI shared handle under `CRITICAL_SECTION`; video pipeline calls `OpenSharedResource` |
| DXGI capture | `KVMService` accesses frames via DXGI Desktop Duplication API (`IDXGIOutputDuplication`) |

---

## KVMService — Core Daemon

### Startup Modes

`KVMService.exe` supports four distinct entry points, selected by command-line argument:

| Invocation | Entry path | Effect |
|------------|-----------|--------|
| `KVMService.exe` (no args, started by SCM) | `wmain` → `StartServiceCtrlDispatcher` → `ServiceMain` | Full Windows Service lifecycle; SCM controls start/stop |
| `KVMService.exe --standalone` | `wmain` → `StartProtocolServers()` directly | Runs in console; Ctrl+C or Enter stops it; useful for dev/debug |
| `KVMService.exe install` | `wmain` → `InstallService()` | Registers with SCM as auto-start, exits |
| `KVMService.exe uninstall` | `wmain` → `UninstallService()` | Stops then deletes SCM entry, exits |

**Double-launch guard**: If `KVMService.exe` is run in a console without `--standalone`, `StartServiceCtrlDispatcher` fails with `ERROR_FAILED_SERVICE_CONTROLLER_CONNECT` and the process exits with an error message.

**Service loop** (when running as a service): `WaitForSingleObject(g_ServiceStopEvent, 100ms)` — wakes on `SERVICE_CONTROL_STOP` / `SERVICE_CONTROL_SHUTDOWN`. `ProcessServiceTasks()` is called on each wake but is currently a no-op; the real work is done by the protocol server threads.

### Protocol Servers

`StartProtocolServers()` is called once during startup and initialises all three servers:

#### 1. Async WebSocket Server — port 8443 (fatal if bind fails)

- **Architecture**: Single select-loop network thread + one injection worker thread
- **Concurrency model**: Non-blocking `select()` with 100ms timeout; I/O dispatched to `WSClient` slots; injection work queued to `std::queue<InjectionMessage>` and processed by the injection thread
- **Max clients**: Read from `settings.json` → `WsMaxClients` (default 10, hard cap `WS_MAX_CLIENTS = 32`)
- **Protocol**: JSON-RPC 2.0 over WebSocket (RFC 6455)
- **Screen stream**: DXGI Desktop Duplication → GDI+ JPEG encode → binary WebSocket frames; starts on first `display.start_stream`, stops when last streaming client disconnects
- **Rate limiting**: Tier-aware; `targetFps × 2` inputs/sec per client at current adaptive quality tier
- **Controller management**: Auto-claims VHF slots 0–3 per client; holds slot for 30s on disconnect (reconnect grace window)
- **Oversized frame guard**: Frames > 16 MB rejected; WebSocket close sent

#### 2. VNC Server — port 5900 (non-fatal if bind fails)

- **Architecture**: One accept thread + one detached thread per connected client
- **Protocol**: RFB 3.8 (compatible with RealVNC, TightVNC, TigerVNC, UltraVNC)
- **Security types offered**: `SecTypeAnonTLS` (18) first if enabled, `SecTypeVNCAuth` (2) if password set, `SecTypeNone` (1) if no password
- **VNCAuth**: BCrypt DES challenge-response (16-byte challenge, bit-reversed key per RFB spec)
- **AnonTLS**: Schannel self-signed cert (`CN=KVM-Drivers-VNC`); inner auth negotiated over TLS
- **Encodings**: Hextile (preferred), Raw (fallback); client negotiates via `SetEncodings`
- **Framebuffer**: Updated by DXGI capture thread (CaptureLoop); copy to staging texture → CPU readback → shared `framebuffer_` behind `framebufferMutex_`
- **Key injection**: `X11ToWindows(keysym)` → `InjectVirtualKey(vk, keyUp, extended)` → `SendInput` directly (VK codes, not HID usage codes)
- **Max clients**: From `settings.json` → `VncMaxClients` (default 10); enforced in accept loop

#### 3. HTTP Server — port 8080 (non-fatal if bind fails)

- **Architecture**: Single accept + inline handler (no per-client thread; requests are tiny)
- **Purpose**: Serves `index.html` (web client) so users can open a browser to the machine's IP
- **Resolution order** for `index.html`:
  1. `<ExeDir>\webclient\index.html`
  2. `<ExeDir>\index.html`
  3. Walk up to 6 parent directories looking for `src\webclient\index.html` (dev tree)
- **Routes**: `GET /` and `GET /index.html` → 200 with HTML; `GET /favicon.ico` → 204 No Content; all other paths → 404
- **If index.html not found**: Server still listens; all requests return a minimal 404 page

### DriverInterface Abstraction

`DriverInterface` (`src/usermode/core/driver_interface.cpp`) provides a unified, thread-safe API over the four kernel drivers. It is instantiated separately by each protocol server (not shared globally).

**Handle opening** (`Initialize()`):
```
CreateFile("\\.\vhidkb",   ...)  → keyboardHandle
CreateFile("\\.\vhidmouse",...)  → mouseHandle
CreateFile("\\.\vxinput",  ...)  → controllerHandle
CreateFile("\\.\vdisplay", ...)  → displayHandle
```
Any handle that fails to open is set to `INVALID_HANDLE_VALUE`. The `useDriverInjection` atomic flag is set to `true` only if at least `keyboardHandle` or `mouseHandle` succeeded.

**Input path selection per method:**

| Method | Driver path | SendInput fallback |
|--------|-------------|-------------------|
| `InjectKeyDown/Up` | `IOCTL_VKB_INJECT_KEYDOWN/UP` with `VKB_INPUT_REPORT` (HID usage codes) | `HidToVirtualKey(hidCode)` → `SendKeyWithModifiers()` |
| `InjectVirtualKey` | *(skips driver — VK codes only)* | Always `SendInput` with `KEYEVENTF_KEYUP` / `KEYEVENTF_EXTENDEDKEY` as appropriate |
| `InjectMouseMove` | `IOCTL_VMOUSE_MOVE_ABSOLUTE/RELATIVE` | `SendInput` with `MOUSEEVENTF_MOVE` / `MOUSEEVENTF_ABSOLUTE` |
| `InjectMouseButton` | `IOCTL_VMOUSE_BUTTON` | `SendInput` with `MOUSEEVENTF_LEFT/RIGHT/MIDDLEDOWN/UP` |
| `InjectMouseScroll` | `IOCTL_VMOUSE_SCROLL` | `SendInput` with `MOUSEEVENTF_WHEEL` |
| `InjectControllerReportSlot` | `IOCTL_VXINPUT_SUBMIT_REPORT` with slot index | *(no SendInput fallback for multi-slot)* |
| `SetControllerRumble` | `IOCTL_VXINPUT_SET_RUMBLE` with `XUSB_RUMBLE_STATE` | *(no fallback)* |

> `InjectVirtualKey` exists specifically for the VNC server: X11 keysym → VK code mapping produces Windows VK codes, not HID usage codes. Passing VK codes to `InjectKeyDown` would send the wrong key through the kernel driver. `InjectVirtualKey` bypasses the driver entirely and calls `SendInput` directly.

All public methods are protected by `std::mutex handleMutex_` around handle access. `useDriverInjection` is `std::atomic<bool>`.

---

## KVMTray — Management UI

`KVMTray.exe` is a WPF .NET 8 application. It does **not** host any network servers — its role is configuration, monitoring, and acting as the TOFU approval UI.

### Initialization Sequence

```
wmain (Program.cpp)
  └─ CreateMutex("KVM_Tray_Single_Instance")
       ├─ ERROR_ALREADY_EXISTS → MessageBox + exit (single-instance guard)
       └─ Success → new WPF Application + new MainWindow
            ├─ App.OnStartup → TaskbarIcon initialized (NotifyIcon via Hardcodet.Wpf.TaskbarNotification)
            └─ MainWindow ctor
                 ├─ LoadSettings()         → %PROGRAMDATA%\KVM-Drivers\settings.json
                 ├─ ApplySettings()        → populates all UI fields
                 ├─ InitializeData()       → creates empty connections ObservableCollection
                 ├─ RefreshConnectionUrls() → fills URL display fields from local IP + port settings
                 ├─ SetupTimer()           → DispatcherTimer, 2s tick → UpdateDriverStatus()
                 ├─ InitializeApprovalManager() → starts ConnectionApprovalManager.PollLoop()
                 ├─ CheckFirewallOnStartup()    → 2s async delay, then checks firewall rules
                 ├─ AutoStartDrivers()          → if settings.AutoStartDrivers, starts enabled drivers
                 └─ if settings.StartMinimized → Hide() window (tray only)
```

**2-second refresh timer** (`UpdateDriverStatus`): Queries SCM status of `vhidkb`, `vhidmouse`, `vxinput`, `vdisplay` via `ServiceController`. Updates green/gray dots and Start/Stop button text. No network I/O.

**Tray icon context menu** (`App.xaml`): "Show Panel" (brings `MainWindow` to front) and "Exit".

**Single-instance enforcement**: The mutex `KVM_Tray_Single_Instance` is held for the process lifetime. A second launch detects `ERROR_ALREADY_EXISTS` immediately and shows a dialog then exits.

### Connection Approval Gate (TOFU)

Trust On First Use (TOFU) is implemented as a file-based IPC handshake between `KVMService` (C++) and `KVMTray` (C#):

```
New unknown client connects
         │
    C++ server
         │
    ConnectionAuthGate::Evaluate()
         │
    ├─ 127.0.0.1 / ::1 → ALLOW immediately (no files written)
    ├─ IP in trusted_clients.txt (not expired) → ALLOW immediately
    ├─ Bearer token matches settings.AuthToken → ALLOW immediately
    ├─ IP not in AllowedIPs (if list non-empty) → DENY immediately
    └─ Unknown IP →
            Write <UUID>.request to pending_approvals\
            Poll for <UUID>.result (up to 30s, 100ms intervals)
                    │
            KVMTray ConnectionApprovalManager.PollLoop()
                    │
                    Reads <UUID>.request → shows ConnectionApprovalDialog
                    User choice:
                      Approve         → writes "approved"  to <UUID>.result
                      Approve + Trust → writes "approved"  to <UUID>.result
                                        appends IP + expiry to trusted_clients.txt
                      Block           → writes "rejected"  to <UUID>.result
                    │
            C++ reads result → ALLOW or DENY
                    │
            If timeout (30s, no result file) → DENY
            If tray not running (no result file ever) → DENY after 30s
```

**Request file format** (`<UUID>.request`):
```json
{"id":"<UUID>","ip":"192.168.1.42","protocol":"WebSocket","timestamp":"...","authenticated":false}
```

**Result file format** (`<UUID>.result`): single word `approved` or `rejected`.

---

## Automation Framework

```
┌──────────────────────────────────────────────────┐
│              Application / Test Code              │
│  C++ direct  │  C# P/Invoke  │  YAML script      │
└──────┬────────┴───────┬───────┴────────┬──────────┘
       │                │               │
       ▼                ▼               ▼
┌─────────────────────────────────────────────────┐
│            kvm_automation.dll                    │
│  TestRunner  │  ActionHandlers  │  ScreenCapture │
└──────────────────────────┬──────────────────────┘
                           │
                    DriverInterface
                           │
              Kernel drivers / SendInput
```

| Layer | File(s) | Purpose |
|-------|---------|---------|
| C++ framework | `automation_framework.h/cpp` | Plugin-based `TestRunner`, all `ActionType` handlers, screenshot-on-failure, retry, variables |
| Game extensions | `game_automation.h/cpp` | App launcher, UI automation, OCR (Tesseract), smart-click |
| Screen capture | `screen_capture.h/cpp` | GDI+ / DXGI frame capture used by assertion actions |
| C# wrapper | `framework/dotnet/AutomationFramework.cs` | P/Invoke over `kvm_automation.dll`; fluent API for .NET test code |
| Local automation | `local_automation.cpp` | Standalone binary; YAML runner + interactive REPL; no network |

**YAML test script structure:**
```yaml
name: My Test
description: optional
setup:         # runs first
  - action: ...
steps:         # main sequence
  - action: ...
assertions:    # checked after steps
  - action: ...
teardown:      # always runs last
  - action: ...
```

Available actions: `mouse_move`, `mouse_click`, `mouse_scroll`, `mouse_drag`, `key_down`, `key_up`, `key_press`, `key_combo`, `key_type`, `wait`, `screenshot`, `assert_image` (pixel RMSE), `assert_pixel`, `assert_window`.

---

## Network Topology

| Port | Protocol | Server | Bindable to | Notes |
|------|----------|--------|-------------|-------|
| **8443** | WebSocket (ws://) | AsyncWebSocketServer | `0.0.0.0` (all interfaces) | JSON-RPC 2.0; fatal if bind fails |
| **5900** | TCP (RFB 3.8) | VNCServer | `0.0.0.0` | Standard VNC port; non-fatal if busy |
| **8080** | HTTP | Inline HTTP server | `0.0.0.0` | Serves `index.html` only; non-fatal if busy |

Ports are read from `settings.json` at service startup (`WebSocketPort`, `VncPort`, `HttpPort`). Defaults shown above.

**Firewall**: Windows Firewall inbound rules must allow all three ports. The tray Diagnostics tab can add these rules automatically, or they can be added manually:
```powershell
netsh advfirewall firewall add rule name="KVM WebSocket" dir=in action=allow protocol=TCP localport=8443
netsh advfirewall firewall add rule name="KVM VNC"       dir=in action=allow protocol=TCP localport=5900
netsh advfirewall firewall add rule name="KVM Web"       dir=in action=allow protocol=TCP localport=8080
```

---

## Authentication Flow

```
Client connects
      │
      ├─ Protocol: WebSocket (port 8443)
      │       │
      │       ├─ Source IP = 127.0.0.1 or ::1 → PASS (localhost bypass)
      │       ├─ Source IP in AllowedIPs list (non-empty) → continue; else DENY
      │       ├─ IP in trusted_clients.txt (not expired) → PASS
      │       ├─ Client sends auth.authenticate{token} matching settings.AuthToken → PASS
      │       └─ Unknown → TOFU file-IPC → tray dialog → PASS or DENY (30s timeout = DENY)
      │
      └─ Protocol: VNC (port 5900)
              │
              ├─ Security type negotiation:
              │       AnonTLS (18) offered first if VncAnonTls=true
              │           → Schannel TLS handshake with self-signed cert
              │           → Optional cert pinning (VncCertPin SHA-1 thumbprint)
              │           → Inner security type negotiated over TLS:
              │                 VNCAuth or None
              │       VNCAuth (2) offered if VncPassword non-empty
              │           → BCrypt random 16-byte challenge sent
              │           → Client encrypts with DES (bit-reversed key per RFB spec)
              │           → Server validates; failure sends reason string + closes
              │       None (1) offered if no password set
              └─ No IP allowlist / TOFU for VNC (enforced at TCP level by firewall only)
```

**Trusted client lifetime**: Entries in `trusted_clients.txt` include an expiry epoch. Expired entries are ignored (client goes through TOFU again). Revocation is available in the tray Settings → Trusted Clients tab.

---

## Inter-Process Communication

KVMService and KVMTray run as separate processes and communicate exclusively through the filesystem:

| Mechanism | Direction | Path | Purpose |
|-----------|-----------|------|---------|
| `settings.json` | Tray → Service | `%PROGRAMDATA%\KVM-Drivers\` | Service reads at startup; tray writes on Save |
| `trusted_clients.txt` | Tray writes, Service reads | `%PROGRAMDATA%\KVM-Drivers\` | Approved IPs + expiry |
| `*.request` files | Service → Tray | `…\pending_approvals\` | New connection pending approval |
| `*.result` files | Tray → Service | `…\pending_approvals\` | Approval decision (approved/rejected) |
| `audit_log.csv` | Service writes, Tray reads | `%PROGRAMDATA%\KVM-Drivers\` | Per-connection event log |

There is **no named pipe, no COM, no socket** between the two processes. This is intentional: it keeps the service's security boundary clean and avoids privilege escalation paths.

> **Implication**: If `KVMTray.exe` is not running when a new unknown client connects, the `.request` file is never read. The 30-second server-side timeout fires and the client is rejected. This is fail-closed behavior.

---

## Adaptive Quality System

`AdaptiveQuality` (`src/common/adaptive_quality.h`) is a shared controller used by both the WebSocket stream thread and the VNC server. It automatically degrades quality under load and recovers when conditions improve.

| Tier | FPS target | JPEG quality | Max frame size |
|------|-----------|-------------|---------------|
| ULTRA | 60 | 95 | 1920×1080×4 |
| HIGH | 30 | 80 | 1920×1080×4 |
| MEDIUM | 20 | 65 | 1280×720×4 |
| LOW | 10 | 50 | 1280×720×4 |
| MINIMAL | 5 | 30 | 640×480×4 |

**Degrade triggers** (moves tier down one step):
- 3 consecutive frames with send latency > `degradeThresholdMs` (default 150ms)
- Dropped frame reported (queue full / client too slow); weighted ×2
- CPU usage > 90% (polled every 5s via `GetSystemTimes`)

**Recovery triggers** (moves tier up one step):
- 10 consecutive frames with send latency < `recoverThresholdMs` (default 50ms)
- CPU usage < 50% (polled every 5s)

The WebSocket `StreamLoop` reads `adaptiveQuality_.GetSettings().jpegQuality` each frame and applies it to GDI+ JPEG encoding. The sleep interval between frames uses `GetFrameIntervalMs()` (1000 / targetFps). The VNC server calls `CheckSystemLoad()` and `ReportFrameLatency()` per framebuffer update request.

---

## Logging and Observability

### Unified Logger (C++ — user-mode)
Lock-free ring buffer logger (`src/common/logging/`). Categories: `GENERAL`, `DRIVER`, `NETWORK`, `INPUT`, `DISPLAY`, `PERFORMANCE`. Writes to a log file alongside the executable. Zero spinlocks on the hot write path.

### ETW (kernel — Event Tracing for Windows)
Kernel drivers emit ETW events via `EtwWrite`. Viewable in Windows Event Viewer or with `xperf`/`WPA`.

### Audit log (`audit_log.csv`)
Written by `ConnectionSecurityContext::AuditLog` in C++. Events: `Connected`, `Disconnected`, `AuthFailed`, `RateLimited`. Consumed by the tray's Diagnostics tab and exportable as CSV.

### Performance monitor
`PERF_MONITOR_CONTEXT` (`src/common/performance/`): hitch detection, p50/p95/p99 latency tracking. `PerfMonitorStart`/`PerfMonitorEnd` bracket injection operations in the WebSocket injection worker.

### Tray log viewer
`MainWindow.LogViewer` (WPF `TextBox`) — `AppendLog()` appends timestamped lines, auto-scrolls, caps at 100 KB (trims to last 50 KB). Exportable via "Export Logs" button to `%PROGRAMDATA%\KVM-Drivers\kvmlogs.txt`.
