/*
 * ZcanShim.c —— 假 zlgcan.dll（周立功新 API 门面，底层接 VCI 兼容盒子）
 *
 * 目标（用户要求）：周立功「新老 API」都通用，只做 CAN 2.0，不做 FD。
 *   老 API：VCI_*  —— 由 UniVciBridge.dll 提供（已完成，真机验证过）
 *   新 API：ZCAN_* —— 由本文件提供（zlgcan.dll 的 57 个导出）
 *
 * 权威依据：周立功官方 SDK CAN_lib.zip 里的 zlgcan_x86\zlgcan.h +
 *           canframe.h + typedef.h（本地已下载解压）
 *
 * ★ 关键布局（照官方头文件抄，不能想当然）：
 *   typedef void* DEVICE_HANDLE;  typedef void* CHANNEL_HANDLE;
 *   can_frame = { canid_t can_id; BYTE can_dlc, __pad, __res0, __res1;
 *                 BYTE data[8]; }                      → 16 字节
 *   can_id 高位的标志位：
 *     CAN_EFF_FLAG 0x80000000  扩展帧
 *     CAN_RTR_FLAG 0x40000000  远程帧
 *     CAN_ERR_FLAG 0x20000000  错误帧
 *     CAN_SFF_MASK 0x000007FF / CAN_EFF_MASK 0x1FFFFFFF
 *   ZCAN_Transmit_Data = can_frame(16) + UINT transmit_type(4)   → 20
 *   ZCAN_Receive_Data  = can_frame(16) + UINT64 timestamp(8)     → 24
 *   ZCAN_CHANNEL_INIT_CONFIG = UINT can_type(4) + union(16)      → 20
 *     其中 can 分支 = { acc_code, acc_mask, reserved, filter, timing0, timing1, mode }
 *   FUNC_CALL = __stdcall
 *
 * ★ 只做 CAN 2.0：
 *   FD / LIN / UDS / ZCLOUD 全部导出成桩函数返回失败，
 *   这样静态导入它们的程序仍能加载，只是相关功能用不了。
 */
#include <windows.h>
#include <stdio.h>

typedef unsigned char      BYTE;
typedef unsigned int       UINT;
typedef unsigned long      ULONG;
typedef unsigned long long UINT64;
typedef unsigned short     USHORT;
typedef unsigned char      UCHAR;

typedef void *DEVICE_HANDLE;
typedef void *CHANNEL_HANDLE;
typedef UINT  canid_t;

#define CAN_EFF_FLAG 0x80000000U
#define CAN_RTR_FLAG 0x40000000U
#define CAN_ERR_FLAG 0x20000000U
#define CAN_SFF_MASK 0x000007FFU
#define CAN_EFF_MASK 0x1FFFFFFFU
#define CAN_MAX_DLEN 8

#define TYPE_CAN    0
#define TYPE_CANFD  1

typedef struct {
    canid_t can_id;
    BYTE    can_dlc;
    BYTE    __pad;
    BYTE    __res0;
    BYTE    __res1;
    BYTE    data[CAN_MAX_DLEN];
} can_frame;                              /* 16 字节 */

typedef struct {
    UINT can_type;
    union {
        struct {
            UINT acc_code;
            UINT acc_mask;
            UINT reserved;
            BYTE filter;
            BYTE timing0;
            BYTE timing1;
            BYTE mode;
        } can;
        struct {
            UINT acc_code, acc_mask, abit_timing, dbit_timing, brp;
            BYTE filter, mode;
            USHORT pad;
            UINT reserved;
        } canfd;
    };
} ZCAN_CHANNEL_INIT_CONFIG;               /* 20 字节 */

typedef struct {
    UINT error_code;
    BYTE passive_ErrData[3];
    BYTE arLost_ErrData;
} ZCAN_CHANNEL_ERR_INFO;

