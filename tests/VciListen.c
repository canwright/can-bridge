/*
 * VciListen.c —— 用 VCI 接口监听：走的是【真正的产品路径】
 *
 *   本程序 → USBCAN.dll（桥）→ vxlapi.dll → VN1630A → 总线
 *
 * 和 VxlListen.exe 的区别：
 *   VxlListen 直接调 vxlapi，只验证 Vector 驱动的收发
 *   VciListen 调的是 VCI_*，也就是 ZCANPRO 用的同一套接口 ——
 *   它通了，等于 ZCANPRO 那条路也通了
 *
 * 用法：
 *   VciListen.exe [型号] [通道] [监听秒数]
 *   默认 21（USBCAN-2E-U）、通道 0、5 秒
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
/* 官方 16 字节布局（不是 18 字节 —— 那个坑踩过） */
typedef struct {
    DWORD AccCode;
    DWORD AccMask;
    DWORD Reserved;
    BYTE  Filter;
    BYTE  Timing0;
    BYTE  Timing1;
    BYTE  Mode;
} VCI_CFG;

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
} VCI_OBJ;
#pragma pack(pop)

typedef char CHECK_CFG[(sizeof(VCI_CFG) == 16) ? 1 : -1];
typedef char CHECK_OBJ[(sizeof(VCI_OBJ) == 24) ? 1 : -1];

typedef DWORD (__stdcall *PF_OPEN)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_CLOSE)(DWORD, DWORD);
typedef DWORD (__stdcall *PF_INIT)(DWORD, DWORD, DWORD, VCI_CFG *);
typedef DWORD (__stdcall *PF_START)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_RX)(DWORD, DWORD, DWORD, VCI_OBJ *, DWORD, int);
typedef DWORD (__stdcall *PF_NUM)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_TX)(DWORD, DWORD, DWORD, VCI_OBJ *, DWORD);

int main(int argc, char **argv)
{
    HMODULE d;
    PF_OPEN  pOpen;
    PF_CLOSE pClose;
    PF_INIT  pInit;
    PF_START pStart;
    PF_RX    pRx;
    PF_NUM   pNum;
    PF_TX    pTx;
    DWORD dev, ch;
    int secs, k, total = 0;
    VCI_CFG cfg;
    VCI_OBJ rx[64];
    DWORD t0;

    setvbuf(stdout, NULL, _IONBF, 0);

    dev  = (argc >= 2) ? (DWORD)atoi(argv[1]) : 21;
    ch   = (argc >= 3) ? (DWORD)atoi(argv[2]) : 0;
    secs = (argc >= 4) ? atoi(argv[3]) : 5;

    printf("=== VCI 监听（走桥）  型号=%lu 通道=%lu  %d 秒 ===\n\n",
           (unsigned long)dev, (unsigned long)ch, secs);

    d = LoadLibraryA("USBCAN.dll");
    if (!d) { printf("  加载 USBCAN.dll 失败\n"); return 1; }

    pOpen  = (PF_OPEN)  GetProcAddress(d, "VCI_OpenDevice");
    pClose = (PF_CLOSE) GetProcAddress(d, "VCI_CloseDevice");
    pInit  = (PF_INIT)  GetProcAddress(d, "VCI_InitCAN");
    pStart = (PF_START) GetProcAddress(d, "VCI_StartCAN");
    pRx    = (PF_RX)    GetProcAddress(d, "VCI_Receive");
    pNum   = (PF_NUM)   GetProcAddress(d, "VCI_GetReceiveNum");
    pTx    = (PF_TX)    GetProcAddress(d, "VCI_Transmit");
    if (!pOpen || !pInit || !pStart || !pRx) { printf("  导出不全\n"); return 1; }

    printf("  OpenDevice(%lu) -> %lu\n", (unsigned long)dev, pOpen(dev, 0, 0));

    ZeroMemory(&cfg, sizeof(cfg));
    cfg.AccMask = 0xFFFFFFFF;
    cfg.Filter = 1;
    cfg.Timing0 = 0x00;
    cfg.Timing1 = 0x1C;      /* 500k（16MHz 时序表） */
    printf("  InitCAN(通道%lu) -> %lu\n", (unsigned long)ch, pInit(dev, 0, ch, &cfg));
    printf("  StartCAN(通道%lu) -> %lu\n\n", (unsigned long)ch, pStart(dev, 0, ch));

    printf("  监听 %d 秒（对端在发就能看到）...\n", secs);
    t0 = GetTickCount();
    while ((int)(GetTickCount() - t0) < secs * 1000) {
        DWORD got;
        if (pNum) {
            DWORD n = pNum(dev, 0, ch);
            if (n == 0) { Sleep(50); continue; }
        }
        got = pRx(dev, 0, ch, rx, 64, 100);
        for (k = 0; k < (int)got && k < 64; k++) {
            total++;
            printf("  [%d] ID=0x%lX  dlc=%u  数据=%02X %02X %02X %02X %02X %02X %02X %02X %s%s\n",
                   total, (unsigned long)rx[k].ID, (unsigned)rx[k].DataLen,
                   rx[k].Data[0], rx[k].Data[1], rx[k].Data[2], rx[k].Data[3],
                   rx[k].Data[4], rx[k].Data[5], rx[k].Data[6], rx[k].Data[7],
                   rx[k].ExternFlag ? "[扩展]" : "",
                   rx[k].RemoteFlag ? "[远程]" : "");
        }
        Sleep(20);
    }

    printf("\n  === 共收到 %d 帧 ===\n", total);
    if (total == 0)
        printf("  一帧都没有。桥打开了、启动了，但没报文 —— 看桥日志里的 VECTOR 部分。\n");

    pClose(dev, 0);
    return 0;
}
