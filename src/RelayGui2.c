/*
 * RelayGui2.c —— 反向桥接单文件界面版（合并中继 + 配置 + 日志）
 * 输出成 RelayGui.exe。旧的 RelayGui.c 保留作对照，不再编译。
 *
 * 要点：
 *   · 中继循环跑在工作线程，没有黑窗口
 *   · [一键分配] 调 xlSetApplConfig 直接写 Vector 的通道分配
 *   · [诊断] 列通道 / 试开盒子   · [一键恢复] 全部收干净
 *   · CANape 里显示的是 Virtual Channel（驱动命名），功能不受影响
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef long long XLuint64;
typedef XLuint64   XLaccess;
typedef DWORD      XLportHandle;
typedef DWORD      XLstatus;

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
#define XL_BUS_TYPE_CAN 1
#define XL_IVER 3
#define XL_TX 10
#define XL_RX 1
#define XL_ERRF 0x01

typedef XLstatus(__stdcall*PF_openDrv)(void);
typedef XLstatus(__stdcall*PF_closeDrv)(void);
typedef XLstatus(__stdcall*PF_getCfg)(XLdriverConfig*);
typedef XLstatus(__stdcall*PF_setAppl)(const char*,unsigned,unsigned,unsigned,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_openPort)(XLportHandle*,const char*,XLaccess,XLaccess*,unsigned,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_closePort)(XLportHandle);
typedef XLstatus(__stdcall*PF_setBaud)(XLportHandle,XLaccess,unsigned);
typedef XLstatus(__stdcall*PF_act)(XLportHandle,XLaccess,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_deact)(XLportHandle,XLaccess);
typedef XLstatus(__stdcall*PF_recv)(XLportHandle,unsigned*,unsigned,XLevent*);
typedef XLstatus(__stdcall*PF_xmit)(XLportHandle,XLaccess,unsigned,void*);
typedef DWORD(__stdcall*PF_vOpen)(DWORD,DWORD,DWORD);
typedef DWORD(__stdcall*PF_vClose)(DWORD,DWORD);
typedef DWORD(__stdcall*PF_vInit)(DWORD,DWORD,DWORD,VCI_INIT_CONFIG_EU*);
typedef DWORD(__stdcall*PF_vStart)(DWORD,DWORD,DWORD);
typedef DWORD(__stdcall*PF_vTx)(DWORD,DWORD,DWORD,VCI_CAN_OBJ*,DWORD);
typedef DWORD(__stdcall*PF_vRx)(DWORD,DWORD,DWORD,VCI_CAN_OBJ*,DWORD,int);

static HMODULE g_hVxl=0,g_hBrg=0;
static PF_openDrv pOpenDrv; static PF_closeDrv pCloseDrv; static PF_getCfg pGetCfg;
static PF_setAppl pSetAppl; static PF_openPort pOpenPort; static PF_closePort pClosePort;
static PF_setBaud pSetBaud; static PF_act pAct; static PF_deact pDeact;
static PF_recv pRecv; static PF_xmit pXmit;
static PF_vOpen pVOpen; static PF_vClose pVClose; static PF_vInit pVInit;
static PF_vStart pVStart; static PF_vTx pVTx; static PF_vRx pVRx;

static volatile LONG g_run=0;
static XLportHandle g_port=0; static XLaccess g_mask=0; static unsigned g_vidx=0;
static DWORD g_dev=21; static unsigned long g_toHw=0,g_toVxl=0,g_drop=0;
static CRITICAL_SECTION g_cs;
static HWND g_log=0,g_be=0,g_hw=0,g_vch=0,g_baud=0,g_ape=0,g_stat=0,g_main=0,g_devc=0;
static int g_drvOpen=0;
static int g_devOpen=0;
static int g_devStarted=0;   /* 通道只能 Start 一次，重复 Start 会把桥搞崩 */   /* 桥的设备句柄：打开后不再关，避免"关了又开"把桥搞崩 */   /* xlOpenDriver 只能调一次，重复调用会把驱动搞坏 */

