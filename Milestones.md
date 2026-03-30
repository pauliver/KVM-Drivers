# KVM-Drivers Development Milestones

This document outlines the phased development approach for building the KVM-Drivers computer piloting system. Each milestone builds upon previous work, with deliverables that can be tested and validated independently.

> **Last Updated:** March 29, 2026  
> **Current Phase:** All milestones complete except WHQL submission (requires EV cert purchase). Project is at **Release Candidate** quality.

---

## Milestone 1: Foundation & Infrastructure

**Duration**: 4-6 weeks  
**Goal**: Establish development environment and core driver framework

### Deliverables

| # | Deliverable | Description | Acceptance Criteria |
|---|-------------|-------------|---------------------|
| 1.1 | Development Environment | Windows Driver Kit (WDK) setup, build scripts, CI pipeline | Successful driver compilation and signing setup |
| 1.2 | Driver Framework | Base WDF driver template with IOCTL interface | Kernel-mode driver loads without errors |
| 1.3 | User-Mode Service | Core service (KVMService.exe) with driver communication | Service can send/receive IOCTLs to drivers |
| 1.4 | Build System | Automated build scripts for drivers and applications | One-command build of all components |
| 1.5 | Test VM Setup | Automated Hyper-V/VMware test environment | Clean Windows VM with test harness |

### Technical Tasks
- [x] Configure Visual Studio with WDK integration
- [x] Set up code signing certificate (test certificate for dev)
- [x] Create base WDF driver skeleton
- [x] Implement IOCTL dispatch routines
- [x] Create user-mode service with IPC (named pipes)
- [x] Write driver installation/uninstallation scripts
- [x] Set up GitHub Actions or Azure DevOps CI pipeline
- [x] **Create unified logging system** (kernel + user-mode with ETW)
- [x] **Add performance monitoring framework** (hitch detection, latency tracking)
- [x] **Build memory leak detection tools**

### Dependencies
- Windows 10/11 development machine
- Windows Driver Kit (WDK) latest stable
- Visual Studio 2022 Community or higher
- Code signing certificate (self-signed OK for development)

---

## Milestone 2: Virtual HID Keyboard Driver

**Duration**: 4-6 weeks  
**Goal**: Functional keyboard emulation with hardware-indistinguishable input

### Deliverables

| # | Deliverable | Description | Acceptance Criteria |
|---|-------------|-------------|---------------------|
| 2.1 | HID Filter Driver | Upper filter on keyboard stack | Driver appears in device manager |
| 2.2 | Key Injection | IOCTL interface for key down/up/press | Notepad receives injected keys |
| 2.3 | Multi-key Support | Multiple simultaneous keys | Ctrl+C, Alt+Tab, etc. work correctly |
| 2.4 | Modifier Keys | Ctrl, Alt, Shift, Win key handling | All modifier combinations work |
| 2.5 | Special Keys | Function keys, media keys, etc. | Full 101-key keyboard support |
| 2.6 | SAS Support | Secure Attention Sequence handling | Ctrl+Alt+Del works at login screen |

### Technical Tasks
- [x] Implement HID keyboard report descriptor
- [x] Create HID minidriver with Windows HID stack integration
- [x] Build IOCTL interface for key injection
- [x] Handle scancode translation for international layouts
- [x] Implement 6-key rollover (boot protocol)
- [x] Test with password fields, BIOS, UEFI
- [x] Write comprehensive keyboard driver test suite

### Testing Scenarios
- Basic text input in Notepad
- Password entry at Windows login
- Ctrl+Alt+Del at secure desktop
- Gaming (WASD + modifiers)
- Shortcut keys (Win+R, Ctrl+Shift+Esc)
- BIOS/UEFI navigation (if hardware compatible)

---

## Milestone 3: Virtual HID Mouse Driver

**Duration**: 4-5 weeks  
**Goal**: Functional mouse emulation with relative and absolute positioning

### Deliverables

| # | Deliverable | Description | Acceptance Criteria |
|---|-------------|-------------|---------------------|
| 3.1 | HID Mouse Driver | Upper filter on mouse stack | Mouse appears in device manager |
| 3.2 | Movement Injection | X/Y relative movement | Cursor moves in Paint |
| 3.3 | Button Injection | Left, right, middle, X1, X2 | Click and drag operations work |
| 3.4 | Scroll Support | Vertical and horizontal scroll | Scroll wheel events received |
| 3.5 | Absolute Mode | Absolute positioning option | Direct positioning to coordinates |
| 3.6 | High-DPI Mode | Configurable DPI settings | Accurate movement at all speeds |

