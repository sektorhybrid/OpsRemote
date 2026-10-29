@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
rem Starts OpsRemote on Windows: the fallback for the native launcher
rem (OpsRemote.exe). Sessions, settings, and the credential vault are shared
rem with the Linux version in the package's settings\ folder. Checks the
rem system first and explains any problem instead of failing silently.
rem
rem The program is stored compressed (compressed\windows-x86_64.tar.zst).
rem The first start of a version extracts it into the package's runtime\
rem folder; later starts reuse that copy, and copies of older versions are
rem removed once they are no longer running.
setlocal
set "HERE=%~dp0"
rem This file lives in legacy\; the package is the folder holding data\.
rem The path is made absolute, as the old-version clean-up compares paths.
if not exist "%HERE%data" for %%i in ("%~dp0..") do set "HERE=%%~fi\"
set "PLATFORM=windows-x86_64"
set "ARCHIVE=%HERE%compressed\%PLATFORM%.tar.zst"

if /i not "%PROCESSOR_ARCHITECTURE%"=="AMD64" if /i not "%PROCESSOR_ARCHITEW6432%"=="AMD64" (
    call :fail "OpsRemote needs 64-bit (x64) Windows."
    exit /b 1
)

rem "ver" prints e.g. "Microsoft Windows [Version 10.0.26100.8246]".
set "MAJOR=0"
set "BUILD=0"
for /f "tokens=4-6 delims=[]. " %%a in ('ver') do (
    set "MAJOR=%%a"
    set "BUILD=%%c"
)
if %MAJOR% LSS 10 goto :too_old
if %BUILD% LSS 17763 goto :too_old

if not exist "%HERE%data" goto :incomplete
if exist "%ARCHIVE%" goto :compressed
rem An uncompressed package (make-package.sh --uncompressed).
set "APP=%HERE%%PLATFORM%\OpsRemote.exe"
goto :check

:compressed
set "ID="
if exist "%HERE%compressed\%PLATFORM%.id" set /p ID=<"%HERE%compressed\%PLATFORM%.id"
if "%ID%"=="" goto :incomplete
set "RUNTIME=%HERE%runtime\%PLATFORM%-%ID%"
set "APP=%RUNTIME%\OpsRemote.exe"
if exist "%APP%" goto :cleanup
if not exist "%HERE%tools\zstd-%PLATFORM%.exe" goto :incomplete
if not exist "%SystemRoot%\System32\tar.exe" (
    call :fail "The Windows tar program is missing; OpsRemote needs Windows 10 version 1809 or newer."
    exit /b 1
)
if not exist "%HERE%runtime" (
    mkdir "%HERE%runtime" 2>nul || goto :read_only
)
set "PARTIAL=%RUNTIME%.partial%RANDOM%"
if exist "%PARTIAL%" rmdir /s /q "%PARTIAL%"
mkdir "%PARTIAL%" 2>nul || goto :read_only
echo OpsRemote: preparing this version (first start only)...
"%HERE%tools\zstd-%PLATFORM%.exe" -dcq "%ARCHIVE%" | "%SystemRoot%\System32\tar.exe" -xf - -C "%PARTIAL%"
if not exist "%PARTIAL%\OpsRemote.exe" (
    rmdir /s /q "%PARTIAL%" 2>nul
    call :fail "Could not unpack compressed\%PLATFORM%.tar.zst. Is the disk full?"
    exit /b 1
)
rem Another start may have finished first; either copy is complete.
move "%PARTIAL%" "%RUNTIME%" >nul 2>&1 || rmdir /s /q "%PARTIAL%" 2>nul

:cleanup
rem Copies of other versions; one that is still running stays until next time.
for /d %%d in ("%HERE%runtime\%PLATFORM%-*") do (
    if /i not "%%~fd"=="%RUNTIME%" rmdir /s /q "%%~fd" 2>nul
)

rem Earlier packages unpacked into a hidden .runtime folder.
for /d %%d in ("%HERE%.runtime\%PLATFORM%-*") do rmdir /s /q "%%~fd" 2>nul
rmdir "%HERE%.runtime" 2>nul

:check
for %%f in ("%APP%" "%APP%\..\Qt6Core.dll" "%APP%\..\plugins\platforms\qwindows.dll") do (
    if not exist "%%~f" goto :incomplete
)

set "OPSREMOTE_HOME=%HERE%."
start "" "%APP%" %*
exit /b 0

:incomplete
call :fail "This copy of OpsRemote is incomplete. Extract the whole archive again."
exit /b 1

:read_only
call :fail "Cannot write to %HERE%. OpsRemote unpacks itself into this folder, so it must be writable."
exit /b 1

:too_old
call :fail "OpsRemote needs Windows 10 version 1809 or newer, or Windows 11. This is Windows %MAJOR% build %BUILD%."
exit /b 1

:fail
echo OpsRemote cannot start: %~1
rem A double-clicked batch file closes at once, so also show a message box.
powershell -NoProfile -Command "Add-Type -AssemblyName PresentationFramework; [void][System.Windows.MessageBox]::Show('%~1', 'OpsRemote')" > nul 2>&1
exit /b 0
