/*
 * XferTest2.c —— 反向验证（32 位）
 *   发送端：真 Vector 驱动 → VN1630A → 总线
 *   接收端：我们的门面 → 周立功盒子（泵线程收帧）
 * 收到帧 = 门面的收帧通路成立。
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

typedef XLstatus(__stdcall*PF_open)(void);
typedef XLstatus(__stdcall*PF_port)(XLportHandle*,const char*,XLaccess,XLaccess*,unsigned,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_baud)(XLportHandle,XLaccess,unsigned);
typedef XLstatus(__stdcall*PF_act)(XLportHandle,XLaccess,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_recv)(XLportHandle,unsigned*,Ev*);
typedef XLstatus(__stdcall*PF_xmit)(XLportHandle,XLaccess,unsigned,void*);
typedef XLstatus(__stdcall*PF_notif)(XLportHandle,HANDLE*,int);

typedef struct { HMODULE h; PF_open open; PF_port port; PF_baud baud; PF_act act;
                 PF_recv recv; PF_xmit xmit; PF_notif notif; } API;

static int Load(API*a,const char*f)
{
    ZeroMemory(a,sizeof(*a)); a->h=LoadLibraryA(f); if(!a->h) return 0;
    a->open=(PF_open)GetProcAddress(a->h,"xlOpenDriver");
    a->port=(PF_port)GetProcAddress(a->h,"xlOpenPort");
    a->baud=(PF_baud)GetProcAddress(a->h,"xlCanSetChannelBitrate");
    a->act =(PF_act) GetProcAddress(a->h,"xlActivateChannel");
    a->recv=(PF_recv)GetProcAddress(a->h,"xlReceive");
    a->xmit=(PF_xmit)GetProcAddress(a->h,"xlCanTransmit");
    a->notif=(PF_notif)GetProcAddress(a->h,"xlSetNotification");
    return a->open&&a->port&&a->act;
}

static unsigned mid(const Ev*e){ unsigned v; memcpy(&v,&e->d[0],4); return v&0x1FFFFFFF; }

int main(void)
{
    API real,fac;
    XLportHandle pa=0,pb=0; XLaccess sa=0,sb=0;
    int i,sent=0,got=0; DWORD t0;

    printf("============================================================\n");
    printf("  反向验证：VN1630A 发  ->  门面(周立功) 收\n");
    printf("============================================================\n\n");

    if(!Load(&real,"xlreal.dll")){ printf("[x] 加载 xlreal.dll 失败\n"); return 1; }
    if(!Load(&fac,"vxlapi.dll")){ printf("[x] 加载 vxlapi.dll 失败\n"); return 1; }
    printf("[ok] 两个 DLL 已加载\n\n");

    if(real.open()!=0){ printf("[x] 真驱动 open 失败\n"); return 1; }
    sa=0x1;
    if(real.port(&pa,"T2Tx",sa,&sa,8192,3,1)!=0){ printf("[x] 真驱动 openPort 失败\n"); return 1; }
    real.baud(pa,sa,500000);
    if(real.act(pa,sa,1,0)!=0){ printf("[x] VN1630A 激活失败\n"); return 1; }
    printf("[ok] VN1630A 已激活（发送端）\n");

    if(fac.open()!=0){ printf("[x] 门面 open 失败\n"); return 1; }
    sb=0x1;
    if(fac.port(&pb,"T2Rx",sb,&sb,8192,3,1)!=0){ printf("[x] 门面 openPort 失败\n"); return 1; }
    fac.baud(pb,sb,500000);
    if(fac.act(pb,sb,1,0)!=0){ printf("[x] 门面激活失败（周立功）\n"); return 1; }
    { HANDLE ev=NULL; if(fac.notif) fac.notif(pb,&ev,1); printf("[ok] 门面已激活（接收端）事件=%p\n",(void*)ev); }

    printf("\nVN1630A 发 20 帧 ID=0x321 ...\n");
    for(i=0;i<20;i++){
        Ev e; ZeroMemory(&e,sizeof(e));
        e.tag=TAG_TX;
        memcpy(&e.d[0],"\x21\x03\x00\x00",4);
        memcpy(&e.d[6],"\x08\x00",2);
        e.d[16]=(BYTE)i; e.d[17]=0x11; e.d[18]=0x22; e.d[19]=0x33;
        e.d[20]=0x44; e.d[21]=0x55; e.d[22]=0x66; e.d[23]=(BYTE)(i+1);
        if(real.xmit(pa,sa,1,&e)==0) sent++;
        Sleep(20);
    }
    printf("  发送成功 %d / 20\n\n",sent);

    printf("门面侧接收 3 秒 ...\n");
    t0=GetTickCount();
    while((DWORD)(GetTickCount()-t0)<3000){
        Ev ev[16]; unsigned n=16,k;
        if(fac.recv(pb,&n,ev)==0&&n>0){
            for(k=0;k<n&&k<16;k++){ if(ev[k].tag!=TAG_RX) continue; if(mid(&ev[k])==0x321) got++; }
        }
        Sleep(5);
    }
    printf("\n════════════════════════════════════════\n");
    printf("  发出 %d 帧    门面收到 %d 帧\n", sent, got);
    printf("════════════════════════════════════════\n");
    if(got>0) printf("  ✅ 收帧通路成立\n");
    else      printf("  ✗ 没收到：总线没通、波特率不一致，或泵线程没工作\n");
    return got>0?0:2;
}
