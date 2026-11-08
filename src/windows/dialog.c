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
static void cn_style_snapshot_buttons(HWND hwnd)
{
    HWND hdel = NULL, hinit = NULL, hlist = NULL;
    HWND h = 0;
    /* cn: 按钮尺寸调整（宽+5/高-2）只在首次执行时做一次——本函数会被
     * repin 定时器重复调用，反复做会越缩越小。 */
    bool cn_resize = !GetProp(hwnd, "cn_btn_styled");
    while (true) {
        h = FindWindowEx(hwnd, h, "Button", NULL);
        if (!h)
            break;
        char text[64];
        GetWindowText(h, text, sizeof(text));
        if (strncmp(text, "删除", 6) == 0)
            hdel = h;
        else if (strncmp(text, "初始化设置", 15) == 0)
            hinit = h;
    }
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
     * standard height, and safely inside the group frame. */
    if (cn_resize) {
        SetProp(hwnd, "cn_btn_styled", (HANDLE)1);
        HWND btns[2] = { hdel, hinit };
        for (int i = 0; i < 2; i++) {
            RECT r;
            if (!btns[i])
                continue;
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
        SetWindowPos(hdel, NULL, p1.x, p1.y + dy_top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
        int dy_bottom = rl.bottom - (ri.bottom - ri.top) - ri.top;
        SetWindowPos(hinit, NULL, p2.x, p2.y + dy_bottom, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);

        /*
         * cn: 组框底边收紧到「列表底 + 5px」——布局 ypos 给组框留的
         * 空隙偏大（历史遗留约 20px），主人指定收窄到 5px。
         */
        HWND hgrp = NULL;
        h = 0;
        while ((h = FindWindowEx(hwnd, h, "Button", NULL)) != NULL) {
            char text[64];
            GetWindowText(h, text, sizeof(text));
            if (strncmp(text, "已保存的会话", 18) == 0) {
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
                char text[64];
                GetWindowText(h, text, sizeof(text));
                if (strncmp(text, "保存", 6) == 0 ||
                    strncmp(text, "文件传输", 12) == 0 ||
                    strncmp(text, "连接", 6) == 0 ||
                    strncmp(text, "退出", 6) == 0) {
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
                char text[64];
                GetWindowText(h, text, sizeof(text));
                if (strncmp(text, "保存", 6) == 0) {
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
    int mbret = cn_confirm_dialog_texts(dp->hwnd, changes, &cn_quit_texts,
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
    int mbret = cn_confirm_dialog_texts(dp->hwnd, changes, &cn_launch_texts,
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

    if (pds->dp->wintitle)     /* apply override title, if provided */
        SetWindowText(hwnd, pds->dp->wintitle);

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
    int count = SendMessage(listbox, LB_GETCOUNT, 0, 0);
    strbuf *sb = strbuf_new();
    WPARAM maxwidth = 0;
    for (int i = 0; i < count; i++) {
        size_t len = SendMessage(listbox, LB_GETTEXTLEN, i, 0);
        strbuf_clear(sb);
        SendMessage(listbox, LB_GETTEXT, i, (LPARAM)strbuf_append(sb, len));
        SIZE size;
        if (GetTextExtentPoint(hdc, sb->s, sb->len, &size)) {
            if (maxwidth < size.cx)
                maxwidth = size.cx;
        }
    }
    strbuf_free(sb);
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
        SetWindowText(hwnd, str);
        sfree(str);

        static int tabs[4] = { 78, 108 };
        SendDlgItemMessage(hwnd, IDN_LIST, LB_SETTABSTOPS, 2,
                           (LPARAM) tabs);

        for (i = 0; i < ninitial; i++)
            SendDlgItemMessage(hwnd, IDN_LIST, LB_ADDSTRING,
                               0, (LPARAM) events_initial[i]);
        for (i = 0; i < ncircular; i++)
            SendDlgItemMessage(hwnd, IDN_LIST, LB_ADDSTRING,
                               0, (LPARAM) events_circular[(circular_first + i) % LOGEVENT_CIRCULAR_MAX]);
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
        SetWindowText(hwnd, str);
        sfree(str);
        SetDlgItemText(hwnd, IDA_TEXT, LICENCE_TEXT("\r\n\r\n"));
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
        SetWindowText(hwnd, str);
        sfree(str);
        char *buildinfo_text = buildinfo("\r\n");
        char *text = dupprintf(
            "%s\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s",
            appname, ver, buildinfo_text,
            "(C) " SHORT_COPYRIGHT_DETAILS ". 保留所有权利。");
        sfree(buildinfo_text);
        SetDlgItemText(hwnd, IDA_TEXT, text);
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
    TVINSERTSTRUCT ins;
    int i;
    HTREEITEM newitem;
    ins.hParent = (level > 0 ? faff->lastat[level - 1] : TVI_ROOT);
    ins.hInsertAfter = faff->lastat[level];
#if _WIN32_IE >= 0x0400 && defined NONAMELESSUNION
#define INSITEM DUMMYUNIONNAME.item
#else
#define INSITEM item
#endif
    ins.INSITEM.mask = TVIF_TEXT | TVIF_PARAM;
    ins.INSITEM.pszText = text;
    ins.INSITEM.cchTextMax = strlen(text)+1;
    ins.INSITEM.lParam = (LPARAM)path;
    newitem = TreeView_InsertItem(faff->treeview, &ins);
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
 * cn: 在树控件里按 pathname 找对应项。树项的 lParam 保存的正是
 * ctrl_settitle / ctrl_getset 用过的 pathname 字符串，所以深度
 * 优先比对即可，无需拼装父节点文字。
 */
static HTREEITEM treeview_find_by_path(HWND treeview, HTREEITEM item,
                                       const char *pathname)
{
    while (item) {
        TVITEMEX tv;
        char buf[256];
        tv.mask = TVIF_TEXT | TVIF_PARAM | TVIF_HANDLE;
        tv.hItem = item;
        tv.pszText = buf;
        tv.cchTextMax = sizeof(buf);
        if (TreeView_GetItem(treeview, &tv)) {
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
            tvstatic = CreateWindowEx(0, "STATIC", "分类(&G)：",
                                      WS_CHILD | WS_VISIBLE,
                                      r.left, r.top,
                                      r.right - r.left, r.bottom - r.top,
                                      hwnd, (HMENU) IDCX_TVSTATIC, hinst,
                                      NULL);
            font = SendMessage(hwnd, WM_GETFONT, 0, 0);
            SendMessage(tvstatic, WM_SETFONT, font, MAKELPARAM(true, 0));

            r.left = 3;
            r.right = r.left + 95;
            r.top = 13;
            /* cn: 高度 219→202，树底不与左下的「保存(V)」按钮重叠
             * （按钮行已上移到组框底+10，树需同步让位）。 */
            r.bottom = r.top + 202;
            MapDialogRect(hwnd, &r);
            treeview = CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEW, "",
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

        if (dialog_box_demo_screenshot_filename)
            SetTimer(hwnd, DEMO_SCREENSHOT_TIMER_ID, TICKSPERSEC, NULL);

        pds_initdialog_finish(pds);
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
                char text[64];
                GetWindowText(h, text, sizeof(text));
                if (strncmp(text, "初始化设置", 15) == 0)
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
                MessageBox(hwnd, err, "演示截图失败",
                           MB_OK | MB_ICONERROR);
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
            TVITEM item;
            char buffer[64];

            if (!pds->initialised)
                return 0;

            i = TreeView_GetSelection(((LPNMHDR) lParam)->hwndFrom);

            SendMessage (hwnd, WM_SETREDRAW, false, 0);

            item.hItem = i;
            item.pszText = buffer;
            item.cchTextMax = sizeof(buffer);
            item.mask = TVIF_TEXT | TVIF_PARAM;
            TreeView_GetItem(((LPNMHDR) lParam)->hwndFrom, &item);
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
        SendDlgItemMessage(logbox, IDN_LIST, LB_ADDSTRING,
                           0, (LPARAM) *location);
        count = SendDlgItemMessage(logbox, IDN_LIST, LB_GETCOUNT, 0, 0);
        SendDlgItemMessage(logbox, IDN_LIST, LB_SETTOPINDEX, count - 1, 0);

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
                HWND ctl = CreateWindowEx(
                    0, "STATIC", key, WS_CHILD | WS_VISIBLE,
                    rk.left, rk.top, rk.right, rk.bottom,
                    hwnd, (HMENU)(ULONG_PTR)index++, hinst, NULL);
                SendMessage(ctl, WM_SETFONT, font, MAKELPARAM(true, 0));

                MapDialogRect(hwnd, &rv);
                ctl = CreateWindowEx(
                    WS_EX_CLIENTEDGE, "EDIT", item->text, editstyle,
                    rv.left, rv.top, rv.right, rv.bottom,
                    hwnd, (HMENU)(ULONG_PTR)index++, hinst, NULL);
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
            SetDlgItemText(hwnd, IDC_HK_TITLE, scary_heading);
            iconid = IDI_WARNING;
        }

        SetDlgItemText(hwnd, IDC_HK_TEXT, dlg_text->s);
        MakeDlgItemBorderless(hwnd, IDC_HK_TEXT);
        strbuf_free(dlg_text);

        SetWindowText(hwnd, dlg_title);

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

    int mbret = MessageBox(NULL, dlg_text->s, dlg_title,
                           MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
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

    int mbret = MessageBox(NULL, dlg_text->s, dlg_title,
                           MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
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

    MessageBox(NULL, msg, title, MB_OK);

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
