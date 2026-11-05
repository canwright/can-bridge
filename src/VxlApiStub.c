/*
 * VxlApiStub.c —— Vector XL API 空壳探针（只记日志，不实现功能）
 *
 * 目的：写「把周立功/PCAN 盒子模拟成 VN1610/VN1630」的假 vxlapi.dll 之前，
 *       先弄清 CANape 16.0 到底调哪些函数、什么顺序、参数怎么给。
 *
 * 用法：
 *   1. 编译出 VxlApiStub.dll
 *   2. 改名成 vxlapi.dll，放进 CANape 安装目录（★ 不要动 System32）
 *   3. 打开 CANape，走到「选择设备 / 连接通道」
 *   4. 看同目录 vxlapi_stub.log
 *   5. 测完用 CanRestore.exe 一键还原
 *
 * 一律返回 0（XL_SUCCESS）是刻意的：让调用方继续深入，暴露更多调用。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define STUB_LOG_MAX_PER_FUNC 200

typedef struct { const char *name; int used; } STUB_SLOT;

static CRITICAL_SECTION g_cs;
static int g_csInit = 0;
static FILE *g_log = NULL;
static int g_opened = 0;
static STUB_SLOT g_slots[600];
static int g_slotN = 0;

static int StubHit(const char *name)
{
    int i;
    for (i = 0; i < g_slotN; i++)
        if (g_slots[i].name == name) return ++g_slots[i].used;
    if (g_slotN < 600) {
        g_slots[g_slotN].name = name; g_slots[g_slotN].used = 1; g_slotN++;
        return 1;
    }
    return 0;
}

static void StubOpen(void)
{
    char p[1024];
    HMODULE hm = NULL;
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = 1; }
    if (g_opened) return;
    g_opened = 1;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&StubOpen, &hm)) {
        DWORD n = GetModuleFileNameA(hm, p, 1000);
        if (n > 0) {
            char *s = strrchr(p, 0x5C);
            if (s) { *s = 0; strcat(p, "\\vxlapi_stub.log"); }
            g_log = fopen(p, "a");
        }
    }
    if (!g_log) {
        char tmp[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tmp) > 0) { strcat(tmp, "vxlapi_stub.log"); g_log = fopen(tmp, "a"); }
    }
    if (g_log) {
        fprintf(g_log, "\n===== VxlApiStub loaded tick=%lu =====\n", (unsigned long)GetTickCount());
        fflush(g_log);
    }
}

static void StubLog(const char *fn, int nargs, const unsigned long *a)
{
    int i, n;
    StubOpen();
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    n = StubHit(fn);
    if (n <= STUB_LOG_MAX_PER_FUNC) {
        fprintf(g_log, "[%lu] %s(", (unsigned long)GetTickCount(), fn);
        for (i = 0; i < nargs; i++) fprintf(g_log, i ? ", 0x%lX" : "0x%lX", a[i]);
        fprintf(g_log, ")\n");
        if ((n % 50) == 0) fflush(g_log);
    } else if (n == STUB_LOG_MAX_PER_FUNC + 1) {
        fprintf(g_log, "[%lu] %s(...) beyond limit\n", (unsigned long)GetTickCount(), fn);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_cs);
}

/* _ethDefaultConfigCleanAllIds@4 */
unsigned long __stdcall ethDefaultConfigCleanAllIds(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("ethDefaultConfigCleanAllIds", 1, a);
    return 0;
}

/* _ethDefaultConfigCommitChanges@4 */
unsigned long __stdcall ethDefaultConfigCommitChanges(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("ethDefaultConfigCommitChanges", 1, a);
    return 0;
}

/* _ethDefaultConfigReadIdData@16 */
unsigned long __stdcall ethDefaultConfigReadIdData(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("ethDefaultConfigReadIdData", 4, a);
    return 0;
}

/* _ethDefaultConfigReadIdHeader@16 */
unsigned long __stdcall ethDefaultConfigReadIdHeader(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("ethDefaultConfigReadIdHeader", 4, a);
    return 0;
}

/* _ethDefaultConfigStartModification@12 */
unsigned long __stdcall ethDefaultConfigStartModification(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("ethDefaultConfigStartModification", 3, a);
    return 0;
}

/* _ethDefaultConfigWriteId@20 */
unsigned long __stdcall ethDefaultConfigWriteId(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("ethDefaultConfigWriteId", 5, a);
    return 0;
}

/* _xlA429Receive@8 */
unsigned long __stdcall xlA429Receive(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlA429Receive", 2, a);
    return 0;
}

/* _xlA429SetChannelParams@16 */
unsigned long __stdcall xlA429SetChannelParams(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlA429SetChannelParams", 4, a);
    return 0;
}

/* _xlA429Transmit@24 */
unsigned long __stdcall xlA429Transmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlA429Transmit", 6, a);
    return 0;
}

/* _xlActivateChannel@20 */
unsigned long __stdcall xlActivateChannel(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlActivateChannel", 5, a);
    return 0;
}

/* _xlAddChannelToPort@28 */
unsigned long __stdcall xlAddChannelToPort(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlAddChannelToPort", 7, a);
    return 0;
}

/* _xlAddRemoteDevice@12 */
unsigned long __stdcall xlAddRemoteDevice(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlAddRemoteDevice", 3, a);
    return 0;
}

/* _xlAddStatusProviderToPort@16 */
unsigned long __stdcall xlAddStatusProviderToPort(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlAddStatusProviderToPort", 4, a);
    return 0;
}

/* _xlCanAddAcceptanceRange@20 */
unsigned long __stdcall xlCanAddAcceptanceRange(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCanAddAcceptanceRange", 5, a);
    return 0;
}

/* _xlCanFdSetConfiguration@16 */
unsigned long __stdcall xlCanFdSetConfiguration(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanFdSetConfiguration", 4, a);
    return 0;
}

/* _xlCanFlushTransmitQueue@12 */
unsigned long __stdcall xlCanFlushTransmitQueue(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlCanFlushTransmitQueue", 3, a);
    return 0;
}

/* _xlCanGetEventString@4 */
unsigned long __stdcall xlCanGetEventString(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlCanGetEventString", 1, a);
    return 0;
}

/* _xlCanReceive@8 */
unsigned long __stdcall xlCanReceive(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlCanReceive", 2, a);
    return 0;
}

/* _xlCanRemoveAcceptanceRange@20 */
unsigned long __stdcall xlCanRemoveAcceptanceRange(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCanRemoveAcceptanceRange", 5, a);
    return 0;
}

/* _xlCanRequestChipState@12 */
unsigned long __stdcall xlCanRequestChipState(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlCanRequestChipState", 3, a);
    return 0;
}

/* _xlCanResetAcceptance@16 */
unsigned long __stdcall xlCanResetAcceptance(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanResetAcceptance", 4, a);
    return 0;
}

/* _xlCanSetChannelAcceptance@24 */
unsigned long __stdcall xlCanSetChannelAcceptance(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlCanSetChannelAcceptance", 6, a);
    return 0;
}

/* _xlCanSetChannelBitrate@16 */
unsigned long __stdcall xlCanSetChannelBitrate(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanSetChannelBitrate", 4, a);
    return 0;
}

/* _xlCanSetChannelMode@20 */
unsigned long __stdcall xlCanSetChannelMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCanSetChannelMode", 5, a);
    return 0;
}

/* _xlCanSetChannelOutput@16 */
unsigned long __stdcall xlCanSetChannelOutput(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanSetChannelOutput", 4, a);
    return 0;
}

/* _xlCanSetChannelParams@16 */
unsigned long __stdcall xlCanSetChannelParams(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanSetChannelParams", 4, a);
    return 0;
}

/* _xlCanSetChannelParamsC200@20 */
unsigned long __stdcall xlCanSetChannelParamsC200(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCanSetChannelParamsC200", 5, a);
    return 0;
}

/* _xlCanSetChannelTransceiver@24 */
unsigned long __stdcall xlCanSetChannelTransceiver(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlCanSetChannelTransceiver", 6, a);
    return 0;
}

