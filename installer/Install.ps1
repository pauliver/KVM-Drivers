# Install.ps1 — KVM-Drivers one-shot installer
#
# Drop this script (and the rest of the unzipped package) anywhere on the
# target machine and run it from PowerShell.  It will self-elevate to
# Administrator if needed.
#
# Steps performed:
#   1  OS prep       — enable test-signing (dev-signed drivers); prompt for reboot
#   2  Copy files    — C:\KVM-Drivers\  (service, tray, web client, drivers)
#   3  Install drivers — pnputil /add-driver /install for each .inf
#   4  Register service — KVMService.exe install (SCM registration)
#   5  Start service  — Start-Service KVMService
#   6  Tray autostart  — HKLM Run key + launch now
#   7  Verify         — service status, listening ports, PnP devices
#   8  Open log folder — explorer.exe to %PROGRAMDATA%\KVM-Drivers\
#
# Usage:
#   Right-click Install.ps1 → "Run with PowerShell"   (auto-elevates)
#   — or —
#   powershell -ExecutionPolicy Bypass -File .\Install.ps1
# ─────────────────────────────────────────────────────────────────────────────

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ── Self-elevation ────────────────────────────────────────────────────────────
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Not running as Administrator - re-launching elevated..." -ForegroundColor Yellow
    $args_ = "-ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Path)`""
    Start-Process powershell -ArgumentList $args_ -Verb RunAs
    exit
}

# ── Config ────────────────────────────────────────────────────────────────────
$InstallDir = "C:\KVM-Drivers"
$DriversDir = "$InstallDir\Drivers"
$ServiceExe = "$InstallDir\KVMService.exe"
$TrayExe    = "$InstallDir\KVMTray.exe"
$LogDir     = "$env:PROGRAMDATA\KVM-Drivers"
$Here       = Split-Path -Parent $MyInvocation.MyCommand.Path   # folder the zip was unzipped to

# ── UI helpers ────────────────────────────────────────────────────────────────
function Write-Banner {
    Clear-Host
    Write-Host ""
    Write-Host "  +======================================================+" -ForegroundColor Cyan
    Write-Host "  |       KVM-Drivers  -  Installation Wizard            |" -ForegroundColor Cyan
    Write-Host "  +======================================================+" -ForegroundColor Cyan
    Write-Host ""
}

function Write-Step ([int]$n, [string]$msg) {
    Write-Host ""
    Write-Host "  [$n/8] $msg" -ForegroundColor Cyan
}

function Write-Ok    ([string]$msg) { Write-Host "        OK  $msg" -ForegroundColor Green  }
function Write-Skip  ([string]$msg) { Write-Host "      SKIP  $msg" -ForegroundColor DarkGray }
function Write-Warn  ([string]$msg) { Write-Host "      WARN  $msg" -ForegroundColor Yellow }
function Write-Fail  ([string]$msg) { Write-Host "      FAIL  $msg" -ForegroundColor Red    }

function Read-Confirmation ([string]$question, [string]$default = 'Y') {
    $hint = if ($default -eq 'Y') { '[Y/n]' } else { '[y/N]' }
    $ans  = Read-Host "        $question $hint"
    if ([string]::IsNullOrWhiteSpace($ans)) { $ans = $default }
    return ($ans -match '^[Yy]')
}

# ═══════════════════════════════════════════════════════════════════════════════
Write-Banner
Write-Host "  This script will install KVM-Drivers on this machine." -ForegroundColor White
Write-Host "  Install directory : $InstallDir" -ForegroundColor DarkGray
Write-Host "  Log directory     : $LogDir"     -ForegroundColor DarkGray
Write-Host ""
Write-Host "  NOTE: Drivers are DEV-SIGNED only - test-signing will be enabled." -ForegroundColor Yellow
Write-Host "        For production use, WHQL-signed drivers require an EV cert." -ForegroundColor DarkGray
Write-Host ""
if (-not (Read-Confirmation "Continue with installation?")) {
    Write-Host "  Installation cancelled." -ForegroundColor DarkGray
    exit 0
}


# ─────────────────────────────────────────────────────────────────────────────
# STEP 1 — OS prep: test-signing
# ─────────────────────────────────────────────────────────────────────────────
Write-Step 1 "OS prep - test-signing mode"

$bcdedit = (bcdedit /enum '{current}' 2>$null) -join "`n"
$tsOn = $bcdedit -match 'testsigning\s+Yes'

