# Contributing to KVM-Drivers

Thank you for your interest in contributing! This document covers the development workflow, code standards, and how to submit changes.

---

## Table of Contents
- [Development Setup](#development-setup)
- [Code Standards](#code-standards)
- [Submitting Changes](#submitting-changes)
- [Testing Requirements](#testing-requirements)
- [Driver Development Notes](#driver-development-notes)
- [Security Reporting](#security-reporting)

---

## Development Setup

### Prerequisites

| Tool | Version | Purpose |
|------|---------|---------|
| Visual Studio 2022 | 17.x+ | C++/C# development |
| Windows Driver Kit (WDK) | 10.0.26100+ | Kernel driver compilation |
| .NET 8 SDK | 8.x+ | Tray application |
| Git | any | Version control |

### Clone and Build

```powershell
git clone https://github.com/pauliver/KVM-Drivers.git
cd KVM-Drivers
.\scripts\build.ps1
```

### Running Tests

```powershell
# Unit tests
.\tests\test_harness.exe

# Short stress test (1 hour)
.\tests\stress_test.exe --hours 1 --output results.json

# Full 72-hour stability test
.\tests\stress_test.exe --72h --output results_72h.json
```

---

## Code Standards

### C++ (Drivers & User-mode)

- **No blocking calls** on hot paths — use `select()` with timeouts, never infinite `recv()` or `Sleep()` in server loops
- **No detached threads** — all `std::thread` objects must be tracked and joined on shutdown
- **RAII everywhere** — use `new`/`delete` (not `malloc`/`free`), RAII wrappers for handles
- **Thread safety** — protect shared state with `std::mutex`; use `std::atomic` for flags and counters
- **IOCTL validation** — always validate `InputBufferLength` before `WdfRequestRetrieveInputBuffer`; return `STATUS_BUFFER_TOO_SMALL` on invalid input
- **Logging** — add `KdPrint`/`std::cout` with `[Component]` prefix for all significant operations; log errors with `NT_SUCCESS` check results
- **Error paths** — every function that can fail must check and propagate the error; no silent failures

### C# (Tray Application)

- Use `async`/`await` for all UI-blocking operations (`Task.Run` for CPU-bound work)
- Bind data via `ObservableCollection` — no direct UI manipulation from background threads
- Persist all user-configurable state through `SettingsManager`
- Use `DiagnosticsEngine.LogAuditEvent` for all connection/security events

### Kernel Mode Specifics

- Allocate with `ExAllocatePool2` (not the deprecated `ExAllocatePoolWithTag`)
- Every allocation must have a matching free on all exit paths
- No spinlocks held across calls that might page fault
- Use `_IRQL_requires_max_` annotations on all functions
- Lock-free ring buffer pattern preferred over spinlocks for logging

---

## Submitting Changes

1. **Fork** the repository and create a feature branch:
   ```powershell
   git checkout -b feature/my-change
   ```

2. **Write tests** for new functionality before implementing (or alongside)

3. **Run the audit scripts** before opening a PR:
   ```powershell
   .\scripts\MemoryLeakAudit.ps1 -SourcePath .\src -Verbose
   .\scripts\PerformanceAudit.ps1 -SourcePath .\src
   ```

4. **Commit with descriptive messages** — describe *what* and *why*, not just *what*:
   ```
   Add Hextile encoding to VNC server: tile-level uniform detection reduces
   bandwidth 3-5x for typical desktop content vs Raw encoding
   ```

5. **Open a Pull Request** against `main`. Your PR description should include:
   - What the change does
   - How you tested it
   - Any security implications
   - Performance impact (if applicable)

---

## Testing Requirements

All PRs must pass:

| Test | Requirement |
|------|-------------|
| Build (x64 Release) | Zero errors, zero warnings |
| `test_keyboard.cpp` | All keyboard injection tests pass |
| `test_mouse.cpp` | All mouse injection tests pass |
| `test_controller_xusb.c` | XUSB report tests pass |
| Stress test (30 min) | Error rate < 0.1%, no watchdog fires |
| Memory leak audit | No new unmatched alloc/free pairs |

### For driver changes specifically:
- Run with **Driver Verifier** enabled (pool tracking, deadlock detection)
- Test unload/reload at least 10 times without BSOD
- Test with slow/faulty network simulation if network-adjacent

---

## Driver Development Notes

### HID Report Descriptors
- Keyboard: 8-byte boot protocol (Report ID, Modifier, Reserved, 6×KeyCode)
- Mouse: 7-byte (Report ID, Buttons, X[2], Y[2], Wheel, HWheel)
- Controller: XUSB-compatible 20-byte report

### Adding a New IOCTL
1. Define in the driver's `*_ioctl.h` with `CTL_CODE`
2. Add to the `switch` in `*EvtIoDeviceControl`
3. Add `InputBufferLength` pre-check before `ValidateIoctlBuffer`
4. Add log statements for success and failure
5. Add corresponding `DeviceIoControl` call in `driver_interface.cpp`
6. Add test in the relevant `test_*.cpp`

### Adding a New Server Security Feature
Use `ConnectionSecurityContext::Global()` — it provides:
- `auditLog` — ETW + stderr audit trail
- `certPinner` — SHA-1 thumbprint validation
- `ipAllowlist` — CIDR/IP enforcement
- `mutualAuth` — client cert requirement config

---

## Security Reporting

**Do not open public issues for security vulnerabilities.**

Report security issues privately by emailing the maintainer directly. Include:
- Description of the vulnerability
- Steps to reproduce
- Potential impact
- Suggested fix (if known)

We aim to acknowledge within 48 hours and patch within 14 days for critical issues.

---

## License

By contributing, you agree your contributions will be licensed under the same license as this project. See [LICENSE](LICENSE) for details.