/* _xlCanSetReceiveMode@12 */
unsigned long __stdcall xlCanSetReceiveMode(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlCanSetReceiveMode", 3, a);
    return 0;
}

/* _xlCanStressConfigure@16 */
unsigned long __stdcall xlCanStressConfigure(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanStressConfigure", 4, a);
    return 0;
}

/* _xlCanStressConfigureAnalogComponents@16 */
unsigned long __stdcall xlCanStressConfigureAnalogComponents(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanStressConfigureAnalogComponents", 4, a);
    return 0;
}

/* _xlCanStressConfigurePulse@16 */
unsigned long __stdcall xlCanStressConfigurePulse(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanStressConfigurePulse", 4, a);
    return 0;
}

/* _xlCanStressConfigureTriggerTiming@16 */
unsigned long __stdcall xlCanStressConfigureTriggerTiming(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanStressConfigureTriggerTiming", 4, a);
    return 0;
}

/* _xlCanStressDeactivateStress@16 */
unsigned long __stdcall xlCanStressDeactivateStress(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanStressDeactivateStress", 4, a);
    return 0;
}

/* _xlCanStressDeactivateTrigger@16 */
unsigned long __stdcall xlCanStressDeactivateTrigger(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanStressDeactivateTrigger", 4, a);
    return 0;
}

/* _xlCanStressGeneratePulse@16 */
unsigned long __stdcall xlCanStressGeneratePulse(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanStressGeneratePulse", 4, a);
    return 0;
}

/* _xlCanStressGetAnalogComponentDefinitions@16 */
unsigned long __stdcall xlCanStressGetAnalogComponentDefinitions(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanStressGetAnalogComponentDefinitions", 4, a);
    return 0;
}

/* _xlCanStressGetAnalogComponentValues@16 */
unsigned long __stdcall xlCanStressGetAnalogComponentValues(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanStressGetAnalogComponentValues", 4, a);
    return 0;
}

/* _xlCanStressTriggerNow@16 */
unsigned long __stdcall xlCanStressTriggerNow(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCanStressTriggerNow", 4, a);
    return 0;
}

/* _xlCanTransmit@20 */
unsigned long __stdcall xlCanTransmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCanTransmit", 5, a);
    return 0;
}

/* _xlCanTransmitEx@24 */
unsigned long __stdcall xlCanTransmitEx(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlCanTransmitEx", 6, a);
    return 0;
}

/* _xlCheckLicense@16 */
unsigned long __stdcall xlCheckLicense(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCheckLicense", 4, a);
    return 0;
}

/* _xlCloseDriver@0 */
unsigned long __stdcall xlCloseDriver(void)
{
    StubLog("xlCloseDriver", 0, 0);
    return 0;
}

/* _xlClosePort@4 */
unsigned long __stdcall xlClosePort(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlClosePort", 1, a);
    return 0;
}

/* _xlCreateDriverConfig@8 */
unsigned long __stdcall xlCreateDriverConfig(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlCreateDriverConfig", 2, a);
    return 0;
}

/* _xlCreatePort@24 */
unsigned long __stdcall xlCreatePort(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlCreatePort", 6, a);
    return 0;
}

/* _xlCryptoCloseSession@8 */
unsigned long __stdcall xlCryptoCloseSession(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlCryptoCloseSession", 2, a);
    return 0;
}

/* _xlCryptoGetErrorString@8 */
unsigned long __stdcall xlCryptoGetErrorString(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlCryptoGetErrorString", 2, a);
    return 0;
}

/* _xlCryptoKeyAuthorizationComplete@20 */
unsigned long __stdcall xlCryptoKeyAuthorizationComplete(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCryptoKeyAuthorizationComplete", 5, a);
    return 0;
}

/* _xlCryptoKeyAuthorizationStart@20 */
unsigned long __stdcall xlCryptoKeyAuthorizationStart(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCryptoKeyAuthorizationStart", 5, a);
    return 0;
}

/* _xlCryptoReadDeviceInfo@20 */
unsigned long __stdcall xlCryptoReadDeviceInfo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCryptoReadDeviceInfo", 5, a);
    return 0;
}

/* _xlCryptoReadPublicKey@12 */
unsigned long __stdcall xlCryptoReadPublicKey(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlCryptoReadPublicKey", 3, a);
    return 0;
}

/* _xlCryptoReadTde@12 */
unsigned long __stdcall xlCryptoReadTde(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlCryptoReadTde", 3, a);
    return 0;
}

/* _xlCryptoSignCounterComplete@16 */
unsigned long __stdcall xlCryptoSignCounterComplete(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlCryptoSignCounterComplete", 4, a);
    return 0;
}

/* _xlCryptoSignCounterExpand@12 */
unsigned long __stdcall xlCryptoSignCounterExpand(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlCryptoSignCounterExpand", 3, a);
    return 0;
}

/* _xlCryptoSignCounterStart@20 */
unsigned long __stdcall xlCryptoSignCounterStart(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCryptoSignCounterStart", 5, a);
    return 0;
}

/* _xlCryptoSignRandomComplete@20 */
unsigned long __stdcall xlCryptoSignRandomComplete(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCryptoSignRandomComplete", 5, a);
    return 0;
}

/* _xlCryptoSignRandomExpand@12 */
unsigned long __stdcall xlCryptoSignRandomExpand(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlCryptoSignRandomExpand", 3, a);
    return 0;
}

/* _xlCryptoSignRandomStart@20 */
unsigned long __stdcall xlCryptoSignRandomStart(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCryptoSignRandomStart", 5, a);
    return 0;
}

/* _xlCryptoStartSession@20 */
unsigned long __stdcall xlCryptoStartSession(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlCryptoStartSession", 5, a);
    return 0;
}

/* _xlDAIORequestMeasurement@12 */
unsigned long __stdcall xlDAIORequestMeasurement(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlDAIORequestMeasurement", 3, a);
    return 0;
}

/* _xlDAIOSetAnalogOutput@28 */
unsigned long __stdcall xlDAIOSetAnalogOutput(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlDAIOSetAnalogOutput", 7, a);
    return 0;
}

/* _xlDAIOSetAnalogParameters@24 */
unsigned long __stdcall xlDAIOSetAnalogParameters(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlDAIOSetAnalogParameters", 6, a);
    return 0;
}

/* _xlDAIOSetAnalogTrigger@24 */
unsigned long __stdcall xlDAIOSetAnalogTrigger(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlDAIOSetAnalogTrigger", 6, a);
    return 0;
}

/* _xlDAIOSetDigitalOutput@20 */
unsigned long __stdcall xlDAIOSetDigitalOutput(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlDAIOSetDigitalOutput", 5, a);
    return 0;
}

/* _xlDAIOSetDigitalParameters@20 */
unsigned long __stdcall xlDAIOSetDigitalParameters(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlDAIOSetDigitalParameters", 5, a);
    return 0;
}

/* _xlDAIOSetDigitalTrigger@16 */
unsigned long __stdcall xlDAIOSetDigitalTrigger(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlDAIOSetDigitalTrigger", 4, a);
    return 0;
}

/* _xlDAIOSetMeasurementFrequency@16 */
unsigned long __stdcall xlDAIOSetMeasurementFrequency(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlDAIOSetMeasurementFrequency", 4, a);
    return 0;
}

/* _xlDAIOSetPWMOutput@20 */
unsigned long __stdcall xlDAIOSetPWMOutput(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlDAIOSetPWMOutput", 5, a);
    return 0;
}

/* _xlDeactivateChannel@12 */
unsigned long __stdcall xlDeactivateChannel(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlDeactivateChannel", 3, a);
    return 0;
}

/* _xlDefaultConfigGPBRead@12 */
unsigned long __stdcall xlDefaultConfigGPBRead(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlDefaultConfigGPBRead", 3, a);
    return 0;
}