#define ID_BE 1001
#define ID_HW 1002
#define ID_VCH 1003
#define ID_BAUD 1004
#define ID_APE 1005
#define ID_DEV 1006
#define ID_ASSIGN 1010
#define ID_START 1011
#define ID_STOP 1012
#define ID_DIAG 1013
#define ID_RESTORE 1014
#define ID_LOG 1020
#define ID_STAT 1021
#define ID_TX 1030

static const char*kBeVal[]={"auto","vci","pcan","gcan","kvaser","tosun"};
static const char*kBeTxt[]={"自动探测","周立功","PEAK","广成","Kvaser","同星"};
static const char*kBdVal[]={"100000","125000","250000","500000","800000","1000000"};
static const char*kBdTxt[]={"100K","125K","250K","500K","800K","1M"};

static DWORD DevFromCombo(void)
{
    char b[32]; int i;
    if(!g_devc) return 21;
    i=(int)SendMessageA(g_devc,CB_GETCURSEL,0,0);
    if(i<0) return 21;
    GetWindowTextA(g_devc,b,32);
    { char *p=b; while(*p&&(*p<'0'||*p>'9')) p++;
      if(*p) return (DWORD)strtoul(p,0,10); }
    return 21;
}

static void SelfDir(char*out,DWORD n){GetModuleFileNameA(0,out,n);{char*s=strrchr(out,'\\');if(s)*s=0;}}

static void UI(const char*fmt,...)
{
    char b[2048]; va_list ap; va_start(ap,fmt); wvsprintfA(b,fmt,ap); va_end(ap);
    if(!g_log) return;
    EnterCriticalSection(&g_cs);
    {int n=GetWindowTextLengthA(g_log);
     SendMessageA(g_log,EM_SETSEL,n,n);
     SendMessageA(g_log,EM_REPLACESEL,FALSE,(LPARAM)b);
     SendMessageA(g_log,EM_REPLACESEL,FALSE,(LPARAM)"\r\n");
     SendMessageA(g_log,EM_SETSEL,-1,-1);}
    LeaveCriticalSection(&g_cs);
}

static int LoadVxl(void)
{
    if(g_hVxl) return 1;
    g_hVxl=LoadLibraryA("vxlapi.dll");
    if(!g_hVxl){UI("[x] vxlapi.dll 加载失败");return 0;}
    pOpenDrv=(PF_openDrv)GetProcAddress(g_hVxl,"xlOpenDriver");
    pCloseDrv=(PF_closeDrv)GetProcAddress(g_hVxl,"xlCloseDriver");
    pGetCfg=(PF_getCfg)GetProcAddress(g_hVxl,"xlGetDriverConfig");
    pSetAppl=(PF_setAppl)GetProcAddress(g_hVxl,"xlSetApplConfig");
    pOpenPort=(PF_openPort)GetProcAddress(g_hVxl,"xlOpenPort");
    pClosePort=(PF_closePort)GetProcAddress(g_hVxl,"xlClosePort");
    pSetBaud=(PF_setBaud)GetProcAddress(g_hVxl,"xlCanSetChannelBitrate");
    pAct=(PF_act)GetProcAddress(g_hVxl,"xlActivateChannel");
    pDeact=(PF_deact)GetProcAddress(g_hVxl,"xlDeactivateChannel");
    pRecv=(PF_recv)GetProcAddress(g_hVxl,"xlReceive");
    pXmit=(PF_xmit)GetProcAddress(g_hVxl,"xlCanTransmit");
    return pOpenDrv&&pGetCfg&&pOpenPort&&pRecv&&pXmit;
}

static int LoadBrg(void)
{
    char d[MAX_PATH],p[MAX_PATH];
    if(g_hBrg) return 1;
    SelfDir(d,MAX_PATH); sprintf(p,"%s\\UniVciBridge.dll",d);
    g_hBrg=LoadLibraryA(p);
    if(!g_hBrg){sprintf(p,"%s\\USBCAN.dll",d);g_hBrg=LoadLibraryA(p);}
    if(!g_hBrg){UI("[x] UniVciBridge.dll 加载失败");return 0;}
    pVOpen=(PF_vOpen)GetProcAddress(g_hBrg,"VCI_OpenDevice");
    pVClose=(PF_vClose)GetProcAddress(g_hBrg,"VCI_CloseDevice");
    pVInit=(PF_vInit)GetProcAddress(g_hBrg,"VCI_InitCAN");
    pVStart=(PF_vStart)GetProcAddress(g_hBrg,"VCI_StartCAN");
    pVTx=(PF_vTx)GetProcAddress(g_hBrg,"VCI_Transmit");
    pVRx=(PF_vRx)GetProcAddress(g_hBrg,"VCI_Receive");
    return pVInit!=0;
}

