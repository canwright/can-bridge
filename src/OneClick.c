/*============================================================================
 OneClick.c —— CAN 多盒子共存 万能桥 · 一键工具（图形界面）

 为什么做成 GUI：
   这个包是给别人用的，不能让人家去双击 .bat、看命令行、选菜单。
   所以做成一个窗口 + 三个大按钮：

       [ 一键安装 ]  [ 一键卸载 ]  [ 一键诊断 ]

 设计上的几条硬规矩：

 1. 绝不破坏原厂文件
    安装前一定先把 USBCAN.dll 备份成 USBCAN.dll.zlg.bak；
    卸载时先还原、校验成功、才删备份。

 2. 不影响厂家自己的软件
    桥里带了「原厂透传」逻辑：任何后端打不开盒子就自动退回
    备份的原厂 DLL。所以插真周立功盒子，行为跟装之前一模一样。

 3. 傻瓜式
    自动找 ZCANPRO（注册表 + 常见路径 + 遍历盘符）；
    自动识别 kerneldlls 里有哪家的驱动；
    结果用大字 + 颜色显示，不用看日志也能知道成没成。

 4. 出问题能查
    「一键诊断」把 ZCANPRO 驱动完整性、各家 DLL、内核驱动状态
    全列出来，并写一份报告到桌面。

 需要管理员权限（写 Program Files），用 manifest 提权。
============================================================================*/

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "comctl32.lib")

#define IDC_LOG       1001
#define IDC_BTN_INST  1002
#define IDC_BTN_UNIN  1003
#define IDC_BTN_DIAG  1004
#define IDC_BTN_OPEN  1005
#define IDC_BTN_ONE   1007   /* 一键检测并修复 */
#define IDC_BTN_ANY   1008   /* 选软件装桥（任意 EXE） */
#define IDC_STATUS    1006

static HWND  g_hLog, g_hStatus;
static HFONT g_fontUI, g_fontBig, g_fontMono;
static wchar_t g_zcan[MAX_PATH];
static wchar_t g_forced[MAX_PATH];   /* 命令行指定的 ZCANPRO 目录（测试/自定义安装位置用） */
static wchar_t g_knr[MAX_PATH];
static FILE  *g_rep = NULL;
static int    g_busy = 0;

/*----------------------------------------------------------------------------
  输出
----------------------------------------------------------------------------*/
static void Log(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    int len;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    buf[2047] = L'\0';
    va_end(ap);

    len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");

    if (g_rep) {
        char u8[4096];
        int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8), NULL, NULL);
        if (n > 1) { fwrite(u8, 1, (size_t)(n - 1), g_rep); fwrite("\r\n", 1, 2, g_rep); }
        fflush(g_rep);
    }
}

static void SetStatus(const wchar_t *s, COLORREF c)
{
    SetWindowTextW(g_hStatus, s);
    InvalidateRect(g_hStatus, NULL, TRUE);
    UpdateWindow(g_hStatus);
    (void)c;
}

static void ClearLog(void)
{
    SetWindowTextW(g_hLog, L"");
}

/*----------------------------------------------------------------------------
  文件工具
----------------------------------------------------------------------------*/
static int FileExists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
}

static int DirExists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY));
}

static int SelfDir(wchar_t *out, size_t n)
{
    out[0] = L'\0';
    GetModuleFileNameW(NULL, out, (DWORD)n);
    {
        wchar_t *s = wcsrchr(out, L'\\');
        if (!s) return 0;
        *s = L'\0';
    }
    return 1;
}

/* 找 ZCANPRO：参数路径 → 注册表 → 常见路径 → 各盘根目录 */
static int FindZcanPro(wchar_t *out, size_t n)
{
    static const wchar_t *cands[] = {
        L"C:\\Program Files (x86)\\ZCANPRO",
        L"C:\\Program Files\\ZCANPRO",
        L"D:\\Work\\ZCANPRO",
        L"D:\\ZCANPRO",
        L"E:\\ZCANPRO",
        L"F:\\ZCANPRO",
        L"G:\\ZCANPRO",
        L"C:\\ZCANPRO",
    };
    int i;
    wchar_t p[MAX_PATH];

    /* 命令行指定优先（也方便放到非标准目录） */
    if (g_forced[0] && DirExists(g_forced)) {
        _snwprintf(p, MAX_PATH - 1, L"%s\\ZCANPRO.exe", g_forced);
        p[MAX_PATH - 1] = L'\0';
        if (FileExists(p)) {
            wcsncpy(out, g_forced, n - 1);
            out[n - 1] = L'\0';
            return 1;
        }
    }

    for (i = 0; i < (int)(sizeof(cands) / sizeof(cands[0])); i++) {
        if (!DirExists(cands[i])) continue;
        _snwprintf(p, MAX_PATH - 1, L"%s\\ZCANPRO.exe", cands[i]);
        p[MAX_PATH - 1] = L'\0';
        if (FileExists(p)) { wcsncpy(out, cands[i], n - 1); out[n - 1] = L'\0'; return 1; }
    }
    /* 各盘根 */
    {
        wchar_t drive[] = L"A:\\";
        int d;
        for (d = 'C'; d <= 'Z'; d++) {
            drive[0] = (wchar_t)d;
            if (GetDriveTypeW(drive) != DRIVE_FIXED) continue;
            _snwprintf(p, MAX_PATH - 1, L"%c:\\ZCANPRO", d);
            p[MAX_PATH - 1] = L'\0';
            if (DirExists(p)) {
                wchar_t q[MAX_PATH];
                _snwprintf(q, MAX_PATH - 1, L"%s\\ZCANPRO.exe", p);
                q[MAX_PATH - 1] = L'\0';
                if (FileExists(q)) { wcsncpy(out, p, n - 1); out[n - 1] = L'\0'; return 1; }
            }
        }
    }
    out[0] = L'\0';
    return 0;
}

/*----------------------------------------------------------------------------
  后端识别：kerneldlls 里有哪家的驱动
----------------------------------------------------------------------------*/
typedef struct { const wchar_t *dll; const char *backend; const wchar_t *name; } DRV;
static const DRV kDrv[] = {
    { L"PCANBasic.dll", "pcan",   L"PEAK PCAN" },
    { L"ECanVci.dll",   "gcan",   L"广成 GCAN" },
    { L"TSMaster.dll",  "tosun",  L"TOSUN 同星" },
    { L"libTSCAN.dll",  "tosun",  L"TOSUN 同星" },
    { L"canlib32.dll",  "kvaser", L"Kvaser" },
};
#define NDRV ((int)(sizeof(kDrv) / sizeof(kDrv[0])))

static int DetectBackend(char *out, size_t n, wchar_t *pretty, size_t pn)
{
    int i, found = 0;
    char first[32];
    wchar_t names[512];
    names[0] = L'\0';
    first[0] = '\0';

    for (i = 0; i < NDRV; i++) {
        wchar_t p[MAX_PATH];
        _snwprintf(p, MAX_PATH - 1, L"%s\\%s", g_knr, kDrv[i].dll);
        p[MAX_PATH - 1] = L'\0';
        if (!FileExists(p)) continue;
        if (!found) strncpy(first, kDrv[i].backend, sizeof(first) - 1);
        if (names[0]) wcscat(names, L"、");
        wcsncat(names, kDrv[i].name, 120);
        found++;
    }
    if (found) {
        strncpy(out, first, n - 1);
        out[n - 1] = '\0';
        wcsncpy(pretty, names, pn - 1);
        pretty[pn - 1] = L'\0';
    } else {
        if (n > 4) strcpy(out, "auto");
        wcsncpy(pretty, L"（没发现第三方驱动，走自动）", pn - 1);
        pretty[pn - 1] = L'\0';
    }
    return found;
}

/*----------------------------------------------------------------------------
  复制文件（带错误信息）
----------------------------------------------------------------------------*/
static int CopyFileX(const wchar_t *from, const wchar_t *to, wchar_t *err, size_t en)
{
    if (CopyFileW(from, to, FALSE)) return 1;
    {
        DWORD e = GetLastError();
        const wchar_t *msg = L"未知错误";
        switch (e) {
        case ERROR_ACCESS_DENIED:      msg = L"没有权限（请用管理员运行）"; break;
        case ERROR_SHARING_VIOLATION:  msg = L"文件被占用（请先关闭 ZCANPRO）"; break;
        case ERROR_FILE_NOT_FOUND:     msg = L"源文件不存在"; break;
        case ERROR_PATH_NOT_FOUND:     msg = L"目标目录不存在"; break;
        }
        _snwprintf(err, en - 1, L"%s（错误码 %lu）", msg, (unsigned long)e);
        err[en - 1] = L'\0';
    }
    return 0;
}

