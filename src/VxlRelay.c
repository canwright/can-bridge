/*
 * VxlRelay.c —— 把真实 CAN 盒子桥接成 Vector 的虚拟通道
 *
 * ==========================================================================
 *  ★ 为什么走"虚拟通道"，不写假 vxlapi.dll
 *
 *  两种做法：
 *
 *    A. 假 vxlapi.dll — 替掉 Vector 的 DLL，自己实现那 340 个导出
 *       ✗ CANape 的【许可检查也走 Vector 驱动】。实测：装上探针后
 *         CANape 加载了 DLL，随即自己退出 —— 许可读不到就不让用。
 *       ✗ 要对着 CANape 的脾气实现几十上百个函数，猜错一个就崩。
 *
 *    B. 虚拟通道中继（本文件）— 用【真】Vector 驱动，一行不改
 *       ✓ 许可、驱动配置、所有 API 行为都是原厂的，CANape 完全无感。
 *       ✓ 我们要做的只有"搬帧"，一个 API 都不用实现。
 *
 *  实测确认（2026-11-06，本机，无硬件）：
 *      进程 A 打开 Virtual Channel 1（app=SideA，perm=0x1）发帧
 *      进程 B 打开 Virtual Channel 2（app=SideB，perm=0x2）收帧
 *      → B 收到了 A 发的 0x1AB，说明 Vector 驱动把两路虚拟通道
 *        连成了一根"虚拟电缆"，两个应用各占一路即可互通。
 *
 *      这也是本程序的立足点：CANape 占第 N 路，我们占第 N+1 路。
 *
 * ==========================================================================
 *  数据流
 *
 *      CANape
 *        │ 真 vxlapi64.dll（64 位，原厂）
 *        ▼
 *      Virtual Channel N          ← CANape 看到的"设备"
 *        ↕  （Vector 驱动内部配对，不需要我们做任何事）
 *      Virtual Channel N+1        ← 本程序占这一路
 *        │ xlReceive / xlCanTransmit
 *        ▼
 *      本程序（32 位进程）
 *        │ VCI_Receive / VCI_Transmit
 *        ▼
 *      UniVciBridge.dll（backend=周立功/PCAN/…）
 *        │ 厂家驱动
 *        ▼
 *      真实硬件
 *
 *  为什么 32 位能配 64 位的 CANape：
 *      虚拟通道的配对发生在【驱动里】，不在进程里。所以 32 位中继
 *      用 32 位的 vxlapi.dll、64 位 CANape 用 64 位的 vxlapi64.dll，
 *      两边照样落在同一条虚拟总线上 —— 实测已验证。
 *
 * ==========================================================================
 *  用法
 *
 *      VxlRelay.exe <虚拟通道序号，1 起> <波特率> [硬件型号]
 *
 *      例：VxlRelay.exe 2 500000 21
 *          占用第 2 个虚拟通道，500k，硬件按型号 21（周立功 2E-U）
 *
 *  硬件后端由同目录的 UniVciBridge.ini 决定：
 *      backend=vci     周立功
 *      backend=pcan    PEAK
 *      backend=auto    自动探测
 *
 *  注意：CANape 那边要用【另一个】虚拟通道（比如 1），
 *        两个通道必须是相邻的一对，Vector 的配对关系就靠这个。
 * ==========================================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static FILE *g_logf = NULL;
static void RLog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (!g_logf) {
        char p[MAX_PATH];
        HMODULE hm = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&RLog, &hm)) {
            DWORD n = GetModuleFileNameA(hm, p, MAX_PATH - 32);
            if (n > 0) {
                char *q = strrchr(p, '\\');
                if (q) { *q = 0; strcat(p, "\\VxlRelay.log"); }
                g_logf = fopen(p, "a");
            }
        }
    }
    if (g_logf) {
        va_start(ap, fmt);
        vfprintf(g_logf, fmt, ap);
        va_end(ap);
        fflush(g_logf);
    }
}

/* ──────────────────────────────────────────────────────────────
   Vector XL 结构和常量
   —— 直接从 UniVciBridge.c 抄过来的，已用真驱动验证过尺寸：
      XLevent=48, XLcanMsg=32, XLchannelConfig=227, XLdriverConfig=14576,
      channelMask 偏移 42。不要重新推导。
   ────────────────────────────────────────────────────────────── */
