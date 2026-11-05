/*
 * VxlListen.c —— 逐个 Vector CAN 通道监听，找出哪个口上有报文
 *
 * 为什么需要它：
 *   对端（另一台电脑的周立功盒子）在发报文，但不知道线接在 VN1630 的哪一路。
 *   VN1630 有 4 个 CAN 口，其中两个可能被别的程序占着（perm=0）。
 *
 *   这个工具按顺序对每个真 CAN 通道做一次完整流程：
 *       xlSetApplConfig 分配 → xlOpenPort（permissionMask 先填好）
 *       → xlCanSetChannelBitrate(500k) → xlActivateChannel
 *       → 监听 N 秒，统计收到的帧数 → 关闭
 *   然后打印一张表，告诉你哪一路有报文、哪一路被占、哪一路总线状态异常。
 *
 * 用法：
 *   VxlListen.exe [波特率] [每路监听秒数]
 *   默认 500000 和 3 秒。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "VxlApiModel.h"

/* 事件 tag（手册 3.5.1 的枚举名） */
#define TAG_RECEIVE_MSG   1
#define TAG_CHIP_STATE    4
#define TAG_TRANSMIT_MSG  10

typedef XLstatus (__stdcall *PF_VOID)(void);
typedef XLstatus (__stdcall *PF_CFG)(void *);
typedef XLstatus (__stdcall *PF_OPENPORT)(XLportHandle *, const char *,
                                          XLaccess, XLaccess *,
                                          unsigned int, unsigned int, unsigned int);
typedef XLstatus (__stdcall *PF_CLOSEPORT)(XLportHandle);
typedef XLstatus (__stdcall *PF_SETAPPL)(const char *, unsigned int, unsigned int,
                                         unsigned int, unsigned int, unsigned int);
typedef XLstatus (__stdcall *PF_SETBITRATE)(XLportHandle, XLaccess, unsigned int);
typedef XLstatus (__stdcall *PF_ACTIVATE)(XLportHandle, XLaccess, unsigned int, unsigned int);
typedef XLstatus (__stdcall *PF_DEACTIVATE)(XLportHandle, XLaccess);
typedef XLstatus (__stdcall *PF_RECEIVE)(XLportHandle, unsigned int *, void *);

