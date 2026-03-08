@echo off
REM Auto-patch script for SSI audio clock configuration
REM Run this AFTER RASC regenerates code to restore EXTERNAL audio clock setting

echo === Patching hal_data.c for SSI EXTERNAL audio clock ===

set TARGET_FILE=ra_gen\hal_data.c
set BACKUP_FILE=ra_gen\hal_data.c.backup

REM Create backup
if exist "%TARGET_FILE%" (
    copy /Y "%TARGET_FILE%" "%BACKUP_FILE%" >nul
    echo Backup created: %BACKUP_FILE%
)

REM Apply patch using PowerShell
powershell -Command "(Get-Content '%TARGET_FILE%') -replace 'SSI_AUDIO_CLOCK_INTERNAL,\s*/\*.*MASTER.*\*/', 'SSI_AUDIO_CLOCK_EXTERNAL,  /* Use GPT external clock for INMP441 */' | Set-Content '%TARGET_FILE%'"

REM Verify patch
findstr /C:"SSI_AUDIO_CLOCK_EXTERNAL" "%TARGET_FILE%" >nul
if %ERRORLEVEL% EQU 0 (
    echo [SUCCESS] Patch applied successfully
    echo SSI is now configured to use EXTERNAL audio clock
) else (
    echo [ERROR] Patch failed - SSI may still be using INTERNAL clock
    echo Please check %TARGET_FILE% manually
    exit /b 1
)

echo.
echo Done. You can now rebuild and flash the project.
pause