typedef long long XLuint64;
typedef XLuint64   XLaccess;
typedef DWORD      XLportHandle;
typedef DWORD      XLstatus;

#pragma pack(push, 1)
typedef struct {
    DWORD bitRate;
    BYTE  sjw, tseg1, tseg2, sam, outputMode;
    BYTE  reserved[7];
    BYTE  canOpMode;
} XLbusParamsDataCan;

typedef union {
    XLbusParamsDataCan can;
    BYTE raw[28];
} XLbusParamsData;

typedef struct {
    DWORD          busType;
    XLbusParamsData data;
} XLbusParams;

typedef struct {
    char  name[32];
    BYTE  hwType, hwIndex, hwChannel;
    WORD  transceiverType, transceiverState, configError;
    BYTE  channelIndex;
    XLuint64 channelMask;
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
} XLchannelConfig;

typedef struct {
    DWORD          dllVersion;
    DWORD          channelCount;
    DWORD          reserved[10];
    XLchannelConfig channel[64];
} XLdriverConfig;

typedef struct {
    DWORD id;
    WORD  flags;
    WORD  dlc;
    XLuint64 res1;
    BYTE  data[8];
    XLuint64 res2;
} XLcanMsg;

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
} XLevent;

/* VCI 结构（和桥一致，24 / 16 字节） */
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

typedef struct {
    DWORD AccCode;
    DWORD AccMask;
    DWORD Reserved;
    BYTE  Filter;
    BYTE  Timing0;
    BYTE  Timing1;
    BYTE  Mode;
} VCI_INIT_CONFIG_EU;
#pragma pack(pop)

#define XL_SUCCESS                0
#define XL_BUS_TYPE_CAN           1
#define XL_BUS_COMPATIBLE_CAN     0x00000001
#define XL_INTERFACE_VERSION_V3   3
#define XL_TRANSMIT_MSG           10
#define XL_CAN_MSG_FLAG_ERROR_FRAME 0x01

/* ──────────────────────────────────────────────────────────────
   vxlapi 函数指针（只取需要的几个）
   ────────────────────────────────────────────────────────────── */
typedef XLstatus (__stdcall *PF_xlOpenDriver)(void);
typedef XLstatus (__stdcall *PF_xlCloseDriver)(void);
typedef XLstatus (__stdcall *PF_xlGetDriverConfig)(XLdriverConfig *);
typedef XLstatus (__stdcall *PF_xlSetApplConfig)(const char *, unsigned int,
                                                 unsigned int, unsigned int,
                                                 unsigned int, unsigned int);
typedef XLstatus (__stdcall *PF_xlOpenPort)(XLportHandle *, const char *,
                                            XLaccess, XLaccess *, unsigned int,
                                            unsigned int, unsigned int);
typedef XLstatus (__stdcall *PF_xlClosePort)(XLportHandle);
typedef XLstatus (__stdcall *PF_xlCanSetChannelBitrate)(XLportHandle, XLaccess, unsigned int);
typedef XLstatus (__stdcall *PF_xlActivateChannel)(XLportHandle, XLaccess, unsigned int, unsigned int);
typedef XLstatus (__stdcall *PF_xlReceive)(XLportHandle, unsigned int *, unsigned int, XLevent *);
typedef XLstatus (__stdcall *PF_xlCanTransmit)(XLportHandle, XLaccess, unsigned int, void *);

/* 桥的 VCI 导出 */
typedef DWORD (__stdcall *PF_VciOpen)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_VciClose)(DWORD, DWORD);
typedef DWORD (__stdcall *PF_VciInit)(DWORD, DWORD, DWORD, VCI_INIT_CONFIG_EU *);
typedef DWORD (__stdcall *PF_VciStart)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_VciTx)(DWORD, DWORD, DWORD, VCI_CAN_OBJ *, DWORD);
typedef DWORD (__stdcall *PF_VciRx)(DWORD, DWORD, DWORD, VCI_CAN_OBJ *, DWORD, int);