static int IsVirt(XLchannelConfig*c)
{
    return strstr(c->name,"Virtual")||strstr(c->transceiverName,"Virtual");
}

static int ListCh(XLdriverConfig*cf,int*virt)
{
    int i; *virt=0;
    if(!LoadVxl()) return 0;
    if(!g_drvOpen){
        if(pOpenDrv()!=XL_SUCCESS){UI("[x] xlOpenDriver 失败");return 0;}
        g_drvOpen=1;
    }
    ZeroMemory(cf,sizeof(*cf));
    if(pGetCfg(cf)!=XL_SUCCESS){UI("[x] xlGetDriverConfig 失败");return 0;}
    UI("[诊断] 驱动报告 %lu 个通道",(unsigned long)cf->channelCount);
    for(i=0;i<(int)cf->channelCount&&i<64;i++){
        if(IsVirt(&cf->channel[i]))(*virt)++;
        UI("   %2d %-22s mask=0x%I64X%s",i,cf->channel[i].name,
           (unsigned long long)cf->channel[i].channelMask,IsVirt(&cf->channel[i])?" [虚拟]":"");
    }
    UI("[诊断] 虚拟通道 %d 个",*virt);
    return 1;
}

static void DumpBridgeLog(void)
{
    char d[MAX_PATH],p[MAX_PATH];
    SelfDir(d,MAX_PATH); sprintf(p,"%s\\UniVciBridge.log",d);
    if(GetFileAttributesA(p)==INVALID_FILE_ATTRIBUTES)
        UI("[日志] 没有 UniVciBridge.log —— 说明桥还没跑过");
    else
        UI("[日志] 桥的详细日志在这里（用记事本打开）：%s",p);
}
static void DoDiag(void)
{
    XLdriverConfig cfg; int v=0;
    UI("──────── 诊断 ────────");
    if(!ListCh(&cfg,&v)) return;
    if(v<2) UI("[!] 虚拟通道不足 2 个：Vector Hardware Config 里需要至少两条 Virtual CAN");
    g_dev = DevFromCombo();
    if(LoadBrg()){
        DWORD r = g_devOpen ? 1 : pVOpen(g_dev,0,0);
        if(r==1) g_devOpen=1;
        UI("[诊断] 盒子 OpenDevice -> %lu %s",(unsigned long)r,r==1?"OK":"失败");
        if(r==1){
            VCI_INIT_CONFIG_EU ic; ZeroMemory(&ic,sizeof(ic));
            ic.AccMask=0xFFFFFFFFu; ic.Filter=1; ic.Timing0=0; ic.Timing1=0x1C;
            UI("[诊断] 型号 %lu：InitCAN -> %lu",(unsigned long)g_dev,(unsigned long)pVInit(g_dev,0,0,&ic));
            UI("[诊断] 只做初始化检查，不启动通道（避免和【启动中继】抢着 Start）");
        } else UI("[!] 盒子没插、或被别的程序占着");
    }
    DumpBridgeLog();
    UI("──────── 结束 ────────");
}

