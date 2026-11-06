#include "putty.h"
#include "storage.h"

extern bool sesslist_demo_mode;
extern Filename *dialog_box_demo_screenshot_filename;
static strbuf *demo_terminal_data = NULL;
static Filename *terminal_demo_screenshot_filename;

/* cn: -sftp-list 自测用的远端路径 */
static char *sftp_list_path = NULL;

/* cn: -sftp-win 打开双栏窗口做界面自检 */
static bool sftp_win_selftest = false;
/* cn: -sftp-open 只开窗口不跑自检，供人工看图 */
static bool sftp_win_open_only = false;
/* cn: -sftp-open-fail 故意让连接失败，验证不弹框/不残留窗口 */
static bool sftp_win_open_fail = false;
/* cn: -sftp-recurse 目录递归传输自检的源与目标 */
static char *sftp_recurse_src = NULL;
static char *sftp_recurse_dst = NULL;
/* cn: -sftp-resume 断点续传自检的本地文件与远端路径 */
static char *sftp_resume_src = NULL;
static char *sftp_resume_dst = NULL;

/* cn: 自测入口，实现在 sftpxfer.c / sftpwin.c */
void sftp_selftest_list(Conf *conf, const char *path);
void sftp_selftest_window(Conf *conf);
void sftp_selftest_open_fail(Conf *conf);
void sftp_selftest_recurse_run(Conf *conf, const char *src, const char *dst);
void sftp_selftest_resume_run(Conf *conf, const char *localpath,
                              const char *remotepath);
void sftp_window_detached(Conf *conf);

const unsigned cmdline_tooltype =
    TOOLTYPE_HOST_ARG |
    TOOLTYPE_PORT_ARG |
    TOOLTYPE_NO_VERBOSE_OPTION;

