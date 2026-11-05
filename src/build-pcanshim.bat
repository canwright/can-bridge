@echo off
REM Keep this file ASCII-only: cmd.exe mis-decodes UTF-8 in .bat.
setlocal
cd /d "%~dp0"
call "%~dp0vcenv.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
REM fake PCANBasic.dll facade
echo [INFO] building PCANBasic.dll ...
cl /nologo /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fo:out\ PcanShim.c /Fe:out\PCANBasic.dll /LD /link /DEF:PcanShim.def
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] out\PCANBasic.dll
endlocal
