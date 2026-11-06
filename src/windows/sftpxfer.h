/*
 * sftpxfer.h: 在 PuTTY 窗口内提供文件传输能力。
 *
 * 这一层只负责「说 SFTP」：独立连接、协议握手、目录与文件操作。
 * 界面怎么摆、按钮放哪，全在 sftpwin.c 里，两边通过本头文件对接。
 */

#ifndef PUTTY_WIN_SFTPXFER_H
#define PUTTY_WIN_SFTPXFER_H

#include "putty.h"

struct fxp_attrs;

typedef struct SftpXfer SftpXfer;

/*
 * 一条目录项。两端（本地 / 远端）共用这个结构，界面就能用同一套
 * 代码画两个列表。
 */
typedef struct SftpEntry {
    char *name;                 /* 文件名，不含路径 */
    bool is_dir;
    /*
     * 符号链接标记。
     *
     * 远端靠 LSTAT 的 S_ISLNK 位判定；本地靠 Win32 的
     * FILE_ATTRIBUTE_REPARSE_POINT。递归传输据此跳过链接目录 ——
     * 指向目录的链接若照常钻进去，遇到 a -> .. 这种会绕回自己，
     * 只能等深度上限才停，既慢又可能把同一批文件传很多遍。
     */
    bool is_symlink;
    uint64_t size;              /* 目录无意义 */
    unsigned long mtime;        /* Unix 时间戳，0 表示未知 */
} SftpEntry;

/* 释放一批目录项。 */
void sftp_entries_free(SftpEntry *entries, size_t count);

/*
 * 进度回调。done 是已传字节数，total 是总字节数（0 表示未知）。
 * 返回 false 表示「停下来」，传输会尽快收尾并按取消处理。
 */
typedef bool (*SftpProgressFn)(void *ctx, uint64_t done, uint64_t total);

/* 建立独立 SFTP 连接。失败时返回 NULL，并把原因写进 *errmsg。 */
SftpXfer *sftp_xfer_open(Conf *conf, HWND hwnd, char **errmsg);

/* 协议握手，取得远端起始目录。 */
bool sftp_xfer_init(SftpXfer *sx);

void sftp_xfer_close(SftpXfer *sx);

/* 远端当前目录。返回的内存由调用者释放。 */
char *sftp_xfer_pwd(SftpXfer *sx);

/*
 * 列目录。返回条目数组共 *count 项，调用者用 sftp_entries_free 释放。
 * 传 "." 之类的相对路径也支持，服务器会按当前目录解析。
 */
SftpEntry *sftp_xfer_list(SftpXfer *sx, const char *path, size_t *count,
                          char **errmsg);

bool sftp_xfer_mkdir(SftpXfer *sx, const char *path, char **errmsg);
bool sftp_xfer_rmdir(SftpXfer *sx, const char *path, char **errmsg);
bool sftp_xfer_remove(SftpXfer *sx, const char *path, char **errmsg);
bool sftp_xfer_rename(SftpXfer *sx, const char *src, const char *dst,
                      char **errmsg);
bool sftp_xfer_stat(SftpXfer *sx, const char *path, struct fxp_attrs *attrs);

/*
 * 传输结果。
 *
 * 分四种而不是 bool，是因为「暂停」和「终止」对半截文件的处理正好
 * 相反：暂停要留着断点等着续，终止要把它删干净。调用方必须能分辨。
 */
typedef enum {
    XFER_OK,                    /* 完整传完 */
    XFER_FAIL,                  /* 出错，半截文件已清理 */
    XFER_PAUSED,                /* 用户暂停，半截文件保留供续传 */
    XFER_CANCELLED              /* 用户终止，半截文件已删除 */
} XferOutcome;

/*
 * 传输。两端都是本机能直接打开的路径 / 远端路径。
 *
 * allow_resume 决定目标端已有同名文件时怎么办：
 *   真 —— 目标比源小就从断点接着传，目标等于源就视为已传完直接跳过
 *         （零字节重发）。这是「暂停后点续传」该走的路径。
 *   假 —— 一律从头覆盖，不看目标端现有内容。用户明确选了「覆盖」、
 *         或者这是一次全新的传输，都走这条：此时目标端哪怕大小恰好
 *         相同也绝不能跳过 —— 内容可能根本不一样，跳过等于假装传完了。
 */
