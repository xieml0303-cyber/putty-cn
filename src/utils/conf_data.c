#include "putty.h"

#define CONF_ENUM(name, ...)                                            \
    static const ConfSaveEnumValue conf_enum_values_##name[] = {        \
        __VA_ARGS__                                                     \
    }; const ConfSaveEnumType conf_enum_##name = {                      \
        .values = conf_enum_values_##name,                              \
        .nvalues = lenof(conf_enum_values_##name),                      \
    };

#define VALUE(eval, sval) { eval, sval, false }
#define VALUE_OBSOLETE(eval, sval) { eval, sval, true }

#include "conf-enums.h"

bool conf_enum_map_to_storage(const ConfSaveEnumType *etype,
                              int confval, int *storageval_out)
{
    for (size_t i = 0; i < etype->nvalues; i++)
        if (!etype->values[i].obsolete &&
            etype->values[i].confval == confval) {
            *storageval_out = etype->values[i].storageval;
            return true;
        }
    return false;
}

bool conf_enum_map_from_storage(const ConfSaveEnumType *etype,
                                int storageval, int *confval_out)
{
    for (size_t i = 0; i < etype->nvalues; i++)
        if (etype->values[i].storageval == storageval) {
            *confval_out = etype->values[i].confval;
            return true;
        }
    return false;
}

#define CONF_OPTION(id, ...) { __VA_ARGS__ },
#define VALUE_TYPE(x) .value_type = CONF_TYPE_ ## x
#define SUBKEY_TYPE(x) .subkey_type = CONF_TYPE_ ## x
#define DEFAULT_INT(x) .default_value.ival = x
#define DEFAULT_STR(x) .default_value.sval = x
#define DEFAULT_BOOL(x) .default_value.bval = x
#define SAVE_KEYWORD(x) .save_keyword = x
#define STORAGE_ENUM(x) .storage_enum = &conf_enum_ ## x
#define SAVE_CUSTOM .save_custom = true
#define LOAD_CUSTOM .load_custom = true
#define NOT_SAVED .not_saved = true

const ConfKeyInfo conf_key_info[] = {
    #include "conf.h"
};

/*
 * cn: The connection targets (host, port, protocol) are deliberately
 * NOT persisted by the save button — they are written as pristine
 * defaults (empty host, SSH / 22) and the startup box resets them.
 * They therefore do not count as "unsaved changes": neither the
 * dirty gate nor the change list takes them into account.
 */
static bool cn_is_connection_target(size_t key)
{
    return key == CONF_host || key == CONF_port ||
        key == CONF_protocol;
}

/*
 * cn: Chinese display names for commonly changed options, shown in
 * the change list instead of the English save keywords. Options not
 * in this table fall back to their save_keyword.
 */
