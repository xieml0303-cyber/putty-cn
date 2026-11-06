/*
 * sftpxfer.c: 在 PuTTY 窗口内提供文件传输能力。
 *
 * 设计要点（参考 WinSCP 的 TSecureShell::EventSelectLoop）：
 *
 *  - SFTP 需要一条独立的 SSH 连接，不复用终端那条。这样终端主链路
 *    完全不受影响，坏了也不会拖累会话。
 *
 *  - PuTTY 的 sftp.c 协议层假设 sftp_recvdata() 会阻塞到收够字节。
 *    psftp 靠自己的阻塞循环满足它，但那是命令行程序。GUI 里直接在
 *    主线程死等会把窗口冻住，所以这里用「嵌套消息泵」：传输过程中
 *    原地跑一个小循环，抽消息、跑定时器、驱动顶层回调，数据到了就
 *    跳出，把控制权交回 sftp_recvdata()。
 *
 *  - 前提是网络事件能被主线程取到。PuTTY 的 select-gui.c 用
 *    WSAAsyncSelect 把网络活动转成 WM_NETEVENT 窗口消息，正好由上面
 *    那个嵌套泵抽出来，因此完全不用改 select 层。
 */

#include <winsock2.h> /* 必须放在最前面 */
#include <assert.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <direct.h>

#include "putty.h"
#include "ssh.h"
#include "ssh/sftp.h"
#include "win-gui-seat.h"
#include "sftpxfer.h"

/* ----------------------------------------------------------------------
 * 连接状态。
 */

typedef struct SftpXfer {
    Backend *backend;
    Conf *conf;
    LogContext *logctx;
    Seat seat;
    HWND hwnd;                  /* 用于弹框的父窗口，可为 NULL */

    bufchain received_data;     /* 后端吐给我们的原始字节 */

    bool backend_error;
    char *error_message;

    char *pwd;                  /* 远端当前目录 */
    char *homedir;

    bool cancel_requested;      /* 传输中被打断（终止语义：丢弃半截） */
    bool pause_requested;       /* 传输中要求暂停（保留断点） */
} SftpXfer;

static SftpXfer *sftp_active = NULL;

void sftp_xfer_close(SftpXfer *sx);
static HWND sftp_ensure_hidden_window(void);

/* ----------------------------------------------------------------------
 * 日志策略。
 *
 * logevent() 会无条件解引用 policy，传 NULL 必崩，所以两条路径都得给
 * 一个真实的 policy。
 */

/* 正常路径：用 putty 自己的 GUI policy，日志进进程内环形缓冲。 */
static LogPolicy sftp_gui_logpolicy[1];

/* 自测路径：把日志事件直接写进文件。 */
static FILE *sftp_selftest_logfp = NULL;
static LogPolicy sftp_selftest_logpolicy[1];

/* 供界面自检打开日志，否则卡住时看不到任何握手细节。 */
void sftp_selftest_enable_log(const char *path)
{
    if (!sftp_selftest_logfp)
        sftp_selftest_logfp = fopen(path, "w");
}

/*
 * cn: 自检输出统一写到 exe 所在目录。自检跑的时候工作目录不可控
 * （从仓库根、从别的目录都能启动），报告写相对路径就会散落在
 * 调用者的 cwd——2026-11-07 曾在仓库根捡到 sftp-log.txt 等残留。
 * 返回值是调用方须 sfree 的完整路径。
 */
char *sftp_selftest_outpath(const char *name)
{
    wchar_t exedir[MAX_PATH];
    wchar_t *slash;
    char *dir, *out;

    if (GetModuleFileNameW(NULL, exedir, MAX_PATH) == 0)
        return dupstr(name);            /* 拿不到 exe 路径就退回 cwd */
    slash = wcsrchr(exedir, L'\\');
    if (slash)
        *slash = L'\0';
    dir = dup_wc_to_mb(CP_UTF8, exedir, "?");
    out = dupprintf("%s\\%s", dir, name);
    sfree(dir);
    return out;
}

static void sftp_selftest_eventlog(LogPolicy *lp, const char *string)
{
    if (sftp_selftest_logfp) {
        fprintf(sftp_selftest_logfp, "LOG: %s\n", string);
        fflush(sftp_selftest_logfp);
    }
}

static int sftp_selftest_askappend(LogPolicy *lp, Filename *filename,
                                   void (*callback)(void *ctx, int result),
                                   void *ctx)
{
    return 2;                     /* 2 = 覆盖已有日志文件 */
}

static void sftp_selftest_logging_error(LogPolicy *lp, const char *event)
{
    sftp_selftest_eventlog(lp, event);
}

static const LogPolicyVtable sftp_selftest_logpolicy_vt = {
    .eventlog = sftp_selftest_eventlog,
    .askappend = sftp_selftest_askappend,
    .logging_error = sftp_selftest_logging_error,
    .verbose = null_lp_verbose_yes,
};

static LogPolicy sftp_gui_logpolicy[1];

/* ----------------------------------------------------------------------
 * Seat 实现：只实现传输需要的部分，其余留空。
 */

static size_t sftp_output(Seat *seat, SeatOutputType type,
                          const void *data, size_t len)
{
    SftpXfer *sx = container_of(seat, SftpXfer, seat);
    bufchain_add(&sx->received_data, data, len);
    if (sftp_selftest_logfp) {
        fprintf(sftp_selftest_logfp, "OUTPUT: type=%d len=%d total=%d\n",
                (int)type, (int)len, (int)bufchain_size(&sx->received_data));
        fflush(sftp_selftest_logfp);
    }
    return bufchain_size(&sx->received_data);
}

static bool sftp_eof(Seat *seat)
{
    /* 连接被对端关掉时不要直接崩，交给上层判错。 */
    return false;
}

static void sftp_connection_fatal(Seat *seat, const char *message)
{
    SftpXfer *sx = container_of(seat, SftpXfer, seat);
    sx->backend_error = true;
    sfree(sx->error_message);
    sx->error_message = dupstr(message);
}

static void sftp_nonfatal(Seat *seat, const char *message)
{
    (void)seat;
    (void)message;
}

static void sftp_notify_remote_exit(Seat *seat)
{
    SftpXfer *sx = container_of(seat, SftpXfer, seat);
    sx->backend_error = true;
}

static void sftp_notify_remote_disconnect(Seat *seat)
{
    SftpXfer *sx = container_of(seat, SftpXfer, seat);
    sx->backend_error = true;
}

/*
 * cn: 传输连接的认证应答。
 *
 * 原来是 nullseat_get_userpass_input —— 后端一问密码就完蛋，传输窗口
 * 里没有任何能输入的地方，表现是连上但一直卡着不动。
 *
 * 现在先走 cmdline 那条通道（配置框上「凭据（密码）」框填的值就存在
 * 那儿，见 cmdline_set_password），拿到就拿它应答；拿不到说明用户没填
 * 密码，此时退回 null 语义 —— 密钥认证能过就过，过不了就明确报错，
 * 不让窗口无声地僵在那里。
 */
static SeatPromptResult sftp_seat_get_userpass_input(Seat *seat, prompts_t *p)
{
    static cmdline_get_passwd_input_state cmdline_state =
        CMDLINE_GET_PASSWD_INPUT_STATE_INIT;
    SeatPromptResult spr;

    (void)seat;
    spr = cmdline_get_passwd_input(p, &cmdline_state, false);
    return spr;
}

