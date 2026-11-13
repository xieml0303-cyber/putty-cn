/*
 * no-cmdline-tooltype.c - 给不带命令行前端的测试程序用的桩。
 *
 * settings 静态库里装着 cmdline.c，而 cmdline.c 引用 cmdline_error()
 * 和 cmdline_tooltype —— 这两个本来由各前端（putty.c / pscp.c /
 * psftp.c ...）提供，测试程序没有前端，不补就会链接断裂。
 *
 * 这里只在链接期填空，正常测试路径不会走到 cmdline_error()：真走到了
 * 说明测试程序在解析命令行参数，那本来就该报错退出。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "putty.h"

const unsigned cmdline_tooltype = 0;

void cmdline_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

bool set_legacy_charset_handling(bool newvalue)
{
    return false;
}