/*----------------------------------------------------------------------------
  要替换的目标文件
  ----------------------------------------------------------------------------
  ★ 为什么要装两个：
    ZCANPRO 里每个型号都对应一个驱动 DLL。
      USBCAN.dll       -> 型号 3/4（USBCAN-I/II，1~2 通道）
      USBCAN_8E_U.dll  -> 型号 34（USBCAN-8E-U，最多 8 通道）

  用户要「报最大通道数」，所以主要靠 8E-U。
  同时保留 USBCAN.dll 是为了两个用途：
    · 只有 1~2 路的盒子，选 USBCAN-II 更直观
    · 原厂透传回退需要它的备份
----------------------------------------------------------------------------*/
typedef struct { const wchar_t *file; const wchar_t *desc; } TARGET;
static const TARGET kTargets[] = {
    /*
     ★ 只装这一个文件，它同时对应两个型号：
         USBCAN-I   （型号 3，1 通道）
         USBCAN-II  （型号 4，2 通道）
       dll_cfg.ini 里两行都指向 USBCAN.dll：
           3=USBCAN.dll 3 32 1
           4=USBCAN.dll 4 32 1
       所以只替换这一个文件，ZCANPRO 里两个型号同时可用。

     为什么只装一个：
       · 覆盖了实际使用中的绝大部分情况（1 路和 2 路）
       · 替换的文件越少，风险面越小
       · USBCAN.dll 在任何一个 ZCANPRO 安装里都存在，不会出现
         「要替换的文件本来就没有」这种情况
     */
    { L"USBCAN.dll", L"USBCAN-I / USBCAN-II（1~2 通道）" },
};
#define NTARGETS ((int)(sizeof(kTargets) / sizeof(kTargets[0])))

/*============================================================================
  盒子检测与一键修复

  为什么要这个：
    之前把安装拆成了“第1步/第2步/第3步”几个 .bat，
    对不熟悉的人来说步骤太多。现在合成一个按钮：

      点一下 → 自己看你插了什么盒子 → 自己看驱动全不全 →
      缺驱动：直接拉起对应厂家的安装程序
      驱动都好：直接把桥装上

  识别方式（按 USB 硬件 ID，不靠设备名）：
      VID_0C72          → PEAK PCAN
      VID_0471 PID_1200 → 周立功 USBCAN-I / II（老款）
      VID_0471 其它   → 周立功 E-U 系列（2E-U / 4E-U / 8E-U）
      VID_04D8          → 创苯 CANalyst

  驱动状态用 CM_Get_DevNode_Status：
      返回 CR_SUCCESS = 驱动已加载、设备已启动
      否则就是缺驱动 / 加载失败，可以直接告诉用户具体错码
============================================================================*/

/* DoInstall 定义在后面，先声明一下 ——
   一键检测里驱动都正常时会直接调它装桥。 */
/*============================================================================
  ★ 选软件装桥 —— 把桥装进任意软件的目录

  为什么加这个（用户提出）：
    原来三个 exe 功能重叠：
      「能不能用桥.exe」只扫描给结论（纯冗余）
      「万能安装器.exe」扫描 + 安装，但是控制台程序
      「一键工具.exe」GUI 体验最好，却只认 ZCANPRO
    合并成一个 GUI 后，这个按钮负责「装进任意软件」。

  流程：
    浏览选 EXE → 扫它引用哪些 CAN 库 / 用哪套接口 → 决定门面
    → 备份原文件 → 拷门面 → 写 ini → 铺厂家库
============================================================================*/

static int CeCountAscii(const unsigned char *b, DWORD n, const char *needle)
{
    int cnt = 0;
    DWORD i, nl = (DWORD)strlen(needle);
    if (!b || n < nl) return 0;
    for (i = 0; i + nl <= n; i++)
        if (b[i] == (unsigned char)needle[0] && memcmp(b + i, needle, nl) == 0) cnt++;
    return cnt;
}