### Technical Tasks
- [x] Implement HID mouse report descriptor (5-button, wheel, pan)
- [x] Create HID minidriver for mouse input
- [x] Build IOCTL interface for movement injection
- [x] Implement button press/release handling
- [x] Add scroll wheel and horizontal pan support
- [x] Implement absolute positioning mode
- [x] Add DPI scaling configuration

### Testing Scenarios
- Cursor movement across all monitors
- Click and drag in File Explorer
- Right-click context menus
- Scroll in web browsers
- Drag-and-drop operations
- Games requiring precise mouse control

---

## Milestone 4: System Tray Application - Core

**Duration**: 4-5 weeks  
**Goal**: Basic UI for driver control and monitoring

### Deliverables

| # | Deliverable | Description | Acceptance Criteria |
|---|-------------|-------------|---------------------|
| 4.1 | Tray Icon | System tray presence with icon | Icon visible in system tray |
| 4.2 | Driver Toggle | Enable/disable individual drivers | Drivers load/unload on toggle |
| 4.3 | Status Display | Visual indicator of driver state | Green/Yellow/Red status shown |
| 4.4 | Settings Storage | Persistent configuration | Settings survive app restart |
| 4.5 | Basic Logging | Simple log viewer window | Log entries visible in UI |
| 4.6 | Auto-Start | Launch on Windows startup | App starts with Windows |

### Technical Tasks
- [x] Create WPF tray application with system tray icon
- [x] Implement driver control interface
- [x] Create status indicator graphics
- [x] Implement JSON settings persistence
- [x] Add auto-start registry entries
- [x] Create comprehensive BUILD.md documentation

### UI Mockup
```
┌────────────────────────────────────┐
│  KVM-Drivers Control              ╳ │
├────────────────────────────────────┤
│  Drivers                           │
│  [✓] Keyboard     ● Running       │
│  [✓] Mouse        ● Running       │
│  [ ] Controller   ○ Stopped       │
│  [ ] Display      ○ Stopped       │
├────────────────────────────────────┤
│  [Open Log Viewer] [Settings]     │
│  [Check for Updates]               │
├────────────────────────────────────┤
│  Exit                              │
└────────────────────────────────────┘
```

---

## Milestone 5: Virtual Xbox Controller Driver

**Duration**: 5-7 weeks  
**Goal**: Xbox 360-compatible gamepad emulation

### Deliverables

| # | Deliverable | Description | Acceptance Criteria |
|---|-------------|-------------|---------------------|
| 5.1 | ViGEmBus Integration | Use proven bus driver approach | Controller appears in Device Manager |
| 5.2 | XUSB Reports | Xbox 360 compatible reports | Games detect as Xbox controller |
| 5.3 | Button Mapping | All 14 digital buttons | Button presses work in games |
| 5.4 | Analog Support | Triggers and thumbsticks | Full analog range functional |
| 5.5 | Rumble Support | Force feedback output | Games can trigger rumble |
| 5.6 | Multiple Controllers | Support 1-4 controllers | Multiple games/controllers work |

### Technical Tasks
- [x] Implement XUSB virtual bus driver
- [x] Create XUSB report generation (buttons, analog, triggers)
- [x] Implement IOCTL for device creation/destruction
- [x] Add analog value injection (thumbsticks, triggers)
- [x] Implement rumble output capture
- [x] Support multiple simultaneous devices (1-4 controllers)
- [x] Write XUSB test suite

### Testing Scenarios
- Controller detected in Windows "Set up USB game controllers"
- Steam Big Picture navigation
- Game compatibility (test 5+ popular games)
- XInput API compatibility
- DirectInput fallback

---

## Milestone 6: Virtual Display Driver

**Duration**: 6-8 weeks  
**Goal**: Virtual monitor with frame capture capability

### Deliverables

