/*
 * dialog.c - dialogs for PuTTY(tel), including the configuration dialog.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include <ctype.h>
#include <time.h>

#include "putty.h"
#include "ssh.h"
#include "putty-rc.h"
#include "win-gui-seat.h"
#include "storage.h"
#include "dialog.h"
#include "licence.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#ifdef MSVC4
#define TVINSERTSTRUCT  TV_INSERTSTRUCT
#define TVITEM          TV_ITEM
#define ICON_BIG        1
#endif

/* cn: 当前打开的配置框（模态、单实例），供设置页跳转回调定位。 */
static struct PortableDialogStuff *active_config_box;
static bool config_box_navigate_to_path(dlgparam *dp, const char *pathname);
static void config_box_focus_treeview(dlgparam *dp);

typedef struct PortableDialogStuff {
    /*
     * These are the various bits of data required to handle a dialog
     * box that's been built up from the cross-platform dialog.c
     * system.
     */

    /* The 'controlbox' that was returned from the portable setup function */
    struct controlbox *ctrlbox;

    /* The dlgparam that's passed to all the runtime dlg_* functions.
     * Declared as an array of 1 so it's convenient to pass it as a pointer. */
    struct dlgparam dp[1];

    /*
     * Collections of instantiated controls. There can be more than
     * one of these, because sometimes you want to destroy and
     * recreate a subset of them - e.g. when switching panes in the
     * main PuTTY config box, you delete and recreate _most_ of the
     * controls, but not the OK and Cancel buttons at the bottom.
     */
    size_t nctrltrees;
    struct winctrls *ctrltrees;

    /*
     * Flag indicating whether the dialog box has been initialised.
     * Used to suppresss spurious firing of message handlers during
     * setup.
     */
    bool initialised;
} PortableDialogStuff;

/*
 * Initialise a PortableDialogStuff, before launching the dialog box.
 */
static PortableDialogStuff *pds_new(size_t nctrltrees)
{
    PortableDialogStuff *pds = snew(PortableDialogStuff);
    memset(pds, 0, sizeof(*pds));

    pds->ctrlbox = ctrl_new_box();

    dp_init(pds->dp);

    pds->nctrltrees = nctrltrees;
    pds->ctrltrees = snewn(pds->nctrltrees, struct winctrls);
    for (size_t i = 0; i < pds->nctrltrees; i++) {
        winctrl_init(&pds->ctrltrees[i]);
        dp_add_tree(pds->dp, &pds->ctrltrees[i]);
    }

    pds->dp->errtitle = dupprintf("%s 错误", appname);

    pds->initialised = false;

    return pds;
}

static void pds_free(PortableDialogStuff *pds)
{
    /* cn: 别让跳转钩子继续指向正在销毁的对话框。 */
    if (active_config_box == pds)
        active_config_box = NULL;

    ctrl_free_box(pds->ctrlbox);

    dp_cleanup(pds->dp);

    for (size_t i = 0; i < pds->nctrltrees; i++)
        winctrl_cleanup(&pds->ctrltrees[i]);
    sfree(pds->ctrltrees);

    sfree(pds);
}

/*
 * cn: restyle the Delete and 设置初始化 buttons of the startup config
 * box — smaller font (so 设置初始化 fits on one line), shorter
 * buttons, and the pair pinned to the bottom edge of the history
 * list box.
 */
/*
 * cn: 按控件属性找按钮，文字匹配只作兜底。
 *
 * 控件是 ANSI 创建的，GetWindowText 拿回来的是系统代码页字节，与源码
 * 里的 UTF-8 字面量并不总能对上；而配置框切页会重建控件，靠文字找一
 * 旦对不上，整个定位函数就静默返回、什么都不做。属性挂在控件自身，
 * 随控件生死，不依赖任何编码假设。
 */
static wchar_t *cn_w(const char *s)
{
    return dup_mb_to_wc(CP_UTF8, s ? s : "");
}

/*
 * cn: 控件文本一律按宽字符读回，再与源码里的 UTF-8 字面量比较。
 * 用 A 版 GetWindowText 读回来的是进程代码页字节，在 GBK 系统上与
 * UTF-8 字面量根本对不上，这类「按文字找控件」的布局代码就会静默
 * 失效。前缀比较，允许按钮文本带助记符后缀。
 */
static bool cn_text_is(const wchar_t *text, const char *utf8)
{
    wchar_t *w = cn_w(utf8);
    bool match = !wcsncmp(text, w, wcslen(w));
    sfree(w);
    return match;
}

static HWND cn_find_button(HWND hwnd, const char *prop, const char *label)
{
    HWND h = NULL;

    while ((h = FindWindowEx(hwnd, h, "Button", NULL)) != NULL)
        if (prop && GetProp(h, prop))
            return h;
    if (label && *label) {
        h = NULL;
        while ((h = FindWindowEx(hwnd, h, "Button", NULL)) != NULL) {
            wchar_t text[64];
            GetWindowTextW(h, text, lenof(text));
            if (cn_text_is(text, label))
                return h;
        }
    }
    return NULL;
}

