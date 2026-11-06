@echo off
REM Keep this file ASCII-only: cmd.exe mis-decodes UTF-8 in .bat.
setlocal
cd /d "%~dp0"
call "%~dp0vcenv.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
REM 32-bit on purpose: it loads the 32-bit vxlapi.dll and the 32-bit
REM UniVciBridge.dll. The virtual-channel pairing happens inside the
REM Vector driver, so a 32-bit relay talks to a 64-bit CANape fine.
echo [INFO] building VxlRelay.exe ...
cl /nologo /O2 /W3 /source-charset:utf-8 /execution-charset:gbk /D_CRT_SECURE_NO_WARNINGS /Fo:out\ VxlRelay.c /Fe:out\VxlRelay.exe /link /SUBSYSTEM:CONSOLE /Brepro
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] out\VxlRelay.exe
endlocal
