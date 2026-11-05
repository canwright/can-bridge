/*============================================================================
 UniVciBridge.c —— 一个 VCI 桥，接多家 CAN 盒子（给 ZCANPRO 用）

 ★★ 集成架构（踩过坑后唯一正确的做法，别改）★★

 ZCANPRO 启动时按 kerneldlls\dll_cfg.ini 的每一条去加载对应 DLL，
 **并且要求 devices_property\<型号>.xml 存在**。所以：

     错的：往 dll_cfg.ini 新增一条（改 COUNT）→ 找不到 XML → 卡死
     对的：替换 dll_cfg.ini 里**已有型号**的驱动 DLL，COUNT 不变

  做法参考（仅思路，未使用其任何代码）：
      社区里有项目采用「替换 dll_cfg.ini 已有条目的驱动 DLL」这一思路，
      把型号 2 (PCIE9810) 换成 PeakCAN.dll、
         型号 3 (USBCAN-I) 换成 KVASERCAN.dll，
         COUNT 保持 33，devices_property 放配套 XML。

  ★ 本项目仅借鉴该【思路】，代码为独立实现。
    该项目仓库未声明任何许可证（默认保留所有权利），
    因此本项目不包含、也不会包含它的任何代码。

 ★★ 后端 ★★

     PCAN     PCANBasic.dll   CAN_Initialize / CAN_Read / CAN_Write
     GCAN     ECanVci.dll     OpenDevice / InitCAN / Transmit / Receive
     TOSUN    libTSCAN.dll    tsapp_connect / tsapp_transmit_can_async / ...
     KVASER   canlib32.dll    canOpenChannel / canWrite / canRead / ...
     VCI      任意 VCI 兼容 DLL（TOSUN ControlCAN、CANalyst 等）

 后端由 UniVciBridge.ini 的 backend= 指定，或按目录里的驱动自动探测。

 ★★ 和旧版的关键差别 ★★

 1. 通道数钳位：广成驱动会把通道数报成 50（实际 2），
    ZCANPRO 按这个数遍历 → 越界 → 闪退。这里强制钳到 1~4。
 2. VCI_SetReference(RefType=0) 真正实现波特率设置；若在 OpenDevice
    之前被调用（底层会拒绝），先缓存、Open 后补发 —— USBCAN-2E-U
    必须走这个时序。
 3. 所有导出函数都有后端失败兜底，返回失败而不是崩。
 4. DllMain 只做最轻的事（不加载后端、不读配置），loader lock 安全。

 编译（必须 32 位，ZCANPRO 是 32 位）：
     cl /O2 /LD /utf-8 UniVciBridge.c /link /DEF:UniVciBridge.def /SUBSYSTEM:WINDOWS
============================================================================*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/*
 ★ 中立模型（Chameleon Transport Model）
   —— 厂商无关的「帧 / 通道配置 / 能力 / 冒充画像」

   纯新增文件，目前只做编译验证，还没接进收发路径。
   后期要做的事都靠它：
     · VN1630A（Vector）：CAN FD 64 字节载荷 + LIN + 硬件时间戳
     · 把任意设备模拟成想要的设备：CtCapabilities + CtProfile
     · 能力不足时明确降级/拒绝，而不是"能打开但数据不对"
 */
#include "CtModel.h"

typedef unsigned char      BYTE;
typedef unsigned short     WORD;
typedef unsigned long      DWORD;
typedef void              *PVOID;
typedef int                BOOL32;

/*==========================================================================
  1. VCI 结构体（与周立功 ControlCAN.h 逐字节一致）
 ==========================================================================*/
/*
 ★ 上限跟着 ZCANPRO 里最大的那个型号走：型号 34 = USBCAN-8E-U，
   8 个通道（devices_property\usbcan-8e-u.xml 里确实有 channel_0~7）。
 ★ 设计原则（用户定下的）：
   不管实际盒子有几路，都报最大值。
   实际没有的那几路在 InitCAN 时干净地失败，
   ZCANPRO 会把它们显示为不可用 —— 至少实际有的那几路能用。
   这比「只报 2 路」更好：四通道盒子也能用上全部 4 路。
 */
#define VCI_MAX_CHANNEL 8

/* 桥的身份识别码：UniVciBridgeIdent() 返回这个值，
   用来认出「改了名的自己」，避免探测时无限递归。 */
#define UNIVCI_IDENT_MAGIC 0x55434247u   /* 'UCBG' */

#pragma pack(push, 1)

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
} VCI_CAN_OBJ;                              /* 24 字节 */

/*
 ★ VCI_BOARD_INFO —— 周立功官方布局（ControlCAN.h）

   之前这里写的是「4 个 DWORD + 20 + 40 + 4」，
   大小虽然也是 80 字节，但字段完全错位：
       官方 can_Num 是【偏移 10 的一个 BYTE】
       我们却当成【偏移 12 的一个 DWORD】

   后果：为了"修正"通道数而写 pInfo->can_Num，
        实际写进了 str_Serial_Num 开头，而真正的 can_Num 被清成 0，
        ZCANPRO 的 channelNumber_count(info.type) 对不上 → 断言崩溃。
 */
/*
 ★ 官方头文件没有 pack，按自然对齐 sizeof = 80。

   但文件开头有个【全局 pack(push,1)】（为了让 VCI_CAN_OBJ 紧凑成 24 字节），
   它会把这里也压成 79 字节。
   所以必须在结构体前临时 pop、之后 push 回来 ——
   这就是编译期断言当场抓到的那个坑。
 */
#pragma pack(pop)
typedef struct {
    USHORT hw_Version;
    USHORT fw_Version;
    USHORT dr_Version;
    USHORT in_Version;
    USHORT irq_Num;
    BYTE   can_Num;
    char   str_Serial_Num[20];
    char   str_hw_Type[40];
    USHORT Reserved[4];
} VCI_BOARD_INFO;                           /* 80 字节 */
#pragma pack(push, 1)                       /* 恢复外层紧凑对齐 */

/*
 ★ E-U 系列（20/21/31/34）用的初始化结构体：16 字节，
   没有前面的 Mode / InitBytes（和经典 USBCAN-I/II 的 18 字节版不同）。
   两者混用会让所有字段错位 2 字节 —— 滤波和时序全乱。
 */
typedef struct {
    DWORD AccCode;
    DWORD AccMask;
    DWORD Reserved;
    BYTE  Filter;
    BYTE  Timing0;
    BYTE  Timing1;
    BYTE  Mode;
} VCI_INIT_CONFIG_EU;      /* 16 字节 */

/*
 ★ VCI_INIT_CONFIG（周立功官方定义，手册 csrj-jk-V3.12 第 18 页）
   一共 16 字节，前面没有 Mode/InitBytes。

   之前我写成了 18 字节（前面多 Mode、InitBytes），
   导致读出来的 Filter / Timing0 / Timing1 / Mode 全部错位 2 字节，
   真机日志里那些 Filter=28、timing=00/88、Mode2=121 就是这么来的。
 */
#pragma pack(push, 1)
typedef struct {
    DWORD AccCode;
    DWORD AccMask;
    DWORD Reserved;
    BYTE  Filter;
    BYTE  Timing0;
    BYTE  Timing1;
    BYTE  Mode;
} VCI_INIT_CONFIG;
#pragma pack(pop)                          /* 16 字节 */

typedef struct {
    BYTE  ErrCode;
    BYTE  Passive_ErrData[3];
    BYTE  ArLost_ErrData;
} VCI_ERR_INFO;                             /* 8 字节 */

typedef struct {
    DWORD Baudrate;
    DWORD Tseg1;
    DWORD Tseg2;
    DWORD SJW;
    DWORD SAM;
    DWORD OutputMode;
} VCI_CAN_STATUS;                           /* 24 字节 */

#pragma pack(pop)

#define STATUS_OK  1
#define STATUS_ERR 0

/*==========================================================================
  2. 常量与转换表
 ==========================================================================*/
#define BE_NONE    0
#define BE_PCAN    1
#define BE_GCAN    2
#define BE_TOSUN   3
#define BE_KVASER  4
#define BE_VCI     5
#define BE_VECTOR  6

/*==========================================================================
  2. 标准模型：波特率（厂商中立）

  ★ 为什么要有这一层

    同一个物理波特率（500 kbps），各家表达方式完全不同：

      · 周立功经典 USBCAN-I/II、创芯、广成
            往 VCI_INIT_CONFIG.Timing0/Timing1 填【寄存器时序字节】
      · 周立功 E-U / 2E-U 系列（型号 20/21/31/34）
            时序字节被固件忽略，必须 VCI_SetReference(RefType=0, 常量)
            常量形如 0x060007（这个数不是 bps，也不是简单位域）
      · PEAK
            CAN_Initialize 要 BTR0BTR1 编码
      · Kvaser / Vector
            直接给 bps

    这些编码互不相同。散落在各处时，一处抄错就全线静默，
    而且完全不报错 —— 真机踩过两次：
      · 拿 16MHz 的时序表去算 36MHz 的 E-U 控制器 → 一帧收不到
      · ZLG 波特率常量表里除了 500K 全写错

    所以集中成一张表：按物理 bps 一查，各家编码都在手边。

  ★ 加新厂商 = 在这张表里加一列，不在各处塞 switch。
==========================================================================*/
typedef struct {
    DWORD rate;        /* 物理量：500000 表示 500 kbps */
    BYTE  t0, t1;      /* ZLG / 创芯 / 广成 经典 Timing 字节 */
    DWORD zlg_code;    /* ZLG E-U 系列 VCI_SetReference 常量 */
    DWORD pcan_baud;   /* PEAK BTR0BTR1 编码（高字节 BTR0，低字节 BTR1） */
} CT_BAUD;

/*
  ★ 数据来源（都可核对，不要凭公式推）

    t0/t1          周立功《CAN-bus 通用测试软件及接口函数库使用手册》
                   的经典时序表；创芯/广成的行为与之一致。
    zlg_code       同手册第 36 页，适用
                   USBCAN-E-U / USBCAN-2E-U / PCI-5010-U / PCI-5020-U。
                   ★ 0x060007 = 500K 有官方示例直接佐证。
    pcan_baud      PEAK 官方 PCANBasic 波特率常量（BTR0<<8 | BTR1）。
                   ★ 目前桥里 PEAK 固定用 500K，这一列先备着。
 */
static const CT_BAUD kCtBaud[] = {
    /*    bps        t0     t1      zlg_code    pcan_baud  */
    { 1000000,   0x00,  0x14,   0x060003,   0x0014 },
    {  800000,   0x00,  0x16,   0x060004,   0x0016 },
    {  500000,   0x00,  0x1C,   0x060007,   0x001C },
    {  250000,   0x01,  0x1C,   0x1C0008,   0x011C },
    {  125000,   0x03,  0x1C,   0x1C0011,   0x031C },
    {  100000,   0x43,  0x2F,   0x160023,   0x432F },
    {   50000,   0x47,  0x2F,   0x1C002C,   0x472F },
    {   20000,   0x53,  0x2F,   0x1600B3,   0x532F },
    {   10000,   0x67,  0x2F,   0x1C00E0,   0x672F },
    {    5000,   0x7F,  0x7F,   0x1C01C1,   0x7F7F },
};
#define NCTBAUD ((int)(sizeof(kCtBaud) / sizeof(kCtBaud[0])))

/* 按物理 bps 查 */
static const CT_BAUD *CtBaudByRate(DWORD rate)
{
    int i;
    for (i = 0; i < NCTBAUD; i++)
        if (kCtBaud[i].rate == rate) return &kCtBaud[i];
    return NULL;
}

/* 按经典 Timing 字节查（创芯/广成/VCI 经典路径拿到的就是这两个字节） */
static const CT_BAUD *CtBaudByTiming(BYTE t0, BYTE t1)
{
    int i;
    for (i = 0; i < NCTBAUD; i++)
        if (kCtBaud[i].t0 == t0 && kCtBaud[i].t1 == t1) return &kCtBaud[i];
    return NULL;
}

/* 按 ZLG 参考常量查（VCI_SetReference 的 RefType=0 传的就是它） */
static const CT_BAUD *CtBaudByZlgCode(DWORD code)
{
    int i;
    for (i = 0; i < NCTBAUD; i++)
        if (kCtBaud[i].zlg_code == code) return &kCtBaud[i];
    return NULL;
}

/*---- 下面 4 个是旧接口，保留名字让调用处不用改，内部统一查表 ----*/

static void RateToTiming(DWORD rate, BYTE *t0, BYTE *t1)
{
    const CT_BAUD *b = CtBaudByRate(rate);
    if (!b) b = CtBaudByRate(500000);      /* 不认识的波特率按 500K（和原来一致） */
    *t0 = b->t0;
    *t1 = b->t1;
}

static DWORD TimingToRate(BYTE t0, BYTE t1)
{
    const CT_BAUD *b = CtBaudByTiming(t0, t1);
    return b ? b->rate : 500000;           /* 认不出就按 500K（和原来一致） */
}

static DWORD RateToZlgCode(DWORD rate)
{
    const CT_BAUD *b = CtBaudByRate(rate);
    return b ? b->zlg_code : 0;            /* 不认识就返回 0，不乱设 */
}

static DWORD ZlgCodeToRate(DWORD code)
{
    const CT_BAUD *b = CtBaudByZlgCode(code);
    return b ? b->rate : 0;
}

/*==========================================================================
  ★ 编译期尺寸断言

    真机踩过：VCI_INIT_CONFIG 我以为 18 字节，官方 16 字节，
    导致读出来的 Filter/Timing/Mode 全部错位 2 字节，
    据此做的判断全跑偏（而且不报任何错）。

    这里把已知布局钉死：尺寸变了直接编译失败，不用等真机。
==========================================================================*/
typedef char CT_ASSERT_INIT_CONFIG[(sizeof(VCI_INIT_CONFIG) == 16) ? 1 : -1];
typedef char CT_ASSERT_INIT_CONFIG_EU[(sizeof(VCI_INIT_CONFIG_EU) == 16) ? 1 : -1];
typedef char CT_ASSERT_CAN_OBJ[(sizeof(VCI_CAN_OBJ) == 24) ? 1 : -1];
typedef char CT_ASSERT_BOARD_INFO[(sizeof(VCI_BOARD_INFO) == 80) ? 1 : -1];

/*==========================================================================
  3. 日志
 ==========================================================================*/
static HMODULE  g_hSelf = NULL;
static CRITICAL_SECTION g_cs;
static BOOL     g_csReady = 0;

static void PutUtf8(FILE *f, const wchar_t *s)
{
    char u8[4096];
    int n;
    if (!s || !*s) return;
    n = WideCharToMultiByte(CP_UTF8, 0, s, -1, u8, sizeof(u8), NULL, NULL);
    if (n > 1) fwrite(u8, 1, (size_t)(n - 1), f);
}