static void cn_style_snapshot_buttons(HWND hwnd)
{
    HWND hdel, hinit, hlist = NULL, hlog, hnew;
    HWND h = 0;

    hdel = cn_find_button(hwnd, "cn_btn_delete", "删除");
    hinit = cn_find_button(hwnd, "cn_btn_init", "初始化设置");
    hlog = cn_find_button(hwnd, "cn_btn_log", "记录日志");
    hnew = cn_find_button(hwnd, "cn_btn_newfolder", "新目录");
    if (!hdel || !hinit)
        return;

    h = 0;
    while (true) {
        h = FindWindowEx(hwnd, h, "ListBox", NULL);
        if (!h)
            break;
        LONG style = GetWindowLong(h, GWL_STYLE);
        if (style & LBS_NOTIFY) {
            hlist = h;
            break;
        }
    }

    /* Balanced button size: a touch wider than the layout default so
     * 初始化设置 fits on one line (68 px), a touch shorter than the
     * standard height, and safely inside the group frame.
     *
     * cn: 「只做一次」的标记必须挂在按钮自身，不能挂对话框——切页会
     * 销毁并重建面板控件，但对话框 hwnd 不变，标记留在对话框上就会
     * 让重建出来的新按钮跳过这一步，宽度退回布局默认值（68→63），
     * 于是「初始化设置」五个字被截断。挂在按钮上，新按钮没有标记，
     * 自然会重新调整一次；同一按钮被 repin 定时器重复调用时标记已
     * 存在，仍然幂等，不会越缩越小。
     *
     * cn: 复选框（hlog）不参与这套微调——它自带勾选方块，靠 +5/-2 的
     * 按钮式缩放只会把文字挤歪。它按布局给的原尺寸铺满该列即可。 */
    {
        HWND btns[3] = { hnew, hdel, hinit };
        for (int i = 0; i < 3; i++) {
            RECT r;
            if (!btns[i])
                continue;
            if (GetProp(btns[i], "cn_btn_styled"))
                continue;
            SetProp(btns[i], "cn_btn_styled", (HANDLE)1);
            GetWindowRect(btns[i], &r);
            POINT tl = { r.left, r.top }, br = { r.right, r.bottom };
            ScreenToClient(hwnd, &tl);
            ScreenToClient(hwnd, &br);
            SetWindowPos(btns[i], NULL, tl.x, tl.y, br.x - tl.x + 5,
                         br.y - tl.y - 2, SWP_NOZORDER);
        }
    }

    /* Pin 删除 to the top of the history list box (next to the
     * entries it acts on), 初始化设置 to the bottom. */
    if (hlist) {
        RECT rl, rd, ri;
        GetWindowRect(hlist, &rl);
        GetWindowRect(hdel, &rd);
        GetWindowRect(hinit, &ri);
        /*
         * cn: 首次执行时列表高度尚未终态（自绘行高随后还会把它缩一行
         * 增量），此时 pin 出来的位置会偏高。挂一个一次性定时器在窗口
         * 完全稳定后重调一次（幂等）；终态后这一次才是有效定位。
         */
        SetTimer(hwnd, 1231, 50, NULL);
        POINT p1 = { rd.left, rd.top }, p2 = { ri.left, ri.top };
        ScreenToClient(hwnd, &p1);
        ScreenToClient(hwnd, &p2);
        int dy_top = rl.top - rd.top;
        int del_bottom;
        int dy_bottom = rl.bottom - (ri.bottom - ri.top) - ri.top;

        if (hnew) {
            /*
             * cn: 「新目录」压在列表顶、「删除」退到它正下方 —— 主人
             * 指定新目录在删除上方。两者同列同宽，间距 4px 贴合这一列
             * 原本的视觉密度。
             */
            RECT rn;
            GetWindowRect(hnew, &rn);
            SetWindowPos(hnew, NULL, p1.x, p1.y + dy_top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
            del_bottom = p1.y + dy_top + (rn.bottom - rn.top) + 4;
            SetWindowPos(hdel, NULL, p1.x, del_bottom, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
            del_bottom += rd.bottom - rd.top;
        } else {
            SetWindowPos(hdel, NULL, p1.x, p1.y + dy_top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
            del_bottom = p1.y + dy_top + (rd.bottom - rd.top);
        }
        SetWindowPos(hinit, NULL, p2.x, p2.y + dy_bottom, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
        /*
         * cn: 「记录日志」夹在两者正中间。删除已贴列表顶（有「新目录」
         * 时它下面再压一个按钮）、初始化设置贴列表底，取两者中间的空档
         * 放中点即可。高度都用各自 rect 的原始尺寸算（SetWindowPos 不
         * 改高度，所以屏幕坐标差就等于客户区高度差）。
         */
        if (hlog) {
            RECT rlog;
            POINT pl = { 0, 0 };
            int init_top = p2.y + dy_bottom;
            int gap, hh;

            GetWindowRect(hlog, &rlog);
            pl.x = rlog.left;
            ScreenToClient(hwnd, &pl);
            hh = rlog.bottom - rlog.top;
            gap = (init_top - del_bottom) - hh;
            if (gap < 0)
                gap = 0;
            SetWindowPos(hlog, NULL, pl.x, del_bottom + gap / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
        }

        /*
         * cn: 组框底边收紧到「列表底 + 5px」——布局 ypos 给组框留的
         * 空隙偏大（历史遗留约 20px），主人指定收窄到 5px。
         */
        HWND hgrp = NULL;
        h = 0;
        while ((h = FindWindowEx(hwnd, h, "Button", NULL)) != NULL) {
            wchar_t text[64];
            GetWindowTextW(h, text, lenof(text));
            if (cn_text_is(text, "已保存的会话")) {
                hgrp = h;
                break;
            }
        }
        if (hgrp) {
            RECT rg;
            GetWindowRect(hgrp, &rg);
            POINT gp = { rg.left, rg.top }, gb = { rg.right, rg.bottom };
            ScreenToClient(hwnd, &gp);
            ScreenToClient(hwnd, &gb);
            /* 目标底边 = 列表底（转客户区坐标）+ 5px；宽度沿用原值。 */
            POINT lb = { rl.left, rl.bottom };
            ScreenToClient(hwnd, &lb);
            int cn_h = (lb.y + 5) - gp.y;
            SetWindowPos(hgrp, NULL, gp.x, gp.y, gb.x - gp.x,
                         cn_h, SWP_NOZORDER);

            /*
             * cn: 底部标准按钮行（保存／文件传输／连接／退出）整体上移，
             * 与组框底的空隙收到 10px（布局体系给的距离偏大）。绝对
             * 定位（y = 组框底 + 10），重复调用幂等。
             */
            h = 0;
            while ((h = FindWindowEx(hwnd, h, "Button", NULL)) != NULL) {
                wchar_t text[64];
                GetWindowTextW(h, text, lenof(text));
                if (cn_text_is(text, "保存") ||
                    cn_text_is(text, "文件传输") ||
                    cn_text_is(text, "连接") ||
                    cn_text_is(text, "退出")) {
                    RECT rb;
                    if (!GetWindowRect(h, &rb))
                        continue;
                    POINT bp = { rb.left, rb.top };
                    ScreenToClient(hwnd, &bp);
                    /* 组框底 = 列表底+5；按钮行 y = 组框底 + 10。 */
                    SetWindowPos(h, NULL, bp.x, lb.y + 15, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER);
                }
            }
        }

        /*
         * cn: 窗口底边收到「按钮行底 + 10px」——对话框模板给的高度
         * 偏大，主人在按钮行上移后要求底部留白同步收窄。
         */
        {
            HWND hsave = NULL;
            h = 0;
            while ((h = FindWindowEx(hwnd, h, "Button", NULL)) != NULL) {
                wchar_t text[64];
                GetWindowTextW(h, text, lenof(text));
                if (cn_text_is(text, "保存")) {
                    hsave = h;
                    break;
                }
            }
            if (hsave) {
                RECT rb, rw;
                GetWindowRect(hsave, &rb);
                GetWindowRect(hwnd, &rw);
                int cn_newh = (rb.bottom + 10) - rw.top;
                if (rw.bottom - rw.top > cn_newh)
                    SetWindowPos(hwnd, NULL, 0, 0, rw.right - rw.left,
                                 cn_newh, SWP_NOMOVE | SWP_NOZORDER);
            }
        }

        /*
         * cn: 「载入密钥」不再在这里定位。它现在由会话页的列布局摆到
         * 「凭据（密码）」框右侧，属于那一个 controlset 的成员；从这个
         * 函数里再动它会把两套定位打架，按钮会跑到老位置上。
         */
    }
}

/*
 * cn: Save-confirmation prompt for the save button, showing the list
 * of changes about to be persisted. Returns true to go ahead.
 */
bool dlg_confirm_save(dlgparam *dp)
{
    char *changes = conf_describe_changes(dp->baseline, dp->data);
    /* cn: 自绘弹窗（照「确认覆盖」画法），首行文案在模板内顶格。 */
    int mbret = cn_confirm_save_dialog(dp->hwnd, changes);
    sfree(changes);
    return mbret == IDOK;
}

/*
 * cn: generic change-list confirmation with a caller-supplied title
 * and list (used by the settings-snapshot restore).
 */
bool dlg_confirm_changes(dlgparam *dp, const char *title,
                         const char *changes)
{
    int mbret = message_box(dp->hwnd, changes, title,
                            MB_OKCANCEL | MB_ICONQUESTION, true, 0);
    return mbret == IDOK;
}

/*
 * cn: 「退出」按钮与标题栏 X 共用的未保存改动确认。
 *
 * 两者走同一套判据、同一个自绘弹窗（与「确认保存」同源），避免两条
 * 退出路径给出不同界面、不同答案 —— 这正是主人截图指出的问题：
 * 点 X 弹的是旧的 IDD_SAVECHANGES 模板（三个按钮、另一套字号），
 * 点「退出」弹的是通用 message_box，两处长得完全不一样。
 *
 * 三个按钮，语义与旧模板一一对应：
 *   直接退出（左，红框）→ CN_QUIT_DISCARD，放弃改动关窗；
 *   取消（中）          → CN_QUIT_STAY，留在配置框里；
 *   保存并退出（右，蓝框默认）→ CN_QUIT_SAVE，落盘后关窗。
 *
 * 清单为空（本来就没改动）直接返回 CN_QUIT_DISCARD，一次弹窗都不出。
 */
CnQuitChoice dlg_confirm_quit(dlgparam *dp)
{
    if (!dlg_has_unsaved_changes(dp))
        return CN_QUIT_DISCARD;

    char *changes = conf_describe_changes(dp->baseline, dp->data);
    /* cn: 文案取自 cn_quit_texts（唯一定义处），与看图入口同源。 */
    int mbret = cn_confirm_dialog_texts(dp->hwnd, changes, NULL, &cn_quit_texts,
                                        NULL);
    sfree(changes);

    if (mbret == IDYES)
        return CN_QUIT_SAVE;           /* 右按钮「保存并退出」 */
    if (mbret == IDOK)
        return CN_QUIT_DISCARD;        /* 左按钮「直接退出」 */
    return CN_QUIT_STAY;               /* 中按钮「取消」/ ❌ / Esc */
}

/*
 * cn: 点「连接」或「文件传输」前的改动确认。
 *
 * 清单为空（没改过任何配置）就直接放行，一次弹窗都不出现 —— 大多数
 * 用户就是「打开、填个地址、连接」，不该被拦一道。
 *
 * 有改动才弹，复用「确认保存」那套自绘弹窗，只换文案：
 *   左按钮「仅本次生效」→ 改动跟着这次连接走，不落盘；
 *   右按钮「长期生效」  → 改动写进注册表（默认按钮，蓝框）；
 *   右上角 ❌           → 什么都不做，连弹窗带连接一起取消。
 *
 * ⚠️ 本函数只负责问，不负责启动连接 —— 调用方拿到 CN_LAUNCH_ONCE /
 * CN_LAUNCH_PERSIST 后必须继续原定的操作，避免「弹了框却没连」。
 */
CnLaunchChoice dlg_confirm_launch(dlgparam *dp)
{
    if (!dlg_has_unsaved_changes(dp))
        return CN_LAUNCH_GO;

    char *changes = conf_describe_changes(dp->baseline, dp->data);
    /* cn: 文案取自 cn_launch_texts（唯一定义处），与看图入口同源。 */
    bool closed_by_x = false;
    int mbret = cn_confirm_dialog_texts(dp->hwnd, changes, NULL, &cn_launch_texts,
                                        &closed_by_x);
    sfree(changes);

    if (mbret == IDOK)
        return CN_LAUNCH_ONCE;         /* 左按钮「仅本次生效」 */
    if (closed_by_x)
        return CN_LAUNCH_CANCEL;       /* ❌：只关弹窗，不连接 */
    return CN_LAUNCH_PERSIST;          /* 右按钮「长期生效」 */
}

static INT_PTR pds_default_dlgproc(PortableDialogStuff *pds, HWND hwnd,
                                   UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_LBUTTONUP:
        /*
         * Button release should trigger WM_OK if there was a
         * previous double click on the host CA list.
         */
        ReleaseCapture();
        if (pds->dp->ended)
            ShinyEndDialog(hwnd, pds->dp->endresult ? 1 : 0);
        break;
      case WM_COMMAND:
      case WM_DRAWITEM:
      default: {                       /* also handle drag list msg here */
        /*
         * Only process WM_COMMAND once the dialog is fully formed.
         */
        int ret;
        if (pds->initialised) {
            ret = winctrl_handle_command(pds->dp, msg, wParam, lParam);
            if (pds->dp->ended && GetCapture() != hwnd)
                ShinyEndDialog(hwnd, pds->dp->endresult ? 1 : 0);
        } else
            ret = 0;
        return ret;
      }
      case WM_HELP:
        if (!winctrl_context_help(pds->dp,
                                  hwnd, ((LPHELPINFO)lParam)->iCtrlId))
            MessageBeep(0);
        break;
      case WM_CLOSE:
        /*
         * cn: 标题栏 X 与「退出」按钮现在走同一个确认函数、同一个自绘
         * 弹窗（三个按钮：直接退出 / 取消 / 保存并退出）。此前这里是
         * 另一套旧的 IDD_SAVECHANGES 模板，两处界面长得完全不一样 ——
         * 主人截图对比后要求统一，故删掉旧模板，只留这一条路径。
         */
        if (dlg_has_unsaved_changes(pds->dp)) {
            switch (dlg_confirm_quit(pds->dp)) {
              case CN_QUIT_STAY:
                return TRUE;           /* 取消：留在配置框里 */
              case CN_QUIT_SAVE:
                quit_help(hwnd);
                if (pds->dp->apply_cb) {
                    /* Mid-session box: apply the settings in place. */
                    if (dlg_apply_config(pds->dp))
                        ShinyEndDialog(hwnd, 1);
                    else
                        return TRUE;
                } else {
                    /* Startup box: persist to the registry, using the
                     * same protections as the save button. */
                    char *errmsg = cn_persist_default_settings(
                        (Conf *)pds->dp->data);
                    if (errmsg) {
                        sfree(errmsg);
                        return TRUE;   /* stay so the user can retry */
                    }
                    ShinyEndDialog(hwnd, 1);
                }
                return TRUE;
              case CN_QUIT_DISCARD:
              default:
                quit_help(hwnd);
                ShinyEndDialog(hwnd, 0);   /* discard, then exit */
                return TRUE;
            }
        }
        quit_help(hwnd);
        ShinyEndDialog(hwnd, 0);
        return 0;

        /* Grrr Explorer will maximize Dialogs! */
      case WM_SIZE:
        if (wParam == SIZE_MAXIMIZED)
            force_normal(hwnd);
        return 0;

    }
    return 0;
}

static void pds_initdialog_start(PortableDialogStuff *pds, HWND hwnd)
{
    pds->dp->hwnd = hwnd;

    if (pds->dp->wintitle) {   /* apply override title, if provided */
        wchar_t *wtitle = cn_w(pds->dp->wintitle);
        SetWindowTextW(hwnd, wtitle);
        sfree(wtitle);
    }

    /*
     * The title bar deliberately carries no context-help "?" button;
     * the dialog templates don't request one, and we make sure the
     * window style doesn't add one either. The help file (if any) is
     * still reachable through the normal Help menu.
     */
    {
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, style & ~WS_EX_CONTEXTHELP);
    }
    {
        /* If the dialog template provided a top-level Help button,
         * delete it. */
        HWND item = GetDlgItem(hwnd, IDC_HELPBTN);
        if (item)
            DestroyWindow(item);
    }
}

/* ----------------------------------------------------------------------
 * cn: 会话页「主机名称」下拉框隐藏系统下拉箭头。
 *
 * 交互不变：点击原箭头区域（以及框内任意处）仍展开历史列表——系统
 * 命中测试未动，只是箭头像素被窗口背景色盖掉。COMBOBOX 的下拉按钮
 * 不是独立子窗口而是系统自绘，无法单独隐藏，只能子类化后在默认
 * 绘制完成之后用 COLOR_WINDOW 把 rcButton 区域盖住；主题会随鼠标
 * 悬停/焦点变化重画按钮，所以这些消息在默认处理后都要再补一次盖绘。
 */

#define CN_HOSTBOX_SUBCLASS_ID 20261107

/*
 * cn: 网络模式下主机框的下拉列表保持为空（不灌历史），此时整个
 * 下拉交互视为不存在：箭头像素盖掉之外，按钮区的鼠标消息与
 * F4/下箭头键也要吞掉，否则点击/按键仍会弹出空列表框。
 * 串口模式下列表非空（端口选择器），一切照常放行。
 */
static bool cn_hostbox_dropdown_dead(HWND hcmb)
{
    return SendMessage(hcmb, CB_GETCOUNT, 0, 0) == 0;
}

static void cn_hostbox_cover_arrow(HWND hcmb)
{
    COMBOBOXINFO cbi;
    HDC hdc;
    RECT rc;

    if (!cn_hostbox_dropdown_dead(hcmb))
        return;
    memset(&cbi, 0, sizeof(cbi));
    cbi.cbSize = sizeof(cbi);
    if (!GetComboBoxInfo(hcmb, &cbi))
        return;
    hdc = GetDC(hcmb);
    if (!hdc)
        return;
    rc = cbi.rcButton;                  /* 客户区坐标 */
    rc.left -= 2;                       /* 吃掉按钮左侧的分隔竖线 */
    FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
    ReleaseDC(hcmb, hdc);
}

/*
 * cn: 按「列表是否为空」重新同步箭头像素。
 *
 * 盖绘是破坏性的 FillRect，一旦盖上就没有自动还原的路径 —— 列表
 * 由空变非空时（串口模式灌入波特率档位）必须主动让系统重画按钮区，
 * 否则箭头会一直缺到鼠标划过、主题重画为止。波特率下拉框「初始
 * 不显示箭头、鼠标移过才出现」就是这个原因。
 */
static void cn_hostbox_sync_arrow(HWND hcmb)
{
    if (cn_hostbox_dropdown_dead(hcmb)) {
        cn_hostbox_cover_arrow(hcmb);
    } else {
        COMBOBOXINFO cbi;

        memset(&cbi, 0, sizeof(cbi));
        cbi.cbSize = sizeof(cbi);
        if (GetComboBoxInfo(hcmb, &cbi))
            InvalidateRect(hcmb, &cbi.rcButton, true);
    }
}

static LRESULT CALLBACK cn_hostbox_subproc(HWND hwnd, UINT msg,
                                           WPARAM wp, LPARAM lp,
                                           UINT_PTR id, DWORD_PTR ref)
{
    /*
     * 网络模式（空列表）下吞掉一切能触发下拉的输入：按钮区域的
     * 鼠标按下，以及 F4 / 下箭头键。吞在默认处理之前，系统根本
     * 收不到展开指令。
     */
    /*
     * cn: ⚠️ 防递归——dropdown_dead 的 CB_GETCOUNT 会派发回本过程，
     * 不在这里短路的话就是无限递归炸栈（02:38 版启动即死的根因）。
     * CB_GETCOUNT 对 combobox 的默认实现是纯查询，直接透传。
     */
    if (msg == CB_GETCOUNT)
        return DefSubclassProc(hwnd, msg, wp, lp);

    if (cn_hostbox_dropdown_dead(hwnd)) {
        switch (msg) {
          case WM_LBUTTONDOWN:
          case WM_LBUTTONDBLCLK: {
            /*
             * cn: 坐标从 lParam 解析（客户区坐标）。不能用
             * GetMessagePos——SendMessage 伪造消息时它反映的是
             * 上一次队列消息的位置，与本次 lp 无关（02:40 版拦截
             * 打空的根因）。
             */
            POINT pt;
            COMBOBOXINFO cbi;
            pt.x = (short)(lp & 0xFFFF);
            pt.y = (short)((lp >> 16) & 0xFFFF);
            memset(&cbi, 0, sizeof(cbi));
            cbi.cbSize = sizeof(cbi);
            if (GetComboBoxInfo(hwnd, &cbi) &&
                PtInRect(&cbi.rcButton, pt))
                return 0;               /* 按钮区：吞掉，不展开 */
            break;
          }
          case WM_KEYDOWN:
          case WM_SYSKEYDOWN:
            if (wp == VK_DOWN || wp == VK_F4)
                return 0;               /* 键盘展开：吞掉 */
            break;
        }
    }

    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);

    /*
     * cn: 列表内容变化后立刻按新的空/非空状态同步箭头。
     *
     * 这两条消息来自 dlg_listbox_add / dlg_listbox_clear。串口模式的
     * 波特率档位是在控件创建、子类装好之后才灌进去的，而
     * dlg_update_done 只对 CTRL_LISTBOX 调 InvalidateRect，对 combo
     * box 不管，所以这里不补一下，被盖掉的箭头就没有还原的机会。
     */
    switch (msg) {
      case CB_ADDSTRING:
      case CB_DELETESTRING:
      case CB_RESETCONTENT:
        cn_hostbox_sync_arrow(hwnd);
        return r;
    }

    /*
     * 默认绘制完成后再盖一次箭头：主题会在悬停/焦点变化时重画
     * 按钮，不补盖的话箭头会在鼠标划过时闪现。
     */
    switch (msg) {
      case WM_PAINT:
      case WM_MOUSEMOVE:
      case WM_LBUTTONUP:
      case WM_SETFOCUS:
      case WM_KILLFOCUS:
      case WM_THEMECHANGED:
        cn_hostbox_cover_arrow(hwnd);
        break;
    }
    return r;
}

static void cn_hostbox_subclass_tree(PortableDialogStuff *pds, size_t tree)
{
    struct winctrl *c;

    for (int i = 0; (c = winctrl_findbyindex(&pds->ctrltrees[tree], i)) != NULL;
         i++) {
        HWND hcmb;

        if (!c->ctrl || c->ctrl->type != CTRL_EDITBOX ||
            !c->ctrl->editbox.has_list)
            continue;
        hcmb = GetDlgItem(pds->dp->hwnd, c->base_id + 1);
        if (!hcmb || GetPropW(hcmb, L"cn_hostbox_arrow_hidden"))
            continue;
        SetWindowSubclass(hcmb, cn_hostbox_subproc,
                          CN_HOSTBOX_SUBCLASS_ID, 0);
        SetPropW(hcmb, L"cn_hostbox_arrow_hidden", (HANDLE)1);
        cn_hostbox_cover_arrow(hcmb);
    }
}

/*
 * Create the panelfuls of controls in the configuration box.
 */
static void pds_create_controls(
    PortableDialogStuff *pds, size_t which_tree, int base_id,
    int left, int right, int top, char *path)
{
    struct ctlpos cp;

    ctlposinit(&cp, pds->dp->hwnd, left, right, top);

    for (int index = -1; (index = ctrl_find_path(
                              pds->ctrlbox, path, index)) >= 0 ;) {
        struct controlset *s = pds->ctrlbox->ctrlsets[index];
        winctrl_layout(pds->dp, &pds->ctrltrees[which_tree], &cp, s, &base_id);
    }
}

static void pds_initdialog_finish(PortableDialogStuff *pds)
{
    /*
     * Set focus into the first available control in ctrltree #0,
     * which the caller was expected to set up to be the one
     * containing the dialog controls likely to be used first.
     */
    struct winctrl *c;
    for (int i = 0; (c = winctrl_findbyindex(&pds->ctrltrees[0], i)) != NULL;
         i++) {
        if (c->ctrl) {
            dlg_set_focus(c->ctrl, pds->dp);
            break;
        }
    }

    /*
     * Now we've finished creating our initial set of controls,
     * it's safe to actually show the window without risking setup
     * flicker.
     */
    dlg_update_apply_buttons(pds->dp);
    /* cn: startup box — shrink the Delete / 设置初始化 buttons and pin
     * them to the bottom of the session list box. */
    if (!pds->dp->apply_cb)
        cn_style_snapshot_buttons(pds->dp->hwnd);

    /*
     * cn: 装好设置页跳转钩子，供 config.c 里「载入密钥」之类的
     * 按钮把用户带到指定页。active_config_box 让回调能找到当前
     * 对话框（见 config_box_navigate_to_path）。
     */
    active_config_box = pds;
    pds->dp->navigate_to_path = config_box_navigate_to_path;
    pds->dp->focus_treeview = config_box_focus_treeview;

    ShowWindow(pds->dp->hwnd, SW_SHOWNORMAL);

    pds->initialised = true;
}

#define LOGEVENT_INITIAL_MAX 128
#define LOGEVENT_CIRCULAR_MAX 128

static char *events_initial[LOGEVENT_INITIAL_MAX];
static char *events_circular[LOGEVENT_CIRCULAR_MAX];
static int ninitial = 0, ncircular = 0, circular_first = 0;

#define PRINTER_DISABLED_STRING "无（禁止打印）"

void force_normal(HWND hwnd)
{
    static bool recurse = false;

    WINDOWPLACEMENT wp;

    if (recurse)
        return;
    recurse = true;

    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd, &wp) && wp.showCmd == SW_SHOWMAXIMIZED) {
        wp.showCmd = SW_SHOWNORMAL;
        SetWindowPlacement(hwnd, &wp);
    }
    recurse = false;
}

