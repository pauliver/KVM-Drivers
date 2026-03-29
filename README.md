# KVM-Drivers

A Windows-based computer "piloting" system for remote management, remote control, and automated testing using virtual input/output devices that are indistinguishable from physical hardware.

## Overview

KVM-Drivers creates a virtual hardware abstraction layer that allows external systems to control a Windows machine as if they were physically present. The system uses custom kernel-mode and user-mode drivers to emulate:

- **Virtual Keyboard** - Full HID keyboard emulation with all standard keys
- **Virtual Mouse** - Full HID mouse emulation with movement, buttons, and scroll
- **Virtual Xbox Controller** - Full XInput-compliant gamepad emulation
- **Virtual Monitor** - Display output capture and virtual display injection

## Key Features

### For Remote Management
- Control machines without physical access
- BIOS/UEFI-level remote control (with compatible hardware)
- Works across network boundaries

### For Remote Control
- Seamless desktop sharing with input forwarding
- Low-latency input streaming
- Multi-user session support with permission controls

### For Automated Testing
- Scriptable input sequences
- Screenshot/display capture for validation
- Integration with CI/CD pipelines
- Parallel test execution on multiple machines

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
├── docs/                    # Additional documentation
├── src/
│   ├── drivers/            # Kernel-mode drivers
│   │   ├── vhidkb/         # Virtual HID Keyboard
│   │   ├── vhidmouse/      # Virtual HID Mouse
│   │   ├── vxinput/        # Virtual Xbox Controller
│   │   └── vdisplay/       # Virtual Display Driver
│   ├── usermode/           # User-mode services
│   │   ├── core/           # Core service infrastructure
│   │   ├── remote/         # Remote management protocols
│   │   └── automation/     # Testing automation engine
│   └── tray/               # System tray application
├── tests/                   # Test suites
├── tools/                   # Development and debugging tools
└── scripts/                 # Build and deployment scripts
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

## Security Considerations

- All remote connections use TLS 1.3 encryption
- Certificate-based authentication for remote endpoints
- Audit logging for all input events and connections
- Configurable IP allowlists for remote access
- Driver code signing for production builds

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development guidelines.

## License

[License information to be added]

## Related Documentation

- [DesignDoc.md](DesignDoc.md) - Technical architecture and implementation details
- [Milestones.md](Milestones.md) - Development roadmap and milestones