static void LogLine(const char *msg)
{
    wchar_t dir[MAX_PATH * 2];
    wchar_t path[MAX_PATH * 2];
    wchar_t wmsg[2048];
    SYSTEMTIME st;
    FILE *f;

    if (!g_csReady || !msg) return;
    GetLocalTime(&st);

    dir[0] = L'\0';
    if (g_hSelf) GetModuleFileNameW(g_hSelf, dir, MAX_PATH * 2);
    if (dir[0]) {
        wchar_t *s = wcsrchr(dir, L'\\');
        if (s) *s = L'\0';
        _snwprintf(path, MAX_PATH * 2 - 1, L"%s\\UniVciBridge.log", dir);
    } else {
        wcscpy(path, L"UniVciBridge.log");
    }

    MultiByteToWideChar(CP_UTF8, 0, msg, -1, wmsg, 2048);
    wmsg[2047] = L'\0';

    {
        int needBom = (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
        f = _wfopen(path, L"ab");
        if (!f) return;
        if (needBom) {
            const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            fwrite(bom, 1, 3, f);
        }
    }
    {
        wchar_t head[64];
        _snwprintf(head, 63, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
        head[63] = L'\0';
        PutUtf8(f, head);
        PutUtf8(f, wmsg);
        fwrite("\r\n", 1, 2, f);
    }
    fclose(f);
}

static void LogF(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = '\0';
    va_end(ap);
    LogLine(buf);
}

/*==========================================================================
  4. 通用 VCI 后端表
 ==========================================================================*/
typedef DWORD (__stdcall *PF_OpenDevice)   (DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_CloseDevice)  (DWORD, DWORD);
typedef DWORD (__stdcall *PF_InitCAN)      (DWORD, DWORD, DWORD, VCI_INIT_CONFIG *);
typedef DWORD (__stdcall *PF_StartCAN)     (DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_ResetCAN)     (DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_ClearBuffer)  (DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_GetReceiveNum)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_Transmit)     (DWORD, DWORD, DWORD, VCI_CAN_OBJ *, DWORD);
typedef DWORD (__stdcall *PF_Receive)      (DWORD, DWORD, DWORD, VCI_CAN_OBJ *, DWORD, int);
typedef DWORD (__stdcall *PF_ReadBoardInfo)(DWORD, DWORD, VCI_BOARD_INFO *);
typedef DWORD (__stdcall *PF_ReadErrInfo)  (DWORD, DWORD, DWORD, VCI_ERR_INFO *);
typedef DWORD (__stdcall *PF_ReadCANStatus)(DWORD, DWORD, DWORD, VCI_CAN_STATUS *);
typedef DWORD (__stdcall *PF_GetReference)(DWORD, DWORD, DWORD, DWORD, PVOID);
typedef DWORD (__stdcall *PF_SetReference)(DWORD, DWORD, DWORD, DWORD, PVOID);

typedef struct {
    PF_OpenDevice     OpenDevice;
    PF_CloseDevice    CloseDevice;
    PF_InitCAN        InitCAN;
    PF_StartCAN       StartCAN;
    PF_ResetCAN       ResetCAN;
    PF_ClearBuffer    ClearBuffer;
    PF_GetReceiveNum  GetReceiveNum;
    PF_Transmit       Transmit;
    PF_Receive        Receive;
    PF_ReadBoardInfo  ReadBoardInfo;
    PF_ReadErrInfo    ReadErrInfo;
    PF_ReadCANStatus  ReadCANStatus;
    PF_GetReference   GetReference;
    PF_SetReference   SetReference;
} VCI_TABLE;

/*==========================================================================
  5. PCAN
 ==========================================================================*/
typedef WORD  TPCANHandle;
typedef WORD  TPCANBaudrate;
typedef BYTE  TPCANType;

/*
 ★★ TPCANMsg 绝对不能用 pack(1) ★★

   PCAN-Basic 的 TPCANMsg 是：
       DWORD ID; BYTE MSGTYPE; BYTE LEN; BYTE DATA[8];
   按默认对齐是 16 字节（4+1+1+8 = 14，尾部补齐到 4 的倍数）。

   如果用 pack(1) 变成 14 字节，而我们把这个 14 字节变量的地址
   传给 CAN_Write，PCANBasic 会往里写 16 字节 —— 多写 2 字节到栈上，
   把栈保护 cookie 覆盖掉，进程直接 0xC0000409 崩掉。

   这个 bug 在没有真 PCAN 硬件时永远测不出来 —— 因为不会走到 CAN_Write。
 */
typedef struct {
    DWORD ID;
    BYTE  MSGTYPE;
    BYTE  LEN;
    BYTE  DATA[8];
} TPCANMsg;

typedef struct {
    DWORD millis;
    WORD  millis_overflow;
    WORD  micros;
} TPCANTimestamp;

typedef DWORD (__stdcall *PF_CAN_Initialize)  (TPCANHandle, TPCANBaudrate, TPCANType, DWORD, WORD);
typedef DWORD (__stdcall *PF_CAN_Uninitialize)(TPCANHandle);
typedef DWORD (__stdcall *PF_CAN_Reset)       (TPCANHandle);
/*
 ★ CAN_Read 是 3 个参数，不是 4 个：
     TPCANStatus CAN_Read(TPCANHandle, TPCANMsg*, TPCANTimestamp*);
   多传一个参数会让 __stdcall 的退栈不一致
   （callee 只退 12 字节，调用方压了 16 字节），同样崩栈。
 */
typedef DWORD (__stdcall *PF_CAN_Read)        (TPCANHandle, TPCANMsg *, TPCANTimestamp *);
typedef DWORD (__stdcall *PF_CAN_Write)       (TPCANHandle, TPCANMsg *);
typedef DWORD (__stdcall *PF_CAN_GetValue)    (TPCANHandle, BYTE, void *, DWORD);
typedef DWORD (__stdcall *PF_CAN_SetValue)    (TPCANHandle, BYTE, void *, DWORD);

#define PCAN_USBBUS1           0x51
/* PCAN-Basic 实测：PCAN_CHANNEL_CONDITION = 0x000D
   （对着真 PCAN-USB 读到 0x2 = PCAN_CHANNEL_OCCUPIED，
     因为此时通道正被我们自己占着）。
   注意：已经不靠它判断可用性（改用 CAN_Initialize 探），
   因为 OCCUPIED 不含 AVAILABLE 位，拿 cond 做门禁会把自己误杀。 */
#define PCAN_CHANNEL_CONDITION 0x000D
#define PCAN_CHANNEL_AVAILABLE 0x01
#define PCAN_BAUD_500K         0x001C
#define PCAN_ERROR_OK          0x00000
#define PCAN_ERROR_NODRIVER    0x00200
#define PCAN_ERROR_HWINUSE     0x00400
#define PCAN_ALLOW_ECHO_FRAMES 0x0B   /* CAN_SetValue 参数：允许回显自己发的帧 */

/*==========================================================================
  6. TOSUN（libTSCAN / TSMaster）
 ==========================================================================*/
#pragma pack(push, 1)
typedef struct {
    BYTE      FIdxChn;
    BYTE      FProperties;
    BYTE      FDLC;
    BYTE      FReserved;
    int       FIdentifier;
    long long FTimeUs;
    BYTE      FData[8];
} TLIB_CAN;                                  /* 20 字节，_pack_=1 */

/* TLIBCAN 与 VCI_CAN_OBJ 布局不同，必须转换，不能直接强转 */
#pragma pack(pop)

/*
 ★ TOSUN 的函数名和签名是照 TSMaster.h 核对的，别凭印象写。
   踩过两个坑：
   1) tsapp_* 这些函数在 **TSMaster.dll** 里，不在 libTSCAN.dll 里。
      libTSCAN.dll 只有 tscan_* / tsfifo_* 那一层。
   2) tsfifo_receive_can_msgs 是 **4 个参数**
      （ACANBuffers, ps32 ACANBufferSize, s32 AIdxChn, bool AIncludeTx）。
      原来只传 3 个 —— __stdcall 由被调用方清栈，
      少一个参数会让 ESP 偏移 4 字节 → 栈损坏 → 崩 ZCANPRO。
      这和 Kvaser 那个 canSetBusParams 是同一类错误。
*/
typedef int (__stdcall *PF_ts_initialize)    (const char *);
typedef int (__stdcall *PF_ts_finalize)      (void);
typedef int (__stdcall *PF_ts_connect)       (void);
typedef int (__stdcall *PF_ts_disconnect)    (void);
typedef int (__stdcall *PF_ts_chan_count)    (int *);
typedef int (__stdcall *PF_ts_cfg_baud)      (const int, const float, const BOOL32, const BOOL32);
typedef int (__stdcall *PF_ts_tx_can)        (const TLIB_CAN *);
/* 4 个参数：buffers, size(in/out), channel, includeTx */
typedef int (__stdcall *PF_ts_rx_can)        (TLIB_CAN *, int *, const int, const BOOL32);
typedef int (__stdcall *PF_ts_clear_rx)      (const int);
/* 读接收缓冲帧数，给 VCI_GetReceiveNum 用 */
typedef int (__stdcall *PF_ts_rx_count)      (const int, int *);

/*==========================================================================
  7. Kvaser（canlib32）
 ==========================================================================*/
typedef int   Kstatus;

typedef struct {
    long  id;
    DWORD timestamp;
    BYTE  flags;
    BYTE  dlc;
    BYTE  data[8];
} KcanMsg;

/*
 ★ 标志位用 canstat.h 里的真实值，别自己编：
     canMSG_RTR = 0x0001   canMSG_STD = 0x0002   canMSG_EXT = 0x0004
   我一开始把"扩展帧"写成 0x10，那是 canMSG_NERR（错误计数激活），
   结果扩展帧标志全错。
   还有个坑：canWrite 时如果 EXT 和 STD 都不给，canlib **默认当扩展帧**，
   所以标准帧必须显式带 canMSG_STD。
*/
#define K_MSG_RTR  0x0001
#define K_MSG_STD  0x0002
#define K_MSG_EXT  0x0004
#define K_OPEN_ACCEPT_VIRTUAL 0x0020

typedef Kstatus (__stdcall *PF_canOpenChannel) (int, int);
typedef Kstatus (__stdcall *PF_canClose)       (int);
typedef Kstatus (__stdcall *PF_canBusOn)       (int);
typedef Kstatus (__stdcall *PF_canBusOff)      (int);
/* ★ canSetBusParams 是 7 个参数（最后一个是 syncmode）。
   原来我只写了 6 个 —— __stdcall 由被调用方清栈，
   少传一个参数会让 ESP 偏移 4 字节，直接栈损坏、崩 ZCANPRO。 */
typedef Kstatus (__stdcall *PF_canSetBusParams)(int, long, unsigned, unsigned,
                                                unsigned, unsigned, unsigned);
typedef Kstatus (__stdcall *PF_canWrite)       (int, long, void *, unsigned, unsigned);
/* ★ canRead 参数顺序是 (hnd, id, msg, dlc, flag, time)。
   原来我把 flag 和 time 传反了，导致时间戳里装的是标志位。 */
typedef Kstatus (__stdcall *PF_canRead)        (int, long *, void *, unsigned *,
                                                unsigned *, DWORD *);
typedef Kstatus (__stdcall *PF_canFlushRx)     (int);
typedef Kstatus (__stdcall *PF_canGetNumberOfChannels)(int *);
typedef Kstatus (__stdcall *PF_canGetErrorText)(Kstatus, char *, unsigned);

/*==========================================================================
  7b. Vector（vxlapi）
  ------------------------------------------------------------------------
  ★ 结构体布局不是猜的：用 32 位 Python ctypes 按官方 vxlapi.h 定义，
    再调用真实驱动 xlGetDriverConfig() 验证过 —— 返回 channelCount=2、
    channelMask 分别是 0x1/0x2（2 的幂），说明布局正确。
    实测尺寸：s_xl_can_msg=32, XLevent=48, XLchannelConfig=227,
              XLdriverConfig=14576, channelMask 偏移 42。
    这还有个好处：Vector 驱动自带 Virtual Channel，不接硬件也能测通收发。
 ==========================================================================*/
typedef long long XLuint64;
typedef XLuint64   XLaccess;
typedef DWORD      XLportHandle;             /* unsigned long，32 位 */
typedef DWORD      XLstatus;

#pragma pack(push, 1)
typedef struct {
    DWORD bitRate;
    BYTE  sjw, tseg1, tseg2, sam, outputMode;
    BYTE  reserved[7];
    BYTE  canOpMode;
} XLbusParamsDataCan;                        /* 16 字节 */

typedef union {
    XLbusParamsDataCan can;
    BYTE raw[28];
} XLbusParamsData;

typedef struct {
    DWORD          busType;
    XLbusParamsData data;
} XLbusParams;                               /* 32 字节 */

typedef struct {
    char  name[32];
    BYTE  hwType, hwIndex, hwChannel;
    WORD  transceiverType, transceiverState, configError;
    BYTE  channelIndex;
    XLuint64 channelMask;                    /* 偏移 42 */
    DWORD channelCapabilities, channelBusCapabilities;
    BYTE  isOnBus;
    DWORD connectedBusType;
    XLbusParams busParams;
    DWORD _doNotUse, driverVersion, interfaceVersion;
    DWORD raw_data[10];
    DWORD serialNumber, articleNumber;
    char  transceiverName[32];
    DWORD specialCabFlags, dominantTimeout;
    BYTE  dominantRecessiveDelay, recessiveDominantDelay;
    BYTE  connectionInfo, currentlyAvailableTimestamps;
    WORD  minimalSupplyVoltage, maximalSupplyVoltage;
    DWORD maximalBaudrate;
    BYTE  fpgaCoreCapabilities, specialDeviceStatus;
    WORD  channelBusActiveCapabilities, breakOffset, delimiterOffset;
    DWORD reserved[3];
} XLchannelConfig;                           /* 227 字节 */

typedef struct {
    DWORD          dllVersion;
    DWORD          channelCount;
    DWORD          reserved[10];
    XLchannelConfig channel[64];
} XLdriverConfig;                            /* 14576 字节 */

typedef struct {
    DWORD id;
    WORD  flags;
    WORD  dlc;
    XLuint64 res1;
    BYTE  data[8];
    XLuint64 res2;
} XLcanMsg;
/* XLcanMsg 必须是 32 字节：4(id)+2(flags)+2(dlc)+8(res1)+8(data)+8(res2)
   手册 5.5.1 原文，尺寸错了 xlCanTransmit 会返回 101 */
typedef char CHECK_XL_CANMSG[(sizeof(XLcanMsg) == 32) ? 1 : -1];                                  /* 32 字节 */

typedef union {
    XLcanMsg msg;
    BYTE     raw[32];
} XLtagData;

typedef struct {
    BYTE     tag;
    BYTE     chanIndex;
    WORD     transId;
    WORD     portHandle;
    BYTE     flags;
    BYTE     reserved;
    XLuint64 timeStamp;
    XLtagData tagData;
} XLevent;                                   /* 48 字节 */
#pragma pack(pop)

#define XL_SUCCESS                  0
#define XL_BUS_TYPE_CAN             1
/* 通道总线兼容性位（VxlApiModel.h 里有同名定义；桥不包含那个头，这里补一份） */
#define XL_BUS_COMPATIBLE_CAN       0x00000001
#define XL_BUS_ACTIVE_CAP_CAN       0x00010000
/* 常用错误码（数值来自 XL Driver Library Manual 18.1 节的错误码表） */
#define XL_ERR_NOT_IMPLEMENTED      117
#define XL_ERR_NO_DATA_DETECTED     143
#define XL_ERR_INIT_ACCESS_MISSING  158
#define XL_INTERFACE_VERSION_V3     3

/* ═══ 版本标记 ═══
   日志会把它打出来，用来确认「跑的到底是哪一版」。
   这个项目反复出现"测的是旧 DLL 却以为在测新版"，白排查好几轮 ——
   加这一行就是让这种事一眼可见。改功能时顺手改一下这个字符串。 */
#ifndef UNIVCI_BUILD_TAG
#define UNIVCI_BUILD_TAG            "2026-11-05-vector-chan"
#endif

#define XL_ACTIVATE_NONE            0
#define XL_ACTIVATE_RESET_CLOCK     8

#define XL_RECEIVE_MSG              1
#define XL_CHIP_STATE               4
#define XL_TRANSMIT_MSG             10

#define XL_CAN_EXT_MSG_ID           0x80000000
#define XL_CAN_MSG_FLAG_ERROR_FRAME 0x01
#define XL_CAN_MSG_FLAG_REMOTE_FRAME 0x02
#define XL_CAN_MSG_FLAG_TX_COMPLETED 0x80

typedef XLstatus (__stdcall *PF_xlOpenDriver)      (void);
typedef XLstatus (__stdcall *PF_xlCloseDriver)     (void);
typedef XLstatus (__stdcall *PF_xlGetDriverConfig) (XLdriverConfig *);
/* ★ 7 个参数，但 accessMask 是 XLaccess（8 字节），所以总栈宽是 32：
       ptr(4) + app(4) + mask(8) + permPtr(4) + rxQ(4) + ifaceVer(4) + busType(4) = 32
   与真 DLL 的导出名 _xlOpenPort@32 吻合。
   ★ 曾经把 XLaccess 误写成 unsigned int（4 字节），结果 mask 少压 4 字节，
     驱动读到垃圾高位，表现为崩溃或 perm=0。 */
typedef XLstatus (__stdcall *PF_xlOpenPort)        (XLportHandle *, const char *,
                                                    XLaccess, XLaccess *,
                                                    unsigned int, unsigned int,
                                                    unsigned int);
typedef XLstatus (__stdcall *PF_xlClosePort)       (XLportHandle);
typedef XLstatus (__stdcall *PF_xlCanSetChannelBitrate)(XLportHandle, XLaccess,
                                                        DWORD);
typedef XLstatus (__stdcall *PF_xlActivateChannel) (XLportHandle, XLaccess,
                                                    unsigned int, unsigned int);
typedef XLstatus (__stdcall *PF_xlDeactivateChannel)(XLportHandle, XLaccess);
/* 把通道分配给应用；不分配的话 xlOpenPort 只给 port 不给 perm */
typedef XLstatus (__stdcall *PF_xlSetApplConfig)   (const char *, unsigned int,
                                                    unsigned int, unsigned int,
                                                    unsigned int, unsigned int);
typedef XLstatus (__stdcall *PF_xlCanTransmit)     (XLportHandle, XLaccess,
                                                    unsigned int *, void *);
typedef XLstatus (__stdcall *PF_xlReceive)         (XLportHandle,
                                                    unsigned int *, XLevent *);
typedef XLstatus (__stdcall *PF_xlFlushReceiveQueue)(XLportHandle);
typedef XLstatus (__stdcall *PF_xlGetErrorString)  (XLstatus, char **);

/*==========================================================================
  8. 全局状态
 ==========================================================================*/
static int     g_backend = BE_NONE;
static char    g_backendName[32] = "none";
static HMODULE g_hBackend = NULL;

static VCI_TABLE g_vci;
static int     g_opened = 0;
/* 后端实际有几路通道（用来让「不存在的那几路」干净失败）*/
static int     g_realChans = 1;
/*
 ★ vci 后端协商出来的「真实型号」。

   各家 VCI 兼容库对型号编号的定义不统一（真机实测）：
     创苯 SDK ControlCAN.dll  → 认 3 / 4 / 20 / 21
     周立功 USBCAN_E.DLL     → 只认 20（对应 USBCAN-E-U）
   而 ZCANPRO 请求的是它自己的型号（USBCAN-II = 4）。
   所以第一次 OpenDevice 时要试一遍，把能开的那个记下来。
 */
static DWORD   g_vciRealType = 0;
/* vci 后端是不是已经处于「已打开」状态（探测时开的那次会保留下来） */
static int     g_vciOpened = 0;

/* 探测失败的时刻（GetTickCount），0 = 没失败过 */
static DWORD   g_probeFailTick = 0;

/*
 ★ 上层（ZCANPRO 等）请求的型号。
   探测后端时应优先用它 —— 创苯的 ControlCAN.dll 对型号 21 也返回成功，
   如果不优先用请求型号，创苯盒子会被当成 2E-U，
   走上 E-U 专用的 SetReference 设波特率路径，结果一帧收不到。
 */
static DWORD   g_reqType = 0;

/* ini 里的 pcan_echo=1 → 打开 PCAN 回声帧（自己发的帧也进接收队列） */
static int     g_pcanEcho = 0;

/*
 ★ 是否自己构造 E-U 初始化配置（默认 0 = 原样转发）

   默认关闭的原因（真机证据）：
     装了桥 → ZCANPRO 收不到报文
     卸载之后 → 能读到报文
   说明原厂路径是通的，而我在 InitCAN 里把 ZCANPRO 算好的时序换成了自己算的
   timing=00/1C。E-U 系列的时序字节很可能不等于经典 USBCAN-II 的值，
   波特率一错就是「能启动、错误帧狂刷、报文一帧解不出」。

   现在改回：桥只做转发，不擅自定义参数。
   万一某些型号确实需要，在 ini 里写 eu_layout=1 就能把旧行为开回来。
 */
static int     g_euLayout = 0;

/* 发送 / 接收 计数（用来限流写日志） */
static DWORD   g_txCount[VCI_MAX_CHANNEL] = { 0 };
static DWORD   g_rxCount[VCI_MAX_CHANNEL] = { 0 };

/*
 ★ 桥自己的 VCI 接收缓冲。

   为什么要自己缓冲（真机原因）：
     ZCANPRO 的取帧流程是「先问 GetReceiveNum 有几帧，
     返回 >0 才调 Receive 去取」。
     之前 GetReceiveNum 直接转发给厂家库，一旦厂家库返回 0
     （或语义不同），上层就永远不来取 —— 表现就是「一帧收不到」。
     创苯库连 BoardInfo.can_Num 都能返回 808470065 这种垃圾值，
     说明不能完全信它的返回值。

   现在改成：桥自己 pump 到缓冲，计数和取帧都由桥负责，
   两者必然一致（和 PCAN 分支当初的修法一致）。
 */
#define VCI_RXBUF 2048
/* ★ 接收缓冲存 CtFrame（中立模型），
   对外取帧时才转回 VCI_CAN_OBJ。
   CtFrame 约 96 字节（含 64 字节载荷，为了后期 CAN FD）。 */
static CtFrame     g_vciRx[VCI_MAX_CHANNEL][VCI_RXBUF];
static int         g_vciRxN[VCI_MAX_CHANNEL];
static DWORD       g_vciRxDrop[VCI_MAX_CHANNEL];
static int     g_started[VCI_MAX_CHANNEL];
static DWORD   g_bitrate = 500000;
static DWORD   g_canNum = 1;
static DWORD   g_devType = 3;
static int     g_pendingRef = 0;
static DWORD   g_pendingRefCode = 0;

/*
 ★ 模拟画像（对外演成什么盒子）

   读 UniVciBridge.ini：
       profile=21      对外声称 USBCAN-2E-U
       channels=2      对外声称几路（不写则按型号推）

   0 = 不模拟，保持现在的自动行为。
 */
static DWORD   g_profileType  = 0;
static int     g_profileChans = 0;

static PF_CAN_Initialize   pPcanInit = NULL;
static PF_CAN_Uninitialize pPcanUninit = NULL;
static PF_CAN_Reset        pPcanReset = NULL;
static PF_CAN_Read         pPcanRead = NULL;
static PF_CAN_Write        pPcanWrite = NULL;
static PF_CAN_GetValue     pPcanGetVal = NULL;
static PF_CAN_SetValue     pPcanSetVal = NULL;
static TPCANHandle g_pcanCh = PCAN_USBBUS1;

static PF_ts_initialize     pTsInit = NULL;
static PF_ts_finalize       pTsFin = NULL;
static PF_ts_connect        pTsConnect = NULL;
static PF_ts_disconnect    pTsDisc = NULL;
static PF_ts_chan_count     pTsChanCount = NULL;
static PF_ts_cfg_baud       pTsCfgBaud = NULL;
static PF_ts_tx_can         pTsTx = NULL;
static PF_ts_rx_can         pTsRx = NULL;
static PF_ts_clear_rx       pTsClearRx = NULL;
static PF_ts_rx_count      pTsRxCount = NULL;
static int g_tsInited = 0;

static PF_canOpenChannel  pKOpen = NULL;
static PF_canClose        pKClose = NULL;
static PF_canBusOn        pKBusOn = NULL;
static PF_canBusOff       pKBusOff = NULL;
static PF_canSetBusParams pKSetBus = NULL;
static PF_canWrite        pKWrite = NULL;
static PF_canRead         pKRead = NULL;
static PF_canFlushRx      pKFlushRx = NULL;
static PF_canGetNumberOfChannels pKChanCount = NULL;
static int g_kHandle = -1;

/* Vector */
static PF_xlOpenDriver       pXlOpen = NULL;
static PF_xlCloseDriver      pXlClose = NULL;
static PF_xlGetDriverConfig  pXlCfg = NULL;
static PF_xlOpenPort         pXlOpenPort = NULL;
static PF_xlClosePort        pXlClosePort = NULL;
static PF_xlCanSetChannelBitrate pXlSetBitrate = NULL;
static PF_xlActivateChannel  pXlActivate = NULL;
static PF_xlDeactivateChannel pXlDeactivate = NULL;
static PF_xlSetApplConfig    pXlSetAppl = NULL;

/* 每个可用 CAN 通道的硬件定位信息，xlSetApplConfig 要用。
   VN1630 有 4 个 CAN 口，逐个试，某个被占就换下一个。 */
typedef struct { XLaccess mask; unsigned int hwType, hwIndex, hwChannel; } XLCANINFO;
#define XL_MAX_CAN_INFO 32
static XLCANINFO g_xlCan[XL_MAX_CAN_INFO];
static int       g_xlCanN = 0;
static const char *g_xlAppName = "UniVciBridge";
/* 用户指定用第几路 Vector 硬件通道（1 起）。0 = 自动挑第一个有权限的 */
static int g_xlWantChan = 0;
static PF_xlCanTransmit      pXlTransmit = NULL;
static PF_xlReceive          pXlReceiveFn = NULL;
static PF_xlFlushReceiveQueue pXlFlushRx = NULL;
static PF_xlGetErrorString   pXlErrStr = NULL;

static XLportHandle g_xlPort = 0;
static XLaccess     g_xlMask = 0;
static int          g_xlDriverOpen = 0;
static int          g_xlPortOpen = 0;    /* ★ port handle 可以是 0，不能拿它当标志 */
static XLaccess     g_xlAvailMask = 0;
static int          g_xlChanCount = 0;

/*==========================================================================
  9. 工具
 ==========================================================================*/
static void SelfDir(wchar_t *out, size_t n)
{
    out[0] = L'\0';
    if (!g_hSelf) { wcscpy(out, L"."); return; }
    GetModuleFileNameW(g_hSelf, out, (DWORD)n);
    {
        wchar_t *s = wcsrchr(out, L'\\');
        if (s) *s = L'\0';
    }
}

static HMODULE LoadNear(const wchar_t *dir, const wchar_t *name)
{
    wchar_t p[MAX_PATH * 2];
    HMODULE h;
    _snwprintf(p, MAX_PATH * 2 - 1, L"%s\\%ls", dir, name);
    h = LoadLibraryW(p);
    if (h) return h;
    return LoadLibraryW(name);
}

/*
 ★ 优先用系统里装的那份 DLL（PEAK 官方驱动装到 System32/SysWOW64 的那份）。

 为什么这对「不影响厂家软件」很重要：
   PEAK 的内核驱动和 PCANBasic.dll 是配套版本。如果我们从 kerneldlls
   加载一份旧副本，而 PCAN-View 用系统那份，两边版本不一致，
   轻则行为不同，重则打不开通道。
   所以对 PEAK 一律优先系统路径，让桥用的是厂商自己装的那份。

   顺带一个好处：装了 PEAK 驱动之后，PCAN 用户**完全不用往
   kerneldlls 里复制任何文件**。
*/
static HMODULE LoadSystemFirst(const wchar_t *dir, const wchar_t *name)
{
    HMODULE h = LoadLibraryW(name);        /* 先按系统搜索顺序 */
    if (h) return h;
    {
        wchar_t p[MAX_PATH * 2];
        _snwprintf(p, MAX_PATH * 2 - 1, L"%s\\%ls", dir, name);
        return LoadLibraryW(p);            /* 再退回本地目录 */
    }
}

/* 能不能按系统搜索顺序加载到这个 DLL。
   用来判断「厂商驱动装在系统目录」这种情况（PEAK/Kvaser 都是）。 */
static int DllLoadable(const wchar_t *name)
{
    HMODULE h = LoadLibraryW(name);
    if (!h) return 0;
    FreeLibrary(h);
    return 1;
}

static int FileNear(const wchar_t *dir, const wchar_t *name)
{
    wchar_t p[MAX_PATH * 2];
    _snwprintf(p, MAX_PATH * 2 - 1, L"%s\\%ls", dir, name);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static int LoadVciTable(HMODULE h, VCI_TABLE *t)
{
    if (!h || !t) return 0;
    ZeroMemory(t, sizeof(*t));
    t->OpenDevice     = (PF_OpenDevice)   GetProcAddress(h, "VCI_OpenDevice");
    t->CloseDevice    = (PF_CloseDevice)  GetProcAddress(h, "VCI_CloseDevice");
    t->InitCAN        = (PF_InitCAN)      GetProcAddress(h, "VCI_InitCAN");
    t->StartCAN       = (PF_StartCAN)     GetProcAddress(h, "VCI_StartCAN");
    t->ResetCAN       = (PF_ResetCAN)     GetProcAddress(h, "VCI_ResetCAN");
    t->ClearBuffer    = (PF_ClearBuffer)  GetProcAddress(h, "VCI_ClearBuffer");
    t->GetReceiveNum  = (PF_GetReceiveNum)GetProcAddress(h, "VCI_GetReceiveNum");
    t->Transmit       = (PF_Transmit)     GetProcAddress(h, "VCI_Transmit");
    t->Receive        = (PF_Receive)      GetProcAddress(h, "VCI_Receive");
    t->ReadBoardInfo  = (PF_ReadBoardInfo)GetProcAddress(h, "VCI_ReadBoardInfo");
    t->ReadErrInfo    = (PF_ReadErrInfo)  GetProcAddress(h, "VCI_ReadErrInfo");
    t->ReadCANStatus  = (PF_ReadCANStatus)GetProcAddress(h, "VCI_ReadCANStatus");
    t->GetReference   = (PF_GetReference) GetProcAddress(h, "VCI_GetReference");
    t->SetReference   = (PF_SetReference) GetProcAddress(h, "VCI_SetReference");

    /* 广成 ECanVci 的导出不带 VCI_ 前缀 */
    if (!t->OpenDevice) {
        t->OpenDevice     = (PF_OpenDevice)   GetProcAddress(h, "OpenDevice");
        t->CloseDevice    = (PF_CloseDevice)  GetProcAddress(h, "CloseDevice");
        t->InitCAN        = (PF_InitCAN)      GetProcAddress(h, "InitCAN");
        t->StartCAN       = (PF_StartCAN)     GetProcAddress(h, "StartCAN");
        t->ResetCAN       = (PF_ResetCAN)     GetProcAddress(h, "ResetCAN");
        t->ClearBuffer    = (PF_ClearBuffer)  GetProcAddress(h, "ClearBuffer");
        t->GetReceiveNum  = (PF_GetReceiveNum)GetProcAddress(h, "GetReceiveNum");
        t->Transmit       = (PF_Transmit)     GetProcAddress(h, "Transmit");
        t->Receive        = (PF_Receive)      GetProcAddress(h, "Receive");
        t->ReadBoardInfo  = (PF_ReadBoardInfo)GetProcAddress(h, "ReadBoardInfo");
        t->ReadErrInfo    = (PF_ReadErrInfo)  GetProcAddress(h, "ReadErrInfo");
        t->ReadCANStatus  = (PF_ReadCANStatus)GetProcAddress(h, "ReadCANStatus");
        t->GetReference   = (PF_GetReference) GetProcAddress(h, "GetReference");
        t->SetReference   = (PF_SetReference) GetProcAddress(h, "SetReference");
    }

    return (t->OpenDevice && t->CloseDevice && t->InitCAN &&
            t->StartCAN && t->Transmit && t->Receive) ? 1 : 0;
}

/*==========================================================================
  10. 后端探测
 ==========================================================================*/
/* 判断候选 DLL 是不是自己。实现在 ProbePlainVci 前，
   但 ProbeGcan/ProbeKvaser 也用，所以必须提到这里声明。 */
static int IsSelfDll(const wchar_t *dir, const wchar_t *fname);

static int ProbePcan(const wchar_t *dir)
{
    HMODULE h = LoadSystemFirst(dir, L"PCANBasic.dll");
    if (!h) { LogF("PCAN: 找不到 PCANBasic.dll"); return 0; }

    pPcanInit   = (PF_CAN_Initialize)  GetProcAddress(h, "CAN_Initialize");
    pPcanUninit = (PF_CAN_Uninitialize)GetProcAddress(h, "CAN_Uninitialize");
    pPcanReset  = (PF_CAN_Reset)       GetProcAddress(h, "CAN_Reset");
    pPcanRead   = (PF_CAN_Read)        GetProcAddress(h, "CAN_Read");
    pPcanWrite  = (PF_CAN_Write)       GetProcAddress(h, "CAN_Write");
    pPcanGetVal = (PF_CAN_GetValue)    GetProcAddress(h, "CAN_GetValue");
    pPcanSetVal = (PF_CAN_SetValue)    GetProcAddress(h, "CAN_SetValue");

    if (!pPcanInit || !pPcanRead || !pPcanWrite) {
        LogF("PCAN: PCANBasic.dll 导出不全");
        FreeLibrary(h);
        return 0;
    }
    /*
     ★★ 必须真的能打开通道才算可用 ★★

       一键工具会把 drivers\ 里的 PCANBasic.dll 复制到每台机器的
       ZCANPRO 程序目录。如果只看导出就选 PCAN 后端，
       创苯 / 周立功用户的桥就会选错后端，然后打不开 —— 全盘失败。

       这里真去 CAN_Initialize 一下：能初始化才算真有硬件。
       探完立即 Uninitialize，不影响后面 ZCANPRO 的正常 InitCAN。
     */
    {
        int k, found = 0, first = -1;
        for (k = 0; k < VCI_MAX_CHANNEL; k++) {
            TPCANHandle hch = (TPCANHandle)(PCAN_USBBUS1 + k);
            if (pPcanInit(hch, PCAN_BAUD_500K, 0, 0, 0) == PCAN_ERROR_OK) {
                if (pPcanUninit) pPcanUninit(hch);
                found++;
                if (first < 0) first = k;
            }
        }
        if (found == 0) {
            LogF("PCAN: PCANBasic.dll 存在，但打不开任何通道"
                 "（没插盒子 / 内核驱动没装）");
            FreeLibrary(h);
            pPcanInit = NULL; pPcanRead = NULL; pPcanWrite = NULL;
            pPcanUninit = NULL; pPcanReset = NULL;
            return 0;
        }
        if (first > 0) g_pcanCh = (TPCANHandle)(PCAN_USBBUS1 + first);
        g_realChans = found;
        LogF("PCAN: 探到 %d 路可用", found);
    }
    g_hBackend = h;
    g_backend = BE_PCAN;
    strcpy(g_backendName, "pcan");
    LogF("后端: PEAK PCANBasic.dll");
    return 1;
}

static int ProbeGcan(const wchar_t *dir)
{
    HMODULE h;
    VCI_TABLE t;
    static const DWORD kTypes[] = { 4, 3, 21, 20, 2, 1, 31, 34 };
    int i;

    if (IsSelfDll(dir, L"ECanVci.dll")) return 0;
    h = LoadNear(dir, L"ECanVci.dll");
    if (!h) { LogF("GCAN: 找不到 ECanVci.dll"); return 0; }
    if (!LoadVciTable(h, &t)) {
        LogF("GCAN: ECanVci.dll 导出不全");
        FreeLibrary(h);
        return 0;
    }
    /*
     ★★ 必须真的能打开设备才算可用 ★★

       一键工具会把 drivers\ 里的 ECanVci.dll 复制到每台机器的
       ZCANPRO 程序目录。如果只看导出就选广成后端，
       创苯/周立功用户的桥就会选错，然后打不开 —— 全盘失败。
       所以这里要真去 OpenDevice，能开才算数。
     */
    for (i = 0; i < (int)(sizeof(kTypes) / sizeof(kTypes[0])); i++) {
        DWORD ty = kTypes[i];
        if (t.OpenDevice(ty, 0, 0) == STATUS_OK) {
            /* 不关闭，把已开状态留给后面复用 */
            g_vci = t;
            g_vciRealType = ty;
            g_devType = ty;   /* 关设备时要用 */
            g_vciOpened = 1;
            g_hBackend = h;
            g_backend = BE_GCAN;
            strcpy(g_backendName, "gcan");
            LogF("后端: 广成 ECanVci.dll（型号 %lu 能打开）",
                 (unsigned long)ty);
            return 1;
        }
    }
    LogF("GCAN: ECanVci.dll 导出齐全，但打不开任何设备");
    FreeLibrary(h);
    return 0;
}

static int ProbeTosun(const wchar_t *dir)
{
    HMODULE h;
    int isTsmaster;

    /*
     ★ 先找 TSMaster.dll（tsapp_* 在这里），再退回 libTSCAN.dll。
       两者导出的层级不同：
         TSMaster.dll  797 导出，含 tsapp_connect / tsapp_transmit_can_async
         libTSCAN.dll  257 导出，只有 tscan_* / tsfifo_*
       我们的实现用的是 tsapp_* 那一层，所以优先 TSMaster.dll。
    */
    h = LoadNear(dir, L"TSMaster.dll");
    isTsmaster = (h != NULL);
    if (!h) {
        h = LoadNear(dir, L"libTSCAN.dll");
        if (!h) { LogF("TOSUN: 找不到 TSMaster.dll 也找不到 libTSCAN.dll"); return 0; }
    }

    pTsInit      = (PF_ts_initialize) GetProcAddress(h, "initialize_lib_tsmaster");
    pTsFin       = (PF_ts_finalize)   GetProcAddress(h, "finalize_lib_tsmaster");
    pTsConnect   = (PF_ts_connect)    GetProcAddress(h, "tsapp_connect");
    pTsDisc      = (PF_ts_disconnect) GetProcAddress(h, "tsapp_disconnect");
    pTsChanCount = (PF_ts_chan_count) GetProcAddress(h, "tsapp_get_can_channel_count");
    pTsCfgBaud   = (PF_ts_cfg_baud)   GetProcAddress(h, "tsapp_configure_baudrate_can");
    pTsTx        = (PF_ts_tx_can)     GetProcAddress(h, "tsapp_transmit_can_async");
    /* ★ 注意名字：是 tsfifo_receive_can_msgs，不是 tsapp_receive_can_msgs */
    pTsRx        = (PF_ts_rx_can)     GetProcAddress(h, "tsfifo_receive_can_msgs");
    pTsClearRx   = (PF_ts_clear_rx)   GetProcAddress(h, "tsfifo_clear_can_receive_buffers");
    pTsRxCount   = (PF_ts_rx_count)   GetProcAddress(h, "tsfifo_read_can_buffer_frame_count");

    if (!pTsInit || !pTsConnect || !pTsTx || !pTsRx) {
        LogF("TOSUN: %ls 关键导出缺失（初始化/连接/发送/接收）",
             isTsmaster ? L"TSMaster.dll" : L"libTSCAN.dll");
        FreeLibrary(h);
        return 0;
    }
    g_hBackend = h;
    g_backend = BE_TOSUN;
    strcpy(g_backendName, "tosun");
    LogF("后端: TOSUN %ls（通道数接口%s）",
         isTsmaster ? L"TSMaster.dll" : L"libTSCAN.dll",
         pTsRxCount ? "可用" : "缺失");
    return 1;
}

static int ProbeKvaser(const wchar_t *dir)
{
    HMODULE h;
    if (IsSelfDll(dir, L"canlib32.dll")) return 0;
    h = LoadNear(dir, L"canlib32.dll");
    if (!h) { LogF("KVASER: 找不到 canlib32.dll"); return 0; }

    pKOpen    = (PF_canOpenChannel) GetProcAddress(h, "canOpenChannel");
    pKClose   = (PF_canClose)       GetProcAddress(h, "canClose");
    pKBusOn   = (PF_canBusOn)       GetProcAddress(h, "canBusOn");
    pKBusOff  = (PF_canBusOff)      GetProcAddress(h, "canBusOff");
    pKSetBus  = (PF_canSetBusParams)GetProcAddress(h, "canSetBusParams");
    pKWrite   = (PF_canWrite)       GetProcAddress(h, "canWrite");
    pKRead    = (PF_canRead)        GetProcAddress(h, "canRead");
    pKFlushRx = (PF_canFlushRx)     GetProcAddress(h, "canFlushReceiveQueue");
    pKChanCount = (PF_canGetNumberOfChannels)GetProcAddress(h, "canGetNumberOfChannels");

    if (!pKOpen || !pKWrite || !pKRead) {
        LogF("KVASER: canlib32.dll 关键导出缺失");
        FreeLibrary(h);
        return 0;
    }
    /*
     ★★ 必须真的有 Kvaser 通道才算可用 ★★

       一键工具会把 drivers\canlib32.dll 复制到每个人的 ZCANPRO 目录。
       用创苯 / 周立功盒子的人目录里就有 canlib32.dll，
       但没装 Kvaser 驱动 —— 旧探测会误选 Kvaser 后端，
       然后 OpenDevice 拿到 -8，全盘失败（真机推演出来的）。

       canGetNumberOfChannels：
         驱动没装   → 返回 -8（canERR_NOTINITIALIZED）
         驱动装了没硬件 → 返回 0
         有硬件     → 返回 >=1
       两种情况都不能选它。
     */
    if (pKChanCount) {
        int nch = 0;
        Kstatus ks = pKChanCount(&nch);
        if (ks != 0 || nch <= 0) {
            LogF("KVASER: canlib32.dll 存在，但没有可用通道"
                 "（返回 %d，通道 %d）—— Kvaser 驱动没装？",
                 (int)ks, nch);
            FreeLibrary(h);
            pKOpen = NULL; pKWrite = NULL; pKRead = NULL;
            pKChanCount = NULL;
            return 0;
        }
    }
    g_hBackend = h;
    g_backend = BE_KVASER;
    strcpy(g_backendName, "kvaser");
    LogF("后端: Kvaser canlib32.dll");
    return 1;
}

/*
 Vector（vxlapi）。

 注意两点：
   1. vxlapi.dll 一般装在 SysWOW64（系统目录），不在 kerneldlls。
      所以先试 kerneldlls，再让 LoadLibrary 走系统路径。
   2. Vector 驱动自带 Virtual Channel，没有硬件也能 OpenPort 成功，
      所以这里额外检查「有没有真正的 CAN 通道可用」，
      避免没插硬件时抢走别的后端。
*/

/* 大小写不敏感的子串查找（不引 shlwapi，保持零额外依赖） */
static int ContainsNoCase(const char *hay, const char *needle)
{
    size_t nl, hl, i, j;
    if (!hay || !needle) return 0;
    nl = strlen(needle); hl = strlen(hay);
    if (nl == 0 || hl < nl) return 0;
    for (i = 0; i + nl <= hl; i++) {
        for (j = 0; j < nl; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

static int ProbeVector(const wchar_t *dir, int explicit_req)
{
    HMODULE h = LoadNear(dir, L"vxlapi.dll");
    if (!h) { LogF("VECTOR: 找不到 vxlapi.dll"); return 0; }

    pXlOpen       = (PF_xlOpenDriver)      GetProcAddress(h, "xlOpenDriver");
    pXlClose      = (PF_xlCloseDriver)     GetProcAddress(h, "xlCloseDriver");
    pXlCfg        = (PF_xlGetDriverConfig) GetProcAddress(h, "xlGetDriverConfig");
    pXlOpenPort   = (PF_xlOpenPort)        GetProcAddress(h, "xlOpenPort");
    pXlClosePort  = (PF_xlClosePort)       GetProcAddress(h, "xlClosePort");
    pXlSetBitrate = (PF_xlCanSetChannelBitrate)GetProcAddress(h, "xlCanSetChannelBitrate");
    pXlActivate   = (PF_xlActivateChannel) GetProcAddress(h, "xlActivateChannel");
    pXlDeactivate = (PF_xlDeactivateChannel)GetProcAddress(h, "xlDeactivateChannel");
    pXlTransmit   = (PF_xlCanTransmit)     GetProcAddress(h, "xlCanTransmit");
    pXlReceiveFn  = (PF_xlReceive)         GetProcAddress(h, "xlReceive");
    pXlFlushRx    = (PF_xlFlushReceiveQueue)GetProcAddress(h, "xlFlushReceiveQueue");
    pXlErrStr     = (PF_xlGetErrorString)  GetProcAddress(h, "xlGetErrorString");
    pXlSetAppl    = (PF_xlSetApplConfig)   GetProcAddress(h, "xlSetApplConfig");

    if (!pXlOpen || !pXlCfg || !pXlOpenPort || !pXlTransmit || !pXlReceiveFn) {
        LogF("VECTOR: vxlapi.dll 关键导出缺失");
        FreeLibrary(h);
        return 0;
    }

    /* 打开驱动并读通道表，顺便验证结构体布局 */
    {
        XLstatus st = pXlOpen();
        if (st != XL_SUCCESS) {
            LogF("VECTOR: xlOpenDriver -> %lu", (unsigned long)st);
            FreeLibrary(h);
            return 0;
        }
        g_xlDriverOpen = 1;
        {
            XLdriverConfig *cfg = (XLdriverConfig *)calloc(1, sizeof(XLdriverConfig));
            if (!cfg) { pXlClose(); g_xlDriverOpen = 0; FreeLibrary(h); return 0; }
            st = pXlCfg(cfg);
            if (st == XL_SUCCESS && cfg->channelCount <= 64) {
                DWORD i;
                int realHw = 0;
                g_xlChanCount = (int)cfg->channelCount;
                g_xlAvailMask = 0;
                g_xlCanN = 0;
                /* ★ 关键：Vector 的"虚拟通道"不是硬件。
                   判据：transceiverName / name 里含 "Virtual"。
                   真 VN1630 的 transceiverName 是 "CANpiggy" 之类。 */
                for (i = 0; i < cfg->channelCount; i++) {
                    XLchannelConfig *c = &cfg->channel[i];
                    int virt = ContainsNoCase(c->transceiverName, "virtual") ||
                               ContainsNoCase(c->name, "virtual");
                    if (virt) {
                        LogF("VECTOR:   通道%lu \"%s\" 是虚拟通道，跳过",
                             (unsigned long)i, c->name);
                        continue;
                    }
                    realHw = 1;
                    if (c->channelBusCapabilities & XL_BUS_COMPATIBLE_CAN) {
                        g_xlAvailMask |= c->channelMask;
                        if (g_xlCanN < XL_MAX_CAN_INFO) {
                            g_xlCan[g_xlCanN].mask      = c->channelMask;
                            g_xlCan[g_xlCanN].hwType    = c->hwType;
                            g_xlCan[g_xlCanN].hwIndex   = c->hwIndex;
                            g_xlCan[g_xlCanN].hwChannel = c->hwChannel;
                            g_xlCanN++;
                        }
                    }
                    LogF("VECTOR:   通道%lu \"%s\" 真硬件 transceiver=\"%s\" mask=0x%I64X",
                         (unsigned long)i, c->name, c->transceiverName,
                         (unsigned long long)c->channelMask);
                }
                if (!g_xlAvailMask && explicit_req) {
                    /*
                     显式要求 vector 时允许用虚拟通道（方便无硬件时调试）。

                     ★ 这里必须【同时】填 g_xlCan[] 硬件信息表，
                       否则后面打开端口那个循环（按 g_xlCanN 遍历）会是空的，
                       日志就变成"试了 0 个 Vector 通道"，一个口都没试。
                       这个 bug 是本机实测暴露的。
                     */
                    for (i = 0; i < cfg->channelCount; i++) {
                        XLchannelConfig *c = &cfg->channel[i];
                        if (!(c->channelBusCapabilities & XL_BUS_COMPATIBLE_CAN)) continue;
                        g_xlAvailMask |= c->channelMask;
                        if (g_xlCanN < XL_MAX_CAN_INFO) {
                            g_xlCan[g_xlCanN].mask      = c->channelMask;
                            g_xlCan[g_xlCanN].hwType    = c->hwType;
                            g_xlCan[g_xlCanN].hwIndex   = c->hwIndex;
                            g_xlCan[g_xlCanN].hwChannel = c->hwChannel;
                            g_xlCanN++;
                        }
                    }
                    LogF("VECTOR: 显式指定 vector，允许使用虚拟通道 mask=0x%I64X（%d 个口）",
                         (unsigned long long)g_xlAvailMask, g_xlCanN);
                }
                if (!g_xlAvailMask) {
                    LogF("VECTOR: 没有可用的真 Vector 通道（只有虚拟通道），跳过");
                    free(cfg);
                    pXlClose(); g_xlDriverOpen = 0;
                    FreeLibrary(h);
                    return 0;
                }
                (void)realHw;
                LogF("VECTOR: 驱动 OK, 通道数 %lu, 可用 mask 0x%I64X, dllVer 0x%X",
                     (unsigned long)cfg->channelCount,
                     (unsigned long long)g_xlAvailMask,
                     (unsigned)cfg->dllVersion);
            } else {
                LogF("VECTOR: xlGetDriverConfig -> %lu (channelCount=%lu) 布局可疑",
                     (unsigned long)st, (unsigned long)cfg->channelCount);
                free(cfg);
                pXlClose(); g_xlDriverOpen = 0;
                FreeLibrary(h);
                return 0;
            }
            free(cfg);
        }
    }

    g_hBackend = h;
    g_backend = BE_VECTOR;
    strcpy(g_backendName, "vector");
    LogF("后端: Vector vxlapi.dll");
    return 1;
}

/*
 ★★ 判断「这个候选文件是不是我自己」★★

   桥在 ZCANPRO 目录里就叫 USBCAN.dll（替换原厂），
   而探测列表里也有 USBCAN.dll / ControlCAN.dll。
   如果不排除自己，LoadLibrary 会拿到自己的句柄，
   然后在探测里调用自己的 VCI_OpenDevice → 又进 SelectBackend → 死循环。

   注意：这里必须比较「完整路径」，不能只比文件名 ——
   别人的 USBCAN.dll（原厂备份、系统目录那份）要照常能加载。
*/
static int IsSelfDll(const wchar_t *dir, const wchar_t *fname)
{
    wchar_t self[MAX_PATH * 2];
    wchar_t cand[MAX_PATH * 2];

    if (!g_hSelf) return 0;
    self[0] = L'\0';
    cand[0] = L'\0';
    if (!GetModuleFileNameW(g_hSelf, self, MAX_PATH * 2)) return 0;
    _snwprintf(cand, MAX_PATH * 2 - 1, L"%s\\%s", dir, fname);
    cand[MAX_PATH * 2 - 1] = L'\0';
    return (_wcsicmp(self, cand) == 0) ? 1 : 0;
}

/*
 ★ 加载 VCI 后端 DLL：优先找宿主自带的 kerneldlls\ 目录

   为什么要这一步（真机证据）：

     ZCANPRO 2.4.1 的程序目录里【没有】USBCAN.dll，
     它真正的设备插件放在 kerneldlls\USBCAN.dll（54,744 B）。
     原厂就是靠这份驱盒子的 —— 真机验证能收 152 / 331 帧。

     而桥的 VCI 候选表里也有 "USBCAN.dll" 这个名字。
     原来用 LoadNear 会先命中 E:\ZCANPRO\USBCAN.dll ——
     那是桥自己，被跳过之后就再也没机会轮到 kerneldlls 那份。
     结果：USBCAN_E.dll（SysWOW64 里那份 2.3.0.6）打不开这个 2E-U，
           而真正能打开它的 kerneldlls\USBCAN.dll 从来没被试过。

   候选顺序：
     ① <dir>\kerneldlls\<name>   宿主自带、和它版本配套的那份
     ② <dir>\<name>              程序目录里的第三方库（创芯/广成）
     ③ <name>                    系统搜索（PEAK 等）

   每一份都会检查「是不是桥自己」（靠 UniVciBridgeIdent 标记），
   是就跳过，继续下一份。这样既不会递归，也不会漏掉后面的候选。
 */
static HMODULE LoadVciNear(const wchar_t *dir, const wchar_t *name)
{
    typedef DWORD (__stdcall *PF_Ident)(void);
    wchar_t p[MAX_PATH * 2];
    int i;

    for (i = 0; i < 3; i++) {
        HMODULE h = NULL;

        if (i == 0) {
            _snwprintf(p, MAX_PATH * 2 - 1, L"%s\\kerneldlls\\%ls", dir, name);
            p[MAX_PATH * 2 - 1] = L'\0';
            h = LoadLibraryW(p);
        } else if (i == 1) {
            _snwprintf(p, MAX_PATH * 2 - 1, L"%s\\%ls", dir, name);
            p[MAX_PATH * 2 - 1] = L'\0';
            h = LoadLibraryW(p);
        } else {
            h = LoadLibraryW(name);
        }
        if (!h) continue;

        {
            PF_Ident pf = (PF_Ident)GetProcAddress(h, "UniVciBridgeIdent");
            if (pf && pf() == UNIVCI_IDENT_MAGIC) {
                FreeLibrary(h);
                continue;                 /* 是桥自己，换下一份 */
            }
        }
        return h;
    }
    return NULL;
}

static int ProbePlainVci(const wchar_t *dir, const wchar_t *fname)
{
    HMODULE h;
    VCI_TABLE t;
    /*
     ★ 探测顺序：2E-U(21) 要在 E-U(20) 前面。

       真机踩到：用户的盒子是 USBCAN-2E-U（双通道，线插在 CAN1），
       但 20 先命中，等于告诉厂家库「这是单通道设备」，
       然后去 InitCAN 第 1 路 —— CH1 一直刷错误帧、接收帧数 0。

       2E-U 是 E-U 的超集：真 2E-U 命中 21 两路都对；
       真 E-U 若拒绝 21 会退到 20，也不会更差。
     */
    /*
     ★ 候选型号：【上层请求的型号】排最前面，再接固定候选表。

       真机证据：创苯的 ControlCAN.dll 对型号 21 也返回成功，
       如果固定从 21 开始试，创苯盒子就被当成 2E-U，
       走上 E-U 专用的 SetReference 路径（创苯靠 timing 字节设波特率），
       结果波特率没配对，一帧收不到。

       顺序：请求型号 → 21 → 20 → 31 → 34 → 4 → 3 → 5 → 2 → 1
     */
    DWORD types[10];
    int   nTypes = 0;
    {
        static const DWORD baseEU[]  = { 21, 20, 31, 34, 4, 3, 5, 2, 1 };
        static const DWORD baseGen[] = { 4, 3, 21, 20, 31, 34, 5, 2, 1 };
        const DWORD *base;
        int b, k, isEu = 0;

        /*
         ★ 按 DLL 家族决定试哪些型号。

           vi 命问题（14:36 那次改动引入的回归）：
             为了让创苯（ControlCAN.dll）用上它请求的型号 4，
             我改成「优先用上层请求的型号」。但 USBCAN_E.dll 对 4 也返回成功，
             于是 2E-U 被当成型号 4 打开 —— 去要 BoardInfo 时厂家填了垃圾型号，
             ZCANPRO 拿去查 channelNumber_count() 就断言崩溃。

           现在：
             周立功 E-U 家族（USBCAN_E / 4E_U / 8E_U）
                 → 固定 21 → 20 → 31 → 34 → 4 → 3，不听上层的 4
             其它厂家库（创苯 ControlCAN / 广成等）
                 → 优先用上层请求的型号，不行再按通用顺序
         */
        if (wcsstr(fname, L"USBCAN_E") || wcsstr(fname, L"4E_U") || wcsstr(fname, L"8E_U"))
            isEu = 1;

        base = isEu ? baseEU : baseGen;
        if (!isEu && g_reqType >= 1 && g_reqType <= 40) types[nTypes++] = g_reqType;
        for (b = 0; b < 9; b++) {
            int dup = 0;
            for (k = 0; k < nTypes; k++) if (types[k] == base[b]) dup = 1;
            if (!dup && nTypes < 10) types[nTypes++] = base[b];
        }
    }
    int i;

    /*
     ★ 不再按「文件名等于桥自己的路径」提前出局。
       原因：dir\USBCAN.dll 可能正是桥自己，而 kerneldlls\USBCAN.dll
       才是宿主真正用的那份。提前 return 会把整条候选路径掐断。
       现在改成加载后靠 UniVciBridgeIdent 判身份，是桥自己就换下一份。
     */
    h = LoadVciNear(dir, fname);
    if (!h) return 0;
    if (!LoadVciTable(h, &t)) {
        LogF("VCI: %ls 导出不全", fname);
        FreeLibrary(h);
        return 0;
    }
    /*
     ★ 身份证检查：能调到 UniVciBridgeIdent 并拿到密码的，就是桥自己
       （哪怕它被改名成 ControlCAN.dll / USBCAN.dll 另存一份）。
       不跳过就会无限递归。
     */
    /* 身份检查已在 LoadVciNear 里做过 */
    /*
     ★★ 必须真的能打开设备才算可用 ★★

       只看导出会被「装了但用不了」的库骗过：
         创苯的 ControlCAN.dll 导出很齐，但只能开创苯硬件；
         周立功的 USBCAN_E.DLL 也很齐，但只能开周立功硬件。
       只有真正能 Open 成功的那个，才是本机该用的后端。

       同时把能开的型号记进 g_vciRealType ——
       各家的型号编号不一样，不能拿 ZCANPRO 的型号直接用。
     */
    for (i = 0; i < nTypes; i++) {
        DWORD ty = types[i];
        /*
         ★ 失败就把设备关掉，别留脏状态。

           周立功的型号（20 / 21）对应不同固件协议。
           拿 20 去 open 一个 2E-U，即使返回失败，
           USB 端点和固件通道状态可能已经被扰乱，
           紧接着再 open 21 就可能卡在异常状态（表现就是能开、
           能启动、但收不到报文）。所以每次失败都清一次。
         */
        if (t.OpenDevice(ty, 0, 0) == STATUS_OK) {
            /*
             ★ 不关闭！直接把这个「已打开」状态保留下来。
               周立功的库对 open->close->open 不友好，
               第二次打开后 InitCAN 会一直失败（真机实测）。
             */
            g_vciOpened = 1;
            g_vci = t;
            g_vciRealType = ty;
            g_devType = ty;   /* 关设备时要用 */
            g_hBackend = h;
            g_backend = BE_VCI;
            strcpy(g_backendName, "vci");
            LogF("后端: VCI 兼容 %ls（型号 %lu 能打开）",
                 fname, (unsigned long)ty);
            return 1;
        }
        /* 这一个型号没打开：立即关掉，不给下一个型号留下脏状态 */
        /* 注意：探测失败【不要】在这里 CloseDevice。
           有些驱动会把这个 Close 当成"关掉整个设备"，
           导致后面用正确型号也打不开 —— 越探越坏。 */
    }
    LogF("VCI: %ls 导出齐全，但打不开任何设备", fname);
    FreeLibrary(h);
    return 0;
}

/*
 依次试所有「通用 VCI 兼容库」。

 为什么需要它，而不是只试 ControlCAN.dll：
   周立功把 E-U 系列的库命名成 USBCAN_E.dll（装在 SysWOW64），
   创芯叫 ControlCAN.dll，广成叫 ECanVci.dll，文件名各不相同。
   挨个试一遍，谁能真的打开设备就用谁。
*/
static int ProbeAnyVci(const wchar_t *dir)
{
    static const wchar_t *names[] = {
        L"USBCAN_E.dll",      /* 周立功 E-U / 2E-U / 4E-U */
        L"USBCAN_4E_U.dll",
        L"USBCAN_8E_U.dll",
        L"ControlCAN.dll",    /* 创芯 / 通用 */
        L"ECanVci.dll",       /* 广成 */
        L"GCANVci.dll",
        L"USBCAN.dll",
        L"ControlCAN_32.dll"
    };
    int i;
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        if (ProbePlainVci(dir, names[i])) return 1;
    }
    return 0;
}

/*
 从 UniVciBridge.ini 里读一个布尔开关（key=1 / key=y）。

 为什么单独写一个：
   ReadIniBackend 只认 backend 这一行，其它键它直接跳过。
   这里用 strstr 找键名，再确认紧跟的是 '='，避免误匹配到别的行。
*/
static int ReadIniFlag(const wchar_t *dir, const char *key)
{
    wchar_t path[MAX_PATH * 2];
    FILE *f;
    char raw[2048];
    size_t got, kl;
    char *p, *hit;

    kl = strlen(key);
    if (kl == 0) return 0;

    _snwprintf(path, MAX_PATH * 2 - 1, L"%s\\UniVciBridge.ini", dir);
    path[MAX_PATH * 2 - 1] = L'\0';

    f = _wfopen(path, L"rb");
    if (!f) return 0;
    got = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    raw[got] = '\0';

    p = raw;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) p += 3;

    hit = p;
    while ((hit = strstr(hit, key)) != NULL) {
        char *q = hit + kl;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '=') {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            return (*q == '1' || *q == 'y' || *q == 'Y' ||
                    *q == 't' || *q == 'T');
        }
        hit += kl;
    }
    return 0;
}

/*
 从 ini 读一个整数（key=123）。没有就返回 0。
 ReadIniFlag 只认布尔，模拟型号需要数字。
 */
static DWORD ReadIniInt(const wchar_t *dir, const char *key)
{
    wchar_t path[MAX_PATH * 2];
    FILE *f;
    char raw[2048];
    size_t got, kl;

    kl = strlen(key);
    if (kl == 0) return 0;

    _snwprintf(path, MAX_PATH * 2 - 1, L"%s\\UniVciBridge.ini", dir);
    path[MAX_PATH * 2 - 1] = L'\0';

    f = _wfopen(path, L"rb");
    if (!f) return 0;
    got = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    raw[got] = '\0';

    {
        char *hit = raw;
        if ((unsigned char)hit[0] == 0xEF && (unsigned char)hit[1] == 0xBB &&
            (unsigned char)hit[2] == 0xBF) hit += 3;

        while ((hit = strstr(hit, key)) != NULL) {
            char *q = hit + kl;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                return (DWORD)strtoul(q, NULL, 0);   /* 支持 21 和 0x15 两种写法 */
            }
            hit += kl;
        }
    }
    return 0;
}

static void ReadIniBackend(const wchar_t *dir, char *out, size_t n)
{
    wchar_t path[MAX_PATH * 2];
    FILE *f;

    out[0] = '\0';
    _snwprintf(path, MAX_PATH * 2 - 1, L"%s\\UniVciBridge.ini", dir);

    f = _wfopen(path, L"rb");
    if (!f) return;
    {
        /* 手工读，兼容 UTF-8 / UTF-8 BOM / ANSI */
        char raw[2048];
        size_t got = fread(raw, 1, sizeof(raw) - 1, f);
        char *p;
        fclose(f);
        raw[got] = '\0';
        p = raw;
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF) p += 3;
        {
            char *ln = p;
            while (*ln) {
                char *e = ln;
                while (*e && *e != '\n' && *e != '\r') e++;
                {
                    char saved = *e;
                    *e = '\0';
                    {
                        char *eq = strchr(ln, '=');
                        if (eq) {
                            *eq = '\0';
                            if (_stricmp(ln, "backend") == 0) {
                                char *v = eq + 1;
                                while (*v == ' ' || *v == '\t') v++;
                                strncpy(out, v, n - 1);
                                out[n - 1] = '\0';
                                {
                                    char *t = out + strlen(out);
                                    while (t > out && (t[-1] == ' ' || t[-1] == '\t' ||
                                                       t[-1] == '\r' || t[-1] == '\n')) *--t = '\0';
                                }
                                return;
                            }
                        }
                    }
                    *e = saved;
                    ln = e;
                    while (*ln == '\r' || *ln == '\n') ln++;
                }
            }
        }
    }
}