static const SeatVtable sftp_seat_vt = {
    .output = sftp_output,
    .eof = sftp_eof,
    .sent = nullseat_sent,
    .banner = nullseat_banner,
    .get_userpass_input = sftp_seat_get_userpass_input,
    .notify_session_started = nullseat_notify_session_started,
    .notify_remote_exit = sftp_notify_remote_exit,
    .notify_remote_disconnect = sftp_notify_remote_disconnect,
    .connection_fatal = sftp_connection_fatal,
    .nonfatal = sftp_nonfatal,
    .update_specials_menu = nullseat_update_specials_menu,
    .get_ttymode = nullseat_get_ttymode,
    .set_busy_status = nullseat_set_busy_status,
    .confirm_ssh_host_key = nullseat_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = nullseat_confirm_weak_crypto_primitive,
    .confirm_weak_cached_hostkey = nullseat_confirm_weak_cached_hostkey,
    .prompt_descriptions = nullseat_prompt_descriptions,
    .is_utf8 = nullseat_is_never_utf8,
    .echoedit_update = nullseat_echoedit_update,
    .get_display = nullseat_get_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = nullseat_get_window_pixel_size,
    .stripctrl_new = nullseat_stripctrl_new,
    .set_trust_status = nullseat_set_trust_status,
    .can_set_trust_status = nullseat_can_set_trust_status_no,
    .has_mixed_input_stream = nullseat_has_mixed_input_stream_no,
    .verbose = nullseat_verbose_no,
    .interactive = nullseat_interactive_no,
    .get_cursor_position = nullseat_get_cursor_position,
};

/* ----------------------------------------------------------------------
 * 嵌套消息泵。
 *
 * 每调用一次，处理最多 MSEC 毫秒或直到有新网络数据到达。
 * 返回 true 表示「有新数据」，false 表示「等超时了」。
 */

static bool sftp_pump(int msec, bool want_data, size_t prev_data_size)
{
    unsigned long now = GETTICKCOUNT();
    unsigned long next;
    DWORD ticks;

    if (toplevel_callback_pending()) {
        ticks = 0;
        next = now;
    } else if (run_timers(now, &next)) {
        unsigned long then = now;
        now = GETTICKCOUNT();
        ticks = (now - then > next - then) ? 0 : (next - now);
    } else if (msec >= 0) {
        ticks = (DWORD)msec;
    } else {
        ticks = INFINITE;
    }

    HandleWaitList *hwl = get_handle_wait_list();
    DWORD n = MsgWaitForMultipleObjects(hwl->nhandles, hwl->handles, false,
                                       ticks, QS_ALLINPUT);
    bool got_handle = false;
    if ((unsigned)(n - WAIT_OBJECT_0) < (unsigned)hwl->nhandles) {
        handle_wait_activate(hwl, n - WAIT_OBJECT_0);
        got_handle = true;
    }
    if (sftp_selftest_logfp) {
        fprintf(sftp_selftest_logfp,
                "PUMP: handles=%d ticks=%lu n=%lu got=%d "
                "sendok=%d sendbuf=%d connected=%d exit=%d\n",
                (int)hwl->nhandles, (unsigned long)ticks,
                (unsigned long)n, (int)got_handle,
                (int)backend_sendok(sftp_active->backend),
                (int)backend_sendbuffer(sftp_active->backend),
                (int)backend_connected(sftp_active->backend),
                backend_exitcode(sftp_active->backend));
        fflush(sftp_selftest_logfp);
    }
    handle_wait_list_free(hwl);

    MSG msg;
    int nmsg = 0;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            break;
        if (sftp_selftest_logfp) {
            fprintf(sftp_selftest_logfp, "PUMP: msg 0x%04x\n",
                    (unsigned)msg.message);
            fflush(sftp_selftest_logfp);
        }
        nmsg++;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (sftp_selftest_logfp && nmsg) {
        fprintf(sftp_selftest_logfp, "PUMP: dispatched %d msgs\n", nmsg);
        fflush(sftp_selftest_logfp);
    }

    run_toplevel_callbacks();

    if (got_handle)
        return true;
    if (want_data) {
        SftpXfer *sx = sftp_active;
        if (sx && bufchain_size(&sx->received_data) > prev_data_size)
            return true;
    }
    return false;
}

/* ----------------------------------------------------------------------
 * sftp.c 要求的三个收发回调。
 */

bool sftp_recvdata(char *buf, size_t len)
{
    SftpXfer *sx = sftp_active;
    if (!sx)
        return false;

    if (sftp_selftest_logfp) {
        fprintf(sftp_selftest_logfp, "RECV: want %d, have %d\n",
                (int)len, (int)bufchain_size(&sx->received_data));
        fflush(sftp_selftest_logfp);
    }

    while (len > 0) {
        while (bufchain_size(&sx->received_data) == 0) {
            if (sx->backend_error)
                return false;
            if (backend_exitcode(sx->backend) >= 0)
                return false;
            sftp_pump(-1, true, 0);
        }
        size_t got = bufchain_fetch_consume_up_to(&sx->received_data, buf, len);
        buf += got;
        len -= got;

        /*
         * 关键：把已消费的量告诉后端。SSH 通道有窗口流量控制，不调
         * backend_unthrottle 的话后端认为我们还没处理，就不再往下发，
         * 表现是收完第一批数据后连接彻底安静。
         */
        backend_unthrottle(sx->backend, bufchain_size(&sx->received_data));
    }
    return true;
}

bool sftp_senddata(const char *buf, size_t len)
{
    SftpXfer *sx = sftp_active;
    if (!sx)
        return false;
    if (sftp_selftest_logfp) {
        fprintf(sftp_selftest_logfp, "SEND: len=%d sendok=%d bytes=",
                (int)len, (int)backend_sendok(sx->backend));
        for (size_t i = 0; i < len && i < 32; i++)
            fprintf(sftp_selftest_logfp, "%02x", (unsigned char)buf[i]);
        fprintf(sftp_selftest_logfp, "\n");
        fflush(sftp_selftest_logfp);
    }
    backend_send(sx->backend, buf, len);
    return true;
}

size_t sftp_sendbuffer(void)
{
    SftpXfer *sx = sftp_active;
    if (!sx)
        return 0;
    return backend_sendbuffer(sx->backend);
}

/* ----------------------------------------------------------------------
 * 连接建立。
 */

static bool sftp_wait_connected(SftpXfer *sx)
{
    while (!backend_sendok(sx->backend)) {
        if (sx->backend_error)
            return false;
        if (backend_exitcode(sx->backend) >= 0)
            return false;
        sftp_pump(-1, false, 0);
    }
    return true;
}