static const struct { int key; const char *cn; } cn_option_names[] = {
    { CONF_font, "字体" },
    { CONF_font_quality, "字体质量" },
    { CONF_funky_type, "Linux 功能键" },
    { CONF_savelines, "回滚行数" },
    { CONF_close_on_exit, "退出时关闭窗口" },
    { CONF_wintitle, "窗口标题" },
    { CONF_blink_cur, "文本光标闪烁" },
    { CONF_answerback, "应答字符串" },
    { CONF_logtype, "会话日志记录" },
    { CONF_logfilename, "日志文件名" },
    { CONF_logxfovr, "日志文件已存在时" },
    { CONF_logflush, "快速刷新日志" },
    { CONF_logheader, "日志包含行首标题" },
    { CONF_logomitpass, "忽略密码域" },
    { CONF_logomitdata, "忽略会话数据" },
    { CONF_username, "自动登录用户名" },
    { CONF_termtype, "终端类型" },
    { CONF_termspeed, "终端速度" },
    { CONF_compression, "SSH 压缩" },
    { CONF_tcp_nodelay, "禁用 Nagle" },
    { CONF_tcp_keepalives, "TCP 保活" },
    { CONF_proxy_type, "代理类型" },
    { CONF_proxy_host, "代理主机" },
    { CONF_proxy_port, "代理端口" },
    { CONF_proxy_username, "代理用户名" },
    { CONF_scroll_on_key, "按键时回滚" },
    { CONF_scroll_on_disp, "输出时回滚" },
    { CONF_erase_to_scrollback, "回滚时擦除" },
    { CONF_alt_f4, "Alt+F4 快捷键" },
    { CONF_alt_space, "系统菜单快捷键" },
    { CONF_alwaysontop, "总是置顶" },
    { CONF_fullscreenonaltenter, "Alt+Enter 全屏" },
    { CONF_beep_ind, "响铃方式" },
    { CONF_bellovl, "蜂鸣过载保护" },
    { CONF_wrap_mode, "自动换行" },
    { CONF_dec_om, "DEC 起始模式" },
    { CONF_nethack_keypad, "NetHack 小键盘" },
    { CONF_app_cursor, "应用光标键" },
    { CONF_app_keypad, "应用小键盘" },
    { CONF_telnet_keyboard, "Telnet 键盘模式" },
    { CONF_telnet_newline, "Telnet 新行" },
    { CONF_serspeed, "波特率" },
    { CONF_serdatabits, "数据位" },
    { CONF_serstopbits, "停止位" },
    { CONF_serparity, "校验位" },
    { CONF_serflow, "流控" },
    { CONF_ssh_rekey_time, "密钥重协商时间" },
    { CONF_ssh_rekey_data, "密钥重协商数据量" },
    { CONF_addressfamily, "互联网协议版本" },
    { CONF_agentfwd, "启用代理转发" },
    { CONF_ansi_colour, "启用终端指定 ANSI 颜色" },
    { CONF_auth_plugin, "要运行的插件命令" },
    { CONF_bce, "使用背景颜色清除屏幕" },
    { CONF_beep, "发生响铃时动作" },
    { CONF_bellovl_n, "重复响铃最少数目" },
    { CONF_bellovl_s, "被禁用响铃的时间(秒)" },
    { CONF_bellovl_t, "重复响铃计算时间(秒)" },
    { CONF_bksp_is_delete, "Backspace 回退键" },
    { CONF_blinktext, "启用闪烁字文本" },
    { CONF_bold_style, "粗体文字的表示方式" },
    { CONF_change_username, "启用尝试在 SSH-2 中修改用户名" },
    { CONF_cjk_ambig_wide, "将 CJK 歧义字符按宽字符处理" },
    { CONF_crhaslf, "在每个 CR 字符后补充 LF 字符" },
    { CONF_cursor_type, "光标显示" },
    { CONF_detached_cert, "与私钥一起使用的证书（可选）" },
    { CONF_even_proxy_localhost, "本地连接也考虑使用代理" },
    { CONF_gssapifwd, "启用 GSSAPI 凭据委托" },
    { CONF_gssapirekey, "GSS 检查间隔 (单位：分钟) (0 表示从不)" },
    { CONF_height, "行" },
    { CONF_hide_mouseptr, "在窗口中输入时隐藏鼠标指针" },
    { CONF_keyfile, "认证私钥文件" },
    { CONF_lfhascr, "在每个 LF 字符前补充 CR 字符" },
    { CONF_localecho, "本地回应" },
    { CONF_localedit, "本地行编辑" },
    { CONF_lport_acceptall, "本地端口接受其他主机的连接" },
    { CONF_mouse_override, "Shift 上档键可与鼠标组合使用" },
    { CONF_mouseautocopy, "自动复制选择的文本" },
    { CONF_no_alt_screen, "禁用切换终端屏幕" },
    { CONF_no_applic_c, "禁用应用光标键模式" },
    { CONF_no_applic_k, "禁用应用小键盘模式" },
    { CONF_no_arabicshaping, "禁用修整阿拉伯文本" },
    { CONF_no_bidi, "禁用双向文本显示" },
    { CONF_no_bracketed_paste, "禁用括号粘贴模式" },
    { CONF_no_dbackspace, "禁用服务器发送 ^? 时破坏性回退删除" },
    { CONF_no_mouse_rep, "禁用 xterm 类型鼠标报告" },
    { CONF_no_remote_charset, "禁用远程控制字符集设置" },
    { CONF_no_remote_clearscroll, "禁用远程控制清除回滚" },
    { CONF_no_remote_resize, "禁用改变远程控制终端大小" },
    { CONF_no_remote_wintitle, "禁用改变远程控制窗口标题" },
    { CONF_nopty, "不分配伪终端" },
    { CONF_passive_telnet, "Telnet 通讯模式" },
    { CONF_paste_controls, "启用粘贴含有控制字符的文本" },
    { CONF_ping_interval, "空包发送时间间隔(秒，0 表示关闭)" },
    { CONF_pre_connect_command, "连接前运行命令" },
    { CONF_proxy_dns, "在代理端进行 DNS 域名解析" },
    { CONF_proxy_exclude_list, "排除在外的地址" },
    { CONF_proxy_log_to_term, "代理日志输出到终端" },
    { CONF_proxy_password, "密码" },
    { CONF_proxy_telnet_command, "要发送到代理的命令" },
    { CONF_rawcnp, "复制粘贴线条绘制字符为 lqqqk" },
    { CONF_rect_select, "默认选择模式(Alt 换档键拖放为另一种模式)" },
    { CONF_remote_cmd, "远程命令" },
    { CONF_remote_qtitle_action, "远程标题查询回应(SECURITY)" },
    { CONF_rfc_environ, "OLD_ENVIRON 歧义处理" },
    { CONF_rport_acceptall, "远程端口同样接受连接（仅 SSH-2）" },
    { CONF_rxvt_homeend, "Home 和 End 键" },
    { CONF_scrollbar, "显示滚动条" },
    { CONF_sharrow_type, "Shift/Ctrl/Alt 配合方向键" },
    { CONF_ssh2_des_cbc, "启用 SSH-2 兼容使用单一 DES 算法" },
    { CONF_ssh_connection_sharing, "共享可用的 SSH 连接" },
    { CONF_ssh_connection_sharing_downstream, "下游 (连接到上游 PuTTY)" },
    { CONF_ssh_connection_sharing_upstream, "上游 (连接到实际服务器)" },
    { CONF_ssh_gss_custom, "用户支持的 GSSAPI 库路径" },
    { CONF_ssh_no_shell, "完全不运行 Shell 或命令" },
    { CONF_ssh_no_trivial_userauth, "过于迅速认证成功时断开连接" },
    { CONF_ssh_no_userauth, "完全绕过认证，只限于 SSH-2" },
    { CONF_ssh_prefer_known_hostkeys, "已知主机密钥的首选算法" },
    { CONF_ssh_show_banner, "显示预认证提示，只限于 SSH-2" },
    { CONF_sshbug_chanreq, "回复已关闭通道的请求" },
    { CONF_sshbug_derivekey2, "SSH-2 加密密钥计算有误" },
    { CONF_sshbug_dropstart, "丢弃在握手前发送的数据" },
    { CONF_sshbug_filter_kexinit, "阻塞 PuTTY 完整的 KEXINIT" },
    { CONF_sshbug_hmac2, "SSH-2 HMAC 密钥计算有误" },
    { CONF_sshbug_ignore1, "无法处理 SSH-1 ignore 消息" },
    { CONF_sshbug_ignore2, "无法处理 SSH-2 ignore 消息" },
    { CONF_sshbug_maxpkt2, "忽略 SSH-2 最大包大小" },
    { CONF_sshbug_oldgex2, "只支持 pre-RFC4419 SSH-2 DH GEX" },
    { CONF_sshbug_pksessid2, "SSH-2 公钥认证中误用会话 ID" },
    { CONF_sshbug_plainpw1, "拒绝所有 SSH-1 明文密码认证" },
    { CONF_sshbug_rekey2, "无法正确处理 SSH-2 密钥重新交换" },
    { CONF_sshbug_rsa1, "阻塞 SSH-1 RSA 认证" },
    { CONF_sshbug_rsa_sha2_cert_userauth, "旧的 RSA/SHA2 证书算法命名" },
    { CONF_sshbug_rsapad2, "需要对 SSH-2 RSA 签名进行填充" },
    { CONF_sshbug_winadj, "无法处理 PuTTY 的 SSH-2 'winadj' 请求" },
    { CONF_sshprot, "首选的 SSH 协议版本" },
    { CONF_true_colour, "启用终端使用 24 位色" },
    { CONF_try_gssapi_auth, "尝试使用 GSSAPI 认证，只限于 SSH-2" },
    { CONF_try_gssapi_kex, "尝试 GSSAPI 密钥交换" },
    { CONF_try_ki_auth, "尝试 keyboard-interactive 认证 (SSH-2)" },
    { CONF_try_tis_auth, "尝试 TIS 或 CryptoCard 认证 (SSH-1)" },
    { CONF_tryagent, "尝试使用 Pageant 认证" },
    { CONF_username_from_env, "从环境变量获取用户名" },
    { CONF_utf8linedraw, "即使在 UTF-8 模式下也启用 VT100 线条绘制" },
    { CONF_vtmode, "线条绘制字符处理" },
    { CONF_warn_on_close, "关闭窗口时警告" },
    { CONF_width, "列" },
    { CONF_window_border, "文本与窗口边界的距离" },
    { CONF_x11_auth, "远程 X11 认证协议" },
    { CONF_x11_display, "X 显示位置" },
    { CONF_x11_forward, "启用 X11 转发" },
    { CONF_xterm_256_colour, "启用终端使用 xterm 256 色模式" },
};

