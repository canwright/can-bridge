/*
 * VxlPortProbe.c —— 直接问真 vxlapi.dll：xlOpenPort 到底要什么才能拿到权限
 *
 * 背景：
 *   桥里这么调：
 *       xlOpenPort(&port, "UniVciBridge", mask, &perm, 8192,
 *                  XL_INTERFACE_VERSION_V3, XL_BUS_TYPE_CAN)
 *   而桥自己把 XL_INTERFACE_VERSION_V3 定义成 3。
 *   实测返回 st=0 但 perm=0 —— 拿不到权限。
 *
 * 要分清是"驱动不授权"还是"我们参数不对"，唯一的办法是直接试：
 *   同一次运行里换不同的 xlInterfaceVersion 试 xlOpenPort，看 perm 变不变。
 *
 * 顺带把通道的 interfaceVersion / driverVersion / isOnBus 打出来对照。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "VxlApiModel.h"

typedef XLstatus (__stdcall *PF_OPEN)(void);
typedef XLstatus (__stdcall *PF_CLOSE)(void);
typedef XLstatus (__stdcall *PF_CFG)(void *);
/* busType 必须是 __int64：导出名 _xlOpenPort@32（8 个 4 字节位） */
/* 7 个参数；accessMask 是 XLaccess(8 字节) → 总宽 32，对上 _xlOpenPort@32 */
typedef XLstatus (__stdcall *PF_OPENPORT)(XLportHandle *, const char *,
                                          XLaccess, XLaccess *,
                                          unsigned int, unsigned int,
                                          unsigned int);
typedef XLstatus (__stdcall *PF_CLOSEPORT)(XLportHandle);
/* 通道要先用它分配给应用，xlOpenPort 才给 init 权限 */
typedef XLstatus (__stdcall *PF_SETAPPLCFG)(const char *, unsigned int,
                                            unsigned int, unsigned int,
                                            unsigned int, unsigned int);
typedef XLstatus (__stdcall *PF_ERRSTR)(XLstatus, char *);

static const char *ErrName(XLstatus s)
{
    switch (s) {
    case 0:   return "XL_SUCCESS";
    case 201: return "XL_ERR_CANNOT_OPEN_DRIVER";
    case 202: return "XL_ERR_WRONG_BUS_TYPE";
    case 204: return "XL_ERR_INVALID_CHANNEL_MASK";
    case 205: return "XL_ERR_NOT_SUPPORTED";
    case 118: return "XL_ERR_INVALID_PORT";
    case 101: return "XL_ERR_WRONG_PARAMETER";
    case 160: return "XL_ERR_WRONG_VERSION";
    case 255: return "XL_ERROR";
    default:  return "?";
    }
}

