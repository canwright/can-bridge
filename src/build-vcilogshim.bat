@echo off
setlocal
cd /d "%~dp0"
call "%~dp0vcenv.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
cl /nologo /O2 /W3 /source-charset:utf-8 /execution-charset:gbk /D_CRT_SECURE_NO_WARNINGS /Fo:out\ VciLogShim.c /Fe:out\VciLogShim.dll /LD /link /DEF:VciLogShim.def /Brepro
if errorlevel 1 exit /b 1
echo [OK] out\VciLogShim.dll
endlocal