static const char *cn_option_display_name(size_t key)
{
    for (size_t i = 0; i < lenof(cn_option_names); i++)
        if ((size_t)cn_option_names[i].key == key)
            return cn_option_names[i].cn;
    return conf_key_info[key].save_keyword;
}

/*
 * cn: Chinese display names for enumerated (multi-choice) settings.
 * One table per storage enum, mapping each internal value to the
 * option text shown in the UI. Looked up via the ConfKeyInfo's
 * storage_enum pointer; options whose enum is absent here fall back
 * to the raw number.
 */
typedef struct { int v; const char *name; } CnValueName;

static const CnValueName cnvals_addressfamily[] = {
    { ADDRTYPE_UNSPEC, "自动" }, { ADDRTYPE_IPV4, "IPv4" },
    { ADDRTYPE_IPV6, "IPv6" },
};
static const CnValueName cnvals_on_off_auto[] = {
    { FORCE_ON, "开" }, { FORCE_OFF, "关" }, { AUTO, "自动" },
};
static const CnValueName cnvals_off_auto_on[] = {
    { FORCE_OFF, "关" }, { AUTO, "自动" }, { FORCE_ON, "开" },
};
static const CnValueName cnvals_auto_off_on[] = {
    { AUTO, "自动" }, { FORCE_OFF, "关" }, { FORCE_ON, "开" },
};
static const CnValueName cnvals_off1_on2[] = {
    { FORCE_OFF, "关" }, { FORCE_ON, "开" },
};
static const CnValueName cnvals_ssh_protocol[] = {
    { 0, "SSH-2" }, { 3, "SSH-1" },
};
static const CnValueName cnvals_serparity[] = {
    { SER_PAR_NONE, "无" }, { SER_PAR_ODD, "奇" },
    { SER_PAR_EVEN, "偶" }, { SER_PAR_MARK, "标记" },
    { SER_PAR_SPACE, "空格" },
};
static const CnValueName cnvals_serflow[] = {
    { SER_FLOW_NONE, "无" }, { SER_FLOW_XONXOFF, "XON/XOFF" },
    { SER_FLOW_RTSCTS, "RTS/CTS" }, { SER_FLOW_DSRDTR, "DSR/DTR" },
};
static const CnValueName cnvals_proxy_type[] = {
    { PROXY_NONE, "无" }, { PROXY_SOCKS4, "SOCKS 4" },
    { PROXY_SOCKS5, "SOCKS 5" }, { PROXY_HTTP, "HTTP" },
    { PROXY_TELNET, "Telnet" }, { PROXY_CMD, "本地命令" },
    { PROXY_SSH_TCPIP, "SSH TCPIP" }, { PROXY_SSH_EXEC, "SSH exec" },
    { PROXY_SSH_SUBSYSTEM, "SSH subsystem" },
};
static const CnValueName cnvals_funky_type[] = {
    { FUNKY_TILDE, "Shift+3" }, { FUNKY_LINUX, "Linux" },
    { FUNKY_XTERM, "xterm" }, { FUNKY_VT400, "VT400+" },
    { FUNKY_VT100P, "VT100+" }, { FUNKY_SCO, "SCO" },
    { FUNKY_XTERM_216, "xterm 216" },
};
static const CnValueName cnvals_sharrow_type[] = {
    { SHARROW_APPLICATION, "应用序列" }, { SHARROW_BITMAP, "位图序列" },
};
static const CnValueName cnvals_remote_qtitle_action[] = {
    { TITLE_NONE, "无" }, { TITLE_EMPTY, "空标题" },
    { TITLE_REAL, "真实标题" },
};
static const CnValueName cnvals_cursor_type[] = {
    { CURSOR_BLOCK, "块状" }, { CURSOR_UNDERLINE, "下划线" },
    { CURSOR_VERTICAL_LINE, "竖线" },
};
static const CnValueName cnvals_beep[] = {
    { BELL_DISABLED, "禁用" }, { BELL_DEFAULT, "默认" },
    { BELL_VISUAL, "可视化" }, { BELL_WAVEFILE, "波形文件" },
    { BELL_PCSPEAKER, "机箱喇叭" },
};
static const CnValueName cnvals_beep_indication[] = {
    { B_IND_DISABLED, "禁用" }, { B_IND_FLASH, "闪烁" },
    { B_IND_STEADY, "常亮" },
};
static const CnValueName cnvals_resize_effect[] = {
    { RESIZE_TERM, "改变终端尺寸" }, { RESIZE_DISABLED, "禁止" },
    { RESIZE_FONT, "改变字体" }, { RESIZE_EITHER, "二者" },
};
static const CnValueName cnvals_font_quality[] = {
    { FQ_DEFAULT, "默认" }, { FQ_ANTIALIASED, "平滑" },
    { FQ_NONANTIALIASED, "不平滑" }, { FQ_CLEARTYPE, "ClearType" },
};
static const CnValueName cnvals_log_type[] = {
    { LGTYP_NONE, "无" }, { LGTYP_ASCII, "可打印输出" },
    { LGTYP_DEBUG, "所有会话输出" }, { LGTYP_PACKETS, "SSH 包" },
    { LGTYP_SSHRAW, "SSH 包和 RAW 数据" },
};
static const CnValueName cnvals_log_to_existing_file[] = {
    { LGXF_OVR, "覆盖" }, { LGXF_APN, "追加" }, { LGXF_ASK, "每次询问" },
};
static const CnValueName cnvals_bold_style[] = {
    { BOLD_STYLE_FONT, "粗体字体" }, { BOLD_STYLE_COLOUR, "粗体颜色" },
    { BOLD_STYLE_FONT | BOLD_STYLE_COLOUR, "字体加颜色" },
};
static const CnValueName cnvals_mouse_buttons[] = {
    { MOUSE_COMPROMISE, "折中" }, { MOUSE_XTERM, "xterm" },
    { MOUSE_WINDOWS, "Windows" },
};
static const CnValueName cnvals_line_drawing[] = {
    { VT_XWINDOWS, "Unicode" }, { VT_OEMANSI, "OEM ANSI" },
    { VT_OEMONLY, "仅 OEM" }, { VT_POORMAN, "简化绘制" },
    { VT_UNICODE, "Unicode 全部" },
};
static const CnValueName cnvals_x11_auth[] = {
    { X11_NO_AUTH, "无" }, { X11_MIT, "MIT-Magic-Cookie" },
    { X11_XDM, "XDM-Authorization" },
};

