/*
 * VxlProbe.c —— 用真 vxlapi.dll 量出 XLchannelConfig 的真实布局
 *
 * 为什么必须量：
 *   XLdriverConfig.channel[] 是【定长数组】，每个元素的字节数决定一切。
 *   手册给了字段顺序，但没给 XL_MAX_LENGTH 的值 —— 而它是两个 char 数组的长度：
 *       char name[XL_MAX_LENGTH + 1];
 *       char transceiverName[XL_MAX_LENGTH + 1];
 *   猜错 1 个字节，整个数组全部错位 → 门面永远对不上。
 *
 * 办法：
 *   1. 把缓冲区填成 0xCC
 *   2. 调 xlOpenDriver + xlGetDriverConfig
 *   3. 在缓冲区里找通道名字符串（"VN1630A Channel 1" 之类）
 *   4. 相邻两个名字的偏移差 = sizeof(XLchannelConfig)  ← 这就是答案
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int  XLstatus;
typedef unsigned int  XLaccess;
typedef unsigned long long XLuint64;
typedef unsigned int  XLportHandle;

typedef XLstatus (__stdcall *PF_OPEN_DRIVER)(void);
typedef XLstatus (__stdcall *PF_CLOSE_DRIVER)(void);
typedef XLstatus (__stdcall *PF_GET_CFG)(void *pConfig);
typedef XLstatus (__stdcall *PF_GET_ERRSTR)(XLstatus, char *buf, int len);

#define BUF_SIZE (256 * 1024)

static unsigned char g_buf[BUF_SIZE];

int main(void)
{
    HMODULE h;
    PF_OPEN_DRIVER  pOpen;
    PF_CLOSE_DRIVER pClose;
    PF_GET_CFG     pCfg;
    PF_GET_ERRSTR  pErrStr = NULL;
    XLstatus st;
    unsigned int cnt = 0, i;

    printf("=== vxlapi 真实布局探针 ===\n\n");

    /* 用全路径，避免误加载探针自己 */
    h = LoadLibraryA("C:\\Windows\\SysWOW64\\vxlapi.dll");
    if (!h) h = LoadLibraryA("vxlapi.dll");
    if (!h) {
        printf("  加载 vxlapi.dll 失败 (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    printf("  vxlapi.dll = %p\n", (void *)h);

    pOpen   = (PF_OPEN_DRIVER)GetProcAddress(h, "_xlOpenDriver@0");
    if (!pOpen) pOpen = (PF_OPEN_DRIVER)GetProcAddress(h, "xlOpenDriver");
    pClose  = (PF_CLOSE_DRIVER)GetProcAddress(h, "_xlCloseDriver@0");
    if (!pClose) pClose = (PF_CLOSE_DRIVER)GetProcAddress(h, "xlCloseDriver");
    pCfg    = (PF_GET_CFG)GetProcAddress(h, "_xlGetDriverConfig@4");
    if (!pCfg) pCfg = (PF_GET_CFG)GetProcAddress(h, "xlGetDriverConfig");
    pErrStr = (PF_GET_ERRSTR)GetProcAddress(h, "_xlGetErrorString@12");
    if (!pErrStr) pErrStr = (PF_GET_ERRSTR)GetProcAddress(h, "xlGetErrorString");

    if (!pOpen || !pCfg) {
        printf("  找不到 xlOpenDriver / xlGetDriverConfig\n");
        return 1;
    }

    st = pOpen();
    printf("  xlOpenDriver() -> %u\n", st);
    if (st != 0) {
        char e[256] = {0};
        if (pErrStr) pErrStr(st, e, 255);
        printf("  驱动打不开：%s\n", e);
        return 1;
    }

    memset(g_buf, 0xCC, sizeof(g_buf));
    st = pCfg(g_buf);
    printf("  xlGetDriverConfig() -> %u\n", st);
    if (st != 0) { pClose(); return 1; }

    /* 前 8 字节：dllVersion, channelCount */
    {
        unsigned int dllVer = *(unsigned int *)g_buf;
        cnt = *(unsigned int *)(g_buf + 4);
        printf("  dllVersion    = 0x%08X\n", dllVer);
        printf("  channelCount  = %u\n\n", cnt);
    }

    /* 找名字字符串：结构体里 name 在第 0 字节，都是可打印 ASCII */
    {
        unsigned int base = 12;          /* dllVersion + channelCount + reserved[10] */
        unsigned int found[64], nf = 0;
        unsigned int off = base;

        while (off < 64 * 1024 && nf < 64) {
            char *p = (char *)(g_buf + off);
            int len = 0;
            /* 判断这一段开头是不是可打印字符串 */
            while (len < 40 && p[len] >= 0x20 && p[len] < 0x7F) len++;
            if (len >= 4 && p[len] == 0) {
                found[nf++] = off;
                printf("  通道名 @ +%5u : \"%s\"\n", off, p);
                /* 跳到下一候选：至少 32 字节之后再找 */
                off += 32;
            } else {
                off += 1;
            }
        }

        if (nf >= 2) {
            printf("\n  ★ 相邻通道名间隔 = %u 字节（这就是 sizeof(XLchannelConfig)）\n",
                   found[1] - found[0]);
            for (i = 1; i < nf; i++) {
                printf("    %u -> %u : %u\n", found[i - 1], found[i], found[i] - found[i - 1]);
            }
        } else {
            printf("\n  只找到 %u 个名字，无法推算步长\n", nf);
        }

        /* 把第一个通道的原始字节打出来，供对照字段 */
        if (nf >= 1) {
            unsigned int s = found[0];
            unsigned int n = (nf >= 2) ? (found[1] - found[0]) : 96;
            if (n > 160) n = 160;
            printf("\n  第一个通道原始字节（%u 字节）:\n   ", n);
            for (i = 0; i < n; i++) {
                printf("%02X ", g_buf[s + i]);
                if ((i + 1) % 16 == 0) printf("\n   ");
            }
            printf("\n");
        }
    }

    pClose();
    printf("\n=== 完成 ===\n");
    return 0;
}