/* _xlDefaultConfigGPBWrite@12 */
unsigned long __stdcall xlDefaultConfigGPBWrite(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlDefaultConfigGPBWrite", 3, a);
    return 0;
}

/* _xlDestroyDriverConfig@4 */
unsigned long __stdcall xlDestroyDriverConfig(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlDestroyDriverConfig", 1, a);
    return 0;
}

/* _xlDisableDevice@8 */
unsigned long __stdcall xlDisableDevice(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlDisableDevice", 2, a);
    return 0;
}

/* _xlEnableDevice@8 */
unsigned long __stdcall xlEnableDevice(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlEnableDevice", 2, a);
    return 0;
}

/* _xlEthCoreGetDefaultConfig@16 */
unsigned long __stdcall xlEthCoreGetDefaultConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlEthCoreGetDefaultConfig", 4, a);
    return 0;
}

/* _xlEthCoreSetDefaultConfig@16 */
unsigned long __stdcall xlEthCoreSetDefaultConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlEthCoreSetDefaultConfig", 4, a);
    return 0;
}

/* _xlEthDefaultConfigCompatible@12 */
unsigned long __stdcall xlEthDefaultConfigCompatible(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlEthDefaultConfigCompatible", 3, a);
    return 0;
}

/* _xlEthGetConfig@20 */
unsigned long __stdcall xlEthGetConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlEthGetConfig", 5, a);
    return 0;
}

/* _xlEthReadPhyRegister@16 */
unsigned long __stdcall xlEthReadPhyRegister(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlEthReadPhyRegister", 4, a);
    return 0;
}

/* _xlEthReceive@8 */
unsigned long __stdcall xlEthReceive(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlEthReceive", 2, a);
    return 0;
}

/* _xlEthRequestChannelStatus@16 */
unsigned long __stdcall xlEthRequestChannelStatus(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlEthRequestChannelStatus", 4, a);
    return 0;
}

/* _xlEthSetBypass@20 */
unsigned long __stdcall xlEthSetBypass(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlEthSetBypass", 5, a);
    return 0;
}

/* _xlEthSetConfig@20 */
unsigned long __stdcall xlEthSetConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlEthSetConfig", 5, a);
    return 0;
}

/* _xlEthStartAutoConfig@16 */
unsigned long __stdcall xlEthStartAutoConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlEthStartAutoConfig", 4, a);
    return 0;
}

/* _xlEthTransmit@20 */
unsigned long __stdcall xlEthTransmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlEthTransmit", 5, a);
    return 0;
}

/* _xlEthTwinkleStatusLed@16 */
unsigned long __stdcall xlEthTwinkleStatusLed(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlEthTwinkleStatusLed", 4, a);
    return 0;
}

/* _xlEthTwinkleStatusLedEx@8 */
unsigned long __stdcall xlEthTwinkleStatusLedEx(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlEthTwinkleStatusLedEx", 2, a);
    return 0;
}

/* _xlEthWritePhyRegister@16 */
unsigned long __stdcall xlEthWritePhyRegister(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlEthWritePhyRegister", 4, a);
    return 0;
}

/* _xlFinalizePort@4 */
unsigned long __stdcall xlFinalizePort(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlFinalizePort", 1, a);
    return 0;
}

/* _xlFlushReceiveQueue@4 */
unsigned long __stdcall xlFlushReceiveQueue(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlFlushReceiveQueue", 1, a);
    return 0;
}

/* _xlFrActivateGateway@16 */
unsigned long __stdcall xlFrActivateGateway(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlFrActivateGateway", 4, a);
    return 0;
}

/* _xlFrActivateSpy@16 */
unsigned long __stdcall xlFrActivateSpy(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlFrActivateSpy", 4, a);
    return 0;
}

/* _xlFrGetChannelConfiguration@16 */
unsigned long __stdcall xlFrGetChannelConfiguration(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlFrGetChannelConfiguration", 4, a);
    return 0;
}

/* _xlFrInitStartupAndSync@16 */
unsigned long __stdcall xlFrInitStartupAndSync(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlFrInitStartupAndSync", 4, a);
    return 0;
}

/* _xlFrReceive@8 */
unsigned long __stdcall xlFrReceive(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlFrReceive", 2, a);
    return 0;
}

/* _xlFrSendSymbolWindow@16 */
unsigned long __stdcall xlFrSendSymbolWindow(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlFrSendSymbolWindow", 4, a);
    return 0;
}

/* _xlFrSetAcceptanceFilter@16 */
unsigned long __stdcall xlFrSetAcceptanceFilter(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlFrSetAcceptanceFilter", 4, a);
    return 0;
}

/* _xlFrSetConfiguration@16 */
unsigned long __stdcall xlFrSetConfiguration(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlFrSetConfiguration", 4, a);
    return 0;
}

/* _xlFrSetMode@16 */
unsigned long __stdcall xlFrSetMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlFrSetMode", 4, a);
    return 0;
}

/* _xlFrSetTransceiverMode@20 */
unsigned long __stdcall xlFrSetTransceiverMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlFrSetTransceiverMode", 5, a);
    return 0;
}

/* _xlFrSetupSymbolWindow@20 */
unsigned long __stdcall xlFrSetupSymbolWindow(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlFrSetupSymbolWindow", 5, a);
    return 0;
}

/* _xlFrTransmit@16 */
unsigned long __stdcall xlFrTransmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlFrTransmit", 4, a);
    return 0;
}

/* _xlGNSSGetBatteryState@16 */
unsigned long __stdcall xlGNSSGetBatteryState(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGNSSGetBatteryState", 4, a);
    return 0;
}

/* _xlGNSSGetNavCfg@16 */
unsigned long __stdcall xlGNSSGetNavCfg(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGNSSGetNavCfg", 4, a);
    return 0;
}

/* _xlGNSSGetNmeaCfg@16 */
unsigned long __stdcall xlGNSSGetNmeaCfg(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGNSSGetNmeaCfg", 4, a);
    return 0;
}

/* _xlGNSSGetRcvCfg@16 */
unsigned long __stdcall xlGNSSGetRcvCfg(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGNSSGetRcvCfg", 4, a);
    return 0;
}

/* _xlGNSSReceive@8 */
unsigned long __stdcall xlGNSSReceive(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlGNSSReceive", 2, a);
    return 0;
}

/* _xlGNSSSetNavCfg@16 */
unsigned long __stdcall xlGNSSSetNavCfg(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGNSSSetNavCfg", 4, a);
    return 0;
}

/* _xlGNSSSetNmeaCfg@16 */
unsigned long __stdcall xlGNSSSetNmeaCfg(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGNSSSetNmeaCfg", 4, a);
    return 0;
}

/* _xlGNSSSetRcvCfg@16 */
unsigned long __stdcall xlGNSSSetRcvCfg(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGNSSSetRcvCfg", 4, a);
    return 0;
}

/* _xlGenerateSyncPulse@12 */
unsigned long __stdcall xlGenerateSyncPulse(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlGenerateSyncPulse", 3, a);
    return 0;
}

/* _xlGetApiLimitations@16 */
unsigned long __stdcall xlGetApiLimitations(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGetApiLimitations", 4, a);
    return 0;
}

/* _xlGetApplConfig@24 */
unsigned long __stdcall xlGetApplConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlGetApplConfig", 6, a);
    return 0;
}

/* _xlGetApplConfigList@8 */
unsigned long __stdcall xlGetApplConfigList(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlGetApplConfigList", 2, a);
    return 0;
}

/* _xlGetChannelIndex@12 */
unsigned long __stdcall xlGetChannelIndex(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlGetChannelIndex", 3, a);
    return 0;
}

/* _xlGetChannelMask@12 */
unsigned long __stdcall xlGetChannelMask(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlGetChannelMask", 3, a);
    return 0;
}

/* _xlGetChannelTime@16 */
unsigned long __stdcall xlGetChannelTime(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGetChannelTime", 4, a);
    return 0;
}

/* _xlGetChannelVersionInfo@20 */
unsigned long __stdcall xlGetChannelVersionInfo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlGetChannelVersionInfo", 5, a);
    return 0;
}