static const struct {
    const ConfSaveEnumType *etype;
    const CnValueName *values;
    int n;
} cn_enum_display[] = {
    { &conf_enum_addressfamily, cnvals_addressfamily, 3 },
    { &conf_enum_on_off_auto, cnvals_on_off_auto, 3 },
    { &conf_enum_off_auto_on, cnvals_off_auto_on, 3 },
    { &conf_enum_auto_off_on, cnvals_auto_off_on, 3 },
    { &conf_enum_off1_on2, cnvals_off1_on2, 2 },
    { &conf_enum_ssh_protocol, cnvals_ssh_protocol, 2 },
    { &conf_enum_serparity, cnvals_serparity, 5 },
    { &conf_enum_serflow, cnvals_serflow, 4 },
    { &conf_enum_proxy_type, cnvals_proxy_type, 9 },
    { &conf_enum_funky_type, cnvals_funky_type, 7 },
    { &conf_enum_sharrow_type, cnvals_sharrow_type, 2 },
    { &conf_enum_remote_qtitle_action, cnvals_remote_qtitle_action, 3 },
    { &conf_enum_cursor_type, cnvals_cursor_type, 3 },
    { &conf_enum_beep, cnvals_beep, 5 },
    { &conf_enum_beep_indication, cnvals_beep_indication, 3 },
    { &conf_enum_resize_effect, cnvals_resize_effect, 4 },
    { &conf_enum_font_quality, cnvals_font_quality, 4 },
    { &conf_enum_log_type, cnvals_log_type, 5 },
    { &conf_enum_log_to_existing_file, cnvals_log_to_existing_file, 3 },
    { &conf_enum_bold_style, cnvals_bold_style, 3 },
    { &conf_enum_mouse_buttons, cnvals_mouse_buttons, 3 },
    { &conf_enum_line_drawing, cnvals_line_drawing, 5 },
    { &conf_enum_x11_auth, cnvals_x11_auth, 3 },
};

