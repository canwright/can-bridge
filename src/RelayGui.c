/*
 * RelayGui.c —— 反向桥接（真实盒子 -> Vector 虚拟通道）的控制面板
 *
 * 用法：双击运行，选好参数，点【启动中继】。
 *
 * ★ 关于"模拟成什么设备"
 *
 *   虚拟通道方案下，CANape 里显示的是 "Virtual Channel N" —— 这个名字是
 *   Vector 驱动报的，我们改不了。要改就得替换 vxlapi.dll，而那样
 *   CANape 的许可检查会读不到，软件直接退出（已实测）。
 *
 *   所以本面板能选的是"用哪块真硬件、占哪一路虚拟通道、什么波特率"，
 *   不是设备型号。功能上没有任何影响，只是 CANape 里的字样不一样。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define IDC_BACKEND   1001
#define IDC_VCHAN     1002
#define IDC_BAUD      1003
#define IDC_HWCH      1004
#define IDC_START     1010
#define IDC_STOP      1011
#define IDC_STATUS    1020

static HWND g_hBackend, g_hVchan, g_hBaud, g_hHwch, g_hStatus;
static HANDLE g_hProc = NULL;

/* 硬件后端：下拉项顺序和下面的值一一对应 */
static const char *kBackendVal[] = { "auto", "vci", "pcan", "gcan", "kvaser", "tosun" };
static const char *kBackendTxt[] = {
    "自动探测",
    "周立功（USBCAN.dll）",
    "PEAK（PCANBasic.dll）",
    "广成（ECanVci.dll）",
    "Kvaser（canlib32.dll）",
    "同星（libTSCAN.dll）"
};
static const char *kBaudTxt[] = { "125000", "250000", "500000", "1000000" };

static void SelfPath(char *out, DWORD n)
{
    GetModuleFileNameA(NULL, out, n);
    {
        char *s = strrchr(out, '\\');
        if (s) *s = 0;
    }
}

static void SetStatus(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    SetWindowTextA(g_hStatus, buf);
}

/* 把用户选的参数写进 UniVciBridge.ini（中继里的桥要按它选后端） */
static void WriteIni(int beIdx, int hwCh)
{
    char dir[MAX_PATH], p[MAX_PATH];
    FILE *f;
    SelfPath(dir, MAX_PATH);
    sprintf(p, "%s\\UniVciBridge.ini", dir);
    f = fopen(p, "wb");
    if (!f) return;
    fprintf(f, "; 由控制面板生成\r\n");
    fprintf(f, "backend=%s\r\n", kBackendVal[beIdx]);
    fprintf(f, "channels=2\r\n");
    fprintf(f, "xl_channel=%d\r\n", hwCh);
    fclose(f);
}

static void StopRelay(void)
{
    if (g_hProc) {
        TerminateProcess(g_hProc, 0);
        CloseHandle(g_hProc);
        g_hProc = NULL;
        SetStatus("已停止。");
        EnableWindow(GetDlgItem(GetParent(g_hStatus), IDC_START), TRUE);
        EnableWindow(GetDlgItem(GetParent(g_hStatus), IDC_STOP), FALSE);
    }
}

static int StartRelay(HWND hwnd)
{
    char dir[MAX_PATH], exe[MAX_PATH], cmd[1024];
    int be = (int)SendMessageA(g_hBackend, CB_GETCURSEL, 0, 0);
    int vc = (int)SendMessageA(g_hVchan, CB_GETCURSEL, 0, 0) + 1;
    int bd = (int)SendMessageA(g_hBaud, CB_GETCURSEL, 0, 0);
    int hw = (int)SendMessageA(g_hHwch, CB_GETCURSEL, 0, 0) + 1;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD devType;

    if (be < 0 || vc < 1 || bd < 0) { SetStatus("参数没选全。"); return 0; }

    WriteIni(be, hw);

    SelfPath(dir, MAX_PATH);
    sprintf(exe, "%s\\VxlRelay.exe", dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        SetStatus("找不到 VxlRelay.exe —— 它要和本程序放在一起。");
        return 0;
    }

    /* 型号：周立功用 21（2E-U），其它家由桥自己映射 */
    devType = (strcmp(kBackendVal[be], "vci") == 0) ? 21u : 21u;

    sprintf(cmd, "\"%s\" %d %s %lu", exe, vc, kBaudTxt[bd], (unsigned long)devType);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NEW_CONSOLE, NULL, dir, &si, &pi)) {
        SetStatus("启动失败（错误 %lu）。", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    g_hProc = pi.hProcess;

    SetStatus("中继已启动：虚拟通道 %d，波特率 %s，后端 %s。\n"
              "CANape 里请选【相邻的另一个】虚拟通道。",
              vc, kBaudTxt[bd], kBackendTxt[be]);
    EnableWindow(GetDlgItem(hwnd, IDC_START), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_STOP), TRUE);
    return 1;
}

