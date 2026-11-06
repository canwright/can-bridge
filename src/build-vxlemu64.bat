@echo off
setlocal
cd /d "%~dp0"
call "%~dp0vcenv64.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
cl /nologo /O2 /W3 /source-charset:utf-8 /execution-charset:gbk /D_CRT_SECURE_NO_WARNINGS /Fo:out\ VxlEmu64.c VxlEmuStubs64.c /Fe:out\vxlapi64.dll /LD /link /DEF:VxlEmu64.def /Brepro
if errorlevel 1 exit /b 1
echo [OK] out\vxlapi64.dll
endlocal