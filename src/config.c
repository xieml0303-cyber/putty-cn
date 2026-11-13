/*
 * config.c - the platform-independent parts of the PuTTY
 * configuration box.
 */

#include <assert.h>
#include <stdlib.h>

#ifdef _WIN32
#include <shlobj.h>
#ifndef CSIDL_DESKTOPDIRECTORY
#define CSIDL_DESKTOPDIRECTORY 0x0010
#endif
#endif

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

/*
 * cn: 把串口参数强制重置为默认值（2026-11-09，两次修订）。
 *
 * 背景：「连接/串口」页面已删除，界面上只留波特率一项，其余四项按现行
 * 默认配置走（8 数据位、1 停止位、无校验、无流控）。但存量配置文件里
 * 可能存着用户早年手改过的非默认值 —— 页面没了就再也改不回来，程序
 * 却仍照着那些值去连，形成用户看不见也修不了的暗坑。所以每次打开配置
 * 框时把这四项归位。
 *
 * 第二次修订（主人 2026-11-09 追加）：波特率也一并归到 9600。波特率
 * 从此是「本次窗口的临时值」—— 打开配置框固定 9600，改了不进改动清单、
 * 不点亮保存按钮、保存时不落盘（见 cn_persist_default_settings 与
 * conf_data.c 的 cn_is_connection_target），关闭窗口即失效。
 *
 * 只动这几项，不碰 serline（串口名）。调用点必须在 baseline 快照之前，
 * 否则归位动作会被当成「用户改动」标脏。
 */
