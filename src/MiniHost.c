/* MiniHost.c —— 极小的 VCI 宿主程序（v2：支持自测模式 + 接收显示）
 *
 * 为什么需要它：
 *   桥用 GetModuleFileName(NULL) 取「自己所在目录」找 ini / 厂家库 / 日志。
 *   用 python.exe 当宿主，那个目录是 python 的安装目录，不是沙盘 —— 测不准。
 *   把本程序放进沙盘，桥的「自己所在目录」就等于沙盘。
 *
 * 用法:
 *   MiniHost.exe <dll路径> [设备型号] [模式] [接收等待毫秒]
 *
 *   模式 0 = 正常（要外部 ACK 才能发成功）
 *   模式 1 = 只听（不发）
 *   模式 2 = 自测/回环（自己发自己收，不需要外部对端）★
 *
 * 自测模式是验证「收发链路」的关键：
 *   单块盒子没接对端时，正常模式发送会一直被 ACK 错误挡住；
 *   自测模式把 TX 直接回环到 RX，能证明桥的发送和接收都通。
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

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
} CFG;

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
} OBJ;
#pragma pack(pop)

int main(int argc, char **argv)
{
    HMODULE d;
    DWORD (__stdcall *pOpen)(DWORD, DWORD, DWORD);
    DWORD (__stdcall *pClose)(DWORD, DWORD);
    DWORD (__stdcall *pInit)(DWORD, DWORD, DWORD, CFG *);
    DWORD (__stdcall *pStart)(DWORD, DWORD, DWORD);
    DWORD (__stdcall *pTX)(DWORD, DWORD, DWORD, OBJ *, DWORD);
    DWORD (__stdcall *pRX)(DWORD, DWORD, DWORD, OBJ *, DWORD, int);
    DWORD (__stdcall *pRef)(DWORD, DWORD, DWORD, DWORD, void *);
    DWORD (__stdcall *pNum)(DWORD, DWORD, DWORD);
    DWORD dev, mode;
    int waitms;
    CFG cfg;
    OBJ o, rx[32];
    int ch, ok = -1;
    DWORD code = 0x060007;

    if (argc < 2) {
        printf("usage: MiniHost <dll> [devtype] [mode] [waitms]\n");
        return 2;
    }
    dev    = (argc >= 3) ? (DWORD)atoi(argv[2]) : 4;
    mode   = (argc >= 4) ? (DWORD)atoi(argv[3]) : 0;
    waitms = (argc >= 5) ? atoi(argv[4]) : 300;

    d = LoadLibraryA(argv[1]);
    if (!d) { printf("  LoadLibrary failed: %lu\n", GetLastError()); return 1; }

    /* 周立功风格带 VCI_ 前缀；广成 ECanVci 不带前缀 —— 两种都试 */
#define GETPF(n) GetProcAddress(d, "VCI_" n); if (!p##n) p##n = GetProcAddress(d, n)
    pOpen  = (void *)GetProcAddress(d, "VCI_OpenDevice");
    if (!pOpen)  pOpen  = (void *)GetProcAddress(d, "OpenDevice");
    pClose = (void *)GetProcAddress(d, "VCI_CloseDevice");
    if (!pClose) pClose = (void *)GetProcAddress(d, "CloseDevice");
    pInit  = (void *)GetProcAddress(d, "VCI_InitCAN");
    if (!pInit)  pInit  = (void *)GetProcAddress(d, "InitCAN");
    pStart = (void *)GetProcAddress(d, "VCI_StartCAN");
    if (!pStart) pStart = (void *)GetProcAddress(d, "StartCAN");
    pTX    = (void *)GetProcAddress(d, "VCI_Transmit");
    if (!pTX)    pTX    = (void *)GetProcAddress(d, "Transmit");
    pRX    = (void *)GetProcAddress(d, "VCI_Receive");
    if (!pRX)    pRX    = (void *)GetProcAddress(d, "Receive");
    pRef   = (void *)GetProcAddress(d, "VCI_SetReference");
    if (!pRef)   pRef   = (void *)GetProcAddress(d, "SetReference");
    pNum   = (void *)GetProcAddress(d, "VCI_GetReceiveNum");
    if (!pNum)   pNum   = (void *)GetProcAddress(d, "GetReceiveNum");
    if (!pOpen || !pClose) { printf("  missing exports\n"); return 1; }

    printf("  === dev=%lu mode=%lu ===\n", dev, mode);
    printf("  OpenDevice(%lu) -> %lu\n", dev, pOpen(dev, 0, 0));

    if (pRef) printf("  SetReference  -> %lu\n", pRef(dev, 0, 0, 0, &code));

    if (pInit && pStart) {
        ZeroMemory(&cfg, sizeof(cfg));
        cfg.Mode = (BYTE)mode;
        cfg.Timing0 = 0x00;
        cfg.Timing1 = 0x1C;
        cfg.Filter = 1;
        cfg.AccMask = 0xFFFFFFFF;
        for (ch = 0; ch < 4; ch++) {
            DWORD ri = pInit(dev, 0, ch, &cfg);
            DWORD rs = (ri == 1) ? pStart(dev, 0, ch) : 0;
            printf("  ch%d: InitCAN=%lu StartCAN=%lu\n", ch, ri, rs);
            if (ri == 1 && rs == 1 && ok < 0) ok = ch;
        }
    }

    if (ok >= 0 && pTX) {
        ZeroMemory(&o, sizeof(o));
        o.ID = 0x321;
        o.DataLen = 8;
        o.Data[0] = 0xAA; o.Data[1] = 0xBB;
        o.Data[2] = 0xCC; o.Data[3] = 0xDD;
        printf("  Transmit ch%d -> %lu\n", ok, pTX(dev, 0, (DWORD)ok, &o, 1));
        Sleep(waitms);
        if (pNum) printf("  待收帧数 -> %lu\n", pNum(dev, 0, (DWORD)ok));
        if (pRX) {
            DWORD got = pRX(dev, 0, (DWORD)ok, rx, 32, 200);
            printf("  Receive -> %lu 帧\n", got);
            if (got > 0 && got <= 32) {
                DWORD k;
                for (k = 0; k < got; k++)
                    printf("     [%lu] ID=0x%lX 长度=%u 数据=%02X %02X %02X %02X\n",
                           k, (unsigned long)rx[k].ID, rx[k].DataLen,
                           rx[k].Data[0], rx[k].Data[1], rx[k].Data[2], rx[k].Data[3]);
            }
        }
    }

    printf("  CloseDevice   -> %lu\n", pClose(dev, 0));
    return 0;
}
