/*
 * config.c - the platform-independent parts of the PuTTY
 * configuration box.
 */

#include <assert.h>
#include <stdlib.h>

#include "putty.h"
#include "dialog.h"
#include "storage.h"
#include "tree234.h"

#define PRINTER_DISABLED_STRING "无（禁止打印）"

#define HOST_BOX_TITLE "主机名称（或 IP 地址）(N)"
#define PORT_BOX_TITLE "端口(P)"

/*
 * cn: 会话页的按钮可用状态跟着主机名 / 端口 / 协议走，而这三个
 * 设置在主机名框、端口框、协议单选里都可能被改。这三处 handler
 * 定义在本文件靠前，按钮那套逻辑在靠后，先声明一下。
 */
static void cn_session_buttons_changed(dlgparam *dlg, Conf *conf);

void conf_radiobutton_handler(dlgcontrol *ctrl, dlgparam *dlg,
                              void *data, int event)
{
    int button;
    Conf *conf = (Conf *)data;

    /*
     * For a standard radio button set, the context parameter gives
     * the primary key (CONF_foo), and the extra data per button
     * gives the value the target field should take if that button
     * is the one selected.
     */
    if (event == EVENT_REFRESH) {
        int val = conf_get_int(conf, ctrl->context.i);
        for (button = 0; button < ctrl->radio.nbuttons; button++)
            if (val == ctrl->radio.buttondata[button].i)
                break;
        /* We expected that `break' to happen, in all circumstances. */
        assert(button < ctrl->radio.nbuttons);
        dlg_radiobutton_set(ctrl, dlg, button);
    } else if (event == EVENT_VALCHANGE) {
        button = dlg_radiobutton_get(ctrl, dlg);
        assert(button >= 0 && button < ctrl->radio.nbuttons);
        conf_set_int(conf, ctrl->context.i,
                     ctrl->radio.buttondata[button].i);
    }
}

void conf_radiobutton_bool_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    int button;
    Conf *conf = (Conf *)data;

    /*
     * Same as conf_radiobutton_handler, but using conf_set_bool in
     * place of conf_set_int, because it's dealing with a bool-typed
     * config option.
     */
    if (event == EVENT_REFRESH) {
        int val = conf_get_bool(conf, ctrl->context.i);
        for (button = 0; button < ctrl->radio.nbuttons; button++)
            if (val == ctrl->radio.buttondata[button].i)
                break;
        /* We expected that `break' to happen, in all circumstances. */
        assert(button < ctrl->radio.nbuttons);
        dlg_radiobutton_set(ctrl, dlg, button);
    } else if (event == EVENT_VALCHANGE) {
        button = dlg_radiobutton_get(ctrl, dlg);
        assert(button >= 0 && button < ctrl->radio.nbuttons);
        conf_set_bool(conf, ctrl->context.i,
                      ctrl->radio.buttondata[button].i);
    }
}

#define CHECKBOX_INVERT (1<<30)
void conf_checkbox_handler(dlgcontrol *ctrl, dlgparam *dlg,
                           void *data, int event)
{
    int key;
    bool invert;
    Conf *conf = (Conf *)data;

    /*
     * For a standard checkbox, the context parameter gives the
     * primary key (CONF_foo), optionally ORed with CHECKBOX_INVERT.
     */
    key = ctrl->context.i;
    if (key & CHECKBOX_INVERT) {
        key &= ~CHECKBOX_INVERT;
        invert = true;
    } else
        invert = false;

    /*
     * C lacks a logical XOR, so the following code uses the idiom
     * (!a ^ !b) to obtain the logical XOR of a and b. (That is, 1
     * iff exactly one of a and b is nonzero, otherwise 0.)
     */

    if (event == EVENT_REFRESH) {
        bool val = conf_get_bool(conf, key);
        dlg_checkbox_set(ctrl, dlg, (!val ^ !invert));
    } else if (event == EVENT_VALCHANGE) {
        conf_set_bool(conf, key, !dlg_checkbox_get(ctrl,dlg) ^ !invert);
    }
}

const struct conf_editbox_handler_type conf_editbox_str = {.type = EDIT_STR};
const struct conf_editbox_handler_type conf_editbox_int = {.type = EDIT_INT};

void conf_editbox_handler(dlgcontrol *ctrl, dlgparam *dlg,
                          void *data, int event)
{
    /*
     * The standard edit-box handler expects the main `context' field
     * to contain the primary key. The secondary `context2' field is a
     * pointer to the struct conf_editbox_handler_type defined in
     * putty.h.
     */
    int key = ctrl->context.i;
    const struct conf_editbox_handler_type *type = ctrl->context2.cp;
    Conf *conf = (Conf *)data;

    if (type->type == EDIT_STR) {
        if (event == EVENT_REFRESH) {
            bool utf8;
            char *field = conf_get_str_ambi(conf, key, &utf8);
            if (utf8)
                dlg_editbox_set_utf8(ctrl, dlg, field);
            else
                dlg_editbox_set(ctrl, dlg, field);
        } else if (event == EVENT_VALCHANGE) {
            char *field = dlg_editbox_get_utf8(ctrl, dlg);
            if (!conf_try_set_utf8(conf, key, field)) {
                sfree(field);
                field = dlg_editbox_get(ctrl, dlg);
                conf_set_str(conf, key, field);
            }
            sfree(field);
        }
    } else {
        if (event == EVENT_REFRESH) {
            char str[80];
            int value = conf_get_int(conf, key);
            if (type->type == EDIT_INT)
                sprintf(str, "%d", value);
            else
                sprintf(str, "%g", (double)value / type->denominator);
            dlg_editbox_set(ctrl, dlg, str);
        } else if (event == EVENT_VALCHANGE) {
            char *str = dlg_editbox_get(ctrl, dlg);
            if (type->type == EDIT_INT)
                conf_set_int(conf, key, atoi(str));
            else
                conf_set_int(conf, key, (int)(type->denominator * atof(str)));
            sfree(str);
        }
    }
}

void conf_filesel_handler(dlgcontrol *ctrl, dlgparam *dlg,
                          void *data, int event)
{
    int key = ctrl->context.i;
    Conf *conf = (Conf *)data;

    if (event == EVENT_REFRESH) {
        dlg_filesel_set(
            ctrl, dlg, conf_get_filename(conf, key));
    } else if (event == EVENT_VALCHANGE) {
        Filename *filename = dlg_filesel_get(ctrl, dlg);
        if (key == CONF_keyfile) {
            /*
             * 私钥文件框特判：选中外部格式私钥时提示自动转换为
             * 同目录同名 PPK 并填入（见 windows/keyimport.c）。
             */
            Filename *ppk = NULL;
            if (keyimport_auto_convert(dlg_get_platform_window(dlg),
                                       filename, &ppk)) {
                conf_set_filename(conf, key, ppk);
                dlg_filesel_set(ctrl, dlg, ppk);
                filename_free(ppk);
                filename_free(filename);
                return;
            }
        }
        conf_set_filename(conf, key, filename);
        filename_free(filename);
    }
}

/*
 * 「导入并转换密钥」按钮：调起进程内的 puttygen 简化流程
 * （选外部私钥 → 同目录转出 PPK），转换成功后把生成的 PPK
 * 路径回填到「认证私钥文件」框。按钮的 context2 挂着同组
 * 私钥文件框控件，见证书页构建处。
 */
static void keyimport_button_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                     void *data, int event)
{
    if (event == EVENT_ACTION) {
        void *hwnd = dlg_get_platform_window(dlg);
        Filename *ppk = NULL;
        if (!keyimport_run(hwnd, &ppk))
            return;                      /* 取消或失败，保持原值 */

        Conf *conf = (Conf *)data;
        conf_set_filename(conf, CONF_keyfile, ppk);
        /* 让私钥框按新的 conf 值刷新显示（EVENT_REFRESH 路径） */
        dlgcontrol *keyfilesel = ctrl->context2.p;
        if (keyfilesel)
            dlg_refresh(keyfilesel, dlg);
        dlg_update_apply_buttons(dlg);
        filename_free(ppk);
    }
}

void conf_fontsel_handler(dlgcontrol *ctrl, dlgparam *dlg,
                          void *data, int event)
{
    int key = ctrl->context.i;
    Conf *conf = (Conf *)data;

    if (event == EVENT_REFRESH) {
        dlg_fontsel_set(
            ctrl, dlg, conf_get_fontspec(conf, key));
    } else if (event == EVENT_VALCHANGE) {
        FontSpec *fontspec = dlg_fontsel_get(ctrl, dlg);
        conf_set_fontspec(conf, key, fontspec);
        fontspec_free(fontspec);
    }
}

/*
 * Fill a serial-port combo box with the ports this machine actually
 * has. If the configured port is empty or absent from this machine,
 * fall back to the first one that is present, so a stale setting (a
 * COM number left over from another machine, say) doesn't leave the
 * box showing a port that can't be opened.
 */
/*
 * The port list shows each entry with a （物理）/（蓝牙） suffix. The
 * stored setting must remain a bare port name ("COM4"), so strip the
 * suffix whenever a list entry's text becomes a configuration value.
 */
static void strip_port_suffix(char *s)
{
    char *p = strstr(s, "（");
    if (p)
        *p = '\0';
}

/*
 * cn: 主机名框的语义是「主机」，不含 URL scheme。用户从浏览器地址栏
 * 复制内容时常常把 "https://" 一并粘进来，而 SSH 层会把唯一的冒号
 * 当作 host:port 分隔符，于是 "https://example.com" 被截成主机名
 * "https"，最后报出「主机不存在」这种毫无指向性的错误。这里在点击
 * 打开时就识别出协议前缀，直接给出明确提示，避免用户去怀疑 DNS。
 *
 * 只认 URI scheme 的形态（字母开头、后接字母数字/+/-/. 再接 "://"），
 * 因此 "example.com:2222" 这类合法的 host:port 写法不会被误判 —— 它
 * 冒号前没有 "//"。返回识别到的协议名（静态字符串），否则返回 NULL。
 */
static const char *hostbox_scheme_prefix(const char *host)
{
    const char *p = host;

    if (!p || !*p)
        return NULL;

    /* URI scheme 必须以字母开头。 */
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')))
        return NULL;

    p++;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
           (*p >= '0' && *p <= '9') ||
           *p == '+' || *p == '-' || *p == '.')
        p++;

    /* 后面必须紧跟 "://"，否则不是 scheme 前缀。 */
    if (p[0] != ':' || p[1] != '/' || p[2] != '/')
        return NULL;

    /*
     * 返回 scheme 原文供提示语引用。scheme 长度上限为 32 个字符，
     * 用静态缓冲区保存即可；配置框是模态且单实例的，不存在重入。
     */
    {
        static char scheme[33];
        size_t n = (size_t)(p - host);
        if (n > sizeof(scheme) - 1)
            n = sizeof(scheme) - 1;
        memcpy(scheme, host, n);
        scheme[n] = '\0';
        return scheme;
    }
}

/*
 * 供 putty.c 的命令行启动路径复用：主机名带协议前缀时返回一条提示
 * 文案（malloc 所得，调用方负责 sfree），否则返回 NULL。
 */
char *cn_hostbox_scheme_complaint(Conf *conf)
{
    const char *scheme;

    if (conf_get_int(conf, CONF_protocol) == PROT_SERIAL)
        return NULL;

    scheme = hostbox_scheme_prefix(conf_get_str(conf, CONF_host));
    if (!scheme)
        return NULL;

    return dupprintf("主机名称格式错误，不能携带 https、http 等协议前缀。"
                     "\n\n"
                     "请去掉前缀后重新打开，例："
                     "\n\n"
                     "%s://example.com  改为  example.com",
                     scheme);
}

/*
 * Serial-port auto-fallback bookkeeping (H3). When
 * fill_serial_port_list() rewrites CONF_serline because the saved
 * port is absent, or because a physical lead displaced a Bluetooth
 * virtual one, we remember the value the user actually had saved and
 * whether the user has since picked a port by hand. Saving must keep
 * the user's value; only an explicit user choice may be persisted.
 *
 * The startup config dialog is modal and one per process, so these
 * file-scope statics live exactly as long as one config dialog use
 * (same lifetime argument as the M1/M2 serial_ui analysis); they are
 * reset at every dialog entry via serial_fallback_reset().
 */
static char *serial_fallback_original = NULL;
static bool serial_fallback_active = false;
static bool serial_user_picked = false;

void serial_fallback_reset(void)
{
    sfree(serial_fallback_original);
    serial_fallback_original = NULL;
    serial_fallback_active = false;
    serial_user_picked = false;
}

static void fill_serial_port_list(dlgcontrol *ctrl, dlgparam *dlg,
                                  Conf *conf, const char *current)
{
    char *saved = current ? dupstr(current) : NULL;
    if (saved)
        strip_port_suffix(saved);
    char **ports;
    int nports, i, selected = -1;
    bool present = false;

    dlg_update_start(ctrl, dlg);
    dlg_listbox_clear(ctrl, dlg);
    ports = get_serial_ports(&nports);
    for (i = 0; i < nports; i++) {
        char item[64];
        snprintf(item, sizeof(item), "%s（%s）", ports[i],
                 serial_port_is_bluetooth(ports[i]) ? "蓝牙" : "物理");
        dlg_listbox_add(ctrl, dlg, item);
        if (saved && !strcmp(saved, ports[i])) {
            present = true;
            selected = i;
        }
    }
    if (!present && nports > 0) {
        if (!serial_fallback_active) {
            /*
             * First auto-fallback in this dialog's lifetime: remember
             * the user's saved value so that saving keeps it instead
             * of baking in our substitution (H3).
             */
            serial_fallback_original = saved ? dupstr(saved) : dupstr("");
            serial_fallback_active = true;
        }
        sfree(saved);
        saved = dupstr(ports[0]);
        conf_set_str(conf, CONF_serline, saved);
        selected = 0;
    } else if (present && nports > 0 &&
               serial_port_is_bluetooth(saved)) {
        /*
         * The saved port is a Bluetooth virtual one. If a real serial
         * port is present (a USB console lead has been plugged in
         * since the setting was last used), prefer that instead of
         * staying on the Bluetooth entry.
         */
        for (i = 0; i < nports; i++) {
            if (!serial_port_is_bluetooth(ports[i])) {
                if (!serial_fallback_active) {
                    serial_fallback_original =
                        saved ? dupstr(saved) : dupstr("");
                    serial_fallback_active = true;
                }
                sfree(saved);
                saved = dupstr(ports[i]);
                conf_set_str(conf, CONF_serline, saved);
                selected = i;
                break;
            }
        }
    }
    free_serial_ports(ports, nports);
    /*
     * Highlight the port we're actually using, so that opening the
     * drop-down shows which entry is selected. This also puts the
     * text in the typing area, which is why it comes first.
     */
    if (selected >= 0)
        dlg_listbox_select(ctrl, dlg, selected);
    if (saved && saved[0]) {
        char display[96];
        if (serial_fallback_active && !serial_user_picked)
            snprintf(display, sizeof(display), "%s（%s，自动匹配）", saved,
                     serial_port_is_bluetooth(saved) ? "蓝牙" : "物理");
        else
            snprintf(display, sizeof(display), "%s（%s）", saved,
                     serial_port_is_bluetooth(saved) ? "蓝牙" : "物理");
        dlg_editbox_set(ctrl, dlg, display);
    } else {
        dlg_editbox_set(ctrl, dlg, "");
    }
    dlg_update_done(ctrl, dlg);
    sfree(saved);
}

static void cn_refresh_userbox(dlgparam *dlg);

static void config_host_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                void *data, int event)
{
    Conf *conf = (Conf *)data;

    /*
     * This function works just like the standard edit box handler,
     * only it has to choose the control's label and text from two
     * different places depending on the protocol.
     */
    if (event == EVENT_REFRESH) {
        bool serial = (conf_get_int(conf, CONF_protocol) == PROT_SERIAL);

        /*
         * In serial mode this control is a port picker, not a text
         * field, so forbid typing into it; the drop-down list is the
         * only way to choose a port. The same control doubles as the
         * host name box for the network protocols, where typing must
         * of course still work.
         */
        dlg_editbox_set_readonly(ctrl, dlg, serial);

        if (serial) {
            /*
             * This label text is carefully chosen to contain an n,
             * since that's the shortcut for the host name control.
             */
            dlg_label_change(ctrl, dlg, "串口(N)");
            fill_serial_port_list(ctrl, dlg, conf,
                                  conf_get_str(conf, CONF_serline));
        } else {
            /*
             * cn: 网络模式下不再灌历史条目，且必须清空列表——串口
             * 模式灌进来的端口条目若不清，切回 SSH 后列表非空，
             * 盖绘放行（箭头重现）且下拉弹出的还是 COM 残留。
             * 控件本体是 ctrl_combobox（has_list=true），列表操作
             * 合法；断言只在控件被换成无列表 editbox 时才会炸。
             */
            /*
             * cn: 标签必须同步恢复——串口分支把它改成了「串口(N)」，
             * 漏掉这行的话 SSH/Telnet 切回来标签就停在「串口」。
             */
            dlg_label_change(ctrl, dlg, HOST_BOX_TITLE);
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            dlg_editbox_set(ctrl, dlg, conf_get_str(conf, CONF_host));
            dlg_update_done(ctrl, dlg);
        }
    } else if (event == EVENT_VALCHANGE) {
        char *s = dlg_editbox_get(ctrl, dlg);
        if (conf_get_int(conf, CONF_protocol) == PROT_SERIAL) {
            /*
             * Picking an entry from the drop-down puts its display text
             * ("COM4（蓝牙）") into the edit box. Store the bare port
             * name; the box keeps showing the annotated form.
             */
            strip_port_suffix(s);
            conf_set_str(conf, CONF_serline, s);
            serial_user_picked = true;
            serial_fallback_active = false; /* the user's choice wins */
        } else {
            conf_set_str(conf, CONF_host, s);
            /*
             * cn: 用户习惯把用户名写在主机名里（root@1.2.3.4）。那样
             * SSH 层得自己拆，主机名框看着也别扭。这里当场把 @ 前面那
             * 段提到「用户名」框，主机名只留地址本身 —— 四项内容各归
             * 各位，后面「填齐四项就直连」的判断才有一致的依据。
             */
            char *at = strrchr(s, '@');
            if (at && at != s && at[1]) {
                char *user = dupstr(s);
                user[at - s] = '\0';
                conf_set_str(conf, CONF_username, user);
                sfree(user);
                char *hostonly = dupstr(at + 1);
                conf_set_str(conf, CONF_host, hostonly);
                dlg_editbox_set(ctrl, dlg, hostonly);
                sfree(hostonly);
                /*
                 * 用户名框是另一个控件，得让它重新读一次值。只刷这
                 * 一个控件 —— dlg_refresh(NULL, dlg) 会把全对话框的
                 * 控制都刷一遍，连带触发历史列表重建，没必要。
                 */
                cn_refresh_userbox(dlg);
            }
        }
        sfree(s);
        /* cn: 主机名变了，会话页按钮的可用状态要跟着重算。 */
        cn_session_buttons_changed(dlg, conf);
    }
}

static void config_port_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                void *data, int event)
{
    Conf *conf = (Conf *)data;
    char buf[80];

    /*
     * This function works similarly to the standard edit box handler,
     * only it has to choose the control's label and text from two
     * different places depending on the protocol.
     */
    if (event == EVENT_REFRESH) {
        if (conf_get_int(conf, CONF_protocol) == PROT_SERIAL) {
            /*
             * This label text is carefully chosen to contain a p,
             * since that's the shortcut for the port control.
             */
            dlg_label_change(ctrl, dlg, "波特率(P)");
            sprintf(buf, "%d", conf_get_int(conf, CONF_serspeed));
        } else {
            dlg_label_change(ctrl, dlg, PORT_BOX_TITLE);
            if (conf_get_int(conf, CONF_port) != 0)
                sprintf(buf, "%d", conf_get_int(conf, CONF_port));
            else
                /* Display an (invalid) port of 0 as blank */
                buf[0] = '\0';
        }
        dlg_editbox_set(ctrl, dlg, buf);
    } else if (event == EVENT_VALCHANGE) {
        char *s = dlg_editbox_get(ctrl, dlg);
        int i = atoi(s);
        sfree(s);

        if (conf_get_int(conf, CONF_protocol) == PROT_SERIAL)
            conf_set_int(conf, CONF_serspeed, i);
        else
            conf_set_int(conf, CONF_port, i);

        /* cn: 端口变了，会话页按钮的可用状态要跟着重算。 */
        cn_session_buttons_changed(dlg, conf);
    }
}

struct sessionsaver_data;       /* 定义见下方的会话页按钮集合 */

/*
 * cn: 「用户名」输入框。
 *
 * 直接读写 CONF_username —— 它就是 SSH 会拿去当登录名的那一项，所以
 * 在会话页填了用户名，SSH 握手时就不会再问 "login as:"。用 ambi 版本
 * 读写保持与 conf_editbox_handler 的 STR_AMBI 语义一致（UTF-8 与本地
 * 编码都认），避免在这个控件上踩 conf_get_str 的断言。
 */
static void config_user_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                void *data, int event)
{
    Conf *conf = (Conf *)data;

    if (event == EVENT_REFRESH) {
        bool utf8;
        char *field = conf_get_str_ambi(conf, CONF_username, &utf8);
        if (utf8)
            dlg_editbox_set_utf8(ctrl, dlg, field);
        else
            dlg_editbox_set(ctrl, dlg, field);
    } else if (event == EVENT_VALCHANGE) {
        char *field = dlg_editbox_get_utf8(ctrl, dlg);
        if (!conf_try_set_utf8(conf, CONF_username, field)) {
            sfree(field);
            field = dlg_editbox_get(ctrl, dlg);
            conf_set_str(conf, CONF_username, field);
        }
        sfree(field);
        /* cn: 用户名填了，按钮的可用状态要重算。 */
        cn_session_buttons_changed(dlg, conf);
    }
}

/*
 * cn: 「凭据（密码）」输入框。
 *
 * 密码不进 Conf —— 主人明确选了「不落盘、只在本次连接生效」。它被
 * 暂存在 cmdline.c 的自动应答槽位里（cmdline_set_password），SSH 层
 * 遇到密码提示时会直接取用，不再弹输入框。
 *
 * 控件本身是掩码输入框（dialog.h 的 editbox.password），值用明文取回
 * 来即可，显示由系统负责。
 */