void cn_serial_params_reset(Conf *conf)
{
    conf_set_int(conf, CONF_serdatabits, 8);
    /* 停止位以半个位为单位，1 个停止位记作 2。 */
    conf_set_int(conf, CONF_serstopbits, 2);
    conf_set_int(conf, CONF_serparity, SER_PAR_NONE);
    conf_set_int(conf, CONF_serflow, SER_FLOW_NONE);
    conf_set_int(conf, CONF_serspeed, 9600);
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

/*
 * cn: 波特率下拉框的预设值（2026-11-09 主人定：只保留三个常用档 + 自定义）。
 *
 * 「连接/串口」页面已删除，四项参数（数据位/停止位/校验/流控）固定走
 * 默认值，界面上只留波特率一项。这里只列网络设备控制台真正会遇到的
 * 三个档位：9600（华为/华三/思科等绝大多数）、19200（工业设备）、
 * 115200（MikroTik/Ubiquiti/服务器 BMC）。其余值靠「自定义」手填。
 */
static const int cn_serial_speed_presets[] = { 9600, 19200, 115200 };
#define NCN_SERIAL_SPEED_PRESETS ((int)lenof(cn_serial_speed_presets))

static void config_port_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                void *data, int event)
{
    Conf *conf = (Conf *)data;
    char buf[80];

    /*
     * This function works similarly to the standard edit box handler,
     * only it has to choose the control's label and text from two
     * different places depending on the protocol.
     *
     * cn: 这个控件是 combo box（可输入的下拉框），两种协议共用：
     *   - 网络模式：端口号，可手输（波特率列表清空、不显示箭头）；
     *   - 串口模式：波特率，下拉给三个常用档，也可手填任意值。
     */
    if (event == EVENT_REFRESH) {
        if (conf_get_int(conf, CONF_protocol) == PROT_SERIAL) {
            /*
             * This label text is carefully chosen to contain a p,
             * since that's the shortcut for the port control.
             */
            dlg_label_change(ctrl, dlg, "波特率(P)");
            sprintf(buf, "%d", conf_get_int(conf, CONF_serspeed));

            /*
             * 灌入预设档位。与主机名框同一套做法：列表只在串口模式
             * 下非空，网络模式必须清空 —— 残留的波特率条目会让端口框
             * 冒出下拉箭头，点开还是波特率数字，看着像串口没退干净。
             */
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (int i = 0; i < NCN_SERIAL_SPEED_PRESETS; i++) {
                char num[16];
                sprintf(num, "%d", cn_serial_speed_presets[i]);
                dlg_listbox_add(ctrl, dlg, num);
            }
            dlg_listbox_add(ctrl, dlg, "自定义");
            dlg_update_done(ctrl, dlg);
        } else {
            dlg_label_change(ctrl, dlg, PORT_BOX_TITLE);
            if (conf_get_int(conf, CONF_port) != 0)
                sprintf(buf, "%d", conf_get_int(conf, CONF_port));
            else
                /* Display an (invalid) port of 0 as blank */
                buf[0] = '\0';

            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            dlg_update_done(ctrl, dlg);
        }
        dlg_editbox_set(ctrl, dlg, buf);
    } else if (event == EVENT_VALCHANGE) {
        char *s = dlg_editbox_get(ctrl, dlg);
        int i;

        /*
         * cn: 选中「自定义」不是要写入 0 —— 它只是让用户能接着手填。
         * combo box 是 CBS_DROPDOWN，选中项的文字会被填进编辑区，
         * 所以这里遇到非数字就当作「用户还要继续输入」，什么都不写，
         * 也不回写框内容（回写会把刚清空的框又塞回旧值）。
         */
        if (!s || !*s || strcmp(s, "自定义") == 0) {
            sfree(s);
            return;
        }
        for (const char *q = s; *q; q++) {
            if (*q < '0' || *q > '9') {
                sfree(s);
                return;
            }
        }
        i = atoi(s);
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

/*
 * cn: 用户是否主动把密码框清空过。
 *
 * cmdline 槽位只有「有密码」和「没有」两种状态，分不出「用户清空了」
 * 和「本次没填」。而写历史时这两者必须区别对待：前者要真把记录里的
 * 密码删掉，后者得保留原值，否则用户从命令行启动一次 PuTTY，所有
 * 会话记着的密码就全没了。
 */
static bool cn_password_cleared_by_user;

/*
 * cn: 写历史时该给这条会话配什么密码。
 *
 * NULL 表示「不知道，保留原值」；空串表示「用户清空了，删掉记录」。
 */
static const char *cn_password_for_history(void)
{
    const char *pw = cmdline_get_password();
    if (!pw && cn_password_cleared_by_user)
        return "";
    return pw;
}

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
        cn_password_cleared_by_user = !s[0];
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

/* cn: 未填齐时点按钮的统一提示。 */
#define CN_INPUT_INCOMPLETE_MSG \
    "请先输入主机名称（IP 地址）和端口号"

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
 * cn: 用户在会话页点了「记录日志」。
 *
 * 需求是「一次性生效」：只在本次连接记，绝不写注册表、也不让保存按钮
 * 变脏。所以这里跟「文件传输」用同一套意图标志的办法 —— 点按钮时置位
 * 并结束对话框，窗口启动流程在 log_init 之前把它注入到窗口自己那份
 * Conf 副本里（log_init 内部还会再 conf_copy 一次，用户配置始终没被碰）。
 *
 * 值存的是要记的日志类型（LGTYP_ASCII 等），0 表示没这个意图。
 */
static int cn_session_logtype_request;
static char *cn_session_logfile_request;

int cn_take_log_request(char **path_out)
{
    int ret = cn_session_logtype_request;
    if (path_out)
        *path_out = cn_session_logfile_request;
    else
        sfree(cn_session_logfile_request);
    cn_session_logtype_request = 0;
    cn_session_logfile_request = NULL;
    return ret;
}

/*
 * cn: 把「记录日志」的意图落到 Conf 上。
 *
 * 调用点必须在窗口自己那份 Conf 副本上、且在 log_init 之前：log_init
 * 会再 conf_copy 一次，logtype/logfilename 都从这份副本读，所以用户
 * 的注册表配置始终没被碰过，下一次打开程序自然回到「不记录」。
 *
 * 覆盖策略固定为「追加」：文件名里已带秒级时间戳，正常情况下不会重名；
 * 万一同一秒内重复点击，追加比静默覆盖安全 —— 不丢数据。
 */
void cn_apply_log_request(Conf *conf)
{
    char *path = NULL;
    int logtype = cn_take_log_request(&path);

    if (logtype) {
        conf_set_int(conf, CONF_logtype, logtype);
        conf_set_int(conf, CONF_logxfovr, LGXF_APN);
        if (path) {
            Filename *fn = filename_from_str(path);
            conf_set_filename(conf, CONF_logfilename, fn);
            filename_free(fn);
        }
    }
    sfree(path);
}

/*
 * cn: 丢掉尚未消费的「记录日志」意图。
 *
 * 开关按钮不再结束配置框，所以意图可能在配置框里被点开之后，用户又
 * 点了「取消」/直接关窗 —— 那次连接根本没发生。若不清理，下一次打开
 * 配置框时按钮显示「记录日志」，静态槽位里却还留着上一轮的路径，
 * 两者对不上，用户点了连接就会莫名其妙开始记录。
 *
 * 在 do_config 入口调用：每次进配置框都是全新一轮，上一轮的开关状态
 * 一律作废。已连接的会话不受影响 —— 那边的日志早已注入窗口自己的
 * Conf 副本，与这个槽位无关。
 */
void cn_reset_log_request(void)
{
    sfree(cn_session_logfile_request);
    cn_session_logfile_request = NULL;
    cn_session_logtype_request = 0;
}

/*
 * cn: 生成「一次性日志」的完整路径：程序目录\主机名_年月日_时分秒.log
 *
 * 文件名里不拼 &H 占位符而是直接写死主机名，是因为占位符由 logging.c
 * 在打开文件那一刻才展开，而这里需要在点击时就把路径确定下来（按钮上
 * 要显示、日志页也要能看见）。主机名可能含 IPv6 冒号等非法字符，用
 * filename_char_sanitise 逐个消毒。
 *
 * 拿不到 exe 路径时退回相对路径，行为与上游默认值一致。
 */
char *cn_log_path_for_runtime(Conf *conf)
{
    char fname[512];
    char stamp[64];
    struct tm tm = ltime();
    const char *dest = conf_dest(conf);
    size_t n = 0;

    if (!dest || !*dest)
        dest = "session";
    /* 主机名/串口名逐个消毒：IPv6 地址里的冒号在 Windows 文件名中非法。 */
    for (const char *p = dest; *p && n < sizeof(fname) - 32; p++)
        fname[n++] = filename_char_sanitise(*p);
    fname[n++] = '_';

    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    for (const char *p = stamp; *p && n < sizeof(fname) - 8; p++)
        fname[n++] = *p;
    strcpy(fname + n, ".log");

    /*
     * 落到用户桌面（2026-11-09 主人指定）：日志是给人看的产物，桌面
     * 最顺手，不用去程序目录翻。
     *
     * 取法照搬 storage.c 的既有套路：动态加载 shell32 的
     * SHGetFolderPathA，走 CSIDL_DESKTOPDIRECTORY。动态加载而不是直接
     * 链接，是因为仓库一直避免对 shell32 建静态依赖（老系统上未必有）。
     *
     * 拿不到桌面路径时退回程序目录 —— 不能退回 cwd：从快捷方式启动
     * 时 cwd 可能是系统目录，日志会散落到用户找不到的地方。
     */
    {
        static HMODULE shell32_module = NULL;
        static HRESULT (WINAPI *p_SHGetFolderPathA)(
            HWND, int, HANDLE, DWORD, LPSTR) = NULL;
        static bool tried_desktop = false;

        if (!tried_desktop) {
            shell32_module = load_system32_dll("shell32.dll");
            if (shell32_module) {
                p_SHGetFolderPathA = (HRESULT (WINAPI *)(HWND, int, HANDLE,
                                                         DWORD, LPSTR))
                    GetProcAddress(shell32_module, "SHGetFolderPathA");
            }
            tried_desktop = true;
        }
        if (p_SHGetFolderPathA) {
            char desk[MAX_PATH + 1];
            if (SUCCEEDED(p_SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY,
                                             NULL, SHGFP_TYPE_CURRENT, desk))) {
                char *out = dupprintf("%s\\%s", desk, fname);
                return out;
            }
        }
    }
    {
        wchar_t exedir[MAX_PATH];
        if (GetModuleFileNameW(NULL, exedir, MAX_PATH) != 0) {
            wchar_t *slash = wcsrchr(exedir, L'\\');
            if (slash) {
                char *dir, *out;
                *slash = L'\0';
                dir = dup_wc_to_mb(CP_UTF8, exedir, "?");
                out = dupprintf("%s\\%s", dir, fname);
                sfree(dir);
                return out;
            }
        }
    }
    return dupstr(fname);
}

/*
 * cn: 「记录日志」复选框的 handler（2026-11-09 由按钮改版）。
 *
 * 与 conf_checkbox_handler 的根本区别：勾选结果不进 Conf，只写进静态
 * 意图槽位。Conf 一旦被写就会点亮「保存」按钮、并可能被落盘，而这条
 * 开关的语义是「只对本次连接生效」，落盘就变成永久设置了。
 *
 * 两个事件：
 *   - EVENT_VALCHANGE：用户勾/取消。勾选时校验目标是否填齐（日志文件名
 *     要从主机名生成），不齐则弹提示并把勾去掉，回到未勾选态。
 *   - EVENT_REFRESH：切页会销毁重建面板控件，新控件默认未勾选。这里按
 *     意图槽位把勾选态重新施加一遍，否则开关看着是关的、实际还开着。
 *
 * 取消勾选不做目标校验：已经开着的开关必须能关掉，哪怕用户把主机名
 * 清空了也得能关。
 */
static void cn_session_logcheckbox_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                           void *data, int event)
{
    Conf *conf = (Conf *)data;

    if (event == EVENT_VALCHANGE) {
        if (dlg_checkbox_get(ctrl, dlg)) {
            if (!session_page_inputs_filled(conf)) {
                dlg_info_msg(dlg, CN_INPUT_INCOMPLETE_MSG);
                /* 退回未勾选：意图没建立起来，勾选态就不该留着。 */
                dlg_checkbox_set(ctrl, dlg, false);
                return;
            }
            sfree(cn_session_logfile_request);
            cn_session_logfile_request = cn_log_path_for_runtime(conf);
            cn_session_logtype_request = LGTYP_ASCII;
        } else {
            sfree(cn_session_logfile_request);
            cn_session_logfile_request = NULL;
            cn_session_logtype_request = 0;
        }
        /*
         * 勾选态落定后把焦点交还给分类树。复选框带着焦点时，系统会在
         * 文字周围画一圈虚线焦点框，看着像「文字被选中」；切页时上游
         * 本来就把焦点给了树，虚线随之消失 —— 这里复用同一落点，让
         * 勾选后的观感与切页一致，勾选状态本身不受影响。
         */
        if (dlg->focus_treeview)
            dlg->focus_treeview(dlg);
    } else if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, cn_session_logtype_request != 0);
    }
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

/*
 * cn: 目录名上限。目录只做一级，名字只出现在列表行里，64 字节够用；
 * 放在这里是因为 struct sessionsaver_data 拿它当定长字段（见 cur_folder）。
 */
#define CN_FOLDER_NAME_MAX  64