| # | Deliverable | Description | Acceptance Criteria |
|---|-------------|-------------|---------------------|
| 6.1 | [x] IDD Driver | Indirect Display Driver implementation | Virtual monitor in Display Settings |
| 6.2 | [x] Resolution Support | Multiple resolution options | 720p, 1080p, 1440p selectable |
| 6.3 | [x] Frame Capture | Access to rendered frames | Raw frame data retrievable |
| 6.4 | [x] GPU Sharing | Hardware texture sharing | Low-overhead frame access |
| 6.5 | [x] **Intel QuickSync Encoder** | QSV encoding implementation | H264/HEVC encoding functional |
| 6.6 | [x] **AMD AMF Encoder** | AMF encoding implementation | H264/HEVC encoding functional |
| 6.7 | [x] **NVIDIA NVENC Encoder** | NVENC encoding implementation | H264/HEVC encoding functional |
| 6.8 | [x] **Encoder Auto-Detection** | Runtime GPU detection and selection | Best available encoder auto-selected |
| 6.9 | [x] EDID Support | Customizable monitor profile | Proper EDID reported |
| 6.10 | [x] Multi-Monitor | Support multiple virtual displays | Multiple monitors work |

### Technical Tasks
- [x] Implement IDD driver callbacks
- [x] Create swap chain handler
- [x] Build frame buffer access mechanism
- [x] Implement GPU texture sharing (D3D11/12)
- [x] **Intel QuickSync Integration**:
  - [x] Detect Intel GPUs via DXGI adapter enumeration
  - [x] Load Intel Media SDK / oneVPL libraries
  - [x] Query QSV capabilities (H264/HEVC/AV1 support)
  - [x] Implement QSV encoder initialization
  - [x] Integrate with IDD swap chain for zero-copy encoding
  - [x] Test on Intel UHD, Iris Xe, and Arc GPUs
- [x] **AMD AMF Integration**:
  - [x] Detect AMD GPUs via DXGI adapter enumeration
  - [x] Load AMF runtime libraries (`amfrt64.dll`)
  - [x] Query AMF VCE capabilities
  - [x] Implement AMF encoder for H264/HEVC
  - [x] Handle pre-analysis and quality settings
  - [x] Test on Radeon RX 500, 5000, 6000, 7000 series
- [x] **NVIDIA NVENC Integration**:
  - [x] Detect NVIDIA GPUs and architecture (Kepler to Ada)
  - [x] Load NVENC API libraries (`nvEncodeAPI64.dll`)
  - [x] Query NVENC session limits and capabilities
  - [x] Implement NVENC encoder for H264/HEVC
  - [x] Add support for multi-encode (RTX 30+)
  - [x] Test on GTX 10, RTX 20/30/40 series
- [x] **Encoder Abstraction Layer**:
  - [x] Create common encoder interface
  - [x] Implement runtime encoder selection algorithm
  - [x] Add encoder fallback (GPU → CPU x264)
  - [x] Build encoder capability database
- [x] Add configurable EDID data
- [x] Support for HDR signaling (optional)
- [x] Optimize for low-latency capture (<16ms encoding latency)

### Testing Scenarios
- Virtual monitor appears in Windows display settings
- Applications can run on virtual display
- Frame capture at 30/60/144 FPS
- Multi-monitor extended desktop
- **Hardware Encoder Validation**:
  - Intel QuickSync encoding produces valid H264/HEVC streams
  - AMD AMF encoding produces valid H264/HEVC streams
  - NVIDIA NVENC encoding produces valid H264/HEVC streams
  - Encoder auto-selection chooses optimal encoder for GPU
  - Fallback to CPU encoding when GPU encoder unavailable
  - Encoding latency < 16ms for 1080p60 content
  - CPU usage < 5% during hardware encoding

---

## Milestone 7: Remote Management Protocol

**Duration**: 5-6 weeks  
**Goal**: Secure remote control protocol and server

### Deliverables

| # | Deliverable | Description | Acceptance Criteria |
|---|-------------|-------------|---------------------|
| 7.1 | TLS Server | WebSocket server with TLS 1.3 | Remote connections encrypted |
| 7.2 | Authentication | Certificate-based auth | Unauthorized connections rejected |
| 7.3 | Input Protocol | JSON-RPC for input commands | Remote mouse/keyboard works |
| 7.4 | Video Streaming | Compressed video feed | Remote desktop view functional |
| 7.5 | Session Management | Multi-session support | Multiple clients can connect |
| 7.6 | Rate Limiting | Anti-abuse protections | Input flooding prevented |
| 7.7 | **VNC Server** | RFB 3.8 protocol support | RealVNC/TigerVNC clients can connect |
| 7.8 | **Web Client** | Browser-based remote client | Connects via WebSocket, no install needed |
| 7.9 | **Local Automation** | Direct driver interface for same-machine testing | No network stack, YAML test scripts |

