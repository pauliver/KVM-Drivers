#Requires -RunAsAdministrator
<#
.SYNOPSIS
    KVM-Drivers installer/uninstaller.

.DESCRIPTION
    Installs or uninstalls KVM-Drivers from a pre-built Release directory.
    Handles drivers (pnputil), Windows Service (sc.exe), shortcuts, registry
    entries, and the shared %PROGRAMDATA%\KVM-Drivers\ data directory.

.PARAMETER Action
    install   (default) — copy files, install drivers, register service
    uninstall           — stop/remove service, uninstall drivers, remove files
    repair              — re-run driver installation over an existing deployment
    status              — print current state without changing anything

.PARAMETER BuildDir
    Path to the build output directory.
    Defaults to: <script-dir>\..\build\Release

.PARAMETER InstallDir
    Target installation directory.
    Defaults to: C:\Program Files\KVM-Drivers

.PARAMETER NoTestSign
    When present, skips enabling test-signing mode.
    Use only if drivers are WHQL-signed.

.EXAMPLE
    .\Setup.ps1
    .\Setup.ps1 -Action install -BuildDir P:\KVM-Drivers\build\Release
    .\Setup.ps1 -Action uninstall
    .\Setup.ps1 -Action status
#>
[CmdletBinding()]
param(
    [ValidateSet('install','uninstall','repair','status')]
    [string] $Action    = 'install',
    [string] $BuildDir  = '',
    [string] $InstallDir = 'C:\Program Files\KVM-Drivers',
    [switch] $NoTestSign
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ── Helpers ──────────────────────────────────────────────────────────────────

function Write-Step  ([string]$msg) { Write-Host "  ► $msg" -ForegroundColor Cyan }
function Write-Ok    ([string]$msg) { Write-Host "    ✓ $msg" -ForegroundColor Green }
function Write-Warn  ([string]$msg) { Write-Host "    ⚠ $msg" -ForegroundColor Yellow }
function Write-Fail  ([string]$msg) { Write-Host "    ✗ $msg" -ForegroundColor Red }

function Invoke-Pnputil {
    param([string[]]$Args)
    $result = & pnputil.exe @Args 2>&1
    # pnputil exit 259 = ERROR_NO_MORE_ITEMS (driver already installed) — treat as OK
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) {
        Write-Warn "pnputil exited $LASTEXITCODE: $result"
        return $false
    }
    return $true
}

function Get-ServiceStatus ([string]$Name) {
    try { return (Get-Service -Name $Name -ErrorAction Stop).Status }
    catch { return $null }
}

function Stop-ServiceSafe ([string]$Name) {
    $status = Get-ServiceStatus $Name
    if ($status -eq 'Running') {
        Write-Step "Stopping service '$Name'..."
        Stop-Service -Name $Name -Force -ErrorAction SilentlyContinue
        $deadline = (Get-Date).AddSeconds(15)
        while ((Get-ServiceStatus $Name) -ne 'Stopped' -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 200
        }
    }
}

function New-Shortcut ([string]$LinkPath, [string]$Target, [string]$Description) {
    $wsh = New-Object -ComObject WScript.Shell
    $sc  = $wsh.CreateShortcut($LinkPath)
    $sc.TargetPath       = $Target
    $sc.Description      = $Description
    $sc.WorkingDirectory = Split-Path $Target
    $sc.Save()
}

# ── Resolve paths ─────────────────────────────────────────────────────────────

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) {
    $BuildDir = Join-Path $scriptDir '..\build\Release'
}
$BuildDir = (Resolve-Path $BuildDir -ErrorAction SilentlyContinue)?.Path
if (-not $BuildDir -or -not (Test-Path $BuildDir)) {
    Write-Fail "Build directory not found: $BuildDir"
    Write-Host "  Run 'scripts\build.bat Release' first, or pass -BuildDir <path>"
    exit 1
}

$DriversDir = Join-Path $BuildDir 'drivers'
$BinDir     = Join-Path $BuildDir 'bin'

$ServiceExe = Join-Path $BinDir 'KVMService.exe'
$TrayExe    = Join-Path $BinDir 'KVMTray.exe'
$IndexHtml  = Join-Path $BinDir 'webclient\index.html'
$AutoDll    = Join-Path $BinDir 'kvm_automation.dll'

$DataDir    = Join-Path $env:ProgramData 'KVM-Drivers'

# ── STATUS ────────────────────────────────────────────────────────────────────