void gui_term_process_cmdline(Conf *conf, char *cmdline)
{
    char *p;
    bool special_launchable_argument = false;
    bool demo_config_box = false;

    settings_set_default_protocol(be_default_protocol);
    /* Find the appropriate default port. */
    {
        const struct BackendVtable *vt =
            backend_vt_from_proto(be_default_protocol);
        settings_set_default_port(0); /* illegal */
        if (vt)
            settings_set_default_port(vt->default_port);
    }
    conf_set_int(conf, CONF_logtype, LGTYP_NONE);

    do_defaults(NULL, conf);

    p = handle_restrict_acl_cmdline_prefix(cmdline);

    if (handle_special_sessionname_cmdline(p, conf)) {
        if (!conf_launchable(conf) && !do_config(conf)) {
            cleanup_exit(0);
        }
        special_launchable_argument = true;
    } else if (handle_special_filemapping_cmdline(p, conf)) {
        special_launchable_argument = true;
    } else if (!*p) {
        /* Do-nothing case for an empty command line - or rather,
         * for a command line that's empty _after_ we strip off
         * the &R prefix. */
    } else {
        /*
         * Otherwise, break up the command line and deal with
         * it sensibly.
         */
        CmdlineArgList *arglist = cmdline_arg_list_from_GetCommandLineW();
        size_t arglistpos = 0;
        while (arglist->args[arglistpos]) {
            CmdlineArg *arg = arglist->args[arglistpos++];
            CmdlineArg *nextarg = arglist->args[arglistpos];
            const char *p = cmdline_arg_to_str(arg);
            int ret = cmdline_process_param(arg, nextarg, 1, conf);
            if (ret == -2) {
                cmdline_error("选项 \"%s\" 需要参数", p);
            } else if (ret == 2) {
                arglistpos++;          /* skip next argument */
            } else if (ret == 1) {
                continue;          /* nothing further needs doing */
            } else if (!strcmp(p, "-cleanup")) {
                /*
                 * `putty -cleanup'. Remove all registry
                 * entries associated with PuTTY, and also find
                 * and delete the random seed file.
                 */
                char *s1, *s2;
                s1 = dupprintf("此过程将删除所有与 %s 相关联\n"
                               "注册表项，并且还将删除随机\n"
                               "种子文件。（这只会影响到当前\n"
                               "登录的用户。）\n"
                               "\n"
                               "此操作将会删除保存的会话。\n"
                               "真的确定要继续吗？",
                               appname);
                s2 = dupprintf("%s 警告", appname);
                if (message_box(NULL, s1, s2,
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2,
                                false, HELPCTXID(option_cleanup)) == IDYES) {
                    cleanup_all();
                }
                sfree(s1);
                sfree(s2);
                exit(0);
            } else if (!strcmp(p, "-pgpfp")) {
                pgp_fingerprints_msgbox(NULL);
                exit(0);
            } else if (has_ca_config_box &&
                       (!strcmp(p, "-host-ca") || !strcmp(p, "--host-ca") ||
                        !strcmp(p, "-host_ca") || !strcmp(p, "--host_ca"))) {
                show_ca_config_box(NULL);
                exit(0);
            } else if (!strcmp(p, "-demo-config-box")) {
                if (!arglist->args[arglistpos]) {
                    cmdline_error("%s 需要一个输出文件名", p);
                } else {
                    demo_config_box = true;
                    dialog_box_demo_screenshot_filename =
                        cmdline_arg_to_filename(arglist->args[arglistpos++]);
                }
            } else if (!strcmp(p, "-demo-terminal")) {
                if (!arglist->args[arglistpos] ||
                    !arglist->args[arglistpos+1]) {
                    cmdline_error("%s 需要输入和输出文件名", p);
                } else {
                    const char *infile =
                        cmdline_arg_to_str(arglist->args[arglistpos++]);
                    terminal_demo_screenshot_filename =
                        cmdline_arg_to_filename(arglist->args[arglistpos++]);
                    FILE *fp = fopen(infile, "rb");
                    if (!fp)
                        cmdline_error("无法打开输入文件 \"%s\"", infile);
                    demo_terminal_data = strbuf_new();
                    char buf[4096];
                    int retd;
                    while ((retd = fread(buf, 1, sizeof(buf), fp)) > 0)
                        put_data(demo_terminal_data, buf, retd);
                    fclose(fp);
                }
            } else if (!strcmp(p, "-sftp-list")) {
                /*
                 * cn: 自测入口。用给定主机建一条独立 SFTP 连接，
                 * 列出目录后退出。仅用于验证传输通道，不进 GUI。
                 */
                if (!arglist->args[arglistpos]) {
                    cmdline_error("%s 需要远端路径", p);
                } else {
                    sftp_list_path =
                        dupstr(cmdline_arg_to_str(arglist->args[arglistpos++]));
                }
            } else if (!strcmp(p, "-sftp-win")) {
                /* cn: 界面自检，打开双栏窗口检查几何后退出。 */
                sftp_win_selftest = true;
            } else if (!strcmp(p, "-sftp-open")) {
                /*
                 * cn: 只把双栏窗口开起来、不跑自检，用于人工看图与截图。
                 * 自检模式走完会自己销毁窗口，截不到图。
                 */
                sftp_win_open_only = true;
            } else if (!strcmp(p, "-sftp-open-fail")) {
                /*
                 * cn: 打开失败自检。故意连一个不可达主机/端口，让
                 * sftp_xfer_open 失败，验证「不弹模态框、不残留窗口、
                 * 错误文案上交」三条防复发断言。用法：
                 *   putty -sftp-open-fail -P 1 127.0.0.1
                 * 结果写 sftp-open-fail-selftest.txt。
                 */
                sftp_win_open_fail = true;
            } else if (!strcmp(p, "-sftp-recurse")) {
                /*
                 * cn: 目录递归传输自检。用法：
                 *   putty -sftp-recurse <远端目录> <本地目录>
                 * 把远端目录整棵下载到本地目录，结果写 sftp-recurse.txt。
                 */
                if (!arglist->args[arglistpos] ||
                    !arglist->args[arglistpos + 1]) {
                    cmdline_error("%s 需要远端目录与本地目录", p);
                } else {
                    sftp_recurse_src =
                        dupstr(cmdline_arg_to_str(arglist->args[arglistpos++]));
                    sftp_recurse_dst =
                        dupstr(cmdline_arg_to_str(arglist->args[arglistpos++]));
                }
            } else if (!strcmp(p, "-sftp-resume")) {
                /*
                 * cn: 断点续传自检。用法：
                 *   putty -sftp-resume <本地文件> <远端路径>
                 * 先完整上传，再故意造出半截文件，然后续传，验「没重发
                 * 已完成的字节」。结果写 sftp-resume.txt。
                 */
                if (!arglist->args[arglistpos] ||
                    !arglist->args[arglistpos + 1]) {
                    cmdline_error("%s 需要本地文件与远端路径", p);
                } else {
                    sftp_resume_src =
                        dupstr(cmdline_arg_to_str(arglist->args[arglistpos++]));
                    sftp_resume_dst =
                        dupstr(cmdline_arg_to_str(arglist->args[arglistpos++]));
                }
            } else if (*p != '-') {
                cmdline_error("意外的参数 \"%s\"", p);
            } else {
                cmdline_error("未知选项 \"%s\"", p);
            }
        }
    }

    cmdline_run_saved(conf);

    if (demo_config_box) {
        sesslist_demo_mode = true;
        load_open_settings(NULL, conf);
        conf_set_str(conf, CONF_host, "demo-server.example.com");
        do_config(conf);
        cleanup_exit(0);
    } else if (sftp_list_path) {
        /*
         * cn: 自测模式。这里不进 GUI，直接把结果写到标准输出。
         * 因为 putty 是 GUI 子系统程序，需要先把 stdout 接回来。
         */
        sftp_selftest_list(conf, sftp_list_path);
        cleanup_exit(0);
    } else if (sftp_win_selftest) {
        /* cn: 界面自检，检查完自己销毁窗口退出。 */
        sftp_selftest_window(conf);
        cleanup_exit(0);
    } else if (sftp_win_open_fail) {
        /* cn: 打开失败自检，不弹框、不残留窗口，结果写文件后退出。 */
        sftp_selftest_open_fail(conf);
        cleanup_exit(0);
    } else if (sftp_win_open_only) {
        /* cn: 只开窗口，等人看/截图，关窗后退出。 */
        sftp_window_detached(conf);
        cleanup_exit(0);
    } else if (sftp_recurse_src && sftp_recurse_dst) {
        /* cn: 目录递归传输自检。 */
        sftp_selftest_recurse_run(conf, sftp_recurse_src, sftp_recurse_dst);
        cleanup_exit(0);
    } else if (sftp_resume_src && sftp_resume_dst) {
        /* cn: 断点续传自检。 */
        sftp_selftest_resume_run(conf, sftp_resume_src, sftp_resume_dst);
        cleanup_exit(0);
    } else if (demo_terminal_data) {
        /* Ensure conf will cause an immediate session launch */
        load_open_settings(NULL, conf);
        conf_set_str(conf, CONF_host, "demo-server.example.com");
        conf_set_int(conf, CONF_close_on_exit, FORCE_OFF);
    } else {
        /*
         * Bring up the config dialog if the command line hasn't
         * (explicitly) specified a launchable configuration.
         */
        if (!(special_launchable_argument || cmdline_host_ok(conf))) {
            if (!do_config(conf))
                cleanup_exit(0);
            /*
             * cn: 配置框里的「文件传输」——用户的意图是开传输窗口，
             * 不是连终端。这里直接接手，不再往下走会话启动流程。
             */
            if (cn_take_xfer_request()) {
                sftp_window_detached(conf);
                cleanup_exit(0);
            }
        } else {
            /*
             * cn: 命令行/桌面快捷方式直接把主机名塞进 conf 时，同样可能
             * 带着 https:// 之类的协议前缀。这条路径不经过配置框，
             * 若不在这里拦，SSH 层会把冒号后的部分当端口丢掉，主机名
             * 退化成 "https"，最后只报「主机不存在」。这里给出明确的
             * 格式错误提示，然后转入配置框让用户改。
             */
            char *msg = cn_hostbox_scheme_complaint(conf);
            if (msg) {
                MessageBox(NULL, msg, "PuTTY 错误", MB_OK | MB_ICONERROR);
                sfree(msg);
                if (!do_config(conf))
                    cleanup_exit(0);
                if (cn_take_xfer_request()) {
                    sftp_window_detached(conf);
                    cleanup_exit(0);
                }
            }
        }
    }

    prepare_session(conf);
}

