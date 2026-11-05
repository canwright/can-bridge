/*
 * PcanShim.c —— 假 PCANBasic.dll（PCAN 门面，底层接 VCI 兼容盒子）
 *
 * 为什么做这个：
 *   桥的第一半是「假 USBCAN.dll / ControlCAN.dll」，让只认周立功 VCI 的
 *   软件能用创芯/广成盒子。
 *   这一半是反过来的：PEAK 官方的 PCAN-CCP / PCAN-UDS / PCAN-ISO-TP /
 *   PCAN-XCP / PCAN-PassThru 这些 API 全都建立在 PCAN-Basic 之上
 *   （PEAK 官网原文：「PCAN-CCP 利用编程接口 PCAN-Basic」）。
 *   所以在它们旁边放一个假的 PCANBasic.dll，它们就会以为插的是 PEAK，
 *   实际打到创芯/广成盒子上。
 *
 * 实现依据：
 *   全部原型、结构体、常量取自 PEAK 官方
 *   C:\...\PCAN-Basic-v501\Include\PCANBasic.h（17 个导出函数）
 *
 * ★ 注意 ABI 细节（踩过的坑，别再犯）：
 *   1. TPCANMsg 必须是 16 字节 —— 不能加 #pragma pack(1)。
 *      pack(1) 会变成 14 字节，PCAN-Basic 写 16 字节会把栈写坏，
 *      表现为 0xC0000409（栈溢出）。
 *   2. CAN_Read 是 3 个参数（Channel, TPCANMsg*, TPCANTimestamp*）。
 *   3. TPCANTimestamp 是 3 个 WORD：millis / millis_overflow / micros。
 *   4. 所有导出都是 __stdcall，名字不能带 @N 装饰 —— 用 .def 文件控制。
 */
#include <windows.h>
#include <stdio.h>

/*==================== PCAN 基础类型 ====================*/
typedef DWORD  TPCANStatus;
typedef WORD   TPCANHandle;
typedef DWORD  TPCANBaudrate;
typedef DWORD  TPCANParameter;
typedef DWORD  TPCANType;
typedef DWORD  TPCANMode;
typedef char  *TPCANBitrateFD;
typedef char  *TPCANBitrateXL;

#define PCAN_NONEBUS            0x00U
#define PCAN_USBBUS1            0x51U

/* 状态码（取自 PCANBasic.h） */
#define PCAN_ERROR_OK           0x00000U
#define PCAN_ERROR_XMTFULL      0x00001U
#define PCAN_ERROR_OVERRUN      0x00002U
#define PCAN_ERROR_BUSLIGHT     0x00004U
#define PCAN_ERROR_BUSHEAVY     0x00008U
#define PCAN_ERROR_BUSOFF       0x00010U
#define PCAN_ERROR_ANYBUSERR    (PCAN_ERROR_BUSLIGHT | PCAN_ERROR_BUSHEAVY | PCAN_ERROR_BUSOFF)
#define PCAN_ERROR_QRCVEMPTY    0x00020U
#define PCAN_ERROR_QOVR         0x00040U
#define PCAN_ERROR_NODRIVER     0x00200U
#define PCAN_ERROR_HWINUSE      0x00400U
#define PCAN_ERROR_ILLHW        0x01400U
#define PCAN_ERROR_ILLPARAMVAL  0x08000U
#define PCAN_ERROR_ILLOPERATION 0x10000U
#define PCAN_ERROR_INITIALIZE   0x4000000U

/* 消息类型位 */
#define PCAN_MESSAGE_STANDARD   0x00U
#define PCAN_MESSAGE_RTR        0x01U
#define PCAN_MESSAGE_EXTENDED   0x02U
#define PCAN_MESSAGE_ECHO       0x20U

