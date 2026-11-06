/*
 * VxlHelper.c —— 32 位辅助进程
 *
 * 64 位门面无法加载 32 位的周立功 DLL，所以由它代劳：
 *   打开 UniVciBridge → 打开周立功 → 收发
 *   收到的帧写共享内存，要发的帧从共享内存读
 *
 * 用法（由门面自动拉起）：
 *   VxlHelper.exe <设备型号> <通道> <波特率>
 *   VxlHelper.exe 4 0 500000
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "VxlShm.h"

typedef DWORD(__stdcall*PF_vOpen)(DWORD,DWORD,DWORD);
typedef DWORD(__stdcall*PF_vClose)(DWORD,DWORD);
typedef DWORD(__stdcall*PF_vInit)(DWORD,DWORD,DWORD,void*);
typedef DWORD(__stdcall*PF_vStart)(DWORD,DWORD,DWORD);
typedef DWORD(__stdcall*PF_vTx)(DWORD,DWORD,DWORD,void*,DWORD);
typedef DWORD(__stdcall*PF_vRx)(DWORD,DWORD,DWORD,void*,DWORD,int);

#pragma pack(push,1)
typedef struct { DWORD ID,TimeStamp; BYTE TimeFlag,SendType,RemoteFlag,ExternFlag,DataLen,Data[8],Reserved[3]; } VCI_CAN_OBJ;
typedef struct { DWORD AccCode,AccMask,Reserved; BYTE Filter,Timing0,Timing1,Mode; } INITCFG;
#pragma pack(pop)

static FILE *g_log=NULL;
static void L(const char *fmt,...)
{
    va_list ap; char p[MAX_PATH];
    if(!g_log){
        GetModuleFileNameA(NULL,p,MAX_PATH);
        { char*s=strrchr(p,'\\'); if(s) strcpy(s,"\\VxlHelper.log"); }
        g_log=fopen(p,"a");
    }
    if(!g_log) return;
    va_start(ap,fmt); vfprintf(g_log,fmt,ap); va_end(ap);
    fprintf(g_log,"\n"); fflush(g_log);
}

int main(int argc,char**argv)
{
    DWORD dev = (argc>=2)?(DWORD)strtoul(argv[1],0,10):4;
    DWORD ch  = (argc>=3)?(DWORD)strtoul(argv[2],0,10):0;
    DWORD baud= (argc>=4)?(DWORD)strtoul(argv[3],0,10):500000u;
    HANDLE hMap,evRx,evTx;
    VxlShm *shm;
    HMODULE hb;
    PF_vOpen pO; PF_vClose pC; PF_vInit pI; PF_vStart pS; PF_vTx pT; PF_vRx pR;
    INITCFG ic;
    char dll[MAX_PATH];

    L("=== VxlHelper 启动 dev=%lu ch=%lu baud=%lu ===",(unsigned long)dev,(unsigned long)ch,(unsigned long)baud);

    hMap=CreateFileMappingA(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,sizeof(VxlShm),VXL_SHM_NAME);
    if(!hMap){ L("[x] 共享内存创建失败 %lu",GetLastError()); return 1; }
    shm=(VxlShm*)MapViewOfFile(hMap,FILE_MAP_ALL_ACCESS,0,0,sizeof(VxlShm));
    if(!shm){ L("[x] 映射失败"); return 1; }
    ZeroMemory(shm,sizeof(VxlShm));
    shm->magic=VXL_SHM_MAGIC;

    evRx=CreateEventA(NULL,FALSE,FALSE,VXL_EVT_RX);
    evTx=CreateEventA(NULL,FALSE,FALSE,VXL_EVT_TX);

    GetModuleFileNameA(NULL,dll,MAX_PATH);
    { char*s=strrchr(dll,'\\'); if(s) strcpy(s,"\\UniVciBridge.dll"); }
    hb=LoadLibraryA(dll);
    if(!hb){ char p2[MAX_PATH]; GetModuleFileNameA(NULL,p2,MAX_PATH);
             { char*s=strrchr(p2,'\\'); if(s) strcpy(s,"\\USBCAN.dll"); } hb=LoadLibraryA(p2); }
    if(!hb){ L("[x] 桥加载失败"); shm->err=1; return 1; }

    pO=(PF_vOpen) GetProcAddress(hb,"VCI_OpenDevice");
    pC=(PF_vClose)GetProcAddress(hb,"VCI_CloseDevice");
    pI=(PF_vInit) GetProcAddress(hb,"VCI_InitCAN");
    pS=(PF_vStart)GetProcAddress(hb,"VCI_StartCAN");
    pT=(PF_vTx)   GetProcAddress(hb,"VCI_Transmit");
    pR=(PF_vRx)   GetProcAddress(hb,"VCI_Receive");
    if(!pO||!pI||!pS||!pT||!pR){ L("[x] 桥缺导出"); shm->err=2; return 1; }

    { DWORD r=pO(dev,0,0); L("VCI_OpenDevice(%lu) -> %lu",(unsigned long)dev,(unsigned long)r);
      if(r!=1){ shm->err=3; return 1; } }
    ZeroMemory(&ic,sizeof(ic));
    ic.AccMask=0xFFFFFFFFu; ic.Filter=1; ic.Timing0=0; ic.Timing1=(BYTE)((baud==250000)?0x1C:0x1C);
    { DWORD r=pI(dev,0,ch,&ic); L("VCI_InitCAN -> %lu",(unsigned long)r); if(r!=1){ shm->err=4; return 1; } }
    { DWORD r=pS(dev,0,ch);     L("VCI_StartCAN -> %lu",(unsigned long)r); if(r!=1){ shm->err=5; return 1; } }

    shm->ready=1;
    L("[ok] 就绪，开始搬运");

    for(;;){
        VCI_CAN_OBJ ob[64];
        DWORD i;

        if(shm->quit) { L("收到退出信号"); break; }

        /* 门面 → 硬件 */
        if(shm->txTail!=shm->txHead){
            int n=0;
            while(shm->txTail!=shm->txHead && n<64){
                VxlFrame*f=&shm->tx[shm->txTail % VXL_RING];
                ZeroMemory(&ob[n],sizeof(ob[n]));
                ob[n].ID=f->id;
                ob[n].DataLen=(BYTE)(f->dlc>8?8:f->dlc);
                memcpy(ob[n].Data,f->data,8);
                ob[n].ExternFlag=(BYTE)((f->flags&2)?1:0);
                ob[n].RemoteFlag=(BYTE)((f->flags&1)?1:0);
                shm->txTail++; n++;
            }
            if(n) pT(dev,0,ch,ob,(DWORD)n);
        }

        /* 硬件 → 门面 */
        { DWORD got=pR(dev,0,ch,ob,64,1);
          if(got>0 && got<=64){
              for(i=0;i<got;i++){
                  VxlFrame*f=&shm->rx[shm->rxHead % VXL_RING];
                  f->id=ob[i].ID;
                  f->timeStamp=ob[i].TimeStamp;
                  f->dlc=(unsigned char)(ob[i].DataLen>8?8:ob[i].DataLen);
                  f->flags=(unsigned char)((ob[i].ExternFlag?2:0)|(ob[i].RemoteFlag?1:0));
                  memcpy(f->data,ob[i].Data,8);
                  shm->rxHead++;
                  if(shm->rxHead-shm->rxTail>VXL_RING){ shm->rxTail=shm->rxHead-VXL_RING; shm->rxOverflow++; }
              }
              SetEvent(evRx);
          } else Sleep(1);
        }
    }

    if(pC) pC(dev,0);
    shm->ready=0;
    L("=== 退出 ===");
    return 0;
}
