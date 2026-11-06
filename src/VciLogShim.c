/*
 * VciLogShim.c —— 塞在 ZCANPRO 和真周立功 DLL 之间的"日志门面"
 *
 *   ZCANPRO → 本 DLL（记录 + 转发）→ zlg.dll（真周立功）
 *
 * 为什么要它：一个 CAN 盒子同时只能被一个进程打开，所以"另开探针在旁边看"
 * 做不到（探针 OpenDevice 必然返回 0）。把日志做在中间，就能在【正常操作
 * ZCANPRO】的同时，看到每一次调用的参数和返回值。
 *
 * 装法：
 *   1. 真周立功 USBCAN.dll 改名成 zlg.dll
 *   2. 本 DLL 改名成 USBCAN.dll，放进 ZCANPRO 目录
 *   3. 正常用 ZCANPRO，随便拔插线
 *   4. 看同目录 VciLogShim.log
 *
 * 记录策略（避免日志爆炸）：
 *   ReadErrInfo / ReadCANStatus —— 只在返回值或内容变化时记，带 16 字节 raw
 *   InitCAN / StartCAN / Open / Close —— 每次记
 *   其余 —— 只记失败
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef DWORD (__stdcall *PF3)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF2)(DWORD, DWORD);
typedef DWORD (__stdcall *PF3p)(DWORD, DWORD, void *);
typedef DWORD (__stdcall *PF4p)(DWORD, DWORD, DWORD, void *);
typedef DWORD (__stdcall *PF5p)(DWORD, DWORD, DWORD, DWORD, void *);
typedef DWORD (__stdcall *PF_tx)(DWORD, DWORD, DWORD, void *, DWORD);
typedef DWORD (__stdcall *PF_rx)(DWORD, DWORD, DWORD, void *, DWORD, int);

static HMODULE g_h = NULL;
static CRITICAL_SECTION g_cs;
static int g_csOk = 0;
static FILE *g_f = NULL;

static PF3   pOpen, pStart, pReset, pClear, pNum;
static PF2   pClose;
static PF3p  pBoard;
static PF4p  pInit;
static PF4p  pErr, pStatus;
static PF5p  pSetRef, pGetRef;
static PF_tx pTx;
static PF_rx pRx;

static void LogOpen(void)
{
    char p[MAX_PATH];
    HMODULE hm = NULL;
    if (!g_csOk) { InitializeCriticalSection(&g_cs); g_csOk = 1; }
    if (g_f) return;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&LogOpen, &hm)) {
        DWORD n = GetModuleFileNameA(hm, p, MAX_PATH - 32);
        if (n > 0) {
            char *s = strrchr(p, '\\');
            if (s) { *s = 0; strcat(p, "\\VciLogShim.log"); }
            g_f = fopen(p, "a");
        }
    }
    if (g_f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(g_f, "\n===== VciLogShim loaded %02d:%02d:%02d =====\n",
                st.wHour, st.wMinute, st.wSecond);
        fflush(g_f);
    }
}

static void L(const char *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&g_cs);
    LogOpen();
    if (g_f) {
        va_start(ap, fmt);
        vfprintf(g_f, fmt, ap);
        va_end(ap);
        fprintf(g_f, "\n");
        fflush(g_f);
    }
    LeaveCriticalSection(&g_cs);
}

static int Resolve(void)
{
    if (g_h) return 1;
    EnterCriticalSection(&g_cs);
    if (!g_h) {
        g_h = LoadLibraryA("zlg.dll");
        if (g_h) {
            pOpen  = (PF3)  GetProcAddress(g_h, "VCI_OpenDevice");
            pClose = (PF2)  GetProcAddress(g_h, "VCI_CloseDevice");
            pInit  = (PF4p) GetProcAddress(g_h, "VCI_InitCAN");
            pStart = (PF3)  GetProcAddress(g_h, "VCI_StartCAN");
            pReset = (PF3)  GetProcAddress(g_h, "VCI_ResetCAN");
            pClear = (PF3)  GetProcAddress(g_h, "VCI_ClearBuffer");
            pNum   = (PF3)  GetProcAddress(g_h, "VCI_GetReceiveNum");
            pBoard = (PF3p) GetProcAddress(g_h, "VCI_ReadBoardInfo");
            pErr   = (PF4p) GetProcAddress(g_h, "VCI_ReadErrInfo");
            pStatus= (PF4p) GetProcAddress(g_h, "VCI_ReadCANStatus");
            pSetRef= (PF5p) GetProcAddress(g_h, "VCI_SetReference");
            pGetRef= (PF5p) GetProcAddress(g_h, "VCI_GetReference");
            pTx    = (PF_tx)GetProcAddress(g_h, "VCI_Transmit");
            pRx    = (PF_rx)GetProcAddress(g_h, "VCI_Receive");
            L("[ok] zlg.dll loaded, exports resolved");
        } else {
            L("[x] zlg.dll NOT found (err=%lu)", GetLastError());
        }
    }
    LeaveCriticalSection(&g_cs);
    return g_h != NULL;
}

#define BEGIN() if (!Resolve()) return 0

__declspec(dllexport) DWORD __stdcall VCI_OpenDevice(DWORD t, DWORD i, DWORD r)
{
    DWORD v; BEGIN();
    v = pOpen ? pOpen(t, i, r) : 0;
    L("OpenDevice(type=%lu) -> %lu", (unsigned long)t, (unsigned long)v);
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_CloseDevice(DWORD t, DWORD i)
{
    DWORD v; BEGIN();
    v = pClose ? pClose(t, i) : 0;
    L("CloseDevice(type=%lu) -> %lu", (unsigned long)t, (unsigned long)v);
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_InitCAN(DWORD t, DWORD i, DWORD c, void *p)
{
    DWORD v; unsigned char *b = (unsigned char *)p;
    BEGIN();
    if (b) L("InitCAN(ch=%lu) raw16: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             (unsigned long)c, b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
             b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
    v = pInit ? pInit(t, i, c, p) : 0;
    L("InitCAN(ch=%lu) -> %lu", (unsigned long)c, (unsigned long)v);
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_StartCAN(DWORD t, DWORD i, DWORD c)
{
    DWORD v; BEGIN();
    v = pStart ? pStart(t, i, c) : 0;
    L("StartCAN(ch=%lu) -> %lu", (unsigned long)c, (unsigned long)v);
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_ResetCAN(DWORD t, DWORD i, DWORD c)
{
    DWORD v; BEGIN();
    v = pReset ? pReset(t, i, c) : 0;
    if (!v) L("ResetCAN(ch=%lu) -> %lu", (unsigned long)c, (unsigned long)v);
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_ClearBuffer(DWORD t, DWORD i, DWORD c)
{
    DWORD v; BEGIN();
    v = pClear ? pClear(t, i, c) : 0;
    if (!v) L("ClearBuffer(ch=%lu) -> %lu", (unsigned long)c, (unsigned long)v);
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_GetReceiveNum(DWORD t, DWORD i, DWORD c)
{
    BEGIN();
    return pNum ? pNum(t, i, c) : 0;
}

__declspec(dllexport) DWORD __stdcall VCI_ReadBoardInfo(DWORD t, DWORD i, void *p)
{
    DWORD v; BEGIN();
    v = pBoard ? pBoard(t, i, p) : 0;
    if (!v) L("ReadBoardInfo -> %lu", (unsigned long)v);
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_ReadErrInfo(DWORD t, DWORD i, DWORD c, void *p)
{
    static DWORD last[8];
    static int inited = 0;
    DWORD v, sig = 0;
    unsigned char *b = (unsigned char *)p;
    int k;
    BEGIN();
    if (p) { unsigned char *z = (unsigned char *)p; for (k = 0; k < 16; k++) z[k] = 0; }
    v = pErr ? pErr(t, i, c, p) : 0;
    if (!inited) { for (k = 0; k < 8; k++) last[k] = 0xDEADBEEF; inited = 1; }
    if (b) { for (k = 0; k < 16; k++) sig = (sig * 131) + b[k]; }
    sig += v;
    if (c < 8 && last[c] != sig) {
        last[c] = sig;
        if (b)
            L("ReadErrInfo(ch=%lu) -> %lu  raw16: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
              "%02X %02X %02X %02X %02X %02X",
              (unsigned long)c, (unsigned long)v,
              b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
              b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
        else
            L("ReadErrInfo(ch=%lu) -> %lu (null)", (unsigned long)c, (unsigned long)v);
    }
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_ReadCANStatus(DWORD t, DWORD i, DWORD c, void *p)
{
    static DWORD last[8];
    static int inited = 0;
    DWORD v;
    unsigned int *q = (unsigned int *)p;
    BEGIN();
    if (p) ZeroMemory(p, 32);
    v = pStatus ? pStatus(t, i, c, p) : 0;
    if (!inited) { int k; for (k = 0; k < 8; k++) last[k] = 0xDEADBEEF; inited = 1; }
    if (q && c < 8) {
        DWORD sig = q[0] + q[1] * 7 + q[2] * 13 + q[5] * 17 + v;
        if (last[c] != sig) {
            last[c] = sig;
            L("ReadCANStatus(ch=%lu) -> %lu  Baud=%u Tseg1=%u Tseg2=%u SJW=%u SAM=%u Out=%u",
              (unsigned long)c, (unsigned long)v,
              q[0], q[1], q[2], q[3], q[4], q[5]);
        }
    }
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_SetReference(DWORD t, DWORD i, DWORD c, DWORD ref, void *p)
{
    DWORD v; BEGIN();
    v = pSetRef ? pSetRef(t, i, c, ref, p) : 0;
    if (!v) L("SetReference(ch=%lu ref=%lu) -> %lu", (unsigned long)c, (unsigned long)ref, (unsigned long)v);
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_GetReference(DWORD t, DWORD i, DWORD c, DWORD ref, void *p)
{
    DWORD v; BEGIN();
    v = pGetRef ? pGetRef(t, i, c, ref, p) : 0;
    if (!v) L("GetReference(ch=%lu ref=%lu) -> %lu", (unsigned long)c, (unsigned long)ref, (unsigned long)v);
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_Transmit(DWORD t, DWORD i, DWORD c, void *p, DWORD len)
{
    DWORD v; BEGIN();
    v = pTx ? pTx(t, i, c, p, len) : 0;
    if (!v) L("Transmit(ch=%lu len=%lu) -> %lu", (unsigned long)c, (unsigned long)len, (unsigned long)v);
    return v;
}

__declspec(dllexport) DWORD __stdcall VCI_Receive(DWORD t, DWORD i, DWORD c, void *p, DWORD len, int wt)
{
    BEGIN();
    return pRx ? pRx(t, i, c, p, len, wt) : 0;
}