static char *getevent(int i)
{
    if (i < ninitial)
        return events_initial[i];
    if ((i -= ninitial) < ncircular)
        return events_circular[(circular_first + i) % LOGEVENT_CIRCULAR_MAX];
    return NULL;
}

static HWND logbox;
HWND event_log_window(void) { return logbox; }

static void update_logbox_horizontal_extent(HWND logbox)
{
    /* Find the width of every entry in the current Event Log, and set
     * the horizontal scrollbar so that you can scroll right to see
     * all of them. */
    HWND listbox = GetDlgItem(logbox, IDN_LIST);
    HDC hdc = GetDC(listbox);
    int count = SendMessageW(listbox, LB_GETCOUNT, 0, 0);
    WPARAM maxwidth = 0;
    for (int i = 0; i < count; i++) {
        int len = (int)SendMessageW(listbox, LB_GETTEXTLEN, i, 0);
        wchar_t *wtxt = snewn(len + 1, wchar_t);
        SendMessageW(listbox, LB_GETTEXT, i, (LPARAM)wtxt);
        SIZE size;
        if (GetTextExtentPointW(hdc, wtxt, len, &size)) {
            if (maxwidth < size.cx)
                maxwidth = size.cx;
        }
        sfree(wtxt);
    }
    ReleaseDC(listbox, hdc);

    /*
     * This doesn't seem to set _exactly_ the right width. If I scroll
     * the scrollbar right as far as it will go, I find I end up with
     * a bit of extra space on the right of the longest piece of text.
     * I don't know why that is.
     *
     * Testing with debug(), GetTextExtentPoint seems to be correctly
     * returning the exact width in pixels of each log entry's text.
     * And the docs for LB_SETHORIZONTALEXTENT say that it wants a
     * width in pixels too. So I don't think it can be the usual
     * problem of confusing logical dialog units and pixels. (In any
     * case, on the high-DPI display where I tested, the difference
     * between those is more than a factor of 2, whereas I'm seeing a
     * discrepancy of only about 10%.)
     */
    SendMessage(listbox, LB_SETHORIZONTALEXTENT, maxwidth, 0);
}

