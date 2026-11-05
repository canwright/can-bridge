@echo off
setlocal
cd /d "%~dp0"
call "%~dp0..\src\vcenv.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
echo [INFO] building VxlListen.exe ...
cl /nologo /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I..\src /Fo:out\ VxlListen.c /Fe:out\VxlListen.exe /link /SUBSYSTEM:CONSOLE /Brepro
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] out\VxlListen.exe
endlocal