static void DoAssign(void)
{
    XLdriverConfig cfg; int v=0,i,seen=0;
    int ape=(int)SendMessageA(g_ape,CB_GETCURSEL,0,0)+1;
    int our=(int)SendMessageA(g_vch,CB_GETCURSEL,0,0)+1;
    if(ape==our){UI("[x] CANape 和本中继不能同路，会互相抢权限");return;}
    if(!ListCh(&cfg,&v)) return;
    for(i=0;i<(int)cfg.channelCount&&i<64;i++){
        if(!IsVirt(&cfg.channel[i])) continue;
        seen++;
        if(seen==ape&&pSetAppl){
            XLstatus s=pSetAppl("CANape",0,cfg.channel[i].hwType,cfg.channel[i].hwIndex,cfg.channel[i].hwChannel,XL_BUS_TYPE_CAN);
            UI("[分配] CANape -> 第 %d 路  (rc=%lu)",ape,(unsigned long)s);
        }
        if(seen==our&&pSetAppl){
            XLstatus s=pSetAppl("UniVciRelay",0,cfg.channel[i].hwType,cfg.channel[i].hwIndex,cfg.channel[i].hwChannel,XL_BUS_TYPE_CAN);
            UI("[分配] 中继   -> 第 %d 路  (rc=%lu)",our,(unsigned long)s);
        }
    }
    UI("[分配] 完成。Vector Hardware Config 里右键 Refresh 看一眼。");
    UI("         CANape 如果开着，要重开才生效。");
}

static void PollOnce(void)
{
    XLevent ev[64]; VCI_CAN_OBJ ob[64]; static XLevent tx[64];
    DWORD last=0;
    if(!g_run) return;
    {
        unsigned cnt=0;
        if(pRecv(g_port,&cnt,20,ev)==XL_SUCCESS&&cnt){
            unsigned k,m=0;
            for(k=0;k<cnt;k++){
                if(ev[k].tag!=XL_RX) continue;
                if(ev[k].tagData.msg.flags&XL_ERRF){g_drop++;continue;}
                ZeroMemory(&ob[m],sizeof(ob[m]));
                ob[m].ID=ev[k].tagData.msg.id;
                ob[m].DataLen=(BYTE)(ev[k].tagData.msg.dlc>8?8:ev[k].tagData.msg.dlc);
                memcpy(ob[m].Data,ev[k].tagData.msg.data,8);
                ob[m].ExternFlag=(ev[k].tagData.msg.flags&2)?1:0;
                ob[m].RemoteFlag=(ev[k].tagData.msg.flags&1)?1:0;
                ob[m].TimeStamp=(DWORD)(ev[k].timeStamp/100000ULL); ob[m].TimeFlag=1;
                if(++m>=64)break;
            }
            if(m){DWORD s=pVTx(g_dev,0,0,ob,m); if(s<=m)g_toHw+=s;}
        }
        {DWORD got=0; if(0) got=pVRx(g_dev,0,0,ob,64,0);   /* 二分：先不碰桥的接收 */
         if(got>0&&got<=64){
            unsigned k,m=0; ZeroMemory(tx,sizeof(tx));
            for(k=0;k<got&&k<64;k++){
                tx[m].tag=XL_TX; tx[m].chanIndex=(BYTE)g_vidx;
                tx[m].tagData.msg.id=ob[k].ID;
                tx[m].tagData.msg.dlc=(WORD)(ob[k].DataLen>8?8:ob[k].DataLen);
                memcpy(tx[m].tagData.msg.data,ob[k].Data,8);
                tx[m].tagData.msg.flags=(WORD)((ob[k].ExternFlag?2:0)|(ob[k].RemoteFlag?1:0));
                m++;
            }
            if(m&&pXmit(g_port,g_mask,m,tx)==XL_SUCCESS)g_toVxl+=m;
         }}
        {DWORD now=GetTickCount();
         if((DWORD)(now-last)>=1000){
            char b[256]; last=now;
            sprintf(b,"虚拟->硬件 %lu 帧    硬件->虚拟 %lu 帧    丢错误帧 %lu",g_toHw,g_toVxl,g_drop);
            if(g_stat) SetWindowTextA(g_stat,b);
         }}

    }
}