/* _xlGetDriverConfig2@4 */
unsigned long __stdcall xlGetDriverConfig2(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlGetDriverConfig2", 1, a);
    return 0;
}

/* _xlGetDriverConfig@4 */
unsigned long __stdcall xlGetDriverConfig(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlGetDriverConfig", 1, a);
    return 0;
}

/* _xlGetDriverConfigEx@20 */
unsigned long __stdcall xlGetDriverConfigEx(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlGetDriverConfigEx", 5, a);
    return 0;
}

/* _xlGetErrorString@4 */
unsigned long __stdcall xlGetErrorString(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlGetErrorString", 1, a);
    return 0;
}

/* _xlGetEttData@24 */
unsigned long __stdcall xlGetEttData(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlGetEttData", 6, a);
    return 0;
}

/* _xlGetEventString@4 */
unsigned long __stdcall xlGetEventString(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlGetEventString", 1, a);
    return 0;
}

/* _xlGetHwChannelDescription@20 */
unsigned long __stdcall xlGetHwChannelDescription(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlGetHwChannelDescription", 5, a);
    return 0;
}

/* _xlGetHwSegmentDescription@20 */
unsigned long __stdcall xlGetHwSegmentDescription(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlGetHwSegmentDescription", 5, a);
    return 0;
}

/* _xlGetKeymanBoxes@4 */
unsigned long __stdcall xlGetKeymanBoxes(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlGetKeymanBoxes", 1, a);
    return 0;
}

/* _xlGetKeymanInfo@16 */
unsigned long __stdcall xlGetKeymanInfo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGetKeymanInfo", 4, a);
    return 0;
}

/* _xlGetLicenseInfo@16 */
unsigned long __stdcall xlGetLicenseInfo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGetLicenseInfo", 4, a);
    return 0;
}

/* _xlGetLicenseInfoEx@16 */
unsigned long __stdcall xlGetLicenseInfoEx(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGetLicenseInfoEx", 4, a);
    return 0;
}

/* _xlGetQueueNotifyMode@16 */
unsigned long __stdcall xlGetQueueNotifyMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGetQueueNotifyMode", 4, a);
    return 0;
}

/* _xlGetReceiveQueueLevel@8 */
unsigned long __stdcall xlGetReceiveQueueLevel(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlGetReceiveQueueLevel", 2, a);
    return 0;
}

/* _xlGetRemoteDeviceCapabilities@8 */
unsigned long __stdcall xlGetRemoteDeviceCapabilities(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlGetRemoteDeviceCapabilities", 2, a);
    return 0;
}

/* _xlGetRemoteDeviceInfo@12 */
unsigned long __stdcall xlGetRemoteDeviceInfo(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlGetRemoteDeviceInfo", 3, a);
    return 0;
}

/* _xlGetRemoteDriverConfig@4 */
unsigned long __stdcall xlGetRemoteDriverConfig(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlGetRemoteDriverConfig", 1, a);
    return 0;
}

/* _xlGetRemoteHwInfo@16 */
unsigned long __stdcall xlGetRemoteHwInfo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlGetRemoteHwInfo", 4, a);
    return 0;
}

/* _xlGetRemoteObjectIdentity@12 */
unsigned long __stdcall xlGetRemoteObjectIdentity(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlGetRemoteObjectIdentity", 3, a);
    return 0;
}

/* _xlGetSyncTime@8 */
unsigned long __stdcall xlGetSyncTime(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlGetSyncTime", 2, a);
    return 0;
}

/* _xlGetToolAccessApplConfig@20 */
unsigned long __stdcall xlGetToolAccessApplConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlGetToolAccessApplConfig", 5, a);
    return 0;
}

/* _xlIoConfigurePorts@16 */
unsigned long __stdcall xlIoConfigurePorts(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoConfigurePorts", 4, a);
    return 0;
}

/* _xlIoResetCom@16 */
unsigned long __stdcall xlIoResetCom(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoResetCom", 4, a);
    return 0;
}

/* _xlIoSetAckMode@16 */
unsigned long __stdcall xlIoSetAckMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoSetAckMode", 4, a);
    return 0;
}

/* _xlIoSetActivationLine@16 */
unsigned long __stdcall xlIoSetActivationLine(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoSetActivationLine", 4, a);
    return 0;
}

/* _xlIoSetAnalogOutput@16 */
unsigned long __stdcall xlIoSetAnalogOutput(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoSetAnalogOutput", 4, a);
    return 0;
}

/* _xlIoSetCaptureTimeout@16 */
unsigned long __stdcall xlIoSetCaptureTimeout(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoSetCaptureTimeout", 4, a);
    return 0;
}

/* _xlIoSetDigInThreshold@16 */
unsigned long __stdcall xlIoSetDigInThreshold(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoSetDigInThreshold", 4, a);
    return 0;
}

/* _xlIoSetDigOutLevel@16 */
unsigned long __stdcall xlIoSetDigOutLevel(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoSetDigOutLevel", 4, a);
    return 0;
}

/* _xlIoSetDigitalOutput@16 */
unsigned long __stdcall xlIoSetDigitalOutput(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoSetDigitalOutput", 4, a);
    return 0;
}

/* _xlIoSetDigitalOutputWithHold@16 */
unsigned long __stdcall xlIoSetDigitalOutputWithHold(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoSetDigitalOutputWithHold", 4, a);
    return 0;
}

/* _xlIoSetPwmOutput@16 */
unsigned long __stdcall xlIoSetPwmOutput(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoSetPwmOutput", 4, a);
    return 0;
}

/* _xlIoSetTriggerMode@16 */
unsigned long __stdcall xlIoSetTriggerMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoSetTriggerMode", 4, a);
    return 0;
}

/* _xlIoStartSampling@16 */
unsigned long __stdcall xlIoStartSampling(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlIoStartSampling", 4, a);
    return 0;
}

/* _xlKlineFastInitTester@24 */
unsigned long __stdcall xlKlineFastInitTester(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlKlineFastInitTester", 6, a);
    return 0;
}

/* _xlKlineInit5BdEcu@16 */
unsigned long __stdcall xlKlineInit5BdEcu(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlKlineInit5BdEcu", 4, a);
    return 0;
}

/* _xlKlineInit5BdTester@16 */
unsigned long __stdcall xlKlineInit5BdTester(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlKlineInit5BdTester", 4, a);
    return 0;
}

/* _xlKlineSetBaudrate@16 */
unsigned long __stdcall xlKlineSetBaudrate(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlKlineSetBaudrate", 4, a);
    return 0;
}

/* _xlKlineSetCommunicationTimingEcu@16 */
unsigned long __stdcall xlKlineSetCommunicationTimingEcu(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlKlineSetCommunicationTimingEcu", 4, a);
    return 0;
}

/* _xlKlineSetCommunicationTimingTester@16 */
unsigned long __stdcall xlKlineSetCommunicationTimingTester(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlKlineSetCommunicationTimingTester", 4, a);
    return 0;
}

/* _xlKlineSetUartParams@16 */
unsigned long __stdcall xlKlineSetUartParams(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlKlineSetUartParams", 4, a);
    return 0;
}

/* _xlKlineSwitchHighspeedMode@16 */
unsigned long __stdcall xlKlineSwitchHighspeedMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlKlineSwitchHighspeedMode", 4, a);
    return 0;
}

/* _xlKlineSwitchTesterResistor@16 */
unsigned long __stdcall xlKlineSwitchTesterResistor(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlKlineSwitchTesterResistor", 4, a);
    return 0;
}

/* _xlKlineTransmit@20 */
unsigned long __stdcall xlKlineTransmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlKlineTransmit", 5, a);
    return 0;
}

/* _xlLinSendRequest@20 */
unsigned long __stdcall xlLinSendRequest(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlLinSendRequest", 5, a);
    return 0;
}