SftpXfer *sftp_xfer_open(Conf *conf, HWND hwnd, char **errmsg)
{
    SftpXfer *sx;
    char *realhost = NULL;
    const char *err;
    Conf *beconf;
    char *tracepath = sftp_selftest_outpath("sftp-trace.txt");
    FILE *dbg = fopen(tracepath, "a");
    sfree(tracepath);

#define TRACE(...) do { if (dbg) { fprintf(dbg, __VA_ARGS__); fflush(dbg); } } while (0)

    if (errmsg)
        *errmsg = NULL;

    TRACE("open: enter\n");

    if (sftp_active) {
        if (errmsg)
            *errmsg = dupstr("已有传输连接在使用中。");
        TRACE("open: already active\n");
        if (dbg) fclose(dbg);
        return NULL;
    }

    sx = snew(SftpXfer);
    memset(sx, 0, sizeof(*sx));
    sx->seat.vt = &sftp_seat_vt;
    sx->hwnd = hwnd;
    bufchain_init(&sx->received_data);
    TRACE("open: struct ready\n");

    /* 复制一份配置，不动调用者的。 */
    beconf = conf_copy(conf);
    sx->conf = beconf;
    TRACE("open: conf copied protocol=%d\n",
          conf_get_int(beconf, CONF_protocol));

    /*
     * 拆掉主机名里的 user@ 前缀。psftp 也是这么做的：后端拿到的
     * host 必须是纯主机名，否则会拿去直接解析，报 WSANO_RECOVERY。
     */
    {
        char *hostbuf = dupstr(conf_get_str(beconf, CONF_host));
        char *host = hostbuf;
        char *at;

        /* 去掉前导空白 */
        host += strspn(host, " \t");
        at = strrchr(host, '@');
        if (at) {
            *at = '\0';
            if (*host)
                conf_set_str(beconf, CONF_username, host);
            host = at + 1;
        }
        /* 去掉内部空白 */
        {
            char *p = hostbuf, *q = host;
            while (*q) {
                if (*q != ' ' && *q != '\t')
                    *p++ = *q;
                q++;
            }
            *p = '\0';
        }
        conf_set_str(beconf, CONF_host, hostbuf);
        TRACE("open: host parsed -> '%s' user '%s'\n",
              conf_get_str(beconf, CONF_host),
              conf_get_str_ambi(beconf, CONF_username, NULL));
        sfree(hostbuf);
    }

    /*
     * 强制走 SSH-2 + SFTP 子系统，且不要 pty。
     *
     * 注意这里必须用 PROT_SSH 而不是 PROT_SSHCONN：后者的版本串是
     * "SSHCONNECTION@putty.projects..."，普通 sshd 不认，会在版本交换
     * 阶段直接关闭连接。psftp 用的也是 PROT_SSH。
     */
    conf_set_int(beconf, CONF_protocol, PROT_SSH);
    /*
     * CONF_sshprot 的合法值只有 0（仅 SSH-1）和 3（仅 SSH-2）。
     * 历史上还有 1 和 2 表示带回退的双版本协商，现在已废弃，
     * ssh.c 里有断言把关，写 2 会直接崩。
     */
    conf_set_int(beconf, CONF_sshprot, 3);
    conf_set_str(beconf, CONF_remote_cmd, "sftp");
    conf_set_bool(beconf, CONF_ssh_subsys, true);
    conf_set_bool(beconf, CONF_nopty, true);
    conf_set_bool(beconf, CONF_x11_forward, false);
    conf_set_bool(beconf, CONF_agentfwd, false);
    conf_set_bool(beconf, CONF_ssh_simple, true);
    {
        char *key;
        while ((key = conf_get_str_nthstrkey(beconf, CONF_portfwd, 0)) != NULL)
            conf_del_str_str(beconf, CONF_portfwd, key);
    }

    /*
     * logevent() 会无条件解引用 policy，传 NULL 必崩。正常路径用 putty
     * 自己的 GUI policy（日志进进程内环形缓冲，没有窗口也安全）；
     * 自测路径换成写文件的 policy，方便看握手/认证细节。
     */
    if (sftp_selftest_logfp) {
        sftp_selftest_logpolicy->vt = &sftp_selftest_logpolicy_vt;
        sx->logctx = log_init(sftp_selftest_logpolicy, beconf);
    } else {
        sftp_gui_logpolicy->vt = &win_gui_logpolicy_vt;
        sx->logctx = log_init(sftp_gui_logpolicy, beconf);
    }
    TRACE("open: logctx ready\n");

    sftp_active = sx;

    TRACE("open: calling backend_init host=%s port=%d\n",
          conf_get_str(beconf, CONF_host), conf_get_int(beconf, CONF_port));

    /*
     * 建连接期间把网络事件绑到专用隐藏窗口。终端会话占着默认窗口，
     * 不这么做的话两条连接的套接字会抢同一份 WM_NETEVENT，谁后设置
     * 谁独占，另一方直接僵死。
     *
     * 用隐藏窗口而不是 sx->hwnd：界面窗口可能被用户中途关掉，而传输
     * 连接的生命周期比它长（关闭时还要收尾），事件窗口必须活得比套接字
     * 久。隐藏窗口进程级常驻，正好合适。
     */
    {
        HWND evwhwnd = sftp_ensure_hidden_window();
        if (evwhwnd)
            winselgui_push_hwnd(evwhwnd);

        err = backend_init(backend_vt_from_proto(PROT_SSH),
                           &sx->seat, &sx->backend, sx->logctx, beconf,
                           conf_get_str(beconf, CONF_host),
                           conf_get_int(beconf, CONF_port), &realhost,
                           conf_get_bool(beconf, CONF_tcp_nodelay),
                           conf_get_bool(beconf, CONF_tcp_keepalives));

        winselgui_pop_hwnd();
    }

    TRACE("open: backend_init returned err=%s\n", err ? err : "(null)");
    if (err) {
        if (errmsg)
            *errmsg = dupstr(err);
        sftp_active = NULL;
        sfree(realhost);
        /* 后端没建起来，手工回收。 */
        bufchain_clear(&sx->received_data);
        conf_free(sx->conf);
        log_free(sx->logctx);
        sfree(sx->error_message);
        sfree(sx);
        return NULL;
    }
    sfree(realhost);

    if (!sftp_wait_connected(sx)) {
        if (errmsg) {
            *errmsg = sx->error_message ? dupstr(sx->error_message)
                                        : dupstr("SSH 连接建立失败。");
        }
        sftp_xfer_close(sx);
        return NULL;
    }

    return sx;
}

/* ----------------------------------------------------------------------
 * 远端目录操作。
 */

/* 发一个请求并等回应，失败返回 NULL。 */
static struct sftp_packet *sftp_xfer_roundtrip(SftpXfer *sx,
                                               struct sftp_request *req)
{
    struct sftp_packet *pktin;

    sftp_register(req);
    pktin = sftp_recv();
    if (!pktin) {
        sx->backend_error = true;
        return NULL;
    }
    if (sftp_find_request(pktin) != req) {
        sftp_pkt_free(pktin);
        sx->backend_error = true;
        return NULL;
    }
    return pktin;
}

bool sftp_xfer_init(SftpXfer *sx)
{
    char *canon;
    struct sftp_packet *pktin;
    struct sftp_request *req;

    if (!fxp_init())
        return false;

    /* 取 home 作为起始目录。 */
    req = fxp_realpath_send(".");
    pktin = sftp_xfer_roundtrip(sx, req);
    if (!pktin)
        return false;
    canon = fxp_realpath_recv(pktin, req);
    if (!canon)
        return false;

    sfree(sx->homedir);
    sx->homedir = dupstr(canon);
    sfree(sx->pwd);
    sx->pwd = canon;
    return true;
}

/*
 * 取会话的 home 目录（REALPATH "." 的结果）。
 *
 * SftpXfer 是不透明结构，界面层读不到字段；自检要确认 REALPATH
 * 真的成功过，所以开一个只读访问口。未连接时返回 NULL。
 */
const char *sftp_xfer_homedir(SftpXfer *sx)
{
    return sx ? sx->homedir : NULL;
}

/* 列目录。返回条目数组，调用者用 sftp_entries_free 释放。 */
void sftp_entries_free(SftpEntry *entries, size_t count)
{
    if (!entries)
        return;
    for (size_t i = 0; i < count; i++)
        sfree(entries[i].name);
    sfree(entries);
}

/*
 * 判定一个远端目录项是不是目录。
 *
 * 首选 SSH_FILEXFER_ATTR_PERMISSIONS 的 S_ISDIR 位，这是规范做法。
 * 但部分设备（嵌入式固件、精简 SFTP 实现）在 READDIR 的 attrs 里
 * 根本不带权限位，只回 size/mtime。此时若直接按「非目录」处理，
 * 目录会被显示成文件、双击进不去，递归传输也会拿它去 OPEN 然后失败。
 *
 * 兜底：解析 longname 的首字符。SFTP v3 的 longname 是 ls -l 风格的
 * 文本（如 "drwxr-xr-x 2 user group 4096 Jan 1 00:00 dirname"），
 * 首位 'd' 即目录、'-' 为普通文件、'l' 为符号链接。OpenSSH 与
 * 绝大多数服务端都会填这个字段；服务端省略时它会是空串。
 *
 * 两级都判不出来时返回 false（按文件处理），与原有行为一致 ——
 * 至少不会把普通文件误当目录去递归。
 */