static void DoStart(void)
{
    XLdriverConfig cfg; int v=0,i,seen=0,ok=0;
    int sel=(int)SendMessageA(g_vch,CB_GETCURSEL,0,0)+1;
    int be=(int)SendMessageA(g_be,CB_GETCURSEL,0,0);
    int bd=(int)SendMessageA(g_baud,CB_GETCURSEL,0,0);
    unsigned baud=(unsigned)strtoul(kBdVal[bd],0,10);
    char d[MAX_PATH],ini[MAX_PATH]; FILE*f;

    if(g_run){UI("[!] 已经在跑了");return;}

    SelfDir(d,MAX_PATH); sprintf(ini,"%s\\UniVciBridge.ini",d);
    f=fopen(ini,"wb");
    if(f){fprintf(f,"; 控制面板生成\r\nbackend=%s\r\nchannels=2\r\nxl_channel=1\r\n",kBeVal[be]);fclose(f);}

    if(!ListCh(&cfg,&v)) return;
    if(v<1){UI("[x] 没有虚拟通道");return;}
    for(i=0;i<(int)cfg.channelCount&&i<64;i++){
        if(!IsVirt(&cfg.channel[i])) continue;
        if(++seen==sel){g_mask=cfg.channel[i].channelMask;g_vidx=cfg.channel[i].channelIndex;ok=1;break;}
    }
    if(!ok){UI("[x] 没有第 %d 个虚拟通道",sel);return;}

    if(pSetAppl) pSetAppl("UniVciRelay",0,cfg.channel[g_vidx].hwType,cfg.channel[g_vidx].hwIndex,cfg.channel[g_vidx].hwChannel,XL_BUS_TYPE_CAN);
    {XLaccess perm=g_mask;
     XLstatus r=pOpenPort(&g_port,"UniVciRelay",g_mask,&perm,8192,XL_IVER,XL_BUS_TYPE_CAN);
     UI("[中继] xlOpenPort mask=0x%I64X -> %lu perm=0x%I64X",(unsigned long long)g_mask,(unsigned long)r,(unsigned long long)perm);
     if(r!=XL_SUCCESS||!perm){UI("[x] 拿不到权限 —— 换一路虚拟通道试试");return;}}
    UI("[中继] 设波特率 %u -> %lu",baud,(unsigned long)pSetBaud(g_port,g_mask,baud));
    UI("[中继] 激活 -> %lu",(unsigned long)pAct(g_port,g_mask,XL_BUS_TYPE_CAN,0));

    g_dev = DevFromCombo();
    if(!LoadBrg()){pClosePort(g_port);g_port=0;return;}
    {DWORD r = g_devOpen ? 1 : pVOpen(g_dev,0,0);
     if(r==1) g_devOpen=1;
     UI("[中继] 盒子 OpenDevice -> %lu%s",(unsigned long)r,g_devOpen?" (复用已打开的句柄)":"");
     if(r!=1){UI("[x] 盒子打不开（没插/被占用）");pClosePort(g_port);g_port=0;return;}}
    if(g_devStarted){ UI("[中继] 通道已启动过，直接复用"); }
    else {
    {VCI_INIT_CONFIG_EU ic; ZeroMemory(&ic,sizeof(ic));
     DWORD ri, rs;
     ic.AccMask=0xFFFFFFFFu; ic.Filter=1; ic.Timing0=0; ic.Timing1=0x1C;
     ri=pVInit(g_dev,0,0,&ic);
     UI("[中继] InitCAN -> %lu",(unsigned long)ri);
     if(ri==0){
        UI("[x] 初始化失败 —— 型号不对或盒子被占用。");
        UI("    换【盒子型号】再试：21 / 20 / 4 / 3 逐个试");
        if(pVClose) pVClose(g_dev,0);
        if(pClosePort) pClosePort(g_port);
        g_port=0;
        return;                 /* 关键：绝不带着坏状态起线程，否则宿主崩溃 */
     }
     rs=pVStart(g_dev,0,0);
     UI("[中继] StartCAN -> %lu",(unsigned long)rs);
     if(rs==0){
        UI("[x] StartCAN 失败，停止");
        if(pVClose) pVClose(g_dev,0);
        if(pClosePort) pClosePort(g_port);
        g_port=0; return;
     }}
    g_devStarted=1;
    }

    g_toHw=g_toVxl=g_drop=0; Sleep(200); g_run=1;
    SetTimer(g_main,1,5,0);   /* 界面线程定时轮询：Vector API 不能跨线程 */
    UI("[中继] 已启动 —— CANape 请用【另一路】虚拟通道");
    EnableWindow(GetDlgItem(g_main,ID_START),FALSE);
    EnableWindow(GetDlgItem(g_main,ID_STOP),TRUE);
}

