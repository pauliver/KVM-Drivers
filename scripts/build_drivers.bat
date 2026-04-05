@echo off
REM build_drivers.bat - Build KVM kernel drivers
REM
REM  This script supports two strategies, tried in order:
REM   1. WDK VS Integration  - uses the installed WDK + VS toolset
REM      (requires: WDK for Windows 11/10 installed with VS integration)
REM   2. WDK NuGet packages  - restores WDK/SDK via NuGet and builds without
REM      VS WDK integration (works on any VS 2022+ including VS 2026 v18)
REM
REM  Usage:  build_drivers.bat [Release|Debug]
REM  Output: build\<CONFIG>\drivers\  (.sys + .inf)
REM
REM  NOTE: Driver signing is NOT performed here. To install unsigned drivers
REM  on a test machine run:  bcdedit /set testsigning on  (requires reboot).
REM  For production, sign the .sys files with a valid EV code-signing cert.
setlocal enabledelayedexpansion

set CONFIGURATION=%~1
if "%CONFIGURATION%"=="" set CONFIGURATION=Release

echo ==========================================
echo Building KVM Kernel Drivers (%CONFIGURATION%)
echo ==========================================

REM ── Locate Visual Studio (prefer VS 2022+) ──────────────────────────────────
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% set VSWHERE="%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