if ($tsOn) {
    Write-Ok "Test-signing already enabled"
} else {
    Write-Warn "Test-signing is OFF. Enabling now (requires a reboot to take effect)..."

    bcdedit /set testsigning on | Out-Null
    Write-Ok "bcdedit /set testsigning on"

    Write-Host ""
    Write-Host "  +------------------------------------------------------------+" -ForegroundColor Yellow
    Write-Host "  |  A REBOOT IS REQUIRED before drivers can be installed.     |" -ForegroundColor Yellow
    Write-Host "  |  After rebooting, re-run this script - Step 1 will be      |" -ForegroundColor Yellow
    Write-Host "  |  skipped automatically and installation will continue.     |" -ForegroundColor Yellow
    Write-Host "  +------------------------------------------------------------+" -ForegroundColor Yellow
    Write-Host ""

    if (Read-Confirmation "Reboot now?") {
        Restart-Computer -Force
    } else {
        Write-Host ""
        Write-Host "  Reboot manually, then re-run Install.ps1 to continue." -ForegroundColor Yellow
        Write-Host "  Press Enter to exit..."
        Read-Host | Out-Null
        exit 0
    }
}


# ─────────────────────────────────────────────────────────────────────────────
# STEP 2 — Copy files to C:\KVM-Drivers\
# ─────────────────────────────────────────────────────────────────────────────
Write-Step 2 "Copying files to $InstallDir"

New-Item -ItemType Directory -Path $InstallDir  -Force | Out-Null
New-Item -ItemType Directory -Path $DriversDir  -Force | Out-Null
New-Item -ItemType Directory -Path $LogDir      -Force | Out-Null

# Main binaries
$binaries = @('KVMService.exe', 'KVMTray.exe', 'index.html', 'openh264.dll')
foreach ($f in $binaries) {
    $src = Join-Path $Here $f
    if (Test-Path $src) {
        Copy-Item $src $InstallDir -Force
        Write-Ok "Copied $f"
    } else {
        Write-Skip "$f not in package"
    }
}

# Drivers sub-folder
$driversSrc = Join-Path $Here 'drivers'
if (Test-Path $driversSrc) {
    $count = 0
    Get-ChildItem $driversSrc | ForEach-Object {
        Copy-Item $_.FullName $DriversDir -Force
        $count++
    }
    Write-Ok "Copied $count driver files to $DriversDir"
} else {
    Write-Fail "drivers\ folder not found in package - cannot proceed"
    Read-Host "Press Enter to exit"
    exit 1
}


# ─────────────────────────────────────────────────────────────────────────────
# STEP 3 — Install kernel drivers via pnputil
# ─────────────────────────────────────────────────────────────────────────────
Write-Step 3 "Installing kernel drivers"

$driverOrder = @('vhidkb', 'vhidmouse', 'vxinput', 'vdisplay')
foreach ($d in $driverOrder) {
    $inf = Join-Path $DriversDir "$d.inf"
    if (-not (Test-Path $inf)) { Write-Skip "$d.inf not found"; continue }
    Write-Host "        Installing $d..." -NoNewline
    $out = & pnputil /add-driver $inf /install 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host " OK" -ForegroundColor Green
    } else {
        Write-Host " FAILED" -ForegroundColor Red
        Write-Warn ($out -join ' ')
    }
}


# ─────────────────────────────────────────────────────────────────────────────
# STEP 4 — Register KVMService with SCM
# ─────────────────────────────────────────────────────────────────────────────
Write-Step 4 "Registering KVMService with Windows Service Manager"

$svc = Get-Service 'KVMService' -ErrorAction SilentlyContinue
if ($svc) {
    Write-Ok "KVMService already registered (status: $($svc.Status))"
} else {
    if (-not (Test-Path $ServiceExe)) {
        Write-Fail "KVMService.exe not found at $ServiceExe"
        Read-Host "Press Enter to exit"
        exit 1
    }
    & $ServiceExe install
    if ($LASTEXITCODE -eq 0) { Write-Ok "KVMService registered" }
    else                     { Write-Warn "KVMService.exe install returned $LASTEXITCODE - check event log" }
}


# ─────────────────────────────────────────────────────────────────────────────
# STEP 5 — Start KVMService
# ─────────────────────────────────────────────────────────────────────────────
Write-Step 5 "Starting KVMService"

try {
    Start-Service 'KVMService'
    Start-Sleep -Seconds 2
    $svc = Get-Service 'KVMService'
    if ($svc.Status -eq 'Running') {
        Write-Ok "KVMService is RUNNING"
    } else {
        Write-Warn "KVMService status: $($svc.Status)  - check $LogDir\KVMService.log"
    }
} catch {
    Write-Warn "Could not start service: $_"
}


