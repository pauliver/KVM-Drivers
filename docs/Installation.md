# KVM-Drivers Installation Guide

All methods for installing, updating, and removing KVM-Drivers on a Windows 10/11 x64 system.

---

## Contents

- [Prerequisites](#prerequisites)
- [Installation Methods](#installation-methods)
  - [A — MSI Installer (end-user)](#a--msi-installer-end-user)
  - [B — Script install (dev / CI)](#b--script-install-dev--ci)
  - [C — Manual pnputil (per-driver)](#c--manual-pnputil-per-driver)
  - [D — Service-only registration](#d--service-only-registration)
- [Driver Signing](#driver-signing)
- [Auto-start Configuration](#auto-start-configuration)
- [Verifying the Installation](#verifying-the-installation)
- [Updating](#updating)
- [Uninstallation](#uninstallation)
- [Runtime File Locations](#runtime-file-locations)

---

## Prerequisites

| Requirement | Notes |
|-------------|-------|
| Windows 10 or 11, 64-bit | Build 19041+ (20H1) recommended |
| Administrator privileges | Required for all driver and service operations |
| .NET 8 Runtime | Required for `KVMTray.exe` only (tray app) |
| VCRuntime 2022 | Required for `KVMService.exe` and drivers |
| Virtualization-based Security (VBS) | Must be disabled **or** the system must be configured to allow third-party kernel modules |
| Secure Boot | Must be disabled for dev-signed (unsigned) drivers; not required for WHQL-signed releases |

> **WHQL status**: As of v1.0 RC3, all four drivers are dev-signed. WHQL submission requires an EV code-signing certificate purchase — this is the only remaining step before a public release. See [`docs/WHQL_Certification_Guide.md`](WHQL_Certification_Guide.md).

---

## Installation Methods

### A — MSI Installer (end-user)

The WiX-built MSI (`KVM-Drivers-1.0.0.msi`) is the recommended path for end users.

**Building the MSI** (from source):
```
installer\build_installer.bat
```

**Running the installer** (as Administrator):
```
msiexec /i KVM-Drivers-1.0.0.msi
```
Or double-click the `.msi` file and follow the wizard.

**What the MSI does, in order:**

| Step | Detail |
|------|--------|
| Prerequisite checks | Requires Windows 10+ (`VersionNT >= 603`) and 64-bit Windows (`VersionNT64`) |
| File copy | Drivers → `%ProgramFiles%\KVM-Drivers\Drivers\` |
| File copy | `KVMService.exe` → `%ProgramFiles%\KVM-Drivers\Service\` |
| File copy | `KVMTray.exe`, `index.html`, `kvm_automation.dll` → `%ProgramFiles%\KVM-Drivers\Applications\` |
| Driver install | Runs `pnputil.exe /add-driver vhidkb.inf /install` as a deferred custom action (elevated) |
| Service registration | Registers `KVMService` (display: "KVM Remote Control Service") as `SERVICE_AUTO_START`, account `NT AUTHORITY\LocalService`, depends on `RpcSs` |
| Service start | Starts `KVMService` immediately after install |
| Registry | Writes `HKLM\Software\KVM-Drivers` → `InstallPath`, `Version`, `Installed=1` |
| Auto-start tray | Writes `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run` → `KVMTray.exe` (all users) |
| Start Menu | Shortcut to `KVM-Drivers Tray` in `%ProgramMenu%\KVM-Drivers\` |
| Desktop | Shortcut to `KVM-Drivers Tray` |

**Silent install:**
```
msiexec /i KVM-Drivers-1.0.0.msi /quiet /norestart INSTALLFOLDER="C:\KVM-Drivers"
```

---

### B — Script install (dev / CI)

`scripts/install.bat` installs drivers from the build output. Must be run as Administrator.

```bat
REM Install Release build (default)
scripts\install.bat

REM Install Debug build
scripts\install.bat Debug
```

The script:
1. Checks for admin privileges via `net session`
2. For each driver (`vhidkb`, `vhidmouse`, `vxinput`, `vdisplay`), checks whether the corresponding `.sys` / `.dll` exists in `build\<Config>\`
3. Runs `pnputil /add-driver src\drivers\<name>\<name>.inf /install` for each found binary
4. Warns but continues if an individual driver fails
5. Prompts for reboot

**The service is not registered by this script.** Register it separately:
```
build\Release\bin\KVMService.exe install
```
Or start it manually for one-shot use:
```
build\Release\bin\KVMService.exe --standalone
```

---

### C — Manual pnputil (per-driver)

Any subset of the four drivers can be installed independently. The service degrades gracefully for any missing driver (opens fewer handles and falls back to `SendInput` for that input type).

```powershell
# Virtual HID Keyboard
pnputil /add-driver build\Release\drivers\vhidkb.inf /install

# Virtual HID Mouse
pnputil /add-driver build\Release\drivers\vhidmouse.inf /install

# Virtual Xbox Controller (XInput / HID gamepad)
pnputil /add-driver build\Release\drivers\vxinput.inf /install

# Virtual Display (IDD — Indirect Display Driver)
pnputil /add-driver build\Release\drivers\vdisplay.inf /install
# Alternative: regsvr32 build\Release\drivers\vdisplay.dll
```

> Each `pnputil /add-driver` call copies the INF + SYS + CAT into `%SystemRoot%\INF\` and registers the device. The driver loads immediately; no reboot is required for HID drivers. The display driver may require a reboot.

**Removing a single driver:**
```powershell
# Find its oem*.inf name first
pnputil /enum-drivers | findstr /i "vhidkb"
# Then remove
pnputil /delete-driver oem42.inf /uninstall /force
```

---

### D — Service-only registration

Use when drivers are already installed and you only need to register or re-register `KVMService.exe` with the Windows Service Control Manager.

```bat
REM Register as auto-start service
KVMService.exe install

REM Remove service registration (stops service first)
KVMService.exe uninstall
```

`install` calls `CreateService()` with `SERVICE_AUTO_START`, `SERVICE_WIN32_OWN_PROCESS`, no service account specified (runs as LocalSystem by default when invoked this way — unlike the MSI which uses `NT AUTHORITY\LocalService`).

---

## Driver Signing

| Scenario | Required action |
|----------|----------------|
| **WHQL-signed release** | No action needed; drivers install and load normally |
| **Dev-signed (current builds)** | Enable test signing: `bcdedit /set testsigning on` and reboot |
| **Self-signed cert** | Generate via `scripts\sign_drivers.bat`; import cert to Trusted Root; enable test signing |
| **Secure Boot enabled** | Either disable Secure Boot in UEFI, or enroll the signing cert in MOK (Machine Owner Key) |
| **VBS / HVCI** | `bcdedit /set hypervisorlaunchtype off` (or configure WDAC policy to allow the driver) |

After enabling test signing, a watermark "Test Mode" appears in the bottom-right corner of the Windows desktop. This is expected.

For WHQL submission details see [`docs/WHQL_Certification_Guide.md`](WHQL_Certification_Guide.md).

---

## Auto-start Configuration

There are two independent auto-start mechanisms:

### 1. KVMService (Windows Service — auto-start)
Set by the MSI installer, or by `KVMService.exe install`. The service starts at every boot before any user logs in. This is the recommended production configuration.

To change start type after install:
```powershell
Set-Service -Name KVMService -StartupType Automatic    # or Manual / Disabled
```

### 2. KVMTray (tray application — user login)
Set via:
- MSI: writes `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run` (all users)
- Tray Settings tab → **Auto-start with Windows** checkbox: writes `HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Run` (current user only)

The tray app is purely a management UI and is **not required** for the service or drivers to function. If no tray is running, connection approval dialogs cannot appear and unknown remote clients will be rejected after a 30-second timeout.

### 3. Individual driver auto-start
Each kernel driver (`vhidkb`, `vhidmouse`, `vxinput`, `vdisplay`) is registered as a Windows service with its own start type. After `pnputil /add-driver … /install`, the driver typically starts on-demand when the PnP manager enumerates its device node.

The tray Drivers tab Start/Stop buttons control these per-driver services via `ServiceController`.

---

## Verifying the Installation

### Check installed drivers
```powershell
# List all HID-class devices (shows vhidkb, vhidmouse)
pnputil /enum-devices /class HIDClass

# List gamepad devices (shows vxinput)
pnputil /enum-devices /class XnaComposite

# Via Device Manager
devmgmt.msc
# → Human Interface Devices: "Virtual HID Keyboard", "Virtual HID Mouse"
# → Sound, video and game controllers: "Virtual Xbox Controller"
# → Display adapters: "Virtual Display Monitor"
```

### Check driver handles are accessible
```powershell
$handles = @("vhidkb","vhidmouse","vxinput","vdisplay")
foreach ($h in $handles) {
    $dev = Get-Item "\\.\$h" -ErrorAction SilentlyContinue
    Write-Host "$h : $(if ($dev) {'OK'} else {'NOT ACCESSIBLE'})"
}
```

### Check service status
```powershell
Get-Service KVMService | Select-Object Status, StartType
Get-Service vhidkb, vhidmouse, vxinput, vdisplay | Select-Object Name, Status
```

### Check network ports
```powershell
netstat -ano | findstr "8443 5900 8080"
# Expect LISTENING entries for each
```

### Run the built-in test harness
```powershell
cd build\Release\tests
.\test_harness.exe
# Output: "Injection mode: Kernel Drivers" (or "SendInput Fallback" if drivers not accessible)
```

### Run tray diagnostics
Open `KVMTray.exe` → **Diagnostics** tab → **Run Health Checks**. All rows should show ✅. Any ⚠️ or ❌ row has a **Repair Selected** button for self-repair.

---

## Updating

### MSI upgrade
The MSI uses a fixed `UpgradeCode` (`{12345678-1234-1234-1234-123456789012}`). Run the new MSI over an existing install — the `MajorUpgrade` element handles removal of the old version automatically. Downgrading is blocked.

### Manual update
1. Stop the service: `net stop KVMService` (or tray → Restart Server)
2. Replace binaries in the install directory
3. If drivers changed: `pnputil /delete-driver oem*.inf /uninstall /force` then `pnputil /add-driver <new>.inf /install`
4. Start the service: `net start KVMService`

---

## Uninstallation

### MSI uninstall
```
msiexec /x KVM-Drivers-1.0.0.msi
# Or via: Programs and Features → KVM-Drivers → Uninstall
```
The MSI runs `pnputil.exe /delete-driver oem*.inf /uninstall` before file removal.

### Script uninstall
```bat
scripts\uninstall.bat
```
Enumerates published INFs matching `vhidkb.inf`, `vhidmouse.inf`, `vxinput.inf`, `vdisplay.inf` via `pnputil /enum-drivers`, then calls `pnputil /delete-driver <oem>.inf /uninstall /force` for each.

### Manual uninstall
```powershell
# Stop and remove service
net stop KVMService
KVMService.exe uninstall          # or: sc delete KVMService

# Remove each driver
pnputil /enum-drivers | findstr /i "vhidkb\|vhidmouse\|vxinput\|vdisplay"
# For each oem*.inf found:
pnputil /delete-driver oem42.inf /uninstall /force

# Remove auto-start registry entries
Remove-ItemProperty HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run -Name "KVM-Drivers" -ErrorAction SilentlyContinue
Remove-ItemProperty HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run -Name "KVM-Drivers" -ErrorAction SilentlyContinue
Remove-Item HKLM:\Software\KVM-Drivers -Recurse -ErrorAction SilentlyContinue

# Remove user data (optional)
Remove-Item "$env:LOCALAPPDATA\KVM-Drivers" -Recurse -Force
```

---

## Runtime File Locations

| Path | Contents |
|------|----------|
| `%LOCALAPPDATA%\KVM-Drivers\settings.json` | Tray settings (ports, auth, driver flags) — read by both tray and service at startup |
| `%LOCALAPPDATA%\KVM-Drivers\trusted_clients.txt` | Approved remote client IPs with expiry timestamps (one per line: `<IP> <epoch>`) |
| `%LOCALAPPDATA%\KVM-Drivers\audit_log.csv` | Per-connection ETW audit trail (Connected, Disconnected, AuthFailed, RateLimited events) |
| `%LOCALAPPDATA%\KVM-Drivers\pending_approvals\` | File-based IPC between C++ servers and the C# tray; `<UUID>.request` written by server, `<UUID>.result` written by tray |
| `%LOCALAPPDATA%\KVM-Drivers\settings.json.backup` | Automatic backup of settings before each save |
| `<ExeDir>\webclient\index.html` | Web client HTML (also searched at `<ExeDir>\index.html` and up to 6 parent dirs) |
| `<ExeDir>\openh264.dll` | Optional: Cisco OpenH264 software encoder (drop alongside `KVMService.exe` to activate H.264) |