/* _xlLinSetChannelParams@28 */
unsigned long __stdcall xlLinSetChannelParams(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlLinSetChannelParams", 7, a);
    return 0;
}

/* _xlLinSetChecksum@16 */
unsigned long __stdcall xlLinSetChecksum(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlLinSetChecksum", 4, a);
    return 0;
}

/* _xlLinSetDLC@16 */
unsigned long __stdcall xlLinSetDLC(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlLinSetDLC", 4, a);
    return 0;
}

/* _xlLinSetSlave@28 */
unsigned long __stdcall xlLinSetSlave(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlLinSetSlave", 7, a);
    return 0;
}

/* _xlLinSetSleepMode@20 */
unsigned long __stdcall xlLinSetSleepMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlLinSetSleepMode", 5, a);
    return 0;
}

/* _xlLinSwitchSlave@20 */
unsigned long __stdcall xlLinSwitchSlave(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlLinSwitchSlave", 5, a);
    return 0;
}

/* _xlLinWakeUp@12 */
unsigned long __stdcall xlLinWakeUp(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlLinWakeUp", 3, a);
    return 0;
}

/* _xlLogGetMountPoint@16 */
unsigned long __stdcall xlLogGetMountPoint(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlLogGetMountPoint", 4, a);
    return 0;
}

/* _xlMost150AsyncConfigureBusload@20 */
unsigned long __stdcall xlMost150AsyncConfigureBusload(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150AsyncConfigureBusload", 5, a);
    return 0;
}

/* _xlMost150AsyncGenerateBusload@20 */
unsigned long __stdcall xlMost150AsyncGenerateBusload(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150AsyncGenerateBusload", 5, a);
    return 0;
}

/* _xlMost150AsyncTransmit@20 */
unsigned long __stdcall xlMost150AsyncTransmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150AsyncTransmit", 5, a);
    return 0;
}

/* _xlMost150ConfigureRxBuffer@24 */
unsigned long __stdcall xlMost150ConfigureRxBuffer(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlMost150ConfigureRxBuffer", 6, a);
    return 0;
}

/* _xlMost150CtrlBusStatistic@16 */
unsigned long __stdcall xlMost150CtrlBusStatistic(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150CtrlBusStatistic", 4, a);
    return 0;
}

/* _xlMost150CtrlConfigureBusload@20 */
unsigned long __stdcall xlMost150CtrlConfigureBusload(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150CtrlConfigureBusload", 5, a);
    return 0;
}

/* _xlMost150CtrlGenerateBusload@20 */
unsigned long __stdcall xlMost150CtrlGenerateBusload(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150CtrlGenerateBusload", 5, a);
    return 0;
}

/* _xlMost150CtrlSyncAudio@20 */
unsigned long __stdcall xlMost150CtrlSyncAudio(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150CtrlSyncAudio", 5, a);
    return 0;
}

/* _xlMost150CtrlTransmit@20 */
unsigned long __stdcall xlMost150CtrlTransmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150CtrlTransmit", 5, a);
    return 0;
}

/* _xlMost150EclConfigureSeq@28 */
unsigned long __stdcall xlMost150EclConfigureSeq(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMost150EclConfigureSeq", 7, a);
    return 0;
}

/* _xlMost150EclGenerateSeq@20 */
unsigned long __stdcall xlMost150EclGenerateSeq(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150EclGenerateSeq", 5, a);
    return 0;
}

/* _xlMost150EthernetTransmit@20 */
unsigned long __stdcall xlMost150EthernetTransmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150EthernetTransmit", 5, a);
    return 0;
}

/* _xlMost150FilterEvent@20 */
unsigned long __stdcall xlMost150FilterEvent(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150FilterEvent", 5, a);
    return 0;
}

/* _xlMost150GenerateBypassStress@28 */
unsigned long __stdcall xlMost150GenerateBypassStress(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMost150GenerateBypassStress", 7, a);
    return 0;
}

/* _xlMost150GenerateLightError@28 */
unsigned long __stdcall xlMost150GenerateLightError(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMost150GenerateLightError", 7, a);
    return 0;
}

/* _xlMost150GenerateLockError@28 */
unsigned long __stdcall xlMost150GenerateLockError(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMost150GenerateLockError", 7, a);
    return 0;
}

/* _xlMost150GetDeviceMode@16 */
unsigned long __stdcall xlMost150GetDeviceMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150GetDeviceMode", 4, a);
    return 0;
}

/* _xlMost150GetDeviceState@20 */
unsigned long __stdcall xlMost150GetDeviceState(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150GetDeviceState", 5, a);
    return 0;
}

/* _xlMost150GetECLInfo@16 */
unsigned long __stdcall xlMost150GetECLInfo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150GetECLInfo", 4, a);
    return 0;
}

/* _xlMost150GetFrequency@16 */
unsigned long __stdcall xlMost150GetFrequency(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150GetFrequency", 4, a);
    return 0;
}

/* _xlMost150GetMPR@20 */
unsigned long __stdcall xlMost150GetMPR(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150GetMPR", 5, a);
    return 0;
}

/* _xlMost150GetNPR@20 */
unsigned long __stdcall xlMost150GetNPR(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150GetNPR", 5, a);
    return 0;
}

/* _xlMost150GetRxLightLockStatus@20 */
unsigned long __stdcall xlMost150GetRxLightLockStatus(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150GetRxLightLockStatus", 5, a);
    return 0;
}

/* _xlMost150GetSPDIFMode@16 */
unsigned long __stdcall xlMost150GetSPDIFMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150GetSPDIFMode", 4, a);
    return 0;
}

/* _xlMost150GetSSOResult@16 */
unsigned long __stdcall xlMost150GetSSOResult(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150GetSSOResult", 4, a);
    return 0;
}

/* _xlMost150GetShutdownFlag@16 */
unsigned long __stdcall xlMost150GetShutdownFlag(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150GetShutdownFlag", 4, a);
    return 0;
}

/* _xlMost150GetSpecialNodeInfo@20 */
unsigned long __stdcall xlMost150GetSpecialNodeInfo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150GetSpecialNodeInfo", 5, a);
    return 0;
}

/* _xlMost150GetSystemLockFlag@16 */
unsigned long __stdcall xlMost150GetSystemLockFlag(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150GetSystemLockFlag", 4, a);
    return 0;
}

/* _xlMost150GetTxLight@16 */
unsigned long __stdcall xlMost150GetTxLight(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150GetTxLight", 4, a);
    return 0;
}

/* _xlMost150Receive@8 */
unsigned long __stdcall xlMost150Receive(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlMost150Receive", 2, a);
    return 0;
}

/* _xlMost150SetDeviceMode@20 */
unsigned long __stdcall xlMost150SetDeviceMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SetDeviceMode", 5, a);
    return 0;
}

/* _xlMost150SetECLGlitchFilter@20 */
unsigned long __stdcall xlMost150SetECLGlitchFilter(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SetECLGlitchFilter", 5, a);
    return 0;
}

/* _xlMost150SetECLLine@20 */
unsigned long __stdcall xlMost150SetECLLine(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SetECLLine", 5, a);
    return 0;
}

/* _xlMost150SetECLTermination@20 */
unsigned long __stdcall xlMost150SetECLTermination(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SetECLTermination", 5, a);
    return 0;
}

/* _xlMost150SetFrequency@20 */
unsigned long __stdcall xlMost150SetFrequency(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SetFrequency", 5, a);
    return 0;
}

/* _xlMost150SetSPDIFMode@20 */
unsigned long __stdcall xlMost150SetSPDIFMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SetSPDIFMode", 5, a);
    return 0;
}

/* _xlMost150SetSSOResult@20 */
unsigned long __stdcall xlMost150SetSSOResult(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SetSSOResult", 5, a);
    return 0;
}

/* _xlMost150SetSpecialNodeInfo@20 */
unsigned long __stdcall xlMost150SetSpecialNodeInfo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SetSpecialNodeInfo", 5, a);
    return 0;
}