# ─────────────────────────────────────────────────────────────────────────────
# STEP 6 — Tray: set autostart + launch now
# ─────────────────────────────────────────────────────────────────────────────
Write-Step 6 "Configuring KVMTray - autostart at login + launch"

if (-not (Test-Path $TrayExe)) {
    Write-Warn "KVMTray.exe not found at $TrayExe - skipping tray setup"
} else {
    # Write HKLM Run key (all users)
    $runKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
    Set-ItemProperty -Path $runKey -Name 'KVMTray' -Value "`"$TrayExe`"" -Type String
    Write-Ok "KVMTray added to HKLM\\...\\Run (will start for all users on login)"

    # Launch now if not already running
    if (Get-Process 'KVMTray' -ErrorAction SilentlyContinue) {
        Write-Ok "KVMTray already running"
    } else {
        Start-Process $TrayExe
        Start-Sleep -Seconds 2
        if (Get-Process 'KVMTray' -ErrorAction SilentlyContinue) {
            Write-Ok "KVMTray launched"
        } else {
            Write-Warn "KVMTray did not appear in process list - .NET 8 runtime may be missing"
            Write-Warn "Install: winget install Microsoft.DotNet.DesktopRuntime.8"
        }
    }
}


# ─────────────────────────────────────────────────────────────────────────────
# STEP 7 — Verify
# ─────────────────────────────────────────────────────────────────────────────
Write-Step 7 "Verifying installation"

Write-Host ""
Write-Host "  Service status:" -ForegroundColor White
Get-Service 'KVMService' -ErrorAction SilentlyContinue |
    Format-Table @{L='Name';E={$_.Name}},
                 @{L='Status';E={$_.Status}},
                 @{L='StartType';E={$_.StartType}} -AutoSize |
    Out-String | ForEach-Object { Write-Host "  $_" }

Write-Host "  Listening ports  (8080=HTTP  8443=WebSocket  5900=VNC):" -ForegroundColor White
$listening = netstat -an 2>$null | Select-String ':8080|:8443|:5900'
if ($listening) {
    $listening | ForEach-Object { Write-Host "    $_" -ForegroundColor Green }
} else {
    Write-Warn "No KVM ports LISTENING yet - service may still be starting"
}

Write-Host ""
Write-Host "  KVM PnP devices:" -ForegroundColor White
$devs = Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -match 'KVM|Virtual HID|vhid|vxinput|vdisplay' }
if ($devs) {
    $devs | Format-Table FriendlyName, Status, Class -AutoSize |
            Out-String | ForEach-Object { Write-Host "  $_" }
} else {
    Write-Warn "No KVM devices visible yet - a reboot may be needed for drivers to enumerate"
}


# ─────────────────────────────────────────────────────────────────────────────
# STEP 8 — Open log folder in Explorer
# ─────────────────────────────────────────────────────────────────────────────
Write-Step 8 "Opening log folder in Explorer"

if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }
Start-Process explorer.exe $LogDir
Write-Ok "Opened $LogDir"


# ─────────────────────────────────────────────────────────────────────────────
# Done
# ─────────────────────────────────────────────────────────────────────────────
$localIP = (Get-NetIPAddress -AddressFamily IPv4 |
            Where-Object { $_.InterfaceAlias -notmatch 'Loopback|vEthernet' } |
            Sort-Object PrefixLength |
            Select-Object -First 1).IPAddress

Write-Host ""
Write-Host "  +==============================================================+" -ForegroundColor Green
Write-Host "  |   Installation complete!                                     |" -ForegroundColor Green
Write-Host "  +==============================================================+" -ForegroundColor Green
Write-Host "  |                                                              |" -ForegroundColor Green
Write-Host "  |  Web client :  http://${localIP}:8080/                        |" -ForegroundColor Green
Write-Host "  |  VNC        :  ${localIP}:5900                                |" -ForegroundColor Green
Write-Host "  |  WebSocket  :  ws://${localIP}:8443                           |" -ForegroundColor Green
Write-Host "  |                                                              |" -ForegroundColor Green
Write-Host "  |  Log file   :  $LogDir\KVMService.log                          |" -ForegroundColor Green
Write-Host "  |                                                              |" -ForegroundColor Green
Write-Host "  |  First remote connection will show an approval dialog         |" -ForegroundColor Green
Write-Host "  |  in the tray.  Localhost connections are auto-approved.       |" -ForegroundColor Green
Write-Host "  +==============================================================+" -ForegroundColor Green
Write-Host ""
Read-Host "  Press Enter to close"