/*
 后端探测顺序。

 ★ 优先读 UniVciBridge.ini 的 backend=。
   一个目录里同时存在 ECanVci.dll 和 PCANBasic.dll 时，
   自动探测很容易选错，所以安装器会显式写 ini。
*/
/*
 ★★ 原厂透传（passthrough）★★

 为什么必须有这个：
   本方案的做法是把 USBCAN.dll 换成桥。但这样一来，
   **真周立功盒子也会被桥接管** —— 如果桥只认 PCAN/广成，
   插一块真 USBCAN-II 就打不开了，等于破坏了厂家软件。

 解决办法：
   安装时把原厂 DLL 备份成 USBCAN.dll.zlg.bak（它本身还是
   一个完整的 14 函数 VCI 实现）。桥在最后把它当成一个普通
   VCI 后端加载，所有调用原样转过去。

 于是行为变成「智能回退」：
   · 插的是 PCAN/广成/…   → 用对应后端
   · 插的是真周立功盒子     → 后端探测全部失败，自动落到原厂 DLL
   · 什么后端都没有         → 落到原厂 DLL，行为与安装前完全一致
 这样「不影响厂家自己软件」这条要求才成立。
*/
static int ProbePassthrough(const wchar_t *dir)
{
    /*
     ★ 桥可能装成不同的文件名：
         USBCAN.dll          → 型号 3/4（USBCAN-I/II，1~2 通道）
         USBCAN_4E_U.dll     → 型号 31（4 通道）
         USBCAN_8E_U.dll     → 型号 34（8 通道，最大）
         USBCAN_E.dll        → 型号 20/21（E-U / 2E-U）

       所以原厂备份也就有几种可能。
       优先用「和自己同名」的那个备份，再依次退到其它。
     */
    static const wchar_t *kNames[] = {
        L"%s.zlg.bak",              /* 自己同名（最优） */
        L"USBCAN.dll.zlg.bak",
        L"USBCAN_8E_U.dll.zlg.bak",
        L"USBCAN_4E_U.dll.zlg.bak",
        L"USBCAN_E.dll.zlg.bak",
    };
    wchar_t self[MAX_PATH * 2];
    wchar_t selfName[MAX_PATH];
    wchar_t bak[MAX_PATH * 2];
    HMODULE h;
    int k;

    /* 取自己的文件名 */
    selfName[0] = L'\0';
    self[0] = L'\0';
    if (GetModuleFileNameW(g_hSelf, self, MAX_PATH * 2)) {
        wchar_t *s = wcsrchr(self, L'\\');
        if (s) {
            wcsncpy(selfName, s + 1, MAX_PATH - 1);
            selfName[MAX_PATH - 1] = L'\0';
        }
    }

    for (k = 0; k < (int)(sizeof(kNames) / sizeof(kNames[0])); k++) {
        if (k == 0) {
            if (!selfName[0]) continue;
            _snwprintf(bak, MAX_PATH * 2 - 1, L"%s\\%s.zlg.bak", dir, selfName);
        } else {
            _snwprintf(bak, MAX_PATH * 2 - 1, L"%s\\%s", dir, kNames[k]);
        }
        bak[MAX_PATH * 2 - 1] = L'\0';
        {
            DWORD at = GetFileAttributesW(bak);
            if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY))
                continue;
        }

        h = LoadLibraryW(bak);
        if (!h) {
            LogF("原厂透传: 加载 %ls 失败（错误 %lu）",
                 bak, (unsigned long)GetLastError());
            continue;
        }
        if (!LoadVciTable(h, &g_vci)) {
            LogF("原厂透传: %ls 的 14 个 VCI 导出不全", bak);
            FreeLibrary(h);
            continue;
        }
        g_hBackend = h;
        g_backend = BE_VCI;
        strcpy(g_backendName, "zlg-original");
        LogF("★ 后端: 原厂 DLL 透传（%ls）—— 真周立功盒子照旧可用",
             bak);
        return 1;
    }
    return 0;
}