static INT_PTR CALLBACK LogProc(HWND hwnd, UINT msg,
                                WPARAM wParam, LPARAM lParam)
{
    int i;

    switch (msg) {
      case WM_INITDIALOG: {
        char *str = dupprintf("%s 事件日志", appname);
        wchar_t *wstr = cn_w(str);
        SetWindowTextW(hwnd, wstr);
        sfree(wstr);
        sfree(str);

        static int tabs[4] = { 78, 108 };
        SendDlgItemMessage(hwnd, IDN_LIST, LB_SETTABSTOPS, 2,
                           (LPARAM) tabs);

        for (i = 0; i < ninitial; i++) {
            wchar_t *w = cn_w(events_initial[i]);
            SendDlgItemMessageW(hwnd, IDN_LIST, LB_ADDSTRING, 0, (LPARAM) w);
            sfree(w);
        }
        for (i = 0; i < ncircular; i++) {
            wchar_t *w = cn_w(
                events_circular[(circular_first + i) % LOGEVENT_CIRCULAR_MAX]);
            SendDlgItemMessageW(hwnd, IDN_LIST, LB_ADDSTRING, 0, (LPARAM) w);
            sfree(w);
        }
        update_logbox_horizontal_extent(hwnd);

        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            logbox = NULL;
            SetActiveWindow(GetParent(hwnd));
            DestroyWindow(hwnd);
            return 0;
          case IDN_COPY:
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                int selcount;
                int *selitems;
                selcount = SendDlgItemMessage(hwnd, IDN_LIST,
                                              LB_GETSELCOUNT, 0, 0);
                if (selcount == 0) {   /* don't even try to copy zero items */
                    MessageBeep(0);
                    break;
                }

                selitems = snewn(selcount, int);
                if (selitems) {
                    int count = SendDlgItemMessage(hwnd, IDN_LIST,
                                                   LB_GETSELITEMS,
                                                   selcount,
                                                   (LPARAM) selitems);
                    static const unsigned char sel_nl[] = SEL_NL;

                    if (count == 0) {  /* can't copy zero stuff */
                        MessageBeep(0);
                        break;
                    }

                    strbuf *sb = strbuf_new();
                    for (int i = 0; i < count; i++) {
                        char *q = getevent(selitems[i]);
                        put_datapl(sb, ptrlen_from_asciz(q));
                        put_data(sb, sel_nl, sizeof(sel_nl));
                    }
                    write_aclip(hwnd, CLIP_SYSTEM, sb->s, sb->len);
                    strbuf_free(sb);
                    sfree(selitems);

                    for (i = 0; i < (ninitial + ncircular); i++)
                        SendDlgItemMessage(hwnd, IDN_LIST, LB_SETSEL,
                                           false, i);
                }
            }
            return 0;
        }
        return 0;
      case WM_CLOSE:
        logbox = NULL;
        SetActiveWindow(GetParent(hwnd));
        DestroyWindow(hwnd);
        return 0;
    }
    return 0;
}

static INT_PTR CALLBACK LicenceProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_INITDIALOG: {
        char *str = dupprintf("%s 许可证", appname);
        wchar_t *wstr = cn_w(str);
        SetWindowTextW(hwnd, wstr);
        sfree(wstr);
        sfree(str);
        wchar_t *wlic = cn_w(LICENCE_TEXT("\r\n\r\n"));
        SetDlgItemTextW(hwnd, IDA_TEXT, wlic);
        sfree(wlic);
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            EndDialog(hwnd, 1);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        EndDialog(hwnd, 1);
        return 0;
    }
    return 0;
}

static INT_PTR CALLBACK AboutProc(HWND hwnd, UINT msg,
                                  WPARAM wParam, LPARAM lParam)
{
    char *str;

    switch (msg) {
      case WM_INITDIALOG: {
        str = dupprintf("关于 %s", appname);
        wchar_t *wstr = cn_w(str);
        SetWindowTextW(hwnd, wstr);
        sfree(wstr);
        sfree(str);
        char *buildinfo_text = buildinfo("\r\n");
        char *text = dupprintf(
            "%s\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s",
            appname, ver, buildinfo_text,
            "(C) " SHORT_COPYRIGHT_DETAILS ". 保留所有权利。");
        sfree(buildinfo_text);
        wchar_t *wtext = cn_w(text);
        SetDlgItemTextW(hwnd, IDA_TEXT, wtext);
        sfree(wtext);
        MakeDlgItemBorderless(hwnd, IDA_TEXT);
        sfree(text);
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            EndDialog(hwnd, true);
            return 0;
          case IDA_LICENCE:
            EnableWindow(hwnd, 0);
            DialogBox(hinst, MAKEINTRESOURCE(IDD_LICENCEBOX),
                      hwnd, LicenceProc);
            EnableWindow(hwnd, 1);
            SetActiveWindow(hwnd);
            return 0;

          case IDA_WEB:
            /* Load web browser */
            ShellExecute(hwnd, "open",
                         "https://www.chiark.greenend.org.uk/~sgtatham/putty/",
                         0, 0, SW_SHOWDEFAULT);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        EndDialog(hwnd, true);
        return 0;
    }
    return 0;
}

/*
 * Null dialog procedure.
 */
static INT_PTR CALLBACK NullDlgProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    return 0;
}

enum {
    IDCX_ABOUT = IDC_ABOUT,
    IDCX_TVSTATIC,
    IDCX_TREEVIEW,
    IDCX_STDBASE,
    IDCX_PANELBASE = IDCX_STDBASE + 32
};

struct treeview_faff {
    HWND treeview;
    HTREEITEM lastat[4];
};

static HTREEITEM treeview_insert(struct treeview_faff *faff,
                                 int level, char *text, char *path)
{
    /* cn: 树的文本走 W 接口。pszText 必须按 UTF-8 解码成宽字符，
     * 否则 A 接口会拿进程代码页(GBK)去解 UTF-8 字节，整棵树乱码。 */
    TVINSERTSTRUCTW ins;
    int i;
    HTREEITEM newitem;
    wchar_t *wtext = cn_w(text);
    ins.hParent = (level > 0 ? faff->lastat[level - 1] : TVI_ROOT);
    ins.hInsertAfter = faff->lastat[level];
#if _WIN32_IE >= 0x0400 && defined NONAMELESSUNION
#define INSITEM DUMMYUNIONNAME.item
#else
#define INSITEM item
#endif
    ins.INSITEM.mask = TVIF_TEXT | TVIF_PARAM;
    ins.INSITEM.pszText = wtext;
    ins.INSITEM.cchTextMax = 0;
    ins.INSITEM.lParam = (LPARAM)path;
    newitem = (HTREEITEM)SendMessageW(faff->treeview, TVM_INSERTITEMW,
                                      0, (LPARAM)&ins);
    sfree(wtext);
    if (level > 0)
        TreeView_Expand(faff->treeview, faff->lastat[level - 1],
                        (level > 1 ? TVE_COLLAPSE : TVE_EXPAND));
    faff->lastat[level] = newitem;
    for (i = level + 1; i < 4; i++)
        faff->lastat[i] = NULL;
    return newitem;
}

Filename *dialog_box_demo_screenshot_filename = NULL;

/*
 * cn: 演示截图要落的页面路径，来自 PUTTY_CN_DEMO_PANEL。
 * 空/NULL 表示保持默认的第一页。
 */
static char *demo_screenshot_panel = NULL;

/*
 * cn: 在树控件里按 pathname 找对应项。树项的 lParam 保存的正是
 * ctrl_settitle / ctrl_getset 用过的 pathname 字符串，所以深度
 * 优先比对即可，无需拼装父节点文字。
 */
static HTREEITEM treeview_find_by_path(HWND treeview, HTREEITEM item,
                                       const char *pathname)
{
    while (item) {
        TVITEMW tv;
        tv.mask = TVIF_PARAM | TVIF_HANDLE;
        tv.hItem = item;
        if (SendMessageW(treeview, TVM_GETITEMW, 0, (LPARAM)&tv)) {
            if (tv.lParam && !strcmp((const char *)tv.lParam, pathname))
                return item;

            HTREEITEM child = TreeView_GetChild(treeview, item);
            if (child) {
                HTREEITEM hit = treeview_find_by_path(treeview, child,
                                                      pathname);
                if (hit)
                    return hit;
            }
        }
        item = TreeView_GetNextSibling(treeview, item);
    }
    return NULL;
}

/*
 * cn: 由 config.c 的处理器请求「跳到某一页」。选中树项会触发
 * TVN_SELCHANGED，后者负责销毁旧面板、创建新面板，因此这里只需
 * 把选中项挪过去。
 *
 * 配置框是模态且进程内单实例的，用一个文件级静态指针即可定位当前
 * 那个对话框；离开时清空，避免指向已销毁的窗口。
 */
static bool config_box_navigate_to_path(dlgparam *dp, const char *pathname)
{
    PortableDialogStuff *pds = active_config_box;
    HWND treeview;
    HTREEITEM hit;

    if (!pds || pds->dp != dp || !dp->hwnd)
        return false;
    if (!pds->initialised)
        return false;

    treeview = GetDlgItem(dp->hwnd, IDCX_TREEVIEW);
    if (!treeview)
        return false;

    hit = treeview_find_by_path(treeview, TreeView_GetRoot(treeview),
                                pathname);
    if (!hit)
        return false;

    if (TreeView_GetSelection(treeview) == hit)
        return true;            /* 已经在这一页上了 */

    TreeView_SelectItem(treeview, hit);
    TreeView_EnsureVisible(treeview, hit);
    return true;
}

/*
 * cn: 把焦点交还给左侧分类树。
 *
 * 用途：复选框勾选后，系统会在按钮文字周围画一圈虚线焦点框，观感上
 * 像「文字被选中」而不是「选项被勾上」。切页时上游本来就执行
 * SetFocus(treeview)（见 TVN_SELCHANGED 分支），虚线随之消失；这里
 * 复用同一个落点，让勾选后的观感与切页一致，且不改变任何既有行为。
 */
static void config_box_focus_treeview(dlgparam *dp)
{
    PortableDialogStuff *pds = active_config_box;
    HWND treeview;

    if (!pds || pds->dp != dp || !dp->hwnd)
        return;

    treeview = GetDlgItem(dp->hwnd, IDCX_TREEVIEW);
    if (treeview)
        SetFocus(treeview);
}

/* ctrltrees indices for the main dialog box */
enum {
    TREE_PANEL, /* things we swap out every time treeview selects a new pane */
    TREE_BASE, /* fixed things at the bottom like OK and Cancel buttons */
};

/*
 * This function is the configuration box.
 * (Being a dialog procedure, in general it returns 0 if the default
 * dialog processing should be performed, and 1 if it should not.)
 */