struct sessionsaver_data {
    dlgcontrol *hostbox;        /* 上方「主机名称(或IP地址)」输入框 */
    dlgcontrol *portbox;        /* 旁边「端口」输入框，随历史条目一起回填 */
    dlgcontrol *userbox;        /* cn: 「用户名」输入框 */
    dlgcontrol *passbox;        /* cn: 「凭据（密码）」输入框 */
    dlgcontrol *listbox;
    dlgcontrol *savebutton, *delbutton;
    dlgcontrol *initbutton;     /* cn: 恢复到设置初始化快照 */
    dlgcontrol *logbutton;      /* cn: 本次连接记录会话日志（复选框） */
    dlgcontrol *loadkeybutton;  /* cn: 凭据框右侧的「载入密钥」 */
    dlgcontrol *eyebutton;      /* cn: 凭据框右侧的明文切换「小眼睛」 */
    /*
     * cn: 密码框当前是否明文显示。切页会销毁重建面板控件，重建出来的
     * 密码框又退回 ES_PASSWORD 的掩码态，所以状态得存在这里，每次面板
     * 刷新再照它施加一遍。
     */
    bool pass_plain;
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
    /*
     * cn: 会话目录（只做一级）。folders 与 history 一起在 refresh_history
     * 里重读，nfolders 是它的长度；目录行在列表里的 id 由下标编码而来。
     */
    char **folders;
    int nfolders;
    /*
     * cn: 当前「进入」了哪个目录，空串表示停在根层。
     *
     * 这是导航状态而不是持久化状态：关掉配置框就回根层，跟资源管理器
     * 重新打开时回到某个固定起点一样，不需要记住上次进了哪。
     * 按名字存而不是按下标：目录删掉或改名后下标会移位。
     */
    char cur_folder[CN_FOLDER_NAME_MAX];
    dlgcontrol *newfolderbutton; /* cn: 「新目录」，位于「删除」正上方 */
    bool midsession;
};

