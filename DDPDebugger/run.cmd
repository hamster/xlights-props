@echo off
rem Double-click-friendly wrapper: runs run.ps1 with a policy override so it works even if
rem this machine's PowerShell execution policy would otherwise block local scripts.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1"
if errorlevel 1 pause