int main(int argc, char **argv)
{
    HMODULE h;
    PF_VOID       pOpen, pClose;
    PF_CFG        pCfg;
    PF_OPENPORT   pOpenPort;
    PF_CLOSEPORT  pClosePort;
    PF_SETAPPL    pSetAppl;
    PF_SETBITRATE pSetBitrate;
    PF_ACTIVATE   pActivate;
    PF_DEACTIVATE pDeactivate;
    PF_RECEIVE    pReceive;
    static unsigned char buf[256 * 1024];
    XlDriverConfig *cfg;
    unsigned int bitrate = 500000;
    int secs = 3;
    unsigned int i;
    int withFrames = 0, denied = 0, nolink = 0;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc >= 2) bitrate = (unsigned int)atoi(argv[1]);
    if (argc >= 3) secs = atoi(argv[2]);

    printf("=== Vector 逐通道监听   %u bps，每路 %d 秒 ===\n\n", bitrate, secs);

    h = LoadLibraryA("vxlapi.dll");
    if (!h) h = LoadLibraryA("C:\\Windows\\SysWOW64\\vxlapi.dll");
    if (!h) { printf("  加载 vxlapi.dll 失败\n"); return 1; }

    pOpen       = (PF_VOID)      GetProcAddress(h, "xlOpenDriver");
    pClose      = (PF_VOID)      GetProcAddress(h, "xlCloseDriver");
    pCfg        = (PF_CFG)       GetProcAddress(h, "xlGetDriverConfig");
    pOpenPort   = (PF_OPENPORT)  GetProcAddress(h, "xlOpenPort");
    pClosePort  = (PF_CLOSEPORT) GetProcAddress(h, "xlClosePort");
    pSetAppl    = (PF_SETAPPL)   GetProcAddress(h, "xlSetApplConfig");
    pSetBitrate = (PF_SETBITRATE)GetProcAddress(h, "xlCanSetChannelBitrate");
    pActivate   = (PF_ACTIVATE)  GetProcAddress(h, "xlActivateChannel");
    pDeactivate = (PF_DEACTIVATE)GetProcAddress(h, "xlDeactivateChannel");
    pReceive    = (PF_RECEIVE)   GetProcAddress(h, "xlReceive");

    if (!pOpen || !pCfg || !pOpenPort || !pReceive) {
        printf("  关键导出缺失\n"); return 1;
    }
    if (pOpen() != 0) { printf("  xlOpenDriver 失败\n"); return 1; }

    memset(buf, 0, sizeof(buf));
    if (pCfg(buf) != 0) { printf("  xlGetDriverConfig 失败\n"); return 1; }
    cfg = (XlDriverConfig *)buf;
    printf("  channelCount = %u\n\n", cfg->channelCount);

    for (i = 0; i < cfg->channelCount && i < XL_CONFIG_MAX_CHANNELS; i++) {
        XlChannelConfig *c = &cfg->channel[i];
        XLportHandle port = 0;
        XLaccess perm, mask = c->channelMask;
        XLstatus st;
        int virt, ver, got = 0;
        unsigned int total = 0, errFrames = 0, chipKnown = 0;
        int chipState = -1, tec = 0, rec = 0;
        DWORD t0;
        char first[160];
        first[0] = '\0';

        /* 虚拟通道和非 CAN 通道跳过 */
        if (!(c->channelBusCapabilities & XL_BUS_COMPATIBLE_CAN)) {
            printf("  通道%u \"%s\"  不是 CAN，跳过\n", i, c->name);
            continue;
        }
        virt = 0;
        {
            const char *a = c->transceiverName, *b = c->name;
            if ((a && (strstr(a, "irtual") || strstr(a, "IRTUAL"))) ||
                (b && (strstr(b, "irtual") || strstr(b, "IRTUAL")))) virt = 1;
        }
        if (virt) {
            printf("  通道%u \"%s\"  虚拟通道，跳过\n", i, c->name);
            continue;
        }

        printf("  === 通道%u \"%s\"  mask=0x%I64X  transceiver=\"%s\" ===\n",
               i, c->name, (unsigned long long)mask, c->transceiverName);

        /* 分配 + 打开，试两个接口版本 */
        for (ver = 0; ver < 2 && !got; ver++) {
            unsigned int verValue = ver ? 4u : 3u;
            XLaccess p = mask;
            if (pSetAppl) {
                XLstatus sa = pSetAppl("UniVciBridge", 0, c->hwType, c->hwIndex,
                                       c->hwChannel, XL_BUS_TYPE_CAN);
                printf("     xlSetApplConfig -> %u\n", sa);
            }
            port = 0;
            p = mask;
            st = pOpenPort(&port, "UniVciBridge", mask, &p, 8192,
                           verValue, XL_BUS_TYPE_CAN);
            printf("     xlOpenPort(ver=%u) -> %u  port=%u  perm=0x%I64X\n",
                   verValue, st, (unsigned)port, (unsigned long long)p);
            if (st == 0 && p != 0) {
                perm = p;
                got = 1;
            } else {
                if (pClosePort && port) pClosePort(port);
                port = 0;
            }
        }

        if (!got) {
            printf("     ★ 拿不到 init 权限（被别的程序占着？）\n\n");
            denied++;
            continue;
        }

        /* 设波特率 + 激活 */
        if (pSetBitrate) {
            st = pSetBitrate(port, mask, bitrate);
            printf("     xlCanSetChannelBitrate(%u) -> %u\n", bitrate, st);
        }
        if (pActivate) {
            /* ★ 顺序是 (port, mask, busType, flags) —— 别写反，写反会得到 112 */
            st = pActivate(port, mask, XL_BUS_TYPE_CAN, 8 /* XL_ACTIVATE_RESET_CLOCK */);
            printf("     xlActivateChannel -> %u\n", st);
        }

        /* 监听 */
        printf("     监听 %d 秒...\n", secs);
        t0 = GetTickCount();
        while ((int)(GetTickCount() - t0) < secs * 1000) {
            static unsigned char ev[128 * 64];
            unsigned int n = 128;
            unsigned int k;
            XLstatus rs;

            memset(ev, 0, sizeof(ev));
            rs = pReceive(port, &n, ev);
            if (rs != 0 && rs != 10) {          /* 10 = XL_ERR_QUEUE_IS_EMPTY */
                printf("     xlReceive -> %u\n", rs);
                Sleep(20);
                continue;
            }
            {
                unsigned char *base = ev;
                for (k = 0; k < n && k < 128; k++) {
                    unsigned char *e = base + (size_t)k * 48;   /* sizeof(XLevent)=48 */
                    unsigned char tag = e[0];
                    if (tag == TAG_CHIP_STATE) {
                        chipState = e[16];      /* tagData 起于 16，首字节 busStatus */
                        tec = e[17];
                        rec = e[18];
                        chipKnown = 1;
                    } else if (tag == TAG_RECEIVE_MSG) {
                        /* tagData.msg: id@16 flags@20 dlc@22 res1@24 data@32 res2@40 */
                        unsigned long id = *(unsigned long *)(e + 16);
                        unsigned short dlc = *(unsigned short *)(e + 22);
                        total++;
                        if (total == 1) {
                            _snprintf(first, sizeof(first) - 1,
                                      "第一帧：ID=0x%lX dlc=%u 数据=%02X %02X %02X %02X",
                                      id, (unsigned)dlc,
                                      e[32], e[33], e[34], e[35]);
                            first[sizeof(first) - 1] = '\0';
                        }
                    } else if (tag == 0) {
                        /* 空事件 */
                    }
                }
            }
            Sleep(10);
        }

        printf("     → 收到 %u 帧", total);
        if (first[0]) printf("   %s", first);
        printf("\n");
        if (chipKnown)
            printf("     总线状态 busStatus=%d (%s) TEC=%d REC=%d\n",
                   chipState,
                   chipState == 0 ? "正常" : chipState == 1 ? "总线关闭" :
                   chipState == 2 ? "错误主动" : chipState == 3 ? "错误被动" : "?",
                   tec, rec);
        else
            printf("     没有收到总线状态事件（正常：驱动只在状态变化时上报）\n");

        if (total > 0) withFrames++;
        else if (chipKnown && chipState != 0) nolink++;
        printf("\n");

        if (pDeactivate) pDeactivate(port, mask);
        if (pClosePort && port) pClosePort(port);
    }

    printf("=== 汇总 ===\n");
    printf("  有报文的通道：%d 路\n", withFrames);
    printf("  拿不到权限  ：%d 路\n", denied);
    printf("  有报文但错误：%d 路\n", nolink);
    if (withFrames == 0)
        printf("\n  一路都没收到。检查：\n"
               "    ① 对端真的在发吗（ZCANPRO 里能看到发送计数在涨？）\n"
               "    ② CAN_H / CAN_L 接对了吗\n"
               "    ③ 120Ω 终端电阻接了吗（两端各一个，或一端一个）\n"
               "    ④ 波特率一致吗（对端是多少，这里就传多少）\n");

    if (pClose) pClose();
    return 0;
}