static void AddCombo(HWND h, int id, const char **items, int n, int sel)
{
    HWND c = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                           0, 0, 200, 200, h, (HMENU)(INT_PTR)id, NULL, NULL);
    int i;
    for (i = 0; i < n; i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageA(c, CB_SETCURSEL, sel, 0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        CreateWindowA("STATIC", "真实硬件（后面接的那个盒子）", WS_CHILD | WS_VISIBLE,
                      16, 12, 320, 18, hwnd, NULL, NULL, NULL);
        g_hBackend = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                   16, 32, 320, 200, hwnd, (HMENU)IDC_BACKEND, NULL, NULL);
        {
            int i;
            for (i = 0; i < 6; i++) SendMessageA(g_hBackend, CB_ADDSTRING, 0, (LPARAM)kBackendTxt[i]);
            SendMessageA(g_hBackend, CB_SETCURSEL, 1, 0);   /* 默认周立功 */
        }

        CreateWindowA("STATIC", "硬件通道", WS_CHILD | WS_VISIBLE,
                      16, 66, 140, 18, hwnd, NULL, NULL, NULL);
        CreateWindowA("STATIC", "Vector 虚拟通道（我们占这一路）", WS_CHILD | WS_VISIBLE,
                      176, 66, 260, 18, hwnd, NULL, NULL, NULL);

        g_hHwch = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                16, 86, 140, 200, hwnd, (HMENU)IDC_HWCH, NULL, NULL);
        SendMessageA(g_hHwch, CB_ADDSTRING, 0, (LPARAM)"CAN1 / 第 1 路");
        SendMessageA(g_hHwch, CB_ADDSTRING, 0, (LPARAM)"CAN2 / 第 2 路");
        SendMessageA(g_hHwch, CB_SETCURSEL, 0, 0);

        g_hVchan = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                 176, 86, 160, 200, hwnd, (HMENU)IDC_VCHAN, NULL, NULL);
        {
            int i; char b[32];
            for (i = 1; i <= 4; i++) { sprintf(b, "第 %d 路", i); SendMessageA(g_hVchan, CB_ADDSTRING, 0, (LPARAM)b); }
            SendMessageA(g_hVchan, CB_SETCURSEL, 1, 0);     /* 默认第 2 路 */
        }

        CreateWindowA("STATIC", "波特率", WS_CHILD | WS_VISIBLE,
                      16, 120, 100, 18, hwnd, NULL, NULL, NULL);
        g_hBaud = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                16, 140, 140, 200, hwnd, (HMENU)IDC_BAUD, NULL, NULL);
        {
            int i; char b[32];
            for (i = 0; i < 4; i++) { sprintf(b, "%s bps", kBaudTxt[i]); SendMessageA(g_hBaud, CB_ADDSTRING, 0, (LPARAM)b); }
            SendMessageA(g_hBaud, CB_SETCURSEL, 2, 0);      /* 默认 500k */
        }

        CreateWindowA("BUTTON", "启动中继", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      16, 180, 150, 34, hwnd, (HMENU)IDC_START, NULL, NULL);
        CreateWindowA("BUTTON", "停止", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      176, 180, 100, 34, hwnd, (HMENU)IDC_STOP, NULL, NULL);
        EnableWindow(GetDlgItem(hwnd, IDC_STOP), FALSE);

        g_hStatus = CreateWindowA("STATIC",
            "说明：CANape 里会显示成 \"Virtual Channel N\"，这是 Vector 驱动的命名，改不了。\r\n"
            "      但收发功能完全正常 —— 我们的中继占用相邻的另一路虚拟通道。",
            WS_CHILD | WS_VISIBLE, 16, 226, 520, 60, hwnd, (HMENU)IDC_STATUS, NULL, NULL);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_START: StartRelay(hwnd); return 0;
        case IDC_STOP:  StopRelay();      return 0;
        }
        return 0;

    case WM_CLOSE:
        StopRelay();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    (void)hPrev; (void)cmd;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "VxlRelayGui";
    RegisterClassA(&wc);

    hwnd = CreateWindowA("VxlRelayGui",
                         "反向桥接 —— 真实盒子 -> Vector 虚拟通道",
                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                         CW_USEDEFAULT, CW_USEDEFAULT, 570, 340,
                         NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
