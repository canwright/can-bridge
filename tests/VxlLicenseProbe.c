/*
 * VxlLicenseProbe.c —— 直接问 Vector 驱动：有没有 Keyman 授权狗、授权位是什么
 *
 * 背景：
 *   用户要"找 keyman 授权文件"。全盘扫下来只有 vKeyman.dll（客户端驱动），
 *   没有任何授权文件 —— 因为 Vector 的 Keyman 是【硬件加密狗】，不是文件。
 *
 *   与其继续找文件，不如直接问驱动本人：
 *       xlGetKeymanBoxes(&count)          有几根狗连着
 *       xlGetKeymanInfo(0, &mask, &serial, &licBits)   序列号和授权位
 *       xlGetLicenseInfo(chMask, arr, n)  某个通道上有哪些授权
 *
 *   这三句是唯一权威的答案，比在任何目录里找 .lic 文件都可靠。
 *
 * 用法：
 *   直接运行。插着 Keyman 狗时 boxCount 会 >= 1。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "VxlApiModel.h"

typedef XLstatus (__stdcall *PF_VOID)(void);
typedef XLstatus (__stdcall *PF_CFG)(void *);
typedef XLstatus (__stdcall *PF_KM_BOXES)(unsigned int *);
typedef XLstatus (__stdcall *PF_KM_INFO)(unsigned int, unsigned int *,
                                         unsigned int *, XLuint64 *);
typedef XLstatus (__stdcall *PF_LICINFO)(XLaccess, void *, unsigned int);

int main(void)
{
    HMODULE h;
    PF_VOID      pOpen, pClose;
    PF_CFG       pCfg;
    PF_KM_BOXES  pKmBoxes;
    PF_KM_INFO   pKmInfo;
    PF_LICINFO   pLicInfo;
    XLstatus st;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== Vector Keyman / 授权探测 ===\n\n");

    h = LoadLibraryA("vxlapi.dll");
    if (!h) h = LoadLibraryA("C:\\Windows\\SysWOW64\\vxlapi.dll");
    if (!h) { printf("  加载 vxlapi.dll 失败\n"); return 1; }

    pOpen     = (PF_VOID)     GetProcAddress(h, "xlOpenDriver");
    pClose    = (PF_VOID)     GetProcAddress(h, "xlCloseDriver");
    pCfg      = (PF_CFG)      GetProcAddress(h, "xlGetDriverConfig");
    pKmBoxes  = (PF_KM_BOXES) GetProcAddress(h, "xlGetKeymanBoxes");
    pKmInfo   = (PF_KM_INFO)  GetProcAddress(h, "xlGetKeymanInfo");
    pLicInfo  = (PF_LICINFO)  GetProcAddress(h, "xlGetLicenseInfo");

    if (!pOpen) { printf("  缺 xlOpenDriver\n"); return 1; }
    st = pOpen();
    printf("  xlOpenDriver() -> %u\n", st);
    if (st != 0) return 1;

    /* ── 1. 有几根 Keyman 授权狗 ── */
    printf("\n=== 1. Keyman 授权狗（xlGetKeymanBoxes）===\n");
    if (!pKmBoxes) {
        printf("  这个驱动版本没有 xlGetKeymanBoxes 导出\n");
    } else {
        unsigned int n = 0;
        st = pKmBoxes(&n);
        printf("  -> %u, boxCount = %u\n", st, n);
        if (st == 0 && n > 0) {
            printf("  ★ 检测到 %u 根 Keyman 狗\n", n);
        } else {
            printf("  · 没有检测到 Keyman 授权狗（没插，或驱动不要求狗）\n");
        }

        /* ── 2. 每根狗的序列号和授权位 ── */
        if (pKmInfo && n > 0) {
            unsigned int i;
            printf("\n=== 2. 狗的信息（xlGetKeymanInfo）===\n");
            for (i = 0; i < n && i < 8; i++) {
                unsigned int bm = 0, sn = 0;
                XLuint64 lic = 0;
                st = pKmInfo(i, &bm, &sn, &lic);
                printf("  [%u] -> %u  boxMask=0x%X  serial=%u (0x%X)  licBits=0x%I64X\n",
                       i, st, bm, sn, sn, (unsigned long long)lic);
            }
        }
    }

    /* ── 3. 通道上的授权（xlGetLicenseInfo）── */
    printf("\n=== 3. 通道授权（xlGetLicenseInfo）===\n");
    if (pCfg && pLicInfo) {
        static unsigned char buf[256 * 1024];
        XlDriverConfig *cfg;
        XLaccess mask = 0;
        unsigned int i;

        memset(buf, 0, sizeof(buf));
        if (pCfg(buf) == 0) {
            cfg = (XlDriverConfig *)buf;
            for (i = 0; i < cfg->channelCount && i < 8; i++) {
                XlChannelConfig *c = &cfg->channel[i];
                printf("  通道%u \"%s\" mask=0x%I64X transceiver=\"%s\"\n",
                       i, c->name, (unsigned long long)c->channelMask,
                       c->transceiverName);
                if (!mask && (c->channelBusCapabilities & XL_BUS_COMPATIBLE_CAN))
                    mask = c->channelMask;
            }
            if (mask) {
                /* XLlicenseInfo 的具体字段手册在 3.4.4，这里先用原始字节看有没有非 0 */
                unsigned char arr[16 * 64];
                memset(arr, 0, sizeof(arr));
                st = pLicInfo(mask, arr, 16);
                printf("\n  xlGetLicenseInfo(mask=0x%I64X, n=16) -> %u\n",
                       (unsigned long long)mask, st);
                {
                    int k, nz = 0;
                    const unsigned int *w = (const unsigned int *)arr;
                    printf("  非零数据字：");
                    for (k = 0; k < 16 * 16; k++) {
                        if (w[k]) {
                            if (nz < 12) printf(" [%d]=0x%X", k, w[k]);
                            nz++;
                        }
                    }
                    if (!nz) printf(" 全部为 0（该通道没有授权信息）");
                    printf("\n  非零字数：%d\n", nz);
                }
            }
        }
    } else {
        printf("  缺 xlGetDriverConfig 或 xlGetLicenseInfo\n");
    }

    printf("\n=== 结论 ===\n");
    printf("  Keyman 是【硬件加密狗】，不是文件。\n");
    printf("  全盘找不到任何授权文件是正常的 —— 授权在狗里。\n");
    printf("  如果 boxCount=0 且你需要狗才能用某些功能，那就要插上狗。\n");

    if (pClose) pClose();
    return 0;
}