/* 运行时回退：指定/探测到的后端打不开盒子时，退回原厂 DLL。
   这样「插 PCAN 就用 PCAN，插真周立功就用原厂驱动」自动成立。 */
static int TryOriginalPassthrough(void)
{
    wchar_t dir[MAX_PATH * 2];
    if (g_backend == BE_VCI && _stricmp(g_backendName, "zlg-original") == 0)
        return 0;                       /* 已经是原厂透传，别再切 */
    SelfDir(dir, MAX_PATH * 2);
    return ProbePassthrough(dir);
}

static void SelectBackend(void)
{
    wchar_t dir[MAX_PATH * 2];
    char want[32];

    if (g_backend != BE_NONE) return;

    /*
     ★ 优化 3：探测失败后 3 秒内不再重探。

       原因：失败时 g_backend 仍是 BE_NONE，于是 VCI_OpenDevice /
       SetReference / InitCAN / Transmit 每个入口都会把整条探测链
       （PCAN→GCAN→KVASER→VCI×3→VECTOR）重跑一遍 ——
       真机日志里能看到同一轮链重复 3~4 次，既慢又刷屏。
     */
    if (g_probeFailTick != 0 && (GetTickCount() - g_probeFailTick) < 3000)
        return;
    SelfDir(dir, MAX_PATH * 2);
    ReadIniBackend(dir, want, sizeof(want));
    g_pcanEcho = ReadIniFlag(dir, "pcan_echo");
    /* ★ 指定 Vector 硬件通道（VN1630A 的第几路 CAN，1 起） */
    {
        DWORD want = ReadIniInt(dir, "xl_channel");
        g_xlWantChan = (want >= 1 && want <= 8) ? (int)want : 0;
        if (g_xlWantChan)
            LogF("★ 指定 Vector 通道: 第 %d 路（xl_channel=%d）",
                 g_xlWantChan, g_xlWantChan);
    }
    g_euLayout = ReadIniFlag(dir, "eu_layout");

    /* ★ 模拟画像：对外演成哪个型号 */
    {
        DWORD pf = ReadIniInt(dir, "profile");
        if (pf >= 1 && pf <= 40) {
            g_profileType = pf;
            LogF("★ 模拟画像：对外声称型号 %lu（profile=%lu）",
                 (unsigned long)pf, (unsigned long)pf);
        } else {
            g_profileType = 0;
        }
        {
            DWORD ch = ReadIniInt(dir, "channels");
            g_profileChans = (ch >= 1 && ch <= VCI_MAX_CHANNEL) ? (int)ch : 0;
            if (g_profileChans)
                LogF("★ 模拟画像：对外声称 %d 路（channels=%d）",
                     g_profileChans, g_profileChans);
        }
    }

    if (want[0]) {
        LogF("配置指定后端: %s", want);
        if (_stricmp(want, "pcan") == 0)        { if (ProbePcan(dir))   return; }
        else if (_stricmp(want, "gcan") == 0)   { if (ProbeGcan(dir))   return; }
        else if (_stricmp(want, "tosun") == 0)  { if (ProbeTosun(dir))  return; }
        else if (_stricmp(want, "kvaser") == 0) { if (ProbeKvaser(dir)) return; }
        else if (_stricmp(want, "vector") == 0) { if (ProbeVector(dir, 1)) return; }
        else if (_stricmp(want, "zlg") == 0)    { if (ProbePassthrough(dir)) return; }
        /*
         ★ vci = 通用 VCI 兼容库（ControlCAN.dll / ECanVci.dll 这类）

           创芯 CANalyst 系列走的就是这条路：它提供的 ControlCAN.dll 导出
           14 个 VCI_* 函数，行为和周立功 ControlCAN 一致。

           以前这里漏了 vci 分支 —— 写 backend=vci 会被当成「未知后端」
           直接跳过，然后退回自动探测，结果选到别的后端去了（真机上踩到）。
         */
        else if (_stricmp(want, "vci") == 0) {
            static const wchar_t *vciNames[] = {
                L"USBCAN_E.dll",       /* 周立功 E-U / 2E-U / 4E-U */
                L"USBCAN_4E_U.dll",
                L"USBCAN_8E_U.dll",
                L"ControlCAN.dll",     /* 通用 VCI（创苯/广成等） */
                L"ECanVci.dll",        /* 广成 */
                L"GCANVci.dll",
                L"USBCAN.dll",
                L"ControlCAN_32.dll"
            };
            int vi;
            for (vi = 0; vi < (int)(sizeof(vciNames) / sizeof(vciNames[0])); vi++) {
                /*
                 不用 FileNear 限定只看自己目录 ——
                 周立功把 USBCAN_E.DLL 装在 SysWOW64，
                 创苯把 ControlCAN.dll 放在它自己的目录。
                 LoadNear 会先找本目录、再按系统搜索顺序找，能都覆盖到。
                 */
                if (ProbePlainVci(dir, vciNames[vi])) return;
            }
            LogF("vci: 没找到能打开设备的 VCI 兼容库");
        }
        LogF("指定的后端不可用，改为自动探测");
    }

    /* PEAK/Kvaser 的 DLL 通常装在系统目录，不在 kerneldlls */
    if (FileNear(dir, L"PCANBasic.dll") || DllLoadable(L"PCANBasic.dll"))
        { if (ProbePcan(dir))   return; }
    if (FileNear(dir, L"ECanVci.dll"))   { if (ProbeGcan(dir))   return; }
    if (FileNear(dir, L"TSMaster.dll") ||
        FileNear(dir, L"libTSCAN.dll"))  { if (ProbeTosun(dir))  return; }
    if (FileNear(dir, L"canlib32.dll") || DllLoadable(L"canlib32.dll"))
        { if (ProbeKvaser(dir)) return; }
    /*
     ★ Vector 放到最后自动探测：vxlapi.dll 在系统目录里，
       几乎每台装过 Vector 工具的机器都能加载成功，
       而且它自带 Virtual Channel，会把别的后端挡掉。
       所以只有在别的后端都不可用时才试它。
    */
    /* ★ 通用 VCI 兼容库（真驱动）必须排在 Vector 虚拟通道之前，
       否则装了 Vector 软件的机器上，虚拟通道会把真硬件挡掉。 */
    if (ProbeAnyVci(dir)) return;

    /*
     ★ Vector 默认【不探测】（用户要求，2026-11-04 暂时关闭）

       原因：
         · vxlapi.dll 几乎每台装过 Vector 工具的机器都能加载，
           会被选成后端，然后 xlOpenPort 返回 perm=0（没被授权），
           白耗 2 秒还污染日志，最后还得回退。
         · 而且它自带 Virtual Channel，会把真硬件挡掉。

       要用的话：在 UniVciBridge.ini 里写
           backend=vector
       就会走上面那条「配置指定后端」的分支，正常启用。
     */
    if (ProbePcan(dir))   return;
    if (ProbeGcan(dir))   return;
    if (ProbeTosun(dir))  return;
    if (ProbeKvaser(dir)) return;

    /* ★ Vector：现在可以放进自动链了。
       以前不敢放，是因为"虚拟通道"会让它抢走后端（perm=0，白耗时间）。
       现在 ProbeVector 会用 transceiverName 判断虚拟/真硬件：
         · 只有虚拟通道 → 自动跳过，绝不抢后端
         · 有真 VN 系列 → 正常选中（真硬件 xlOpenPort 会拿到 perm）
       显式写 backend=vector 时不看这个判断，虚拟通道也能用（方便调试）。 */
    if (ProbeVector(dir, 0)) return;

    /* ★ 最后的兜底：原厂 DLL 透传 */
    if (ProbePassthrough(dir)) return;

    /*
     ★ 优化 2：全失败后隔 150ms 再来一轮。

       实测（周立功 2E-U）：
         第一次探测：VCI: USBCAN_E.dll 导出齐全，但打不开任何设备
         紧接着    ：后端: VCI 兼容 USBCAN_E.dll（型号 20 能打开）
       同一块盒子，第一次失败第二次成功 —— 刚插上/刚上电时设备还没就绪。
     */
    {
        static int g_retried = 0;
        if (!g_retried) {
            g_retried = 1;
            LogF("★ 第一轮探测全失败，150ms 后重试一轮");
            Sleep(150);
            SelectBackend();
            return;
        }
    }

    LogF("★ 没有任何可用后端（连原厂备份都没有）");
    strcpy(g_backendName, "none");

    /* ★ 优化 3：记下失败时间，3 秒内不再重复探测（否则每次 API 调用都重探） */
    g_probeFailTick = GetTickCount();
}