typedef struct {
    BYTE errInterrupt, regMode, regStatus, regALCapture;
    BYTE regECCapture, regEWLimit, regRECounter, regTECounter;
    UINT Reserved;
} ZCAN_CHANNEL_STATUS;

typedef struct { can_frame frame; UINT   transmit_type; } ZCAN_Transmit_Data;
typedef struct { can_frame frame; UINT64 timestamp;     } ZCAN_Receive_Data;

typedef struct {
    USHORT hw_Version, fw_Version, dr_Version, in_Version, irq_Num;
    BYTE   can_Num;
    UCHAR  str_Serial_Num[20];
    UCHAR  str_hw_Type[40];
    USHORT reserved[4];
} ZCAN_DEVICE_INFO;

/*==================== VCI 结构体（必须 pack(1)） ====================*/
#pragma pack(push, 1)
typedef struct {
    BYTE Mode, InitBytes;
    DWORD AccCode, AccMask, Reserved;
    BYTE Filter, Timing0, Timing1, Mode2;
} VCI_INIT_CONFIG;

typedef struct {
    DWORD ID, TimeStamp;
    BYTE  TimeFlag, SendType, RemoteFlag, ExternFlag, DataLen;
    BYTE  Data[8];
    BYTE  Reserved[3];
} VCI_CAN_OBJ;
#pragma pack(pop)

typedef DWORD (__stdcall *PF_Open)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_Close)(DWORD, DWORD);
typedef DWORD (__stdcall *PF_Init)(DWORD, DWORD, DWORD, VCI_INIT_CONFIG *);
typedef DWORD (__stdcall *PF_Start)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_Reset)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_Clear)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_Num)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_TX)(DWORD, DWORD, DWORD, VCI_CAN_OBJ *, DWORD);
typedef DWORD (__stdcall *PF_RX)(DWORD, DWORD, DWORD, VCI_CAN_OBJ *, DWORD, int);
typedef DWORD (__stdcall *PF_Ref)(DWORD, DWORD, DWORD, DWORD, void *);

static HMODULE g_h;
static PF_Open vOpen;  static PF_Close vClose; static PF_Init  vInit;
static PF_Start vStart; static PF_Reset vReset; static PF_Clear vClear;
static PF_Num vNum;    static PF_TX vTx;       static PF_RX    vRx;
static PF_Ref vRef;

static int   g_opened;
static DWORD g_devType;
static int   g_started[16];
static DWORD g_rate[16];

#define HANDLE_DEV_BASE 0x5A000000UL
#define HANDLE_CH_BASE  0x5B000000UL

static DWORD ChanOf(CHANNEL_HANDLE h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if ((v & 0xFFFF0000UL) != HANDLE_CH_BASE) return 0xFFFFFFFFUL;
    return (DWORD)(v & 0xFF);
}

static void SelfDir(wchar_t *out, DWORD cch)
{
    DWORD n = GetModuleFileNameW(NULL, out, cch);
    wchar_t *s;
    if (!n) { wcscpy(out, L"."); return; }
    s = wcsrchr(out, L'\\');
    if (s) *s = 0;
}

static void LogF(const char *fmt, ...)
{
    wchar_t dir[MAX_PATH * 2], path[MAX_PATH * 2];
    char line[1024];
    va_list ap;
    FILE *f;
    SYSTEMTIME st;

    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = 0;

    SelfDir(dir, MAX_PATH * 2);
    _snwprintf(path, MAX_PATH * 2 - 1, L"%s\\ZcanShim.log", dir);
    path[MAX_PATH * 2 - 1] = 0;
    f = _wfopen(path, L"ab");
    if (!f) return;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, line);
    fclose(f);
}

static HMODULE LoadNear(const wchar_t *dir, const wchar_t *name)
{
    wchar_t p[MAX_PATH * 2];
    HMODULE h;
    _snwprintf(p, MAX_PATH * 2 - 1, L"%s\\%ls", dir, name);
    p[MAX_PATH * 2 - 1] = 0;
    h = LoadLibraryW(p);
    if (h) return h;
    return LoadLibraryW(name);
}

