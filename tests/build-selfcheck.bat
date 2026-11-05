@echo off
REM Keep this file ASCII-only: cmd.exe mis-decodes UTF-8 in .bat.
setlocal
cd /d "%~dp0"
call "%~dp0..\src\vcenv.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
echo [INFO] building SelfCheck.exe ...
cl /nologo /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fo:out\ SelfCheck.c /Fe:out\SelfCheck.exe /link /SUBSYSTEM:CONSOLE /Brepro
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] out\SelfCheck.exe
endlocal