/*==========================================================================
  11. 型号翻译 + 通道数钳位
 ==========================================================================*/
/*
 ★ 对外暴露几路通道（用户定的规则）

     硬件 1 个 CAN 口           → 1 路（当 USBCAN-I 用）
     硬件 2 个及以上 CAN 口   → 2 路（当 USBCAN-II 用）
     多余的口                   → 默认不使用（封顶 2 路）

   另外不能超过「软件请求的型号」本身的上限：
     软件要 USBCAN-I（型号只声明 1 路）→ 就算盒子有 8 路也只给 1 路。
 */
/*
 ★ 从「设备型号」直接推通道数。

   为什么不用 BoardInfo：
     真机上创苯 / 周立功的 BoardInfo.can_Num 都返回过
     825700657、808470065 这种垃圾值，兜底成 2 会让
     单通道的 E-U 也被当成 2 路 —— 违反「1 个口当 USBCAN-I」。

   型号是「真能打开设备」的那一个，它本身就声明了几路，比 BoardInfo 可靠。
 */
static int ChannelsOfType(DWORD t)
{
    switch (t) {
    case 3:  return 1;      /* USBCAN-I      1 路 */
    case 4:  return 2;      /* USBCAN-II     2 路 */
    case 20: return 1;      /* USBCAN-E-U    1 路 */
    case 21: return 2;      /* USBCAN-2E-U   2 路 */
    case 31: return 4;      /* USBCAN-4E-U   4 路 */
    case 34: return 8;      /* USBCAN-8E-U   8 路 */
    }
    return 0;               /* 未知 → 让调用方兜底 */
}

static int EffectiveChans(DWORD modelMax)
{
    int hw = g_realChans;

    if (hw < 1) hw = 1;
    if (hw > 2) hw = 2;                 /* 多余口不用 */
    if ((DWORD)hw > modelMax && modelMax > 0) hw = (int)modelMax;
    if (hw > VCI_MAX_CHANNEL) hw = VCI_MAX_CHANNEL;
    return hw;
}

static DWORD MapDevType(DWORD zcanType, DWORD *canCount)
{
    DWORD real = zcanType;

    switch (zcanType) {
        case 2:  *canCount = 1; real = 2;  break;   /* PCIE9810   */
        case 3:  *canCount = 1; real = 3;  break;   /* USBCAN-I   */
        case 4:  *canCount = 2; real = 4;  break;   /* USBCAN-II  */
        case 20: *canCount = 1; real = 20; break;   /* USBCAN-E-U */
        case 21: *canCount = 2; real = 21; break;   /* USBCAN-2E-U */
        case 31: *canCount = 4; real = 31; break;
        case 34: *canCount = 8; real = 34; break;
        default: *canCount = 2; real = zcanType; break;
    }

    /* ★ 通道数必须钳位：广成驱动会把通道数报成 50（实际 2），
       ZCANPRO 按这个数去分配/遍历 → 越界 → 闪退。 */
    if (*canCount == 0) *canCount = 1;
    if (*canCount > VCI_MAX_CHANNEL) *canCount = VCI_MAX_CHANNEL;

    /*
     ★ vci 后端已经协商出真实型号 → 后续所有调用都用它。
       注意：此时 canCount 仍然按 ZCANPRO 请求的型号算（它决定 UI 显示几路），
       实际有几路另由 g_realChans 控制，两者分开。
     */
    if (g_backend == BE_VCI && g_vciRealType != 0)
        return g_vciRealType;

    return real;
}

/* ReleaseHardware 定义在文件末尾（DllMain 前），此处先声明，
   供 VCI_CloseDevice 使用。 */
static void ReleaseHardware(void);

/* PCAN 接收缓冲（实现在 GetReceiveNum 之前）。
   必须在所有导出函数之前声明，
   否则 ResetCAN / ClearBuffer 会把它当成隐式 int 函数，编译失败。 */
/* 清空桥自己的 VCI 接收缓冲 */

/* ★ 转换器抽到 CtConvert.h：桥和单元测试用同一份代码。
   native\CtConvTest.c 就是靠这一点做往返验证的。 */
#include "CtConvert.h"

static void VciRxReset(int CANInd)
{
    if (CANInd < 0 || CANInd >= VCI_MAX_CHANNEL) return;
    g_vciRxN[CANInd] = 0;
}

static void PcanRxReset(int CANInd);
static int  PcanPump(DWORD CANInd);

/*==========================================================================
  12. 14 个 VCI 导出
 ==========================================================================*/
/*==========================================================================
  ★ 桥的身份标记（非 VCI 标准函数，只给自己认自己用）★

  为什么需要：
    桥在 ZCANPRO 目录里叫 USBCAN.dll，而探测列表里也有 USBCAN.dll /
    ControlCAN.dll。如果用户把桥另外复制一份叫 ControlCAN.dll（为了配合
    只认周立功 VCI 的第三方软件），那就是两个不同文件、路径不同 ——
    只比路径认不出来，于是探测会把自己当后端加载 → 无限递归 → 栈溢出。

  所以靠这个导出当身份证：能调到它、并且返回值对的，就是桥自己。
==========================================================================*/
__declspec(dllexport) DWORD __stdcall UniVciBridgeIdent(void)
{
    return UNIVCI_IDENT_MAGIC;
}

__declspec(dllexport) DWORD __stdcall VCI_OpenDevice(DWORD DeviceType,
                                                     DWORD DeviceInd,
                                                     DWORD Reserved)
{
    g_reqType = DeviceType;   /* 记下上层请求的型号，探测时优先用 */
    DWORD cnt = 1, real;
    (void)Reserved;

    EnterCriticalSection(&g_cs);
    SelectBackend();
    /*
     ★ 把后端的短标识写进日志。

       人看的是上面那行「后端: PEAK PCANBasic.dll」；
       但自动化工具（自检脚本）需要一个能精确比对的短串：
         pcan / gcan / tosun / kvaser / vector / vci / zlg-original / none

       没有这个标识，工具就分不清两种情况：
         · 真的用上了 pcan
         · 指定的 pcan 不可用，自动退到了 vector（假阳性）
       而这两者对排错的含义完全相反。
     */
    LogF("★ 后端标识: %s", g_backendName);
    real = MapDevType(DeviceType, &cnt);
    g_devType = real;
    g_canNum = cnt;

    LogF("VCI_OpenDevice: 请求 %lu -> 实际 %lu, 通道 %lu, 后端 %s",
         (unsigned long)DeviceType, (unsigned long)real,
         (unsigned long)cnt, g_backendName);

    if (g_backend == BE_NONE) { goto L_FAIL; }
    if (g_opened)             { LeaveCriticalSection(&g_cs); return STATUS_OK; }

    switch (g_backend) {
    case BE_PCAN: {
        /*
         ★★ 不能用 CAN_GetValue(CHANNEL_CONDITION) 判断「有没有盒子」 ★★

           实测（PCAN-USB + PEAK 驱动 4.2.1，硬件确实可用）：
             CAN_GetValue(USBBUS1, CHANNEL_CONDITION) -> 0x8000, cond=0
             CAN_Initialize(USBBUS1, 500k)            -> 0x0 成功
           按 cond 判断会把一块好盒子判成「不可用」。

           所以改成：直接拿 CAN_Initialize 当可用性探针。
             能初始化   = 通道真的在
             初始化失败 = 没插 / 驱动没装 / 被别的程序占用
           探完立刻 CAN_Uninitialize，不影响后面 ZCANPRO 的正常 InitCAN。

           为什么这里用 500k 试：
             此时 ZCANPRO 还没发 SetReference（波特率在 InitCAN 里才知道），
             但“能不能打开”和波特率无关，任意合法波特率都能验证硬件在不在。
         */
        int k, total = 0, first = -1;
        DWORD lastErr = 0;
        if (!pPcanInit) {
            LogF("  ★ PCAN 缺 CAN_Initialize 导出");
            goto L_FAIL;
        }
        for (k = 0; k < VCI_MAX_CHANNEL; k++) {
            TPCANHandle hch = (TPCANHandle)(PCAN_USBBUS1 + k);
            DWORD r = pPcanInit(hch, PCAN_BAUD_500K, 0, 0, 0);
            if (r == PCAN_ERROR_OK) {
                if (pPcanUninit) pPcanUninit(hch);
                total++;
                if (first < 0) first = k;
            } else {
                lastErr = r;
            }
        }
        LogF("  PCAN 探到 %d 路可用（最后一个失败码 0x%X）",
             total, (unsigned)lastErr);
        if (total == 0) {
            if (lastErr == PCAN_ERROR_NODRIVER)
                LogF("  ★ PEAK 内核驱动没加载（装 PEAK 官方驱动）");
            else if (lastErr == PCAN_ERROR_HWINUSE)
                LogF("  ★ 通道被别的程序占着（PCAN-View 之类）");
            else
                LogF("  ★ PEAK 没插盒子 / 驱动没装");
            goto L_FAIL;
        }
        if (first > 0) g_pcanCh = (TPCANHandle)(PCAN_USBBUS1 + first);
        g_realChans = total;
        g_opened = 1;
        break;
    }
    case BE_TOSUN: {
        int rc;
        if (!g_tsInited) {
            rc = pTsInit("UniVciBridge");
            LogF("  TOSUN initialize_lib_tsmaster -> %d", rc);
            g_tsInited = 1;
        }
        rc = pTsConnect();
        LogF("  TOSUN tsapp_connect -> %d", rc);
        if (rc != 0) { goto L_FAIL; }
        if (pTsChanCount) {
            int nch = 0;
            pTsChanCount(&nch);
            LogF("  TOSUN 通道数 %d", nch);
            g_realChans = (nch > 0 && nch <= VCI_MAX_CHANNEL) ? nch : 1;
        } else {
            g_realChans = 1;
        }
        g_opened = 1;
        break;
    }
    case BE_KVASER: {
        int h = -1, i;

        /* 先试真实通道（flags=0） */
        for (i = 0; i < 8; i++) {
            h = pKOpen(i, 0);
            LogF("  Kvaser canOpenChannel(%d, 0) -> %d", i, h);
            if (h >= 0) break;
            /* -8 = canERR_NOTINITIALIZED：Kvaser 驱动没装，再试也没用 */
            if (h == -8) {
                LogF("  ★ canlib 未初始化 —— Kvaser 驱动没装。");
                LogF("    光有 canlib32.dll 不够，必须装 Kvaser 官方驱动包");
                LogF("    （Kvaser Drivers for Windows），否则 -8 一直存在");
            goto L_FAIL;
            }
        }

        /* 没真实通道就试着开虚拟通道（方便无硬件时自测） */
        if (h < 0) {
            for (i = 0; i < 4; i++) {
                h = pKOpen(i, K_OPEN_ACCEPT_VIRTUAL);
                LogF("  Kvaser canOpenChannel(%d, ACCEPT_VIRTUAL) -> %d", i, h);
                if (h >= 0) break;
            }
        }

        if (h < 0) {
            LogF("  ★ 没有任何 Kvaser 通道可打开（没插盒子 或 驱动没装）");
            goto L_FAIL;
        }
        g_kHandle = h;
        g_realChans = 1;
        if (pKChanCount) {
            int nch = 0;
            if (pKChanCount(&nch) == 0 && nch > 0 && nch <= VCI_MAX_CHANNEL)
                g_realChans = nch;
        }
        LogF("  Kvaser 已打开 handle=%d，实际通道 %d 路", h, g_realChans);
        g_opened = 1;
        break;
    }
    case BE_VECTOR: {
        /*
         Vector 的通道由 channelMask 标识，不是从 0 开始的序号。
         取第一个可用通道的 mask 打开一个 port，rxQueue 给 8192。
        */
        XLaccess perm = 0;
        XLstatus st;
        XLaccess mask;

        if (!g_xlDriverOpen) {
            st = pXlOpen();
            LogF("  Vector xlOpenDriver -> %lu", (unsigned long)st);
            if (st != XL_SUCCESS) { goto L_FAIL; }
            g_xlDriverOpen = 1;
            {
                XLdriverConfig *cfg = (XLdriverConfig *)calloc(1, sizeof(XLdriverConfig));
                if (cfg) {
                    if (pXlCfg(cfg) == XL_SUCCESS && cfg->channelCount <= 64) {
                        DWORD i;
                        g_xlAvailMask = 0;
                        g_xlChanCount = (int)cfg->channelCount;
                        for (i = 0; i < cfg->channelCount; i++)
                            g_xlAvailMask |= cfg->channel[i].channelMask;
                    }
                    free(cfg);
                }
            }
        }

        if (!g_xlAvailMask) {
            LogF("  ★ Vector 没有可用通道（没接硬件）");
            goto L_FAIL;
        }
        /*
         ★ 逐个通道试，而不是只试最低位那一个。

           原因（VN1630A 实测）：一块 VN1630 有 4 个 CAN 口，
           其中某个口可能已经被别的程序（CANoe / CANape / 上次残留）
           占着 init 权限，这时 perm 会是 0。只试第一个口就判失败
           太武断，换一个口往往就能用。

           每一步都先 xlSetApplConfig 把口分配给本应用 ——
           Vector 的规矩：没分配过就永远拿不到 perm。
        */
        {
            /*
             ★ 三层重试，把"猜"变成"试"：

               手册说 xlInterfaceVersion 要传"当前 API 版本"，但 vxlapi.h
               不随驱动发布，我们原来把 XL_INTERFACE_VERSION_V3 当成 3 ——
               这是猜的。真驱动报出来的 channel[].interfaceVersion 是 4，
               所以 3 很可能不对。与其继续猜，不如挨个试。

               第 1 层：每个通道 × 4 个版本号
               第 2 层：整轮失败后重启驱动再来一轮
                        （有些驱动版本要在 xlCloseDriver/xlOpenDriver 之后
                          才认 xlSetApplConfig 写进去的通道分配）

               哪一步拿到 perm != 0 就用哪一步，全部记进日志。
             */
            static const unsigned int kVers[4] = { 3, 4, 0x0400, 0x0300 };
            int pass, ci, vi, tried = 0, gotPort = 0;

            st = XL_ERR_NOT_IMPLEMENTED;
            for (pass = 0; pass < 2 && !gotPort; pass++) {
                if (pass == 1) {
                    LogF("  Vector: 第 1 轮全失败，重启驱动后再试一轮");
                    if (pXlClose) pXlClose();
                    g_xlDriverOpen = 0;
                    Sleep(250);
                    if (pXlOpen() == XL_SUCCESS) {
                        g_xlDriverOpen = 1;
                    } else {
                        LogF("  Vector: 重开驱动失败，放弃");
                        break;
                    }
                }

                for (ci = 0; ci < g_xlCanN && !gotPort; ci++) {
                    XLaccess one = g_xlCan[ci].mask;

                    if (!(g_xlAvailMask & one)) continue;

                    /* 用户指定了通道就只试那一个（按 hwChannel+1 对齐 VN1630A 的"Channel N"命名） */
                    if (g_xlWantChan && (int)g_xlCan[ci].hwChannel + 1 != g_xlWantChan) {
                        if (pass == 0)
                            LogF("  Vector 跳过 VN1630A Channel %u（用户指定第 %d 路）",
                                 g_xlCan[ci].hwChannel + 1, g_xlWantChan);
                        continue;
                    }

                    if (pass == 0) tried++;

                    if (pXlSetAppl) {
                        XLstatus sa = pXlSetAppl(g_xlAppName, 0,
                                                 g_xlCan[ci].hwType,
                                                 g_xlCan[ci].hwIndex,
                                                 g_xlCan[ci].hwChannel,
                                                 XL_BUS_TYPE_CAN);
                        LogF("  Vector xlSetApplConfig(%s, ch0, hwType=%u, hwIndex=%u, hwChannel=%u) -> %lu",
                             g_xlAppName, g_xlCan[ci].hwType, g_xlCan[ci].hwIndex,
                             g_xlCan[ci].hwChannel, (unsigned long)sa);
                    }

                    for (vi = 0; vi < 4 && !gotPort; vi++) {
                        XLportHandle ph = 0;
                        /*
                         ★★ permissionMask 是 in/out 参数 ★★

                           手册示例写得很清楚：
                               xlPermissionMask = m_xlChannelMask;   ← 调用前先填
                               xlOpenPort(..., &xlPermissionMask, ...);

                           它既是"我要申请哪些通道"，也是"实际给了你哪些"。
                           一直传 0 进去，等于一个通道都不申请 ——
                           驱动就老老实实返回 perm=0，而 status 仍然是 0，
                           表现成"能打开、没权限"。

                           这就是 32 次尝试全部 perm=0 的真正原因：
                           不是版本号错，也不是通道被占，是我们把入参填成了 0。
                         */
                        XLaccess p2 = one;
                        XLstatus r = pXlOpenPort(&ph, g_xlAppName, one, &p2, 8192,
                                                 kVers[vi], XL_BUS_TYPE_CAN);
                        LogF("  Vector xlOpenPort(mask=0x%I64X, ver=%u) -> %lu port=%lu perm_in=0x%I64X perm_out=0x%I64X",
                             (unsigned long long)one, kVers[vi],
                             (unsigned long)r, (unsigned long)ph,
                             (unsigned long long)one, (unsigned long long)p2);
                        /*
                         ★ 判据只看 perm，不看句柄是否为 0。

                           实测反证（本机 virtual channel）：
                             port=0 + perm=1 → 波特率设置 OK、激活 OK
                             port=1 + perm=0 → 波特率返回 255(XL_ERROR)
                           说明真正被授权的就是第一次那个 port=0；
                           Vector 这里允许句柄为 0。硬要求非 0 反而把好的扔掉。
                         */
                        if (r == XL_SUCCESS && p2 != 0) {
                            mask = one;
                            perm = p2;
                            g_xlPort = ph;
                            st = r;
                            gotPort = 1;
                            LogF("  ★ Vector 拿到 init 权限：mask=0x%I64X ver=%u port=%lu"
                                 " → 用的是 VN1630A Channel %u",
                                 (unsigned long long)one, kVers[vi], (unsigned long)ph,
                                 g_xlCan[ci].hwChannel + 1);
                            /*
                             这条提示很重要（VN1630A 实测踩到）：
                             前几路被别的程序占着时，桥会自动退到后面有空的口，
                             但用户的 CAN 线可能接在前面的口上 —— 那就会
                             "打开成功、开始接收、一帧没有"。
                            */
                            if (!g_xlWantChan && ci > 0) {
                                int k;
                                LogF("     ★ 注意：前面的通道被占用了，所以用了第 %u 路。",
                                     g_xlCan[ci].hwChannel + 1);
                                for (k = 0; k < ci; k++)
                                    LogF("       VN1630A Channel %u 没有拿到权限（被占用或未分配）",
                                         g_xlCan[k].hwChannel + 1);
                                LogF("       如果 CAN 线接在别的口上，请二选一：");
                                LogF("         · 跑「释放Vector通道.bat」腾出前几路，或");
                                LogF("         · 在 UniVciBridge.ini 里写 xl_channel=<实际那一路>");
                            }
                        } else {
                            if (pXlClosePort && ph) pXlClosePort(ph);
                            st = r;
                            perm = 0;
                        }
                    }
                }
            }

            if (!gotPort) {
                LogF("  ★ 试了 %d 个 Vector 通道 × 4 个接口版本 × 2 轮，都没拿到 init 权限", tried);
                st = (tried ? XL_ERR_INIT_ACCESS_MISSING : XL_ERR_NO_DATA_DETECTED);
                goto L_FAIL;
            }
        }
        if (st != XL_SUCCESS) { goto L_FAIL; }

        g_xlMask = mask & (perm ? perm : mask);
        if (!g_xlMask) g_xlMask = mask;
        g_xlPortOpen = 1;
        g_realChans = (g_xlChanCount > 0 && g_xlChanCount <= VCI_MAX_CHANNEL)
                      ? g_xlChanCount : 1;
        LogF("  Vector 实际通道 %d 路", g_realChans);
        if (!perm) {
            /*
             ★ 权限为 0 就按失败处理，不能假成功。

               实测日志：
                 Vector xlOpenPort -> 0 port=0 perm=0x0
                 但 VCI_OpenDevice 仍返回 1
               上层程序会以为打开成功，然后发送全军覆没，
               最后把问题归到"桥不好使"。

               perm=0 的官方含义：本程序对该通道没有使用权
               （没在 Vector Hardware Config 里分配，或被别的程序占着）。
               这种情况下 xlCanTransmit 必然失败，没有任何可用性。
             */
            LogF("  ★ Vector 权限为 0，按【失败】处理");
            LogF("     桥已自动调 xlSetApplConfig 把 4 个通道都分配给 \"%s\"，", g_xlAppName);
            LogF("     分配全部成功但仍然拿不到 init 权限 —— 几乎可以肯定：");
            LogF("       通道被【别的程序】占着 init 访问权。");
            LogF("       Vector 规矩：同一个通道只有第一个打开它的应用能拿 init。");
            LogF("       最常见的就是 CANape 的后台录制服务，它一直占着硬件。");
            LogF("");
            LogF("     ★ 处理（按顺序）：");
            LogF("       ① 用管理员运行本目录的「释放Vector通道.bat」");
            LogF("          （它会停掉 CANapeRecordingService 并关掉 CANape/CANoe）");
            LogF("       ② 或者手动：任务管理器里结束 CANapeRecordingService");
            LogF("       ③ 再打开 ZCANPRO 重试");
            LogF("       ④ 还不行 → Vector Hardware Manager 里把通道指派给 \"%s\"", g_xlAppName);
            if (pXlClosePort && g_xlPort) pXlClosePort(g_xlPort);
            g_xlPort = 0;
            g_xlPortOpen = 0;
            g_opened = 0;
            /*
             把后端状态也释放掉：
             不然接下来的 SetReference / InitCAN 会一直走 Vector
             这条死路，真正插上的盒子反而没机会被试到。
             */
            g_backend = BE_NONE;
            strcpy(g_backendName, "none");
            g_probeFailTick = GetTickCount();   /* 3 秒内不重复探测，避免刷屏 */
            LeaveCriticalSection(&g_cs);
            return STATUS_ERR;
        }

        /*
         ★ 这里**不**设波特率、**不**激活通道。
           Vector 要求 xlCanSetChannelBitrate 在通道激活之前调用，
           而 ZCANPRO 的时序是 SetReference(设波特率) → InitCAN → StartCAN。
           所以波特率在 SetReference / InitCAN 里设，激活在 InitCAN 里做。
        */
        g_opened = 1;
        break;
    }
    default: {
        DWORD r;
        /*
         ★型号协商：先试 ZCANPRO 请求的型号，不行再依次试候选。
           周立功 USBCAN_E.DLL 只认 20；创苯只认 3/4/20/21。
         */
        /* 同理：2E-U(21) 优于 E-U(20) */
        static const DWORD kTypeCands[] = { 0, 21, 20, 31, 34, 4, 3, 2, 5, 1 };
        int ti;

        if (g_vciOpened) {
            /* 探测时已经打开过了，直接用，不重复 open */
            r = STATUS_OK;
            LogF("  后端已在探测时打开（型号 %lu），复用",
                 (unsigned long)g_vciRealType);
        } else if (g_vciRealType != 0) {
            r = g_vci.OpenDevice(g_vciRealType, DeviceInd, 0);
            LogF("  后端 OpenDevice(已协商型号 %lu) -> %lu",
                 (unsigned long)g_vciRealType, (unsigned long)r);
            if (r == STATUS_OK) g_vciOpened = 1;
        } else {
            r = g_vci.OpenDevice(real, DeviceInd, 0);
            LogF("  后端 OpenDevice(请求型号 %lu) -> %lu",
                 (unsigned long)real, (unsigned long)r);
            if (r != STATUS_OK) {
                for (ti = 0; ti < (int)(sizeof(kTypeCands) / sizeof(kTypeCands[0])); ti++) {
                    DWORD t = kTypeCands[ti];
                    if (t == 0 || t == real) continue;
                    r = g_vci.OpenDevice(t, DeviceInd, 0);
                    LogF("    试型号 %lu -> %lu", (unsigned long)t, (unsigned long)r);
                    if (r == STATUS_OK) {
                        g_vciRealType = t;
                        g_devType = t;
                        LogF("  ★ 型号协商成功：用 %lu 代替 %lu",
                             (unsigned long)t, (unsigned long)real);
                        break;
                    }
                }
            } else {
                g_vciRealType = real;
                g_vciOpened = 1;
            }
        }
        if (r != STATUS_OK) { goto L_FAIL; }
        /*
         ★ 从 BoardInfo 拿真实通道数，并钳到 1~8。
           广成驱动有时会报 50，不钳位 ZCANPRO 会越界。
         */
        {
            /*
             ★ 通道数优先从型号推（可靠），BoardInfo 只做兜底。
               用户规则：1 口 → 1 路；≥ 2 口 → 2 路；多余不用；不超过桥上限。
             */
            int byType = ChannelsOfType(g_vciRealType ? g_vciRealType : real);
            VCI_BOARD_INFO bi2;
            DWORD biNum = 0;
            int byBoard = 0;

            if (g_vci.ReadBoardInfo) {
                ZeroMemory(&bi2, sizeof(bi2));
                if (g_vci.ReadBoardInfo(real, DeviceInd, &bi2) == STATUS_OK) {
                    biNum = bi2.can_Num;
                    if (bi2.can_Num >= 1 && bi2.can_Num <= VCI_MAX_CHANNEL)
                        byBoard = (int)bi2.can_Num;
                }
            }

            if (byType > 0) {
                g_realChans = byType;
                LogF("  通道数：按型号 %lu 推出 %d 路"
                     "（BoardInfo 报 %lu，已忽略）",
                     (unsigned long)(g_vciRealType ? g_vciRealType : real),
                     byType, (unsigned long)biNum);
            } else if (byBoard > 0) {
                g_realChans = byBoard;
                LogF("  通道数：型号未知，用 BoardInfo %lu → %d 路",
                     (unsigned long)biNum, byBoard);
            } else {
                g_realChans = 1;
                LogF("  通道数：型号与 BoardInfo 都不可靠，保守取 1 路");
            }
        }
        g_opened = 1;
        if (g_pendingRef && g_vci.SetReference) {
            DWORD r2 = g_vci.SetReference(real, DeviceInd, 0, 0, &g_pendingRefCode);
            LogF("  补发 SetReference(0x%X) -> %lu",
                 (unsigned)g_pendingRefCode, (unsigned long)r2);
            if (r2 == STATUS_OK) g_pendingRef = 0;
        }
        break;
    }
    }

    LeaveCriticalSection(&g_cs);
    return STATUS_OK;

L_FAIL:
    /*
     ★ 智能回退：当前后端打不开盒子（没插 / 驱动没装 / 型号不对），
       就退回原厂 USBCAN.dll（安装时备份成 USBCAN.dll.zlg.bak）。
       于是插真周立功盒子时行为和安装前完全一致 ——
       不会因为装了桥而破坏厂家自己的软件。
    */
    LogF("  ↓ 后端 %s 打不开，尝试回退原厂 DLL", g_backendName);
    if (TryOriginalPassthrough()) {
        DWORD r3 = g_vci.OpenDevice(real, DeviceInd, 0);
        LogF("  原厂 OpenDevice -> %lu", (unsigned long)r3);
        if (r3 == STATUS_OK) {
            g_opened = 1;
            g_devType = real;
            LeaveCriticalSection(&g_cs);
            return STATUS_OK;
        }
    }
    LogF("  ★ 回退也失败 —— 这块盒子用不了");
    /* 光说"用不了"没意义，要告诉用户查什么。
       这个项目最大的使用障碍不是"打不开"，而是用户不知道打不开该干嘛。 */
    LogLine("");
    LogLine("  ┌──────────────────────────────────────────────────────────┐");
    LogLine("  │ 盒子和驱动都正常时，绝不会所有型号都返回 0。请依次检查：   │");
    LogLine("  └──────────────────────────────────────────────────────────┘");
    LogLine("    ① USB 线插紧了吗？换个 USB 口试试（优先主板后置口，别用扩展坞）");
    LogLine("    ② 设备管理器里能看到它吗？");
    LogLine("         通用串行总线设备 → ZLG USBCAN / USBCAN_2E_U / PCAN-USB");
    LogLine("       有黄色感叹号 = 驱动没装好；完全看不到 = 盒子没被识别");
    LogLine("    ③ 厂家驱动装了吗？");
    LogLine("         周立功 → USBCAN 全系列驱动（服务名 ZLGCAN / USBCAN_E）");
    LogLine("         PEAK   → PCAN-Basic + PCAN 驱动（服务名 PCAN_USB）");
    LogLine("    ④ 盒子是不是被别的软件占着？");
    LogLine("         一个 CAN 盒子同时只能被一个进程打开。先关掉");
    LogLine("         ZCANPRO / CANTest / CANoe / CANape，或拔插一次盒子");
    LogLine("    ⑤ 换一根 USB 线 —— 劣质线供电不足，最常见也最容易被忽略");
    LogLine("");
    LogLine("  怎么快速判断盒子在不在：");
    LogLine("     设备管理器 → 查看 → 显示隐藏的设备 → 通用串行总线设备");
    LogLine("     同名设备如果是灰色（未连接），就是没插或线坏了");
    LeaveCriticalSection(&g_cs);
    return STATUS_ERR;
}

