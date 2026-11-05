/*
 * ProbeTag.dll —— 只导出一个函数，返回一个标记字符串
 * 用来验证 App.exe.local 目录重定向是否还有效
 */
__declspec(dllexport) const char *ProbeTag(void)
{
    return "MACRO_PLACEHOLDER";
}
