/*
 * VxlEmu64.c —— 64 位 Vector 门面：让 CANape/CANoe 看到 VN1630A
 *
 *   64 位 CANape → 本 DLL → 共享内存 → VxlHelper.exe（32位）→ 周立功
 *
 * 为什么不用直接调：周立功只有 32 位 DLL，64 位进程加载不了。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "VxlShm.h"

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

typedef struct { DWORD id; WORD flags,dlc; u64 res1; BYTE data[8]; u64 res2; } XLcanMsg;
typedef union  { XLcanMsg msg; BYTE raw[32]; } XLtagData;
typedef struct { BYTE tag,chanIndex; WORD transId,portHandle; BYTE flags,reserved; u64 timeStamp; XLtagData tagData; } XLevent;
#define Ev XLevent

#define XL_OK 0
#define XL_ERR_PARAM 101
#define XL_ERR_NOTINIT 118
#define XL_RX 1
#define XL_TX 10

static HANDLE g_map=0,g_evRx=0,g_evTx=0,g_proc=0,g_evt=0;
static VxlShm *g_shm=0;
static int g_drv=0, g_open=0;
static DWORD g_dev=4, g_ch=0, g_baud=500000;
static char g_dir[MAX_PATH];
static XLportHandle g_port=1;
static XLaccess g_mask=0;

static void Dir(void);
static void DLog(const char *fmt,...);
#define LOG(...) DLog(__VA_ARGS__)
static HMODULE g_real=0;
long __stdcall VxlFwd(const char *name)
{
    typedef long(*PF0)(void);
    PF0 f;
    if(!g_real){
        char p[MAX_PATH]; Dir();
        _snprintf(p,sizeof(p),"%s\\xlrealdrv64.dll",g_dir);
        g_real=LoadLibraryA(p);
        if(!g_real){ _snprintf(p,sizeof(p),"%s\\vxlapi64.dll.原厂备份",g_dir); g_real=LoadLibraryA(p); }
        LOG("  [fwd] 原厂副本: %s", g_real?"已加载":"没有");
    }
    if(!g_real) return 0;
    f=(PF0)GetProcAddress(g_real,name);
    return f?f():0;
}

static void Dir(void)
{
    HMODULE hm=0;
    if(g_dir[0]) return;
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)&Dir,&hm)){
        DWORD n=GetModuleFileNameA(hm,g_dir,MAX_PATH);
        if(n>0){ char*s=strrchr(g_dir,'\\'); if(s)*s=0; }
    }
}
#define LOG(...) DLog(__VA_ARGS__)
static void DLog(const char *fmt,...)
{
    static FILE*fp=0; char p[MAX_PATH]; va_list ap;
    if(!fp){ Dir(); _snprintf(p,sizeof(p),"%s\\VxlEmu64.log",g_dir); fp=fopen(p,"a"); }
    if(!fp) return;
    va_start(ap,fmt); vfprintf(fp,fmt,ap); va_end(ap);
    fprintf(fp,"\n"); fflush(fp);
}

static void ReadIni(void)
{
    char p[MAX_PATH],b[256]; FILE*f;
    Dir(); _snprintf(p,sizeof(p),"%s\\UniVciBridge.ini",g_dir);
    f=fopen(p,"rb"); if(!f) return;
    while(fgets(b,sizeof(b),f)){
        char*k=b,*v; while(*k==' '||*k=='\t')k++;
        v=strchr(k,'='); if(!v)continue; *v++=0;
        { char*e=v+strlen(v); while(e>v&&(e[-1]=='\n'||e[-1]=='\r'||e[-1]==' '))*--e=0; }
        if(!_stricmp(k,"device_type")) g_dev=(DWORD)strtoul(v,0,10);
        if(!_stricmp(k,"baud"))        g_baud=(DWORD)strtoul(v,0,10);
    }
    fclose(f);
}

/* 拉起辅助进程并等它就绪 */
static int StartHelper(void)
{
    char exe[MAX_PATH],cmd[MAX_PATH*2],dir[MAX_PATH];
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    int i;

    Dir(); strcpy(dir,g_dir);
    _snprintf(exe,sizeof(exe),"%s\\VxlHelper.exe",dir);
    if(GetFileAttributesA(exe)==INVALID_FILE_ATTRIBUTES){
        LOG("[x] 找不到 VxlHelper.exe（要放在本 DLL 同目录）");
        return 0;
    }
    _snprintf(cmd,sizeof(cmd),"\"%s\" %lu %lu %lu",exe,
              (unsigned long)g_dev,(unsigned long)g_ch,(unsigned long)g_baud);

    if(!g_map){
        g_map=CreateFileMappingA(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,sizeof(VxlShm),VXL_SHM_NAME);
        if(!g_map){ LOG("[x] 共享内存失败 %lu",GetLastError()); return 0; }
    }
    g_shm=(VxlShm*)MapViewOfFile(g_map,FILE_MAP_ALL_ACCESS,0,0,sizeof(VxlShm));
    if(!g_shm){ LOG("[x] 映射失败"); return 0; }
    g_evRx=CreateEventA(NULL,FALSE,FALSE,VXL_EVT_RX);
    g_evTx=CreateEventA(NULL,FALSE,FALSE,VXL_EVT_TX);

    ZeroMemory(&si,sizeof(si)); si.cb=sizeof(si);
    ZeroMemory(&pi,sizeof(pi));
    if(!CreateProcessA(NULL,cmd,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,dir,&si,&pi)){
        LOG("[x] 拉起辅助进程失败 %lu",GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    g_proc=pi.hProcess;
    LOG("[ok] 辅助进程已拉起 pid=%lu",(unsigned long)pi.dwProcessId);

    for(i=0;i<300;i++){
        if(g_shm->ready) { LOG("[ok] 辅助进程就绪"); return 1; }
        if(g_shm->err){ LOG("[x] 辅助进程报错 err=%ld",g_shm->err); return 0; }
        Sleep(20);
    }
    LOG("[x] 等辅助进程超时");
    return 0;
}

static void FillChannel(ChanCfg*c,int i)
{
    ZeroMemory(c,sizeof(*c));
        _snprintf(c->name,sizeof(c->name),"VN1630A Channel %d",i+1);
    c->hwType=57; c->hwIndex=0; c->hwChannel=(BYTE)i;
    /* ch1/2 = CANpiggy(322)，ch3/4 = 板上CAN(316)，ch5 = D/A(1021) */
    c->transceiverType=(WORD)(i<2?322:(i<4?316:1021));
    c->transceiverState=1;
    c->channelIndex=(BYTE)i;
    c->channelMask=(XLaccess)1<<i;
    c->channelCapabilities=0xE2008017;
    c->channelBusCapabilities=0x00010903;
    c->isOnBus=0;
    c->connectedBusType=1;
    c->busParams.busType=1;
    c->busParams.data.can.bitRate=g_baud;
    c->busParams.data.can.sjw=1;
    c->busParams.data.can.tseg1=13;
    c->busParams.data.can.tseg2=2;
    c->busParams.data.can.outputMode=1;
    c->driverVersion=0x0B00000E;
    c->interfaceVersion=3;
    c->serialNumber=0x0000171E;
    strcpy(c->transceiverName, i<2?"CANpiggy 1057Gcap (Highspeed)":
                                 (i<4?"On board CAN 1051cap(Highspeed)":"On board D/A IO 1021"));
}

__declspec(dllexport) long __stdcall xlOpenDriver(void)
{
    Dir(); ReadIni(); g_drv=1;
    LOG("=== xlOpenDriver  dev=%lu baud=%lu",(unsigned long)g_dev,(unsigned long)g_baud);
    return XL_OK;
}

__declspec(dllexport) long __stdcall xlCloseDriver(void)
{
    if(g_shm) g_shm->quit=1;
    if(g_proc){ WaitForSingleObject(g_proc,1000); CloseHandle(g_proc); g_proc=0; }
    if(g_shm){ UnmapViewOfFile(g_shm); g_shm=0; }
    if(g_map){ CloseHandle(g_map); g_map=0; }
    g_drv=0; g_open=0;
    return XL_OK;
}

__declspec(dllexport) long __stdcall xlGetDriverConfig(DrvCfg*cfg)
{
    int i;
    if(!cfg) return XL_ERR_PARAM;
    ZeroMemory(cfg,sizeof(*cfg));
    cfg->dllVersion=0x0B00000E;
    cfg->channelCount=5;                 /* 真 VN1630A 报 5 路：4×CAN + 1×D/A */
    for(i=0;i<5;i++) FillChannel(&cfg->channel[i],i);
    LOG("  xlGetDriverConfig -> %lu 通道",(unsigned long)cfg->channelCount);
    return XL_OK;
}

/* CANape 新版会调这个（带版本号的变体），必须自己实现，否则会转发给原厂、
   拿到的是真设备信息，我们的假设备就被绕过去了 */
__declspec(dllexport) long __stdcall xlGetDriverConfig2(void*cfg,unsigned size)
{
    (void)size;
    LOG("  xlGetDriverConfig2 被调用 size=%lu",(unsigned long)size);
    return xlGetDriverConfig((DrvCfg*)cfg);
}

__declspec(dllexport) long __stdcall xlOpenPort(XLportHandle*port,const char*name,
        XLaccess access,XLaccess*perm,unsigned rxq,unsigned ver,unsigned busType)
{
    (void)rxq;(void)ver;(void)busType;
    if(!port||!perm) return XL_ERR_PARAM;
    if(!g_drv) return XL_ERR_NOTINIT;
    g_mask=access; g_ch=0;
    while(g_ch<32 && !((access>>g_ch)&1)) g_ch++;
    *port=g_port; *perm=access;
    LOG("  xlOpenPort name=%s access=0x%I64X -> port=%lu perm=0x%I64X",
        name?name:"(null)",(unsigned long long)access,(unsigned long)*port,(unsigned long long)*perm);
    return XL_OK;
}

__declspec(dllexport) long __stdcall xlClosePort(XLportHandle port){ (void)port; return XL_OK; }

__declspec(dllexport) long __stdcall xlCanSetChannelBitrate(XLportHandle port,XLaccess mask,unsigned baud)
{
    (void)port;(void)mask;
    if(baud) g_baud=baud;
    LOG("  xlCanSetChannelBitrate %lu",(unsigned long)baud);
    return XL_OK;
}

__declspec(dllexport) long __stdcall xlActivateChannel(XLportHandle port,XLaccess mask,unsigned busType,unsigned flags)
{
    (void)port;(void)mask;(void)busType;(void)flags;
    if(g_open) return XL_OK;
    if(!g_shm){ if(!StartHelper()) return XL_ERR_NOTINIT; }
    g_open=1;
    LOG("  xlActivateChannel -> 就绪");
    return XL_OK;
}

__declspec(dllexport) long __stdcall xlDeactivateChannel(XLportHandle port,XLaccess mask)
{
    (void)port;(void)mask; g_open=0; return XL_OK;
}

__declspec(dllexport) long __stdcall xlReceive(XLportHandle port,unsigned*count,XLevent*ev)
{
    unsigned want,got=0;
    (void)port;
    if(!count||!ev) return XL_ERR_PARAM;
    want=*count; if(!want) want=1; if(want>64) want=64;
    *count=0;
    if(!g_open||!g_shm) return XL_OK;
    while(got<want && g_shm->rxTail!=g_shm->rxHead){
        VxlFrame*f=&g_shm->rx[g_shm->rxTail % VXL_RING];
        ZeroMemory(&ev[got],sizeof(XLevent));
        ev[got].tag=XL_RX;
        ev[got].chanIndex=(BYTE)g_ch;
        ev[got].portHandle=(WORD)port;
        ev[got].tagData.msg.id=f->id;
        ev[got].tagData.msg.dlc=(WORD)(f->dlc>8?8:f->dlc);
        memcpy(ev[got].tagData.msg.data,f->data,8);
        ev[got].tagData.msg.flags=(WORD)(f->flags&3);
        ev[got].timeStamp=(u64)f->timeStamp*100000ULL;
        g_shm->rxTail++; got++;
    }
    *count=got;
    return XL_OK;
}

__declspec(dllexport) long __stdcall xlCanTransmit(XLportHandle port,XLaccess mask,unsigned count,void*events)
{
    XLevent*e=(XLevent*)events; unsigned i,m=0;
    (void)port;(void)mask;
    if(!e||!count) return XL_ERR_PARAM;
    if(!g_open||!g_shm) return XL_ERR_NOTINIT;
    if(count>64) count=64;
    for(i=0;i<count;i++){
        VxlFrame*f;
        if(e[i].tag!=XL_TX) continue;
        f=&g_shm->tx[g_shm->txHead % VXL_RING];
        f->id=e[i].tagData.msg.id;
        f->dlc=(unsigned char)(e[i].tagData.msg.dlc>8?8:e[i].tagData.msg.dlc);
        f->flags=(unsigned char)(e[i].tagData.msg.flags&3);
        memcpy(f->data,e[i].tagData.msg.data,8);
        g_shm->txHead++; m++;
    }
    if(m && g_evTx) SetEvent(g_evTx);
    if(m && g_shm->txHead-g_shm->txTail>VXL_RING) g_shm->txTail=g_shm->txHead-VXL_RING;
    return XL_OK;
}

__declspec(dllexport) long __stdcall xlSetApplConfig(const char*a,unsigned b,unsigned c,unsigned d,unsigned e,unsigned f)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return XL_OK; }

__declspec(dllexport) long __stdcall xlGetErrorString(long st)
{ static char b[64]; _snprintf(b,sizeof(b),"XL error %ld",st); return (long)(LONG_PTR)b; }

__declspec(dllexport) long __stdcall xlSetNotification(XLportHandle port,HANDLE*h,int num)
{
    (void)port;(void)num;
    if(!g_evt) g_evt=CreateEventA(NULL,FALSE,FALSE,NULL);
    if(h) *h=g_evt;
    LOG("  xlSetNotification -> %p",(void*)g_evt);
    return XL_OK;
}

__declspec(dllexport) long __stdcall xlGetSyncTime(XLportHandle p,u64*t)
{ (void)p; if(t)*t=(u64)GetTickCount()*1000000ULL; return XL_OK; }
__declspec(dllexport) long __stdcall xlGetChannelTime(XLportHandle p,XLaccess m,u64*t)
{ (void)p;(void)m; if(t)*t=(u64)GetTickCount()*1000000ULL; return XL_OK; }
__declspec(dllexport) long __stdcall xlFlushReceiveQueue(XLportHandle p){ (void)p; return XL_OK; }
__declspec(dllexport) long __stdcall xlResetClock(XLportHandle p){ (void)p; return XL_OK; }
__declspec(dllexport) long __stdcall xlGetChannelIndex(long mask,int*idx)
{ int i=0; while(i<32&&!((mask>>i)&1))i++; if(idx)*idx=i; return XL_OK; }
__declspec(dllexport) long __stdcall xlGetChannelMask(int ch,XLaccess*m)
{ if(m)*m=(XLaccess)1<<(ch&31); return XL_OK; }

BOOL WINAPI DllMain(HINSTANCE hi,DWORD r,LPVOID p)
{ (void)hi;(void)p; if(r==DLL_PROCESS_ATTACH) Dir(); return TRUE; }
