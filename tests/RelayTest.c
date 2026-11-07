/*
 * RelayTest.c —— 验证虚拟通道中继的收发（32 位，只用真 Vector 驱动）
 *
 *   端口 A：Virtual Channel（中继占另一条，帧经它转发到周立功盒子）
 *   端口 B：VN1630A Channel 1（真硬件）
 *
 *   测试一：A 发 → 中继 → 周立功 → 总线 → VN1630A → B 收
 *   测试二：B 发 → 总线 → 周立功 → 中继 → A 收
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
typedef struct { DWORD id; WORD flags,dlc; u64 res1; BYTE data[8]; u64 res2; } XLcanMsg;
typedef union  { XLcanMsg msg; BYTE raw[32]; } XLtagData;
typedef struct { BYTE tag,chanIndex; WORD transId,portHandle; BYTE flags,reserved; u64 timeStamp; XLtagData tagData; } XLevent;
#pragma pack(pop)
#define XLTX 10
#define XLRX 1

typedef XLstatus(__stdcall*PF_open)(void);
typedef XLstatus(__stdcall*PF_port)(XLportHandle*,const char*,XLaccess,XLaccess*,unsigned,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_baud)(XLportHandle,XLaccess,unsigned);
typedef XLstatus(__stdcall*PF_act)(XLportHandle,XLaccess,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_recv)(XLportHandle,unsigned*,XLevent*);
typedef XLstatus(__stdcall*PF_xmit)(XLportHandle,XLaccess,unsigned,void*);
typedef XLstatus(__stdcall*PF_mode)(XLportHandle,XLaccess,unsigned,unsigned);
typedef XLstatus(__stdcall*PF_notif)(XLportHandle,HANDLE*,int);

static HMODULE H; static PF_open pOpen; static PF_port pPort; static PF_baud pBaud;
static PF_act pAct; static PF_recv pRecv; static PF_xmit pXmit;
static PF_mode pMode; static PF_notif pNotif;

static int OpenPort(XLportHandle*ph, const char*name, XLaccess mask, unsigned*outIdx)
{
    XLaccess perm = mask;
    XLstatus r = pPort(ph, name, mask, &perm, 8192, 3, 1);
    if (r != 0 || perm == 0) return 0;
    { int i = 0; while (i < 32 && !((mask>>i)&1)) i++; if (outIdx) *outIdx = i; }
    return 1;
}

static int Send(XLportHandle h, XLaccess m, unsigned idx, unsigned id, unsigned seq)
{
    XLevent e; ZeroMemory(&e,sizeof(e));
    e.tag = XLTX;
    (void)idx;   /* chanIndex 留 0：由 mask 选通道 */
    e.tagData.msg.id = id;
    e.tagData.msg.dlc = 8;
    e.tagData.msg.data[0] = (BYTE)seq;
    e.tagData.msg.data[1] = 0xAA; e.tagData.msg.data[2] = 0x55; e.tagData.msg.data[3] = 0x11;
    e.tagData.msg.data[4] = 0x22; e.tagData.msg.data[5] = 0x33; e.tagData.msg.data[6] = 0x44; e.tagData.msg.data[7] = 0x55;
    return pXmit(h, m, 1, &e) == 0;
}

static int RecvCount(XLportHandle h, unsigned id, int ms)
{
    DWORD t0 = GetTickCount(); int n = 0;
    while ((DWORD)(GetTickCount()-t0) < (DWORD)ms) {
        XLevent ev[32]; unsigned cnt = 32, k;
        if (pRecv(h, &cnt, ev) == 0 && cnt > 0 && cnt <= 32) {
            for (k = 0; k < cnt; k++) {
                if (ev[k].tag != XLRX) continue;
                if ((ev[k].tagData.msg.id & 0x1FFFFFFF) == id) n++;
            }
        }
        Sleep(5);
    }
    return n;
}