XferOutcome sftp_xfer_download(SftpXfer *sx, const char *remotepath,
                               const char *localpath, bool allow_resume,
                               SftpProgressFn progress,
                               void *progctx, char **errmsg);
XferOutcome sftp_xfer_upload(SftpXfer *sx, const char *localpath,
                             const char *remotepath, bool allow_resume,
                             SftpProgressFn progress,
                             void *progctx, char **errmsg);

/* 请求取消正在进行的传输（终止语义：半截文件会被删掉）。 */
void sftp_xfer_request_cancel(SftpXfer *sx);

/*
 * 用 LSTAT 复核一个远端路径是不是符号链接。
 * 递归进目录前调用，避免把链接目录当真实目录钻进去绕成环。
 * LSTAT 不可用或失败时返回 false（按非链接处理）。
 */
bool sftp_xfer_is_symlink(SftpXfer *sx, const char *path);

/* 取会话的 home 目录（REALPATH "." 的结果）；未连接时返回 NULL。 */
const char *sftp_xfer_homedir(SftpXfer *sx);

/* 清掉取消标志，开始新传输前调用。 */
void sftp_xfer_reset_cancel(SftpXfer *sx);

/* 请求暂停正在进行的传输：当前文件停下并保留断点。 */
void sftp_xfer_request_pause(SftpXfer *sx);

/* 清掉暂停标志，续传或开始新传输前调用。 */
void sftp_xfer_reset_pause(SftpXfer *sx);

/* 取回连接级错误（如致命断线），没有则返回 NULL。 */
char *sftp_xfer_error(SftpXfer *sx);

/* ----------------------------------------------------------------------
 * 本地文件系统。条目结构和远端一致，界面能复用同一套绘制代码。
 */

SftpEntry *sftp_local_list(const char *path, size_t *count, char **errmsg);
bool sftp_local_mkdir(const char *path, char **errmsg);
bool sftp_local_rmdir(const char *path, char **errmsg);
bool sftp_local_remove(const char *path, char **errmsg);
bool sftp_local_rename(const char *src, const char *dst, char **errmsg);

/* 拼路径，会自动处理分隔符（本地用 \，远端用 /）。 */
char *cn_path_join(const char *dir, const char *name, bool remote);
/* 取上级目录，已到根则返回根。 */
char *cn_path_parent(const char *path, bool remote);

/* ----------------------------------------------------------------------
 * 双栏传输窗口。整块界面都在 sftpwin.c 里。
 */

/* 返回 NULL 表示成功，否则为调用方须 sfree 的错误文案；内部不弹模态框。 */
char *sftp_window_open(Conf *conf, HWND parent);

/*
 * 传输窗口当前是否已开（含正在打开中）。配置框点「文件传输」后就地
 * 打开传输窗，靠它判断成功与否决定是否结束配置框。
 */
bool sftp_window_is_open(void);

/*
 * 没有主消息循环时用它：打开窗口并自己跑循环，窗口关掉才返回。
 * 供配置框里点「文件传输」这条独立启动路径使用。
 */
void sftp_window_detached(Conf *conf);

/* 界面自检入口，供 -sftp-win 参数调用。 */
void sftp_selftest_window(Conf *conf);

/*
 * 打开失败自检，供 -sftp-open-fail 参数调用。
 * 故意用不可达主机让连接失败，验证「不弹模态框、不残留窗口、
 * 错误文案能返回」这三条防复发断言。
 */
void sftp_selftest_open_fail(Conf *conf);

/* 自检时把握手日志写到指定文件，方便定位卡在哪一步。 */
void sftp_selftest_enable_log(const char *path);
/*
 * cn: 自检/日志输出统一落到 exe 所在目录（工作目录不可控，相对
 * 路径会把报告散落在调用者的 cwd）。返回调用方须 sfree 的路径。
 */
char *sftp_selftest_outpath(const char *name);

#endif /* PUTTY_WIN_SFTPXFER_H */
