/*
 * CanRestore.c —— 一键恢复
 *
 * ★ 设计前提：这是"出事了"才用的工具，所以它必须
 *     · 不依赖桥（UniVciBridge.dll 坏没坏都不影响它）
 *     · 不依赖一键工具
 *     · 不依赖任何配置文件
 *     · 双击就能跑，看到结果心里有数
 *
 * 它怎么找到"被改过的东西"
 *   CAN 相关软件被改的文件，一律留了【同名备份】，扩展名两种：
 *       xxx.dll.zlg.bak          （一键工具 / 选软件装桥 留下的）
 *       xxx.dll.canbridge.bak    （以后 vxlapi 探针等新工具统一用这个）
 *   所以恢复 = 在几个软件目录里找这两种备份，还原回原名。
 *
 *   不靠清单文件：清单位置可能丢、可能被删，而备份就在现场，最可靠。
 *
 * 用法：
 *   双击                      自动找 ZCANPRO / CANape / CANoe，列出发现，问 Y/N
 *   CanRestore.exe <目录>      只处理指定目录
 *   CanRestore.exe <目录> --yes 不问，直接恢复
 *   CanRestore.exe --scan      只看不动
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <conio.h>

static int g_pass = 0, g_fail = 0;
static int g_interactive = 1;   /* 没有控制台（重定向/被脚本调用）时不暂停 */

/* 判断 stdin 是不是真的控制台。
   ★ 不能靠 isatty(stdin)：_getch() 读的是控制台本身，不走 stdin。
     所以要用 GetConsoleMode 判断。 */
static int IsConsoleInput(void)
{
    DWORD mode;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) ? 1 : 0;
}

static void Out(const wchar_t *fmt, ...)
{
    wchar_t wbuf[4096];
    char    u8[16384];
    va_list ap;
    int wl, al;

    va_start(ap, fmt);
    wl = _vsnwprintf(wbuf, 4095, fmt, ap);
    va_end(ap);
    if (wl <= 0) return;
    wbuf[4095] = L'\0';
    if (wl > 4000) wl = 4000;
    al = WideCharToMultiByte(CP_UTF8, 0, wbuf, wl, u8, (int)sizeof(u8) - 1, NULL, NULL);
    if (al <= 0) return;
    u8[al] = 0;
    fputs(u8, stdout);
    fflush(stdout);
}

