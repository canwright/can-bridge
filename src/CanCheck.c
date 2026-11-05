/*
 * CanCheck.c —— 「这个软件能不能用桥？」检测器
 *
 * 用法：
 *   把某个软件的主 EXE（或它的文件夹）拖到本程序上，
 *   或者命令行：CanCheck.exe "D:\某软件\主程序.exe"
 *
 * 它做三件事：
 *   1. 扫 EXE 里引用了哪些 CAN 库（ControlCAN.dll / USBCAN.dll / zlgcan.dll / ...）
 *   2. 扫 EXE 里用了哪套 API（VCI_* / ZCAN_* / CAN_Initialize / canOpenChannel / xl*）
 *   3. 扫同目录的 DLL 导出表，判断「这个位置换上桥能不能顶替」
 *
 * 判断依据（这是整件事的核心）：
 *
 *   桥本身只实现周立功的 VCI 接口那 14 个函数：
 *     VCI_OpenDevice / VCI_CloseDevice / VCI_InitCAN / VCI_StartCAN
 *     VCI_ResetCAN / VCI_ClearBuffer / VCI_GetReceiveNum / VCI_Transmit
 *     VCI_Receive / VCI_ReadBoardInfo / VCI_ReadErrInfo / VCI_ReadCANStatus
 *     VCI_GetReference / VCI_SetReference
 *
 *   所以：
 *     · 软件 DllImport 的是 ControlCAN.dll 或 USBCAN.dll，调 VCI_*  → 能顶替
 *     · 软件 DllImport 的是 zlgcan.dll，调 ZCAN_*                   → 直接顶不了
 *       （但 zlgcan.dll 内部会按 kerneldlls\dll_cfg.ini 去加载插件，
 *         那里 3=/4= 通常就指向 USBCAN.dll —— 可以换插件试试，未验证）
 *     · 软件只用 PCANBasic / canlib32 / vxlapi                       → 本来就不用桥
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <io.h>

#define MAXF 64

static int  g_cfgHasUsbcan = 0;
static wchar_t g_cfgUsbcanName[64] = L"";

/* ---------- 小工具 ---------- */

/* ---------- 输出 ---------- *
 * 为什么不用 wprintf 直接写：
 *   重定向到文件时，宽字符会被按当前 locale 转换，非 ASCII 全丢；
 *   这里自己转成 UTF-8 再用 fputs 写，控制台和文件都正常。
 */
static void Out(const wchar_t *fmt, ...)
{
    wchar_t wbuf[4096];
    char    u8[16384];
    va_list ap;
    int wl, al;

    va_start(ap, fmt);
    wl = _vsnwprintf(wbuf, 4095, fmt, ap);
    va_end(ap);
    if (wl <= 0) return;
    wbuf[4095] = L'\0';
    if (wl > 4000) wl = 4000;

    al = WideCharToMultiByte(CP_UTF8, 0, wbuf, wl, u8, (int)sizeof(u8) - 1, NULL, NULL);
    if (al <= 0) return;
    u8[al] = 0;
    fputs(u8, stdout);
}

static unsigned char *ReadAll(const wchar_t *path, DWORD *outLen)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD sz, got = 0;
    unsigned char *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0) { CloseHandle(h); return NULL; }
    buf = (unsigned char *)malloc(sz);
    if (!buf) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, sz, &got, NULL) || got != sz) {
        free(buf); CloseHandle(h); return NULL;
    }
    CloseHandle(h);
    *outLen = sz;
    return buf;
}

/* 统计 ASCII 子串出现次数 */
static int CountAscii(const unsigned char *b, DWORD n, const char *needle)
{
    int cnt = 0;
    DWORD i, nl = (DWORD)strlen(needle);
    if (!b || n < nl) return 0;
    for (i = 0; i + nl <= n; i++) {
        if (b[i] == (unsigned char)needle[0] && memcmp(b + i, needle, nl) == 0)
            cnt++;
    }
    return cnt;
}

/* 宽字符（UTF-16LE）子串出现次数，用于找中文/带点的名字 */
static int CountWide(const unsigned char *b, DWORD n, const wchar_t *needle)
{
    int cnt = 0;
    DWORD i, nl = (DWORD)wcslen(needle) * 2;
    const unsigned char *p = (const unsigned char *)needle;
    if (!b || n < nl) return 0;
    for (i = 0; i + nl <= n; i++) {
        if (b[i] == p[0] && memcmp(b + i, p, nl) == 0)
            cnt++;
    }
    return cnt;
}

