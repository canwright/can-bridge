/*
 * VxlEmu.c —— Vector 门面：让任何后端硬件（周立功/PEAK/…）冒充 VN1630A
 *
 * 定位
 * ----
 *   上位机（CANape / CANoe / 自研工具）
 *        ↓ 调用 vxlapi64.dll
 *   本 DLL（报一个 VN1630A，收发转给后端）
 *        ↓ 通过 UniVciBridge
 *   真实硬件（周立功等）
 *
 *   许可：把原厂 vxlapi64.dll 改名成 xlrealdrv.dll 放在同目录，
 *         未实现的导出全部【转发】给它 —— 许可校验、驱动服务查询
 *         都走原厂逻辑，我们完全不碰。
 *
 * 设计原则
 * --------
 *   1. 只实现数据通路需要的那几个函数，其余一律转发原厂
 *   2. 任何异常都返回错误码，绝不崩宿主
 *   3. 32/64 位各编一份（本文件同一份代码，靠宏区分结构体尺寸）
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long long XLuint64;
typedef XLuint64   XLaccess;
typedef DWORD      XLportHandle;
typedef long       XLstatus;

#pragma pack(push,1)
typedef struct { DWORD bitRate; BYTE sjw,tseg1,tseg2,sam,outputMode; BYTE reserved[7]; BYTE canOpMode; } XLBPDCan;
typedef union  { XLBPDCan can; BYTE raw[28]; } XLBPD;
typedef struct { DWORD busType; XLBPD data; } XLbusParams;
typedef struct {
    char name[32]; BYTE hwType,hwIndex,hwChannel;
    WORD transceiverType,transceiverState,configError; BYTE channelIndex;
    XLuint64 channelMask; DWORD channelCapabilities,channelBusCapabilities;
    BYTE isOnBus; DWORD connectedBusType; XLbusParams busParams;
    DWORD _doNotUse,driverVersion,interfaceVersion; DWORD raw_data[10];
    DWORD serialNumber,articleNumber; char transceiverName[32];
    DWORD specialCabFlags,dominantTimeout;
    BYTE dominantRecessiveDelay,recessiveDominantDelay,connectionInfo,currentlyAvailableTimestamps;
    WORD minimalSupplyVoltage,maximalSupplyVoltage; DWORD maximalBaudrate;
    BYTE fpgaCoreCapabilities,specialDeviceStatus;
    WORD channelBusActiveCapabilities,breakOffset,delimiterOffset; DWORD reserved[3];
} XLchannelConfig;
typedef struct { DWORD dllVersion,channelCount,reserved[10]; XLchannelConfig channel[64]; } XLdriverConfig;
typedef struct { DWORD id; WORD flags,dlc; XLuint64 res1; BYTE data[8]; XLuint64 res2; } XLcanMsg;
typedef union  { XLcanMsg msg; BYTE raw[32]; } XLtagData;
typedef struct { BYTE tag,chanIndex; WORD transId,portHandle; BYTE flags,reserved; XLuint64 timeStamp; XLtagData tagData; } XLevent;
typedef struct { DWORD ID,TimeStamp; BYTE TimeFlag,SendType,RemoteFlag,ExternFlag,DataLen,Data[8],Reserved[3]; } VCI_CAN_OBJ;
typedef struct { DWORD AccCode,AccMask,Reserved; BYTE Filter,Timing0,Timing1,Mode; } VCI_INIT_CONFIG_EU;
#pragma pack(pop)

#define XL_SUCCESS 0
#define XL_ERR_NOT_INITIALIZED 118
#define XL_ERR_WRONG_PARAMETER 101
#define XL_BUS_TYPE_CAN 1
#define XL_TX 10
#define XL_RX 1
#define XL_ERRF 0x01

typedef DWORD(__stdcall*PF_vOpen)(DWORD,DWORD,DWORD);
typedef DWORD(__stdcall*PF_vClose)(DWORD,DWORD);
typedef DWORD(__stdcall*PF_vInit)(DWORD,DWORD,DWORD,VCI_INIT_CONFIG_EU*);
typedef DWORD(__stdcall*PF_vStart)(DWORD,DWORD,DWORD);
typedef DWORD(__stdcall*PF_vTx)(DWORD,DWORD,DWORD,VCI_CAN_OBJ*,DWORD);
typedef DWORD(__stdcall*PF_vRx)(DWORD,DWORD,DWORD,VCI_CAN_OBJ*,DWORD,int);
typedef long(__stdcall*PF_realCfg)(XLdriverConfig*);

static HMODULE g_real = 0, g_brg = 0;
static PF_vOpen  pVOpen;  static PF_vClose pVClose; static PF_vInit pVInit;
static PF_vStart pVStart; static PF_vTx    pVTx;    static PF_vRx   pVRx;
static PF_realCfg pRealCfg;

static int  g_drvOpen = 0;
static int  g_brgOpen = 0;      /* 后端设备是否已打开 */
static int  g_actived = 0;      /* 是否已 StartCAN */
static DWORD g_devType = 4;     /* USBCAN-II */
static unsigned g_baud = 500000;
static XLportHandle g_port = 1;
static XLaccess g_mask = 0;
static unsigned g_chanIdx = 0;
static char g_dir[MAX_PATH];