static int Bind(HMODULE h)
{
    vOpen  = (PF_Open) GetProcAddress(h, "VCI_OpenDevice");
    vClose = (PF_Close)GetProcAddress(h, "VCI_CloseDevice");
    vInit  = (PF_Init) GetProcAddress(h, "VCI_InitCAN");
    vStart = (PF_Start)GetProcAddress(h, "VCI_StartCAN");
    vReset = (PF_Reset)GetProcAddress(h, "VCI_ResetCAN");
    vClear = (PF_Clear)GetProcAddress(h, "VCI_ClearBuffer");
    vNum   = (PF_Num)  GetProcAddress(h, "VCI_GetReceiveNum");
    vTx    = (PF_TX)   GetProcAddress(h, "VCI_Transmit");
    vRx    = (PF_RX)   GetProcAddress(h, "VCI_Receive");
    vRef   = (PF_Ref)  GetProcAddress(h, "VCI_SetReference");
    if (!vOpen) {
        vOpen  = (PF_Open) GetProcAddress(h, "OpenDevice");
        vClose = (PF_Close)GetProcAddress(h, "CloseDevice");
        vInit  = (PF_Init) GetProcAddress(h, "InitCAN");
        vStart = (PF_Start)GetProcAddress(h, "StartCAN");
        vReset = (PF_Reset)GetProcAddress(h, "ResetCAN");
        vClear = (PF_Clear)GetProcAddress(h, "ClearBuffer");
        vNum   = (PF_Num)  GetProcAddress(h, "GetReceiveNum");
        vTx    = (PF_TX)   GetProcAddress(h, "Transmit");
        vRx    = (PF_RX)   GetProcAddress(h, "Receive");
        vRef   = (PF_Ref)  GetProcAddress(h, "SetReference");
    }
    return (vOpen && vClose && vInit && vStart && vTx && vRx) ? 1 : 0;
}

static void EnsureBackend(void)
{
    static int tried = 0;
    static const wchar_t *cand[] = {
        L"ControlCAN.dll", L"ECanVci.dll", L"GCANVci.dll",
        L"USBCAN_E.dll", L"USBCAN_4E_U.dll", L"USBCAN_8E_U.dll",
        L"USBCAN.dll", L"ControlCAN_32.dll"
    };
    static const DWORD types[] = { 4, 3, 20, 21, 31, 34 };
    wchar_t dir[MAX_PATH * 2];
    int i, k;

    if (tried) return;
    tried = 1;
    SelfDir(dir, MAX_PATH * 2);

    for (i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
        HMODULE h = LoadNear(dir, cand[i]);
        if (!h) continue;
        if (!Bind(h)) { FreeLibrary(h); continue; }
        for (k = 0; k < (int)(sizeof(types) / sizeof(types[0])); k++) {
            if (vOpen(types[k], 0, 0) == 1) {
                g_devType = types[k];
                g_opened = 1;
                g_h = h;
                LogF("后端: %ls（型号 %lu 能打开）", cand[i], (unsigned long)types[k]);
                return;
            }
        }
        LogF("%ls 打不开任何型号", cand[i]);
        FreeLibrary(h);
    }
    LogF("没有可用的 VCI 后端");
}

/* 周立功 timing -> 速率（反向查表，供 GetReceiveNum 之外用） */
static DWORD TimingToRate(BYTE t0, BYTE t1)
{
    DWORD v = ((DWORD)t0 << 8) | t1;
    switch (v) {
    case 0x0014: return 1000000;
    case 0x0016: return 800000;
    case 0x001C: return 500000;
    case 0x011C: return 250000;
    case 0x031C: return 125000;
    case 0x432F: return 100000;
    case 0x472F: return 50000;
    case 0x532F: return 20000;
    case 0x672F: return 10000;
    case 0x7F7F: return 5000;
    }
    return 500000;
}