/* ---------- DLL 导出检查 ---------- */

typedef struct {
    wchar_t file[64];      /* 文件名（必须是数组：指向局部缓冲区会悬空） */
    int  vci14;            /* 14 个 VCI_ 函数齐不齐 */
    int  vci24;            /* 带 FindUsbDevice 的 24 导出（创芯） */
    int  zcan;             /* 有 ZCAN_ 导出（周立功新一代） */
    int  isBridge;         /* 是不是桥自己（靠 UniVciBridgeIdent 认） */
    DWORD size;
} DLLINFO;

static const char *kVci14[] = {
    "VCI_OpenDevice", "VCI_CloseDevice", "VCI_InitCAN", "VCI_StartCAN",
    "VCI_ResetCAN", "VCI_ClearBuffer", "VCI_GetReceiveNum", "VCI_Transmit",
    "VCI_Receive", "VCI_ReadBoardInfo", "VCI_ReadErrInfo", "VCI_ReadCANStatus",
    "VCI_GetReference", "VCI_SetReference"
};

static DLLINFO InspectDll(const wchar_t *full)
{
    DLLINFO d;
    HMODULE h;
    int i, ok = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;

    ZeroMemory(&d, sizeof(d));
    {
        const wchar_t *slash = wcsrchr(full, L'\\');
        const wchar_t *nm = slash ? slash + 1 : full;
        wcsncpy(d.file, nm, 63);
        d.file[63] = 0;
    }

    if (GetFileAttributesExW(full, GetFileExInfoStandard, &fa))
        d.size = fa.nFileSizeLow;

    /* 不解析依赖地加载，只为拿导出地址 */
    h = LoadLibraryExW(full, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!h) return d;

    for (i = 0; i < 14; i++)
        if (GetProcAddress(h, kVci14[i])) ok++;
    d.vci14 = (ok == 14);

    d.vci24 = GetProcAddress(h, "VCI_FindUsbDevice") ? 1 : 0;
    d.zcan  = GetProcAddress(h, "ZCAN_OpenDevice") ? 1 : 0;

    {
        typedef DWORD (__stdcall *PF_Ident)(void);
        PF_Ident pf = (PF_Ident)GetProcAddress(h, "UniVciBridgeIdent");
        if (pf && pf() == 0x55434247u) d.isBridge = 1;
    }

    FreeLibrary(h);
    return d;
}

/* ---------- 扫目录里的 CAN 相关 DLL ---------- */

static int ScanFolder(const wchar_t *dir, DLLINFO *out, int max)
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;

    _snwprintf(pat, MAX_PATH * 2 - 1, L"%s\\*.dll", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t full[MAX_PATH * 2];
        if (n >= max) break;
        _snwprintf(full, MAX_PATH * 2 - 1, L"%s\\%s", dir, fd.cFileName);
        out[n] = InspectDll(full);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* 找同级或上级的 kerneldlls\dll_cfg.ini */
static int FindKernelCfg(const wchar_t *startDir, wchar_t *found, DWORD cch)
{
    wchar_t d[MAX_PATH * 2];
    int lvl;
    wcscpy(d, startDir);
    for (lvl = 0; lvl < 3; lvl++) {
        wchar_t p[MAX_PATH * 2];
        _snwprintf(p, MAX_PATH * 2 - 1, L"%s\\kerneldlls\\dll_cfg.ini", d);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
            wcsncpy(found, p, cch - 1);
            found[cch - 1] = 0;
            return 1;
        }
        {
            wchar_t *slash = wcsrchr(d, L'\\');
            if (!slash || slash == d) break;
            *slash = 0;
        }
    }
    return 0;
}

/* ---------- 读 dll_cfg.ini ---------- *
 * 格式：N=<DLL文件名> <设备类型> <缓冲数> <isCAN>
 *   例：3=USBCAN.dll 3 32 1
 *
 * 这是关键线索：有它说明这个软件是「按配置动态加载设备 DLL」的，
 * 那么 EXE 里搜不到 DLL 名字也很正常，桥照样能顶替。
 */
