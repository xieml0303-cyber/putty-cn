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
