/*
 * DumpRealCfg.c —— 32 位：直接调真 Vector 驱动，把 VN1630A 的通道字段抄下来
 * 目的：拿到 hwType / hwIndex / hwChannel / transceiverType / capabilities 的真实值，
 *       这些是上位机识别设备时看的字段。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef long long u64;
typedef u64 XLaccess;

#pragma pack(push,1)
typedef struct { DWORD bitRate; BYTE sjw,tseg1,tseg2,sam,outputMode; BYTE reserved[7]; BYTE canOpMode; } BPDCan;
typedef union  { BPDCan can; BYTE raw[28]; } BPD;
typedef struct { DWORD busType; BPD data; } BusParams;
typedef struct {
    char name[32]; BYTE hwType,hwIndex,hwChannel;
    WORD transceiverType,transceiverState,configError; BYTE channelIndex;
    u64 channelMask; DWORD channelCapabilities,channelBusCapabilities;
    BYTE isOnBus; DWORD connectedBusType; BusParams busParams;
    DWORD _a,driverVersion,interfaceVersion; DWORD raw_data[10];
    DWORD serialNumber,articleNumber; char transceiverName[32];
    DWORD b,c; BYTE d,e,f,g; WORD h,i; DWORD j; BYTE k,l; WORD m,n,o; DWORD r[3];
} ChanCfg;
typedef struct { DWORD dllVersion,channelCount,reserved[10]; ChanCfg channel[64]; } DrvCfg;
#pragma pack(pop)

typedef long(__stdcall*PF_open)(void);
typedef long(__stdcall*PF_cfg)(DrvCfg*);

int main(int argc,char**argv)
{
    /* 用超大缓冲，防结构尺寸不符导致越界 */
    static unsigned char buf[131072];
    DrvCfg *cfg=(DrvCfg*)buf;
    const char *dll = (argc>=2) ? argv[1] : "C:\\Windows\\SysWOW64\\vxlapi.dll";
    HMODULE h=LoadLibraryA(dll);
    PF_open pOpen; PF_cfg pCfg;
    int i,n;

    printf("DLL: %s\n", dll);
    if(!h){ printf("[x] 加载失败 (%lu)\n",GetLastError()); return 1; }
    pOpen=(PF_open)GetProcAddress(h,"xlOpenDriver");
    pCfg =(PF_cfg) GetProcAddress(h,"xlGetDriverConfig");
    if(!pOpen||!pCfg){ printf("[x] 缺导出\n"); return 1; }

    printf("xlOpenDriver -> %ld\n",(long)pOpen());
    ZeroMemory(buf,sizeof(buf));
    printf("xlGetDriverConfig -> %ld\n",(long)pCfg(cfg));
    n=(int)cfg->channelCount;
    printf("channelCount = %d\n\n",n);
    if(n<0||n>64){ printf("[x] 通道数异常\n"); return 1; }

    for(i=0;i<n;i++){
        ChanCfg*c=&cfg->channel[i];
        printf("通道 %d\n",i);
        printf("    name           = \"%s\"\n",c->name);
        printf("    hwType         = %u\n",c->hwType);
        printf("    hwIndex        = %u\n",c->hwIndex);
        printf("    hwChannel      = %u\n",c->hwChannel);
        printf("    transceiverType= %u\n",c->transceiverType);
        printf("    channelIndex   = %u\n",c->channelIndex);
        printf("    channelMask    = 0x%I64X\n",(unsigned long long)c->channelMask);
        printf("    capabilities   = 0x%08lX\n",(unsigned long)c->channelCapabilities);
        printf("    busCapab       = 0x%08lX\n",(unsigned long)c->channelBusCapabilities);
        printf("    isOnBus        = %u\n",c->isOnBus);
        printf("    busyType       = %lu  (busParams.busType)\n",(unsigned long)c->busParams.busType);
        printf("    bitRate        = %lu\n",(unsigned long)c->busParams.data.can.bitRate);
        printf("    transceiver    = \"%s\"\n",c->transceiverName);
        printf("    serialNumber   = 0x%08lX\n",(unsigned long)c->serialNumber);
        printf("\n");
    }
    return 0;
}