static int ParseDllCfg(const wchar_t *ini, wchar_t names[][64], int max)
{
    HANDLE h = CreateFileW(ini, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char buf[16384];
    DWORD got = 0, i = 0;
    int cnt = 0;

    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL)) { CloseHandle(h); return 0; }
    CloseHandle(h);
    buf[got] = 0;

    while (i < got && cnt < max) {
        /* 一行一行看 */
        DWORD e = i;
        char *eq;
        while (e < got && buf[e] != '\r' && buf[e] != '\n') e++;

        buf[e & (sizeof(buf) - 1)] = buf[e & (sizeof(buf) - 1)];   /* no-op, keep bounds */
        if (e < sizeof(buf)) buf[e] = 0;

        eq = strchr(buf + i, '=');
        if (eq && eq != buf + i) {
            /* 跳过等号左边（编号） */
            char *p = eq + 1;
            char name[64];
            int k = 0;
            /* 可能前面有空格 */
            while (*p == ' ' || *p == '\t') p++;
            while (*p && *p != ' ' && *p != '\t' && k < 63) name[k++] = *p++;
            name[k] = 0;
            if (k > 4) {
                /* 只收 .dll 结尾的 */
                if (_stricmp(name + k - 4, ".dll") == 0) {
                    int dup = 0, j;
                    wchar_t wname[64];
                    MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 64);
                    for (j = 0; j < cnt; j++)
                        if (_wcsicmp(names[j], wname) == 0) dup = 1;
                    if (!dup) {
                        MultiByteToWideChar(CP_ACP, 0, name, -1, names[cnt], 64);
                        cnt++;
                    }
                }
            }
        }

        i = (e < got) ? e + 1 : got;
        /* 跳过 \r\n */
        while (i < got && (buf[i] == '\r' || buf[i] == '\n')) i++;
    }
    return cnt;
}

/* ---------- 主判断 ---------- */

