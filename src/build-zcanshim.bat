@echo off
REM Keep this file ASCII-only: cmd.exe mis-decodes UTF-8 in .bat.
setlocal
cd /d "%~dp0"
call "%~dp0vcenv.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
REM fake zlgcan.dll facade
echo [INFO] building zlgcan.dll ...
cl /nologo /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fo:out\ ZcanShim.c /Fe:out\zlgcan.dll /LD /link /DEF:ZcanShim.def /Brepro
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] out\zlgcan.dll
endlocal
