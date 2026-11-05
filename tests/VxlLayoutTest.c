/*
 * VxlLayoutTest.c —— 校验 VxlApiModel.h 的布局断言
 *
 * 两件事：
 *   1. 编译期：头文件里的 static assert 必须过
 *   2. 运行期：用真 vxlapi.dll 取一次配置，逐字段核对模型里的偏移
 *
 * 为什么值得写：
 *   227 字节步长是从"两个通道名之间的间隔"反推的。
 *   反推有可能巧合对上，所以这里再做一次正向验证：
 *   用模型去【读】真 DLL 写进去的缓冲区，看读出来的值合不合理
 *   （比如 bitRate 应该正好是 500000，isOnBus 应该是 0/1）。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "VxlApiModel.h"

typedef XLstatus (__stdcall *PF_VOID)(void);
typedef XLstatus (__stdcall *PF_CFG)(void *);

static int g_pass = 0, g_fail = 0;

static void Chk(int cond, const char *what)
{
    if (cond) g_pass++;
    else { g_fail++; printf("  [FAIL] %s\n", what); }
}

int main(void)
{
    HMODULE h;
    PF_VOID pOpen, pClose;
    PF_CFG  pCfg;
    static unsigned char buf[256 * 1024];
    XlDriverConfig *cfg;

    printf("=== VxlApiModel 布局校验 ===\n\n");

    /* 编译期断言已在头文件里，能编过就说明尺寸/偏移都对 */
    printf("  sizeof(XlBusParams)     = %u  (期望 36)\n", (unsigned)sizeof(XlBusParams));
    printf("  sizeof(XlChannelConfig) = %u  (期望 227)\n", (unsigned)sizeof(XlChannelConfig));
    printf("  sizeof(XlDriverConfig)  = %u\n", (unsigned)sizeof(XlDriverConfig));
    printf("  sizeof(XlCanMessage)    = %u  (期望 32)\n", (unsigned)sizeof(XlCanMessage));
    printf("  offsetof(transceiverName)= %u  (期望 155)\n\n",
           (unsigned)offsetof(XlChannelConfig, transceiverName));

    h = LoadLibraryA("C:\\Windows\\SysWOW64\\vxlapi.dll");
    if (!h) { printf("  加载真 vxlapi.dll 失败，只做静态校验\n"); return g_fail ? 1 : 0; }

    pOpen  = (PF_VOID)GetProcAddress(h, "_xlOpenDriver@0");
    pClose = (PF_VOID)GetProcAddress(h, "_xlCloseDriver@0");
    pCfg   = (PF_CFG)GetProcAddress(h, "_xlGetDriverConfig@4");
    if (!pOpen || !pCfg) { printf("  取不到入口\n"); return 1; }

    if (pOpen() != 0) { printf("  驱动打不开（可能没装 Vector）\n"); return 0; }

    memset(buf, 0xCC, sizeof(buf));
    if (pCfg(buf) != 0) { pClose(); printf("  取配置失败\n"); return 0; }

    cfg = (XlDriverConfig *)buf;

    printf("  --- 用我们的模型去读真 DLL 写的数据 ---\n");
    printf("  dllVersion   = 0x%08X\n", cfg->dllVersion);
    printf("  channelCount = %u\n\n", cfg->channelCount);

    Chk(cfg->channelCount > 0 && cfg->channelCount <= XL_CONFIG_MAX_CHANNELS,
        "channelCount 在合理范围");

    if (cfg->channelCount > 0) {
        XlChannelConfig *c = &cfg->channel[0];
        printf("  channel[0].name               = \"%s\"\n", c->name);
        printf("  channel[0].hwType             = %u\n", c->hwType);
        printf("  channel[0].hwChannel          = %u\n", c->hwChannel);
        printf("  channel[0].channelIndex       = %u\n", c->channelIndex);
        printf("  channel[0].channelMask        = 0x%llX\n", (unsigned long long)c->channelMask);
        printf("  channel[0].channelBusCap      = 0x%08X\n", c->channelBusCapabilities);
        printf("  channel[0].isOnBus            = %u\n", c->isOnBus);
        printf("  channel[0].busParams.busType  = %u\n", c->busParams.busType);
        printf("  channel[0].busParams.can.bitRate = %u\n", c->busParams.u.can.bitRate);
        printf("  channel[0].transceiverName    = \"%s\"\n\n", c->transceiverName);

        /* 名称必须是一个像样的字符串（能读对说明偏移对） */
        Chk(strlen(c->name) >= 3 && strlen(c->name) < XL_MAX_LENGTH + 1,
            "name 是合法字符串（偏移 0 正确）");
        Chk(strlen(c->transceiverName) >= 3 && strlen(c->transceiverName) < XL_MAX_LENGTH + 1,
            "transceiverName 是合法字符串（偏移 155 正确）");
        Chk(c->channelMask == (1ULL << c->channelIndex),
            "channelMask == 1 << channelIndex（偏移 42 正确）");
        Chk(c->busParams.busType == XL_BUS_TYPE_CAN || c->busParams.busType == XL_BUS_TYPE_LIN,
            "busType 是已知值（偏移 63 正确）");

        if (c->busParams.busType == XL_BUS_TYPE_CAN) {
            unsigned int br = c->busParams.u.can.bitRate;
            Chk(br == 500000 || br == 250000 || br == 125000 || br == 1000000 ||
                br == 800000 || br == 100000 || br == 50000 || br == 20000 ||
                br == 10000 || br == 5000,
                "can.bitRate 是常见波特率（偏移 67 正确）");
        }

        /* 第二个通道（如果有）名字也要能读出来 —— 验证步长 227 */
        if (cfg->channelCount >= 2) {
            XlChannelConfig *c2 = &cfg->channel[1];
            printf("  channel[1].name               = \"%s\"\n", c2->name);
            printf("  channel[1].channelMask        = 0x%llX\n\n",
                   (unsigned long long)c2->channelMask);
            Chk(strlen(c2->name) >= 3 && strlen(c2->name) < XL_MAX_LENGTH + 1,
                "channel[1].name 合法（步长 227 正确）");
            Chk(c2->channelMask == (1ULL << c2->channelIndex),
                "channel[1].channelMask 一致");
        }
    }

    pClose();
    printf("=== 通过 %d 项，失败 %d 项 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