/*
 * cn: 「凭据（密码）」输入框正在被程序回填的标记。
 *
 * 回填后系统会发 EN_CHANGE，handler 会以 EVENT_VALCHANGE 收到一次，
 * 此时框里是「我们要它显示的值」而不是用户输入。若不拦住，回填空串
 * 就会把刚存进 cmdline 的密码当成「用户清空了框」抹掉。
 */
static bool cn_passbox_refilling;

static void config_pass_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                void *data, int event)
{
    if (event == EVENT_REFRESH) {
        /*
         * 密码不持久化，刷新时把框清空 —— 打开配置框、切换面板回来，
         * 都不该把上一次的口令重新显示出来。
         */
        cn_passbox_refilling = true;
        dlg_editbox_set(ctrl, dlg, "");
        cn_passbox_refilling = false;
    } else if (event == EVENT_VALCHANGE) {
        if (cn_passbox_refilling)
            return;             /* 程序回填引起的，不是用户编辑 */
        char *s = dlg_editbox_get(ctrl, dlg);
        cmdline_set_password(s);
        smemclr(s, strlen(s));
        sfree(s);
        cn_session_buttons_changed(dlg, (Conf *)data);
    }
}

struct hostport {
    dlgcontrol *host, *port, *protradio;
    bool mid_refresh;
    /*
     * cn: 反向指到会话页的按钮集合，让协议切换能即时更新「载入
     * 密钥」的显隐与「连接」的可用状态。sessionsaver_data 先于
     * 本结构建立，指针始终有效。
     */
    struct sessionsaver_data *ssd;
};

/*
 * cn: 会话页两个按钮的外观依赖主机名 / 端口 / 协议，而这三个
 * 设置在任意输入框变化时都可能变。hostport 与 sessionsaver_data
 * 都是 config box 打开时一次性分配的，用文件级指针把「当前会话页」
 * 记下来，供这些 handler 在变化后触发一次按钮状态重算。
 * 配置框是模态且进程内单实例，不需要考虑并发。
 */
static struct sessionsaver_data *cn_session_ssd;

static bool session_page_inputs_filled(Conf *conf);
static void set_button_muted(dlgparam *dlg, dlgcontrol *ctrl, bool muted);
static void refresh_session_buttons(struct sessionsaver_data *ssd,
                                    dlgparam *dlg, Conf *conf);

/*
 * cn: 只刷新「用户名」框。
 *
 * 主机名框里拆出 user@host 之后，用户名框需要重新读一次 Conf。用
 * dlg_refresh(NULL, dlg) 会把全对话框刷一遍（含历史列表重建），代价
 * 与收益不成比例，所以单独交给这个控件自己的 handler 走一次
 * EVENT_REFRESH。
 *
 * 实现放在 sessionsaver_data 完整定义之后，这里先留声明。
 */

static void cn_session_buttons_changed(dlgparam *dlg, Conf *conf)
{
    if (cn_session_ssd)
        refresh_session_buttons(cn_session_ssd, dlg, conf);
}

/*
 * cn: 「载入密钥」跳转到证书页后，希望光标直接落在私钥文件框里。
 *
 * 切页会销毁并重建面板控件，跳转那一刻目标控件还不存在，所以只能
 * 先记个意图，等证书页的私钥框收到 EVENT_REFRESH（即它刚被创建、
 * 值也填好）时再把焦点交过去。
 */
static bool cn_focus_keyfile_on_refresh;

/*
 * cn: 用户在配置框里点了「文件传输」，希望关掉配置框后直接开传输窗口。
 *
 * 配置框的返回值只有「连接 / 取消」两态（do_config 返回 bool），多塞
 * 一个返回值要动一路平台接口。这里用一个标志把意图带出去：点按钮时
 * 置位并结束对话框，调用方在 do_config 之后检查它。
 */
static bool cn_session_wants_xfer;

bool cn_take_xfer_request(void)
{
    bool ret = cn_session_wants_xfer;
    cn_session_wants_xfer = false;
    return ret;
}

/*
 * 私钥文件框专用 handler：平时完全等同 conf_filesel_handler，只在
 * 被「载入密钥」点名时额外抢一次焦点。
 */
static void cn_keyfile_handler(dlgcontrol *ctrl, dlgparam *dlg,
                               void *data, int event)
{
    conf_filesel_handler(ctrl, dlg, data, event);

    /*
     * 私钥是「文件传输」门控的凭据之一，但它在证书页改，会话页的
     * 按钮听不到变化 —— 不补这次刷新，选完密钥回来按钮还画成灰的
     * （判定早改对了，纯显示问题）。REFRESH 也刷，覆盖自动转换
     * 回填的路径。
     */
    if (event == EVENT_VALCHANGE || event == EVENT_REFRESH)
        cn_session_buttons_changed(dlg, (Conf *)data);

    if (event == EVENT_REFRESH && cn_focus_keyfile_on_refresh) {
        cn_focus_keyfile_on_refresh = false;
        dlg_set_focus(ctrl, dlg);
    }
}

/*
 * The "Other:" list holds the protocols that did not earn a radio
 * button. It only governs the connection when "Other:" is the chosen
 * type, so that is when it is drawn at full strength; otherwise it is
 * muted, to match the radio button beside it.
 *
 * Whether the current protocol is one of the radio buttons is asked of
 * the button group itself rather than hardcoded, since which protocols
 * get a button depends on which backends were compiled in.
 */
/*
 * Shared handler for protocol radio-button and drop-list controls.
 * Handles the interaction of those two controls, and also changes
 * the setting of the port box to match the protocol if necessary,
 * and refreshes both host and port boxes when switching to/from the
 * serial backend.
 */
static void config_protocols_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                     void *data, int event)
{
    Conf *conf = (Conf *)data;
    int curproto = conf_get_int(conf, CONF_protocol);
    struct hostport *hp = (struct hostport *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        /*
         * Refresh the states of the controls from Conf.
         *
         * When refreshing these controls, we have to watch out for
         * re-entrancy: because there are two controls involved, the
         * refresh is not atomic, so the VALCHANGE and/or SELCHANGE
         * callbacks resulting from our updates here might cause other
         * settings here to change unwantedly. (E.g. setting the list
         * selection shouldn't trigger the SELCHANGE side effect of
         * selecting the Other radio button; setting the radio button
         * to Other here shouldn't have the side effect of selecting
         * whatever protocol is _currently_ selected in the list box,
         * if we haven't selected the right one yet.)
         */
        hp->mid_refresh = true;

        if (ctrl == hp->protradio) {
            /* Available buttons were set up when control was created.
             * Just select one of them, possibly. */
            int hit = -1;
            for (int button = 0; button < ctrl->radio.nbuttons; button++)
                if (curproto == ctrl->radio.buttondata[button].i) {
                    hit = button;
                    break;
                }
            if (hit < 0) {
                /*
                 * The saved protocol no longer exists (e.g. sessions
                 * saved when Rlogin or SUPDUP were still offered).
                 * Fall back to SSH, and make the Conf agree so the
                 * port box follows.
                 */
                hit = 0;
                conf_set_int(conf, CONF_protocol, PROT_SSH);
            }
            dlg_radiobutton_set(ctrl, dlg, hit);
        }

        hp->mid_refresh = false;
    } else if (!hp->mid_refresh) {
        /*
         * Potentially update Conf from the states of the controls.
         */
        int newproto = curproto;

        if (event == EVENT_VALCHANGE && ctrl == hp->protradio) {
            int button = dlg_radiobutton_get(ctrl, dlg);
            assert(button >= 0 && button < ctrl->radio.nbuttons);
            newproto = ctrl->radio.buttondata[button].i;
        }

        if (newproto != curproto) {
            conf_set_int(conf, CONF_protocol, newproto);

            const struct BackendVtable *cvt = backend_vt_from_proto(curproto);
            const struct BackendVtable *nvt = backend_vt_from_proto(newproto);
            assert(cvt);
            assert(nvt);
            /*
             * Iff the user hasn't changed the port from the old
             * protocol's default, update it with the new protocol's
             * default.
             *
             * (This includes a "default" of 0, implying that there is
             * no sensible default for that protocol; in this case
             * it's displayed as a blank.)
             *
             * This helps with the common case of tabbing through the
             * controls in order and setting a non-default port before
             * getting to the protocol; we want that non-default port
             * to be preserved.
             */
            int port = conf_get_int(conf, CONF_port);
            if (port == cvt->default_port)
                conf_set_int(conf, CONF_port, nvt->default_port);

            dlg_refresh(hp->host, dlg);
            dlg_refresh(hp->port, dlg);

            /* cn: 协议换了，「载入密钥」的显隐与「连接」的可用状态都要重算。 */
            refresh_session_buttons(hp->ssd, dlg, conf);
        }
    }
}

static void loggingbuttons_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    int button;
    Conf *conf = (Conf *)data;
    /* This function works just like the standard radio-button handler,
     * but it has to fall back to "不记录日志" in situations where the
     * configured logging type isn't applicable.
     */
    if (event == EVENT_REFRESH) {
        int logtype = conf_get_int(conf, CONF_logtype);

        for (button = 0; button < ctrl->radio.nbuttons; button++)
            if (logtype == ctrl->radio.buttondata[button].i)
                break;

        /* We fell off the end, so we lack the configured logging type */
        if (button == ctrl->radio.nbuttons) {
            button = 0;
            conf_set_int(conf, CONF_logtype, LGTYP_NONE);
        }
        dlg_radiobutton_set(ctrl, dlg, button);
    } else if (event == EVENT_VALCHANGE) {
        button = dlg_radiobutton_get(ctrl, dlg);
        assert(button >= 0 && button < ctrl->radio.nbuttons);
        conf_set_int(conf, CONF_logtype, ctrl->radio.buttondata[button].i);
    }
}

static void numeric_keypad_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    int button;
    Conf *conf = (Conf *)data;
    /*
     * This function works much like the standard radio button
     * handler, but it has to handle two fields in Conf.
     */
    if (event == EVENT_REFRESH) {
        if (conf_get_bool(conf, CONF_nethack_keypad))
            button = 2;
        else if (conf_get_bool(conf, CONF_app_keypad))
            button = 1;
        else
            button = 0;
        assert(button < ctrl->radio.nbuttons);
        dlg_radiobutton_set(ctrl, dlg, button);
    } else if (event == EVENT_VALCHANGE) {
        button = dlg_radiobutton_get(ctrl, dlg);
        assert(button >= 0 && button < ctrl->radio.nbuttons);
        if (button == 2) {
            conf_set_bool(conf, CONF_app_keypad, false);
            conf_set_bool(conf, CONF_nethack_keypad, true);
        } else {
            conf_set_bool(conf, CONF_app_keypad, (button != 0));
            conf_set_bool(conf, CONF_nethack_keypad, false);
        }
    }
}

static void cipherlist_handler(dlgcontrol *ctrl, dlgparam *dlg,
                               void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int i;

        static const struct { const char *s; int c; } ciphers[] = {
            { "ChaCha20 (仅限 SSH-2)",  CIPHER_CHACHA20 },
            { "AES-GCM (仅限 SSH-2)",   CIPHER_AESGCM },
            { "3DES",                   CIPHER_3DES },
            { "Blowfish",               CIPHER_BLOWFISH },
            { "DES",                    CIPHER_DES },
            { "AES (仅限 SSH-2)",       CIPHER_AES },
            { "Arcfour (仅限 SSH-2)",   CIPHER_ARCFOUR },
            { "-- 下面为警告选项 --",  CIPHER_WARN }
        };

        /* Set up the "已选加密算法" box. */
        /* (cipherlist assumed to contain all ciphers) */
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < CIPHER_MAX; i++) {
            int c = conf_get_int_int(conf, CONF_ssh_cipherlist, i);
            int j;
            const char *cstr = NULL;
            for (j = 0; j < (sizeof ciphers) / (sizeof ciphers[0]); j++) {
                if (ciphers[j].c == c) {
                    cstr = ciphers[j].s;
                    break;
                }
            }
            dlg_listbox_addwithid(ctrl, dlg, cstr, c);
        }
        dlg_update_done(ctrl, dlg);

    } else if (event == EVENT_VALCHANGE) {
        int i;

        /* Update array to match the list box. */
        for (i=0; i < CIPHER_MAX; i++)
            conf_set_int_int(conf, CONF_ssh_cipherlist, i,
                             dlg_listbox_getid(ctrl, dlg, i));
    }
}

#ifndef NO_GSSAPI
static void gsslist_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int i;

        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < ngsslibs; i++) {
            int id = conf_get_int_int(conf, CONF_ssh_gsslist, i);
            assert(id >= 0 && id < ngsslibs);
            dlg_listbox_addwithid(ctrl, dlg, gsslibnames[id], id);
        }
        dlg_update_done(ctrl, dlg);

    } else if (event == EVENT_VALCHANGE) {
        int i;

        /* Update array to match the list box. */
        for (i=0; i < ngsslibs; i++)
            conf_set_int_int(conf, CONF_ssh_gsslist, i,
                             dlg_listbox_getid(ctrl, dlg, i));
    }
}
#endif

static void kexlist_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int i;

        static const struct { const char *s; int k; } kexes[] = {
            { "Diffie-Hellman group 1 (1024-bit)",  KEX_DHGROUP1 },
            { "Diffie-Hellman group 14 (2048-bit)", KEX_DHGROUP14 },
            { "Diffie-Hellman group 15 (3072-bit)", KEX_DHGROUP15 },
            { "Diffie-Hellman group 16 (4096-bit)", KEX_DHGROUP16 },
            { "Diffie-Hellman group 17 (6144-bit)", KEX_DHGROUP17 },
            { "Diffie-Hellman group 18 (8192-bit)", KEX_DHGROUP18 },
            { "Diffie-Hellman 组交换",      KEX_DHGEX },
            { "基于 RSA 的密钥交换",             KEX_RSA },
            { "ECDH 密钥交换",                  KEX_ECDH },
            { "NTRU Prime / Curve25519 hybrid kex", KEX_NTRU_HYBRID },
            { "ML-KEM / Curve25519 hybrid kex",     KEX_MLKEM_25519_HYBRID },
            { "ML-KEM / NIST ECDH hybrid kex",      KEX_MLKEM_NIST_HYBRID },
            { "-- 下面为警告选项 --",              KEX_WARN }
        };

        /* Set up the "密钥交换算法优先级" box. */
        /* (kexlist assumed to contain all algorithms) */
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < KEX_MAX; i++) {
            int k = conf_get_int_int(conf, CONF_ssh_kexlist, i);
            int j;
            const char *kstr = NULL;
            for (j = 0; j < (sizeof kexes) / (sizeof kexes[0]); j++) {
                if (kexes[j].k == k) {
                    kstr = kexes[j].s;
                    break;
                }
            }
            dlg_listbox_addwithid(ctrl, dlg, kstr, k);
        }
        dlg_update_done(ctrl, dlg);

    } else if (event == EVENT_VALCHANGE) {
        int i;

        /* Update array to match the list box. */
        for (i=0; i < KEX_MAX; i++)
            conf_set_int_int(conf, CONF_ssh_kexlist, i,
                             dlg_listbox_getid(ctrl, dlg, i));
    }
}

static void hklist_handler(dlgcontrol *ctrl, dlgparam *dlg,
                           void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int i;

        static const struct { const char *s; int k; } hks[] = {
            { "Ed25519",               HK_ED25519 },
            { "Ed448",                 HK_ED448 },
            { "ECDSA",                 HK_ECDSA },
            { "DSA",                   HK_DSA },
            { "RSA",                   HK_RSA },
            { "-- 下面为警告选项 --", HK_WARN }
        };

        /* Set up the "主机密钥算法优先级" box. */
        /* (hklist assumed to contain all algorithms) */
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < HK_MAX; i++) {
            int k = conf_get_int_int(conf, CONF_ssh_hklist, i);
            int j;
            const char *kstr = NULL;
            for (j = 0; j < lenof(hks); j++) {
                if (hks[j].k == k) {
                    kstr = hks[j].s;
                    break;
                }
            }
            dlg_listbox_addwithid(ctrl, dlg, kstr, k);
        }
        dlg_update_done(ctrl, dlg);

    } else if (event == EVENT_VALCHANGE) {
        int i;

        /* Update array to match the list box. */
        for (i=0; i < HK_MAX; i++)
            conf_set_int_int(conf, CONF_ssh_hklist, i,
                             dlg_listbox_getid(ctrl, dlg, i));
    }
}

static void printerbox_handler(dlgcontrol *ctrl, dlgparam *dlg,
                               void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int nprinters, i;
        printer_enum *pe;
        const char *printer;

        dlg_update_start(ctrl, dlg);
        /*
         * Some backends may wish to disable the drop-down list on
         * this edit box. Be prepared for this.
         */
        if (ctrl->editbox.has_list) {
            dlg_listbox_clear(ctrl, dlg);
            dlg_listbox_add(ctrl, dlg, PRINTER_DISABLED_STRING);
            pe = printer_start_enum(&nprinters);
            for (i = 0; i < nprinters; i++)
                dlg_listbox_add(ctrl, dlg, printer_get_name(pe, i));
            printer_finish_enum(pe);
        }
        printer = conf_get_str(conf, CONF_printer);
        if (!printer)
            printer = PRINTER_DISABLED_STRING;
        dlg_editbox_set(ctrl, dlg, printer);
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_VALCHANGE) {
        char *printer = dlg_editbox_get(ctrl, dlg);
        if (!strcmp(printer, PRINTER_DISABLED_STRING))
            printer[0] = '\0';
        conf_set_str(conf, CONF_printer, printer);
        sfree(printer);
    }
}

static void codepage_handler(dlgcontrol *ctrl, dlgparam *dlg,
                             void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int i;
        const char *cp, *thiscp;
        dlg_update_start(ctrl, dlg);
        thiscp = cp_name(decode_codepage(conf_get_str(conf,
                                                      CONF_line_codepage)));
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; (cp = cp_enumerate(i)) != NULL; i++)
            dlg_listbox_add(ctrl, dlg, cp);
        dlg_editbox_set(ctrl, dlg, thiscp);
        conf_set_str(conf, CONF_line_codepage, thiscp);
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_VALCHANGE) {
        char *codepage = dlg_editbox_get(ctrl, dlg);
        conf_set_str(conf, CONF_line_codepage,
                     cp_name(decode_codepage(codepage)));
        sfree(codepage);
    }
}

static void sshbug_handler(dlgcontrol *ctrl, dlgparam *dlg,
                           void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        /*
         * We must fetch the previously configured value from the Conf
         * before we start modifying the drop-down list, otherwise the
         * spurious SELCHANGE we trigger in the process will overwrite
         * the value we wanted to keep.
         */
        int oldconf = conf_get_int(conf, ctrl->context.i);
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        dlg_listbox_addwithid(ctrl, dlg, "自动", AUTO);
        dlg_listbox_addwithid(ctrl, dlg, "关", FORCE_OFF);
        dlg_listbox_addwithid(ctrl, dlg, "开", FORCE_ON);
        switch (oldconf) {
          case AUTO:      dlg_listbox_select(ctrl, dlg, 0); break;
          case FORCE_OFF: dlg_listbox_select(ctrl, dlg, 1); break;
          case FORCE_ON:  dlg_listbox_select(ctrl, dlg, 2); break;
        }
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i < 0)
            i = AUTO;
        else
            i = dlg_listbox_getid(ctrl, dlg, i);
        conf_set_int(conf, ctrl->context.i, i);
    }
}

static void sshbug_handler_manual_only(dlgcontrol *ctrl, dlgparam *dlg,
                                       void *data, int event)
{
    /*
     * This is just like sshbug_handler, except that there's no 'Auto'
     * option. Used for bug workaround flags that can't be
     * autodetected, and have to be manually enabled if they're to be
     * used at all.
     */
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int oldconf = conf_get_int(conf, ctrl->context.i);
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        dlg_listbox_addwithid(ctrl, dlg, "关", FORCE_OFF);
        dlg_listbox_addwithid(ctrl, dlg, "开", FORCE_ON);
        switch (oldconf) {
          case FORCE_OFF: dlg_listbox_select(ctrl, dlg, 0); break;
          case FORCE_ON:  dlg_listbox_select(ctrl, dlg, 1); break;
        }
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i < 0)
            i = FORCE_OFF;
        else
            i = dlg_listbox_getid(ctrl, dlg, i);
        conf_set_int(conf, ctrl->context.i, i);
    }
}

struct sessionsaver_data {
    dlgcontrol *hostbox;        /* 上方「主机名称(或IP地址)」输入框 */
    dlgcontrol *portbox;        /* 旁边「端口」输入框，随历史条目一起回填 */
    dlgcontrol *userbox;        /* cn: 「用户名」输入框 */
    dlgcontrol *passbox;        /* cn: 「凭据（密码）」输入框 */
    dlgcontrol *listbox;
    dlgcontrol *savebutton, *delbutton;
    dlgcontrol *initbutton;     /* cn: 恢复到设置初始化快照 */
    dlgcontrol *loadkeybutton;  /* cn: 凭据框右侧的「载入密钥」 */
    dlgcontrol *xferbutton;     /* cn: 「连接」左侧的「文件传输」 */
    dlgcontrol *okbutton, *cancelbutton;
    /*
     * cn: 凭据框在 SSH 下的原始宽度、它的左边界，以及它最多能延伸到
     * 哪（主机组框的内右边界）。非 SSH 协议下「密钥」按钮隐藏，凭据框
     * 要吃掉那 15% 的空白；切回 SSH 时按这几个值还原，所以必须记住
     * 而不能现算 —— 现算会拿到已经拉宽后的宽度，越切越宽。
     * 三者都在会话页布局时填入，0 表示尚未记下（此时不做伸缩）。
     */
    int passbox_base_width;
    int passbox_left;
    int passbox_max_right;
    struct recent_conn *history; /* recent connections, newest first */
    int nhistory;
    bool midsession;
};

static void sessionsaver_data_free(void *ssdv)
{
    struct sessionsaver_data *ssd = (struct sessionsaver_data *)ssdv;
    /* cn: 别让输入框 handler 继续指向已释放的按钮集合。 */
    if (cn_session_ssd == ssd)
        cn_session_ssd = NULL;
    free_recent_conns(ssd->history, ssd->nhistory);
    sfree(ssd);
}

/*
 * cn: 只刷新「用户名」框（声明见上方，这里是实现）。
 */
static void cn_refresh_userbox(dlgparam *dlg)
{
    if (cn_session_ssd && cn_session_ssd->userbox &&
        cn_session_ssd->userbox->handler)
        cn_session_ssd->userbox->handler(
            cn_session_ssd->userbox, dlg, dlg->data, EVENT_REFRESH);
}