static void InitDir(void)
{
    HMODULE hm=0;
    if(g_dir[0]) return;
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)&InitDir,&hm)){
        DWORD n=GetModuleFileNameA(hm,g_dir,MAX_PATH);
        if(n>0){ char*s=strrchr(g_dir,'\\'); if(s)*s=0; }
    }
}

/* 转发给原厂 DLL。x64 下调用约定是寄存器传参，用无参函数指针转发
   可以原样保留参数（最多 4 个参数的情况）。 */
long __stdcall VxlFwd(const char *name)
{
    typedef long(*PF0)(void);
    PF0 f;
    if(!g_real) return 0;
    f=(PF0)GetProcAddress(g_real,name);
    if(!f) return 0;
    return f();
}

static void LoadReal(void)
{
    char p[MAX_PATH];
    if(g_real) return;
    InitDir();
    sprintf(p,"%s\\xlrealdrv.dll",g_dir);
    g_real=LoadLibraryA(p);
    if(!g_real) g_real=LoadLibraryA("vxlapi64.dll");   /* 兜底 */
    if(g_real) pRealCfg=(PF_realCfg)GetProcAddress(g_real,"xlGetDriverConfig");
}

static int LoadBridge(void)
{
    char p[MAX_PATH];
    if(g_brg) return 1;
    InitDir();
    sprintf(p,"%s\\UniVciBridge.dll",g_dir);
    g_brg=LoadLibraryA(p);
    if(!g_brg){ sprintf(p,"%s\\USBCAN.dll",g_dir); g_brg=LoadLibraryA(p); }
    if(!g_brg) return 0;
    pVOpen =(PF_vOpen) GetProcAddress(g_brg,"VCI_OpenDevice");
    pVClose=(PF_vClose)GetProcAddress(g_brg,"VCI_CloseDevice");
    pVInit =(PF_vInit) GetProcAddress(g_brg,"VCI_InitCAN");
    pVStart=(PF_vStart)GetProcAddress(g_brg,"VCI_StartCAN");
    pVTx   =(PF_vTx)   GetProcAddress(g_brg,"VCI_Transmit");
    pVRx   =(PF_vRx)   GetProcAddress(g_brg,"VCI_Receive");
    return pVInit!=0;
}

static void ReadIni(void)
{
    char p[MAX_PATH],buf[256];
    FILE *f;
    InitDir();
    sprintf(p,"%s\\UniVciBridge.ini",g_dir);
    f=fopen(p,"rb");
    if(!f) return;
    while(fgets(buf,sizeof(buf),f)){
        char *k=buf,*v;
        while(*k==' '||*k=='\t') k++;
        v=strchr(k,'=');
        if(!v) continue;
        *v++=0;
        { char *e=v+strlen(v); while(e>v&&(e[-1]=='\n'||e[-1]=='\r'||e[-1]==' ')) *--e=0; }
        if(!_stricmp(k,"device_type")) g_devType=(DWORD)strtoul(v,0,10);
        if(!_stricmp(k,"baud"))        g_baud=(unsigned)strtoul(v,0,10);
    }
    fclose(f);
}

