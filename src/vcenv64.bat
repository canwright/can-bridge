@echo off
REM vcenv64.bat - load a 64-bit MSVC environment
REM
REM Why this file exists:
REM   CANape 17 is 64-bit and loads vxlapi64.dll, so the Vector facade
REM   must be built x64. The rest of the project stays 32-bit because
REM   ZCANPRO is 32-bit.
REM
REM NOTE - do NOT wrap this file in setlocal/endlocal (vcvars must apply
REM        to the current shell, otherwise cl is not found afterwards).
REM
REM Keep this file ASCII-only: cmd.exe mis-decodes UTF-8 in .bat.
if defined CAN_BRIDGE_VCENV64_OK goto :eof

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if not defined VCVARS (
    if exist "G:\Microsoft\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=G:\Microsoft\VisualStudio\VC\Auxiliary\Build\vcvars64.bat"
)

if not defined VCVARS (
    echo [ERROR] vcvars64.bat not found.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] failed to load vcvars64
    exit /b 1
)

set "CAN_BRIDGE_VCENV64_OK=1"
exit /b 0