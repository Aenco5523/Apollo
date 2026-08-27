@echo off
setlocal

set "CONFIG_DIR=%LOCALAPPDATA%\SudoMaker\Apollo"
set "CONFIG_FILE=%CONFIG_DIR%\sunshine.conf"
set "TEMP_FILE=%CONFIG_DIR%\sunshine.conf.viiper.tmp"

if not exist "%CONFIG_DIR%" mkdir "%CONFIG_DIR%"

if exist "%CONFIG_FILE%" (
    copy /y "%CONFIG_FILE%" "%TEMP_FILE%" >nul
    >"%CONFIG_FILE%" echo virtual_hid = true
    type "%TEMP_FILE%" >>"%CONFIG_FILE%"
    del /q "%TEMP_FILE%" >nul 2>&1
) else (
    >"%CONFIG_FILE%" echo virtual_hid = true
)

endlocal