/* ---------- 构造一个 VN1630A 的通道表 ---------- */
static void FillChannel(XLchannelConfig*c,int i)
{
    ZeroMemory(c,sizeof(*c));
    sprintf(c->name,"VN1630A Channel %d",i+1);
    c->hwType=1; c->hwIndex=0; c->hwChannel=(BYTE)i;
    c->transceiverType=1; c->transceiverState=1; c->configError=0;
    c->channelIndex=(BYTE)i;
    c->channelMask=(XLaccess)1<<i;
    c->channelCapabilities=0x0000FFFF;
    c->channelBusCapabilities=0x0000FFFF;
    c->isOnBus=1;
    c->connectedBusType=XL_BUS_TYPE_CAN;
    c->busParams.busType=XL_BUS_TYPE_CAN;
    c->busParams.data.can.bitRate=500000;
    c->busParams.data.can.sjw=1;
    c->busParams.data.can.tseg1=13;
    c->busParams.data.can.tseg2=2;
    c->busParams.data.can.sam=0;
    c->busParams.data.can.outputMode=1;
    c->busParams.data.can.canOpMode=0;
    c->driverVersion=0x0B00000E;
    c->interfaceVersion=3;
    c->serialNumber=0x005918;
    c->articleNumber=0;
    strcpy(c->transceiverName,"CANpiggy 1057Gcap (Highspeed)");
    c->maximalBaudrate=1000000;
    c->currentlyAvailableTimestamps=1;
}

/* ===================== 导出实现 ===================== */