__declspec(dllexport) DWORD __stdcall VCI_CloseDevice(DWORD DeviceType, DWORD DeviceInd)
{
    g_probeFailTick = 0;   /* 关设备后允许下次重新探测 */
    EnterCriticalSection(&g_cs);
    LogF("VCI_CloseDevice: %lu", (unsigned long)DeviceType);

    /*
     ★ 和 DllMain 的 DETACH 走同一条释放路径（ReleaseHardware）。
       这样「ZCANPRO 关闭设备」和「ZCANPRO 整个退出」两种情况
       都会把 PCAN/Kvaser/Vector 通道真正放开，
       PCAN-View 之类的厂商软件随后就能直接用，不用重启电脑。
    */
    if (g_backend == BE_TOSUN) {
        if (pTsDisc) pTsDisc();          /* 先断开，再在 ReleaseHardware 里 finalize */
        ReleaseHardware();
    } else if (g_backend == BE_GCAN || g_backend == BE_VCI) {
        if (g_vci.CloseDevice) {
            g_vci.CloseDevice(g_devType, DeviceInd);
            LogF("  后端 CloseDevice 已调用");
        }
    } else {
        ReleaseHardware();
    }

    g_opened = 0;
    g_kHandle = -1;
    ZeroMemory(g_started, sizeof(g_started));

    /*
     ★★ 关键：一定要把「VCI 已经打开」的标记清掉 ★★

       真机日志（16:05）暴露的 bug：
           [16:05:16] VCI_CloseDevice: 4
           [16:05:18] VCI_OpenDevice: 请求 4 -> 实际 21
           [16:05:18]   后端已在探测时打开（型号 21），复用   ← 这句是错的
           [16:05:19]   后端 InitCAN -> 0                    ← 拿已关闭的句柄去初始化

       因为 CloseDevice 没清 g_vciOpened，下次 OpenDevice 就以为还开着，
       直接复用 → 用已经关掉的句柄去 InitCAN → 返回 0，设备就打不开了。

       同时清掉协商出来的型号和通道数，让下次 Open 重新探测一遍。
     */
    g_vciOpened   = 0;
    g_vciRealType = 0;
    g_realChans   = 0;
    g_probeFailTick = 0;

    LeaveCriticalSection(&g_cs);
    return STATUS_OK;
}

/*
 ★ 关键：波特率设置。
   RefType==0 时 pData 指向 ZLG 波特率常量（如 0x060007 = 500K）。
   必须在 VCI_InitCAN 之前调用（USBCAN-2E-U 的硬性要求）。

   底层还没 OpenDevice 时会拒绝 —— 这时缓存下来、Open 后补发，
   让调用顺序不再敏感。这是稳定性上最关键的一处兜底。
*/
__declspec(dllexport) DWORD __stdcall VCI_SetReference(DWORD DeviceType,
                                                       DWORD DeviceInd,
                                                       DWORD CANInd,
                                                       DWORD RefType,
                                                       PVOID pData)
{
    DWORD rate = 0;

    EnterCriticalSection(&g_cs);
    SelectBackend();

    if (RefType == 0 && pData) {
        DWORD code = *(DWORD *)pData;
        rate = ZlgCodeToRate(code);
        if (rate) {
            g_bitrate = rate;
            LogF("VCI_SetReference: 常量 0x%X -> %lu bps",
                 (unsigned)code, (unsigned long)rate);
        } else {
            LogF("VCI_SetReference: 常量 0x%X 不在表里，原样透传（不改波特率）",
                 (unsigned)code);
        }
    }

    switch (g_backend) {
    case BE_PCAN:
    case BE_TOSUN:
    case BE_KVASER:
        /* 这几个后端在 InitCAN 里自己设，这里只记录 */
        LeaveCriticalSection(&g_cs);
        return STATUS_OK;

    case BE_VECTOR:
        /*
         Vector 必须在激活通道之前设波特率；port 已开就立刻设。
         ★ 这时通道通常还没激活，xlCanSetChannelBitrate 可以成功。
           真失败（例如没权限）只告警 —— 留给 InitCAN 再试一次，
           最终以「能激活」为准。
        */
        if (g_xlPortOpen && pXlSetBitrate) {
            XLstatus xs = pXlSetBitrate(g_xlPort, g_xlMask, (DWORD)g_bitrate);
            if (xs == XL_SUCCESS)
                LogF("  Vector 预置波特率 %lu bps -> OK", (unsigned long)g_bitrate);
            else
                LogF("  Vector 预置波特率 %lu bps -> %lu (稍后在 InitCAN 再试)",
                     (unsigned long)g_bitrate, (unsigned long)xs);
        }
        LeaveCriticalSection(&g_cs);
        return STATUS_OK;

    default: {
        if (g_vci.SetReference) {
            DWORD cnt = 1;
            DWORD real = MapDevType(DeviceType, &cnt);
            DWORD r = g_vci.SetReference(real, DeviceInd, CANInd, RefType, pData);
            LogF("  后端 SetReference -> %lu", (unsigned long)r);
            if (r == STATUS_OK) {
                g_pendingRef = 0;
            } else if (RefType == 0 && pData) {
                g_pendingRefCode = *(DWORD *)pData;
                g_pendingRef = 1;
                LogF("  ★ 后端拒绝（多半还没 OpenDevice）→ 已缓存，等 Open 后补发");
                LeaveCriticalSection(&g_cs);
                return STATUS_OK;
            }
            LeaveCriticalSection(&g_cs);
            return r;
        }
        LeaveCriticalSection(&g_cs);
        return STATUS_OK;
    }
    }
}

__declspec(dllexport) DWORD __stdcall VCI_GetReference(DWORD DeviceType,
                                                       DWORD DeviceInd,
                                                       DWORD CANInd,
                                                       DWORD RefType,
                                                       PVOID pData)
{
    DWORD r = STATUS_ERR;

    EnterCriticalSection(&g_cs);
    SelectBackend();

    if (g_backend == BE_GCAN || g_backend == BE_VCI) {
        if (g_vci.GetReference)
            r = g_vci.GetReference(DeviceType, DeviceInd, CANInd, RefType, pData);
    } else if (RefType == 0 && pData) {
        *(DWORD *)pData = g_bitrate;
        r = STATUS_OK;
    }


    LeaveCriticalSection(&g_cs);
    return r;
}

__declspec(dllexport) DWORD __stdcall VCI_InitCAN(DWORD DeviceType,
                                                  DWORD DeviceInd,
                                                  DWORD CANInd,
                                                  VCI_INIT_CONFIG *pInit)
{
    DWORD cnt = 1, real, rate;

    EnterCriticalSection(&g_cs);
    SelectBackend();
    real = MapDevType(DeviceType, &cnt);

    {
        /*
         ★ 统一用「有效通道数」判定：
             1 口 → 1 路，≥ 2 口 → 2 路，多余不用，且不超过型号上限。
         */
        int eff = EffectiveChans(cnt);
        if (CANInd >= (DWORD)eff) {
            LogF("VCI_InitCAN: 通道 %lu 不对外暴露"
                 "（有效 %d 路 = 硬件 %d 路与型号 %lu 路取小，封顶 2）",
                 (unsigned long)CANInd, eff, g_realChans, (unsigned long)cnt);
            LeaveCriticalSection(&g_cs);
            return STATUS_ERR;
        }
        if (CANInd == 0)
            LogF("  对外暴露 %d 路（硬件 %d 路，型号上限 %lu 路）",
                 eff, g_realChans, (unsigned long)cnt);
    }
    /*
     ★ 这一条是「报最大通道数」能安全工作的关键：
       ZCANPRO 会为 8 路每一路都调 InitCAN。实际硬件只有 1~2 路时，
       多出来的那几路必须干净地报错，而不能去调后端
       （否则 8 路会全部指向同一个物理通道，数据串到一起）。
     */
    /* （已并入上面的 EffectiveChans 检查，不再重复） */

    /*
     ★ 定波特率。注意不能用 pInit->Timing0/Timing1：

       上层传的是 【 16 字节布局】而桥的结构体是 18 字节，
       pInit->Timing0 实际指向 pB[16]（越过 16 字节的栈垃圾）。
       真实的 timing 在 q[13]/q[14]。
     */
    /*
     ★ 优先级：上层 timing 字节 > 内部 g_bitrate > 默认 500k

       为什么上层优先：ZCANPRO 的「波特率」下拉框在 USBCAN-II 路径上，
       就是把用户选的值换算成 Timing0/Timing1 填进结构体。
       之前先取 g_bitrate（初值非 0），永远走不到字节解析 ——
       等于用户在界面上把波特率改成 250k，桥照样按 500k 去设，白改。
     */
    rate = 0;
    if (pInit) {
        const unsigned char *q0 = (const unsigned char *)pInit;
        DWORD t = TimingToRate(q0[13], q0[14]);
        if (t) rate = t;
    }
    if (rate == 0) rate = g_bitrate;
    if (rate == 0) rate = 500000;      /* 都没线索时按最常见的来 */
    g_bitrate = rate;

    VciRxReset((int)CANInd);

    /*
     ★ 把转发给厂家库的完整配置记下来。

       滤波配错会 100% 收不到帧（AccMask=0 就是全报文都被拦），
       而这个值是上层给的、桥只负责转发，所以必须记下来才能对账。
     */
    /*
     ★ 按【 16 字节布局】解释（真机反推确认）：
         AccCode[0..3] AccMask[4..7] Reserved[8..11]
         Filter[12]    Timing0[13]   Timing1[14] Mode[15]

       之前按 18 字节读，得到的 Filter=28、timing=00/88、Mode2=121
       全是错位产物，会把排查带偏。现在两种解释都打出来。
     */
    if (pInit) {
        const unsigned char *q = (const unsigned char *)pInit;
        DWORD ac  = (DWORD)q[0] | ((DWORD)q[1] << 8) | ((DWORD)q[2] << 16) | ((DWORD)q[3] << 24);
        DWORD am  = (DWORD)q[4] | ((DWORD)q[5] << 8) | ((DWORD)q[6] << 16) | ((DWORD)q[7] << 24);
        LogF("  InitCAN 配置（16字节布局）: "
             "AccCode=0x%lX AccMask=0x%lX Filter=%u Timing=%02X/%02X Mode=%u",
             (unsigned long)ac, (unsigned long)am,
             (unsigned)q[12], q[13], q[14], q[15]);
        if (q[15] == 1)
            LogF("  ★ 警告：Mode=1（只听模式）—— 只听不发 ACK，"
                 "总线会报错，很可能就是收不到的原因");
        if (am == 0)
            LogF("  ★ 警告：AccMask=0 表示拒收所有报文");
    }
    /*
     ★ 把上层传进来的原始字节打出来（18 字节）。

       周立功的 VCI_INIT_CONFIG 有 18 字节（经典）和 16 字节（E-U）两种布局。
       到底 ZCANPRO 用的是哪种，看这串字节就能定死 —— 不用再猜。
       500k 的时序字节是 00 1C：
         出现在偏移 12/13 → 16 字节布局
         出现在偏移 14/15 → 18 字节布局
     */
    if (pInit) {
        const unsigned char *pb = (const unsigned char *)pInit;
        LogF("  InitCAN 原始字节: %02X %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             pb[0], pb[1], pb[2], pb[3], pb[4], pb[5], pb[6], pb[7], pb[8],
             pb[9], pb[10], pb[11], pb[12], pb[13], pb[14], pb[15],
             pb[16], pb[17]);
        if (pb[13] == 0x00 && pb[14] == 0x1C)
            LogF("  → 16 字节 E-U 布局（字节 12/13 = 00/1C = 500k）");
        else if (pb[12] == 0x00 && pb[13] == 0x1C)
            LogF("  → 18 字节经典布局（字节 14/15 = 00/1C = 500k）");
        else
            LogF("  → 两种布局都对不上 500k 时序（可能不是 500k，或上层没填时序）");
    }

    if (pInit && pInit->AccMask == 0)
        LogF("  ★ 警告：AccMask=0 表示拒收所有报文！这会导致一帧都收不到。");

    LogF("VCI_InitCAN: 类型 %lu 通道 %lu timing=%02X/%02X -> %lu bps 后端 %s",
         (unsigned long)real, (unsigned long)CANInd,
         pInit ? pInit->Timing0 : 0, pInit ? pInit->Timing1 : 0,
         (unsigned long)rate, g_backendName);

    switch (g_backend) {
    case BE_PCAN: {
        TPCANBaudrate pb;
        DWORD st;
        if (!g_opened) { LeaveCriticalSection(&g_cs); return STATUS_ERR; }
        switch (rate) {
            case 1000000: pb = 0x0014; break;
            case  800000: pb = 0x0016; break;
            case  500000: pb = 0x001C; break;
            case  250000: pb = 0x011C; break;
            case  125000: pb = 0x031C; break;
            case  100000: pb = 0x432F; break;
            case   50000: pb = 0x472F; break;
            case   20000: pb = 0x532F; break;
            case   10000: pb = 0x672F; break;
            case    5000: pb = 0x7F7F; break;
            default:      pb = 0x001C; break;
        }
        if (g_started[CANInd] && pPcanUninit) pPcanUninit(g_pcanCh);
        st = pPcanInit((TPCANHandle)(g_pcanCh + CANInd), pb, 0, 0, 0);
        LogF("  PCAN CAN_Initialize(%lu, 0x%X) -> 0x%X",
             (unsigned long)rate, (unsigned)pb, (unsigned)st);
        if (st != 0) { LeaveCriticalSection(&g_cs); return STATUS_ERR; }
        /*
         ★ 可选：打开 PCAN 回声帧（ini 里写 pcan_echo=1）。

           目的：单块 PCAN 盒子时尝试验证桥的【接收】方向。
                 平时没有第二个节点 ACK，发送失败、接收永远 0，
                 接收路径就没法验。

           官方依据（PEAK AllowEchoFrames 文档）：
             · PCAN-Basic >= 4.6.0
             · PEAK 驱动 >= 4.3.0
             · PCAN_ALLOW_ECHO_FRAMES (=0x2C)，必须在 Initialize 之后设置

           ⚠ 但别把它当万能：多数控制器的「自接收」是在发送成功之后
             才回灌接收队列。总线没对端 ACK 时 CAN_Write 本身就失败，
             可能压根不产生回声。所以这只是「试一下」。
             真正验证接收仍然需要第二个节点。
         */
        if (g_pcanEcho && pPcanSetVal) {
            DWORD on = 1;
            DWORD ev = pPcanSetVal((TPCANHandle)(g_pcanCh + CANInd),
                                   0x2C /* PCAN_ALLOW_ECHO_FRAMES */,
                                   &on, (DWORD)sizeof(on));
            if (ev == 0)
                LogF("  PCAN 回声帧已开启（自己发的帧会出现在接收里）");
            else
                LogF("  ★ PCAN 回声帧开启失败 0x%X"
                     "（需 PCAN-Basic>=4.6 + PEAK 驱动>=4.3）", (unsigned)ev);
        }
        g_started[CANInd] = 1;
        LeaveCriticalSection(&g_cs);
        return STATUS_OK;
    }
    case BE_TOSUN: {
        int rc;
        if (!g_opened || !pTsCfgBaud) { LeaveCriticalSection(&g_cs); return STATUS_ERR; }
        rc = pTsCfgBaud((int)CANInd, (float)((double)rate / 1000.0), 0, 0);
        LogF("  TOSUN configure_baudrate(ch=%lu, %lu kbps) -> %d",
             (unsigned long)CANInd, (unsigned long)(rate / 1000), rc);
        if (rc != 0) { LeaveCriticalSection(&g_cs); return STATUS_ERR; }
        g_started[CANInd] = 1;
        LeaveCriticalSection(&g_cs);
        return STATUS_OK;
    }
    case BE_KVASER: {
        int rc;
        if (g_kHandle < 0 || !pKSetBus) { LeaveCriticalSection(&g_cs); return STATUS_ERR; }
        /* 7 个参数：hnd, freq, tseg1, tseg2, sjw, noSamp, syncmode
           tseg1=tseg2=0 表示让 canlib 按 freq 自动算时序 */
        rc = pKSetBus(g_kHandle, (long)rate, 0, 0, 0, 1, 0);
        LogF("  Kvaser canSetBusParams(%lu) -> %d", (unsigned long)rate, rc);
        g_started[CANInd] = 1;
        LeaveCriticalSection(&g_cs);
        return STATUS_OK;
    }
    case BE_VECTOR: {
        XLstatus xs;
        if (!g_opened || !g_xlPortOpen) { LeaveCriticalSection(&g_cs); return STATUS_ERR; }

        /* 1) 已激活的话先取消（设波特率要求未激活） */
        if (g_started[CANInd] && pXlDeactivate) pXlDeactivate(g_xlPort, g_xlMask);

        /*
         2) 设波特率。
         ★ 注意：没拿到通道权限时 xlCanSetChannelBitrate 会返回
           XL_ERROR(255)。这时通道仍能激活，用的是驱动里已配置的波特率。
           这里只告警、不硬失败 —— 否则 ZCANPRO 会直接打不开设备。
           真插了 Vector 硬件并且分配了通道，这一步会正常成功。
        */
        xs = pXlSetBitrate(g_xlPort, g_xlMask, (DWORD)rate);
        if (xs == XL_SUCCESS) {
            LogF("  Vector xlCanSetChannelBitrate(%lu) -> OK",
                 (unsigned long)rate);
        } else {
            LogF("  ★ Vector xlCanSetChannelBitrate(%lu) -> %lu (失败)",
                 (unsigned long)rate, (unsigned long)xs);
            LogF("    通道将使用驱动里已配置的波特率；"
                 "要在 Vector Hardware Config 里分配本通道并设好波特率");
        }

        /* 3) 激活首通道 */
        if (CANInd == 0) {
            xs = pXlActivate(g_xlPort, g_xlMask, XL_BUS_TYPE_CAN,
                             XL_ACTIVATE_RESET_CLOCK);
            LogF("  Vector xlActivateChannel -> %lu", (unsigned long)xs);
            if (xs != XL_SUCCESS) { LeaveCriticalSection(&g_cs); return STATUS_ERR; }
        }
        g_started[CANInd] = 1;
        LeaveCriticalSection(&g_cs);
        return STATUS_OK;
    }
    default: {
        DWORD r;
        if (!g_opened) { LeaveCriticalSection(&g_cs); return STATUS_ERR; }

        /*
         ★ E-U 系列（20/21/31/34）要用 16 字节布局，不能直接转发上层的 18 字节结构体。
           并且不依赖上层给的滤波值（它以为自己在配 USBCAN-II），
           直接用【全通过】滤波 + 按波特率算出的时序。
         */
        /*
         ★ 默认（g_euLayout=0）直接转发上层配置 —— 和原厂路径一致。
           真机证实：卸载桥之后 ZCANPRO 能收到报文，说明原厂的参数是对的。
           只有在 ini 里写 eu_layout=1 时才用旧的「自己构造」行为。
         */
        /*
         ★★ E-U 系列（20/21/31/34）必须先设波特率 ★★

           周立功官方要求：在 VCI_InitCAN 之前先调 VCI_SetReference 设波特率。
           而 ZCANPRO 以为自己在配经典 USBCAN-II（靠 timing 字节），
           实测它根本没调 SetReference —— 于是 2E-U 的波特率从头到尾没设过，
           表现就是能开、能启动、但总线报位/填充错误、一帧收不到。

           这里桥自己补上这一步（重复设置波特率是幂等的，无副作用）。
         */
        if ((real == 20 || real == 21 || real == 31 || real == 34) && g_vci.SetReference) {
            DWORD code = RateToZlgCode(rate);
            if (code) {
                DWORD rs = g_vci.SetReference(real, DeviceInd, CANInd, 0, &code);
                LogF("  E-U 系列需先设波特率（官方要求）: "
                     "VCI_SetReference(%lu bps = 0x%X) -> %lu",
                     (unsigned long)rate, (unsigned)code, (unsigned long)rs);
            }
        }

        if (g_euLayout && (real == 20 || real == 21 || real == 31 || real == 34)) {
            VCI_INIT_CONFIG_EU eu;
            BYTE t0 = 0x00, t1 = 0x1C;

            RateToTiming(rate, &t0, &t1);
            ZeroMemory(&eu, sizeof(eu));
            eu.AccCode = 0;
            eu.AccMask = 0xFFFFFFFF;     /* 全通过，不拦任何帧 */
            eu.Reserved = 0;
            eu.Filter = 1;
            eu.Timing0 = t0;
            eu.Timing1 = t1;
            eu.Mode = 0;                 /* 0 = 正常模式 */

            LogF("  E-U 布局（16 字节）: AccMask=0xFFFFFFFF Filter=1 timing=%02X/%02X mode=0",
                 t0, t1);
            r = g_vci.InitCAN(real, DeviceInd, CANInd, (VCI_INIT_CONFIG *)&eu);
            LogF("  后端 InitCAN(EU) -> %lu", (unsigned long)r);

            if (r != STATUS_OK) {
                /* 不行，回退到原来的转发方式，至少不比之前更差 */
                LogF("  ★ E-U 布局失败，回退到转发上层配置");
                r = g_vci.InitCAN(real, DeviceInd, CANInd, pInit);
                LogF("  后端 InitCAN(回退) -> %lu", (unsigned long)r);
            }
        } else {
            r = g_vci.InitCAN(real, DeviceInd, CANInd, pInit);
            LogF("  后端 InitCAN -> %lu", (unsigned long)r);
        }

        if (r == STATUS_OK) g_started[CANInd] = 1;
        LeaveCriticalSection(&g_cs);
        return r;
    }
    }
}

