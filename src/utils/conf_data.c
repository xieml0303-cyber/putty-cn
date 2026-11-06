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
 * cn: The connection targets (host, port, protocol, username) are
 * deliberately NOT persisted by the save button — they are written as
 * pristine defaults (empty host, SSH / 22, empty username) and the
 * startup box resets them. They therefore do not count as "unsaved
 * changes": neither the dirty gate nor the change list takes them into
 * account.
 */
static bool cn_is_connection_target(size_t key)
{
    return key == CONF_host || key == CONF_port ||
        key == CONF_protocol || key == CONF_username;
}

/*
 * cn: Chinese display names for commonly changed options, shown in
 * the change list instead of the English save keywords. Options not
 * in this table fall back to their save_keyword.
 */
static const struct { int key; const char *cn; } cn_option_names[] = {
    { CONF_font, "字体" },
    { CONF_font_quality, "字体质量" },
    { CONF_font_varpitch, "启用可变宽度字体" },
    { CONF_funky_type, "Fn 功能键和小键盘" },
    { CONF_savelines, "回滚行数" },
    { CONF_close_on_exit, "退出时关闭窗口" },
    { CONF_wintitle, "窗口标题" },
    { CONF_blink_cur, "光标闪烁" },
    { CONF_answerback, "^E 回应" },
    { CONF_logtype, "会话日志记录" },
    { CONF_logfilename, "日志文件名" },
    { CONF_logxfovr, "要记录的日志文件已存在时" },
    { CONF_loghost, "远程主机的逻辑名称" },
    { CONF_logflush, "快速刷新缓存到日志文件" },
    { CONF_logheader, "包含行首标题" },
    { CONF_logomitpass, "忽略已知的密码域" },
    { CONF_logomitdata, "忽略会话数据" },
    { CONF_username, "自动登录用户名" },
    { CONF_termtype, "终端类型字符串" },
    { CONF_termspeed, "终端速度" },
    { CONF_xlat_capslockcyr, "Caps Lock 大小写按键用于 Cyrillic 切换" },
    { CONF_line_codepage, "远程字符集" },
    { CONF_compression, "启用压缩" },
    { CONF_tcp_nodelay, "禁用 Nagle 算法(TCP_NODELAY 参数)" },
    { CONF_tcp_keepalives, "启用 TCP 保持活动连接(SO_KEEPALIVE 参数)" },
    { CONF_proxy_type, "代理类型" },
    { CONF_proxy_host, "代理地址" },
    { CONF_proxy_port, "端口" },
    { CONF_proxy_username, "用户名" },
    { CONF_scroll_on_key, "按键时重置回滚" },
    { CONF_try_palette, "尝试使用逻辑调色板" },
    { CONF_system_colour, "使用系统颜色" },
    { CONF_scroll_on_disp, "刷新显示时重置回滚" },
    { CONF_erase_to_scrollback, "将清除的文本压入回滚" },
    { CONF_alt_f4, "Alt+F4 快捷键" },
    { CONF_alt_only, "Alt 键显示系统菜单" },
    { CONF_compose_key, "AltGr 用作 Compose 键" },
    { CONF_ctrlaltkeys, "Ctrl+Alt 键与 AltGr 不同" },
    { CONF_alt_space, "系统菜单快捷键" },
    { CONF_alwaysontop, "总是置顶" },
    { CONF_sunken_edge, "下沉边框（略厚）" },
    { CONF_win_name_always, "窗口标题与图标标题分开显示" },
    { CONF_scrollbar_in_fullscreen, "全屏模式显示滚动条" },
    { CONF_resize_action, "当窗口大小被改变时" },
    { CONF_fullscreenonaltenter, "Alt+Enter 全屏" },
    { CONF_beep_ind, "响铃方式" },
    { CONF_bell_wavefile, "响铃时播放的声音文件" },
    { CONF_bellovl, "大量重复响铃时临时禁用响铃" },
    { CONF_wrap_mode, "初始启用自动换行" },
    { CONF_dec_om, "初始启用 DEC 原始模式" },
    { CONF_nethack_keypad, "NetHack 小键盘" },
    { CONF_app_cursor, "光标键初始状态" },
    { CONF_app_keypad, "数字小键盘初始状态" },
    { CONF_telnet_keyboard, "键盘直接发送 Telnet 特殊命令" },
    { CONF_telnet_newline, "发送 Telnet 新行命令替换 ^M 字符" },
    { CONF_serspeed, "波特率" },
    { CONF_serline, "串口" },
    { CONF_printer, "打印发送 ANSI 打印机输出到" },
    { CONF_portfwd, "端口转发" },
    { CONF_environmt, "环境变量" },
    { CONF_ssh_manual_hostkeys, "手动配置的主机密钥" },
    { CONF_serdatabits, "数据位" },
    { CONF_serstopbits, "停止位" },
    { CONF_serparity, "校验位" },
    { CONF_serflow, "流控" },
    { CONF_ssh_rekey_time, "重新交换最长时间(分钟，0 不限制)" },
    { CONF_ssh_rekey_data, "重新交换最大数据量(0 为不限制)" },
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
    { CONF_mouseautocopy, "自动复制选择的文本到" },
    { CONF_rtf_paste, "以 RTF 和纯文本格式复制到剪贴板" },
    { CONF_mouse_is_xterm, "鼠标按键动作" },
    { CONF_mousepaste, "鼠标粘贴动作" },
    { CONF_ctrlshiftins, "{Ctrl,Shift} + Ins" },
    { CONF_ctrlshiftcv, "Ctrl + Shift + {C,V}" },
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
    { CONF_proxy_log_to_term, "在终端窗口输出代理诊断信息" },
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
    { CONF_username_from_env, "未指定用户名时" },
    { CONF_utf8linedraw, "即使在 UTF-8 模式下也启用 VT100 线条绘制" },
    { CONF_vtmode, "线条绘制字符处理" },
    { CONF_warn_on_close, "关闭窗口时警告" },
    { CONF_width, "列" },
    { CONF_window_border, "文本与窗口边界的距离" },
    { CONF_x11_auth, "远程 X11 认证协议" },
    { CONF_xauthfile, "用于本地显示的 X 认证文件" },
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
    /*
     * cn: Kept in step with the UI wording for completeness. The
     * display path actually goes through cnvals_sshprot_ui, registered
     * against CONF_sshprot in cn_option_values, because the shared
     * storage enum cannot carry "1 (不安全)" on its own.
     */
    { 0, "2" }, { 3, "1 (不安全)" },
};
static const CnValueName cnvals_serparity[] = {
    { SER_PAR_NONE, "无" }, { SER_PAR_ODD, "奇" },
    { SER_PAR_EVEN, "偶" }, { SER_PAR_MARK, "始终为 1" },
    { SER_PAR_SPACE, "始终为 0" },
};
static const CnValueName cnvals_serflow[] = {
    { SER_FLOW_NONE, "无" }, { SER_FLOW_XONXOFF, "XON/XOFF" },
    { SER_FLOW_RTSCTS, "RTS/CTS" }, { SER_FLOW_DSRDTR, "DSR/DTR" },
};
static const CnValueName cnvals_proxy_type[] = {
    { PROXY_NONE, "无" }, { PROXY_SOCKS4, "SOCKS 4" },
    { PROXY_SOCKS5, "SOCKS 5" }, { PROXY_HTTP, "HTTP 连接" },
    { PROXY_TELNET, "Telnet（发送临时命令）" },
    { PROXY_CMD, "本地（运行外部程序连接）" },
    { PROXY_SSH_TCPIP, "通过 SSH 代理使用端口转发" },
    { PROXY_SSH_EXEC, "通过 SSH 代理执行命令" },
    { PROXY_SSH_SUBSYSTEM, "通过 SSH 代理调用子系统" },
};
static const CnValueName cnvals_funky_type[] = {
    { FUNKY_TILDE, "ESC[n~" }, { FUNKY_LINUX, "Linux" },
    { FUNKY_XTERM, "Xterm R6" }, { FUNKY_VT400, "VT400" },
    { FUNKY_VT100P, "VT100+" }, { FUNKY_SCO, "SCO" },
    { FUNKY_XTERM_216, "Xterm 216+" },
};
static const CnValueName cnvals_sharrow_type[] = {
    { SHARROW_APPLICATION, "应用序列" }, { SHARROW_BITMAP, "位图序列" },
};
static const CnValueName cnvals_remote_qtitle_action[] = {
    { TITLE_NONE, "无" }, { TITLE_EMPTY, "空字符串" },
    { TITLE_REAL, "窗口标题" },
};
static const CnValueName cnvals_cursor_type[] = {
    { CURSOR_BLOCK, "显示块" }, { CURSOR_UNDERLINE, "下划线" },
    { CURSOR_VERTICAL_LINE, "垂直线" },
};
static const CnValueName cnvals_beep[] = {
    { BELL_DISABLED, "无 (禁用响铃)" }, { BELL_DEFAULT, "使用系统默认警告声音" },
    { BELL_VISUAL, "可视响铃 (闪动窗口)" },
    { BELL_WAVEFILE, "播放指定的声音文件" },
    { BELL_PCSPEAKER, "使用 PC 扬声器鸣叫" },
};
static const CnValueName cnvals_beep_indication[] = {
    { B_IND_DISABLED, "禁用" }, { B_IND_FLASH, "闪烁" },
    { B_IND_STEADY, "常亮" },
};
static const CnValueName cnvals_resize_effect[] = {
    { RESIZE_TERM, "改变行列数" }, { RESIZE_DISABLED, "完全禁用窗口大小改变" },
    { RESIZE_FONT, "改变字体大小" }, { RESIZE_EITHER, "只在最大化时改变字体大小" },
};
static const CnValueName cnvals_font_quality[] = {
    { FQ_DEFAULT, "默认" }, { FQ_ANTIALIASED, "抗锯齿" },
    { FQ_NONANTIALIASED, "无抗锯齿" }, { FQ_CLEARTYPE, "ClearType" },
};
static const CnValueName cnvals_log_type[] = {
    { LGTYP_NONE, "无" }, { LGTYP_ASCII, "可打印输出" },
    { LGTYP_DEBUG, "所有会话输出" }, { LGTYP_PACKETS, "SSH 包" },
    { LGTYP_SSHRAW, "SSH 包和 RAW 数据" },
};
static const CnValueName cnvals_log_to_existing_file[] = {
    { LGXF_OVR, "总是覆盖" }, { LGXF_APN, "总是添加到末尾" }, { LGXF_ASK, "每次询问" },
};
static const CnValueName cnvals_bold_style[] = {
    { BOLD_STYLE_FONT, "字体" }, { BOLD_STYLE_COLOUR, "颜色" },
    { BOLD_STYLE_FONT | BOLD_STYLE_COLOUR, "两者" },
};
static const CnValueName cnvals_mouse_buttons[] = {
    /*
     * cn: Kept in step with the Windows UI wording. Display actually
     * goes through cnvals_mouse_is_xterm_ui, registered against
     * CONF_mouse_is_xterm in cn_option_values.
     */
    { MOUSE_COMPROMISE, "混合模式 (中键扩展，右键粘贴)" },
    { MOUSE_XTERM, "xterm (右键扩展，中键粘贴)" },
    { MOUSE_WINDOWS, "Windows (中键扩展，右键弹出菜单)" },
};
/*
 * cn: The three-way "鼠标按键动作" radio, as spelled by the labels in
 * windows/config.c. The storage enum is shared, so the wording has to
 * live here rather than in cnvals_mouse_buttons.
 */