set MSBUILD=
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\amd64\MSBuild.exe 2^>nul`) do (
    set MSBUILD=%%i
)
if "!MSBUILD!"=="" (
    echo ERROR: MSBuild not found. Install Visual Studio 2022 or later.
    exit /b 1
)
echo Found MSBuild: !MSBUILD!

REM ── Detect VS root (for vcvarsall / toolset) ─────────────────────────────────
set VS_ROOT=
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    set VS_ROOT=%%i
)

REM ── Output directories ───────────────────────────────────────────────────────
set REPO=%~dp0..
set OUT_DRV=%REPO%\build\%CONFIGURATION%\drivers
if not exist "%OUT_DRV%" mkdir "%OUT_DRV%"

REM ── Strategy 1: Try WDK VS Integration ──────────────────────────────────────
echo.
echo [Strategy 1] Attempting build with installed WDK VS integration...
set WDK_INSTALLED=0

"!MSBUILD!" "%REPO%\src\drivers\vhidkb\vhidkb.vcxproj" ^
    /p:Configuration=%CONFIGURATION% /p:Platform=x64 ^
    /p:SolutionDir="%REPO%\\" ^
    /p:OutDir="%OUT_DRV%\\" ^
    /t:Build /m /v:q /nologo > nul 2>&1
if %errorlevel% equ 0 (
    set WDK_INSTALLED=1
)

if "!WDK_INSTALLED!"=="1" (
    echo [Strategy 1] WDK VS integration detected — building all drivers...
    set FAILED=0
    for %%D in (vhidkb vhidmouse vxinput vdisplay) do (
        echo   Building %%D...
        "!MSBUILD!" "%REPO%\src\drivers\%%D\%%D.vcxproj" ^
            /p:Configuration=%CONFIGURATION% /p:Platform=x64 ^
            /p:SolutionDir="%REPO%\\" ^
            /p:OutDir="%OUT_DRV%\\" ^
            /t:Build /m /v:m /nologo
        if errorlevel 1 (
            echo   ERROR: %%D build failed.
            set FAILED=1
        ) else (
            if exist "%OUT_DRV%\%%D.sys" (
                echo   OK: %OUT_DRV%\%%D.sys
            )
        )
    )
    if "!FAILED!"=="1" (
        echo.
        echo ERROR: One or more drivers failed to build.
        exit /b 1
    )
    goto :success
)

REM ── Strategy 2: WDK via NuGet packages ──────────────────────────────────────
echo [Strategy 1] WDK VS integration NOT found.
echo [Strategy 2] Attempting NuGet WDK package build...
echo.

REM Check for NuGet
set NUGET=
where nuget.exe >nul 2>&1
if %errorlevel% equ 0 (
    set NUGET=nuget.exe
) else (
    REM Try local copy
    if exist "%REPO%\tools\nuget.exe" set NUGET=%REPO%\tools\nuget.exe
)

if "!NUGET!"=="" (
    echo    nuget.exe not found. Downloading to tools\...
    if not exist "%REPO%\tools" mkdir "%REPO%\tools"
    powershell -NoProfile -Command ^
        "Invoke-WebRequest -Uri 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile '%REPO%\tools\nuget.exe'"
    if errorlevel 1 (
        echo    ERROR: Could not download nuget.exe. See manual install instructions below.
        goto :nuget_help
    )
    set NUGET=%REPO%\tools\nuget.exe
)

REM Restore WDK NuGet packages for all driver projects
echo    Restoring WDK NuGet packages...
for %%D in (vhidkb vhidmouse vxinput vdisplay) do (
    if exist "%REPO%\src\drivers\%%D\packages.config" (
        "!NUGET!" restore "%REPO%\src\drivers\%%D\packages.config" ^
            -SolutionDirectory "%REPO%" ^
            -NonInteractive -Verbosity quiet
    )
)

REM Build with NuGet-provided toolset
set FAILED=0
set WDK_NUGET_PKG=%REPO%\packages\Microsoft.Windows.WDK.x64.10.0.26100.2454
set SDK_CPP_PKG=%REPO%\packages\Microsoft.Windows.SDK.CPP.10.0.26100.56
set SDK_CPP_X64_PKG=%REPO%\packages\Microsoft.Windows.SDK.CPP.x64.10.0.26100.56

REM Check packages were actually restored
if not exist "!WDK_NUGET_PKG!" (
    echo    ERROR: WDK NuGet package not restored. Check packages.config files.
    goto :nuget_help
)

for %%D in (vhidkb vhidmouse vxinput vdisplay) do (
    echo   Building %%D (NuGet WDK)...
    "!MSBUILD!" "%REPO%\src\drivers\%%D\%%D.vcxproj" ^
        /p:Configuration=%CONFIGURATION% /p:Platform=x64 ^
        /p:SolutionDir="%REPO%\\" ^
        /p:OutDir="%OUT_DRV%\\" ^
        /p:WDKContentRoot="!WDK_NUGET_PKG!\" ^
        /p:WindowsSdkDir="!SDK_CPP_PKG!\" ^
        /t:Build /m /v:m /nologo
    if errorlevel 1 (
        echo   ERROR: %%D build failed.
        set FAILED=1
    ) else (
        echo   OK: %OUT_DRV%\%%D.sys
    )
)

if "!FAILED!"=="1" (
    echo.
    echo ERROR: One or more drivers failed to build via NuGet WDK.
    echo See :nuget_help section below for manual steps.
    exit /b 1
)

:success
echo.
echo ==========================================
echo Driver build complete. Output: %OUT_DRV%
echo ==========================================
echo.
echo Next steps (test machine, test-signed only):
echo   bcdedit /set testsigning on  ^& reboot
echo   pnputil /add-driver %OUT_DRV%\vhidkb.inf    /install
echo   pnputil /add-driver %OUT_DRV%\vhidmouse.inf /install
echo   pnputil /add-driver %OUT_DRV%\vxinput.inf   /install
endlocal
exit /b 0

:nuget_help
echo.
echo ══════════════════════════════════════════════════════════════════════
echo  MANUAL SETUP — WDK build environment not detected
echo ══════════════════════════════════════════════════════════════════════
echo.
echo  Option A (Recommended): Install WDK VS integration
echo  ─────────────────────────────────────────────────
echo  1. Go to: https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk
echo  2. Download and install the WDK matching your SDK (10.0.26100.x)
echo  3. During install, check "Windows Driver Kit Visual Studio extension"
echo  4. Re-run this script.
echo.
echo  Option B: Use Enterprise WDK (no installer, CI-friendly)
echo  ─────────────────────────────────────────────────────────
echo  1. Download EWDK ISO from the same page above
echo  2. Mount the ISO:  Mount-DiskImage ewdk.iso
echo  3. Launch the EWDK environment:  D:\LaunchBuildEnv.cmd
echo  4. In that environment, run this script.
echo.
echo  Option C: NuGet WDK packages (packages.config already provided)
echo  ─────────────────────────────────────────────────────────────────
echo  1. Install NuGet CLI: winget install Microsoft.NuGet
echo  2. Run this script — it will restore packages automatically.
echo.
endlocal
exit /b 1
