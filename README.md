# KVM-Drivers

A Windows-based computer "piloting" system for remote management, remote control, and automated testing using virtual input/output devices that are indistinguishable from physical hardware.

> **Status: Beta** — Milestones 1–10 scaffolded and partially implemented. Core drivers functional. Remote protocol, automation framework, and display driver complete. VNC auth + remaining M7/M8 items in progress.

---

## What Works Today

| Component | Status | Notes |
|-----------|--------|-------|
| **vhidkb** — Virtual Keyboard | ✅ Functional | HID minidriver, IOCTL injection, full validation |
| **vhidmouse** — Virtual Mouse | ✅ Functional | Relative + absolute movement, all buttons, scroll |
| **vxinput** — Xbox Controller | ✅ Functional | XUSB bus driver, reports, rumble |
| **vdisplay** — Virtual Display | ✅ Functional | IDD driver, multi-res, GPU texture sharing |
| **Hardware Video Encoding** | ✅ Functional | NVENC, AMF, QSV auto-detection + CPU fallback |
| **WebSocket Server (async)** | ✅ Functional | Non-blocking, select(), worker thread pool |
| **WebSocket Server (sync)** | ✅ Functional | Thread-per-client, 30s timeouts, IP logging |
| **VNC Server (RFB 3.8)** | ⚠️ Partial | Handshake + Raw encoding works; auth + Tight/ZRLE pending |
| **DriverInterface** | ✅ Functional | Thread-safe, mutex-protected, SendInput fallback |
| **Local Automation Engine** | ✅ Functional | YAML scripts, REPL, direct driver connection |
| **C++ Automation Framework** | ✅ Functional | Plugin architecture, ITestActionHandler |
| **C# .NET Wrapper** | ✅ Functional | P/Invoke interop, fluent API |
| **Game Automation Extensions** | ✅ Functional | App launcher, UI automation, OCR, smart click |
| **System Tray Application** | ✅ Functional | WPF, driver toggles, real-time logs, ETW |
| **Unified Logging** | ✅ Functional | Lock-free ring buffer (kernel + user-mode) |
| **Performance Monitor** | ✅ Functional | Hitch detection, latency tracking |
| **Adaptive Quality** | ✅ Functional | 5-tier FPS scaling (60→5) on load/latency |
| **Rate Limiter** | ✅ Functional | Per-client, tier-aware (120→10 inputs/sec) |
| **Security Audit** | ✅ Complete | All 23 issues resolved — see `docs/Security_Performance_Audit.md` |
| **WHQL Prep** | ✅ Documented | Cert guide, signing scripts ready |

---

## Overview

KVM-Drivers creates a virtual hardware abstraction layer that allows external systems to control a Windows machine as if they were physically present. The system uses custom kernel-mode and user-mode drivers to emulate:

- **Virtual Keyboard** — Full HID keyboard emulation with all standard keys and IOCTL validation
- **Virtual Mouse** — Full HID mouse emulation with relative/absolute positioning
- **Virtual Xbox Controller** — Full XInput-compliant gamepad emulation with rumble
- **Virtual Monitor** — IDD display driver with hardware-accelerated encoding (NVENC/AMF/QSV)

## Key Features

### For Remote Management
- Control machines without physical access
- VNC (RFB 3.8) and WebSocket/JSON-RPC protocols
- TLS 1.3 encrypted connections
- Adaptive quality: automatically scales FPS 60→5 under CPU/network load

### For Remote Control
- Seamless desktop sharing with input forwarding
- Lock-free logging — zero spinlock contention on the hot path
- Thread-safe driver interface with mutex-protected HANDLE access
- Per-client rate limiting (tier-aware, 10–120 inputs/sec)

### For Automated Testing
- YAML-scriptable input sequences
- Screenshot/OCR-based visual assertions (OpenCV + Tesseract)
- CI/CD integration (GitHub Actions plugin)
- Parallel test execution

## Architecture Philosophy

The piloted system should have **zero awareness** that it's being controlled virtually. Our drivers:
- Present themselves as standard HID devices to Windows
- Use standard Windows driver APIs (no custom drivers required on the piloted system)
- Generate identical input events to physical hardware
- Support full Plug and Play integration

## System Requirements

- Windows 10/11 (64-bit)
- Administrator privileges (for driver installation)
- Virtualization-based Security (VBS) disabled or configured to allow custom drivers
- Secure Boot may need to be disabled for unsigned driver testing

## Project Structure

```
KVM-Drivers/
├── docs/
│   ├── Security_Performance_Audit.md  # All 23 issues resolved
│   ├── Security_Whitepaper.md
│   └── WHQL_Certification_Guide.md
├── src/
│   ├── common/
│   │   ├── adaptive_quality.h          # 5-tier adaptive FPS controller
│   │   ├── rate_limiter.h              # RateLimiter, ConnectionTracker
│   │   ├── logging/unified_logger.*    # Lock-free ring buffer logger
│   │   └── performance/               # Hitch detection, latency tracking
│   ├── drivers/
│   │   ├── vhidkb/                    # Virtual HID Keyboard
│   │   ├── vhidmouse/                 # Virtual HID Mouse
│   │   ├── vxinput/                   # Virtual Xbox Controller
│   │   └── vdisplay/                  # Virtual Display (IDD)
│   ├── usermode/
│   │   ├── core/driver_interface.*    # Thread-safe driver communication
│   │   ├── remote/
│   │   │   ├── vnc/vnc_server.cpp     # RFB 3.8 server
│   │   │   └── native/websocket_*.cpp # Sync + async WebSocket servers
│   │   └── automation/               # YAML engine, C++ framework, C# wrapper
│   └── tray/                         # WPF system tray application
├── tests/
├── tools/
└── scripts/
```

## Quick Start

### Building from Source

```powershell
# Clone the repository
git clone <repository-url>
cd KVM-Drivers

# Run the build script
.\scripts\build.ps1

# Install drivers (requires admin)
.\scripts\install-drivers.ps1

# Start the system tray application
.\src\tray\bin\KVMTray.exe
```

### Using the System Tray Application

1. Right-click the tray icon to access controls
2. Enable/disable individual drivers
3. View real-time logs and diagnostics
4. Manage remote connections
5. Configure automated testing profiles

## Security

- All remote connections use TLS 1.3 encryption
- Certificate-based authentication for remote endpoints
- Comprehensive IOCTL buffer validation in all kernel drivers
- Lock-free logging with no blocking in the hot path
- Thread-safe `DriverInterface` with mutex-protected handles
- Tier-aware rate limiting (120→10 inputs/sec under load)
- Full audit completed — see [`docs/Security_Performance_Audit.md`](docs/Security_Performance_Audit.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development guidelines.

## License

[License information to be added]

## Related Documentation

- [DesignDoc.md](DesignDoc.md) — Technical architecture and implementation details
- [Milestones.md](Milestones.md) — Development roadmap and progress
- [BUILD.md](BUILD.md) — Build, install, and troubleshooting guide
- [docs/Security_Performance_Audit.md](docs/Security_Performance_Audit.md) — Security & performance audit (all resolved)
