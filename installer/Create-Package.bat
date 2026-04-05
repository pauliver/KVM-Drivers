@echo off
REM Create-Package.bat
REM
REM  Builds KVMService.exe (if not already built), then assembles
REM  installer\Install.zip — a self-contained package you can drop on
REM  any Windows 10/11 machine and run Install.ps1 to install everything.
REM
REM  Contents of Install.zip:
REM    Install.ps1          ← guided 8-step install script
REM    KVMService.exe
REM    KVMTray.exe
REM    index.html           ← web client
REM    openh264.dll         ← software H.264 encoder (optional, copied if present)
REM    drivers\
REM      vhidkb.sys  + vhidkb.inf
REM      vhidmouse.sys + vhidmouse.inf
REM      vxinput.sys   + vxinput.inf
REM      vdisplay.sys/dll + vdisplay.inf
REM
REM  Usage:
REM    installer\Create-Package.bat [Release|Debug]
REM
setlocal enabledelayedexpansion

set CONFIG=%~1
if "!CONFIG!"=="" set CONFIG=Release

set REPO=%~dp0..
set BUILD=%REPO%\build\!CONFIG!
set BIN=%BUILD%\bin
set DRV=%BUILD%\drivers
set STAGE=%REPO%\build\pkg_stage
set OUT=%REPO%\installer\Install.zip

echo.
echo ==========================================
echo  KVM-Drivers  --  Create Install Package
echo  Config  : !CONFIG!
echo  Output  : !OUT!
echo ==========================================
echo.

REM ── Step 1: Build KVMService.exe if binary is missing ────────────────────
if not exist "!BIN!\KVMService.exe" (
    echo [1/4] KVMService.exe not found -- building now...
    call "!REPO!\scripts\build_service.bat" !CONFIG!
    if errorlevel 1 (
        echo ERROR: build_service.bat failed. Fix build errors first.
        exit /b 1
    )
) else (
    echo [1/4] KVMService.exe already built -- skipping rebuild.
)

REM Check KVMTray.exe
if not exist "!BIN!\KVMTray.exe" (
    echo.
    echo WARNING: KVMTray.exe not found at !BIN!\KVMTray.exe
    echo          Build the tray app first:  cd src\tray ^&^& dotnet publish
    echo          Continuing without it -- Install.ps1 will skip tray setup.
    echo.
)

REM ── Step 2: Check driver binaries ────────────────────────────────────────
echo [2/4] Checking driver binaries...
set DRIVERS_OK=1
for %%d in (vhidkb vhidmouse vxinput vdisplay) do (
    if not exist "!DRV!\%%d.inf" (
        echo   WARNING: %%d.inf not found in !DRV!
        echo            Build drivers first: scripts\build_drivers.bat !CONFIG!
        set DRIVERS_OK=0
    )
)
if "!DRIVERS_OK!"=="0" (
    echo.
    set /p CONT="   Continue packaging without all drivers? [Y/N]: "
    if /i "!CONT!" neq "Y" exit /b 1
)

REM ── Step 3: Assemble staging folder ──────────────────────────────────────
echo [3/4] Assembling staging folder...
if exist "!STAGE!" rmdir /s /q "!STAGE!"
mkdir "!STAGE!"
mkdir "!STAGE!\drivers"

REM Install script
copy /y "!REPO!\installer\Install.ps1" "!STAGE!\Install.ps1" >nul
echo   + Install.ps1

REM Main binaries
for %%f in (KVMService.exe KVMTray.exe index.html) do (
    if exist "!BIN!\%%f" (
        copy /y "!BIN!\%%f" "!STAGE!\%%f" >nul
        echo   + %%f
    ) else (
        echo   - %%f  [not found, skipped]
    )
)

REM Optional OpenH264 DLL (drop openh264.dll next to KVMService.exe to enable software encoding)
if exist "!BIN!\openh264.dll" (
    copy /y "!BIN!\openh264.dll" "!STAGE!\openh264.dll" >nul
    echo   + openh264.dll
)

REM Drivers
for %%d in (vhidkb vhidmouse vxinput vdisplay) do (
    for %%e in (sys dll inf cat) do (
        if exist "!DRV!\%%d.%%e" (
            copy /y "!DRV!\%%d.%%e" "!STAGE!\drivers\%%d.%%e" >nul
            echo   + drivers\%%d.%%e
        )
    )
)

REM ── Step 4: Zip the staging folder ───────────────────────────────────────
echo [4/4] Creating Install.zip...

REM Delete old zip
if exist "!OUT!" del /f /q "!OUT!"

powershell -NoProfile -Command ^
  "Compress-Archive -Path '!STAGE!\*' -DestinationPath '!OUT!' -CompressionLevel Optimal"

if errorlevel 1 (
    echo ERROR: Compress-Archive failed.
    exit /b 1
)

REM Cleanup staging folder
rmdir /s /q "!STAGE!"

REM Report
for %%Z in ("!OUT!") do set SIZE=%%~zZ
set /a SIZE_KB=!SIZE! / 1024

echo.
echo ==========================================
echo  Package created successfully
echo  File : !OUT!
echo  Size : ~!SIZE_KB! KB
echo ==========================================
echo.
echo  To install on another machine:
echo    1. Copy Install.zip to the target machine
echo    2. Right-click Install.zip -^> Extract All
echo    3. Right-click Install.ps1 -^> Run with PowerShell
echo       (or: powershell -ExecutionPolicy Bypass -File Install.ps1)
echo.
endlocal
