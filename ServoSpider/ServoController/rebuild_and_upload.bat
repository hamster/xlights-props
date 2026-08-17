@echo off
echo.
echo ========================================
echo  Building and Uploading Firmware
echo ========================================
echo.
echo Step 1: Building firmware...
pio run

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Build failed!
    pause
    exit /b 1
)

echo.
echo Step 2: Uploading to device...
pio run --target upload

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Upload failed!
    pause
    exit /b 1
)

echo.
echo ========================================
echo  Success! Firmware uploaded.
echo ========================================
echo.
pause
