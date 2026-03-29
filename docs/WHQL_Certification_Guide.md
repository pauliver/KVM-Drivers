# WHQL Certification Guide

## Overview

This document provides guidance for obtaining Windows Hardware Quality Labs (WHQL) certification for KVM-Drivers, enabling Microsoft driver signing and Windows Update distribution.

## Prerequisites

- Windows Hardware Lab Kit (HLK) - latest version
- Windows 10/11 client and server test systems
- EV Code Signing Certificate (DigiCert, GlobalSign, etc.)
- Microsoft Partner Center account

## Preparation Steps

### 1. Driver Requirements

Before WHQL submission, ensure drivers meet these criteria:

- [ ] Signed with EV certificate (test signing during development)
- [ ] No test/debug code paths in release builds
- [ ] Proper error handling and recovery
- [ ] Resource cleanup in all exit paths
- [ ] No hardcoded paths or credentials
- [ ] Compatible with Windows 10 (2004+) and Windows 11

### 2. HLK Test Setup

1. Install Windows HLK on controller system
2. Configure test clients:
   - Windows 10 x64 (client edition)
   - Windows 11 x64 (client edition)
   - Windows Server 2022 (if server support needed)

3. Install test drivers on clients:
   ```powershell
   pnputil /add-driver vhidkb.inf /install
   pnputil /add-driver vhidmouse.inf /install
   pnputil /add-driver vxinput.inf /install
   pnputil /add-driver vdisplay.inf /install
   ```

### 3. Required HLK Tests

#### HID Class Drivers (vhidkb, vhidmouse)

**Required Test Categories:**
- Device Fundamentals
  - DF - PNP (disable/enable, surprise removal)
  - DF - Reboot reboot test
  - DF - Sleep with IO
  - DF - Stress (long-haul IO)
  
- HID Specific
  - HIDClass - HID Device Test
  - HIDClass - HID Client Test
  - HIDClass - HID Minidriver Test
  
- Security
  - Device Guard - HVCI test
  - Driver security checklist

#### Xbox Controller Driver (vxinput)

**Required Test Categories:**
- Device Fundamentals (same as HID)
- Game Controller Tests
  - XInput - Device Enumeration
  - XInput - Input Reporting
  - XInput - Force Feedback

#### Display Driver (vdisplay - IDD)

**Required Test Categories:**
- Device Fundamentals
- Indirect Display Tests
  - IDD - Adapter Initialization
  - IDD - Monitor Arrival/Removal
  - IDD - Swap Chain Processing
- Display Tests
  - Display - Monitor EDID
  - Display - Resolution Changes

### 4. Running HLK Tests

1. Create project in HLK Studio
2. Select target devices for each driver
3. Run complete test suite
4. Review failures and fix issues
5. Re-run failed tests until passing

```powershell
# HLK PowerShell example
Get-HLKTest -Target <device> | Where-Object {$_.Status -eq "Failed"}
Restart-HLKTest -Test <test_id>
```

### 5. Creating Submission Package

After all tests pass:

1. In HLK Studio: Project → Create Submission Package
2. Export HLKX file
3. Sign with EV certificate:
   ```powershell
   signtool sign /f mycert.pfx /p password /tr http://timestamp.digicert.com mysubmission.hlkx
   ```

## Submission Process

### Microsoft Partner Center

