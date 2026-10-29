@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
rem Starts the extracted package through legacy\OpsRemote.bat, offscreen (no
rem desktop needed, for example over SSH), and reports whether it keeps
rem running and uses the shared settings folder.
setlocal
set "PACKAGE=%~dp0OpsRemote"
if not exist "%PACKAGE%\legacy\OpsRemote.bat" (echo FAIL: %PACKAGE% not found & exit /b 1)
set QT_QPA_PLATFORM=offscreen
set QT_FORCE_STDERR_LOGGING=1
call "%PACKAGE%\legacy\OpsRemote.bat"
ping -n 9 127.0.0.1 > nul
powershell -NoProfile -Command "if (Get-Process OpsRemote -ErrorAction SilentlyContinue) { 'PASS: still running after 8 s'; Stop-Process -Name OpsRemote } else { 'FAIL: not running after 8 s' }"
if exist "%PACKAGE%\windows-x86_64\settings" (echo FAIL: created windows-x86_64\settings instead of using the shared folder) else (echo PASS: uses the shared settings folder)
echo shared settings:
dir /b "%PACKAGE%\settings"