/*
 * cn: display name for an enumerated int option value; NULL if the
 * option has no display table or the value is not mapped.
 */
static const char *cn_enum_display_name(const ConfKeyInfo *info, int val)
{
    if (!info->storage_enum)
        return NULL;
    for (size_t i = 0; i < lenof(cn_enum_display); i++)
        if (cn_enum_display[i].etype == info->storage_enum)
            for (int j = 0; j < cn_enum_display[i].n; j++)
                if (cn_enum_display[i].values[j].v == val)
                    return cn_enum_display[i].values[j].name;
    return NULL;
}

/*
 * cn: Compare two configurations field by field over conf_key_info.
 * Skips options that are never persisted (not_saved, save_custom,
 * mappings with subkeys) and the connection targets. Returns true if
 * identical.
 */
bool conf_equal(const Conf *a, const Conf *b)
{
    for (size_t key = 0; key < N_CONFIG_OPTIONS; key++) {
        const ConfKeyInfo *info = &conf_key_info[key];
        if (info->save_custom || info->not_saved)
            continue;
        if (info->subkey_type != CONF_TYPE_NONE)
            continue;
        if (cn_is_connection_target(key))
            continue;

        switch (info->value_type) {
          case CONF_TYPE_STR: {
            const char *sa = conf_get_str(a, key);
            const char *sb = conf_get_str(b, key);
            if (strcmp(sa, sb) != 0)
                return false;
            break;
          }
          case CONF_TYPE_STR_AMBI: {
            bool a_utf8, b_utf8;
            const char *sa = conf_get_str_ambi(a, key, &a_utf8);
            const char *sb2 = conf_get_str_ambi(b, key, &b_utf8);
            if (strcmp(sa, sb2) != 0)
                return false;
            break;
          }
          case CONF_TYPE_INT:
            if (conf_get_int(a, key) != conf_get_int(b, key))
                return false;
            break;
          case CONF_TYPE_BOOL:
            if (conf_get_bool(a, key) != conf_get_bool(b, key))
                return false;
            break;
          case CONF_TYPE_FILENAME:
            if (strcmp(conf_get_filename(a, key)->utf8path,
                       conf_get_filename(b, key)->utf8path) != 0)
                return false;
            break;
          case CONF_TYPE_FONT:
            if (strcmp(conf_get_fontspec(a, key)->name,
                       conf_get_fontspec(b, key)->name) != 0 ||
                conf_get_fontspec(a, key)->height !=
                conf_get_fontspec(b, key)->height)
                return false;
            break;
          default:
            break;
        }
    }
    return true;
}

