# KVM-Drivers Operations Guide

All active usage scenarios, idle/non-use states, and the troubleshooting implications of each.

---

## Contents

- [Remote Control](#remote-control)
  - [Web Browser Client](#web-browser-client)
  - [VNC Client](#vnc-client)
  - [Direct WebSocket / JSON-RPC](#direct-websocket--json-rpc)
- [Multi-Player Controller](#multi-player-controller)
- [Local Automation](#local-automation)
- [Testing](#testing)
- [Tray Management](#tray-management)
- [Settings](#settings)
- [Idle and Non-Use States](#idle-and-non-use-states)

---

## Remote Control

### Web Browser Client

#### Normal path (primary)

1. Client opens `http://<server-ip>:8080/` in any modern browser.
2. The HTTP server serves `index.html`. The server host field is pre-filled with `window.location.hostname`.
3. User fills in optional auth token and clicks **Connect**.
4. Browser opens `ws://<host>:8443`.
5. Auth gate fires (see [Authentication states](#authentication-states) below).
6. On success, the client sends:
   - `auth.authenticate {token}` (if token was provided)
   - `system.get_capabilities` — server replies with `{screen, keyboard, mouse, controller: true, version, protocol}`
   - `display.start_stream {format, quality, fps}` — DXGI capture starts; JPEG binary frames flow immediately
7. Frames are decoded via `createImageBitmap()` and painted to a `<canvas>`.
8. User clicks the **Control** button — keyboard focus moves to the input-capture overlay.

**Input while in Control mode:**

| User action | JSON-RPC method sent | Params |
|-------------|---------------------|--------|
| Mouse move | `input.mouse.move` | `{x, y, absolute: true}` |
| Mouse click | `input.mouse.button` | `{button: 0\|1\|2, pressed: true\|false}` |
| Scroll wheel | `input.mouse.scroll` | `{vertical, horizontal}` |
| Key down | `input.keyboard.keydown` | `{keyCode: <HID usage>, modifiers}` |
| Key up | `input.keyboard.keyup` | `{keyCode: <HID usage>, modifiers}` |
| Quality change | `display.set_quality` | `{quality: "high"\|"medium"\|"low"\|"adaptive"}` |

**Stats panel** (updates every animation frame): FPS, bandwidth (bytes/sec), latency (via `system.ping` / `"pong"` round-trip).

**Disconnect**: browser sends WebSocket close frame; server clears the client slot, stops streaming if it was the last streaming client, releases any controller slot (after 30 s hold).

#### Direct file open

- User opens `file:///path/to/index.html` from disk.
- `window.location.hostname` is empty → server host field defaults to `localhost`.
- Functionally identical to the served path once the user enters the correct host.

#### TLS path (wss://)

- User checks **Use TLS** in the Settings modal → `wss://` protocol.
- **Currently non-functional**: `UseTls = false` by default; `tls_server.cpp` exists but is not wired to the WebSocket server. Connection will fail at TLS handshake. This path is documented for when TLS is activated.

#### Authentication states

| Client situation | Result |
|-----------------|--------|
| Source IP is `127.0.0.1` or `::1` | Allowed immediately — no dialog, no token required |
| IP is in `trusted_clients.txt` and not expired | Allowed immediately |
| Bearer token provided matches `settings.AuthToken` | Allowed — `auth.authenticate` response is `{status:"authenticated"}` |
| IP blocked by non-empty `AllowedIPs` list | Connection rejected before TOFU |
| Unknown IP, tray running | `<UUID>.request` written; tray shows `ConnectionApprovalDialog` (30 s timeout → rejected) |
| Unknown IP, tray NOT running | `.request` file is never read; 30 s timeout → rejected silently |

---

### VNC Client

Any standard VNC client (RealVNC, TightVNC, TigerVNC, UltraVNC) connects to port **5900**.

#### No auth (`SecTypeNone`)

Used when no VNC password is configured.

1. RFB 3.8 version exchange.
2. Server offers security type `1` (None).
3. Client sends `ClientInit` (shared flag).
4. Server sends `ServerInit`: resolution + 32bpp BGRX pixel format + desktop name `"KVM-Drivers VNC"`.
5. Client requests framebuffer updates → server sends Hextile-encoded (preferred) or Raw rectangles.
6. Key events: `X11 keysym → WindowsVK → InjectVirtualKey() → SendInput`.
7. Pointer events: absolute `InjectMouseMove` + `InjectMouseButton`; scroll wheel via pseudo-buttons (bits 3–6 of button mask).
8. `ClientCutText` is consumed and discarded (clipboard not forwarded).
9. `SetPixelFormat` is consumed and ignored (server keeps BGRX format).

#### VNCAuth (`SecTypeVNCAuth`)

Password configured in Tray → Settings → VNC Password.

1. Server offers `SecTypeVNCAuth`.
2. Server sends 16-byte BCrypt random challenge.
3. Client DES-encrypts challenge with bit-reversed password bytes (per RFB spec).
4. Server validates via `VncDesEncrypt()` (Windows BCrypt, ECB mode).
5. Security result: `0` = OK, `1` = failed + reason string "Authentication failed" + connection close.

#### AnonTLS (`SecTypeAnonTLS` = 18)

Enabled via Tray → Settings → VNC Security → Enable AnonTLS.

1. Server creates a self-signed Schannel cert on first use (`CN=KVM-Drivers-VNC`).
2. Security type 18 is offered first in the list.
3. Client (must support AnonTLS, e.g. TigerVNC) upgrades the TCP socket to TLS.
4. Inner security type list is negotiated over the encrypted channel (VNCAuth or None).
5. Optional cert pinning: if `VncCertPin` (SHA-1 thumbprint) is set in settings, it is validated before proceeding.

#### Hextile encoding

The server prefers Hextile over Raw. The selection is per-client: the server checks the client's `SetEncodings` list and uses Hextile if present.

Hextile tile logic:
- **Uniform tile**: sends `BackgroundSpecified` sub-type with the color (or no sub-type if bg unchanged). Compact.
- **≤ 4 colors**: sends foreground + subrects (1-pixel runs of non-background pixels).
- **> 4 colors**: falls back to Raw tile.

---

### Direct WebSocket / JSON-RPC

Any WebSocket-capable client can connect to `ws://<host>:8443`.

#### Full method reference

| Method | Direction | Params | Response |
|--------|-----------|--------|----------|
| `auth.authenticate` | Client → Server | `{token: string}` | `{status: "authenticated"}` |
| `system.ping` | Client → Server | — | `"pong"` |
| `system.get_capabilities` | Client → Server | — | `{screen, keyboard, mouse, controller, version, protocol}` |
| `system.get_version` | Client → Server | — | `{version: "1.0.0", protocol: "2.0"}` |
| `display.start_stream` | Client → Server | `{format, quality, fps}` | `{streaming: true}` + `display.resolution_change` notification |
| `display.stop_stream` | Client → Server | — | `{streaming: false}` |
| `display.set_quality` | Client → Server | `{quality: "high"\|"medium"\|"low"\|"adaptive"}` | `{quality: <jpegQ 0-100>}` |
| `input.keyboard.keydown` | Client → Server | `{keyCode: <HID usage>, modifiers}` | `{}` |
| `input.keyboard.keyup` | Client → Server | `{keyCode: <HID usage>, modifiers}` | `{}` |
| `input.mouse.move` | Client → Server | `{x, y, absolute: bool}` | `{}` |
| `input.mouse.button` | Client → Server | `{button: 0-4, pressed: bool}` | `{}` |
| `input.mouse.scroll` | Client → Server | `{vertical, horizontal}` | `{}` |
| `input.controller.report` | Client → Server | `{buttons, leftTrigger, rightTrigger, thumbLX, thumbLY, thumbRX, thumbRY}` | `{}` |

> `input.keyboard.keydown/keyup` expect **HID usage codes** (USB HID keyboard page), not Windows Virtual Key codes. The web client maps browser `KeyboardEvent.code` to HID usage codes before sending.

#### Server-to-client notifications (no request id)

| Method | When sent | Params |
|--------|-----------|--------|
| `display.resolution_change` | DXGI output resolution changes | `{width, height}` |
| `input.controller.assigned` | Client auto-claimed a gamepad slot | `{slot: 0-3}` |

#### Error responses

| Code | Meaning |
|------|---------|
| `-32603` | Internal error — injection failed (driver IOCTL returned false) |
| `-32001` | All 4 controller slots occupied |

#### Rate limiting

Input injection methods (`input.*`) are rate-limited per client:

- Limit = `adaptiveQuality.targetFps × 2` inputs/sec at the current tier (120/sec at ULTRA, 10/sec at MINIMAL)
- Exceeded messages are silently dropped; `auditLog.LogRateLimited()` records the event
- Control methods (`display.*`, `system.*`, `auth.*`) are never rate-limited

#### WebSocket frame limits

- Maximum inbound frame: **16 MB** (frames larger than this cause the server to close the client connection)
- Maximum outbound stream buffer per client: **1 MB** — frames are dropped silently if a slow client's buffer is full

---

## Multi-Player Controller

Up to **4 simultaneous gamepad clients** are supported via the vxinput VHF driver.

| Event | What happens |
|-------|-------------|
| First `input.controller.report` from a client | Server calls `ClaimControllerSlot()` → `IOCTL_VXINPUT_CREATE_CONTROLLER` for next free slot (0–3); sends `input.controller.assigned {slot}` notification |
| Subsequent reports | `InjectControllerReportSlot(slot, report)` → `IOCTL_VXINPUT_SUBMIT_REPORT` |
| All 4 slots occupied, new client tries | Error `-32001` sent; client gets no slot until another disconnects |
| Client disconnects | Controller slot is **held** for 30 seconds (reconnect grace window): VHF device stays active, all buttons zeroed via `IOCTL_VXINPUT_SUBMIT_REPORT` with empty report; `controllerSlotOwner[slot] = -1` |
| Same IP reconnects within 30 s | Gets its slot back without re-claiming |
| No reconnect within 30 s | `NetworkLoop` periodic sweep (every ~100ms) calls `ReleaseControllerSlot()` → `IOCTL_VXINPUT_REMOVE_CONTROLLER` |

---

## Local Automation

Local automation connects **directly to the kernel drivers** (no network, no auth). It is the recommended approach for same-machine CI/CD scripts.

### YAML script mode

```bat
local_automation.exe --script tests\my_test.yaml
```

Script structure:
```yaml
name: My Test
description: Optional description
setup:
  - action: wait
    duration: 500
steps:
  - action: key_press
    key: 0x04        # HID usage 'a'
  - action: mouse_click
    x: 960
    y: 540
assertions:
  - action: assert_image
    reference: expected.png
    tolerance: 0.02
teardown:
  - action: key_press
    key: 0xFF        # Escape
```

Available actions:

| Action | Parameters |
|--------|-----------|
| `mouse_move` | `x`, `y` |
| `mouse_click` | `x`, `y`, `button` (0/1/2) |
| `mouse_scroll` | `vertical`, `horizontal` |
| `key_down` | `key` (HID usage code) |
| `key_up` | `key` |
| `key_press` | `key` (down + up) |
| `key_combo` | `keys` (list; all down then all up in reverse) |
| `wait` | `duration` (ms) |
| `screenshot` | `path` |
| `assert_image` | `reference`, `tolerance` (RMSE 0.0–1.0) |

YAML sections are executed in order: `setup` → `steps` → `assertions` → `teardown` (teardown always runs even if earlier sections fail).

Exit code: `0` = all assertions passed, `1` = any failure.

### Smoke test mode

```bat
local_automation.exe --smoke
```

Runs a fixed built-in sequence (A–Z key cycle, mouse move + click, screenshot capture). Use as a quick health check after installing or updating drivers.

### Interactive REPL mode

```bat
local_automation.exe --interactive
# (also the default when no arguments are given)
```

Prompt: `auto> `

| Command | Effect |
|---------|--------|
| `click <x> <y>` | Left-click at absolute coordinates |
| `move <x> <y>` | Move mouse to absolute coordinates |
| `key <code>` | Press and release a key (HID usage code, decimal or hex) |
| `type <text>` | Type a string character by character |
| `wait <ms>` | Sleep for N milliseconds |
| `screenshot` | Capture screen to `interactive.png` |
| `help` | Print command reference |
| `exit` / `quit` | Shutdown and exit |

---

## Testing

### Automated test harness

```bat
cd build\Release\tests
test_harness.exe
```

Creates a `DriverInterface`, prints injection mode ("Kernel Drivers" or "SendInput Fallback"), runs the full keyboard + mouse test battery, and writes results to `test_results.log`. Suitable for CI.

Test coverage: single key injection, modifier combos (Ctrl+C etc.), multi-key sequences, mouse relative movement, mouse absolute movement, mouse button clicks, scroll wheel.

### Manual device tests

| Binary | How to run | What it tests |
|--------|-----------|---------------|
| `test_keyboard.exe` | Run; choose from menu | Interactive: injects A, Enter, Ctrl+C, keyboard reset |
| `test_mouse.exe` | Run; choose from menu | Interactive: relative/absolute move, left click, scroll, center |
| `test_keyboard_hid.c` → exe | Run directly | Low-level `IOCTL_VKB_INJECT_KEYDOWN/UP` round-trip |
| `test_controller_xusb.c` → exe | Run directly | XUSB report injection via `IOCTL_VXINPUT_SUBMIT_REPORT` |

### YAML-driven CI test suite

`tests/test_definitions.yaml` defines test suites at the IOCTL level:

```yaml
test_suites:
  - name: keyboard_input
    tests:
      - id: kb_001  # Single key
      - id: kb_002  # Modifier combo (Ctrl+C)
      - id: kb_003  # 6-key rollover
      - id: kb_004  # Function keys F1-F4
      - id: kb_005  # Reset
  - name: mouse_input
    tests: [...]
  - name: controller_input
    tests: [...]
```

Run with: `local_automation.exe --script tests\test_definitions.yaml`

### Stress test

```bat
cd build\Release\tests

# 1-hour smoke
stress_test.exe --hours 1 --output results_1h.json

# Full 72-hour stability test
stress_test.exe --72h --output results_72h.json
```

Options:

| Flag | Default | Effect |
|------|---------|--------|
| `--hours N` | 12 | Duration in hours |
| `--rate N` | 100 | Input events per second |
| `--72h` | — | Preset: 72 hours at 60 events/sec |
| `--single` | — | Single-threaded instead of parallel keyboard+mouse+controller |
| `--no-watchdog` | — | Disable hang detection |
| `--watchdog-timeout N` | 30 | Seconds before watchdog fires |
| `--output FILE` | — | Write JSON result file |

**Pass criteria**: RESULT: PASSED — error rate < 0.1%, no watchdog fires, no driver reconnects needed.

JSON output includes: total events, errors, error rate, p50/p95/p99 latency per thread, watchdog fire count.

### WebSocket protocol test

```bash
wscat -c ws://localhost:8443
> {"jsonrpc":"2.0","method":"system.ping","id":1}
< {"jsonrpc":"2.0","result":"pong","id":1}
```

Useful for verifying the server is reachable and the JSON-RPC layer is functioning before connecting a full client.

---

## Tray Management

### Driver start / stop

Each driver (Keyboard, Mouse, Controller, Display) has an independent **Start** / **Stop** toggle button in the Drivers tab.

- Calls `ServiceController("<svcname>").Start()` or `.Stop()` with a 5-second timeout.
- On success: status dot turns green/gray, button text toggles.
- State saved to `settings.json` (`KeyboardEnabled`, `MouseEnabled`, `ControllerEnabled`, `DisplayEnabled`).
- 2-second refresh timer independently polls SCM status and corrects the display if the service was started or stopped externally.

Service name mapping:

| Driver | Service name |
|--------|-------------|
| Keyboard | `vhidkb` |
| Mouse | `vhidmouse` |
| Controller | `vxinput` |
| Display | `vdisplay` |

### Restart server

**Restart Server** button → `ServiceController("KVMService").Stop()` wait 10 s → `.Start()` wait 10 s. All active remote connections are dropped during the restart. The service re-reads `settings.json` on startup.

### TOFU connection approval

When an unknown remote client connects:

1. The `ConnectionApprovalDialog` appears with: client IP, protocol (WebSocket / VNC), timestamp, and whether the client provided a bearer token.
2. Choices:
   - **Approve**: allows this connection once; future connections from the same IP will prompt again.
   - **Approve + Trust**: allows this connection and appends `<IP> <expiry-epoch>` to `trusted_clients.txt`; future connections auto-approved until expiry.
   - **Block**: rejects this connection; IP is not added to any list.
3. If the dialog is not answered within 30 seconds, the connection is rejected automatically.

### Trusted clients management

Settings tab → Trusted Clients section:
- **Refresh**: reloads `trusted_clients.txt` into the list view.
- **Revoke Selected**: removes the selected IP from `trusted_clients.txt`; that IP will face TOFU again on next connection.
- **Revoke All**: clears `trusted_clients.txt` entirely.

### Diagnostics

Diagnostics tab:

| Button | Action |
|--------|--------|
| **Run Health Checks** | Checks driver service states, device handle accessibility (`\\.\vhidkb` etc.), firewall rules for ports 8443/5900/8080, disk space, pending reboots. Results shown with ✅/⚠️/❌. |
| **Repair Selected** | Attempts repair for the selected check: starts driver service, runs pnputil reinstall, adds firewall rule, etc. |
| **Fix All Firewall Rules** | Adds inbound allow rules for all three ports via `netsh advfirewall`. Requires elevation. |
| **Export Audit Log** | Opens a save dialog; writes `audit_log.csv` with all recorded connection events. |

**Startup firewall check**: 2 seconds after the tray opens, it checks for missing firewall rules and offers to fix them via a MessageBox prompt. This runs once per tray session.

### Auth token management

Settings tab → Authentication:
- **Generate Token**: creates a 24-byte cryptographically random base64 token. This token can be provided by remote clients in `auth.authenticate {token}` to bypass the TOFU dialog.
- **Save Settings**: required to persist the generated token.

### Log viewer

The Log tab shows real-time service events (timestamped, auto-scrolling). Buffer is capped at 100 KB; trimmed to 50 KB when exceeded. **Export Logs** writes the buffer to `%LOCALAPPDATA%\KVM-Drivers\kvmlogs.txt`. **Clear Logs** empties the buffer.

---

## Settings

Settings are persisted to `%LOCALAPPDATA%\KVM-Drivers\settings.json`. The tray reads this file on startup; `KVMService.exe` reads it independently at startup (using a minimal key-search parser with no JSON library dependency).

### Configurable values

| Setting | Default | Effect |
|---------|---------|--------|
| `VncPort` | 5900 | VNC server port (requires service restart) |
| `WebSocketPort` | 8443 | WebSocket server port (requires service restart) |
| `HttpPort` | 8080 | HTTP server port (requires service restart) |
| `VncRequireAuth` | true | Whether VNCAuth is offered |
| `VncPassword` | `""` | VNC password (max 8 chars for DES) |
| `UseTls` | false | TLS for WebSocket (not yet wired) |
| `VncAnonTls` | false | AnonTLS wrapper for VNC |
| `VncCertPin` | `""` | SHA-1 thumbprint for VNC cert pinning |
| `VncMaxClients` | 10 | Max simultaneous VNC connections |
| `WsMaxClients` | 10 | Max simultaneous WebSocket connections (≤ 32) |
| `RequireRemoteAuth` | true | Enforce TOFU for non-localhost connections |
| `TrustOnFirstUse` | true | Show approval dialog (vs auto-reject unknowns) |
| `AuthToken` | `""` | Pre-shared bearer token |
| `AllowedIPs` | `[]` | Allowlist (empty = allow all) |
| `AutoStartDrivers` | false | Start enabled drivers on tray launch |
| `KeyboardEnabled` | true | Auto-started by `AutoStartDrivers` |
| `MouseEnabled` | true | Auto-started by `AutoStartDrivers` |
| `ControllerEnabled` | false | Auto-started by `AutoStartDrivers` |
| `DisplayEnabled` | false | Auto-started by `AutoStartDrivers` |
| `AutoStartWithWindows` | false | Writes/removes HKCU Run entry |
| `MinimizeToTray` | true | Window hides instead of minimizing to taskbar |
| `StartMinimized` | false | Tray opens with window hidden |

**Import / Export**: settings can be exported to and imported from any `.json` file. Useful for deploying a standard configuration across multiple machines.

**Reset to defaults**: restores all values to the defaults listed above and saves immediately.

---

## Idle and Non-Use States

These are states where the system is fully running but no active remote control or automation is occurring.

| # | State | What is running | What does NOT happen |
|---|-------|----------------|---------------------|
| **N1** | Drivers installed, KVMService stopped | Kernel modules loaded; HID/display devices visible in Device Manager | No IOCTL handles open; no injection possible; no network ports listening |
| **N2** | KVMService running, no client connected | `select()` / `accept()` loops with 100ms timeouts; 100ms service tick; `ProcessServiceTasks()` is a no-op | No frame capture (DXGI capture only starts on `display.start_stream`); near-zero CPU |
| **N3** | Stream running, client paused / slow | `StreamLoop` encodes frames; pushes to `sendBuffer` | If client buffer exceeds 1 MB, frames are silently dropped; adaptive quality may degrade tier |
| **N4** | KVMTray running, minimized to tray | 2s refresh timer polls SCM; `ConnectionApprovalManager` polls `pending_approvals\` every ~500ms | Timer results discarded (window hidden); no visible change |
| **N5** | KVMTray running, all drivers stopped | UI shows gray dots; refresh timer keeps polling | No injection; Start buttons available; no remote control possible |
| **N6** | Service running, drivers not installed | `DriverInterface::Initialize()` fails to open handles; `useDriverInjection = false` | Injection falls back to `SendInput` — works, but input is distinguishable and fails against elevated processes |
| **N7** | HTTP server running, `index.html` not found | HTTP server listens on port 8080 | Every browser `GET /` returns `404 "index.html not found"` page; WebSocket + VNC unaffected |
| **N8** | VNC server failed to bind (port 5900 busy) | Service logs non-fatal error; WebSocket + HTTP continue | No VNC connections accepted; nothing else affected |
| **N9** | WebSocket server failed to bind (port 8443 busy) | Service sets `SERVICE_STOPPED` and exits | This is fatal — no remote control at all; check `netstat -ano \| findstr 8443` |
| **N10** | TOFU pending, tray not running | C++ server blocked; `.request` file in `pending_approvals\` | No dialog shown; 30s timeout → connection rejected; `.request` file remains and must be cleaned manually |
| **N11** | IP allowlist non-empty, client IP not in list | Connection accepted at TCP level → allowlist check fires → connection closed | Client sees close; no TOFU dialog; no trusted_clients entry; event logged to `audit_log.csv` |
| **N12** | Rate limit exceeded | Server drops message; increments drop counter; `ReportDroppedFrame()` may lower quality tier | Message silently discarded; no JSON-RPC error sent; `auditLog.LogRateLimited()` records it |
| **N13** | Controller slot held (30s after disconnect) | VHF device alive; slot marked `controllerSlotOwner = -1` but not released | Input to that slot impossible; slot unavailable to new clients; released by `NetworkLoop` sweep after hold expires |
| **N14** | WebSocket handshake not yet complete | Server buffers TCP bytes; waits for `\r\n\r\n` | Input messages ignored; connection is valid; handshake completes within the first TCP segment in practice |
| **N15** | KVMService.exe launched in console without `--standalone` | `StartServiceCtrlDispatcher` fails immediately | Prints usage hint; exits; no servers start |
| **N16** | KVMTray second-instance launch | Mutex check fires | MessageBox "KVM Tray is already running!" → process exits; no change to running instance |
| **N17** | `settings.json` missing or corrupt | Service: `ReadSettingInt()` returns defaults; Tray: `LoadSettings()` catches exception and returns `new AppSettings()` | System runs with defaults; no error shown to user (debug log only) |
| **N18** | `AutoStartDrivers = false` (default) | Tray starts normally | Driver services remain in whatever SCM state they were in; user must manually click Start |
| **N19** | `display.start_stream` never sent | WebSocket client connected and handshaked | DXGI capture thread never started; no JPEG encoding; connection is valid for input-only use |
| **N20** | `openh264.dll` absent | Video pipeline initializes; runtime load of `openh264.dll` fails | No H.264 encoding; WebSocket stream continues with JPEG frames; VNC unaffected |
| **N21** | vdisplay installed, no DXGI capture session | IDD display adapter visible; `CaptureLoop` calls `AcquireNextFrame` with 100ms timeout | Returns `DXGI_ERROR_WAIT_TIMEOUT` each iteration; clients receive all-black or stale frames |
| **N22** | Adaptive quality at MINIMAL tier | Stream still running; FPS = 5, JPEG quality = 30, max frame 640×480 | Frames are coarse and slow; system is under heavy load; will auto-recover when conditions improve |

### Troubleshooting quick reference

| Symptom | Most likely cause | Check |
|---------|-----------------|-------|
| Remote client rejected with no dialog | Tray not running | Start `KVMTray.exe`; check `pending_approvals\` for stale `.request` files |
| Remote client rejected immediately | IP not in allowlist | Tray → Settings → IP Allowlist (must be empty or include client IP) |
| Web page shows 404 | `index.html` not found | Verify `KVMService.exe` is next to `webclient\index.html` or `index.html` |
| Screen is black | vdisplay not active, or DXGI capture failed | Check Device Manager for virtual display; check service logs for `DuplicateOutput` errors |
| Keys arrive as wrong characters via VNC | N/A — fixed: VNC now uses `InjectVirtualKey` → `SendInput` with VK codes | Verify `vnc_server.cpp` is current build |
| Controller not seen by game | vxinput not started | Tray → Drivers → Controller → Start; check XInput gamepad in Device Manager |
| Service exits immediately | Port 8443 already in use | `netstat -ano \| findstr 8443`; stop conflicting process or change `WebSocketPort` in settings |
| High CPU on server machine | Too many clients streaming | Adaptive quality will reduce FPS and JPEG quality automatically; check tray diagnostics |
| Auth token accepted but screen blank | Stream not started | Client must send `display.start_stream` after authenticating |
| Tray shows "Stopped" but driver is running | SCM state mismatch | Click Refresh in Diagnostics tab; 2s timer will correct on next tick |