static INT_PTR GenericMainDlgProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam, void *ctx)
{
    PortableDialogStuff *pds = (PortableDialogStuff *)ctx;
    const int DEMO_SCREENSHOT_TIMER_ID = 1230;
    HWND treeview;
    struct treeview_faff tvfaff;

    switch (msg) {
      case WM_CLOSE:
        /* 关闭事件必须显式转发：default 分支只处理控件命令。 */
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);

      case WM_INITDIALOG:
        pds_initdialog_start(pds, hwnd);

        pds_create_controls(pds, TREE_BASE, IDCX_STDBASE, 3, 3, 235, "");
        cn_hostbox_subclass_tree(pds, TREE_BASE);

        SendMessage(hwnd, WM_SETICON, (WPARAM) ICON_BIG,
                    (LPARAM) LoadIcon(hinst, MAKEINTRESOURCE(IDI_CFGICON)));

        centre_window(hwnd);

        /*
         * Create the tree view.
         */
        {
            RECT r;
            WPARAM font;
            HWND tvstatic;

            r.left = 3;
            r.right = r.left + 95;
            r.top = 3;
            r.bottom = r.top + 10;
            MapDialogRect(hwnd, &r);
            {
                wchar_t *wtvstatic = cn_w("分类(&G)：");
                tvstatic = CreateWindowExW(0, L"STATIC", wtvstatic,
                                           WS_CHILD | WS_VISIBLE,
                                           r.left, r.top,
                                           r.right - r.left,
                                           r.bottom - r.top,
                                           hwnd, (HMENU) IDCX_TVSTATIC, hinst,
                                           NULL);
                sfree(wtvstatic);
            }
            font = SendMessage(hwnd, WM_GETFONT, 0, 0);
            SendMessage(tvstatic, WM_SETFONT, font, MAKELPARAM(true, 0));

            r.left = 3;
            r.right = r.left + 95;
            r.top = 13;
            /* cn: 高度 219→202，树底不与左下的「保存(V)」按钮重叠
             * （按钮行已上移到组框底+10，树需同步让位）。 */
            r.bottom = r.top + 202;
            MapDialogRect(hwnd, &r);
            treeview = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE |
                                       WS_TABSTOP | TVS_HASLINES |
                                       TVS_DISABLEDRAGDROP | TVS_HASBUTTONS
                                       | TVS_LINESATROOT |
                                       TVS_SHOWSELALWAYS, r.left, r.top,
                                       r.right - r.left, r.bottom - r.top,
                                       hwnd, (HMENU) IDCX_TREEVIEW, hinst,
                                       NULL);
            font = SendMessage(hwnd, WM_GETFONT, 0, 0);
            SendMessage(treeview, WM_SETFONT, font, MAKELPARAM(true, 0));
            tvfaff.treeview = treeview;
            memset(tvfaff.lastat, 0, sizeof(tvfaff.lastat));
        }

        /*
         * Set up the tree view contents.
         */
        {
            HTREEITEM hfirst = NULL;
            int i;
            char *path = NULL;
            char *firstpath = NULL;

            for (i = 0; i < pds->ctrlbox->nctrlsets; i++) {
                struct controlset *s = pds->ctrlbox->ctrlsets[i];
                HTREEITEM item;
                int j;
                char *c;

                if (!s->pathname[0])
                    continue;
                j = path ? ctrl_path_compare(s->pathname, path) : 0;
                if (j == INT_MAX)
                    continue;          /* same path, nothing to add to tree */

                /*
                 * We expect never to find an implicit path
                 * component. For example, we expect never to see
                 * A/B/C followed by A/D/E, because that would
                 * _implicitly_ create A/D. All our path prefixes
                 * are expected to contain actual controls and be
                 * selectable in the treeview; so we would expect
                 * to see A/D _explicitly_ before encountering
                 * A/D/E.
                 */
                assert(j == ctrl_path_elements(s->pathname) - 1);

                c = strrchr(s->pathname, '/');
                if (!c)
                    c = s->pathname;
                else
                    c++;

                item = treeview_insert(&tvfaff, j, c, s->pathname);
                if (!hfirst) {
                    hfirst = item;
                    firstpath = s->pathname;
                }

                path = s->pathname;
            }

            /*
             * Put the treeview selection on to the first panel in the
             * ctrlbox.
             */
            TreeView_SelectItem(treeview, hfirst);

            /*
             * And create the actual control set for that panel, to
             * match the initial treeview selection.
             */
            assert(firstpath);   /* config.c must have given us _something_ */
            pds_create_controls(pds, TREE_PANEL, IDCX_PANELBASE,
                                100, 3, 13, firstpath);
            dlg_refresh(NULL, pds->dp);    /* and set up control values */
            cn_hostbox_subclass_tree(pds, TREE_PANEL);
        }

        if (dialog_box_demo_screenshot_filename) {
            /*
             * cn: 演示截图默认落在树的第一页；环境变量
             * PUTTY_CN_DEMO_PANEL 可以指定要截的页面路径
             * （形如 "连接/SSH/认证/证书"），用于只核对某一页的
             * 文案排版。取值走宽字符入口，避免中文在 ANSI
             * getenv() 下取成乱码。
             */
            WCHAR wpath[256];
            DWORD n = GetEnvironmentVariableW(L"PUTTY_CN_DEMO_PANEL",
                                              wpath, lenof(wpath));
            if (n > 0 && n < lenof(wpath)) {
                char *utf8 = dup_wc_to_mb(CP_UTF8, wpath, "");
                if (utf8 && *utf8)
                    demo_screenshot_panel = dupstr(utf8);
                sfree(utf8);
            }
        }

        pds_initdialog_finish(pds);

        /*
         * 导航必须在 initdialog_finish 之后：在那之前
         * config_box_navigate_to_path() 会因为 !pds->initialised
         * 直接返回 false，页面不会切过去。定时器也要等导航之后
         * 再挂，否则可能先截到第一页。
         */
        if (dialog_box_demo_screenshot_filename) {
            if (demo_screenshot_panel)
                config_box_navigate_to_path(pds->dp, demo_screenshot_panel);
            SetTimer(hwnd, DEMO_SCREENSHOT_TIMER_ID, TICKSPERSEC, NULL);
        }
        return 0;

      case WM_TIMER:
        if ((UINT_PTR)wParam == 1231) {
            /*
             * cn: 列表高度终态后补一次精确 pin（见 cn_style_snapshot_buttons）。
             * 列表 rect 可能在首次 pin 之后仍被调整（自绘行高等），所以
             * 这里校验按钮与列表的底边是否真的齐平，不齐就再挂一次定时
             * 器重调，直到齐平（上限 5 轮，防意外死循环）。
             */
            HWND cn_list = NULL, cn_init = NULL, h = 0;
            KillTimer(hwnd, 1231);
            if (pds->dp->apply_cb)
                return 0;
            while ((h = FindWindowEx(hwnd, h, "ListBox", NULL)) != NULL) {
                if (GetWindowLong(h, GWL_STYLE) & LBS_NOTIFY) {
                    cn_list = h;
                    break;
                }
            }
            h = 0;
            while ((h = FindWindowEx(hwnd, h, "Button", NULL)) != NULL) {
                wchar_t text[64];
                GetWindowTextW(h, text, lenof(text));
                if (cn_text_is(text, "初始化设置"))
                    cn_init = h;
            }
            if (cn_list && cn_init) {
                RECT rl, ri;
                GetWindowRect(cn_list, &rl);
                GetWindowRect(cn_init, &ri);
                cn_style_snapshot_buttons(hwnd);
                GetWindowRect(cn_list, &rl);
                GetWindowRect(cn_init, &ri);
                if (rl.bottom != ri.bottom && ri.bottom - rl.bottom < 100) {
                    SetTimer(hwnd, 1231, 50, NULL);
                }
            }
            return 0;
        }
        if (dialog_box_demo_screenshot_filename &&
            (UINT_PTR)wParam == DEMO_SCREENSHOT_TIMER_ID) {
            KillTimer(hwnd, DEMO_SCREENSHOT_TIMER_ID);
            char *err = save_screenshot(
                hwnd, dialog_box_demo_screenshot_filename);
            if (err) {
                wchar_t *werr = cn_w(err), *wcap = cn_w("演示截图失败");
                MessageBoxW(hwnd, werr, wcap, MB_OK | MB_ICONERROR);
                sfree(werr);
                sfree(wcap);
                sfree(err);
            }
            ShinyEndDialog(hwnd, 0);
        }
        return 0;

      case WM_NOTIFY:
        if (LOWORD(wParam) == IDCX_TREEVIEW &&
            ((LPNMHDR) lParam)->code == TVN_ITEMEXPANDED) {
            /*
             * cn: 展开分类节点后把滚动条拉到最底，让新展开的子项
             * 立即可见（主人反馈：展开后滚动条不自动跟随）。
             */
            LPNMTREEVIEW nmtv = (LPNMTREEVIEW)lParam;
            if (nmtv->action & TVE_EXPAND) {
                HTREEITEM last = TreeView_GetNextItem(
                    nmtv->hdr.hwndFrom, NULL, TVGN_LASTVISIBLE);
                if (last)
                    TreeView_EnsureVisible(nmtv->hdr.hwndFrom, last);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDCX_TREEVIEW &&
            ((LPNMHDR) lParam)->code == TVN_SELCHANGED) {
            /*
             * Selection-change events on the treeview cause us to do
             * a flurry of control deletion and creation - but only
             * after WM_INITDIALOG has finished. The initial
             * selection-change event(s) during treeview setup are
             * ignored.
             */
            HTREEITEM i;
            TVITEMW item;

            if (!pds->initialised)
                return 0;

            i = TreeView_GetSelection(((LPNMHDR) lParam)->hwndFrom);

            SendMessage (hwnd, WM_SETREDRAW, false, 0);

            /* cn: 这里只用到 lParam（页面 pathname），不必再取文本，
             * 顺带避开 A 版取文本带来的代码页问题。 */
            item.hItem = i;
            item.mask = TVIF_PARAM;
            SendMessageW(((LPNMHDR) lParam)->hwndFrom, TVM_GETITEMW,
                         0, (LPARAM)&item);
            {
                /* Destroy all controls in the currently visible panel. */
                int k;
                HWND item;
                struct winctrl *c;

                while ((c = winctrl_findbyindex(
                            &pds->ctrltrees[TREE_PANEL], 0)) != NULL) {
                    for (k = 0; k < c->num_ids; k++) {
                        item = GetDlgItem(hwnd, c->base_id + k);
                        if (item)
                            DestroyWindow(item);
                    }
                    winctrl_rem_shortcuts(pds->dp, c);
                    winctrl_remove(&pds->ctrltrees[TREE_PANEL], c);
                    sfree(c->data);
                    sfree(c);
                }
            }
            pds_create_controls(pds, TREE_PANEL, IDCX_PANELBASE,
                                100, 3, 13, (char *)item.lParam);

            dlg_refresh(NULL, pds->dp);    /* set up control values */
            cn_hostbox_subclass_tree(pds, TREE_PANEL);

            SendMessage (hwnd, WM_SETREDRAW, true, 0);
            InvalidateRect (hwnd, NULL, true);

            SetFocus(((LPNMHDR) lParam)->hwndFrom);     /* ensure focus stays */

            /* cn: a page switch destroys and recreates the panel
             * controls, which resets the snapshot-button styling —
             * reapply it whenever we land back on the Session page. */
            if (!pds->dp->apply_cb)
                cn_style_snapshot_buttons(hwnd);
        }
        return 0;

      default:
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
    }
}

/*
 * Called by the config box when the user hits Apply in a mid-session
 * reconfigure. If the caller installed an Apply hook, adopt the new
 * settings in place and tell the handler to leave the box open.
 */
bool dlg_apply_config(dlgparam *dp)
{
    if (!dp->apply_cb)
        return false;

    dp->apply_cb(dp->apply_ctx);
    return true;
}

void modal_about_box(HWND hwnd)
{
    EnableWindow(hwnd, 0);
    DialogBox(hinst, MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd, AboutProc);
    EnableWindow(hwnd, 1);
    SetActiveWindow(hwnd);
}

void show_help(HWND hwnd)
{
    launch_help(hwnd, NULL);
}

void defuse_showwindow(void)
{
    /*
     * Work around the fact that the app's first call to ShowWindow
     * will ignore the default in favour of the shell-provided
     * setting.
     */
    {
        HWND hwnd;
        hwnd = CreateDialog(hinst, MAKEINTRESOURCE(IDD_ABOUTBOX),
                            NULL, NullDlgProc);
        ShowWindow(hwnd, SW_HIDE);
        SetActiveWindow(hwnd);
        DestroyWindow(hwnd);
    }
}