/* 常用参数（取自 PCANBasic.h） */
#define PCAN_DEVICE_NUMBER            0x01U
#define PCAN_5VOLTS_POWER             0x02U
#define PCAN_RECEIVE_EVENT            0x03U
#define PCAN_MESSAGE_FILTER           0x04U
#define PCAN_API_VERSION              0x05U
#define PCAN_CHANNEL_VERSION          0x06U
#define PCAN_BUSOFF_AUTORESET         0x07U
#define PCAN_LISTEN_ONLY              0x08U
#define PCAN_CHANNEL_CONDITION        0x0DU
#define PCAN_HARDWARE_NAME            0x0EU
#define PCAN_RECEIVE_STATUS           0x0FU
#define PCAN_CONTROLLER_NUMBER        0x10U
#define PCAN_CHANNEL_IDENTIFYING      0x15U
#define PCAN_CHANNEL_FEATURES         0x16U
#define PCAN_ALLOW_STATUS_FRAMES      0x1EU
#define PCAN_ALLOW_RTR_FRAMES         0x1FU
#define PCAN_ALLOW_ERROR_FRAMES       0x20U
#define PCAN_ALLOW_ECHO_FRAMES        0x2CU

/*==================== PCAN 结构体（★ 不能 pack） ====================*/
typedef struct tagTPCANMsg {
    DWORD ID;
    BYTE  MSGTYPE;
    BYTE  LEN;
    BYTE  DATA[8];
} TPCANMsg;                      /* 必须是 16 字节 */

typedef struct tagTPCANTimestamp {
    WORD millis;
    WORD millis_overflow;
    WORD micros;
} TPCANTimestamp;

typedef struct tagTPCANMsgFD {
    DWORD ID;
    BYTE  MSGTYPE;
    BYTE  DLC;
    BYTE  DATA[64];
} TPCANMsgFD;

/*==================== VCI 结构体（★ 必须 pack(1)） ====================*/
#pragma pack(push, 1)
typedef struct {
    BYTE  Mode;
    BYTE  InitBytes;
    DWORD AccCode;
    DWORD AccMask;
    DWORD Reserved;
    BYTE  Filter;
    BYTE  Timing0;
    BYTE  Timing1;
    BYTE  Mode2;
} VCI_INIT_CONFIG;

typedef struct {
    DWORD ID;
    DWORD TimeStamp;
    BYTE  TimeFlag;
    BYTE  SendType;
    BYTE  RemoteFlag;
    BYTE  ExternFlag;
    BYTE  DataLen;
    BYTE  Data[8];
    BYTE  Reserved[3];
} VCI_CAN_OBJ;
#pragma pack(pop)

/*==================== VCI 函数表 ====================*/
typedef DWORD (__stdcall *PF_Open)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_Close)(DWORD, DWORD);
typedef DWORD (__stdcall *PF_InitCAN)(DWORD, DWORD, DWORD, VCI_INIT_CONFIG *);
typedef DWORD (__stdcall *PF_Start)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_Reset)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_Clear)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_Num)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_TX)(DWORD, DWORD, DWORD, VCI_CAN_OBJ *, DWORD);
typedef DWORD (__stdcall *PF_RX)(DWORD, DWORD, DWORD, VCI_CAN_OBJ *, DWORD, int);
typedef DWORD (__stdcall *PF_Ref)(DWORD, DWORD, DWORD, DWORD, void *);

static HMODULE g_h;
static PF_Open   vOpen;
static PF_Close  vClose;
static PF_InitCAN vInit;
static PF_Start  vStart;
static PF_Reset  vReset;
static PF_Clear  vClear;
static PF_Num    vNum;
static PF_TX     vTx;
static PF_RX     vRx;
static PF_Ref    vRef;

static int   g_devType;          /* 协商出来的真实型号 */
static int   g_opened;
static int   g_inited[16];
static DWORD g_baud[16];
static DWORD g_filterFrom[16];
static DWORD g_filterTo[16];
static int   g_listenOnly[16];
static HANDLE g_evt[16];
static DWORD g_rxDrop[16];

#define RXBUF 2048
static VCI_CAN_OBJ g_rx[16][RXBUF];
static int g_rxN[16];

/* 日志：和桥共用同一套命名习惯，放到宿主 EXE 目录 */
static void SelfDir(wchar_t *out, DWORD cch)
{
    DWORD n = GetModuleFileNameW(NULL, out, cch);
    wchar_t *s;
    if (n == 0) { wcscpy(out, L"."); return; }
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
    _snwprintf(path, MAX_PATH * 2 - 1, L"%s\\PcanShim.log", dir);
    path[MAX_PATH * 2 - 1] = 0;

    f = _wfopen(path, L"ab");
    if (!f) return;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, line);
    fclose(f);
}

/*==================== 加载 VCI 后端 ====================*/
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