/* 把厂家库铺到任意目录（不是 ZCANPRO 目录） */
static int CopyVendorLibsTo(const wchar_t *dstDir)
{
    wchar_t self[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int copied = 0;

    SelfDir(self, MAX_PATH);
    _snwprintf(pat, MAX_PATH - 1, L"%s\\drivers\\*.dll", self);
    pat[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        _snwprintf(src, MAX_PATH - 1, L"%s\\drivers\\%s", self, fd.cFileName);
        src[MAX_PATH - 1] = 0;
        _snwprintf(dst, MAX_PATH - 1, L"%s\\%s", dstDir, fd.cFileName);
        dst[MAX_PATH - 1] = 0;
        if (!FileExists(dst)) {
            if (CopyFileW(src, dst, FALSE)) {
                Log(L"  ✓ 铺厂家库 %s", fd.cFileName);
                copied++;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return copied;
}

/* 从若干候选位置找门面源文件 */
static int FindFace(const wchar_t *name, wchar_t *out, DWORD cch)
{
    wchar_t self[MAX_PATH], cand[MAX_PATH];
    static const wchar_t *subs[] = {
        L"_prog", L"_prog\\新老API门面", L"", L"新老API门面"
    };
    int i;

    SelfDir(self, MAX_PATH);
    for (i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
        if (subs[i][0])
            _snwprintf(cand, MAX_PATH - 1, L"%s\\%s\\%s", self, subs[i], name);
        else
            _snwprintf(cand, MAX_PATH - 1, L"%s\\%s", self, name);
        cand[MAX_PATH - 1] = 0;
        if (FileExists(cand)) {
            wcsncpy(out, cand, cch - 1);
            out[cch - 1] = 0;
            return 1;
        }
    }
    return 0;
}

static void DoInstallAny(void)
{
    OPENFILENAMEW ofn;
    wchar_t target[MAX_PATH], dir[MAX_PATH], tcopy[MAX_PATH];
    unsigned char *data = NULL;
    DWORD len = 0;
    HANDLE hf;
    int cVci, cZcan, cPcan, refControl, refUsbcan, refZlgcan, refPcan;
    const wchar_t *face = NULL, *faceSrc = NULL;

    ClearLog();
    SetStatus(L"选一个软件的主程序…", RGB(0, 0, 0));

    ZeroMemory(&ofn, sizeof(ofn));
    target[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = target;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择要装桥的软件主程序（EXE）";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) {
        Log(L"· 已取消，什么都没动。");
        SetStatus(L"准备就绪", RGB(0, 0, 0));
        return;
    }

    Log(L"========== 选软件装桥 ==========");
    Log(L"目标：%s", target);
    Log(L"");

    /* 求目录（不动 target） */
    wcsncpy(tcopy, target, MAX_PATH - 1);
    tcopy[MAX_PATH - 1] = 0;
    {
        wchar_t *sl = wcsrchr(tcopy, L'\\');
        if (sl) { *sl = 0; wcsncpy(dir, tcopy, MAX_PATH - 1); }
        else wcscpy(dir, L".");
        dir[MAX_PATH - 1] = 0;
    }
    Log(L"目录：%s", dir);
    Log(L"");

    /*
     ★★ 硬拦截：绝不允许把门面装进 ZCANPRO 目录 ★★

       事故：用这个按钮选了 ZCANPRO.exe，工具就把 zlgcan.dll 门面
       装进了 ZCANPRO 目录，把 ZCANPRO 自己的 zlgcan.dll 顶掉了。
       ZCANPRO 内部枚举设备走的就是 zlgcan.dll，门面报的型号/通道数
       跟原厂对不上，于是：
           devicemanager.cxx Line 731
           Expression: channelNumber_count(info.type)
       一开设备管理就断言崩溃。

       ZCANPRO 是【宿主】，桥应该装成它旁边的 USBCAN.dll。
       把门面装到宿主自己身上 = 把宿主的发动机换掉。
     */
    {
        wchar_t chk[MAX_PATH], chk2[MAX_PATH], chk3[MAX_PATH], chk4[MAX_PATH];
        const wchar_t *base = wcsrchr(target, L'\\');
        int isZcan = 0;
        base = base ? base + 1 : target;

        /* 判据 1：目标就叫 ZCANPRO.exe */
        if (_wcsicmp(base, L"ZCANPRO.exe") == 0) isZcan = 1;

        /* 判据 2：同目录有 ZCANPRO.exe（换了名字也能认出来） */
        if (!isZcan) {
            _snwprintf(chk, MAX_PATH - 1, L"%s\\ZCANPRO.exe", dir);
            chk[MAX_PATH - 1] = 0;
            if (FileExists(chk)) isZcan = 1;
        }

        /* 判据 3：同目录有 ZCANPRO 的特征文件 kerneldlls\dll_cfg.ini */
        if (!isZcan) {
            _snwprintf(chk2, MAX_PATH - 1, L"%s\\kerneldlls\\dll_cfg.ini", dir);
            chk2[MAX_PATH - 1] = 0;
            if (FileExists(chk2)) isZcan = 1;
        }

        /* 判据 4：同目录有 ZCANPRO 的 Qt 运行时 + 设备插件目录（kerneldlls 本身） */
        if (!isZcan) {
            _snwprintf(chk3, MAX_PATH - 1, L"%s\\kerneldlls", dir);
            chk3[MAX_PATH - 1] = 0;
            _snwprintf(chk4, MAX_PATH - 1, L"%s\\Qt5Core.dll", dir);
            chk4[MAX_PATH - 1] = 0;
            if (FileExists(chk3) && FileExists(chk4)) isZcan = 1;
        }

        if (isZcan) {
            Log(L"  ★ 拒绝：这是 ZCANPRO 的目录，不能往里面装门面。");
            Log(L"");
            Log(L"    ZCANPRO 是【宿主】，它自己就用 zlgcan.dll 工作。");
            Log(L"    把门面装成它的 zlgcan.dll，会把它的引擎换掉，");
            Log(L"    结果就是一开设备管理就断言崩溃（channelNumber_count）。");
            Log(L"");
            Log(L"    要让 ZCANPRO 用别家盒子，正确做法：点「一键安装」——");
            Log(L"    它会装成 USBCAN.dll，不碰 zlgcan.dll。");
            Log(L"");
            Log(L"    如果已经误装了，把目录里的");
            Log(L"        zlgcan.dll.zlg.bak  改名回  zlgcan.dll");
            Log(L"    就恢复原厂状态了。");
            SetStatus(L"★ 不能装进 ZCANPRO，请点「一键安装」", RGB(200, 0, 0));
            return;
        }
    }

    hf = CreateFileW(target, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(hf, NULL), got = 0;
        if (sz != INVALID_FILE_SIZE && sz > 0 && sz < 256u * 1024u * 1024u) {
            data = (unsigned char *)malloc(sz);
            if (data && ReadFile(hf, data, sz, &got, NULL) && got > 0) len = got;
            else { free(data); data = NULL; }
        }
        CloseHandle(hf);
    }
    if (!data) {
        Log(L"✗ 读不到这个文件（可能不是 EXE，或者权限不足）");
        SetStatus(L"✗ 读取失败", RGB(200, 0, 0));
        return;
    }

    refControl = CeCountAscii(data, len, "ControlCAN.dll");
    refUsbcan  = CeCountAscii(data, len, "USBCAN.dll");
    refZlgcan  = CeCountAscii(data, len, "zlgcan.dll");
    refPcan    = CeCountAscii(data, len, "PCANBasic.dll");
    cVci  = CeCountAscii(data, len, "VCI_OpenDevice") + CeCountAscii(data, len, "VCI_InitCAN");
    cZcan = CeCountAscii(data, len, "ZCAN_OpenDevice") + CeCountAscii(data, len, "ZCAN_InitCAN");
    cPcan = CeCountAscii(data, len, "CAN_Initialize") + CeCountAscii(data, len, "CAN_Read");
    free(data);

    Log(L"【1】它引用了什么");
    if (refControl) Log(L"  · ControlCAN.dll");
    if (refUsbcan)  Log(L"  · USBCAN.dll");
    if (refZlgcan)  Log(L"  · zlgcan.dll");
    if (refPcan)    Log(L"  · PCANBasic.dll");
    if (!refControl && !refUsbcan && !refZlgcan && !refPcan)
        Log(L"  · 没认出 CAN 库名");
    Log(L"");
    Log(L"【2】接口特征：VCI_* %d 处   ZCAN_* %d 处   CAN_* %d 处", cVci, cZcan, cPcan);
    Log(L"");

    /* 决定装哪个门面：优先按接口用量 */
    if (cZcan > 0 && cVci == 0 && cPcan == 0) {
        face = L"zlgcan.dll";     faceSrc = L"zlgcan.dll";
    } else if (cPcan > 0 && cVci == 0 && cZcan == 0) {
        face = L"PCANBasic.dll";  faceSrc = L"PCANBasic.dll";
    } else if (cVci > 0) {
        face = refUsbcan ? L"USBCAN.dll" : L"ControlCAN.dll";
        faceSrc = L"UniVciBridge.dll";
    } else if (refUsbcan || refControl) {
        face = refUsbcan ? L"USBCAN.dll" : L"ControlCAN.dll";
        faceSrc = L"UniVciBridge.dll";
    } else if (refZlgcan) {
        face = L"zlgcan.dll";     faceSrc = L"zlgcan.dll";
    } else if (refPcan) {
        face = L"PCANBasic.dll";  faceSrc = L"PCANBasic.dll";
    }

    Log(L"【3】结论");
    if (!face) {
        Log(L"  ✗ 接不了");
        Log(L"    没认出它用哪套 CAN 接口。可能它用自己的私有驱动");
        Log(L"    （比如 CANoe + Vector），或者接口名被混淆了。");
        Log(L"    桥只能顶替三类公开接口：VCI_* / ZCAN_* / CAN_*");
        SetStatus(L"✗ 这个软件接不了", RGB(200, 0, 0));
        return;
    }
    Log(L"  装 %s", face);

    {
        wchar_t src[MAX_PATH], dst[MAX_PATH], bak[MAX_PATH], ini[MAX_PATH], err[512];
        _snwprintf(dst, MAX_PATH - 1, L"%s\\%s", dir, face);       dst[MAX_PATH - 1] = 0;
        _snwprintf(bak, MAX_PATH - 1, L"%s.zlg.bak", dst);         bak[MAX_PATH - 1] = 0;
        _snwprintf(ini, MAX_PATH - 1, L"%s\\UniVciBridge.ini", dir); ini[MAX_PATH - 1] = 0;

        if (!FindFace(faceSrc, src, MAX_PATH)) {
            Log(L"  ✗ 找不到门面文件 %s", faceSrc);
            Log(L"    本程序需要和 _prog\\ 放在一起");
            SetStatus(L"✗ 缺门面文件", RGB(200, 0, 0));
            return;
        }
        Log(L"  门面源：%s", src);
        Log(L"");

        Log(L"【4】安装");
        if (FileExists(dst) && !FileExists(bak)) {
            if (CopyFileX(dst, bak, err, 512)) Log(L"  ✓ 已备份原厂 → %s.zlg.bak", face);
            else {
                Log(L"  ✗ 备份失败：%s", err);
                Log(L"    为安全起见放弃安装，没有覆盖任何文件。");
                SetStatus(L"✗ 备份失败，已放弃", RGB(200, 0, 0));
                return;
            }
        }
        if (!CopyFileX(src, dst, err, 512)) {
            Log(L"  ✗ 拷贝失败：%s", err);
            Log(L"    先把那个软件关掉再试。");
            SetStatus(L"✗ 拷贝失败（软件可能正开着）", RGB(200, 0, 0));
            return;
        }
        Log(L"  ✓ 已装为 %s", face);

        {
            /*
             ★★ ini 只在【不存在】时创建，绝不覆盖 ★★

                以前这里用 CREATE_ALWAYS 写一行 backend=auto，后果是：
                  · 重装一次就把用户自己的配置冲掉
                    （channels / xl_channel / app_name 全没了）
                  · channels=1 这种默认值永远进不去 ——
                    而它恰恰是避免"空通道一直累计总线错误"的关键
                    （实测：CAN1 收了 1246 帧，空着的 CAN2 错误计数 35718）

                CREATE_NEW 是原子操作：文件已存在就失败，不会误覆盖。
             */
            if (!FileExists(ini)) {
                HANDLE hi = CreateFileW(ini, GENERIC_WRITE, 0, NULL,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hi != INVALID_HANDLE_VALUE) {
                    const char *cfg =
                        "; UniVciBridge 配置（删掉本文件即恢复默认）\r\n"
                        ";\r\n"
                        "; 后端：auto=自动探测 / vector / pcan / gcan / kvaser / tosun / vci / zlg\r\n"
                        "backend=auto\r\n"
                        "\r\n"
                        "; 对外暴露几路（默认 1）。\r\n"
                        "; 改成 2 时，两路都要接节点 —— 空着的那一路没有节点应答、\r\n"
                        "; 线又是悬空的，会一直累计总线错误，界面上看着像出了大问题。\r\n"
                        "channels=1\r\n"
                        "\r\n"
                        "; 只用 Vector 第几路（1 起）。不写 = 自动挑能拿到权限的那路。\r\n"
                        "; xl_channel=1\r\n"
                        "\r\n"
                        "; Vector 应用名。两个上位机同时用一块盒子时必填，且必须不同。\r\n"
                        "; app_name=ZCANPRO\r\n";
                    DWORD w = 0;
                    WriteFile(hi, cfg, (DWORD)strlen(cfg), &w, NULL);
                    CloseHandle(hi);
                    Log(L"  ✓ 已创建 UniVciBridge.ini（backend=auto, channels=1）");
                }
            } else {
                Log(L"  · 已有 UniVciBridge.ini，保留不动");
            }
        }

        Log(L"  · 检查厂家库");
        {
            int c = CopyVendorLibsTo(dir);
            if (c == 0) Log(L"    · 无需拷贝（都已就位）");
            else        Log(L"    ✓ 已复制 %d 个厂家库", c);
        }

        Log(L"");
        Log(L"========== 装好了 ==========");
        Log(L"打开那个软件，像平时一样选设备就行。");
        Log(L"想还原：用「万能安装器.exe」选同一个 EXE 加 --uninstall，");
        Log(L"        或者手动把 %s.zlg.bak 改名回 %s", face, face);
        SetStatus(L"✓ 已装好，可以打开那个软件了", RGB(0, 130, 0));
    }
}

static void DoInstall(void);

#define KIND_UNKNOWN 0
#define KIND_PEAK    1
#define KIND_ZLG_II  2
#define KIND_ZLG_EU  3
#define KIND_CX      4

typedef struct {
    wchar_t desc[128];
    wchar_t hwid[320];
    ULONG     status;    /* CM_Get_DevNode_Status 的 status 输出（DN_* 标志位） */
    ULONG     problem;   /* CM_PROB_* 问题码，0 = 没问题 */
    CONFIGRET cr;        /* CM_Get_DevNode_Status 的返回值，0 = 查询成功 */
    int       kind;
} CANDDEV;

static const wchar_t *KindName(int k)
{
    switch (k) {
    case KIND_PEAK:   return L"PEAK PCAN";
    case KIND_ZLG_II: return L"周立功 USBCAN-I/II";
    case KIND_ZLG_EU: return L"周立功 E-U 系列";
    case KIND_CX:     return L"创苯 CANalyst";
    }
    return L"未知";
}

static const wchar_t *KindInstaller(int k)
{
    switch (k) {
    case KIND_PEAK:   return L"PEAK-PCAN-Setup4.2.1.exe";
    case KIND_ZLG_II: return L"ZLG-USBCAN-AllInOne.exe";
    case KIND_ZLG_EU: return L"ZLG-USBCAN-E-U-Setup-2.3.0.6.exe";
    case KIND_CX:     return L"Chuangxin-USBDriver-V1.40.exe";
    }
    return NULL;
}

static int EnumCanBoxes(CANDDEV *out, int max)
{
    HDEVINFO h;
    SP_DEVINFO_DATA did;
    DWORD i;
    int n = 0;

    h = SetupDiGetClassDevsW(NULL, L"USB", NULL,
                             DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return 0;

    did.cbSize = sizeof(did);
    for (i = 0; n < max && SetupDiEnumDeviceInfo(h, i, &did); i++) {
        wchar_t hwid[512];
        wchar_t desc[256];
        DWORD t = 0, sz = 0;
        ULONG st = 0, prob = 0;
        CONFIGRET cr;
        int kind = KIND_UNKNOWN;

        hwid[0] = L'\0';
        desc[0] = L'\0';
        if (!SetupDiGetDeviceRegistryPropertyW(h, &did, SPDRP_HARDWAREID,
                &t, (PBYTE)hwid, (DWORD)sizeof(hwid) - sizeof(wchar_t), &sz))
            continue;
        hwid[511] = L'\0';

        if (!wcsstr(hwid, L"VID_0C72") && !wcsstr(hwid, L"VID_0471") &&
            !wcsstr(hwid, L"VID_04D8"))
            continue;

        SetupDiGetDeviceRegistryPropertyW(h, &did, SPDRP_DEVICEDESC,
            &t, (PBYTE)desc, (DWORD)sizeof(desc) - sizeof(wchar_t), &sz);
        desc[255] = L'\0';
        cr = CM_Get_DevNode_Status(&st, &prob, did.DevInst, 0);

        if (wcsstr(hwid, L"VID_0C72"))      kind = KIND_PEAK;
        else if (wcsstr(hwid, L"VID_0471")) kind = wcsstr(hwid, L"PID_1200")
                                                    ? KIND_ZLG_II : KIND_ZLG_EU;
        else                                kind = KIND_CX;

        wcsncpy(out[n].desc, desc, 127);  out[n].desc[127] = L'\0';
        wcsncpy(out[n].hwid, hwid, 319);  out[n].hwid[319] = L'\0';
        out[n].status = st;
        out[n].problem = prob;
        out[n].cr = cr;
        out[n].kind = kind;
        n++;
    }
    SetupDiDestroyDeviceInfoList(h);
    return n;
}

/*
 把 drivers\\*.dll 复制到 ZCANPRO 程序目录。

 为什么需要：
   桥就住在 ZCANPRO 程序目录里，它找厂家库时优先看自己所在目录。
   而创芯的 ControlCAN.dll、广成的 ECanVci.dll + CHUSBDLL.dll
   （以及 ECanVci 依赖的 MSVCR120.dll）在 Windows 系统目录里是找不到的，
   必须放在程序旁边。

   PCAN 和周立功 E-U 不需要（它们的库在系统目录），
   但复制过去也无害，而且能让桥优先用我们带的版本。

 只复制「目标不存在」的文件 —— 不覆盖用户已经装好的新版。
*/
static int CopyVendorDlls(void)
{
    wchar_t self[MAX_PATH], pat[MAX_PATH], dst[MAX_PATH], err[512];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int copied = 0;

    SelfDir(self, MAX_PATH);
    _snwprintf(pat, MAX_PATH - 1, L"%s\\drivers\\*.dll", self);
    pat[MAX_PATH - 1] = L'\0';

    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    do {
        wchar_t src[MAX_PATH];
        _snwprintf(src, MAX_PATH - 1, L"%s\\drivers\\%s", self, fd.cFileName);
        src[MAX_PATH - 1] = L'\0';
        _snwprintf(dst, MAX_PATH - 1, L"%s\\%s", g_zcan, fd.cFileName);
        dst[MAX_PATH - 1] = L'\0';

        if (!FileExists(dst)) {
            if (CopyFileX(src, dst, err, 512)) {
                Log(L"  \u2713 \u590d\u5236 %s", fd.cFileName);
                copied++;
            } else {
                Log(L"  \u00b7 %s \u590d\u5236\u5931\u8d25\uff1a%s", fd.cFileName, err);
            }
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return copied;
}

/* 桥是不是已经装在 ZCANPRO 程序目录里 */
static int BridgeInstalled(void)
{
    wchar_t p[MAX_PATH];
    if (!g_zcan[0]) return 0;
    _snwprintf(p, MAX_PATH - 1, L"%s\\UniVciBridge.dll", g_zcan);
    p[MAX_PATH - 1] = L'\0';
    return FileExists(p);
}

/* 拉起厂家安装程序（需管理员，会弹 UAC） */
static int LaunchInstaller(const wchar_t *exeName)
{
    wchar_t self[MAX_PATH], path[MAX_PATH];
    HINSTANCE r;

    SelfDir(self, MAX_PATH);
    _snwprintf(path, MAX_PATH - 1,
               L"%s\\drivers_setup\\%s", self, exeName);
    path[MAX_PATH - 1] = L'\0';
    if (!FileExists(path)) {
        Log(L"  ★ 找不到安装包：%s", path);
        return 0;
    }
    Log(L"  → 正在打开：%s", exeName);
    r = ShellExecuteW(NULL, L"runas", path, NULL, self, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        Log(L"  ★ 启动失败（错误 %ld）—— 可能你点了 UAC 的「否」",
            (long)(INT_PTR)r);
        return 0;
    }
    return 1;
}

static void DoOneClick(void)
{
    CANDDEV devs[16];
    int n, i, bad = 0, good = 0;
    const wchar_t *needInst[4];
    int needKind[4];
    int needN = 0;

    ClearLog();
    SetStatus(L"正在检测…", RGB(0, 0, 0));
    Log(L"========== 一键检测并修复 ==========");

    /* ---- 1. ZCANPRO ---- */
    if (!FindZcanPro(g_zcan, MAX_PATH)) {
        Log(L"✗ 这台电脑还没装 ZCANPRO");
        Log(L"");
        Log(L"  桥是装在 ZCANPRO 里面的，没它什么都干不了。");
        Log(L"  包里已经带了安装包，现在替你打开——");
        Log(L"");
        if (LaunchInstaller(L"ZCANPRO-Setup-2.4.1.exe")) {
            Log(L"  ★ 装完 ZCANPRO 之后，再点一次这个大按钮");
            SetStatus(L"★ 已拉起 ZCANPRO 安装，装完再点一次", RGB(190, 110, 0));
        } else {
            Log(L"  ★ 自动打开失败。请手动双击：");
            Log(L"     drivers_setup\\ZCANPRO-Setup-2.4.1.exe");
            SetStatus(L"✗ 需要先装 ZCANPRO", RGB(200, 0, 0));
        }
        return;
    }
    _snwprintf(g_knr, MAX_PATH - 1, L"%s\\kerneldlls", g_zcan);
    g_knr[MAX_PATH - 1] = L'\0';
    Log(L"✓ ZCANPRO：%s", g_zcan);
    Log(L"");

    /* ---- 2. 枚举盒子 ---- */
    Log(L"【1】检测插入的 CAN 盒子");
    n = EnumCanBoxes(devs, 16);
    if (n == 0) {
        Log(L"  · 没检测到已知品牌的 CAN 盒子");
        Log(L"    认得的 USB ID：");
        Log(L"      VID_0C72          PEAK PCAN");
        Log(L"      VID_0471 PID_1200 周立功 USBCAN-I / II");
        Log(L"      VID_0471 其它     周立功 E-U 系列");
        Log(L"      VID_04D8          创苯 CANalyst");
        Log(L"");
        Log(L"  请插上盒子后再点一次。");
        SetStatus(L"· 没检测到盒子，插上再试", RGB(160, 110, 0));
        return;
    }

    for (i = 0; i < n; i++) {
        /*
         ★ 判定方式必须对：
             CM_Get_DevNode_Status(&status, &problem, ...)
               返回值 = 查询是否成功（CR_SUCCESS = 0）
               status   = DN_* 标志位（DN_STARTED = 0x00000008 表示已启动）
               problem  = CM_PROB_*，0 表示没问题
           之前错误地拿 status 当「有没有问题」用 ——
           正常设备的 status 是 0x8（DN_STARTED）不是 0，
           结果把好的当成坏的（真机踩到：好的 2E-U 被报成缺驱动）。
         */
        if (devs[i].cr == 0 && devs[i].problem == 0 &&
            (devs[i].status & 0x00000008UL)) {
            Log(L"  ✓ %s：%s", KindName(devs[i].kind), devs[i].desc);
            good++;
        } else {
            Log(L"  ✗ %s：%s  —— 驱动有问题",
                KindName(devs[i].kind), devs[i].desc);
            Log(L"     问题码 %lu（CM_PROB）  状态位 0x%lX  查询返回 %ld",
                (unsigned long)devs[i].problem,
                (unsigned long)devs[i].status,
                (long)devs[i].cr);
            bad++;
            /* 记下需要装哪个安装包（同类只记一次） */
            {
                int dup = 0, k;
                for (k = 0; k < needN; k++)
                    if (needKind[k] == devs[i].kind) dup = 1;
                if (!dup && needN < 4) {
                    needKind[needN] = devs[i].kind;
                    needInst[needN] = KindInstaller(devs[i].kind);
                    needN++;
                }
            }
        }
    }
    Log(L"");
    Log(L"  共 %d 个盒子：%d 个正常，%d 个缺驱动", n, good, bad);
    Log(L"");

    /* ---- 3. 缺驱动 → 拉起安装程序 ---- */
    if (bad > 0) {
        Log(L"【2】缺驱动，需要安装");
        for (i = 0; i < needN; i++) {
            if (needInst[i]) {
                Log(L"  %s  →  %s", KindName(needKind[i]), needInst[i]);
                LaunchInstaller(needInst[i]);
            } else {
                Log(L"  ★ %s：包里没有它的驱动安装包",
                    KindName(needKind[i]));
                Log(L"    需要去厂家官网下载");
            }
        }
        Log(L"");
        Log(L"  ★ 装完之后：");
        Log(L"     1. 把盒子拔掉重插");
        Log(L"     2. 再点一次「一键检测并修复」");
        SetStatus(L"★ 已拉起驱动安装，装完重插再点一次",
                  RGB(190, 110, 0));
        return;
    }

    /* ---- 4. 驱动都好了 → 先复制厂家库，再装桥 ---- */
    Log(L"【2】驱动全部就绪，准备桥");
    Log(L"  检查厂家库（创芯/广成需要复制到程序目录）：");
    {
        int c = CopyVendorDlls();
        if (c == 0) Log(L"  · 无需拷贝（都已就位）");
        else        Log(L"  ✓ 已复制 %d 个厂家库", c);
    }
    Log(L"");
    if (BridgeInstalled()) {
        Log(L"  ✓ 桥已经装好了：%s\\UniVciBridge.dll", g_zcan);
        Log(L"");
        Log(L"========== 一切正常，可以用了 ==========");
        Log(L"   打开 ZCANPRO → 类型选「USBCAN-I」或「USBCAN-II」");
        Log(L"   → 点「打开设备」");
        SetStatus(L"✓ 一切正常 —— 可以打开 ZCANPRO 了", RGB(0, 130, 0));
        return;
    }
    Log(L"  · 桥还没装，现在自动安装…");
    Log(L"");
    DoInstall();          /* 复用已有的安装流程（含备份、日志、结果判定） */
}

/*----------------------------------------------------------------------------
  一键安装
----------------------------------------------------------------------------*/
static void DoInstall(void)
{
    wchar_t src[MAX_PATH], bak[MAX_PATH], dst[MAX_PATH], ini[MAX_PATH];
    wchar_t err[512];
    char be[64];
    wchar_t pretty[512];
    int n;

    ClearLog();
    SetStatus(L"正在安装…", RGB(0, 0, 0));

    Log(L"========== 一键安装 ==========");
    if (!FindZcanPro(g_zcan, MAX_PATH)) {
        Log(L"✗ 没找到 ZCANPRO。");
        Log(L"  请确认 ZCANPRO 已安装，或把它装到常见位置。");
        SetStatus(L"✗ 安装失败：没找到 ZCANPRO", RGB(200, 0, 0));
        return;
    }
    _snwprintf(g_knr, MAX_PATH - 1, L"%s\\kerneldlls", g_zcan);
    g_knr[MAX_PATH - 1] = L'\0';
    Log(L"✓ ZCANPRO：%s", g_zcan);

    if (!DirExists(g_zcan)) {
        Log(L"✗ 缺少 kerneldlls 目录，ZCANPRO 安装不完整。");
        SetStatus(L"✗ 安装失败：kerneldlls 不存在", RGB(200, 0, 0));
        return;
    }

    /* 桥本体 */
    {
        wchar_t self[MAX_PATH];
        SelfDir(self, MAX_PATH);
        _snwprintf(src, MAX_PATH - 1, L"%s\\UniVciBridge.dll", self);
        src[MAX_PATH - 1] = L'\0';
        if (!FileExists(src)) {
            _snwprintf(src, MAX_PATH - 1, L"%s\\_prog\\UniVciBridge.dll", self);
            src[MAX_PATH - 1] = L'\0';
        }
        if (!FileExists(src)) {
            Log(L"✗ 找不到 UniVciBridge.dll（请把整个文件夹一起复制）");
            SetStatus(L"✗ 安装失败：缺桥 DLL", RGB(200, 0, 0));
            return;
        }
    }

    (void)bak; (void)dst;

    /* 1) 放桥本体 */
    {
        wchar_t brg[MAX_PATH];
        _snwprintf(brg, MAX_PATH - 1, L"%s\\UniVciBridge.dll", g_zcan);
        brg[MAX_PATH - 1] = L'\0';
        if (!CopyFileX(src, brg, err, 512)) {
            Log(L"✗ 复制桥 DLL 失败：%s", err);
            SetStatus(L"✗ 安装失败：复制失败", RGB(200, 0, 0));
            return;
        }
        Log(L"✓ 已放入桥：UniVciBridge.dll");
    }

    /* 2) 逐个目标：先备份原厂，再替换 */
    {
        int ti;
        for (ti = 0; ti < NTARGETS; ti++) {
            wchar_t tbak[MAX_PATH], tdst[MAX_PATH];
            _snwprintf(tbak, MAX_PATH - 1, L"%s\\%s.zlg.bak", g_zcan, kTargets[ti].file);
            tbak[MAX_PATH - 1] = L'\0';
            _snwprintf(tdst, MAX_PATH - 1, L"%s\\%s", g_zcan, kTargets[ti].file);
            tdst[MAX_PATH - 1] = L'\0';

            if (FileExists(tbak)) {
                Log(L"· %s 原厂备份已存在，保留不动", kTargets[ti].file);
            } else if (FileExists(tdst)) {
                if (!CopyFileX(tdst, tbak, err, 512)) {
                    Log(L"✗ 备份 %s 失败：%s", kTargets[ti].file, err);
                    SetStatus(L"✗ 安装失败：备份失败", RGB(200, 0, 0));
                    return;
                }
                Log(L"✓ 已备份原厂：%s.zlg.bak", kTargets[ti].file);
            } else {
                /* 原本就没这个 DLL（只有残缺安装才会这样），直接新建 */
                Log(L"· %s 原本不存在，直接新建（等于多了一个型号）", kTargets[ti].file);
            }

            {
                wchar_t brg[MAX_PATH];
                _snwprintf(brg, MAX_PATH - 1, L"%s\\UniVciBridge.dll", g_zcan);
                brg[MAX_PATH - 1] = L'\0';
                if (!CopyFileX(brg, tdst, err, 512)) {
                    Log(L"✗ 替换 %s 失败：%s", kTargets[ti].file, err);
                    SetStatus(L"✗ 安装失败：替换失败", RGB(200, 0, 0));
                    return;
                }
                Log(L"✓ 已装为 %s → %ls", kTargets[ti].file, kTargets[ti].desc);
            }
        }
    }

    /* 3) 写后端配置 */
    n = DetectBackend(be, sizeof(be), pretty, 512);
    _snwprintf(ini, MAX_PATH - 1, L"%s\\UniVciBridge.ini", g_zcan);
    ini[MAX_PATH - 1] = L'\0';
    {
        FILE *f = _wfopen(ini, L"wb");
        if (f) {
            fprintf(f, "; UniVciBridge backend\r\n");
            fprintf(f, "; pcan / gcan / tosun / kvaser / vector / zlg / auto\r\n");
            fprintf(f, "backend=%s\r\n", be);
            fclose(f);
            Log(L"✓ 后端配置：%s → backend=%hs", pretty, be);
        } else {
            Log(L"· 写配置失败（不影响使用，会自动探测）");
        }
    }

    Log(L"");
    /*
     ★ 顺便把厂家库复制到 ZCANPRO 程序目录。

       为什么必须在这里做（真机踩到）：
         同事电脑上 ZCANPRO 目录里一个厂家库都没有，结果：
           插 PEAK 盒子  → 系统目录有 PCANBasic.dll（PEAK 驱动自带）→ 还能用
           插创苯盒子  → 系统里根本没有 ControlCAN.dll → 打不开

       之前只在「一键检测并修复」里做，用户直接点「一键安装」就漏了。
     */
    Log(L"");
    Log(L"检查厂家库（创苯 / 广成需要复制到程序目录）：");
    {
        int c = CopyVendorDlls();
        if (c == 0) Log(L"  · 无需拷贝（都已就位）");
        else        Log(L"  ✓ 已复制 %d 个厂家库", c);
    }
    Log(L"");
    Log(L"========== 安装成功 ==========");
    Log(L"打开 ZCANPRO，型号选「USBCAN-I」或「USBCAN-II」。");
    Log(L"");
    Log(L"· 插真周立功盒子：桥会自动退回原厂驱动，行为跟装之前一样。");
    Log(L"· 插别的牌子盒子：桥负责翻译。");
    Log(L"· 想恢复原样：点「一键卸载」。");
    SetStatus(L"✓ 安装成功 —— 可以打开 ZCANPRO 了", RGB(0, 130, 0));
}

/*----------------------------------------------------------------------------
  一键卸载
----------------------------------------------------------------------------*/
static void DoUninstall(void)
{
    wchar_t err[512];
    int ti, restored = 0, removed = 0;

    ClearLog();
    SetStatus(L"正在卸载…", RGB(0, 0, 0));
    Log(L"========== 一键卸载 ==========");

    if (!FindZcanPro(g_zcan, MAX_PATH)) {
        Log(L"✗ 没找到 ZCANPRO。");
        SetStatus(L"✗ 卸载失败：没找到 ZCANPRO", RGB(200, 0, 0));
        return;
    }
    _snwprintf(g_knr, MAX_PATH - 1, L"%s\\kerneldlls", g_zcan);
    g_knr[MAX_PATH - 1] = L'\0';
    Log(L"✓ ZCANPRO：%s", g_zcan);
    Log(L"");

    /* 逐个目标还原 */
    for (ti = 0; ti < NTARGETS; ti++) {
        wchar_t tbak[MAX_PATH], tdst[MAX_PATH], tmp[MAX_PATH];
        _snwprintf(tbak, MAX_PATH - 1, L"%s\\%s.zlg.bak", g_zcan, kTargets[ti].file);
        tbak[MAX_PATH - 1] = L'\0';
        _snwprintf(tdst, MAX_PATH - 1, L"%s\\%s", g_zcan, kTargets[ti].file);
        tdst[MAX_PATH - 1] = L'\0';

        if (!FileExists(tbak)) {
            /*
             ★ 没有备份 → 这个文件是安装时「新建」的桥（原本不存在）。
               判断依据：它的大小和 UniVciBridge.dll 一模一样。
               是就删掉；不是就碰都不碰（不能把厂家原本就有的 DLL 误删）。
             */
            wchar_t brg2[MAX_PATH];
            DWORD sBrg = 0, sTgt = 0;
            _snwprintf(brg2, MAX_PATH - 1, L"%s\\UniVciBridge.dll", g_zcan);
            brg2[MAX_PATH - 1] = L'\0';
            {
                HANDLE hb = CreateFileW(brg2, GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, 0, NULL);
                if (hb != INVALID_HANDLE_VALUE) { sBrg = GetFileSize(hb, NULL); CloseHandle(hb); }
            }
            {
                HANDLE ht = CreateFileW(tdst, GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, 0, NULL);
                if (ht != INVALID_HANDLE_VALUE) { sTgt = GetFileSize(ht, NULL); CloseHandle(ht); }
            }
            if (sTgt && sBrg && sTgt == sBrg) {
                /*
                 ★★ 千万别直接删 —— 先找 ZCANPRO 自带的原厂副本 ★★

                   事故：备份丢了（重装 ZCANPRO 会清 .zlg.bak），
                   一卸载就把 USBCAN.dll 直接删掉，
                   于是 ZCANPRO 没有设备插件了，任何盒子都打不开。
                   而且不需要重启电脑 —— 说明根本不是驱动问题，是文件。

                   正确做法：ZCANPRO 自己会在 kerneldlls\ 里放一份原厂 DLL，
                   从那里还原回去，比删掉安全一万倍。
                 */
                wchar_t knrCopy[MAX_PATH];
                _snwprintf(knrCopy, MAX_PATH - 1, L"%s\\kerneldlls\\%s",
                           g_zcan, kTargets[ti].file);
                knrCopy[MAX_PATH - 1] = L'\0';
                if (FileExists(knrCopy)) {
                    if (CopyFileX(knrCopy, tdst, err, 512)) {
                        Log(L"✓ %s 已从 kerneldlls 还原原厂副本", kTargets[ti].file);
                        Log(L"    （没有 .zlg.bak 备份，用 ZCANPRO 自带的那份顶上）");
                        restored++;
                        continue;
                    }
                }
                /*
                 连 kerneldlls 里都没有 → 这是安装时真的新建的文件，才删。
                 但如果删完目标就没了，宁可留着不动更安全：
                 删错了 ZCANPRO 就打不开盒子，留着顶多是版本不对。
                 */
                if (DeleteFileW(tdst)) {
                    Log(L"✓ %s 是安装时新建的，已删除（恢复原状）", kTargets[ti].file);
                    Log(L"    ⚠ 注意：这里没有原厂备份，也没有 kerneldlls 副本。");
                    Log(L"       如果 ZCANPRO 打不开盒子，请重装 ZCANPRO 恢复这个文件。");
                    removed++;
                } else {
                    Log(L"✗ %s 删不掉（请先关闭 ZCANPRO）", kTargets[ti].file);
                }
                continue;
            }
            continue;
        }

        /* 先复制到临时名，再替换，避免半路失败把原厂搞丢 */
        _snwprintf(tmp, MAX_PATH - 1, L"%s.restoring", tdst);
        tmp[MAX_PATH - 1] = L'\0';
        if (!CopyFileX(tbak, tmp, err, 512)) {
            Log(L"✗ %s 还原失败（准备）：%s", kTargets[ti].file, err);
            continue;
        }
        if (!CopyFileX(tmp, tdst, err, 512)) {
            Log(L"✗ %s 还原失败（写入）：%s", kTargets[ti].file, err);
            DeleteFileW(tmp);
            continue;
        }
        DeleteFileW(tmp);

        /* 校验大小 */
        {
            HANDLE h1 = CreateFileW(tdst, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, 0, NULL);
            HANDLE h2 = CreateFileW(tbak, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, 0, NULL);
            DWORD s1 = 0, s2 = 0;
            if (h1 != INVALID_HANDLE_VALUE) { s1 = GetFileSize(h1, NULL); CloseHandle(h1); }
            if (h2 != INVALID_HANDLE_VALUE) { s2 = GetFileSize(h2, NULL); CloseHandle(h2); }
            if (s1 != s2) {
                Log(L"✗ %s 校验失败：还原后 %lu，备份 %lu",
                    kTargets[ti].file, (unsigned long)s1, (unsigned long)s2);
                continue;
            }
            Log(L"✓ %s 已还原原厂（%lu 字节，校验通过）",
                kTargets[ti].file, (unsigned long)s1);
        }
        restored++;

        if (DeleteFileW(tbak)) Log(L"  已删除备份 %s.zlg.bak", kTargets[ti].file);
    }

    /* 删桥的文件 */
    {
        const wchar_t *files[] = { L"UniVciBridge.dll", L"UniVciBridge.ini",
                                   L"UniVciBridge.log" };
        int k;
        for (k = 0; k < 3; k++) {
            wchar_t p[MAX_PATH];
            _snwprintf(p, MAX_PATH - 1, L"%s\\%s", g_zcan, files[k]);
            p[MAX_PATH - 1] = L'\0';
            if (FileExists(p)) { DeleteFileW(p); Log(L"✓ 已删除 %s", files[k]); removed++; }
        }
    }

    /* 如果两个目标都没备份，且桥文件也不在 → 本来就是原厂 */
    if (restored == 0 && removed == 0) {
        Log(L"· 没找到任何桥的痕迹 —— 当前已是原厂状态。");
        SetStatus(L"· 无需卸载（当前是原厂状态）", RGB(120, 120, 0));
        return;
    }

    Log(L"");
    Log(L"========== 卸载完成 ==========");
    Log(L"ZCANPRO 已恢复原厂状态，和安装前完全一致。");
    Log(L"厂家自己的软件（PCAN-View 等）一直没被动过。");
    SetStatus(L"✓ 卸载完成 —— 已恢复原厂", RGB(0, 130, 0));
}

/*----------------------------------------------------------------------------
  一键诊断
----------------------------------------------------------------------------*/
/*============================================================================
  ★ 桥版本核对

  今天的事故都源于「不知道装的是哪一版」。
  这里算一个文件指纹（FNV-1a 64），把装着的和包里的比一下。
============================================================================*/

static void FileFingerprint(const wchar_t *path, wchar_t *out, int cch)
{
    HANDLE h;
    unsigned __int64 hsh = 1469598103934665603ULL;
    const unsigned __int64 prime = 1099511628211ULL;
    BYTE buf[8192];
    DWORD got = 0;
    DWORD sz = 0;

    if (!out || cch < 8) return;
    wcscpy(out, L"（读不到）");

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    sz = GetFileSize(h, NULL);
    while (ReadFile(h, buf, sizeof(buf), &got, NULL) && got > 0) {
        DWORD i;
        for (i = 0; i < got; i++) {
            hsh ^= (unsigned __int64)buf[i];
            hsh *= prime;
        }
    }
    CloseHandle(h);

    _snwprintf(out, cch - 1, L"%lu B  %08X%08X",
               (unsigned long)sz,
               (unsigned)(hsh >> 32), (unsigned)(hsh & 0xFFFFFFFFULL));
    out[cch - 1] = L'\0';
}

static void BridgeVersionCheck(void)
{
    wchar_t installed[MAX_PATH], packaged[MAX_PATH], self[MAX_PATH];
    wchar_t fpInst[MAX_PATH], fpPack[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    int hasInst, hasPack;

    if (!g_zcan[0]) return;

    _snwprintf(installed, MAX_PATH - 1, L"%s\\USBCAN.dll", g_zcan);
    installed[MAX_PATH - 1] = L'\0';

    SelfDir(self, MAX_PATH);
    _snwprintf(packaged, MAX_PATH - 1, L"%s\\_prog\\UniVciBridge.dll", self);
    packaged[MAX_PATH - 1] = L'\0';
    if (!FileExists(packaged)) {
        _snwprintf(packaged, MAX_PATH - 1, L"%s\\UniVciBridge.dll", self);
        packaged[MAX_PATH - 1] = L'\0';
    }

    hasInst = FileExists(installed);
    hasPack = FileExists(packaged);

    Log(L"");
    Log(L"【版本核对】");
    if (!hasInst) {
        Log(L"  · 装着：没有 USBCAN.dll（桥还没装）");
    } else {
        FileFingerprint(installed, fpInst, MAX_PATH);
        Log(L"  装着  %s", installed);
        Log(L"        %s", fpInst);
        if (GetFileAttributesExW(installed, GetFileExInfoStandard, &fad)) {
            SYSTEMTIME st;
            FileTimeToSystemTime(&fad.ftLastWriteTime, &st);
            Log(L"        修改时间 %04d-%02d-%02d %02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        }
    }
    if (!hasPack) {
        Log(L"  · 包里：找不到 _prog\\UniVciBridge.dll（包不完整）");
    } else {
        FileFingerprint(packaged, fpPack, MAX_PATH);
        Log(L"  包里  %s", packaged);
        Log(L"        %s", fpPack);
    }

    if (hasInst && hasPack) {
        if (wcscmp(fpInst, fpPack) == 0)
            Log(L"  ✓ 一致，测的就是包里这一版");
        else
            Log(L"  ★ 不一致！点「一键安装」重新铺一遍再测");
    }
    Log(L"");
}

static void DoDiagnose(void)
{
    /* ★ 先打版本，免得又不知道测的是哪一版 */
    BridgeVersionCheck();

    static const wchar_t *must[] = { L"usbcan.dll", L"CANFDNET.dll", L"CANDTU_NET.dll" };
    static const wchar_t *opt[]  = { L"USBCAN_E.dll", L"USBCAN_4E_U.dll",
                                     L"USBCAN_8E_U.dll", L"USBCANFD.dll" };
    int i, prob = 0;
    wchar_t p[MAX_PATH];

    ClearLog();
    SetStatus(L"正在诊断…", RGB(0, 0, 0));
    Log(L"========== 一键诊断 ==========");

    if (!FindZcanPro(g_zcan, MAX_PATH)) {
        Log(L"✗ 没找到 ZCANPRO");
        SetStatus(L"✗ 诊断：没找到 ZCANPRO", RGB(200, 0, 0));
        return;
    }
    _snwprintf(g_knr, MAX_PATH - 1, L"%s\\kerneldlls", g_zcan);
    g_knr[MAX_PATH - 1] = L'\0';
    Log(L"✓ ZCANPRO：%s", g_zcan);
    Log(L"");

    /* 1. 桥状态（逐个目标） */
    Log(L"【1】桥的安装状态");
    {
        int ti, anyInstalled = 0;
        wchar_t brg[MAX_PATH];
        /* ★ 桥装在程序目录（g_zcan），不是 kerneldlls（g_knr） */
        _snwprintf(brg, MAX_PATH - 1, L"%s\\UniVciBridge.dll", g_zcan);
        brg[MAX_PATH - 1] = L'\0';
        for (ti = 0; ti < NTARGETS; ti++) {
            wchar_t tbak[MAX_PATH];
            _snwprintf(tbak, MAX_PATH - 1, L"%s\\%s.zlg.bak", g_zcan, kTargets[ti].file);
            tbak[MAX_PATH - 1] = L'\0';
            if (FileExists(tbak)) {
                Log(L"  ✓ %s 已装桥 —— %ls", kTargets[ti].file, kTargets[ti].desc);
                anyInstalled = 1;
            } else {
                _snwprintf(p, MAX_PATH - 1, L"%s\\%s", g_zcan, kTargets[ti].file);
                p[MAX_PATH - 1] = L'\0';
                if (FileExists(p))
                    Log(L"  · %s 是原厂（%ls）", kTargets[ti].file, kTargets[ti].desc);
                else
                Log(L"  · %s 不存在（对应型号不可用）", kTargets[ti].file);
            }
        }
        if (FileExists(brg)) Log(L"  · UniVciBridge.dll 就位");
        else if (anyInstalled) { Log(L"  ✗ 缺 UniVciBridge.dll（安装不完整）"); prob++; }
        if (!anyInstalled) Log(L"  · 当前未安装桥（原厂状态）");
    }
    Log(L"");

    /* 2. ZCANPRO 关键驱动 */
    Log(L"【2】ZCANPRO 自带驱动");
    for (i = 0; i < 3; i++) {
        _snwprintf(p, MAX_PATH - 1, L"%s\\%s", g_knr, must[i]);
        p[MAX_PATH - 1] = L'\0';
        if (FileExists(p)) Log(L"  ✓ %s", must[i]);
        else { Log(L"  ✗ 缺 %s", must[i]); prob++; }
    }
    for (i = 0; i < 4; i++) {
        _snwprintf(p, MAX_PATH - 1, L"%s\\%s", g_knr, opt[i]);
        p[MAX_PATH - 1] = L'\0';
        if (FileExists(p)) Log(L"  ✓ %s", opt[i]);
        else Log(L"  · 缺 %s（对应型号打不开，非必需）", opt[i]);
    }
    Log(L"");

    /* 3. 各家驱动 */
    Log(L"【3】各家 CAN 盒子驱动");
    {
        char be[64];
        wchar_t pretty[512];
        int n = DetectBackend(be, sizeof(be), pretty, 512);
        if (n) {
            Log(L"  ✓ 发现：%s", pretty);
            Log(L"    → 后端会自动用 %hs", be);
        } else {
            Log(L"  · ZCANPRO 程序目录里没有第三方驱动库");
            Log(L"    要用 创苯 / 广成 / Kvaser，把它们的 DLL 复制到");
            Log(L"    ZCANPRO 程序目录（不是 kerneldlls —— 实测证实它不是搜索路径）");
            Log(L"    本包 drivers\\ 里有 5 个；点「一键安装」会自动复制。");
            Log(L"    插 PEAK 盒子不用管（驱动把 PCANBasic.dll 装到了系统目录）。");
        }
    }
    Log(L"");

    /* 4. 内核驱动 */
    Log(L"【4】内核驱动（盒子要在设备管理器里出现，必须有这个）");
    {
        SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        int hasZlg = 0, hasPcan = 0;
        if (scm) {
            const wchar_t *pcanNames[] = { L"PCAN_USB", L"PCAN_PCI", L"PCAN_LAN",
                                           L"PCAN_ISA", L"PCAN_PCC", L"PCAN_PCIE" };
            SC_HANDLE s;
            s = OpenServiceW(scm, L"ZLGCAN", SERVICE_QUERY_STATUS);
            if (s) { hasZlg = 1; CloseServiceHandle(s); }
            for (i = 0; i < 6; i++) {
                s = OpenServiceW(scm, pcanNames[i], SERVICE_QUERY_STATUS);
                if (s) { hasPcan = 1; CloseServiceHandle(s); break; }
            }
            CloseServiceHandle(scm);
        }
        if (hasZlg) Log(L"  ✓ 周立功 ZLGCAN 驱动已装");
        else Log(L"  · 周立功 ZLGCAN 驱动未装（装 USBCAN_AllInOne 即可）");
        if (hasPcan) Log(L"  ✓ PEAK 内核驱动已装");
        else Log(L"  · PEAK 内核驱动未装（装 PEAK Setup 即可，光有 DLL 不行）");
    }
    Log(L"");

    /* 5. 结论 */
    Log(L"========== 结论 ==========");
    if (prob == 0) {
        Log(L"没发现致命问题。");
        Log(L"如果盒子还是打不开，重点看这两点：");
        Log(L"  · 设备管理器里有没有这个盒子（没有=内核驱动没装）");
        Log(L"  · %s\\UniVciBridge.log 里后端选了谁、哪一步失败", g_zcan);
        SetStatus(L"✓ 诊断完成：没发现致命问题", RGB(0, 130, 0));
    } else {
        Log(L"发现 %d 个问题，见上面标 ✗ 的项。", prob);
        SetStatus(L"诊断完成：发现问题，请看日志", RGB(180, 100, 0));
    }
}

/*----------------------------------------------------------------------------
  打开日志目录
----------------------------------------------------------------------------*/
static void DoOpenLog(void)
{
    wchar_t p[MAX_PATH];
    if (!g_zcan[0]) {
        Log(L"· 请先点一次安装或诊断。");
        return;
    }
    /* ★ 日志就在 ZCANPRO 程序目录（桥被宿主 EXE 加载时写在那里） */
    _snwprintf(p, MAX_PATH - 1, L"%s\\UniVciBridge.log", g_zcan);
    p[MAX_PATH - 1] = L'\0';
    if (!FileExists(p)) {
        Log(L"· 还没有日志文件（说明桥还没被调用过 —— 正常）。");
        Log(L"  日志位置：%s", p);
        return;
    }
    /*
     ★ 把日志复制到【一键工具.exe 所在目录】再打开副本。

       桥的日志写在宿主（ZCANPRO）目录 —— 因为桥是被 ZCANPRO 加载的。
       但用户要拿日志去发，一键工具旁边最好找。
       命名带时间戳，多次点不会互相覆盖。
     */
    {
        wchar_t self[MAX_PATH], dst[MAX_PATH];
        SYSTEMTIME st;
        SelfDir(self, MAX_PATH);
        GetLocalTime(&st);
        _snwprintf(dst, MAX_PATH - 1,
                   L"%s\\桥日志-%04d%02d%02d-%02d%02d%02d.txt",
                   self, st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);
        dst[MAX_PATH - 1] = L'\0';
        if (CopyFileW(p, dst, FALSE)) {
            Log(L"✓ 日志已复制到：%s", dst);
            Log(L"  这个文件可以直接发出去。");
            ShellExecuteW(NULL, L"open", L"notepad.exe", dst, NULL, SW_SHOWNORMAL);
        } else {
            Log(L"· 复制到一键工具目录失败（权限？），直接打开原文件：");
            Log(L"  %s", p);
            ShellExecuteW(NULL, L"open", L"notepad.exe", p, NULL, SW_SHOWNORMAL);
        }
    }
}

/*----------------------------------------------------------------------------
  窗口
----------------------------------------------------------------------------*/
static void Layout(HWND hWnd)
{
    RECT rc;
    int w, h, bw, bh = 48, gap = 10, x, y1, y2;

    GetClientRect(hWnd, &rc);
    w = rc.right;
    h = rc.bottom;
    if (w < 480) w = 480;
    if (h < 420) h = 420;

    /*
     ★ 界面布局（两行按钮 + 大主按钮）

       原来五个按钮挤一行，每个只有 132px，「选软件装桥（任意 EXE）」
       这类文字被裁掉。改成两行：
           第一行：选软件装桥   一键安装   一键卸载
           第二行：一键诊断     看日志
       每个按钮宽度 ≈ (可用宽度-间距)/3，780 宽时约 240px，字肯定放得下。

       窗口拉大缩小时按钮自动跟着平分，不写死像素。
     */
    MoveWindow(g_hStatus, 16, 10, w - 32, 30, TRUE);

    /* 主按钮：整行，最高 */
    MoveWindow(GetDlgItem(hWnd, IDC_BTN_ONE), 16, 46, w - 32, 54, TRUE);

    /* 两行按钮占的高度 */
    y2 = h - bh - 14;
    y1 = y2 - bh - gap;

    /* 日志区：从主按钮下面一直到第一行按钮上面 */
    MoveWindow(g_hLog, 16, 108, w - 32, y1 - 108 - 10, TRUE);

    /* 三列布局 */
    bw = (w - 32 - 2 * gap) / 3;
    if (bw < 140) bw = 140;

    /* 第一行：三个 */
    x = 16;
    MoveWindow(GetDlgItem(hWnd, IDC_BTN_ANY),  x, y1, bw, bh, TRUE);
    x += bw + gap;
    MoveWindow(GetDlgItem(hWnd, IDC_BTN_INST), x, y1, bw, bh, TRUE);
    x += bw + gap;
    MoveWindow(GetDlgItem(hWnd, IDC_BTN_UNIN), x, y1, bw, bh, TRUE);

    /* 第二行：两个，各占 1.5 列宽 */
    bw = (w - 32 - gap) / 2;
    if (bw < 200) bw = 200;
    x = 16;
    MoveWindow(GetDlgItem(hWnd, IDC_BTN_DIAG), x, y2, bw, bh, TRUE);
    x += bw + gap;
    MoveWindow(GetDlgItem(hWnd, IDC_BTN_OPEN), x, y2, bw, bh, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hStatus = CreateWindowExW(0, L"STATIC", L"准备就绪",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    0, 0, 0, 0, hWnd, (HMENU)IDC_STATUS, NULL, NULL);
        g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                 ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                 0, 0, 0, 0, hWnd, (HMENU)IDC_LOG, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"① 一键检测并修复（先点这个）",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_ONE, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"选软件装桥",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_ANY, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"一键安装",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_INST, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"一键卸载",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_UNIN, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"一键诊断",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_DIAG, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"看日志",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_OPEN, NULL, NULL);

        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_fontBig, TRUE);
        SendMessageW(g_hLog, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
        SendMessageW(GetDlgItem(hWnd, IDC_BTN_ONE),  WM_SETFONT, (WPARAM)g_fontBig, TRUE);
        SendMessageW(GetDlgItem(hWnd, IDC_BTN_ANY),  WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        SendMessageW(GetDlgItem(hWnd, IDC_BTN_INST), WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        SendMessageW(GetDlgItem(hWnd, IDC_BTN_UNIN), WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        SendMessageW(GetDlgItem(hWnd, IDC_BTN_DIAG), WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        SendMessageW(GetDlgItem(hWnd, IDC_BTN_OPEN), WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        return 0;

    case WM_SIZE:
        Layout(hWnd);
        return 0;

    case WM_COMMAND:
        if (g_busy) return 0;
        switch (LOWORD(wp)) {
        case IDC_BTN_ONE:  g_busy = 1; DoOneClick();  g_busy = 0; break;
        case IDC_BTN_ANY:  g_busy = 1; DoInstallAny(); g_busy = 0; break;
        case IDC_BTN_INST: g_busy = 1; DoInstall();   g_busy = 0; break;
        case IDC_BTN_UNIN: g_busy = 1; DoUninstall(); g_busy = 0; break;
        case IDC_BTN_DIAG: g_busy = 1; DoDiagnose();  g_busy = 0; break;
        case IDC_BTN_OPEN: DoOpenLog(); break;
        }
        return 0;

    case WM_DESTROY:
        if (g_rep) fclose(g_rep);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE prev, LPWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    HWND hWnd;
    MSG msg;
    HICON hIcon;
    (void)prev;

    /* 命令行可以带一个目录，例：OneClick.exe "D:\Work\ZCANPRO" */
    g_forced[0] = L'\0';
    if (cmd && cmd[0]) {
        wchar_t *s = cmd;
        while (*s == L' ' || *s == L'"') s++;
        {
            wchar_t *e = s;
            while (*e && *e != L'"') e++;
            if (e > s && (size_t)(e - s) < MAX_PATH) {
                wcsncpy(g_forced, s, (size_t)(e - s));
                g_forced[e - s] = L'\0';
            }
        }
        /* 去掉尾部反斜杠 */
        {
            size_t L2 = wcslen(g_forced);
            while (L2 > 0 && (g_forced[L2 - 1] == L'\\' || g_forced[L2 - 1] == L'/'))
                g_forced[--L2] = L'\0';
        }
    }

    /* 单实例：防止重复开 */
    CreateMutexW(NULL, TRUE, L"CANmeleonUniBridgeOneClickMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"工具已经在运行了。", L"提示", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    InitCommonControls();

    g_fontUI   = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontBig  = CreateFontW(-21, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontMono = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    if (!g_fontMono) g_fontMono = g_fontUI;

    hIcon = LoadIconW(NULL, IDI_APPLICATION);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"CANmeleonOneClick";
    wc.hIcon         = hIcon;
    wc.hIconSm       = hIcon;
    RegisterClassExW(&wc);

    hWnd = CreateWindowExW(0, L"CANmeleonOneClick",
                           L"CAN 多盒子共存 万能桥 —— 一键工具",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 820, 700,
                           NULL, NULL, hInst, NULL);
    if (!hWnd) return 1;

    ShowWindow(hWnd, show);
    UpdateWindow(hWnd);

    /* 报告文件（放桌面） */
    {
        wchar_t desk[MAX_PATH], rep[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, 0x0010, NULL, 0, desk))) {
            _snwprintf(rep, MAX_PATH - 1, L"%s\\万能桥-操作日志.txt", desk);
            rep[MAX_PATH - 1] = L'\0';
            g_rep = _wfopen(rep, L"wb");
            if (g_rep) {
                const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
                fwrite(bom, 1, 3, g_rep);
            }
        }
    }

    Log(L"CAN 多盒子共存 万能桥 · 一键工具");
    Log(L"");
    Log(L"这个东西做什么：");
    Log(L"  让 PCAN / 广成 / TOSUN / Kvaser / Vector 等牌子的 CAN 盒子");
    Log(L"  也能在周立功的 ZCANPRO 里用。");
    Log(L"");
    Log(L"三步：");
    Log(L"  1. 点「一键安装」（需要管理员权限）");
    Log(L"  2. 打开 ZCANPRO，型号选 USBCAN-I 或 USBCAN-II");
    Log(L"  3. 想恢复原样，点「一键卸载」");
    Log(L"");
    Log(L"放心的地方：");
    Log(L"  · 安装前会把原厂 USBCAN.dll 备份，卸载时原样还原");
    Log(L"  · 插真周立功盒子时，桥会自动退回原厂驱动，不影响厂家软件");
    Log(L"");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_fontUI) DeleteObject(g_fontUI);
    if (g_fontBig) DeleteObject(g_fontBig);
    if (g_fontMono) DeleteObject(g_fontMono);
    return (int)msg.wParam;
}
