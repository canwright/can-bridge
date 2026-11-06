/*
 * VciErrProbe.c —— 错误状态探针：直接问 DLL "你有没有错"
 *
 * 目的
 * ----
 * 同一个物理操作（拔插 CAN 线），
 *   真周立功盒子 + 真 USBCAN.dll → ZCANPRO 的错误帧计数不动
 *   我们的桥 + VN1630A           → 一直在涨
 *
 * 到底是谁的差别？这个工具直接问 DLL：
 *
 *   1. VCI_ReadErrInfo 返回的 ErrCode 变不变？
 *   2. VCI_ReadCANStatus 里的错误计数变不变？
 *   3. VCI_GetReceiveNum 有没有帧进来（证明通道是活的）？
 *
 * 用法
 * ----
 *   VciErrProbe.exe <DLL路径或名字> <设备型号> <通道> <秒数>
 *
 *   对比真周立功：
 *       copy "X:\ZCANPRO\USBCAN.dll.zlg.bak" zlg.dll
 *       VciErrProbe.exe zlg.dll 4 0 20
 *       → 拔插 CAN 线，看它报不报
 *
 *   对比我们的桥：
 *       VciErrProbe.exe USBCAN.dll 4 0 20
 *
 * 输出原则
 * --------
 *   只在【值变化】时打印一行，外加每秒一次心跳。
 *   拔插动作会在图上留下清晰的时间点。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef DWORD (__stdcall *PF_OPEN) (DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_CLOSE)(DWORD, DWORD);
typedef DWORD (__stdcall *PF_INIT) (DWORD, DWORD, DWORD, void *);
typedef DWORD (__stdcall *PF_START)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_E)    (DWORD, DWORD, DWORD, void *);
typedef DWORD (__stdcall *PF_NUM)  (DWORD, DWORD, DWORD);

/* 16 字节初始化配置（E-U 布局），和桥里一致 */
#pragma pack(push, 1)
typedef struct {
    DWORD AccCode;
    DWORD AccMask;
    DWORD Reserved;
    BYTE  Filter;
    BYTE  Timing0;
    BYTE  Timing1;
    BYTE  Mode;
} INITCFG;
#pragma pack(pop)

int main(int argc, char **argv)
{
    const char *dll = (argc >= 2) ? argv[1] : "USBCAN.dll";
    DWORD devType   = (argc >= 3) ? (DWORD)strtoul(argv[2], NULL, 0) : 4u;
    DWORD chan      = (argc >= 4) ? (DWORD)strtoul(argv[3], NULL, 0) : 0u;
    int   secs      = (argc >= 5) ? atoi(argv[4]) : 20;
    HMODULE h;
    PF_OPEN  pO; PF_CLOSE pC; PF_INIT pI; PF_START pS;
    PF_E     pErr, pSt;
    PF_NUM   pNum;
    INITCFG cfg;
    unsigned char err[16];
    unsigned int  st[8];
    int i, lastCode = -1, lastTec = -1, lastRec = -1, lastBaud = -1;
    DWORD t0;

    printf("============================================================\n");
    printf("  VCI 错误状态探针\n");
    printf("============================================================\n");
    printf("  DLL      %s\n", dll);
    printf("  型号     %lu\n", (unsigned long)devType);
    printf("  通道     %lu\n", (unsigned long)chan);
    printf("  时长     %d 秒\n\n", secs);

    h = LoadLibraryA(dll);
    if (!h) { printf("[x] 加载失败 (%lu)：%s\n", GetLastError(), dll); return 1; }
    printf("[ok] DLL 已加载\n");

    pO   = (PF_OPEN) GetProcAddress(h, "VCI_OpenDevice");
    pC   = (PF_CLOSE)GetProcAddress(h, "VCI_CloseDevice");
    pI   = (PF_INIT) GetProcAddress(h, "VCI_InitCAN");
    pS   = (PF_START)GetProcAddress(h, "VCI_StartCAN");
    pErr = (PF_E)    GetProcAddress(h, "VCI_ReadErrInfo");
    pSt  = (PF_E)    GetProcAddress(h, "VCI_ReadCANStatus");
    pNum = (PF_NUM)  GetProcAddress(h, "VCI_GetReceiveNum");
    if (!pO || !pI || !pS) { printf("[x] 缺关键导出\n"); return 1; }
    printf("[ok] 导出齐全（ReadErrInfo=%s ReadCANStatus=%s）\n\n",
           pErr ? "有" : "无", pSt ? "有" : "无");

    printf("[..] OpenDevice -> %lu\n", (unsigned long)pO(devType, 0, 0));
    ZeroMemory(&cfg, sizeof(cfg));
    cfg.AccMask = 0xFFFFFFFFu;
    cfg.Filter = 1;
    cfg.Timing0 = 0x00; cfg.Timing1 = 0x1C;      /* 500k */
    printf("[..] InitCAN    -> %lu\n", (unsigned long)pI(devType, 0, chan, &cfg));
    printf("[..] StartCAN   -> %lu\n\n", (unsigned long)pS(devType, 0, chan));

    printf("---- 开始采样（现在去拔插 CAN 线）----\n");
    printf("%8s  %-8s %-4s %-4s %-6s %s\n", "时间ms", "ErrCode", "TEC", "REC", "待收帧", "说明");

    t0 = GetTickCount();
    for (i = 0; i < secs * 20; i++) {
        DWORD el = GetTickCount() - t0;
        DWORD num = pNum ? pNum(devType, 0, chan) : 0;
        int code = -1, tec = -1, rec = -1, baud = -1;

        if (pErr) {
            ZeroMemory(err, sizeof(err));
            if (pErr(devType, 0, chan, err) == 1) {
                code = err[0];
                tec  = err[1];
                rec  = err[2];
            }
        }
        if (pSt) {
            ZeroMemory(st, sizeof(st));
            if (pSt(devType, 0, chan, st) == 1) baud = (int)st[0];
        }

        if (code != lastCode || tec != lastTec || rec != lastRec || baud != lastBaud) {
            printf("%8lu  %-8d %-4d %-4d %-6lu %s\n",
                   (unsigned long)el, code, tec, rec, (unsigned long)num,
                   (code != lastCode) ? "← ErrCode 变了" :
                   (baud != lastBaud) ? "← 波特率变了" : "← 计数变了");
            lastCode = code; lastTec = tec; lastRec = rec; lastBaud = baud;
            fflush(stdout);
        }
        Sleep(50);
    }

    printf("\n---- 结束 ----\n");
    if (pC) pC(devType, 0);
    return 0;
}