static int StrContainsNoCase(const char *s, const char *sub)
{
    size_t i, n = strlen(sub);
    for (; *s; s++) {
        for (i = 0; i < n; i++) {
            char a = s[i], b = sub[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int vchan = (argc >= 2) ? atoi(argv[1]) : 2;
    unsigned int baud = (argc >= 3) ? (unsigned int)strtoul(argv[2], NULL, 10) : 500000u;
    DWORD devType = (argc >= 4) ? (DWORD)strtoul(argv[3], NULL, 10) : 21u;

    HMODULE hv = NULL, hb = NULL;
    PF_xlOpenDriver pOpen; PF_xlCloseDriver pClose; PF_xlGetDriverConfig pCfg;
    PF_xlSetApplConfig pAppl; PF_xlOpenPort pPort; PF_xlClosePort pClosePort;
    PF_xlCanSetChannelBitrate pBitrate; PF_xlActivateChannel pAct;
    PF_xlReceive pRx; PF_xlCanTransmit pTx;

    PF_VciOpen pVOpen; PF_VciClose pVClose; PF_VciInit pVInit;
    PF_VciStart pVStart; PF_VciTx pVTx; PF_VciRx pVRx;

    XLdriverConfig cfg;
    XLportHandle port = 0;
    XLaccess wantMask = 0, perm = 0;
    unsigned int vchanIndex = 0;
    int found = 0, i;
    char self[MAX_PATH];

    unsigned long nToHw = 0, nToVxl = 0, nErrSkip = 0;

    RLog("============================================================\n");
    RLog("  VxlRelay - 真实 CAN 盒子 -> Vector 虚拟通道\n");
    RLog("============================================================\n\n");
    RLog("  虚拟通道    第 %d 个（CANape 应该占相邻的那一路）\n", vchan);
    RLog("  波特率      %u\n", baud);
    RLog("  硬件型号    %lu（由 UniVciBridge.ini 的 backend 决定用哪家）\n\n", (unsigned long)devType);

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    /* ---- 定位自己的目录 ---- */
    GetModuleFileNameA(NULL, self, MAX_PATH);
    {
        char *s = strrchr(self, '\\');
        if (s) *s = 0;
    }

    /* ---- 加载 vxlapi.dll（32 位；系统目录里那份） ---- */
    hv = LoadLibraryA("vxlapi.dll");
    if (!hv) { RLog("[x] vxlapi.dll 加载失败 (%lu)\n", GetLastError()); return 1; }
    RLog("[ok] vxlapi.dll 已加载\n");

    pOpen    = (PF_xlOpenDriver)      GetProcAddress(hv, "xlOpenDriver");
    pClose   = (PF_xlCloseDriver)     GetProcAddress(hv, "xlCloseDriver");
    pCfg     = (PF_xlGetDriverConfig) GetProcAddress(hv, "xlGetDriverConfig");
    pAppl    = (PF_xlSetApplConfig)   GetProcAddress(hv, "xlSetApplConfig");
    pPort    = (PF_xlOpenPort)        GetProcAddress(hv, "xlOpenPort");
    pClosePort = (PF_xlClosePort)     GetProcAddress(hv, "xlClosePort");
    pBitrate = (PF_xlCanSetChannelBitrate)GetProcAddress(hv, "xlCanSetChannelBitrate");
    pAct     = (PF_xlActivateChannel) GetProcAddress(hv, "xlActivateChannel");
    pRx      = (PF_xlReceive)         GetProcAddress(hv, "xlReceive");
    pTx      = (PF_xlCanTransmit)     GetProcAddress(hv, "xlCanTransmit");
    if (!pOpen || !pCfg || !pPort || !pRx || !pTx) {
        RLog("[x] vxlapi.dll 缺关键导出\n"); return 1;
    }

    if (pOpen() != XL_SUCCESS) { RLog("[x] xlOpenDriver 失败\n"); return 1; }
    ZeroMemory(&cfg, sizeof(cfg));
    if (pCfg(&cfg) != XL_SUCCESS) { RLog("[x] xlGetDriverConfig 失败\n"); return 1; }

    RLog("\n[..] 驱动报了 %lu 个通道：\n", (unsigned long)cfg.channelCount);
    for (i = 0; i < (int)cfg.channelCount && i < 64; i++) {
        XLchannelConfig *c = &cfg.channel[i];
        int virt = StrContainsNoCase(c->name, "virtual") ||
                   StrContainsNoCase(c->transceiverName, "virtual");
        RLog("       %2d \"%s\"%s mask=0x%I64X\n",
               i, c->name, virt ? "  [虚拟]" : "",
               (unsigned long long)c->channelMask);
    }

    /* ---- 找第 N 个虚拟通道 ---- */
    {
        int seen = 0;
        for (i = 0; i < (int)cfg.channelCount && i < 64; i++) {
            XLchannelConfig *c = &cfg.channel[i];
            int virt = StrContainsNoCase(c->name, "virtual") ||
                       StrContainsNoCase(c->transceiverName, "virtual");
            if (!virt) continue;
            seen++;
            if (seen == vchan) {
                wantMask = c->channelMask;
                vchanIndex = c->channelIndex;
                RLog("\n[ok] 选用虚拟通道第 %d 个：\"%s\" mask=0x%I64X chanIndex=%u\n",
                       vchan, c->name, (unsigned long long)wantMask, vchanIndex);
                found = 1;
                break;
            }
        }
    }
    if (!found) { RLog("\n[x] 没有第 %d 个虚拟通道（驱动里只有 %d 个）\n", vchan, i); return 1; }

    /* ---- 打开端口（permissionMask 必须先填好，这是踩过的坑） ---- */
    if (pAppl) pAppl("UniVciRelay", 0, cfg.channel[vchanIndex].hwType,
                     cfg.channel[vchanIndex].hwIndex,
                     cfg.channel[vchanIndex].hwChannel, XL_BUS_TYPE_CAN);
    perm = wantMask;
    {
        XLstatus r = pPort(&port, "UniVciRelay", wantMask, &perm, 8192,
                           XL_INTERFACE_VERSION_V3, XL_BUS_TYPE_CAN);
        RLog("[..] xlOpenPort -> %lu port=%lu perm=0x%I64X\n",
               (unsigned long)r, (unsigned long)port, (unsigned long long)perm);
        if (r != XL_SUCCESS || perm == 0) {
            RLog("[x] 拿不到虚拟通道权限 —— 是不是 CANape 已经占了这一路？\n");
            RLog("    换一个 <虚拟通道序号> 再试（CANape 占一路，我们占相邻那路）\n");
            return 1;
        }
    }
    if (pBitrate) RLog("[..] xlCanSetChannelBitrate(%u) -> %lu\n",
                         baud, (unsigned long)pBitrate(port, wantMask, baud));
    if (pAct) RLog("[..] xlActivateChannel -> %lu\n",
                     (unsigned long)pAct(port, wantMask, XL_BUS_TYPE_CAN, 0));

    /* ---- 加载桥，打开真实硬件 ---- */
    {
        char dll[MAX_PATH * 2];
        sprintf(dll, "%s\\UniVciBridge.dll", self);
        hb = LoadLibraryA(dll);
        if (!hb) {
            sprintf(dll, "%s\\USBCAN.dll", self);
            hb = LoadLibraryA(dll);
        }
    }
    if (!hb) { RLog("[x] UniVciBridge.dll 加载失败（要放在本程序旁边）\n"); return 1; }
    RLog("[ok] 桥已加载\n");

    pVOpen  = (PF_VciOpen) GetProcAddress(hb, "VCI_OpenDevice");
    pVClose = (PF_VciClose)GetProcAddress(hb, "VCI_CloseDevice");
    pVInit  = (PF_VciInit) GetProcAddress(hb, "VCI_InitCAN");
    pVStart = (PF_VciStart)GetProcAddress(hb, "VCI_StartCAN");
    pVTx    = (PF_VciTx)   GetProcAddress(hb, "VCI_Transmit");
    pVRx    = (PF_VciRx)   GetProcAddress(hb, "VCI_Receive");
    if (!pVOpen || !pVInit || !pVStart || !pVTx || !pVRx) {
        RLog("[x] 桥缺导出\n"); return 1;
    }

    RLog("\n[..] 打开硬件（型号 %lu）…\n", (unsigned long)devType);
    if (pVOpen(devType, 0, 0) != 1) { RLog("[x] VCI_OpenDevice 失败\n"); return 1; }
    {
        VCI_INIT_CONFIG_EU ic;
        ZeroMemory(&ic, sizeof(ic));
        ic.AccMask = 0xFFFFFFFFu;
        ic.Filter = 1;
        ic.Timing0 = 0x00; ic.Timing1 = 0x1C;      /* 500k */
        RLog("[..] VCI_InitCAN -> %lu\n", (unsigned long)pVInit(devType, 0, 0, &ic));
        RLog("[..] VCI_StartCAN -> %lu\n", (unsigned long)pVStart(devType, 0, 0));
    }

    RLog("\n============================================================\n");
    RLog("  中继已启动。Ctrl+C 结束。\n");
    RLog("  CANape 那边请选相邻的另一个虚拟通道。\n");
    RLog("============================================================\n\n");

    /* ---- 主循环：双向搬帧 ---- */
    {
        XLevent ev[256];
        VCI_CAN_OBJ objs[256];
        DWORD lastStat = GetTickCount();

        for (;;) {
            unsigned int cnt = 0;
            XLstatus r;

            /* 虚拟通道 -> 硬件 */
            cnt = 0;
            r = pRx(port, &cnt, 20, ev);
            if (r == XL_SUCCESS && cnt > 0) {
                unsigned int k, m = 0;
                for (k = 0; k < cnt; k++) {
                    if (ev[k].tag != 1 /* XL_RECEIVE_MSG */) continue;
                    if (ev[k].tagData.msg.flags & XL_CAN_MSG_FLAG_ERROR_FRAME) { nErrSkip++; continue; }
                    ZeroMemory(&objs[m], sizeof(objs[m]));
                    objs[m].ID = ev[k].tagData.msg.id;
                    objs[m].DataLen = (BYTE)(ev[k].tagData.msg.dlc > 8 ? 8 : ev[k].tagData.msg.dlc);
                    memcpy(objs[m].Data, ev[k].tagData.msg.data, 8);
                    objs[m].ExternFlag = (ev[k].tagData.msg.flags & 0x02) ? 1 : 0;
                    objs[m].RemoteFlag = (ev[k].tagData.msg.flags & 0x01) ? 1 : 0;
                    objs[m].TimeStamp = (DWORD)(ev[k].timeStamp / 100000ULL);
                    objs[m].TimeFlag = 1;
                    m++;
                    if (m >= 256) break;
                }
                if (m) {
                    DWORD sent = pVTx(devType, 0, 0, objs, m);
                    if (sent <= m) nToHw += sent;   /* 返回值不合理就不计，避免计数爆掉 */
                }
            }

            /* 硬件 -> 虚拟通道 */
            {
                DWORD got = pVRx(devType, 0, 0, objs, 256, 0);
                if (got > 0 && got <= 256) {   /* 防御：失败时桥可能返回垃圾值 */
                    unsigned int k, m = 0;
                    XLevent tx[256];
                    ZeroMemory(tx, sizeof(tx));
                    for (k = 0; k < got && k < 256; k++) {
                        tx[m].tag = XL_TRANSMIT_MSG;
                        tx[m].chanIndex = (BYTE)vchanIndex;
                        tx[m].tagData.msg.id = objs[k].ID;
                        tx[m].tagData.msg.dlc = objs[k].DataLen > 8 ? 8 : objs[k].DataLen;
                        memcpy(tx[m].tagData.msg.data, objs[k].Data, 8);
                        tx[m].tagData.msg.flags = (WORD)((objs[k].ExternFlag ? 0x02 : 0) |
                                                         (objs[k].RemoteFlag ? 0x01 : 0));
                        m++;
                    }
                    if (m) {
                        XLstatus tr = pTx(port, wantMask, m, tx);
                        if (tr == XL_SUCCESS) nToVxl += m;
                    }
                }
            }

            /* 每秒报一次数 */
            {
                DWORD now = GetTickCount();
                if ((DWORD)(now - lastStat) >= 1000) {
                    lastStat = now;
                    RLog("\r  虚拟->硬件 %lu 帧    硬件->虚拟 %lu 帧    丢掉错误帧 %lu 个   ",
                           nToHw, nToVxl, nErrSkip);
                    fflush(stdout);
                }
            }

            if (cnt == 0) Sleep(2);
        }
    }

    return 0;
}
