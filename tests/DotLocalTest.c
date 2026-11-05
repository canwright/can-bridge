/*
 * DotLocalTest.c —— 验证 App.exe.local 目录重定向在 Win11 上是否还有效
 *
 * 做法：
 *   同目录放两个 probe_tag.dll：
 *       测试目录\probe_tag.dll          -> 标记 "APPDIR"
 *       测试目录\DotLocalTest.exe.local\probe_tag.dll -> 标记 "LOCALDIR"
 *
 *   本程序只按【裸名】LoadLibrary("probe_tag.dll")，打印拿到的标记：
 *       拿到 LOCALDIR  -> .local 重定向有效，可以用它来装 vxlapi 而不碰原文件
 *       拿到 APPDIR    -> .local 已被系统忽略，只能走"改程序目录"
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int main(void)
{
    HMODULE h;
    const char *(__stdcall *pTag)(void);

    printf("=== App.exe.local 重定向测试 ===\n\n");

    h = LoadLibraryA("probe_tag.dll");
    if (!h) {
        printf("  加载 probe_tag.dll 失败 (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    pTag = (const char *(__stdcall *)(void))GetProcAddress(h, "ProbeTag");
    if (!pTag) { printf("  找不到 ProbeTag\n"); return 1; }

    printf("  LoadLibrary(\"probe_tag.dll\") 得到: %s\n", pTag());

    /* 再把实际加载的路径打出来，双重确认 */
    {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(h, path, MAX_PATH);
        printf("  实际加载的模块路径: %s\n", path);
    }

    printf("\n=== 完成 ===\n");
    return 0;
}