static unsigned g_txId=0x123;
static void DoTx(void)
{
    XLevent tx; 
    if(!g_port){ UI("[发帧] 中继还没启动"); return; }
    ZeroMemory(&tx,sizeof(tx));
    tx.tag=XL_TX; tx.chanIndex=(BYTE)g_vidx;
    tx.tagData.msg.id=g_txId++;
    tx.tagData.msg.dlc=8;
    tx.tagData.msg.data[0]=(BYTE)(g_txId&0xFF);
    tx.tagData.msg.data[1]=(BYTE)((g_txId>>8)&0xFF);
    tx.tagData.msg.data[2]=0xAA; tx.tagData.msg.data[3]=0x55;
    { XLstatus r=pXmit(g_port,g_mask,1,&tx);
      UI("[发帧] ID=0x%03X -> %lu %s",(unsigned)g_txId-1,(unsigned long)r,r==0?"已发到虚拟通道":"失败"); }
}

static void DoStop(void)
{
    if(g_run){g_run=0;} if(g_main) KillTimer(g_main,1);
    if(g_port){if(pDeact)pDeact(g_port,g_mask); if(pClosePort)pClosePort(g_port); g_port=0;}
    UI("[中继] 已停止，虚拟通道已释放（盒子保持打开，避免反复开关）");
    if(g_main){
        EnableWindow(GetDlgItem(g_main,ID_START),TRUE);
        EnableWindow(GetDlgItem(g_main,ID_STOP),FALSE);
    }
}

static void DoRestore(void)
{
    char d[MAX_PATH],ini[MAX_PATH];
    DoStop();
    SelfDir(d,MAX_PATH); sprintf(ini,"%s\\UniVciBridge.ini",d);
    DeleteFileA(ini);
    if(g_devOpen&&g_hBrg&&pVClose){ pVClose(g_dev,0); g_devOpen=0; UI("[恢复] 盒子已关闭"); }
    if(g_stat) SetWindowTextA(g_stat,"");
    UI("[恢复] 已停中继 / 关设备 / 释放通道 / 删 ini —— 回到初始状态");
}