1. Log in to [Partner Center Hardware Dashboard](https://partner.microsoft.com/dashboard)
2. Navigate to "Hardware submissions"
3. Click "Create new submission"
4. Upload signed HLKX package
5. Provide driver details:
   - Driver version
   - Supported OS versions
   - Device IDs
   - Hardware IDs
   - Feature scores

### Submission Details Template

```
Driver Name: KVM Virtual HID Keyboard
Version: 1.0.0.0
OS Support: Windows 10 2004+, Windows 11
Device IDs: HID_DEVICE_SYSTEM_KEYBOARD
Hardware IDs: HID\VID_5A63&PID_0001

Driver Name: KVM Virtual HID Mouse
Version: 1.0.0.0
OS Support: Windows 10 2004+, Windows 11
Device IDs: HID_DEVICE_SYSTEM_MOUSE
Hardware IDs: HID\VID_5A63&PID_0002

Driver Name: KVM Virtual Xbox Controller
Version: 1.0.0.0
OS Support: Windows 10 2004+, Windows 11
Device IDs: XUSB\VID_045E&PID_028E
Hardware IDs: USB\VID_045E&PID_028E

Driver Name: KVM Virtual Display
Version: 1.0.0.0
OS Support: Windows 10 2004+, Windows 11
Device IDs: IndirectDisplay\KVM_VDisplay
Hardware IDs: KVM\VirtualDisplay
```

### Expected Timeline

- Submission review: 24-72 hours
- Test verification: 2-5 business days
- Certificate generation: 1-2 days after pass

## EV Code Signing Setup

### Certificate Requirements

- Extended Validation (EV) Code Signing Certificate
- From Microsoft-trusted CA (DigiCert, GlobalSign, Sectigo, etc.)
- Hardware token or HSM for private key storage

### Signing Process

1. **Install certificate** on build machine:
   ```powershell
   # Import to certificate store
   certutil -importPFX mycert.pfx AT_KEYEXCHANGE
   ```

2. **Sign driver files** (post-build):
   ```powershell
   signtool sign /s MY /n "Your Company" /tr http://timestamp.digicert.com `
     /td SHA256 /fd SHA256 /a driver.sys
   ```

3. **Verify signature**:
   ```powershell
   signtool verify /pa driver.sys
   signtool verify /kp driver.sys  # Kernel mode
   ```

4. **Catalog file creation**:
   ```powershell
   inf2cat /driver:package_dir /os:10_X64,Server10_X64
   signtool sign /s MY /n "Your Company" driver.cat
   ```

### Automated Signing in CI/CD

```yaml
# .github/workflows/sign.yml
name: Sign Drivers
on:
  release:
    types: [created]

jobs:
  sign:
    runs-on: windows-latest
    steps:
      - name: Checkout
        uses: actions/checkout@v3
        
      - name: Setup EV Certificate
        uses: ./.github/actions/setup-ev-cert
        with:
          pfx-base64: ${{ secrets.EV_CERT_BASE64 }}
          password: ${{ secrets.EV_CERT_PASSWORD }}
          
      - name: Build Drivers
        run: scripts/build.bat Release drivers
        
      - name: Sign Drivers
        run: |
          Get-ChildItem build/Release/drivers/*.sys | ForEach-Object {
            signtool sign /s MY /n "KVM-Drivers" /tr http://timestamp.digicert.com `
              /td SHA256 /fd SHA256 $_.FullName
          }
          
      - name: Create Catalog
        run: |
          inf2cat /driver:build/Release/drivers /os:10_X64,Server10_X64
          signtool sign /s MY /n "KVM-Drivers" build/Release/drivers/*.cat
          
      - name: Upload Signed Drivers
        uses: actions/upload-artifact@v3
        with:
          name: signed-drivers
          path: build/Release/drivers/*
```

## Stress Testing Requirements

### HLK Stress Tests (Required)

1. **Sleep and PNP Tests**
   - 100+ sleep/resume cycles
   - No crashes or data corruption
   - Driver reloads correctly

2. **IO Stress**
   - Continuous IO for 12+ hours
   - Target: 1M+ input events
   - Memory usage stable

3. **Concurrent Access**
   - Multiple clients simultaneous
   - No deadlocks or race conditions
   - Proper synchronization

### Custom Stress Suite

```powershell
# stress_test.ps1 - Run before WHQL
param(
    [int]$DurationHours = 12,
    [int]$EventsPerSecond = 100
)

# Continuous input injection
$startTime = Get-Date
$eventCount = 0

while ((Get-Date) - $startTime -lt [TimeSpan]::FromHours($DurationHours)) {
    # Inject random input
    test_harness.exe --random --duration 1
    
    $eventCount += $EventsPerSecond
    Start-Sleep -Milliseconds (1000 / $EventsPerSecond)
    
    # Log progress every hour
    if ($eventCount % ($EventsPerSecond * 3600) -eq 0) {
        Write-Host "Progress: $(([Get-Date] - $startTime).TotalHours) hours, $eventCount events"
    }
}

Write-Host "Stress test complete: $eventCount events processed"
```

### Memory Leak Detection

Run driver verifier with pool tracking:

```powershell
# Enable driver verifier
verifier /standard /driver vhidkb.sys vhidmouse.sys vxinput.sys vdisplay.dll

# Reboot and run tests
# Monitor pool usage
!poolused 2  # WinDbg command
```

## Common WHQL Failures

| Failure | Cause | Solution |
|---------|-------|----------|
| DF - Reboot Test | Driver doesn't unload properly | Ensure EvtDriverUnload cleans up all resources |
| DF - Sleep Test | Power IRP handling | Implement proper power management callbacks |
| IO Stress Timeout | Slow operation completion | Optimize data path, reduce lock contention |
| Security Test | Buffer overflow vulnerabilities | Use safe string functions, validate all inputs |
| INF Test | INF file syntax errors | Run infverif.exe, fix all warnings |

## Post-Certification

### Windows Update Distribution

After WHQL passing:

1. Driver signed by Microsoft (re-signed)
2. Automatic Windows Update distribution
3. Optional: Targeted rollouts via Windows Update for Business

### Maintenance

- Monitor telemetry for crashes
- Respond to bug reports within SLA
- Submit updated drivers for new Windows versions

## Resources

- [Windows HLK Documentation](https://docs.microsoft.com/windows-hardware/test/hlk/)
- [Driver Signing Requirements](https://docs.microsoft.com/windows-hardware/drivers/install/kernel-mode-code-signing-requirements)
- [Partner Center Help](https://docs.microsoft.com/windows-hardware/drivers/dashboard/)
- [WHQL Blog](https://techcommunity.microsoft.com/t5/windows-hardware-certification/bg-p/WindowsHardwareCertification)
