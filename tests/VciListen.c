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
    if (argc >= 3 && strcmp(argv[2], "unit") == 0) {
        /*
         unit 模式：每 1000ms 发一帧，记录【墙上时间】和【报文时间戳】，
         事后就能精确算出 VCI TimeStamp 的单位到底是 0.1ms / ms / 其他。
         用法：VciListen.exe 21 unit [帧数]
         */
        int nf = (argc >= 4) ? atoi(argv[3]) : 6;
        HMODULE d6;
        /*
         ★ ch 在这个分支里还没被赋值（下面那行 ch = atoi(argv[2]) 在特殊模式之后），
           之前直接用 ch 拿到的是栈上的垃圾值 —— 日志里出现"通道 21 / 通道 -20"。
           这里显式指定。
         */
        ch  = (argc >= 5) ? (DWORD)atoi(argv[4]) : 0;
        d6 = LoadLibraryA("USBCAN.dll");
        PF_OPEN  uO; PF_INIT uI; PF_START uS; PF_RX uR; PF_TX uT; PF_CLOSE uC;
        VCI_CFG c6;
        int f;
        LARGE_INTEGER freq, t0, tn;
        double lastWall = 0;
        DWORD lastTs = 0;
        int first = 1;

        printf("=== 时间戳单位标定  每 1000ms 发一帧  共 %d 帧 ===\n\n", nf);
        if (!d6) { printf("  加载失败\n"); return 1; }
        uO = (PF_OPEN)  GetProcAddress(d6, "VCI_OpenDevice");
        uC = (PF_CLOSE) GetProcAddress(d6, "VCI_CloseDevice");
        uI = (PF_INIT)  GetProcAddress(d6, "VCI_InitCAN");
        uS = (PF_START) GetProcAddress(d6, "VCI_StartCAN");
        uR = (PF_RX)    GetProcAddress(d6, "VCI_Receive");
        uT = (PF_TX)    GetProcAddress(d6, "VCI_Transmit");

        QueryPerformanceFrequency(&freq);
        printf("  OpenDevice -> %lu\n", uO(dev, 0, 0));
        ZeroMemory(&c6, sizeof(c6));
        c6.AccMask = 0xFFFFFFFF; c6.Filter = 1; c6.Timing0 = 0x00; c6.Timing1 = 0x1C;
        /*
         ★ 必须把另一个通道也开起来当 ACK 对端。

           CAN 没有其他节点应答时，发送不会完成，也就收不到自己的回显 ——
           上一版只开了 ch，结果全是"没收到自己的帧"。
         */
        uI(dev, 0, ch, &c6); uS(dev, 0, ch);
        uI(dev, 0, 1 - ch, &c6); uS(dev, 0, 1 - ch);
        QueryPerformanceCounter(&t0);
        printf("  墙上时间(s)   报文时间戳   差值(单位)   单位换算\n\n");

        for (f = 0; f < nf; f++) {
            VCI_OBJ t, buf[32];
            DWORD g;
            double wall;
            QueryPerformanceCounter(&tn);
            wall = (double)(tn.QuadPart - t0.QuadPart) / (double)freq.QuadPart;

            ZeroMemory(&t, sizeof(t));
            t.ID = 0x1AB; t.DataLen = 2; t.Data[0] = (BYTE)f;
            uT(dev, 0, ch, &t, 1);
            Sleep(30);
            g = uR(dev, 0, ch, buf, 32, 100);
            if (g > 0) {
                DWORD ts = buf[g - 1].TimeStamp;
                if (!first) {
                    double dw = wall - lastWall;
                    long dt = (long)(ts - lastTs);
                    printf("  %8.3f      %10lu   %10ld   %.1f 单位/毫秒\n",
                           wall, (unsigned long)ts, dt,
                           dw > 0 ? (double)dt / (dw * 1000.0) : 0.0);
                } else {
                    printf("  %8.3f      %10lu   (首帧)\n", wall, (unsigned long)ts);
                    first = 0;
                }
                lastWall = wall; lastTs = ts;
            } else {
                printf("  %8.3f      (没收到自己的帧)\n", wall);
            }
            Sleep(970);
        }
        printf("\n  判读：上面最后一列（单位/毫秒）如果是\n");
        printf("     10   → 单位是 0.1 毫秒\n");
        printf("      1   → 单位是 毫秒\n");
        printf("   1000   → 单位是 微秒\n");
        printf(" 1000000  → 单位是 纳秒\n");
        uC(dev, 0);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "cross") == 0) {
        /*
         cross 模式：在通道 0 发，然后同时读两个通道。
         用来验证"两个通道真的在同一根总线上互通"：
           两个 VN 口对接时，ch0 发的帧应该 ch1 也收到（ch1 还负责应答）。
         用法：VciListen.exe 21 cross [秒数] [ID]
         */
        int secsX = (argc >= 4) ? atoi(argv[3]) : 4;
        DWORD idX = (argc >= 5) ? (DWORD)strtoul(argv[4], NULL, 16) : 0x1AB;
        HMODULE d5 = LoadLibraryA("USBCAN.dll");
        PF_OPEN  xO; PF_INIT xI; PF_START xS; PF_RX xR; PF_TX xT; PF_CLOSE xC;
        VCI_CFG c5;
        DWORD t0e;
        int c0x = 0, c1x = 0, seen0 = 0, seen1 = 0;

        printf("=== 交叉收发：ch0 发, 两路都读  型号=%lu  %d 秒  ID=0x%lX ===\n\n",
               (unsigned long)dev, secsX, (unsigned long)idX);
        if (!d5) { printf("  加载 USBCAN.dll 失败\n"); return 1; }
        xO = (PF_OPEN)  GetProcAddress(d5, "VCI_OpenDevice");
        xC = (PF_CLOSE) GetProcAddress(d5, "VCI_CloseDevice");
        xI = (PF_INIT)  GetProcAddress(d5, "VCI_InitCAN");
        xS = (PF_START) GetProcAddress(d5, "VCI_StartCAN");
        xR = (PF_RX)    GetProcAddress(d5, "VCI_Receive");
        xT = (PF_TX)    GetProcAddress(d5, "VCI_Transmit");

        printf("  OpenDevice -> %lu\n", xO(dev, 0, 0));
        ZeroMemory(&c5, sizeof(c5));
        c5.AccMask = 0xFFFFFFFF; c5.Filter = 1;
        c5.Timing0 = 0x00; c5.Timing1 = 0x1C;
        printf("  InitCAN  ch0=%lu ch1=%lu\n", xI(dev, 0, 0, &c5), xI(dev, 0, 1, &c5));
        printf("  StartCAN ch0=%lu ch1=%lu\n\n", xS(dev, 0, 0), xS(dev, 0, 1));

        {
            VCI_OBJ t;
            ZeroMemory(&t, sizeof(t));
            t.ID = idX; t.DataLen = 4;
            t.Data[0] = 0xC0; t.Data[1] = 0xFF; t.Data[2] = 0xEE; t.Data[3] = 0x01;
            printf("  [ch0 发] ID=0x%lX -> %lu\n\n",
                   (unsigned long)idX, xT(dev, 0, 0, &t, 1));
        }

        t0e = GetTickCount();
        {
            DWORD lastSend = 0, sentX = 0;
            while ((int)(GetTickCount() - t0e) < secsX * 1000) {
                int c;
                /* 每 50ms 发一帧，data[0..1] 是递增计数器 */
                if ((DWORD)(GetTickCount() - lastSend) >= 50) {
                    VCI_OBJ t2;
                    ZeroMemory(&t2, sizeof(t2));
                    t2.ID = idX; t2.DataLen = 8;
                    t2.Data[0] = (BYTE)(sentX & 0xFF);
                    t2.Data[1] = (BYTE)((sentX >> 8) & 0xFF);
                    t2.Data[2] = 0xC0; t2.Data[3] = 0xFF;
                    if (xT(dev, 0, 0, &t2, 1) == 1) sentX++;
                    lastSend = GetTickCount();
                }
                for (c = 0; c < 2; c++) {
                    VCI_OBJ buf[32];
                    DWORD n = xR(dev, 0, (DWORD)c, buf, 32, 5);
                    DWORD kk;
                    for (kk = 0; kk < n && kk < 32; kk++) {
                        if (buf[kk].ID != idX) continue;
                        if (c == 0) { c0x++; if (c0x <= 60)
                            printf("  [ch0] ts=%lu 数据=%02X %02X\n",
                                   (unsigned long)buf[kk].TimeStamp,
                                   buf[kk].Data[0], buf[kk].Data[1]); }
                        else        { c1x++; if (c1x <= 60)
                            printf("  [ch1] ts=%lu 数据=%02X %02X\n",
                                   (unsigned long)buf[kk].TimeStamp,
                                   buf[kk].Data[0], buf[kk].Data[1]); }
                    }
                }
                Sleep(3);
            }
            printf("\n  发送 %lu 帧\n", (unsigned long)sentX);
        }

        printf("\n  === ch0 收到 %d 帧，ch1 收到 %d 帧 ===\n", c0x, c1x);
        if (c0x > 0 && c1x > 0) {
            printf("  ✅ 两个通道在同一总线上互通，且各自独立收帧\n");
            printf("     （ch0 的回显 + ch1 的实收，说明另一路确实在应答）\n");
        } else if (c0x > 0 && c1x == 0) {
            printf("  · 只有 ch0 收到回显 —— 说明 CAN 2 没接在这根线上\n");
        } else if (c0x == 0 && c1x > 0) {
            printf("  · 只有 ch1 收到 —— ch0 没接上\n");
        } else {
            printf("  ★ 两路都没收到 —— 两个口没接在同一根线上，或没有终端电阻\n");
        }
        xC(dev, 0);
        return 0;
    }
    if (argc >= 4 && strcmp(argv[3], "spam") == 0) {
        /*
         spam 模式：每 100ms 发一帧，data[0] 是递增计数器。
         用法：VciListen.exe 21 <通道> <秒数> spam [ID]
         用来做"一个盒子发、另一个盒子收"的闭环测试。
         */
        HMODULE d4 = LoadLibraryA("USBCAN.dll");
        PF_OPEN  hO; PF_INIT hI; PF_START hS; PF_RX hR; PF_TX hT; PF_CLOSE hC;
        PF_NUM   hN;
        VCI_CFG c4;
        DWORD t0d, sent = 0;
        DWORD txId = (argc >= 6) ? (DWORD)strtoul(argv[5], NULL, 16) : 0x123;
        int recvN = 0;

        printf("=== spam 发送  型号=%lu 通道=%lu  %d 秒  ID=0x%lX ===\n\n",
               (unsigned long)dev, (unsigned long)ch, secs, (unsigned long)txId);
        if (!d4) { printf("  加载 USBCAN.dll 失败\n"); return 1; }
        hO = (PF_OPEN)  GetProcAddress(d4, "VCI_OpenDevice");
        hC = (PF_CLOSE) GetProcAddress(d4, "VCI_CloseDevice");
        hI = (PF_INIT)  GetProcAddress(d4, "VCI_InitCAN");
        hS = (PF_START) GetProcAddress(d4, "VCI_StartCAN");
        hR = (PF_RX)    GetProcAddress(d4, "VCI_Receive");
        hT = (PF_TX)    GetProcAddress(d4, "VCI_Transmit");
        hN = (PF_NUM)   GetProcAddress(d4, "VCI_GetReceiveNum");

        printf("  OpenDevice -> %lu\n", hO(dev, 0, 0));
        ZeroMemory(&c4, sizeof(c4));
        c4.AccMask = 0xFFFFFFFF; c4.Filter = 1;
        c4.Timing0 = 0x00; c4.Timing1 = 0x1C;
        printf("  InitCAN -> %lu  StartCAN -> %lu\n\n",
               hI(dev, 0, ch, &c4), hS(dev, 0, ch));

        t0d = GetTickCount();
        while ((int)(GetTickCount() - t0d) < secs * 1000) {
            VCI_OBJ o;
            DWORD r;
            ZeroMemory(&o, sizeof(o));
            o.ID = txId;
            o.DataLen = 8;
            o.Data[0] = (BYTE)(sent & 0xFF);
            o.Data[1] = (BYTE)((sent >> 8) & 0xFF);
            o.Data[2] = 0x5A; o.Data[3] = 0xA5;
            r = hT(dev, 0, ch, &o, 1);
            if (r == 1) sent++;
            /* 顺便收一下回显，确认自己发出去的在总线上 */
            {
                VCI_OBJ buf[32];
                DWORD g = hR(dev, 0, ch, buf, 32, 5);
                recvN += (int)g;
            }
            Sleep(100);
        }
        printf("  共发送 %lu 帧，自己收到 %d 帧（含回显/对端）\n",
               (unsigned long)sent, recvN);
        if (hN) printf("  待收队列剩 %lu\n", (unsigned long)hN(dev, 0, ch));
        hC(dev, 0);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "bothtx") == 0) {
        /* 两路同时：各发一帧，再同时读，验证互不阻塞、不串通道 */
        int secs3 = (argc >= 4) ? atoi(argv[3]) : 3;
        HMODULE d3 = LoadLibraryA("USBCAN.dll");
        PF_OPEN  gO; PF_INIT gI; PF_START gS; PF_RX gR; PF_TX gT; PF_CLOSE gC;
        VCI_CFG cf;
        VCI_OBJ tx;
        DWORD t0c;
        int got0 = 0, got1 = 0, saw; 

        printf("=== 两路同时收发  型号=%lu  %d 秒 ===\n\n", (unsigned long)dev, secs3);
        if (!d3) { printf("  加载 USBCAN.dll 失败\n"); return 1; }
        gO = (PF_OPEN)  GetProcAddress(d3, "VCI_OpenDevice");
        gC = (PF_CLOSE) GetProcAddress(d3, "VCI_CloseDevice");
        gI = (PF_INIT)  GetProcAddress(d3, "VCI_InitCAN");
        gS = (PF_START) GetProcAddress(d3, "VCI_StartCAN");
        gR = (PF_RX)    GetProcAddress(d3, "VCI_Receive");
        gT = (PF_TX)    GetProcAddress(d3, "VCI_Transmit");

        printf("  OpenDevice -> %lu\n", gO(dev, 0, 0));
        ZeroMemory(&cf, sizeof(cf));
        cf.AccMask = 0xFFFFFFFF; cf.Filter = 1;
        cf.Timing0 = 0x00; cf.Timing1 = 0x1C;
        printf("  InitCAN  ch0=%lu ch1=%lu\n",
               gI(dev, 0, 0, &cf), gI(dev, 0, 1, &cf));
        printf("  StartCAN ch0=%lu ch1=%lu\n\n",
               gS(dev, 0, 0), gS(dev, 0, 1));

        /* 通道 0 发 0x1AB，通道 1 发 0x2CD —— 两个 ID 不同，好区分 */
        ZeroMemory(&tx, sizeof(tx));
        tx.ID = 0x1AB; tx.DataLen = 4;
        tx.Data[0] = 0xAA; tx.Data[1] = 0x01;
        printf("  [ch0 发] ID=0x1AB -> %lu\n", gT(dev, 0, 0, &tx, 1));
        ZeroMemory(&tx, sizeof(tx));
        tx.ID = 0x2CD; tx.DataLen = 4;
        tx.Data[0] = 0xBB; tx.Data[1] = 0x02;
        printf("  [ch1 发] ID=0x2CD -> %lu\n\n", gT(dev, 0, 1, &tx, 1));

        t0c = GetTickCount();
        while ((int)(GetTickCount() - t0c) < secs3 * 1000) {
            int c;
            for (c = 0; c < 2; c++) {
                VCI_OBJ buf[16];
                DWORD n = gR(dev, 0, (DWORD)c, buf, 16, 10);
                DWORD kk;
                for (kk = 0; kk < n && kk < 16; kk++) {
                    if (buf[kk].ID != 0x1AB && buf[kk].ID != 0x2CD) continue;  /* 只看自己的 */
                    if (c == 0) got0++; else got1++;
                    printf("  [ch%u 收到自己的] ID=0x%lX 数据=%02X %02X\n",
                           (unsigned)c, (unsigned long)buf[kk].ID,
                           buf[kk].Data[0], buf[kk].Data[1]);
                }
            }
            Sleep(5);
        }
        saw = got0 + got1;
        printf("\n  === ch0 收到自己 %d 帧，ch1 收到自己 %d 帧 ===\n", got0, got1);
        if (got0 > 0 && got1 > 0) printf("  ✅ 两路同时收发都成功\n");
        else printf("  ★ 有一路没收到自己的回显\n");
        gC(dev, 0);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "both") == 0) {
        /* 双通道同时读：用来判断"同一帧是否两路都收到" */
        int secs2 = (argc >= 4) ? atoi(argv[3]) : 5;
        DWORD t0b;
        int c0 = 0, c1 = 0, k2;
        HMODULE d2 = LoadLibraryA("USBCAN.dll");
        PF_OPEN  fO; PF_INIT fI; PF_START fS; PF_RX fR; PF_NUM fN; PF_CLOSE fC;

        printf("=== 双通道同时读  型号=%lu  %d 秒 ===\n\n", (unsigned long)dev, secs2);
        if (!d2) { printf("  加载 USBCAN.dll 失败\n"); return 1; }
        fO = (PF_OPEN)  GetProcAddress(d2, "VCI_OpenDevice");
        fC = (PF_CLOSE) GetProcAddress(d2, "VCI_CloseDevice");
        fI = (PF_INIT)  GetProcAddress(d2, "VCI_InitCAN");
        fS = (PF_START) GetProcAddress(d2, "VCI_StartCAN");
        fR = (PF_RX)    GetProcAddress(d2, "VCI_Receive");
        fN = (PF_NUM)   GetProcAddress(d2, "VCI_GetReceiveNum");

        printf("  OpenDevice -> %lu\n", fO(dev, 0, 0));
        {
            VCI_CFG cf;
            ZeroMemory(&cf, sizeof(cf));
            cf.AccMask = 0xFFFFFFFF; cf.Filter = 1;
            cf.Timing0 = 0x00; cf.Timing1 = 0x1C;
            printf("  InitCAN ch0 -> %lu   ch1 -> %lu\n",
                   fI(dev, 0, 0, &cf), fI(dev, 0, 1, &cf));
        }
        printf("  StartCAN ch0 -> %lu  ch1 -> %lu\n\n",
               fS(dev, 0, 0), fS(dev, 0, 1));

        t0b = GetTickCount();
        while ((int)(GetTickCount() - t0b) < secs2 * 1000) {
            int c;
            for (c = 0; c < 2; c++) {
                VCI_OBJ buf[16];
                DWORD got = fR(dev, 0, (DWORD)c, buf, 16, 20);
                for (k2 = 0; k2 < (int)got && k2 < 16; k2++) {
                    if (c == 0) c0++; else c1++;
                    printf("  [ch%u] ID=0x%lX dlc=%u 数据=%02X %02X %02X %02X\n",
                           (unsigned)c, (unsigned long)buf[k2].ID,
                           (unsigned)buf[k2].DataLen,
                           buf[k2].Data[0], buf[k2].Data[1],
                           buf[k2].Data[2], buf[k2].Data[3]);
                }
            }
            Sleep(10);
        }
        printf("\n  === ch0 共 %d 帧，ch1 共 %d 帧 ===\n", c0, c1);
        fC(dev, 0);
        return 0;
    }
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

    /* 第 4 个参数是 tx 时，先在本通道发一帧 —— 用来验证分流：
       自己发的帧会被 Vector 回显，应该回到【同一个通道】。 */
    if (argc >= 5 && strcmp(argv[4], "tx") == 0 && pTx) {
        VCI_OBJ o;
        DWORD n;
        ZeroMemory(&o, sizeof(o));
        o.ID = 0x1AB;
        o.DataLen = 8;
        o.Data[0] = 0xDE; o.Data[1] = 0xAD; o.Data[2] = 0xBE; o.Data[3] = 0xEF;
        o.Data[4] = 0x55; o.Data[5] = 0xAA; o.Data[6] = 0x11; o.Data[7] = 0x22;
        n = pTx(dev, 0, ch, &o, 1);
        printf("  [发送测试] Transmit(通道%lu, ID=0x1AB) -> %lu\n",
               (unsigned long)ch, (unsigned long)n);
    }

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
            printf("  [%d] ts=%lu  ID=0x%lX  dlc=%u  数据=%02X %02X %02X %02X %02X %02X %02X %02X %s%s\n",
                   total, (unsigned long)rx[k].TimeStamp,
                   (unsigned long)rx[k].ID, (unsigned)rx[k].DataLen,
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
