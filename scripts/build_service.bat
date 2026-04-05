@echo off
REM build_service.bat - Compile KVMService.exe directly via cl.exe (bypasses
REM MSBuild platform-toolset detection, which fails on VS 2026 v18.4.x).
REM Usage:  build_service.bat [Release|Debug]
setlocal enabledelayedexpansion

set CONFIGURATION=%~1
if "%CONFIGURATION%"=="" set CONFIGURATION=Release

echo ==========================================
echo Building KVMService (%CONFIGURATION%)
echo ==========================================

REM ── Locate Visual Studio ────────────────────────────────────────────────────
set VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community
if not exist "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo ERROR: vcvarsall.bat not found under %VS_ROOT%
    exit /b 1
)

REM ── Initialize build environment ────────────────────────────────────────────
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 > nul 2>&1

REM ── Directories ─────────────────────────────────────────────────────────────
set REPO=%~dp0..
set SRC_CORE=%REPO%\src\usermode\core
set SRC_REMOTE=%REPO%\src\usermode\remote
set SRC_COMMON=%REPO%\src\common
set OUT_DIR=%REPO%\build\%CONFIGURATION%
set BIN_DIR=%OUT_DIR%\bin
set OBJ_DIR=%OUT_DIR%\obj\KVMService

if not exist "%BIN_DIR%"  mkdir "%BIN_DIR%"
if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"

REM ── Windows SDK / WDK include/lib paths ─────────────────────────────────────
set SDK_VER=10.0.26100.0
set SDK_INC=C:\Program Files (x86)\Windows Kits\10\Include\%SDK_VER%
set SDK_LIB=C:\Program Files (x86)\Windows Kits\10\Lib\%SDK_VER%

REM ── Compiler flags ──────────────────────────────────────────────────────────
set VC_TOOLS=%VS_ROOT%\VC\Tools\MSVC\14.50.35717
set DEFINES=/DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /DNDEBUG ^
            /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000000 ^
            /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS
if "%CONFIGURATION%"=="Debug" (
    set DEFINES=/DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /D_DEBUG ^
                /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000000 ^
                /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS
    set OPT=/Od /Zi /MDd
) else (
    set OPT=/O2 /GL /MT
)

set INCLUDES=/I"%SRC_CORE%" /I"%SRC_REMOTE%" /I"%SRC_COMMON%" ^
             /I"%SDK_INC%\um" /I"%SDK_INC%\ucrt" /I"%SDK_INC%\shared" ^
             /I"%VC_TOOLS%\include"

set CFLAGS=/nologo /W3 /WX- /EHsc /std:c++17 /fp:precise %DEFINES% %OPT% %INCLUDES%

REM ── Collect source files ─────────────────────────────────────────────────────
set SOURCES=
for %%F in ("%SRC_CORE%\*.cpp") do set SOURCES=!SOURCES! "%%F"
for /r "%SRC_REMOTE%" %%F in (*.cpp) do set SOURCES=!SOURCES! "%%F"
for /r "%SRC_COMMON%\logging" %%F in (*_user.cpp) do set SOURCES=!SOURCES! "%%F"

if "!SOURCES!"=="" (
    echo ERROR: No .cpp files found under src\usermode\
    exit /b 1
)

REM ── Linker libs ─────────────────────────────────────────────────────────────
set LIBPATH=/LIBPATH:"%SDK_LIB%\um\x64" /LIBPATH:"%SDK_LIB%\ucrt\x64"
set LIBS=ws2_32.lib advapi32.lib user32.lib kernel32.lib d3d11.lib dxgi.lib ^
         dxguid.lib ole32.lib shell32.lib gdi32.lib bcrypt.lib ncrypt.lib ^
         shlwapi.lib secur32.lib crypt32.lib

REM ── Link flags ──────────────────────────────────────────────────────────────
set LFLAGS=/NOLOGO /SUBSYSTEM:CONSOLE /MACHINE:X64 %LIBPATH% %LIBS%
if "%CONFIGURATION%"=="Release" set LFLAGS=%LFLAGS% /LTCG

REM ── Compile ─────────────────────────────────────────────────────────────────
echo.
echo Compiling sources...
cl.exe %CFLAGS% /Fo"%OBJ_DIR%\\" /Fe"%BIN_DIR%\KVMService.exe" ^
    !SOURCES! /link %LFLAGS%

if errorlevel 1 (
    echo.
    echo ERROR: Compilation failed.
    exit /b 1
)

echo.
echo ==========================================
echo  Built: %BIN_DIR%\KVMService.exe
echo ==========================================
endlocal
exit /b 0
