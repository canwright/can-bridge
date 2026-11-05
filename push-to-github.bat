@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo.
echo  ============================================================
echo   把本仓库推送到 GitHub
echo  ============================================================
echo.

:: ── 1. 检查 git ──
where git >nul 2>&1
if errorlevel 1 (
    echo  [X] 没找到 git
    echo.
    echo      请先装 Git for Windows： https://git-scm.com/download/win
    echo      装完关掉这个窗口，重新双击本文件。
    echo.
    pause
    exit /b 1
)

echo  [OK] git 找到了
git --version
echo.

:: ── 2. 身份 ──
git config user.name >nul 2>&1
if errorlevel 1 (
    echo  [!] 还没设 git 身份，请输入：
    set /p GNAME=      名字（比如你的 GitHub 用户名）:
    set /p GMAIL=     邮箱（GitHub 账号邮箱）:
    git config user.name "!GNAME!"
    git config user.email "!GMAIL!"
    echo  [OK] 已设置
    echo.
)

:: ── 3. 仓库地址 ──
set "REMOTE="
set /p REMOTE=  GitHub 仓库地址（形如 https://github.com/你的名字/can-bridge.git）:
if "!REMOTE!"=="" (
    echo  [X] 没输入地址，退出
    pause
    exit /b 1
)
echo.

:: ── 4. 初始化并提交 ──
if not exist ".git" (
    echo  初始化仓库...
    git init -b main
    if errorlevel 1 git init
    echo.
)

echo  加入文件...
git add -A
echo.
echo  当前将提交的内容：
git status --short
echo.

set /p OK= 确认提交并推送？(Y/N):
if /i not "!OK!"=="Y" (
    echo  已取消
    pause
    exit /b 0
)

git commit -m "CAN Universal Bridge: multi-vendor CAN DLL shim with neutral frame model"

echo.
echo  设置远端...
git remote remove origin >nul 2>&1
git remote add origin "!REMOTE!"

echo.
echo  推送（会要求登录 GitHub，用浏览器授权或个人访问令牌）...
git push -u origin main
if errorlevel 1 (
    echo.
    echo  [X] 推送失败。常见原因：
    echo      1) 仓库还没在 GitHub 上创建  ^-^> 先去 github.com 新建一个空仓库
    echo      2) 认证失败                    ^-^> 用浏览器登录，或改用 Personal Access Token
    echo      3) 分支名不是 main             ^-^> 试 git push -u origin master
    echo.
    pause
    exit /b 1
)

echo.
echo  ============================================================
echo   推送成功
echo  ============================================================
echo.
echo   下一步建议：
echo     · 在仓库设置里加 Topics：can, canbus, zlg, peak, vector, vxlapi, dll-shim
echo     · 加一个简短描述：多厂商 CAN 盒子的 DLL 门面，让 ZCANPRO/CANape 用别的牌子硬件
echo     · 确认 drivers 之类没被误提交（本仓库的 .gitignore 已经挡住）
echo.
pause