static bool cn_remote_is_dir(const struct fxp_attrs *at, const char *longname)
{
    if (at->flags & SSH_FILEXFER_ATTR_PERMISSIONS)
        return S_ISDIR(at->permissions);

    if (longname && *longname)
        return longname[0] == 'd';

    return false;
}

/*
 * 符号链接的类型位判定。
 *
 * Windows 的 <sys/stat.h> 只提供 S_IFMT / S_IFDIR 等，既没有 S_IFLNK
 * 也没有 S_ISLNK（mingw 与 MSVC 都一样）。但 SFTP 是跨平台协议，
 * 服务端返回的 mode 用的是 POSIX 的位布局，S_IFLNK 固定为 0xA000。
 * 所以这里按 POSIX 定义自己算，不依赖宿主头文件。
 */
#define CN_S_IFMT  0170000
#define CN_S_IFLNK 0120000
#define CN_ISLNK(mode) (((mode) & CN_S_IFMT) == CN_S_IFLNK)

/*
 * 判定一个远端目录项是不是符号链接。
 *
 * 与 cn_remote_is_dir 同一套路：READDIR 的 attrs 带权限位时直接看
 * 链接类型位；不带时回退解析 longname 首字符是否为 'l'。
 *
 * 注意 SFTP 的 READDIR 语义上对应 LSTAT（OpenSSH 如此），所以 attrs
 * 里的类型位描述的是链接本身而非目标。但这不是所有服务端都遵守的，
 * 因此递归时还会用 LSTAT 复核一次（见 sftp_xfer_is_symlink）。
 */
static bool cn_remote_is_symlink(const struct fxp_attrs *at,
                                 const char *longname)
{
    if (at->flags & SSH_FILEXFER_ATTR_PERMISSIONS)
        return CN_ISLNK(at->permissions);

    if (longname && *longname)
        return longname[0] == 'l';

    return false;
}

/*
 * 用 LSTAT 单独复核一个远端路径是不是符号链接。
 *
 * READDIR 给的类型位不可全信（部分服务端在 READDIR 里就跟随了链接），
 * 递归进目录前用这个复核一次，避免把链接目录当真实目录钻进去绕成环。
 * LSTAT 失败时按「不是链接」处理 —— 保守放行，不让一次探测失败就
 * 把整个目录树跳过。
 */
bool sftp_xfer_is_symlink(SftpXfer *sx, const char *path)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    struct fxp_attrs attrs;

    req = fxp_lstat_send(path);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (!pktin)
        return false;
    if (!fxp_lstat_recv(pktin, req, &attrs))
        return false;

    if (!(attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS))
        return false;
    return CN_ISLNK(attrs.permissions);
}

SftpEntry *sftp_xfer_list(SftpXfer *sx, const char *path, size_t *count,
                          char **errmsg)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    struct fxp_handle *dirh;
    struct fxp_names *names;
    SftpEntry *result = NULL;
    size_t n = 0, size = 0;

    if (errmsg)
        *errmsg = NULL;
    *count = 0;

    req = fxp_opendir_send(path);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (!pktin) {
        if (errmsg)
            *errmsg = dupstr(sx->error_message ? sx->error_message
                                               : "无法打开远端目录。");
        return NULL;
    }
    dirh = fxp_opendir_recv(pktin, req);
    if (!dirh) {
        if (errmsg)
            *errmsg = dupstr(fxp_error());
        return NULL;
    }

    while (1) {
        req = fxp_readdir_send(dirh);
        pktin = sftp_xfer_roundtrip(sx, req);
        if (!pktin) {
            if (errmsg)
                *errmsg = dupstr(sx->error_message ? sx->error_message
                                                   : "读取远端目录失败。");
            break;
        }
        names = fxp_readdir_recv(pktin, req);
        if (!names) {
            if (fxp_error_type() != SSH_FX_EOF && errmsg)
                *errmsg = dupstr(fxp_error());
            break;
        }
        if (names->nnames == 0) {
            fxp_free_names(names);
            break;
        }
        for (int i = 0; i < names->nnames; i++) {
            struct fxp_name *nm = &names->names[i];
            struct fxp_attrs *at = &nm->attrs;

            /*
             * "." 与 ".." 在这里丢弃：真实服务器（OpenSSH）的 READDIR
             * 一定会返回它们。漏出去的话，界面上双击 ".." 会拼出
             * 「dir/..」这种脏路径，目录递更会顺着 "." 指回自己无限
             * 套娃，直到深度上限才停。「向上」按钮已负责回上级。
             */
            if (!strcmp(nm->filename, ".") || !strcmp(nm->filename, ".."))
                continue;

            if (n >= size)
                sgrowarray(result, size, n);
            result[n].name = dupstr(nm->filename);
            result[n].is_dir = cn_remote_is_dir(at, nm->longname);
            result[n].is_symlink = cn_remote_is_symlink(at, nm->longname);
            result[n].size =
                (at->flags & SSH_FILEXFER_ATTR_SIZE) ? at->size : 0;
            result[n].mtime =
                (at->flags & SSH_FILEXFER_ATTR_ACMODTIME) ? at->mtime : 0;
            n++;
        }
        fxp_free_names(names);
    }

    req = fxp_close_send(dirh);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (pktin)
        fxp_close_recv(pktin, req);

    /*
     * 空目录也返回非 NULL：NULL 只留给「失败」。否则调用方分不清
     * 「这目录是空的」和「列目录失败」，递归会把空目录整棵当失败。
     */
    if (!result)
        result = snewn(1, SftpEntry);
    *count = n;
    return result;
}

/* ----------------------------------------------------------------------
 * 远端目录与文件管理。
 */

bool sftp_xfer_mkdir(SftpXfer *sx, const char *path, char **errmsg)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    struct fxp_attrs attrs;

    if (errmsg)
        *errmsg = NULL;

    attrs.flags = 0;
    PUT_PERMISSIONS(attrs, 0777);
    req = fxp_mkdir_send(path, &attrs);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (!pktin) {
        if (errmsg)
            *errmsg = dupstr(sx->error_message ? sx->error_message
                                               : "创建目录失败。");
        return false;
    }
    if (!fxp_mkdir_recv(pktin, req)) {
        if (errmsg)
            *errmsg = dupstr(fxp_error());
        return false;
    }
    return true;
}

bool sftp_xfer_rmdir(SftpXfer *sx, const char *path, char **errmsg)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;

    if (errmsg)
        *errmsg = NULL;

    req = fxp_rmdir_send(path);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (!pktin) {
        if (errmsg)
            *errmsg = dupstr(sx->error_message ? sx->error_message
                                               : "删除目录失败。");
        return false;
    }
    if (!fxp_rmdir_recv(pktin, req)) {
        if (errmsg)
            *errmsg = dupstr(fxp_error());
        return false;
    }
    return true;
}

bool sftp_xfer_remove(SftpXfer *sx, const char *path, char **errmsg)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;

    if (errmsg)
        *errmsg = NULL;

    req = fxp_remove_send(path);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (!pktin) {
        if (errmsg)
            *errmsg = dupstr(sx->error_message ? sx->error_message
                                               : "删除文件失败。");
        return false;
    }
    if (!fxp_remove_recv(pktin, req)) {
        if (errmsg)
            *errmsg = dupstr(fxp_error());
        return false;
    }
    return true;
}

bool sftp_xfer_rename(SftpXfer *sx, const char *src, const char *dst,
                      char **errmsg)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;

    if (errmsg)
        *errmsg = NULL;

    req = fxp_rename_send(src, dst);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (!pktin) {
        if (errmsg)
            *errmsg = dupstr(sx->error_message ? sx->error_message
                                               : "重命名失败。");
        return false;
    }
    if (!fxp_rename_recv(pktin, req)) {
        if (errmsg)
            *errmsg = dupstr(fxp_error());
        return false;
    }
    return true;
}