static const CnValueName cnvals_mouse_is_xterm_ui[] = {
    { MOUSE_WINDOWS, "Windows (中键扩展，右键弹出菜单)" },
    { MOUSE_COMPROMISE, "混合模式 (中键扩展，右键粘贴)" },
    { MOUSE_XTERM, "xterm (右键扩展，中键粘贴)" },
};
static const CnValueName cnvals_line_drawing[] = {
    { VT_XWINDOWS, "字体使用 X 窗口系统编码" },
    { VT_OEMANSI, "ANSI 和 OEM 模式下都使用字体" },
    { VT_OEMONLY, "仅在 OEM 模式下使用字体" },
    { VT_POORMAN, "简单线条绘制（+、- 和 |）" },
    { VT_UNICODE, "使用 Unicode 线条绘制码位" },
};
static const CnValueName cnvals_x11_auth[] = {
    { X11_NO_AUTH, "无" }, { X11_MIT, "MIT-Magic-Cookie-1" },
    { X11_XDM, "XDM-Authorization-1" },
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
 * cn: Per-key value names for radio-button settings whose option text
 * cannot be derived from the storage enum.
 *
 * Two separate problems force this to be keyed on the option rather
 * than on the enum:
 *
 *  1. A boolean option presented as a two-way radio button has no
 *     storage enum at all, yet its on-screen text is a pair of words
 *     ("Control-H" / "Control-? (127)"), not "开" / "关". Printing a
 *     bare bool as 开/关 tells the user nothing about what actually
 *     changed.
 *
 *  2. Several options share one storage enum but show different words
 *     in the UI. CONF_localecho shows 自动/强制开/强制关 while
 *     CONF_proxy_dns, on the same off_auto_on enum, shows 否/自动/是.
 *     Mapping by enum would therefore be wrong for at least one of
 *     them.
 *
 * CONF_app_keypad is a third case again: the single "数字小键盘初始
 * 状态" control edits two bools (app_keypad + nethack_keypad), so its
 * three states are resolved by a dedicated helper rather than a plain
 * table.
 *
 * Entries here take precedence over cn_enum_display[].
 */
static const CnValueName cnvals_bksp_is_delete[] = {
    { 0, "Control-H" }, { 1, "Control-? (127)" },
};
static const CnValueName cnvals_rxvt_homeend[] = {
    { 0, "标准" }, { 1, "rxvt" },
};
static const CnValueName cnvals_app_cursor[] = {
    { 0, "常规" }, { 1, "应用" },
};
static const CnValueName cnvals_app_keypad[] = {
    { 0, "常规" }, { 1, "应用" }, { 2, "NetHack" },
};
static const CnValueName cnvals_rect_select[] = {
    { 0, "常规" }, { 1, "矩形框" },
};
static const CnValueName cnvals_username_from_env[] = {
    { 0, "提示" }, { 1, "使用系统用户名" },
};
static const CnValueName cnvals_rfc_environ[] = {
    { 0, "BSD (常用)" }, { 1, "RFC 1408 (不常用)" },
};
static const CnValueName cnvals_passive_telnet[] = {
    { 0, "主动" }, { 1, "被动" },
};
static const CnValueName cnvals_sharrow_type_ui[] = {
    { SHARROW_APPLICATION, "Ctrl 切换应用模式" },
    { SHARROW_BITMAP, "xterm 风格位图" },
};

/*
 * cn: the shared off_auto_on / on_off_auto enums, spelled the way the
 * individual controls spell them. These cannot live in
 * cn_enum_display[] because one storage enum is used by several
 * controls that word the same value differently.
 */
static const CnValueName cnvals_on_off_auto_ui[] = {
    { FORCE_ON, "强制开" }, { FORCE_OFF, "强制关" }, { AUTO, "自动" },
};
static const CnValueName cnvals_off_auto_on_ui[] = {
    { FORCE_OFF, "否" }, { AUTO, "自动" }, { FORCE_ON, "是" },
};
static const CnValueName cnvals_proxy_log_to_term_ui[] = {
    { FORCE_OFF, "否" }, { FORCE_ON, "是" }, { AUTO, "仅限会话开始前" },
};
static const CnValueName cnvals_sshprot_ui[] = {
    { 0, "1 (不安全)" }, { 3, "2" },
};

/*
 * cn: The clipboard-action dropdowns, as spelled by the labels in
 * config.c (clipboard_selector_handler) and by CLIPNAME_* in the
 * platform header. Windows builds have no NAMED_CLIPBOARDS, so the
 * only three choices are these.
 */
static const CnValueName cnvals_clipboard_action[] = {
    { CLIPUI_NONE,     "无动作" },
    { CLIPUI_IMPLICIT, "上次选择的文本" },
    { CLIPUI_EXPLICIT, "系统剪贴板" },
    { CLIPUI_CUSTOM,   "自定义剪贴板" },
};

/* Radio/enum options whose UI text differs from the generic enum table. */
static const struct {
    size_t key;
    const CnValueName *values;
    int n;
} cn_option_values[] = {
    { CONF_bksp_is_delete,  cnvals_bksp_is_delete,  2 },
    { CONF_rxvt_homeend,    cnvals_rxvt_homeend,    2 },
    { CONF_app_cursor,      cnvals_app_cursor,      2 },
    { CONF_app_keypad,      cnvals_app_keypad,      3 },
    { CONF_rect_select,     cnvals_rect_select,     2 },
    { CONF_username_from_env, cnvals_username_from_env, 2 },
    { CONF_rfc_environ,     cnvals_rfc_environ,     2 },
    { CONF_passive_telnet,  cnvals_passive_telnet,  2 },
    { CONF_sharrow_type,    cnvals_sharrow_type_ui, 2 },
    /* Enums whose shared storage table carries the wrong words. */
    { CONF_localecho,       cnvals_on_off_auto_ui,  3 },
    { CONF_localedit,       cnvals_on_off_auto_ui,  3 },
    { CONF_proxy_dns,       cnvals_off_auto_on_ui,  3 },
    { CONF_proxy_log_to_term, cnvals_proxy_log_to_term_ui, 3 },
    { CONF_sshprot,         cnvals_sshprot_ui,      2 },
    { CONF_mouse_is_xterm,  cnvals_mouse_is_xterm_ui, 3 },
    { CONF_mousepaste,      cnvals_clipboard_action, 4 },
    { CONF_ctrlshiftins,    cnvals_clipboard_action, 4 },
    { CONF_ctrlshiftcv,     cnvals_clipboard_action, 4 },
};

/*
 * cn: Chinese value names for the shared enums, as the UI actually
 * spells them (as opposed to cn_enum_display[] above, which is keyed
 * on the storage enum and therefore cannot hold conflicting spellings).
 */
static const char *cn_option_value_name(size_t key, int val)
{
    for (size_t i = 0; i < lenof(cn_option_values); i++)
        if (cn_option_values[i].key == key)
            for (int j = 0; j < cn_option_values[i].n; j++)
                if (cn_option_values[i].values[j].v == val)
                    return cn_option_values[i].values[j].name;
    return NULL;
}

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
 * cn: Compare a string-to-string mapping (SUBKEY_TYPE(STR) with
 * VALUE_TYPE(STR)) between two configs: port forwardings, environment
 * variables and manual host keys are all stored this way.
 *
 * The iteration order of conf_get_str_strs() follows the tree, so both
 * sides walk their subkeys in the same order and a straight pairwise
 * comparison is sound. Returns false on the first difference.
 */
static bool cn_map_equal(Conf *a, Conf *b, int key)
{
    char *keypos = NULL;   /* subkey to start after; NULL for the first */
    char *ka, *kb;

    for (;;) {
        char *va = conf_get_str_strs(a, key, keypos, &ka);
        char *vb = conf_get_str_strs(b, key, keypos, &kb);
        if (!va || !vb)
            return !va && !vb;          /* both exhausted, or one is longer */
        if (strcmp(ka, kb) != 0 || strcmp(va, vb) != 0)
            return false;
        /*
         * Reuse one side's subkey as the cursor. conf_get_str_strs()
         * only reads it during the lookup, so pointing at the tree's
         * own string is safe here.
         */
        keypos = ka;
    }
}

/*
 * cn: conf_equal takes const Conf pointers but the mapping accessors do
 * not, so this thin cast wrapper keeps the const-correct signature at
 * the call sites. Nothing in cn_map_equal modifies the config.
 */
static bool cn_map_equal_const(const Conf *a, const Conf *b, int key)
{
    return cn_map_equal((Conf *)a, (Conf *)b, key);
}

/*
 * cn: Emit one line per added, removed or altered mapping entry, in
 * the same "【name】 old → new" shape as the scalar cases. Keys only
 * present on one side are shown against "(无)" so the direction is
 * obvious.
 */
static int cn_map_describe(strbuf *sb, Conf *base, Conf *cur, int key)
{
    int n = 0;
    char *keypos = NULL;
    char *kb, *kc;

    for (;;) {
        char *vb = conf_get_str_strs(base, key, keypos, &kb);
        char *vc = conf_get_str_strs(cur, key, keypos, &kc);
        if (!vb && !vc)
            break;

        if (vb && vc && strcmp(kb, kc) == 0) {
            if (strcmp(vb, vc) != 0) {
                put_fmt(sb, "【%s】 %s：%s → %s\r\n",
                        cn_option_display_name(key), kb, vb, vc);
                n++;
            }
            keypos = kb;
            continue;
        }

        /*
         * One side is missing this subkey, or the two sides have
         * diverged. Report the one that sorts first and advance only
         * that side, so insertions and deletions both come out.
         */
        if (vb && (!vc || strcmp(kb, kc) < 0)) {
            put_fmt(sb, "【%s】 %s：%s → (无)\r\n",
                    cn_option_display_name(key), kb, vb);
            n++;
            keypos = kb;
        } else {
            put_fmt(sb, "【%s】 %s：(无) → %s\r\n",
                    cn_option_display_name(key), kc, vc);
            n++;
            keypos = kc;
        }
    }
    return n;
}

/*
 * cn: Compare two configurations field by field over conf_key_info.
 * Skips options that are never persisted (not_saved) and the
 * connection targets. Returns true if identical.
 *
 * String-to-string mappings (port forwardings, environment variables,
 * manual host keys) are compared entry by entry, because leaving them
 * out meant adding or deleting a port forwarding never woke the Save
 * button up and never appeared in the change list.
 *
 * save_custom is deliberately NOT skipped here. It only means that
 * loading/saving uses hand-written code instead of the generic
 * keyword scanner; the value still lives in the Conf in the ordinary
 * scalar form, so it can be compared normally. Skipping it used to
 * make whole groups of controls invisible to the dirty check — the
 * three clipboard-action dropdowns (CONF_mousepaste,
 * CONF_ctrlshiftins, CONF_ctrlshiftcv), the ping interval and the
 * GSSAPI options all failed to light up the Save button at all.
 */
bool conf_equal(const Conf *a, const Conf *b)
{
    for (size_t key = 0; key < N_CONFIG_OPTIONS; key++) {
        const ConfKeyInfo *info = &conf_key_info[key];
        if (info->not_saved)
            continue;
        /*
         * Mappings need their own comparison. Only the string-to-string
         * ones have an iteration API here (portforwardings, environment
         * variables, manual host keys); the int-keyed tables (colours,
         * wordness, the SSH preference lists) have no public iterator,
         * so they stay excluded.
         */
        if (info->subkey_type != CONF_TYPE_NONE) {
            if (info->subkey_type == CONF_TYPE_STR &&
                info->value_type == CONF_TYPE_STR &&
                !cn_map_equal_const(a, b, key))
                return false;
            continue;
        }
        if (cn_is_connection_target(key))
            continue;
        /*
         * These two only get an entry in the Conf tree when
         * OSX_META_KEY_CONFIG is defined (see load_open_settings), so
         * on other platforms conf_get_*() would assert on them. The
         * rest of the save_custom keys are fine: their custom loader
         * always creates the entry.
         */
        if (key == CONF_osx_option_meta || key == CONF_osx_command_meta)
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
 * per changed option, formatted as "【关键字】 旧值 → 新值". Used by
 * the save-confirmation and close prompts so the user can see exactly
 * what is about to be persisted.
 *
 * The option name is wrapped in full-width brackets so it reads as a
 * heading and visually separates from the value that follows; these
 * prompts use plain-text controls that cannot carry per-character
 * styling (bold or underline), so the distinction has to come from
 * the characters themselves.
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
        if (info->not_saved)
            continue;
        /*
         * cn: String-to-string mappings get a line per changed entry,
         * so adding or deleting a port forwarding shows up in the
         * confirmation prompt. Other mapping types have no public
         * iterator and stay excluded, matching conf_equal.
         */
        if (info->subkey_type != CONF_TYPE_NONE) {
            if (info->subkey_type == CONF_TYPE_STR &&
                info->value_type == CONF_TYPE_STR)
                nchanges += cn_map_describe(sb, (Conf *)baseline,
                                            (Conf *)current, key);
            continue;
        }
        if (cn_is_connection_target(key))
            continue;

        /*
         * cn: CONF_mousepaste_custom and friends only carry a string
         * when the corresponding dropdown is set to "custom clipboard",
         * which Windows builds cannot do (NAMED_CLIPBOARDS is not
         * defined). Reporting them would show a value the user never
         * picked, so the three dropdowns below are reported on their
         * own.
         */
        if (key == CONF_mousepaste_custom ||
            key == CONF_ctrlshiftins_custom ||
            key == CONF_ctrlshiftcv_custom)
            continue;

        /*
         * cn: No entry in the Conf tree unless OSX_META_KEY_CONFIG is
         * defined; see the matching comment in conf_equal.
         */
        if (key == CONF_osx_option_meta || key == CONF_osx_command_meta)
            continue;

        /*
         * cn: CONF_nethack_keypad has no control of its own — it is the
         * third state of the single "数字小键盘初始状态" radio button,
         * which writes both CONF_app_keypad and CONF_nethack_keypad.
         * Report it through CONF_app_keypad below so the list shows one
         * line with the three-state wording instead of two separate
         * 开/关 lines matching no control the user ever saw.
         */
        if (key == CONF_nethack_keypad)
            continue;
        if (key == CONF_app_keypad) {
            int a = conf_get_bool(baseline, CONF_nethack_keypad) ? 2 :
                conf_get_bool(baseline, CONF_app_keypad);
            int b = conf_get_bool(current, CONF_nethack_keypad) ? 2 :
                conf_get_bool(current, CONF_app_keypad);
            if (a != b) {
                put_fmt(sb, "【%s】 %s → %s\r\n",
                        cn_option_display_name(key),
                        cn_option_value_name(key, a),
                        cn_option_value_name(key, b));
                nchanges++;
            }
            continue;
        }

        switch (info->value_type) {
          case CONF_TYPE_STR: {
            const char *a = conf_get_str(baseline, key);
            const char *b = conf_get_str(current, key);
            if (strcmp(a, b) != 0) {
                put_fmt(sb, "【%s】 %s → %s\r\n",
                        cn_option_display_name(key),
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
                put_fmt(sb, "【%s】 %s → %s\r\n",
                        cn_option_display_name(key),
                        *a ? a : "(空)", *b ? b : "(空)");
                nchanges++;
            }
            break;
          }
          case CONF_TYPE_INT: {
            int a = conf_get_int(baseline, key);
            int b = conf_get_int(current, key);
            if (a != b) {
                const char *an = cn_option_value_name(key, a);
                const char *bn = cn_option_value_name(key, b);
                if (!an)
                    an = cn_enum_display_name(info, a);
                if (!bn)
                    bn = cn_enum_display_name(info, b);
                if (an && bn)
                    put_fmt(sb, "【%s】 %s → %s\r\n",
                            cn_option_display_name(key), an, bn);
                else
                    put_fmt(sb, "【%s】 %d → %d\r\n",
                            cn_option_display_name(key), a, b);
                nchanges++;
            }
            break;
          }
          case CONF_TYPE_BOOL:
            if (conf_get_bool(baseline, key) !=
                conf_get_bool(current, key)) {
                /*
                 * A bool shown as a two-way radio button has proper
                 * option text; only a genuine checkbox is best
                 * described as 开/关.
                 */
                const char *an = cn_option_value_name(
                    key, conf_get_bool(baseline, key));
                const char *bn = cn_option_value_name(
                    key, conf_get_bool(current, key));
                if (an && bn)
                    put_fmt(sb, "【%s】 %s → %s\r\n",
                            cn_option_display_name(key), an, bn);
                else
                    put_fmt(sb, "【%s】 %s → %s\r\n",
                            cn_option_display_name(key),
                            conf_get_bool(baseline, key) ? "开" : "关",
                            conf_get_bool(current, key) ? "开" : "关");
                nchanges++;
            }
            break;
          case CONF_TYPE_FILENAME: {
            const Filename *a = conf_get_filename(baseline, key);
            const Filename *b = conf_get_filename(current, key);
            if (strcmp(a->utf8path, b->utf8path) != 0) {
                put_fmt(sb, "【%s】 %s → %s\r\n",
                        cn_option_display_name(key),
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
                put_fmt(sb, "【%s】 %s %d → %s %d\r\n",
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
