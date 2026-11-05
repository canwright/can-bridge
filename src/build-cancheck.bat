@echo off
REM Keep this file ASCII-only: cmd.exe mis-decodes UTF-8 in .bat.
setlocal
cd /d "%~dp0"
call "%~dp0vcenv.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
REM scan an exe and decide whether the bridge can drive it
echo [INFO] building CanCheck.exe ...
cl /nologo /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fo:out\ CanCheck.c /Fe:out\CanCheck.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] out\CanCheck.exe
endlocal