/*
 * The list is drawn as a two-column table: host name on the left, port
 * on the right. A tab separates the fields, which is the standard way
 * for a dialog list to carry multi-column data; the port field is left
 * out entirely when no port is recorded so that the port column stays
 * empty rather than repeating the host.
 *
 * Keeping the port in its own field (rather than "host:port") matters
 * for two reasons: two entries for the same host on different ports
 * stay tellable apart, and the port always lines up in one column
 * instead of drifting with the length of the host name.
 */
static char *recent_entry_text(const struct recent_conn *rc)
{
    if (rc->port > 0)
        return dupprintf("%s\t%d", rc->host, rc->port);
    return dupprintf("%s\t", rc->host);
}

/*
 * cn: 会话页底部那两个按钮的可用状态。
 *
 * 需求要求「主机名与端口都非空才可点」，同时在不可点状态下点击还要
 * 弹提示。这两件事在 Windows 上互斥 —— EnableWindow(FALSE) 之后按钮
 * 根本收不到 WM_COMMAND。因此这里用视觉弱化代替真正的禁用：按钮依然
 * 可点，只是文字画成灰色，点下去由处理器负责弹提示。
 *
 * 判断用 Conf 里的实际值，而不是控件上显示的文字，这样会话列表回填、
 * 协议切换改端口等路径都能自动跟上。
 */
static bool session_page_inputs_filled(Conf *conf)
{
    const char *host = conf_get_str(conf, CONF_host);
    int protocol = conf_get_int(conf, CONF_protocol);

    if (protocol == PROT_SERIAL)
        return conf_get_str(conf, CONF_serline)[0] != 0;

    if (!host || !host[0])
        return false;

    if (protocol == PROT_TELNET)
        return conf_get_str(conf, CONF_host)[0] != 0;

    /*
     * SSH / Telnet 都要求端口有值。端口框里显示空白等价于「未填写」，
     * 上游用 0 表示这种状态（见 config_port_handler）。
     */
    return conf_get_int(conf, CONF_port) != 0;
}

/*
 * cn: SSH 下「四项填齐」的判定 —— 主机名、端口、用户名、凭据。
 *
 * 这是「点连接/传输就直接进会话、不再弹身份验证」的前提条件。四项齐
 * 了才有把握跳过一次交互：用户名给了 CONF_username，SSH 不问 login as；
 * 凭据给了自动应答槽位，SSH 不问 password。
 *
 * 与 session_page_inputs_filled 分开，是因为后者还要服务 Telnet 和串口，
 * 那两个协议既没有用户名也不是口令认证，不该被这道门拦住。
 */
static bool session_ssh_creds_filled(Conf *conf)
{
    if (!session_page_inputs_filled(conf))
        return false;
    if (!conf_get_str_ambi(conf, CONF_username, NULL)[0])
        return false;
    /*
     * 密码与私钥是同级凭据，任一就绪即可（原先只认密码，填了私钥的
     * 合法组合会被误判成「缺凭据」）。私钥在 CONF_keyfile，路径非空
     * 即已配置。
     */
    if (cmdline_have_password())
        return true;
    return conf_get_filename(conf, CONF_keyfile)->utf8path[0] != '\0';
}

/*
 * cn: 指出四项里到底缺了哪项，给用户一句能照着补的提示。
 * 齐了返回 NULL。
 */
static char *cn_ssh_creds_missing(Conf *conf)
{
    if (!session_page_inputs_filled(conf))
        return dupstr("请先填齐主机名称（或 IP 地址）和端口号。");
    if (!conf_get_str_ambi(conf, CONF_username, NULL)[0])
        return dupstr("文件传输需要用户名，请先填写「用户名」。");
    if (!cmdline_have_password() &&
        !conf_get_filename(conf, CONF_keyfile)->utf8path[0])
        return dupstr("文件传输需要凭据，请在「凭据（密码）」框中填写"
                      "密码，或点「密钥」选择私钥文件。");
    return NULL;
}

/*
 * cn: 把某个按钮画成「弱化」或「正常」样式。
 *
 * 做法是给按钮挂一个自绘子类，把文字画成灰色（未填齐）或恢复默认
 * 的窗口文字色（填齐），不改 EnableWindow —— 按钮始终可以接收点击，
 * 这样「不可点状态下点击要弹提示」才做得到。
 */
static void set_button_muted(dlgparam *dlg, dlgcontrol *ctrl, bool muted)
{
    int id;

    if (!ctrl)
        return;
    id = dlg_get_ctrl_base_id(dlg, ctrl);
    if (id < 0)
        return;
    dlg_set_button_muted(dlg, id, muted);
}

/* cn: 未填齐时点按钮的统一提示。 */
#define CN_INPUT_INCOMPLETE_MSG \
    "请先输入主机名称（IP 地址）和端口号"

/*
 * cn: 按当前 Conf 刷新会话页两个按钮的外观与显隐。
 *
 *   「载入密钥」——仅在 SSH 协议下显示；主机名与端口填齐才显示为可用。
 *   「连接」    ——SSH/Telnet 要求主机名与端口填齐；串口只要选了串口。
 *
 * 两个按钮都在不可用状态下保持可点，点下去弹 CN_INPUT_INCOMPLETE_MSG。
 */
static void refresh_session_buttons(struct sessionsaver_data *ssd,
                                    dlgparam *dlg, Conf *conf)
{
    bool ssh = conf_get_int(conf, CONF_protocol) == PROT_SSH;
    bool filled = session_page_inputs_filled(conf);
    /*
     * cn: 两个按钮的门控故意不同。
     *
     *   「连接」    —— 只要主机名与端口填齐即可点。只填这两项时照旧
     *                 走原有流程（连接过程中再问用户名/密码）；四项都
     *                 填了则一路直达交互式会话，不再弹认证界面。
     *   「文件传输」—— 必须四项填齐才转为可点。传输窗口是个独立窗口，
     *                 里面没有输入框可以补认证信息，少了任何一项都只
     *                 能卡在连不上的状态，不如一开始就不放行。
     */
    bool creds_ready = ssh && session_ssh_creds_filled(conf);

    if (ssd->midsession)
        return;

    /*
     * 「载入密钥」只对 SSH 有意义：串口、Telnet 下没有任何密钥认证
     * 可言，按钮直接隐藏（连同它所占的格子一起让给列表）。
     */
    if (ssd->loadkeybutton) {
        int id = dlg_get_ctrl_base_id(dlg, ssd->loadkeybutton);
        if (id >= 0)
            dlg_set_ctrl_visible(dlg, id, ssh);
        /* 按钮本身只要目标填了就可以点，与认证方式无关。 */
        set_button_muted(dlg, ssd->loadkeybutton, !filled);
    }

    /*
     * cn: 「密钥」藏起来之后，它右边那 15% 的列宽就空着了，凭据框右
     * 边会平白多出一段留白。这里让凭据框接着往右长，吃掉这段空白。
     *
     * 隐藏只是 ShowWindow，布局不会重排，所以宽度得自己给。
     *
     * 基准值（SSH 下的宽度、左边界）只在第一次拿到有效尺寸时记一次：
     * 必须趁协议还是 SSH 的时候记，否则记下来的就是已经拉宽的值。
     * 延伸的右边界用「端口框右端」—— 它和主机名框是同一组的两列，
     * 合起来占满整行，所以端口框的右端就是本页输入区的自然右界，
     * 凭据框拉到那里正好与上方两框右端对齐。
     */
    if (ssd->passbox && ssd->portbox) {
        int pw = dlg_get_ctrl_width(dlg, ssd->passbox);
        int pl = dlg_get_ctrl_left(dlg, ssd->passbox);
        int pr = dlg_get_ctrl_left(dlg, ssd->portbox);
        int pwid = dlg_get_ctrl_width(dlg, ssd->portbox);
        {
            if (ssd->passbox_base_width <= 0 && pw > 0 && pl >= 0) {
                ssd->passbox_base_width = pw;
                ssd->passbox_left = pl;
                if (pr >= 0 && pwid > 0)
                    ssd->passbox_max_right = pr + pwid;
            }

            {
                int width = ssd->passbox_base_width;
                if (!ssh && ssd->passbox_max_right > ssd->passbox_left) {
                    int wide = ssd->passbox_max_right - ssd->passbox_left;
                    if (wide > width)
                        width = wide;
                }
                if (width > 0)
                    dlg_set_ctrl_width(dlg, ssd->passbox, width);
            }
        }
    }

    set_button_muted(dlg, ssd->okbutton, !filled);

    /* 「文件传输」：SSH 且四项齐全，缺一不可。 */
    if (ssd->xferbutton)
        set_button_muted(dlg, ssd->xferbutton, !creds_ready);
}

/*
 * Refill the list box from the persistent history of recently used
 * connections.
 */
static void refresh_history(struct sessionsaver_data *ssd, dlgparam *dlg)
{
    int i;

    free_recent_conns(ssd->history, ssd->nhistory);
    ssd->history = get_recent_conns(&ssd->nhistory);

    dlg_update_start(ssd->listbox, dlg);
    dlg_listbox_clear(ssd->listbox, dlg);
    for (i = 0; i < ssd->nhistory; i++) {
        char *text = recent_entry_text(&ssd->history[i]);
        dlg_listbox_addwithid(ssd->listbox, dlg, text, i);
        sfree(text);
    }
    dlg_update_done(ssd->listbox, dlg);
}

/*
 * Put the history entry selected in the list box into the host name
 * and port boxes above it, and keep the Conf in sync so「打开」can
 * use them. Entries with no recorded port leave the port box alone,
 * so the protocol default stays in force.
 *
 * The history is a list of network connections. Under the serial
 * backend the host box is the serial port picker and the port box is
 * the baud rate, so an entry must not be poured into either of them.
 */
static void select_history_entry(struct sessionsaver_data *ssd,
                                 dlgparam *dlg, Conf *conf)
{
    int i = dlg_listbox_index(ssd->listbox, dlg);

    if (conf_get_int(conf, CONF_protocol) == PROT_SERIAL)
        return;

    if (i >= 0 && ssd->hostbox) {
        int id = dlg_listbox_getid(ssd->listbox, dlg, i);
        if (id >= 0 && id < ssd->nhistory) {
            struct recent_conn *rc = &ssd->history[id];
            dlg_editbox_set(ssd->hostbox, dlg, rc->host);
            conf_set_str(conf, CONF_host, rc->host);
            if (rc->port > 0 && ssd->portbox) {
                char portbuf[16];
                sprintf(portbuf, "%d", rc->port);
                dlg_editbox_set(ssd->portbox, dlg, portbuf);
                conf_set_int(conf, CONF_port, rc->port);
            }
            /*
             * 用户名与端口同构：条目里记了才回填（旧条目没有，用户名
             * 框保持不动）。列表文本不含用户名，显示层不受影响。
             */
            if (rc->username && rc->username[0] && ssd->userbox) {
                dlg_editbox_set(ssd->userbox, dlg, rc->username);
                conf_set_str(conf, CONF_username, rc->username);
            }
        }
    }
}

/*
 * cn: Persist the "默认设置" session with the same protections as the
 * save button: the auto-fallback serial port is not baked in (H3),
 * and the connection targets (host/port/protocol) are written as the
 * pristine defaults. Used by the save button and by "保存并退出"
 * in the close prompt. Returns NULL on success, else the error text
 * (caller frees).
 */
char *cn_persist_default_settings(Conf *conf)
{
    /*
     * H3: the auto-fallback port must not be baked into the registry.
     * If a fallback happened and the user has not picked a port by
     * hand since, persist the originally saved value instead, then
     * restore the runtime value so that "Open" still uses the
     * substituted port.
     */
    char *runtime_port = NULL;
    if (serial_fallback_active && !serial_user_picked &&
        serial_fallback_original) {
        runtime_port = dupstr(conf_get_str(conf, CONF_serline));
        conf_set_str(conf, CONF_serline, serial_fallback_original);
    }
    /*
     * The host name, port and protocol are connection targets, not
     * settings: persist the pristine startup defaults (empty host,
     * SSH / 22) so that the next launch starts clean, then restore
     * what the user is currently editing so "Open" still works right
     * after saving.
     *
     * cn: 用户名同属连接目标 —— 连过某台设备后再点保存，不该把那台
     * 设备的用户名固化成全局默认，否则下一次打开配置框会预填别人的
     * 登录名（WinTitle=TEST 固化事故的同款机制）。凭据本来就不落盘，
     * 无需处理。
     */
    char *runtime_host = dupstr(conf_get_str(conf, CONF_host));
    /*
     * cn: CONF_username is STR_AMBI — conf_get_str() would trip its
     * value_type assertion (crash dialog on every save). Use the
     * ambi accessor; conf_set_str() below is fine for STR_AMBI.
     */
    bool cn_user_utf8;
    char *runtime_username =
        dupstr(conf_get_str_ambi(conf, CONF_username, &cn_user_utf8));
    /*
     * cn: 私钥文件与独立证书同属连接凭据，不固化进全局默认 ——
     * 保存时写成空，落盘后立刻还原运行值（同 host/username 机制）。
     */
    Filename *runtime_keyfile =
        filename_copy(conf_get_filename(conf, CONF_keyfile));
    Filename *runtime_cert =
        filename_copy(conf_get_filename(conf, CONF_detached_cert));
    int runtime_portnum = conf_get_int(conf, CONF_port);
    int runtime_proto = conf_get_int(conf, CONF_protocol);
    conf_set_str(conf, CONF_host, "");
    conf_set_str(conf, CONF_username, "");
    Filename *cn_empty_fn = filename_from_str("");
    conf_set_filename(conf, CONF_keyfile, cn_empty_fn);
    conf_set_filename(conf, CONF_detached_cert, cn_empty_fn);
    filename_free(cn_empty_fn);
    conf_set_int(conf, CONF_port, 22);
    conf_set_int(conf, CONF_protocol, PROT_SSH);
    char *errmsg = save_settings("", conf);
    conf_set_str(conf, CONF_host, runtime_host);
    conf_set_str(conf, CONF_username, runtime_username);
    conf_set_filename(conf, CONF_keyfile, runtime_keyfile);
    conf_set_filename(conf, CONF_detached_cert, runtime_cert);
    conf_set_int(conf, CONF_port, runtime_portnum);
    conf_set_int(conf, CONF_protocol, runtime_proto);
    sfree(runtime_host);
    sfree(runtime_username);
    filename_free(runtime_keyfile);
    filename_free(runtime_cert);
    if (runtime_port) {
        conf_set_str(conf, CONF_serline, runtime_port);
        sfree(runtime_port);
    }
    return errmsg;
}

static void sessionsaver_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                 void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct sessionsaver_data *ssd =
        (struct sessionsaver_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == ssd->listbox)
            refresh_history(ssd, dlg);
        /*
         * 按钮外观 / 显隐依赖协议与主机端口，任何一次面板刷新都重算
         * 一遍（切页回来时控件是新建的，状态必须重新施加）。
         */
        refresh_session_buttons(ssd, dlg, conf);
    } else if (event == EVENT_SELCHANGE) {
        if (ctrl == ssd->listbox)
            select_history_entry(ssd, dlg, conf);
    } else if (event == EVENT_ACTION) {
        if (ctrl == ssd->listbox) {
            /* Double-clicking a history entry fills it in, too; it
             * deliberately does not start a connection. */
            select_history_entry(ssd, dlg, conf);
        } else if (ctrl == ssd->savebutton) {
            {
                /* 全局只保留「默认设置」一个保存目录：候选框里的地址
                 * 只是连接目标，不再被当成会话名建目录。 */
                /*
                 * Confirm with a list of what is about to be saved.
                 * The button is only clickable when dirty, so the
                 * list is never empty here.
                 */
                if (!dlg_confirm_save(dlg))
                    return;
                char *errmsg = cn_persist_default_settings(conf);
                if (errmsg) {
                    dlg_error_msg(dlg, errmsg);
                    sfree(errmsg);
                } else {
                    /* cn: no success popup — the save button going
                     * dark again is the visible confirmation. */
                    dlg_mark_clean(dlg);
                }
            }
        } else if (ssd->initbutton && ctrl == ssd->initbutton) {
            /*
             * cn: 恢复到「设置初始化快照」。快照在配置框每次打开时
             * 自动确保存在（首次以当时的配置为基准），点击恢复需要
             * 二次确认，并展示将回退的内容清单。
             */
            Conf *snap = conf_new();
            char *changes = NULL;
            bool go = false;
            load_settings("SettingsSnapshot", snap);
            if (!conf_equal(conf, snap))
                changes = conf_describe_changes(conf, snap);
            if (!changes) {
                dlg_info_msg(dlg, "当前设置已与初始化快照一致。");
            } else if (dlg_confirm_changes(dlg, "确认恢复", changes)) {
                go = true;
            }
            if (go) {
                conf_copy_into(conf, snap);
                dlg_refresh(NULL, dlg);
                dlg_mark_clean(dlg);
            }
            sfree(changes);
            conf_free(snap);
        } else if (!ssd->midsession &&
                   ssd->loadkeybutton && ctrl == ssd->loadkeybutton) {
            /*
             * cn: 「载入密钥」。主机名与端口没填齐时不做跳转，先让
             * 用户补齐目标 —— 密钥是给某个具体连接用的，没有目标
             * 就跳过去也填不出所以然。
             */
            if (!session_page_inputs_filled(conf)) {
                dlg_info_msg(dlg, CN_INPUT_INCOMPLETE_MSG);
                return;
            }
            /* 切过去之后把焦点送进私钥框，省用户一次点击。 */
            cn_focus_keyfile_on_refresh = true;
            if (!dlg_navigate_to(dlg, "连接/SSH/认证/证书")) {
                cn_focus_keyfile_on_refresh = false;
                dlg_info_msg(dlg, "无法打开密钥设置页，请从左侧分类手动进入"
                             "「连接 / SSH / 认证 / 证书」。");
            }
        } else if (!ssd->midsession &&
                   ssd->xferbutton && ctrl == ssd->xferbutton) {
            /*
             * cn: 「文件传输」。前置条件与「连接」完全一致：先允许用
             * 历史列表里选中的条目补齐主机名与端口，再校验填齐，再拦
             * 协议前缀。区别只是不走 SSH 终端，而是直接进传输界面。
             */
            if (!conf_launchable(conf) &&
                conf_get_int(conf, CONF_protocol) != PROT_SERIAL) {
                int i = dlg_listbox_index(ssd->listbox, dlg);
                if (i >= 0) {
                    int id = dlg_listbox_getid(ssd->listbox, dlg, i);
                    if (id >= 0 && id < ssd->nhistory) {
                        struct recent_conn *rc = &ssd->history[id];
                        conf_set_str(conf, CONF_host, rc->host);
                        if (rc->port > 0)
                            conf_set_int(conf, CONF_port, rc->port);
                    }
                }
            }
            if (!session_page_inputs_filled(conf)) {
                dlg_info_msg(dlg, CN_INPUT_INCOMPLETE_MSG);
                refresh_session_buttons(ssd, dlg, conf);
                return;
            }
            if (conf_get_int(conf, CONF_protocol) != PROT_SSH) {
                dlg_info_msg(dlg, "文件传输仅支持 SSH 连接。");
                return;
            }
            /*
             * cn: 传输窗口是独立窗口，没有地方补认证信息，所以这里
             * 必须四项齐全才放行 —— 缺哪项就点名要哪项，别让人反复
             * 猜是哪里没填。
             */
            {
                char *missing = cn_ssh_creds_missing(conf);
                if (missing) {
                    dlg_info_msg(dlg, missing);
                    refresh_session_buttons(ssd, dlg, conf);
                    return;
                }
            }
            {
                char *msg = cn_hostbox_scheme_complaint(conf);
                if (msg) {
                    dlg_info_msg(dlg, msg);
                    sfree(msg);
                    return;
                }
            }
            if (conf_launchable(conf))
                add_recent_conn(conf_get_str(conf, CONF_host),
                                conf_get_int(conf, CONF_port),
                                conf_get_str_ambi(conf, CONF_username, NULL));
            /*
             * 就地打开传输窗口：配置框保持显示，加载动画转完（传输窗口
             * 就绪）才结束对话框。
             *
             * ⚠️ 错误提示必须由本处理器自己弹（owner = 配置框，可见）。
             * sftp_window_open 是同步的，早先它内部拿未显示的传输主窗
             * 弹 MessageBox，弹框不被置前、还把嵌套模态循环独占 GUI
             * 线程，外层 ShinyDialogBox 循环被堵死，配置框所有退出方式
             * 全部失效（2026-11-06 实测事故）。现在底层只返回错误文案，
             * 弹框结束后本处理器正常返回，外层循环得以继续。
             *
             * 等待期间 sftp_pump 泵消息，配置框与加载动画照常活动；
             * 打开函数内部有 opening 挡板，连接期间重复点击不会叠出
             * 第二条连接。失败时不置 cn_session_wants_xfer、不 dlg_end，
             * 保留配置框供改参数重试。
             */
            {
                char *err = sftp_window_open(conf, NULL);
                if (err) {
                    /* dlg_error_msg 以 dp->hwnd（配置框）为 owner，天然可见 */
                    dlg_error_msg(dlg, err);
                    sfree(err);
                } else if (sftp_window_is_open()) {
                    cn_session_wants_xfer = true;
                    dlg_end(dlg, 1);
                }
            }
        } else if (!ssd->midsession &&
                   ssd->delbutton && ctrl == ssd->delbutton) {
            int i = dlg_listbox_index(ssd->listbox, dlg);
            if (i < 0) {
                dlg_beep(dlg);
            } else {
                int id = dlg_listbox_getid(ssd->listbox, dlg, i);
                if (id >= 0 && id < ssd->nhistory) {
                    del_recent_conn(ssd->history[id].host,
                                    ssd->history[id].port,
                                    ssd->history[id].username);
                    refresh_history(ssd, dlg);
                }
            }
        } else if (ctrl == ssd->okbutton) {
            /* 退出时关闭窗口固定为「从不」。 */
            conf_set_int(conf, CONF_close_on_exit, FORCE_OFF);
            if (ssd->midsession) {
                /* In a mid-session Change Settings, Apply takes effect
                 * at once, but the box stays open so that further
                 * settings can be changed and applied as well. */
                if (!dlg_apply_config(dlg))
                    dlg_end(dlg, 1);
                else {
                    /* 已保存，应用/撤销按钮回到置灰状态。 */
                    dlg_mark_clean(dlg);
                    dlg_info_msg(dlg, "修改成功！");
                }
                return;
            }
            /*
             * If no host name has been entered, fall back to the
             * history entry selected in the list box. Under the serial
             * backend the list is irrelevant: the host box is the
             * serial port picker, and the history holds network hosts.
             */
            if (!conf_launchable(conf) &&
                conf_get_int(conf, CONF_protocol) != PROT_SERIAL) {
                int i = dlg_listbox_index(ssd->listbox, dlg);
                if (i >= 0) {
                    int id = dlg_listbox_getid(ssd->listbox, dlg, i);
                    if (id >= 0 && id < ssd->nhistory) {
                        struct recent_conn *rc = &ssd->history[id];
                        conf_set_str(conf, CONF_host, rc->host);
                        if (rc->port > 0)
                            conf_set_int(conf, CONF_port, rc->port);
                    }
                }
            }
            /*
             * cn: SSH / Telnet 下要求主机名与端口都填了才放行。历史
             * 列表回填发生在上面，所以从列表里选一条再点「连接」不会
             * 被这道门拦住。
             */
            if (!session_page_inputs_filled(conf)) {
                dlg_info_msg(dlg, CN_INPUT_INCOMPLETE_MSG);
                refresh_session_buttons(ssd, dlg, conf);
                return;
            }
            if (conf_launchable(conf)) {
                /*
                 * 主机名里带了 https:// 之类的协议前缀。SSH 层会把它
                 * 按 host:port 拆开，主机名退化成 "https"，最终报
                 * 「主机不存在」，用户根本看不出是格式问题。在这里
                 * 拦下来，明确告之主机名不能带协议。
                 *
                 * 串口模式下这个框是串口选择器，CONF_host 里可能残留
                 * 上一次网络配置填的网址，故只在网络协议下检查。
                 */
                {
                    char *msg = cn_hostbox_scheme_complaint(conf);
                    if (msg) {
                        dlg_info_msg(dlg, msg);
                        sfree(msg);
                        return;
                    }
                }
                /*
                 * Only network connections go into the history: the
                 * serial port box holds a device name, not a host, and
                 * the neighbouring box is a baud rate rather than a
                 * port number.
                 */
                if (conf_get_int(conf, CONF_protocol) != PROT_SERIAL)
                    add_recent_conn(conf_get_str(conf, CONF_host),
                                    conf_get_int(conf, CONF_port),
                                    conf_get_str_ambi(conf, CONF_username, NULL));
                dlg_end(dlg, 1);
            } else
                dlg_info_msg(dlg, "请先输入主机名称（或 IP 地址），"
                             "或从历史连接记录中选择一项。");
        } else if (ctrl == ssd->cancelbutton) {
            if (ssd->midsession) {
                /* 撤销最近一步修改；平台不支持历史时退回取消语义。 */
                if (!dlg_undo_change(dlg))
                    dlg_end(dlg, 0);
            } else {
                dlg_end(dlg, 0);
            }
        }
    }
}