static int IsDir(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void SelfDir(wchar_t *out, DWORD cch)
{
    GetModuleFileNameW(NULL, out, cch);
    {
        wchar_t *s = wcsrchr(out, L'\\');
        if (s) *s = 0;
    }
}

/*----------------------------------------------------------------------------
  在一个目录里找 "xxx.<后缀>.bak"，还原成 xxx
----------------------------------------------------------------------------*/
static int RestoreInDir(const wchar_t *dir, int apply, int *found)
{
    static const wchar_t *suffixes[] = { L".zlg.bak", L".canbridge.bak" };
    int i, restored = 0;

    for (i = 0; i < 2; i++) {
        wchar_t pat[MAX_PATH * 2];
        WIN32_FIND_DATAW fd;
        HANDLE h;

        _snwprintf(pat, MAX_PATH * 2 - 1, L"%s\\*%s", dir, suffixes[i]);
        pat[MAX_PATH * 2 - 1] = L'\0';

        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            wchar_t bak[MAX_PATH * 2], orig[MAX_PATH * 2];
            size_t len;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            _snwprintf(bak, MAX_PATH * 2 - 1, L"%s\\%s", dir, fd.cFileName);
            bak[MAX_PATH * 2 - 1] = L'\0';

            /* 去掉后缀 = 原始文件名 */
            wcsncpy(orig, bak, MAX_PATH * 2 - 1);
            orig[MAX_PATH * 2 - 1] = L'\0';
            len = wcslen(orig);
            len -= wcslen(suffixes[i]);
            orig[len] = L'\0';

            (*found)++;
            Out(L"  发现备份  %s\n", fd.cFileName);
            Out(L"      原名  %s\n", orig + wcslen(dir) + 1);

            if (!apply) continue;

            {
                /* 备一份现场（万一原名那份是好的，别丢了） */
                wchar_t now[MAX_PATH * 2];
                DWORD e;

                _snwprintf(now, MAX_PATH * 2 - 1, L"%s.replaced", orig);
                now[MAX_PATH * 2 - 1] = L'\0';

                SetLastError(0);
                if (GetFileAttributesW(orig) != INVALID_FILE_ATTRIBUTES) {
                    if (!CopyFileW(orig, now, FALSE)) {
                        e = GetLastError();
                        Out(L"      · 现场备份失败(%lu)，继续尝试还原\n", (unsigned long)e);
                    }
                }

                SetLastError(0);
                if (CopyFileW(bak, orig, FALSE)) {
                    Out(L"      ✓ 已还原\n");
                    DeleteFileW(bak);
                    restored++;
                } else {
                    e = GetLastError();
                    Out(L"      ✗ 还原失败(%lu): CopyFile(\"%s\" -> \"%s\")\n",
                        (unsigned long)e, bak, orig);
                    if (e == 5) {
                        Out(L"         拒绝访问。常见原因：\n");
                        Out(L"           · 目标文件是只读属性\n");
                        Out(L"           · 目标被进程占用\n");
                    }
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    return restored;
}

/*----------------------------------------------------------------------------
  在某个根目录下浅层找 ZCANPRO.exe / CANape*.exe / CANoe*.exe，返回它们所在目录
----------------------------------------------------------------------------*/
static int FindAppsUnder(const wchar_t *root, int depth,
                         wchar_t dirs[][MAX_PATH * 2], int maxDirs, int ndirs)
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (depth <= 0 || ndirs >= maxDirs) return ndirs;
    if (!IsDir(root)) return ndirs;

    /* 本层：有没有目标 exe */
    {
        static const wchar_t *exes[] = { L"ZCANPRO.exe", L"CANape*.exe", L"CANoe*.exe" };
        int i;
        for (i = 0; i < 3 && ndirs < maxDirs; i++) {
            wchar_t p[MAX_PATH * 2];
            _snwprintf(p, MAX_PATH * 2 - 1, L"%s\\%s", root, exes[i]);
            p[MAX_PATH * 2 - 1] = L'\0';
            h = FindFirstFileW(p, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                int k, dup = 0;
                FindClose(h);
                for (k = 0; k < ndirs; k++) if (_wcsicmp(dirs[k], root) == 0) dup = 1;
                if (!dup) {
                    wcsncpy(dirs[ndirs], root, MAX_PATH * 2 - 1);
                    dirs[ndirs][MAX_PATH * 2 - 1] = L'\0';
                    ndirs++;
                }
                break;
            }
        }
    }

    /* 下钻一层 */
    _snwprintf(pat, MAX_PATH * 2 - 1, L"%s\\*", root);
    pat[MAX_PATH * 2 - 1] = L'\0';
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return ndirs;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (ndirs >= maxDirs) break;
        {
            wchar_t sub[MAX_PATH * 2];
            _snwprintf(sub, MAX_PATH * 2 - 1, L"%s\\%s", root, fd.cFileName);
            sub[MAX_PATH * 2 - 1] = L'\0';
            ndirs = FindAppsUnder(sub, depth - 1, dirs, maxDirs, ndirs);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    return ndirs;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t dirs[32][MAX_PATH * 2];
    int ndirs = 0, i;
    int apply = 0, found = 0, restored = 0;
    wchar_t self[MAX_PATH * 2];

    SetConsoleOutputCP(65001);
    g_interactive = IsConsoleInput();
    SelfDir(self, MAX_PATH * 2);

    Out(L"\n");
    Out(L"  ══════════════════════════════════════════════════════\n");
    Out(L"   CAN 万能桥 · 一键恢复\n");
    Out(L"  ══════════════════════════════════════════════════════\n\n");
    Out(L"   作用：把被桥/探针替换过的文件，还原成厂家的原版。\n");
    Out(L"   原理：找到所有 xxx.dll.zlg.bak / xxx.dll.canbridge.bak，\n");
    Out(L"         把它改名回 xxx.dll（原本那份会另存为 .replaced）。\n\n");

    /* 参数 */
    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--yes") == 0)        apply = 1;
        else if (_wcsicmp(argv[i], L"--scan") == 0)  apply = 0;
        else if (argv[i][0] != L'-' && ndirs < 32) {
            wcsncpy(dirs[ndirs], argv[i], MAX_PATH * 2 - 1);
            dirs[ndirs][MAX_PATH * 2 - 1] = L'\0';
            ndirs++;
        }
    }

    /* 没给目录就自动找 */
    if (ndirs == 0) {
        static const wchar_t *roots[] = {
            L"C:\\", L"D:\\", L"E:\\", L"F:\\",
            L"C:\\Program Files", L"C:\\Program Files (x86)"
        };
        Out(L" 正在查找 ZCANPRO / CANape / CANoe …\n\n");
        for (i = 0; i < (int)(sizeof(roots) / sizeof(roots[0])) && ndirs < 32; i++)
            ndirs = FindAppsUnder(roots[i], 3, dirs, 32, ndirs);

        if (ndirs == 0) {
            Out(L" 没找到这几个软件。\n\n");
            Out(L" 你可以把要恢复的目录直接拖到本程序上。\n\n");
            Out(L" 按任意键退出…\n");
            if (g_interactive) _getch();
            return 0;
        }
    }

    Out(L" 要检查 %d 个目录：\n\n", ndirs);
    for (i = 0; i < ndirs; i++) Out(L"   [%d] %s\n", i + 1, dirs[i]);
    Out(L"\n");

    if (!apply) {
        Out(L" ── 只看不动（要真恢复请加 --yes，或直接双击后按 Y）──\n\n");
    }

    Out(L" ── 扫描备份 ──\n");
    for (i = 0; i < ndirs; i++) {
        Out(L"\n [%d] %s\n", i + 1, dirs[i]);
        restored += RestoreInDir(dirs[i], apply && 1, &found);
    }

    Out(L"\n ── 顺带清理桥自己留下的文件（不影响厂家软件）──\n");
    for (i = 0; i < ndirs; i++) {
        static const wchar_t *junk[] = {
            L"UniVciBridge.dll", L"UniVciBridge.ini",
            L"UniVciBridge.log", L"vxlapi_stub.log"
        };
        int k;
        for (k = 0; k < 4; k++) {
            wchar_t p[MAX_PATH * 2];
            _snwprintf(p, MAX_PATH * 2 - 1, L"%s\\%s", dirs[i], junk[k]);
            p[MAX_PATH * 2 - 1] = L'\0';
            if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
                if (apply) {
                    if (DeleteFileW(p)) Out(L"   已删除 %s\n", junk[k]);
                    else Out(L"   删不掉 %s（被占用？）\n", junk[k]);
                } else {
                    Out(L"   会删除 %s\n", junk[k]);
                }
            }
        }
    }

    Out(L"\n  ══════════════════════════════════════════════════════\n");
    if (!apply) {
        Out(L"   发现 %d 个备份，尚未动任何文件。\n", found);
        Out(L"   要恢复，按 Y；其它键退出。\n\n");
        {
            int ch = g_interactive ? _getch() : (int)'n';
            if (ch != 'y' && ch != 'Y') {
                Out(L"   已取消，什么都没动。\n\n");
                Out(L" 按任意键退出…\n");
                if (g_interactive) _getch();
                return 0;
            }
        }
        Out(L"\n   开始恢复…\n\n");
        restored = 0;
        for (i = 0; i < ndirs; i++) restored += RestoreInDir(dirs[i], 1, &found);
        for (i = 0; i < ndirs; i++) {
            static const wchar_t *junk[] = {
                L"UniVciBridge.dll", L"UniVciBridge.ini",
                L"UniVciBridge.log", L"vxlapi_stub.log"
            };
            int k;
            for (k = 0; k < 4; k++) {
                wchar_t p[MAX_PATH * 2];
                _snwprintf(p, MAX_PATH * 2 - 1, L"%s\\%s", dirs[i], junk[k]);
                p[MAX_PATH * 2 - 1] = L'\0';
                DeleteFileW(p);
            }
        }
    }

    Out(L"   %d 个备份，已还原 %d 个\n", found, restored);
    Out(L"   原来的文件另存为 xxx.replaced，确认没问题后可以手动删掉。\n");
    Out(L"\n   现在可以直接打开厂家的软件试试了。\n");
    Out(L"\n  ══════════════════════════════════════════════════════\n\n");
    Out(L" 按任意键退出…\n");
    if (g_interactive) _getch();
    return 0;
}