bool do_config(Conf *conf)
{
    bool ret;
    PortableDialogStuff *pds = pds_new(2);

    serial_fallback_reset();
    /*
     * cn: 「连接/串口」页面已删除，数据位/停止位/校验/流控四项固定走
     * 默认值。存量配置里若留有旧的非默认值，在这里归位 —— 必须在
     * baseline 快照之前，否则这次归位会被记成用户的改动。
     */
    cn_serial_params_reset(conf);
    /*
     * cn: 上一轮配置框里点开的「记录日志」开关一律作废。开关不再直接
     * 发起连接，用户点了取消/关窗后意图会残留，不清理会让下一次的
     * 按钮显示与实际意图对不上。
     */
    cn_reset_log_request();
    /* cn: enable unsaved-changes tracking for the startup config box,
     * so the save button is dirty-gated and closing prompts with the
     * change list. */
    pds->dp->track_changes = true;
    pds->dp->baseline = conf_copy(conf);
    pds->dp->ctrlbox = pds->ctrlbox;
    /* cn: ensure the settings-initialization snapshot exists, taking
     * the configuration at first open as the baseline. */
    {
        Conf *snap = conf_new();
        if (!load_settings("SettingsSnapshot", snap))
            save_settings("SettingsSnapshot", conf);
        conf_free(snap);
    }
    setup_config_box(pds->ctrlbox, false, 0, 0);
    win_setup_config_box(pds->ctrlbox, &pds->dp->hwnd, has_help(), false, 0);

    pds->dp->wintitle = dupprintf("%s 配置", appname);
    pds->dp->data = conf;

    dlg_auto_set_fixed_pitch_flag(pds->dp);

    pds->dp->shortcuts['g'] = true;          /* the treeview: `Cate&gory' */

    ret = ShinyDialogBox(hinst, MAKEINTRESOURCE(IDD_MAINBOX), "PuTTYConfigBox",
                         NULL, GenericMainDlgProc, pds);

    pds_free(pds);

    return ret;
}

struct reconfig_apply_ctx {
    void (*cb)(void *ctx);
    void *ctx;
    Conf *conf;                  /* the live config being edited */
    Conf *applied;               /* settings currently in effect */
};

/*
 * Wrapper passed to the dialog as its Apply hook: hand the new
 * settings to the caller, then remember them as the state the session
 * will fall back to if the box is cancelled afterwards.
 */
static void reconfig_apply_hook(void *vctx)
{
    struct reconfig_apply_ctx *actx = (struct reconfig_apply_ctx *)vctx;

    actx->cb(actx->ctx);
    conf_copy_into(actx->applied, actx->conf);
}

bool do_reconfig(HWND hwnd, Conf *conf, int protcfginfo,
                 void (*apply_cb)(void *), void *apply_ctx)
{
    Conf *backup_conf;
    bool ret;
    int protocol;
    PortableDialogStuff *pds = pds_new(2);
    struct reconfig_apply_ctx actx;

    serial_fallback_reset();
    pds->dp->baseline = conf_copy(conf);
    backup_conf = conf_copy(conf);

    actx.cb = apply_cb;
    actx.ctx = apply_ctx;
    actx.conf = conf;
    actx.applied = backup_conf;

    protocol = conf_get_int(conf, CONF_protocol);
    setup_config_box(pds->ctrlbox, true, protocol, protcfginfo);
    win_setup_config_box(pds->ctrlbox, &pds->dp->hwnd, has_help(),
                         true, protocol);

    pds->dp->wintitle = dupprintf("%s 会话设置", appname);
    pds->dp->data = conf;
    pds->dp->apply_cb = apply_cb ? reconfig_apply_hook : NULL;
    pds->dp->apply_ctx = &actx;
    pds->dp->ctrlbox = pds->ctrlbox;
    pds->dp->track_changes = true;

    dlg_auto_set_fixed_pitch_flag(pds->dp);

    pds->dp->shortcuts['g'] = true;          /* the treeview: `Cate&gory' */

    ret = ShinyDialogBox(hinst, MAKEINTRESOURCE(IDD_MAINBOX), "PuTTYConfigBox",
                         NULL, GenericMainDlgProc, pds);

    pds_free(pds);

    if (!ret)
        conf_copy_into(conf, backup_conf);

    conf_free(backup_conf);

    return ret;
}

static void win_gui_eventlog(LogPolicy *lp, const char *string)
{
    char timebuf[40];
    char **location;
    struct tm tm;

    tm=ltime();
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S\t", &tm);

    if (ninitial < LOGEVENT_INITIAL_MAX)
        location = &events_initial[ninitial];
    else
        location = &events_circular[(circular_first + ncircular) % LOGEVENT_CIRCULAR_MAX];

    if (*location)
        sfree(*location);
    *location = dupcat(timebuf, string);
    if (logbox) {
        int count;
        {
            wchar_t *w = cn_w(*location);
            SendDlgItemMessageW(logbox, IDN_LIST, LB_ADDSTRING, 0, (LPARAM) w);
            sfree(w);
        }
        count = SendDlgItemMessageW(logbox, IDN_LIST, LB_GETCOUNT, 0, 0);
        SendDlgItemMessageW(logbox, IDN_LIST, LB_SETTOPINDEX, count - 1, 0);

        update_logbox_horizontal_extent(logbox);
    }
    if (ninitial < LOGEVENT_INITIAL_MAX) {
        ninitial++;
    } else if (ncircular < LOGEVENT_CIRCULAR_MAX) {
        ncircular++;
    } else if (ncircular == LOGEVENT_CIRCULAR_MAX) {
        circular_first = (circular_first + 1) % LOGEVENT_CIRCULAR_MAX;
        sfree(events_circular[circular_first]);
        events_circular[circular_first] = dupstr("..");
    }
}

static void win_gui_logging_error(LogPolicy *lp, const char *event)
{
    WinGuiSeat *wgs = container_of(lp, WinGuiSeat, logpolicy);

    /* Send 'can't open log file' errors to the terminal window.
     * (Marked as stderr, although terminal.c won't care.) */
    seat_stderr_pl(&wgs->seat, ptrlen_from_asciz(event));
    seat_stderr_pl(&wgs->seat, PTRLEN_LITERAL("\r\n"));
}

void showeventlog(HWND hwnd)
{
    if (!logbox) {
        logbox = CreateDialog(hinst, MAKEINTRESOURCE(IDD_LOGBOX),
                              hwnd, LogProc);
        ShowWindow(logbox, SW_SHOWNORMAL);
    }
    SetActiveWindow(logbox);
}

void showabout(HWND hwnd)
{
    DialogBox(hinst, MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd, AboutProc);
}

struct hostkey_dialog_ctx {
    SeatDialogText *text;
    bool has_title;
    const char *helpctx;
};

static INT_PTR HostKeyMoreInfoProc(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam, void *vctx)
{
    struct hostkey_dialog_ctx *ctx = (struct hostkey_dialog_ctx *)vctx;

    switch (msg) {
      case WM_INITDIALOG: {
        int index = 100, y = 12;

        WPARAM font = SendMessage(hwnd, WM_GETFONT, 0, 0);

        const char *key = NULL;
        for (SeatDialogTextItem *item = ctx->text->items,
                 *end = item + ctx->text->nitems; item < end; item++) {
            switch (item->type) {
              case SDT_MORE_INFO_KEY:
                key = item->text;
                break;
              case SDT_MORE_INFO_VALUE_SHORT:
              case SDT_MORE_INFO_VALUE_BLOB: {
                RECT rk, rv;
                DWORD editstyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                    ES_AUTOHSCROLL | ES_READONLY;
                if (item->type == SDT_MORE_INFO_VALUE_BLOB) {
                    rk.left = 12;
                    rk.right = 376;
                    rk.top = y;
                    rk.bottom = 8;
                    y += 10;

                    editstyle |= ES_MULTILINE;
                    rv.left = 12;
                    rv.right = 376;
                    rv.top = y;
                    rv.bottom = 64;
                    y += 68;
                } else {
                    rk.left = 12;
                    rk.right = 80;
                    rk.top = y+2;
                    rk.bottom = 8;

                    rv.left = 100;
                    rv.right = 288;
                    rv.top = y;
                    rv.bottom = 12;

                    y += 16;
                }

                MapDialogRect(hwnd, &rk);
                wchar_t *wkey = cn_w(key);
                HWND ctl = CreateWindowExW(
                    0, L"STATIC", wkey, WS_CHILD | WS_VISIBLE,
                    rk.left, rk.top, rk.right, rk.bottom,
                    hwnd, (HMENU)(ULONG_PTR)index++, hinst, NULL);
                sfree(wkey);
                SendMessage(ctl, WM_SETFONT, font, MAKELPARAM(true, 0));

                MapDialogRect(hwnd, &rv);
                {
                    wchar_t *wval = cn_w(item->text);
                    ctl = CreateWindowExW(
                        WS_EX_CLIENTEDGE, L"EDIT", wval, editstyle,
                        rv.left, rv.top, rv.right, rv.bottom,
                        hwnd, (HMENU)(ULONG_PTR)index++, hinst, NULL);
                    sfree(wval);
                }
                SendMessage(ctl, WM_SETFONT, font, MAKELPARAM(true, 0));
                break;
              }
              default:
                break;
            }
        }

        /*
         * Now resize the overall window, and move the Close button at
         * the bottom.
         */
        RECT r;
        r.left = 176;
        r.top = y + 10;
        r.right = r.bottom = 0;
        MapDialogRect(hwnd, &r);
        HWND ctl = GetDlgItem(hwnd, IDOK);
        SetWindowPos(ctl, NULL, r.left, r.top, 0, 0,
                     SWP_NOSIZE | SWP_NOREDRAW | SWP_NOZORDER);

        r.left = r.top = r.right = 0;
        r.bottom = 300;
        MapDialogRect(hwnd, &r);
        int oldheight = r.bottom;

        r.left = r.top = r.right = 0;
        r.bottom = y + 30;
        MapDialogRect(hwnd, &r);
        int newheight = r.bottom;

        GetWindowRect(hwnd, &r);

        SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left,
                     r.bottom - r.top + newheight - oldheight,
                     SWP_NOMOVE | SWP_NOREDRAW | SWP_NOZORDER);

        ShowWindow(hwnd, SW_SHOWNORMAL);
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
            ShinyEndDialog(hwnd, 0);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        ShinyEndDialog(hwnd, 0);
        return 0;
    }
    return 0;
}

static const char *process_seatdialogtext(
    strbuf *dlg_text, const char **scary_heading, SeatDialogText *text)
{
    const char *dlg_title = "";

    for (SeatDialogTextItem *item = text->items,
             *end = item + text->nitems; item < end; item++) {
        switch (item->type) {
          case SDT_PARA:
            put_fmt(dlg_text, "%s\r\n\r\n", item->text);
            break;
          case SDT_DISPLAY:
            put_fmt(dlg_text, "%s\r\n\r\n", item->text);
            break;
          case SDT_SCARY_HEADING:
            assert(scary_heading != NULL && "只有在"
                   "对话框有位置放置时，才会显示醒目的警告标题");
            *scary_heading = item->text;
            break;
          case SDT_TITLE:
            dlg_title = item->text;
            break;
          default:
            break;
        }
    }

    /* Trim any trailing newlines */
    while (strbuf_chomp(dlg_text, '\r') || strbuf_chomp(dlg_text, '\n'));

    return dlg_title;
}