function Show-Status {
    Write-Host "`nKVM-Drivers Status" -ForegroundColor White
    Write-Host "══════════════════════════════════════════"

    foreach ($drv in @('vhidkb','vhidmouse','vxinput','vdisplay')) {
        $svc = Get-ServiceStatus $drv
        $col = if ($svc -eq 'Running') { 'Green' } elseif ($svc) { 'Yellow' } else { 'Red' }
        Write-Host ("  Driver  {0,-12} {1}" -f $drv, ($svc ?? 'NOT INSTALLED')) -ForegroundColor $col
    }

    $kvmSvc = Get-ServiceStatus 'KVMService'
    $col    = if ($kvmSvc -eq 'Running') { 'Green' } elseif ($kvmSvc) { 'Yellow' } else { 'Red' }
    Write-Host ("  Service KVMService    {0}" -f ($kvmSvc ?? 'NOT INSTALLED')) -ForegroundColor $col

    $ports = @(8443, 5900, 8080)
    foreach ($p in $ports) {
        $listening = (netstat -ano 2>$null | Select-String ":$p\s") -ne $null
        $col = if ($listening) { 'Green' } else { 'Gray' }
        Write-Host ("  Port    {0,-6}        {1}" -f $p, $(if ($listening) { 'LISTENING' } else { 'idle' })) -ForegroundColor $col
    }

    $dataExists = Test-Path $DataDir
    Write-Host ("  DataDir {0}  {1}" -f $DataDir, $(if ($dataExists) { '[exists]' } else { '[absent]' }))
    Write-Host ""
}

if ($Action -eq 'status') { Show-Status; exit 0 }

# ── UNINSTALL ─────────────────────────────────────────────────────────────────

function Invoke-Uninstall {
    Write-Host "`nKVM-Drivers Uninstall" -ForegroundColor White
    Write-Host "══════════════════════════════════════════"

    Write-Step "Stopping and removing KVMService..."
    Stop-ServiceSafe 'KVMService'
    if (Get-ServiceStatus 'KVMService') {
        sc.exe delete KVMService | Out-Null
        Write-Ok "KVMService removed"
    } else {
        Write-Warn "KVMService was not registered"
    }

    Write-Step "Uninstalling kernel drivers..."
    $infNames = @('vhidkb.inf','vhidmouse.inf','vxinput.inf','vdisplay.inf')
    $enumOutput = pnputil /enum-drivers 2>&1
    foreach ($inf in $infNames) {
        # Find published oem name matching original INF
        $match = $enumOutput | Select-String "(?i)$([regex]::Escape($inf))" -Context 5,0
        if ($match) {
            $oemLine = ($match.Context.PreContext + $match.Line) |
                       Select-String 'oem\d+\.inf' | Select-Object -First 1
            if ($oemLine) {
                $oem = ([regex]'oem\d+\.inf').Match($oemLine.Line).Value
                Write-Step "  Removing $oem ($inf)..."
                Invoke-Pnputil /delete-driver, $oem, /uninstall, /force | Out-Null
                Write-Ok "  $inf removed"
            }
        } else {
            Write-Warn "  $inf not found in driver store (already removed?)"
        }
    }

    Write-Step "Removing auto-start registry entry..."
    Remove-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' `
        -Name 'KVM-Drivers' -ErrorAction SilentlyContinue
    Remove-ItemProperty 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' `
        -Name 'KVM-Drivers' -ErrorAction SilentlyContinue
    Write-Ok "Registry cleaned"

    Write-Step "Removing Start Menu shortcut..."
    $sm = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\KVM-Drivers'
    Remove-Item $sm -Recurse -Force -ErrorAction SilentlyContinue
    Write-Ok "Start Menu cleaned"

    Write-Step "Removing install directory..."
    if (Test-Path $InstallDir) {
        Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
        Write-Ok "Install directory removed: $InstallDir"
    } else {
        Write-Warn "Install directory not found: $InstallDir"
    }

    Write-Host ""
    Write-Host "Uninstall complete." -ForegroundColor Green
    Write-Host "User data ($DataDir) was preserved. Remove it manually if desired."
    Write-Host ""
}

# ── INSTALL / REPAIR ──────────────────────────────────────────────────────────