static void sessionsaver_data_free(void *ssdv)
{
    struct sessionsaver_data *ssd = (struct sessionsaver_data *)ssdv;
    /* cn: 别让输入框 handler 继续指向已释放的按钮集合。 */
    if (cn_session_ssd == ssd)
        cn_session_ssd = NULL;
    free_recent_conns(ssd->history, ssd->nhistory);
    free_recent_folders(ssd->folders, ssd->nfolders);
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
    /*
     * cn: 用户改过显示名就画显示名，否则画真实主机名。右列永远是端口 ——
     * 改名不改连接目标，端口也就没有理由跟着变。
     */
    const char *shown = (rc->label && rc->label[0]) ? rc->label : rc->host;

    if (rc->port > 0)
        return dupprintf("%s\t%d", shown, rc->port);
    return dupprintf("%s\t", shown);
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

/*
 * cn: 把「明文显示」状态施加到密码框与那颗小眼睛上。
 *
 * 按钮上的字说的是「点它会怎样」，不是「现在是什么」：掩码态显示
 * 「显」（点我看明文），明文态显示「隐」（点我盖回去）。
 */
static void cn_apply_pass_plain(struct sessionsaver_data *ssd,
                                dlgparam *dlg)
{
    if (ssd->passbox)
        dlg_editbox_set_plain(dlg, ssd->passbox, ssd->pass_plain);
    if (ssd->eyebutton)
        dlg_label_change(ssd->eyebutton, dlg,
                         ssd->pass_plain ? "隐" : "显");
}

/*
 * cn: 按当前 Conf 刷新会话页三个按钮的外观与显隐。
 *
 *   「载入密钥」——仅在 SSH 协议下显示；主机名与端口填齐才显示为可用。
 *   「连接」    ——SSH/Telnet 要求主机名与端口填齐；串口只要选了串口。
 *
 * 另有「显 / 隐」明文切换按钮，任何协议下都显示，永远可点。
 *
 * 三个按钮都在不可用状态下保持可点，点下去弹 CN_INPUT_INCOMPLETE_MSG。
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
     * cn: 「记录日志」复选框的勾选态跟着意图走。
     *
     * 切页回来时控件是新建的，勾选态回到未勾选 —— 若此时意图还开着，
     * 复选框就会显示成未开启，与实际状态对不上。所以每次刷新都按静态
     * 槽位重新施加一遍。
     *
     * dlg_get_ctrl_base_id 的存活检查不能省：refresh_session_buttons 在
     * 切到别的页之后仍会被调用，那时本页控件已经销毁，直接拿指针去
     * dlg_findbyctrl 会命中 assert（曾导致点「密钥」必崩）。
     */
    if (ssd->logbutton && dlg_get_ctrl_base_id(dlg, ssd->logbutton) >= 0)
        dlg_checkbox_set(ssd->logbutton, dlg, cn_session_logtype_request != 0);

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
        int limit = -1;

        /*
         * cn: 密码框最多能长到哪。
         *
         * 右邻先是「小眼睛」（始终显示），再往右才是「密钥」（仅 SSH）。
         * 所以硬边界是眼睛按钮的左沿，留 4px 呼吸 —— 越过它按钮就被
         * 盖住了。拿不到眼睛位置时才退回端口框右端：它和主机名框是同
         * 一组的两列，合起来占满整行，右端就是本页输入区的自然右界。
         */
        if (ssd->eyebutton) {
            int el = dlg_get_ctrl_left(dlg, ssd->eyebutton);
            if (el > 0)
                limit = el - 4;
        }
        if (limit <= 0) {
            int pr = dlg_get_ctrl_left(dlg, ssd->portbox);
            int pwid = dlg_get_ctrl_width(dlg, ssd->portbox);
            if (pr >= 0 && pwid > 0)
                limit = pr + pwid;
        }

        if (ssd->passbox_base_width <= 0 && pw > 0 && pl >= 0) {
            ssd->passbox_base_width = pw;
            ssd->passbox_left = pl;
            ssd->passbox_max_right = limit;
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

    /*
     * cn: 明文显示状态跟着刷新走。切页回来密码框是新建的，又回到掩码
     * 态，用户明明开着明文却被悄悄盖回去，这里照 pass_plain 重施加。
     * 存活检查的理由同上面的「记录日志」复选框：切到别的页之后本页
     * 控件已销毁，直接拿指针去 dlg_findbyctrl 会命中 assert。
     */
    if (ssd->eyebutton && dlg_get_ctrl_base_id(dlg, ssd->eyebutton) >= 0)
        cn_apply_pass_plain(ssd, dlg);

    set_button_muted(dlg, ssd->okbutton, !filled);

    /* 「文件传输」：SSH 且四项齐全，缺一不可。 */
    if (ssd->xferbutton)
        set_button_muted(dlg, ssd->xferbutton, !creds_ready);
}

/*
 * cn: 目录行在列表里的 id。
 *
 * 会话行的 id 仍然是它在 history 数组里的下标（0..nhistory-1）；目录行
 * 用一段负数。会话页里所有「取 id 当下标」的地方都带着 id >= 0 的守卫，
 * 负数 id 天然被当成空气，那些分支一行都不用改。
 */
#define CN_FOLDER_ID_BASE   (-1000)
#define CN_FOLDER_ID(f)     (CN_FOLDER_ID_BASE - (f))
#define CN_IS_FOLDER_ID(id) ((id) <= CN_FOLDER_ID_BASE)
#define CN_FOLDER_OF_ID(id) (CN_FOLDER_ID_BASE - (id))
/*
 * 「..」返回行的 id。刻意挑一个既不 >= 0（会话行）也不 <= -1000（目录行）
 * 的值：列表里每一处分类判断都只认这两段，-1 会落到所有分支之外，只有
 * 专门认它的双击和删除逻辑会处理它。
 */
#define CN_PARENT_ID        (-1)
#define CN_NO_SELECT        (0x7fffffff) /* 「别恢复选中」的哨兵 */

/*
 * cn: listbox 是 ANSI 控件，但本程序的 manifest 声明了
 * <activeCodePage>UTF-8</activeCodePage>（见 windows/putty.mft），
 * 进程内 GetACP() 就是 65001，ANSI 控件收发和 GDI 绘制都按 UTF-8
 * 解释。所以仓库内部的 UTF-8 char * 可以直接塞进控件，不用转码。
 */

/*
 * 目录行的显示文本：名字前一个 ▸ 表示「可以进去」，制表符保住两列分栏。
 *
 * 有内容时在后面带上条目数：根层视图不显示目录里的会话，会话被移进去
 * 就从列表上消失了，不给个数字用户会以为移丢了。
 */
static char *recent_folder_row_text(struct sessionsaver_data *ssd, int f)
{
    int i, n = 0;

    for (i = 0; i < ssd->nhistory; i++)
        if (ssd->history[i].folder &&
            !strcmp(ssd->history[i].folder, ssd->folders[f]))
            n++;

    if (n > 0)
        return dupprintf("▸ %s (%d)\t", ssd->folders[f], n);
    return dupprintf("▸ %s\t", ssd->folders[f]);
}

/* 目录名在 ssd->folders 里的下标，找不到返回 -1。 */
static int cn_folder_index(struct sessionsaver_data *ssd, const char *name)
{
    int f;

    if (!name || !*name)
        return -1;
    for (f = 0; f < ssd->nfolders; f++)
        if (!strcmp(ssd->folders[f], name))
            return f;
    return -1;
}

/*
 * cn: 把刚记进历史的那条连接归到当前所在的目录。
 *
 * 必须在 add_recent_conn 返回之后调用，不能指望它自己认路：它会把同
 * host/port/user 的旧条目整个继承过来（folder 一起继承），所以在目录
 * 里重连一条原本未归类的会话，它照样留在根层，自动归类就成了空话。
 *
 * 串口不记历史（端口框里是波特率而不是端口号），这里一并跳过。
 */
static void cn_adopt_current_folder(struct sessionsaver_data *ssd, Conf *conf)
{
    if (!ssd->cur_folder[0])
        return;
    if (conf_get_int(conf, CONF_protocol) == PROT_SERIAL)
        return;
    set_recent_conn_folder(conf_get_str(conf, CONF_host),
                           conf_get_int(conf, CONF_port),
                           conf_get_str_ambi(conf, CONF_username, NULL),
                           ssd->cur_folder);
}

/*
 * Refill the list box from the persistent history of recently used
 * connections.
 *
 * cn: 列表有两种视图，由 ssd->cur_folder 决定 ——
 *
 *   根层（cur_folder 为空）：先列目录行，再列未归类的会话。归了类的
 *   会话不在这里出现，跟资源管理器不在根目录铺开所有文件同理。
 *
 *   目录内（cur_folder 非空）：首行是「..」返回行，其下只有该目录里的
 *   会话。
 *
 * want_id 指定重建后要选中哪一行的 id，传 CN_NO_SELECT 则尽量保持当前
 * 选中不变。
 */
static void refresh_history(struct sessionsaver_data *ssd, dlgparam *dlg,
                            int want_id)
{
    int i, f, row = 0, sel_row = -1;
    int keep = want_id;
    char *text;

    /*
     * 重建会清空选中态。默认把重建前选中的那一行选回去 —— 归类操作
     * 必然触发整表重建，选中态跟着消失的话，用户移完一条就找不到刚
     * 才那条了。
     */
    if (keep == CN_NO_SELECT) {
        int cur = dlg_listbox_index(ssd->listbox, dlg);
        if (cur >= 0)
            keep = dlg_listbox_getid(ssd->listbox, dlg, cur);
    }

    free_recent_conns(ssd->history, ssd->nhistory);
    ssd->history = get_recent_conns(&ssd->nhistory);
    free_recent_folders(ssd->folders, ssd->nfolders);
    ssd->folders = get_recent_folders(&ssd->nfolders);

    /*
     * 当前目录可能刚被删掉或改了名（右键菜单、别处手改注册表），这时
     * 待在里头只会看见一个空列表。退回根层。
     */
    if (ssd->cur_folder[0] && cn_folder_index(ssd, ssd->cur_folder) < 0)
        ssd->cur_folder[0] = '\0';

    dlg_update_start(ssd->listbox, dlg);
    dlg_listbox_clear(ssd->listbox, dlg);

    if (ssd->cur_folder[0]) {
        /*
         * 目录内视图：首行是返回行，左列「..」（跟资源管理器一致），
         * 右列写「返回」。
         *
         * 右列原本显示当前目录名，但那一列是给端口号用的窄列，用户
         * 自己的目录名（「新目录」这类三个字）塞进去会被压成一条缝，
         * 字挤在一起完全看不清。这行本来就只需要表达「点我回去」，
         * 不需要重复目录名 —— 用户刚双击进来，心里清楚在哪一层。
         */
        text = dupstr("..\t返回");
        dlg_listbox_addwithid(ssd->listbox, dlg, text, CN_PARENT_ID);
        if (keep == CN_PARENT_ID && sel_row < 0)
            sel_row = row;
        row++;
        sfree(text);

        for (i = 0; i < ssd->nhistory; i++) {
            if (!ssd->history[i].folder ||
                strcmp(ssd->history[i].folder, ssd->cur_folder))
                continue;
            text = recent_entry_text(&ssd->history[i]);
            dlg_listbox_addwithid(ssd->listbox, dlg, text, i);
            if (i == keep && sel_row < 0)
                sel_row = row;
            row++;
            sfree(text);
        }
    } else {
        /* 根层视图：目录行在前，未归类会话在后。 */
        for (f = 0; f < ssd->nfolders; f++) {
            text = recent_folder_row_text(ssd, f);
            dlg_listbox_addwithid(ssd->listbox, dlg, text, CN_FOLDER_ID(f));
            if (CN_FOLDER_ID(f) == keep && sel_row < 0)
                sel_row = row;
            row++;
            sfree(text);
        }

        /*
         * 未归类的会话挂在最后。归属指向一个已不存在的目录时也落到这里
         * —— 注册表被手改过、或旧版本留下的悬空归属，不能让条目彻底消失。
         */
        for (i = 0; i < ssd->nhistory; i++) {
            if (cn_folder_index(ssd, ssd->history[i].folder) >= 0)
                continue;
            text = recent_entry_text(&ssd->history[i]);
            dlg_listbox_addwithid(ssd->listbox, dlg, text, i);
            if (i == keep && sel_row < 0)
                sel_row = row;
            row++;
            sfree(text);
        }
    }

    dlg_update_done(ssd->listbox, dlg);

    if (sel_row >= 0)
        dlg_listbox_select(ssd->listbox, dlg, sel_row);
}

/*
 * cn: 给会话页那几个按钮挂属性标记，供 dialog.c 的定位函数按属性找。
 *
 * 用属性而不是按文字找：控件是 ANSI 创建的，GetWindowText 回来的是系统
 * 代码页字节，与源码里的 UTF-8 字面量未必对得上，按文字匹配会静默失效。
 */
static void cn_tag_session_buttons(struct sessionsaver_data *ssd,
                                   dlgparam *dlg)
{
    static const struct { int which; const char *prop; } tags[] = {
        { 0, "cn_btn_newfolder" },
        { 1, "cn_btn_delete" },
        { 2, "cn_btn_init" },
        { 3, "cn_btn_log" },
        /*
         * cn: 「连接 / 应用」按钮也打标记。自动化测试要能可靠地点到它
         * ——它是 isdefault，按文本找又会撞上 ANSI 控件取文本的老问题。
         */
        { 4, "cn_btn_ok" },
    };
    HWND dlgwnd = (HWND)dlg_get_platform_window(dlg);
    size_t k;

    if (!dlgwnd)
        return;
    for (k = 0; k < lenof(tags); k++) {
        dlgcontrol *ctrl = NULL;
        HWND h;
        int base;

        switch (tags[k].which) {
          case 0: ctrl = ssd->newfolderbutton; break;
          case 1: ctrl = ssd->delbutton; break;
          case 2: ctrl = ssd->initbutton; break;
          case 3: ctrl = ssd->logbutton; break;
          case 4: ctrl = ssd->okbutton; break;
        }
        if (!ctrl)
            continue;
        base = dlg_get_ctrl_base_id(dlg, ctrl);
        if (base <= 0)
            continue;
        h = GetDlgItem(dlgwnd, base);
        if (h)
            SetProp(h, tags[k].prop, (HANDLE)1);
    }
}

/*
 * cn: 取历史列表控件本身的 HWND。
 *
 * 列表控件的窗口 ID 是 base_id + 1：base_id 那个位置是控件自带的
 * STATIC 标题（见 controls.c 的 listbox()，它按 sid / lid 两个 ID 分别
 * 建标题和列表）。用 base_id 去 GetDlgItem 拿到的是标题，不是列表。
 */
static HWND cn_list_hwnd(struct sessionsaver_data *ssd, dlgparam *dlg)
{
    HWND dlgwnd = (HWND)dlg_get_platform_window(dlg);
    int base;

    if (!dlgwnd || !ssd->listbox)
        return NULL;
    base = dlg_get_ctrl_base_id(dlg, ssd->listbox);
    if (base <= 0)
        return NULL;
    return GetDlgItem(dlgwnd, base + 1);
}

/*
 * cn: 按列表项 id 找行号（找不到返回 -1）。
 *
 * 目录行的 id 是 CN_FOLDER_ID(下标)、会话行的 id 是 history 下标，两者
 * 都是「按 id 认行」而不是「按行号认行」—— 行号会随视图切换而变。
 */
static int cn_row_of_id(struct sessionsaver_data *ssd, dlgparam *dlg, int id)
{
    HWND lb = cn_list_hwnd(ssd, dlg);
    int n, i;

    if (!lb)
        return -1;
    n = (int)SendMessage(lb, LB_GETCOUNT, 0, 0);
    for (i = 0; i < n; i++) {
        if ((int)SendMessage(lb, LB_GETITEMDATA, i, 0) == id)
            return i;
    }
    return -1;
}

/* 新建一个目录并立刻进入改名。取消则保留默认名（需求：不改就用默认名）。 */
static void cn_new_folder(struct sessionsaver_data *ssd, dlgparam *dlg)
{
    char name[CN_FOLDER_NAME_MAX];
    char edited[CN_FOLDER_NAME_MAX];
    int n = 1, f, row;

    /* 默认名「新目录」，重名就往上加序号。 */
    for (;;) {
        if (n == 1)
            snprintf(name, sizeof(name), "新目录");
        else
            snprintf(name, sizeof(name), "新目录 %d", n);
        if (cn_folder_index(ssd, name) < 0)
            break;
        if (++n > 999)
            return;
    }

    if (!add_recent_folder(name)) {
        dlg_error_msg(dlg, "目录数量已达上限（20 个），请先删除不用的目录。");
        return;
    }

    /*
     * 目录只做一级，新目录总是落在根层。正待在别的目录里时先退出去，
     * 否则建完了却在列表上看不见它，只剩一个「按钮没反应」的观感。
     */
    ssd->cur_folder[0] = '\0';
    refresh_history(ssd, dlg, CN_NO_SELECT);

    /*
     * 下标要等重建之后才算得准：refresh 之前 ssd->folders 还是旧列表，
     * 里面根本没有刚建的这个目录。就地编辑框得落在它那一行上。
     */
    f = cn_folder_index(ssd, name);
    row = (f >= 0) ? cn_row_of_id(ssd, dlg, CN_FOLDER_ID(f)) : -1;
    if (row >= 0)
        dlg_listbox_select(ssd->listbox, dlg, row);

    snprintf(edited, sizeof(edited), "%s", name);
    /*
     * 就地改名：编辑框直接浮在刚建好的那一行上，改完回车即生效，ESC
     * 保留默认名。清空名字再回车 = 不要这个目录。
     */
    if (cn_inline_edit(cn_list_hwnd(ssd, dlg), row,
                       edited, sizeof(edited))) {
        if (!edited[0])
            del_recent_folder(name);      /* 清空名字 = 不要这个目录 */
        else if (strcmp(edited, name))
            rename_recent_folder(name, edited);
        refresh_history(ssd, dlg, CN_NO_SELECT);
    }
}

/*
 * cn: 会话列表的右键菜单。
 *
 * listbox 的默认过程会把 WM_RBUTTONUP 转成发给父窗的 WM_CONTEXTMENU，
 * 父窗（配置框）不认这条消息，转过去只会多一声系统蜂鸣；所以在这里
 * 自己弹菜单并把消息吃掉。
 *
 * 目录只做一级，所以「移入」的子菜单里不会出现目录下的目录；删除目录
 * 只清会话的归属、不删会话本身，因此不弹二次确认。
 */
static void cn_show_list_menu(struct sessionsaver_data *ssd, dlgparam *dlg,
                              HWND hwnd, int sx, int sy, int row)
{
    int id, fidx, cmd;

    if (row < 0)
        return;                     /* 空白处右键：没有作用对象 */

    id = (int)SendMessage(hwnd, LB_GETITEMDATA, row, 0);

    if (CN_IS_FOLDER_ID(id)) {
        fidx = CN_FOLDER_OF_ID(id);
        if (fidx < 0 || fidx >= ssd->nfolders)
            return;
        cmd = cn_session_list_menu(hwnd, sx, sy, true, false, NULL, 0);
        if (cmd == CN_SESS_MENU_OPEN) {
            snprintf(ssd->cur_folder, sizeof(ssd->cur_folder), "%s",
                     ssd->folders[fidx]);
            refresh_history(ssd, dlg, CN_PARENT_ID);
        } else if (cmd == CN_SESS_MENU_RENAME) {
            char name[CN_FOLDER_NAME_MAX];
            snprintf(name, sizeof(name), "%s", ssd->folders[fidx]);
            if (cn_inline_edit(cn_list_hwnd(ssd, dlg), row,
                               name, sizeof(name))) {
                if (!name[0]) {
                    del_recent_folder(ssd->folders[fidx]);
                } else if (strcmp(name, ssd->folders[fidx]) &&
                           rename_recent_folder(ssd->folders[fidx], name)) {
                    /*
                     * 正待在这个目录里改名时，cur_folder 里存的还是旧名，
                     * 重建会把它当成「目录已不存在」踢回根层。同步跟上。
                     */
                    if (!strcmp(ssd->cur_folder, ssd->folders[fidx]))
                        snprintf(ssd->cur_folder,
                                 sizeof(ssd->cur_folder), "%s", name);
                }
            }
            refresh_history(ssd, dlg, CN_NO_SELECT);
        } else if (cmd == CN_SESS_MENU_DELETE) {
            del_recent_folder(ssd->folders[fidx]);
            refresh_history(ssd, dlg, CN_NO_SELECT);
        }
        return;
    }

    if (id < 0 || id >= ssd->nhistory)
        return;

    cmd = cn_session_list_menu(hwnd, sx, sy, false,
                               ssd->history[id].folder &&
                               ssd->history[id].folder[0],
                               ssd->folders, ssd->nfolders);
    if (cmd == CN_SESS_MENU_RENAME) {
        /*
         * 改的只是显示名：host / port / username 一个都不动，双击回填
         * 和真正连过去的地址保持原样。留空即恢复显示真实主机名。
         */
        char name[CN_FOLDER_NAME_MAX];
        const char *cur = ssd->history[id].label;
        snprintf(name, sizeof(name), "%s",
                 (cur && cur[0]) ? cur : ssd->history[id].host);
        if (cn_inline_edit(cn_list_hwnd(ssd, dlg), row,
                           name, sizeof(name))) {
            /* 含分列符的名字会把列表画乱，宁可整条忽略。 */
            if (!strpbrk(name, "\t\r\n"))
                set_recent_conn_label(ssd->history[id].host,
                                      ssd->history[id].port,
                                      ssd->history[id].username, name);
        }
        refresh_history(ssd, dlg, id);
    } else if (cmd == CN_SESS_MENU_MOVE_OUT) {
        set_recent_conn_folder(ssd->history[id].host,
                               ssd->history[id].port,
                               ssd->history[id].username, "");
        refresh_history(ssd, dlg, id);
    } else if (cmd >= CN_SESS_MENU_MOVE_BASE &&
               cmd - CN_SESS_MENU_MOVE_BASE < ssd->nfolders) {
        int f = cmd - CN_SESS_MENU_MOVE_BASE;
        set_recent_conn_folder(ssd->history[id].host,
                               ssd->history[id].port,
                               ssd->history[id].username,
                               ssd->folders[f]);
        /*
         * 移进去之后这条会话就离开当前视图了：根层不显示归类过的，
         * 目录内不显示别的目录的。选中落到目标目录行上，顺手让用户
         * 看见它的条目数涨了一个，否则会以为移丢了。
         */
        refresh_history(ssd, dlg,
                        ssd->cur_folder[0] ? CN_NO_SELECT : CN_FOLDER_ID(f));
    }
}

static LRESULT CALLBACK cn_list_wndproc(HWND hwnd, UINT msg,
                                        WPARAM wparam, LPARAM lparam)
{
    struct sessionsaver_data *ssd =
        (struct sessionsaver_data *)GetProp(hwnd, "cn_ssd");
    dlgparam *dlg = (dlgparam *)GetProp(hwnd, "cn_dlg");
    WNDPROC oldproc = (WNDPROC)GetProp(hwnd, "cn_oldproc");

    if (ssd && dlg && msg == WM_RBUTTONUP) {
        POINT pt, cp;
        LRESULT hit;
        int row;

        GetCursorPos(&pt);
        cp = pt;
        ScreenToClient(hwnd, &cp);
        hit = SendMessage(hwnd, LB_ITEMFROMPOINT, 0,
                          MAKELPARAM(cp.x, cp.y));
        row = HIWORD(hit) ? -1 : (int)LOWORD(hit);
        if (row >= 0) {
            /* 右键点到哪一行就作用于哪一行，先把它选上。 */
            SendMessage(hwnd, LB_SETCURSEL, row, 0);
            cn_show_list_menu(ssd, dlg, hwnd, pt.x, pt.y, row);
            return 0;
        }
    }

    /*
     * cn: 双击目录行 = 进入目录，双击「..」行 = 退回根层。
     *
     * 这条消息必须自己接住：listbox 的默认过程会把它变成 LBN_DBLCLK
     * 通知发给父窗，而配置框不认这条通知，双击就只剩「选了两次」的
     * 效果 —— 看起来就是怎么点都进不去目录。会话行不拦，双击仍然
     * 落回默认的选中/回填行为。
     */
    if (ssd && dlg && msg == WM_LBUTTONDBLCLK) {
        POINT cp;
        LRESULT hit;
        int row, id, f;

        cp.x = (short)LOWORD(lparam);
        cp.y = (short)HIWORD(lparam);
        hit = SendMessage(hwnd, LB_ITEMFROMPOINT, 0,
                          MAKELPARAM(cp.x, cp.y));
        row = HIWORD(hit) ? -1 : (int)LOWORD(hit);
        if (row >= 0) {
            id = (int)SendMessage(hwnd, LB_GETITEMDATA, row, 0);
            if (CN_IS_FOLDER_ID(id)) {
                f = CN_FOLDER_OF_ID(id);
                if (f >= 0 && f < ssd->nfolders) {
                    snprintf(ssd->cur_folder, sizeof(ssd->cur_folder),
                             "%s", ssd->folders[f]);
                    /*
                     * 进去之后选中「..」那一行：别顺手把目录里第一条
                     * 会话选上，否则上方的回填框会莫名多出一个主机名。
                     */
                    refresh_history(ssd, dlg, CN_PARENT_ID);
                    return 0;
                }
            } else if (id == CN_PARENT_ID) {
                /* 退回根层，并把刚离开的那个目录选上，一眼看得出从哪回来。 */
                char back[CN_FOLDER_NAME_MAX];
                snprintf(back, sizeof(back), "%s", ssd->cur_folder);
                ssd->cur_folder[0] = '\0';
                f = cn_folder_index(ssd, back);
                refresh_history(ssd, dlg,
                                f >= 0 ? CN_FOLDER_ID(f) : CN_NO_SELECT);
                return 0;
            }
        }
    }

    if (oldproc)
        return CallWindowProcW(oldproc, hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* 把右键子类挂到历史列表上（幂等，重复调用不会叠罗汉）。 */
static void cn_hook_list_menu(struct sessionsaver_data *ssd, dlgparam *dlg)
{
    HWND dlgwnd = (HWND)dlg_get_platform_window(dlg);
    int base;
    HWND hwnd;
    LONG_PTR old;

    if (!dlgwnd || !ssd->listbox)
        return;
    base = dlg_get_ctrl_base_id(dlg, ssd->listbox);
    if (base <= 0)
        return;
    /*
     * 列表控件的窗口 ID 是 base_id + 1：base_id 那个位置是控件自带
     * 的 STATIC 标题（见 controls.c 的 listbox()，它按 sid / lid 两个
     * ID 分别建标题和列表）。用 base_id 去 GetDlgItem 拿到的是标题，
     * 拿不到列表本身。
     */
    hwnd = GetDlgItem(dlgwnd, base + 1);
    if (!hwnd || GetProp(hwnd, "cn_ssd"))
        return;                     /* 没控件，或已经挂过 */

    /*
     * ssd 与 dlg 存属性而不是 GWLP_USERDATA：listbox 会被 comctl32 内部
     * 用过 GWLP_USERDATA，写进去可能被覆盖。属性名归我们自己，不会撞。
     */
    SetProp(hwnd, "cn_ssd", (HANDLE)ssd);
    SetProp(hwnd, "cn_dlg", (HANDLE)dlg);
    /*
     * 必须用 W 版：SetWindowLongPtrA 设置 GWLP_WNDPROC 时会把窗口的
     * Unicode 标志清掉，控件随即按 ANSI 解释字符串，列表行首的「▸」
     * 这类 GBK 里没有的字符就会被降级成「?」。配套的
     * CallWindowProcW 同理，A 版会在转交前把字符串消息降级。
     */
    old = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)cn_list_wndproc);
    SetProp(hwnd, "cn_oldproc", (HANDLE)old);
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
            /*
             * cn: 密码回填。条目里没记密码就清空 —— 绝不能沿用上一条
             * 会话留在 cmdline 槽位里的口令，那是拿别人的钥匙开锁。
             * 回填本身会触发 EN_CHANGE，用 refilling 标记挡掉，改由这
             * 里直接写槽位，保证「框里显示的」和「SSH/Telnet 会用的」
             * 是同一个值。
             */
            cn_passbox_refilling = true;
            if (ssd->passbox)
                dlg_editbox_set(ssd->passbox, dlg,
                                rc->password ? rc->password : "");
            cn_passbox_refilling = false;
            cmdline_set_password(rc->password && rc->password[0]
                                 ? rc->password : NULL);
            /* 这是程序回填，不是用户清空，别把记录里的密码当垃圾删了。 */
            cn_password_cleared_by_user = false;
            cn_session_buttons_changed(dlg, conf);
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
    /*
     * cn: 波特率是「本次窗口的临时值」，不落盘（2026-11-09 主人定）。
     * 保存时写成默认 9600，落盘后立刻还原运行值，这样「保存」不会把
     * 当前窗口手填的波特率固化进全局默认，而紧接着点「打开」仍用用户
     * 刚填的那个值。机制与 host/username 完全一致。
     */
    int runtime_serspeed = conf_get_int(conf, CONF_serspeed);
    /*
     * cn: 会话日志同样是「本次窗口的临时状态」，不落盘（2026-11-09）。
     *
     * 「日志」菜单里点「开始记录日志」会往窗口这份 Conf 写 logtype 与
     * logfilename —— 那是运行态注入，用户的本意绝不是「以后每次启动都
     * 记日志」。若不加处理，这里 save_settings 会把整份 Conf 原样写进
     * 注册表，LogType 和那个带时间戳的文件名就被固化成全局默认，下一次
     * 启动自动开始记录、还会往一个早就过时的文件名里追加。
     *
     * 处理办法与 host/username 完全一致：落盘时写成默认值，落盘后立刻
     * 还原运行值。这样「保存」不影响正在进行的日志，注册表也不会被污染。
     */
    int runtime_logtype = conf_get_int(conf, CONF_logtype);
    int runtime_logxfovr = conf_get_int(conf, CONF_logxfovr);
    Filename *runtime_logfile =
        filename_copy(conf_get_filename(conf, CONF_logfilename));
    conf_set_str(conf, CONF_host, "");
    conf_set_str(conf, CONF_username, "");
    Filename *cn_empty_fn = filename_from_str("");
    conf_set_filename(conf, CONF_keyfile, cn_empty_fn);
    conf_set_filename(conf, CONF_detached_cert, cn_empty_fn);
    conf_set_filename(conf, CONF_logfilename, cn_empty_fn);
    filename_free(cn_empty_fn);
    conf_set_int(conf, CONF_port, 22);
    conf_set_int(conf, CONF_protocol, PROT_SSH);
    conf_set_int(conf, CONF_serspeed, 9600);
    conf_set_int(conf, CONF_logtype, LGTYP_NONE);
    conf_set_int(conf, CONF_logxfovr, LGXF_ASK);
    char *errmsg = save_settings("", conf);
    conf_set_str(conf, CONF_host, runtime_host);
    conf_set_str(conf, CONF_username, runtime_username);
    conf_set_filename(conf, CONF_keyfile, runtime_keyfile);
    conf_set_filename(conf, CONF_detached_cert, runtime_cert);
    conf_set_int(conf, CONF_port, runtime_portnum);
    conf_set_int(conf, CONF_protocol, runtime_proto);
    conf_set_int(conf, CONF_serspeed, runtime_serspeed);
    conf_set_int(conf, CONF_logtype, runtime_logtype);
    conf_set_int(conf, CONF_logxfovr, runtime_logxfovr);
    conf_set_filename(conf, CONF_logfilename, runtime_logfile);
    filename_free(runtime_logfile);
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
            refresh_history(ssd, dlg, CN_NO_SELECT);
        /*
         * 按钮外观 / 显隐依赖协议与主机端口，任何一次面板刷新都重算
         * 一遍（切页回来时控件是新建的，状态必须重新施加）。
         */
        refresh_session_buttons(ssd, dlg, conf);
        /*
         * cn: 切页会销毁重建面板控件，所以按钮标记与列表右键子类也在
         * 这里补一次（两个函数都是幂等的，重复调用无副作用）。
         */
        cn_tag_session_buttons(ssd, dlg);
        cn_hook_list_menu(ssd, dlg);
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
        } else if (ssd->eyebutton && ctrl == ssd->eyebutton) {
            /*
             * cn: 明文 / 掩码切换。密码本身一个字都不动，只换显示方式，
             * 所以不会触发 EN_CHANGE，也就不会污染 cmdline 槽位。
             */
            ssd->pass_plain = !ssd->pass_plain;
            cn_apply_pass_plain(ssd, dlg);
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
            if (conf_launchable(conf)) {
                add_recent_conn(conf_get_str(conf, CONF_host),
                                conf_get_int(conf, CONF_port),
                                conf_get_str_ambi(conf, CONF_username, NULL),
                                cn_password_for_history());
                cn_adopt_current_folder(ssd, conf);
            }
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
            /*
             * cn: 与「连接」同一道确认：清单为空直接放行，有改动才
             * 问「仅本次 / 长期」。选长期就把改动落盘，然后继续开
             * 传输窗口；点 ❌ 则什么都不做（连窗口都不开）。
             */
            {
                CnLaunchChoice lc = dlg_confirm_launch(dlg);
                if (lc == CN_LAUNCH_CANCEL)
                    return;
                if (lc == CN_LAUNCH_PERSIST) {
                    char *errmsg = cn_persist_default_settings(conf);
                    if (errmsg) {
                        dlg_error_msg(dlg, errmsg);
                        sfree(errmsg);
                        return;
                    }
                    dlg_mark_clean(dlg);
                }
            }
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
                   ssd->newfolderbutton && ctrl == ssd->newfolderbutton) {
            cn_new_folder(ssd, dlg);
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
                    refresh_history(ssd, dlg, CN_NO_SELECT);
                } else if (CN_IS_FOLDER_ID(id)) {
                    /*
                     * 选中目录行时「删除」删的是目录本身，会话只退回
                     * 未归类、不跟着消失。
                     */
                    int f = CN_FOLDER_OF_ID(id);
                    if (f >= 0 && f < ssd->nfolders) {
                        del_recent_folder(ssd->folders[f]);
                        refresh_history(ssd, dlg, CN_NO_SELECT);
                    }
                } else {
                    /* 「..」返回行不是可删的对象。 */
                    dlg_beep(dlg);
                }
            }
        } else if (!ssd->midsession && ctrl == ssd->cancelbutton) {
            /*
             * cn: 「退出」按钮。这个按钮的控件 ID 被映射成系统
             * IDCANCEL（见 controls.c 里 button.iscancel 的处理），
             * 而按钮点击默认让 winctrl_handle_command 返回 false，
             * 消息就落到 Windows 对话框管理器的默认处理上直接关窗
             * —— 未保存的改动被无声丢弃。这里认领这次点击
             * （action_handled），关不关窗完全由下面的判断说了算：
             * 有改动就走与标题栏 X 相同的确认流程，用户点「取消」
             * 时留在配置框里。
             */
            dlg->action_handled = true;
            switch (dlg_confirm_quit(dlg)) {
              case CN_QUIT_DISCARD:
                /* 放弃改动，直接关窗。 */
                dlg_end(dlg, 0);
                break;
              case CN_QUIT_SAVE:
                /*
                 * 保存并退出：先落盘，成功才关窗。落盘失败就留在
                 * 配置框里，让用户看见错误、还有机会重试或改。
                 */
                {
                    char *errmsg = cn_persist_default_settings(conf);
                    if (errmsg) {
                        dlg_error_msg(dlg, errmsg);
                        sfree(errmsg);
                    } else {
                        dlg_mark_clean(dlg);
                        dlg_end(dlg, 0);
                    }
                }
                break;
              case CN_QUIT_STAY:
              default:
                /* 取消：什么都不做，留在配置框里。 */
                break;
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
                 * cn: 启动前先问改动怎么生效。清单为空直接放行；
                 * 有改动才弹「仅本次生效 / 长期生效」。点 ❌ 则取消
                 * 本次连接；其余情况都继续连，不中断、不重复触发。
                 */
                {
                    CnLaunchChoice lc = dlg_confirm_launch(dlg);
                    if (lc == CN_LAUNCH_CANCEL)
                        return;
                    if (lc == CN_LAUNCH_PERSIST) {
                        char *errmsg = cn_persist_default_settings(conf);
                        if (errmsg) {
                            dlg_error_msg(dlg, errmsg);
                            sfree(errmsg);
                            return;
                        }
                        dlg_mark_clean(dlg);
                    }
                }
                /*
                 * Only network connections go into the history: the
                 * serial port box holds a device name, not a host, and
                 * the neighbouring box is a baud rate rather than a
                 * port number.
                 */
                if (conf_get_int(conf, CONF_protocol) != PROT_SERIAL) {
                    add_recent_conn(conf_get_str(conf, CONF_host),
                                    conf_get_int(conf, CONF_port),
                                    conf_get_str_ambi(conf, CONF_username, NULL),
                                    cn_password_for_history());
                    cn_adopt_current_folder(ssd, conf);
                }
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
        c = ctrl_combobox(s, PORT_BOX_TITLE, 'p', 100,
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
         * cn: 列宽 42/35/9/14。
         *
         * 第三列（9%）给「小眼睛」，第四列（14%）给「载入密钥」。
         *
         * 密码那一列必须守住 35%：标签「凭据（密码）：」七个字符在默认
         * GUI 字体下实测约 84px，而 35% 换算下来约 88px，刚好放得下 ——
         * 再窄就截成「凭据（密…」。所以能压缩的只有用户名列，从 50%
         * 让到 42%（它的标签只有四个字，宽裕得很）。
         *
         * 「密钥」两字约 24px，14% 约 35px 够用；眼睛是单字，9% 约
         * 22px 也够。
         */
        ctrl_columns(s, 4, 42, 35, 9, 14);
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
        /*
         * cn: 「小眼睛」—— 点一下把密码框从圆点换成明文，再点一下盖回去。
         *
         * 文案用单字：这一列只有 9%，两个字放不下。掩码态写「显」、明文
         * 态写「隐」，说的都是「点下去会怎样」。高度跟输入框对齐，同下
         * 面「密钥」按钮。
         */
        ssd->eyebutton = ctrl_pushbutton(s, "显", NO_SHORTCUT,
                                         HELPCTX(session_hostname),
                                         sessionsaver_handler, P(ssd));
        ssd->eyebutton->column = 2;
        ssd->eyebutton->button.cn_editheight = true;

        ssd->loadkeybutton = ctrl_pushbutton(s, "密钥", NO_SHORTCUT,
                                             HELPCTX(ssh_auth_privkey),
                                             sessionsaver_handler, P(ssd));
        ssd->loadkeybutton->column = 3;
        /*
         * cn: 按钮默认按 PUSHBTNHEIGHT（14 个对话框单位）画，比同排的
         * 输入框 EDITHEIGHT（12）高出 2 个单位 —— 当前 DPI 下就是 6px，
         * 顶边对齐、底边却探出输入框一截，看着像没对齐。这里让它改用
         * 输入框的高度，两个控件顶底都齐。
         */
        ssd->loadkeybutton->button.cn_editheight = true;

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
        /*
         * cn: 「新目录」—— 在历史列表里插一个文件夹条目，用来给会话按
         * IP 归类。位置在「删除」正上方（由 cn_style_snapshot_buttons
         * 钉），点一下就新建并直接进入改名待改状态。
         */
        ssd->newfolderbutton = ctrl_pushbutton(s, "新目录", NO_SHORTCUT,
                                               HELPCTX(no_help),
                                               sessionsaver_handler, P(ssd));
        ssd->newfolderbutton->column = 1;
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
        /*
         * cn: 「记录日志」—— 一次性开关，只对本次连接生效。
         *
         * 2026-11-09 主人改版：由按钮改为复选框。原按钮靠文字变化
         * （记录日志 ↔ 关闭记录）表达状态，切页回来焦点被上游
         * SetFocus(treeview) 抢走，蓝框消失后光看文字容易忽略状态；
         * 复选框自带勾选态，不依赖焦点也能一眼看出开没开。
         *
         * 位置在「删除」正下方、「初始化设置」上方（三者竖排在历史列表
         * 右侧那一列，具体像素位置由 cn_style_snapshot_buttons 钉）。
         * 勾选只改意图，不写 Conf、不点亮保存按钮 —— 这是「一次性」的
         * 全部含义，因此用自有 handler 而非 conf_checkbox_handler。
         */
        ssd->logbutton = ctrl_checkbox(s, "记录日志", NO_SHORTCUT,
                                       HELPCTX(logging_main),
                                       cn_session_logcheckbox_handler, P(ssd));
        ssd->logbutton->column = 1;
    } else {
        /* Disable the Delete button mid-session too, for UI consistency. */
        ssd->newfolderbutton = NULL;
        ssd->delbutton = NULL;
        ssd->logbutton = NULL;
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
              "&T 表示时间，&H 表示主机名称，"
              "&P 表示端口号，&& 表示 & 字符)",
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
             * 每行一个 ctrl_text，各自独立成段，不靠 \n 拼接。
             * staticwrap() 现在是在宽字符空间测量与断行的（见
             * windows/controls.c），中文折行本身已经可靠；分成两个
             * 控件仍然更合适：这两句说的是两件事（支持哪些格式 /
             * 转换后做什么），分成两段在视觉上就是两组信息。
             *
             * 两句长度都控制在卡片可用宽度内（120 DPI 下约 266 px），
             * 一行放得下，不再出现「第二行只剩几个字」的断行。
             * 改文案时请复测：超宽会被折行，且第二行会明显短于第一行。
             *
             * 内容必须与 keyimport.c 的 importable_formats 一致：
             * 转换器实际只吃 OpenSSH 新旧两种和 ssh.com 三种。
             */
            ctrl_text(s, "支持 OpenSSH 新旧格式与 ssh.com 私钥。",
                      HELPCTX(no_help));
            ctrl_text(s, "转换后同目录生成 .ppk 并填入上方私钥框。",
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

    /*
     * cn: 「连接/串口」页面已删除（2026-11-09 主人决定）。
     *
     * 串口参数只在会话页留一个「波特率(P)」下拉框（9600 / 19200 /
     * 115200 / 自定义），其余四项（数据位、停止位、奇偶校验、流控）
     * 固定走默认值 8-N-1 无流控 —— 详见 cn_serial_params_reset()，
     * 它在每次打开配置框时把这四项归位，防止旧存档里的非默认值变成
     * 用户看不见也改不了的暗坑。
     *
     * 原页面里的「厂商标准」预设表（serial_standards / serial_ui /
     * serial_standard_handler 等）随页面一并删除：那些条目除了「老式
     * 设备」的 7-E-1 之外全是 8-N-1，只有波特率不同，而波特率已经由
     * 下拉框覆盖。主人明确表示不再保老式设备。
     */

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
