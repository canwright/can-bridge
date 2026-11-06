@echo off
REM Keep this file ASCII-only: cmd.exe mis-decodes UTF-8 in .bat.
setlocal
cd /d "%~dp0"
call "%~dp0vcenv64.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
REM x64 build of the vxlapi call-logging stub.
REM CANape 17 is 64-bit and loads vxlapi64.dll, so the probe must be x64.
REM NOTE: x64 has no stdcall name decoration, so VxlApiStub64.def (plain
REM       names) is used instead of VxlApiStub.def (which has _name@N).
echo [INFO] building VxlApiStub64.dll ...
cl /nologo /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fo:out\ VxlApiStub.c /Fe:out\VxlApiStub64.dll /LD /link /DEF:VxlApiStub64.def /Brepro
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] out\VxlApiStub64.dll
endlocal