function Invoke-Install ([bool]$repair = $false) {
    $verb = if ($repair) { 'Repair' } else { 'Install' }
    Write-Host "`nKVM-Drivers $verb" -ForegroundColor White
    Write-Host "══════════════════════════════════════════"
    Write-Host "  Build dir  : $BuildDir"
    Write-Host "  Install dir: $InstallDir"
    Write-Host ""

    # ── Validate build artefacts ──────────────────────────────────────────────
    Write-Step "Validating build artefacts..."
    $required = @(
        (Join-Path $DriversDir 'vhidkb.sys'),
        (Join-Path $DriversDir 'vhidmouse.sys'),
        (Join-Path $DriversDir 'vxinput.sys'),
        $ServiceExe,
        $TrayExe
    )
    $missing = $required | Where-Object { -not (Test-Path $_) }
    if ($missing) {
        Write-Fail "Missing build artefacts:"
        $missing | ForEach-Object { Write-Host "      $_" }
        Write-Host "  Run 'scripts\build.bat Release' first."
        exit 1
    }
    Write-Ok "All required binaries present"

    # ── Test signing (skip for WHQL) ──────────────────────────────────────────
    if (-not $NoTestSign) {
        Write-Step "Checking/enabling test signing mode..."
        $bcde = bcdedit /enum 2>&1 | Select-String 'testsigning'
        if ($bcde -match 'Yes') {
            Write-Ok "Test signing already enabled"
        } else {
            Write-Warn "Test signing not enabled — enabling now (requires reboot)"
            bcdedit /set testsigning on | Out-Null
            Write-Ok "Test signing enabled — reboot after install completes"
        }
    }

    # ── Create directories ────────────────────────────────────────────────────
    Write-Step "Creating install directories..."
    foreach ($dir in @(
        $InstallDir,
        (Join-Path $InstallDir 'Drivers'),
        (Join-Path $InstallDir 'Service'),
        (Join-Path $InstallDir 'Applications'),
        (Join-Path $InstallDir 'Applications\webclient'),
        $DataDir,
        (Join-Path $DataDir 'pending_approvals')
    )) {
        New-Item -ItemType Directory -Path $dir -Force -ErrorAction SilentlyContinue | Out-Null
    }

    # Grant LocalService write access to the shared data directory
    $acl = Get-Acl $DataDir
    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        'NT AUTHORITY\LocalService', 'Modify', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
    $acl.AddAccessRule($rule)
    Set-Acl $DataDir $acl -ErrorAction SilentlyContinue
    Write-Ok "Directories created, LocalService ACL applied to DataDir"

    # ── Copy driver files ─────────────────────────────────────────────────────
    Write-Step "Copying driver files..."
    $dstDrivers = Join-Path $InstallDir 'Drivers'
    foreach ($ext in @('*.sys','*.inf','*.cat')) {
        Get-ChildItem (Join-Path $DriversDir $ext) -ErrorAction SilentlyContinue |
            Copy-Item -Destination $dstDrivers -Force
    }
    Write-Ok "Driver files copied to $dstDrivers"

    # ── Copy application files ────────────────────────────────────────────────
    Write-Step "Copying application binaries..."
    $dstSvc  = Join-Path $InstallDir 'Service'
    $dstApps = Join-Path $InstallDir 'Applications'
    Copy-Item $ServiceExe -Destination $dstSvc -Force
    Copy-Item $TrayExe    -Destination $dstApps -Force

    # Optional files (non-fatal if absent)
    if (Test-Path $AutoDll)  { Copy-Item $AutoDll  -Destination $dstApps -Force }
    if (Test-Path $IndexHtml) {
        $dstWeb = Join-Path $dstApps 'webclient'
        New-Item $dstWeb -ItemType Directory -Force -ErrorAction SilentlyContinue | Out-Null
        Copy-Item $IndexHtml -Destination $dstWeb -Force
        Write-Ok "Web client (index.html) deployed"
    } else {
        Write-Warn "index.html not found in build — HTTP web client will return 404"
    }

    # Copy openh264.dll if present (optional H.264 software encoder)
    $h264dll = Join-Path $BinDir 'openh264.dll'
    if (Test-Path $h264dll) {
        Copy-Item $h264dll -Destination $dstSvc -Force
        Write-Ok "OpenH264 encoder copied"
    }

    Write-Ok "Application binaries copied"

    # ── Install drivers ───────────────────────────────────────────────────────
    Write-Step "Installing kernel drivers..."
    $infList = @('vhidkb','vhidmouse','vxinput','vdisplay')
    foreach ($name in $infList) {
        $infPath = Join-Path $dstDrivers "$name.inf"
        if (Test-Path $infPath) {
            Write-Step "  Installing $name.inf..."
            $ok = Invoke-Pnputil /add-driver, $infPath, /install
            if ($ok) { Write-Ok "  $name installed" } else { Write-Warn "  $name install failed (check Event Log)" }
        } else {
            Write-Warn "  $name.inf not found — skipping"
        }
    }

    # ── Register Windows service ──────────────────────────────────────────────
    Write-Step "Registering KVMService..."
    $svcExePath = Join-Path $dstSvc 'KVMService.exe'
    $existing   = Get-ServiceStatus 'KVMService'
    if ($existing) {
        Write-Warn "KVMService already registered — updating binary path..."
        Stop-ServiceSafe 'KVMService'
        sc.exe config KVMService binPath= "`"$svcExePath`"" | Out-Null
    } else {
        sc.exe create KVMService `
            binPath= "`"$svcExePath`"" `
            DisplayName= "KVM Remote Control Service" `
            Description= "Core service for KVM virtual drivers" `
            start= auto `
            obj= "NT AUTHORITY\LocalService" | Out-Null
        sc.exe description KVMService "Hosts WebSocket, VNC, and HTTP servers for KVM-Drivers remote control" | Out-Null
    }
    Write-Ok "KVMService registered (auto-start, LocalService)"

    # ── Start the service ─────────────────────────────────────────────────────
    Write-Step "Starting KVMService..."
    try {
        Start-Service -Name 'KVMService' -ErrorAction Stop
        $deadline = (Get-Date).AddSeconds(15)
        while ((Get-ServiceStatus 'KVMService') -ne 'Running' -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 200
        }
        if ((Get-ServiceStatus 'KVMService') -eq 'Running') {
            Write-Ok "KVMService running"
        } else {
            Write-Warn "KVMService did not reach Running state — check Event Viewer → Application"
        }
    } catch {
        Write-Warn "Could not start KVMService: $($_.Exception.Message)"
    }

    # ── Registry: install path + auto-start tray ──────────────────────────────
    Write-Step "Writing registry entries..."
    $regBase = 'HKLM:\Software\KVM-Drivers'
    if (-not (Test-Path $regBase)) { New-Item $regBase -Force | Out-Null }
    Set-ItemProperty $regBase -Name 'InstallPath' -Value $InstallDir
    Set-ItemProperty $regBase -Name 'Version'     -Value '1.0.0'
    Set-ItemProperty $regBase -Name 'Installed'   -Value 1 -Type DWord

    # Auto-start tray for all users (HKLM Run)
    $trayPath = Join-Path $dstApps 'KVMTray.exe'
    Set-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' `
        -Name 'KVM-Drivers' -Value "`"$trayPath`"" -ErrorAction SilentlyContinue
    Write-Ok "Registry entries written"

    # ── Start Menu shortcut ───────────────────────────────────────────────────
    Write-Step "Creating shortcuts..."
    $smDir = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\KVM-Drivers'
    New-Item $smDir -ItemType Directory -Force -ErrorAction SilentlyContinue | Out-Null
    New-Shortcut (Join-Path $smDir 'KVM-Drivers Tray.lnk') $trayPath 'KVM-Drivers System Tray'
    $desktopDir = [Environment]::GetFolderPath('CommonDesktopDirectory')
    New-Shortcut (Join-Path $desktopDir 'KVM-Drivers Tray.lnk') $trayPath 'KVM-Drivers System Tray'
    Write-Ok "Start Menu and Desktop shortcuts created"

    # ── Summary ───────────────────────────────────────────────────────────────
    Write-Host ""
    Write-Host "══════════════════════════════════════════"
    Write-Host "  KVM-Drivers install complete!" -ForegroundColor Green
    Write-Host ""
    Write-Host "  Web client : http://$(hostname):8080/"
    Write-Host "  WebSocket  : ws://$(hostname):8443"
    Write-Host "  VNC        : $(hostname):5900"
    Write-Host ""
    Write-Host "  Launch KVMTray.exe to manage drivers and connections."
    Write-Host "  (A reboot may be required for drivers to load if test signing was just enabled.)"
    Write-Host ""
    Show-Status
}

# ── Dispatch ──────────────────────────────────────────────────────────────────

switch ($Action) {
    'install'   { Invoke-Install $false }
    'repair'    { Invoke-Install $true  }
    'uninstall' { Invoke-Uninstall }
    'status'    { Show-Status }
}