static INT_PTR HostKeyDialogProc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam, void *vctx)
{
    struct hostkey_dialog_ctx *ctx = (struct hostkey_dialog_ctx *)vctx;

    switch (msg) {
      case WM_INITDIALOG: {
        strbuf *dlg_text = strbuf_new();
        const char *scary_heading = NULL;
        const char *dlg_title = process_seatdialogtext(
            dlg_text, &scary_heading, ctx->text);

        LPCTSTR iconid = IDI_QUESTION;
        if (scary_heading) {
            wchar_t *wh = cn_w(scary_heading);
            SetDlgItemTextW(hwnd, IDC_HK_TITLE, wh);
            sfree(wh);
            iconid = IDI_WARNING;
        }

        {
            wchar_t *wt = cn_w(dlg_text->s);
            SetDlgItemTextW(hwnd, IDC_HK_TEXT, wt);
            sfree(wt);
        }
        MakeDlgItemBorderless(hwnd, IDC_HK_TEXT);
        strbuf_free(dlg_text);

        {
            wchar_t *wtitle = cn_w(dlg_title);
            SetWindowTextW(hwnd, wtitle);
            sfree(wtitle);
        }

        if (!ctx->has_title) {
            HWND item = GetDlgItem(hwnd, IDC_HK_TITLE);
            if (item)
                DestroyWindow(item);
        }

        /*
         * Find out how tall the text in the edit control really ended
         * up (after line wrapping), and adjust the height of the
         * whole box to match it.
         */
        int height = SendDlgItemMessage(hwnd, IDC_HK_TEXT,
                                        EM_GETLINECOUNT, 0, 0);
        height *= 9; /* height of a text line, by definition of dialog units */

        int edittop = ctx->has_title ? 40 : 20;

        RECT r;
        r.left = 40;
        r.top = edittop;
        r.right = 290;
        r.bottom = height;
        MapDialogRect(hwnd, &r);
        SetWindowPos(GetDlgItem(hwnd, IDC_HK_TEXT), NULL,
                     r.left, r.top, r.right, r.bottom,
                     SWP_NOREDRAW | SWP_NOZORDER);

        static const struct {
            int id, x;
        } buttons[] = {
            { IDCANCEL, 288 },
            { IDC_HK_ACCEPT, 168 },
            { IDC_HK_ONCE, 216 },
            { IDC_HK_MOREINFO, 60 },
            { IDHELP, 12 },
        };
        for (size_t i = 0; i < lenof(buttons); i++) {
            HWND ctl = GetDlgItem(hwnd, buttons[i].id);
            r.left = buttons[i].x;
            r.top = edittop + height + 20;
            r.right = r.bottom = 0;
            MapDialogRect(hwnd, &r);
            SetWindowPos(ctl, NULL, r.left, r.top, 0, 0,
                         SWP_NOSIZE | SWP_NOREDRAW | SWP_NOZORDER);
        }

        r.left = r.top = r.right = 0;
        r.bottom = 240;
        MapDialogRect(hwnd, &r);
        int oldheight = r.bottom;

        r.left = r.top = r.right = 0;
        r.bottom = edittop + height + 40;
        MapDialogRect(hwnd, &r);
        int newheight = r.bottom;

        GetWindowRect(hwnd, &r);

        SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left,
                     r.bottom - r.top + newheight - oldheight,
                     SWP_NOMOVE | SWP_NOREDRAW | SWP_NOZORDER);

        HANDLE icon = LoadImage(
            NULL, iconid, IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
            LR_SHARED);
        SendDlgItemMessage(hwnd, IDC_HK_ICON, STM_SETICON, (WPARAM)icon, 0);

        if (!has_help()) {
            HWND item = GetDlgItem(hwnd, IDHELP);
            if (item)
                DestroyWindow(item);
        }

        ShowWindow(hwnd, SW_SHOWNORMAL);

        return 1;
      }
      case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND control = (HWND)lParam;

        if (GetWindowLongPtr(control, GWLP_ID) == IDC_HK_TITLE &&
            ctx->has_title) {
            SetBkMode(hdc, TRANSPARENT);
            HFONT prev_font = (HFONT)SelectObject(
                hdc, (HFONT)GetStockObject(SYSTEM_FONT));
            LOGFONT lf;
            if (GetObject(prev_font, sizeof(lf), &lf)) { 
                lf.lfWeight = FW_BOLD;
                lf.lfHeight = lf.lfHeight * 3 / 2;
                HFONT bold_font = CreateFontIndirect(&lf);
                if (bold_font)
                    SelectObject(hdc, bold_font);
            }
            return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
        }
        return 0;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDC_HK_ACCEPT:
          case IDC_HK_ONCE:
          case IDCANCEL:
            ShinyEndDialog(hwnd, LOWORD(wParam));
            return 0;
          case IDHELP: {
            launch_help(hwnd, ctx->helpctx);
            return 0;
          }
          case IDC_HK_MOREINFO: {
            ShinyDialogBox(hinst, MAKEINTRESOURCE(IDD_HK_MOREINFO),
                           "PuTTYHostKeyMoreInfo", hwnd,
                           HostKeyMoreInfoProc, ctx);
          }
        }
        return 0;
      case WM_CLOSE:
        ShinyEndDialog(hwnd, IDCANCEL);
        return 0;
    }
    return 0;
}

const SeatDialogPromptDescriptions *win_seat_prompt_descriptions(Seat *seat)
{
    static const SeatDialogPromptDescriptions descs = {
        .hk_accept_action = "点击“接受”",
        .hk_connect_once_action = "点击“只连接一次”",
        .hk_cancel_action = "点击“取消”",
        .hk_cancel_action_Participle = "点击“取消”",
        .weak_accept_action = "点击“是”",
        .weak_cancel_action = "点击“否”",
    };
    return &descs;
}

SeatPromptResult win_seat_confirm_ssh_host_key(
    Seat *seat, const char *host, int port, const char *keytype,
    char *keystr, SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *cbctx)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);

    struct hostkey_dialog_ctx ctx[1];
    ctx->text = text;
    ctx->helpctx = helpctx;

    int mbret = ShinyDialogBox(
        hinst, MAKEINTRESOURCE(IDD_HOSTKEY), "PuTTYHostKeyDialog",
        wgs->term_hwnd, HostKeyDialogProc, ctx);
    assert(mbret==IDC_HK_ACCEPT || mbret==IDC_HK_ONCE || mbret==IDCANCEL);
    if (mbret == IDC_HK_ACCEPT) {
        store_host_key(seat, host, port, keytype, keystr);
        return SPR_OK;
    } else if (mbret == IDC_HK_ONCE) {
        return SPR_OK;
    }

    return SPR_USER_ABORT;
}

/*
 * Ask whether the selected algorithm is acceptable (since it was
 * below the configured 'warn' threshold).
 */
SeatPromptResult win_seat_confirm_weak_crypto_primitive(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    strbuf *dlg_text = strbuf_new();
    const char *dlg_title = process_seatdialogtext(dlg_text, NULL, text);

    wchar_t *wtext = cn_w(dlg_text->s), *wtitle = cn_w(dlg_title);
    int mbret = MessageBoxW(NULL, wtext, wtitle,
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    sfree(wtext);
    sfree(wtitle);
    socket_reselect_all();
    strbuf_free(dlg_text);

    if (mbret == IDYES)
        return SPR_OK;
    else
        return SPR_USER_ABORT;
}

SeatPromptResult win_seat_confirm_weak_cached_hostkey(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    strbuf *dlg_text = strbuf_new();
    const char *dlg_title = process_seatdialogtext(dlg_text, NULL, text);

    wchar_t *wtext = cn_w(dlg_text->s), *wtitle = cn_w(dlg_title);
    int mbret = MessageBoxW(NULL, wtext, wtitle,
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    sfree(wtext);
    sfree(wtitle);
    socket_reselect_all();
    strbuf_free(dlg_text);

    if (mbret == IDYES)
        return SPR_OK;
    else
        return SPR_USER_ABORT;
}

/*
 * Ask whether to wipe a session log file before writing to it.
 * Returns 2 for wipe, 1 for append, 0 for cancel (don't log).
 */
static int win_gui_askappend(LogPolicy *lp, Filename *filename,
                             void (*callback)(void *ctx, int result),
                             void *ctx)
{
    static const char msgtemplate[] =
        "会话日志文件 \"%.*s\" 已经存在。\n"
        "可以使用新会话日志覆盖旧文件，\n"
        "或者在旧日志文件结尾增加新日志，\n"
        "或在此会话中禁用日志记录。\n"
        "点击“是”覆盖为新文件，“否”附加到旧文件，\n"
        "或者点击“取消”禁用日志记录。";
    char *message;
    char *mbtitle;
    int mbret;

    message = dupprintf(msgtemplate, FILENAME_MAX, filename->utf8path);
    mbtitle = dupprintf("%s 日志记录到文件", appname);

    mbret = message_box(NULL, message, mbtitle,
                        MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON3,
                        true, 0);

    socket_reselect_all();

    sfree(message);
    sfree(mbtitle);

    if (mbret == IDYES)
        return 2;
    else if (mbret == IDNO)
        return 1;
    else
        return 0;
}

const LogPolicyVtable win_gui_logpolicy_vt = {
    .eventlog = win_gui_eventlog,
    .askappend = win_gui_askappend,
    .logging_error = win_gui_logging_error,
    .verbose = null_lp_verbose_yes,
};

/*
 * Warn about the obsolescent key file format.
 *
 * Uniquely among these functions, this one does _not_ expect a
 * frontend handle. This means that if PuTTY is ported to a
 * platform which requires frontend handles, this function will be
 * an anomaly. Fortunately, the problem it addresses will not have
 * been present on that platform, so it can plausibly be
 * implemented as an empty function.
 */
void old_keyfile_warning(void)
{
    static const char mbtitle[] = "%s 密钥文件警告";
    static const char message[] =
        "现在载入的是一个旧版本文件格式的 SSH2\n"
        "私钥。这意味着该私钥文件没有足够的安全\n"
        "性。未来版本的 %s 可能会停止支持\n"
        "此私钥格式，建议将其转换为新的格式。\n"
        "\n"
        "请使用 PuTTYgen 载入该密钥进行转换然\n"
        "后保存。";

    char *msg, *title;
    msg = dupprintf(message, appname);
    title = dupprintf(mbtitle, appname);

    {
        wchar_t *wmsg = cn_w(msg), *wtitle = cn_w(title);
        MessageBoxW(NULL, wmsg, wtitle, MB_OK);
        sfree(wmsg);
        sfree(wtitle);
    }

    socket_reselect_all();

    sfree(msg);
    sfree(title);
}

static INT_PTR CAConfigProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                            void *ctx)
{
    PortableDialogStuff *pds = (PortableDialogStuff *)ctx;

    switch (msg) {
      case WM_INITDIALOG:
        pds_initdialog_start(pds, hwnd);

        SendMessage(hwnd, WM_SETICON, (WPARAM) ICON_BIG,
                    (LPARAM) LoadIcon(hinst, MAKEINTRESOURCE(IDI_CFGICON)));

        centre_window(hwnd);

        pds_create_controls(pds, 0, IDCX_PANELBASE, 3, 3, 3, "Main");
        pds_create_controls(pds, 0, IDCX_STDBASE, 3, 3, 243, "");
        cn_hostbox_subclass_tree(pds, 0);
        dlg_refresh(NULL, pds->dp);    /* and set up control values */

        pds_initdialog_finish(pds);
        return 0;

      default:
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
    }
}

void show_ca_config_box(dlgparam *dp)
{
    PortableDialogStuff *pds = pds_new(1);

    setup_ca_config_box(pds->ctrlbox);

    ShinyDialogBox(hinst, MAKEINTRESOURCE(IDD_CA_CONFIG), "PuTTYConfigBox",
                   dp ? dp->hwnd : NULL, CAConfigProc, pds);

    pds_free(pds);
}

/* ----------------------------------------------------------------------
 * cn: 会话目录 —— 右键菜单与单行输入框。
 *
 * 两个都放在这里（配置框的 Windows 平台层）：config.c 是跨平台文件，
 * 只负责目录数据与列表重组，Win32 交互全部落在这一层。
 */

int cn_session_list_menu(HWND parent, int screen_x, int screen_y,
                         bool is_folder, bool in_folder,
                         char **folders, int nfolders)
{
    HMENU menu = CreatePopupMenu();
    int cmd, i;

    if (!menu)
        return CN_SESS_MENU_CANCEL;

    if (is_folder) {
        /*
         * 「进入目录」排第一：双击目录行是同一个动作，但光靠双击没有
         * 任何提示，右键菜单里得有个看得见的入口。
         */
        AppendMenuW(menu, MF_STRING, CN_SESS_MENU_OPEN, L"进入目录(&O)");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, CN_SESS_MENU_RENAME,
                    L"重命名目录(&R)");
        AppendMenuW(menu, MF_STRING, CN_SESS_MENU_DELETE,
                    L"删除目录(&D)");
    } else {
        /*
         * 会话行的「重命名」只改列表里显示的名字，连接目标不动 —— 见
         * set_recent_conn_label 的说明。放在最上面，因为它是单行操作，
         * 比归类更常用。
         */
        AppendMenuW(menu, MF_STRING, CN_SESS_MENU_RENAME,
                    L"重命名(&R)");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        /*
         * 「移入目录」做成子菜单，而不是再弹一层选择窗：一级目录最多
         * 二十个，平铺在子菜单里一眼看全，比二次弹窗快得多。
         */
        HMENU sub = CreatePopupMenu();
        if (sub && nfolders > 0) {
            for (i = 0; i < nfolders; i++) {
                wchar_t *wname =
                    dup_mb_to_wc(CP_UTF8, folders[i] ? folders[i] : "");
                AppendMenuW(sub, MF_STRING, CN_SESS_MENU_MOVE_BASE + i,
                            wname);
                sfree(wname);
            }
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"移入目录(&M)");
        } else {
            if (sub)
                DestroyMenu(sub);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"移入目录(&M)");
        }
        if (in_folder)
            AppendMenuW(menu, MF_STRING, CN_SESS_MENU_MOVE_OUT,
                        L"移出目录(&O)");
    }

    cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD |
                              TPM_NONOTIFY, screen_x, screen_y, 0, parent,
                              NULL);
    /*
     * 菜单连同子菜单一起销毁：MF_POPUP 挂进去的子菜单归父菜单所有，
     * 这里不能再单独 DestroyMenu 它，否则是二次释放。
     */
    DestroyMenu(menu);
    return cmd;
}