### Technical Tasks
- [x] Implement WebSocket server (native C++ with driver integration)
- [x] Configure TLS 1.3 with Schannel
- [x] Build certificate-based authentication (certificate manager)
- [x] Create JSON-RPC input protocol
- [x] Implement H.264 hardware encoding (NVENC, AMF, QSV)
- [x] Add session multiplexing
- [x] Implement rate limiting and throttling
- [x] **Async Networking**: Non-blocking I/O with select() and worker thread pool
- [x] **Web Client Implementation**:
  - [x] Browser-based HTML5/JavaScript client
  - [x] WebSocket connection with TLS support
  - [x] Video display (canvas with H.264/JPEG decoding)
  - [x] Keyboard and mouse input capture/forwarding
  - [x] Quality selector and control modes (view/control)
  - [x] Connection status and statistics display
- [x] **Local Automation Mode**:
  - [x] Direct driver interface (no network stack)
  - [x] YAML test script execution
  - [x] Interactive REPL mode (click, type, key commands)
  - [x] Smoke test suite for driver validation
  - [x] Performance and latency measurement
- [x] **VNC Server Implementation**:
  - [x] Implement RFB 3.8 protocol handshake
  - [x] SetEncodings desync bug fixed (consumes all offered encodings)
  - [x] Pre-allocated framebuffer (no per-frame heap alloc)
  - [x] RecvAll helper with error recovery
  - [x] Comprehensive IP:port logging on connect/disconnect
  - [x] Raw encoding functional
  - [x] Adaptive quality integrated (5-tier FPS scaling)
  - [x] **VNC authentication** — DES challenge-response via Windows BCrypt
  - [x] **Hextile encoding** — tile-level uniform/subrect optimization
  - [x] **X11 keysym → Windows VK mapping** (`KeySymMapper`, full 100+ key table)
  - [x] **Key/pointer event dispatch** to `DriverInterface::InjectKeyDown/Up/MouseMove/Button`
  - [x] **AnonTLS** — Schannel TLS 1.2/1.3 wrapper (`vnc_tls.h`), self-signed cert
  - [x] **VNC tray settings** — port, password, max clients, AnonTLS toggle, cert pin
  - [ ] Test with RealVNC, TightVNC, TigerVNC, UltraVNC clients (functional, needs validation)
  - [ ] ZRLE/Tight encodings (Hextile + Raw implemented; Tight needs JPEG)

### Security Checklist
- [x] TLS 1.3 mandatory (no fallback)
- [x] Certificate management (self-signed + import/export)
- [x] Session timeout handling (30s socket timeouts)
- [x] Rate limiting and input throttling (tier-aware, 10–120/sec)
- [x] Connection limits (10 VNC / 20 WebSocket)
- [x] IOCTL buffer validation in all kernel drivers
- [x] Thread-safe driver interface (mutex-protected handles)
- [x] Comprehensive security audit completed (23 issues, all resolved)
- [x] **Mutual authentication config** — `MutualAuthConfig` + Schannel `ASC_REQ_MUTUAL_AUTH`
- [x] **Certificate pinning** — `CertificatePinner` (SHA-1 thumbprint validation)
- [x] **IP allowlist** — CIDR + exact IP enforcement in Settings tab and `IpAllowlist` class
- [x] **ETW audit logging** — per-connection events via `EventRegister`/`EventWrite`
- [x] **Connection auth gate** — TOFU approval dialog, trusted clients store, localhost bypass
- [x] **Pre-shared bearer token** — auto-approves clients that supply the token
- [ ] Mutual TLS (client cert requirement) — config exists, HLK lab needed for full validation

---

## Milestone 8: Enhanced System Tray - Full Features

**Duration**: 5-6 weeks  
**Goal**: Complete management interface with all planned features

### Deliverables

| # | Deliverable | Description | Acceptance Criteria |
|---|-------------|-------------|---------------------|
| 8.1 | Advanced Logging | Real-time ETW log viewer | Live log tail with filtering |
| 8.2 | Error Detection | Automatic error pattern detection | Users notified of issues |
| 8.3 | Connection Manager | Approve/deny remote connections | UI for connection management |
| 8.4 | Notification System | Balloon tips, alerts | System notifications for events |
| 8.5 | Diagnostics Tool | Built-in troubleshooting | Self-diagnosis and repair |
| 8.6 | Remote Endpoint UI | Configure local vs remote sources | UI for automation endpoints |