bool sftp_xfer_stat(SftpXfer *sx, const char *path, struct fxp_attrs *attrs)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;

    req = fxp_stat_send(path);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (!pktin)
        return false;
    if (fxp_stat_recv(pktin, req, attrs))
        return true;

    /*
     * STAT 失败时用 LSTAT 兜底。
     *
     * 少数设备的 SFTP 实现只做了 LSTAT、没做 STAT（或 STAT 只对
     * 特定路径有效）。而调用方用这个函数拿文件大小、判断目标是否
     * 已存在，拿不到就会误判成「远端没有这个文件」，续传和覆盖
     * 判断都会跟着错。
     *
     * LSTAT 不跟随链接，对普通文件的结果与 STAT 相同；对符号链接
     * 拿到的是链接自身的大小（通常很小）。后者会让续传判断偏小、
     * 从而重传，属于安全方向的偏差 —— 宁可重传也不误判已传完。
     */
    req = fxp_lstat_send(path);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (!pktin)
        return false;
    return fxp_lstat_recv(pktin, req, attrs);
}

/* ----------------------------------------------------------------------
 * 本地文件读写。
 *
 * 统一走宽字符 API：远端传来的文件名是 UTF-8，里面可能有中文，用窄
 * 字符 fopen 会按 ANSI 代码页解释，中文路径直接失败。
 */

static FILE *sftp_local_fopen(const char *path, const wchar_t *mode)
{
    wchar_t *wpath = dup_mb_to_wc(DEFAULT_CODEPAGE, path);
    FILE *fp = _wfopen(wpath, mode);
    sfree(wpath);
    return fp;
}

static int64_t sftp_local_size(const char *path)
{
    wchar_t *wpath = dup_mb_to_wc(DEFAULT_CODEPAGE, path);
    struct _stat64 st;
    int ok = _wstat64(wpath, &st) == 0;
    sfree(wpath);
    return ok ? (int64_t)st.st_size : -1;
}

/* ----------------------------------------------------------------------
 * 下载：远端 -> 本地。
 *
 * 和 psftp 用同一套流水线（xfer_download_*），但多两件事：
 *  - 进度回调，供界面刷新进度条；
 *  - 取消标志，让长传输能中途停下。
 */

static bool sftp_download_cancelled(SftpXfer *sx)
{
    return sx->cancel_requested;
}

/*
 * 取本地文件现有大小，用作续传起点。文件不存在或读不到属性返回 0
 * （等于从头传）。
 */
static uint64_t cn_local_resume_offset(const char *localpath)
{
    wchar_t *w = dup_mb_to_wc(DEFAULT_CODEPAGE, localpath);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    uint64_t size = 0;

    if (GetFileAttributesExW(w, GetFileExInfoStandard, &fad) &&
        !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    sfree(w);
    return size;
}

/*
 * 取远端文件现有大小，用作续传起点。失败返回 0。
 */
static uint64_t cn_remote_resume_offset(SftpXfer *sx, const char *remotepath)
{
    struct fxp_attrs attrs;

    if (sftp_xfer_stat(sx, remotepath, &attrs) &&
        (attrs.flags & SSH_FILEXFER_ATTR_SIZE))
        return attrs.size;
    return 0;
}

XferOutcome sftp_xfer_download(SftpXfer *sx, const char *remotepath,
                               const char *localpath, bool allow_resume,
                               SftpProgressFn progress,
                               void *progctx, char **errmsg)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    struct fxp_handle *fh;
    struct fxp_attrs attrs;
    struct fxp_xfer *xfer;
    FILE *fp;
    uint64_t total = 0, done = 0, offset = 0;
    bool ok = true;
    XferOutcome outcome = XFER_OK;

    if (errmsg)
        *errmsg = NULL;

    /* 先取大小，进度条要用；拿不到也不算错，进度改成不定长。 */
    if (sftp_xfer_stat(sx, remotepath, &attrs) &&
        (attrs.flags & SSH_FILEXFER_ATTR_SIZE))
        total = attrs.size;

    /*
     * 断点续传：本地已有同名文件且比远端小，就从它的大小处接着取。
     *
     * allow_resume 为假时整段跳过 —— 用户明确要覆盖，或者这是一次
     * 全新传输，都不能拿「大小恰好相同」当作「已经传完了」：内容可能
     * 完全不一样，跳过就等于骗人说传好了。
     */
    if (allow_resume && total) {
        uint64_t have = cn_local_resume_offset(localpath);
        if (have == total) {
            /*
             * 已经传完了，一个字节都不用再发。续传时必然走到这里：
             * 前一次暂停前传完的文件，这次直接跳过。
             */
            if (progress)
                progress(progctx, total, total);
            return XFER_OK;
        }
        if (have > 0 && have < total)
            offset = have;
    }

    req = fxp_open_send(remotepath, SSH_FXF_READ, NULL);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (!pktin) {
        if (errmsg)
            *errmsg = dupstr(sx->error_message ? sx->error_message
                                               : "打开远端文件失败。");
        return XFER_FAIL;
    }
    fh = fxp_open_recv(pktin, req);
    if (!fh) {
        if (errmsg)
            *errmsg = dupstr(fxp_error());
        return XFER_FAIL;
    }

    /* 续传用 "ab" 追加，从头来用 "wb" 覆盖。 */
    fp = sftp_local_fopen(localpath, offset ? L"ab" : L"wb");
    if (!fp) {
        req = fxp_close_send(fh);
        pktin = sftp_xfer_roundtrip(sx, req);
        if (pktin)
            fxp_close_recv(pktin, req);
        if (errmsg)
            *errmsg = dupstr("无法创建本地文件。");
        return XFER_FAIL;
    }

    /* 进度从断点起算，界面看到的百分比才是真实的整体进度。 */
    done = offset;
    xfer = xfer_download_init(fh, offset);
    while (!xfer_done(xfer)) {
        void *vbuf;
        int retd, len;

        if (sftp_download_cancelled(sx)) {
            ok = false;
            outcome = XFER_CANCELLED;
            if (errmsg)
                *errmsg = dupstr("传输已取消。");
            break;
        }
        if (sx->pause_requested) {
            ok = false;
            outcome = XFER_PAUSED;
            if (errmsg)
                *errmsg = dupstr("传输已暂停。");
            break;
        }

        xfer_download_queue(xfer);
        pktin = sftp_recv();
        if (!pktin) {
            ok = false;
            if (errmsg)
                *errmsg = dupstr("从远端读取数据失败。");
            break;
        }
        retd = xfer_download_gotpkt(xfer, pktin);
        if (retd <= 0) {
            if (retd == INT_MIN)
                sfree(pktin);
            ok = false;
            if (errmsg)
                *errmsg = dupstr(fxp_error());
            break;
        }

        while (xfer_download_data(xfer, &vbuf, &len)) {
            if (len > 0 && fwrite(vbuf, 1, len, fp) != (size_t)len) {
                ok = false;
                if (errmsg)
                    *errmsg = dupstr("写入本地文件失败。");
                xfer_set_error(xfer);
            }
            done += len;
            sfree(vbuf);
            if (progress && !progress(progctx, done, total))
                sx->cancel_requested = true;
        }
    }
    xfer_cleanup(xfer);

    req = fxp_close_send(fh);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (pktin)
        fxp_close_recv(pktin, req);

    if (fclose(fp) != 0 && ok) {
        ok = false;
        if (errmsg)
            *errmsg = dupstr("关闭本地文件失败。");
    }

    /*
     * 只有终止和出错才把半截文件删掉。暂停必须留着 —— 那就是断点，
     * 删了下次就得从头传，「续传」等于白做。
     */
    if (!ok && outcome != XFER_PAUSED) {
        wchar_t *wpath = dup_mb_to_wc(DEFAULT_CODEPAGE, localpath);
        _wremove(wpath);
        sfree(wpath);
    }

    return ok ? XFER_OK : outcome;
}