/*==================== CAN 2.0 核心实现 ====================*/

__declspec(dllexport) DEVICE_HANDLE __stdcall ZCAN_OpenDevice(
    UINT device_type, UINT device_index, UINT reserved)
{
    (void)device_type; (void)reserved;
    EnsureBackend();
    if (!g_opened) { LogF("ZCAN_OpenDevice: 无可用后端"); return 0; }
    LogF("ZCAN_OpenDevice(devType=%lu idx=%lu) -> OK（底层型号 %lu）",
         (unsigned long)device_type, (unsigned long)device_index,
         (unsigned long)g_devType);
    return (DEVICE_HANDLE)(ULONG_PTR)(HANDLE_DEV_BASE | (device_index & 0xFF));
}

__declspec(dllexport) UINT __stdcall ZCAN_CloseDevice(DEVICE_HANDLE dh)
{
    int i;
    (void)dh;
    for (i = 0; i < 16; i++) g_started[i] = 0;
    if (g_h && vClose && g_opened) {
        vClose(g_devType, 0);
        LogF("ZCAN_CloseDevice -> 已关闭底层设备");
    }
    g_opened = 0;
    return 1;
}

__declspec(dllexport) UINT __stdcall ZCAN_GetDeviceInf(
    DEVICE_HANDLE dh, ZCAN_DEVICE_INFO *pInfo)
{
    (void)dh;
    if (!pInfo) return 0;
    ZeroMemory(pInfo, sizeof(*pInfo));
    pInfo->hw_Version = 0x0100;
    pInfo->fw_Version = 0x0100;
    pInfo->dr_Version = 0x0100;
    pInfo->in_Version = 0x0100;
    pInfo->can_Num = 2;
    strncpy((char *)pInfo->str_Serial_Num, "BRIDGE0001", 19);
    strncpy((char *)pInfo->str_hw_Type, "USBCAN (UniVciBridge)", 39);
    return 1;
}

__declspec(dllexport) UINT __stdcall ZCAN_GetDeviceInfoEx(
    DEVICE_HANDLE dh, void *pInfo)
{
    (void)dh;
    if (pInfo) ZeroMemory(pInfo, 256);
    return 1;
}

__declspec(dllexport) UINT __stdcall ZCAN_IsDeviceOnLine(DEVICE_HANDLE dh)
{
    (void)dh;
    return g_opened ? 1 : 0;
}

__declspec(dllexport) CHANNEL_HANDLE __stdcall ZCAN_InitCAN(
    DEVICE_HANDLE dh, UINT can_index, ZCAN_CHANNEL_INIT_CONFIG *cfg)
{
    VCI_INIT_CONFIG ic;
    BYTE t0 = 0x00, t1 = 0x1C;

    if (!cfg) return 0;
    if (cfg->can_type != TYPE_CAN) {
        LogF("ZCAN_InitCAN: 只支持 CAN 2.0（can_type=%lu）", (unsigned long)cfg->can_type);
        return 0;
    }
    if (!g_opened) { LogF("ZCAN_InitCAN: 设备没打开"); return 0; }

    t0 = cfg->can.timing0;
    t1 = cfg->can.timing1;
    g_rate[can_index] = TimingToRate(t0, t1);

    if (vRef) vRef(g_devType, 0, can_index, 0, &g_rate[can_index]);

    ZeroMemory(&ic, sizeof(ic));
    ic.Mode = cfg->can.mode;
    ic.AccCode = cfg->can.acc_code;
    ic.AccMask = cfg->can.acc_mask;
    ic.Filter = cfg->can.filter;
    ic.Timing0 = t0;
    ic.Timing1 = t1;

    if (vInit(g_devType, 0, can_index, &ic) != 1) {
        LogF("ZCAN_InitCAN ch=%lu: 底层 InitCAN 失败", (unsigned long)can_index);
        return 0;
    }
    LogF("ZCAN_InitCAN ch=%lu timing=%02X/%02X -> %lu bps",
         (unsigned long)can_index, t0, t1, (unsigned long)g_rate[can_index]);
    return (CHANNEL_HANDLE)(ULONG_PTR)(HANDLE_CH_BASE | (can_index & 0xFF));
}

