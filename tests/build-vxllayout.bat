@echo off
REM Keep this file ASCII-only: cmd.exe mis-decodes UTF-8 in .bat.
setlocal
cd /d "%~dp0"
call "%~dp0..\src\vcenv.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
REM validate XL struct layout against the real vxlapi.dll
echo [INFO] building VxlLayoutTest.exe ...
cl /nologo /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I..\src /Fo:out\ VxlLayoutTest.c /Fe:out\VxlLayoutTest.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] out\VxlLayoutTest.exe
endlocal