int main(void)
{
    /* 无缓冲：崩溃时也能看到走到哪一步 */
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE h;
    PF_OPEN     pOpen;
    PF_CLOSE    pClose;
    PF_CFG      pCfg;
    PF_OPENPORT pOpenPort;
    PF_CLOSEPORT pClosePort;
    PF_SETAPPLCFG pSetAppl;
    PF_ERRSTR   pErr;
    static unsigned char buf[256 * 1024];
    XlDriverConfig *cfg;
    XLaccess mask = 0;
    XLstatus st;
    unsigned int vers[] = { 4, 3, 0x0300, 0x0400, 0x0500 };
    int i;

    printf("=== vxlapi xlOpenPort 参数探针 ===\n\n");
    fflush(stdout);

    h = LoadLibraryA("vxlapi.dll");
    if (!h) h = LoadLibraryA("C:\\Windows\\SysWOW64\\vxlapi.dll");
    if (!h) { printf("  加载 vxlapi.dll 失败\n"); return 1; }

    pOpen      = (PF_OPEN)     GetProcAddress(h, "xlOpenDriver");
    pClose     = (PF_CLOSE)    GetProcAddress(h, "xlCloseDriver");
    pCfg       = (PF_CFG)      GetProcAddress(h, "xlGetDriverConfig");
    pClosePort = (PF_CLOSEPORT)GetProcAddress(h, "xlClosePort");
    pSetAppl   = (PF_SETAPPLCFG)GetProcAddress(h, "xlSetApplConfig");
    pErr       = (PF_ERRSTR)   GetProcAddress(h, "xlGetErrorString");

    /* ★ 优先取 stdcall 修饰名。真 vxlapi.dll 同时导出 "xlOpenPort" 和
       "_xlOpenPort@28"；未修饰名那次调用直接访问违例（栈失衡）。
       修饰名 = 7 个参数 × 4 字节 = 28，调用约定确定。 */
    pOpenPort = (PF_OPENPORT) GetProcAddress(h, "_xlOpenPort@32");
    if (pOpenPort) printf("  导出名: _xlOpenPort@28\n");
    else {
        pOpenPort = (PF_OPENPORT) GetProcAddress(h, "xlOpenPort");
        printf("  导出名: xlOpenPort（未修饰）\n");
    }
    fflush(stdout);
    if (!pOpen || !pCfg || !pOpenPort) { printf("  关键导出缺失\n"); return 1; }

    st = pOpen();
    printf("  sizeof(XLaccess)=%u  sizeof(XLportHandle)=%u\n",
           (unsigned)sizeof(XLaccess), (unsigned)sizeof(XLportHandle));
    fflush(stdout);
    printf("  xlOpenDriver() -> %u (%s)\n", st, ErrName(st));
    if (st != 0) return 1;

    memset(buf, 0, sizeof(buf));
    st = pCfg(buf);
    printf("  xlGetDriverConfig() -> %u\n", st);
    cfg = (XlDriverConfig *)buf;
    printf("  channelCount = %u\n", cfg->channelCount);

    for (i = 0; i < (int)cfg->channelCount && i < 4; i++) {
        XlChannelConfig *c = &cfg->channel[i];
        printf("  --- 通道 %d ---\n", i);
        printf("      name               = \"%s\"\n", c->name);
        printf("      hwType=%u hwIndex=%u hwChannel=%u channelIndex=%u\n",
               c->hwType, c->hwIndex, c->hwChannel, c->channelIndex);
        printf("      channelMask        = 0x%llX\n",
               (unsigned long long)c->channelMask);
        printf("      channelCapabilities= 0x%08X\n", c->channelCapabilities);
        printf("      busCapabilities    = 0x%08X\n", c->channelBusCapabilities);
        printf("      isOnBus            = %u\n", c->isOnBus);
        printf("      connectedBusType   = %u\n", c->connectedBusType);
        printf("      driverVersion      = 0x%08X\n", c->driverVersion);
        printf("      interfaceVersion   = 0x%08X\n", c->interfaceVersion);
        printf("      transceiverName    = \"%s\"\n", c->transceiverName);
        if (mask == 0 && (c->channelBusCapabilities & XL_BUS_COMPATIBLE_CAN))
            mask = c->channelMask;
    }

    if (mask == 0) { printf("\n  没有 CAN 兼容通道，后面无法测\n"); return 0; }

    /* ★ 关键一步：把通道分配给本应用，否则 xlOpenPort 只给 port 不给 perm。
       参数来自 xlGetDriverConfig 读到的 hwType/hwIndex/hwChannel。 */
    if (pSetAppl) {
        XlChannelConfig *c0 = &cfg->channel[0];
        XLstatus sa = pSetAppl("VxlPortProbe", 0, c0->hwType, c0->hwIndex,
                               c0->hwChannel, XL_BUS_TYPE_CAN);
        printf("\n  xlSetApplConfig(app=%s, ch0, hwType=%u, hwIndex=%u, hwChannel=%u) -> %u %s\n",
               "VxlPortProbe", c0->hwType, c0->hwIndex, c0->hwChannel,
               sa, ErrName(sa));
        fflush(stdout);
    } else {
        printf("\n  (没有 xlSetApplConfig 导出)\n");
    }

    printf("\n  === 用不同 xlInterfaceVersion 试 xlOpenPort（mask=0x%llX）===\n",
           (unsigned long long)mask);
    for (i = 0; i < (int)(sizeof(vers) / sizeof(vers[0])); i++) {
        XLportHandle port = 0;
        XLaccess perm = 0;
        printf("     调用中 ver=%u ...", vers[i]);
        fflush(stdout);
        st = pOpenPort(&port, "VxlPortProbe", mask, &perm, 8192,
                       vers[i], XL_BUS_TYPE_CAN);
        printf("     ver=0x%04X (%u) -> st=%u (%-6s) port=%u perm=0x%llX %s\n",
               vers[i], vers[i], st, ErrName(st), (unsigned)port,
               (unsigned long long)perm,
               (st == 0 && perm) ? "<<< 拿到权限" : "");
        if (st == 0 && port && pClosePort) pClosePort(port);
    }

    printf("\n  === 结论 ===\n");
    printf("    如果所有版本都 perm=0 → 是驱动的通道授权问题（要在\n");
    printf("    Vector Hardware Config 里把通道分配给应用），不是代码问题。\n");
    printf("    如果某个版本 perm!=0 → 桥里的 XL_INTERFACE_VERSION_V3 取值错了。\n");

    pClose();
    return 0;
}