__declspec(dllexport) DWORD __stdcall VCI_StartCAN(DWORD DeviceType, DWORD DeviceInd,
                                                   DWORD CANInd)
{
    DWORD cnt = 1, r = STATUS_OK;
    DWORD real = MapDevType(DeviceType, &cnt);

    EnterCriticalSection(&g_cs);
    if (CANInd >= VCI_MAX_CHANNEL) {
        LeaveCriticalSection(&g_cs);
        return STATUS_ERR;
    }
    switch (g_backend) {
    case BE_PCAN:   r = g_started[CANInd] ? STATUS_OK : STATUS_ERR; break;
    case BE_TOSUN:  r = g_started[CANInd] ? STATUS_OK : STATUS_ERR; break;
    case BE_KVASER: r = (g_kHandle >= 0 && pKBusOn) ?
                        (pKBusOn(g_kHandle) == 0 ? STATUS_OK : STATUS_ERR) : STATUS_ERR;
                    break;
    case BE_VECTOR: r = (g_opened && g_xlPortOpen && g_started[CANInd]) ?
                        STATUS_OK : STATUS_ERR;
                    break;
    default:        if (g_vci.StartCAN)
                        r = g_vci.StartCAN(real, DeviceInd, CANInd);
                    break;
    }
    LogF("VCI_StartCAN: 通道 %lu -> %lu", (unsigned long)CANInd, (unsigned long)r);
    if (r == STATUS_OK) g_started[CANInd] = 1;
    LeaveCriticalSection(&g_cs);
    return r;
}