struct charclass_data {
    dlgcontrol *listbox, *editbox, *button;
};

static void charclass_handler(dlgcontrol *ctrl, dlgparam *dlg,
                              void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct charclass_data *ccd =
        (struct charclass_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == ccd->listbox) {
            int i;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (i = 0; i < 128; i++) {
                char str[100];
                sprintf(str, "%d\t(0x%02X)\t%c\t%d", i, i,
                        (i >= 0x21 && i != 0x7F) ? i : ' ',
                        conf_get_int_int(conf, CONF_wordness, i));
                dlg_listbox_add(ctrl, dlg, str);
            }
            dlg_update_done(ctrl, dlg);
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == ccd->button) {
            char *str;
            int i, n;
            str = dlg_editbox_get(ccd->editbox, dlg);
            n = atoi(str);
            sfree(str);
            for (i = 0; i < 128; i++) {
                if (dlg_listbox_issel(ccd->listbox, dlg, i))
                    conf_set_int_int(conf, CONF_wordness, i, n);
            }
            dlg_refresh(ccd->listbox, dlg);
        }
    }
}

struct colour_data {
    dlgcontrol *listbox, *redit, *gedit, *bedit, *button;
};

/* Array of the user-visible colour names defined in the list macro in
 * putty.h */
static const char *const colours[] = {
    #define CONF_COLOUR_NAME_DECL(id,name) name,
    CONF_COLOUR_LIST(CONF_COLOUR_NAME_DECL)
    #undef CONF_COLOUR_NAME_DECL
};

static void colour_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct colour_data *cd =
        (struct colour_data *)ctrl->context.p;
    bool update = false, clear = false;
    int r, g, b;

    if (event == EVENT_REFRESH) {
        if (ctrl == cd->listbox) {
            int i;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (i = 0; i < lenof(colours); i++)
                dlg_listbox_add(ctrl, dlg, colours[i]);
            dlg_update_done(ctrl, dlg);
            clear = true;
            update = true;
        }
    } else if (event == EVENT_SELCHANGE) {
        if (ctrl == cd->listbox) {
            /* The user has selected a colour. Update the RGB text. */
            int i = dlg_listbox_index(ctrl, dlg);
            if (i < 0) {
                clear = true;
            } else {
                clear = false;
                r = conf_get_int_int(conf, CONF_colours, i*3+0);
                g = conf_get_int_int(conf, CONF_colours, i*3+1);
                b = conf_get_int_int(conf, CONF_colours, i*3+2);
            }
            update = true;
        }
    } else if (event == EVENT_VALCHANGE) {
        if (ctrl == cd->redit || ctrl == cd->gedit || ctrl == cd->bedit) {
            /* The user has changed the colour using the edit boxes. */
            char *str;
            int i, cval;

            str = dlg_editbox_get(ctrl, dlg);
            cval = atoi(str);
            sfree(str);
            if (cval > 255) cval = 255;
            if (cval < 0)   cval = 0;

            i = dlg_listbox_index(cd->listbox, dlg);
            if (i >= 0) {
                if (ctrl == cd->redit)
                    conf_set_int_int(conf, CONF_colours, i*3+0, cval);
                else if (ctrl == cd->gedit)
                    conf_set_int_int(conf, CONF_colours, i*3+1, cval);
                else if (ctrl == cd->bedit)
                    conf_set_int_int(conf, CONF_colours, i*3+2, cval);
            }
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == cd->button) {
            int i = dlg_listbox_index(cd->listbox, dlg);
            if (i < 0) {
                dlg_beep(dlg);
                return;
            }
            /*
             * Start a colour selector, which will send us an
             * EVENT_CALLBACK when it's finished and allow us to
             * pick up the results.
             */
            dlg_coloursel_start(ctrl, dlg,
                                conf_get_int_int(conf, CONF_colours, i*3+0),
                                conf_get_int_int(conf, CONF_colours, i*3+1),
                                conf_get_int_int(conf, CONF_colours, i*3+2));
        }
    } else if (event == EVENT_CALLBACK) {
        if (ctrl == cd->button) {
            int i = dlg_listbox_index(cd->listbox, dlg);
            /*
             * Collect the results of the colour selector. Will
             * return nonzero on success, or zero if the colour
             * selector did nothing (user hit Cancel, for example).
             */
            if (dlg_coloursel_results(ctrl, dlg, &r, &g, &b)) {
                conf_set_int_int(conf, CONF_colours, i*3+0, r);
                conf_set_int_int(conf, CONF_colours, i*3+1, g);
                conf_set_int_int(conf, CONF_colours, i*3+2, b);
                clear = false;
                update = true;
            }
        }
    }

    if (update) {
        if (clear) {
            dlg_editbox_set(cd->redit, dlg, "");
            dlg_editbox_set(cd->gedit, dlg, "");
            dlg_editbox_set(cd->bedit, dlg, "");
        } else {
            char buf[40];
            sprintf(buf, "%d", r); dlg_editbox_set(cd->redit, dlg, buf);
            sprintf(buf, "%d", g); dlg_editbox_set(cd->gedit, dlg, buf);
            sprintf(buf, "%d", b); dlg_editbox_set(cd->bedit, dlg, buf);
        }
    }
}

struct ttymodes_data {
    dlgcontrol *valradio, *valbox, *setbutton, *listbox;
};

static void ttymodes_handler(dlgcontrol *ctrl, dlgparam *dlg,
                             void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct ttymodes_data *td =
        (struct ttymodes_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == td->listbox) {
            char *key, *val;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (val = conf_get_str_strs(conf, CONF_ttymodes, NULL, &key);
                 val != NULL;
                 val = conf_get_str_strs(conf, CONF_ttymodes, key, &key)) {
                char *disp = dupprintf("%s\t%s", key,
                                       (val[0] == 'A') ? "(自动)" :
                                       ((val[0] == 'N') ? "(不发送)"
                                                        : val+1));
                dlg_listbox_add(ctrl, dlg, disp);
                sfree(disp);
            }
            dlg_update_done(ctrl, dlg);
        } else if (ctrl == td->valradio) {
            dlg_radiobutton_set(ctrl, dlg, 0);
        }
    } else if (event == EVENT_SELCHANGE) {
        if (ctrl == td->listbox) {
            int ind = dlg_listbox_index(td->listbox, dlg);
            char *val;
            if (ind < 0) {
                return; /* no item selected */
            }
            val = conf_get_str_str(conf, CONF_ttymodes,
                                   conf_get_str_nthstrkey(conf, CONF_ttymodes,
                                                          ind));
            assert(val != NULL);
            /* Do this first to defuse side-effects on radio buttons: */
            dlg_editbox_set(td->valbox, dlg, val+1);
            dlg_radiobutton_set(td->valradio, dlg,
                                val[0] == 'A' ? 0 : (val[0] == 'N' ? 1 : 2));
        }
    } else if (event == EVENT_VALCHANGE) {
        if (ctrl == td->valbox) {
            /* If they're editing the text box, we assume they want its
             * value to be used. */
            dlg_radiobutton_set(td->valradio, dlg, 2);
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == td->setbutton) {
            int ind = dlg_listbox_index(td->listbox, dlg);
            const char *key;
            char *str, *val;
            char type;

            {
                const char types[] = {'A', 'N', 'V'};
                int button = dlg_radiobutton_get(td->valradio, dlg);
                assert(button >= 0 && button < lenof(types));
                type = types[button];
            }

            /* Construct new entry */
            if (ind >= 0) {
                key = conf_get_str_nthstrkey(conf, CONF_ttymodes, ind);
                str = (type == 'V' ? dlg_editbox_get(td->valbox, dlg)
                                   : dupstr(""));
                val = dupprintf("%c%s", type, str);
                sfree(str);
                conf_set_str_str(conf, CONF_ttymodes, key, val);
                sfree(val);
                dlg_refresh(td->listbox, dlg);
                dlg_listbox_select(td->listbox, dlg, ind);
            } else {
                /* Not a multisel listbox, so this means nothing selected */
                dlg_beep(dlg);
            }
        }
    }
}

struct environ_data {
    dlgcontrol *varbox, *valbox, *addbutton, *rembutton, *listbox;
};

static void environ_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct environ_data *ed =
        (struct environ_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == ed->listbox) {
            char *key, *val;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (val = conf_get_str_strs(conf, CONF_environmt, NULL, &key);
                 val != NULL;
                 val = conf_get_str_strs(conf, CONF_environmt, key, &key)) {
                char *p = dupprintf("%s\t%s", key, val);
                dlg_listbox_add(ctrl, dlg, p);
                sfree(p);
            }
            dlg_update_done(ctrl, dlg);
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == ed->addbutton) {
            char *key, *val, *str;
            key = dlg_editbox_get(ed->varbox, dlg);
            if (!*key) {
                sfree(key);
                dlg_beep(dlg);
                return;
            }
            val = dlg_editbox_get(ed->valbox, dlg);
            if (!*val) {
                sfree(key);
                sfree(val);
                dlg_beep(dlg);
                return;
            }
            conf_set_str_str(conf, CONF_environmt, key, val);
            str = dupcat(key, "\t", val);
            dlg_editbox_set(ed->varbox, dlg, "");
            dlg_editbox_set(ed->valbox, dlg, "");
            sfree(str);
            sfree(key);
            sfree(val);
            dlg_refresh(ed->listbox, dlg);
        } else if (ctrl == ed->rembutton) {
            int i = dlg_listbox_index(ed->listbox, dlg);
            if (i < 0) {
                dlg_beep(dlg);
            } else {
                char *key, *val;

                key = conf_get_str_nthstrkey(conf, CONF_environmt, i);
                if (key) {
                    /* Populate controls with the entry we're about to delete
                     * for ease of editing */
                    val = conf_get_str_str(conf, CONF_environmt, key);
                    dlg_editbox_set(ed->varbox, dlg, key);
                    dlg_editbox_set(ed->valbox, dlg, val);
                    /* And delete it */
                    conf_del_str_str(conf, CONF_environmt, key);
                }
            }
            dlg_refresh(ed->listbox, dlg);
        }
    }
}

struct portfwd_data {
    dlgcontrol *addbutton, *rembutton, *listbox;
    dlgcontrol *sourcebox, *destbox, *direction;
#ifndef NO_IPV6
    dlgcontrol *addressfamily;
#endif
};

static void portfwd_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct portfwd_data *pfd =
        (struct portfwd_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == pfd->listbox) {
            char *key, *val;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (val = conf_get_str_strs(conf, CONF_portfwd, NULL, &key);
                 val != NULL;
                 val = conf_get_str_strs(conf, CONF_portfwd, key, &key)) {
                char *p;
                if (!strcmp(val, "D")) {
                    char *L;
                    /*
                     * A dynamic forwarding is stored as L12345=D or
                     * 6L12345=D (since it's mutually exclusive with
                     * L12345=anything else), but displayed as D12345
                     * to match the fiction that 'Local', 'Remote' and
                     * 'Dynamic' are three distinct modes and also to
                     * align with OpenSSH's command line option syntax
                     * that people will already be used to. So, for
                     * display purposes, find the L in the key string
                     * and turn it into a D.
                     */
                    p = dupprintf("%s\t", key);
                    L = strchr(p, 'L');
                    if (L) *L = 'D';
                } else
                    p = dupprintf("%s\t%s", key, val);
                dlg_listbox_add(ctrl, dlg, p);
                sfree(p);
            }
            dlg_update_done(ctrl, dlg);
        } else if (ctrl == pfd->direction) {
            /*
             * Default is Local.
             */
            dlg_radiobutton_set(ctrl, dlg, 0);
#ifndef NO_IPV6
        } else if (ctrl == pfd->addressfamily) {
            dlg_radiobutton_set(ctrl, dlg, 0);
#endif
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == pfd->addbutton) {
            const char *family, *type;
            char *src, *key, *val;
            int whichbutton;

#ifndef NO_IPV6
            whichbutton = dlg_radiobutton_get(pfd->addressfamily, dlg);
            if (whichbutton == 1)
                family = "4";
            else if (whichbutton == 2)
                family = "6";
            else
#endif
                family = "";

            whichbutton = dlg_radiobutton_get(pfd->direction, dlg);
            if (whichbutton == 0)
                type = "L";
            else if (whichbutton == 1)
                type = "R";
            else
                type = "D";

            src = dlg_editbox_get(pfd->sourcebox, dlg);
            if (!*src) {
                dlg_error_msg(dlg, "必须指定源端口号");
                sfree(src);
                return;
            }
            if (*type != 'D') {
                val = dlg_editbox_get(pfd->destbox, dlg);
                if (!*val || !host_strchr(val, ':')) {
                    dlg_error_msg(dlg,
                                  "目标地址需按 host.name:port 格式填写");
                    sfree(src);
                    sfree(val);
                    return;
                }
            } else {
                type = "L";
                val = dupstr("D");     /* special case */
            }

            key = dupcat(family, type, src);
            sfree(src);

            if (conf_get_str_str_opt(conf, CONF_portfwd, key)) {
                dlg_error_msg(dlg, "指定的转发已经存在");
            } else {
                conf_set_str_str(conf, CONF_portfwd, key, val);
            }

            sfree(key);
            sfree(val);
            dlg_refresh(pfd->listbox, dlg);
        } else if (ctrl == pfd->rembutton) {
            int i = dlg_listbox_index(pfd->listbox, dlg);
            if (i < 0) {
                dlg_beep(dlg);
            } else {
                char *key, *p;
                const char *val;

                key = conf_get_str_nthstrkey(conf, CONF_portfwd, i);
                if (key) {
                    static const char *const afs = "A46";
                    static const char *const dirs = "LRD";
                    const char *afp;
                    int dir;
#ifndef NO_IPV6
                    int idx;
#endif

                    /* Populate controls with the entry we're about to delete
                     * for ease of editing */
                    p = key;

                    afp = strchr(afs, *p);
#ifndef NO_IPV6
                    idx = afp ? afp-afs : 0;
#endif
                    if (afp)
                        p++;
#ifndef NO_IPV6
                    dlg_radiobutton_set(pfd->addressfamily, dlg, idx);
#endif

                    dir = *p;

                    val = conf_get_str_str(conf, CONF_portfwd, key);
                    if (!strcmp(val, "D")) {
                        dir = 'D';
                        val = "";
                    }

                    dlg_radiobutton_set(pfd->direction, dlg,
                                        strchr(dirs, dir) - dirs);
                    p++;

                    dlg_editbox_set(pfd->sourcebox, dlg, p);
                    dlg_editbox_set(pfd->destbox, dlg, val);
                    /* And delete it */
                    conf_del_str_str(conf, CONF_portfwd, key);
                }
            }
            dlg_refresh(pfd->listbox, dlg);
        }
    }
}

struct manual_hostkey_data {
    dlgcontrol *addbutton, *rembutton, *listbox, *keybox;
};

static void manual_hostkey_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct manual_hostkey_data *mh =
        (struct manual_hostkey_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == mh->listbox) {
            char *key, *val;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (val = conf_get_str_strs(conf, CONF_ssh_manual_hostkeys,
                                         NULL, &key);
                 val != NULL;
                 val = conf_get_str_strs(conf, CONF_ssh_manual_hostkeys,
                                         key, &key)) {
                dlg_listbox_add(ctrl, dlg, key);
            }
            dlg_update_done(ctrl, dlg);
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == mh->addbutton) {
            char *key;

            key = dlg_editbox_get(mh->keybox, dlg);
            if (!*key) {
                dlg_error_msg(dlg, "必须指定主机密钥或指纹");
                sfree(key);
                return;
            }

            if (!validate_manual_hostkey(key)) {
                dlg_error_msg(dlg, "主机密钥格式无效");
            } else if (conf_get_str_str_opt(conf, CONF_ssh_manual_hostkeys,
                                            key)) {
                dlg_error_msg(dlg, "指定的主机密钥已存在");
            } else {
                conf_set_str_str(conf, CONF_ssh_manual_hostkeys, key, "");
            }

            sfree(key);
            dlg_refresh(mh->listbox, dlg);
        } else if (ctrl == mh->rembutton) {
            int i = dlg_listbox_index(mh->listbox, dlg);
            if (i < 0) {
                dlg_beep(dlg);
            } else {
                char *key;

                key = conf_get_str_nthstrkey(conf, CONF_ssh_manual_hostkeys, i);
                if (key) {
                    dlg_editbox_set(mh->keybox, dlg, key);
                    /* And delete it */
                    conf_del_str_str(conf, CONF_ssh_manual_hostkeys, key);
                }
            }
            dlg_refresh(mh->listbox, dlg);
        }
    }
}

static void clipboard_selector_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                       void *data, int event)
{
    Conf *conf = (Conf *)data;
    int setting = ctrl->context.i;
#ifdef NAMED_CLIPBOARDS
    int strsetting = ctrl->context2.i;
#endif

    static const struct {
        const char *name;
        int id;
    } options[] = {
        {"无动作", CLIPUI_NONE},
        {CLIPNAME_IMPLICIT, CLIPUI_IMPLICIT},
        {CLIPNAME_EXPLICIT, CLIPUI_EXPLICIT},
    };

    if (event == EVENT_REFRESH) {
        int i, val = conf_get_int(conf, setting);

        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);

#ifdef NAMED_CLIPBOARDS
        for (i = 0; i < lenof(options); i++)
            dlg_listbox_add(ctrl, dlg, options[i].name);
        if (val == CLIPUI_CUSTOM) {
            const char *sval = conf_get_str(conf, strsetting);
            for (i = 0; i < lenof(options); i++)
                if (!strcmp(sval, options[i].name))
                    break;             /* needs escaping */
            if (i < lenof(options) || sval[0] == '=') {
                char *escaped = dupcat("=", sval);
                dlg_editbox_set(ctrl, dlg, escaped);
                sfree(escaped);
            } else {
                dlg_editbox_set(ctrl, dlg, sval);
            }
        } else {
            dlg_editbox_set(ctrl, dlg, options[0].name); /* fallback */
            for (i = 0; i < lenof(options); i++)
                if (val == options[i].id)
                    dlg_editbox_set(ctrl, dlg, options[i].name);
        }
#else
        for (i = 0; i < lenof(options); i++)
            dlg_listbox_addwithid(ctrl, dlg, options[i].name, options[i].id);
        dlg_listbox_select(ctrl, dlg, 0); /* fallback */
        for (i = 0; i < lenof(options); i++)
            if (val == options[i].id)
                dlg_listbox_select(ctrl, dlg, i);
#endif
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE
#ifdef NAMED_CLIPBOARDS
               || event == EVENT_VALCHANGE
#endif
        ) {
#ifdef NAMED_CLIPBOARDS
        char *sval = dlg_editbox_get(ctrl, dlg);
        int i;

        for (i = 0; i < lenof(options); i++)
            if (!strcmp(sval, options[i].name)) {
                conf_set_int(conf, setting, options[i].id);
                conf_set_str(conf, strsetting, "");
                break;
            }
        if (i == lenof(options)) {
            conf_set_int(conf, setting, CLIPUI_CUSTOM);
            if (sval[0] == '=')
                sval++;
            conf_set_str(conf, strsetting, sval);
        }

        sfree(sval);
#else
        int index = dlg_listbox_index(ctrl, dlg);
        if (index >= 0) {
            int val = dlg_listbox_getid(ctrl, dlg, index);
            conf_set_int(conf, setting, val);
        }
#endif
    }
}