static int BindTable(HMODULE h)
{
    vOpen  = (PF_Open)  GetProcAddress(h, "VCI_OpenDevice");
    vClose = (PF_Close) GetProcAddress(h, "VCI_CloseDevice");
    vInit  = (PF_InitCAN)GetProcAddress(h, "VCI_InitCAN");
    vStart = (PF_Start) GetProcAddress(h, "VCI_StartCAN");
    vReset = (PF_Reset) GetProcAddress(h, "VCI_ResetCAN");
    vClear = (PF_Clear) GetProcAddress(h, "VCI_ClearBuffer");
    vNum   = (PF_Num)   GetProcAddress(h, "VCI_GetReceiveNum");
    vTx    = (PF_TX)    GetProcAddress(h, "VCI_Transmit");
    vRx    = (PF_RX)    GetProcAddress(h, "VCI_Receive");
    vRef   = (PF_Ref)   GetProcAddress(h, "VCI_SetReference");
    if (!vOpen) {   /* 广成 ECanVci 不带前缀 */
        vOpen  = (PF_Open)  GetProcAddress(h, "OpenDevice");
        vClose = (PF_Close) GetProcAddress(h, "CloseDevice");
        vInit  = (PF_InitCAN)GetProcAddress(h, "InitCAN");
        vStart = (PF_Start) GetProcAddress(h, "StartCAN");
        vReset = (PF_Reset) GetProcAddress(h, "ResetCAN");
        vClear = (PF_Clear) GetProcAddress(h, "ClearBuffer");
        vNum   = (PF_Num)   GetProcAddress(h, "GetReceiveNum");
        vTx    = (PF_TX)    GetProcAddress(h, "Transmit");
        vRx    = (PF_RX)    GetProcAddress(h, "Receive");
        vRef   = (PF_Ref)   GetProcAddress(h, "SetReference");
    }
    return (vOpen && vClose && vInit && vStart && vTx && vRx) ? 1 : 0;
}

/* 启动时找一个能真的打开设备的 VCI 库 */
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
        if (!BindTable(h)) { FreeLibrary(h); continue; }
        for (k = 0; k < (int)(sizeof(types) / sizeof(types[0])); k++) {
            if (vOpen(types[k], 0, 0) == 1) {
                g_devType = (int)types[k];
                g_opened = 1;
                g_h = h;
                LogF("后端: %ls（型号 %d 能打开）", cand[i], (int)types[k]);
                return;
            }
        }
        LogF("%ls 打不开任何型号", cand[i]);
        FreeLibrary(h);
    }
    LogF("没有可用的 VCI 后端");
}