static LRESULT CALLBACK WP(HWND h,UINT m,WPARAM w,LPARAM l)
{
    switch(m){
    case WM_CREATE:
        g_main=h;
        CreateWindowA("STATIC","真实硬件",WS_CHILD|WS_VISIBLE,14,8,150,16,h,0,0,0);
        CreateWindowA("STATIC","硬件通道",WS_CHILD|WS_VISIBLE,174,8,120,16,h,0,0,0);
        CreateWindowA("STATIC","波特率",WS_CHILD|WS_VISIBLE,304,8,90,16,h,0,0,0);
        CreateWindowA("STATIC","盒子型号",WS_CHILD|WS_VISIBLE,404,8,100,16,h,0,0,0);
        g_be  =CreateWindowA("COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,14,26,150,200,h,(HMENU)ID_BE,0,0);
        g_hw  =CreateWindowA("COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,174,26,120,200,h,(HMENU)ID_HW,0,0);
        g_baud=CreateWindowA("COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,304,26,90,200,h,(HMENU)ID_BAUD,0,0);
        g_devc=CreateWindowA("COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,404,26,100,200,h,(HMENU)ID_DEV,0,0);
        { const char*dt[]={"4 - USBCAN-II","21 - USBCAN-2E-U","20 - USBCAN-E-U","3 - USBCAN-I","34 - FD-800U","31 - FD-200U"};
          int di; for(di=0;di<6;di++) SendMessageA(g_devc,CB_ADDSTRING,0,(LPARAM)dt[di]);
          SendMessageA(g_devc,CB_SETCURSEL,0,0); }   /* 默认 USBCAN-II (4) */
        {int i;
         for(i=0;i<6;i++) SendMessageA(g_be,CB_ADDSTRING,0,(LPARAM)kBeTxt[i]);
         SendMessageA(g_be,CB_SETCURSEL,1,0);
         SendMessageA(g_hw,CB_ADDSTRING,0,(LPARAM)"CAN1");
         SendMessageA(g_hw,CB_ADDSTRING,0,(LPARAM)"CAN2");
         SendMessageA(g_hw,CB_SETCURSEL,0,0);
         for(i=0;i<6;i++) SendMessageA(g_baud,CB_ADDSTRING,0,(LPARAM)kBdTxt[i]);
         SendMessageA(g_baud,CB_SETCURSEL,3,0);}
        CreateWindowA("STATIC","中继占这一路",WS_CHILD|WS_VISIBLE,14,56,240,16,h,0,0,0);
        CreateWindowA("STATIC","CANape 用这一路",WS_CHILD|WS_VISIBLE,304,56,150,16,h,0,0,0);
        g_vch=CreateWindowA("COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,14,74,240,200,h,(HMENU)ID_VCH,0,0);
        g_ape=CreateWindowA("COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,304,74,100,200,h,(HMENU)ID_APE,0,0);
        {int i;char b[32];
         for(i=1;i<=7;i++){sprintf(b,"第 %d 路",i);
            SendMessageA(g_vch,CB_ADDSTRING,0,(LPARAM)b);
            SendMessageA(g_ape,CB_ADDSTRING,0,(LPARAM)b);}
         SendMessageA(g_vch,CB_SETCURSEL,1,0);
         SendMessageA(g_ape,CB_SETCURSEL,0,0);}
        CreateWindowA("BUTTON","一键分配",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,14,104,110,30,h,(HMENU)ID_ASSIGN,0,0);
        CreateWindowA("BUTTON","启动中继",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,134,104,110,30,h,(HMENU)ID_START,0,0);
        CreateWindowA("BUTTON","停止",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,254,104,70,30,h,(HMENU)ID_STOP,0,0);
        CreateWindowA("BUTTON","诊断",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,334,104,70,30,h,(HMENU)ID_DIAG,0,0);
        CreateWindowA("BUTTON","发测试帧",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,414,104,90,30,h,(HMENU)ID_TX,0,0);
        CreateWindowA("BUTTON","一键恢复",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,512,104,90,30,h,(HMENU)ID_RESTORE,0,0);
        EnableWindow(GetDlgItem(h,ID_STOP),FALSE);
        g_stat=CreateWindowA("STATIC","",WS_CHILD|WS_VISIBLE|SS_SUNKEN,14,142,490,20,h,(HMENU)ID_STAT,0,0);
        g_log=CreateWindowA("EDIT","",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_BORDER,
                            14,168,490,300,h,(HMENU)ID_LOG,0,0);
        UI("准备就绪。顺序：诊断 → 一键分配 → 启动中继 → 开 CANape");
        UI("提示：CANape 里显示 Virtual Channel，不是 VN1630（驱动命名，功能不受影响）");
        return 0;
    case WM_COMMAND:
        switch(LOWORD(w)){
        case ID_ASSIGN: DoAssign(); return 0;
        case ID_START:  DoStart();  return 0;
        case ID_STOP:   DoStop();   return 0;
        case ID_DIAG:   DoDiag();   return 0;
        case ID_RESTORE:DoRestore();return 0;
        case ID_TX:     DoTx();     return 0;
        }
        return 0;
    case WM_TIMER: if(w==1) PollOnce(); return 0;
    case WM_CLOSE: DoStop(); DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h,m,w,l);
}

int WINAPI WinMain(HINSTANCE hi,HINSTANCE hp,LPSTR cl,int sh)
{
    WNDCLASSA wc; HWND h; MSG msg; (void)hp;(void)cl;
    InitializeCriticalSection(&g_cs);
    ZeroMemory(&wc,sizeof(wc));
    wc.lpfnWndProc=WP; wc.hInstance=hi; wc.hCursor=LoadCursor(0,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    wc.lpszClassName="VxlRelayGui3";
    RegisterClassA(&wc);
    h=CreateWindowA("VxlRelayGui3","反向桥接 —— 真实盒子 -> Vector 虚拟通道",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,630,530,0,0,hi,0);
    if(!h) return 1;
    ShowWindow(h,sh); UpdateWindow(h);
    while(GetMessage(&msg,0,0,0)>0){TranslateMessage(&msg);DispatchMessage(&msg);}
    return 0;
}
