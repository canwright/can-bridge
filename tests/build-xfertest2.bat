@echo off
setlocal
cd /d "%~dp0"
call "%~dp0..\src\vcenv.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out
cl /nologo /O2 /W3 /source-charset:utf-8 /execution-charset:gbk /D_CRT_SECURE_NO_WARNINGS /Fo:out\ XferTest2.c /Fe:out\XferTest2.exe /link /SUBSYSTEM:CONSOLE /Brepro
if errorlevel 1 exit /b 1
echo [OK] out\XferTest2.exe
endlocal