__declspec(dllexport) DWORD __stdcall VCI_ResetCAN(DWORD DeviceType, DWORD DeviceInd,
                                                   DWORD CANInd)
{
    DWORD cnt = 1, r = STATUS_OK;
    DWORD real = MapDevType(DeviceType, &cnt);

    EnterCriticalSection(&g_cs);
    if (CANInd >= VCI_MAX_CHANNEL) { LeaveCriticalSection(&g_cs); return STATUS_ERR; }
    switch (g_backend) {
    case BE_PCAN:   PcanRxReset((int)CANInd);
                    VciRxReset((int)CANInd);
                    if (pPcanReset) pPcanReset((TPCANHandle)(g_pcanCh + CANInd));
                    g_started[CANInd] = 0; break;
    case BE_TOSUN:  g_started[CANInd] = 0; break;
    case BE_KVASER: if (g_kHandle >= 0 && pKBusOff) pKBusOff(g_kHandle);
                    g_started[CANInd] = 0; break;
    case BE_VECTOR: if (g_xlPortOpen && pXlDeactivate) pXlDeactivate(g_xlPort, g_xlMask);
                    g_started[CANInd] = 0; break;
    default:        if (g_vci.ResetCAN) r = g_vci.ResetCAN(real, DeviceInd, CANInd); break;
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

__declspec(dllexport) DWORD __stdcall VCI_ClearBuffer(DWORD DeviceType,
                                                      DWORD DeviceInd,
                                                      DWORD CANInd)
{
    DWORD cnt = 1, r = STATUS_OK;
    DWORD real = MapDevType(DeviceType, &cnt);

    EnterCriticalSection(&g_cs);
    switch (g_backend) {
    case BE_PCAN: {
        TPCANMsg m;
        TPCANTimestamp ts;
        int guard = 0;
        ZeroMemory(&ts, sizeof(ts));
        while (pPcanRead && guard++ < 2000) {
            if (pPcanRead((TPCANHandle)(g_pcanCh + CANInd), &m, &ts) != 0) break;
        }
        PcanRxReset((int)CANInd);   /* 硬件队列 + 中转缓冲都要清 */
        break;
    }
    case BE_TOSUN:  if (pTsClearRx) pTsClearRx((int)CANInd); break;
    case BE_KVASER: if (g_kHandle >= 0 && pKFlushRx) pKFlushRx(g_kHandle); break;
    case BE_VECTOR: if (g_xlPortOpen && pXlFlushRx) pXlFlushRx(g_xlPort); break;
    default:        if (g_vci.ClearBuffer) r = g_vci.ClearBuffer(real, DeviceInd, CANInd); break;
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

/*==========================================================================
  PCAN 接收缓冲

  PCAN-Basic 没有「查队列深度」的 API，只能读出来自己数。
  如果 GetReceiveNum 像以前那样硬编码返回 1，会让上层（ZCANPRO）
  永远认为有帧，一直空转 Receive —— 白占 CPU，界面还卡。

  所以这里做一个每通道的缓冲：
    PcanPump()     把硬件队列里的帧抽到缓冲
    GetReceiveNum  返回缓冲里的真实数量
    VCI_Receive    从缓冲里取
  两者永远一致。
==========================================================================*/
#define PCAN_RXBUF 1024

static VCI_CAN_OBJ g_pcanRx[VCI_MAX_CHANNEL][PCAN_RXBUF];
static int         g_pcanRxCount[VCI_MAX_CHANNEL];

static void PcanRxReset(int CANInd)
{
    if (CANInd >= 0 && CANInd < VCI_MAX_CHANNEL) g_pcanRxCount[CANInd] = 0;
}

/* 把硬件队列里的帧抽到缓冲，返回缓冲里的总数 */
static int PcanPump(DWORD CANInd)
{
    int k = (int)CANInd;
    int c;

    if (k < 0 || k >= VCI_MAX_CHANNEL) return 0;
    c = g_pcanRxCount[k];
    if (!pPcanRead) return c;

    while (c < PCAN_RXBUF) {
        TPCANMsg m;
        TPCANTimestamp ts;
        ZeroMemory(&ts, sizeof(ts));
        if (pPcanRead((TPCANHandle)(g_pcanCh + CANInd), &m, &ts) != 0) break;
        {
            VCI_CAN_OBJ *o = &g_pcanRx[k][c];
            ZeroMemory(o, sizeof(*o));
            o->ID = m.ID;
            o->TimeStamp = ts.millis;
            o->TimeFlag = 1;
            o->SendType = 0;
            o->RemoteFlag = (m.MSGTYPE & 0x01) ? 1 : 0;
            o->ExternFlag = (m.MSGTYPE & 0x02) ? 1 : 0;
            o->DataLen = m.LEN > 8 ? 8 : m.LEN;
            memcpy(o->Data, m.DATA, 8);
            c++;
        }
    }
    g_pcanRxCount[k] = c;
    return c;
}

/*
 把厂家队列里的帧抽进桥自己的缓冲，返回缓冲里的总帧数。

 注意 WaitTime 一律用 0：这里只想把已经在队列里的帧取出来，
 不能阻塞（阻塞会让上层的轮询卡住）。
 */
static int VciPump(DWORD CANInd, int budget)
{
    int n = 0;
    int ch = (int)CANInd;
    DWORD real;

    if (ch < 0 || ch >= VCI_MAX_CHANNEL) return 0;
    if (!g_vci.Receive || !g_opened) return 0;

    {
        DWORD cnt = 1;
        real = MapDevType(g_devType, &cnt);
    }

    /*
     ★ 对账：先问厂家库「你那边有几帧」。
       它说有帧、而我们 Receive 取不到 → 我们调用方式有问题；
       它说 0 帧 → 硬件/总线/滤波/波特率问题，不是桥的错。
     */
    if (g_vci.GetReceiveNum) {
        static DWORD qN = 0;
        DWORD raw = g_vci.GetReceiveNum(real, 0, CANInd);
        qN++;
        if (raw > 0 && qN <= 40) {
            LogF("  底层报有 %lu 帧（ch=%d）→ 开始取",
                 (unsigned long)raw, ch);
        } else if (raw == 0 && (qN == 50 || qN == 500 || qN == 2000)) {
            LogF("  接收轮询第 %lu 次：底层始终报 0 帧"
                 "（ch=%d）—— 总线没流量、波特率不对、"
                 "或滤波把帧全拦了",
                 (unsigned long)qN, ch);
        }
    }

    while (n < budget && g_vciRxN[ch] < VCI_RXBUF) {
        VCI_CAN_OBJ o;
        DWORD r;
        ZeroMemory(&o, sizeof(o));
        r = g_vci.Receive(real, 0, CANInd, &o, 1, 0);
        if (r != 1) break;
        /* ★ 转成中立模型再存：上层取帧时再转回去 */
        CtFromVciObj(&o, &g_vciRx[ch][g_vciRxN[ch]], ch);
        g_vciRxN[ch]++;
        n++;
    }
    return g_vciRxN[ch];
}

__declspec(dllexport) DWORD __stdcall VCI_GetReceiveNum(DWORD DeviceType,
                                                        DWORD DeviceInd,
                                                        DWORD CANInd)
{
    DWORD cnt = 1, r = 0;
    DWORD real = MapDevType(DeviceType, &cnt);

    EnterCriticalSection(&g_cs);
    if (g_backend == BE_GCAN || g_backend == BE_VCI) {
        /*
         ★ 不再直接信厂家库的计数：先把帧抽进桥缓冲，
           然后报缓冲里的真实数量。这样上层问多少就能取多少。
         */
        r = (DWORD)VciPump(CANInd, 256);
    } else if (g_backend == BE_TOSUN && pTsRxCount) {
        /* TOSUN 能读真实帧数，比固定报 1 好 */
        int cnt2 = 0;
        if (pTsRxCount((int)CANInd, &cnt2) == 0 && cnt2 > 0)
            r = (DWORD)cnt2;
    } else if (g_backend == BE_PCAN) {
        /*
         ★ 报真实数量（pump 到缓冲后数）。
           以前这里硬编码 r=1，导致 ZCANPRO 一直认为有帧、
           空转 Receive。
         */
        r = (DWORD)PcanPump(CANInd);
    } else {
        r = 1;   /* 其它后端无法预读数量 */
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

__declspec(dllexport) DWORD __stdcall VCI_Transmit(DWORD DeviceType, DWORD DeviceInd,
                                                   DWORD CANInd, VCI_CAN_OBJ *pSend,
                                                   DWORD Len)
{
    DWORD cnt = 1, r = 0, i;
    DWORD real = MapDevType(DeviceType, &cnt);

    if (!pSend || Len == 0) return 0;

    EnterCriticalSection(&g_cs);
    switch (g_backend) {
    case BE_PCAN: {
        for (i = 0; i < Len; i++) {
            TPCANMsg m;
            ZeroMemory(&m, sizeof(m));
            m.ID = pSend[i].ID;
            m.LEN = pSend[i].DataLen > 8 ? 8 : pSend[i].DataLen;
            if (pSend[i].ExternFlag) m.MSGTYPE |= 0x02;
            if (pSend[i].RemoteFlag) m.MSGTYPE |= 0x01;
            memcpy(m.DATA, pSend[i].Data, 8);
            if (pPcanWrite &&
            pPcanWrite((TPCANHandle)(g_pcanCh + CANInd), &m) == 0) r++;
        }
        break;
    }
    case BE_TOSUN: {
        for (i = 0; i < Len; i++) {
            TLIB_CAN c;
            ZeroMemory(&c, sizeof(c));
            c.FIdxChn = (BYTE)CANInd;
            c.FDLC = pSend[i].DataLen > 8 ? 8 : pSend[i].DataLen;
            c.FIdentifier = (int)pSend[i].ID;
            c.FProperties = 0x01;                   /* bit0 = TX */
            if (pSend[i].ExternFlag) c.FProperties |= 0x04;
            if (pSend[i].RemoteFlag) c.FProperties |= 0x02;
            memcpy(c.FData, pSend[i].Data, 8);
            if (pTsTx && pTsTx(&c) == 0) r++;
        }
        break;
    }
    case BE_KVASER: {
        if (g_kHandle >= 0 && pKWrite) {
            for (i = 0; i < Len; i++) {
                unsigned fl;
                /* ★ 标准帧必须显式带 canMSG_STD：
                   EXT 和 STD 都不给时 canlib 会默认当扩展帧 */
                fl = pSend[i].ExternFlag ? K_MSG_EXT : K_MSG_STD;
                if (pSend[i].RemoteFlag) fl |= K_MSG_RTR;
                if (pKWrite(g_kHandle, (long)pSend[i].ID, pSend[i].Data,
                            pSend[i].DataLen > 8 ? 8 : pSend[i].DataLen,
                            fl) == 0) r++;
            }
        }
        break;
    }
    case BE_VECTOR: {
        if (g_xlPortOpen && pXlTransmit) {
            /*
             ★★ 必须传 XLevent 数组，不是裸 XLcanMsg ★★

               手册 5.5.1 + 示例（Transmitting 100 CAN messages）：
                   XLevent xlEvent[100];
                   memset(xlEvent, 0, sizeof(xlEvent));   // required init.
                   xlEvent[i].tag            = XL_TRANSMIT_MSG;
                   xlEvent[i].tagData.msg.id = 0x04;
                   ...
                   xlCanTransmit(port, mask, &nCount, xlEvent);

               之前直接传 XLcanMsg，驱动会把 msg.id(0x321) 当 tag 读，
               于是返回 101 = XL_ERR_WRONG_PARAMETER。
               这个 101 卡了很久，靠打印参数 + 手册示例才定位到。
             */
            XLevent ev[64];
            unsigned int n = 0;
            XLstatus xs;

            if (Len > 64) Len = 64;
            ZeroMemory(ev, sizeof(ev));
            for (i = 0; i < Len; i++) {
                XLcanMsg *m = &ev[i].tagData.msg;
                ev[i].tag      = XL_TRANSMIT_MSG;
                ev[i].chanIndex = 0;
                m->id = pSend[i].ID;
                if (pSend[i].ExternFlag) m->id |= XL_CAN_EXT_MSG_ID;
                m->dlc = pSend[i].DataLen > 8 ? 8 : pSend[i].DataLen;
                m->flags = 0;
                if (pSend[i].RemoteFlag)
                    m->flags |= XL_CAN_MSG_FLAG_REMOTE_FRAME;
                memcpy(m->data, pSend[i].Data, 8);
            }
            n = Len;
            xs = pXlTransmit(g_xlPort, g_xlMask, &n, ev);
            if (xs == XL_SUCCESS) r = Len;
            LogF("  Vector xlCanTransmit(%lu 帧) -> %lu, 已发 %lu",
                 (unsigned long)Len, (unsigned long)xs, (unsigned long)r);
            LogF("    参数: port=%lu mask=0x%I64X count=%u sizeofMsg=%u",
                 (unsigned long)g_xlPort, (unsigned long long)g_xlMask,
                 n, (unsigned)sizeof(XLcanMsg));
            if (Len > 0)
                LogF("    ev[0]: tag=%u id=0x%lX flags=0x%X dlc=%u data=%02X %02X %02X %02X",
                     (unsigned)ev[0].tag,
                     (unsigned long)ev[0].tagData.msg.id,
                     (unsigned)ev[0].tagData.msg.flags,
                     (unsigned)ev[0].tagData.msg.dlc,
                     ev[0].tagData.msg.data[0], ev[0].tagData.msg.data[1],
                     ev[0].tagData.msg.data[2], ev[0].tagData.msg.data[3]);
        }
        break;
    }
    default:
        /*
         ★ 发送也过一遍模型：
             上层 VCI_CAN_OBJ → CtFrame → VCI_CAN_OBJ → 厂家 DLL

           表面上绕了一圈，但这是刻意的：
             · 转换只存在 CtFromVciObj / CtToVciObj 一对，全后端共用
             · 以后虚拟通道 / 常驻服务要在这个位置拦截帧
             · 一旦有不可转换的字段，会在这里暴露而不是到真机才发现
         */
        if (g_vci.Transmit) {
            VCI_CAN_OBJ conv[128];
            DWORD bn = Len > 128 ? 128 : Len;
            DWORD bi;
            for (bi = 0; bi < bn; bi++) {
                CtFrame f;
                CtFromVciObj(&pSend[bi], &f, (int)CANInd);
                f.dir = CT_DIR_TX;
                CtToVciObj(&f, &conv[bi]);
            }
            r = g_vci.Transmit(real, DeviceInd, CANInd, conv, bn);
        }
        /*
         ★ vci 后端之前不写日志，导致「发成功」和「没调」分不出来。
           限流：每通道前 20 次逐条记，之后每 50 次记一条。
         */
        if (CANInd < VCI_MAX_CHANNEL) {
            g_txCount[CANInd]++;
            if (g_txCount[CANInd] <= 20 || (g_txCount[CANInd] % 50) == 0) {
                /*
                 ★ 注意别把「帧数」和「数据长度」打混。

                   原来这里打的是 Len（本次发几帧，ZCANPRO 通常传 1），
                   却写成「长度=1」，看起来像 DLC 被截成一个字节。
                   真机上很容易误判成数据被截断 —— 而帧其实原样转发了。
                 */
                LogF("  VCI 发送 ch=%lu ID=0x%lX 帧数=%lu 数据长度=%lu -> 返回%lu（累计 %lu 帧）",
                     (unsigned long)CANInd,
                     (unsigned long)(pSend ? pSend[0].ID : 0),
                     (unsigned long)Len,
                     (unsigned long)(pSend ? pSend[0].DataLen : 0),
                     (unsigned long)r,
                     (unsigned long)g_txCount[CANInd]);
            }
        }
        break;
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

__declspec(dllexport) DWORD __stdcall VCI_Receive(DWORD DeviceType, DWORD DeviceInd,
                                                  DWORD CANInd, VCI_CAN_OBJ *pRecv,
                                                  DWORD Len, int WaitTime)
{
    DWORD cnt = 1, r = 0, i;
    DWORD real = MapDevType(DeviceType, &cnt);
    (void)WaitTime;

    if (!pRecv || Len == 0) return 0;

    EnterCriticalSection(&g_cs);
    switch (g_backend) {
    case BE_PCAN: {
        /*
         从缓冲里取（和 GetReceiveNum 共用同一个缓冲，不会不一致）。
         缓冲没有就按 WaitTime 等一下，期间反复 pump。
         */
        int k = (int)CANInd;
        if (k < 0 || k >= VCI_MAX_CHANNEL) break;
        {
            int want = (int)(Len > (DWORD)PCAN_RXBUF ? (DWORD)PCAN_RXBUF : Len);
            int waited = 0;
            int budget = WaitTime > 0 ? WaitTime : 0;

            PcanPump(CANInd);
            while (g_pcanRxCount[k] == 0 && waited < budget) {
                Sleep(1);
                waited += 1;
                PcanPump(CANInd);
            }
            while (r < (DWORD)want && g_pcanRxCount[k] > 0) {
                pRecv[r] = g_pcanRx[k][0];
                if (g_pcanRxCount[k] > 1)
                    memmove(&g_pcanRx[k][0], &g_pcanRx[k][1],
                            sizeof(VCI_CAN_OBJ) * (size_t)(g_pcanRxCount[k] - 1));
                g_pcanRxCount[k]--;
                r++;
            }
        }
        break;
    }
    case BE_TOSUN: {
        /* ★ TLIBCAN(20B) 和 VCI_CAN_OBJ(24B) 布局不同，必须逐条转换，
           不能直接把 pRecv 当 TLIBCAN* 塞进去再当 VCI_CAN_OBJ 读。 */
        if (pTsRx && Len <= 256) {
            TLIB_CAN buf[256];
            int n = (int)Len, rc;
            ZeroMemory(buf, sizeof(TLIB_CAN) * Len);
            /* ★ 4 个参数：buffers, size(in/out), channel, includeTx
               少传第 4 个 → __stdcall 栈损坏（和 Kvaser 同类错误） */
            rc = pTsRx(buf, &n, (int)CANInd, 0);
            if (rc == 0 && n > 0) {
                if (n > (int)Len) n = (int)Len;
                for (i = 0; i < (DWORD)n; i++) {
                    ZeroMemory(&pRecv[i], sizeof(VCI_CAN_OBJ));
                    pRecv[i].ID = (DWORD)buf[i].FIdentifier;
                    pRecv[i].DataLen = buf[i].FDLC > 8 ? 8 : buf[i].FDLC;
                    pRecv[i].ExternFlag = (buf[i].FProperties & 0x04) ? 1 : 0;
                    pRecv[i].RemoteFlag = (buf[i].FProperties & 0x02) ? 1 : 0;
                    memcpy(pRecv[i].Data, buf[i].FData, 8);
                    pRecv[i].TimeStamp = (DWORD)(buf[i].FTimeUs / 1000);
                    pRecv[i].TimeFlag = 1;
                    r++;
                }
            }
        }
        break;
    }
    case BE_KVASER: {
        if (g_kHandle >= 0 && pKRead) {
            for (i = 0; i < Len; i++) {
                long id = 0;
                DWORD ts = 0, fl = 0;
                BYTE data[8];
                unsigned dlc = 0;
                ZeroMemory(data, sizeof(data));
                /* ★ 顺序是 (hnd, id, msg, dlc, flag, time)
                   原来我把 flag 和 time 传反了 */
                if (pKRead(g_kHandle, &id, data, &dlc, &fl, &ts) != 0) break;
                ZeroMemory(&pRecv[i], sizeof(VCI_CAN_OBJ));
                pRecv[i].ID = (DWORD)id;
                pRecv[i].DataLen = dlc > 8 ? 8 : (BYTE)dlc;
                pRecv[i].ExternFlag = (fl & K_MSG_EXT) ? 1 : 0;
                pRecv[i].RemoteFlag = (fl & K_MSG_RTR) ? 1 : 0;
                memcpy(pRecv[i].Data, data, 8);
                pRecv[i].TimeStamp = ts;
                pRecv[i].TimeFlag = 1;
                r++;
            }
        }
        break;
    }
    case BE_VECTOR: {
        /*
         xlReceive 返回的是事件流，里面混着报文/芯片状态/发送确认。
         只取 tag==XL_RECEIVE_MSG 的，其余丢掉。
        */
        if (g_xlPortOpen && pXlReceiveFn) {
            XLevent evs[128];
            unsigned int n = (Len > 128) ? 128 : (unsigned int)Len;
            XLstatus xs;

            ZeroMemory(evs, sizeof(evs));
            xs = pXlReceiveFn(g_xlPort, &n, evs);
            if (xs == XL_SUCCESS || xs == 0x0B /* XL_ERR_QUEUE_IS_EMPTY */) {
                unsigned int k;
                for (k = 0; k < n && r < Len; k++) {
                    XLcanMsg *m;
                    if (evs[k].tag != XL_RECEIVE_MSG) continue;
                    m = &evs[k].tagData.msg;
                    ZeroMemory(&pRecv[r], sizeof(VCI_CAN_OBJ));
                    pRecv[r].ID = m->id & 0x1FFFFFFF;
                    pRecv[r].ExternFlag = (m->id & XL_CAN_EXT_MSG_ID) ? 1 : 0;
                    pRecv[r].RemoteFlag =
                        (m->flags & XL_CAN_MSG_FLAG_REMOTE_FRAME) ? 1 : 0;
                    pRecv[r].DataLen = m->dlc > 8 ? 8 : (BYTE)m->dlc;
                    memcpy(pRecv[r].Data, m->data, 8);
                    pRecv[r].TimeStamp = (DWORD)(evs[k].timeStamp & 0xFFFFFFFF);
                    pRecv[r].TimeFlag = 1;
                    r++;
                }
            }
        }
        break;
    }
    default:
        /*
         ★ 从桥自己的缓冲取，保证和 GetReceiveNum 对得上。

           缓冲空时先 pump 一次；还是空就按 WaitTime 适度等一下（有上限）。
         */
        {
            int ch2 = (int)CANInd;
            DWORD got = 0;
            if (ch2 >= 0 && ch2 < VCI_MAX_CHANNEL) {
                VciPump(CANInd, 256);
                if (g_vciRxN[ch2] == 0 && WaitTime > 0) {
                    int waited = 0, cap = WaitTime > 200 ? 200 : WaitTime;
                    while (g_vciRxN[ch2] == 0 && waited < cap) {
                        Sleep(1);
                        waited++;
                        VciPump(CANInd, 32);
                    }
                }
                while (got < Len && g_vciRxN[ch2] > 0) {
                    int i2;
                    /* ★ 缓冲里存的是中立模型 CtFrame，取出时转回 VCI_CAN_OBJ 交给上层 */
                    CtToVciObj(&g_vciRx[ch2][0], &pRecv[got]);
                    got++;
                    for (i2 = 1; i2 < g_vciRxN[ch2]; i2++)
                        g_vciRx[ch2][i2 - 1] = g_vciRx[ch2][i2];
                    g_vciRxN[ch2]--;
                }
            }
            r = got;
        }
        /*
         ★ 收到帧才记（r > 0），否则轮询时会把日志刷爆。
           限流同发送：前 20 次逐条，之后每 50 次一条。
         */
        if (r > 0 && CANInd < VCI_MAX_CHANNEL) {
            g_rxCount[CANInd] += r;
            if (g_rxCount[CANInd] <= 20 || (g_rxCount[CANInd] % 50) == 0) {
                LogF("  VCI 接收 ch=%lu 本次%lu 帧 ID=0x%lX（累计 %lu 帧）",
                     (unsigned long)CANInd, (unsigned long)r,
                     (unsigned long)pRecv[0].ID,
                     (unsigned long)g_rxCount[CANInd]);
            }
        }
        break;
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

__declspec(dllexport) DWORD __stdcall VCI_ReadBoardInfo(DWORD DeviceType,
                                                        DWORD DeviceInd,
                                                        VCI_BOARD_INFO *pInfo)
{
    DWORD cnt = 1, r = STATUS_ERR;
    DWORD real = MapDevType(DeviceType, &cnt);

    if (!pInfo) return STATUS_ERR;

    EnterCriticalSection(&g_cs);
    SelectBackend();

    if (g_backend == BE_GCAN || g_backend == BE_VCI) {
        if (g_vci.ReadBoardInfo) {
            r = g_vci.ReadBoardInfo(real, DeviceInd, pInfo);
            if (r == STATUS_OK) {
                /*
                 ★★ 强制覆盖 DeviceType / DeviceInd / can_Num ★★

                   为什么：厂家这个结构体填得不可靠。
                   创苯的 can_Num 就返回过 808470065 这种垃圾值，
                   DeviceType 同理。而 ZCANPRO 会拿这个型号去查
                   channelNumber_count(info.type)，查表返回 0，和实际通道数对不上
                   就弹出断言崩溃：
                       devicemanager.cxx Line 731
                       Expression: channelNumber_count(info.type)

                   所以这三个字段不能信厂家，必须报桥自己的值：
                     型号   = 上层请求的型号（保证 ZCANPRO 查表能查到）
                     索引   = 上层请求的索引
                     通道数 = 桥实际对外暴露的路数（保证与型号自洽）
                 */
                /*
                 ★ 只修正 can_Num，别的字段一概不动。

                   can_Num 在官方布局里是【偏移 10 的一个 BYTE】。
                   之前按错误布局当成 DWORD 写偏移 12，
                   结果把 str_Serial_Num 写坏、真正的 can_Num 反而被清 0，
                   ZCANPRO 一查表就断言。

                   值取 cnt（MapDevType 按型号给出），保证「型号↔通道数」自洽。
                 */
                {
                    BYTE want = (BYTE)(cnt ? cnt : 1);
                    if (want > (BYTE)VCI_MAX_CHANNEL) want = (BYTE)VCI_MAX_CHANNEL;
                    if (pInfo->can_Num != want) {
                        LogF("  BoardInfo.can_Num %u -> %u（按型号 %lu 修正）",
                             (unsigned)pInfo->can_Num, (unsigned)want,
                             (unsigned long)DeviceType);
                        pInfo->can_Num = want;
                    }
                }
            }
        }
    }

    if (r != STATUS_OK) {
        ZeroMemory(pInfo, sizeof(*pInfo));
        pInfo->can_Num = (BYTE)(cnt ? cnt : 1);
        strcpy(pInfo->str_hw_Type, "UniVciBridge");
        switch (g_backend) {
        case BE_PCAN:
            strcpy((char *)pInfo->str_Serial_Num, "PCAN0");
            strcpy((char *)pInfo->str_hw_Type, "PCAN-USB (UniVciBridge)");
            break;
        case BE_TOSUN:
            strcpy((char *)pInfo->str_Serial_Num, "TOSUN0");
            strcpy((char *)pInfo->str_hw_Type, "TOSUN (UniVciBridge)");
            break;
        case BE_KVASER:
            strcpy((char *)pInfo->str_Serial_Num, "KVASER0");
            strcpy((char *)pInfo->str_hw_Type, "Kvaser (UniVciBridge)");
            break;
        case BE_VECTOR:
            strcpy((char *)pInfo->str_Serial_Num, "VECTOR0");
            strcpy((char *)pInfo->str_hw_Type, "Vector XL (UniVciBridge)");
            break;
        default:
            strcpy((char *)pInfo->str_Serial_Num, "BRIDGE0");
            strcpy((char *)pInfo->str_hw_Type, "UniVciBridge");
            break;
        }
        r = STATUS_OK;
    }

    /*
     ★ 模拟画像：对外声称几路 + 硬件类型字符串

       注意【不改】pInfo->DeviceType —— 那个必须和上层请求的型号一致，
       否则 ZCANPRO 的 channelNumber_count(info.type) 会断言崩溃（真机踩过）。

       所以"模拟"体现在两处：
         · 对外通道数（channels= / profile= 推出）
         · str_hw_Type 里的型号名（有些上位机会按字符串判断）
     */
    if (r == STATUS_OK && g_profileType) {
        int chans = g_profileChans ? g_profileChans
                                   : (int)ChannelsOfType(g_profileType);
        if (chans < 1) chans = 1;
        if (chans > VCI_MAX_CHANNEL) chans = VCI_MAX_CHANNEL;
        pInfo->can_Num = (BYTE)chans;

        switch (g_profileType) {
        case 3:  strcpy(pInfo->str_hw_Type, "USBCAN-I");      break;
        case 4:  strcpy(pInfo->str_hw_Type, "USBCAN-II");     break;
        case 20: strcpy(pInfo->str_hw_Type, "USBCAN-E-U");    break;
        case 21: strcpy(pInfo->str_hw_Type, "USBCAN-2E-U");   break;
        case 31: strcpy(pInfo->str_hw_Type, "USBCAN-4E-U");   break;
        case 34: strcpy(pInfo->str_hw_Type, "USBCAN-8E-U");   break;
        default: break;
        }
        LogF("  \u2605 \u6a21\u62df\u753b\u50cf\u751f\u6548\uff1a\u5bf9\u5916 %d \u8def\uff0c\u578b\u53f7\u4e32\u300c%s\u300d",
             chans, pInfo->str_hw_Type);
    }

    /*
     ★ 模拟画像：对外声称几路 + 硬件类型字符串

       注意【不改】pInfo->DeviceType —— 那个必须和上层请求的型号一致，
       否则 ZCANPRO 的 channelNumber_count(info.type) 会断言崩溃（真机踩过）。

       所以"模拟"体现在两处：
         · 对外通道数（channels= / profile= 推出）
         · str_hw_Type 里的型号名（有些上位机会按字符串判断型号）
     */
    if (r == STATUS_OK && g_profileType) {
        int chans = g_profileChans ? g_profileChans
                                   : (int)ChannelsOfType(g_profileType);
        if (chans < 1) chans = 1;
        if (chans > VCI_MAX_CHANNEL) chans = VCI_MAX_CHANNEL;
        pInfo->can_Num = (BYTE)chans;

        switch (g_profileType) {
        case 3:  strcpy(pInfo->str_hw_Type, "USBCAN-I");    break;
        case 4:  strcpy(pInfo->str_hw_Type, "USBCAN-II");   break;
        case 20: strcpy(pInfo->str_hw_Type, "USBCAN-E-U");  break;
        case 21: strcpy(pInfo->str_hw_Type, "USBCAN-2E-U"); break;
        case 31: strcpy(pInfo->str_hw_Type, "USBCAN-4E-U"); break;
        case 34: strcpy(pInfo->str_hw_Type, "USBCAN-8E-U"); break;
        default: break;
        }
        LogF("  ★ 模拟画像生效：对外 %d 路，型号串「%s」",
             chans, pInfo->str_hw_Type);
    }

    LeaveCriticalSection(&g_cs);
    return r;
}

__declspec(dllexport) DWORD __stdcall VCI_ReadErrInfo(DWORD DeviceType, DWORD DeviceInd,
                                                       DWORD CANInd, VCI_ERR_INFO *pErr)
{
    DWORD cnt = 1, r = STATUS_OK;
    DWORD real = MapDevType(DeviceType, &cnt);

    EnterCriticalSection(&g_cs);
    if ((g_backend == BE_GCAN || g_backend == BE_VCI) && g_vci.ReadErrInfo) {
        r = g_vci.ReadErrInfo(real, DeviceInd, CANInd, pErr);
    } else if (pErr) {
        ZeroMemory(pErr, sizeof(*pErr));
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

__declspec(dllexport) DWORD __stdcall VCI_ReadCANStatus(DWORD DeviceType,
                                                         DWORD DeviceInd,
                                                         DWORD CANInd,
                                                         VCI_CAN_STATUS *pStatus)
{
    DWORD cnt = 1, r = STATUS_OK;
    DWORD real = MapDevType(DeviceType, &cnt);

    EnterCriticalSection(&g_cs);
    if ((g_backend == BE_GCAN || g_backend == BE_VCI) && g_vci.ReadCANStatus) {
        r = g_vci.ReadCANStatus(real, DeviceInd, CANInd, pStatus);
    } else if (pStatus) {
        ZeroMemory(pStatus, sizeof(*pStatus));
        pStatus->Baudrate = g_bitrate;
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

/*==========================================================================
  13. DllMain —— 只做最轻的事，保证 loader lock 安全
 ==========================================================================*/
/*
 ★★ 统一释放所有硬件资源 ★★

 这一条是「插 PCAN → 用 ZCANPRO → 关掉 ZCANPRO → 用 PCAN-View」
 全程不用人操作的关键：

   PCAN 的通道是**独占**的。桥打开通道后，必须显式 CAN_Uninitialize，
   PCAN-View 才能拿到。ZCANPRO 正常关闭时会调 VCI_CloseDevice，
   但如果它没调（崩溃、被强杀、或某些版本不调），通道就会一直占着，
   用户就得重启电脑才能用 PCAN-View —— 那就是"影响了厂家软件"。

   所以这里做两层保证：
     · VCI_CloseDevice 时释放（正常路径）
     · DLL_PROCESS_DETACH 时再释放一次（兜底路径）
   两个函数都做幂等判断，重复调用无害。
*/
static void ReleaseHardware(void)
{
    if (g_backend == BE_PCAN && pPcanUninit) {
        /* PCAN 通道独占，退出前必须放开，否则 PCAN-View 打不开 */
        pPcanUninit(g_pcanCh);
        LogLine("已释放 PEAK 通道（PCAN-View 现在可以用了）");
    } else if (g_backend == BE_KVASER) {
        if (g_kHandle >= 0) {
            if (pKBusOff) pKBusOff(g_kHandle);
            if (pKClose)  pKClose(g_kHandle);
            g_kHandle = -1;
            LogLine("已关闭 Kvaser 通道");
        }
    } else if (g_backend == BE_VECTOR) {
        if (g_xlPortOpen) {
            if (pXlDeactivate) pXlDeactivate(g_xlPort, g_xlMask);
            if (pXlClosePort)  pXlClosePort(g_xlPort);
            g_xlPortOpen = 0;
            g_xlPort = 0;
            LogLine("已关闭 Vector 端口");
        }
    } else if (g_backend == BE_TOSUN) {
        if (g_tsInited && pTsFin) { pTsFin(); g_tsInited = 0; LogLine("已结束 TOSUN 会话"); }
    }

    /*
     ★★ VCI 设备句柄必须无条件释放 ★★

       原来这句包在 `else if (g_backend == BE_GCAN || g_backend == BE_VCI)` 里，
       于是有个漏洞：
         探测阶段把设备打开了（g_vciOpened = 1），
         但后面后端判定失败、g_backend 变回 BE_NONE，
       这个分支就进不去 → 厂家设备句柄永远不关。

       后果（真机现象）：驱动里留着「设备已被打开」的记录，
       占用它的进程已经死了，之后谁也打不开，只能
       重装 ZCANPRO（顺带重置驱动）或重启电脑才能恢复。

       所以这里改成【只看 g_vciOpened】，跟后端是什么无关。
     */
    if (g_vciOpened && g_vci.CloseDevice) {
        /*
         ★★ 关设备必须用【实际打开时的型号】，不能用 g_devType ★★

           真机症状：桥用型号 21 打开 2E-U，退出时却拿 g_devType
           （初值 3！）去调 CloseDevice(3, 0) —— 型号不对，驱动不认，
           设备根本没关掉 → 句柄泄漏在驱动里 → 之后连原厂 ZCANPRO
           都打不开，只能重装 ZCANPRO（顺带重置驱动）或重启电脑。

           这正是用户说的「重装 ZCANPRO 就好使，否则不好使」。

           g_vciRealType 是探测时真正打开成功的型号（每个探测分支都会设），
           所以优先用它；只有它为 0 时才退回 g_devType。
         */
        DWORD closeType = g_vciRealType ? g_vciRealType : g_devType;
        DWORD r = g_vci.CloseDevice(closeType, 0);
        LogF("退出兜底关闭 VCI 设备：CloseDevice(%lu) -> %lu",
             (unsigned long)closeType, (unsigned long)r);
        g_vciOpened = 0;
        g_vciRealType = 0;
        g_devType = 3;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    (void)lpReserved;

    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = hModule;
        InitializeCriticalSection(&g_cs);
        g_csReady = 1;
        DisableThreadLibraryCalls(hModule);
        /* ★ 不在这里加载后端 DLL、不做重活 */
        /*
           日志必须自己报版本。
           这个项目已经反复出现「测的是旧版 DLL，得出错误结论」——
           光看日志头分不清跑的是哪一版，白排查好几轮。
           __DATE__/__TIME__ 是编译期常量，不同构建必然不同。
        */
        LogLine("=== UniVciBridge 已加载 ===");
        LogF("构建: v%s", UNIVCI_BUILD_TAG);
        /* 不打印 __DATE__/__TIME__：那样每次编译哈希都变，
           "认指纹"就失去意义。版本靠 UNIVCI_BUILD_TAG 认。 */
    } else if (reason == DLL_PROCESS_DETACH) {
        LogLine("=== UniVciBridge 已卸载 ===");
        /*
         ★ 这里必须释放硬件通道。
           否则 ZCANPRO 退出时如果没走 VCI_CloseDevice（崩溃/强杀），
           PCAN 通道会一直被占，PCAN-View 就打不开了。
           那就等于「影响了厂家自己软件」，正是要避免的。
        */
        if (g_csReady) ReleaseHardware();
        g_csReady = 0;
        if (g_hBackend) { FreeLibrary(g_hBackend); g_hBackend = NULL; }
        DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}
