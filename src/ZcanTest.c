/* ZcanTest.c —— 测「假 zlgcan.dll」的 ABI 和基本调用序列
 *
 * 重点不是"能不能收到数据"（需要第二个节点），而是：
 *   1. 导出名能不能按名字取到（不带 @N 装饰）
 *   2. __stdcall 参数清理对不对（不对会立刻栈溢出/崩溃）
 *   3. 调用链能不能走到底层盒子
 */
#include <windows.h>
#include <stdio.h>

typedef unsigned int UINT;
typedef unsigned char BYTE;

typedef void *DEVICE_HANDLE;
typedef void *CHANNEL_HANDLE;

typedef struct {
    UINT can_id;
    BYTE can_dlc, __pad, __res0, __res1;
    BYTE data[8];
} can_frame;

typedef struct { can_frame frame; UINT transmit_type; } ZCAN_Transmit_Data;
typedef struct { can_frame frame; unsigned long long timestamp; } ZCAN_Receive_Data;

typedef struct {
    UINT can_type;
    union {
        struct { UINT acc_code, acc_mask, reserved; BYTE filter, timing0, timing1, mode; } can;
        struct { UINT a, b, c, d, e; BYTE f, g; unsigned short h; UINT i; } canfd;
    };
} ZCAN_CHANNEL_INIT_CONFIG;

typedef DEVICE_HANDLE (__stdcall *PF_Open)(UINT, UINT, UINT);
typedef UINT (__stdcall *PF_Close)(DEVICE_HANDLE);
typedef CHANNEL_HANDLE (__stdcall *PF_InitCAN)(DEVICE_HANDLE, UINT, ZCAN_CHANNEL_INIT_CONFIG *);
typedef UINT (__stdcall *PF_Start)(CHANNEL_HANDLE);
typedef UINT (__stdcall *PF_TX)(CHANNEL_HANDLE, ZCAN_Transmit_Data *, UINT);
typedef UINT (__stdcall *PF_RX)(CHANNEL_HANDLE, ZCAN_Receive_Data *, UINT, int);
typedef UINT (__stdcall *PF_Num)(CHANNEL_HANDLE, BYTE);
typedef UINT (__stdcall *PF_Inf)(DEVICE_HANDLE, void *);

int main(int argc, char **argv)
{
    HMODULE d;
    PF_Open pOpen; PF_Close pClose; PF_InitCAN pInit; PF_Start pStart;
    PF_TX pTx; PF_RX pRx; PF_Num pNum; PF_Inf pInf;
    DEVICE_HANDLE dh;
    CHANNEL_HANDLE ch;
    ZCAN_CHANNEL_INIT_CONFIG cfg;
    ZCAN_Transmit_Data tx;
    ZCAN_Receive_Data rx;
    UINT r;
    const char *dll = (argc >= 2) ? argv[1] : "zlgcan.dll";

    d = LoadLibraryA(dll);
    if (!d) { printf("  LoadLibrary(%s) 失败: %lu\n", dll, GetLastError()); return 1; }
    printf("  ✓ 已加载 %s\n", dll);

    pOpen  = (PF_Open)  GetProcAddress(d, "ZCAN_OpenDevice");
    pClose = (PF_Close) GetProcAddress(d, "ZCAN_CloseDevice");
    pInit  = (PF_InitCAN)GetProcAddress(d, "ZCAN_InitCAN");
    pStart = (PF_Start) GetProcAddress(d, "ZCAN_StartCAN");
    pTx    = (PF_TX)    GetProcAddress(d, "ZCAN_Transmit");
    pRx    = (PF_RX)    GetProcAddress(d, "ZCAN_Receive");
    pNum   = (PF_Num)   GetProcAddress(d, "ZCAN_GetReceiveNum");
    pInf   = (PF_Inf)   GetProcAddress(d, "ZCAN_GetDeviceInf");

    printf("  按名字取导出：Open=%p Init=%p Tx=%p Rx=%p Inf=%p\n",
           (void *)pOpen, (void *)pInit, (void *)pTx, (void *)pRx, (void *)pInf);
    if (!pOpen || !pInit || !pTx || !pRx) { printf("  ★ 关键导出缺失\n"); return 1; }

    dh = pOpen(4 /*ZCAN_USBCAN2*/, 0, 0);
    printf("  ZCAN_OpenDevice(4) -> %p\n", dh);
    if (!dh) { printf("  · 没有硬件，提前结束（这不代表 ABI 有问题）\n"); return 0; }

    if (pInf) {
        char info[128];
        ZeroMemory(info, sizeof(info));
        r = pInf(dh, info);
        printf("  ZCAN_GetDeviceInf -> %lu  can_Num=%d hwType='%s'\n",
               r, (int)info[10], info + 31);
    }

    ZeroMemory(&cfg, sizeof(cfg));
    cfg.can_type = 0;              /* TYPE_CAN */
    cfg.can.acc_mask = 0xFFFFFFFF;
    cfg.can.filter = 1;
    cfg.can.timing0 = 0x00;
    cfg.can.timing1 = 0x1C;        /* 500k */
    ch = pInit(dh, 0, &cfg);
    printf("  ZCAN_InitCAN(ch0,500k) -> %p\n", ch);

    if (ch) {
        printf("  ZCAN_StartCAN -> %lu\n", pStart(ch));

        ZeroMemory(&tx, sizeof(tx));
        tx.frame.can_id = 0x321;
        tx.frame.can_dlc = 8;
        tx.frame.data[0] = 0xDE; tx.frame.data[1] = 0xAD;
        tx.frame.data[2] = 0xBE; tx.frame.data[3] = 0xEF;
        printf("  ZCAN_Transmit(1帧) -> %lu\n", pTx(ch, &tx, 1));

        Sleep(200);
        printf("  ZCAN_GetReceiveNum -> %lu\n", pNum ? pNum(ch, 0) : 0);

        ZeroMemory(&rx, sizeof(rx));
        r = pRx(ch, &rx, 1, 100);
        printf("  ZCAN_Receive -> %lu", r);
        if (r) printf("  ID=0x%X dlc=%d data=%02X %02X", rx.frame.can_id, rx.frame.can_dlc,
                      rx.frame.data[0], rx.frame.data[1]);
        printf("\n");
    }

    printf("  ZCAN_CloseDevice -> %lu\n", pClose(dh));
    return 0;
}
