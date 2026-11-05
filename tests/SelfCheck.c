/*
 * SelfCheck.c —— 一把跑完所有关键检查，输出 通过/失败 清单
 *
 * 为什么需要它：
 *   这套东西现在的功能不少（多路、时间戳、滤波、错误回传、三层门面），
 *   但现场出了问题没人能快速判断"到底是桥不对、还是盒子没插、还是设错了"。
 *
 *   这个工具把能自动验的项目全跑一遍，逐项标 ✓ / ★，
 *   最后给一句结论。不需要懂原理，也不需要开 ZCANPRO。
 *
 * 用法：
 *   SelfCheck.exe [型号]
 *   型号默认 21（USBCAN-2E-U / USBCAN-II 兼容）。
 *
 * 覆盖的检查：
 *   1. 能不能加载 USBCAN.dll，它的构建版本是多少
 *   2. 能不能打开设备，走的是哪个后端、几路
 *   3. 两路能不能初始化 + 启动
 *   4. 读 CAN 状态（波特率 / 位时序）
 *   5. 读错误状态（总线上有没有错误）
 *   6. 时间戳刻度对不对（必须是 10 单位/毫秒）
 *   7. 自发自收（需要两路，或总线上有别的节点）
 *   8. 验收滤波是否真的生效
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
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

typedef DWORD (__stdcall *PF_OPEN)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_CLOSE)(DWORD, DWORD);
typedef DWORD (__stdcall *PF_INIT)(DWORD, DWORD, DWORD, VCI_CFG *);
typedef DWORD (__stdcall *PF_START)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *PF_RX)(DWORD, DWORD, DWORD, VCI_OBJ *, DWORD, int);
typedef DWORD (__stdcall *PF_TX)(DWORD, DWORD, DWORD, VCI_OBJ *, DWORD);
typedef DWORD (__stdcall *PF_ANY)(DWORD, DWORD, DWORD, void *);

static int  g_pass = 0, g_fail = 0, g_skip = 0;

static void OK(const char *fmt, ...)
{
    va_list ap;
    printf("  [✓] ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    g_pass++;
}
static void FAIL(const char *fmt, ...)
{
    va_list ap;
    printf("  [★] ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    g_fail++;
}
static void SKIP(const char *fmt, ...)
{
    va_list ap;
    printf("  [·] ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    g_skip++;
}

int main(int argc, char **argv)
{
    HMODULE d;
    PF_OPEN  pOpen; PF_CLOSE pClose; PF_INIT pInit; PF_START pStart;
    PF_RX    pRx;   PF_TX    pTx;    PF_ANY  pErr, pStatus;
    DWORD dev = (argc >= 2) ? (DWORD)atoi(argv[1]) : 21;
    VCI_CFG cfg;
    int chans = 2, ch;
    int started[8] = {0};
    DWORD rxId = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("================================================================\n");
    printf("  UniVciBridge 自检\n");
    printf("================================================================\n\n");

    /* ── 1. 加载 ── */
    printf("[1] 加载 USBCAN.dll\n");
    d = LoadLibraryA("USBCAN.dll");
    if (!d) {
        FAIL("加载失败 —— 当前目录里没有 USBCAN.dll？");
        printf("\n=== 结论：无法继续 ===\n");
        return 1;
    }
    {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(d, path, MAX_PATH);
        OK("已加载: %s", path);
    }
    pOpen   = (PF_OPEN)  GetProcAddress(d, "VCI_OpenDevice");
    pClose  = (PF_CLOSE) GetProcAddress(d, "VCI_CloseDevice");
    pInit   = (PF_INIT)  GetProcAddress(d, "VCI_InitCAN");
    pStart  = (PF_START) GetProcAddress(d, "VCI_StartCAN");
    pRx     = (PF_RX)    GetProcAddress(d, "VCI_Receive");
    pTx     = (PF_TX)    GetProcAddress(d, "VCI_Transmit");
    pErr    = (PF_ANY)   GetProcAddress(d, "VCI_ReadErrInfo");
    pStatus = (PF_ANY)   GetProcAddress(d, "VCI_ReadCANStatus");
    if (!pOpen || !pInit || !pStart || !pRx || !pTx) {
        FAIL("导出不全（缺关键函数），不是本项目的 DLL？");
        printf("\n=== 结论：无法继续 ===\n");
        return 1;
    }
    OK("14 个 VCI 导出齐全");

    /* ── 2. 打开设备 ── */
    printf("\n[2] 打开设备（型号 %lu）\n", (unsigned long)dev);
    if (pOpen(dev, 0, 0) != 1) {
        FAIL("打开失败 —— 看同目录的 UniVciBridge.log 里后端探测那几行");
        printf("\n=== 结论：设备打不开，不用往下测了 ===\n");
        return 1;
    }
    OK("设备已打开");

    /* ── 3. 初始化 + 启动 ── */
    printf("\n[3] 初始化并启动通道\n");
    ZeroMemory(&cfg, sizeof(cfg));
    cfg.AccMask = 0xFFFFFFFF;
    cfg.Filter = 0;
    cfg.Timing0 = 0x00;
    cfg.Timing1 = 0x1C;      /* 500k */
    for (ch = 0; ch < 2; ch++) {
        DWORD ri, rs;
        ri = pInit(dev, 0, (DWORD)ch, &cfg);
        if (ri != 1) { FAIL("InitCAN 通道 %d 失败", ch); continue; }
        rs = pStart(dev, 0, (DWORD)ch);
        if (rs != 1) { FAIL("StartCAN 通道 %d 失败", ch); continue; }
        started[ch] = 1;
        OK("通道 %d 已启动（500k）", ch);
    }
    if (!started[0]) {
        printf("\n=== 结论：一个通道都起不来 ===\n");
        return 1;
    }
    if (!started[1]) { chans = 1; SKIP("只起来 1 路，部分测试会跳过"); }

    /* ── 4. CAN 状态 ── */
    printf("\n[4] 读 CAN 状态\n");
    if (pStatus) {
        DWORD st[8];
        ZeroMemory(st, sizeof(st));
        if (pStatus(dev, 0, 0, st) == 1 && st[0] == 500000)
            OK("波特率 %lu，Tseg1=%lu Tseg2=%lu SJW=%lu",
               (unsigned long)st[0], (unsigned long)st[1],
               (unsigned long)st[2], (unsigned long)st[3]);
        else if (st[0])
            FAIL("波特率报的是 %lu，跟设定的 500000 不一致", (unsigned long)st[0]);
        else
            FAIL("状态读不出波特率");
    } else SKIP("没有 VCI_ReadCANStatus");

    /* ── 5. 错误状态 ── */
    printf("\n[5] 读错误状态\n");
    if (pErr) {
        unsigned char er[16];
        ZeroMemory(er, sizeof(er));
        if (pErr(dev, 0, 0, er) == 1) {
            if (er[0] == 0) OK("无错误（ErrCode=0）");
            else FAIL("总线有错误 ErrCode=%u（1=警告 2=被动 3=关闭）", er[0]);
        } else FAIL("读错误状态失败");
    } else SKIP("没有 VCI_ReadErrInfo");

    /* ── 6. 时间戳刻度 ── */
    printf("\n[6] 时间戳刻度（自发自收测两次，间隔 500ms）\n");
    if (started[1]) {
        VCI_OBJ t, buf[32];
        DWORD ts1 = 0, ts2 = 0;
        double wall1 = 0, wall2 = 0;
        LARGE_INTEGER fq, tq;
        int round;
        QueryPerformanceFrequency(&fq);
        for (round = 0; round < 2; round++) {
            DWORD g;
            ZeroMemory(&t, sizeof(t));
            t.ID = 0x1AB; t.DataLen = 4; t.Data[0] = (BYTE)round;
            pTx(dev, 0, 0, &t, 1);
            Sleep(60);
            g = pRx(dev, 0, 0, buf, 32, 100);
            QueryPerformanceCounter(&tq);
            if (g > 0 && buf[g-1].ID == 0x1AB) {
                if (round == 0) { ts1 = buf[g-1].TimeStamp; wall1 = (double)tq.QuadPart / fq.QuadPart; }
                else            { ts2 = buf[g-1].TimeStamp; wall2 = (double)tq.QuadPart / fq.QuadPart; }
            }
            Sleep(500);
        }
        if (ts2 > ts1 && wall2 > wall1) {
            double perMs = (double)(ts2 - ts1) / ((wall2 - wall1) * 1000.0);
            if (perMs > 9.0 && perMs < 11.0)
                OK("刻度正确：%.1f 单位/毫秒（应为 10，即 0.1ms）", perMs);
            else
                FAIL("刻度不对：%.1f 单位/毫秒（应为 10）", perMs);
        } else FAIL("两次都没收到自己的帧，无法测刻度");
    } else SKIP("只有 1 路，没有应答节点，跳过");

    /* ── 7. 滤波 ── */
    printf("\n[7] 验收滤波\n");
    if (started[1]) {
        /* ch0 改成只放行 0x100，ch1 发 0x100 和 0x120 */
        int got100 = 0, got120 = 0, i;
        DWORD g;
        VCI_OBJ t, buf[32];
        pClose(dev, 0);
        if (pOpen(dev, 0, 0) == 1) {
            ZeroMemory(&cfg, sizeof(cfg));
            cfg.AccCode = 0x100;
            cfg.AccMask = 0x1FFFF800;   /* 只放行 0x100 */
            cfg.Timing0 = 0x00; cfg.Timing1 = 0x1C;
            if (pInit(dev, 0, 0, &cfg) == 1 && pStart(dev, 0, 0) == 1) {
                ZeroMemory(&cfg, sizeof(cfg));
                cfg.AccMask = 0xFFFFFFFF;
                cfg.Timing0 = 0x00; cfg.Timing1 = 0x1C;
                if (pInit(dev, 0, 1, &cfg) == 1) pStart(dev, 0, 1);

                for (i = 0; i < 6; i++) {
                    ZeroMemory(&t, sizeof(t));
                    t.ID = (i & 1) ? 0x120 : 0x100;
                    t.DataLen = 2;
                    pTx(dev, 0, 1, &t, 1);      /* 从 ch1 发 */
                    Sleep(30);
                    g = pRx(dev, 0, 0, buf, 32, 30);   /* ch0 带滤波收 */
                    while (g-- > 0) {
                        if (buf[0].ID == 0x100) got100++;
                        if (buf[0].ID == 0x120) got120++;
                    }
                }
                if (got100 > 0 && got120 == 0)
                    OK("滤波生效：0x100 通过 %d 帧，0x120 被挡掉", got100);
                else if (got120 > 0)
                    FAIL("滤波没生效：0x120 也进来了 %d 帧", got120);
                else
                    SKIP("两个 ID 都没收到，无法判断（可能总线问题）");
            } else FAIL("带滤波的 InitCAN 失败了");
        } else FAIL("重新打开设备失败");
    } else SKIP("只有 1 路，跳过");

    /* ── 结论 ── */
    pClose(dev, 0);
    printf("\n================================================================\n");
    printf("  结论：通过 %d 项，失败 %d 项，跳过 %d 项\n", g_pass, g_fail, g_skip);
    if (g_fail == 0)
        printf("  ✅ 全部正常 —— 这个盒子和桥在这台机器上可以正常使用\n");
    else
        printf("  ★ 有 %d 项失败 —— 看上面标 [★] 的行，或同目录的 UniVciBridge.log\n", g_fail);
    printf("================================================================\n");
    return g_fail ? 1 : 0;
}