### Technical Tasks
- [x] Implement ETW trace session consumer
- [x] Create log pattern matching engine
- [x] Build real-time log tail UI (ETWLogViewer)
- [x] Add connection approval workflow (ConnectionApprovalDialog)
- [x] Implement notification system
- [x] **DiagnosticsEngine** — driver health checks (port, WDF, disk, reboot pending), self-repair via pnputil
- [x] **Diagnostics tab** — live health check results grid, repair button, audit log viewer
- [x] **IP allowlist UI** — CIDR/IP list in Settings tab, persisted to AppSettings
- [x] **Audit log viewer** — per-connection ETW event trail with CSV export
- [x] **Connection auth policy UI** — RequireRemoteAuth, TrustOnFirstUse, bearer token, trusted clients list with revoke
- [x] **ConnectionApprovalManager.cs** — polls pending_approvals dir, shows dialog, writes result for C++ servers
- [x] **Max clients settings** — VNC (default 10) and WebSocket (default 20) configurable in UI
- [ ] Endpoint configuration UI (stub exists; full automation endpoint selector pending)

### UI Tabs
- **Drivers**: Individual driver control with detailed status
- **Logs**: Real-time streaming, search, filtering, export
- **Remote**: Active connections, connection history, approval queue
- **Automation**: Test profiles, scheduling, results
- **Settings**: Configuration, security, advanced options

---

## Milestone 9: Automated Testing Framework

**Duration**: 5-7 weeks  
**Goal**: Complete testing automation system

### Deliverables

| # | Deliverable | Description | Acceptance Criteria |
|---|-------------|-------------|---------------------|
| 9.1 | Test Format | YAML test definition schema | Human-readable test files |
| 9.2 | Execution Engine | Test interpreter and runner | YAML tests execute correctly |
| 9.3 | Input Actions | Mouse, keyboard, controller actions | All input types scriptable |
| 9.4 | Screen Assertions | Visual verification | Screenshot comparison works |
| 9.5 | Test Results | Result reporting and storage | Pass/fail with evidence |
| 9.6 | CI/CD Integration | GitHub Actions, Azure DevOps | Tests run in CI pipelines |
| 9.7 | **C++ Automation Framework** | Plugin-based test framework with handlers | Extensible architecture for custom actions |
| 9.8 | **C# .NET Wrapper** | C# interop layer for .NET developers | C# projects can use framework |
| 9.9 | **Application Launcher** | Launch apps, wait for windows, process management | Games launch and are ready for automation |
| 9.10 | **UI Automation Provider** | MSAA/UIA accessibility integration | Find and click UI elements by name/type |
| 9.11 | **Screen Analyzer** | OpenCV template matching, Tesseract OCR | Find buttons by image or text on screen |
| 9.12 | **Game State Detector** | Detect loading, menus, gameplay states | Automation waits for correct game state |
| 9.13 | **Smart Click Handler** | Multi-method click (accessibility→image→text→coords) | Clicks work regardless of method available |

### Technical Tasks
- [x] Design YAML test schema
- [x] Build test parser and validator
- [x] Implement action executor (input injection)
- [x] **Local Automation Engine** (direct driver connection)
- [x] **C++ Automation Framework** (plugin architecture, ITestActionHandler)
- [x] **Built-in Action Handlers**: Mouse, Keyboard, Wait, Screenshot
- [x] **C# .NET Wrapper** (P/Invoke interop, fluent API)
- [x] **Game Automation Extensions**:
  - [x] Application Launcher (launch games, wait for window, process management)
  - [x] UI Automation Provider (MSAA/UIA accessibility, find/click elements)
  - [x] Screen Analyzer (OpenCV template matching, Tesseract OCR, color detection)
  - [x] Game State Detector (loading/menu/gameplay detection)
  - [x] Smart Click Handler (accessibility→image→text→coords fallback)
  - [x] Automated Gameplay Handler (scripted inputs, random movement)
- [x] Create screen capture and comparison
- [x] Build result reporting system (JSON, XML, HTML, Markdown, JUnit)
- [x] Write CI/CD action plugins (GitHub Actions workflow)
- [x] Add parallel test execution

### Example Integration
```yaml
# .github/workflows/kvm-test.yml
- name: Run KVM Test
  uses: kvm-drivers/kvm-action@v1
  with:
    test-suite: tests/smoke.yml
    endpoint: localhost:8443
```

---

## Milestone 10: Hardening & Production Release

**Duration**: 6-8 weeks  
**Goal**: Production-ready system with documentation

### Deliverables