/* PCAN 波特率常量 -> 速率 */
static DWORD PcanBaudToRate(DWORD b)
{
    switch (b) {
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

/* 速率 -> ZLG 的 Timing0/Timing1 */
static void RateToZlg(DWORD rate, BYTE *t0, BYTE *t1)
{
    switch (rate) {
    case 1000000: *t0 = 0x00; *t1 = 0x14; break;
    case  800000: *t0 = 0x00; *t1 = 0x16; break;
    case  500000: *t0 = 0x00; *t1 = 0x1C; break;
    case  250000: *t0 = 0x01; *t1 = 0x1C; break;
    case  125000: *t0 = 0x03; *t1 = 0x1C; break;
    case  100000: *t0 = 0x43; *t1 = 0x2F; break;
    case   50000: *t0 = 0x47; *t1 = 0x2F; break;
    case   20000: *t0 = 0x53; *t1 = 0x2F; break;
    case   10000: *t0 = 0x67; *t1 = 0x2F; break;
    case    5000: *t0 = 0x7F; *t1 = 0x7F; break;
    default:      *t0 = 0x00; *t1 = 0x1C; break;
    }
}

static int ChanOf(TPCANHandle h)
{
    int c = (int)h - (int)PCAN_USBBUS1;
    if (c < 0 || c > 15) return -1;
    return c;
}

/*==================== 导出：17 个 ====================*/

__declspec(dllexport) TPCANStatus __stdcall CAN_Initialize(
    TPCANHandle Channel, TPCANBaudrate Btr0Btr1,
    BYTE deprecated1, DWORD deprecated2, WORD deprecated3)
{
    int ch;
    DWORD rate;
    BYTE t0, t1;
    VCI_INIT_CONFIG cfg;

    (void)deprecated1; (void)deprecated2; (void)deprecated3;

    ch = ChanOf(Channel);
    if (ch < 0) return PCAN_ERROR_ILLHW;

    EnsureBackend();
    if (!g_opened) { LogF("CAN_Initialize: 没有可用后端"); return PCAN_ERROR_NODRIVER; }

    rate = PcanBaudToRate(Btr0Btr1);
    RateToZlg(rate, &t0, &t1);

    if (vRef) vRef(g_devType, 0, 0, 0, &rate);

    ZeroMemory(&cfg, sizeof(cfg));
    cfg.Mode = g_listenOnly[ch] ? 1 : 0;
    cfg.Filter = 1;
    cfg.AccMask = 0xFFFFFFFF;
    cfg.Timing0 = t0;
    cfg.Timing1 = t1;

    if (vInit(g_devType, 0, (DWORD)ch, &cfg) != 1) {
        LogF("CAN_Initialize ch=%d rate=%lu: VCI_InitCAN 失败", ch, (unsigned long)rate);
        return PCAN_ERROR_ILLHW;
    }
    if (vStart(g_devType, 0, (DWORD)ch) != 1) {
        LogF("CAN_Initialize ch=%d: VCI_StartCAN 失败", ch);
        return PCAN_ERROR_ILLHW;
    }

    g_inited[ch] = 1;
    g_baud[ch] = rate;
    g_filterFrom[ch] = 0;
    g_filterTo[ch] = 0xFFFFFFFF;
    g_rxN[ch] = 0;
    g_rxDrop[ch] = 0;
    if (vClear) vClear(g_devType, 0, (DWORD)ch);

    LogF("CAN_Initialize ch=%d -> OK（%lu bps）", ch, (unsigned long)rate);
    return PCAN_ERROR_OK;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_InitializeFD(
    TPCANHandle Channel, TPCANBitrateFD BitrateFD)
{
    (void)Channel; (void)BitrateFD;
    LogF("CAN_InitializeFD: 不支持（底层盒子只有 CAN 2.0）");
    return PCAN_ERROR_ILLOPERATION;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_InitializeXL(
    TPCANHandle Channel, TPCANBitrateXL BitrateXL)
{
    (void)Channel; (void)BitrateXL;
    return PCAN_ERROR_ILLOPERATION;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_Uninitialize(TPCANHandle Channel)
{
    int ch = ChanOf(Channel);
    if (Channel == PCAN_NONEBUS) {
        int i;
        for (i = 0; i < 16; i++) g_inited[i] = 0;
        LogF("CAN_Uninitialize: 全部");
        return PCAN_ERROR_OK;
    }
    if (ch < 0) return PCAN_ERROR_ILLHW;
    g_inited[ch] = 0;
    g_rxN[ch] = 0;
    LogF("CAN_Uninitialize ch=%d", ch);
    return PCAN_ERROR_OK;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_Reset(TPCANHandle Channel)
{
    int ch = ChanOf(Channel);
    if (ch < 0) return PCAN_ERROR_ILLHW;
    EnsureBackend();
    if (vClear) vClear(g_devType, 0, (DWORD)ch);
    g_rxN[ch] = 0;
    return PCAN_ERROR_OK;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_GetStatus(TPCANHandle Channel)
{
    int ch = ChanOf(Channel);
    if (ch < 0) return PCAN_ERROR_ILLHW;
    if (!g_inited[ch]) return PCAN_ERROR_INITIALIZE;
    return PCAN_ERROR_OK;
}

/* 把底层队列里的帧抽到本地环形缓冲 */
static int Pump(int ch)
{
    int n = 0;
    if (!vRx || !g_opened) return 0;
    while (n < 64) {
        VCI_CAN_OBJ o;
        DWORD r;
        if (g_rxN[ch] >= RXBUF) { g_rxDrop[ch]++; break; }
        r = vRx(g_devType, 0, (DWORD)ch, &o, 1, 0);
        if (r != 1) break;
        g_rx[ch][g_rxN[ch]++] = o;
        n++;
    }
    return n;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_Read(
    TPCANHandle Channel, TPCANMsg *MessageBuffer, TPCANTimestamp *TimestampBuffer)
{
    int ch = ChanOf(Channel);
    VCI_CAN_OBJ o;
    int i;

    if (ch < 0 || !MessageBuffer) return PCAN_ERROR_ILLPARAMVAL;
    if (!g_inited[ch]) return PCAN_ERROR_INITIALIZE;

    if (g_rxN[ch] == 0) Pump(ch);
    if (g_rxN[ch] == 0) return PCAN_ERROR_QRCVEMPTY;

    o = g_rx[ch][0];
    for (i = 1; i < g_rxN[ch]; i++) g_rx[ch][i - 1] = g_rx[ch][i];
    g_rxN[ch]--;

    ZeroMemory(MessageBuffer, sizeof(*MessageBuffer));
    MessageBuffer->ID = o.ID;
    MessageBuffer->MSGTYPE = 0;
    if (o.ExternFlag) MessageBuffer->MSGTYPE |= PCAN_MESSAGE_EXTENDED;
    if (o.RemoteFlag) MessageBuffer->MSGTYPE |= PCAN_MESSAGE_RTR;
    MessageBuffer->LEN = o.DataLen > 8 ? 8 : o.DataLen;
    memcpy(MessageBuffer->DATA, o.Data, 8);

    if (TimestampBuffer) {
        TimestampBuffer->millis = (WORD)(o.TimeStamp & 0xFFFF);
        TimestampBuffer->millis_overflow = (WORD)((o.TimeStamp >> 16) & 0xFFFF);
        TimestampBuffer->micros = 0;
    }
    return PCAN_ERROR_OK;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_ReadFD(
    TPCANHandle Channel, TPCANMsgFD *MessageBuffer, void *TimestampBuffer)
{
    (void)Channel; (void)MessageBuffer; (void)TimestampBuffer;
    return PCAN_ERROR_ILLOPERATION;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_ReadXL(
    TPCANHandle Channel, void *MessageBuffer, void *TimestampBuffer)
{
    (void)Channel; (void)MessageBuffer; (void)TimestampBuffer;
    return PCAN_ERROR_ILLOPERATION;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_Write(
    TPCANHandle Channel, TPCANMsg *MessageBuffer)
{
    int ch = ChanOf(Channel);
    VCI_CAN_OBJ o;
    DWORD r;

    if (ch < 0 || !MessageBuffer) return PCAN_ERROR_ILLPARAMVAL;
    if (!g_inited[ch]) return PCAN_ERROR_INITIALIZE;
    EnsureBackend();
    if (!g_opened || !vTx) return PCAN_ERROR_NODRIVER;

    ZeroMemory(&o, sizeof(o));
    o.ID = MessageBuffer->ID;
    o.DataLen = MessageBuffer->LEN > 8 ? 8 : MessageBuffer->LEN;
    if (MessageBuffer->MSGTYPE & PCAN_MESSAGE_EXTENDED) o.ExternFlag = 1;
    if (MessageBuffer->MSGTYPE & PCAN_MESSAGE_RTR) o.RemoteFlag = 1;
    o.SendType = 0;
    memcpy(o.Data, MessageBuffer->DATA, 8);

    r = vTx(g_devType, 0, (DWORD)ch, &o, 1);

    static DWORD txN[16];
    txN[ch]++;
    if (txN[ch] <= 20 || (txN[ch] % 50) == 0)
        LogF("CAN_Write ch=%d ID=0x%lX len=%d -> VCI_Transmit=%lu（累计 %lu）",
             ch, (unsigned long)o.ID, (int)o.DataLen,
             (unsigned long)r, (unsigned long)txN[ch]);

    if (r == 1) return PCAN_ERROR_OK;
    /* 发送失败最常见的根因是总线无对端 ACK */
    return PCAN_ERROR_ANYBUSERR;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_WriteFD(
    TPCANHandle Channel, TPCANMsgFD *MessageBuffer)
{
    (void)Channel; (void)MessageBuffer;
    return PCAN_ERROR_ILLOPERATION;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_WriteXL(
    TPCANHandle Channel, void *MessageBuffer)
{
    (void)Channel; (void)MessageBuffer;
    return PCAN_ERROR_ILLOPERATION;
}

/*
 过滤：PCAN 的过滤是「累加」语义，而且取的是 ID 区间。
 这里只做区间判断，过滤在 CAN_Read 里执行（底层盒子统一收全）。
 */
__declspec(dllexport) TPCANStatus __stdcall CAN_FilterMessages(
    TPCANHandle Channel, DWORD FromID, DWORD ToID, TPCANMode Mode)
{
    int ch = ChanOf(Channel);
    (void)Mode;
    if (ch < 0) return PCAN_ERROR_ILLHW;
    g_filterFrom[ch] = FromID;
    g_filterTo[ch] = ToID;
    LogF("CAN_FilterMessages ch=%d 0x%lX~0x%lX",
         ch, (unsigned long)FromID, (unsigned long)ToID);
    return PCAN_ERROR_OK;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_GetValue(
    TPCANHandle Channel, TPCANParameter Parameter,
    void *Buffer, DWORD BufferLength)
{
    int ch = ChanOf(Channel);
    if (!Buffer) return PCAN_ERROR_ILLPARAMVAL;

    switch (Parameter) {
    case PCAN_CHANNEL_CONDITION:
        if (BufferLength < 4) return PCAN_ERROR_ILLPARAMVAL;
        EnsureBackend();
        /* 0x02 = 通道被本程序占用（已初始化） */
        if (ch >= 0 && g_inited[ch]) { *(DWORD *)Buffer = 0x02; return PCAN_ERROR_OK; }
        *(DWORD *)Buffer = g_opened ? 0x01 : 0x00;
        return PCAN_ERROR_OK;

    case PCAN_CHANNEL_VERSION:
        if (BufferLength < 256) return PCAN_ERROR_ILLPARAMVAL;
        strcpy((char *)Buffer, "UniVciBridge PCAN shim 1.0");
        return PCAN_ERROR_OK;

    case PCAN_API_VERSION:
        if (BufferLength < 256) return PCAN_ERROR_ILLPARAMVAL;
        strcpy((char *)Buffer, "5.0.1.1131");
        return PCAN_ERROR_OK;

    case PCAN_HARDWARE_NAME:
        if (BufferLength < 256) return PCAN_ERROR_ILLPARAMVAL;
        strcpy((char *)Buffer, "USBCAN (bridged)");
        return PCAN_ERROR_OK;

    case PCAN_DEVICE_NUMBER:
        if (BufferLength < 4) return PCAN_ERROR_ILLPARAMVAL;
        *(DWORD *)Buffer = 0;
        return PCAN_ERROR_OK;

    case PCAN_CONTROLLER_NUMBER:
        if (BufferLength < 4) return PCAN_ERROR_ILLPARAMVAL;
        *(DWORD *)Buffer = (ch >= 0) ? (DWORD)ch : 0;
        return PCAN_ERROR_OK;

    case PCAN_CHANNEL_FEATURES:
        if (BufferLength < 4) return PCAN_ERROR_ILLPARAMVAL;
        *(DWORD *)Buffer = 0;     /* 不支持 FD */
        return PCAN_ERROR_OK;

    case PCAN_RECEIVE_STATUS:
        if (BufferLength < 4) return PCAN_ERROR_ILLPARAMVAL;
        *(DWORD *)Buffer = (ch >= 0 && g_inited[ch]) ? 1 : 0;
        return PCAN_ERROR_OK;

    case PCAN_LISTEN_ONLY:
        if (BufferLength < 4) return PCAN_ERROR_ILLPARAMVAL;
        *(DWORD *)Buffer = (ch >= 0 && g_listenOnly[ch]) ? 1 : 0;
        return PCAN_ERROR_OK;

    case PCAN_BUSOFF_AUTORESET:
    case PCAN_ALLOW_ECHO_FRAMES:
    case PCAN_ALLOW_ERROR_FRAMES:
    case PCAN_ALLOW_STATUS_FRAMES:
    case PCAN_ALLOW_RTR_FRAMES:
        if (BufferLength < 4) return PCAN_ERROR_ILLPARAMVAL;
        *(DWORD *)Buffer = 0;
        return PCAN_ERROR_OK;
    }
    return PCAN_ERROR_ILLPARAMVAL;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_SetValue(
    TPCANHandle Channel, TPCANParameter Parameter,
    void *Buffer, DWORD BufferLength)
{
    int ch = ChanOf(Channel);
    DWORD v = 0;

    if (!Buffer) return PCAN_ERROR_ILLPARAMVAL;
    if (BufferLength >= 4) v = *(DWORD *)Buffer;

    switch (Parameter) {
    case PCAN_RECEIVE_EVENT:
        if (ch >= 0 && BufferLength >= sizeof(HANDLE)) {
            g_evt[ch] = *(HANDLE *)Buffer;
            LogF("CAN_SetValue ch=%d RECEIVE_EVENT=%p", ch, g_evt[ch]);
        }
        return PCAN_ERROR_OK;

    case PCAN_LISTEN_ONLY:
        if (ch >= 0) {
            g_listenOnly[ch] = v ? 1 : 0;
            LogF("CAN_SetValue ch=%d LISTEN_ONLY=%lu", ch, (unsigned long)v);
        }
        return PCAN_ERROR_OK;

    case PCAN_MESSAGE_FILTER:
        if (ch >= 0 && BufferLength >= 4) {
            g_filterFrom[ch] = 0;
            g_filterTo[ch] = 0xFFFFFFFF;
        }
        return PCAN_ERROR_OK;

    case PCAN_BUSOFF_AUTORESET:
    case PCAN_ALLOW_ECHO_FRAMES:
    case PCAN_ALLOW_ERROR_FRAMES:
    case PCAN_ALLOW_STATUS_FRAMES:
    case PCAN_ALLOW_RTR_FRAMES:
    case PCAN_CHANNEL_IDENTIFYING:
        /* 底层盒子没有对应能力，接受但不改变行为 */
        return PCAN_ERROR_OK;

    case PCAN_5VOLTS_POWER:
        return PCAN_ERROR_ILLOPERATION;   /* 底层盒子不支持 5V 控制 */
    }
    return PCAN_ERROR_ILLPARAMVAL;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_GetErrorText(
    TPCANStatus Error, WORD Language, LPSTR Buffer)
{
    (void)Language;
    if (!Buffer) return PCAN_ERROR_ILLPARAMVAL;
    switch (Error) {
    case PCAN_ERROR_OK:           strcpy(Buffer, "No error"); break;
    case PCAN_ERROR_XMTFULL:      strcpy(Buffer, "Transmit queue in use"); break;
    case PCAN_ERROR_OVERRUN:      strcpy(Buffer, "CAN controller overrun"); break;
    case PCAN_ERROR_BUSLIGHT:     strcpy(Buffer, "Bus error: error counter light"); break;
    case PCAN_ERROR_BUSHEAVY:     strcpy(Buffer, "Bus error: error counter heavy"); break;
    case PCAN_ERROR_BUSOFF:       strcpy(Buffer, "Bus error: controller is bus-off"); break;
    case PCAN_ERROR_QRCVEMPTY:    strcpy(Buffer, "Receive queue is empty"); break;
    case PCAN_ERROR_QOVR:         strcpy(Buffer, "Receive queue overrun"); break;
    case PCAN_ERROR_NODRIVER:     strcpy(Buffer, "No CAN driver found (bridged backend not available)"); break;
    case PCAN_ERROR_HWINUSE:      strcpy(Buffer, "Hardware already in use"); break;
    case PCAN_ERROR_ILLHW:        strcpy(Buffer, "Invalid hardware handle"); break;
    case PCAN_ERROR_ILLPARAMVAL:  strcpy(Buffer, "Invalid parameter value"); break;
    case PCAN_ERROR_ILLOPERATION: strcpy(Buffer, "Operation not supported by bridged hardware"); break;
    case PCAN_ERROR_INITIALIZE:   strcpy(Buffer, "Channel is not initialized"); break;
    default:                      sprintf(Buffer, "Unknown error 0x%lX", (unsigned long)Error); break;
    }
    return PCAN_ERROR_OK;
}

__declspec(dllexport) TPCANStatus __stdcall CAN_LookUpChannel(
    LPSTR Parameters, TPCANHandle *FoundChannel)
{
    (void)Parameters;
    if (!FoundChannel) return PCAN_ERROR_ILLPARAMVAL;
    EnsureBackend();
    if (!g_opened) return PCAN_ERROR_ILLHW;
    *FoundChannel = PCAN_USBBUS1;
    return PCAN_ERROR_OK;
}

/*==================== 进程退出时收尾 ====================*/
BOOL WINAPI DllMain(HANDLE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        if (g_h && vClose) {
            int i;
            for (i = 0; i < 16; i++) if (g_inited[i]) g_inited[i] = 0;
            vClose((DWORD)g_devType, 0);
            LogF("已关闭底层设备");
        }
    }
    return TRUE;
}
