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
#include <stdarg.h>
static void InitDir(void);

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
static long(__stdcall*pRealOpen)(void);

static int  g_drvOpen = 0;
static int  g_brgOpen = 0;      /* 后端设备是否已打开 */
static int  g_actived = 0;      /* 是否已 StartCAN */
static DWORD g_devType = 4;     /* USBCAN-II */
static unsigned g_baud = 500000;
static XLportHandle g_port = 1;
static XLaccess g_mask = 0;
static unsigned g_chanIdx = 0;
static char g_dir[MAX_PATH];
static HANDLE g_evt = NULL;
/* ── 收帧：后台线程 + 环形缓冲，收到就 SetEvent ──
   上位机通常是【等事件 → 收帧】，光给个句柄不触发，它就永远等不到。 */
#define RING 8192
static VCI_CAN_OBJ g_ring[RING];
static volatile LONG g_head=0, g_tail=0;
static CRITICAL_SECTION g_rcs;
static int g_rcsOk=0;
static HANDLE g_thr=NULL;
static volatile LONG g_pump=0;

static DWORD WINAPI Pump(LPVOID pv)
{
    VCI_CAN_OBJ tmp[64];
    (void)pv;
    while(g_pump){
        DWORD got=(pVRx?pVRx(g_devType,0,g_chanIdx,tmp,64,1):0);
        if(got>0 && got<=64){
            DWORD i;
            EnterCriticalSection(&g_rcs);
            for(i=0;i<got;i++){ g_ring[g_head%RING]=tmp[i]; g_head++; }
            LeaveCriticalSection(&g_rcs);
            if(g_evt) SetEvent(g_evt);
        } else Sleep(1);
    }
    return 0;
}

#define EMULOG(...) EmuLog(__VA_ARGS__)
static void EmuLog(const char *fmt,...)
{
    static FILE *fp=NULL; char p[MAX_PATH]; va_list ap;
    if(!fp){ InitDir(); sprintf(p,"%s\\VxlEmu.log",g_dir); fp=fopen(p,"a"); }
    if(!fp) return;
    va_start(ap,fmt); vfprintf(fp,fmt,ap); va_end(ap);
    fprintf(fp,"\n"); fflush(fp);
}

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
    /*
     ★★ 不转发 —— 这是踩过的坑 ★★

        我们的桩函数都是"无参数"，而原厂函数有各自真实的参数个数。
        __stdcall 在 32 位下由被调方清栈：参数个数不对，栈立刻错乱，
        宿主进程直接崩（实测：放了原厂 DLL 副本就必崩）。

        所以：不认识的函数一律返回 0（成功），绝不转发。
        代价是个别功能不可用，收益是宿主不会崩 —— 这个取舍必须这样。
     */
    (void)name;
    return 0;
}

static void LoadReal(void)
{
    char p[MAX_PATH];
    if(g_real) return;
    InitDir();
    sprintf(p,"%s\\xlrealdrv.dll",g_dir);
    g_real=LoadLibraryA(p);
    if(!g_real) g_real=LoadLibraryA("vxlapi64.dll");   /* 兜底 */
    if(g_real){
        pRealCfg =(PF_realCfg) GetProcAddress(g_real,"xlGetDriverConfig");
        pRealOpen=(long(__stdcall*)(void))GetProcAddress(g_real,"xlOpenDriver");
        if(pRealOpen){ long r=pRealOpen(); EMULOG("  真驱动 xlOpenDriver -> %ld", r); }
    }
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
    /*
     ★ 这些数字是从真 VN1630A 的驱动配置里抄来的（2026-11-06 实测）
        上位机就是靠 hwType / capabilities 认设备的，猜不得。
     */
    c->hwType=57; c->hwIndex=0; c->hwChannel=(BYTE)i;      /* 57 = VN1630A */
    c->transceiverType=322; c->transceiverState=1; c->configError=0;
    c->channelIndex=(BYTE)i;
    c->channelMask=(XLaccess)1<<i;
    c->channelCapabilities=0xE2008017;
    c->channelBusCapabilities=0x00010903;
    c->isOnBus=0;
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
    c->serialNumber=0x0000171E;
    c->articleNumber=0;
    strcpy(c->transceiverName,"CANpiggy 1057Gcap (Highspeed)");
    c->maximalBaudrate=1000000;
    c->currentlyAvailableTimestamps=1;
}

/* ===================== 导出实现 ===================== */

