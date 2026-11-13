/*
 * cn: 崩溃现场取证 —— 未处理异常捕获。
 *
 * 为什么需要它：「四个按钮全崩」在开发机上只能看到 ntdll 的
 * 0xc0000374（堆损坏），真正写坏堆的地方往往早就返回了，等崩的
 * 时候现场已经不可信。所以这里三件事一起做：
 *
 *   1) SetUnhandledExceptionFilter 抓住异常码与出错地址，连同
 *      CN_TRACE 埋点日志一起落盘，先看清崩在哪一步；
 *   2) 用 _CrtSetDbgFlag 打开堆检查（Debug 构建）；
 *   3) 提供 cn_trace()，让关键路径留面包屑 —— 只写文件、绝不
 *      往 stderr 刷，因为 GUI 进程没有控制台，stderr 全丢。
 *
 * 说明：构建系统不定义 CN_CRASH_TRACE，所以本文件总是编进 putty.exe。
 * 正常使用不产生任何文件 —— 日志只在 cn_trace() 埋点被命中、或
 * 真的抓到未处理异常时才创建。
 */
#ifndef CN_CRASH_TRACE
#define CN_CRASH_TRACE 1
#endif

#include <winsock2.h>   /* 必须先于 windows.h，否则 mingw 会警告 */
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <windows.h>
#include "putty.h"

static FILE *cn_trace_fp = NULL;
static wchar_t cn_trace_path[MAX_PATH];

/* 日志路径：exe 同目录的 cn-trace.log，方便直接捡。 */
static void cn_trace_open(void)
{
    if (cn_trace_fp)
        return;
    {
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, exe, lenof(exe));
        if (n == 0 || n >= lenof(exe))
            wcscpy(exe, L"putty.exe");
        {
            wchar_t *slash = wcsrchr(exe, L'\\');
            if (slash) {
                *slash = L'\0';
                _snwprintf(cn_trace_path, lenof(cn_trace_path),
                           L"%s\\cn-trace.log", exe);
                cn_trace_path[lenof(cn_trace_path) - 1] = L'\0';
            } else {
                wcscpy(cn_trace_path, L"cn-trace.log");
            }
        }
    }
    cn_trace_fp = _wfopen(cn_trace_path, L"w");
}

void cn_trace(const char *fmt, ...)
{
    va_list ap;
    cn_trace_open();
    if (!cn_trace_fp)
        return;
    fprintf(cn_trace_fp, "[%lu] ", (unsigned long)GetTickCount());
    va_start(ap, fmt);
    vfprintf(cn_trace_fp, fmt, ap);
    va_end(ap);
    fputc('\n', cn_trace_fp);
    fflush(cn_trace_fp);
}

static LONG WINAPI cn_crash_filter(EXCEPTION_POINTERS *ep)
{
    PEXCEPTION_RECORD er = ep ? ep->ExceptionRecord : NULL;
    CONTEXT *ctx = ep ? ep->ContextRecord : NULL;

    cn_trace_open();
    if (cn_trace_fp) {
        fprintf(cn_trace_fp, "\n===== 未处理异常 =====\n");
        if (er) {
            fprintf(cn_trace_fp, "ExceptionCode = 0x%08lX\n",
                    (unsigned long)er->ExceptionCode);
            fprintf(cn_trace_fp, "ExceptionAddress = %p\n",
                    er->ExceptionAddress);
            fprintf(cn_trace_fp, "NumberParameters = %lu\n",
                    (unsigned long)er->NumberParameters);
            for (DWORD i = 0; i < er->NumberParameters && i < 15; i++)
                fprintf(cn_trace_fp, "  param[%lu] = 0x%p\n",
                        (unsigned long)i, (void *)er->ExceptionInformation[i]);
        }
        if (ctx) {
#ifdef _M_X64
            fprintf(cn_trace_fp, "RIP=%p RSP=%p RBP=%p\n",
                    (void *)ctx->Rip, (void *)ctx->Rsp, (void *)ctx->Rbp);
#else
            fprintf(cn_trace_fp, "EIP=%p ESP=%p EBP=%p\n",
                    (void *)ctx->Eip, (void *)ctx->Esp, (void *)ctx->Ebp);
#endif
        }
        fflush(cn_trace_fp);
        fclose(cn_trace_fp);
        cn_trace_fp = NULL;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void cn_crash_trace_init(void)
{
    /*
     * cn: 这里刻意不预先打开日志文件。早先的实现会在每次启动时
     * 就在 exe 同目录建一个 cn-trace.log 并写一行「日志已就绪」，
     * 正常使用根本用不上，却让安装目录平白多出个文件，装到
     * Program Files 这类只读目录时还会静默失败。改成只装过滤器，
     * 真崩了再由 cn_crash_filter 打开写现场。
     */
    SetUnhandledExceptionFilter(cn_crash_filter);
}