__declspec(dllexport) UINT __stdcall ZCAN_StartCAN(CHANNEL_HANDLE ch)
{
    DWORD c = ChanOf(ch);
    if (c == 0xFFFFFFFFUL) return 0;
    if (!g_opened) return 0;
    if (vStart(g_devType, 0, c) != 1) {
        LogF("ZCAN_StartCAN ch=%lu 失败", (unsigned long)c);
        return 0;
    }
    g_started[c] = 1;
    LogF("ZCAN_StartCAN ch=%lu -> OK", (unsigned long)c);
    return 1;
}

__declspec(dllexport) UINT __stdcall ZCAN_ResetCAN(CHANNEL_HANDLE ch)
{
    DWORD c = ChanOf(ch);
    if (c == 0xFFFFFFFFUL || !g_opened) return 0;
    if (vReset) vReset(g_devType, 0, c);
    return 1;
}

__declspec(dllexport) UINT __stdcall ZCAN_ClearBuffer(CHANNEL_HANDLE ch)
{
    DWORD c = ChanOf(ch);
    if (c == 0xFFFFFFFFUL || !g_opened) return 0;
    if (vClear) vClear(g_devType, 0, c);
    return 1;
}

__declspec(dllexport) UINT __stdcall ZCAN_ReadChannelErrInfo(
    CHANNEL_HANDLE ch, ZCAN_CHANNEL_ERR_INFO *pErr)
{
    (void)ch;
    if (!pErr) return 0;
    ZeroMemory(pErr, sizeof(*pErr));
    return 1;
}

__declspec(dllexport) UINT __stdcall ZCAN_ReadChannelStatus(
    CHANNEL_HANDLE ch, ZCAN_CHANNEL_STATUS *pSt)
{
    (void)ch;
    if (!pSt) return 0;
    ZeroMemory(pSt, sizeof(*pSt));
    return 1;
}

__declspec(dllexport) UINT __stdcall ZCAN_GetReceiveNum(CHANNEL_HANDLE ch, BYTE type)
{
    DWORD c = ChanOf(ch);
    if (c == 0xFFFFFFFFUL || !g_opened) return 0;
    if (type == TYPE_CANFD) return 0;      /* 不支持 FD */
    if (!vNum) return 0;
    return vNum(g_devType, 0, c);
}

__declspec(dllexport) UINT __stdcall ZCAN_Transmit(
    CHANNEL_HANDLE ch, ZCAN_Transmit_Data *p, UINT len)
{
    DWORD c = ChanOf(ch);
    UINT i, sent = 0;

    if (c == 0xFFFFFFFFUL || !p || !g_opened || !vTx) return 0;

    for (i = 0; i < len; i++) {
        VCI_CAN_OBJ o;
        DWORD r;
        ZeroMemory(&o, sizeof(o));
        o.ID = p[i].frame.can_id & CAN_EFF_MASK;
        o.DataLen = p[i].frame.can_dlc > 8 ? 8 : p[i].frame.can_dlc;
        if (p[i].frame.can_id & CAN_EFF_FLAG) o.ExternFlag = 1;
        if (p[i].frame.can_id & CAN_RTR_FLAG) o.RemoteFlag = 1;
        memcpy(o.Data, p[i].frame.data, 8);
        r = vTx(g_devType, 0, c, &o, 1);
        if (r == 1) sent++;
    }
    LogF("ZCAN_Transmit ch=%lu 请求 %lu 帧 -> 成功 %lu",
         (unsigned long)c, (unsigned long)len, (unsigned long)sent);
    return sent;
}

