@echo off
setlocal
cd /d "%~dp0"
call "%~dp0vcenv64.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
cl /nologo /O2 /W3 /source-charset:utf-8 /execution-charset:gbk /D_CRT_SECURE_NO_WARNINGS /Fo:out\ VxlEmu.c VxlEmuStubs.c /Fe:out\vxlapi64.dll /LD /link /DEF:VxlApiStub64.def /Brepro
if errorlevel 1 exit /b 1
echo [OK] out\vxlapi64.dll
endlocal