/* _xlMost150SetTxLight@20 */
unsigned long __stdcall xlMost150SetTxLight(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SetTxLight", 5, a);
    return 0;
}

/* _xlMost150SetTxLightPower@20 */
unsigned long __stdcall xlMost150SetTxLightPower(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SetTxLightPower", 5, a);
    return 0;
}

/* _xlMost150Shutdown@16 */
unsigned long __stdcall xlMost150Shutdown(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150Shutdown", 4, a);
    return 0;
}

/* _xlMost150Startup@16 */
unsigned long __stdcall xlMost150Startup(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150Startup", 4, a);
    return 0;
}

/* _xlMost150StreamClearTxFifo@20 */
unsigned long __stdcall xlMost150StreamClearTxFifo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150StreamClearTxFifo", 5, a);
    return 0;
}

/* _xlMost150StreamClose@20 */
unsigned long __stdcall xlMost150StreamClose(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150StreamClose", 5, a);
    return 0;
}

/* _xlMost150StreamGetInfo@20 */
unsigned long __stdcall xlMost150StreamGetInfo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150StreamGetInfo", 5, a);
    return 0;
}

/* _xlMost150StreamInitRxFifo@12 */
unsigned long __stdcall xlMost150StreamInitRxFifo(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlMost150StreamInitRxFifo", 3, a);
    return 0;
}

/* _xlMost150StreamOpen@20 */
unsigned long __stdcall xlMost150StreamOpen(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150StreamOpen", 5, a);
    return 0;
}

/* _xlMost150StreamReceiveData@20 */
unsigned long __stdcall xlMost150StreamReceiveData(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150StreamReceiveData", 5, a);
    return 0;
}

/* _xlMost150StreamStart@28 */
unsigned long __stdcall xlMost150StreamStart(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMost150StreamStart", 7, a);
    return 0;
}

/* _xlMost150StreamStop@20 */
unsigned long __stdcall xlMost150StreamStop(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150StreamStop", 5, a);
    return 0;
}

/* _xlMost150StreamTransmitData@28 */
unsigned long __stdcall xlMost150StreamTransmitData(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMost150StreamTransmitData", 7, a);
    return 0;
}

/* _xlMost150SwitchEventSources@20 */
unsigned long __stdcall xlMost150SwitchEventSources(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SwitchEventSources", 5, a);
    return 0;
}

/* _xlMost150SwitchLed@20 */
unsigned long __stdcall xlMost150SwitchLed(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SwitchLed", 5, a);
    return 0;
}

/* _xlMost150SyncGetAllocTable@16 */
unsigned long __stdcall xlMost150SyncGetAllocTable(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150SyncGetAllocTable", 4, a);
    return 0;
}

/* _xlMost150SyncGetMute@20 */
unsigned long __stdcall xlMost150SyncGetMute(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SyncGetMute", 5, a);
    return 0;
}

/* _xlMost150SyncGetVolume@20 */
unsigned long __stdcall xlMost150SyncGetVolume(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMost150SyncGetVolume", 5, a);
    return 0;
}

/* _xlMost150SyncSetMute@24 */
unsigned long __stdcall xlMost150SyncSetMute(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlMost150SyncSetMute", 6, a);
    return 0;
}

/* _xlMost150SyncSetVolume@24 */
unsigned long __stdcall xlMost150SyncSetVolume(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlMost150SyncSetVolume", 6, a);
    return 0;
}

/* _xlMost150TwinklePowerLed@16 */
unsigned long __stdcall xlMost150TwinklePowerLed(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMost150TwinklePowerLed", 4, a);
    return 0;
}

/* _xlMostAsyncConfigureBusload@20 */
unsigned long __stdcall xlMostAsyncConfigureBusload(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostAsyncConfigureBusload", 5, a);
    return 0;
}

/* _xlMostAsyncGenerateBusload@20 */
unsigned long __stdcall xlMostAsyncGenerateBusload(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostAsyncGenerateBusload", 5, a);
    return 0;
}

/* _xlMostAsyncTransmit@20 */
unsigned long __stdcall xlMostAsyncTransmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostAsyncTransmit", 5, a);
    return 0;
}

/* _xlMostCtrlConfigureBusload@20 */
unsigned long __stdcall xlMostCtrlConfigureBusload(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostCtrlConfigureBusload", 5, a);
    return 0;
}

/* _xlMostCtrlGenerateBusload@20 */
unsigned long __stdcall xlMostCtrlGenerateBusload(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostCtrlGenerateBusload", 5, a);
    return 0;
}

/* _xlMostCtrlRxBuffer@20 */
unsigned long __stdcall xlMostCtrlRxBuffer(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostCtrlRxBuffer", 5, a);
    return 0;
}

/* _xlMostCtrlSyncAudio@28 */
unsigned long __stdcall xlMostCtrlSyncAudio(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMostCtrlSyncAudio", 7, a);
    return 0;
}

/* _xlMostCtrlSyncAudioEx@28 */
unsigned long __stdcall xlMostCtrlSyncAudioEx(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMostCtrlSyncAudioEx", 7, a);
    return 0;
}

/* _xlMostCtrlTransmit@20 */
unsigned long __stdcall xlMostCtrlTransmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostCtrlTransmit", 5, a);
    return 0;
}

/* _xlMostGenerateLightError@28 */
unsigned long __stdcall xlMostGenerateLightError(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMostGenerateLightError", 7, a);
    return 0;
}

/* _xlMostGenerateLockError@28 */
unsigned long __stdcall xlMostGenerateLockError(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMostGenerateLockError", 7, a);
    return 0;
}

/* _xlMostGetAllBypass@16 */
unsigned long __stdcall xlMostGetAllBypass(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMostGetAllBypass", 4, a);
    return 0;
}

/* _xlMostGetFrequency@16 */
unsigned long __stdcall xlMostGetFrequency(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMostGetFrequency", 4, a);
    return 0;
}

/* _xlMostGetLockStatus@16 */
unsigned long __stdcall xlMostGetLockStatus(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMostGetLockStatus", 4, a);
    return 0;
}

/* _xlMostGetRxLight@16 */
unsigned long __stdcall xlMostGetRxLight(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMostGetRxLight", 4, a);
    return 0;
}

/* _xlMostGetTimingMode@16 */
unsigned long __stdcall xlMostGetTimingMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMostGetTimingMode", 4, a);
    return 0;
}

/* _xlMostGetTxLight@16 */
unsigned long __stdcall xlMostGetTxLight(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMostGetTxLight", 4, a);
    return 0;
}

/* _xlMostReadRegister@24 */
unsigned long __stdcall xlMostReadRegister(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlMostReadRegister", 6, a);
    return 0;
}

/* _xlMostReceive@8 */
unsigned long __stdcall xlMostReceive(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlMostReceive", 2, a);
    return 0;
}

/* _xlMostSetAllBypass@20 */
unsigned long __stdcall xlMostSetAllBypass(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostSetAllBypass", 5, a);
    return 0;
}

/* _xlMostSetFrequency@20 */
unsigned long __stdcall xlMostSetFrequency(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostSetFrequency", 5, a);
    return 0;
}

/* _xlMostSetLightPower@20 */
unsigned long __stdcall xlMostSetLightPower(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostSetLightPower", 5, a);
    return 0;
}

/* _xlMostSetTimingMode@20 */
unsigned long __stdcall xlMostSetTimingMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostSetTimingMode", 5, a);
    return 0;
}

/* _xlMostSetTxLight@20 */
unsigned long __stdcall xlMostSetTxLight(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostSetTxLight", 5, a);
    return 0;
}

/* _xlMostStreamBufferAllocate@28 */
unsigned long __stdcall xlMostStreamBufferAllocate(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMostStreamBufferAllocate", 7, a);
    return 0;
}

/* _xlMostStreamBufferClearAll@20 */
unsigned long __stdcall xlMostStreamBufferClearAll(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostStreamBufferClearAll", 5, a);
    return 0;
}

