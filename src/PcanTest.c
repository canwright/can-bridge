/* PcanTest.c —— 测「假 PCANBasic.dll」能不能真的驱动盒子
 *
 * 走一遍 PCAN-CCP / UDS 那类软件最常用的调用序列：
 *   CAN_Initialize -> CAN_SetValue(CHANNEL_CONDITION 查询) ->
 *   CAN_Write -> CAN_Read -> CAN_Uninitialize
 */
#include <windows.h>
#include <stdio.h>

typedef DWORD  TPCANStatus;
typedef WORD   TPCANHandle;
typedef DWORD  TPCANBaudrate;
typedef DWORD  TPCANParameter;

typedef struct {
    DWORD ID;
    BYTE  MSGTYPE;
    BYTE  LEN;
    BYTE  DATA[8];
} TPCANMsg;

typedef struct {
    WORD millis;
    WORD millis_overflow;
    WORD micros;
} TPCANTimestamp;

typedef TPCANStatus (__stdcall *PF_Init)(TPCANHandle, TPCANBaudrate, BYTE, DWORD, WORD);
typedef TPCANStatus (__stdcall *PF_Uninit)(TPCANHandle);
typedef TPCANStatus (__stdcall *PF_Read)(TPCANHandle, TPCANMsg *, TPCANTimestamp *);
typedef TPCANStatus (__stdcall *PF_Write)(TPCANHandle, TPCANMsg *);
typedef TPCANStatus (__stdcall *PF_Get)(TPCANHandle, TPCANParameter, void *, DWORD);
typedef TPCANStatus (__stdcall *PF_Set)(TPCANHandle, TPCANParameter, void *, DWORD);
typedef TPCANStatus (__stdcall *PF_Err)(TPCANStatus, WORD, char *);
typedef TPCANStatus (__stdcall *PF_Status)(TPCANHandle);

#define PCAN_USBBUS1          0x51U
#define PCAN_BAUD_500K        0x001CU
#define PCAN_CHANNEL_CONDITION 0x0DU
#define PCAN_API_VERSION      0x05U
#define PCAN_HARDWARE_NAME    0x0EU

int main(int argc, char **argv)
{
    HMODULE d;
    PF_Init pInit;
    PF_Uninit pUninit;
    PF_Read pRead;
    PF_Write pWrite;
    PF_Get pGet;
    PF_Set pSet;
    PF_Err pErr;
    PF_Status pStatus;
    const char *dll = (argc >= 2) ? argv[1] : "PCANBasic.dll";
    TPCANStatus r;
    TPCANMsg m;
    TPCANTimestamp ts;
    char buf[256];
    DWORD v = 0;

    d = LoadLibraryA(dll);
    if (!d) { printf("  LoadLibrary(%s) 失败: %lu\n", dll, GetLastError()); return 1; }
    printf("  ✓ 已加载 %s\n", dll);

    pInit   = (PF_Init)  GetProcAddress(d, "CAN_Initialize");
    pUninit = (PF_Uninit)GetProcAddress(d, "CAN_Uninitialize");
    pRead   = (PF_Read)  GetProcAddress(d, "CAN_Read");
    pWrite  = (PF_Write) GetProcAddress(d, "CAN_Write");
    pGet    = (PF_Get)   GetProcAddress(d, "CAN_GetValue");
    pSet    = (PF_Set)   GetProcAddress(d, "CAN_SetValue");
    pErr    = (PF_Err)   GetProcAddress(d, "CAN_GetErrorText");
    pStatus = (PF_Status)GetProcAddress(d, "CAN_GetStatus");
    if (!pInit || !pUninit || !pRead || !pWrite || !pGet) {
        printf("  ★ 导出不全\n"); return 1;
    }
    printf("  ✓ 17 个导出里关键 5 个都在\n\n");

    if (pGet) {
        r = pGet(0, PCAN_API_VERSION, buf, sizeof(buf));
        printf("  CAN_GetValue(API_VERSION)  -> 0x%lX  '%s'\n", (unsigned long)r, r ? "" : buf);
        r = pGet(0, PCAN_HARDWARE_NAME, buf, sizeof(buf));
        printf("  CAN_GetValue(HW_NAME)      -> 0x%lX  '%s'\n", (unsigned long)r, r ? "" : buf);
    }

    r = pInit(PCAN_USBBUS1, PCAN_BAUD_500K, 0, 0, 0);
    printf("  CAN_Initialize(USBBUS1,500k) -> 0x%lX", (unsigned long)r);
    if (pErr) { pErr(r, 0x09, buf); printf("  (%s)", buf); }
    printf("\n\n");

    if (pGet) {
        r = pGet(PCAN_USBBUS1, PCAN_CHANNEL_CONDITION, &v, 4);
        printf("  CAN_GetValue(CHANNEL_CONDITION) -> 0x%lX  cond=0x%lX\n",
               (unsigned long)r, (unsigned long)v);
    }
    if (pStatus) {
        r = pStatus(PCAN_USBBUS1);
        printf("  CAN_GetStatus                   -> 0x%lX\n\n", (unsigned long)r);
    }

    ZeroMemory(&m, sizeof(m));
    m.ID = 0x123;
    m.MSGTYPE = 0;
    m.LEN = 8;
    m.DATA[0] = 0x11; m.DATA[1] = 0x22; m.DATA[2] = 0x33; m.DATA[3] = 0x44;
    m.DATA[4] = 0x55; m.DATA[5] = 0x66; m.DATA[6] = 0x77; m.DATA[7] = 0x88;
    r = pWrite(PCAN_USBBUS1, &m);
    printf("  CAN_Write(ID=0x123,8B) -> 0x%lX", (unsigned long)r);
    if (pErr) { pErr(r, 0x09, buf); printf("  (%s)", buf); }
    printf("\n\n");

    Sleep(300);
    ZeroMemory(&m, sizeof(m));
    ZeroMemory(&ts, sizeof(ts));
    r = pRead(PCAN_USBBUS1, &m, &ts);
    printf("  CAN_Read -> 0x%lX", (unsigned long)r);
    if (r == 0) {
        printf("  ID=0x%lX len=%d data=%02X %02X %02X %02X\n",
               (unsigned long)m.ID, m.LEN, m.DATA[0], m.DATA[1], m.DATA[2], m.DATA[3]);
    } else {
        if (pErr) { pErr(r, 0x09, buf); printf("  (%s)", buf); }
        printf("\n");
    }

    r = pUninit(PCAN_USBBUS1);
    printf("  CAN_Uninitialize -> 0x%lX\n", (unsigned long)r);
    return 0;
}