/* ----------------------------------------------------------------------
 * 上传：本地 -> 远端。
 */

XferOutcome sftp_xfer_upload(SftpXfer *sx, const char *localpath,
                             const char *remotepath, bool allow_resume,
                             SftpProgressFn progress,
                             void *progctx, char **errmsg)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    struct fxp_handle *fh;
    struct fxp_attrs attrs;
    struct fxp_xfer *xfer;
    FILE *fp;
    uint64_t total, done = 0, offset = 0;
    bool ok = true, eof = false;
    XferOutcome outcome = XFER_OK;

    if (errmsg)
        *errmsg = NULL;

    total = (uint64_t)sftp_local_size(localpath);
    if (total == (uint64_t)-1)
        total = 0;

    fp = sftp_local_fopen(localpath, L"rb");
    if (!fp) {
        if (errmsg)
            *errmsg = dupstr("无法打开本地文件。");
        return XFER_FAIL;
    }

    /*
     * 断点续传：远端已有同名文件且比本地小，就从它的大小处接着传。
     *
     * allow_resume 为假时整段跳过，一律从头覆盖：用户点过「覆盖」、
     * 或这是一次全新传输，此时远端哪怕大小恰好等于本地也不能跳过 ——
     * 内容可能完全不同，跳过就是假传完。
     */
    if (allow_resume && total) {
        uint64_t have = cn_remote_resume_offset(sx, remotepath);
        if (have == total) {
            /* 已经传完，不必再发一遍（续传时前一次传完的文件走这里）。 */
            fclose(fp);
            if (progress)
                progress(progctx, total, total);
            return XFER_OK;
        }
        if (have > 0 && have < total) {
            offset = have;
            /* 从本地文件对应位置开始读。 */
            if (fseek(fp, (long)offset, SEEK_SET) != 0)
                offset = 0;
        }
    }

    attrs.flags = 0;
    PUT_PERMISSIONS(attrs, 0644);
    /*
     * 续传时不能带 TRUNC —— 那会把远端已有的那段抹掉，断点直接作废。
     */
    req = fxp_open_send(remotepath,
                        SSH_FXF_WRITE | SSH_FXF_CREAT |
                        (offset ? 0 : SSH_FXF_TRUNC),
                        &attrs);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (!pktin) {
        fclose(fp);
        if (errmsg)
            *errmsg = dupstr(sx->error_message ? sx->error_message
                                               : "打开远端文件失败。");
        return XFER_FAIL;
    }
    fh = fxp_open_recv(pktin, req);
    if (!fh) {
        fclose(fp);
        if (errmsg)
            *errmsg = dupstr(fxp_error());
        return XFER_FAIL;
    }

    /* 进度从断点起算。 */
    done = offset;
    xfer = xfer_upload_init(fh, offset);
    while ((!ok || !eof) || !xfer_done(xfer)) {
        char buffer[32768];

        if (sftp_download_cancelled(sx)) {
            ok = false;
            outcome = XFER_CANCELLED;
            if (errmsg)
                *errmsg = dupstr("传输已取消。");
            break;
        }
        if (sx->pause_requested) {
            ok = false;
            outcome = XFER_PAUSED;
            if (errmsg)
                *errmsg = dupstr("传输已暂停。");
            break;
        }

        while (ok && !eof && xfer_upload_ready(xfer)) {
            int len = fread(buffer, 1, sizeof(buffer), fp);
            if (len < 0) {
                ok = false;
                if (errmsg)
                    *errmsg = dupstr("读取本地文件失败。");
            } else if (len == 0) {
                eof = true;
            } else {
                xfer_upload_data(xfer, buffer, len);
                done += len;
                if (progress && !progress(progctx, done, total))
                    sx->cancel_requested = true;
            }
        }

        if (toplevel_callback_pending() && ok && !eof) {
            run_toplevel_callbacks();
            continue;
        }

        if (!xfer_done(xfer)) {
            int ret;
            pktin = sftp_recv();
            if (!pktin) {
                ok = false;
                if (errmsg)
                    *errmsg = dupstr("向远端发送数据失败。");
                break;
            }
            ret = xfer_upload_gotpkt(xfer, pktin);
            if (ret <= 0) {
                if (ret == INT_MIN)
                    sfree(pktin);
                if (ok) {
                    ok = false;
                    if (errmsg)
                        *errmsg = dupstr(fxp_error());
                }
            }
        }
    }
    xfer_cleanup(xfer);

    req = fxp_close_send(fh);
    pktin = sftp_xfer_roundtrip(sx, req);
    if (pktin)
        fxp_close_recv(pktin, req);

    fclose(fp);

    /*
     * 只有终止和出错才删远端半截文件；暂停要留着当断点，删了就没法
     * 续传了。
     */
    if (!ok && outcome != XFER_PAUSED) {
        req = fxp_remove_send(remotepath);
        pktin = sftp_xfer_roundtrip(sx, req);
        if (pktin)
            fxp_remove_recv(pktin, req);
    }

    return ok ? XFER_OK : outcome;
}

/* ----------------------------------------------------------------------
 * 本地目录列举。
 *
 * 远端和本地都填 SftpEntry，界面就能用一套代码画两个列表。本地文件名
 * 走宽字符 API 再转回 UTF-8，中文名不会变乱码。
 */

/* FILETIME -> Unix 时间戳。两纪元差 11644473600 秒。 */
static unsigned long cn_filetime_to_unix(FILETIME ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    uint64_t secs = u.QuadPart / 10000000ULL;
    if (secs <= 11644473600ULL)
        return 0;
    return (unsigned long)(secs - 11644473600ULL);
}