/* _xlMostStreamBufferDeallocateAll@20 */
unsigned long __stdcall xlMostStreamBufferDeallocateAll(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostStreamBufferDeallocateAll", 5, a);
    return 0;
}

/* _xlMostStreamBufferSetNext@28 */
unsigned long __stdcall xlMostStreamBufferSetNext(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMostStreamBufferSetNext", 7, a);
    return 0;
}

/* _xlMostStreamClose@20 */
unsigned long __stdcall xlMostStreamClose(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostStreamClose", 5, a);
    return 0;
}

/* _xlMostStreamGetInfo@20 */
unsigned long __stdcall xlMostStreamGetInfo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostStreamGetInfo", 5, a);
    return 0;
}

/* _xlMostStreamOpen@20 */
unsigned long __stdcall xlMostStreamOpen(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostStreamOpen", 5, a);
    return 0;
}

/* _xlMostStreamStart@24 */
unsigned long __stdcall xlMostStreamStart(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlMostStreamStart", 6, a);
    return 0;
}

/* _xlMostStreamStop@20 */
unsigned long __stdcall xlMostStreamStop(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostStreamStop", 5, a);
    return 0;
}

/* _xlMostSwitchEventSources@20 */
unsigned long __stdcall xlMostSwitchEventSources(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostSwitchEventSources", 5, a);
    return 0;
}

/* _xlMostSyncGetAllocTable@16 */
unsigned long __stdcall xlMostSyncGetAllocTable(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMostSyncGetAllocTable", 4, a);
    return 0;
}

/* _xlMostSyncGetMuteStatus@20 */
unsigned long __stdcall xlMostSyncGetMuteStatus(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostSyncGetMuteStatus", 5, a);
    return 0;
}

/* _xlMostSyncGetVolumeStatus@20 */
unsigned long __stdcall xlMostSyncGetVolumeStatus(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlMostSyncGetVolumeStatus", 5, a);
    return 0;
}

/* _xlMostSyncMute@24 */
unsigned long __stdcall xlMostSyncMute(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlMostSyncMute", 6, a);
    return 0;
}

/* _xlMostSyncVolume@24 */
unsigned long __stdcall xlMostSyncVolume(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlMostSyncVolume", 6, a);
    return 0;
}

/* _xlMostTwinklePowerLed@16 */
unsigned long __stdcall xlMostTwinklePowerLed(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlMostTwinklePowerLed", 4, a);
    return 0;
}

/* _xlMostWriteRegister@28 */
unsigned long __stdcall xlMostWriteRegister(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMostWriteRegister", 7, a);
    return 0;
}

/* _xlMostWriteRegisterBit@28 */
unsigned long __stdcall xlMostWriteRegisterBit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7)
{
    unsigned long a[7];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    StubLog("xlMostWriteRegisterBit", 7, a);
    return 0;
}

/* _xlNetActivateNetwork@4 */
unsigned long __stdcall xlNetActivateNetwork(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlNetActivateNetwork", 1, a);
    return 0;
}

/* _xlNetAddVirtualPort@20 */
unsigned long __stdcall xlNetAddVirtualPort(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlNetAddVirtualPort", 5, a);
    return 0;
}

/* _xlNetCloseNetwork@4 */
unsigned long __stdcall xlNetCloseNetwork(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlNetCloseNetwork", 1, a);
    return 0;
}

/* _xlNetConnectMeasurementPoint@16 */
unsigned long __stdcall xlNetConnectMeasurementPoint(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlNetConnectMeasurementPoint", 4, a);
    return 0;
}

/* _xlNetDeactivateNetwork@4 */
unsigned long __stdcall xlNetDeactivateNetwork(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlNetDeactivateNetwork", 1, a);
    return 0;
}

/* _xlNetEthOpenNetwork@20 */
unsigned long __stdcall xlNetEthOpenNetwork(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlNetEthOpenNetwork", 5, a);
    return 0;
}

/* _xlNetEthReceive@16 */
unsigned long __stdcall xlNetEthReceive(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlNetEthReceive", 4, a);
    return 0;
}

/* _xlNetEthRequestChannelStatus@4 */
unsigned long __stdcall xlNetEthRequestChannelStatus(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlNetEthRequestChannelStatus", 1, a);
    return 0;
}

/* _xlNetEthSend@16 */
unsigned long __stdcall xlNetEthSend(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlNetEthSend", 4, a);
    return 0;
}

/* _xlNetFlushReceiveQueue@4 */
unsigned long __stdcall xlNetFlushReceiveQueue(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlNetFlushReceiveQueue", 1, a);
    return 0;
}

/* _xlNetOpenVirtualPort@16 */
unsigned long __stdcall xlNetOpenVirtualPort(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlNetOpenVirtualPort", 4, a);
    return 0;
}

/* _xlNetReleaseMACAddress@8 */
unsigned long __stdcall xlNetReleaseMACAddress(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlNetReleaseMACAddress", 2, a);
    return 0;
}

/* _xlNetRequestMACAddress@8 */
unsigned long __stdcall xlNetRequestMACAddress(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlNetRequestMACAddress", 2, a);
    return 0;
}

/* _xlNetSetNotification@12 */
unsigned long __stdcall xlNetSetNotification(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlNetSetNotification", 3, a);
    return 0;
}

/* _xlOemContact@20 */
unsigned long __stdcall xlOemContact(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlOemContact", 5, a);
    return 0;
}

/* _xlOpenDriver@0 */
unsigned long __stdcall xlOpenDriver(void)
{
    StubLog("xlOpenDriver", 0, 0);
    return 0;
}

/* _xlOpenPort@32 */
unsigned long __stdcall xlOpenPort(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8)
{
    unsigned long a[8];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    a[7] = a8;
    StubLog("xlOpenPort", 8, a);
    return 0;
}

/* _xlPopupHwConfig@8 */
unsigned long __stdcall xlPopupHwConfig(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlPopupHwConfig", 2, a);
    return 0;
}

/* _xlPopupLogHwConfig@8 */
unsigned long __stdcall xlPopupLogHwConfig(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlPopupLogHwConfig", 2, a);
    return 0;
}

/* _xlQueryETTs@16 */
unsigned long __stdcall xlQueryETTs(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlQueryETTs", 4, a);
    return 0;
}

/* _xlReadCmActLicenseData@16 */
unsigned long __stdcall xlReadCmActLicenseData(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlReadCmActLicenseData", 4, a);
    return 0;
}

/* _xlReadDebugFlagsEx@16 */
unsigned long __stdcall xlReadDebugFlagsEx(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlReadDebugFlagsEx", 4, a);
    return 0;
}

/* _xlReceive@12 */
unsigned long __stdcall xlReceive(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlReceive", 3, a);
    return 0;
}

/* _xlRegisterRemoteDevice@12 */
unsigned long __stdcall xlRegisterRemoteDevice(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlRegisterRemoteDevice", 3, a);
    return 0;
}

/* _xlReleaseRemoteDeviceInfo@4 */
unsigned long __stdcall xlReleaseRemoteDeviceInfo(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlReleaseRemoteDeviceInfo", 1, a);
    return 0;
}

/* _xlRemoteConfigureHostIf@8 */
unsigned long __stdcall xlRemoteConfigureHostIf(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlRemoteConfigureHostIf", 2, a);
    return 0;
}

/* _xlRemoteGetApplConfigAutoAdjustment@12 */
unsigned long __stdcall xlRemoteGetApplConfigAutoAdjustment(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlRemoteGetApplConfigAutoAdjustment", 3, a);
    return 0;
}

/* _xlRemoveRemoteDevice@12 */
unsigned long __stdcall xlRemoveRemoteDevice(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlRemoveRemoteDevice", 3, a);
    return 0;
}

/* _xlResetClock@4 */
unsigned long __stdcall xlResetClock(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlResetClock", 1, a);
    return 0;
}

/* _xlRtcGetTimeDate@16 */
unsigned long __stdcall xlRtcGetTimeDate(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlRtcGetTimeDate", 4, a);
    return 0;
}