int wmain(int argc, wchar_t **argv)
{
    wchar_t target[MAX_PATH * 2];
    wchar_t dir[MAX_PATH * 2];
    DWORD len = 0;
    unsigned char *data;
    int cVci = 0, cZcan = 0, cPcan = 0, cKvaser = 0, cXl = 0;
    int refControlCAN = 0, refUSBCAN = 0, refZlgcan = 0, refControlCAN1 = 0;
    int refPcanBasic = 0, refKvaser = 0, refVxl = 0, refECan = 0;
    DLLINFO dlls[MAXF];
    int nDll = 0, i;
    wchar_t kcfg[MAX_PATH * 2] = L"";
    int hasKcfg = 0;
    int wantW = 0;   /* 1 = 输出里有中文，需要宽字符 */
    int bridgeOk = 0, nativeOk = 0;

    SetConsoleOutputCP(65001);

    if (argc < 2) {
        Out(L"\n  用法：把某个软件的主程序（EXE）或它的文件夹拖到本程序上\n\n");
        Out(L"     CanCheck.exe \"D:\\软件目录\\主程序.exe\"\n\n");
        Out(L"  它会告诉你：这个软件能不能用「万能桥」把别的盒子伪装成周立功 USBCAN\n\n");
        getchar();
        return 1;
    }

    wcsncpy(target, argv[1], MAX_PATH * 2 - 1);
    target[MAX_PATH * 2 - 1] = 0;

    if (GetFileAttributesW(target) & FILE_ATTRIBUTE_DIRECTORY)
        _snwprintf(dir, MAX_PATH * 2 - 1, L"%s", target);
    else {
        /* ★ 注意：不能直接在 target 上截断，
           否则 target 会变成目录路径，后面 ReadAll(target) 就读不到文件了
           （真机踩到：target 里 .exe 被截掉）。 */
        wchar_t tcopy[MAX_PATH * 2];
        wchar_t *slash;
        wcsncpy(tcopy, target, MAX_PATH * 2 - 1);
        tcopy[MAX_PATH * 2 - 1] = 0;
        slash = wcsrchr(tcopy, L'\\');
        if (slash) { *slash = 0; wcsncpy(dir, tcopy, MAX_PATH * 2 - 1); }
        else wcscpy(dir, L".");
    }
    dir[MAX_PATH * 2 - 1] = 0;

    Out(L"\n  ══════════════════════════════════════════════════\n");
    Out(L"   能不能用桥？检测器\n");
    Out(L"  ══════════════════════════════════════════════════\n\n");
    Out(L"  目标：%s\n", target);
    Out(L"  目录：%s\n\n", dir);

    data = ReadAll(target, &len);
    if (!data) {
        Out(L"  ★ 读不到这个文件（是不是选成了文件夹？把里面的 EXE 拖进来）\n\n");
        getchar();
        return 1;
    }

    /* ---- 1. EXE 里引用了哪些库 ---- */
    refControlCAN1 = CountAscii(data, len, "ControlCAN1.dll");
    refControlCAN  = CountAscii(data, len, "ControlCAN.dll");
    refUSBCAN      = CountAscii(data, len, "USBCAN.dll");
    refZlgcan      = CountAscii(data, len, "zlgcan.dll");
    refPcanBasic   = CountAscii(data, len, "PCANBasic.dll");
    refKvaser      = CountAscii(data, len, "canlib32.dll");
    refVxl         = CountAscii(data, len, "vxlapi");
    refECan        = CountAscii(data, len, "ECanVci.dll");

    /* ---- 2. 用了哪套 API ---- */
    cVci    = CountAscii(data, len, "VCI_OpenDevice") + CountAscii(data, len, "VCI_InitCAN")
            + CountAscii(data, len, "VCI_Transmit")   + CountAscii(data, len, "VCI_Receive");
    cZcan   = CountAscii(data, len, "ZCAN_OpenDevice") + CountAscii(data, len, "ZCAN_InitCAN")
            + CountAscii(data, len, "ZCAN_Transmit")   + CountAscii(data, len, "ZCAN_Receive");
    cPcan   = CountAscii(data, len, "CAN_Initialize") + CountAscii(data, len, "CAN_Read");
    cKvaser = CountAscii(data, len, "canOpenChannel") + CountAscii(data, len, "canWrite");
    cXl     = CountAscii(data, len, "xlOpenPort") + CountAscii(data, len, "xlCanTransmit");

    /* USBCAN / 型号名（宽字符），用来判断它认哪些型号 */
    {
        int wUSBCAN2 = CountWide(data, len, L"USBCAN-II");
        int wUSBCAN1 = CountWide(data, len, L"USBCAN-I");
        int w2EU     = CountWide(data, len, L"USBCAN-2E-U");
        int wAnalyst = CountWide(data, len, L"CANalyst-II");
        (void)wUSBCAN2; (void)wUSBCAN1; (void)w2EU;
        wantW = wAnalyst;
        if (wAnalyst)
            Out(L"  · 菜单里有「CANalyst-II」（创芯原生支持）\n");
        if (w2EU)
            Out(L"  · 菜单里有「USBCAN-2E-U」（周立功 E-U 原生支持）\n");
        if (wAnalyst || w2EU) Out(L"\n");
    }
    free(data);

    Out(L"  ── 1. 它引用了哪些 CAN 库 ──\n");
    if (refControlCAN1) Out(L"     ControlCAN1.dll     （创芯 USB-CAN/CANalyst-II 常用名）\n");
    if (refControlCAN)  Out(L"     ControlCAN.dll      （周立功经典 VCI 接口）\n");
    if (refUSBCAN)      Out(L"     USBCAN.dll          （周立功设备 DLL）\n");
    if (refZlgcan)      Out(L"     zlgcan.dll          （周立功新一代 ZCAN_ 接口）\n");
    if (refPcanBasic)   Out(L"     PCANBasic.dll       （PEAK）\n");
    if (refKvaser)      Out(L"     canlib32.dll        （Kvaser）\n");
    if (refVxl)         Out(L"     vxlapi*.dll         （Vector）\n");
    if (refECan)        Out(L"     ECanVci.dll         （广成）\n");
    if (!refControlCAN1 && !refControlCAN && !refUSBCAN && !refZlgcan &&
        !refPcanBasic && !refKvaser && !refVxl && !refECan)
        Out(L"     （一个都没认出来 —— 可能库名是动态拼的，或者根本不用 CAN）\n");
    Out(L"\n");

    Out(L"  ── 2. 它用的是哪套接口 ──\n");
    if (cVci)    Out(L"     VCI_*   %d 处   ← 桥实现的正是这一套\n", cVci);
    if (cZcan)   Out(L"     ZCAN_*  %d 处   ← 桥【不】实现这一套\n", cZcan);
    if (cPcan)   Out(L"     CAN_*   %d 处   （PEAK Basic）\n", cPcan);
    if (cKvaser) Out(L"     can*    %d 处   （Kvaser）\n", cKvaser);
    if (cXl)     Out(L"     xl*     %d 处   （Vector）\n", cXl);
    if (!cVci && !cZcan && !cPcan && !cKvaser && !cXl)
        Out(L"     （没认出接口名 —— 可能是 .NET 包装类，名字被混淆了）\n");
    Out(L"\n");

    /* ---- 3. 扫目录里的 DLL ---- */
    Out(L"  ── 3. 目录里的 CAN 相关库 ──\n");
    nDll = ScanFolder(dir, dlls, MAXF);
    {
        int shown = 0;
        for (i = 0; i < nDll; i++) {
            if (!dlls[i].vci14 && !dlls[i].zcan && !dlls[i].isBridge) continue;
            Out(L"     %-22s %9lu B   %s%s%s\n",
                    dlls[i].file, (unsigned long)dlls[i].size,
                    dlls[i].isBridge ? L"【已经是桥】" : L"",
                    dlls[i].vci14 ? L"VCI 14 函数齐全 " : L"",
                    dlls[i].zcan ? L"含 ZCAN_" : L"");
            shown++;
            if (dlls[i].vci14 && !dlls[i].isBridge) bridgeOk = 1;
            if (dlls[i].vci24) nativeOk = 1;   /* 创芯自己的库在旁边 */
        }
        if (!shown) Out(L"     （这个目录里没有可直接顶替的 VCI 库）\n");
    }
    Out(L"\n");

    hasKcfg = FindKernelCfg(dir, kcfg, MAX_PATH * 2);
    {
        static wchar_t cfgNames[64][64];
        int nCfg = 0;
        if (hasKcfg) nCfg = ParseDllCfg(kcfg, cfgNames, 64);
        if (hasKcfg) {
            Out(L"  · 发现插件配置：%s\n", kcfg);
            Out(L"     这说明它是「按配置动态加载设备 DLL」的，\n");
            Out(L"     EXE 里搜不到 DLL 名字也很正常。\n");
            if (nCfg) {
                int j, shown = 0;
                Out(L"     配置里列了这些设备 DLL：\n");
                for (j = 0; j < nCfg && shown < 10; j++) {
                    Out(L"       %s\n", cfgNames[j]);
                    shown++;
                }
                /* VCI 类（能顶替的目标） */
                for (j = 0; j < nCfg; j++) {
                    if (wcsstr(cfgNames[j], L"USBCAN") || wcsstr(cfgNames[j], L"usbcan")) {
                        g_cfgHasUsbcan = 1;
                        wcsncpy(g_cfgUsbcanName, cfgNames[j], 63);
                        break;
                    }
                }
            }
            Out(L"\n");
        }
    }

    /* ---- 4. 结论 ---- */
    Out(L"  ══════════════════════════════════════════════════\n");
    Out(L"   结论\n");
    Out(L"  ══════════════════════════════════════════════════\n\n");

    if (g_cfgHasUsbcan) {
        Out(L"  ✓ 能用桥（推荐路径：按它自带的设备配置顶替）\n\n");
        Out(L"     它的 dll_cfg.ini 里有 %s 的映射，\n", g_cfgUsbcanName);
        Out(L"     说明选 USBCAN-I / USBCAN-II 时会加载这个文件。\n\n");
        Out(L"     做法：\n");
        Out(L"       ① 把 _prog\\UniVciBridge.dll 复制到程序目录，\n");
        Out(L"          改名成 %s（原文件先备份）\n", g_cfgUsbcanName);
        Out(L"       ② 同一目录新建 UniVciBridge.ini，内容一行：backend=vci\n");
        Out(L"       ③ 把盒子厂家的库也放进去\n\n");
        Out(L"     ★ 这条路径在 ZCANPRO 上已经真机验证通过\n");
        Out(L"       （创苯 2 路、周立功 2E-U 2 路都能打开）。\n\n");
        if (nativeOk)
            Out(L"     ★ 不过这个目录里已经有厂家自己的库了，\n"
                L"       如果菜单能直接选到你的盒子，优先用原生路径。\n\n");
    } else if (cVci && (refControlCAN || refUSBCAN)) {
        Out(L"  ✓ 能用桥\n\n");
        Out(L"     它调的是 VCI_* 接口，正是桥实现的那 14 个函数。\n");
        Out(L"     做法：把桥改名成它引用的那个 DLL，放到它旁边，再加一个 ini 和厂家的库。\n\n");
        Out(L"        ① 把 _prog\\UniVciBridge.dll 复制过去，改名成 ");
        Out(L"%s\n", refUSBCAN ? L"USBCAN.dll" : L"ControlCAN.dll");
        Out(L"        ② 同一个目录新建 UniVciBridge.ini，内容一行：backend=vci\n");
        Out(L"        ③ 把盒子厂家的库也放进去（创芯：ControlCAN.dll 2MB 那份；\n");
        Out(L"           广成：ECanVci.dll + CHUSBDLL.dll + MSVCR120.dll）\n\n");
        if (nativeOk)
            Out(L"     ★ 但注意：这个目录里已经有创芯自己的库了。\n");
        if (nativeOk)
            Out(L"       如果软件菜单里能直接选到创芯设备，优先用原生路径 —— 更可靠。\n\n");
    } else if (cZcan && refZlgcan) {
        Out(L"  ▲ 不能直接顶替，但还有一条路\n\n");
        Out(L"     它调的是 ZCAN_* 接口（zlgcan.dll），桥没有这套接口。\n");
        if (hasKcfg) {
            Out(L"     不过 zlgcan.dll 内部会按 dll_cfg.ini 加载插件 ——\n");
            Out(L"     其中 3=/4=（USBCAN-I / USBCAN-II）通常指向 kerneldlls\\usbcan.dll。\n\n");
            Out(L"     可以试：把桥改名成 kerneldlls\\usbcan.dll（原文件先备份）\n");
            Out(L"     并把 UniVciBridge.ini 和厂家库也放进 kerneldlls\\\n\n");
            Out(L"     ⚠ 这条路我没在真机上验证过，只在你这个软件的结构上推出来的。\n");
            Out(L"       先备份，不行就还原。\n\n");
        } else {
            Out(L"     这个目录里也没有 kerneldlls\\dll_cfg.ini，\n");
            Out(L"     所以连插件这条路也走不通。\n\n");
        }
        if (nativeOk)
            Out(L"     ★ 这个目录里有创芯的库 —— 先看软件菜单能不能直接选创芯设备。\n\n");
    } else if (cPcan || cKvaser || cXl) {
        Out(L"  ○ 不需要桥\n\n");
        Out(L"     它自己就支持 PCAN / Kvaser / Vector 之一，直接插对应盒子就行。\n");
        Out(L"     桥的意义只是把非周立功盒子伪装成周立功，这里用不上。\n\n");
    } else {
        Out(L"  ? 判断不出来\n\n");
        Out(L"     没在 EXE 里认出 CAN 接口名。可能是：\n");
        Out(L"       · .NET 程序，把 P/Invoke 名字混淆了\n");
        Out(L"       · 通过中间 DLL 再转发（那就得把那个中间 DLL 也拖进来查）\n");
        Out(L"       · 根本不是 CAN 软件\n\n");
        Out(L"     把它的子目录里的其它 DLL 也试一遍，或者直接看它的设备菜单有哪些选项。\n\n");
    }

    Out(L"  ── 记住一件事 ──\n");
    Out(L"  桥只实现周立功 VCI 那 14 个函数。\n");
    Out(L"  软件调的接口对得上 → 能顶替；对不上 → 顶不了。\n");
    Out(L"  另外：刷写是双向通信，桥的【接收】方向我没在真机上验证过。\n");
    Out(L"  刷写这种高风险操作，优先用软件原生支持的那条路。\n\n");

    Out(L"  按回车键退出…");
    if (_isatty(_fileno(stdin))) getchar();
    return 0;
}