static void clipboard_control(struct controlset *s, const char *label,
                              char shortcut, int percentage, HelpCtx helpctx,
                              int setting, int strsetting)
{
#ifdef NAMED_CLIPBOARDS
    ctrl_combobox(s, label, shortcut, percentage, helpctx,
                  clipboard_selector_handler, I(setting), I(strsetting));
#else
    /* strsetting isn't needed in this case */
    ctrl_droplist(s, label, shortcut, percentage, helpctx,
                  clipboard_selector_handler, I(setting));
#endif
}

static void serial_parity_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                  void *data, int event)
{
    static const struct {
        const char *name;
        int val;
    } parities[] = {
        {"无", SER_PAR_NONE},
        {"奇", SER_PAR_ODD},
        {"偶", SER_PAR_EVEN},
        {"始终为 1", SER_PAR_MARK},
        {"始终为 0", SER_PAR_SPACE},
    };
    int mask = ctrl->context.i;
    int i, j;
    Conf *conf = (Conf *)data;

    if (event == EVENT_REFRESH) {
        /* Fetching this once at the start of the function ensures we
         * remember what the right value is supposed to be when
         * operations below cause reentrant calls to this function. */
        int oldparity = conf_get_int(conf, CONF_serparity);

        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < lenof(parities); i++)  {
            if (mask & (1 << parities[i].val))
                dlg_listbox_addwithid(ctrl, dlg, parities[i].name,
                                      parities[i].val);
        }
        for (i = j = 0; i < lenof(parities); i++) {
            if (mask & (1 << parities[i].val)) {
                if (oldparity == parities[i].val) {
                    dlg_listbox_select(ctrl, dlg, j);
                    break;
                }
                j++;
            }
        }
        if (i == lenof(parities)) {    /* an unsupported setting was chosen */
            dlg_listbox_select(ctrl, dlg, 0);
            oldparity = SER_PAR_NONE;
        }
        dlg_update_done(ctrl, dlg);
        conf_set_int(conf, CONF_serparity, oldparity);    /* restore */
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i < 0)
            i = SER_PAR_NONE;
        else
            i = dlg_listbox_getid(ctrl, dlg, i);
        conf_set_int(conf, CONF_serparity, i);
    }
}

static void serial_flow_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                void *data, int event)
{
    static const struct {
        const char *name;
        int val;
    } flows[] = {
        {"无", SER_FLOW_NONE},
        {"XON/XOFF", SER_FLOW_XONXOFF},
        {"RTS/CTS", SER_FLOW_RTSCTS},
        {"DSR/DTR", SER_FLOW_DSRDTR},
    };
    int mask = ctrl->context.i;
    int i, j;
    Conf *conf = (Conf *)data;

    if (event == EVENT_REFRESH) {
        /* Fetching this once at the start of the function ensures we
         * remember what the right value is supposed to be when
         * operations below cause reentrant calls to this function. */
        int oldflow = conf_get_int(conf, CONF_serflow);

        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < lenof(flows); i++)  {
            if (mask & (1 << flows[i].val))
                dlg_listbox_addwithid(ctrl, dlg, flows[i].name, flows[i].val);
        }
        for (i = j = 0; i < lenof(flows); i++) {
            if (mask & (1 << flows[i].val)) {
                if (oldflow == flows[i].val) {
                    dlg_listbox_select(ctrl, dlg, j);
                    break;
                }
                j++;
            }
        }
        if (i == lenof(flows)) {       /* an unsupported setting was chosen */
            dlg_listbox_select(ctrl, dlg, 0);
            oldflow = SER_FLOW_NONE;
        }
        dlg_update_done(ctrl, dlg);
        conf_set_int(conf, CONF_serflow, oldflow);/* restore */
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i < 0)
            i = SER_FLOW_NONE;
        else
            i = dlg_listbox_getid(ctrl, dlg, i);
        conf_set_int(conf, CONF_serflow, i);
    }
}

/*
 * 常见厂商的控制台串口参数。企业网络设备（华为、华三、思科、锐捷、
 * 中兴、Juniper、Aruba）基本清一色 9600-8-N-1 且无流控；MikroTik、
 * Ubiquiti 和服务器 BMC 走 115200-8-N-1；老式设备还有 7-E-1 的组合。
 * 选一个标准就把下面五项参数一次性填好，省得逐项手改。
 *
 * CONF_serstopbits 以半个位为单位，所以 1 个停止位记作 2。
 */
struct serial_standard {
    const char *name;
    int speed, databits, stopbits, parity, flow;
};

static const struct serial_standard serial_standards[] = {
    {"华为/华三",     9600, 8, 2, SER_PAR_NONE, SER_FLOW_NONE},
    {"思科",          9600, 8, 2, SER_PAR_NONE, SER_FLOW_NONE},
    {"锐捷",          9600, 8, 2, SER_PAR_NONE, SER_FLOW_NONE},
    {"中兴",          9600, 8, 2, SER_PAR_NONE, SER_FLOW_NONE},
    {"Juniper",       9600, 8, 2, SER_PAR_NONE, SER_FLOW_NONE},
    {"Aruba/HPE",     9600, 8, 2, SER_PAR_NONE, SER_FLOW_NONE},
    {"MikroTik",    115200, 8, 2, SER_PAR_NONE, SER_FLOW_NONE},
    {"Ubiquiti",    115200, 8, 2, SER_PAR_NONE, SER_FLOW_NONE},
    {"服务器BMC",   115200, 8, 2, SER_PAR_NONE, SER_FLOW_NONE},
    {"工业设备",     19200, 8, 2, SER_PAR_NONE, SER_FLOW_NONE},
    {"老式设备",      9600, 7, 2, SER_PAR_EVEN, SER_FLOW_NONE},
};

/* cn: lenof 返回无符号 sizeof 表达式，与 int 循环变量比较会触发
 * -Wsign-compare；这里收成 int，串口标准区块的循环全部受益。 */
#define NSERIAL_STANDARDS ((int)lenof(serial_standards))

/* 串口页上需要联动的控件，构建面板时填入。 */
static struct {
    dlgcontrol *standard, *speed, *databits, *stopbits, *parity, *flow;
} serial_ui;

/* 用户上一次选中的标准，用来区分参数完全相同的厂商条目。 */
static int serial_standard_last = -1;

static bool serial_standard_equals(int idx, Conf *conf)
{
    const struct serial_standard *std = &serial_standards[idx];

    return std->speed == conf_get_int(conf, CONF_serspeed) &&
        std->databits == conf_get_int(conf, CONF_serdatabits) &&
        std->stopbits == conf_get_int(conf, CONF_serstopbits) &&
        std->parity == conf_get_int(conf, CONF_serparity) &&
        std->flow == conf_get_int(conf, CONF_serflow);
}

static int serial_standard_match(Conf *conf)
{
    int i;

    for (i = 0; i < NSERIAL_STANDARDS; i++)
        if (serial_standard_equals(i, conf))
            return i;
    return -1;
}

/* 参数被手工改动后，让标准下拉重新对上号（对不上就显示“自定义”）。 */
static void serial_standard_sync(dlgparam *dlg)
{
    if (serial_ui.standard)
        dlg_refresh(serial_ui.standard, dlg);
}

static void serial_standard_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                    void *data, int event)
{
    Conf *conf = (Conf *)data;
    int i;

    if (event == EVENT_REFRESH) {
        int selected;

        if (serial_standard_last >= 0 &&
            serial_standard_last < NSERIAL_STANDARDS &&
            serial_standard_equals(serial_standard_last, conf)) {
            selected = serial_standard_last;
        } else {
            int match = serial_standard_match(conf);
            selected = (match >= 0) ? match : NSERIAL_STANDARDS;
        }

        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < NSERIAL_STANDARDS; i++)
            dlg_listbox_add(ctrl, dlg, serial_standards[i].name);
        dlg_listbox_add(ctrl, dlg, "自定义");
        dlg_listbox_select(ctrl, dlg, selected);
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int idx = dlg_listbox_index(ctrl, dlg);

        if (idx < 0)
            return;
        if (idx >= NSERIAL_STANDARDS) {
            serial_standard_last = NSERIAL_STANDARDS;
            return;
        }

        const struct serial_standard *std = &serial_standards[idx];
        conf_set_int(conf, CONF_serspeed, std->speed);
        conf_set_int(conf, CONF_serdatabits, std->databits);
        conf_set_int(conf, CONF_serstopbits, std->stopbits);
        conf_set_int(conf, CONF_serparity, std->parity);
        conf_set_int(conf, CONF_serflow, std->flow);
        serial_standard_last = idx;

        dlg_refresh(serial_ui.speed, dlg);
        dlg_refresh(serial_ui.databits, dlg);
        dlg_refresh(serial_ui.stopbits, dlg);
        dlg_refresh(serial_ui.parity, dlg);
        dlg_refresh(serial_ui.flow, dlg);
    }
}

/* 包一层通用处理函数，手工改参数时把标准下拉同步过去。 */
static void serial_editbox_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    conf_editbox_handler(ctrl, dlg, data, event);
    if (event == EVENT_VALCHANGE)
        serial_standard_sync(dlg);
}

static void serial_parity_std_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                      void *data, int event)
{
    serial_parity_handler(ctrl, dlg, data, event);
    if (event == EVENT_SELCHANGE)
        serial_standard_sync(dlg);
}

static void serial_flow_std_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                    void *data, int event)
{
    serial_flow_handler(ctrl, dlg, data, event);
    if (event == EVENT_SELCHANGE)
        serial_standard_sync(dlg);
}

void proxy_type_handler(dlgcontrol *ctrl, dlgparam *dlg,
                        void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        /*
         * We must fetch the previously configured value from the Conf
         * before we start modifying the drop-down list, otherwise the
         * spurious SELCHANGE we trigger in the process will overwrite
         * the value we wanted to keep.
         */
        int proxy_type = conf_get_int(conf, CONF_proxy_type);

        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);

        int index_to_select = 0, current_index = 0;

#define ADD(id, title) do {                                     \
            dlg_listbox_addwithid(ctrl, dlg, title, id);        \
            if (id == proxy_type)                               \
                index_to_select = current_index;                \
            current_index++;                                    \
        } while (0)

        ADD(PROXY_NONE, "无");
        ADD(PROXY_SOCKS5, "SOCKS 5");
        ADD(PROXY_SOCKS4, "SOCKS 4");
        ADD(PROXY_HTTP, "HTTP 连接");
        if (ssh_proxy_supported) {
            ADD(PROXY_SSH_TCPIP, "通过 SSH 代理使用端口转发");
            ADD(PROXY_SSH_EXEC, "通过 SSH 代理执行命令");
            ADD(PROXY_SSH_SUBSYSTEM, "通过 SSH 代理调用子系统");
        }
        if (ctrl->context.i & PROXY_UI_FLAG_LOCAL) {
            ADD(PROXY_CMD, "本地（运行外部程序连接）");
        }
        ADD(PROXY_TELNET, "Telnet（发送临时命令）");

#undef ADD

        dlg_listbox_select(ctrl, dlg, index_to_select);

        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i < 0)
            i = AUTO;
        else
            i = dlg_listbox_getid(ctrl, dlg, i);
        conf_set_int(conf, CONF_proxy_type, i);
    }
}

static void host_ca_button_handler(dlgcontrol *ctrl, dlgparam *dp,
                                   void *data, int event)
{
    if (event == EVENT_ACTION)
        show_ca_config_box(dp);
}