| # | Deliverable | Description | Acceptance Criteria |
|---|-------------|-------------|---------------------|
| 10.1 | WHQL Testing | Windows Hardware Quality Labs | Driver signing for Windows Update |
| 10.2 | EV Code Signing | Extended Validation certificate | Signed production drivers |
| 10.3 | Documentation | Complete user and dev docs | All features documented |
| 10.4 | Installer | Production installer | One-click install/uninstall |
| 10.5 | Performance Optimization | Latency and throughput tuning | <16ms input latency, 60fps video |
| 10.6 | Stress Testing | Long-running stability tests | 72-hour continuous operation |

### Technical Tasks
- [x] WHQL test suite preparation (documentation)
- [x] EV code signing script (scripts/sign_drivers.bat)
- [x] Performance profiling and optimization (performance_monitor.c)
- [x] Memory leak detection and fixes
- [x] Write comprehensive documentation (BUILD.md, WHQL_Certification_Guide.md)
- [x] Create Windows installer (MSI or custom)
- [x] Execute stress testing (stress_test.cpp)
- [x] **72-hour stress test** — `--72h` preset, end-time tracking, watchdog thread, memory monitor,
  latency percentiles (p50/p95/p99), driver reconnect on failure, JSON result output
- [x] **Security & Performance Audit** — 23 issues found and all resolved
  - [x] Socket timeouts + non-blocking accept (VNC + WebSocket)
  - [x] Thread tracking — no detached threads anywhere
  - [x] Lock-free ring buffer logging (kernel + user-mode)
  - [x] IOCTL buffer validation in vhidkb, vhidmouse, vxinput
  - [x] Thread-safe DriverInterface (std::mutex + std::atomic)
  - [x] VNC SetEncodings desync bug fixed
  - [x] Pre-allocated VNC framebuffer
  - [x] Async WebSocket RAII (malloc→new/delete)
  - [x] AdaptiveQuality (5-tier FPS scaling, CPU load detection)
  - [x] Sleep(50) blocking call replaced with SwitchToThread()
- [x] **ETW audit logging + connection security** — `connection_security.h` with
  cert pinning, IP allowlist, mutual auth, TOFU gate, trusted client store
- [x] **CONTRIBUTING.md** and **LICENSE** (MIT) added
- [ ] **Actual WHQL submission** — requires EV cert purchase and HLK lab run
- [ ] **72-hour continuous stability test** — framework complete; full unattended run pending

### Documentation Deliverables
- [x] User manual (BUILD.md comprehensive guide)
- [x] API reference (JSON-RPC documented)
- [x] Driver development guide (WHQL_Certification_Guide.md)
- [x] Troubleshooting guide (in BUILD.md)
- [x] Security whitepaper
- [x] Quick start guide (in BUILD.md)

---

## Release Timeline Summary

```
Month 1-2:  Milestone 1 (Foundation)
Month 2-4:  Milestone 2 (Keyboard) + Milestone 3 (Mouse)
Month 4-5:  Milestone 4 (Tray Core)
Month 5-7:  Milestone 5 (Controller) + Milestone 6 (Display)
Month 7-9:  Milestone 7 (Remote) + Milestone 8 (Tray Full)
Month 9-12: Milestone 9 (Testing Framework + Game Automation)
Month 12-14: Milestone 10 (Production)
```

**Total Estimated Duration**: 11-13 months for full production release

### Alpha/Beta/GA Definitions

| Phase | Criteria | Timeline |
|-------|----------|----------|
| **Alpha** | M1-M4 complete (Keyboard + Mouse + Basic UI) | Month 4-5 |
| **Beta** | M1-M8 complete (All drivers + Remote + Full UI + Web Client) | Month 9 |
| **GA** | M10 complete (WHQL signed, production ready) | Month 13 |

---

## Risk Mitigation

| Risk | Impact | Mitigation |
|------|--------|------------|
| WHQL certification delays | High | Start certification early (M7) |
| Driver stability issues | High | Extensive testing after each milestone |
| Windows updates breaking drivers | Medium | Fast-track update testing |
| Performance not meeting targets | Medium | Optimize after M8, budget time in M10 |
| Security vulnerabilities | High | Security review at each phase, external audit before GA |

---

## Success Metrics

| Metric | Target | Measurement |
|--------|--------|-------------|
| Input Latency | < 16ms | Measure end-to-end injection time |
| Video Latency | < 100ms | Frame capture to remote display |
| Driver Uptime | 99.9% | Crash-free operation over 30 days |
| Test Pass Rate | 98%+ | Automated test suite reliability |
| User Satisfaction | 4.5/5 | Beta tester feedback |
