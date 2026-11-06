/*
 * Stub: 文件传输（SFTP 双栏窗口）只在带 SSH 的 putty 里存在。
 *
 * puttytel 是纯 Telnet 客户端，不链接 sshclient/sftp，但它和 putty 共用
 * window.c 与 putty.c，那里会引用传输入口。给一套空实现让链接通过；
 * puttytel 的菜单项本来就是灰的，点不到。
 */

#include "putty.h"

char *sftp_window_open(Conf *conf, HWND parent)
{
    (void)conf;
    (void)parent;
    return dupstr("此程序不支持文件传输。");
}

bool sftp_window_is_open(void)
{
    return false;
}

void sftp_window_detached(Conf *conf)
{
    (void)conf;
}

void sftp_selftest_list(Conf *conf, const char *path)
{
    (void)conf;
    (void)path;
}

void sftp_selftest_window(Conf *conf)
{
    (void)conf;
}

void sftp_selftest_open_fail(Conf *conf)
{
    (void)conf;
}

void sftp_selftest_enable_log(const char *path)
{
    (void)path;
}

void sftp_selftest_recurse_run(Conf *conf, const char *src, const char *dst)
{
    (void)conf;
    (void)src;
    (void)dst;
}

void sftp_selftest_resume_run(Conf *conf, const char *localpath,
                              const char *remotepath)
{
    (void)conf;
    (void)localpath;
    (void)remotepath;
}
