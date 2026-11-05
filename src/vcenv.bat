@echo off
REM ============================================================
REM  vcenv.bat - locate and load a 32-bit MSVC environment
REM
REM  Why this file exists:
REM    The build scripts used to hard-code one machine's vcvars32.bat
REM    path, so nobody else could compile the project.
REM    This looks it up via vswhere (shipped with VS 2017+), then falls
REM    back to a few common install paths.
REM
REM  Why 32-bit:
REM    ZCANPRO / CANape are 32-bit, and a 32-bit host cannot load a
REM    64-bit DLL. Always build x86.
REM
REM  NOTE - do NOT wrap this file in setlocal/endlocal:
REM    vcvars32 sets PATH/INCLUDE/LIB for the CURRENT shell.
REM    endlocal would throw all of that away and 'cl' would not be
REM    found afterwards. (That bug happened once - do not reintroduce it.)
REM
REM  Keep this file ASCII-only: cmd.exe mis-decodes UTF-8 in .bat.
REM ============================================================
if defined CAN_BRIDGE_VCENV_OK goto :eof

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat"
    )
)

if not defined VCVARS (
    for %%p in (
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Community"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Professional"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Enterprise"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise"
        "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
    ) do (
        if not defined VCVARS if exist "%%~p\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%%~p\VC\Auxiliary\Build\vcvars32.bat"
    )
)

if not defined VCVARS (
    echo [ERROR] vcvars32.bat not found.
    echo.
    echo   Install "Visual Studio Build Tools" with the
    echo   "Desktop development with C++" workload:
    echo       https://visualstudio.microsoft.com/downloads/
    echo.
    echo   Or run the build from an "x86 Native Tools Command Prompt".
    exit /b 1
)

echo [INFO] using %VCVARS%
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvars32.bat failed to initialize
    exit /b 1
)

set "CAN_BRIDGE_VCENV_OK=1"
goto :eof