/*
 * cn: 列表行内就地编辑（改名不弹窗）。
 *
 * 目录名、会话显示名都在列表那一行上直接改 —— 跟资源管理器按 F2 一个
 * 观感，省掉「弹窗-输入-确定」那趟往返。
 *
 * ⚠️ 父窗是配置框，是真对话框：它的消息泵会先拿 IsDialogMessage 过一遍，
 * 回车会被解释成「按了默认按钮」，而配置框的默认按钮是「打开」—— 不改
 * 的话，改完名字敲回车就直接连出去了。所以编辑框必须回
 * DLGC_WANTALLKEYS，把回车/ESC/Tab 全抢下来自己处理。
 */

/* 控件 ID：随便挑个远离对话框模板 ID 区间的值，不会跟已有控件撞。 */
#define CN_INLINE_EDIT_ID 0x7F31

typedef struct CnInlineEdit {
    HWND edit;
    char *result;
    int resultcap;
    bool accepted;
    bool done;
    WNDPROC oldproc;
} CnInlineEdit;

static void cn_inline_finish(CnInlineEdit *ie, bool accept)
{
    if (ie->done)
        return;
    ie->done = true;
    ie->accepted = accept;

    if (accept) {
        int len = GetWindowTextLengthW(ie->edit);
        wchar_t *wbuf = snewn(len + 1, wchar_t);
        char *mb;

        GetWindowTextW(ie->edit, wbuf, len + 1);
        mb = dup_wc_to_mb(CP_UTF8, wbuf, "");
        snprintf(ie->result, ie->resultcap, "%s", mb);
        sfree(mb);
        sfree(wbuf);
    }

    /* 唤醒下面本地模态循环里阻塞着的 GetMessage。 */
    PostMessageW(ie->edit, WM_NULL, 0, 0);
}

static LRESULT CALLBACK cn_inline_wndproc(HWND hwnd, UINT msg,
                                          WPARAM wparam, LPARAM lparam)
{
    CnInlineEdit *ie = (CnInlineEdit *)GetProp(hwnd, "cn_inline");

    switch (msg) {
      case WM_GETDLGCODE:
        /* 见上：不让对话框过程把回车当成默认按钮。 */
        return DLGC_WANTALLKEYS;

      case WM_KEYDOWN:
        if (ie) {
            if (wparam == VK_RETURN) {
                cn_inline_finish(ie, true);
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                cn_inline_finish(ie, false);
                return 0;
            }
        }
        break;

      case WM_CHAR:
        /* 单行 EDIT 收到 \r 会蜂鸣，回车已经在 WM_KEYDOWN 里吃掉了。 */
        if (wparam == '\r' || wparam == '\n' || wparam == VK_ESCAPE)
            return 0;
        break;

      case WM_KILLFOCUS:
        /* 点到别处 = 提交，跟资源管理器一致。 */
        if (ie)
            cn_inline_finish(ie, true);
        break;
    }

    if (ie && ie->oldproc)
        return CallWindowProcW(ie->oldproc, hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool cn_inline_edit(HWND listbox, int row, char *buf, int cap)
{
    HINSTANCE inst = GetModuleHandle(NULL);
    CnInlineEdit ie;
    HWND parent, edit;
    RECT ir, cr;
    POINT tl;
    HFONT font;
    MSG msg;
    int width, height, leftmargin;
    wchar_t *winit;

    if (!listbox || !buf || cap <= 0 || row < 0)
        return false;

    /*
     * LB_GETITEMRECT 只对可见行有效，拿不到矩形就没法定位。先把它选上
     * —— listbox 会顺手滚到这一行。
     */
    SendMessageW(listbox, LB_SETCURSEL, row, 0);
    if (SendMessageW(listbox, LB_GETITEMRECT, row, (LPARAM)&ir) == LB_ERR)
        return false;

    parent = GetParent(listbox);
    if (!parent)
        return false;

    /*
     * 行矩形是 listbox 的客户坐标，得先换到屏幕再换到父窗客户坐标 ——
     * 少转一道就会因为边框宽度差几个像素而错位。
     */
    tl.x = ir.left;
    tl.y = ir.top;
    ClientToScreen(listbox, &tl);
    ScreenToClient(parent, &tl);

    GetClientRect(listbox, &cr);
    width = cr.right - ir.left;

    /*
     * 右边界止于端口列的分割线。整行拉满会把端口号那一列也框进来，
     * 改名时看着像连端口一起改，所以按左列文字区的右边界收窄。
     */
    {
        int textright = cn_listbox_text_right(listbox);
        if (textright > ir.left)
            width = textright - ir.left;
    }

    /*
     * 左内缩取列表自绘文字的缩进，让编辑框里的字跟上下行的字落在同
     * 一条竖线上。差几像素的话，一眼就能看出这行是另贴上去的。
     */
    {
        int textleft = cn_listbox_text_left(listbox);
        leftmargin = textleft > ir.left ? textleft - ir.left : 2;
    }

    if (width < 60)
        width = 60;

    /*
     * 高度让出一像素。列表的行分隔线画在行矩形的最后一行像素上
     * （controls.c 的 WM_DRAWITEM：MoveToEx(r.left, r.bottom - 1)），
     * 编辑框若正好占满行高，就会把这条线整条盖掉 —— 这一行和下一行
     * 看着就粘在一起，像贴了张白纸。
     */
    height = ir.bottom - ir.top - 1;
    if (height < 10)
        height = ir.bottom - ir.top;

    /*
     * 取消列表的选中：行退回普通的白底黑字，编辑框无边框浮在同一位置、
     * 同一个字体，看上去就是这行文字自己在改。留着选中反而更糟 —— 行
     * 是蓝底白字，编辑框是白底黑字，一块白纸贴在蓝条上。
     */
    SendMessageW(listbox, LB_SETCURSEL, (WPARAM)-1, 0);

    memset(&ie, 0, sizeof(ie));
    ie.result = buf;
    ie.resultcap = cap;

    winit = dup_mb_to_wc(CP_UTF8, buf);
    edit = CreateWindowExW(0, L"EDIT", winit,
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                           tl.x, tl.y, width, height, parent,
                           (HMENU)CN_INLINE_EDIT_ID, inst, NULL);
    sfree(winit);
    if (!edit)
        return false;
    ie.edit = edit;

    /* 字体跟列表一致，否则编辑框里的字会跟旁边的行对不齐。 */
    font = (HFONT)SendMessageW(listbox, WM_GETFONT, 0, 0);
    if (font)
        SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);

    /*
     * 无边框之后控件自己没有内缩，文字会顶在行首，得手动补上左边距
     * （值取自上面 cn_listbox_text_left 那一段）。
     */
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELONG(leftmargin, leftmargin));

    SetProp(edit, "cn_inline", (HANDLE)&ie);
    ie.oldproc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC,
                                            (LONG_PTR)cn_inline_wndproc);

    /* 全选：改名时一个字符都不用先删。 */
    SendMessageW(edit, EM_SETSEL, 0, -1);
    SetFocus(edit);

    /*
     * 本地模态循环。
     *
     * ⚠️ 别换成 IsDialogMessageW：本循环跑在配置框的消息泵之外，而配置
     * 框是对话框，IsDialogMessageW 会拿它去解释回车/方向键，实测会把
     * 主窗最小化（同一个坑记在 sftpwin.c 的 cn_input_box 里）。回车与
     * ESC 已由 cn_inline_wndproc 接住，这里只负责把消息派发下去。
     *
     * 点到编辑框以外的地方：先提交再退出，并且把这一下点击吃掉 —— 放
     * 它过去的话，落点可能是「打开」按钮，改个名字就把连接发出去了。
     */
    while (!ie.done && IsWindow(edit) && GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.hwnd != edit &&
            (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
             msg.message == WM_LBUTTONDBLCLK ||
             msg.message == WM_NCLBUTTONDOWN)) {
            cn_inline_finish(&ie, true);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveProp(edit, "cn_inline");
    if (IsWindow(edit))
        DestroyWindow(edit);

    /*
     * 把选中还给这一行。提交时调用方多半会重建列表（选中由它定），
     * 但按 ESC 取消时列表不重建 —— 不还回来就等于白白丢了选中。
     */
    if ((int)SendMessageW(listbox, LB_GETCOUNT, 0, 0) > row)
        SendMessageW(listbox, LB_SETCURSEL, row, 0);

    return ie.accepted;
}
