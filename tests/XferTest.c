/*
 * XferTest.c —— 收发验证（32 位）
 *
 *   发送端：我们的门面 vxlapi.dll  → 周立功盒子 → CAN 总线
 *   接收端：真 Vector 驱动         → VN1630A    → 同一条总线
 *
 * 两个盒子必须用双绞线连在一起（CAN_H 对 CAN_H，CAN_L 对 CAN_L）。
 * 收到帧 = 整条链路成立；收不到 = 线没接或波特率不一致。
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
typedef struct { BYTE tag,chanIndex; WORD transId,portHandle; BYTE flags,reserved; u64 timeStamp; BYTE d[32]; } Ev;
#pragma pack(pop)

#define TAG_TX 10
#define TAG_RX 1

typedef XLstatus(__stdcall*PF_openDrv)(void);
typedef XLstatus(__stdcall*PF_cfg)(void*);
typedef XLstatus(__stdcall*PF_openPort)(XLportHandle*,const char*,XLaccess,XLaccess*,unsigned,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_baud)(XLportHandle,XLaccess,unsigned);
typedef XLstatus(__stdcall*PF_act)(XLportHandle,XLaccess,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_recv)(XLportHandle,unsigned*,Ev*);
typedef XLstatus(__stdcall*PF_xmit)(XLportHandle,XLaccess,unsigned,void*);
typedef XLstatus(__stdcall*PF_closePort)(XLportHandle);
typedef XLstatus(__stdcall*PF_closeDrv)(void);

typedef struct {
    HMODULE h; PF_openDrv open; PF_cfg cfg; PF_openPort port; PF_baud baud;
    PF_act act; PF_recv recv; PF_xmit xmit; PF_closePort cp; PF_closeDrv cd;
} API;

static int Load(API*a,const char*file)
{
    ZeroMemory(a,sizeof(*a));
    a->h=LoadLibraryA(file);
    if(!a->h) return 0;
    a->open =(PF_openDrv) GetProcAddress(a->h,"xlOpenDriver");
    a->cfg  =(PF_cfg)     GetProcAddress(a->h,"xlGetDriverConfig");
    a->port =(PF_openPort)GetProcAddress(a->h,"xlOpenPort");
    a->baud =(PF_baud)    GetProcAddress(a->h,"xlCanSetChannelBitrate");
    a->act  =(PF_act)     GetProcAddress(a->h,"xlActivateChannel");
    a->recv =(PF_recv)    GetProcAddress(a->h,"xlReceive");
    a->xmit =(PF_xmit)    GetProcAddress(a->h,"xlCanTransmit");
    a->cp   =(PF_closePort)GetProcAddress(a->h,"xlClosePort");
    a->cd   =(PF_closeDrv)GetProcAddress(a->h,"xlCloseDriver");
    return a->open&&a->port&&a->act&&a->xmit;
}

static unsigned msgId(const Ev*e)
{
    unsigned id; memcpy(&id,&e->d[0],4);   /* XLcanMsg 的第一个 dword 就是 id */
    return id & 0x1FFFFFFF;
}

int main(void)
{
    API me,zlg;   /* me = 真 Vector（收）  zlg = 我们的门面（发） */
    XLportHandle pa=0,pb=0; XLaccess sa=0,sb=0;
    int i,sent=0,got=0;
    DWORD t0;

    printf("============================================================\n");
    printf("  收发验证：门面下用周立功发  →  VN1630A 收\n");
    printf("============================================================\n\n");

    if(!Load(&me,"xlreal.dll")){ printf("[x] 加载 xlreal.dll（真 Vector 驱动副本）失败\n"); return 1; }
    if(!Load(&zlg,"vxlapi.dll")){ printf("[x] 加载 vxlapi.dll（我们的门面）失败\n"); return 1; }
    printf("[ok] 两个 DLL 都已加载\n\n");

    /* 接收端：真 Vector，VN1630A Channel 1 = mask 0x1 */
    if(me.open()!=0){ printf("[x] 真驱动 xlOpenDriver 失败\n"); return 1; }
    sa=0x1;
    if(me.port(&pa,"XferRx",sa,&sa,8192,3,1)!=0){ printf("[x] 真驱动 xlOpenPort 失败\n"); return 1; }
    me.baud(pa,sa,500000);
    if(me.act(pa,sa,1,0)!=0){ printf("[x] 真驱动激活失败（VN1630A 被占用？）\n"); return 1; }
    printf("[ok] VN1630A Channel 1 已激活（500k）\n");

    /* 发送端：我们的门面，周立功 */
    if(zlg.open()!=0){ printf("[x] 门面 xlOpenDriver 失败\n"); return 1; }
    sb=0x1;
    if(zlg.port(&pb,"XferTx",sb,&sb,8192,3,1)!=0){ printf("[x] 门面 xlOpenPort 失败\n"); return 1; }
    zlg.baud(pb,sb,500000);
    if(zlg.act(pb,sb,1,0)!=0){ printf("[x] 门面激活失败（周立功没打开？）\n"); return 1; }
    printf("[ok] 周立功盒子已激活（500k）\n\n");

    /* 发 20 帧 0x321 */
    printf("发送 20 帧 ID=0x321 ...\n");
    for(i=0;i<20;i++){
        Ev e; XLstatus r;
        ZeroMemory(&e,sizeof(e));
        e.tag=TAG_TX; e.chanIndex=0;
        memcpy(&e.d[0],"\x21\x03\x00\x00",4);      /* id 小端 0x00000321 */
        memcpy(&e.d[6],"\x08\x00",2);              /* dlc = 8 (flags 后面两位) */
        e.d[16]=(BYTE)i; e.d[17]=0xAA; e.d[18]=0x55; e.d[19]=0x11;
        e.d[20]=0x22; e.d[21]=0x33; e.d[22]=0x44; e.d[23]=0x55;
        r=zlg.xmit(pb,sb,1,&e);
        if(r==0) sent++;
        Sleep(20);
    }
    printf("  发送返回成功 %d / 20\n\n",sent);

    /* 收 2 秒 */
    printf("VN1630A 侧接收 2 秒 ...\n");
    t0=GetTickCount();
    while((DWORD)(GetTickCount()-t0)<2000){
        Ev ev[32]; unsigned n=32; unsigned k;
        if(me.recv(pa,&n,ev)==0&&n>0){
            for(k=0;k<n&&k<32;k++){
                if(ev[k].tag!=TAG_RX) continue;
                if(msgId(&ev[k])==0x321) got++;
            }
        }
        Sleep(5);
    }
    printf("\n════════════════════════════════════════\n");
    printf("  发出 %d 帧    VN1630A 收到 %d 帧\n", sent, got);
    printf("════════════════════════════════════════\n");
    if(got>0) printf("  ✅ 链路成立：周立功的帧真的上了总线，VN1630A 收到了\n");
    else      printf("  ✗ 没收到：两个盒子没接同一条总线，或波特率不一致\n");

    me.cp(pa); me.cd();
    zlg.cp(pb); zlg.cd();
    return got>0?0:2;
}