/*
 * cn: Describe the differences between two configurations, one line
 * per changed option, formatted as "关键字: 旧值 → 新值". Used by the
 * save-confirmation and close prompts so the user can see exactly
 * what is about to be persisted.
 *
 * CONF_protocol is special-cased to display protocol names rather
 * than raw enum numbers.
 */
char *conf_describe_changes(const Conf *baseline, const Conf *current)
{
    strbuf *sb = strbuf_new();
    int nchanges = 0;

    for (size_t key = 0; key < N_CONFIG_OPTIONS; key++) {
        const ConfKeyInfo *info = &conf_key_info[key];
        if (info->save_custom || info->not_saved)
            continue;
        if (info->subkey_type != CONF_TYPE_NONE)
            continue;
        if (cn_is_connection_target(key))
            continue;

        switch (info->value_type) {
          case CONF_TYPE_STR: {
            const char *a = conf_get_str(baseline, key);
            const char *b = conf_get_str(current, key);
            if (strcmp(a, b) != 0) {
                put_fmt(sb, "%s: %s → %s\r\n", cn_option_display_name(key),
                        *a ? a : "(空)", *b ? b : "(空)");
                nchanges++;
            }
            break;
          }
          case CONF_TYPE_STR_AMBI: {
            bool a_utf8, b_utf8;
            const char *a = conf_get_str_ambi(baseline, key, &a_utf8);
            const char *b = conf_get_str_ambi(current, key, &b_utf8);
            if (strcmp(a, b) != 0) {
                put_fmt(sb, "%s: %s → %s\r\n", cn_option_display_name(key),
                        *a ? a : "(空)", *b ? b : "(空)");
                nchanges++;
            }
            break;
          }
          case CONF_TYPE_INT: {
            int a = conf_get_int(baseline, key);
            int b = conf_get_int(current, key);
            if (a != b) {
                const char *an = cn_enum_display_name(info, a);
                const char *bn = cn_enum_display_name(info, b);
                if (an && bn)
                    put_fmt(sb, "%s: %s → %s\r\n",
                            cn_option_display_name(key), an, bn);
                else
                    put_fmt(sb, "%s: %d → %d\r\n",
                            cn_option_display_name(key), a, b);
                nchanges++;
            }
            break;
          }
          case CONF_TYPE_BOOL:
            if (conf_get_bool(baseline, key) !=
                conf_get_bool(current, key)) {
                put_fmt(sb, "%s: %s → %s\r\n", cn_option_display_name(key),
                        conf_get_bool(baseline, key) ? "开" : "关",
                        conf_get_bool(current, key) ? "开" : "关");
                nchanges++;
            }
            break;
          case CONF_TYPE_FILENAME: {
            const Filename *a = conf_get_filename(baseline, key);
            const Filename *b = conf_get_filename(current, key);
            if (strcmp(a->utf8path, b->utf8path) != 0) {
                put_fmt(sb, "%s: %s → %s\r\n", cn_option_display_name(key),
                        *a->utf8path ? a->utf8path : "(空)",
                        *b->utf8path ? b->utf8path : "(空)");
                nchanges++;
            }
            break;
          }
          case CONF_TYPE_FONT: {
            const FontSpec *a = conf_get_fontspec(baseline, key);
            const FontSpec *b = conf_get_fontspec(current, key);
            if (strcmp(a->name, b->name) != 0 || a->height != b->height) {
                put_fmt(sb, "%s: %s %d → %s %d\r\n",
                        cn_option_display_name(key),
                        a->name, a->height, b->name, b->height);
                nchanges++;
            }
            break;
          }
          default:
            break;
        }
    }

    if (nchanges == 0)
        return dupstr("(无可见改动)");

    return strbuf_to_str(sb);
}
