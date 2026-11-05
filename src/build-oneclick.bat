@echo off
REM Keep this file ASCII-only: cmd.exe mis-decodes UTF-8 in .bat.
setlocal
cd /d "%~dp0"
call "%~dp0vcenv.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
REM GUI one-click tool
echo [INFO] building OneClick.exe ...
cl /nologo /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fo:out\ OneClick.c /Fe:out\OneClick.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comdlg32.lib shell32.lib advapi32.lib setupapi.lib cfgmgr32.lib /MANIFEST:EMBED /MANIFESTINPUT:UI.manifest /MANIFESTUAC:NO
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] out\OneClick.exe
endlocal