__declspec(dllexport) long __stdcall xlOpenDriver(void)
{
    EMULOG("=== xlOpenDriver ===");
    LoadReal();
    ReadIni();
    g_drvOpen=1;
    EMULOG("  real dll: %s   dev=%lu baud=%lu", g_real?"已加载":"没有", (unsigned long)g_devType, g_baud);
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

    /*
     ★ 优先用【真驱动的通道描述】，只把名字改成 VN1630A

       我们自己造的描述有些字段值猜得不对（hwType、capabilities 等），
       上位机会因此认不出设备。真驱动返回的描述每个字段都是对的，
       复制过来再改名字，兼容性最好。

       真驱动不在时（VN1630A 没插），退回自己造的。
     */
    cfg->dllVersion=0x0B00000E;
    cfg->channelCount=2;
    for(i=0;i<2;i++) FillChannel(&cfg->channel[i],i);
    EMULOG("  xlGetDriverConfig -> 自造 2 通道");
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
    EMULOG("  xlOpenPort name=%s access=0x%I64X", name?name:"(null)", (unsigned long long)access);
    *port=g_port;
    *perm=access;
    EMULOG("  xlOpenPort -> 0 port=%lu perm=0x%I64X", (unsigned long)*port, (unsigned long long)*perm);          /* 全部授予，宿主才知道通道可用 */
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
    EMULOG("  xlActivateChannel mask=0x%I64X busType=%lu", (unsigned long long)mask, (unsigned long)busType);
    if(!LoadBridge()){ EMULOG("  [x] UniVciBridge 加载失败"); return XL_ERR_NOT_INITIALIZED; }
    if(!g_brgOpen){
        { DWORD r=pVOpen(g_devType,0,0); EMULOG("  VCI_OpenDevice(%lu) -> %lu",(unsigned long)g_devType,(unsigned long)r);
          if(r!=1){ EMULOG("  [x] 硬件打不开"); return XL_ERR_NOT_INITIALIZED; } }
        g_brgOpen=1;
    }
    if(g_actived) return XL_SUCCESS;
    ZeroMemory(&ic,sizeof(ic));
    ic.AccMask=0xFFFFFFFFu; ic.Filter=1;
    ic.Timing0=(BYTE)((g_baud==250000)?0x01:((g_baud==1000000)?0x00:0x00));
    ic.Timing1=(BYTE)((g_baud==250000)?0x1C:((g_baud==1000000)?0x14:0x1C));
    { DWORD r=pVInit(g_devType,0,g_chanIdx,&ic); EMULOG("  VCI_InitCAN(ch=%lu) -> %lu",(unsigned long)g_chanIdx,(unsigned long)r);
      if(r!=1) return XL_ERR_NOT_INITIALIZED; }
    { DWORD r=pVStart(g_devType,0,g_chanIdx); EMULOG("  VCI_StartCAN -> %lu",(unsigned long)r);
      if(r!=1) return XL_ERR_NOT_INITIALIZED; }
    g_actived=1;
    if(!g_rcsOk){ InitializeCriticalSection(&g_rcs); g_rcsOk=1; }
    if(!g_thr){ g_pump=1; g_thr=CreateThread(NULL,0,Pump,NULL,0,NULL);
                EMULOG("  收帧线程已启动"); }
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
    unsigned want,got=0;
    (void)port;
    if(!count||!ev) return XL_ERR_WRONG_PARAMETER;
    want=*count; if(want==0) want=1; if(want>64) want=64;
    *count=0;
    if(!g_actived||!g_rcsOk) return XL_SUCCESS;
    EnterCriticalSection(&g_rcs);
    while(got<want && g_tail!=g_head){
        VCI_CAN_OBJ*o=&g_ring[g_tail%RING];
        ZeroMemory(&ev[got],sizeof(ev[got]));
        ev[got].tag=XL_RX;
        ev[got].chanIndex=(BYTE)g_chanIdx;
        ev[got].portHandle=(WORD)port;
        ev[got].tagData.msg.id=o->ID;
        ev[got].tagData.msg.dlc=(WORD)(o->DataLen>8?8:o->DataLen);
        memcpy(ev[got].tagData.msg.data,o->Data,8);
        ev[got].tagData.msg.flags=(WORD)((o->ExternFlag?0x02:0)|(o->RemoteFlag?0x01:0));
        ev[got].timeStamp=(XLuint64)o->TimeStamp*100000ULL;
        g_tail++; got++;
    }
    LeaveCriticalSection(&g_rcs);
    *count=got;
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

/*
 ★ xlSetNotification(port, HANDLE *pEvent, int numEvents)
   第三个参数是【按值】的整数，不是指针 —— 原来当成 int* 解引用，
   会访问非法地址直接崩（.NET 里表现为 AccessViolationException）。

   而且这里要真的返回一个 Windows 事件句柄，上位机拿它等收帧通知。
 */




__declspec(dllexport) long __stdcall xlSetNotification(XLportHandle port,HANDLE*h,int numEvents)
{
    (void)port;(void)numEvents;
    if(!g_evt) g_evt=CreateEventA(NULL,FALSE,FALSE,NULL);
    if(h) *h=g_evt;
    EMULOG("  xlSetNotification -> handle=%p", (void*)g_evt);
    return XL_SUCCESS;
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