SftpEntry *sftp_local_list(const char *path, size_t *count, char **errmsg)
{
    wchar_t *wpath, *wpattern;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    SftpEntry *result = NULL;
    size_t n = 0, size = 0;

    if (errmsg)
        *errmsg = NULL;
    *count = 0;

    wpath = dup_mb_to_wc(DEFAULT_CODEPAGE, path);
    {
        size_t plen = wcslen(wpath);
        wpattern = snewn(plen + 3, wchar_t);
        wcscpy(wpattern, wpath);
        wcscat(wpattern, L"\\*");
    }
    h = FindFirstFileW(wpattern, &fd);
    sfree(wpattern);
    sfree(wpath);

    if (h == INVALID_HANDLE_VALUE) {
        if (errmsg)
            *errmsg = dupstr("无法读取本地目录。");
        return NULL;
    }

    do {
        /* 上一层目录用界面上的「向上」表达，不塞进列表。 */
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;

        if (n >= size)
            sgrowarray(result, size, n);
        result[n].name = dup_wc_to_mb(DEFAULT_CODEPAGE, fd.cFileName, "?");
        result[n].is_dir =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        /*
         * Windows 没有 Unix 那样的符号链接类型位，重解析点
         * （FILE_ATTRIBUTE_REPARSE_POINT）涵盖符号链接、目录联接
         * 和挂载点。递归传输对它们一视同仁地跳过，正是想要的行为 ——
         * 钻进去都可能绕回自己。
         */
        result[n].is_symlink =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        result[n].size =
            ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        result[n].mtime = cn_filetime_to_unix(fd.ftLastWriteTime);
        n++;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    *count = n;
    /*
     * 空目录必须返回非 NULL。
     *
     * 返回 NULL 在调用方是「列目录失败」的意思，调用方还会去拼错误信息。
     * 空目录本来无罪，返回 NULL 就让递归传输报「列目录 X：(无错误信息)」
     * 这种不着边际的失败 —— 拖一个空文件夹进去就踩这个坑。
     *
     * 返回一块零长度数组，调用方按 count == 0 处理即可，语义就不含糊了。
     */
    if (!result)
        result = snewn(1, SftpEntry);
    return result;
}

bool sftp_local_mkdir(const char *path, char **errmsg)
{
    wchar_t *w = dup_mb_to_wc(DEFAULT_CODEPAGE, path);
    bool ok = _wmkdir(w) == 0;
    sfree(w);
    if (!ok && errmsg)
        *errmsg = dupprintf("无法创建目录（错误码 %d）。", errno);
    return ok;
}

bool sftp_local_rmdir(const char *path, char **errmsg)
{
    wchar_t *w = dup_mb_to_wc(DEFAULT_CODEPAGE, path);
    bool ok = _wrmdir(w) == 0;
    sfree(w);
    if (!ok && errmsg)
        *errmsg = dupprintf("无法删除目录（错误码 %d）。", errno);
    return ok;
}

bool sftp_local_remove(const char *path, char **errmsg)
{
    wchar_t *w = dup_mb_to_wc(DEFAULT_CODEPAGE, path);
    bool ok = _wremove(w) == 0;
    sfree(w);
    if (!ok && errmsg)
        *errmsg = dupprintf("无法删除文件（错误码 %d）。", errno);
    return ok;
}

bool sftp_local_rename(const char *src, const char *dst, char **errmsg)
{
    wchar_t *ws = dup_mb_to_wc(DEFAULT_CODEPAGE, src);
    wchar_t *wd = dup_mb_to_wc(DEFAULT_CODEPAGE, dst);
    bool ok = _wrename(ws, wd) == 0;
    sfree(ws);
    sfree(wd);
    if (!ok && errmsg)
        *errmsg = dupprintf("无法重命名（错误码 %d）。", errno);
    return ok;
}

/* ----------------------------------------------------------------------
 * 路径拼接。
 */

static bool cn_path_is_root(const char *path, bool remote)
{
    if (!path || !*path)
        return false;
    if (remote)
        return !strcmp(path, "/");
    /* Windows 根：C:\ 或 \\server\share 之类，粗判即可 */
    if (strlen(path) <= 3 && path[1] == ':')
        return true;
    return false;
}

char *cn_path_join(const char *dir, const char *name, bool remote)
{
    char sep = remote ? '/' : '\\';
    size_t dl = strlen(dir);

    if (!*dir)
        return dupstr(name);
    /* 根目录已经带分隔符了，别再补一个 */
    if (dir[dl - 1] == '/' || dir[dl - 1] == '\\')
        return dupcat(dir, name);
    return dupcat(dir, sep == '/' ? "/" : "\\", name);
}

char *cn_path_parent(const char *path, bool remote)
{
    char *copy, *p;
    char sep = remote ? '/' : '\\';

    if (!path || !*path)
        return dupstr(remote ? "/" : "");
    if (cn_path_is_root(path, remote))
        return dupstr(path);

    copy = dupstr(path);
    /* 去掉结尾多余分隔符 */
    p = copy + strlen(copy);
    while (p > copy + 1 && p[-1] == sep)
        *--p = '\0';

    /* 往回找最后一个分隔符 */
    p = strrchr(copy, sep);
    if (!p) {
        if (!remote) {
            /* 本地相对路径没有分隔符，交给上层处理 */
            sfree(copy);
            return dupstr(".");
        }
        sfree(copy);
        return dupstr("/");
    }

    if (p == copy) {
        /* 根就是 "/" 或 "\" */
        p[1] = '\0';
        return copy;
    }

    /* Windows 盘符根 C:\ 要保留反斜杠 */
    if (!remote && p == copy + 2 && copy[1] == ':') {
        p[1] = '\0';
        return copy;
    }

    *p = '\0';
    return copy;
}

void sftp_xfer_request_cancel(SftpXfer *sx)
{
    if (sx)
        sx->cancel_requested = true;
}

void sftp_xfer_reset_cancel(SftpXfer *sx)
{
    if (sx)
        sx->cancel_requested = false;
}

void sftp_xfer_request_pause(SftpXfer *sx)
{
    if (sx)
        sx->pause_requested = true;
}

void sftp_xfer_reset_pause(SftpXfer *sx)
{
    if (sx)
        sx->pause_requested = false;
}

char *sftp_xfer_error(SftpXfer *sx)
{
    if (!sx || !sx->error_message)
        return NULL;
    return dupstr(sx->error_message);
}

char *sftp_xfer_pwd(SftpXfer *sx)
{
    return sx->pwd ? dupstr(sx->pwd) : NULL;
}

void sftp_xfer_close(SftpXfer *sx)
{
    if (!sx)
        return;

    if (sx->backend) {
        backend_free(sx->backend);
        sx->backend = NULL;
    }
    bufchain_clear(&sx->received_data);
    if (sx->logctx)
        log_free(sx->logctx);
    if (sx->conf)
        conf_free(sx->conf);
    sfree(sx->error_message);
    sfree(sx->pwd);
    sfree(sx->homedir);
    if (sftp_active == sx)
        sftp_active = NULL;
    sfree(sx);
}

/* ----------------------------------------------------------------------
 * 自测入口：putty -sftp-list <远端路径>
 *
 * putty 是 GUI 子系统程序，没有控制台，所以结果写进固定文件。
 */

/*
 * 网络事件需要一个窗口来接。PuTTY 的 select-gui.c 用 WSAAsyncSelect
 * 把套接字活动变成发往 winsel_hwnd 的 WM_NETEVENT 消息，所以这里必须
 * 有一个真实窗口，哪怕它从不显示。
 */
static void sftp_selftest_transfer(SftpXfer *sx, FILE *fp);

/*
 * cn: 目录递归复制的自检入口，实现在 sftpwin.c 里（它与界面共用
 * cn_xfer_recurse）。这里只声明，供 -sftp-list 自检流程调用。
 */
bool sftp_selftest_recurse(SftpXfer *sx, const char *src, const char *dst,
                           bool to_remote, char **errmsg);

static HWND sftp_hidden_hwnd = NULL;

static LRESULT CALLBACK sftp_hidden_wndproc(HWND hwnd, UINT msg,
                                            WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_NETEVENT || msg == WM_DONE_WITH_SOCKET) {
        winselgui_response(msg, wparam, lparam);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static HWND sftp_ensure_hidden_window(void)
{
    WNDCLASSW wc;
    static const wchar_t classname[] = L"PuTTYSftpXferHidden";

    if (sftp_hidden_hwnd && IsWindow(sftp_hidden_hwnd))
        return sftp_hidden_hwnd;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = sftp_hidden_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = classname;
    if (!GetClassInfoW(wc.hInstance, classname, &wc)) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = sftp_hidden_wndproc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = classname;
        if (!RegisterClassW(&wc))
            return NULL;
    }

    /*
     * 必须是普通窗口，不能用 HWND_MESSAGE 的消息专用窗口：
     * WSAAsyncSelect 投递的 WM_NETEVENT 不会进消息专用窗口。
     * 建好之后立刻藏起来，用户看不见。
     *
     * 注意这里不再调 winselgui_set_hwnd：那是「默认窗口」，终端会话
     * 正占着，覆盖它会让终端的 SSH 收不到网络事件。改由
     * sftp_xfer_open 在建连接期间用 winselgui_push_hwnd 临时绑定。
     */
    sftp_hidden_hwnd = CreateWindowExW(0, classname, L"PuTTY SFTP",
                                       0, 0, 0, 0, 0,
                                       NULL, NULL, wc.hInstance, NULL);
    return sftp_hidden_hwnd;
}

void sftp_selftest_list(Conf *conf, const char *path)
{
    SftpXfer *sx;
    char *errmsg = NULL;
    SftpEntry *entries = NULL;
    size_t count = 0;
    char *outpath = sftp_selftest_outpath("sftp-selftest.txt");
    FILE *fp = fopen(outpath, "w");
    sfree(outpath);

    if (!fp)
        return;

    /* 自测专用：日志写进单独文件，看握手和认证细节。 */
    char *logpath = sftp_selftest_outpath("sftp-log.txt");
    sftp_selftest_logfp = fopen(logpath, "w");
    sfree(logpath);

    fprintf(fp, "[0] 创建网络事件窗口 ...\n");
    fflush(fp);
    if (!sftp_ensure_hidden_window()) {
        fprintf(fp, "失败：无法创建窗口\n");
        fclose(fp);
        return;
    }
    fprintf(fp, "成功\n");

    fprintf(fp, "[1] 建立独立 SFTP 连接 ...\n");
    fflush(fp);

    sx = sftp_xfer_open(conf, NULL, &errmsg);
    if (!sx) {
        fprintf(fp, "失败：%s\n", errmsg ? errmsg : "(未知)");
        sfree(errmsg);
        fclose(fp);
        return;
    }
    fprintf(fp, "成功\n");

    fprintf(fp, "[2] SFTP 协议握手 ...\n");
    fflush(fp);
    if (!sftp_xfer_init(sx)) {
        fprintf(fp, "失败：%s\n", fxp_error());
        sftp_xfer_close(sx);
        fclose(fp);
        return;
    }
    fprintf(fp, "成功，远端起始目录 = %s\n", sx->pwd ? sx->pwd : "(空)");

    fprintf(fp, "[3] 列出目录 %s ...\n", path);
    fflush(fp);
    entries = sftp_xfer_list(sx, path, &count, &errmsg);
    if (!entries) {
        fprintf(fp, "失败：%s\n", errmsg ? errmsg : "(未知)");
        sfree(errmsg);
        sftp_xfer_close(sx);
        fclose(fp);
        return;
    }
    fprintf(fp, "成功，共 %zu 项\n", count);
    for (size_t i = 0; i < count; i++) {
        fprintf(fp, "  %s%s\n", entries[i].name,
                entries[i].is_dir ? "/" : "");
    }
    sftp_entries_free(entries, count);

    /* ---- 上传下载往返验证 ---- */
    sftp_selftest_transfer(sx, fp);

    fprintf(fp, "[4] 关闭连接 ...\n");
    sftp_xfer_close(sx);
    fprintf(fp, "完成\n");
    fclose(fp);
}

/* ----------------------------------------------------------------------
 * 传输自测：造几种刁钻文件，传上去再拿下来，逐字节比对。
 */

static bool st_write_file(const char *path, const void *data, size_t len)
{
    FILE *fp = sftp_local_fopen(path, L"wb");
    if (!fp)
        return false;
    bool ok = (len == 0) || fwrite(data, 1, len, fp) == len;
    fclose(fp);
    return ok;
}

static unsigned char *st_read_file(const char *path, size_t *len)
{
    FILE *fp = sftp_local_fopen(path, L"rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *buf = snewn(sz > 0 ? sz : 1, unsigned char);
    size_t got = (sz > 0) ? fread(buf, 1, sz, fp) : 0;
    fclose(fp);
    *len = got;
    return buf;
}

static bool st_progress(void *ctx, uint64_t done, uint64_t total)
{
    FILE *fp = (FILE *)ctx;
    if (fp)
        fprintf(fp, "    进度 %llu/%llu\n",
                (unsigned long long)done, (unsigned long long)total);
    return true;                      /* 不打断 */
}

static void sftp_selftest_transfer(SftpXfer *sx, FILE *fp)
{
    /* 说明 / 文件名 / 字节数 */
    static const struct {
        const char *desc;
        const char *fname;
        size_t size;
    } cases[] = {
        {"空文件",       "cn-empty.bin",        0},
        {"单字节",       "cn-one.bin",          1},
        {"跨请求边界",   "cn-edge.bin",     32768},
        {"多块大文件",   "cn-big.bin",     300000},
        {"中文文件名",   "中文测试文件.bin",  5000},
    };
    const int ncases = (int)(sizeof(cases) / sizeof(cases[0]));
    const char *tmpdir = "sftp-test";
    char *errmsg = NULL;
    int pass = 0, fail = 0;

    fprintf(fp, "\n[4] 上传下载往返验证\n");
    fflush(fp);

    /* 本地暂存目录 */
    {
        wchar_t *w = dup_mb_to_wc(DEFAULT_CODEPAGE, tmpdir);
        _wmkdir(w);
        sfree(w);
    }

    for (int i = 0; i < ncases; i++) {
        size_t srclen = 0, gotlen = 0;
        unsigned char *src, *got;
        char *local = dupcat(tmpdir, "\\", cases[i].fname);
        char *localback = dupcat(tmpdir, "\\回到-", cases[i].fname);
        char *remote = dupcat("/tmp/", cases[i].fname);
        bool ok = true;

        fprintf(fp, "  [%s] %s（%zu 字节）\n",
                cases[i].desc, cases[i].fname, cases[i].size);

        /* 造内容：确定性伪随机，便于比对 */
        src = snewn(cases[i].size ? cases[i].size : 1, unsigned char);
        for (size_t j = 0; j < cases[i].size; j++)
            src[j] = (unsigned char)((j * 37 + 11) & 0xFF);
        srclen = cases[i].size;

        if (!st_write_file(local, src, srclen)) {
            fprintf(fp, "    本地造文件失败\n");
            fail++;
            goto next;
        }

        /* 自测走「全新传输」语义：一律从头传，不按断点跳过。 */
        if (sftp_xfer_upload(sx, local, remote, false,
                             st_progress, NULL, &errmsg) != XFER_OK) {
            fprintf(fp, "    上传失败：%s\n", errmsg ? errmsg : "(未知)");
            sfree(errmsg);
            errmsg = NULL;
            ok = false;
        }

        if (ok && sftp_xfer_download(sx, remote, localback, false,
                                     st_progress, NULL, &errmsg) != XFER_OK) {
            fprintf(fp, "    下载失败：%s\n", errmsg ? errmsg : "(未知)");
            sfree(errmsg);
            errmsg = NULL;
            ok = false;
        }

        if (ok) {
            got = st_read_file(localback, &gotlen);
            if (!got) {
                fprintf(fp, "    读回文件失败\n");
                ok = false;
            } else {
                if (gotlen != srclen) {
                    fprintf(fp, "    长度不符：期望 %zu 实际 %zu\n",
                            srclen, gotlen);
                    ok = false;
                } else if (memcmp(src, got, srclen) != 0) {
                    fprintf(fp, "    内容不符\n");
                    ok = false;
                }
                sfree(got);
            }
        }

        /* 清理远端与本地 */
        if (!sftp_xfer_remove(sx, remote, &errmsg)) {
            fprintf(fp, "    （清理远端失败：%s）\n",
                    errmsg ? errmsg : "(未知)");
            sfree(errmsg);
            errmsg = NULL;
        }
        {
            wchar_t *w = dup_mb_to_wc(DEFAULT_CODEPAGE, local);
            _wremove(w);
            sfree(w);
            w = dup_mb_to_wc(DEFAULT_CODEPAGE, localback);
            _wremove(w);
            sfree(w);
        }

        if (ok) {
            fprintf(fp, "    通过\n");
            pass++;
        } else {
            fail++;
        }

      next:
        sfree(src);
        sfree(local);
        sfree(localback);
        sfree(remote);
        fflush(fp);
    }

    fprintf(fp, "  小计：通过 %d 项，失败 %d 项\n", pass, fail);
    fflush(fp);
}