const struct BackendVtable *backend_vt_from_conf(Conf *conf)
{
    if (demo_terminal_data) {
        return &null_backend;
    }

    /*
     * Select protocol. This is farmed out into a table in a
     * separate file to enable an ssh-free variant.
     */
    const struct BackendVtable *vt = backend_vt_from_proto(
        conf_get_int(conf, CONF_protocol));
    if (!vt) {
        char *str = dupprintf("%s 内部错误", appname);
        MessageBox(NULL, "发现不支持的协议号",
                   str, MB_OK | MB_ICONEXCLAMATION);
        sfree(str);
        cleanup_exit(1);
    }
    return vt;
}

const wchar_t *get_app_user_model_id(void)
{
    return L"SimonTatham.PuTTY";
}

static void demo_terminal_screenshot(void *ctx, unsigned long now)
{
    HWND hwnd = (HWND)ctx;
    char *err = save_screenshot(hwnd, terminal_demo_screenshot_filename);
    if (err) {
        MessageBox(hwnd, err, "演示截图失败", MB_OK | MB_ICONERROR);
        sfree(err);
    }
    cleanup_exit(0);
}

void gui_terminal_ready(HWND hwnd, Seat *seat, Backend *backend)
{
    if (demo_terminal_data) {
        ptrlen data = ptrlen_from_strbuf(demo_terminal_data);
        seat_stdout(seat, data.ptr, data.len);
        schedule_timer(TICKSPERSEC, demo_terminal_screenshot, (void *)hwnd);
    }
}