__declspec(dllexport) UINT __stdcall ZCAN_Receive(
    CHANNEL_HANDLE ch, ZCAN_Receive_Data *p, UINT len, int wait_time)
{
    DWORD c = ChanOf(ch);
    UINT got = 0;

    if (c == 0xFFFFFFFFUL || !p || !g_opened || !vRx) return 0;

    while (got < len) {
        VCI_CAN_OBJ o;
        DWORD r = vRx(g_devType, 0, c, &o, 1, (got == 0) ? wait_time : 0);
        if (r != 1) break;
        ZeroMemory(&p[got], sizeof(p[got]));
        p[got].frame.can_id = (o.ExternFlag ? (o.ID | CAN_EFF_FLAG) : o.ID);
        if (o.RemoteFlag) p[got].frame.can_id |= CAN_RTR_FLAG;
        p[got].frame.can_dlc = o.DataLen > 8 ? 8 : o.DataLen;
        memcpy(p[got].frame.data, o.Data, 8);
        p[got].timestamp = (UINT64)o.TimeStamp * 1000ULL;   /* ms -> us */
        got++;
    }
    if (got) LogF("ZCAN_Receive ch=%lu -> %lu 帧", (unsigned long)c, (unsigned long)got);
    return got;
}

__declspec(dllexport) UINT __stdcall ZCAN_SetValue(
    DEVICE_HANDLE dh, const char *path, const void *value)
{
    (void)dh; (void)value;
    LogF("ZCAN_SetValue(%s): 未实现，忽略", path ? path : "");
    return 1;
}

__declspec(dllexport) const void *__stdcall ZCAN_GetValue(
    DEVICE_HANDLE dh, const char *path)
{
    (void)dh; (void)path;
    return NULL;
}

__declspec(dllexport) UINT __stdcall ZCAN_SetReference(
    CHANNEL_HANDLE ch, UINT refType, void *data)
{
    DWORD c = ChanOf(ch);
    if (c == 0xFFFFFFFFUL) return 0;
    if (vRef && data) vRef(g_devType, 0, c, refType, data);
    return 1;
}

__declspec(dllexport) UINT __stdcall ZCAN_GetReference(
    CHANNEL_HANDLE ch, UINT refType, void *data)
{
    (void)ch; (void)refType;
    if (data) *(DWORD *)data = 0;
    return 1;
}

__declspec(dllexport) UINT __stdcall ZCAN_GetAvailableDevices(void *pInfo, UINT *pLen)
{
    (void)pInfo;
    EnsureBackend();
    if (pLen) *pLen = g_opened ? 1 : 0;
    return g_opened ? 1 : 0;
}

/*==================== 其余全部桩函数（返回失败/0） ====================*/
#define STUB_UINT(name, args)  __declspec(dllexport) UINT __stdcall name args { LogF(#name ": 未实现（只做 CAN 2.0）"); return 0; }
#define STUB_VOIDP(name, args) __declspec(dllexport) void *__stdcall name args { LogF(#name ": 未实现"); return 0; }

STUB_UINT(ZCAN_TransmitFD, (CHANNEL_HANDLE c, void *p, UINT l))
STUB_UINT(ZCAN_ReceiveFD, (CHANNEL_HANDLE c, void *p, UINT l, int w))
STUB_UINT(ZCAN_TransmitData, (DEVICE_HANDLE d, void *p, UINT l))
STUB_UINT(ZCAN_ReceiveData, (DEVICE_HANDLE d, void *p, UINT l, int w))
STUB_UINT(ZCAN_OpenDeviceByName, (void *p, void *q, UINT r))
STUB_UINT(ZCAN_GetOnlineDevices, (void *p, UINT *l))
STUB_UINT(ZCAN_SetDeviceChangeCallback, (DEVICE_HANDLE d, void *cb))
STUB_UINT(ZCAN_BaudrateScan, (CHANNEL_HANDLE c, UINT *b))
STUB_VOIDP(GetIProperty, (DEVICE_HANDLE d))
STUB_UINT(ReleaseIProperty, (void *p))

