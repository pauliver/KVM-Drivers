@echo off
REM sign_drivers.bat - EV Code Signing script for production releases
REM Requires: EV Code Signing Certificate installed in certificate store

setlocal enabledelayedexpansion

echo ==========================================
echo KVM-Drivers EV Code Signing
echo ==========================================
echo.

REM Configuration
set CERT_SUBJECT="KVM-Drivers Project"
set TIMESTAMP_URL=http://timestamp.digicert.com
set HASH_ALG=SHA256

REM Parse arguments
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set DRY_RUN=%2
if /I "%DRY_RUN%"=="--dry-run" (
    echo [DRY RUN] No actual signing will occur
echo.
    set DRY_RUN=1
) else (
    set DRY_RUN=0
)

set DRIVER_DIR=build\%CONFIG%\drivers

REM Check for signtool
where signtool >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: signtool not found. Install Windows SDK.
    exit /b 1
)

REM Verify certificate exists
echo Checking for EV certificate...
signtool verify /q /s MY /n %CERT_SUBJECT% >nul 2>&1
if %errorlevel% neq 0 (
    echo WARNING: Certificate not found in store. Checking certutil...
    certutil -store MY | findstr /C:%CERT_SUBJECT% >nul
    if %errorlevel% neq 0 (
        echo ERROR: EV certificate not found in MY store.
        echo Install certificate: certutil -importPFX mycert.pfx
        exit /b 1
    )
)
echo Certificate found.
echo.

REM Check if driver files exist
if not exist %DRIVER_DIR%\*.sys (
    echo ERROR: No driver files found in %DRIVER_DIR%
    echo Build drivers first: build.bat %CONFIG% drivers
    exit /b 1
)

echo Files to sign:
dir /b %DRIVER_DIR%\*.sys %DRIVER_DIR%\*.dll 2>nul
echo.

REM Sign each driver
set SIGN_FAIL=0

for %%f in (%DRIVER_DIR%\*.sys %DRIVER_DIR%\*.dll) do (
    echo Signing: %%~nxf
    
    if %DRY_RUN%==1 (
        echo [DRY RUN] Would execute:
        echo   signtool sign /s MY /n %CERT_SUBJECT% /tr %TIMESTAMP_URL% /td %HASH_ALG% /fd %HASH_ALG% "%%f"
    ) else (
        signtool sign /s MY /n %CERT_SUBJECT% /tr %TIMESTAMP_URL% /td %HASH_ALG% /fd %HASH_ALG% "%%f"
        if !errorlevel! neq 0 (
            echo FAILED: %%~nxf
            set SIGN_FAIL=1
        ) else (
            echo OK: %%~nxf
        )
    )
)

echo.

REM Create and sign catalog files
echo Creating catalog files...

for %%d in (vhidkb vhidmouse vxinput vdisplay) do (
    if exist %DRIVER_DIR%\%%d.inf (
        echo Creating catalog for %%d...
        
        if %DRY_RUN%==1 (
            echo [DRY RUN] Would execute:
            echo   inf2cat /driver:%DRIVER_DIR% /os:10_X64 /verbose /nocat
            echo   inf2cat /driver:%DRIVER_DIR% /os:10_X64
        ) else (
            inf2cat /driver:%DRIVER_DIR% /os:10_X64,Server10_X64 /verbose >nul 2>&1
            if !errorlevel! neq 0 (
                echo WARNING: inf2cat had warnings for %%d
            )
        )
    )
)

REM Sign catalog files
for %%f in (%DRIVER_DIR%\*.cat) do (
    echo Signing catalog: %%~nxf
    
    if %DRY_RUN%==1 (
        echo [DRY RUN] Would sign catalog
    ) else (
        signtool sign /s MY /n %CERT_SUBJECT% /tr %TIMESTAMP_URL% /td %HASH_ALG% /fd %HASH_ALG% "%%f"
        if !errorlevel! neq 0 (
            echo FAILED: %%~nxf
            set SIGN_FAIL=1
        ) else (
            echo OK: %%~nxf
        )
    )
)

echo.

REM Verify signatures
echo Verifying signatures...
for %%f in (%DRIVER_DIR%\*.sys %DRIVER_DIR%\*.cat) do (
    if %DRY_RUN%==0 (
        signtool verify /pa "%%f" >nul 2>&1
        if !errorlevel! neq 0 (
            echo VERIFICATION FAILED: %%~nxf
            set SIGN_FAIL=1
        ) else (
            echo Verified: %%~nxf
        )
    )
)

echo.
echo ==========================================

if %SIGN_FAIL%==1 (
    echo SIGNING COMPLETED WITH ERRORS
echo Check output above for details.
    exit /b 1
) else (
    if %DRY_RUN%==1 (
        echo DRY RUN COMPLETE - No files were modified
    ) else (
        echo SIGNING COMPLETE - All files signed successfully
    )
)

echo ==========================================
echo.

endlocal