void setup_config_box(struct controlbox *b, bool midsession,
                      int protocol, int protcfginfo)
{
    const struct BackendVtable *backvt;
    struct controlset *s;
    struct sessionsaver_data *ssd;
    struct charclass_data *ccd;
    struct colour_data *cd;
    struct ttymodes_data *td;
    struct environ_data *ed;
    struct portfwd_data *pfd;
    struct manual_hostkey_data *mh;
    dlgcontrol *c;
    bool resize_forbidden = false;
    char *str;

    ssd = (struct sessionsaver_data *)
        ctrl_alloc_with_free(b, sizeof(struct sessionsaver_data),
                             sessionsaver_data_free);
    memset(ssd, 0, sizeof(*ssd));
    ssd->midsession = midsession;
    /* cn: 让主机名 / 端口输入框能触发按钮状态重算。 */
    cn_session_ssd = ssd;

    /*
     * The standard panel that appears at the bottom of all panels:
     * Open, Cancel, Apply etc.
     */
    s = ctrl_getset(b, "", "", "");
    ctrl_columns(s, 5, 20, 20, 20, 20, 20);
    ssd->savebutton = ctrl_pushbutton(s, "保存(V)", 'v',
                                      HELPCTX(no_help),
                                      sessionsaver_handler, P(ssd));
    ssd->savebutton->column = 0;
    ssd->okbutton = ctrl_pushbutton(s,
                                    (midsession ? "应用(A)" : "连接(O)"),
                                    (char)(midsession ? 'a' : 'o'),
                                    HELPCTX(no_help),
                                    sessionsaver_handler, P(ssd));
    ssd->okbutton->button.isdefault = true;
    ssd->okbutton->column = 3;
    /*
     * cn: 「文件传输」紧挨在「连接」左侧。前置条件与「连接」相同，
     * 点下去不进终端而是直接开传输窗口。
     *
     * 快捷键用 'f'：这一页的 a/c/d/n/o/p/r/s/t/u/v/w/x 都已被占用。
     */
    if (!midsession) {
        ssd->xferbutton = ctrl_pushbutton(s, "文件传输(F)", 'f',
                                          HELPCTX(no_help),
                                          sessionsaver_handler, P(ssd));
        ssd->xferbutton->column = 2;
    }
    ssd->cancelbutton = ctrl_pushbutton(s,
                                        (midsession ? "撤销(U)" : "退出(C)"),
                                        (char)(midsession ? 'u' : 'c'),
                                        HELPCTX(no_help),
                                        sessionsaver_handler, P(ssd));
    ssd->cancelbutton->button.iscancel = true;
    ssd->cancelbutton->column = 4;
    if (midsession) {
        /* 会话设置窗口：应用/撤销按钮的置灰状态由修改历史驱动。 */
        b->apply_button = ssd->okbutton;
        b->undo_button = ssd->cancelbutton;
    } else if (ssd->savebutton) {
        /* cn: the startup box's save button is dirty-gated the same
         * way (disabled until the settings change, rebased after each
         * successful save). */
        b->apply_button = ssd->savebutton;
    }
    /* We carefully don't close the 5-column part, so that platform-
     * specific add-ons can put extra buttons alongside Open and Cancel. */

    /*
     * The Session panel.
     */
    str = dupprintf("%s 会话基本设置", appname);
    ctrl_settitle(b, "会话", str);
    sfree(str);

    if (!midsession) {
        struct hostport *hp = (struct hostport *)
            ctrl_alloc(b, sizeof(struct hostport));
        memset(hp, 0, sizeof(*hp));
        hp->ssd = ssd;          /* cn: 协议切换时更新会话页按钮 */

        s = ctrl_getset(b, "会话", "hostport",
                        "填写要连接的目标地址");
        ctrl_columns(s, 2, 75, 25);
        /*
         * cn: 必须保持 ctrl_combobox（has_list=true）——串口模式下这
         * 个控件兼任端口选择器，下拉列表是功能必需（dlg_listbox_*
         * 对无列表控件会触发断言）。网络模式下列表留空不再灌历史，
         * 配合 dialog.c 的箭头盖绘按「列表为空」动态生效：网络模式
         * 看不到箭头也点不出下拉，串口模式端口选择器完好。历史连接
         * 的选择走下方会话列表（双击回填主机与端口）。
         */
        c = ctrl_combobox(s, HOST_BOX_TITLE, 'n', 100,
                          HELPCTX(session_hostname),
                          config_host_handler, I(0), I(0));
        c->column = 0;
        hp->host = c;
        ssd->hostbox = c;
        c = ctrl_editbox(s, PORT_BOX_TITLE, 'p', 100,
                         HELPCTX(session_hostname),
                         config_port_handler, I(0), I(0));
        c->column = 1;
        hp->port = c;
        ssd->portbox = c;

        /*
         * cn: 第二组 —— 「用户名」与「凭据（密码）」。
         *
         * 布局意图：第一行只放两个标签，第二行放两个输入框，最右侧
         * 一列（15%）留给「载入密钥」按钮。
         *
         * 两行的实现方式不同：
         *   标签行用 ctrl_text —— 宽度由它所在的列决定，正好压在下面
         *   输入框的正上方，形成「标题在上、输入框在下」的竖排关系。
         *   输入框行用 percentwidth=100 —— 走 editboxfw 分支，label 传
         *   NULL 时它只画 Edit 框并铺满整列，不会再挤出一个空标签占位。
         *
         * 早先试过 percentwidth=1 想让标签和框同行的做法：staticedit
         * 按百分比算框的起点，1% 只能算出不到一像素，框直接消失。
         *
         * 开新列组之前必须先收回单列 —— ctrl_columns 有断言
         * s->ncolumns == 1 || ncolumns == 1，连开两组会直接断言失败。
         */
        ctrl_columns(s, 1, 100);
        /*
         * 列宽按 50/35/15。第三列给「载入密钥」，15% 换算下来约 36px。
         * 注意：四个中文字在默认 GUI 字体下实测要 48px，加内边距至少
         * 58px，这一列装不下完整文案。此处保持约定的 50/35/15，按钮
         * 文字的实际显示由 dlg 层决定（见 cn_style_snapshot_buttons 里
         * 对该按钮的放行说明）。
         */
        ctrl_columns(s, 3, 50, 35, 15);
        c = ctrl_text(s, "用户名：", HELPCTX(session_hostname));
        c->column = 0;
        c = ctrl_text(s, "凭据（密码）：", HELPCTX(session_hostname));
        c->column = 1;
        /* 第三列这一行留空，给下一行的按钮让位。 */
        c = ctrl_text(s, "", HELPCTX(no_help));
        c->column = 2;

        c = ctrl_editbox(s, NULL, 'u', 100,
                         HELPCTX(session_hostname),
                         config_user_handler, I(0), I(0));
        c->column = 0;
        ssd->userbox = c;
        c = ctrl_editbox(s, NULL, 'w', 100,
                         HELPCTX(session_hostname),
                         config_pass_handler, I(0), I(0));
        c->column = 1;
        /* cn: 密码掩码 —— 别把口令明晃晃地摊在屏幕上。 */
        c->editbox.password = true;
        ssd->passbox = c;
        /*
         * cn: 「载入密钥」。原来是「删除」下面的竖排按钮，现在挪到
         * 凭据框右侧 —— 它跟密码是同一个用途的两条路（密钥或口令），
         * 摆在一起用户一眼就知道二选一。仅在 SSH 下显示。
         *
         * 文案用「密钥」而不是「载入密钥」：第三列按 50/35/15 分下来
         * 只有 36px，而四个中文字在默认 GUI 字体下实测要 48px 以上，
         * 会被截成「载入密…」。三字 36px 正好放得下。
         */
        ssd->loadkeybutton = ctrl_pushbutton(s, "密钥", NO_SHORTCUT,
                                             HELPCTX(ssh_auth_privkey),
                                             sessionsaver_handler, P(ssd));
        ssd->loadkeybutton->column = 2;

        ctrl_columns(s, 1, 100);
        c = ctrl_text(s, "连接类型：", HELPCTX(session_hostname));
        /* cn change: full-width row; the three buttons are spread
         * evenly across it by radioline_common's nacross logic. */
        c = ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                              HELPCTX(session_hostname),
                              config_protocols_handler, P(hp));
        hp->protradio = c;
        c->radio.buttons = sresize(c->radio.buttons, PROTOCOL_LIMIT, char *);
        c->radio.shortcuts = sresize(c->radio.shortcuts, PROTOCOL_LIMIT, char);
        c->radio.buttondata = sresize(c->radio.buttondata, PROTOCOL_LIMIT,
                                      intorptr);
        assert(c->radio.nbuttons == 0);
        /* UI design assumes there exists at least one 'real' radio button */
        assert(n_ui_backends > 0 && n_ui_backends < PROTOCOL_LIMIT);
        for (size_t i = 0; i < n_ui_backends; i++) {
            assert(backends[i]);
            c->radio.buttons[c->radio.nbuttons] =
                dupstr(backends[i]->displayname_tc);
            c->radio.shortcuts[c->radio.nbuttons] =
                (backends[i]->protocol == PROT_SSH ? 's' :
                 backends[i]->protocol == PROT_SERIAL ? 'r' :
                 backends[i]->protocol == PROT_RAW ? 'w' :  /* FIXME unused */
                 NO_SHORTCUT);
            c->radio.buttondata[c->radio.nbuttons] =
                I(backends[i]->protocol);
            c->radio.nbuttons++;
        }
        /* UI design assumes there exists at least one droplist entry */
        assert(backends[c->radio.nbuttons]);

        c->radio.buttons[c->radio.nbuttons] = dupstr("Telnet");
        c->radio.shortcuts[c->radio.nbuttons] = 't';
        c->radio.buttondata[c->radio.nbuttons] = I(PROT_TELNET);
        c->radio.nbuttons++;
    }

    /*
     * The Load/Save panel is available even in mid-session.
     */
    s = ctrl_getset(b, "会话", "savedsessions",
                    midsession ? "保存当前会话设置" :
                    "已保存的会话（载入／保存／删除）");
    ctrl_columns(s, 2, 75, 25);
    ssd->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                HELPCTX(session_saved),
                                sessionsaver_handler, P(ssd));
    ssd->listbox->column = 0;
    /*
     * cn: 行数 11 —— 会话页上方新增了「用户名 / 凭据」两行输入框，
     * 整个面板被往下推了约 42px；「退出时关闭窗口」下拉已移除，
     * 腾出的高度正好容纳 11 行列表（2026-11-07，主人指示）。
     *
     * WS_VSCROLL（由 Windows listbox 助手设置）会在历史条目超出可视
     * 范围时自动出现滚动条，因此行数不影响可访问的会话数量。
     */
    ssd->listbox->listbox.height = 11;
    /* 每行下方画一条细分隔线，让历史列表看起来像表格。 */
    ssd->listbox->listbox.ruledlines = true;
    /* 主机名与端口分两列显示，中间一条竖线，端口列宽固定。 */
    ssd->listbox->listbox.hostportcols = true;
    /* "Save" button lives in the standard panel at the bottom. */
    if (!midsession) {
        ssd->delbutton = ctrl_pushbutton(s, "删除(D)", 'd',
                                         HELPCTX(session_saved),
                                         sessionsaver_handler, P(ssd));
        ssd->delbutton->column = 1;
        /* cn: 一键恢复到设置初始化快照（快照由配置框打开时自动建立）。
         * 按钮贴历史列表框底边（cn_style_snapshot_buttons 定位），
         * 尺寸与删除键一致；字体小一号保证五个字完整显示。 */
        ssd->initbutton = ctrl_pushbutton(s, "初始化设置", NO_SHORTCUT,
                                          HELPCTX(no_help),
                                          sessionsaver_handler, P(ssd));
        ssd->initbutton->column = 1;
        ssd->initbutton->button.cn_smallfont = true;
    } else {
        /* Disable the Delete button mid-session too, for UI consistency. */
        ssd->delbutton = NULL;
    }
    ctrl_columns(s, 1, 100);
    /*
     * cn: 「退出时关闭窗口」下拉已按主人指示移除——会话页高度紧张，
     * 这行本来就只是固定「从不」的展示。行为保障在 sessionsaver_handler
     * 的 okbutton 分支里继续强制 CONF_close_on_exit = FORCE_OFF。
     */

    /*
     * The Session/Logging panel.
     */
    ctrl_settitle(b, "会话/日志记录", "会话日志记录选项");

    s = ctrl_getset(b, "会话/日志记录", "main", NULL);
    /*
     * The logging buttons change depending on whether SSH packet
     * logging can sensibly be available.
     */
    {
        const char *sshlogname, *sshrawlogname;
        if ((midsession && protocol == PROT_SSH) ||
            (!midsession && backend_vt_from_proto(PROT_SSH))) {
            sshlogname = "SSH 包";
            sshrawlogname = "SSH 包和 RAW 数据";
        } else {
            sshlogname = NULL;         /* this will disable both buttons */
            sshrawlogname = NULL;      /* this will just placate optimisers */
        }
        ctrl_radiobuttons(s, "会话日志记录：", NO_SHORTCUT, 2,
                          HELPCTX(logging_main),
                          loggingbuttons_handler,
                          I(CONF_logtype),
                          "无(T)", 't', I(LGTYP_NONE),
                          "可打印输出(P)", 'p', I(LGTYP_ASCII),
                          "所有会话输出(L)", 'l', I(LGTYP_DEBUG),
                          sshlogname, 's', I(LGTYP_PACKETS),
                          sshrawlogname, 'r', I(LGTYP_SSHRAW));
    }
    ctrl_filesel(s, "日志文件名(F)：", 'f',
                 FILTER_ALL_FILES, true, "选择会话的日志文件名",
                 HELPCTX(logging_filename),
                 conf_filesel_handler, I(CONF_logfilename));
    ctrl_text(s, "(日志文件名可以包含 &Y &M &D 表示年月日，"
              "&T 表示时间，&H 表示主机名称)",
              HELPCTX(logging_filename));
    ctrl_radiobuttons(s, "要记录的日志文件已存在时(E)：", 'e', 1,
                      HELPCTX(logging_exists),
                      conf_radiobutton_handler, I(CONF_logxfovr),
                      "总是覆盖", I(LGXF_OVR),
                      "总是添加到末尾", I(LGXF_APN),
                      "每次询问", I(LGXF_ASK));
    ctrl_checkbox(s, "快速刷新缓存到日志文件(U)", 'u',
                  HELPCTX(logging_flush),
                  conf_checkbox_handler, I(CONF_logflush));
    ctrl_checkbox(s, "包含行首标题(I)", 'i',
                  HELPCTX(logging_header),
                  conf_checkbox_handler, I(CONF_logheader));

    if ((midsession && protocol == PROT_SSH) ||
        (!midsession && backend_vt_from_proto(PROT_SSH))) {
        s = ctrl_getset(b, "会话/日志记录", "ssh",
                        "仅适用于 SSH 数据包日志记录的选项");
        ctrl_checkbox(s, "在日志中省略已知的密码字段(K)", 'k',
                      HELPCTX(logging_ssh_omit_password),
                      conf_checkbox_handler, I(CONF_logomitpass));
        ctrl_checkbox(s, "在日志中省略会话数据(D)", 'd',
                      HELPCTX(logging_ssh_omit_data),
                      conf_checkbox_handler, I(CONF_logomitdata));
    }

    /*
     * The Terminal panel.
     */
    ctrl_settitle(b, "终端", "终端模拟设置");

    s = ctrl_getset(b, "终端", "general", "设置各项终端选项");
    ctrl_checkbox(s, "初始启用自动换行(W)", 'w',
                  HELPCTX(terminal_autowrap),
                  conf_checkbox_handler, I(CONF_wrap_mode));
    ctrl_checkbox(s, "初始启用 DEC 原点模式（DEC Origin Mode）", 'd',
                  HELPCTX(terminal_decom),
                  conf_checkbox_handler, I(CONF_dec_om));
    ctrl_checkbox(s, "收到 LF 时自动补 CR", 'r',
                  HELPCTX(terminal_lfhascr),
                  conf_checkbox_handler, I(CONF_lfhascr));
    ctrl_checkbox(s, "收到 CR 时自动补 LF", 'f',
                  HELPCTX(terminal_crhaslf),
                  conf_checkbox_handler, I(CONF_crhaslf));
    ctrl_checkbox(s, "使用背景颜色清除屏幕(E)", 'e',
                  HELPCTX(terminal_bce),
                  conf_checkbox_handler, I(CONF_bce));
    ctrl_checkbox(s, "启用闪烁文本(N)", 'n',
                  HELPCTX(terminal_blink),
                  conf_checkbox_handler, I(CONF_blinktext));
    ctrl_editbox(s, "^E 的应答内容(S)：", 's', 100,
                 HELPCTX(terminal_answerback),
                 conf_editbox_handler, I(CONF_answerback), ED_STR);

    s = ctrl_getset(b, "终端", "ldisc", "行规程选项");
    ctrl_radiobuttons(s, "本地回显(L)：", 'l', 3,
                      HELPCTX(terminal_localecho),
                      conf_radiobutton_handler,I(CONF_localecho),
                      "自动", I(AUTO),
                      "强制开", I(FORCE_ON),
                      "强制关", I(FORCE_OFF));
    ctrl_radiobuttons(s, "本地行编辑(T)：", 't', 3,
                      HELPCTX(terminal_localedit),
                      conf_radiobutton_handler,I(CONF_localedit),
                      "自动", I(AUTO),
                      "强制开", I(FORCE_ON),
                      "强制关", I(FORCE_OFF));

    s = ctrl_getset(b, "终端", "printing", "远程控制打印");
    ctrl_combobox(s, "用于发送 ANSI 打印机输出的打印机(P)：", 'p', 100,
                  HELPCTX(terminal_printing),
                  printerbox_handler, P(NULL), P(NULL));

    /*
     * The Terminal/Keyboard panel.
     */
    ctrl_settitle(b, "终端/键盘",
                  "按键效果设置");

    s = ctrl_getset(b, "终端/键盘", "mappings",
                    "更改以下按键发送的序列：");
    ctrl_radiobuttons(s, "Backspace（退格）键", 'b', 2,
                      HELPCTX(keyboard_backspace),
                      conf_radiobutton_bool_handler,
                      I(CONF_bksp_is_delete),
                      "Control-H", I(0), "Control-? (127)", I(1));
    ctrl_radiobuttons(s, "Home 和 End 键", 'e', 2,
                      HELPCTX(keyboard_homeend),
                      conf_radiobutton_bool_handler,
                      I(CONF_rxvt_homeend),
                      "标准", I(false), "rxvt", I(true));
    ctrl_radiobuttons(s, "功能键与小键盘", 'f', 4,
                      HELPCTX(keyboard_funkeys),
                      conf_radiobutton_handler,
                      I(CONF_funky_type),
                      "ESC[n~", I(FUNKY_TILDE),
                      "Linux", I(FUNKY_LINUX),
                      "Xterm R6", I(FUNKY_XTERM),
                      "VT400", I(FUNKY_VT400),
                      "VT100+", I(FUNKY_VT100P),
                      "SCO", I(FUNKY_SCO),
                      "Xterm 216+", I(FUNKY_XTERM_216));
    ctrl_radiobuttons(s, "Shift/Ctrl/Alt 配合方向键(W)", 'w', 2,
                      HELPCTX(keyboard_sharrow),
                      conf_radiobutton_handler,
                      I(CONF_sharrow_type),
                      "Ctrl 切换应用模式", I(SHARROW_APPLICATION),
                      "xterm 风格位图", I(SHARROW_BITMAP));

    s = ctrl_getset(b, "终端/键盘", "appkeypad",
                    "小键盘应用模式设置：");
    ctrl_radiobuttons(s, "光标键初始状态(R)：", 'r', 3,
                      HELPCTX(keyboard_appcursor),
                      conf_radiobutton_bool_handler,
                      I(CONF_app_cursor),
                      "常规", I(0), "应用", I(1));
    ctrl_radiobuttons(s, "数字小键盘初始状态(N)：", 'n', 3,
                      HELPCTX(keyboard_appkeypad),
                      numeric_keypad_handler, P(NULL),
                      "常规", I(0), "应用", I(1), "NetHack", I(2));

    /*
     * The Terminal/Bell panel.
     */
    ctrl_settitle(b, "终端/响铃",
                  "终端响铃设置");

    s = ctrl_getset(b, "终端/响铃", "style", "设置响铃方式");
    ctrl_radiobuttons(s, "响铃时执行的动作(B)：", 'b', 1,
                      HELPCTX(bell_style),
                      conf_radiobutton_handler, I(CONF_beep),
                      "无 (禁用响铃)", I(BELL_DISABLED),
                      "使用系统默认警告声音", I(BELL_DEFAULT),
                      "可视响铃 (闪动窗口)", I(BELL_VISUAL));

    s = ctrl_getset(b, "终端/响铃", "overload",
                    "控制响铃过载行为");
    ctrl_checkbox(s, "大量重复响铃时临时禁用响铃(D)", 'd',
                  HELPCTX(bell_overload),
                  conf_checkbox_handler, I(CONF_bellovl));
    ctrl_editbox(s, "判定为过度响铃的次数(M)：", 'm', 20,
                 HELPCTX(bell_overload),
                 conf_editbox_handler, I(CONF_bellovl_n), ED_INT);

    static const struct conf_editbox_handler_type conf_editbox_tickspersec = {
        .type = EDIT_FIXEDPOINT, .denominator = TICKSPERSEC};

    ctrl_editbox(s, "判定时间窗口（秒）(T)：", 't', 20,
                 HELPCTX(bell_overload),
                 conf_editbox_handler, I(CONF_bellovl_t),
                 CP(&conf_editbox_tickspersec));
    ctrl_text(s, "静默数秒后自动重新启用响铃。",
              HELPCTX(bell_overload));
    ctrl_editbox(s, "重新启用前需静默的秒数(S)：", 's', 20,
                 HELPCTX(bell_overload),
                 conf_editbox_handler, I(CONF_bellovl_s),
                 CP(&conf_editbox_tickspersec));

    /*
     * The Terminal/Features panel.
     */
    ctrl_settitle(b, "终端/特性",
                  "启用／禁用高级终端特性");

    s = ctrl_getset(b, "终端/特性", "main", NULL);
    ctrl_checkbox(s, "禁用应用光标键模式(U)", 'u',
                  HELPCTX(features_application),
                  conf_checkbox_handler, I(CONF_no_applic_c));
    ctrl_checkbox(s, "禁用应用小键盘模式(K)", 'k',
                  HELPCTX(features_application),
                  conf_checkbox_handler, I(CONF_no_applic_k));
    ctrl_checkbox(s, "禁用 xterm 风格的鼠标上报", 'x',
                  HELPCTX(features_mouse),
                  conf_checkbox_handler, I(CONF_no_mouse_rep));
    ctrl_checkbox(s, "禁止远端调整终端大小(S)", 's',
                  HELPCTX(features_resize),
                  conf_checkbox_handler,
                  I(CONF_no_remote_resize));
    ctrl_checkbox(s, "禁止切换到备用终端屏幕(W)", 'w',
                  HELPCTX(features_altscreen),
                  conf_checkbox_handler, I(CONF_no_alt_screen));
    ctrl_checkbox(s, "禁止远端控制修改窗口标题(T)", 't',
                  HELPCTX(features_retitle),
                  conf_checkbox_handler,
                  I(CONF_no_remote_wintitle));
    ctrl_radiobuttons(s, "对远端标题查询的响应（安全相关）(Q)：", 'q', 3,
                      HELPCTX(features_qtitle),
                      conf_radiobutton_handler,
                      I(CONF_remote_qtitle_action),
                      "无", I(TITLE_NONE),
                      "空字符串", I(TITLE_EMPTY),
                      "窗口标题", I(TITLE_REAL));
    ctrl_checkbox(s, "禁止远端清空回滚缓冲区(E)", 'e',
                  HELPCTX(features_clearscroll),
                  conf_checkbox_handler,
                  I(CONF_no_remote_clearscroll));
    ctrl_checkbox(s, "禁止服务端发送 ^? 时执行破坏性退格(B)",'b',
                  HELPCTX(features_dbackspace),
                  conf_checkbox_handler, I(CONF_no_dbackspace));
    ctrl_checkbox(s, "禁止远端控制字符集配置(R)",
                  'r', HELPCTX(features_charset), conf_checkbox_handler,
                  I(CONF_no_remote_charset));
    ctrl_checkbox(s, "禁用阿拉伯文字形整形(L)",
                  'l', HELPCTX(features_arabicshaping), conf_checkbox_handler,
                  I(CONF_no_arabicshaping));
    ctrl_checkbox(s, "禁用双向文本显示(D)",
                  'd', HELPCTX(features_bidi), conf_checkbox_handler,
                  I(CONF_no_bidi));
    ctrl_checkbox(s, "禁用括号粘贴模式(P)",
                  'p', HELPCTX(features_bracketed_paste), conf_checkbox_handler,
                  I(CONF_no_bracketed_paste));

    /*
     * The Window panel.
     */
    str = dupprintf("%s 窗口设置", appname);
    ctrl_settitle(b, "窗口", str);
    sfree(str);

    backvt = backend_vt_from_proto(protocol);
    if (backvt)
        resize_forbidden = (backvt->flags & BACKEND_RESIZE_FORBIDDEN);

    if (!resize_forbidden || !midsession) {
        s = ctrl_getset(b, "窗口", "size", "设置窗口大小");
        ctrl_columns(s, 2, 50, 50);
        c = ctrl_editbox(s, "列(M)", 'm', 100,
                         HELPCTX(window_size),
                         conf_editbox_handler, I(CONF_width), ED_INT);
        c->column = 0;
        c = ctrl_editbox(s, "行(R)", 'r', 100,
                         HELPCTX(window_size),
                         conf_editbox_handler, I(CONF_height),ED_INT);
        c->column = 1;
        ctrl_columns(s, 1, 100);
    }

    s = ctrl_getset(b, "窗口", "scrollback",
                    "设置窗口回滚缓冲区");
    ctrl_editbox(s, "回滚行数(S)", 's', 50,
                 HELPCTX(window_scrollback),
                 conf_editbox_handler, I(CONF_savelines), ED_INT);
    ctrl_checkbox(s, "显示滚动条(D)", 'd',
                  HELPCTX(window_scrollback),
                  conf_checkbox_handler, I(CONF_scrollbar));
    ctrl_checkbox(s, "按键时清空回滚缓冲区(K)", 'k',
                  HELPCTX(window_scrollback),
                  conf_checkbox_handler, I(CONF_scroll_on_key));
    ctrl_checkbox(s, "有显示输出时重置回滚缓冲区(P)", 'p',
                  HELPCTX(window_scrollback),
                  conf_checkbox_handler, I(CONF_scroll_on_disp));
    ctrl_checkbox(s, "把被清除的文本移入回滚缓冲区(E)", 'e',
                  HELPCTX(window_erased),
                  conf_checkbox_handler,
                  I(CONF_erase_to_scrollback));

    /*
     * The Window/Appearance panel.
     */
    str = dupprintf("配置 %s 窗口外观", appname);
    ctrl_settitle(b, "窗口/外观", str);
    sfree(str);

    s = ctrl_getset(b, "窗口/外观", "cursor",
                    "调整光标的使用方式");
    ctrl_radiobuttons(s, "光标外观：", NO_SHORTCUT, 3,
                      HELPCTX(appearance_cursor),
                      conf_radiobutton_handler,
                      I(CONF_cursor_type),
                      "显示块(L)", 'l', I(CURSOR_BLOCK),
                      "下划线(U)", 'u', I(CURSOR_UNDERLINE),
                      "垂直线(V)", 'v', I(CURSOR_VERTICAL_LINE));
    ctrl_checkbox(s, "光标闪烁(B)", 'b',
                  HELPCTX(appearance_cursor),
                  conf_checkbox_handler, I(CONF_blink_cur));

    s = ctrl_getset(b, "窗口/外观", "font",
                    "字体设置");
    ctrl_fontsel(s, "终端窗口使用的字体(N)", 'n',
                 HELPCTX(appearance_font),
                 conf_fontsel_handler, I(CONF_font));

    s = ctrl_getset(b, "窗口/外观", "mouse",
                    "调整鼠标指针的使用方式");
    ctrl_checkbox(s, "在窗口中输入时隐藏鼠标指针(P)", 'p',
                  HELPCTX(appearance_hidemouse),
                  conf_checkbox_handler, I(CONF_hide_mouseptr));

    s = ctrl_getset(b, "窗口/外观", "border",
                    "调整窗口边框");
    ctrl_editbox(s, "文本与窗口边界的距离(E)：", 'e', 20,
                 HELPCTX(appearance_border),
                 conf_editbox_handler,
                 I(CONF_window_border), ED_INT);

    /*
     * The Window/Behaviour panel.
     */
    str = dupprintf("配置 %s 窗口行为", appname);
    ctrl_settitle(b, "窗口/行为", str);
    sfree(str);

    s = ctrl_getset(b, "窗口/行为", "title",
                    "调整窗口标题行为");
    {
        dlgcontrol *cn_wt = ctrl_editbox(
            s, "窗口标题(T)：", 't', 100,
            HELPCTX(appearance_title),
            conf_editbox_handler, I(CONF_wintitle), ED_STR);
        cn_wt->editbox.cn_cue = "默认为：主机名/IP地址：端口号";
    }
    ctrl_checkbox(s, "窗口标题与图标标题分别设置(I)", 'i',
                  HELPCTX(appearance_title),
                  conf_checkbox_handler,
                  I(CHECKBOX_INVERT | CONF_win_name_always));

    s = ctrl_getset(b, "窗口/行为", "main", NULL);
    ctrl_checkbox(s, "关闭窗口前给出警告(W)", 'w',
                  HELPCTX(behaviour_closewarn),
                  conf_checkbox_handler, I(CONF_warn_on_close));

    /*
     * The Window/Translation panel.
     */
    ctrl_settitle(b, "窗口/转换",
                  "字符集转换设置");

    s = ctrl_getset(b, "窗口/转换", "trans",
                    "字符集转换");
    ctrl_combobox(s, "远程字符集(R)：",
                  'r', 100, HELPCTX(translation_codepage),
                  codepage_handler, P(NULL), P(NULL));

    s = ctrl_getset(b, "窗口/转换", "tweaks", NULL);
    ctrl_checkbox(s, "将 CJK 宽度歧义字符按全角处理(W)", 'w',
                  HELPCTX(translation_cjk_ambig_wide),
                  conf_checkbox_handler, I(CONF_cjk_ambig_wide));

    str = dupprintf("调整 %s 如何绘制线条字符", appname);
    s = ctrl_getset(b, "窗口/转换", "linedraw", str);
    sfree(str);
    ctrl_radiobuttons(
        s, "线条绘制字符处理：", NO_SHORTCUT,1,
        HELPCTX(translation_linedraw),
        conf_radiobutton_handler, I(CONF_vtmode),
        "使用 Unicode 线条绘制码位(U)",'u',I(VT_UNICODE),
        "简单线条绘制（+、- 和 |）(P)",'p',I(VT_POORMAN));
    ctrl_checkbox(s, "复制粘贴时用 lqqqk 等 ASCII 字符表示线条绘制字符(D)",'d',
                  HELPCTX(selection_linedraw),
                  conf_checkbox_handler, I(CONF_rawcnp));
    ctrl_checkbox(s, "即使在 UTF-8 模式下也启用 VT100 线条绘制",'8',
                  HELPCTX(translation_utf8linedraw),
                  conf_checkbox_handler, I(CONF_utf8linedraw));

    /*
     * The Window/Selection panel.
     */
    ctrl_settitle(b, "窗口/选择", "复制粘贴设置");

    s = ctrl_getset(b, "窗口/选择", "mouse",
                    "鼠标使用设置");
    ctrl_checkbox(s, "按 Shift 时覆盖应用程序对鼠标的控制(P)", 'p',
                  HELPCTX(selection_shiftdrag),
                  conf_checkbox_handler, I(CONF_mouse_override));
    ctrl_radiobuttons(s,
                      "默认选择模式（按 Alt 拖动则使用另一种模式）：",
                      NO_SHORTCUT, 2,
                      HELPCTX(selection_rect),
                      conf_radiobutton_bool_handler,
                      I(CONF_rect_select),
                      "常规(N)", 'n', I(false),
                      "矩形框(R)", 'r', I(true));

    s = ctrl_getset(b, "窗口/选择", "clipboards",
                    "为复制／粘贴操作指定剪贴板");
    ctrl_checkbox(s, "自动复制选择的文本到："
                  CLIPNAME_EXPLICIT_OBJECT,
                  NO_SHORTCUT, HELPCTX(selection_autocopy),
                  conf_checkbox_handler, I(CONF_mouseautocopy));
    clipboard_control(s, "鼠标粘贴动作：", NO_SHORTCUT, 60,
                      HELPCTX(selection_clipactions),
                      CONF_mousepaste, CONF_mousepaste_custom);
    clipboard_control(s, "{Ctrl,Shift} + Ins:", NO_SHORTCUT, 60,
                      HELPCTX(selection_clipactions),
                      CONF_ctrlshiftins, CONF_ctrlshiftins_custom);
    clipboard_control(s, "Ctrl + Shift + {C,V}:", NO_SHORTCUT, 60,
                      HELPCTX(selection_clipactions),
                      CONF_ctrlshiftcv, CONF_ctrlshiftcv_custom);

    s = ctrl_getset(b, "窗口/选择", "paste",
                    "控制从剪贴板向终端粘贴文本");
    ctrl_checkbox(s, "允许粘贴文本中包含控制字符",
                  NO_SHORTCUT, HELPCTX(selection_pastectrl),
                  conf_checkbox_handler, I(CONF_paste_controls));

    /*
     * The Window/Selection/Copy panel.
     */
    ctrl_settitle(b, "窗口/选择/复制",
                  "从终端复制到剪贴板的设置");

    s = ctrl_getset(b, "窗口/选择/复制", "charclass",
                    "按类归组的字符类别");
    ccd = (struct charclass_data *)
        ctrl_alloc(b, sizeof(struct charclass_data));
    ccd->listbox = ctrl_listbox(s, "字符类别(E)：", 'e',
                                HELPCTX(copy_charclasses),
                                charclass_handler, P(ccd));
    ccd->listbox->listbox.multisel = 1;
    ccd->listbox->listbox.ncols = 4;
    ccd->listbox->listbox.percentages = snewn(4, int);
    ccd->listbox->listbox.percentages[0] = 15;
    ccd->listbox->listbox.percentages[1] = 25;
    ccd->listbox->listbox.percentages[2] = 20;
    ccd->listbox->listbox.percentages[3] = 40;
    ctrl_columns(s, 2, 67, 33);
    ccd->editbox = ctrl_editbox(s, "设为类别(T)：", 't', 50,
                                HELPCTX(copy_charclasses),
                                charclass_handler, P(ccd), P(NULL));
    ccd->editbox->column = 0;
    ccd->button = ctrl_pushbutton(s, "设置(S)", 's',
                                  HELPCTX(copy_charclasses),
                                  charclass_handler, P(ccd));
    ccd->button->column = 1;
    ctrl_columns(s, 1, 100);

    /*
     * The Window/Colours panel.
     */
    ctrl_settitle(b, "窗口/颜色", "颜色使用设置");

    s = ctrl_getset(b, "窗口/颜色", "general",
                    "颜色使用的通用选项");
    ctrl_checkbox(s, "允许终端指定 ANSI 颜色", 'i',
                  HELPCTX(colours_ansi),
                  conf_checkbox_handler, I(CONF_ansi_colour));
    ctrl_checkbox(s, "允许终端使用 xterm 256 色模式", '2',
                  HELPCTX(colours_xterm256), conf_checkbox_handler,
                  I(CONF_xterm_256_colour));
    ctrl_checkbox(s, "允许终端使用 24 位真彩色", '4',
                  HELPCTX(colours_truecolour), conf_checkbox_handler,
                  I(CONF_true_colour));
    ctrl_radiobuttons(s, "通过更改以下项显示粗体文字(B)：", 'b', 3,
                      HELPCTX(colours_bold),
                      conf_radiobutton_handler, I(CONF_bold_style),
                      "字体", I(BOLD_STYLE_FONT),
                      "颜色", I(BOLD_STYLE_COLOUR),
                      "两者", I(BOLD_STYLE_FONT | BOLD_STYLE_COLOUR));

    str = dupprintf("调整 %s 显示的精确颜色", appname);
    s = ctrl_getset(b, "窗口/颜色", "adjust", str);
    sfree(str);
    ctrl_text(s, "选择列表中的颜色，然后点击"
              "“修改”按钮改变其具体数值。",
              HELPCTX(colours_config));
    ctrl_columns(s, 2, 67, 33);
    cd = (struct colour_data *)ctrl_alloc(b, sizeof(struct colour_data));
    cd->listbox = ctrl_listbox(s, "选择颜色进行修改(U)：", 'u',
                               HELPCTX(colours_config), colour_handler, P(cd));
    cd->listbox->column = 0;
    cd->listbox->listbox.height = 7;
    c = ctrl_text(s, "RGB 值：", HELPCTX(colours_config));
    c->column = 1;
    cd->redit = ctrl_editbox(s, "红(R)", 'r', 50, HELPCTX(colours_config),
                             colour_handler, P(cd), P(NULL));
    cd->redit->column = 1;
    cd->gedit = ctrl_editbox(s, "绿(N)", 'n', 50, HELPCTX(colours_config),
                             colour_handler, P(cd), P(NULL));
    cd->gedit->column = 1;
    cd->bedit = ctrl_editbox(s, "蓝(E)", 'e', 50, HELPCTX(colours_config),
                             colour_handler, P(cd), P(NULL));
    cd->bedit->column = 1;
    cd->button = ctrl_pushbutton(s, "修改(M)", 'm', HELPCTX(colours_config),
                                 colour_handler, P(cd));
    cd->button->column = 1;
    ctrl_columns(s, 1, 100);

    /*
     * The Connection panel. This doesn't show up if we're in a
     * non-network utility such as pterm. We tell this by being
     * passed a protocol < 0.
     */
    if (protocol >= 0) {
        ctrl_settitle(b, "连接", "连接设置");

        s = ctrl_getset(b, "连接", "keepalive",
                        "通过发送空包保持会话活跃");
        ctrl_editbox(s, "空包发送时间间隔(秒，0 表示关闭)(K)：", 'k', 20,
                     HELPCTX(connection_keepalive),
                     conf_editbox_handler, I(CONF_ping_interval), ED_INT);

        if (!midsession) {
            s = ctrl_getset(b, "连接", "tcp",
                            "底层 TCP 连接选项");
            ctrl_checkbox(s, "禁用 Nagle 算法（TCP_NODELAY 选项）",
                          'n', HELPCTX(connection_nodelay),
                          conf_checkbox_handler,
                          I(CONF_tcp_nodelay));
            ctrl_checkbox(s, "启用 TCP 保活（SO_KEEPALIVE 选项）",
                          'p', HELPCTX(connection_tcpkeepalive),
                          conf_checkbox_handler,
                          I(CONF_tcp_keepalives));
#ifndef NO_IPV6
            s = ctrl_getset(b, "连接", "ipversion",
                            "IP 协议版本");
            ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                              HELPCTX(connection_ipversion),
                              conf_radiobutton_handler,
                              I(CONF_addressfamily),
                              "自动(U)", 'u', I(ADDRTYPE_UNSPEC),
                              "IPv4", '4', I(ADDRTYPE_IPV4),
                              "IPv6", '6', I(ADDRTYPE_IPV6));
#endif

            {
                const char *label = backend_vt_from_proto(PROT_SSH) ?
                    "远程主机的逻辑名称（例如用于查找 SSH 主机密钥）(M)：" :
                    "远程主机的逻辑名称(M)：";
                s = ctrl_getset(b, "连接", "identity",
                                "远程主机的逻辑名称");
                ctrl_editbox(s, label, 'm', 100,
                             HELPCTX(connection_loghost),
                             conf_editbox_handler, I(CONF_loghost), ED_STR);
            }

            s = ctrl_getset(b, "连接", "hooks",
                            "连接事件发生时运行的命令");
            ctrl_editbox(s, "连接前运行命令(B)", 'b', 100,
                         HELPCTX(connection_pre_hook),
                         conf_editbox_handler, I(CONF_pre_connect_command), ED_STR);
        }

        /*
         * A sub-panel Connection/Data, containing options that
         * decide on data to send to the server.
         */
        if (!midsession) {
            ctrl_settitle(b, "连接/数据", "传送到服务器的数据");

            s = ctrl_getset(b, "连接/数据", "login",
                            "登录信息");
            ctrl_editbox(s, "自动登录用户名(U)：", 'u', 50,
                         HELPCTX(connection_username),
                         conf_editbox_handler, I(CONF_username), ED_STR);
            {
                /* We assume the local username is sufficiently stable
                 * to include on the dialog box. */
                char *user = get_username();
                char *userlabel = dupprintf("使用系统用户名 (%s)",
                                            user ? user : "");
                sfree(user);
                ctrl_radiobuttons(s, "未指定用户名时(N)：", 'n', 4,
                                  HELPCTX(connection_username_from_env),
                                  conf_radiobutton_bool_handler,
                                  I(CONF_username_from_env),
                                  "提示", I(false),
                                  userlabel, I(true));
                sfree(userlabel);
            }

            s = ctrl_getset(b, "连接/数据", "term",
                            "终端信息");
            ctrl_editbox(s, "终端类型字符串(T)：", 't', 50,
                         HELPCTX(connection_termtype),
                         conf_editbox_handler, I(CONF_termtype), ED_STR);
            ctrl_editbox(s, "终端速度(S)：", 's', 50,
                         HELPCTX(connection_termspeed),
                         conf_editbox_handler, I(CONF_termspeed), ED_STR);

            s = ctrl_getset(b, "连接/数据", "env",
                            "环境变量");
            ctrl_columns(s, 2, 80, 20);
            ed = (struct environ_data *)
                ctrl_alloc(b, sizeof(struct environ_data));
            ed->varbox = ctrl_editbox(s, "变量(V)：", 'v', 60,
                                      HELPCTX(telnet_environ),
                                      environ_handler, P(ed), P(NULL));
            ed->varbox->column = 0;
            ed->valbox = ctrl_editbox(s, "值(L)：", 'l', 60,
                                      HELPCTX(telnet_environ),
                                      environ_handler, P(ed), P(NULL));
            ed->valbox->column = 0;
            ed->addbutton = ctrl_pushbutton(s, "添加(D)", 'd',
                                            HELPCTX(telnet_environ),
                                            environ_handler, P(ed));
            ed->addbutton->column = 1;
            ed->rembutton = ctrl_pushbutton(s, "删除(R)", 'r',
                                            HELPCTX(telnet_environ),
                                            environ_handler, P(ed));
            ed->rembutton->column = 1;
            ctrl_columns(s, 1, 100);
            ed->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                       HELPCTX(telnet_environ),
                                       environ_handler, P(ed));
            ed->listbox->listbox.height = 3;
            ed->listbox->listbox.ncols = 2;
            ed->listbox->listbox.percentages = snewn(2, int);
            ed->listbox->listbox.percentages[0] = 30;
            ed->listbox->listbox.percentages[1] = 70;
        }

    }

    if (!midsession) {
        /*
         * The Connection/Proxy panel.
         */
        ctrl_settitle(b, "连接/代理",
                      "代理使用设置");

        s = ctrl_getset(b, "连接/代理", "basics", NULL);
        c = ctrl_droplist(s, "代理类型(T)：", 't', 70,
                          HELPCTX(proxy_type), proxy_type_handler, I(0));
        ctrl_columns(s, 2, 80, 20);
        c = ctrl_editbox(s, "代理主机名(Y)", 'y', 100,
                         HELPCTX(proxy_main),
                         conf_editbox_handler,
                         I(CONF_proxy_host), ED_STR);
        c->column = 0;
        c = ctrl_editbox(s, "端口(P)", 'p', 100,
                         HELPCTX(proxy_main),
                         conf_editbox_handler,
                         I(CONF_proxy_port),
                         ED_INT);
        c->column = 1;
        ctrl_columns(s, 1, 100);
        ctrl_editbox(s, "排除的主机／IP(E)", 'e', 100,
                     HELPCTX(proxy_exclude),
                     conf_editbox_handler,
                     I(CONF_proxy_exclude_list), ED_STR);
        ctrl_checkbox(s, "本地连接也考虑使用代理(X)", 'x',
                      HELPCTX(proxy_exclude),
                      conf_checkbox_handler,
                      I(CONF_even_proxy_localhost));
        ctrl_radiobuttons(s, "在代理端进行 DNS 域名解析：", 'd', 3,
                          HELPCTX(proxy_dns),
                          conf_radiobutton_handler,
                          I(CONF_proxy_dns),
                          "否", I(FORCE_OFF),
                          "自动", I(AUTO),
                          "是", I(FORCE_ON));
        ctrl_editbox(s, "用户名(U)：", 'u', 60,
                     HELPCTX(proxy_auth),
                     conf_editbox_handler,
                     I(CONF_proxy_username), ED_STR);
        c = ctrl_editbox(s, "密码(W)：", 'w', 60,
                         HELPCTX(proxy_auth),
                         conf_editbox_handler,
                         I(CONF_proxy_password), ED_STR);
        c->editbox.password = true;
        ctrl_editbox(s, "发送给代理的命令（仅部分代理类型）(M)", 'm', 100,
                     HELPCTX(proxy_command),
                     conf_editbox_handler,
                     I(CONF_proxy_telnet_command), ED_STR);

        ctrl_radiobuttons(s, "在终端窗口"
                          "输出代理诊断信息(R)", 'r', 5,
                          HELPCTX(proxy_logging),
                          conf_radiobutton_handler,
                          I(CONF_proxy_log_to_term),
                          "否", I(FORCE_OFF),
                          "是", I(FORCE_ON),
                          "仅限会话开始前", I(AUTO));
    }

    /*
     * Each per-protocol configuration GUI panel is conditionally
     * displayed. We don't display it if this binary doesn't contain a
     * backend for its protocol at all; we don't display it if we're
     * already in mid-session with a different protocol selected; and
     * even if we _do_ have this protocol selected, we don't display
     * the panel if the protocol doesn't permit any mid-session
     * reconfiguration anyway.
     */