__declspec(dllexport) long __stdcall xlOpenDriver(void)
{
    LoadReal();
    ReadIni();
    g_drvOpen=1;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlCloseDriver(void)
{
    if(pVClose&&g_brgOpen){ pVClose(g_devType,0); g_brgOpen=0; g_actived=0; }
    g_drvOpen=0;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlGetDriverConfig(XLdriverConfig*cfg)
{
    int i;
    if(!cfg) return XL_ERR_WRONG_PARAMETER;
    ZeroMemory(cfg,sizeof(*cfg));
    cfg->dllVersion=0x0B00000E;
    cfg->channelCount=2;
    for(i=0;i<2;i++) FillChannel(&cfg->channel[i],i);
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlOpenPort(XLportHandle*port,const char*name,
        XLaccess access,XLaccess*perm,unsigned rxq,unsigned ver,unsigned busType)
{
    (void)name;(void)rxq;(void)ver;(void)busType;
    if(!port||!perm) return XL_ERR_WRONG_PARAMETER;
    if(!g_drvOpen) return XL_ERR_NOT_INITIALIZED;
    g_mask=access;
    g_chanIdx=0;
    while(g_chanIdx<32 && !((access>>g_chanIdx)&1)) g_chanIdx++;
    *port=g_port;
    *perm=access;          /* 全部授予，宿主才知道通道可用 */
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlClosePort(XLportHandle port)
{
    (void)port;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlCanSetChannelBitrate(XLportHandle port,XLaccess mask,unsigned baud)
{
    (void)port;(void)mask;
    if(baud) g_baud=baud;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlActivateChannel(XLportHandle port,XLaccess mask,unsigned busType,unsigned flags)
{
    VCI_INIT_CONFIG_EU ic;
    (void)port;(void)busType;(void)flags;
    if(!LoadBridge()) return XL_ERR_NOT_INITIALIZED;
    if(!g_brgOpen){
        if(pVOpen(g_devType,0,0)!=1) return XL_ERR_NOT_INITIALIZED;
        g_brgOpen=1;
    }
    if(g_actived) return XL_SUCCESS;
    ZeroMemory(&ic,sizeof(ic));
    ic.AccMask=0xFFFFFFFFu; ic.Filter=1;
    ic.Timing0=(BYTE)((g_baud==250000)?0x01:((g_baud==1000000)?0x00:0x00));
    ic.Timing1=(BYTE)((g_baud==250000)?0x1C:((g_baud==1000000)?0x14:0x1C));
    if(pVInit(g_devType,0,g_chanIdx,&ic)!=1) return XL_ERR_NOT_INITIALIZED;
    if(pVStart(g_devType,0,g_chanIdx)!=1) return XL_ERR_NOT_INITIALIZED;
    g_actived=1;
    (void)mask;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlDeactivateChannel(XLportHandle port,XLaccess mask)
{
    (void)port;(void)mask;
    g_actived=0;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlReceive(XLportHandle port,unsigned*count,XLevent*ev)
{
    VCI_CAN_OBJ ob[64];
    DWORD got;
    unsigned i,m=0;
    (void)port;
    if(!count||!ev) return XL_ERR_WRONG_PARAMETER;
    *count=0;
    if(!g_actived||!pVRx) return XL_SUCCESS;
    { unsigned mx=*count; if(mx>64) mx=64; if(!mx) mx=64;
      got=pVRx(g_devType,0,g_chanIdx,ob,mx,0);
      if(got==0||got>mx) return XL_SUCCESS; }
    for(i=0;i<got;i++){
        ZeroMemory(&ev[m],sizeof(ev[m]));
        ev[m].tag=XL_RX;
        ev[m].chanIndex=(BYTE)g_chanIdx;
        ev[m].portHandle=(WORD)port;
        ev[m].tagData.msg.id=ob[i].ID;
        ev[m].tagData.msg.dlc=(WORD)(ob[i].DataLen>8?8:ob[i].DataLen);
        memcpy(ev[m].tagData.msg.data,ob[i].Data,8);
        ev[m].tagData.msg.flags=(WORD)((ob[i].ExternFlag?0x02:0)|(ob[i].RemoteFlag?0x01:0));
        ev[m].timeStamp=(XLuint64)ob[i].TimeStamp*100000ULL;   /* 0.1ms -> ns */
        m++;
    }
    *count=m;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlCanTransmit(XLportHandle port,XLaccess mask,unsigned count,void*events)
{
    VCI_CAN_OBJ ob[64];
    XLevent*ev=(XLevent*)events;
    unsigned i,m=0;
    (void)port;(void)mask;
    if(!ev||!count) return XL_ERR_WRONG_PARAMETER;
    if(!g_actived||!pVTx) return XL_ERR_NOT_INITIALIZED;
    if(count>64) count=64;
    for(i=0;i<count;i++){
        if(ev[i].tag!=XL_TX) continue;
        ZeroMemory(&ob[m],sizeof(ob[m]));
        ob[m].ID=ev[i].tagData.msg.id;
        ob[m].DataLen=(BYTE)(ev[i].tagData.msg.dlc>8?8:ev[i].tagData.msg.dlc);
        memcpy(ob[m].Data,ev[i].tagData.msg.data,8);
        ob[m].ExternFlag=(BYTE)((ev[i].tagData.msg.flags&0x02)?1:0);
        ob[m].RemoteFlag=(BYTE)((ev[i].tagData.msg.flags&0x01)?1:0);
        ob[m].SendType=0;
        m++;
    }
    if(!m) return XL_SUCCESS;
    if(pVTx(g_devType,0,g_chanIdx,ob,m)!=m) return XL_ERR_WRONG_PARAMETER;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlSetApplConfig(const char*app,unsigned appCh,
        unsigned hwType,unsigned hwIndex,unsigned hwChannel,unsigned busType)
{
    (void)app;(void)appCh;(void)hwType;(void)hwIndex;(void)hwChannel;(void)busType;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlGetErrorString(long status)
{
    static char b[64];
    _snprintf(b,sizeof(b),"XL error %ld",status);
    return (long)(LONG_PTR)b;
}

__declspec(dllexport) long __stdcall xlGetChannelIndex(XLaccess mask,int*idx)
{
    int i=0;
    while(i<32 && !((mask>>i)&1)) i++;
    if(idx) *idx=i;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlGetChannelMask(int hwType,int hwIndex,int hwChannel)
{
    (void)hwType;(void)hwIndex;
    return (long)((XLaccess)1<<(hwChannel&31));
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlGetSyncTime(XLportHandle port,XLuint64*t)
{
    (void)port;
    if(t) *t=(XLuint64)GetTickCount()*1000000ULL;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlGetChannelTime(XLportHandle port,XLaccess mask,XLuint64*t)
{
    (void)port;(void)mask;
    if(t) *t=(XLuint64)GetTickCount()*1000000ULL;
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlFlushReceiveQueue(XLportHandle port)
{
    VCI_CAN_OBJ ob[64];
    (void)port;
    if(g_actived&&pVRx){ int k; for(k=0;k<50;k++){ DWORD g=pVRx(g_devType,0,g_chanIdx,ob,64,0); if(!g||g>64) break; } }
    return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlResetClock(XLportHandle port)
{
    (void)port; return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlSetNotification(XLportHandle port,HANDLE*h,int*num)
{
    (void)port;(void)h; if(num)*num=0; return XL_SUCCESS;
}

__declspec(dllexport) long __stdcall xlSetTimer(int handle,unsigned period)
{
    (void)handle;(void)period; return XL_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE hi,DWORD reason,LPVOID r)
{
    (void)hi;(void)r;
    if(reason==DLL_PROCESS_ATTACH) InitDir();
    return TRUE;
}
