/*
 * VxlEmuTest.c —— 门面 DLL 自检（x64）
 * 直接调用我们自己的 vxlapi64.dll，验证它对外表现是否像一块 VN1630A
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef long long u64;
typedef u64 XLaccess;
typedef DWORD XLportHandle;
typedef long XLstatus;

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

typedef XLstatus(__stdcall*PF_openDrv)(void);
typedef XLstatus(__stdcall*PF_closeDrv)(void);
typedef XLstatus(__stdcall*PF_cfg)(DrvCfg*);
typedef XLstatus(__stdcall*PF_openPort)(XLportHandle*,const char*,XLaccess,XLaccess*,unsigned,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_act)(XLportHandle,XLaccess,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_baud)(XLportHandle,XLaccess,unsigned);
typedef XLstatus(__stdcall*PF_closePort)(XLportHandle);

int main(void)
{
    HMODULE h=LoadLibraryA("vxlapi.dll");
    if(!h) h=LoadLibraryA("vxlapi64.dll");
    if(!h){ printf("[x] vxlapi64.dll 加载失败 (%lu)\n",GetLastError()); return 1; }
    printf("============================================================\n");
    printf("  VN1630 门面自检\n");
    printf("============================================================\n\n");

    PF_openDrv  pOpen =(PF_openDrv) GetProcAddress(h,"xlOpenDriver");
    PF_cfg      pCfg  =(PF_cfg)     GetProcAddress(h,"xlGetDriverConfig");
    PF_openPort pPort =(PF_openPort)GetProcAddress(h,"xlOpenPort");
    PF_baud     pBaud =(PF_baud)    GetProcAddress(h,"xlCanSetChannelBitrate");
    PF_act      pAct  =(PF_act)     GetProcAddress(h,"xlActivateChannel");
    PF_closePort pCp  =(PF_closePort)GetProcAddress(h,"xlClosePort");
    PF_closeDrv pCd   =(PF_closeDrv)GetProcAddress(h,"xlCloseDriver");

    printf("  导出检查: OpenDriver=%s GetDriverConfig=%s OpenPort=%s Activate=%s\n\n",
        pOpen?"有":"缺", pCfg?"有":"缺", pPort?"有":"缺", pAct?"有":"缺");
    if(!pOpen||!pCfg||!pPort||!pAct){ printf("[x] 核心导出缺失\n"); return 1; }

    printf("  xlOpenDriver          -> %ld\n",(long)pOpen());

    {
        DrvCfg cfg; int i;
        ZeroMemory(&cfg,sizeof(cfg));
        printf("  xlGetDriverConfig     -> %ld\n",(long)pCfg(&cfg));
        printf("  通道数 = %lu\n",(unsigned long)cfg.channelCount);
        if(cfg.channelCount==0||cfg.channelCount>16){ printf("[x] 通道数不合理\n"); return 1; }
        for(i=0;i<(int)cfg.channelCount;i++){
            ChanCfg*c=&cfg.channel[i];
            printf("    %d  %-26s mask=0x%I64X  busType=%lu  isOnBus=%d\n",
                i, c->name, (unsigned long long)c->channelMask,
                (unsigned long)c->busParams.busType, c->isOnBus);
            printf("       收发器 \"%s\"  波特率=%lu\n", c->transceiverName,
                (unsigned long)c->busParams.data.can.bitRate);
        }
        printf("\n  名称检查: %s\n",
            (strstr(cfg.channel[0].name,"VN1630")&&strstr(cfg.channel[1].name,"VN1630"))
            ? "两个通道都报 VN1630A ✓" : "★ 名称不是 VN1630A");
    }

    {
        XLportHandle port=0; XLaccess perm=0;
        XLstatus r=pPort(&port,"TestApp",0x3,&perm,8192,3,1);
        printf("\n  xlOpenPort(mask=0x3)  -> %ld  port=%lu perm=0x%I64X\n",
            (long)r,(unsigned long)port,(unsigned long long)perm);
        if(r==0&&perm){
            printf("  xlCanSetChannelBitrate(500000) -> %ld\n",(long)pBaud(port,perm,500000));
            printf("  xlActivateChannel      -> %ld   (这里会去开周立功盒子)\n",(long)pAct(port,perm,1,0));
            pCp(port);
        }
    }
    pCd();
    printf("\n============================================================\n");
    printf("  自检结束\n");
    printf("============================================================\n");
    return 0;
}