#define DISPLAY_RECONFIGURABLE_PROTOCOL(which_proto) \
    (backend_vt_from_proto(which_proto) && \
     (!midsession || protocol == (which_proto)))
#define DISPLAY_NON_RECONFIGURABLE_PROTOCOL(which_proto) \
    (backend_vt_from_proto(which_proto) && !midsession)

    if (DISPLAY_RECONFIGURABLE_PROTOCOL(PROT_SSH) ||
        DISPLAY_RECONFIGURABLE_PROTOCOL(PROT_SSHCONN)) {
        /*
         * The Connection/SSH panel.
         */
        ctrl_settitle(b, "连接/SSH",
                      "SSH 连接设置");

        /* SSH-1 or connection-sharing downstream */
        if (midsession && (protcfginfo == 1 || protcfginfo == -1)) {
            s = ctrl_getset(b, "连接/SSH", "disclaimer", NULL);
            ctrl_text(s, "此面板上的任何设置都不能在会话进行中更改；"
                      "保留此面板只是为了让子面板的存在不显得突兀",
                      HELPCTX(no_help));
        }

        if (!midsession) {

            s = ctrl_getset(b, "连接/SSH", "data",
                            "传送到服务器的数据");
            ctrl_editbox(s, "远程命令(R)：", 'r', 100,
                         HELPCTX(ssh_command),
                         conf_editbox_handler, I(CONF_remote_cmd), ED_STR);

            s = ctrl_getset(b, "连接/SSH", "protocol", "协议选项");
            ctrl_checkbox(s, "完全不运行 Shell 或命令(N)", 'n',
                          HELPCTX(ssh_noshell),
                          conf_checkbox_handler,
                          I(CONF_ssh_no_shell));
        }

        if (!midsession || !(protcfginfo == 1 || protcfginfo == -1)) {
            s = ctrl_getset(b, "连接/SSH", "protocol", "协议选项");

            ctrl_checkbox(s, "启用压缩(E)", 'e',
                          HELPCTX(ssh_compress),
                          conf_checkbox_handler,
                          I(CONF_compression));
        }

        if (!midsession) {
            s = ctrl_getset(b, "连接/SSH", "sharing", "在 PuTTY 工具之间共享 SSH 连接");

            ctrl_checkbox(s, "共享可用的 SSH 连接", 's',
                          HELPCTX(ssh_share),
                          conf_checkbox_handler,
                          I(CONF_ssh_connection_sharing));

            ctrl_text(s, "共享连接中允许的角色：",
                      HELPCTX(ssh_share));
            ctrl_checkbox(s, "上游（连接真实服务器）(U)", 'u',
                          HELPCTX(ssh_share),
                          conf_checkbox_handler,
                          I(CONF_ssh_connection_sharing_upstream));
            ctrl_checkbox(s, "下游（连接上游 PuTTY）(D)", 'd',
                          HELPCTX(ssh_share),
                          conf_checkbox_handler,
                          I(CONF_ssh_connection_sharing_downstream));
        }

        if (!midsession) {
            s = ctrl_getset(b, "连接/SSH", "protocol", "协议选项");

            ctrl_radiobuttons(s, "首选的 SSH 协议版本：", NO_SHORTCUT, 2,
                              HELPCTX(ssh_protocol),
                              conf_radiobutton_handler,
                              I(CONF_sshprot),
                              "2", '2', I(3),
                              "1 (不安全)", '1', I(0));
        }

        /*
         * The Connection/SSH/Kex panel. (Owing to repeat key
         * exchange, much of this is meaningful in mid-session _if_
         * we're using SSH-2 and are not a connection-sharing
         * downstream, or haven't decided yet.)
         */
        if (protcfginfo != 1 && protcfginfo != -1) {
            ctrl_settitle(b, "连接/SSH/密钥",
                          "SSH 密钥交换设置");

            s = ctrl_getset(b, "连接/SSH/密钥", "main",
                            "密钥交换算法选项");
            c = ctrl_draglist(s, "算法优先级(S)：", 's',
                              HELPCTX(ssh_kexlist),
                              kexlist_handler, P(NULL));
            c->listbox.height = 10;
#ifndef NO_GSSAPI
            ctrl_checkbox(s, "尝试 GSSAPI 密钥交换(K)",
                          'k', HELPCTX(ssh_gssapi),
                          conf_checkbox_handler,
                          I(CONF_try_gssapi_kex));
#endif

            s = ctrl_getset(b, "连接/SSH/密钥", "repeat",
                            "密钥重新交换选项");

            ctrl_editbox(s, "触发重新交换密钥前的最大时长（分钟，0 表示不限制）(T)：", 't', 20,
                         HELPCTX(ssh_kex_repeat),
                         conf_editbox_handler,
                         I(CONF_ssh_rekey_time),
                         ED_INT);
#ifndef NO_GSSAPI
            ctrl_editbox(s, "GSS 检查间隔（分钟，0 表示不检查）", NO_SHORTCUT, 20,
                         HELPCTX(ssh_kex_repeat),
                         conf_editbox_handler,
                         I(CONF_gssapirekey),
                         ED_INT);
#endif
            ctrl_editbox(s, "触发重新交换密钥的最大数据量（0 表示不限制）(X)：", 'x', 20,
                         HELPCTX(ssh_kex_repeat),
                         conf_editbox_handler,
                         I(CONF_ssh_rekey_data),
                         ED_STR);
            ctrl_text(s, "(用 1M 表示 1 兆字节，1G 表示 1 吉字节，依此类推)",
                      HELPCTX(ssh_kex_repeat));
        }

        /*
         * The 'Connection/SSH/Host keys' panel.
         */
        if (protcfginfo != 1 && protcfginfo != -1) {
            ctrl_settitle(b, "连接/SSH/主机密钥",
                          "SSH 主机密钥设置");

            s = ctrl_getset(b, "连接/SSH/主机密钥", "main",
                            "主机密钥算法优先顺序");
            c = ctrl_draglist(s, "算法优先级(S)：", 's',
                              HELPCTX(ssh_hklist),
                              hklist_handler, P(NULL));
            c->listbox.height = 5;

            ctrl_checkbox(s, "已知主机密钥的首选算法(P)",
                          'p', HELPCTX(ssh_hk_known), conf_checkbox_handler,
                          I(CONF_ssh_prefer_known_hostkeys));
        }

        /*
         * Manual host key configuration is irrelevant mid-session,
         * as we enforce that the host key for rekeys is the
         * same as that used at the start of the session.
         */
        if (!midsession) {
            s = ctrl_getset(b, "连接/SSH/主机密钥", "hostkeys",
                            "为本连接手动配置主机密钥");

            ctrl_columns(s, 2, 75, 25);
            c = ctrl_text(s, "可接受的主机密钥或指纹：",
                          HELPCTX(ssh_kex_manual_hostkeys));
            c->column = 0;
            /* You want to select from the list, _then_ hit Remove. So
             * tab order should be that way round. */
            mh = (struct manual_hostkey_data *)
                ctrl_alloc(b,sizeof(struct manual_hostkey_data));
            mh->rembutton = ctrl_pushbutton(s, "删除(R)", 'r',
                                            HELPCTX(ssh_kex_manual_hostkeys),
                                            manual_hostkey_handler, P(mh));
            mh->rembutton->column = 1;
            mh->rembutton->delay_taborder = true;
            mh->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                       HELPCTX(ssh_kex_manual_hostkeys),
                                       manual_hostkey_handler, P(mh));
            /* This list box can't be very tall, because there's not
             * much room in the pane on Windows at least. This makes
             * it become really unhelpful if a horizontal scrollbar
             * appears, so we suppress that. */
            mh->listbox->listbox.height = 2;
            mh->listbox->listbox.hscroll = false;
            ctrl_tabdelay(s, mh->rembutton);
            mh->keybox = ctrl_editbox(s, "密钥(K)", 'k', 80,
                                      HELPCTX(ssh_kex_manual_hostkeys),
                                      manual_hostkey_handler, P(mh), P(NULL));
            mh->keybox->column = 0;
            mh->addbutton = ctrl_pushbutton(s, "添加密钥(Y)", 'y',
                                            HELPCTX(ssh_kex_manual_hostkeys),
                                            manual_hostkey_handler, P(mh));
            mh->addbutton->column = 1;
            ctrl_columns(s, 1, 100);
        }

        /*
         * But there's no reason not to forbid access to the host CA
         * configuration box, which is common across sessions in any
         * case.
         */
        s = ctrl_getset(b, "连接/SSH/主机密钥", "ca",
                        "配置受信任的证书颁发机构");
        c = ctrl_pushbutton(s, "配置主机 CA", NO_SHORTCUT,
                            HELPCTX(ssh_kex_cert),
                            host_ca_button_handler, I(0));

        if (!midsession || !(protcfginfo == 1 || protcfginfo == -1)) {
            /*
             * The Connection/SSH/Cipher panel.
             */
            ctrl_settitle(b, "连接/SSH/加密",
                          "SSH 加密设置");

            s = ctrl_getset(b, "连接/SSH/加密",
                            "encryption", "加密选项");
            c = ctrl_draglist(s, "加密算法优先级(S)：", 's',
                              HELPCTX(ssh_ciphers),
                              cipherlist_handler, P(NULL));
            c->listbox.height = 6;

            ctrl_checkbox(s, "在 SSH-2 中启用旧版单重 DES(I)", 'i',
                          HELPCTX(ssh_ciphers),
                          conf_checkbox_handler,
                          I(CONF_ssh2_des_cbc));
        }

        if (!midsession) {

            /*
             * The Connection/SSH/Auth panel.
             */
            ctrl_settitle(b, "连接/SSH/认证",
                          "SSH 认证设置");

            s = ctrl_getset(b, "连接/SSH/认证", "main", NULL);
            ctrl_checkbox(s, "显示认证前通告（仅 SSH-2）(D)",
                          'd', HELPCTX(ssh_auth_banner),
                          conf_checkbox_handler,
                          I(CONF_ssh_show_banner));
            ctrl_checkbox(s, "完全跳过认证（仅 SSH-2）(B)", 'b',
                          HELPCTX(ssh_auth_bypass),
                          conf_checkbox_handler,
                          I(CONF_ssh_no_userauth));
            ctrl_checkbox(s, "认证未经实质校验即成功时断开连接(N)",
                          'n', HELPCTX(ssh_no_trivial_userauth),
                          conf_checkbox_handler,
                          I(CONF_ssh_no_trivial_userauth));

            s = ctrl_getset(b, "连接/SSH/认证", "methods",
                            "认证方式");
            ctrl_checkbox(s, "尝试使用 Pageant 认证", 'p',
                          HELPCTX(ssh_auth_pageant),
                          conf_checkbox_handler,
                          I(CONF_tryagent));
            ctrl_checkbox(s, "尝试使用 TIS 或 CryptoCard 认证（SSH-1）(M)", 'm',
                          HELPCTX(ssh_auth_tis),
                          conf_checkbox_handler,
                          I(CONF_try_tis_auth));
            ctrl_checkbox(s, "尝试使用 keyboard-interactive（键盘交互）认证（SSH-2）(I)",
                          'i', HELPCTX(ssh_auth_ki),
                          conf_checkbox_handler,
                          I(CONF_try_ki_auth));

            s = ctrl_getset(b, "连接/SSH/认证", "aux",
                            "其他认证相关选项");
            ctrl_checkbox(s, "允许密钥代理转发(F)", 'f',
                          HELPCTX(ssh_auth_agentfwd),
                          conf_checkbox_handler, I(CONF_agentfwd));
            ctrl_checkbox(s, "允许在 SSH-2 中尝试更改用户名", NO_SHORTCUT,
                          HELPCTX(ssh_auth_changeuser),
                          conf_checkbox_handler,
                          I(CONF_change_username));

            ctrl_settitle(b, "连接/SSH/认证/证书",
                          "用于认证的凭据");

            s = ctrl_getset(b, "连接/SSH/认证/证书", "publickey",
                            "公钥认证");
            /*
             * 顺序：证书框在上，私钥框在下。
             *
             * 只是显示次序的调整，两者的 CONF 绑定、快捷键字母都不动：
             * 私钥仍是 (K)、证书仍是 (E)，导入按钮的 context2 仍指私钥框
             * （转换结果回填到私钥，不是证书）。控件顺序与配置语义无关，
             * 但 keyfilesel 必须在私钥那一次 ctrl_filesel 之后才拿到句柄，
             * 所以私钥这次调用要保留在按钮之前。
             */
            ctrl_filesel(s, "与私钥一起使用的证书（可选）(E)：", 'e',
                         FILTER_ALL_FILES, false, "选择证书文件",
                         HELPCTX(ssh_auth_cert),
                         conf_filesel_handler, I(CONF_detached_cert));
            dlgcontrol *keyfilesel =
                ctrl_filesel(s, "认证私钥文件(K)：", 'k',
                             FILTER_KEY_FILES, false, "选择私钥文件",
                             HELPCTX(ssh_auth_privkey),
                             cn_keyfile_handler, I(CONF_keyfile));
            dlgcontrol *importbutton =
                ctrl_pushbutton(s, "导入并转换密钥(I)...", 'i',
                                HELPCTX(no_help),
                                keyimport_button_handler, I(0));
            /* 按钮回调据此把转换结果回填到上面的私钥框 */
            importbutton->context2 = P(keyfilesel);

            /*
             * 导入按钮下方的格式说明。
             *
             * 每行一个 ctrl_text，不用 \n 或自动换行：staticwrap()
             * 把 _GetTextExtentExPoint() 返回的字符数当字节偏移用，
             * 只在 ACP==UTF-8 时才做字节修正（见 windows/controls.c
             * 的 _GetTextExtentExPoint 包装）。简体中文系统 ACP 是
             * 936，修正分支不生效，含中文的自动换行会切在字符中间。
             * 分行写就不依赖那段逻辑。
             *
             * 内容必须与 keyimport.c 的 importable_formats 一致：
             * 转换器实际只吃 OpenSSH 新旧两种和 ssh.com 三种。
             */
            ctrl_text(s, "支持 OpenSSH 私钥（新旧格式）与 ssh.com 私钥。",
                      HELPCTX(no_help));
            ctrl_text(s, "转换后将在同一目录生成 .ppk 文件，并自动填入上方的私钥文件框。",
                      HELPCTX(no_help));

            s = ctrl_getset(b, "连接/SSH/认证/证书", "plugin",
                            "提供认证响应的插件");
            ctrl_editbox(s, "要运行的插件命令：", NO_SHORTCUT, 100,
                         HELPCTX(ssh_auth_plugin),
                         conf_editbox_handler, I(CONF_auth_plugin), ED_STR);
#ifndef NO_GSSAPI
            /*
             * Connection/SSH/Auth/GSSAPI, which sadly won't fit on
             * the main Auth panel.
             */
            ctrl_settitle(b, "连接/SSH/认证/GSSAPI",
                          "GSSAPI 认证设置");
            s = ctrl_getset(b, "连接/SSH/认证/GSSAPI", "gssapi", NULL);

            ctrl_checkbox(s, "尝试使用 GSSAPI 认证（仅 SSH-2）(T)",
                          't', HELPCTX(ssh_gssapi),
                          conf_checkbox_handler,
                          I(CONF_try_gssapi_auth));

            ctrl_checkbox(s, "尝试使用 GSSAPI 密钥交换（仅 SSH-2）(K)",
                          'k', HELPCTX(ssh_gssapi),
                          conf_checkbox_handler,
                          I(CONF_try_gssapi_kex));

            ctrl_checkbox(s, "允许 GSSAPI 凭据委托(L)", 'l',
                          HELPCTX(ssh_gssapi_delegation),
                          conf_checkbox_handler,
                          I(CONF_gssapifwd));

            /*
             * GSSAPI library selection.
             */
            if (ngsslibs > 1) {
                c = ctrl_draglist(s, "GSSAPI 库优先级：",
                                  'p', HELPCTX(ssh_gssapi_libraries),
                                  gsslist_handler, P(NULL));
                c->listbox.height = ngsslibs;

                /*
                 * I currently assume that if more than one GSS
                 * library option is available, then one of them is
                 * 'user-supplied' and so we should present the
                 * following file selector. This is at least half-
                 * reasonable, because if we're using statically
                 * linked GSSAPI then there will only be one option
                 * and no way to load from a user-supplied library,
                 * whereas if we're using dynamic libraries then
                 * there will almost certainly be some default
                 * option in addition to a user-supplied path. If
                 * anyone ever ports PuTTY to a system on which
                 * dynamic-library GSSAPI is available but there is
                 * absolutely no consensus on where to keep the
                 * libraries, there'll need to be a flag alongside
                 * ngsslibs to control whether the file selector is
                 * displayed.
                 */

                ctrl_filesel(s, "用户支持的 GSSAPI 库路径：", 's',
                             FILTER_DYNLIB_FILES, false, "选择库文件",
                             HELPCTX(ssh_gssapi_libraries),
                             conf_filesel_handler,
                             I(CONF_ssh_gss_custom));
            }
#endif
        }

        if (!midsession) {
            /*
             * The Connection/SSH/TTY panel.
             */
            ctrl_settitle(b, "连接/SSH/TTY", "远程终端设置");

            s = ctrl_getset(b, "连接/SSH/TTY", "sshtty", NULL);
            ctrl_checkbox(s, "不分配伪终端(P)", 'p',
                          HELPCTX(ssh_nopty),
                          conf_checkbox_handler,
                          I(CONF_nopty));

            s = ctrl_getset(b, "连接/SSH/TTY", "ttymodes",
                            "终端模式");
            td = (struct ttymodes_data *)
                ctrl_alloc(b, sizeof(struct ttymodes_data));
            ctrl_text(s, "要发送的终端模式：", HELPCTX(ssh_ttymodes));
            td->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                       HELPCTX(ssh_ttymodes),
                                       ttymodes_handler, P(td));
            td->listbox->listbox.height = 8;
            td->listbox->listbox.ncols = 2;
            td->listbox->listbox.percentages = snewn(2, int);
            td->listbox->listbox.percentages[0] = 40;
            td->listbox->listbox.percentages[1] = 60;
            ctrl_columns(s, 2, 75, 25);
            c = ctrl_text(s, "为所选模式发送：", HELPCTX(ssh_ttymodes));
            c->column = 0;
            td->setbutton = ctrl_pushbutton(s, "设置(S)", 's',
                                            HELPCTX(ssh_ttymodes),
                                            ttymodes_handler, P(td));
            td->setbutton->column = 1;
            td->setbutton->delay_taborder = true;
            ctrl_columns(s, 1, 100);        /* column break */
            /* Bit of a hack to get the value radio buttons and
             * edit-box on the same row. */
            ctrl_columns(s, 2, 75, 25);
            td->valradio = ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                                             HELPCTX(ssh_ttymodes),
                                             ttymodes_handler, P(td),
                                             "自动", NO_SHORTCUT, P(NULL),
                                             "无", NO_SHORTCUT, P(NULL),
                                             "指定：", NO_SHORTCUT, P(NULL));
            td->valradio->column = 0;
            td->valbox = ctrl_editbox(s, NULL, NO_SHORTCUT, 100,
                                      HELPCTX(ssh_ttymodes),
                                      ttymodes_handler, P(td), P(NULL));
            td->valbox->column = 1;
            td->valbox->align_next_to = td->valradio;
            ctrl_tabdelay(s, td->setbutton);
        }

        if (!midsession) {
            /*
             * The Connection/SSH/X11 panel.
             */
            ctrl_settitle(b, "连接/SSH/X11",
                          "SSH X11 转发设置");

            s = ctrl_getset(b, "连接/SSH/X11", "x11", "X11 转发");
            ctrl_checkbox(s, "启用 X11 转发(E)", 'e',
                          HELPCTX(ssh_tunnels_x11),
                          conf_checkbox_handler,I(CONF_x11_forward));
            ctrl_editbox(s, "X 显示位置（DISPLAY）：", 'x', 50,
                         HELPCTX(ssh_tunnels_x11),
                         conf_editbox_handler, I(CONF_x11_display), ED_STR);
            ctrl_radiobuttons(s, "远程 X11 认证协议(U)", 'u', 2,
                              HELPCTX(ssh_tunnels_x11auth),
                              conf_radiobutton_handler,
                              I(CONF_x11_auth),
                              "MIT-Magic-Cookie-1", I(X11_MIT),
                              "XDM-Authorization-1", I(X11_XDM));
        }

        /*
         * The Tunnels panel _is_ still available in mid-session.
         */
        ctrl_settitle(b, "连接/SSH/隧道",
                      "SSH 端口转发设置");

        s = ctrl_getset(b, "连接/SSH/隧道", "portfwd",
                        "端口转发");
        ctrl_checkbox(s, "本地端口接受其他主机的连接(T)",'t',
                      HELPCTX(ssh_tunnels_portfwd_localhost),
                      conf_checkbox_handler,
                      I(CONF_lport_acceptall));
        ctrl_checkbox(s, "远程端口同样接受连接（仅 SSH-2）(P)", 'p',
                      HELPCTX(ssh_tunnels_portfwd_localhost),
                      conf_checkbox_handler,
                      I(CONF_rport_acceptall));

        ctrl_columns(s, 3, 55, 20, 25);
        c = ctrl_text(s, "转发端口：", HELPCTX(ssh_tunnels_portfwd));
        c->column = COLUMN_FIELD(0,2);
        /* You want to select from the list, _then_ hit Remove. So tab order
         * should be that way round. */
        pfd = (struct portfwd_data *)ctrl_alloc(b,sizeof(struct portfwd_data));
        pfd->rembutton = ctrl_pushbutton(s, "删除(R)", 'r',
                                         HELPCTX(ssh_tunnels_portfwd),
                                         portfwd_handler, P(pfd));
        pfd->rembutton->column = 2;
        pfd->rembutton->delay_taborder = true;
        pfd->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                    HELPCTX(ssh_tunnels_portfwd),
                                    portfwd_handler, P(pfd));
        pfd->listbox->listbox.height = 3;
        pfd->listbox->listbox.ncols = 2;
        pfd->listbox->listbox.percentages = snewn(2, int);
        pfd->listbox->listbox.percentages[0] = 20;
        pfd->listbox->listbox.percentages[1] = 80;
        ctrl_tabdelay(s, pfd->rembutton);
        ctrl_text(s, "添加新的转发端口：", HELPCTX(ssh_tunnels_portfwd));
        /* You want to enter source, destination and type, _then_ hit Add.
         * Again, we adjust the tab order to reflect this. */
        pfd->addbutton = ctrl_pushbutton(s, "添加(D)", 'd',
                                         HELPCTX(ssh_tunnels_portfwd),
                                         portfwd_handler, P(pfd));
        pfd->addbutton->column = 2;
        pfd->addbutton->delay_taborder = true;
        pfd->sourcebox = ctrl_editbox(s, "源端口(S)：", 's', 40,
                                      HELPCTX(ssh_tunnels_portfwd),
                                      portfwd_handler, P(pfd), P(NULL));
        pfd->sourcebox->column = 0;
        pfd->destbox = ctrl_editbox(s, "目标地址(I)", 'i', 67,
                                    HELPCTX(ssh_tunnels_portfwd),
                                    portfwd_handler, P(pfd), P(NULL));
        pfd->direction = ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                                           HELPCTX(ssh_tunnels_portfwd),
                                           portfwd_handler, P(pfd),
                                           "本地(L)", 'l', P(NULL),
                                           "远程(M)", 'm', P(NULL),
                                           "动态(Y)", 'y', P(NULL));