/* LIN */
STUB_UINT(ZCAN_InitLIN, (DEVICE_HANDLE d, UINT i, void *p))
STUB_UINT(ZCAN_StartLIN, (CHANNEL_HANDLE c))
STUB_UINT(ZCAN_ResetLIN, (CHANNEL_HANDLE c))
STUB_UINT(ZCAN_TransmitLIN, (CHANNEL_HANDLE c, void *p, UINT l))
STUB_UINT(ZCAN_GetLINReceiveNum, (CHANNEL_HANDLE c))
STUB_UINT(ZCAN_ReceiveLIN, (CHANNEL_HANDLE c, void *p, UINT l, int w))
STUB_UINT(ZCAN_SetLINSubscribe, (CHANNEL_HANDLE c, void *p, UINT n))
STUB_UINT(ZCAN_SetLINPublish, (CHANNEL_HANDLE c, void *p, UINT n))
STUB_UINT(ZCAN_SetLINPublishEx, (CHANNEL_HANDLE c, void *p, UINT n))
STUB_UINT(ZCAN_WakeUpLIN, (CHANNEL_HANDLE c))
STUB_UINT(ZCAN_SetLINSlaveMsg, (CHANNEL_HANDLE c, void *p, UINT n))
STUB_UINT(ZCAN_ClearLINSlaveMsg, (CHANNEL_HANDLE c, BYTE *p, UINT n))
STUB_UINT(ZCAN_CreateLINSchedule, (DEVICE_HANDLE d, void *p, UINT n))
STUB_UINT(ZCAN_DestroyLINSchedule, (DEVICE_HANDLE d, void *h))
STUB_UINT(ZCAN_AddLINSchedule, (CHANNEL_HANDLE c, void *h, UINT n))
STUB_UINT(ZCAN_ClrLINSchedule, (CHANNEL_HANDLE c))
STUB_UINT(ZCAN_SetLINScheduleEnabled, (CHANNEL_HANDLE c, void *h, UINT e))
STUB_UINT(ZCAN_SetLINScheduleItemEnabled, (CHANNEL_HANDLE c, void *h, UINT i, UINT e))
STUB_UINT(ZCAN_GetLINScheduleStatus, (CHANNEL_HANDLE c, void *h, void *s))
STUB_UINT(ZCAN_StartLINSchedule, (CHANNEL_HANDLE c))
STUB_UINT(ZCAN_StopLINSchedule, (CHANNEL_HANDLE c))

/* UDS */
STUB_UINT(ZCAN_UDS_Request, (DEVICE_HANDLE d, const void *q, void *r, BYTE *b, UINT n))
STUB_UINT(ZCAN_UDS_Control, (DEVICE_HANDLE d, const void *q, void *r))
STUB_UINT(ZCAN_UDS_RequestEX, (DEVICE_HANDLE d, const void *q, void *r, BYTE *b, UINT n))
STUB_UINT(ZCAN_UDS_ControlEX, (DEVICE_HANDLE d, UINT t, const void *q, void *r))
STUB_UINT(ZCAN_UDS_SetTesterPresent, (DEVICE_HANDLE d, const void *p))

/* ZCLOUD */
STUB_UINT(ZCLOUD_SetServerInfo, (const char *a, USHORT b, const char *c, USHORT d))
STUB_UINT(ZCLOUD_ConnectServer, (const char *u, const char *p))
STUB_UINT(ZCLOUD_IsConnected, (void))
STUB_UINT(ZCLOUD_DisconnectServer, (void))
STUB_VOIDP(ZCLOUD_GetUserData, (int u))
STUB_UINT(ZCLOUD_ReceiveGPS, (DEVICE_HANDLE d, void *p, UINT l, int w))

BOOL WINAPI DllMain(HANDLE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_DETACH && g_h && vClose && g_opened) {
        vClose(g_devType, 0);
    }
    return TRUE;
}