int main(void)
{
    XLportHandle pa = 0, pb = 0;
    XLaccess maskA = 0, maskB = 0x1;   /* B = VN1630A Channel 1 */
    unsigned idxA = 0, idxB = 0;
    XLaccess tryMasks[2] = { 0x20, 0x40 };
    int i, sentA = 0, sentB = 0;

    setvbuf(stdout, NULL, _IONBF, 0);   /* 关缓冲：崩了也能看到走到哪 */
    printf("============================================================\n");
    printf("  虚拟通道中继 收发验证\n");
    printf("============================================================\n\n");

    H = LoadLibraryA("vxlapi.dll");
    if (!H) { printf("[x] vxlapi.dll 加载失败\n"); return 1; }
    pOpen = (PF_open)GetProcAddress(H,"xlOpenDriver");
    pPort = (PF_port)GetProcAddress(H,"xlOpenPort");
    pBaud = (PF_baud)GetProcAddress(H,"xlCanSetChannelBitrate");
    pAct  = (PF_act) GetProcAddress(H,"xlActivateChannel");
    pRecv = (PF_recv)GetProcAddress(H,"xlReceive");
    pXmit = (PF_xmit)GetProcAddress(H,"xlCanTransmit");
    pMode = (PF_mode)GetProcAddress(H,"xlCanSetChannelMode");
    pNotif= (PF_notif)GetProcAddress(H,"xlSetNotification");
    printf("  函数指针: open=%p port=%p baud=%p act=%p recv=%p xmit=%p\n",
           (void*)pOpen,(void*)pPort,(void*)pBaud,(void*)pAct,(void*)pRecv,(void*)pXmit);
    if(!pXmit){ printf("[x] xlCanTransmit 没找到\n"); return 1; }
    printf("xlOpenDriver -> %ld\n", (long)pOpen());

    /* 端口 B：VN1630A Channel 1 */
    if (!OpenPort(&pb, "RT_B", maskB, &idxB)) { printf("[x] 打不开 VN1630A Channel 1\n"); return 1; }
    pBaud(pb, maskB, 500000);
    if (pMode) { printf("  xlCanSetChannelMode -> %ld\n",(long)pMode(pb,maskB,0,0)); }
    if (pNotif){ HANDLE ev=0; pNotif(pb,&ev,1); }
    if (pAct(pb, maskB, 1, 0) != 0) { printf("[x] VN1630A 激活失败（被占用？）\n"); return 1; }
    printf("[ok] VN1630A Channel 1 已激活\n");

    /* 端口 A：找一条空闲的虚拟通道（中继占了另一条） */
    for (i = 0; i < 2; i++) {
        if (OpenPort(&pa, "RT_A", tryMasks[i], &idxA)) { maskA = tryMasks[i]; break; }
    }
    if (!maskA) { printf("[x] 两条虚拟通道都拿不到（中继占了？先停中继）\n"); return 1; }
    pBaud(pa, maskA, 500000);
    if (pMode) { printf("  xlCanSetChannelMode -> %ld\n",(long)pMode(pa,maskA,0,0)); }
    if (pNotif){ HANDLE ev=0; pNotif(pa,&ev,1); }
    if (pAct(pa, maskA, 1, 0) != 0) { printf("[x] 虚拟通道激活失败\n"); return 1; }
    printf("[ok] 虚拟通道 mask=0x%I64X（chanIndex=%u）已激活\n\n", (unsigned long long)maskA, idxA);

    /* 测试一：虚拟通道发 → 期望 VN1630A 收 */
    { XLevent t; ZeroMemory(&t,sizeof(t)); t.tag=XLTX; t.chanIndex=(BYTE)idxA;
      printf("  试调 xmit(count=0) ...\n");
      printf("  xmit(count=0) -> %ld\n", (long)pXmit(pa, maskA, 0, &t));
      printf("  试调 xmit(count=0) 完成，没崩\n"); }
    printf("（跳过测试一：真驱动在虚拟通道上发送会崩，先做测试二）\n\n");
    for (i = 0; i < 0; i++) { }
    printf("  发出 %d 帧\n", sentA);
    { int got = 0;
      printf("  VN1630A 收到 %d 帧  %s\n\n", got, got ? "✅ 通" : "✗ 不通"); }

    /* 测试二：VN1630A 发 → 期望虚拟通道收 */
    printf("测试二：VN1630A 发 10 帧 0x322 ...\n");
    for (i = 0; i < 10; i++) { if (Send(pb, maskB, idxB, 0x322, (unsigned)i)) sentB++; Sleep(30); }
    printf("  发出 %d 帧\n", sentB);
    { int got = RecvCount(pa, 0x322, 1500);
      printf("  虚拟通道收到 %d 帧  %s\n\n", got, got ? "✅ 通" : "✗ 不通"); }

    printf("============================================================\n");
    return 0;
}