/* _xlRtcSetTimeDate@16 */
unsigned long __stdcall xlRtcSetTimeDate(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlRtcSetTimeDate", 4, a);
    return 0;
}

/* _xlSentFlushTxQueue@16 */
unsigned long __stdcall xlSentFlushTxQueue(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlSentFlushTxQueue", 4, a);
    return 0;
}

/* _xlSentReceive@8 */
unsigned long __stdcall xlSentReceive(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlSentReceive", 2, a);
    return 0;
}

/* _xlSentSetConfig@20 */
unsigned long __stdcall xlSentSetConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlSentSetConfig", 5, a);
    return 0;
}

/* _xlSentSetMode@20 */
unsigned long __stdcall xlSentSetMode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlSentSetMode", 5, a);
    return 0;
}

/* _xlSentSetSupplyVoltage@20 */
unsigned long __stdcall xlSentSetSupplyVoltage(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlSentSetSupplyVoltage", 5, a);
    return 0;
}

/* _xlSentTransmit@20 */
unsigned long __stdcall xlSentTransmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlSentTransmit", 5, a);
    return 0;
}

/* _xlSetApplConfig@24 */
unsigned long __stdcall xlSetApplConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6)
{
    unsigned long a[6];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    StubLog("xlSetApplConfig", 6, a);
    return 0;
}

/* _xlSetApplConfigCount@12 */
unsigned long __stdcall xlSetApplConfigCount(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlSetApplConfigCount", 3, a);
    return 0;
}

/* _xlSetApplicationInfo@20 */
unsigned long __stdcall xlSetApplicationInfo(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlSetApplicationInfo", 5, a);
    return 0;
}

/* _xlSetApplicationTimeSync@8 */
unsigned long __stdcall xlSetApplicationTimeSync(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlSetApplicationTimeSync", 2, a);
    return 0;
}

/* _xlSetDefaultBaudrateEx@36 */
unsigned long __stdcall xlSetDefaultBaudrateEx(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9)
{
    unsigned long a[9];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    a[5] = a6;
    a[6] = a7;
    a[7] = a8;
    a[8] = a9;
    StubLog("xlSetDefaultBaudrateEx", 9, a);
    return 0;
}

/* _xlSetDeviceRegionCode@16 */
unsigned long __stdcall xlSetDeviceRegionCode(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlSetDeviceRegionCode", 4, a);
    return 0;
}

/* _xlSetGlobalTimeSync@8 */
unsigned long __stdcall xlSetGlobalTimeSync(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlSetGlobalTimeSync", 2, a);
    return 0;
}

/* _xlSetHwChannelDescription@20 */
unsigned long __stdcall xlSetHwChannelDescription(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlSetHwChannelDescription", 5, a);
    return 0;
}

/* _xlSetHwSegmentDescription@20 */
unsigned long __stdcall xlSetHwSegmentDescription(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlSetHwSegmentDescription", 5, a);
    return 0;
}

/* _xlSetNotification@12 */
unsigned long __stdcall xlSetNotification(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlSetNotification", 3, a);
    return 0;
}

/* _xlSetNotificationHandle@12 */
unsigned long __stdcall xlSetNotificationHandle(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlSetNotificationHandle", 3, a);
    return 0;
}

/* _xlSetTimerBasedNotify@8 */
unsigned long __stdcall xlSetTimerBasedNotify(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlSetTimerBasedNotify", 2, a);
    return 0;
}

/* _xlSetTimerRate@8 */
unsigned long __stdcall xlSetTimerRate(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlSetTimerRate", 2, a);
    return 0;
}

/* _xlSetTimerRateAndChannel@12 */
unsigned long __stdcall xlSetTimerRateAndChannel(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlSetTimerRateAndChannel", 3, a);
    return 0;
}

/* _xlSetToolAccessApplConfig@20 */
unsigned long __stdcall xlSetToolAccessApplConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlSetToolAccessApplConfig", 5, a);
    return 0;
}

/* _xlSetToolAccessApplConfigCount@12 */
unsigned long __stdcall xlSetToolAccessApplConfigCount(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlSetToolAccessApplConfigCount", 3, a);
    return 0;
}

/* _xlStatusReceive@8 */
unsigned long __stdcall xlStatusReceive(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlStatusReceive", 2, a);
    return 0;
}

/* _xlTsCreateClock@16 */
unsigned long __stdcall xlTsCreateClock(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlTsCreateClock", 4, a);
    return 0;
}

/* _xlTsDestroyClock@4 */
unsigned long __stdcall xlTsDestroyClock(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlTsDestroyClock", 1, a);
    return 0;
}

/* _xlTsGetClkCfgStatString@12 */
unsigned long __stdcall xlTsGetClkCfgStatString(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlTsGetClkCfgStatString", 3, a);
    return 0;
}

/* _xlTsGetClockIdString@12 */
unsigned long __stdcall xlTsGetClockIdString(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlTsGetClockIdString", 3, a);
    return 0;
}

/* _xlTsGetCurrentTime@12 */
unsigned long __stdcall xlTsGetCurrentTime(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlTsGetCurrentTime", 3, a);
    return 0;
}

/* _xlTsGetDomainTime@12 */
unsigned long __stdcall xlTsGetDomainTime(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlTsGetDomainTime", 3, a);
    return 0;
}

/* _xlTsGetProtocolString@12 */
unsigned long __stdcall xlTsGetProtocolString(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlTsGetProtocolString", 3, a);
    return 0;
}

/* _xlTsGetTimeScaleString@12 */
unsigned long __stdcall xlTsGetTimeScaleString(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlTsGetTimeScaleString", 3, a);
    return 0;
}

/* _xlTsSetHwSyncClockConfiguration@8 */
unsigned long __stdcall xlTsSetHwSyncClockConfiguration(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlTsSetHwSyncClockConfiguration", 2, a);
    return 0;
}

/* _xlUpdateDebugFlags@12 */
unsigned long __stdcall xlUpdateDebugFlags(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlUpdateDebugFlags", 3, a);
    return 0;
}

/* _xlUpdateDebugFlagsEx@16 */
unsigned long __stdcall xlUpdateDebugFlagsEx(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlUpdateDebugFlagsEx", 4, a);
    return 0;
}

/* _xlUpdateRemoteDeviceInfo@8 */
unsigned long __stdcall xlUpdateRemoteDeviceInfo(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlUpdateRemoteDeviceInfo", 2, a);
    return 0;
}

/* _xlW11pGetConfig@16 */
unsigned long __stdcall xlW11pGetConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlW11pGetConfig", 4, a);
    return 0;
}

/* _xlW11pReceive@8 */
unsigned long __stdcall xlW11pReceive(unsigned long a1, unsigned long a2)
{
    unsigned long a[2];
    a[0] = a1;
    a[1] = a2;
    StubLog("xlW11pReceive", 2, a);
    return 0;
}

/* _xlW11pRequestTemp@12 */
unsigned long __stdcall xlW11pRequestTemp(unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long a[3];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    StubLog("xlW11pRequestTemp", 3, a);
    return 0;
}

/* _xlW11pSetConfig@16 */
unsigned long __stdcall xlW11pSetConfig(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
    unsigned long a[4];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    StubLog("xlW11pSetConfig", 4, a);
    return 0;
}

/* _xlW11pTransmit@20 */
unsigned long __stdcall xlW11pTransmit(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlW11pTransmit", 5, a);
    return 0;
}

/* _xlW11pUnlockTransmission@4 */
unsigned long __stdcall xlW11pUnlockTransmission(unsigned long a1)
{
    unsigned long a[1];
    a[0] = a1;
    StubLog("xlW11pUnlockTransmission", 1, a);
    return 0;
}

/* _xlWriteCmActLicenseData@20 */
unsigned long __stdcall xlWriteCmActLicenseData(unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    unsigned long a[5];
    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = a4;
    a[4] = a5;
    StubLog("xlWriteCmActLicenseData", 5, a);
    return 0;
}