#ifndef NO_IPV6
        pfd->addressfamily =
            ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                              HELPCTX(ssh_tunnels_portfwd_ipversion),
                              portfwd_handler, P(pfd),
                              "自动(U)", 'u', I(ADDRTYPE_UNSPEC),
                              "IPv4", '4', I(ADDRTYPE_IPV4),
                              "IPv6", '6', I(ADDRTYPE_IPV6));
#endif
        ctrl_tabdelay(s, pfd->addbutton);
        ctrl_columns(s, 1, 100);

        if (!midsession) {
            /*
             * The Connection/SSH/Bugs panels.
             */
            ctrl_settitle(b, "连接/SSH/错误兼容",
                          "SSH 服务器缺陷兼容设置");

            s = ctrl_getset(b, "连接/SSH/错误兼容", "main",
                            "检测 SSH 服务端中的已知缺陷");
            ctrl_droplist(s, "无法处理 SSH-2 ignore 消息", '2', 20,
                          HELPCTX(ssh_bugs_ignore2),
                          sshbug_handler, I(CONF_sshbug_ignore2));
            ctrl_droplist(s, "无法正确处理 SSH-2 密钥重新交换(K)", 'k', 20,
                          HELPCTX(ssh_bugs_rekey2),
                          sshbug_handler, I(CONF_sshbug_rekey2));
            ctrl_droplist(s, "无法处理 PuTTY 的 SSH-2 'winadj' 请求", 'j',
                          20, HELPCTX(ssh_bugs_winadj),
                          sshbug_handler, I(CONF_sshbug_winadj));
            ctrl_droplist(s, "仍回复已关闭通道上的请求(Q)", 'q', 20,
                          HELPCTX(ssh_bugs_chanreq),
                          sshbug_handler, I(CONF_sshbug_chanreq));
            ctrl_droplist(s, "忽略 SSH-2 最大数据包长度(X)", 'x', 20,
                          HELPCTX(ssh_bugs_maxpkt2),
                          sshbug_handler, I(CONF_sshbug_maxpkt2));

            s = ctrl_getset(b, "连接/SSH/错误兼容", "manual",
                            "手动启用的兼容性处理");
            ctrl_droplist(s, "丢弃在问候语之前发送的数据(D)", 'd', 20,
                          HELPCTX(ssh_bugs_dropstart),
                          sshbug_handler_manual_only,
                          I(CONF_sshbug_dropstart));
            ctrl_droplist(s, "无法处理 PuTTY 的完整 KEXINIT", 'p', 20,
                          HELPCTX(ssh_bugs_filter_kexinit),
                          sshbug_handler_manual_only,
                          I(CONF_sshbug_filter_kexinit));

            ctrl_settitle(b, "连接/SSH/更多错误兼容",
                          "更多 SSH 服务器缺陷兼容设置");

            s = ctrl_getset(b, "连接/SSH/更多错误兼容", "main",
                            "检测 SSH 服务端中的已知缺陷");
            ctrl_droplist(s, "旧的 RSA/SHA2 证书算法命名(L)", 'l', 20,
                          HELPCTX(ssh_bugs_rsa_sha2_cert_userauth),
                          sshbug_handler,
                          I(CONF_sshbug_rsa_sha2_cert_userauth));
            ctrl_droplist(s, "需要对 SSH-2 RSA 签名进行填充(P)", 'p', 20,
                          HELPCTX(ssh_bugs_rsapad2),
                          sshbug_handler, I(CONF_sshbug_rsapad2));
            ctrl_droplist(s, "只支持 pre-RFC4419 SSH-2 DH GEX", 'd', 20,
                          HELPCTX(ssh_bugs_oldgex2),
                          sshbug_handler, I(CONF_sshbug_oldgex2));
            ctrl_droplist(s, "SSH-2 HMAC 密钥计算有误(M)", 'm', 20,
                          HELPCTX(ssh_bugs_hmac2),
                          sshbug_handler, I(CONF_sshbug_hmac2));
            ctrl_droplist(s, "SSH-2 公钥认证中误用会话 ID(N)", 'n', 20,
                          HELPCTX(ssh_bugs_pksessid2),
                          sshbug_handler, I(CONF_sshbug_pksessid2));
            ctrl_droplist(s, "SSH-2 加密密钥计算有误(E)", 'e', 20,
                          HELPCTX(ssh_bugs_derivekey2),
                          sshbug_handler, I(CONF_sshbug_derivekey2));
            ctrl_droplist(s, "无法处理 SSH-1 ignore 消息(I)", 'i', 20,
                          HELPCTX(ssh_bugs_ignore1),
                          sshbug_handler, I(CONF_sshbug_ignore1));
            ctrl_droplist(s, "拒绝所有 SSH-1 明文密码认证(S)", 's', 20,
                          HELPCTX(ssh_bugs_plainpw1),
                          sshbug_handler, I(CONF_sshbug_plainpw1));
            ctrl_droplist(s, "无法处理 SSH-1 RSA 认证", 'r', 20,
                          HELPCTX(ssh_bugs_rsa1),
                          sshbug_handler, I(CONF_sshbug_rsa1));
        }
    }

    if (DISPLAY_RECONFIGURABLE_PROTOCOL(PROT_SERIAL)) {
        const BackendVtable *ser_vt = backend_vt_from_proto(PROT_SERIAL);

        /*
         * The Connection/Serial panel.
         */
        ctrl_settitle(b, "连接/串口",
                      "本地串口设置");

        /*
         * 串口页顶部：选一个厂商标准，下面的五项参数自动填好。
         */
        s = ctrl_getset(b, "连接/串口", "serstd", "选择串口标准");
        serial_ui.standard =
            ctrl_droplist(s, "厂商标准(A)", 'a', 40,
                          HELPCTX(serial_line),
                          serial_standard_handler, I(0));

        s = ctrl_getset(b, "连接/串口", "sercfg", "配置串口");
        serial_ui.speed = ctrl_editbox(s, "波特率(S)", 's', 40,
                                       HELPCTX(serial_speed),
                                       serial_editbox_handler,
                                       I(CONF_serspeed), ED_INT);
        serial_ui.databits = ctrl_editbox(s, "数据位(B)", 'b', 40,
                                          HELPCTX(serial_databits),
                                          serial_editbox_handler,
                                          I(CONF_serdatabits), ED_INT);
        /*
         * Stop bits come in units of one half.
         */
        static const struct conf_editbox_handler_type conf_editbox_stopbits = {
            .type = EDIT_FIXEDPOINT, .denominator = 2};

        serial_ui.stopbits = ctrl_editbox(s, "停止位(T)", 't', 40,
                                          HELPCTX(serial_stopbits),
                                          serial_editbox_handler,
                                          I(CONF_serstopbits),
                                          CP(&conf_editbox_stopbits));
        serial_ui.parity = ctrl_droplist(s, "奇偶校验位(P)", 'p', 40,
                                         HELPCTX(serial_parity),
                                         serial_parity_std_handler,
                                         I(ser_vt->serial_parity_mask));
        serial_ui.flow = ctrl_droplist(s, "流控制(F)", 'f', 40,
                                       HELPCTX(serial_flow),
                                       serial_flow_std_handler,
                                       I(ser_vt->serial_flow_mask));
    }

    if (DISPLAY_RECONFIGURABLE_PROTOCOL(PROT_TELNET)) {
        /*
         * The Connection/Telnet panel.
         */
        ctrl_settitle(b, "连接/Telnet",
                      "Telnet 连接设置");

        s = ctrl_getset(b, "连接/Telnet", "protocol",
                        "Telnet 协议调整");

        if (!midsession) {
            ctrl_radiobuttons(s, "OLD_ENVIRON 歧义处理：",
                              NO_SHORTCUT, 2,
                              HELPCTX(telnet_oldenviron),
                              conf_radiobutton_bool_handler,
                              I(CONF_rfc_environ),
                              "BSD (常用)", 'b', I(false),
                              "RFC 1408 (不常用)", 'f', I(true));
            ctrl_radiobuttons(s, "Telnet 协商模式：", 't', 2,
                              HELPCTX(telnet_passive),
                              conf_radiobutton_bool_handler,
                              I(CONF_passive_telnet),
                              "被动", I(true), "主动", I(false));
        }
        ctrl_checkbox(s, "键盘发送 Telnet 特殊命令(K)", 'k',
                      HELPCTX(telnet_specialkeys),
                      conf_checkbox_handler,
                      I(CONF_telnet_keyboard));
        ctrl_checkbox(s, "回车键发送 Telnet 新行命令而非 ^M",
                      'm', HELPCTX(telnet_newline),
                      conf_checkbox_handler,
                      I(CONF_telnet_newline));
    }
}
