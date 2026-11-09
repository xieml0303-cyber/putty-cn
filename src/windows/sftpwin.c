/*
 * sftpwin.c: 双栏文件传输窗口。
 *
 * 左边远端、右边本地，各自一个列表；中间两个按钮负责互传。设计取向
 * 是「一眼看懂、少点几下」：
 *
 *  - 双击目录进下一层，工具栏「向上」回上一层（两侧各有一套）；
 *  - 选中文件按中间的箭头就传，方向由按钮本身说明；
 *  - 新建目录 / 删除 / 重命名在列表右键菜单里，点哪边就动哪边；
 *  - 传输时窗口不消失，进度条在底部走，随时能取消；
 *  - 传完自动刷新对侧列表，不用手工按刷新。
 *
 * 传输期间 SFTP 层会跑嵌套消息泵（见 sftpxfer.c），所以界面不会被冻
 * 住，但重入要小心：传输过程中不要再触发别的传输，按钮在那期间全部
 * 禁用。
 */

#include <winsock2.h> /* 必须放在最前面 */

/*
 * 这一整个文件都在跑 Unicode 窗口 API。工程全局没有定义 UNICODE，
 * 于是 commctrl.h 的 ListView_* 宏会展开成 ANSI 版本，传宽字符指针
 * 就报类型不符。这里只对本文件声明编译器按 Unicode 处理，正好能压
 * 住那批宏，又不影响别的源文件。必须写在 commctrl.h 之前。
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>    /* StrCmpLogicalW：资源管理器那套自然排序 */
#include <windowsx.h>   /* GET_X_LPARAM / GET_Y_LPARAM */
#include <ole2.h>
#include <math.h>       /* sin/cos：加载动画的转圈 */

#include "putty.h"
#include "dialog.h"     /* dlg_has_unsaved_changes / dlg_confirm_quit */
#include "ssh.h"
#include "ssh/sftp.h"
#include "win-gui-seat.h"
#include "sftpxfer.h"
/* 图标资源 ID（IDI_SFTPXFER），与 putty.rc 共用同一份编号。 */
#include "putty-rc.h"

/* ----------------------------------------------------------------------
 * 控件编号。
 */

#define IDC_LOCAL_PATH   1001
#define IDC_REMOTE_PATH  1002
#define IDC_LOCAL_LIST   1003
#define IDC_REMOTE_LIST  1004
/*
 * 远端在左、本地在右，两侧各留一个「向上」；「刷新」并进右键菜单
 * （和新建目录/删除/重命名放一起，都是对某一侧的操作）。
 */
#define IDC_BTN_UP       1010
#define IDC_BTN_UP_LOCAL 1017
#define IDC_BTN_DESKTOP  1018
#define IDC_BTN_TO_REMOTE 1012
#define IDC_BTN_TO_LOCAL  1013
#define IDC_PROGRESS     1020
#define IDC_STATUS       1021
#define IDC_BTN_PAUSE    1023
#define IDC_BTN_STOP     1024

/*
 * 列表右键菜单的命令号。新建目录 / 删除 / 重命名 / 刷新都在这里：
 * 挂在列表上之后，「作用于哪一侧」由右键落在哪个列表决定，不再靠
 * 焦点猜 —— 原来那三个按钮按下去时焦点跑到按钮自己身上，
 * GetFocus() 比对必然落空，等于永远只对本地生效。
 */
#define IDM_MKDIR        2001
#define IDM_DELETE       2002
#define IDM_RENAME       2003
#define IDM_REFRESH      2004

#define CN_WINDOW_CLASS L"PuTTYSftpXferWindow"

/* 传输状态机的三个状态，见 SftpWin.xfer_state 的注释。 */
#define CN_XFER_IDLE     0
#define CN_XFER_RUNNING  1
#define CN_XFER_PAUSED   2

/* 转义：远端路径走 UTF-8，本地路径也统一用 UTF-8 存，碰窗口时再转宽字符 */

typedef struct SftpWin {
    HWND hwnd;
    HWND local_path, remote_path;
    HWND local_list, remote_list;
    HWND progress, status;
    HWND btn_up, btn_to_remote, btn_to_local;
    HWND btn_up_local;
    HWND btn_desktop_local;     /* cn: 本地栏「桌面」快捷键 */
    HWND btn_pause, btn_stop;   /* 暂停 / 续传（同一个按钮切换文字）、终止 */

    SftpXfer *sx;
    char *localdir;             /* 本地当前目录，UTF-8 */
    char *remotedir;            /* 远端当前目录，UTF-8 */

    bool busy;                  /* 传输进行中，禁掉会重入的操作 */
    /*
     * 递归传输时用户点了取消。和 sx 里的取消标志分开记：那一个是
     * 「当前这个文件停下」，这一层是「整棵子树别再往下走了」。
     */
    bool cancel_requested;

    /*
     * 传输状态机。三个状态互斥：
     *   CN_XFER_IDLE    没在传，暂停/终止都灰着
     *   CN_XFER_RUNNING 正在传，暂停按钮显示「暂停」、可点
     *   CN_XFER_PAUSED  已暂停，暂停按钮变成「续传」、可点
     *
     * 暂停不退出 cn_transfer_entries 那一层，而是就地挂住等续传：
     * 队列、当前下标、断点位置全在栈上活着，续传时接着跑，不重复传
     * 已完成的文件。
     */
    int xfer_state;
    uint64_t progress_done, progress_total;   /* 暂停时保住进度条位置 */

    /*
     * 续传上下文。暂停时把整批清单从栈上搬到这里存着，点「续传」
     * 再拿它重新走一遍 cn_transfer_entries：
     *   - 已传完的文件：目标大小已等于源大小，协议层直接跳过，不重发；
     *   - 传了一半的：目标比源小，从目标现有大小处接着传。
     * 所以「续传」不需要记住下标，靠目标端实际大小自动对齐，即使
     * 中间有文件被删过也不会错位。
     */
    char **resume_names, **resume_srcs;
    bool *resume_dirs;
    size_t resume_count;
    bool resume_to_remote;
    /*
     * 下一批是不是「续传」。cn_transfer_entries 靠它决定是否允许按
     * 目标端已有大小跳过/接续；普通传输必须为假，否则目标端大小恰好
     * 相同就会被误判成「已传完」而实际一个字节没动。
     */
    bool resume_pending;

    /*
     * 列表行图标。两个列表共用同一个 image list：
     * 目录和文件各占一个下标，两栏都用得到，没必要建两份。
     * 尺寸取系统小图标（SM_CXSMICON），与资源管理器详细信息视图
     * 保持同一口径，换 DPI 时系统自己会给对应尺寸。
     */
    HIMAGELIST imglist;
    int img_dir;
    int img_file;

    HFONT font;
    bool font_owned;            /* font 是我们 CreateFont 建的，销毁时要删 */
    HBRUSH bg;

    /*
     * 两个列表各自的排序状态。点列头切换，刷新目录后保持。
     *
     * 为什么分两栏各存一份：左右是两个不同的目录，用户完全可能一边按
     * 时间找最新文件、一边按名称翻目录，共用一个状态会互相干扰。
     *
     * 默认按名称升序（0/true），跟资源管理器的初始行为一致。
     */
    int sort_col_local, sort_col_remote;      /* 0=名称 1=大小 2=时间 */
    bool sort_asc_local, sort_asc_remote;     /* true=升序 */

    /*
     * cn: 打开过程的加载动画状态（原为文件级全局 the_loading）。
     * 生命周期与窗口结构一致：打开失败就地回收 sw 时动画状态一并
     * 消失，不给多窗口扩展留共享态隐患。menuspin 窗口经
     * GWLP_USERDATA 反查所属 sw。
     */
    struct {
        HWND menuspin;          /* 锚点旁小转圈；无锚点时为 NULL */
        int frame;
        int done;               /* CN_LOADING_DONE_* */
    } loading;

    /*
     * cn: 结果提示条（toast）。
     *
     * 一批传输收尾后浮在窗口底部居中，滑入 -> 停留 -> 淡出，几秒后自己
     * 消失。只做「这一批干完了」的通知，不拦鼠标（HTTRANSPARENT），也不
     * 抢焦点（WS_EX_NOACTIVATE），免得挡住用户紧接着要点的按钮。
     *
     * 复用一条窗口而不是每次新建：连续传几批时，上一条还没消失就被新
     * 文案顶掉，比叠出两三个提示条干净得多。
     */
    struct {
        HWND hwnd;              /* 提示条窗口，NULL 表示还没建过 */
        HFONT font;             /* 绘制用字体，借用主窗口那一份，不拥有 */
        wchar_t *text;          /* 当前文案，换一条就重设 */
        int phase;              /* CN_TOAST_* */
        int tick;               /* 当前阶段已过的帧数 */
    } toast;
} SftpWin;

static SftpWin *the_sftpwin = NULL;

/* ----------------------------------------------------------------------
 * 小工具。
 */

static void cn_set_text(HWND hwnd, const char *text)
{
    wchar_t *w = dup_mb_to_wc(DEFAULT_CODEPAGE, text ? text : "");
    SetWindowTextW(hwnd, w);
    sfree(w);
}

/* cn: 打开失败自检用——统计本进程弹过多少次模态框，正常应为 0。 */
static int cn_selftest_modal_count = 0;

static int cn_ask(HWND parent, const char *text, const char *title, UINT flags)
{
    wchar_t *wt = dup_mb_to_wc(DEFAULT_CODEPAGE, text ? text : "");
    wchar_t *wtitle = dup_mb_to_wc(DEFAULT_CODEPAGE, title ? title : "文件传输");

    /* 只统计 sftpwin.c 自己的弹框；上层（配置框/终端）弹的不算。 */
    cn_selftest_modal_count++;

    /*
     * cn: 不可绕过的兜底。owner 不可见时 MessageBox 不会被系统置前，
     * 却仍会把该窗口锁进模态禁用状态——用户看不见、点不着，外层
     * 配置框的消息循环就此被堵死（2026-11-06 实测事故）。宁可弹在
     * 屏幕中间（owner = NULL），也不制造隐藏模态。
     */
    if (parent && !IsWindowVisible(parent))
        parent = NULL;

    int rc = MessageBoxW(parent, wt, wtitle,
                         flags | MB_SETFOREGROUND | MB_TOPMOST);
    sfree(wt);
    sfree(wtitle);
    return rc;
}

/*
 * 以 pt 为中心摆一个 w x h 的窗口，越界就贴边收进工作区。
 *
 * 结果写进 outx / outy（屏幕坐标）。自绘弹窗都走这一套算 ——「居鼠标 +
 * 不出屏」的判据必须只有一处，否则同一个操作在不同弹窗上会落在不同
 * 位置，用起来很怪。
 *
 * 早先用过「挂 WH_CBT 钩子挪 MessageBox」的路子，那是为了给没有位置
 * 参数的系统对话框找补。覆盖确认改成自绘窗口后就没有调用方了，连同
 * 钩子一并删掉，只留这个纯计算的函数。
 */
static void cn_place_near(POINT pt, int w, int h, int *outx, int *outy)
{
    RECT wa;
    int x = pt.x - w / 2;   /* 鼠标居中于弹窗 */
    int y = pt.y - h / 2;

    /* 别让弹窗被推到屏幕外，贴边收进工作区。 */
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        if (x + w > wa.right) x = wa.right - w;
        if (y + h > wa.bottom) y = wa.bottom - h;
        if (x < wa.left) x = wa.left;
        if (y < wa.top) y = wa.top;
    }
    *outx = x;
    *outy = y;
}

/*
 * cn: 「确认保存」弹窗固定居中于屏幕（主人指定）。
 *
 * 不复用 cn_place_near —— 那个是「居鼠标」语义，确认覆盖等弹窗还在用，
 * 改了会连带挪动别的窗口。这里按工作区居中，工作区取不到就退到整屏。
 * 用工作区而不是整屏，是为了避开任务栏，不然居中会偏下。
 */
static void cn_place_center(int w, int h, int *outx, int *outy)
{
    RECT wa;
    int sw, sh, ox = 0, oy = 0;

    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        sw = wa.right - wa.left;
        sh = wa.bottom - wa.top;
        ox = wa.left;
        oy = wa.top;
    } else {
        sw = GetSystemMetrics(SM_CXSCREEN);
        sh = GetSystemMetrics(SM_CYSCREEN);
    }
    *outx = ox + (sw - w) / 2;
    *outy = oy + (sh - h) / 2;
}

static void cn_msg(HWND parent, const char *text, const char *title, UINT flags)
{
    cn_ask(parent, text, title, flags);
}

/* ----------------------------------------------------------------------
 * 单行输入框。
 *
 * 系统没有现成的「输入一行文字」对话框，TaskDialog 又要 Vista 以上且
 * 用法啰嗦，这里自己搭一个小模态窗口：一个标签、一个编辑框、确定取消。
 */

typedef struct InputBox {
    HWND dlg, edit, ok, cancel;
    wchar_t *result;            /* 确定后指向编辑框内容副本 */
    int resultcap;
    bool accepted;
} InputBox;

static LRESULT CALLBACK cn_input_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam)
{
    InputBox *ib = (InputBox *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
      case WM_COMMAND:
        if (LOWORD(wparam) == IDOK && ib) {
            GetWindowTextW(ib->edit, ib->result, ib->resultcap);
            ib->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
      case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/*
 * 弹输入框，接受则把文字写回 buf 并返回 true。
 */
static bool cn_input_box(HWND parent, const char *title, const char *prompt,
                         wchar_t *buf, int cap)
{
    static const wchar_t classname[] = L"PuTTYSftpInputBox";
    HINSTANCE inst = GetModuleHandle(NULL);
    WNDCLASSW wc;
    InputBox ib;
    HWND hwnd;
    MSG msg;
    int x, y, w = 380, h = 170;
    RECT prc;

    memset(&ib, 0, sizeof(ib));
    ib.result = buf;
    ib.resultcap = cap;

    if (!GetClassInfoW(inst, classname, &wc)) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = cn_input_wndproc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = classname;
        RegisterClassW(&wc);
    }

    /* 居中到父窗口 */
    if (parent && GetWindowRect(parent, &prc)) {
        x = prc.left + (prc.right - prc.left - w) / 2;
        y = prc.top + (prc.bottom - prc.top - h) / 2;
    } else {
        x = y = CW_USEDEFAULT;
    }

    {
        wchar_t *wtitle = dup_mb_to_wc(DEFAULT_CODEPAGE, title);
        hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, classname, wtitle,
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, w, h, parent, NULL, inst, NULL);
        sfree(wtitle);
    }
    if (!hwnd)
        return false;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&ib);
    ib.dlg = hwnd;

    {
        wchar_t *wprompt = dup_mb_to_wc(DEFAULT_CODEPAGE, prompt);
        HWND label = CreateWindowExW(0, L"STATIC", wprompt,
                                      WS_CHILD | WS_VISIBLE,
                                      16, 16, w - 48, 20, hwnd, NULL,
                                      inst, NULL);
        SendMessageW(label, WM_SETFONT,
                     (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        sfree(wprompt);
    }

    ib.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", buf,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                               ES_AUTOHSCROLL,
                               16, 44, w - 48, 24, hwnd, (HMENU)100,
                               inst, NULL);
    SendMessageW(ib.edit, WM_SETFONT,
                 (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    /* 全选，方便直接改名字 */
    SendMessageW(ib.edit, EM_SETSEL, 0, -1);

    ib.ok = CreateWindowW(L"BUTTON", L"确定",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                           w - 190, 88, 80, 28, hwnd, (HMENU)IDOK, inst, NULL);
    ib.cancel = CreateWindowW(L"BUTTON", L"取消",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               w - 100, 88, 80, 28, hwnd, (HMENU)IDCANCEL,
                               inst, NULL);
    SendMessageW(ib.ok, WM_SETFONT,
                 (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageW(ib.cancel, WM_SETFONT,
                 (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

    if (parent)
        EnableWindow(parent, FALSE);

    ShowWindow(hwnd, SW_SHOW);
    SetFocus(ib.edit);

    /*
     * 本地模态循环。
     *
     * ⚠️ 别用 IsDialogMessageW：本窗是自绘 WS_POPUP，没有对话框模板，
     * 它会把消息按「父窗是对话框」去解释，实测会把文件传输主窗最小化
     * （历史遗留：早先「确认覆盖」弹窗也踩过同一个坑）。
     *
     * 这里只需要 Esc 取消、Enter 确认两条，其余（Tab 在输入框与按钮间
     * 切换）交给默认过程 —— 系统对 WS_TABSTOP 子控件本来就有基本支持。
     */
    while (IsWindow(hwnd) && GetMessageW(&msg, NULL, 0, 0)) {
        bool mine = (msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd));

        if (mine && msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                break;
            }
            if (msg.wParam == VK_RETURN) {
                SendMessageW(hwnd, WM_COMMAND,
                             MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)ib.ok);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (parent)
        EnableWindow(parent, TRUE);

    return ib.accepted;
}

static void cn_set_status(SftpWin *sw, const char *text)
{
    /* 自检模式没有窗口，状态直接丢掉。 */
    if (!sw || !sw->status)
        return;
    cn_set_text(sw->status, text);
}

/*
 * 刷新中间两个传输按钮的启用状态。
 *
 * 两个按钮各自独立判断，只看「自己那一侧的源列表有没有选中项」：
 *   上传 —— 源是本地列表（远端在左、本地在右，上传是从右往左）
 *   下载 —— 源是远端列表
 * 空目录、刷新后选中丢失、右键点空白清掉选中，都会让对应按钮回到
 * 禁用态；另一侧完全不受影响。
 *
 * 传输进行中一律禁用（busy），否则用户能点第二次、重入传输。
 */
static void cn_update_transfer_buttons(SftpWin *sw)
{
    if (!sw || !sw->btn_to_remote || !sw->btn_to_local)
        return;

    if (sw->busy) {
        EnableWindow(sw->btn_to_remote, FALSE);
        EnableWindow(sw->btn_to_local, FALSE);
        return;
    }

    EnableWindow(sw->btn_to_remote,
                 sw->local_list &&
                     ListView_GetNextItem(sw->local_list, -1, LVNI_SELECTED) >= 0);
    EnableWindow(sw->btn_to_local,
                 sw->remote_list &&
                     ListView_GetNextItem(sw->remote_list, -1, LVNI_SELECTED) >= 0);
}

/*
 * 清空两侧列表的选中，并刷新按钮。
 *
 * 点窗口背景、点标题栏、点列表内空白都走这一条路径 —— 语义都是
 * 「我什么都不选了」。抽出来还有个好处：自检可以直接调它验状态，
 * 不必去发那些会引发系统模态循环的消息（见下面 WM_NCLBUTTONDOWN
 * 那段注释里记的坑）。
 */
static void cn_clear_selection(SftpWin *sw)
{
    bool changed = false;

    if (!sw)
        return;

    if (sw->local_list &&
        ListView_GetNextItem(sw->local_list, -1, LVNI_SELECTED) >= 0) {
        ListView_SetItemState(sw->local_list, -1, 0, LVIS_SELECTED);
        changed = true;
    }
    if (sw->remote_list &&
        ListView_GetNextItem(sw->remote_list, -1, LVNI_SELECTED) >= 0) {
        ListView_SetItemState(sw->remote_list, -1, 0, LVIS_SELECTED);
        changed = true;
    }
    if (changed)
        cn_update_transfer_buttons(sw);
}

/*
 * 传输状态切换的唯一入口。
 *
 * 按钮可用性与文字必须永远跟状态一致，所以只在这里改，别处一律
 * 不许直接 EnableWindow / SetWindowText 那两个按钮 —— 分散着改早晚
 * 会出现「已暂停但按钮写着暂停且能点」这种自相矛盾的界面。
 */
static void cn_set_xfer_state(SftpWin *sw, int state)
{
    sw->xfer_state = state;

    switch (state) {
      case CN_XFER_RUNNING:
        SetWindowTextW(sw->btn_pause, L"暂停");
        EnableWindow(sw->btn_pause, TRUE);
        EnableWindow(sw->btn_stop, TRUE);
        break;

      case CN_XFER_PAUSED:
        /* 暂停后同一个按钮变成「续传」，位置不变，视线不用找。 */
        SetWindowTextW(sw->btn_pause, L"续传");
        EnableWindow(sw->btn_pause, TRUE);
        EnableWindow(sw->btn_stop, TRUE);
        break;

      default:                  /* CN_XFER_IDLE */
        SetWindowTextW(sw->btn_pause, L"暂停");
        EnableWindow(sw->btn_pause, FALSE);
        EnableWindow(sw->btn_stop, FALSE);
        break;
    }
}

/*
 * 续传清单的保存与取回。
 *
 * 暂停时把「还剩什么没传完」记下来；续传时把这份清单再喂给
 * cn_transfer_entries 走一遍。不必记下标，靠目标端实际文件大小对齐：
 * 已传完的会被协议层判为「已完整」直接跳过，传了一半的从断点接上。
 * 好处是用户中途在目标端删了文件也不会错位。
 */
static void cn_resume_free(SftpWin *sw)
{
    if (sw->resume_names) {
        for (size_t i = 0; i < sw->resume_count; i++) {
            sfree(sw->resume_names[i]);
            if (sw->resume_srcs)
                sfree(sw->resume_srcs[i]);
        }
        sfree(sw->resume_names);
        sfree(sw->resume_srcs);
        sfree(sw->resume_dirs);
        sw->resume_names = NULL;
        sw->resume_srcs = NULL;
        sw->resume_dirs = NULL;
        sw->resume_count = 0;
    }
}

/* 把 names/srcs/dirs 深拷贝一份存起来；传 NULL 表示清掉旧的。 */
static void cn_resume_save(SftpWin *sw, char **names, char **srcs,
                           bool *dirs, size_t count, bool to_remote)
{
    cn_resume_free(sw);

    if (!names || !count)
        return;

    sw->resume_names = snewn(count, char *);
    sw->resume_srcs = srcs ? snewn(count, char *) : NULL;
    sw->resume_dirs = snewn(count, bool);
    for (size_t i = 0; i < count; i++) {
        sw->resume_names[i] = dupstr(names[i]);
        if (srcs)
            sw->resume_srcs[i] = dupstr(srcs[i]);
        sw->resume_dirs[i] = dirs[i];
    }
    sw->resume_count = count;
    sw->resume_to_remote = to_remote;
}

static void cn_transfer_entries(SftpWin *sw, char **names, char **srcs,
                                bool *dirs, size_t count, bool to_remote);

/*
 * 续传：把暂停时存下的清单再走一遍 cn_transfer_entries。
 *
 * 不需要记住「传到第几个」——协议层会拿目标端现有大小和源大小比：
 * 已经一样大的直接跳过（一个字节都不重发），小的从断点接着传。
 * 所以即使暂停期间用户在目标端删了文件，也不会错位。
 */
static void cn_resume_run(SftpWin *sw)
{
    char **names, **srcs;
    bool *dirs;
    size_t count;
    bool to_remote;

    if (!sw->resume_names || !sw->resume_count) {
        cn_set_status(sw, "没有可续传的任务。");
        cn_set_xfer_state(sw, CN_XFER_IDLE);
        return;
    }

    /*
     * 把清单从窗口字段搬到局部变量：cn_transfer_entries 会在收尾时
     * 用 cn_resume_save 重新写这些字段（传 NULL 清空），交出去之前
     * 先摘下来，避免它把自己的输入释放掉。
     */
    names = sw->resume_names;
    srcs = sw->resume_srcs;
    dirs = sw->resume_dirs;
    count = sw->resume_count;
    to_remote = sw->resume_to_remote;
    sw->resume_names = NULL;
    sw->resume_srcs = NULL;
    sw->resume_dirs = NULL;
    sw->resume_count = 0;

    /* 标记这一批是续传：允许按目标端已有大小跳过/接续。 */
    sw->resume_pending = true;
    cn_transfer_entries(sw, names, srcs, dirs, count, to_remote);
    sw->resume_pending = false;
}

/* 传输期间禁用会重入的操作。 */
static void cn_set_busy(SftpWin *sw, bool busy)
{
    sw->busy = busy;
    EnableWindow(sw->btn_up, !busy);
    EnableWindow(sw->btn_up_local, !busy);
    EnableWindow(sw->local_list, !busy);
    EnableWindow(sw->remote_list, !busy);
    /* 暂停/终止只在传输期间可用，交给状态机统一管。 */
    if (!busy)
        cn_set_xfer_state(sw, CN_XFER_IDLE);
    /* 传输按钮还多一层「有没有选中」，交给它自己算。 */
    cn_update_transfer_buttons(sw);
}

/* ----------------------------------------------------------------------
 * ListView 的宽字符小封装。
 *
 * commctrl.h 里 ListView_SetItemText / ListView_GetItemText /
 * ListView_InsertItem / ListView_InsertColumn 全都是宏，内部按固定的
 * ANSI 版本展开（ListView_InsertItemW 才是 W 版），不管有没有定义
 * UNICODE；ListView_GetItemData 干脆没有。这里直接把 W 版消息发下去，
 * 绕开那堆宏。
 *
 * 曾经漏掉 InsertItem / InsertColumn 两处：pszText 指向 wchar_t 缓冲区，
 * 而宏按 LPSTR 交给控件，宽字符被逐字节当成 ANSI 读取，界面上文件名与
 * 列头全成乱码（「名称」显示为 "Tey" 之类）。凡是把字符串交给 ListView
 * 的调用，都必须走下面这几个封装。
 */

static void cn_list_set_text(HWND list, int item, int subitem,
                             const wchar_t *text)
{
    LVITEMW lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.iSubItem = subitem;
    lvi.pszText = (LPWSTR)text;
    SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)item, (LPARAM)&lvi);
}

/* ----------------------------------------------------------------------
 * 列表行图标。
 *
 * 目录和文件各一个图标，尺寸取 SM_CXSMICON —— 和资源管理器详细信息
 * 视图同一口径（实测 96 DPI 下 16x16）。不写死 16：系统在不同 DPI 下
 * 会给出不同的小图标尺寸，跟着它走，换屏才不会忽大忽小。
 *
 * 行高不自己给，交给系统按字体算（LVS_EX_ 不加，也不用 OWNERDRAWFIXED）。
 * 这样两块屏缩放不同也能各自显示正常，代价是没法精确控制留白。
 */
static void cn_build_icon_list(SftpWin *sw)
{
    HINSTANCE inst = GetModuleHandle(NULL);
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    HICON ico;

    if (cx <= 0)
        cx = 16;
    if (cy <= 0)
        cy = 16;

    sw->imglist = ImageList_Create(cx, cy, ILC_COLOR32 | ILC_MASK, 2, 2);
    if (!sw->imglist)
        return;

    /* 按系统小图标尺寸取，ICO 里有多档，让系统挑最合适的。 */
    ico = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_LISTDIR), IMAGE_ICON,
                            cx, cy, LR_DEFAULTCOLOR);
    sw->img_dir = ImageList_AddIcon(sw->imglist, ico);
    if (ico)
        DestroyIcon(ico);

    ico = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_LISTFILE), IMAGE_ICON,
                            cx, cy, LR_DEFAULTCOLOR);
    sw->img_file = ImageList_AddIcon(sw->imglist, ico);
    if (ico)
        DestroyIcon(ico);
}

/* 插入一行，返回新行下标；pszText 为宽字符。img 为 image list 下标。 */
static int cn_list_insert_item(HWND list, int item, const wchar_t *text,
                               LPARAM lparam, int img)
{
    LVITEMW lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
    lvi.iItem = item;
    lvi.pszText = (LPWSTR)text;
    lvi.lParam = lparam;
    lvi.iImage = img;
    return (int)SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
}

/* 插入一列；pszText 为宽字符。fmt 传 LVCFMT_LEFT / LVCFMT_RIGHT。 */
static void cn_list_insert_column(HWND list, int index, int width,
                                  const wchar_t *title, int fmt)
{
    LVCOLUMNW col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
    col.iSubItem = index;
    col.pszText = (LPWSTR)title;
    col.cx = width;
    col.fmt = fmt;
    SendMessageW(list, LVM_INSERTCOLUMNW, (WPARAM)index, (LPARAM)&col);
}

/*
 * 列头文字居中 —— 子类化 header 控件，在 WM_PAINT 里自己画。
 *
 * 走过的弯路记一下，免得下回再踩：
 *   1) HDM_SETITEMW(HDI_FORMAT) 改 HDF_CENTER —— 会连带把列内容的
 *      LVCOLUMN.fmt 一起改成居中，右对齐断言当场全红。
 *   2) 挂 HDF_OWNERDRAW + 父窗口 NM_CUSTOMDRAW —— header 不发这个通知，
 *      画出来的还是系统默认的右对齐文字（截图确认）。
 * 现在这版：直接子类化 header，自己接管绘制。列内容的 fmt 一个字节都
 * 不碰，居中完全由这里的 DrawText 决定。
 */

static WNDPROC cn_header_oldproc = NULL;
/* 子类过程地址留一份，自检要比对 header 当前的窗口过程是不是它。 */
static WNDPROC cn_header_proc_addr = NULL;

/*
 * 列头点击（排序）的按下记录。
 *
 * 不用 HDN_ITEMCLICK：那条通知在拖动列宽结束时也可能发，而且还要绕到
 * 父窗口的 WM_NOTIFY 里再分派一次。直接在这里记下按下的列与坐标，抬起
 * 时比对 —— 坐标没动过才算「点击」，拖过宽度的那次松手就自然被排除。
 */
static int cn_hdr_down_col = -1;
static POINT cn_hdr_down_pt;
static bool cn_hdr_dragged = false;

/* cn_fit_columns_ex 自己也要设列宽，别让它被列宽总闸拦住打转。 */
static bool cn_in_fit = false;

/* header 子类过程要用，定义在后面。 */
static void cn_fit_columns_ex(SftpWin *sw, bool keep_name);
static bool cn_is_our_header(HWND header);
static void cn_header_click(HWND header, int col);
/* 点列头排序后要重读目录，这两个定义在更后面。 */
static void cn_refresh_local(SftpWin *sw);
static void cn_refresh_remote(SftpWin *sw);

/* 把屏幕坐标转成 header 客户区坐标，再问是哪一列。返回 -1 表示没命中列。 */
static int cn_header_hit(HWND header, POINT pt_screen)
{
    HDHITTESTINFO ht;

    memset(&ht, 0, sizeof(ht));
    ht.pt = pt_screen;
    ScreenToClient(header, &ht.pt);
    SendMessageW(header, HDM_HITTEST, 0, (LPARAM)&ht);
    if (ht.flags & HHT_ONHEADER)
        return (int)ht.iItem;
    return -1;
}

/*
 * 逻辑像素 -> 物理像素。定义在下面（列宽那一段之前），但列头自绘与
 * 排序箭头在文件更早的位置就要用，所以先声明一次。
 */
static int cn_dpi(int logical);

/*
 * 在列头右端画排序箭头：升序朝上（▲），降序朝下（▼）。
 *
 * 不用字符画 —— 「▲」在不同字体下大小不一、位置也对不齐，用多边形
 * 自己画最稳，尺寸也能按 DPI 折算。
 */
static void cn_header_draw_arrow(HDC hdc, const RECT *rc, bool asc)
{
    int w = cn_dpi(8), h = cn_dpi(5);
    int cx = rc->right - cn_dpi(7);
    int cy = (rc->top + rc->bottom) / 2;
    POINT pt[3];
    HBRUSH br = CreateSolidBrush(GetSysColor(COLOR_HOTLIGHT));
    HBRUSH old = br ? SelectObject(hdc, br) : NULL;

    if (asc) {
        pt[0].x = cx - w / 2; pt[0].y = cy + h / 2;
        pt[1].x = cx + w / 2; pt[1].y = cy + h / 2;
        pt[2].x = cx;         pt[2].y = cy - h / 2;
    } else {
        pt[0].x = cx - w / 2; pt[0].y = cy - h / 2;
        pt[1].x = cx + w / 2; pt[1].y = cy - h / 2;
        pt[2].x = cx;         pt[2].y = cy + h / 2;
    }
    Polygon(hdc, pt, 3);
    if (old)
        SelectObject(hdc, old);
    if (br)
        DeleteObject(br);
}

static LRESULT CALLBACK cn_header_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                       LPARAM lparam)
{
    /* 记下按下的列与位置，抬起时用来判断这是点击还是拖宽度。 */
    if (msg == WM_LBUTTONDOWN && the_sftpwin && cn_is_our_header(hwnd)) {
        POINT pt;
        pt.x = GET_X_LPARAM(lparam);
        pt.y = GET_Y_LPARAM(lparam);
        ClientToScreen(hwnd, &pt);
        cn_hdr_down_pt = pt;
        cn_hdr_down_col = cn_header_hit(hwnd, pt);
        cn_hdr_dragged = false;
        return CallWindowProcW(cn_header_oldproc, hwnd, msg, wparam, lparam);
    }
    /*
     * 拖动列宽时鼠标会移动，超过阈值就认为不是点击。阈值取系统拖动
     * 判定宽度的一半，跟系统口径一致；太小会把轻微手抖误判成拖动，
     * 太大则拖完一点点宽度还会被当成点击去排序。
     */
    if (msg == WM_MOUSEMOVE && cn_hdr_down_col >= 0) {
        POINT pt;
        int dragx = GetSystemMetrics(SM_CXDRAG);
        int dragy = GetSystemMetrics(SM_CYDRAG);
        pt.x = GET_X_LPARAM(lparam);
        pt.y = GET_Y_LPARAM(lparam);
        ClientToScreen(hwnd, &pt);
        if (abs(pt.x - cn_hdr_down_pt.x) > dragx / 2 ||
            abs(pt.y - cn_hdr_down_pt.y) > dragy / 2)
            cn_hdr_dragged = true;
    }

    /*
     * 拖动列宽收尾。header 拖拽是系统内部改宽，既不经过 ListView 的
     * LVM_SETCOLUMNWIDTH，松手事件也落在 header 自己身上 —— 所以只能
     * 在 header 的窗口过程里收口，别处拦不住（实测拦 ListView 无效，
     * 名称列照样被拖到 639、总宽 955 溢出）。
     */
    if ((msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) && the_sftpwin &&
        cn_is_our_header(hwnd)) {
        int down_col = cn_hdr_down_col;
        bool dragged = cn_hdr_dragged;
        LRESULT r = CallWindowProcW(cn_header_oldproc, hwnd, msg, wparam,
                                    lparam);
        cn_hdr_down_col = -1;
        cn_hdr_dragged = false;
        /*
         * 没拖过、且抬起的还是按下那一列 —— 这才算点了列头。
         * 列宽照旧要重算：哪怕只是点了一下，也可能因为排序后行数
         * 变化导致滚动条出现或消失，宽度口径需要跟着更新。
         */
        if (!dragged && down_col >= 0 && msg == WM_LBUTTONUP) {
            POINT pt;
            pt.x = GET_X_LPARAM(lparam);
            pt.y = GET_Y_LPARAM(lparam);
            ClientToScreen(hwnd, &pt);
            if (cn_header_hit(hwnd, pt) == down_col)
                cn_header_click(hwnd, down_col);
        }
        cn_fit_columns_ex(the_sftpwin, true);
        return r;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client;
        int ncol = (int)SendMessageW(hwnd, HDM_GETITEMCOUNT, 0, 0);
        int i;

        GetClientRect(hwnd, &client);

        /* 底色：整个列头一次铺满，省得每格各刷一遍。 */
        FillRect(hdc, &client, GetSysColorBrush(COLOR_BTNFACE));

        for (i = 0; i < ncol; i++) {
            RECT rc;
            wchar_t buf[128];
            HDITEMW hdi;

            if (!SendMessageW(hwnd, HDM_GETITEMRECT, (WPARAM)i, (LPARAM)&rc))
                continue;

            /* 单元格底色与分隔线。 */
            FillRect(hdc, &rc, GetSysColorBrush(COLOR_BTNFACE));
            {
                RECT line = rc;
                line.left = line.right - 1;
                FillRect(hdc, &line, GetSysColorBrush(COLOR_BTNSHADOW));
            }

            memset(&hdi, 0, sizeof(hdi));
            hdi.mask = HDI_TEXT;
            hdi.pszText = buf;
            hdi.cchTextMax = lenof(buf);
            buf[0] = L'\0';
            if (!SendMessageW(hwnd, HDM_GETITEMW, (WPARAM)i, (LPARAM)&hdi))
                continue;

            {
                HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
                HFONT old = font ? SelectObject(hdc, font) : NULL;
                /*
                 * 排序列的列头文字改成强调色（系统高亮色），当前列的
                 * 指示一眼可见。系统列头本身只用字体与底色区分，这里
                 * 跟资源管理器对齐：有排序的那一列文字颜色更深一些。
                 */
                int sort_col = -1;
                bool sort_asc = true;
                if (the_sftpwin) {
                    HWND hl = (HWND)SendMessageW(the_sftpwin->local_list,
                                                 LVM_GETHEADER, 0, 0);
                    if (hwnd == hl) {
                        sort_col = the_sftpwin->sort_col_local;
                        sort_asc = the_sftpwin->sort_asc_local;
                    } else {
                        sort_col = the_sftpwin->sort_col_remote;
                        sort_asc = the_sftpwin->sort_asc_remote;
                    }
                }
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, GetSysColor(i == sort_col ? COLOR_HOTLIGHT
                                                            : COLOR_BTNTEXT));
                rc.left += 4;
                rc.right -= 4;
                /*
                 * 排序那一列要给箭头让出右侧十来像素，否则文字压在
                 * 箭头上。其余列保持满宽居中，跟原来一样。
                 */
                if (i == sort_col) {
                    RECT tr = rc;
                    tr.right -= cn_dpi(12);
                    DrawTextW(hdc, buf, -1, &tr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                              DT_NOPREFIX);
                    cn_header_draw_arrow(hdc, &rc, sort_asc);
                } else {
                    DrawTextW(hdc, buf, -1, &rc,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                              DT_NOPREFIX);
                }
                if (old)
                    SelectObject(hdc, old);
            }
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcW(cn_header_oldproc, hwnd, msg, wparam, lparam);
}

/* 子类化两个列表的 header，只需做一次。 */
static void cn_header_setup(HWND list)
{
    HWND header = (HWND)SendMessageW(list, LVM_GETHEADER, 0, 0);

    if (!header)
        return;
    cn_header_proc_addr = (WNDPROC)cn_header_proc;
    if (!cn_header_oldproc)
        cn_header_oldproc = (WNDPROC)SetWindowLongPtrW(
            header, GWLP_WNDPROC, (LONG_PTR)cn_header_proc);
    else
        SetWindowLongPtrW(header, GWLP_WNDPROC, (LONG_PTR)cn_header_proc);
    InvalidateRect(header, NULL, TRUE);
}

/*
 * 兼容旧调用点：以前是逐列改列头格式，现在整体子类化一次就够。
 */
static void cn_list_center_header(HWND list, int index)
{
    (void)index;
    cn_header_setup(list);
}

static void cn_list_get_text(HWND list, int item, int subitem,
                             wchar_t *buf, int cap)
{
    LVITEMW lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.iSubItem = subitem;
    lvi.pszText = buf;
    lvi.cchTextMax = cap;
    buf[0] = L'\0';
    SendMessageW(list, LVM_GETITEMTEXTW, (WPARAM)item, (LPARAM)&lvi);
}

/* 第二列存的是「是不是目录」，取回来就知道该进目录还是传文件。 */
static bool cn_list_item_is_dir(HWND list, int item)
{
    LVITEMW lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask = LVIF_PARAM;
    lvi.iItem = item;
    if (!SendMessageW(list, LVM_GETITEMW, 0, (LPARAM)&lvi))
        return false;
    return lvi.lParam != 0;
}

/* ----------------------------------------------------------------------
 * 列表填充。
 */

static const wchar_t *cn_format_time(unsigned long t, wchar_t *buf, size_t n)
{
    if (!t) {
        wcscpy(buf, L"");
        return buf;
    }
    time_t tt = (time_t)t;
    struct tm *tm = localtime(&tt);
    if (!tm) {
        wcscpy(buf, L"");
        return buf;
    }
    /*
     * 时间格式：`2026-09-16 14:07`，月日时分一律补零。
     *
     * 补零是刻意的，别拿资源管理器的 `2026/9/28 10:00` 来比 —— 后两列
     * 都是右对齐，补零后每行时间戳等宽，右边缘才真正对齐成一条直线；
     * 不补零的话「10:00」比「9:00」多一位，逐行参差，看着反而乱。
     * 代价是这一列略宽一点，但既然字号已经收下来了，这点宽度给得起。
     */
    wcsftime(buf, n, L"%Y-%m-%d %H:%M", tm);
    return buf;
}

static void cn_format_size(uint64_t size, wchar_t *buf, size_t n)
{
    if (size < 1024)
        _snwprintf(buf, n, L"%llu B", (unsigned long long)size);
    else if (size < 1024 * 1024)
        _snwprintf(buf, n, L"%.1f KB", size / 1024.0);
    else if (size < 1024ULL * 1024 * 1024)
        _snwprintf(buf, n, L"%.1f MB", size / (1024.0 * 1024));
    else
        _snwprintf(buf, n, L"%.1f GB", size / (1024.0 * 1024 * 1024));
}

/* 目录 / 文件对应的图标下标。没有 image list 时返回 -1（不显示图标）。 */
static int cn_icon_for(SftpWin *sw, bool is_dir)
{
    if (!sw || !sw->imglist)
        return -1;
    return is_dir ? sw->img_dir : sw->img_file;
}

/*
 * 列表排序。
 *
 * 名称比较用 StrCmpLogicalW，也就是资源管理器那一套：数字按数值大小
 * 排（file2 在 file10 前面）、中文按拼音排。自己写 strcmp 会把 10 排
 * 到 2 前面，中文更是按 UTF-8 字节排到天边去，完全没法用。
 *
 * 目录优先只在「按名称排序」时生效。按大小或时间排的时候，目录那些
 * 0 大小、固定时间戳会把真实文件挤到一边，反而挡住用户想看的东西，
 * 所以那种情况下目录不特殊对待，一起参与排序。
 *
 * qsort 没法传上下文，用文件静态变量带参。调用都在主线程、不会重入。
 */
/*
 * cn: qsort 没有上下文参数，比较器要用的「列/方向/侧」在每次排序
 * 调用前由 cn_sort_entries 写入这里；状态真值永远以 SftpWin 的
 * sort_* 字段为准（cn_fill_list 从 sw 读出再传入），自检直测时由
 * 测试参数直接填充。单线程、调用期间不重入，用完即被下次覆盖。
 */
static struct {
    int col;                    /* 0=名称 1=大小 2=时间 */
    bool asc;                   /* true=升序 */
    bool remote;                /* true=远端列表（决定名称编码） */
} cn_sort_ctx;

static int cn_sort_cmp(const void *pa, const void *pb)
{
    const SftpEntry *a = (const SftpEntry *)pa;
    const SftpEntry *b = (const SftpEntry *)pb;
    int r;

    /* 按名称排序时目录恒在前，方向反转也不翻 —— 反转只作用于同类 */
    if (cn_sort_ctx.col == 0 && a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;

    switch (cn_sort_ctx.col) {
      case 1:                          /* 大小 */
        if (a->size < b->size) r = -1;
        else if (a->size > b->size) r = 1;
        else r = 0;
        break;
      case 2:                          /* 修改时间 */
        if (a->mtime < b->mtime) r = -1;
        else if (a->mtime > b->mtime) r = 1;
        else r = 0;
        break;
      default: {                       /* 名称 */
        wchar_t *wa = dup_mb_to_wc(cn_sort_ctx.remote ? CP_UTF8
                                                  : DEFAULT_CODEPAGE,
                                   a->name);
        wchar_t *wb = dup_mb_to_wc(cn_sort_ctx.remote ? CP_UTF8
                                                  : DEFAULT_CODEPAGE,
                                   b->name);
        r = StrCmpLogicalW(wa, wb);
        sfree(wa);
        sfree(wb);
        break;
      }
    }

    /*
     * 主键相同就用名称兜底，保证排序稳定、同一份目录每次刷新顺序一致。
     * 否则同样大小的两个文件在 qsort 下每次位置可能不一样，列表会
     * 「自己抖动」，看着像 bug。
     */
    if (r == 0 && cn_sort_ctx.col != 0) {
        wchar_t *wa = dup_mb_to_wc(cn_sort_ctx.remote ? CP_UTF8
                                                  : DEFAULT_CODEPAGE,
                                   a->name);
        wchar_t *wb = dup_mb_to_wc(cn_sort_ctx.remote ? CP_UTF8
                                                  : DEFAULT_CODEPAGE,
                                   b->name);
        r = StrCmpLogicalW(wa, wb);
        sfree(wa);
        sfree(wb);
    }

    return cn_sort_ctx.asc ? r : -r;
}

static void cn_sort_entries(SftpEntry *entries, size_t count, int col,
                            bool asc, bool remote)
{
    if (!entries || count < 2)
        return;
    if (col < 0 || col > 2)
        col = 0;
    cn_sort_ctx.col = col;
    cn_sort_ctx.asc = asc;
    cn_sort_ctx.remote = remote;
    qsort(entries, count, sizeof(SftpEntry), cn_sort_cmp);
}

/*
 * 列头被点击：切换排序并刷新对应那一栏。
 *
 * 规则和资源管理器一致：
 *   - 点的是新的一列 -> 该列升序；
 *   - 点的还是当前列 -> 方向反过来。
 *
 * 刷新走 cn_refresh_local / cn_refresh_remote 而不是就地重排列表项：
 * 那两条路径本来就要读目录、填数据、算列宽、更新按钮状态，重排后行数
 * 可能变化（滚动条出现或消失），这些收尾一样不能少。重新读一次目录
 * 还能顺带反映磁盘上的最新变化，代价是一次列目录，可以接受。
 */
static void cn_header_click(HWND header, int col)
{
    SftpWin *sw = the_sftpwin;
    HWND list;
    bool remote;

    if (!sw || col < 0 || col > 2)
        return;

    /*
     * 判断这个 header 属于哪个列表。两个列表的 header 走同一个子类
     * 过程，所以不能靠「哪个窗口是当前的」来猜，得比一遍句柄。
     */
    {
        HWND hl = (HWND)SendMessageW(sw->local_list, LVM_GETHEADER, 0, 0);
        HWND hr = (HWND)SendMessageW(sw->remote_list, LVM_GETHEADER, 0, 0);
        if (header == hr) {
            list = sw->remote_list;
            remote = true;
        } else if (header == hl) {
            list = sw->local_list;
            remote = false;
        } else {
            return;
        }
    }

    if (remote) {
        if (sw->sort_col_remote == col)
            sw->sort_asc_remote = !sw->sort_asc_remote;
        else {
            sw->sort_col_remote = col;
            sw->sort_asc_remote = true;
        }
    } else {
        if (sw->sort_col_local == col)
            sw->sort_asc_local = !sw->sort_asc_local;
        else {
            sw->sort_col_local = col;
            sw->sort_asc_local = true;
        }
    }

    /* 传输中不许刷新：列表正在被传输流程读取，重填会错位。 */
    if (sw->busy) {
        cn_set_status(sw, "传输进行中，暂不能改排序。");
        /* 状态已经改回去了，列头指示也得跟着还原。 */
        InvalidateRect(header, NULL, TRUE);
        return;
    }

    if (remote)
        cn_refresh_remote(sw);
    else
        cn_refresh_local(sw);

    /*
     * 两栏的列头都要重画：指示箭头只画在当前排序列上，另一栏若原来
     * 也有指示，现在不该再显示（那是它自己的状态，其实要保留）。
     * 实际上两栏各记各的状态，所以只重画自己这一个即可。
     */
    InvalidateRect(header, NULL, TRUE);
    (void)list;
}

static void cn_fill_list(SftpWin *sw, HWND list, SftpEntry *entries,
                         size_t count, bool remote)
{
    /*
     * 先排序再填充。排序参数是「列 + 方向」，比较器需要额外上下文，
     * 而 qsort 传不进去，所以用文件静态变量传参 —— 单线程、调用期间
     * 不会重入，够用。用完清掉，免得下次别的 qsort 调用误用陈值。
     */
    cn_sort_entries(entries, count,
                    remote ? sw->sort_col_remote : sw->sort_col_local,
                    remote ? sw->sort_asc_remote : sw->sort_asc_local,
                    remote);

    ListView_DeleteAllItems(list);

    /* 目录在前、文件在后，同类按名字排序，和资源管理器一致。 */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            if ((int)entries[i].is_dir != (pass == 0))
                continue;

            /*
             * 本地文件名来自 Win32 宽字符 API，转成多字节时用的是系统
             * 代码页（CP_ACP），所以这里照原路解回来。
             *
             * 远端文件名不一样：SFTP 协议里的路径是没有声明编码的原始
             * 字节串，主流服务端（OpenSSH 等）按 UTF-8 存。用 CP_ACP
             * 解它，在中文系统上等于拿 GBK 去啃 UTF-8 字节 —— 这类非法
             * 序列会被 MultiByteToWideChar 替换成 U+FFFD，中文名全成
             * 方块或问号。必须按 UTF-8 解。
             */
            wchar_t *wname = dup_mb_to_wc(remote ? CP_UTF8 : DEFAULT_CODEPAGE,
                                          entries[i].name);
            wchar_t szbuf[32], tbuf[64];
            if (entries[i].is_dir)
                wcscpy(szbuf, L"");
            else {
                cn_format_size(entries[i].size, szbuf, lenof(szbuf));
                (void)remote;
            }
            cn_format_time(entries[i].mtime, tbuf, lenof(tbuf));

            /* 第一列（名称）随行插入；后两列用 SETITEMTEXT 补。 */
            int idx = cn_list_insert_item(list, ListView_GetItemCount(list),
                                          wname,
                                          (LPARAM)entries[i].is_dir,
                                          cn_icon_for(sw, entries[i].is_dir));
            cn_list_set_text(list, idx, 1, szbuf);
            cn_list_set_text(list, idx, 2, tbuf);
            sfree(wname);
        }
    }
}

/* 取列表里第一个选中项的名字；没有选中返回 NULL。
 *
 * remote 决定名字还原成哪种编码：远端名字要以 UTF-8 交还给 SFTP，
 * 本地名字以系统代码页交还给 Win32。两边必须和填充列表时的解码
 * 方式对称，否则中文名一进目录就找不到（显示用的解码和回传用的
 * 编码不一致，等于自己给自己制造了一个坏路径）。 */
static char *cn_selected_name(HWND list, bool *is_dir, bool remote)
{
    int idx = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (idx < 0)
        return NULL;

    wchar_t buf[1024];
    cn_list_get_text(list, idx, 0, buf, lenof(buf));
    if (is_dir)
        *is_dir = cn_list_item_is_dir(list, idx);
    return dup_wc_to_mb(remote ? CP_UTF8 : DEFAULT_CODEPAGE, buf, "?");
}

/* 选中项全部名字，用于多选传输。 */
static char **cn_selected_all(HWND list, size_t *count, bool **dirs,
                              bool remote)
{
    char **names = NULL;
    size_t n = 0, size = 0;
    int idx = -1;
    bool *d = NULL;
    size_t dsize = 0;

    *count = 0;
    while ((idx = ListView_GetNextItem(list, idx, LVNI_SELECTED)) >= 0) {
        wchar_t buf[1024];
        cn_list_get_text(list, idx, 0, buf, lenof(buf));
        if (n >= size)
            sgrowarray(names, size, n);
        if (n >= dsize)
            sgrowarray(d, dsize, n);
        names[n] = dup_wc_to_mb(remote ? CP_UTF8 : DEFAULT_CODEPAGE, buf, "?");
        d[n] = cn_list_item_is_dir(list, idx);
        n++;
    }
    *count = n;
    if (dirs)
        *dirs = d;
    else
        sfree(d);
    return names;
}

/* ----------------------------------------------------------------------
 * 刷新两个列表。
 */

static void cn_fit_columns(SftpWin *sw);

static void cn_refresh_local(SftpWin *sw)
{
    char *errmsg = NULL;
    size_t count = 0;
    SftpEntry *entries = sftp_local_list(sw->localdir, &count, &errmsg);

    ListView_DeleteAllItems(sw->local_list);
    if (!entries) {
        cn_set_status(sw, errmsg ? errmsg : "无法读取本地目录。");
        sfree(errmsg);
        /* 列表已经被清空，上传按钮必须跟着回到禁用态。 */
        cn_update_transfer_buttons(sw);
        return;
    }
    cn_fill_list(sw, sw->local_list, entries, count, false);
    sftp_entries_free(entries, count);
    cn_set_text(sw->local_path, sw->localdir);
    /* 行数变了，竖向滚动条可能刚出现，列宽要跟着重算。 */
    cn_fit_columns(sw);
    /* 重填后选中项全没了，上传按钮要跟着回到禁用态。 */
    cn_update_transfer_buttons(sw);
}

static void cn_refresh_remote(SftpWin *sw)
{
    char *errmsg = NULL;
    size_t count = 0;
    SftpEntry *entries;

    if (!sw->sx) {
        ListView_DeleteAllItems(sw->remote_list);
        cn_update_transfer_buttons(sw);
        return;
    }

    entries = sftp_xfer_list(sw->sx, sw->remotedir, &count, &errmsg);
    ListView_DeleteAllItems(sw->remote_list);
    if (!entries) {
        cn_set_status(sw, errmsg ? errmsg : "无法读取远端目录。");
        sfree(errmsg);
        cn_update_transfer_buttons(sw);
        return;
    }
    cn_fill_list(sw, sw->remote_list, entries, count, true);
    sftp_entries_free(entries, count);
    cn_set_text(sw->remote_path, sw->remotedir);
    cn_fit_columns(sw);
    /* 重填后选中项全没了，下载按钮要跟着回到禁用态。 */
    cn_update_transfer_buttons(sw);
}

/* ----------------------------------------------------------------------
 * 进度条。
 */

typedef struct ProgressCtx {
    SftpWin *sw;
} ProgressCtx;

static bool cn_progress(void *ctx, uint64_t done, uint64_t total)
{
    ProgressCtx *pc = (ProgressCtx *)ctx;
    SftpWin *sw = pc->sw;

    /*
     * 记下当前进度。暂停时进度条就停在这个位置不动，续传后从这里
     * 接着涨 —— 所以这两个值必须在暂停期间保持有效，不能清。
     */
    sw->progress_done = done;
    sw->progress_total = total;

    SendMessageW(sw->progress, PBM_SETRANGE32, 0,
                 (LPARAM)(total ? (int32_t)total : 1000));
    if (total) {
        SendMessageW(sw->progress, PBM_SETPOS, (WPARAM)done, 0);
        {
            char *msg = dupprintf("%.1f / %.1f KB",
                                  done / 1024.0, total / 1024.0);
            cn_set_status(sw, msg);
            sfree(msg);
        }
    } else {
        cn_set_status(sw, "传输中…");
    }

    /*
     * 抽一下消息，让界面刷新、让「暂停 / 终止」按钮真能收到点击。
     * 这里必须照常派发 WM_COMMAND：这两个按钮的处理只是置标志，是
     * 安全的；其他按钮在传输期间已经被 EnableWindow(FALSE) 禁掉，
     * 不会产生点击。递归进入传输由 cn_do_transfer 开头的 sw->busy
     * 检查挡住。
     */
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return true;
}

/* ----------------------------------------------------------------------
 * 传输动作。
 */

/* ----------------------------------------------------------------------
 * 目录递归传输。
 *
 * 选中目录时把整棵子树搬过去。顺序是先建目标目录、再逐个处理子项，
 * 这样空目录也会被完整复制过来。
 *
 * 三道保险，防的是「递归进死循环」这类会卡死界面的情形：
 *
 *   1. 深度上限 CN_XFER_MAXDEPTH。正常情况下几层就到底，超过这个数
 *      基本可以断定数据本身有问题。
 *   2. 跳过符号链接 / 目录联接（junction）。本地侧靠
 *      FILE_ATTRIBUTE_REPARSE_POINT 判定 —— 这是 Windows 上唯一可靠
 *      的判据，只看「是目录」会掉进自引用环。远端侧没有 lstat 可用，
 *      靠深度上限兜底。
 *   3. 每处理一项都查一次取消标志，用户点了「取消」能立刻停住，不必
 *      等整棵子树走完。
 */
#define CN_XFER_MAXDEPTH 32

/*
 * 取本地路径的属性（宽字符转换 + 调 API + 释放，三处共用）。
 * 失败返回 INVALID_FILE_ATTRIBUTES。
 */
static DWORD cn_local_attr(const char *path)
{
    wchar_t *w = dup_mb_to_wc(DEFAULT_CODEPAGE, path);
    DWORD attr = GetFileAttributesW(w);
    sfree(w);
    return attr;
}

/* 本地路径的文件大小。读不到返回 0。 */
static uint64_t cn_local_size(const char *path)
{
    wchar_t *w = dup_mb_to_wc(DEFAULT_CODEPAGE, path);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    uint64_t size = 0;

    if (GetFileAttributesExW(w, GetFileExInfoStandard, &fad))
        size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    sfree(w);
    return size;
}

/*
 * 本地路径是否为重解析点（符号链接 / junction / 挂载点）。
 *
 * 递归复制时这类目录不能进：它们指向别处，按「目录」处理会一路绕回
 * 上层，形成死循环。Windows 上没有 lstat，FILE_ATTRIBUTE_REPARSE_POINT
 * 是判断它的标准依据。
 */
static bool cn_local_is_reparse(const char *path)
{
    DWORD attr = cn_local_attr(path);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return true;            /* 读不到属性就别往里钻了 */
    return (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

typedef struct XferStat {
    int ok;
    int fail;
    int skip;
    char *last_err;
    /*
     * 「只补缺」模式：目标端已存在的文件一律跳过，不覆盖。
     *
     * 语义挂在统计结构上而不是函数参数里，是因为它整棵树都要跟着走 ——
     * 递归下去每一层都得知道，挂在 st 上顺路就传下去了，不用把参数
     * 一路改一遍。整批确认里选了「跳过已存在」时置位。
     */
    bool only_missing;
} XferStat;

/* 递归里要查目标端是否存在，定义在后面（覆盖确认那一段）。 */
static bool cn_dst_exists(SftpWin *sw, const char *dst, bool to_remote);

/* 记一次失败原因，只保留最新一条，避免刷屏。 */
static void cn_xfer_note_fail(XferStat *st, const char *err) {
    st->fail++;
    sfree(st->last_err);
    st->last_err = dupstr(err && *err ? err : "未知错误");
}

/*
 * 递归搬运。src 与 dst 都是完整路径；to_remote 为真表示本地上传。
 * 返回 true 表示这一项（含子树）整体没有问题。
 */
static bool cn_xfer_recurse(SftpWin *sw, const char *src, const char *dst,
                            bool to_remote, int depth, XferStat *st)
{
    char *errmsg = NULL;

    if (sw->cancel_requested)
        return false;
    if (depth > CN_XFER_MAXDEPTH) {
        st->skip++;
        return false;
    }

    /* 先建目录。已存在也当成功继续，否则整棵子树都没地方落。 */
    if (to_remote) {
        if (!sftp_xfer_mkdir(sw->sx, dst, &errmsg)) {
            /*
             * 目录已存在时服务器会报错。这在「目标目录里已经有同名
             * 目录」的场合是常态，不该中断 —— 清掉错误继续往里传。
             */
            sfree(errmsg);
            errmsg = NULL;
        }
    } else {
        if (!sftp_local_mkdir(dst, &errmsg)) {
            sfree(errmsg);
            errmsg = NULL;
        }
    }

    {
        SftpEntry *kids = NULL;
        size_t nkids = 0;

        if (to_remote)
            kids = sftp_local_list(src, &nkids, &errmsg);
        else
            kids = sftp_xfer_list(sw->sx, src, &nkids, &errmsg);

        if (!kids) {
            char *ctx = dupprintf("列目录 %s：%s", src,
                                  errmsg ? errmsg : "(无错误信息)");
            cn_xfer_note_fail(st, ctx);
            sfree(ctx);
            sfree(errmsg);
            return false;
        }

        for (size_t i = 0; i < nkids; i++) {
            char *ssub, *dsub;

            if (sw->cancel_requested)
                break;

            ssub = cn_path_join(src, kids[i].name, !to_remote);
            dsub = cn_path_join(dst, kids[i].name, to_remote);

            if (kids[i].is_dir) {
                /*
                 * 链接目录一律不钻。
                 *
                 * 本地侧原本就跳过重解析点；远端侧此前拿不到链接信息，
                 * 只能靠 CN_XFER_MAXDEPTH 兜底 —— 遇到 a -> .. 这种
                 * 会一路钻到深度上限，把同一批文件反复传很多遍。
                 * 现在远端用 LSTAT 复核（sftp_xfer_is_symlink），
                 * READDIR 的类型位不可全信，所以下载方向也复核一次。
                 */
                bool is_link = kids[i].is_symlink;
                if (to_remote) {
                    if (cn_local_is_reparse(ssub))
                        is_link = true;
                } else if (!is_link) {
                    is_link = sftp_xfer_is_symlink(sw->sx, ssub);
                }

                if (is_link) {
                    st->skip++;
                } else {
                    cn_xfer_recurse(sw, ssub, dsub, to_remote,
                                    depth + 1, st);
                }
            } else {
                char *emsg = NULL;
                ProgressCtx pc;
                bool ok;

                pc.sw = sw;
                {
                    char *msg = dupprintf("正在传输 %s …", kids[i].name);
                    cn_set_status(sw, msg);
                    sfree(msg);
                }

                /*
                 * 「只补缺」模式下，目标端已有的文件直接跳过，不覆盖。
                 * 这是顶层目录确认里选了「否」的策略，整棵子树都跟着走。
                 */
                if (st->only_missing && cn_dst_exists(sw, dsub, to_remote)) {
                    st->skip++;
                    sfree(ssub);
                    sfree(dsub);
                    continue;
                }

                /*
                 * 递归内部是否允许续传，跟随这一批的模式：
                 *
                 *   续传批次 —— 允许。已传完的子文件按大小跳过，否则
                 *   暂停后继续会把整棵树从头再传一遍，白等一场。
                 *
                 *   普通批次 —— 不允许。用户刚点了上传/下载，目标端
                 *   已有同名同大小的文件不代表内容一样，必须真传。
                 *   目录是合并语义、内部不逐个问，所以更要从头传。
                 */
                if (to_remote)
                    ok = (sftp_xfer_upload(sw->sx, ssub, dsub,
                                           sw->resume_pending,
                                           cn_progress, &pc, &emsg) == XFER_OK);
                else
                    ok = (sftp_xfer_download(sw->sx, ssub, dsub,
                                             sw->resume_pending,
                                             cn_progress, &pc, &emsg) == XFER_OK);
                if (ok)
                    st->ok++;
                else {
                    char *ctx = dupprintf("传输 %s：%s", kids[i].name,
                                          emsg ? emsg : "(无错误信息)");
                    cn_xfer_note_fail(st, ctx);
                    sfree(ctx);
                }
                sfree(emsg);
            }

            sfree(ssub);
            sfree(dsub);
        }

        sftp_entries_free(kids, nkids);

        /*
         * 空目录建完就算这一项做成了。
         *
         * 成功数原本只在文件传成功时累加，空目录建好之后一个文件都没有，
         * 于是整批统计成「成功 0」—— 用户拖了个空文件夹，看提示还以为
         * 什么都没干。目录本身也是搬过去的东西，得算数。
         */
        if (nkids == 0)
            st->ok++;
    }

    return true;
}

/* ----------------------------------------------------------------------
 * 目标端是否已有同名项。
 *
 * 远端走 sftp_xfer_stat，本地走 GetFileAttributesW。覆盖策略的询问不是
 * 在这里做的：开跑前由 cn_collect_conflicts 扫一遍全批，汇总进一个弹窗
 * 问一次（见 cn_confirm_batch），这里只负责回答「有没有」。
 *
 * 远端 stat 失败按「不存在」处理：十有八九是 no such file；真有别的
 * 问题，传输那一步自己会报错，不用在这里拦第二遍。
 */

#define CN_OVW_OVERWRITE 1      /* 覆盖 */
#define CN_OVW_SKIP      2      /* 跳过此文件 */
#define CN_OVW_ABORT     3      /* 停止整批 */

/* 目标路径是否已存在（文件或目录都算）。 */
static bool cn_dst_exists(SftpWin *sw, const char *dst, bool to_remote)
{
    if (to_remote) {
        struct fxp_attrs attrs;
        return sw->sx && sftp_xfer_stat(sw->sx, dst, &attrs);
    } else {
        return cn_local_attr(dst) != INVALID_FILE_ATTRIBUTES;
    }
}

/* ----------------------------------------------------------------------
 * 冲突汇总与覆盖确认弹窗。
 *
 * 原来是一个文件问一次、一个目录问一次：拖十个同名文件进去就是十个
 * 弹窗，每弹一次都得抬手点一下，累人且容易点错。改成先整批扫一遍，
 * 把所有「目标端已存在」的项汇总进同一个弹窗，问一次、定一个策略，
 * 整批照它执行。
 *
 * 弹窗里最多列 CN_CONFLICT_SHOW_MAX 项，多出来的用「.......」顶替 ——
 * 窗口不能随着文件数量长到屏幕外面去。鼠标悬停在「.......」那一行时，
 * 用 tooltip 把全部冲突项显出来，这样既短得了，又不丢信息。
 */

#define CN_CONFLICT_SHOW_MAX 3

/* 弹窗里的按钮编号。避开工程里已用的 1xxx / 2xxx 段。 */
#define IDC_CNC_OVERWRITE 3101
#define IDC_CNC_SKIP      3102
#define IDC_CNC_CANCEL    3103
#define IDC_CNC_THIRD     3104   /* cn: 三按钮模式下的右按钮（保存并退出） */

typedef struct ConflictItem {
    wchar_t *name;              /* 显示用，已按来源编码转成宽字符 */
    bool is_dir;
} ConflictItem;

/*
 * cn: 自绘浮层的公共状态（「确认覆盖」与「确认保存」两个弹窗共用）。
 *
 * 抽成独立结构是因为两处需求完全一样：清单被折叠成「.......」时，
 * 鼠标悬停在清单区就把完整内容用浮层铺开。函数只认这个结构 + 父窗口，
 * 不依赖具体是哪个弹窗，避免复制一份两百行的孪生代码。
 */
typedef struct CnTipState {
    HWND tip;                   /* 浮层窗口，NULL 表示没有（未截断时不建） */
    HWND parent;                /* 弹窗窗口，浮层摆位与坐标换算的基准 */
    HFONT font;                 /* 绘制用字体，与所属弹窗同一份 */
    wchar_t *text;              /* 浮层全文，由所属弹窗释放 */
    RECT hot;                   /* 触发区（弹窗客户区坐标） */
    /*
     * cn: 摆位方式。
     *   CN_TIP_BELOW  —— 摆在触发区下方，下方不够翻到上方（确认覆盖用）。
     *   CN_TIP_RIGHT  —— 摆在触发区右侧，顶边与 trigger_top 齐平
     *                    （确认保存用，主人指定「右边、上框线与内容区齐平」）。
     * trigger_top 只在 CN_TIP_RIGHT 下有意义的「内容区上边」。
     */
    int mode;
    int trigger_top;
    bool shown;
} CnTipState;

#define CN_TIP_BELOW 0
#define CN_TIP_RIGHT 1

static void cn_conflicts_free(ConflictItem *items, size_t n)
{
    if (!items)
        return;
    for (size_t i = 0; i < n; i++)
        sfree(items[i].name);
    sfree(items);
}

/*
 * 自建模态弹窗的问号图标（IDI_QHELP）。
 *
 * 进程级缓存一份：图标是常驻资源，所有自建模态窗口共用，不用每建一个
 * 窗就 Load 一遍，也不用谁去 DestroyIcon —— 跟上游 putty.ico 同一套
 * 生命周期。取不到（资源缺失之类）返回 NULL，调用处跳过图标显示即可。
 */
static HICON cn_question_icon(int size)
{
    static HICON cached_small, cached_big;
    HICON *slot = size >= cn_dpi(32) ? &cached_big : &cached_small;

    if (!*slot)
        *slot = (HICON)LoadImageW(GetModuleHandle(NULL),
                                  MAKEINTRESOURCEW(IDI_QHELP), IMAGE_ICON,
                                  size, size, LR_DEFAULTCOLOR);
    return *slot;
}

/*
 * cn: 按资源 ID 取标题栏小图标（进程级按 ID 缓存，见 cn_question_icon）。
 *
 * 「确认覆盖」弹窗复用同一套模板，但标题栏该挂警告图标而不是问号 ——
 * 它问的是「要不要覆盖掉对方已有的文件」，不是「要不要保存」。
 * 缓存按资源 ID 分开存，几种图标各留一份，不必每次弹窗都 Load。
 *
 * 传 0 等价于 IDI_QHELP，方便调用方「不指定就用问号」。
 */
static HICON cn_title_icon(int id, int size)
{
    enum { CN_TITLE_ICON_MAX = 4 };
    static HICON cache[CN_TITLE_ICON_MAX][2];
    static int cache_id[CN_TITLE_ICON_MAX];
    int slot_big = size >= cn_dpi(32);
    int eff = id ? id : IDI_QHELP;

    for (int i = 0; i < CN_TITLE_ICON_MAX; i++) {
        if (cache_id[i] == eff) {
            if (!cache[i][slot_big])
                cache[i][slot_big] =
                    (HICON)LoadImageW(GetModuleHandle(NULL),
                                      MAKEINTRESOURCEW(eff), IMAGE_ICON,
                                      size, size, LR_DEFAULTCOLOR);
            return cache[i][slot_big];
        }
    }
    /* 找空槽；满了就退回不缓存（图标就那几个，正常不会满）。 */
    for (int i = 0; i < CN_TITLE_ICON_MAX; i++) {
        if (cache_id[i] == 0) {
            cache_id[i] = eff;
            cache[i][slot_big] =
                (HICON)LoadImageW(GetModuleHandle(NULL),
                                  MAKEINTRESOURCEW(eff), IMAGE_ICON,
                                  size, size, LR_DEFAULTCOLOR);
            return cache[i][slot_big];
        }
    }
    return cn_question_icon(size);
}

/* ===================================================================
 * cn: 确认类弹窗的新视觉体系（照主人给的设计图）。
 *
 * 设计图要点：浅灰内容区 + 白色圆角卡片 + 自绘标题栏（深蓝粗体标题、
 * 左侧蓝问号、右侧大 X）+ 清单每行按段上色（选项名与旧值同色、
 * 箭头与新值同色、行首勾/叉、右侧橙色装饰折角）+ 蓝框确定/灰框取消。
 *
 * 配色取自设计图像素采样，不是凭感觉调的：
 *   标题文字/图标   #002168   （采样 (0,33,104) 一档）
 *   内容区灰底      #F7F7F5   （采样 (247,247,245)）
 *   白卡片          #FFFFFF
 *   选项名+旧值     #4C4B49   （采样 (76,75,73)）
 *   箭头+新值       #00307C   （采样 (0,48,124)）
 *   装饰折角        #F3761A   （采样 (243,118,26)）
 *   确定按钮蓝框    #0863AB   （采样 (7,98,171)）
 * =================================================================== */

#define CN_CLR_BG         RGB(0xF7, 0xF7, 0xF5)
#define CN_CLR_CARD       RGB(0xFF, 0xFF, 0xFF)
#define CN_CLR_TEXT       RGB(0x4C, 0x4B, 0x49)
#define CN_CLR_ACCENT     RGB(0x00, 0x30, 0x7C)
#define CN_CLR_CHEVRON    RGB(0xF3, 0x76, 0x1A)
#define CN_CLR_BTN_BLUE   RGB(0x08, 0x63, 0xAB)
#define CN_CLR_CARD_EDGE  RGB(0xE4, 0xE4, 0xE1)
#define CN_CLR_HEAD_TEXT  RGB(0x1A, 0x1A, 0x1A)
/* 大问号圆环与字形（设计图里是近黑的深灰，不是蓝） */
#define CN_CLR_RING       RGB(0x3C, 0x3C, 0x3C)

/*
 * cn: 确认弹窗整体字号放大比例（主人 2026-11 要求「字体整体增大 10%」）。
 *
 * 只放大字号，不换字族 —— 字族仍取系统消息字体 lfMessageFont，换族会
 * 在不同系统上出现中文回退跳变。
 *
 * ⚠️ 弹窗里有三处按 lfMessageFont 建字体：正文 font、新值加粗
 *    font_value、以及建窗时测折行用的临时 meas_font。三处必须走同一个
 *    宏，否则会出现「量的是一套字号、画的是另一套」，折行行数算错。
 */
#define CN_FONT_SCALE_NUM 11
#define CN_FONT_SCALE_DEN 10

/*
 * cn: 底部按钮的实际高度（逻辑像素，走 cn_dpi 缩放）。
 *
 * 建窗算按钮带高度、创建按钮控件两处都必须用它 —— 历史上算高度用的
 * 是 32（槽位）、创建用的是 26（实际），多出的 6px 全压在按钮下方，
 * 造成「按钮贴底、上方空太多」的观感。
 */
#define CN_BTN_H 26

/*
 * cn: 文字相对基线（baseline）的视觉垂直中心偏移。
 *
 * 为什么需要它：GDI 的 TextOutW 以**字符框顶部**为 y 传入，而
 * GetTextExtentPoint32W 返回的 sz.cy 是整个字符框高度（含 ascent +
 * descent + 外部行距）。汉字墨迹并不在字符框正中 —— 它偏下，因为
 * ascent 里有一部分是给拉丁字母上伸部留的空白。
 *
 * 以前用 `y - sz.cy / 2` 居中，结果文字整体比左侧图标低约 4px（放大
 * 截图逐像素量出来的：图标墨迹 88..109，文字墨迹 80..109，底边齐平
 * 而顶边差 8px，视觉上就是「字往下掉」）。
 *
 * 正确做法：拿字体真实度量 tmAscent / tmDescent，文字视觉中心相对
 * 基线的位置是 (tmAscent - tmDescent) / 2。要让这个中心落在 cy 上，
 * 基线就该在 cy + (tmAscent - tmDescent) / 2，再用 TA_BASELINE 输出。
 *
 * 返回「基线 y」，调用方配 SetTextAlign(TA_BASELINE) 使用。
 */
static int cn_text_baseline_for_center(HDC hdc, int cy)
{
    TEXTMETRICW tm;

    if (!GetTextMetricsW(hdc, &tm))
        return cy;                 /* 取不到度量就退回原样，不崩 */

    return cy + (tm.tmAscent - tm.tmDescent) / 2;
}

/*
 * cn: 按放大比例调整 LOGFONT 的字高。
 *
 * lfHeight 为负表示「字高（字符框高度）」，为正表示「字符高度」，
 * 两种含义都是数值越大字越大，所以直接按比例缩放绝对值即可。
 * 值为 0 表示「用默认」，不去动它。
 */
static void cn_scale_logfont(LOGFONTW *lf)
{
    if (lf->lfHeight != 0)
        lf->lfHeight = lf->lfHeight * CN_FONT_SCALE_NUM / CN_FONT_SCALE_DEN;
}

/*
 * cn: 确认弹窗首行说明的唯一文案来源。
 *
 * ⚠️ 这个字符串同时被三处使用：文案常量（cn_launch_texts /
 *    cn_quit_texts）、构建函数的默认值、以及宽度测量里量首行宽度那段。
 *    以前三处各写一份字面量，改文案时测量那份忘了同步，窗口宽度就会
 *    按旧文案算、新版面被裁。统一到宏，改一处即可。
 */
#define CN_HEAD_TEXT  "以下配置项已被修改，请核对后保存或撤销："

/*
 * cn: 卡片左侧大问号的尺寸宏（CN_QICON_MIN / MAX）已随图标一起删除
 * （主人 2026-11：只保留标题栏的小问号）。宽度预留、卡片高度下限、
 * 绘制三条路径上的引用都已清掉，恢复时需一并加回。
 */

/*
 * cn: 窗口圆角裁剪（SetWindowRgn）已随自绘外壳一起撤掉。
 * 现在窗口走原生 WS_CAPTION | WS_SYSMENU，圆角由系统按当前 Windows
 * 版本自行处理；再手动裁剪只会把系统边框切出锯齿。
 * 唯一保留的圆角是内容区白卡片（见 cn_fill_round_rect）。
 */

/* 画一个圆角矩形填充（含 1px 描边）。 */
static void cn_fill_round_rect(HDC hdc, RECT *rc, int radius,
                               COLORREF fill, COLORREF edge)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HBRUSH oldbr = SelectObject(hdc, br);
    HPEN oldpen = SelectObject(hdc, pen);

    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom,
              radius, radius);

    SelectObject(hdc, oldbr);
    SelectObject(hdc, oldpen);
    DeleteObject(br);
    DeleteObject(pen);
}

/*
 * cn: 行首状态图标 —— 圆圈里打勾 / 打叉，或调用方指定的资源图标。
 *
 * 早期是纯 GDI 现画（Ellipse 画圈 + LineTo 画叉 / Polyline 画勾）。
 * 不用字体里的 ✓ ✗ 是对的（那套字形在不同系统上缺字或大小不一，
 * 且圆环画不出来），但 GDI 的 Ellipse 不做抗锯齿，18px 下勉强能看，
 * 高 DPI 或放大后就是一圈台阶 —— 与卡片左侧大问号同一个毛病。
 *
 * 现在改为资源图标：
 *   icon_id 非 0                          → 直接用该资源（「确认覆盖」用：
 *                                           目录 → 警告图标，文件 → 圆点）
 *   CN_CHG_TOGGLE_OFF（新值是「关 / 禁用」）→ IDI_CN_CROSS
 *   其余（开启 / 切换 / 改值）              → IDI_CN_CHECK
 * 两个 ICO 各含 16~256 十档，系统挑最接近的尺寸，边缘平滑。
 *
 * 主人指定「关闭类打叉，其余打勾」，非开关改动一律打勾。
 *
 * 调用方给的 (cx, cy) 是圆心，这里换算成左上角交给 DrawIconEx。
 */
static void cn_draw_check_icon(HDC hdc, int cx, int cy, int size,
                               CnChangeKind kind, int icon_id)
{
    HICON ic;
    int id = icon_id ? icon_id
                     : (kind == CN_CHG_TOGGLE_OFF ? IDI_CN_CROSS
                                                  : IDI_CN_CHECK);

    if (size <= 0)
        return;

    ic = (HICON)LoadImageW(GetModuleHandle(NULL),
                           MAKEINTRESOURCEW(id), IMAGE_ICON,
                           size, size, LR_DEFAULTCOLOR);
    if (!ic)
        return;                        /* 资源缺失就不画，不阻塞弹窗 */

    DrawIconEx(hdc, cx - size / 2, cy - size / 2, ic, size, size,
               0, NULL, DI_NORMAL);
    DestroyIcon(ic);
}

/*
 * cn: 行尾装饰折角「>」。
 *
 * 纯装饰 —— 主人明确要求不做悬停反馈、不可点。画两段线拼成折角，
 * 粗细与配色都照设计图（橙 #F3761A）。
 */
static void cn_draw_chevron(HDC hdc, int cx, int cy, int size)
{
    POINT pts[3];
    HPEN pen = CreatePen(PS_SOLID, 2, CN_CLR_CHEVRON);
    HPEN oldpen = SelectObject(hdc, pen);
    int half = size / 2;

    pts[0].x = cx - half / 2;  pts[0].y = cy - half;
    pts[1].x = cx + half / 2;  pts[1].y = cy;
    pts[2].x = cx - half / 2;  pts[2].y = cy + half;
    Polyline(hdc, pts, 3);

    SelectObject(hdc, oldpen);
    DeleteObject(pen);
}

/*
 * cn: 自绘标题栏右侧那个大 X 已删除（cn_draw_close_x）。
 * 现在走原生 WS_SYSMENU，关闭按钮由系统绘制并自带悬停效果，
 * 不再需要自己画、也不再需要 WM_LBUTTONUP 命中判定。
 */


/* 宽字符串复制。工程里只有 char 版的 dupstr，宽字符得自己来。 */
static wchar_t *cn_wcsdup(const wchar_t *s)
{
    size_t n = s ? wcslen(s) + 1 : 1;
    wchar_t *r = snewn(n, wchar_t);
    if (s)
        wcscpy(r, s);
    else
        r[0] = L'\0';
    return r;
}

/*
 * 扫一遍这批条目，把目标端已存在的挑出来。
 *
 * names 来自源列表，编码跟着源侧走：源是远端时是 UTF-8，源是本地时
 * 是系统代码页 —— 这里按 to_remote 反推，别搞错，否则中文名会变成
 * 问号。
 */
static ConflictItem *cn_collect_conflicts(SftpWin *sw, char **names,
                                          bool *dirs, size_t count,
                                          bool to_remote, size_t *outn)
{
    const char *dst_dir = to_remote ? sw->remotedir : sw->localdir;
    int enc = to_remote ? DEFAULT_CODEPAGE : CP_UTF8;
    ConflictItem *items = NULL;
    size_t n = 0, size = 0;

    *outn = 0;
    /* 上传要问远端，没连接就查不出来；下载问的是本地，不需要连接。 */
    if (to_remote && !sw->sx)
        return NULL;

    for (size_t i = 0; i < count; i++) {
        char *dst = cn_path_join(dst_dir, names[i], to_remote);
        bool exists = cn_dst_exists(sw, dst, to_remote);

        sfree(dst);
        if (!exists)
            continue;

        if (n >= size)
            sgrowarray(items, size, n);
        items[n].name = dup_mb_to_wc(enc, names[i]);
        items[n].is_dir = dirs ? dirs[i] : false;
        n++;
    }
    *outn = n;
    return items;
}

/*
 * 自绘浮层：显示全部冲突项。
 *
 * 一个无边框顶层小窗，客户区就是我们自己 WM_PAINT 画的多行文字。宽高
 * 由文本实测决定，位置贴着「.......」那一行的下方，出屏就往上翻。
 *
 * 为什么不用系统 tooltips_class32：见调用处注释 —— 它的自动尺寸在多行
 * 中文下不受 TTM_SETMAXTIPWIDTH 控制，实测恒为 114px 且硬裁文字。
 */
/*
 * cn: 浮层配色 —— 主人指定黑底（#171b23）白字。
 *
 * 不用系统 COLOR_INFOBK/INFOTEXT：那是淡黄底黑字的信息框配色，
 * 在浅灰弹窗上糊成一片，而且主人明确要深色浮层。深色也让浮层与
 * 底下的白色内容区一眼分得开，不会看成内容的一部分。
 */
#define CN_TIP_BG     RGB(0x17, 0x1b, 0x23)
#define CN_TIP_FG     RGB(0xff, 0xff, 0xff)
#define CN_TIP_EDGE   RGB(0x3a, 0x42, 0x50)   /* 深底描边，浅色桌面上不糊边 */

/* 把浮层整体画到给定 DC 上（WM_PAINT 与 WM_PRINTCLIENT 共用）。 */
static void cn_tip_paint(HWND hwnd, HDC hdc, CnTipState *st)
{
    RECT rc;
    HFONT old;

    if (!st || !st->text)
        return;

    GetClientRect(hwnd, &rc);
    /* 底色与描边：与 WM_ERASEBKGND 同一套配色，别两处各写一份。 */
    {
        HBRUSH br = CreateSolidBrush(CN_TIP_BG);
        HBRUSH edge = CreateSolidBrush(CN_TIP_EDGE);
        FillRect(hdc, &rc, br);
        {
            RECT e = rc;
            e.bottom = e.top + 1;
            FillRect(hdc, &e, edge);
            e = rc;
            e.top = e.bottom - 1;
            FillRect(hdc, &e, edge);
            e = rc;
            e.right = e.left + 1;
            FillRect(hdc, &e, edge);
            e = rc;
            e.left = e.right - 1;
            FillRect(hdc, &e, edge);
        }
        DeleteObject(br);
        DeleteObject(edge);
    }

    old = SelectObject(hdc, st->font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, CN_TIP_FG);
    /*
     * 内缩一圈再画。建窗时宽度是按同一套 DrawText 规则量的，
     * 若这里不留白，文字就会顶死在左上角，看着像被裁掉。
     */
    InflateRect(&rc, -cn_dpi(8), -cn_dpi(6));
    /*
     * DT_EDITCONTROL 让多行按行高整齐排布，DT_NOPREFIX 免得
     * 名字里出现 & 时被吃掉。宽高都够（建窗前已按文本量算好），
     * 所以不需要 DT_WORDBREAK 去折行。
     */
    DrawTextW(hdc, st->text, -1, &rc,
              DT_LEFT | DT_TOP | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(hdc, old);
}

static LRESULT CALLBACK cn_tip_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam)
{
    CnTipState *st = (CnTipState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
      case WM_PAINT:
        if (st && st->text) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            cn_tip_paint(hwnd, hdc, st);
            EndPaint(hwnd, &ps);
        }
        return 0;
      /*
       * 自检用：把整窗画到外部 DC（内存位图）上。不接这条，
       * SendMessage(WM_PRINTCLIENT) 会落到默认过程，位图一片空白，
       * 自检读像素就永远是白的（实测踩过）。
       */
      case WM_PRINTCLIENT:
        cn_tip_paint(hwnd, (HDC)wparam, st);
        return 0;
      case WM_ERASEBKGND:
        if (st) {
            HDC hdc = (HDC)wparam;
            RECT rc;
            HBRUSH br = CreateSolidBrush(CN_TIP_BG);
            HBRUSH pen_br = CreateSolidBrush(CN_TIP_EDGE);

            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, br);
            /* 一圈 1px 描边：黑底压在深色终端/壁纸上时不至于没边界。 */
            {
                RECT e = rc;
                e.bottom = e.top + 1;
                FillRect(hdc, &e, pen_br);
                e = rc;
                e.top = e.bottom - 1;
                FillRect(hdc, &e, pen_br);
                e = rc;
                e.right = e.left + 1;
                FillRect(hdc, &e, pen_br);
                e = rc;
                e.left = e.right - 1;
                FillRect(hdc, &e, pen_br);
            }
            DeleteObject(br);
            DeleteObject(pen_br);
        }
        return 1;
      case WM_NCHITTEST:
        /* 浮层不该抢鼠标：被判成透明，消息落回下面的弹窗。 */
        return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/*
 * 建浮层（不显示）。文本为空直接返回 NULL。
 *
 * 宽度取「最长一行实测宽度 + 内边距」，高度取「行数 × 行高 + 内边距」，
 * 都是量出来的，所以换字体、换语言都不会把字切掉。
 */
static HWND cn_tip_create(CnTipState *st)
{
    static const wchar_t classname[] = L"PuTTYSftpConflictTip";
    HINSTANCE inst = GetModuleHandle(NULL);
    WNDCLASSW wc;
    RECT need = { 0, 0, 0, 0 };
    int padx = cn_dpi(8), pady = cn_dpi(6), w, h;

    if (!st->text || !*st->text)
        return NULL;

    /*
     * 用 DT_CALCRECT 量，和 cn_tip_proc 里的 DrawTextW 走同一套规则。
     * 早先用 GetTextExtentPoint32W 逐行量，与绘制端的行高与内缩口径
     * 不一致，算出来的宽高会偏小，文字看着像被切掉。
     */
    {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            HFONT old = SelectObject(hdc, st->font);
            need.right = cn_dpi(2000);   /* 给个够大的宽，只为取最宽行 */
            DrawTextW(hdc, st->text, -1, &need,
                      DT_CALCRECT | DT_LEFT | DT_TOP | DT_NOPREFIX |
                      DT_EDITCONTROL);
            SelectObject(hdc, old);
            ReleaseDC(NULL, hdc);
        }
    }
    if (need.right <= 0 || need.bottom <= 0)
        return NULL;

    w = need.right + padx * 2;
    h = need.bottom + pady * 2;

    if (!GetClassInfoW(inst, classname, &wc)) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = cn_tip_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = classname;
        RegisterClassW(&wc);
    }

    {
        HWND tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                   classname, NULL, WS_POPUP,
                                   0, 0, w, h, NULL, NULL, inst, NULL);
        if (tip)
            SetWindowLongPtrW(tip, GWLP_USERDATA, (LONG_PTR)st);
        return tip;
    }
}

/*
 * 浮层摆位。
 *
 * CN_TIP_BELOW：摆在触发区下方，下方放不下就翻到上方（「确认覆盖」用）。
 * CN_TIP_RIGHT：摆在触发区右侧，顶边与内容区上边齐平（「确认保存」用）。
 *               右侧放不下就翻到左侧；竖直方向只在超出工作区时收拢。
 */
static void cn_tip_place(CnTipState *st)
{
    RECT ar, wa;
    POINT pt;
    int w, h, x, y;

    if (!st->tip || !st->parent)
        return;
    if (!GetWindowRect(st->tip, &ar))
        return;
    w = ar.right - ar.left;
    h = ar.bottom - ar.top;

    if (st->mode == CN_TIP_RIGHT) {
        /*
         * 锚点：弹窗客户区右缘，竖直位置取 trigger_top（内容区上边）。
         *
         * ⚠️ 不能用 st->hot.right 当横向锚点 —— 那是清单文字的右缘，
         *    离客户区右缘只剩十几到二十几像素，浮层从文字右缘 +8 起算
         *    就会压在窗口边框上（实测浮层左缘 1112 压住弹窗右缘 1133）。
         *    「放在右边」指的是整个弹窗的右边外侧，所以取客户区右缘。
         *
         *    竖直方向取 trigger_top 而不是 hot.top：清单在问号图标下方
         *    起算，用 hot.top 会比内容区上框线低一截。
         */
        RECT cr;
        if (!GetClientRect(st->parent, &cr))
            return;
        pt.x = cr.right;
        pt.y = st->trigger_top;
        ClientToScreen(st->parent, &pt);

        x = pt.x + cn_dpi(8);          /* 与弹窗右缘留一点缝 */
        y = pt.y;

        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            if (x + w > wa.right) {    /* 右边放不下，翻到左侧 */
                POINT lp;
                lp.x = 0;
                lp.y = st->trigger_top;
                ClientToScreen(st->parent, &lp);
                x = lp.x - w - cn_dpi(8);
            }
            if (x < wa.left) x = wa.left;
            if (y + h > wa.bottom) y = wa.bottom - h;
            if (y < wa.top) y = wa.top;
        }
        SetWindowPos(st->tip, HWND_TOPMOST, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        st->shown = true;
        return;
    }

    /* 锚点：触发区左下角（屏幕坐标）。 */
    pt.x = st->hot.left;
    pt.y = st->hot.bottom;
    ClientToScreen(st->parent, &pt);

    x = pt.x;
    y = pt.y + cn_dpi(2);

    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        if (y + h > wa.bottom) {          /* 下方不够，翻到行上方 */
            POINT up;
            up.x = st->hot.left;
            up.y = st->hot.top;
            ClientToScreen(st->parent, &up);
            y = up.y - h - cn_dpi(2);
        }
        if (y < wa.top) y = wa.top;
        if (x + w > wa.right) x = wa.right - w;
        if (x < wa.left) x = wa.left;
    }
    SetWindowPos(st->tip, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    st->shown = true;
}

static void cn_tip_hide(CnTipState *st)
{
    if (st->tip && st->shown) {
        ShowWindow(st->tip, SW_HIDE);
        st->shown = false;
    }
}

/*
 * 鼠标移动时统一处理浮层的显隐。两个弹窗的 WM_MOUSEMOVE 都调它，
 * 行为完全一致（进触发区显示、离开隐藏、并订阅 WM_MOUSELEAVE 免得
 * 鼠标移出窗口时收不到通知导致浮层赖着不走）。
 */
static void cn_tip_track(CnTipState *st, HWND hwnd, LPARAM lparam)
{
    POINT pt;
    TRACKMOUSEEVENT tme;

    if (!st->tip)
        return;

    pt.x = GET_X_LPARAM(lparam);
    pt.y = GET_Y_LPARAM(lparam);
    if (PtInRect(&st->hot, pt))
        cn_tip_place(st);
    else
        cn_tip_hide(st);

    memset(&tme, 0, sizeof(tme));
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd;
    TrackMouseEvent(&tme);
}

/* ====================================================================
 * cn: 传输结果提示条（toast）。
 *
 * 一批传输收尾后，在窗口底部浮出一条深色圆角提示：滑入 -> 停留 -> 淡出，
 * 约 1.5 秒后自己消失，不打断任何操作。
 *
 * 为什么不用状态栏那一行：状态栏贴着窗口最底、字又小，传完这一刻用户视线
 * 还在两个列表上，那一行改了也多半没看见。提示条浮在列表下沿附近，正好
 * 落在视线余光里。
 *
 * 几条刻意的取舍：
 *  - 不拦鼠标（HTTRANSPARENT）、不抢焦点（WS_EX_NOACTIVATE）：它压着窗口
 *    底部，用户紧接着可能就要点「终止」或拖下一批文件，被挡一下都烦。
 *  - 用一个窗口反复显示，换文案就地改尺寸重排：连续传几批时新提示顶掉
 *    旧的，不会在屏幕上叠出一摞。
 *  - 圆角靠 SetWindowRgn 裁，淡入淡出靠分层窗口的 alpha，全是 GDI 手段，
 *    不必为此引 Direct2D。
 */
#define CN_TOAST_IN       0
#define CN_TOAST_HOLD     1
#define CN_TOAST_OUT      2

#define CN_TOAST_TIMER        0x5F31  /* 定时器 ID，只挂在提示条自己的窗口上 */
#define CN_TOAST_FRAME_MS     16      /* 约 60fps */
#define CN_TOAST_IN_FRAMES    10      /* 滑入淡入 160ms */
#define CN_TOAST_OUT_FRAMES   10      /* 淡出 160ms */
#define CN_TOAST_HOLD_FRAMES  95      /* 停留 ≈ 1.5 秒 */

/* 与冲突浮层同一套深色配色，界面里深浅两种浮层不必各调一种黑。 */
#define CN_TOAST_BG    RGB(0x17, 0x1b, 0x23)
#define CN_TOAST_FG    RGB(0xff, 0xff, 0xff)
#define CN_TOAST_EDGE  RGB(0x3a, 0x42, 0x50)

/* 提示条用的字体：跟主窗口列表同一套，只是要保证拿得到一份可用的。 */
static HFONT cn_toast_font(SftpWin *sw)
{
    if (!sw->toast.font) {
        /*
         * 借主窗口那份：它在 cn_sftpwin_dispose 里统一释放，这里只借用不
         * 拥有（font_owned 恒为 false），免得两个地方都去删同一个句柄。
         */
        if (sw->font)
            sw->toast.font = sw->font;
        else
            sw->toast.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return sw->toast.font;
}

/*
 * 按当前文案量出提示条客户区尺寸。
 *
 * DT_CALCRECT 用与绘制端完全相同的标志，量出来多少就是多大 —— 早先浮层
 * 那里曾用 GetTextExtentPoint32W 另算一遍，两套规则不一致导致文字被切，
 * 这里直接照浮层的正确做法来。
 */
static bool cn_toast_measure(SftpWin *sw, int *out_w, int *out_h)
{
    RECT need = { 0, 0, 0, 0 };
    HDC hdc;
    HFONT old;
    int padx = cn_dpi(22), pady = cn_dpi(13);

    if (!sw->toast.text || !*sw->toast.text)
        return false;

    hdc = GetDC(NULL);
    if (!hdc)
        return false;
    old = SelectObject(hdc, cn_toast_font(sw));
    need.right = cn_dpi(4000);      /* 给够宽，只为量出单行文字宽度 */
    DrawTextW(hdc, sw->toast.text, -1, &need,
              DT_CALCRECT | DT_LEFT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);

    if (need.right <= 0 || need.bottom <= 0)
        return false;

    *out_w = need.right + padx * 2;
    *out_h = need.bottom + pady * 2;
    return true;
}

/* 把提示条画到给定 DC 上（WM_PAINT 与 WM_PRINTCLIENT 共用）。 */
static void cn_toast_paint(HWND hwnd, HDC hdc, SftpWin *sw)
{
    RECT rc;
    HFONT old;

    if (!sw || !sw->toast.text)
        return;

    GetClientRect(hwnd, &rc);
    {
        HBRUSH br = CreateSolidBrush(CN_TOAST_BG);
        HBRUSH edge = CreateSolidBrush(CN_TOAST_EDGE);
        RECT e;

        FillRect(hdc, &rc, br);
        /* 一圈 1px 描边：深色卡片压在同为深色的终端或壁纸上时才有边界。 */
        e = rc; e.bottom = e.top + 1;      FillRect(hdc, &e, edge);
        e = rc; e.top = e.bottom - 1;      FillRect(hdc, &e, edge);
        e = rc; e.right = e.left + 1;      FillRect(hdc, &e, edge);
        e = rc; e.left = e.right - 1;      FillRect(hdc, &e, edge);
        DeleteObject(br);
        DeleteObject(edge);
    }

    old = SelectObject(hdc, cn_toast_font(sw));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, CN_TOAST_FG);
    /* DT_SINGLELINE + 垂直居中：提示条是定高的一条，文案都是短句。 */
    DrawTextW(hdc, sw->toast.text, -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);
}

/*
 * 摆位：水平居中于主窗口客户区，底边压在窗口底部上方一点。
 *
 * anchor 是提示条的**底边**，slide 是在滑入阶段额外下压的像素数 ——
 * 从下方几像素处往上滑到位，比原地淡入更有「刚发生了什么」的方向感。
 */
static void cn_toast_place(SftpWin *sw, int slide)
{
    RECT cr, tr;
    POINT pt;
    int w, h, x, y;

    if (!sw->toast.hwnd || !sw->hwnd)
        return;
    if (!GetClientRect(sw->hwnd, &cr) || !GetWindowRect(sw->toast.hwnd, &tr))
        return;
    w = tr.right - tr.left;
    h = tr.bottom - tr.top;

    pt.x = (cr.left + cr.right) / 2;
    /*
     * 停在状态栏之上：状态栏那一行是 20 多像素高，再给 20 像素缝隙，
     * 提示条就不会把「完成：成功 3…」那行盖住 —— 两者说的是同一件事，
     * 但一个是即时确认、一个是留存记录，不该互相遮挡。
     */
    pt.y = cr.bottom - cn_dpi(44);
    ClientToScreen(sw->hwnd, &pt);

    x = pt.x - w / 2;
    y = pt.y - h + slide;

    SetWindowPos(sw->toast.hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* 圆角裁剪：成功后 region 归系统所有，不要在调用方删。 */
static void cn_toast_round(HWND hwnd)
{
    RECT rc;
    HRGN rgn;

    if (!GetClientRect(hwnd, &rc))
        return;
    rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1,
                             cn_dpi(14), cn_dpi(14));
    if (rgn)
        SetWindowRgn(hwnd, rgn, TRUE);
}

/*
 * 每帧一次：推进阶段机，更新透明度与位置。
 *
 * 三个阶段各自只数自己的帧，切换时把 tick 归零，读起来比一个全局计数
 * 加减去判断偏移清楚。
 */
static void cn_toast_tick(SftpWin *sw)
{
    int alpha, slide = 0, total, hold = CN_TOAST_HOLD_FRAMES;

    /*
     * 停留时长允许被 CN_TOAST_HOLD_MS 覆盖，只为截图/看图服务 —— 1.5 秒
     * 的节奏下截屏很难对准。这是调试口子，不要写进任何用户配置。
     */
    {
        static int hold_cached = -1;
        if (hold_cached < 0) {
            const char *e = getenv("CN_TOAST_HOLD_MS");
            int ms = e ? atoi(e) : 0;
            hold_cached = ms > 0 ? ms / CN_TOAST_FRAME_MS
                                 : CN_TOAST_HOLD_FRAMES;
        }
        hold = hold_cached;
    }

    sw->toast.tick++;

    switch (sw->toast.phase) {
      case CN_TOAST_IN:
        total = CN_TOAST_IN_FRAMES;
        if (sw->toast.tick >= total) {
            sw->toast.phase = CN_TOAST_HOLD;
            sw->toast.tick = 0;
        }
        break;
      case CN_TOAST_HOLD:
        total = hold;
        if (sw->toast.tick >= total) {
            sw->toast.phase = CN_TOAST_OUT;
            sw->toast.tick = 0;
        }
        break;
      default:                    /* CN_TOAST_OUT */
        total = CN_TOAST_OUT_FRAMES;
        if (sw->toast.tick >= total) {
            /* 淡出到头：收工。窗口留着，下次改文案再用。 */
            KillTimer(sw->toast.hwnd, CN_TOAST_TIMER);
            ShowWindow(sw->toast.hwnd, SW_HIDE);
            sw->toast.phase = CN_TOAST_IN;
            sw->toast.tick = 0;
            return;
        }
        break;
    }

    if (sw->toast.phase == CN_TOAST_IN) {
        alpha = 255 * sw->toast.tick / CN_TOAST_IN_FRAMES;
        /* 滑入：起始下压 8 逻辑像素，随帧数线性收拢到 0。 */
        slide = cn_dpi(8) * (CN_TOAST_IN_FRAMES - sw->toast.tick) /
                CN_TOAST_IN_FRAMES;
    } else if (sw->toast.phase == CN_TOAST_OUT) {
        alpha = 255 * (CN_TOAST_OUT_FRAMES - sw->toast.tick) /
                CN_TOAST_OUT_FRAMES;
    } else {
        alpha = 255;
    }

    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    SetLayeredWindowAttributes(sw->toast.hwnd, 0, (BYTE)alpha, LWA_ALPHA);
    cn_toast_place(sw, slide);
    InvalidateRect(sw->toast.hwnd, NULL, TRUE);
}

static LRESULT CALLBACK cn_toast_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam)
{
    SftpWin *sw = (SftpWin *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
      case WM_PAINT:
        if (sw && sw->toast.text) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            cn_toast_paint(hwnd, hdc, sw);
            EndPaint(hwnd, &ps);
        }
        return 0;
      /* 自检截图用：不接这条，WM_PRINTCLIENT 落到默认过程，位图全白。 */
      case WM_PRINTCLIENT:
        cn_toast_paint(hwnd, (HDC)wparam, sw);
        return 0;
      case WM_ERASEBKGND:
        return 1;               /* WM_PAINT 里整片重画，不再擦一遍闪一下 */
      case WM_NCHITTEST:
        return HTTRANSPARENT;   /* 鼠标穿透：底下的按钮照常能点 */
      case WM_TIMER:
        if (wparam == CN_TOAST_TIMER && sw)
            cn_toast_tick(sw);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/*
 * 建提示条窗口（首次调用时）。文本为空直接返回 false。
 *
 * owner 传主窗口：这样主窗口最小化 / 关闭时系统会一并处理掉提示条，
 * 不至于留一条黑卡片孤零零浮在桌面上。
 */
static bool cn_toast_create(SftpWin *sw)
{
    static const wchar_t classname[] = L"PuTTYSftpToast";
    HINSTANCE inst = GetModuleHandle(NULL);
    WNDCLASSW wc;
    int w, h;

    if (!cn_toast_measure(sw, &w, &h))
        return false;

    if (!GetClassInfoW(inst, classname, &wc)) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = cn_toast_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = classname;
        RegisterClassW(&wc);
    }

    sw->toast.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_LAYERED | WS_EX_TRANSPARENT,
        classname, NULL, WS_POPUP, 0, 0, w, h, sw->hwnd, NULL, inst, NULL);
    if (!sw->toast.hwnd)
        return false;

    SetWindowLongPtrW(sw->toast.hwnd, GWLP_USERDATA, (LONG_PTR)sw);
    cn_toast_round(sw->toast.hwnd);
    return true;
}

/*
 * 显示一条提示（几秒后自己消失）。
 *
 * 无窗口环境（自检里 SftpWin 全是假的）直接丢掉，不建窗 —— 那些用例只验
 * 传输结果，不需要也没有屏幕。
 */
static void cn_toast_show(SftpWin *sw, const char *text)
{
    wchar_t *w;
    int nw, nh;

    if (!sw || !sw->hwnd || !text || !*text)
        return;

    w = dup_mb_to_wc(DEFAULT_CODEPAGE, text);
    if (!w || !*w) {
        sfree(w);
        return;
    }
    sfree(sw->toast.text);
    sw->toast.text = w;

    if (!sw->toast.hwnd && !cn_toast_create(sw)) {
        sfree(sw->toast.text);
        sw->toast.text = NULL;
        return;
    }

    /*
     * 文案换长了就得重排尺寸，否则新字会被旧窗宽切掉。尺寸一样时这一步
     * 也走，代码少一条分支，代价只是两次 SetWindowPos。
     */
    if (cn_toast_measure(sw, &nw, &nh)) {
        RECT tr;
        if (GetWindowRect(sw->toast.hwnd, &tr))
            if (tr.right - tr.left != nw || tr.bottom - tr.top != nh)
                SetWindowPos(sw->toast.hwnd, NULL, 0, 0, nw, nh,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        cn_toast_round(sw->toast.hwnd);
    }

    /* 从滑入重新开始：连续两批之间不会出现「刚显示就淡出」的怪相。 */
    sw->toast.phase = CN_TOAST_IN;
    sw->toast.tick = 0;
    SetLayeredWindowAttributes(sw->toast.hwnd, 0, 0, LWA_ALPHA);
    cn_toast_place(sw, cn_dpi(8));
    SetTimer(sw->toast.hwnd, CN_TOAST_TIMER, CN_TOAST_FRAME_MS, NULL);
    InvalidateRect(sw->toast.hwnd, NULL, TRUE);
}

/*
 * 收掉提示条（窗口销毁 / 释放资源时调）。
 *
 * 先把 GWLP_USERDATA 清掉再销毁：DestroyWindow 会同步派发 WM_DESTROY 与
 * WM_NCDESTROY，清掉归属关系后窗口过程即便再被调用也拿不到即将失效的
 * SftpWin，不会被解引用到已释放内存。
 */
static void cn_toast_destroy(SftpWin *sw)
{
    if (!sw)
        return;
    if (sw->toast.hwnd) {
        KillTimer(sw->toast.hwnd, CN_TOAST_TIMER);
        SetWindowLongPtrW(sw->toast.hwnd, GWLP_USERDATA, 0);
        DestroyWindow(sw->toast.hwnd);
        sw->toast.hwnd = NULL;
    }
    sfree(sw->toast.text);
    sw->toast.text = NULL;
    /* 字体是借用主窗口的，只清指针不删对象。 */
    sw->toast.font = NULL;
}

/*
 * 生成一批传输收尾的提示文案。
 *
 * 覆盖四种行为：上传成功、下载成功、全部覆盖、跳过已存在。混合情形
 * （整棵目录里既有新文件又有已存在的）拼成一句，两件事都交代到。
 *
 * overwrite 表示这一批**真的问过冲突、且答案是「全部覆盖」**。只有它能
 * 把「传成功了」和「把目标端原文件整个重写了一遍」区分开，后者更该让
 * 用户看见。普通上传传成功走的是下面的通用文案，不该带「覆盖」字样 ——
 * 压根没问过的事，不能凭空替用户认下来。
 */
static char *cn_toast_xfer_text(bool to_remote, int ok, int skip,
                               bool overwrite)
{
    if (overwrite && ok > 0)
        return dupprintf(to_remote ? "全部覆盖：已重新上传 %d 项"
                                   : "全部覆盖：已重新下载 %d 项", ok);
    if (skip > 0 && ok > 0)
        return dupprintf("%s %d 项，另跳过 %d 项（已存在）",
                         to_remote ? "已上传" : "已下载", ok, skip);
    if (skip > 0)
        return dupprintf("已跳过 %d 项（目标端已存在）", skip);
    if (ok > 0)
        return dupprintf(to_remote ? "已上传 %d 项到远端"
                                   : "已从远端下载 %d 项", ok);
    return NULL;
}

/*
 * cn: 把一批冲突项用「原始自绘模板」问一遍，返回模板的原始结果
 * （IDOK = 跳过已存在 / IDYES = 全部覆盖 / IDCANCEL = 停止整批）。
 *
 * 单独抽出来是为了让 -confirm-overwrite-show 看图入口能复用同一份
 * 数据映射与文案：截图看到的必须是线上真实效果，不能另拼一份长得像的。
 *
 * 数据适配：ConflictItem 只有名字和「是不是目录」，而模板吃 CnChange
 * （名字 + 旧值 + 新值 + 行首图标）。这里映射成「纯名字行」：旧值 / 新值
 * 留空串，排版器就不会拼冒号、也不会画箭头；行首图标按目录 / 文件分 ——
 * 目录用警告（覆盖目录是连锅端），文件用中性圆点。
 *
 * 按钮排列：模板的三按钮槽位固定是「ok / cancel / third」，其中 third
 * 恒为最右、恒为默认（回车触发）。所以「全部覆盖」放 third，「跳过已存在」
 * 放 ok（最左），「取消」放中间。返回值按 IDOK / IDYES / IDCANCEL 映射，
 * 不依赖按钮资源 ID —— 那些是模板内部的槽位编号，与语义无关。
 */
static int cn_overwrite_ask(HWND parent, ConflictItem *items, size_t n)
{
    /*
     * ⚠️ 清单必须**堆分配**，不能用栈变量再取地址交给
     *    cn_change_list_free —— 那个函数最后一句是 sfree(list)，按
     *    所有权约定它会连结构体一起 free。传栈地址进去等于把栈帧
     *    交给堆管理器释放，点任何一个按钮都必崩（四个按钮都崩、
     *    功能全不生效的真凶）。
     *
     *    自检一直没抓到，是因为它只走字符串路径（changes 非 NULL），
     *    从没经过这个结构化入参的调用方；而 cb->changes 那侧的释放
     *    对象本来就是 snew 出来的，所以碰巧一直是对的。
     */
    CnChangeList *cl = snew(CnChangeList);
    CnConfirmTexts texts;
    char *head;
    int rc;

    /*
     * n 没有上限（一批上千个文件也正常），所以堆上开数组。字符串都
     * 过 dupstr，释放统一交给 cn_change_list_free。
     */
    cl->n = n;
    cl->items = n ? snewn(n, CnChange) : NULL;
    for (size_t i = 0; i < n; i++) {
        cl->items[i].name = dup_wc_to_mb(DEFAULT_CODEPAGE, items[i].name,
                                        "?");
        cl->items[i].oldval = dupstr("");
        cl->items[i].newval = dupstr("");
        cl->items[i].kind = CN_CHG_OTHER;
        /*
         * 行首图标一律中性圆点（主人 2026-11 决定）：
         * 这一行只是陈述「该项已存在于目标位置」，不表达危险度 ——
         * 危险提示交给标题栏那一个警告图标，内容区再挂感叹号是语义重复，
         * 而且「同名目录已存在」本身也不算什么危险动作。
         *
         * 目录 / 文件在清单里一视同仁，不再用图标区分。
         */
        cl->items[i].icon_id = IDI_CN_DOT;
    }

    head = dupprintf("%d 个同名项目已存在于目标位置：", (int)n);

    texts.title = "确认覆盖";
    texts.head = head;
    texts.ok = "跳过已存在";             /* 最左，返回 IDOK */
    texts.cancel = "取消";               /* 中间，返回 IDCANCEL */
    texts.default_on_right = true;
    texts.third = "全部覆盖";             /* 最右，返回 IDYES，默认 */
    texts.icon_id = IDI_CN_WARN;         /* 标题栏挂警告图标，不是问号 */
    texts.third_is_danger = true;        /* 红框挂在最右那个 */

    rc = cn_confirm_dialog_texts(parent, NULL, cl, &texts, NULL);

    cn_trace("cn_overwrite_ask: 弹窗返回 rc=%d n=%lu", rc, (unsigned long)n);
    sfree(head);
    cn_trace("cn_overwrite_ask: head 已释放，准备 free 清单 cl=%p items=%p",
             (void *)cl, cl ? (void *)cl->items : NULL);
    cn_change_list_free(cl);
    cn_trace("cn_overwrite_ask: 清单已释放，正常返回");
    return rc;
}

/*
 * cn: 整批冲突一次问清。没有冲突就静默放行，绝不弹空窗。
 *
 * 从 2026-11 起走「原始自绘模板」（与确认保存 / 退出确认同一套）：
 * 浅灰底 + 白色圆角卡片 + 自绘分段清单 + 底部按钮带。旧的独立实现
 * （cn_conflict_dialog_create / cn_conflict_loop / 原生 BUTTON + STATIC
 * 那一套四百行）已整个删除 —— 两个弹窗视觉不一致的根源就是它。
 *
 * 数据适配：ConflictItem 只有名字和「是不是目录」，而模板吃 CnChange
 * （名字 + 旧值 + 新值 + 行首图标）。这里把冲突项映射成「纯名字行」：
 * 旧值 / 新值留空串，排版器就不会拼冒号、也不会画箭头；行首图标按
 * 目录 / 文件分 —— 目录用警告（覆盖目录是连锅端），文件用中性圆点。
 *
 * 返回值仍是 CN_OVW_*。三个按钮从左到右：
 *   「跳过已存在」(→ CN_OVW_SKIP) / 「取消」(→ CN_OVW_ABORT) /
 *   「全部覆盖」(→ CN_OVW_OVERWRITE，最右、默认、红框危险动作)。
 * 主人 2026-11 明确要求「把全部覆盖的按键位置调整至最右，默认即全部覆盖」。
 */
static int cn_confirm_batch(SftpWin *sw, ConflictItem *items, size_t n)
{
    int rc;

    if (n == 0)
        return CN_OVW_OVERWRITE;

    rc = cn_overwrite_ask(sw->hwnd, items, n);

    /*
     * 返回值映射。Esc 与标题栏 X 在模板里都归到 IDCANCEL，这里一并
     * 当作「停止整批」—— 关掉一个问「要不要覆盖」的窗，绝不能悄悄
     * 当成「全部覆盖」。
     */
    switch (rc) {
      case IDOK:
        return CN_OVW_SKIP;
      case IDYES:
        return CN_OVW_OVERWRITE;
      default:
        return CN_OVW_ABORT;
    }
}

/*
 * cn: 「原始自绘模板」—— 确认保存 / 连接前确认 / 退出确认 / 确认覆盖
 * 四个弹窗共用这一套：浅灰底 + 白色圆角卡片 + 自绘分段清单 + 底部
 * 按钮带。标题栏挂一个图标，卡片内不再放大问号（主人 2026-11 删除）。
 * 清单最多显示 CN_CONFIRM_SAVE_MAX_ITEMS 项，超出的折叠成省略行
 * （同一画法，鼠标悬停用浮层看全部）。
 * 返回 IDOK / IDCANCEL / IDYES（三按钮模式下第三个按钮）。
 *
 * 文案可覆盖：点「连接」「文件传输」时也要弹同一个框，只是标题、首行
 * 和两个按钮的说法不同（「仅本次生效」「长期生效」）。传 NULL 表示
 * 沿用「确认保存」那套默认文案。CnConfirmTexts 定义在 putty.h。
 */
#define CN_CONFIRM_SAVE_MAX_ITEMS 3

typedef struct ConfirmSaveBox {
    HWND dlg;
    HWND head, list, qicon;              /* 首行 / 清单 / 旧的问号图标（已弃用） */
    HWND b_ok, b_cancel;
    HWND b_third;                        /* 三按钮模式下的右按钮，否则为 NULL */
    /*
     * cn: 需要画成红框红字的那个按钮（危险动作），NULL 表示没有。
     *
     * 退出确认里是「直接退出」（最左），确认覆盖里是「全部覆盖」（最右）。
     * 红框挂哪个由文案结构的 third_is_danger 决定，不能靠位置猜 ——
     * 同样是三按钮，两边的危险项恰恰在相反两侧。
     */
    HWND b_danger;
    HFONT font;                          /* 自建则需释放，stock 不释放 */
    HFONT font_value;                    /* 新值粗体（自建，需释放） */
    bool font_owned;
    CnTipState tip;                      /* 折叠成「.......」时的悬浮浮层 */
    int band_h;                          /* 底部按钮带高度（WM_ERASEBKGND 填灰用） */
    int result;
    /*
     * cn: 自绘布局参数。控件仍在（STATIC / BUTTON 保留，便于自检读文本、
     * 保留键盘焦点与无障碍），但清单 STATIC 设为不可见，文字由
     * WM_PAINT 分段画到白卡片上；卡片、勾叉、折角自绘。
     *
     * 标题栏与窗口边框走系统原生（WS_CAPTION | WS_SYSMENU），
     * 不再自绘 —— 自绘的 X 与原生边框风格不统一，主人已否掉。
     *
     * 不彻底删控件的原因：自检要靠 GetWindowTextW 读回文本，按钮要
     * 保留焦点与键盘操作，这些系统能力自己实现代价太大。
     */
    int title_h;                         /* 标题栏占位（原生外壳下为 0） */
    int card_top, card_bottom;           /* 白卡片上下边（客户区坐标） */
    int card_left, card_right;
    int row_h;                           /* 清单每行高 */
    int icon_sz;                         /* 行首勾/叉直径 */
    int chev_sz;                         /* 行尾折角尺寸 */
    /*
     * cn: 卡片左侧大问号已删除（主人 2026-11，只留标题栏小问号），
     * 原来的 qicon_sz 字段随之去掉，不再有「画多大问号」这个状态。
     */
    /*
     * cn: 本帧要画几项清单、要不要补省略行。
     *
     * 由 cn_confirm_save_paint 在居中排版时一次算定，绘制阶段照单执行。
     * 不能在绘制循环里逐行判断「还放不放得下」—— 居中已经把省略号的
     * 位置算进内容高了，再判一次会因 6px 余量把它踢掉（省略号消失）。
     */
    size_t draw_n;
    bool draw_dots;
    bool shot_done;                      /* 截图模式：已存过 BMP */
    CnChangeList *changes;               /* 结构化清单（可能为 NULL） */
    /*
     * cn: 是否由右上角 ❌（WM_CLOSE）关掉。点右按钮和按 ❌ 都返回
     * IDCANCEL，但「连接/文件传输」那个弹窗里两者语义不同 ——
     * ❌ 等同「仅本次生效」（不落盘），右按钮是「长期生效」（落盘）。
     * 只有这个标志能把它们分开。
     */
    bool closed_by_x;
} ConfirmSaveBox;

/*
 * cn: 把清单的某一行按段画出来。
 *
 * 一行 = 勾/叉图标 + 选项名 + 「：」+ 旧值 + 「 → 」+ 新值。
 * 配色照设计图：选项名与旧值同色（灰 #4C4B49），箭头与新值同色
 * （深蓝 #00307C），新值另加粗。行首图标按 kind 决定勾或叉。
 *
 * 为什么不用 STATIC 控件：STATIC 只能整行一个颜色，分段上色必须自绘。
 * STATIC 仍保留但设为不可见，只为自检能 GetWindowTextW 读回文本。
 *
 * cn: 折行规则（主人 2026-11 二次修订）——
 *   「改动项是一个整体，超出了应该先触发改动项的换行，然后再计算
 *     改动值的字数是否需要换行」。
 *
 * 两级折行：
 *   1) 改动项 = 选项名 + 「：」+ 旧值 + 箭头，先当整体量宽度。
 *      - 整体放得下 → 留在首行；
 *      - 整体放不下 → 改动项整体换到下一行（首行只留勾叉图标），
 *        然后在新行上重新判断能不能放下；
 *      - 换了行还放不下（选项名本身就比整行宽）→ 才在改动项内部
 *        按字符折行，绝不让文字冲出卡片右缘。
 *   2) 改动项落位后再量新值：剩余宽度放不下就在新值内部按字符折行。
 *
 * 绘制与测高共用 cn_layout_change_row() 一个排版器，避免两边分叉
 * （历史上分叉过一次，折到第二行的内容被卡片下沿裁掉）。
 */

/*
 * cn: 一项改动的排版结果 —— 每段文字的起始位置与所在行号。
 *
 * 段：0 = 改动项（选项名：旧值），1 = 箭头，2 = 新值。
 * 每段可能跨多行（seg_pos[] 记录该段从第几个字符开始换行）。
 */
typedef struct CnRowLayout {
    int lines;                  /* 总行数 */
    int seg_line[3];            /* 各段首行行号（0 基） */
    int seg_x[3];               /* 各段首行 x 起点 */
    int seg_pos[3];             /* 各段起始字符下标（折行后从 0 开始） */
    int wrap[3];                /* 各段内部是否折行 */
    int arrow_line;             /* 箭头所在行（段 1 画完后的位置） */
    int arrow_x;                /* 箭头 x 起点 */
    /*
     * cn: 本行到底有没有箭头。
     *
     * 纯名字行（旧值、新值都为空，如「确认覆盖」的文件名清单）不该画
     * 箭头；有改动值的行才画。绘制端靠这个标志判断，不再自己猜。
     */
    int arrow_drawn;
    int first_text_x;           /* 文字起点（图标右侧），续行也用它 */
} CnRowLayout;

/* cn: 二分找「从 pos 起、宽度不超过 avail 的最长前缀字符数」。 */
static int cn_fit_prefix(HDC hdc, const wchar_t *s, int len, int avail)
{
    int lo = 1, hi = len, fit = 1;

    if (len <= 0)
        return 0;
    if (avail <= 0)
        return 1;                   /* 至少一个字符，防死循环 */

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        SIZE sz;
        GetTextExtentPoint32W(hdc, s, mid, &sz);
        if (sz.cx <= avail) {
            fit = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return fit;
}

/*
 * cn: 排版一项改动 —— 绘制与测高共用。
 *
 * 只算位置，不画。返回总行数。avail 是「文字起点 → 卡片右内缘」宽度。
 */
static int cn_layout_change_row(HDC hdc, const CnChange *ch, int avail,
                                HFONT font, HFONT font_value,
                                CnRowLayout *out)
{
    static const wchar_t arrow[] = L" → ";
    wchar_t *wseg = NULL, *wnew = NULL, *wold = NULL;
    int segw, arroww, neww, oldw, lines = 1;
    int cursor_x, cursor_line;
    HFONT oldfont;
    SIZE sz;
    int seglen, nlen, olen;

    memset(out, 0, sizeof(*out));
    out->first_text_x = 0;

    wnew = dup_mb_to_wc(DEFAULT_CODEPAGE, ch->newval);
    wold = dup_mb_to_wc(DEFAULT_CODEPAGE, ch->oldval);
    nlen = (int)wcslen(wnew);
    olen = (int)wcslen(wold);

    {
        strbuf *sb = strbuf_new();
        /*
         * cn: 段 0 只放「选项名：」，不含旧值。
         *
         * 主人 2026-11 明确要求「改选项只包含到『：』，不应该连『关』
         * 也包含」。原先这里拼的是 "%s：%s"（名字 + 旧值），视觉上旧值
         * 粘在名字后面，折行时也被当成名字的一部分一起下移，读起来像
         * 名字叫「XXX：关」。现在旧值归到段 1，与箭头一起排。
         *
         * cn: 纯名字行（旧值与新值都为空）不出冒号。
         *
         * 「确认覆盖」弹窗的清单每行只是一个同名项的名字，前面还有个
         * 行首图标，写成「a.txt：」会让人以为冒号后面还该跟一个值，
         * 视觉上像被截断。这里按「没有值就不加冒号」处理。
         */
        if (olen > 0 || nlen > 0)
            put_fmt(sb, "%s：", ch->name);
        else
            put_fmt(sb, "%s", ch->name);
        wseg = dup_mb_to_wc(DEFAULT_CODEPAGE, sb->s);
        strbuf_free(sb);
    }
    seglen = (int)wcslen(wseg);

    oldfont = SelectObject(hdc, font);
    GetTextExtentPoint32W(hdc, wseg, seglen, &sz);
    segw = sz.cx;
    GetTextExtentPoint32W(hdc, arrow, 3, &sz);
    arroww = sz.cx;
    GetTextExtentPoint32W(hdc, wold, olen, &sz);
    oldw = sz.cx;
    SelectObject(hdc, font_value ? font_value : font);
    GetTextExtentPoint32W(hdc, wnew, nlen, &sz);
    neww = sz.cx;
    SelectObject(hdc, font);

    cursor_x = 0;
    cursor_line = 0;

    /*
     * 段 0：只放「选项名：」。
     *
     * 旧值不在这里 —— 它归段 1（旧值 + 箭头），所以名字永远不会被
     * 旧值拖着一起换行。主人明确要求「改选项只包含到冒号」。
     *
     * 名字自己都比整行宽时（超长选项名）才在段内按字符折行。
     */
    if (segw <= avail) {
        out->seg_line[0] = 0;
        out->seg_x[0] = 0;
        out->seg_pos[0] = 0;
        out->wrap[0] = 0;
        cursor_line = 0;
        cursor_x = segw;
    } else {
        /* 段本身比整行还宽 → 段内折行（选项名很长时才会走到这里）。 */
        out->seg_line[0] = 0;
        out->seg_x[0] = 0;
        out->seg_pos[0] = 0;
        out->wrap[0] = 1;
        {
            int pos = 0;
            while (pos < seglen) {
                int fit;
                GetTextExtentPoint32W(hdc, wseg + pos, seglen - pos, &sz);
                if (sz.cx <= avail) {
                    cursor_x = sz.cx;
                    break;
                }
                fit = cn_fit_prefix(hdc, wseg + pos, seglen - pos, avail);
                pos += fit;
                lines++;
                cursor_line = lines - 1;
                cursor_x = 0;
                if (lines > 12)
                    break;
            }
        }
    }

    /*
     * 段 1：旧值 + 箭头，作为**一个不可分整体**排。
     *
     * cn: 主人 2026-11 指出「『关』这个旧值没有和『→开』连一块到下一行」。
     * 根因是旧值与箭头分开排：旧值在行末剩余宽度里放得下就留在上一行，
     * 只有箭头被挤下去，于是「关」孤零零吊在行末、下一行光秃秃一个
     * 「→ 开」。读起来像是选项名结尾带个「关」。
     *
     * 现在把「旧值 → 」当整体：当前行放不下就整段下移，新值紧跟其后，
     * 「关 → 开」一起出现在续行。只有旧值本身比整行还宽（比如超长文件
     * 路径）时才在段内逐字符折行 —— 那种情况再怎么挪也放不下一行。
     */
    out->seg_line[1] = cursor_line;
    out->seg_x[1] = cursor_x;
    out->seg_pos[1] = 0;
    out->wrap[1] = 0;
    {
        int valw = oldw + arroww + neww;   /* 「旧值 → 新值」整段宽度 */
        bool fits_here = (olen > 0 && cursor_x + valw <= avail);
        bool fits_next = (olen > 0 && !fits_here && cursor_x > 0 &&
                          valw <= avail);

        if (fits_here) {
            /* 整段改动值在当前行放得下，原地排。 */
            out->arrow_line = cursor_line;
            out->arrow_x = cursor_x + oldw;
            out->arrow_drawn = 1;
            cursor_x += oldw + arroww;
        } else if (fits_next) {
            /*
             * 当前行放不下、但整段单独一行放得下 → 旧值连箭头、新值
             * 一起下移，保证「关 → 开」在同一行。
             */
            lines++;
            cursor_line = lines - 1;
            out->seg_line[1] = cursor_line;
            out->seg_x[1] = 0;
            out->arrow_line = cursor_line;
            out->arrow_x = oldw;
            out->arrow_drawn = 1;      /* 能进这里必然有旧值 */
            cursor_x = oldw + arroww;
        } else {
            /* 超长旧值：段内折行（无旧值时循环不执行）。 */
            int pos = 0;
            int line = cursor_line;
            int x = cursor_x;

            while (pos < olen) {
                int fit, room = avail - x;
                GetTextExtentPoint32W(hdc, wold + pos, olen - pos, &sz);
                if (sz.cx <= room) {
                    x += sz.cx;
                    break;
                }
                fit = cn_fit_prefix(hdc, wold + pos, olen - pos, room);
                pos += fit;
                out->wrap[1] = 1;
                line++;
                x = 0;
                if (line > 12)
                    break;
            }
            if (line > cursor_line)
                lines = line + 1;
            cursor_line = line;
            cursor_x = x;

            /* 箭头接在旧值末尾；放不下就换行。 */
            if (cursor_x + arroww > avail && cursor_x > 0) {
                lines++;
                cursor_line = lines - 1;
                cursor_x = 0;
            }
            out->arrow_line = cursor_line;
            out->arrow_x = cursor_x;
            /*
             * cn: 只有真画箭头时才把 cursor 往后推一个箭头宽。
             *
             * 纯名字行（无旧值无新值）走这里，若照样推进，光标位置就
             * 白多出一个「 → 」宽 —— 单行时看不出来，一旦该行有后续
             * 内容或折行判断就会莫名其妙提前换行。
             */
            /*
             * cn: 纯名字行（旧值、新值都空）到此不画箭头。
             *
             * 走到 else 分支且 olen == 0，说明没有旧值 —— 「确认覆盖」
             * 的清单就是这样。此时若还把箭头标成「要画」，行尾就吊着
             * 一个孤零零的「→」。只有真带值（旧值或新值非空）才画。
             */
            out->arrow_drawn = (olen > 0 || nlen > 0) ? 1 : 0;
            if (out->arrow_drawn)
                cursor_x += arroww;
        }
    }

    /* 段 2：新值。剩余宽度放不下就折行，续行用整行宽度。 */
    out->seg_line[2] = cursor_line;
    out->seg_x[2] = cursor_x;
    out->seg_pos[2] = 0;
    out->wrap[2] = 0;
    if (nlen > 0) {
        int pos = 0;
        int line = cursor_line;
        int x = cursor_x;
        SelectObject(hdc, font_value ? font_value : font);
        while (pos < nlen) {
            int fit, room = avail - x;
            GetTextExtentPoint32W(hdc, wnew + pos, nlen - pos, &sz);
            if (sz.cx <= room)
                break;
            fit = cn_fit_prefix(hdc, wnew + pos, nlen - pos, room);
            pos += fit;
            out->wrap[2] = 1;
            line++;
            x = 0;
            if (line > 12)
                break;
        }
        if (line > cursor_line)
            lines = line + 1;
        SelectObject(hdc, font);
    }

    out->lines = lines;
    SelectObject(hdc, oldfont);
    sfree(wseg);
    sfree(wnew);
    return lines;
}

/*
 * cn: 把清单的某一行按段画出来。
 *
 * 一行 = 勾/叉图标 + 选项名 + 「：」+ 旧值 + 「 → 」+ 新值。
 * 配色照设计图：选项名与旧值同色（灰 #4C4B49），箭头与新值同色
 * （深蓝 #00307C），新值另加粗。行首图标按 kind 决定勾或叉。
 *
 * 为什么不用 STATIC 控件：STATIC 只能整行一个颜色，分段上色必须自绘。
 * STATIC 仍保留但设为不可见，只为自检能 GetWindowTextW 读回文本。
 *
 * 折行规则见 cn_layout_change_row() —— 本函数只负责把排版结果画出来，
 * 不再自己算折行，避免与测高分叉。
 *
 * 返回本行实际占用高度（可能是多行），供调用方累加。
 */
static int cn_draw_change_row(HDC hdc, const CnChange *ch, int x, int y,
                              int right_limit, int row_h, int icon_sz,
                              int chev_sz, HFONT font, HFONT font_value)
{
    int text_x = x + icon_sz + cn_dpi(8);   /* 文字起点（图标右侧） */
    int avail = right_limit - text_x;
    int cy = y + row_h / 2;
    int line_h = row_h;
    int i;
    SIZE sz;
    wchar_t *segs[3];
    CnRowLayout lay;
    HFONT oldfont;

    if (avail < cn_dpi(16))
        avail = cn_dpi(16);

    /* 行首勾/叉：对齐首行的垂直中心。 */
    cn_draw_check_icon(hdc, x + icon_sz / 2, cy, icon_sz, ch->kind,
                       ch->icon_id);

    SetBkMode(hdc, TRANSPARENT);
    oldfont = SelectObject(hdc, font);

    cn_layout_change_row(hdc, ch, avail, font, font_value, &lay);

    {
        strbuf *sb = strbuf_new();
        /*
         * 段 0 只画「选项名：」，旧值归段 1（见排版器说明）。
         * 纯名字行（无旧值也无新值）不加冒号，与排版器同一判据 ——
         * 两边一旦分叉，量出来是一行、画出来带个多余的冒号。
         */
        if ((ch->oldval && ch->oldval[0]) || (ch->newval && ch->newval[0]))
            put_fmt(sb, "%s：", ch->name);
        else
            put_fmt(sb, "%s", ch->name);
        segs[0] = dup_mb_to_wc(DEFAULT_CODEPAGE, sb->s);
        strbuf_free(sb);
    }
    /* 段 1 = 旧值 + 箭头，但两者颜色不同，绘制时再拆成两小段。 */
    segs[1] = dup_mb_to_wc(DEFAULT_CODEPAGE, ch->oldval);
    segs[2] = dup_mb_to_wc(DEFAULT_CODEPAGE, ch->newval);

    /*
     * 段 0：选项名（灰）。可能段内折行。
     * 段 1：旧值（灰）+ 箭头（蓝），从 lay.seg_x[1] 起顺序画。
     * 段 2：新值（蓝、加粗），从 lay.seg_x[2] 起。
     */
    for (i = 0; i < 3; i++) {
        int pos = 0, line = lay.seg_line[i], lx = lay.seg_x[i];
        int len = (int)wcslen(segs[i]);
        HFONT f = (i == 2 && font_value) ? font_value : font;
        COLORREF clr = (i == 2) ? CN_CLR_ACCENT : CN_CLR_TEXT;

        if (len <= 0)
            continue;
        SelectObject(hdc, f);
        SetTextColor(hdc, clr);

        while (pos < len) {
            int fit, room = avail - lx;
            int liney = cy + line * line_h;

            GetTextExtentPoint32W(hdc, segs[i] + pos, len - pos, &sz);
            if (sz.cx <= room || room <= 0) {
                if (sz.cx <= room) {
                    /*
                     * cn: 用基线对齐 + 视觉中心校正，而不是
                     *     「liney - sz.cy/2」。后者把字符框当墨迹框，
                     *     汉字会整体下沉几个像素（与左侧图标不齐）。
                     */
                    int by = cn_text_baseline_for_center(hdc, liney);
                    SetTextAlign(hdc, TA_LEFT | TA_BASELINE);
                    TextOutW(hdc, text_x + lx, by, segs[i] + pos, len - pos);
                    SetTextAlign(hdc, TA_LEFT | TA_TOP);
                    break;
                }
                /* 房间为 0 但还有内容：换行再画，防死循环。 */
                fit = 1;
            } else {
                fit = cn_fit_prefix(hdc, segs[i] + pos, len - pos, room);
            }
            GetTextExtentPoint32W(hdc, segs[i] + pos, fit, &sz);
            {
                int by = cn_text_baseline_for_center(hdc, liney);
                SetTextAlign(hdc, TA_LEFT | TA_BASELINE);
                TextOutW(hdc, text_x + lx, by, segs[i] + pos, fit);
                SetTextAlign(hdc, TA_LEFT | TA_TOP);
            }
            pos += fit;
            line++;
            lx = 0;
            if (line > 12)
                break;
        }

        /*
         * 段 1 画完后补画箭头（蓝色）。箭头位置由排版器给出，
         * 不在这里重算 —— 两边各算一次必然分叉。
         *
         * cn: 纯名字行（旧值与新值都为空）不画箭头。
         * 排版器那边 `olen > 0` 才排改动值，箭头位置保持初值；
         * 这里若无条件画，就会出现「a.txt    →」这种后面吊着个
         * 光箭头的怪样子 —— 「确认覆盖」的清单每行都是纯名字，
         * 一进来就全中。
         */
        if (i == 1 && lay.arrow_drawn) {
            static const wchar_t arrow[] = L" → ";
            int liney = cy + lay.arrow_line * line_h;
            int by = cn_text_baseline_for_center(hdc, liney);

            SelectObject(hdc, font);
            SetTextColor(hdc, CN_CLR_ACCENT);
            SetTextAlign(hdc, TA_LEFT | TA_BASELINE);
            TextOutW(hdc, text_x + lay.arrow_x, by, arrow, 3);
            SetTextAlign(hdc, TA_LEFT | TA_TOP);
        }
    }

    (void)chev_sz;

    SelectObject(hdc, oldfont);
    sfree(segs[0]);
    sfree(segs[1]);
    sfree(segs[2]);
    return lay.lines * line_h;
}

/*
 * cn: 量一项改动折行后占几行 —— 直接复用排版器，杜绝两套规则分叉。
 *
 * 卡片高度必须在建窗时就定下来（要交给 AdjustWindowRectEx），而绘制
 * 是后来才发生的。历史上这里另写了一套估算，跟绘制对不上，导致折到
 * 第二行的内容被卡片下沿裁掉。现在只保留 cn_layout_change_row() 一个
 * 排版器，绘制与测高都调它。
 *
 * avail 是「图标右侧文字起点 → 卡片右内缘」的宽度。avail <= 0 时返回 1。
 */
static int cn_change_row_lines(HDC hdc, const CnChange *ch, int avail,
                               HFONT font, HFONT font_value)
{
    CnRowLayout lay;

    if (avail <= 0)
        return 1;

    return cn_layout_change_row(hdc, ch, avail, font, font_value, &lay);
}

/* cn: 前置声明 —— dump 函数要调用它，而它定义在后面。 */
static void cn_confirm_save_paint(HWND hwnd, ConfirmSaveBox *cb, HDC hdc);

/* cn: 前置声明 —— 建窗时要用它算卡片高度，定义在绘制函数之后。 */
static int cn_change_row_lines(HDC hdc, const CnChange *ch, int avail,
                               HFONT font, HFONT font_value);

/*
 * cn: 把当前客户区渲染成 24 位 BMP 落盘（环境变量 CN_CONFIRM_SAVE_SHOT
 * 给出路径时启用）。
 *
 * 为什么不靠外部脚本截屏：本机 Windows Terminal / 沙箱环境下，外部
 * PowerShell 枚举不到本进程的顶层窗口（EnumWindows 返回 0 条，连主窗
 * 都没有），PrintWindow 与 FindWindow 全部失效。窗口自己画自己最可靠，
 * 也跟自检取像素走同一条 WM_PRINTCLIENT 路径，看到的就是真实的。
 */
static void cn_confirm_save_dump_bmp(HWND hwnd, ConfirmSaveBox *cb,
                                     const char *path)
{
    RECT rc;
    HDC screen, memdc;
    HBITMAP bmp, oldbmp;
    BITMAPINFO bi;
    int w, h, rowsz;
    unsigned char *bits = NULL;
    FILE *fp;
    wchar_t *wpath;

    if (!path || !*path)
        return;
    /* 整窗尺寸（含标题栏与边框），这样截图能看到完整外壳。 */
    GetWindowRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return;

    screen = GetDC(NULL);
    if (!screen)
        return;
    memdc = CreateCompatibleDC(screen);
    bmp = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(NULL, screen);
    if (!memdc || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (memdc) DeleteDC(memdc);
        return;
    }
    oldbmp = SelectObject(memdc, bmp);

    /* 先铺白底，避免 alpha 区域残影。 */
    {
        RECT all;
        HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
        all.left = 0; all.top = 0; all.right = w; all.bottom = h;
        FillRect(memdc, &all, wb);
        DeleteObject(wb);
    }
    /*
     * 分两层渲染：
     *   ① 非客户区（标题栏、边框）交给系统 —— DefWindowProc 的 WM_PRINT
     *   ② 客户区自己画，子控件（STATIC / BUTTON）逐个画，坐标用
     *      MapWindowPoints 换算到窗口原点
     *
     * PrintWindow(PW_RENDERFULLCONTENT) 试过：标题栏出来了，但两个按钮
     * 始终空白（句柄与坐标都验证正常，是它在这个窗口上的渲染局限）。
     */
    {
        POINT org;
        RECT cr;
        HWND ch;

        DefWindowProcW(hwnd, WM_PRINT, (WPARAM)memdc,
                       PRF_NONCLIENT | PRF_CHECKVISIBLE);

        /* 客户区原点（相对窗口左上角）。 */
        org.x = 0;
        org.y = 0;
        ClientToScreen(hwnd, &org);
        GetWindowRect(hwnd, &cr);
        org.x -= cr.left;
        org.y -= cr.top;

        SetViewportOrgEx(memdc, org.x, org.y, NULL);
        cn_confirm_save_paint(hwnd, cb, memdc);
        SetViewportOrgEx(memdc, 0, 0, NULL);

        /*
         * 子控件：用窗口矩形相对窗口左上角的偏移。
         * ⚠️ 之前用 ClientToScreen 叠算，按钮一直画在窗外；直接用
         * GetWindowRect 相减最稳，不受父窗客户区坐标影响。
         */
        for (ch = GetWindow(hwnd, GW_CHILD); ch; ch = GetWindow(ch, GW_HWNDNEXT)) {
            RECT wr;
            if (!IsWindowVisible(ch))
                continue;
            GetWindowRect(ch, &wr);
            SetViewportOrgEx(memdc, wr.left - cr.left, wr.top - cr.top, NULL);
            SendMessageW(ch, WM_PRINT, (WPARAM)memdc,
                         PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND |
                         PRF_CHECKVISIBLE);
            SetViewportOrgEx(memdc, 0, 0, NULL);
        }
    }

    /* 取像素：DIB 段正向（bottom-up），转成文件里的正向。 */
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;      /* 负数 = top-down，省得手动翻转 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    rowsz = ((w * 3 + 3) / 4) * 4;
    bits = snewn(rowsz * h, unsigned char);
    if (GetDIBits(memdc, bmp, 0, h, bits, &bi, DIB_RGB_COLORS)) {
        wpath = dup_mb_to_wc(CP_UTF8, path);
        fp = _wfopen(wpath, L"wb");
        sfree(wpath);
        if (fp) {
            BITMAPFILEHEADER fh;
            memset(&fh, 0, sizeof(fh));
            fh.bfType = 0x4D42;      /* "BM" */
            fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
            fh.bfSize = fh.bfOffBits + rowsz * h;
            fwrite(&fh, sizeof(fh), 1, fp);
            fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, fp);
            fwrite(bits, 1, (size_t)rowsz * h, fp);
            fclose(fp);
        }
    }
    sfree(bits);

    SelectObject(memdc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(memdc);
}

/*
 * cn: 整个弹窗的自绘主体 —— 灰底、白卡片、清单富文本。
 *
 * 标题栏与窗口边框交给系统原生绘制，这里只画客户区内容。
 *
 * WM_PAINT 与 WM_PRINTCLIENT 共用同一份：自检要拿 WM_PRINTCLIENT 把
 * 窗口渲到内存位图上取像素（未显示的窗口读不到有效表面，实测踩过），
 * 两处各写一份必然分叉。
 *
 * 层次（照设计图）：
 *   1. 客户区铺浅灰 #F7F7F5
 *   2. 白色圆角卡片：包住清单区
 *   3. 卡片内逐行画改动（勾/叉 + 灰段 + 蓝段 + 折角）
 */
static void cn_confirm_save_paint(HWND hwnd, ConfirmSaveBox *cb, HDC hdc)
{
    RECT rc, card;
    HBRUSH br;
    HFONT oldfont;
    int y;

    GetClientRect(hwnd, &rc);

    /* 1. 客户区浅灰底。 */
    br = CreateSolidBrush(CN_CLR_BG);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    /*
     * 2. 白色圆角卡片。
     *    card_top / card_bottom 由构建函数按内容算好，这里只管画。
     */
    card.left = cb->card_left;
    card.top = cb->card_top;
    card.right = cb->card_right;
    card.bottom = cb->card_bottom;
    cn_fill_round_rect(hdc, &card, cn_dpi(12),
                       CN_CLR_CARD, CN_CLR_CARD_EDGE);

    /*
     * 3. 卡片左侧的大问号图标 —— 已删除（主人 2026-11 要求）。
     *
     * 「把大问号图标删除，仅保留标题栏的小问号图标」。标题栏那个
     * 红色小问号由窗口图标（icon_question.ico + WM_SETICON）提供，
     * 属系统绘制，与本处无关。卡片里不再画 40~96px 的大问号，
     * 省下的横向空间全部还给清单文字。
     *
     * 这套绘制与构建函数被三个弹窗共用 —— 确认保存、确认退出、
     * 连接/文件传输（cn_confirm_dialog_texts），所以三处一起生效。
     */

    /*
     * 4. 卡片内的清单。
     *    有结构化数据就逐行画富文本；没有（老调用方只给了字符串）
     *    就退回用 STATIC 的文本整段画，保证不会白屏。
     */
    if (cb->changes && cb->changes->n > 0) {
        /*
         * cn: 大问号撤掉后，清单直接顶到卡片左内边距，不再给它让位。
         * rowx 与 build 里的测量起点必须同一个表达式（见那边注释）。
         */
        int rowx = cb->card_left + cn_dpi(14);
        int right_limit = cb->card_right - cn_dpi(14);
        int avail = right_limit - (rowx + cb->icon_sz + cn_dpi(8));

        /*
         * cn: 清单整块在卡片内垂直居中（行首图标跟着一起居中）。
         *
         * 画几项、画不画省略号，完全照 build 阶段算好的 cb->draw_n /
         * cb->draw_dots 执行，这里一个字都不重算。
         *
         * ⚠️ 历史上这里自作聪明重算过一遍（模拟 content_h → 居中 → 每行
         *    再判「y + row_h > card_bottom - 6」），结果和 build 的判据
         *    打架：居中已经把省略号算进高度，绘制走到省略号时 y 正好顶到
         *    内容底，那个 -6 余量让它判定「放不下」，省略号被直接跳过。
         *    主人截图里「改了三项、末尾没有 .......」正是此因。
         *    现在单一来源：build 算定，paint 照抄。
         */
        {
            int cardh = cb->card_bottom - cb->card_top;
            int content_h = 0;
            int used = 0;

            for (size_t i = 0; i < cb->draw_n; i++) {
                int l = cn_change_row_lines(hdc, &cb->changes->items[i],
                                            avail, cb->font, cb->font_value);
                used += (l < 1) ? 1 : l;
            }
            if (cb->draw_dots)
                used++;
            content_h = used * cb->row_h;

            if (content_h > 0) {
                int pad = cn_dpi(6);
                int top = cb->card_top + (cardh - content_h) / 2;
                if (top < cb->card_top + pad)
                    top = cb->card_top + pad;
                y = top;
            } else {
                y = cb->card_top + cn_dpi(10);
            }
        }
        for (size_t i = 0; i < cb->draw_n; i++) {
            int used;
            /*
             * ⚠️ cn_draw_change_row 返回的是**像素高度**（行数 × 行高），
             *    不是行数。早先这里写成 y += used，把像素当行数加，
             *    每画一项 y 就多跳 row_h 个像素，卡片迅速被判定放不下，
             *    后面的项和「.......」全被 break 掉 —— 表现就是「改了
             *    四项只显示两三行、省略号也不见了」。
             */
            used = cn_draw_change_row(hdc, &cb->changes->items[i], rowx, y,
                                      right_limit, cb->row_h, cb->icon_sz,
                                      cb->chev_sz, cb->font, cb->font_value);
            y += used;              /* 折行的项按实际占用高度推进 */
        }
        /* 被折叠时补一行省略号，提示还有更多（悬停看全部）。 */
        if (cb->draw_dots) {
            static const wchar_t dots[] = L".......";
            int by = cn_text_baseline_for_center(hdc, y + cb->row_h / 2);
            oldfont = SelectObject(hdc, cb->font);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, CN_CLR_TEXT);
            /* 与正文同一套基线对齐，别再混用 sz.cy/2（会贴得偏上）。 */
            SetTextAlign(hdc, TA_LEFT | TA_BASELINE);
            TextOutW(hdc, rowx + cb->icon_sz + cn_dpi(8), by, dots, 7);
            SetTextAlign(hdc, TA_LEFT | TA_TOP);
            SelectObject(hdc, oldfont);
        }
    }
}

static LRESULT CALLBACK cn_confirm_save_proc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam)
{
    ConfirmSaveBox *cb = (ConfirmSaveBox *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    /*
     * cn: 崩溃取证 —— 只记关心的事件，消息洪水不记（WM_PAINT 会刷爆）。
     * 重点是「销毁之后窗口过程还会不会被叫回来」：cb 是调用方的栈变量，
     * 若 WM_NCDESTROY 在栈帧回收后到达，这里拿到的就是野指针。
     */
    if (msg == WM_NCDESTROY || msg == WM_DESTROY)
        cn_trace("proc: msg=0x%04X hwnd=%p cb=%p", msg, (void *)hwnd,
                 (void *)cb);
    if (!cb && msg == WM_CREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    if (!cb)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
      /*
       * cn: 背景与内容全部自绘（照主人给的设计图）。
       *
       * WM_ERASEBKGND 只负责「别让系统拿默认刷子糊一遍」，真正绘制在
       * WM_PAINT 里统一做 —— 两处都画会出现「先画卡片、又被擦掉」
       * 的闪烁，而且配色会分叉。
       */
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc)
            cn_confirm_save_paint(hwnd, cb, hdc);
        EndPaint(hwnd, &ps);
        /*
         * cn: 截图模式 —— 首帧画完后**投递**一条消息再截图，不在这里
         * 直接抓。原因：PrintWindow 会触发重绘，在 WM_PAINT 内部调用
         * 就是重入；投递出去等消息队列空下来，窗口已是完整合成状态。
         */
        if (!cb->shot_done && getenv("CN_CONFIRM_SAVE_SHOT")) {
            cb->shot_done = true;
            PostMessageW(hwnd, WM_APP + 77, 0, 0);
        }
        return 0;
      }
      case WM_APP + 77: {
        /* cn: 截图模式的延迟抓图点（见 WM_PAINT 里的说明）。 */
        const char *shot = getenv("CN_CONFIRM_SAVE_SHOT");
        if (shot) {
            cn_confirm_save_dump_bmp(hwnd, cb, shot);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
      }
      case WM_PRINTCLIENT: {
        /*
         * cn: 自检要把窗口渲到内存位图上取像素核对配色。未显示的窗口
         * 拿不到有效表面（GetPixel 恒为白），必须走这条消息。
         */
        HDC hdc = (HDC)wParam;
        if (hdc)
            cn_confirm_save_paint(hwnd, cb, hdc);
        return 0;
      }
      case WM_PRINT: {
        /*
         * cn: 截图模式用的整窗渲染入口。子控件由调用方
         * （cn_confirm_save_dump_bmp）逐个平移渲染 —— 系统自带的
         * PRF_CHILDREN 会忽略窗口位置，全画到 DC 原点。
         */
        HDC hdc = (HDC)wParam;
        UINT flags = (UINT)lParam;
        if (flags & PRF_NONCLIENT)
            DefWindowProcW(hwnd, WM_PRINT, wParam,
                           PRF_NONCLIENT | PRF_CHECKVISIBLE);
        if (hdc && (flags & PRF_CLIENT))
            cn_confirm_save_paint(hwnd, cb, hdc);
        return 0;
      }
      case WM_CTLCOLORSTATIC:
        /*
         * 清单 STATIC 已隐藏，但首行说明仍是可见控件，必须透明，
         * 否则它会拿系统面板灰当底色，在浅灰内容区上糊出灰方块。
         */
        SetBkMode((HDC)wParam, TRANSPARENT);
        SetTextColor((HDC)wParam, CN_CLR_HEAD_TEXT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
      case WM_LBUTTONUP:
        break;
      case WM_MOUSEMOVE:
        /*
         * cn: 只剩清单折叠时的浮层。X 悬停高亮随自绘标题栏一起去掉，
         * 系统关闭按钮自带悬停效果。
         */
        cn_tip_track(&cb->tip, hwnd, lParam);
        break;
      case WM_KEYDOWN:
        /*
         * cn: Esc 关闭。自绘窗口没有系统对话框的 Esc 处理，得自己接。
         * 语义等同点 X（什么都不做，result 保持 IDCANCEL）。
         */
        if (wParam == VK_ESCAPE) {
            cb->closed_by_x = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
      case WM_COMMAND:
        if (LOWORD(wParam) == IDC_CNC_OVERWRITE) {   /* 确定/直接退出复用该 ID */
            cn_trace("proc: WM_COMMAND ok -> DestroyWindow");
            cb->result = IDOK;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_CNC_CANCEL) {
            cn_trace("proc: WM_COMMAND cancel -> DestroyWindow");
            cb->result = IDCANCEL;
            DestroyWindow(hwnd);
            return 0;
        }
        /*
         * cn: 三按钮模式下的右按钮（「保存并退出」）。用 IDYES 区分于
         * 左按钮的 IDOK，调用方才能把「保存后退出」和「直接退出」分开。
         */
        if (LOWORD(wParam) == IDC_CNC_THIRD) {
            cn_trace("proc: WM_COMMAND third -> DestroyWindow");
            cb->result = IDYES;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
      case WM_DRAWITEM: {
        /*
         * cn: 三按钮模式下的左按钮「直接退出」自绘成红框红字，明示这是
         * 会丢改动的那一项。画法与旧 IDD_SAVECHANGES 模板一致（那边也是
         * BS_OWNERDRAW + FrameRect），换到自绘弹窗后保持视觉不变。
         */
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
        COLORREF red = RGB(0xC0, 0x1E, 0x1E);
        HPEN pen;
        HPEN oldpen;
        bool selected;
        HBRUSH bg;
        RECT rc;
        wchar_t text[64];

        if (!cb->b_danger || dis->hwndItem != cb->b_danger)
            break;
        selected = (dis->itemState & ODS_SELECTED) != 0;
        bg = selected ? CreateSolidBrush(RGB(0xF5, 0xD0, 0xD0))
                      : GetSysColorBrush(COLOR_BTNFACE);
        rc = dis->rcItem;
        FillRect(dis->hDC, &rc, bg);
        pen = CreatePen(PS_SOLID, 2, red);
        oldpen = SelectObject(dis->hDC, pen);
        {
            HBRUSH redb = CreateSolidBrush(red);
            FrameRect(dis->hDC, &rc, redb);
            DeleteObject(redb);
        }
        SelectObject(dis->hDC, oldpen);
        DeleteObject(pen);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, red);
        if (cb->font)
            SelectObject(dis->hDC, cb->font);
        GetWindowTextW(dis->hwndItem, text, lenof(text));
        DrawTextW(dis->hDC, text, -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (selected)
            DeleteObject(bg);
        return TRUE;
      }
      case WM_MOUSELEAVE:
        cn_tip_hide(&cb->tip);
        break;
      case WM_CLOSE:
        /*
         * cn: 右上角 ❌。语义上是「什么都不做」—— 沿用「确认保存」
         * 那边的现行逻辑（result 保持 IDCANCEL）。点「连接/文件传输」
         * 时调用方靠 closed_by_x 把它归到「仅本次生效」，不当成落盘。
         */
        cb->closed_by_x = true;
        DestroyWindow(hwnd);
        return 0;
      /*
       * ⚠️ 这里原来有一句 PostQuitMessage(0)：它往线程消息队列塞退出指令，
       * 主消息循环收到就把整个 putty 关掉 —— 点「确定」「取消」都会让
       * 程序退出，就是它干的。本窗只是个模态子窗，销毁它不该牵连进程。
       * 调用方的循环以 IsWindow(cb.dlg) 为条件，窗口没了自然返回，
       * 不需要任何退出指令。「确认覆盖」那边同样没有这句。
       */
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/*
 * cn: 找「】」在 UTF-8 字节串里的结束位置（即下一段起点）。
 *
 * 「】」U+3011 编码为 E3 80 91，三字节。找不到返回 NULL。
 * 只认这一个字符：清单格式固定是「【标题】 内容」，别把别的全角括号
 * 也算进来，否则遇到「（备注）」这类文本会误断。
 */
static const char *cn_find_bracket_close(const char *s, size_t len)
{
    for (size_t i = 0; i + 2 < len; i++) {
        if ((unsigned char)s[i] == 0xE3 &&
            (unsigned char)s[i+1] == 0x80 &&
            (unsigned char)s[i+2] == 0x91)      /* 】 U+3011 */
            return s + i + 3;
    }
    return NULL;
}

/*
 * cn: 按「】」把过宽的行断开 —— 标题行留完整，改动内容另起一行。
 *
 * 主人指定：内容过长触发换行时，要让「【标题】」和改动内容分开，只把
 * 改动内容换到下一行。不能让 STATIC 自己按字符折行 —— 那样会把
 * 「开 → 关」拦腰切成「开 →」和「关」，看着像坏了。
 *
 * maxw 是「清单文本可用宽度」。整行放得下就原样保留；放不下才断，
 * 且断点只在「】」后。断了之后如果内容段本身还是超宽，就交给 STATIC
 * 兜底折行（总比在标题中间切好）。
 *
 * 返回新分配的字符串（调用方释放）。
 */
static char *cn_wrap_at_bracket(const char *listtxt, int maxw,
                                HDC hdc, HFONT font)
{
    strbuf *sb = strbuf_new();
    const char *p = listtxt;
    HFONT old = SelectObject(hdc, font);

    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        while (l > 0 && (p[l-1] == '\r' || p[l-1] == '\n'))
            l--;
        if (l > 0) {
            int fullw = 0;
            wchar_t *wall = dup_mb_to_wc_c(DEFAULT_CODEPAGE, p, l, NULL);
            if (wall) {
                SIZE s;
                GetTextExtentPoint32W(hdc, wall, (int)wcslen(wall), &s);
                fullw = s.cx;
                sfree(wall);
            }
            const char *close = cn_find_bracket_close(p, l);
            /*
             * 只有「整行超宽」且「能定位到】」且「】后还有内容」时才断。
             * 三条缺一不可，否则原样输出，避免把短行无端撑成两行。
             */
            if (fullw > maxw && close && close < p + l) {
                const char *rest = close;
                while (rest < p + l && *rest == ' ')
                    rest++;
                if (rest < p + l) {
                    put_fmt(sb, "%.*s\r\n%.*s\r\n",
                            (int)(close - p), p, (int)(p + l - rest), rest);
                    p = nl ? nl + 1 : NULL;
                    continue;
                }
            }
            put_fmt(sb, "%.*s\r\n", (int)l, p);
        }
        p = nl ? nl + 1 : NULL;
    }

    SelectObject(hdc, old);
    {
        char *out = strbuf_to_str(sb);
        size_t n = strlen(out);
        if (n >= 2 && out[n-1] == '\n' && out[n-2] == '\r')
            out[n-2] = 0;
        else if (n >= 1 && out[n-1] == '\n')
            out[n-1] = 0;
        return out;
    }
}

/*
 * cn: 建「确认保存」弹窗（不跑消息循环，不显示）。
 *
 * 拆成「建」和「跑循环」两段是为了自检：建完就能量尺寸、读控件矩形、
 * 检查浮层，不必真等用户点。
 *
 * cn: 同一套模板现在也服务「确认覆盖」——它通过 structured 参数把
 * 冲突项传进来，不再有独立的建窗函数。
 *
 * 返回 true 表示建成；cb 里的句柄有效。changes 是待展示的改动清单
 * （多行，\n 分隔），函数内部复制，调用方不必保活。
 */
/*
 * cn: 从「【选项名】 旧值 → 新值」纯文本反解出结构化清单。
 *
 * 为什么还要解析：cn_confirm_save_build 的调用方（含自检、确认覆盖、
 * 连接确认）都只传字符串。改签名要动七处调用点，且其中几处是自检里
 * 硬编码的样例文本，改起来噪音比收益大。
 *
 * 解析规则严格照 conf_describe_changes 的输出格式：
 *   行首「【」…「】」之间是选项名，其后空格分隔，再按第一个「 → 」
 *   （前后带空格的箭头）切成旧值与新值。
 *
 * ⚠️ 与「解析纯字符串不可靠」不矛盾：生产路径上 dialog.c 那边已经
 *    走 conf_describe_changes_struct 拿到结构化数据（不经过这里）；
 *    这里只是给「只给字符串」的老调用方兜底，格式由我们自己生成，
 *    所以能严格按格式切。真正的自由文本不会走到这条路上。
 *
 * 反解失败（没有【】或没有箭头）时返回 NULL，绘制端退回整行单色显示。
 */
static CnChangeList *cn_parse_changes_text(const char *text)
{
    CnChangeList *list = snew(CnChangeList);
    const char *p = text;

    list->items = NULL;
    list->n = 0;
    if (!text)
        return list;

    while (p && *p) {
        const char *nl = strchr(p, '\n');
        int blen = nl ? (int)(nl - p) : (int)strlen(p);
        while (blen > 0 && (p[blen-1] == '\r' || p[blen-1] == '\n'))
            blen--;
        if (blen > 0) {
            const char *close = cn_find_bracket_close(p, (size_t)blen);
            if (close && p[0] == '[' && p[1] == '[') {
                /* 不是「【」开头的行（例如省略行），整行当名字。 */
                close = NULL;
            }
            /*
             * 「【」是 U+3010，UTF-8 编码 E3 80 90 三字节。用字节比较，
             * 不用宽字符 —— 这条路径上全是 char。
             */
            if (blen >= 3 && (unsigned char)p[0] == 0xE3 &&
                (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x90)
                close = cn_find_bracket_close(p, (size_t)blen);
            else
                close = NULL;

            if (close) {
                /* 名字：去掉前 3 字节「【」与后 3 字节「】」。 */
                int nlen = (int)(close - p) - 3 - 3;
                char *name = snewn(nlen + 1, char);
                memcpy(name, p + 3, nlen);
                name[nlen] = '\0';

                /*
                 * 剩余部分按「 → 」切。箭头是 U+2192（E2 86 92），
                 * 前后各带一个半角空格，与 conf_describe_changes 一致。
                 */
                {
                    const char *rest = close;
                    const char *arrow = NULL;
                    int restlen = blen - (int)(close - p);
                    for (int i = 0; i + 5 <= restlen; i++) {
                        if (rest[i] == ' ' &&
                            (unsigned char)rest[i+1] == 0xE2 &&
                            (unsigned char)rest[i+2] == 0x86 &&
                            (unsigned char)rest[i+3] == 0x92 &&
                            rest[i+4] == ' ') {
                            arrow = rest + i;
                            break;
                        }
                    }
                    if (arrow) {
                        /* 旧值：close 之后到 arrow 之前，掐掉前导空格。 */
                        const char *o = close;
                        int olen = (int)(arrow - close);
                        while (olen > 0 && *o == ' ') { o++; olen--; }
                        while (olen > 0 && o[olen-1] == ' ') olen--;
                        /* 新值：arrow + 5 到行尾。 */
                        const char *n = arrow + 5;
                        int nlen2 = blen - (int)(n - p);
                        while (nlen2 > 0 && n[nlen2-1] == ' ') nlen2--;

                        {
                            char *oldv = snewn(olen + 1, char);
                            char *newv = snewn(nlen2 + 1, char);
                            memcpy(oldv, o, olen); oldv[olen] = '\0';
                            memcpy(newv, n, nlen2); newv[nlen2] = '\0';
                            /*
                             * 勾叉分类：新值是「关/禁用」→ 叉，其余 → 勾。
                             * 与 conf_data.c 的 cn_classify_newval 同规则，
                             * 但那边是 static，这里按同一张词表重写一份
                             * （只判 关/禁用/关闭 三个词，够用且不必
                             * 为了共用把函数提到头文件里）。
                             */
                            {
                                CnChangeKind k = CN_CHG_TOGGLE_ON;
                                if (!strcmp(newv, "关") ||
                                    !strcmp(newv, "禁用") ||
                                    !strcmp(newv, "关闭"))
                                    k = CN_CHG_TOGGLE_OFF;
                                else if (!strcmp(newv, "(无)"))
                                    k = CN_CHG_TOGGLE_OFF;
                                list->items = sresize(list->items,
                                                      list->n + 1, CnChange);
                                list->items[list->n].name = name;
                                list->items[list->n].oldval = oldv;
                                list->items[list->n].newval = newv;
                                list->items[list->n].kind = k;
                                /* cn: 解析出的行一律走勾/叉，不指定资源。 */
                                list->items[list->n].icon_id = 0;
                                list->n++;
                            }
                        }
                    } else {
                        sfree(name);
                    }
                }
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return list;
}

static bool cn_confirm_save_build(HWND parent, const char *changes,
                                  const CnChangeList *structured,
                                  const CnConfirmTexts *texts,
                                  ConfirmSaveBox *cb)
{
    static const wchar_t classname[] = L"PuTTYSftpConfirmSave";
    HINSTANCE inst = GetModuleHandle(NULL);
    /*
     * cn: 字体与「确认覆盖」弹窗同源 —— 系统消息字体 lfMessageFont 原尺寸，
     * 不缩放。此前这里按「缩小四分之一」处理，配上首行/清单两个 STATIC
     * 忘了 WM_SETFONT，同一窗里出现两套字号：清单是 24px 点阵宋体，
     * 按钮是缩小版雅黑，看起来又挤又乱（主人截图对比指出的问题）。
     */
    NONCLIENTMETRICSW ncm;
    HFONT font;
    bool font_owned;
    HDC hdc;
    SIZE sz = { 0, 0 };
    int m, lh, list_h, w, h, cw, ch, x, y, ytop;
    int qicon_size = cn_dpi(48);
    int content_h = 0;              /* 内容区高 = max(清单, 图标) */
    int needw = 0, listw_text = 0, needw_head = 0;
    /* cn: 自绘清单的行内装饰尺寸，定宽与绘制共用这两个值。 */
    /*
     * cn: 行首勾/叉直径。
     *
     * 18px 时图标糊成一团 —— 素材的圆环笔画约占直径 12%，18px 下只剩
     * 2px，叉的细节被挤没了（放大截图确认）。改 22px 后圆环约 2.6px、
     * 叉与圆环之间留得出空隙，且仍小于行高，不会把行撑开。
     */
    int icon_sz = cn_dpi(22);       /* 行首勾/叉直径 */
    int chev_sz = cn_dpi(14);       /* 行尾装饰折角尺寸 */
    /* cn: 按钮宽度与数量在定窗口宽时就算好，创建按钮时直接复用。 */
    int bw[3] = { 0, 0, 0 };
    int bw_n = 2;
    bool truncated = false;
    POINT pt;
    wchar_t *wtitle, *wlist;
    char *listtxt;
    /* 文案：未指定就沿用「确认保存」那套默认说法。 */
    const char *txt_title  = (texts && texts->title)  ? texts->title
                                                        : "确认保存";
    const char *txt_head   = (texts && texts->head)   ? texts->head
        : CN_HEAD_TEXT;
    const char *txt_ok     = (texts && texts->ok)     ? texts->ok  : "确定";
    const char *txt_cancel = (texts && texts->cancel) ? texts->cancel
                                                      : "取消";
    /*
     * cn: 第三按钮，NULL 表示不要（两按钮模式）。
     * 有它时按钮行变成「直接退出 / 取消 / 保存并退出」三个。
     */
    const char *txt_third  = (texts && texts->third)  ? texts->third : NULL;
    bool three = (txt_third != NULL);
    /* 默认按钮在哪边：只有显式要求时才挪到右边。 */
    bool default_on_right = (texts != NULL) && texts->default_on_right;
    /*
     * cn: 标题栏图标资源 ID（0 = 红色问号 IDI_QHELP）。
     * 「确认覆盖」那边传警告图标，见 CnConfirmTexts 的说明。
     */
    int title_icon = (texts && texts->icon_id) ? texts->icon_id : IDI_QHELP;
    /* cn: 三按钮模式下红框（危险动作）挂在最右那个按钮上。 */
    bool third_danger = (texts != NULL) && texts->third_is_danger;

    memset(cb, 0, sizeof(*cb));
    cb->result = IDCANCEL;

    /* cn: 取系统消息字体，按 CN_FONT_SCALE 放大（主人要求整体 +10%）。 */
    font_owned = false;
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        LOGFONTW lf = ncm.lfMessageFont;
        cn_scale_logfont(&lf);
        font = CreateFontIndirectW(&lf);
        font_owned = (font != NULL);
    } else {
        font = NULL;
    }
    if (!font)
        font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    {
        WNDCLASSW wc;
        if (!GetClassInfoW(inst, classname, &wc)) {
            memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc = cn_confirm_save_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            /* 内容区白底；底部按钮带在 WM_ERASEBKGND 里另刷灰。 */
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            wc.lpszClassName = classname;
            RegisterClassW(&wc);
        }
    }

    /* 行高实测，与确认覆盖同一口径。 */
    hdc = GetDC(NULL);
    if (hdc) {
        HFONT old = SelectObject(hdc, font);
        GetTextExtentPoint32W(hdc, L"国Ag", 3, &sz);
        SelectObject(hdc, old);
        ReleaseDC(NULL, hdc);
    }
    lh = sz.cy > 0 ? sz.cy : cn_dpi(16);

    m = cn_dpi(14);

    /*
     * cn: 清单文本。
     *
     * 两条路径：
     *   1) 调用方直接给了结构化清单（「确认覆盖」走这条）—— 每行的
     *      文字由结构化数据现拼，不经过「【】→」的字符串格式；
     *   2) 只给了字符串 —— 照旧按行取前 CN_CONFIRM_SAVE_MAX_ITEMS 行。
     *
     * ⚠️ 折叠的判定两处口径不同，别混：
     *    字符串路径在这里就把行数截到上限（历史行为，自检依赖它）；
     *    结构化路径保留全量，由绘制端的行数预算决定画几项 —— 因为
     *    「三项」不等于「三行」，长项折行后三项可能占四行。
     */
    if (structured) {
        strbuf *sb = strbuf_new();
        for (size_t i = 0; i < structured->n; i++) {
            if (i)
                put_fmt(sb, "\r\n");
            put_fmt(sb, "%s", structured->items[i].name);
        }
        listtxt = strbuf_to_str(sb);
        truncated = false;
    } else
    {
        int n = 0;
        size_t len = strlen(changes);
        char *tmp = dupstr(changes);
        char *p = tmp, *nl;
        strbuf *sb = strbuf_new();
        while (p < tmp + len && n < CN_CONFIRM_SAVE_MAX_ITEMS) {
            nl = strchr(p, '\n');
            if (nl) {
                *nl = 0;
                if (nl > p && nl[-1] == '\r')
                    nl[-1] = 0;
            }
            if (*p) {
                put_fmt(sb, "%s\r\n", p);
                n++;
            }
            p = nl ? nl + 1 : tmp + len;
        }
        /*
         * 循环停在三行上限后，若后面还有非空行，才算真截断。
         * 原来只看 n == MAX 就无条件加「.......」，恰好三行时也会多出
         * 一行省略号（主人指出「省略号没生效/多余」的成因之一）。
         */
        truncated = false;
        while (p < tmp + len) {
            nl = strchr(p, '\n');
            if (nl) {
                *nl = 0;
                if (nl > p && nl[-1] == '\r')
                    nl[-1] = 0;
            }
            if (*p) {
                truncated = true;
                break;
            }
            p = nl ? nl + 1 : tmp + len;
        }
        if (truncated)
            put_fmt(sb, ".......");
        listtxt = strbuf_to_str(sb);
        sfree(tmp);
        /* 按行拼接后最后一行也带换行，去掉。 */
        {
            size_t l2 = strlen(listtxt);
            if (l2 >= 2 && listtxt[l2-1] == '\n' && listtxt[l2-2] == '\r')
                listtxt[l2-2] = 0;
            else if (l2 >= 1 && listtxt[l2-1] == '\n')
                listtxt[l2-1] = 0;
        }
    }

    /*
     * cn: 结构化清单 —— 自绘要按段上色，必须拿到分段数据。
     *
     * 两条来源：
     *   1) 调用方直接给了结构化清单（「确认覆盖」）→ 深拷一份接管所有权，
     *      不能只存指针 —— 调用方那头的清单在弹窗关闭后就被释放了，
     *      而自绘/自检在弹窗存续期间还要反复读它。
     *   2) 只给了字符串 → 从自己生成的字符串反解（格式由
     *      conf_describe_changes 保证，见 cn_parse_changes_text 的说明）。
     */
    if (structured) {
        cb->changes = snew(CnChangeList);
        cb->changes->n = structured->n;
        cb->changes->items = structured->n
            ? snewn(structured->n, CnChange) : NULL;
        for (size_t i = 0; i < structured->n; i++) {
            const CnChange *s = &structured->items[i];
            CnChange *d = &cb->changes->items[i];
            d->name = dupstr(s->name);
            d->oldval = dupstr(s->oldval ? s->oldval : "");
            d->newval = dupstr(s->newval ? s->newval : "");
            d->kind = s->kind;
            d->icon_id = s->icon_id;
        }
    } else {
        cb->changes = cn_parse_changes_text(listtxt);
    }
    if (getenv("CN_CONFIRM_SAVE_DEBUG"))
        fprintf(stderr, "[cs] parsed %d change rows (structured=%d)\n",
                cb->changes ? (int)cb->changes->n : -1,
                structured ? (int)structured->n : -1);

    /*
     * cn: 清单区起始高度（首行之下）。
     *
     * 清单高度这里先不急着定 —— 文字要自动折行，折行后的行数取决于
     * 「窗口最终宽度减去图标与边距」，而窗口宽度是下面才定下来的。
     * 所以 list_h 挪到宽度确定之后用 DT_CALCRECT 实测（见下）。
     */
    ytop = m + lh + cn_dpi(6);

    /*
     * 内容宽度测量：取「首行说明」与「清单最长一行」的较宽者。
     *
     * ⚠️ 按行测量必须用 dup_mb_to_wc_c 限定字节数。早先用
     *    dup_mb_to_wc(cp, p) 只给了行首指针 —— 它会把「从这行开头到
     *    整个字符串结尾」全转成宽字符，等于每行都按剩余全文去量。
     *    实测第一行 49 字节被转成 83 个字符、量出 662px，窗口因此永远
     *    撑到上限，右边空一大片。这是主人反复说「太长」的真凶。
     */
    hdc = GetDC(NULL);
    if (hdc) {
        HFONT old = SelectObject(hdc, font);
        {
            const char *head = CN_HEAD_TEXT;
            wchar_t *wh = dup_mb_to_wc(DEFAULT_CODEPAGE, head);
            SIZE sh;
            GetTextExtentPoint32W(hdc, wh, (int)wcslen(wh), &sh);
            needw_head = sh.cx;
            sfree(wh);
        }
        {
            const char *p = listtxt;
            int listw = 0;
            while (p && *p) {
                char *nl = strchr(p, '\n');
                int blen = nl ? (int)(nl - p) : (int)strlen(p);
                /* 行尾可能带 \r，转换前剔掉，别把它算进字宽。 */
                while (blen > 0 && (p[blen-1] == '\r' || p[blen-1] == '\n'))
                    blen--;
                if (blen > 0) {
                    size_t wlen = 0;
                    wchar_t *wl = dup_mb_to_wc_c(DEFAULT_CODEPAGE, p, blen,
                                                 &wlen);
                    if (wl && wlen > 0) {
                        SIZE s2;
                        GetTextExtentPoint32W(hdc, wl, (int)wlen, &s2);
                        if (s2.cx > listw)
                            listw = s2.cx;
                    }
                    sfree(wl);
                }
                p = nl ? nl + 1 : NULL;
            }
            {
                SIZE s3;
                GetTextExtentPoint32W(hdc, L".......", 7, &s3);
                if (s3.cx > listw)
                    listw = s3.cx;
            }
            listw_text = listw;          /* 纯文本宽，浮层命中区要用 */
        }
        SelectObject(hdc, old);
        ReleaseDC(NULL, hdc);
    }
    /*
     * 窗口宽：主人指定默认 300px、最宽 450px（逻辑值，经 cn_dpi 折算），
     * 没到 450 之前宽度跟着内容延伸，不许提前折行。
     *
     * ⚠️ 必须把「首行」和「清单」两条需求的客户区宽度分别算准，取大者。
     *    早先写成一个 needw 再统一 + 一个 m，结果清单那侧少算了一个 m：
     *      清单控件宽 = cw - 2m - 图标 - 6
     *    要放下 246px 文字需要 cw >= 328，而公式只给到 326 —— 差 2px
     *    就触发了折行，窗口明明才 326px、离 450 上限还远得很。
     *    所以两条需求各自带全边距，别再共用一份。
     *
     *   首行顶格：  cw >= m + head_w + m
     *   清单带图标：cw >= lx + listw_text + m   (lx = m + 图标 + 6)
     *
     * 三按钮模式（「直接退出 / 取消 / 保存并退出」）还要加第三条：
     * 按钮组按总宽居中，窗口不能窄到把按钮挤出边距。按钮宽先在这里
     * 算好，后面创建按钮时直接用，避免同一套测量写两遍算出两个答案。
     */
    {
        /*
         * cn: 清单改自绘后，每行左侧多了勾/叉图标，卡片左侧还多了一个
         * 大问号，可用文字宽度相应变窄。需求宽度按新的行结构算：
         *
         *   卡片内 = 问号 + 间距 + 勾叉 + 间距 + 选项名+旧值 + 箭头 + 新值 + 边距
         *
         * 行尾折角已删除（主人 2026-11），所以不再预留 chev_sz + 12 ——
         * 那部分宽度还给文字，同样窗口宽度下能多放两三个字。
         *
         * 勾叉由 icon_sz 决定，与绘制时同一份数值（存在 cb 里，
         * 绘制函数直接取），不许两处各写一个常数。
         *
         * cn: 大问号已删除（主人 2026-11），不再为它预留 CN_QICON_MAX
         * 的横向空间。原先要留 96 + 14 两段，现在全省下来给清单文字，
         * 同样窗口宽度下每行能多放三四个字，折行也少一层。
         */
        int row_deco = icon_sz + cn_dpi(8);
        int need_head = 2 * m + needw_head;
        int need_list = 2 * m + cn_dpi(14) + row_deco + listw_text;
        int gap = cn_dpi(8);
        int bw_need = 0;

        /* 按钮宽度实测（文字宽 + 24，下限 96），与下面创建时同一口径。 */
        bw_n = three ? 3 : 2;
        {
            const char *caps[3];
            caps[0] = txt_ok;
            caps[1] = txt_cancel;
            caps[2] = txt_third;
            bw[0] = bw[1] = bw[2] = 0;
            hdc = GetDC(NULL);
            if (hdc) {
                HFONT old = SelectObject(hdc, font);
                for (int i = 0; i < bw_n; i++) {
                    wchar_t *cw_i = dup_mb_to_wc(DEFAULT_CODEPAGE, caps[i]);
                    SIZE s2;
                    GetTextExtentPoint32W(hdc, cw_i, (int)wcslen(cw_i), &s2);
                    bw[i] = s2.cx + cn_dpi(24);
                    if (bw[i] < cn_dpi(96))
                        bw[i] = cn_dpi(96);
                    sfree(cw_i);
                }
                SelectObject(hdc, old);
                ReleaseDC(NULL, hdc);
            }
            bw_need = bw[0];
            for (int i = 1; i < bw_n; i++)
                bw_need += gap + bw[i];
        }

        needw = need_head > need_list ? need_head : need_list;
        if (bw_need + 2 * m > needw)
            needw = bw_need + 2 * m;
        w = needw + cn_dpi(12);
        if (w < cn_dpi(300))
            w = cn_dpi(300);
        if (w > cn_dpi(450))
            w = cn_dpi(450);
    }
    cw = w;      /* 客户区宽定死，下面量折行高度要用 */
    /*
     * cn: 白卡片的左右边界。
     * 之前漏了这两行，card_left / card_right 一直是 0，
     * RoundRect 退化成零面积，白卡片根本没画出来（整窗只剩灰底）。
     * 卡片左右各留 m 的外边距，与底部按钮带对齐。
     */
    cb->card_left = m;
    cb->card_right = cw - m;
    if (getenv("CN_CONFIRM_SAVE_DEBUG"))
        fprintf(stderr, "[cs] head_w=%d listw_text=%d needw=%d w=%d\n",
                needw_head, listw_text, needw, w);

    /*
     * cn: 超宽行按「】」断开 —— 标题行留完整，改动内容另起一行。
     *
     * 放在这里而不是更早，是因为断行阈值必须是「窗口最终可用宽度」，
     * 而窗口宽度又是按整行宽度算出来的。顺序：
     *   1) 按整行宽定窗口宽（内容宽就撑宽，上限 450）；
     *   2) 拿定下来的可用宽去断行 —— 放得下就一个字不动；
     *   3) 断行后重新量文本宽，宽度只会变窄，不会反过来撑破窗口。
     * 只有真的撞到 450 上限（整行放不下）时，第 2 步才会动手。
     */
    {
        int lx2 = m + qicon_size + cn_dpi(6);
        int availw = cw - lx2 - m;
        if (availw < cn_dpi(40))
            availw = cn_dpi(40);
        hdc = GetDC(NULL);
        if (hdc) {
            char *wrapped = cn_wrap_at_bracket(listtxt, availw, hdc, font);
            if (wrapped && strcmp(wrapped, listtxt) != 0) {
                /*
                 * 断过了：重算 listw_text，命中区与窗口宽都要跟着更新。
                 * 断行只会让每行变短，所以宽度不变或变小。
                 */
                const char *q = wrapped;
                int nw = 0;
                HFONT oldf = SelectObject(hdc, font);
                while (q && *q) {
                    const char *nl2 = strchr(q, '\n');
                    int bl = nl2 ? (int)(nl2 - q) : (int)strlen(q);
                    while (bl > 0 && (q[bl-1] == '\r' || q[bl-1] == '\n'))
                        bl--;
                    if (bl > 0) {
                        size_t wl2 = 0;
                        wchar_t *w2 = dup_mb_to_wc_c(DEFAULT_CODEPAGE, q, bl,
                                                     &wl2);
                        if (w2 && wl2 > 0) {
                            SIZE s4;
                            GetTextExtentPoint32W(hdc, w2, (int)wl2, &s4);
                            if (s4.cx > nw)
                                nw = s4.cx;
                        }
                        sfree(w2);
                    }
                    q = nl2 ? nl2 + 1 : NULL;
                }
                SelectObject(hdc, oldf);
                {
                    SIZE s5;
                    GetTextExtentPoint32W(hdc, L".......", 7, &s5);
                    if (s5.cx > nw)
                        nw = s5.cx;
                }
                if (getenv("CN_CONFIRM_SAVE_DEBUG"))
                    fprintf(stderr, "[cs] wrapped: listw_text %d -> %d\n",
                            listw_text, nw);
                listw_text = nw;
                sfree(listtxt);
                listtxt = wrapped;
            } else {
                sfree(wrapped);
            }
            ReleaseDC(NULL, hdc);
        }
    }

    /*
     * 清单高度：按「窗口最终宽度」实测折行后的真实高度。
     *
     * ⚠️ 顺序不能反 —— 折行行数由可用宽度决定，必须先把 w 定死再量。
     * 之前是先按 '\n' 数行、再算宽，宽度一变折行就溢出 STATIC 被裁。
     * DT_CALCRECT | DT_WORDBREAK 让 GDI 用同一套断行规则算出矩形，
     * 再拿这个高度开控件，中英文混排也不会算错。
     */
    {
        int lx = m + qicon_size + cn_dpi(6);
        int availw = cw - lx - m;
        if (availw < cn_dpi(40))
            availw = cn_dpi(40);
        list_h = 0;
        hdc = GetDC(NULL);
        if (hdc) {
            HFONT old = SelectObject(hdc, font);
            wchar_t *wmeas = dup_mb_to_wc(DEFAULT_CODEPAGE, listtxt);
            RECT rc;
            rc.left = 0;
            rc.top = 0;
            rc.right = availw;
            rc.bottom = 0;
            DrawTextW(hdc, wmeas, -1, &rc,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            list_h = rc.bottom - rc.top;
            sfree(wmeas);
            SelectObject(hdc, old);
            ReleaseDC(NULL, hdc);
        }
        if (list_h <= 0)
            list_h = lh;
        list_h += cn_dpi(6);
        if (getenv("CN_CONFIRM_SAVE_DEBUG"))
            fprintf(stderr, "[cs] availw=%d list_h=%d\n", availw, list_h);
    }

    /*
     * cn: 新视觉的垂直布局（照设计图，但外壳用原生）。
     *
     *   [系统标题栏]                 由 Windows 画，不占客户区
     *   [首行说明 lh + 间距]         「以下配置项已被修改，请核对后保存或撤销。」
     *   [白色圆角卡片 card_h]        左侧大问号 + 清单富文本，每行 row_h
     *   [底部按钮带 band_h]          蓝框确定 / 灰框取消
     *
     * 标题栏高度不再由我们预留（title_h 归零），客户区从首行说明直接开始。
     * 卡片高度按行数算：row_h × 行数 + 上下内边距。
     * 清单仍走 STATIC（不可见）承载文本，自检靠它读回内容。
     */
    cb->title_h = 0;
    cb->icon_sz = icon_sz;
    cb->chev_sz = chev_sz;
    cb->row_h = lh + cn_dpi(14);
    {
        int nrows = 0;
        /*
         * cn: 显示行数 = 每项折行后占用的行数之和。
         *
         * ⚠️ 这里必须与绘制时的折行规则完全一致，否则卡片高度算少、
         *    折到第二行的内容会被卡片下沿裁掉。所以共用
         *    cn_change_row_lines() —— 不许在这里另写一套估算。
         *
         * 以前这里是「数 listtxt 里有几个 \n」，而绘制时超宽就画省略号
         * 不折行，两边碰巧对得上；一旦绘制改成真折行，这里不改就是 bug。
         */
        if (cb->changes && cb->changes->n > 0) {
            /*
             * 文字起点必须与绘制端**完全同一个坐标系**。
             *
             * cn: 大问号删除后两边都简化为 m + 14：
             *     绘制端 rowx = card_left + 14      = m + 14
             *     这里同理。
             * 早先这里漏过开头的 m（差 14px），也漏过大问号的实际占位
             * （差 48px），两次都导致量出的行数比实际少，卡片高度不够。
             *
             * 注：这里不再需要推导「可用文字宽度 avail」—— 折叠改成按
             * 项数算（与确认保存一致），不用逐项量折行了；真正折行时
             * 每项占几行由下面算卡片高度的 nrows 循环按实际行数累加。
             */
            int rowx = m + cn_dpi(14);

            /*
             * cn: 结构化清单的折叠口径，与「确认保存」的字符串路径对齐。
             *
             * 确认保存那套规矩是：**最多显示 CN_CONFIRM_SAVE_MAX_ITEMS
             * 项，若还有更多，额外补一行「.......」**。注意是「项数」，
             * 不是「总行数」—— 折叠后清单可能占 4 行（3 项 + 省略行），
             * 卡片高度本来就按实际行数自适应，撑得住。
             *
             * ⚠️ 这里曾经写成「总行数预算 3 行，其中扣一格给省略号」，
             *    于是 5 个单行项只显示 2 项 —— 省略号白白吃掉一整项的位置，
             *    主人直接问「为啥只有两项就触发了省略」。按项数算就没这问题。
             *
             * 折行的项照样按实际行数累加进卡片高度（见下面 nrows 那段），
             * 所以长选项名不会被裁。
             *
             * 结果存进 cb->draw_n / cb->draw_dots，绘制端照单执行 ——
             * 单一来源，避免两边判据打架。
             */
            {
                int budget = CN_CONFIRM_SAVE_MAX_ITEMS;
                size_t dn = 0;
                bool trunc = false;

                hdc = GetDC(NULL);

                if (cb->changes->n <= (size_t)budget) {
                    /* 装得下：全部显示，不出现省略行。 */
                    dn = cb->changes->n;
                } else {
                    /* 超出：显示前 budget 项，另补一行省略号。 */
                    dn = (size_t)budget;
                    trunc = true;
                }

                if (dn == 0)
                    dn = 1;
                if (dn > cb->changes->n)
                    dn = cb->changes->n;
                cb->draw_n = dn;
                cb->draw_dots = trunc;

                /*
                 * 卡片高度：按真正要画的那几项的行数累加。
                 *
                 * 折行与否只影响高度，不影响「画几项」—— 折叠已经按
                 * 项数定死在 draw_n，所以这里必须给足可用宽度，让长
                 * 选项名能折出正确的行数，卡片才撑得下。
                 */
                {
                    int right_limit = cw - cn_dpi(14);
                    int text_x = rowx + icon_sz + cn_dpi(8);
                    int avail = right_limit - text_x;
                    HFONT mf = NULL;
                    LOGFONTW lf;
                    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,
                                              sizeof(ncm), &ncm, 0)) {
                        lf = ncm.lfMessageFont;
                        lf.lfWeight = FW_BOLD;
                        cn_scale_logfont(&lf);
                        mf = CreateFontIndirectW(&lf);
                    }
                    hdc = GetDC(NULL);
                    for (size_t k = 0; k < cb->draw_n; k++) {
                        int l = hdc ? cn_change_row_lines(
                            hdc, &cb->changes->items[k], avail, font, mf) : 1;
                        nrows += (l < 1) ? 1 : l;
                    }
                    if (hdc)
                        ReleaseDC(NULL, hdc);
                    if (mf)
                        DeleteObject(mf);
                }
                if (cb->draw_dots)
                    nrows++;
                /*
                 * cn: 结构化路径的折叠状态由行数预算决定，字符串路径
                 * 由上面的文本截断决定。浮层（悬停看全部）与「.......」
                 * 都只看 truncated 一个标志，所以这里把预算结果合并
                 * 进去 —— 否则覆盖弹窗清单被折叠了却没有浮层可看，
                 * 用户永远看不到后面的文件。
                 */
                if (cb->draw_dots)
                    truncated = true;
            }
        } else {
            for (const char *p = listtxt; p && *p; ) {
                const char *nl = strchr(p, '\n');
                int bl = nl ? (int)(nl - p) : (int)strlen(p);
                while (bl > 0 && (p[bl-1] == '\r' || p[bl-1] == '\n'))
                    bl--;
                if (bl > 0)
                    nrows++;
                p = nl ? nl + 1 : NULL;
            }
        }
        if (nrows < 1)
            nrows = 1;
        /*
         * cn: 折叠时给「.......」预留一行。
         *
         * 省略号是绘制阶段补画的（不是 changes 里的一项），所以算卡片
         * 高度时必须单独 +1，否则卡片刚好被清单项填满，绘制时
         * 「y + row_h > card_bottom」条件不成立，省略号直接被跳过 ——
         * 表现就是「改了四项只显示三行，末尾该有的 ....... 不见了」。
         *
         * ⚠️ 结构化路径（有 cb->changes）在上面按行数预算排版时已经把
         *    省略号那行算进 nrows 了，这里不能再加一次 —— 多加一行会
         *    让卡片底下空出一条，看起来像内容没画全。
         *    只有「纯字符串、退化成按行显示」的那条路才需要在这里补。
         */
        if (truncated && cb->changes && cb->changes->n == 0)
            nrows++;
        cb->card_top = cb->title_h + m + lh + cn_dpi(10);
        cb->card_bottom = cb->card_top + nrows * cb->row_h + cn_dpi(16);
        /*
         * cn: 大问号已删除（主人 2026-11），卡片高度不再需要为它留
         * 下限。原先「至少容下问号 + 上下各 12px」是为了让大图标不贴
         * 满上下，图标没了这条下限就只剩副作用：单行改动也被撑到
         * 64px 高、下面空一片。现在高度完全由行数决定。
         */
    }
    /*
     * cn: 底部按钮带高度（主人 2026-11 要求）。
     *
     * 要求：「按钮位置改为改动内容区和窗口底部之间垂直居中，以现在
     * 按钮与窗口底部的距离为基准（大概 17px）」。
     *
     * 也就是按钮上下的留白相等，都等于原来的底部留白 m（14px，随 DPI
     * 缩放，100% 缩放下实测约 14px，用户视觉上估 17px）。
     *
     * 所以 band_h = 上留白 m + 按钮高 bh + 下留白 m。
     * ⚠️ 这里必须用按钮真实高度 CN_BTN_H，不能再用过去那个
     *    「16 + 32 + m」的估算 —— 那个 32 是按钮槽位，按钮实际只有 26，
     *    多出来的 6px 全堆在按钮下方，看起来就是「贴着底边、上边空太多」。
     *
     * ⚠️ 客户区总高直接是 card_bottom + band_h，**不要再加一个 m** ——
     *    band_h 自己已经含了卡片与按钮之间那 m 的留白。多加一次会让
     *    按钮上方的空白比下方多出整整一个 m，居中就废了。
     */
    cb->band_h = m + cn_dpi(CN_BTN_H) + m;
    h = cb->card_bottom + cb->band_h;
    ch = h;

    /*
     * cn: 原生外壳 —— 标题栏、关闭按钮、边框全交回系统绘制。
     * 自绘标题栏那套（WS_POPUP 裸窗 + 自绘 X + 圆角裁剪）主人已否掉，
     * 理由是跟系统原生边框风格不统一。
     *
     * 用 WS_CAPTION | WS_SYSMENU 后，CreateWindowExW 的 w/h 是**窗口**尺寸，
     * 客户区要按目标尺寸反推，所以必须过一遍 AdjustWindowRectEx。
     */
    {
        RECT fr;
        fr.left = 0;
        fr.top = 0;
        fr.right = cw;
        fr.bottom = ch;
        AdjustWindowRectEx(&fr, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                           FALSE, WS_EX_DLGMODALFRAME);
        w = fr.right - fr.left;
        h = fr.bottom - fr.top;
    }

    /* cn: 固定居中于屏幕（主人指定），不再跟随鼠标。 */
    cn_place_center(w, h, &x, &y);

    wtitle = dup_mb_to_wc(DEFAULT_CODEPAGE, txt_title);
    cb->dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, classname, wtitle,
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             x, y, w, h, parent, NULL, inst, cb);
    sfree(wtitle);
    if (!cb->dlg) {
        sfree(listtxt);
        if (font_owned && font)
            DeleteObject(font);
        return IDCANCEL;
    }
    SetWindowLongPtrW(cb->dlg, GWLP_USERDATA, (LONG_PTR)cb);
    cb->font = font;
    cb->font_owned = font_owned;

    /*
     * cn: 「新值」用的字体 —— 正文同尺寸加粗（设计图里新值加粗显示）。
     *
     * 标题字体（font_title）已随自绘标题栏一起撤掉：标题现在由系统
     * 标题栏绘制，字体不由我们决定。
     *
     * 从系统消息字体派生，只改字重，不换字族 —— 换字族会在不同系统上
     * 出现「中文回退到别的字体」的跳变。
     */
    cb->font_value = NULL;
    {
        LOGFONTW lf;
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm),
                                  &ncm, 0)) {
            lf = ncm.lfMessageFont;
            lf.lfWeight = FW_BOLD;
            cn_scale_logfont(&lf);       /* 与正文同比例放大 */
            cb->font_value = CreateFontIndirectW(&lf);
        }
    }

    /*
     * cn: 不做窗口区域裁剪 —— 圆角是自绘外壳的一部分，已随标题栏一起撤。
     * 原生边框由系统画，强行 SetWindowRgn 反而会把边框裁出锯齿。
     */

    /* 首行说明：客户区顶部、卡片之上，顶格。 */
    {
        const char *head = txt_head;
        wchar_t *whead = dup_mb_to_wc(DEFAULT_CODEPAGE, head);
        cb->head = CreateWindowExW(0, L"STATIC", whead,
                                  WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                  m, m, cw - 2 * m, lh,
                                  cb->dlg, NULL, inst, NULL);
        sfree(whead);
        if (cb->head)
            SendMessageW(cb->head, WM_SETFONT, (WPARAM)font, TRUE);
    }

    /*
     * 清单：STATIC 仍创建但**不可见** —— 文本由自绘按段上色画到白卡片
     * 上，STATIC 只作为文本载体（自检要 GetWindowTextW 读回来核对，
     * 也方便将来做无障碍）。给它 SS_LEFT 保证折行测量口径一致。
     */
    {
        wlist = dup_mb_to_wc(DEFAULT_CODEPAGE, listtxt);
        cb->list = CreateWindowExW(0, L"STATIC", wlist,
                                  WS_CHILD | SS_LEFT,
                                  m, cb->card_top, cw - 2 * m, list_h,
                                  cb->dlg, NULL, inst, NULL);
        sfree(wlist);
        if (cb->list)
            SendMessageW(cb->list, WM_SETFONT, (WPARAM)font, TRUE);
    }

    /*
     * 折叠时才挂悬停浮层：清单本身完整时再弹一个一样的浮窗纯属碍事。
     * 命中区取白色卡片内的清单区域。
     *
     * cn: 浮层文本不能直接取 changes —— 结构化路径（「确认覆盖」）传进来
     * 的 changes 是 NULL，只给了 CnChangeList。这里按数据来源分开取：
     * 有结构化清单就逐项拼全文，否则退回原字符串。
     *
     * ⚠️ 早先这里无条件 dup_mb_to_wc(changes)，覆盖弹窗一折叠就空指针
     *    崩溃（自检跑到「冲突汇总弹窗」段直接把进程带走）。这是引入
     *    结构化入参后必须同步、也最隐蔽的一处。
     */
    if (truncated) {
        if (structured && structured->n > 0) {
            strbuf *sb = strbuf_new();
            for (size_t i = 0; i < structured->n; i++) {
                if (i)
                    put_fmt(sb, "\r\n");
                put_fmt(sb, "%s", structured->items[i].name);
            }
            cb->tip.text = dup_mb_to_wc(DEFAULT_CODEPAGE, sb->s);
            strbuf_free(sb);
        } else {
            cb->tip.text = dup_mb_to_wc(DEFAULT_CODEPAGE,
                                       changes ? changes : "");
        }
        cb->tip.parent = cb->dlg;
        cb->tip.font = font;
        /* 命中区从自绘清单的左上角起，与绘制位置一致（问号已删）。 */
        cb->tip.hot.left = cb->card_left + cn_dpi(14);
        cb->tip.hot.top = cb->card_top;
        cb->tip.hot.right = cb->tip.hot.left + listw_text;
        cb->tip.hot.bottom = cb->card_bottom;
        cb->tip.mode = CN_TIP_RIGHT;
        cb->tip.trigger_top = cb->card_top;
        cb->tip.tip = cn_tip_create(&cb->tip);
    }

    /*
     * 按钮行：高 cn_dpi(26)，宽在定窗口宽时已实测（bw[]），这里直接摆。
     *
     * cn: 布局规则 —— 三个间距等分（主人 2026-11 要求）。
     *
     * 要求原文：「按键之间的距离和按键与窗口左右边界的距离相等」。
     * 也就是左留白 = 按钮间距 = 右留白，三处同一个值：
     *
     *     gap = (客户区宽 - 各按钮宽之和) / (按钮数 + 1)
     *
     * 早先是「整组按总宽居中、间距固定 8px」，左右留白会随窗口变宽
     * 而远大于 8px，三处距离不等。现在反过来由窗口宽反解间距。
     *
     * 间距设下限 cn_dpi(6) 防按钮贴在一起（窗口极窄时算出 0 或负数）。
     * 三按钮模式（退出键 / 标题栏 X）从左到右是
     * 「直接退出（红框）/ 取消 / 保存并退出（蓝框默认）」，
     * 与原来那个 IDD_SAVECHANGES 模板的排列和语义一致。
     */
    {
        int bh = cn_dpi(CN_BTN_H);
        int by, gap, total;
        int seg_w[4] = { 0, 0, 0, 0 };   /* 各段空白宽（含余数分配） */
        const char *caps[3];
        wchar_t *capw[3] = { NULL, NULL, NULL };

        caps[0] = txt_ok;
        caps[1] = txt_cancel;
        caps[2] = txt_third;
        for (int i = 0; i < bw_n; i++)
            capw[i] = dup_mb_to_wc(DEFAULT_CODEPAGE, caps[i]);

        /* 各按钮宽之和（不含间距）。 */
        total = bw[0];
        for (int i = 1; i < bw_n; i++)
            total += bw[i];

        /*
         * 等分间距：n 个按钮把客户区分成 n+1 段空白。
         * bw_n 恒为 2 或 3，除数是 3 或 4。
         *
         * cn: 余数要摊掉，不能全丢给右留白。
         *
         * 整数除法会剩下 0~(n) px 的零头。早先直接截断，余数全落在
         * 右边 —— 三按钮实测 cw=415、按钮总宽 288、空白 127，127/4=31
         * 余 3，于是左留白与间距都是 31、右留白 34，肉眼能看出偏。
         * 这里把余数按「从左往右逐段 +1」摊到各段空白上，最大偏差
         * 只剩 1px，三处距离视觉上完全相等。
         */
        {
            int slots = bw_n + 1;
            int blanks = cw - total;
            int base = blanks / slots;
            int rem = blanks - base * slots;   /* 0 ~ slots-1 */

            if (base < cn_dpi(6)) {
                base = cn_dpi(6);
                rem = 0;
            }
            /*
             * 逐段间距：第 i 段（0..slots-1）宽 = base + (i < rem)。
             * 按钮之间的段是 1..bw_n-1，首尾段是留白。
             */
            gap = base;                 /* 基准值，供日志用 */
            /*
             * 各段实际宽度存起来，创建按钮时按段推进，保证左右留白
             * 与中间间距都吃到余数分配，不会被右留白独吞。
             */
            for (int i = 0; i < slots; i++)
                seg_w[i] = base + (i < rem ? 1 : 0);
        }
        /*
         * cn: 按钮在底部带内垂直居中（主人 2026-11 要求）。
         *
         * 带顶 = 卡片底边 cb->card_bottom，带高 = m + bh + m，
         * 所以按钮 y = card_bottom + m 时上下留白相等。
         * 旧写法 by = ch - m - bh 在 band_h 已含上留白之后会让按钮整体
         * 下偏，卡片与按钮之间多空出一整个 m，看起来就是「贴底、上面空太多」。
         */
        by = cb->card_bottom + m;
        if (getenv("CN_CONFIRM_SAVE_DEBUG"))
            fprintf(stderr, "[cs] btn: cw=%d ch=%d bw0=%d bh=%d by=%d "
                    "gap=%d total=%d segs=%d/%d/%d/%d\n",
                    cw, ch, bw[0], bh, by, gap, total,
                    seg_w[0], seg_w[1], seg_w[2], seg_w[3]);
        /*
         * cn: 默认按钮（带蓝框、回车触发）放哪边由 default_on_right
         * 决定。「连接/文件传输」那个弹窗要求「长期生效」是默认，
         * 所以那边把左按钮降为普通按钮、右按钮升为 DEFPUSHBUTTON。
         *
         * 三按钮模式一律「最右是默认」（「保存并退出」），不参
         * default_on_right —— 那套语义只有两按钮时才讲得通。
         *
         * ⚠️ 槽位对应关系不能错：st_first 给左按钮，st_second 给
         * 右按钮（两按钮）或中按钮（三按钮），st_third 只给第三个。
         * 曾把右按钮错接成 st_second 而 st_second 没设任何 PUSH 样式位，
         * 「长期生效」的蓝框就静默丢了（自检「是默认按钮」一项失败）。
         */
        {
            DWORD st_first = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
            DWORD st_second = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
            DWORD st_third = WS_CHILD | WS_VISIBLE | WS_TABSTOP;

            if (three) {
                /*
                 * cn: 三按钮模式下红框（危险动作）挂哪个，由 third_danger
                 * 决定：
                 *   false —— 挂最左（退出确认的「直接退出」，会丢改动）；
                 *   true  —— 挂最右（确认覆盖的「全部覆盖」，会覆盖对方
                 *            已有文件；主人拍板它放在最右且是默认）。
                 * 中按钮（取消 / 跳过已存在）永远是普通按钮。
                 * 最右按钮同时是 DEFPUSHBUTTON，蓝框与红框不能并存 ——
                 * 危险时由 WM_DRAWITEM 画红框，蓝框自然让位。
                 */
                st_first |= third_danger ? BS_PUSHBUTTON : BS_OWNERDRAW;
                st_second |= BS_PUSHBUTTON;
                st_third |= third_danger ? BS_OWNERDRAW : BS_DEFPUSHBUTTON;
            } else if (default_on_right) {
                st_first |= BS_PUSHBUTTON;
                st_second |= BS_DEFPUSHBUTTON;
            } else {
                st_first |= BS_DEFPUSHBUTTON;
                st_second |= BS_PUSHBUTTON;
            }

            {
                /*
                 * 按「段」推进：每段空白用 seg_w[i]，按钮本身用 bw[i]。
                 * 这样左留白、按钮间距、右留白都吃到余数分配，不会被
                 * 右留白独吞（见上面 seg_w 的说明）。
                 */
                int cx = seg_w[0];
                cb->b_ok = CreateWindowW(
                    L"BUTTON", capw[0], st_first,
                    cx, by, bw[0], bh, cb->dlg,
                    (HMENU)IDC_CNC_OVERWRITE, inst, NULL);
                cx += bw[0] + seg_w[1];
                cb->b_cancel = CreateWindowW(
                    L"BUTTON", capw[1], st_second,
                    cx, by, bw[1], bh, cb->dlg,
                    (HMENU)IDC_CNC_CANCEL, inst, NULL);
                if (three) {
                    cx += bw[1] + seg_w[2];
                    cb->b_third = CreateWindowW(
                        L"BUTTON", capw[2], st_third,
                        cx, by, bw[2], bh, cb->dlg,
                        (HMENU)IDC_CNC_THIRD, inst, NULL);
                }
                /*
                 * cn: 记下要画红框的那个按钮。
                 * third_danger 时危险动作在最右，否则是最左。
                 */
                cb->b_danger = third_danger ? cb->b_third : cb->b_ok;
            }
        }
        for (int i = 0; i < bw_n; i++)
            sfree(capw[i]);
        if (getenv("CN_CONFIRM_SAVE_DEBUG"))
            fprintf(stderr, "[cs] handles: ok=%p cancel=%p third=%p "
                    "bw_n=%d three=%d\n",
                    (void *)cb->b_ok, (void *)cb->b_cancel,
                    (void *)cb->b_third, bw_n, (int)three);
    }

    /*
     * 统一字体 —— 首行与清单也必须设，这是之前版面难看的主因：
     * 只有按钮拿到缩小版雅黑，两个 STATIC 还是系统默认 24px 点阵宋体，
     * 同一窗里两套字号、两种字体，又大又糊还挤。
     */
    {
        HWND all[] = { cb->head, cb->list, cb->qicon,
                       cb->b_ok, cb->b_cancel, cb->b_third };
        for (size_t i = 0; i < lenof(all); i++)
            if (all[i])
                SendMessageW(all[i], WM_SETFONT, (WPARAM)font, TRUE);
    }

    {
        /*
         * cn: 标题栏图标 —— 默认红色问号，调用方可用 icon_id 换成别的
         * （「确认覆盖」用警告图标）。大图标尺寸也要给，任务栏与 Alt+Tab
         * 用的是 ICON_BIG。
         */
        HICON ico = cn_title_icon(title_icon, cn_dpi(16));
        HICON icobig = cn_title_icon(title_icon, cn_dpi(32));
        if (ico) {
            SendMessageW(cb->dlg, WM_SETICON, ICON_SMALL, (LPARAM)ico);
            SendMessageW(cb->dlg, WM_SETICON, ICON_BIG,
                         (LPARAM)(icobig ? icobig : ico));
        }
    }

    return true;
}

/* 释放建窗期间占用的字体与浮层文本（销毁窗口后调用）。 */
static void cn_confirm_save_release(ConfirmSaveBox *cb)
{
    /*
     * 浮层是独立顶层窗（WS_POPUP），弹窗销毁不会连带它 —— 必须自己收，
     * 否则会留一个无主小窗挂在屏幕上。文本也在这里释放。
     */
    if (cb->tip.tip)
        DestroyWindow(cb->tip.tip);
    sfree(cb->tip.text);
    cb->tip.text = NULL;
    /* 自建字体在这里释放：控件已随窗口销毁，不再有人引用它。 */
    if (cb->font_owned && cb->font)
        DeleteObject(cb->font);
    cb->font = NULL;
    /* 「新值」的派生字体同样是自建的。 */
    if (cb->font_value)
        DeleteObject(cb->font_value);
    cb->font_value = NULL;
    /* 结构化清单：构建时接管所有权，这里统一释放。 */
    cn_change_list_free(cb->changes);
    cb->changes = NULL;
}

int cn_confirm_save_dialog(HWND parent, const char *changes)
{
    return cn_confirm_dialog_texts(parent, changes, NULL, NULL, NULL);
}

/*
 * cn: 同一个确认弹窗，文案可换。点「连接」「文件传输」时用它：
 * 左按钮「仅本次生效」、右按钮「长期生效」，右上角关闭等同于
 * 「仅本次生效」（见 cn_confirm_save_proc 的 WM_CLOSE）。
 *
 * closed_by_x 非空时，把「是不是被 ❌ 关掉的」回传给调用方 ——
 * 因为 ❌ 与右按钮都返回 IDCANCEL，只有它能区分这两种意图。
 */
int cn_confirm_dialog_texts(HWND parent, const char *changes,
                            const CnChangeList *structured,
                            const CnConfirmTexts *texts,
                            bool *closed_by_x)
{
    ConfirmSaveBox cb;
    /* 默认按钮在哪边：与 cn_confirm_save_build 同一判据。 */
    bool default_on_right = (texts != NULL) && texts->default_on_right;

    if (closed_by_x)
        *closed_by_x = false;

    if (!cn_confirm_save_build(parent, changes, structured, texts, &cb))
        return IDCANCEL;

    if (parent)
        EnableWindow(parent, FALSE);
    ShowWindow(cb.dlg, SW_SHOW);
    SetForegroundWindow(cb.dlg);

    {
        MSG msg;
        while (IsWindow(cb.dlg) && GetMessageW(&msg, NULL, 0, 0)) {
            if ((msg.hwnd == cb.dlg || IsChild(cb.dlg, msg.hwnd)) &&
                msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_RETURN) {
                    /*
                     * cn: 回车要落到「默认按钮」上，而默认按钮在哪边
                     * 由文案决定 —— 死按左边会让「长期生效」是默认的
                     * 那个弹窗回车却执行了「仅本次生效」。
                     *
                     * 三按钮模式最右是「保存并退出」，回车落它。
                     */
                    if (cb.b_third) {
                        SendMessageW(cb.dlg, WM_COMMAND,
                                     MAKEWPARAM(IDC_CNC_THIRD, BN_CLICKED),
                                     (LPARAM)cb.b_third);
                    } else if (default_on_right) {
                        SendMessageW(cb.dlg, WM_COMMAND,
                                     MAKEWPARAM(IDC_CNC_CANCEL, BN_CLICKED),
                                     (LPARAM)cb.b_cancel);
                    } else {
                        SendMessageW(cb.dlg, WM_COMMAND,
                                     MAKEWPARAM(IDC_CNC_OVERWRITE, BN_CLICKED),
                                     (LPARAM)cb.b_ok);
                    }
                    continue;
                }
                if (msg.wParam == VK_ESCAPE) {
                    SendMessageW(cb.dlg, WM_COMMAND,
                                 MAKEWPARAM(IDC_CNC_CANCEL, BN_CLICKED),
                                 (LPARAM)cb.b_cancel);
                    continue;
                }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parent) {
        EnableWindow(parent, TRUE);
        ShowWindow(parent, SW_RESTORE);
        SetForegroundWindow(parent);
    }
    cn_trace("dialog_texts: 循环结束 result=%d，准备 release cb=%p",
             cb.result, (void *)&cb);
    cn_confirm_save_release(&cb);
    cn_trace("dialog_texts: release 完成");
    if (closed_by_x)
        *closed_by_x = cb.closed_by_x;
    return cb.result;
}

/*
 * cn: 「连接 / 文件传输」前那个改动确认弹窗的文案，唯一定义处。
 * 声明在 putty.h，dlg_confirm_launch()（真实流程）与本文件
 * sftp_show_confirm_launch()（-confirm-launch-show 看图入口）共用，
 * 改文案只改这里，不会出现「截图看到的不是线上效果」。
 */
const CnConfirmTexts cn_launch_texts = {
    "确保保存方式后继续执行",
    CN_HEAD_TEXT,
    "仅本次生效",
    "长期生效",
    true,                      /* cn: 默认按钮（蓝框）在右，即「长期生效」 */
};

/*
 * cn: 退出确认（三按钮）的文案，唯一定义处。声明在 putty.h，
 * dlg_confirm_quit()（dialog.c，真实流程）与本文件
 * sftp_show_confirm_quit()（-confirm-quit-show 看图入口）共用。
 *
 * 三按钮语义：直接退出（左，红框）/ 取消（中）/ 保存并退出（右，蓝框默认）。
 * third 非 NULL 即开启三按钮模式。
 */
const CnConfirmTexts cn_quit_texts = {
    "确认保存",
    CN_HEAD_TEXT,
    "直接退出",
    "取消",
    false,                     /* 三按钮模式下此项不生效，最右恒为默认 */
    "保存并退出",
};

/*
 * cn: 把「确认保存」弹窗真显示出来，供人工看图/截图（-confirm-save-show）。
 *
 * 自检模式建完就销毁、截不到图，所以单留这个入口。默认用一份五行改动
 * 触发折叠，浮层才会挂上，正好一次看全：宽度、折行、按钮、浮层位置与配色。
 *
 * 想看「超宽行在】后断行」的效果，可用环境变量 CN_CONFIRM_SAVE_LONG=1
 * 换成一条超长改动（标题本身很长 + 改动内容），断行后标题独占一行。
 */
void sftp_show_confirm_save(void)
{
    static const char *demo =
        "【启用终端指定 ANSI 颜色】 开 → 关\n"
        "【启用终端使用 xterm 256 色模式】 开 → 关\n"
        "【启用终端使用 24 位色】 开 → 关\n"
        "【窗口标题】 t9 → 1\n"
        "【主机名】 a → b";
    /*
     * 超长用例：整行远超 450px，用来目视确认断点落在「】」之后，
     * 而不是把「开 → 关」拦腰切开。
     */
    static const char *demo_long =
        "【启用终端使用 xterm 256 色模式并且把配色方案改成 Solarized Dark】 "
        "开 → 关\n"
        "【窗口标题】 t9 → 1\n"
        "【主机名】 a → b\n"
        "【端口】 22 → 2222\n"
        "【用户名】 root → admin";
    /*
     * 与主人截图同款：长选项名 + 四项改动，前两项各折成两行。
     * 用来验证两件事：
     *   1. 「关」与「→ 开」必须连成整体换行，不许把「关」吊在行末；
     *   2. 折行把预算吃满后，第四项折叠，末尾必须出现「.......」。
     */
    static const char *demo_owner =
        "【禁用远程控制清除回滚】 关 → 开\n"
        "【禁用服务器发送 ^? 时破坏性回退删除】 关 → 开\n"
        "【禁用远程控制字符集设置】 关 → 开\n"
        "【禁用远程控制窗口缩放】 关 → 开";
    /*
     * 主人截图的原样：正好三项，第二项折行。用来验证「三项必须完整
     * 显示」—— 折行把预算吃掉后，不许把第三项挤成省略号。
     */
    static const char *demo_owner3 =
        "【禁用远程控制清除回滚】 关 → 开\n"
        "【禁用服务器发送 ^? 时破坏性回退删除】 关 → 开\n"
        "【禁用远程控制字符集设置】 关 → 开";

    if (getenv("CN_CONFIRM_SAVE_OWNER3"))
        cn_confirm_save_dialog(NULL, demo_owner3);
    else if (getenv("CN_CONFIRM_SAVE_OWNER"))
        cn_confirm_save_dialog(NULL, demo_owner);
    else
        cn_confirm_save_dialog(NULL, getenv("CN_CONFIRM_SAVE_LONG") ?
                              demo_long : demo);
}

/*
 * cn: 把「连接/文件传输」前那个确认弹窗真显示出来，供人工看图
 * （-confirm-launch-show）。文案与 dlg_confirm_launch 里的那份保持一致，
 * 改那边记得同步这里，否则截图看的就不是线上效果。
 */
void sftp_show_confirm_launch(void)
{
    static const char *demo =
        "【启用终端指定 ANSI 颜色】 开 → 关\n"
        "【窗口标题】 t9 → 1";
    /* cn: 文案取自 cn_launch_texts（唯一定义处），与真实流程同源。 */
    cn_confirm_dialog_texts(NULL, demo, NULL, &cn_launch_texts, NULL);
}

/*
 * cn: 把退出确认（三按钮：直接退出 / 取消 / 保存并退出）真显示出来，
 * 供人工看图（-confirm-quit-show）。文案取自 cn_quit_texts（唯一定义处），
 * 与 dlg_confirm_quit 的真实流程同源。
 */
void sftp_show_confirm_quit(void)
{
    static const char *demo =
        "【初始启用自动换行】 开 → 关";
    cn_confirm_dialog_texts(NULL, demo, NULL, &cn_quit_texts, NULL);
}

/*
 * cn: 把「确认覆盖」弹窗真显示出来，供人工看图（-confirm-overwrite-show）。
 *
 * 走的是 cn_overwrite_ask —— 与真实文件传输流程同一个函数、同一份
 * 数据映射和文案，所以截图看到的就是线上效果。给到 5 项：超过
 * CN_CONFIRM_SAVE_MAX_ITEMS（3）项，能顺带看到「显示前 3 项 + 一行
 * .......省略」以及悬停浮层的效果。
 *
 * 环境变量 CN_OVERWRITE_LONG=1 换成超长名字：用来验折行 —— 长项必须
 * 折成多行、卡片高度跟着长，不能被裁。短名字看不出这个。
 */
void sftp_show_confirm_overwrite(void)
{
    ConflictItem demo[5];
    static const wchar_t *names[5] = {
        L"build",
        L"config.h",
        L"docs",
        L"README.md",
        L"src",
    };
    /*
     * 超长名字组：长度都逼近或超过卡片文字区宽度，逼排版器折行。
     * 混了一个含中文的，验证中英混排的断行位置。
     */
    static const wchar_t *longnames[5] = {
        L"a-very-long-directory-name-that-will-not-fit-in-one-row-at-all",
        L"configuration-file-with-an-extremely-long-basename-2026.conf",
        L"另一个特别长的中文目录名用来验证中英文混排时的折行位置是否正确",
        L"README.md",
        L"src",
    };
    static const bool dirs[5] = { true, false, true, false, true };
    bool longmode = false;
    {
        const char *e = getenv("CN_OVERWRITE_LONG");
        if (e && *e && strcmp(e, "0"))
            longmode = true;
    }

    for (int i = 0; i < 5; i++) {
        demo[i].name = (wchar_t *)(longmode ? longnames[i] : names[i]);
        demo[i].is_dir = dirs[i];
    }

    cn_overwrite_ask(NULL, demo, 5);
}

/* 释放一批待传名单（names 逐项 + 三个数组）。空指针一律按空数组算。 */
static void cn_free_entries(char **names, char **srcs, bool *dirs, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (names)
            sfree(names[i]);
        if (srcs)
            sfree(srcs[i]);
    }
    sfree(names);
    sfree(srcs);
    sfree(dirs);
}

/* ----------------------------------------------------------------------
 * 传输一批条目（列表选中与拖放共用）。
 *
 * names 是纯文件名数组；dirs 标记哪些是目录；to_remote 定方向。
 * srcs 为 NULL 时源路径由源面板当前目录拼出（列表选中的情形）；拖放
 * 传入的是资源管理器里的完整路径，与当前目录无关，逐项给全。
 *
 * 覆盖询问在开跑前对整批做一次（见 cn_confirm_batch）：把所有目标端已
 * 存在的项汇总进一个弹窗，用户定一次策略，整批照办。不再逐项弹窗 ——
 * 拖十个同名文件进去弹十次，纯属折磨。递归内部更不问：一棵稍大的目录
 * 树动辄几十上百个文件，逐级弹窗没人受得了。
 */
static void cn_transfer_entries(SftpWin *sw, char **names, char **srcs,
                                bool *dirs, size_t count, bool to_remote)
{
    int ok_count = 0, skip_count = 0, fail_count = 0;
    char *last_err = NULL;
    bool aborted = false, paused = false;
    int batch_policy;
    /*
     * 这一批到底问没问过冲突、问了之后选的是不是「全部覆盖」。
     *
     * ⚠️ 不能拿 batch_policy == CN_OVW_OVERWRITE 当判据：那是默认值，
     *    目标端没冲突、压根没弹窗时它也等于它。早先就吃过这个亏 ——
     *    一次干净的上传被提示成「全部覆盖：已重新上传 1 项」，纯属冤枉。
     *    这里单独记一笔「确实弹过窗且答案是覆盖」，两件事分开说。
     */
    bool overwrite_chosen = false;
    const char *src_dir = to_remote ? sw->localdir : sw->remotedir;
    const char *dst_dir = to_remote ? sw->remotedir : sw->localdir;
    /*
     * 这一批是不是「续传」。
     *
     * 只有续传才允许按目标端已有大小跳过或接续。普通传输（用户点上传
     * 下载、或拖放）一律从头传：目标端大小恰好相同不代表内容相同，
     * 拿大小当判据去跳过，就是骗用户说传完了。
     */
    bool resume_mode = sw->resume_pending;

    /* 传输期间抽消息会派发 WM_COMMAND，可能又跑回这里，挡一下。 */
    if (sw->busy) {
        /* 名单是调用方交进来的，这里没接手就得替它释放。 */
        cn_free_entries(names, srcs, dirs, count);
        return;
    }

    cn_set_busy(sw, true);
    cn_set_xfer_state(sw, CN_XFER_RUNNING);
    sw->cancel_requested = false;
    /* 上一轮的取消/暂停标志必须清掉，否则这一轮刚开始就被打断。 */
    if (sw->sx) {
        sftp_xfer_reset_cancel(sw->sx);
        sftp_xfer_reset_pause(sw->sx);
    }
    SendMessageW(sw->progress, PBM_SETPOS, 0, 0);

    /*
     * 开跑之前先把整批的冲突问清。
     *
     * 扫的是目标端已存在的项（文件和目录都算），汇总进一个弹窗，问一次。
     * 续传批次不问 —— 那些就是上一轮传过的，答案必然是「全部覆盖」，
     * 再弹一次纯属烦人。
     *
     * 定下的策略整批照办：
     *   「全部覆盖」  -> 同名项一律从头传，不做断点跳过
     *   「跳过已存在」-> 已存在的文件全跳过（目录仍进去，但只补缺）
     *   「取消」      -> 整批不动
     */
    batch_policy = CN_OVW_OVERWRITE;
    if (!resume_mode) {
        size_t nconf = 0;
        ConflictItem *conf = cn_collect_conflicts(sw, names, dirs, count,
                                                  to_remote, &nconf);
        cn_trace("xfer: 冲突收集完成 nconf=%lu conf=%p",
                 (unsigned long)nconf, (void *)conf);
        if (conf) {
            batch_policy = cn_confirm_batch(sw, conf, nconf);
            /* 真弹过窗了，答案是不是「全部覆盖」到这里才算数。 */
            overwrite_chosen = (batch_policy == CN_OVW_OVERWRITE);
            cn_trace("xfer: 弹窗返回 policy=%d，准备释放冲突列表", batch_policy);
            cn_conflicts_free(conf, nconf);
            cn_trace("xfer: 冲突列表已释放");
        }
        if (batch_policy == CN_OVW_ABORT) {
            /* 整批不动：把名单释放掉，直接收摊。 */
            cn_free_entries(names, srcs, dirs, count);
            cn_set_busy(sw, false);
            cn_set_status(sw, "已取消：目标位置已有同名项，未做任何改动。");
            return;
        }
    }

    for (size_t i = 0; i < count; i++) {
        char *src, *dst;
        char *errmsg = NULL;
        ProgressCtx pc;
        XferOutcome res;
        bool do_transfer = true;
        bool allow_resume = resume_mode;

        if (sw->cancel_requested) {
            aborted = true;
            break;
        }

        src = srcs ? srcs[i] : cn_path_join(src_dir, names[i], !to_remote);
        dst = cn_path_join(dst_dir, names[i], to_remote);

        if (dirs[i]) {
            /*
             * 目录：整棵子树搬过去。先在目标侧建出同名目录，再逐项
             * 复制，空目录因此也能保留下来。
             *
             * 本地侧的符号链接 / junction 跳过 —— 顺着它们递归会绕回
             * 上层，永远走不完。
             *
             * 冲突策略已由 cn_confirm_batch 统一问过：「跳过已存在」时
             * 让递归只补缺（目录本身照样进去，缺的文件才补）。
             */
            do_transfer = false;
            if (to_remote && cn_local_is_reparse(src)) {
                skip_count++;
            } else {
                XferStat sub = {0, 0, 0, NULL, false};
                char *msg;

                if (batch_policy == CN_OVW_SKIP)
                    sub.only_missing = true;

                msg = dupprintf("正在复制目录 %s …", names[i]);
                cn_set_status(sw, msg);
                sfree(msg);

                cn_xfer_recurse(sw, src, dst, to_remote, 0, &sub);
                ok_count += sub.ok;
                fail_count += sub.fail;
                skip_count += sub.skip;
                if (sub.last_err) {
                    sfree(last_err);
                    last_err = sub.last_err;
                }
            }
        } else {
            /*
             * 文件：已存在的处理方式跟着整批策略走。
             *
             *   「跳过已存在」 -> 跳过，不传
             *   「全部覆盖」   -> 从头传，不做断点跳过
             *
             * 续传批次不问、也不跳：那些文件就是上一轮传过的，要接着
             * 传完。allow_resume 的初值已经是 resume_mode，这里只在
             * 「跳过已存在」时多迈一步判断。
             *
             * allow_resume 的取值很关键：只有「续传」这条路才允许按
             * 目标端已有大小跳过。用户亲手点了「全部覆盖」，就必须真的
             * 从头传 —— 目标端哪怕大小恰好和源一样，内容也可能完全不
             * 同，此时跳过等于骗用户说传好了。
             */
            if (batch_policy == CN_OVW_SKIP &&
                cn_dst_exists(sw, dst, to_remote)) {
                skip_count++;
                do_transfer = false;
            }
        }

        if (do_transfer) {
            char *msg = dupprintf("正在传输 %s …", names[i]);
            cn_set_status(sw, msg);
            sfree(msg);

            pc.sw = sw;

            if (to_remote)
                res = sftp_xfer_upload(sw->sx, src, dst, allow_resume,
                                       cn_progress, &pc, &errmsg);
            else
                res = sftp_xfer_download(sw->sx, src, dst, allow_resume,
                                         cn_progress, &pc, &errmsg);

            if (res == XFER_OK) {
                ok_count++;
            } else if (res == XFER_PAUSED) {
                /*
                 * 暂停：当前文件留下断点，不再算成功也不算失败 ——
                 * 它还会被续传。已传完的那些已经计进 ok_count 了。
                 */
                paused = true;
                sfree(errmsg);
                errmsg = NULL;
            } else if (res == XFER_CANCELLED) {
                /* 终止：半截文件已由协议层删掉，整批就此打住。 */
                aborted = true;
                sfree(errmsg);
                errmsg = NULL;
            } else {
                fail_count++;
                sfree(last_err);
                last_err = errmsg ? dupstr(errmsg) : dupstr("未知错误");
                sfree(errmsg);
            }
        }

        if (!srcs)
            sfree(src);
        sfree(dst);

        if (aborted || paused)
            break;
    }

    cn_set_busy(sw, false);

    /*
     * 暂停时要把状态切到 PAUSED，让「暂停」按钮变成「续传」可点。
     * cn_set_busy(false) 刚把状态复位成 IDLE，这里按实际结果覆盖。
     */
    if (paused)
        cn_set_xfer_state(sw, CN_XFER_PAUSED);

    /*
     * 进度条收尾。
     *
     * 暂停时绝不能复位 —— 那正是「保留当前进度、续传后在此基础上继续」
     * 的要求，进度条停在哪儿，用户就知道断点在哪儿。
     * 完成和终止则归零：cn_progress 是按「当前文件」的总字节数设范围的，
     * 不复位就会留下「完成：成功 3」配一根满格绿条，看着像还在跑。
     */
    if (!paused)
        SendMessageW(sw->progress, PBM_SETPOS, 0, 0);

    /*
     * 暂停时把清单留一份给「续传」用。原来那份马上要按惯例释放，
     * 所以这里深拷贝一份。取消/完成时则要把上次遗留的清掉。
     */
    cn_resume_save(sw, paused ? names : NULL,
                   paused ? srcs : NULL,
                   paused ? dirs : NULL,
                   paused ? count : 0,
                   to_remote);

    cn_free_entries(names, srcs, dirs, count);

    cn_refresh_local(sw);
    cn_refresh_remote(sw);

    {
        const char *verb = paused ? "已暂停" : aborted ? "已终止" : "完成";
        char *msg = dupprintf("%s：成功 %d，跳过 %d，失败 %d%s%s",
                              verb, ok_count, skip_count, fail_count,
                              last_err ? "；" : "",
                              last_err ? last_err : "");
        cn_set_status(sw, msg);
        sfree(msg);
    }

    /*
     * 提示条：只在「这一批安稳收尾」时浮出来。
     *
     * 暂停/终止/有失败项时一概不弹 —— 那些情况需要用户真看一眼、甚至
     * 去处理，用一条一闪而过的提示去说「失败了 2 项」等于把坏消息藏
     * 起来。它们照旧留在状态栏，配合失败时的模态框。
     */
    if (!paused && !aborted && fail_count == 0) {
        char *toast = cn_toast_xfer_text(to_remote, ok_count, skip_count,
                                        overwrite_chosen);
        if (toast) {
            cn_toast_show(sw, toast);
            sfree(toast);
        }
    }

    if (fail_count)
        cn_msg(sw->hwnd, last_err ? last_err : "部分文件传输失败。",
               "传输失败", MB_ICONWARNING | MB_OK);
    sfree(last_err);
}

/*
 * 列表选中项的传输入口。选中内容整理出来交给通用函数，剩下的
 * （busy 挡重入、覆盖询问、递归、汇总）都在那边，两条路径共用。
 */
static void cn_do_transfer(SftpWin *sw, bool to_remote)
{
    size_t count = 0;
    bool *dirs = NULL;
    char **names;

    /* 源列表是远端时，名字要以 UTF-8 还原（见 cn_selected_all 注释）。 */
    names = cn_selected_all(to_remote ? sw->local_list : sw->remote_list,
                            &count, &dirs, !to_remote);
    if (count == 0) {
        cn_set_status(sw, "先在列表里选一个文件。");
        return;
    }

    cn_transfer_entries(sw, names, NULL, dirs, count, to_remote);
}

/* ----------------------------------------------------------------------
 * 递归传输的自检入口。
 *
 * 让「目录整棵复制」这条逻辑能被脚本验证，而不必靠人去界面上点选 ——
 * 跨进程往 ListView 里注入双击不可靠（要靠光标真实移动，且行高依赖
 * 字体），验一次要折腾很久，还不稳。
 *
 * 这里只借用 cn_xfer_recurse 本身，其余全是假的：没有窗口、状态栏
 * 丢掉、进度回调传 NULL。所以它测的就是界面会走的那条代码路径。
 */
bool sftp_selftest_recurse(SftpXfer *sx, const char *src, const char *dst,
                           bool to_remote, char **errmsg)
{
    SftpWin fake;
    XferStat st = {0, 0, 0, NULL, false};
    bool ok;

    memset(&fake, 0, sizeof(fake));
    fake.sx = sx;
    fake.cancel_requested = false;

    ok = cn_xfer_recurse(&fake, src, dst, to_remote, 0, &st);

    if (errmsg) {
        char *detail = dupprintf("成功 %d，跳过 %d，失败 %d",
                                 st.ok, st.skip, st.fail);
        char *joined = st.last_err ? dupprintf("%s（%s）", st.last_err, detail)
                                   : dupstr(detail);
        sfree(detail);
        *errmsg = joined;
    }
    sfree(st.last_err);
    return ok && st.fail == 0;
}

static bool cn_selftest_files_equal(const char *a, const char *b);

/*
 * 续传自检用的进度记录。
 *
 * 记首个回调的 done —— 它就是「续传从哪儿起算」的证据。注意它不会
 * 正好等于断点：上传流水线会先灌满若干批（每批 32KB）才开始回吐进度，
 * 所以首个回调是「断点 + 已灌入的整批」，只要 ≥ 断点就说明没有从 0
 * 重来。等于 0 才是真出问题了。
 */
typedef struct ResumeProbe {
    uint64_t first_done;
    uint64_t last_done;
    uint64_t total;
    int calls;
} ResumeProbe;

static bool cn_selftest_first_done(void *ctx, uint64_t done, uint64_t total)
{
    ResumeProbe *p = (ResumeProbe *)ctx;
    if (p) {
        if (p->calls == 0)
            p->first_done = done;
        p->last_done = done;
        p->total = total;
        p->calls++;
    }
    return true;
}

/*
 * 断点续传自检。
 *
 * 验的是这条链：目标端已有前 N 字节 → 从 N 处接着传 → 最终内容完整。
 *
 *   1. 先完整上传一次，确认远端有完整版本；
 *   2. 用另一个更短的文件上传到同名远端路径（TRUNC 生效，远端被截短），
 *      相当于造出「传到一半」的现场；
 *   3. 再用原文件上传 —— 走的就是续传那条路。此时远端大小 = 短文件长度，
 *      续传应从该长度处开始，所以首个进度回调的 done 必须等于它。
 *
 * 第 3 步如果从头重传，内容最终也对，光比对内容验不出「真的续传了」。
 * 所以核心断言是 first_done == 短文件长度：那是「没重发已完成字节」
 * 的直接证据。
 */
bool sftp_selftest_resume(SftpXfer *sx, const char *localpath,
                          const char *remotepath, char **errmsg)
{
    char *emsg = NULL;
    XferOutcome r;
    ResumeProbe probe;
    uint64_t short_size;
    char *shortpath = NULL;

    if (errmsg)
        *errmsg = NULL;

    /* [1] 先完整传一次。 */
    r = sftp_xfer_upload(sx, localpath, remotepath, false, NULL, NULL, &emsg);
    if (r != XFER_OK) {
        if (errmsg)
            *errmsg = dupprintf("首次上传失败：%s", emsg ? emsg : "(未知)");
        sfree(emsg);
        return false;
    }
    sfree(emsg);
    emsg = NULL;

    /* [2] 造一个只有源文件一半大小的临时文件，传到同名远端路径上。 */
    {
        uint64_t full = 0;
        short_size = 0;
        shortpath = dupprintf("%s.resume-selftest", localpath);

        /* 源文件大小：走本文件的 cn_local_size。 */
        full = cn_local_size(localpath);
        if (full < 2) {
            if (errmsg)
                *errmsg = dupstr("源文件太小，无法做续传自检（至少 2 字节）。");
            sfree(shortpath);
            return false;
        }
        short_size = full / 2;

        /* 从源文件读前 short_size 字节写成临时文件。 */
        {
            FILE *in = fopen(localpath, "rb");
            FILE *out = fopen(shortpath, "wb");
            if (!in || !out) {
                if (in) fclose(in);
                if (out) fclose(out);
                if (errmsg)
                    *errmsg = dupstr("无法创建续传自检临时文件。");
                sfree(shortpath);
                return false;
            }
            {
                uint64_t left = short_size;
                char buf[8192];
                while (left > 0) {
                    size_t want = left > sizeof(buf) ? sizeof(buf)
                                                     : (size_t)left;
                    size_t got = fread(buf, 1, want, in);
                    if (got == 0)
                        break;
                    fwrite(buf, 1, got, out);
                    left -= got;
                }
            }
            fclose(in);
            fclose(out);
        }
    }

    r = sftp_xfer_upload(sx, shortpath, remotepath, false, NULL, NULL, &emsg);
    if (r != XFER_OK) {
        if (errmsg)
            *errmsg = dupprintf("造半截文件失败：%s", emsg ? emsg : "(未知)");
        sfree(emsg);
        sfree(shortpath);
        return false;
    }
    sfree(emsg);
    emsg = NULL;

    /* [3] 用完整文件续传，看进度是不是从断点起算的。 */
    memset(&probe, 0, sizeof(probe));
    r = sftp_xfer_upload(sx, localpath, remotepath, true,
                         cn_selftest_first_done, &probe, &emsg);
    if (r != XFER_OK) {
        if (errmsg)
            *errmsg = dupprintf("续传失败：%s", emsg ? emsg : "(未知)");
        sfree(emsg);
        sfree(shortpath);
        return false;
    }
    sfree(emsg);

    /*
     * 关键断言：进度必须从断点起算。
     *
     * 首个回调不会精确等于断点（流水线先灌满几批 32KB 才回吐进度），
     * 所以判据是「≥ 断点且没超过总量」。若从头重传，首个回调会是
     * 32KB 左右，必然小于 50000 这种断点值，一眼能抓出来。
     */
    if (probe.calls == 0 || probe.first_done < short_size ||
        probe.last_done != probe.total) {
        if (errmsg)
            *errmsg = dupprintf(
                "续传起点不对：断点 %llu，首个进度 %llu，末次 %llu/%llu"
                "（首个进度小于断点即说明从头重传了）",
                (unsigned long long)short_size,
                (unsigned long long)probe.first_done,
                (unsigned long long)probe.last_done,
                (unsigned long long)probe.total);
        sfree(shortpath);
        return false;
    }

    remove(shortpath);
    sfree(shortpath);

    /*
     * [4] 反向断言：同样大小、不同内容，且 allow_resume = false 时，
     * 必须真的覆盖过去。
     *
     * 这是「大小相同就跳过」这个判据最危险的漏洞：目标端内容完全
     * 不同但字节数一样，一旦被当成「已传完」跳过，用户看到「成功」
     * 却拿到旧文件。所以单独验一遍。
     */
    {
        char *altpath = dupprintf("%s.alt-selftest", localpath);
        uint64_t full = 0;
        bool wrote = false;

        full = cn_local_size(localpath);

        /* 造一个同样大小、内容全不同的文件。 */
        if (full < 2) {
            /*
             * 取不到源文件大小就会造出空文件，两个空文件比对必然相等，
             * 等于这条断言自动通过。必须先拦掉。
             */
            if (errmsg)
                *errmsg = dupstr("同大小覆盖自检无法进行：取不到源文件大小");
            sfree(altpath);
            return false;
        }
        {
            FILE *out = fopen(altpath, "wb");
            if (out) {
                uint64_t left = full;
                char buf[8192];
                memset(buf, 0x5A, sizeof(buf));   /* 全是 'Z'，和源必然不同 */
                while (left > 0) {
                    size_t want = left > sizeof(buf) ? sizeof(buf)
                                                     : (size_t)left;
                    fwrite(buf, 1, want, out);
                    left -= want;
                }
                fclose(out);
                wrote = true;
            }
        }

        if (!wrote) {
            /*
             * 临时文件都造不出来就等于这一步没验过，不能当通过放行——
             * 假通过的断言比没有断言更害人。
             */
            if (errmsg)
                *errmsg = dupstr("同大小覆盖自检无法进行：造不出临时文件");
            sfree(altpath);
            return false;
        }

        {
            /* 先用 alt 覆盖远端（allow_resume=false，必须真覆盖）。 */
            r = sftp_xfer_upload(sx, altpath, remotepath, false,
                                 NULL, NULL, &emsg);
            if (r != XFER_OK) {
                if (errmsg)
                    *errmsg = dupprintf("同大小覆盖上传失败：%s",
                                        emsg ? emsg : "(未知)");
                sfree(emsg);
                remove(altpath);
                sfree(altpath);
                return false;
            }
            sfree(emsg);
            emsg = NULL;

            /*
             * 再拉回远端内容，和 alt 文件逐字节比。若上面那次被误判成
             * 「已传完」跳过，远端还是原文件内容，这里立刻不等。
             */
            {
                char *backpath = dupprintf("%s.back-selftest", localpath);
                r = sftp_xfer_download(sx, remotepath, backpath, false,
                                       NULL, NULL, &emsg);
                if (r != XFER_OK) {
                    if (errmsg)
                        *errmsg = dupprintf("回读远端文件失败：%s",
                                            emsg ? emsg : "(未知)");
                    sfree(emsg);
                    remove(altpath);
                    remove(backpath);
                    sfree(altpath);
                    sfree(backpath);
                    return false;
                }
                sfree(emsg);

                if (!cn_selftest_files_equal(altpath, backpath)) {
                    if (errmsg)
                        *errmsg = dupstr(
                            "同大小覆盖失败：远端内容与源不同，说明"
                            "「大小相同」被误判成「已传完」而跳过了传输");
                    remove(altpath);
                    remove(backpath);
                    sfree(altpath);
                    sfree(backpath);
                    return false;
                }
                remove(backpath);
                sfree(backpath);
            }
        }

        remove(altpath);
        sfree(altpath);
    }

    return true;
}

/* 逐字节比较两个本地文件；任一侧打不开都算不同。 */
static bool cn_selftest_files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    bool same = true;

    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return false;
    }

    while (same) {
        char ba[8192], bb[8192];
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);

        if (na != nb || memcmp(ba, bb, na) != 0)
            same = false;
        if (na == 0)
            break;
    }

    fclose(fa);
    fclose(fb);
    return same;
}

/* 续传自检：建连接、跑上面那套、写报告。 */
void sftp_selftest_resume_run(Conf *conf, const char *localpath,
                              const char *remotepath)
{
    FILE *fp = fopen("sftp-resume.txt", "w");
    SftpXfer *sx = NULL;
    char *errmsg = NULL;

    if (!fp)
        return;

    sftp_selftest_enable_log("sftp-log.txt");

    fprintf(fp, "断点续传自检\n源: %s\n远端: %s\n\n", localpath, remotepath);
    fflush(fp);

    sx = sftp_xfer_open(conf, NULL, &errmsg);
    if (!sx) {
        fprintf(fp, "失败：建立连接：%s\n", errmsg ? errmsg : "(未知)");
        sfree(errmsg);
        fclose(fp);
        return;
    }
    fprintf(fp, "[1] 连接建立成功\n");
    fflush(fp);

    if (!sftp_xfer_init(sx)) {
        fprintf(fp, "失败：SFTP 握手\n");
        sftp_xfer_close(sx);
        fclose(fp);
        return;
    }
    fprintf(fp, "[2] SFTP 握手成功\n");
    fflush(fp);

    {
        /*
         * sftp_selftest_resume 内部验两件事，任一失败即整体失败：
         *   a) 断点续传从断点起算，没有重发已完成的字节
         *   b) 同大小不同内容、allow_resume=false 时必须真覆盖
         */
        bool ok = sftp_selftest_resume(sx, localpath, remotepath, &errmsg);
        fprintf(fp, "[3] 断点续传（断点起算）%s -> %s : %s\n",
                localpath, remotepath, ok ? "成功" : "失败");
        if (errmsg) {
            fprintf(fp, "    %s\n", errmsg);
            sfree(errmsg);
        }
    }
    fflush(fp);

    sftp_xfer_close(sx);
    fprintf(fp, "完成\n");
    fclose(fp);
}

/*
 * 完整跑一遍：建连接、握手、递归下载、写报告文件。
 * 结果落在当前目录的 sftp-recurse.txt 里。
 */
void sftp_selftest_recurse_run(Conf *conf, const char *src, const char *dst)
{
    FILE *fp = fopen("sftp-recurse.txt", "w");
    SftpXfer *sx = NULL;
    char *errmsg = NULL;

    if (!fp)
        return;

    /* 握手细节全落 sftp-log.txt，卡在哪一步不用猜。 */
    sftp_selftest_enable_log("sftp-log.txt");

    fprintf(fp, "递归传输自检\n源: %s\n目标: %s\n\n", src, dst);
    fflush(fp);

    sx = sftp_xfer_open(conf, NULL, &errmsg);
    if (!sx) {
        fprintf(fp, "失败：建立连接：%s\n", errmsg ? errmsg : "(未知)");
        sfree(errmsg);
        fclose(fp);
        return;
    }
    fprintf(fp, "[1] 连接建立成功\n");
    fflush(fp);

    if (!sftp_xfer_init(sx)) {
        fprintf(fp, "失败：SFTP 握手\n");
        sftp_xfer_close(sx);
        fclose(fp);
        return;
    }
    {
        char *pwd = sftp_xfer_pwd(sx);
        fprintf(fp, "[2] SFTP 握手成功，起始目录 = %s\n", pwd ? pwd : "(空)");
        sfree(pwd);
    }
    fflush(fp);

    {
        bool ok = sftp_selftest_recurse(sx, src, dst, false, &errmsg);
        fprintf(fp, "[3] 递归下载 %s -> %s : %s\n", src, dst,
                ok ? "成功" : "失败");
        if (errmsg) {
            fprintf(fp, "    %s\n", errmsg);
            sfree(errmsg);
        }
    }
    fflush(fp);

    fprintf(fp, "[4] 关闭连接\n");
    sftp_xfer_close(sx);
    fprintf(fp, "完成\n");
    fclose(fp);
}

/* ----------------------------------------------------------------------
 * 建目录 / 删除 / 重命名。
 *
 * 三者都由列表右键菜单触发，remote 由「右键落在哪个列表」直接给出，
 * 不再探测焦点 —— 菜单弹出前焦点已经跟着右键换了地方，靠 GetFocus()
 * 判断只会绕晕自己。
 */

static void cn_do_mkdir(SftpWin *sw, bool remote)
{
    char *errmsg = NULL;
    bool ok;

    /* 沿用系统输入框，省得自绘对话框；名字用默认值提示格式。 */
    {
        wchar_t buf[256];
        wcscpy(buf, L"新建文件夹");
        if (!cn_input_box(sw->hwnd, remote ? "在远端新建目录" : "在本地新建目录",
                          "目录名称：", buf, lenof(buf)))
            return;

        /* 远端新建目录名要按 UTF-8 送出，否则服务器上会多出一个乱码目录。 */
        char *name = dup_wc_to_mb(remote ? CP_UTF8 : DEFAULT_CODEPAGE,
                                  buf, "?");
        char *dir = remote ? sw->remotedir : sw->localdir;
        char *path = cn_path_join(dir, name, remote);

        if (remote)
            ok = sftp_xfer_mkdir(sw->sx, path, &errmsg);
        else
            ok = sftp_local_mkdir(path, &errmsg);

        if (!ok) {
            cn_msg(sw->hwnd, errmsg ? errmsg : "创建失败。", "错误",
                   MB_ICONERROR | MB_OK);
            sfree(errmsg);
        }

        sfree(path);
        sfree(name);
    }

    if (remote)
        cn_refresh_remote(sw);
    else
        cn_refresh_local(sw);
}

static void cn_do_delete(SftpWin *sw, bool remote)
{
    size_t count = 0;
    bool *dirs = NULL;
    char **names = cn_selected_all(remote ? sw->remote_list : sw->local_list,
                                   &count, &dirs, remote);
    int done = 0, failed = 0;
    char *last_err = NULL;

    if (count == 0) {
        cn_set_status(sw, "先选要删除的项。");
        sfree(names);
        sfree(dirs);
        return;
    }

    {
        char *msg = dupprintf("确定删除选中的 %zu 项？此操作无法撤销。", count);
        wchar_t *wt = dup_mb_to_wc(DEFAULT_CODEPAGE, msg);
        int r = MessageBoxW(sw->hwnd, wt, L"确认删除",
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
        sfree(wt);
        sfree(msg);
        if (r != IDYES) {
            sfree(names);
            sfree(dirs);
            return;
        }
    }

    for (size_t i = 0; i < count; i++) {
        char *errmsg = NULL;
        char *path = cn_path_join(remote ? sw->remotedir : sw->localdir,
                                  names[i], remote);
        bool ok;

        if (remote)
            ok = dirs[i] ? sftp_xfer_rmdir(sw->sx, path, &errmsg)
                         : sftp_xfer_remove(sw->sx, path, &errmsg);
        else
            ok = dirs[i] ? sftp_local_rmdir(path, &errmsg)
                         : sftp_local_remove(path, &errmsg);

        if (ok) {
            done++;
        } else {
            failed++;
            sfree(last_err);
            last_err = errmsg ? dupstr(errmsg) : dupstr("未知错误");
            sfree(errmsg);
        }
        sfree(path);
        sfree(names[i]);
    }
    sfree(names);
    sfree(dirs);

    if (remote)
        cn_refresh_remote(sw);
    else
        cn_refresh_local(sw);

    if (failed) {
        cn_msg(sw->hwnd, last_err ? last_err : "部分项目删除失败。",
               "删除失败", MB_ICONWARNING | MB_OK);
    }
    {
        char *msg = dupprintf("删除完成：成功 %d，失败 %d", done, failed);
        cn_set_status(sw, msg);
        sfree(msg);
    }
    sfree(last_err);
}

static void cn_do_rename(SftpWin *sw, bool remote)
{
    bool is_dir = false;
    /* 远端名字走 UTF-8，本地名字走系统代码页，两边对称。 */
    UINT cp = remote ? CP_UTF8 : DEFAULT_CODEPAGE;
    char *oldname = cn_selected_name(remote ? sw->remote_list
                                            : sw->local_list, &is_dir, remote);
    char *errmsg = NULL;
    bool ok = false;

    if (!oldname) {
        cn_set_status(sw, "先选一项再重命名。");
        return;
    }

    {
        wchar_t buf[1024];
        wchar_t *wold = dup_mb_to_wc(cp, oldname);
        wcsncpy(buf, wold, lenof(buf) - 1);
        buf[lenof(buf) - 1] = L'\0';
        sfree(wold);

        if (!cn_input_box(sw->hwnd, "重命名", "新名称：", buf, lenof(buf))) {
            sfree(oldname);
            return;
        }

        char *newname = dup_wc_to_mb(cp, buf, "?");
        char *dir = remote ? sw->remotedir : sw->localdir;
        char *oldpath = cn_path_join(dir, oldname, remote);
        char *newpath = cn_path_join(dir, newname, remote);

        if (remote)
            ok = sftp_xfer_rename(sw->sx, oldpath, newpath, &errmsg);
        else
            ok = sftp_local_rename(oldpath, newpath, &errmsg);

        if (!ok) {
            cn_msg(sw->hwnd, errmsg ? errmsg : "重命名失败。", "错误",
                   MB_ICONERROR | MB_OK);
            sfree(errmsg);
        }

        sfree(oldpath);
        sfree(newpath);
        sfree(newname);
    }

    sfree(oldname);
    (void)is_dir;

    if (remote)
        cn_refresh_remote(sw);
    else
        cn_refresh_local(sw);
}

/* 进入目录（双击用）。 */
static void cn_enter_dir(SftpWin *sw, bool remote, int idx)
{
    wchar_t buf[1024];
    bool is_dir;

    cn_list_get_text(remote ? sw->remote_list : sw->local_list,
                        idx, 0, buf, lenof(buf));
    is_dir = cn_list_item_is_dir(remote ? sw->remote_list : sw->local_list, idx);
    if (!is_dir)
        return;

    {
        /* 远端目录名回传 SFTP 必须是 UTF-8，否则中文目录点进去会 404。 */
        char *name = dup_wc_to_mb(remote ? CP_UTF8 : DEFAULT_CODEPAGE,
                                  buf, "?");
        char *dir = remote ? sw->remotedir : sw->localdir;
        char *path;

        /*
         * 防御层：列表本不该出现 "." / ".."（list 层已过滤），万一
         * 冒出来，"." 原地不动，".." 走上级目录，绝不直接拼路径。
         */
        if (!strcmp(name, "."))
            path = dupstr(dir);
        else if (!strcmp(name, ".."))
            path = cn_path_parent(dir, remote);
        else
            path = cn_path_join(dir, name, remote);

        if (remote) {
            sfree(sw->remotedir);
            sw->remotedir = path;
            cn_refresh_remote(sw);
        } else {
            sfree(sw->localdir);
            sw->localdir = path;
            cn_refresh_local(sw);
        }
        sfree(name);
    }
}

static void cn_go_up(SftpWin *sw, bool remote)
{
    if (remote) {
        char *up = cn_path_parent(sw->remotedir, true);
        sfree(sw->remotedir);
        sw->remotedir = up;
        cn_refresh_remote(sw);
    } else {
        char *up = cn_path_parent(sw->localdir, false);
        sfree(sw->localdir);
        sw->localdir = up;
        cn_refresh_local(sw);
    }
}

/* ----------------------------------------------------------------------
 * 布局。
 */

/*
 * 量出某一列「表头 + 当前所有行」里最宽的那个像素宽度。
 *
 * 早先给后两列写死 80 / 132，结果「修改时间」被截成「2026-07...」，
 * 「大小」下方的数字也贴着左边。这里改成实测：拿控件当前字体建一个
 * DC 逐串丈量，取最大值，再留一点 padding。
 *
 * 行数很多时只量前 CN_MEASURE_ROWS 行 —— 大小与时间的宽度分布很集中，
 * 抽样足够，全量丈量在大目录（几千项）下会明显拖慢刷新。
 */
#define CN_MEASURE_ROWS 200

static int cn_column_content_width(HWND list, int subitem,
                                   const wchar_t *title)
{
    HDC hdc;
    HFONT old, font;
    size_t cap = 0;
    int maxw = 0;
    int rows, n;

    hdc = GetDC(list);
    if (!hdc)
        return -1;
    font = (HFONT)SendMessageW(list, WM_GETFONT, 0, 0);
    old = font ? (HFONT)SelectObject(hdc, font) : NULL;

    /* 先量表头（列头字体在多数主题下与正文一致，差一点无妨）。 */
    if (title) {
        SIZE sz;
        if (GetTextExtentPoint32W(hdc, title, (int)wcslen(title), &sz))
            maxw = sz.cx;
    }

    rows = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
    n = rows < CN_MEASURE_ROWS ? rows : CN_MEASURE_ROWS;
    cap = 512;
    {
        wchar_t *buf = snewn(cap, wchar_t);
        LVITEMW lvi;
        for (int r = 0; r < n; r++) {
            SIZE sz;
            memset(&lvi, 0, sizeof(lvi));
            lvi.iSubItem = subitem;
            lvi.pszText = buf;
            lvi.cchTextMax = (int)cap;
            buf[0] = L'\0';
            SendMessageW(list, LVM_GETITEMTEXTW, (WPARAM)r, (LPARAM)&lvi);
            if (!buf[0])
                continue;
            if (GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &sz) &&
                sz.cx > maxw)
                maxw = sz.cx;
        }
        sfree(buf);
    }

    if (old)
        SelectObject(hdc, old);
    ReleaseDC(list, hdc);
    return maxw;
}

/*
 * 分配三列宽度：名称列吃掉剩余，大小 / 修改时间列按内容实测宽度。
 *
 * 不能拿 MoveWindow 传进去的宽度算 —— 那是含边框的外框宽度；竖滚动条
 * 只在行数超出可视区时出现，宽度还随 DPI 缩放。所以每次都读真实客户区
 * 宽度，并把实测出的两列宽度与它做差，剩余全部给名称列。
 *
 * 必须等列表填完数据后调用（此时滚动条与内容都确定了）。
 */
static int cn_dpi(int logical);

static void cn_fit_columns_ex(SftpWin *sw, bool keep_name)
{
    HWND lists[2] = { sw->remote_list, sw->local_list };

    /* 自己要设列宽，关掉总闸免得被自己的夹紧逻辑拦住。 */
    cn_in_fit = true;

    for (int i = 0; i < 2; i++) {
        RECT cr;
        int avail, name_w, size_w, time_w;
        int cur_name = -1;

        if (!lists[i])
            continue;

        if (keep_name)
            cur_name = (int)SendMessageW(lists[i], LVM_GETCOLUMNWIDTH, 0, 0);
        GetClientRect(lists[i], &cr);
        avail = cr.right;

        /*
         * 关键：不能用客户区宽度当上限。
         *
         * 竖向滚动条是异步出现的 —— 填完数据那一刻 ListView 还没重排，
         * GetClientRect 报的是「没有滚动条」时的宽度（实测 610），等它
         * 真把滚动条摆出来，可用宽度就缩到 578 了。按 610 分配列宽，
         * 结果就是三列之和 610 > 578，横向滚动条必然冒出来。
         *
         * 所以这里主动按「窗口宽 − 竖向滚动条宽」算，无论滚动条当前
         * 是否已经出现，口径都一致，也就不会再溢出。
         */
        {
            RECT wr;
            int vscroll = GetSystemMetrics(SM_CXVSCROLL);
            GetWindowRect(lists[i], &wr);
            avail = (wr.right - wr.left) - 2 * GetSystemMetrics(SM_CXBORDER);
            /* 内容可能超出可视高度 -> 竖滚动条一定会出现，先扣掉。 */
            avail -= vscroll;
        }
        avail -= 2;                        /* 列边界余量 */

        /*
         * 后两列宽度 = 内容实测宽与列头文字宽取大者，再加左右各 10。
         *
         * 只按内容量会出问题：远端目录为空时大小列内容宽是 0，列就缩得
         * 只剩个边，列头「大小」两个字反而被挤出省略号。所以两个都量，
         * 取大的那个 —— 内容普遍是数字与时间戳，列头不会把列撑得很宽。
         *
         * 边距统一 10（原来是 14）：内容宽 55 的大小列加 14 变成 76，
         * 右侧空一大截，主人看图就发现「留的空白太多」。
         */
        size_w = cn_column_content_width(lists[i], 1, L"大小");
        time_w = cn_column_content_width(lists[i], 2, L"修改时间");
        if (size_w < 0)
            size_w = cn_dpi(40);
        if (time_w < 0)
            time_w = cn_dpi(90);
        size_w += cn_dpi(10) * 2;
        time_w += cn_dpi(10) * 2;

        /*
         * 不设固定下限，也不再给封顶。
         *
         * 原来的下限 76 / 150 是字体还是 SimSun 24px 那会儿的观感，字号
         * 收下来之后这两个数就成了纯粹的浪费：大小列内容只有 55，被硬撑
         * 到 76；时间列内容 101，被撑到 150 —— 白白吃掉近百像素，全是
         * 名称列的血。列宽本来就该跟着内容走，内容变窄了就让出来。
         *
         * 封顶同理去掉：内容多宽就给多宽（真超宽了下面的硬约束会和名称
         * 列一起按比例压缩，不会溢出）。
         */

        /*
         * 硬约束：三列之和永不超可用宽度，否则冒出横向滚动条。后两列
         * 实测下来太占地方时按比例压缩它们，而不是让名称列变负。
         */
        if (keep_name && cur_name > 0) {
            /*
             * 用户刚拖过列宽：以他拖出来的名称列为准，把剩下的宽度留给
             * 大小与时间两列。两列都塞不下时先把大小列压到它的实测宽，
             * 再压时间列 —— 总归不能让总和溢出。
             *
             * 这里的「下限」用的是上面刚量出来的 size_w / time_w 自身，
             * 不再是写死的 76 / 150 —— 那两个数是旧字体时代的残留，
             * 现在内容多宽就该多宽。
             */
            int min_size = size_w, min_time = time_w;
            int rest = avail - cur_name;

            if (rest < min_size + min_time) {
                /* 名称列拖得太狠，压到给另两列留出实测宽为止。 */
                cur_name = avail - min_size - min_time;
                if (cur_name < cn_dpi(40))
                    cur_name = cn_dpi(40);
                rest = avail - cur_name;
            }
            name_w = cur_name;
            if (rest >= min_size + min_time) {
                /* 优先保时间列（截断时间戳比截断「5.3 KB」更难看）。 */
                size_w = rest - time_w;
                if (size_w < min_size) {
                    size_w = min_size;
                    time_w = rest - size_w;
                }
            } else {
                size_w = rest / 2;
                time_w = rest - size_w;
            }
        } else if (size_w + time_w > avail - cn_dpi(80)) {
            int fixed = avail - cn_dpi(80);
            if (fixed < cn_dpi(40)) {
                name_w = size_w = time_w = avail / 3;
            } else {
                name_w = cn_dpi(80);
                size_w = fixed * size_w / (size_w + time_w);
                time_w = fixed - size_w;
            }
        } else {
            name_w = avail - size_w - time_w;
        }
        if (name_w < cn_dpi(40))
            name_w = cn_dpi(40);

        SendMessageW(lists[i], LVM_SETCOLUMNWIDTH, 0, name_w);
        SendMessageW(lists[i], LVM_SETCOLUMNWIDTH, 1, size_w);
        SendMessageW(lists[i], LVM_SETCOLUMNWIDTH, 2, time_w);

        /*
         * 重设宽度后列头格式可能被 header 重建清掉，补一次居中。
         * LVM_SETCOLUMNWIDTH 本身不动 fmt，这里属于兜底。
         */
        for (int c = 0; c < 3; c++)
            cn_list_center_header(lists[i], c);
    }

    cn_in_fit = false;
}

/* 自动适配：窗口尺寸变化、填充数据后调用，名称列吃剩余宽度。 */
static void cn_fit_columns(SftpWin *sw)
{
    cn_fit_columns_ex(sw, false);
}

/* ----------------------------------------------------------------------
 * DPI 折算。
 *
 * 这个进程没有 DPI 感知声明，CreateWindowExW / MoveWindow 收到的都是
 * 物理像素。而布局里那一堆尺寸（按钮宽高、边距、列宽下限）是按「逻辑
 * 像素」定的观感，直接用会在高分屏上缩成一小撮、在 100% 屏上又偏胖。
 *
 * 所以凡是给界面用的固定尺寸，都必须过一遍 cn_dpi：它把逻辑值折算成本
 * 机实际要用的物理像素。窗口尺寸那处早就在这么做了（MulDiv(1000, dpi,
 * 96)），这里把口径统一成一个函数，省得各处各算各的。
 */
static int cn_dpi(int logical)
{
    /*
     * DPI 只取一次。
     *
     * 原来每次调用都 GetDC(NULL) + GetDeviceCaps，实测单次约 4.4 微秒；
     * 这个函数在布局、列宽测量、绘制里被调用几十次（列宽那段还要按行
     * 循环调），累计就是白扔的开销。本进程没有 DPI 感知声明，跑在系统
     * DPI 虚拟化下，进程内的 DPI 恒定，缓存一份即可。
     */
    static int cached_dpi = 0;

    if (!cached_dpi) {
        HDC dc = GetDC(NULL);
        int dpi = 96;

        if (dc) {
            int d = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(NULL, dc);
            if (d > 0)
                dpi = d;
        }
        cached_dpi = dpi;
    }
    return MulDiv(logical, cached_dpi, 96);
}

static void cn_layout(SftpWin *sw)
{
    RECT rc;
    int w, h;
    /*
     * 下面这批尺寸全部是「逻辑像素」，必须过 cn_dpi 折算。
     *
     * 它们是当初在 200% 缩放的屏上调出来的物理值（116 的按钮宽在当时
     * 只相当于逻辑 58），直接照搬到 100% 屏幕就变成「按 200% 的物理量
     * 摆 100% 的窗口」—— 控件把窗口挤满，列表只剩一小截，名称一列
     * 显示成「S...」。反过来在 100% 下调出来的值搬到 200% 又太小。
     *
     * 所以一律按逻辑值写，交给 cn_dpi 折算成本机物理像素。
     */
    const int margin = cn_dpi(10), gap = cn_dpi(10);
    const int toolbar_h = cn_dpi(26), path_h = cn_dpi(20),
              status_h = cn_dpi(18), prog_h = cn_dpi(12);
    const int midw = cn_dpi(84);          /* 中间按钮列宽 */
    const int btnw = cn_dpi(58);          /* 「↑ 返回」按钮宽 */
    const int btnh = cn_dpi(22);          /* 「↑ 返回」按钮高 */
    int list_top, list_h, list_w;
    int left_x, right_x, mid_x;

    GetClientRect(sw->hwnd, &rc);
    w = rc.right;
    h = rc.bottom;

    /* 两栏等宽：总宽 = 边距 + 左 + 间距 + 中 + 间距 + 右 + 边距 */
    list_w = (w - margin * 2 - midw - gap * 2) / 2;
    {
        int floor = cn_dpi(120);
        if (list_w < floor)
            list_w = floor;
    }

    left_x = margin;
    mid_x = left_x + list_w + gap;
    right_x = mid_x + midw + gap;

    /*
     * 两侧工具栏：各一个「返回」。远端那侧贴左缘，本地那侧贴右缘 ——
     * 两个按钮各自靠向自己那一栏的外侧，视线不用来回跳。
     *
     * 本地那个右边缘要和下面的路径框右边缘对齐：路径框宽 list_w，所以
     * 按钮左边界 = right_x + list_w - btnw。别在这里减滚动条宽度 ——
     * 滚动条属于列表内部，路径框并没有让位，减了反而错开一截。
     */
    MoveWindow(sw->btn_up, left_x, margin, btnw, btnh, TRUE);
    MoveWindow(sw->btn_up_local, right_x + list_w - btnw, margin, btnw,
               btnh, TRUE);
    /*
     * cn: 「桌面」键在「↑ 返回」左侧，与返回键同行等高，右缘与返回
     * 键左缘之间留 6px 间距。空间不够时（极窄窗口）让位给返回键。
     */
    if (list_w > btnw * 2 + cn_dpi(6))
        MoveWindow(sw->btn_desktop_local,
                   right_x + list_w - btnw * 2 - cn_dpi(6), margin,
                   btnw, btnh, TRUE);
    else
        MoveWindow(sw->btn_desktop_local, right_x, margin, 0, 0, TRUE);

    /* 路径行：各栏上方对齐各自列表（远端在左，本地在右） */
    {
        int y = margin + toolbar_h;
        MoveWindow(sw->remote_path, left_x, y, list_w, path_h, TRUE);
        MoveWindow(sw->local_path, right_x, y, list_w, path_h, TRUE);
    }

    list_top = margin + toolbar_h + path_h + gap;
    list_h = h - list_top - status_h - prog_h - margin * 2 - gap * 2;
    {
        int floor = cn_dpi(60);
        if (list_h < floor)
            list_h = floor;
    }

    MoveWindow(sw->remote_list, left_x, list_top, list_w, list_h, TRUE);
    MoveWindow(sw->local_list, right_x, list_top, list_w, list_h, TRUE);

    /*
     * 中间那一列：上传 / 下载在垂直中段，暂停 / 终止贴列底。
     *
     * 两组分开是刻意的：上传下载是「选完文件就要按」的操作，暂停终止
     * 是「传起来以后才管用」的状态控制，用途不同，挤在一堆会让人对着
     * 四个按钮犹豫。贴在列底既不占列表高度，视线往下扫一下就到。
     */
    {
        const int bh = cn_dpi(22), bgap = cn_dpi(8);
        int total = bh * 2 + bgap;
        int by = list_top + (list_h - total) / 2;
        if (by < list_top)
            by = list_top;

        MoveWindow(sw->btn_to_remote, mid_x, by, midw, bh, TRUE);
        MoveWindow(sw->btn_to_local, mid_x, by + bh + bgap, midw, bh, TRUE);

        /* 列底：终止在最下，暂停在它上面，两个贴着列表底边。 */
        {
            int stop_y = list_top + list_h - bh;
            int pause_y = stop_y - bgap - bh;

            /*
             * 列表太矮时别让它们顶到上传下载身上 —— 按可用高度把
             * 中段那组往上收，保证四者之间至少留一个 bgap。
             */
            if (pause_y < by + bh + bgap) {
                int shift = (by + bh + bgap) - pause_y;
                by -= shift;
                if (by < list_top)
                    by = list_top;
                MoveWindow(sw->btn_to_remote, mid_x, by, midw, bh, TRUE);
                MoveWindow(sw->btn_to_local, mid_x, by + bh + bgap,
                           midw, bh, TRUE);
            }

            MoveWindow(sw->btn_pause, mid_x, pause_y, midw, bh, TRUE);
            MoveWindow(sw->btn_stop, mid_x, stop_y, midw, bh, TRUE);
        }
    }

    /* 底部：进度条吃满整行（暂停/终止已挪到中间列底部） */
    {
        const int bar_h = 22;
        int y = h - margin - status_h - bar_h - gap;
        MoveWindow(sw->progress, margin, y, w - margin * 2, bar_h, TRUE);
        MoveWindow(sw->status, margin, h - margin - status_h,
                   w - margin * 2, status_h, TRUE);
    }

    /*
     * 列表列宽跟着栏宽走：名称列吃掉剩余宽度，后两列固定。
     *
     * 这里只做初算；真正精确的适配在 cn_fit_columns() 里，因为滚动条
     * 是否存在要等列表填了数据才知道。
     */
    cn_fit_columns(sw);
}

/* ----------------------------------------------------------------------
 * 窗口过程。
 */

/* ----------------------------------------------------------------------
 * 拖放上传。
 *
 * DragAcceptFiles 挂在两个列表控件上，不挂主窗口：挂主窗口的话，
 * 拖到列表上方时命中子控件，光标不显示「可放置」，消息也进不来。
 * 而 ListView 收到 WM_DROPFILES 不会转发父窗口，所以子类化一层，
 * 把消息交回主窗口统一处理。HDROP 所有权随消息移交，由处理方
 * DragFinish。
 */

static WNDPROC cn_list_oldproc = NULL;

/* 拖拽起点的判定状态：按下在项上、还没超过阈值。 */
static bool cn_drag_active = false;
static HWND cn_drag_from_list = NULL;
static POINT cn_drag_start;
/*
 * 行上起手的手势分流（三段式：按下只记录 → 长按开门 → 位移裁决）。
 *
 * 三条业务规则：
 *   ① 未选中项 + 单击                     = 选中它（ListView 默认过程完成）
 *   ② 已选中项 + 长按满后拖动             = 拖放这批文件
 *   ③ 未选中项 + 长按满后拖动             = 框选
 *
 * 实现的三个阶段：
 *
 * 【阶段一 按下】只记录，绝不动作
 *   记起点、记时刻。不画框、不传文件，界面完全不动，选中交给默认过程。
 *
 * 【阶段二 长按开门】按住满 CN_LONGPRESS_MS 才允许拖/框选
 *   这条闸门是「单击永远只是单击」的唯一保证：没按满就松手，
 *   不管中间怎么晃，都退化成一次普通单击。
 *
 * 【阶段三 位移裁决】长按已成立后，位移超系统拖拽阈值才动作
 *   按按下前的选中状态分流：原来没选中 → 框选；原来已选中 → 拖放。
 *   长按成立但还没移动 = 继续等，绝不在原地画框。
 *
 * 为什么两个条件都要：只看时长 → 按住不动也在等着，反而没有「意图」；
 * 只看位移 → 单击时手抖几个像素就穿透。时长管「愿意为此花时间」，
 * 位移管「确实要挪东西」，两者同时成立才是真正的拖拽意图。
 *
 * 关键前提：cn_press_was_selected 必须在默认过程之前记录。
 * ListView 默认过程处理 WM_LBUTTONDOWN 时会把按下的项选中，事后再问
 * 「这一项原来选没选中」，答案永远是「选中」，②③ 就分不开。
 *
 * ⚠️ 长按开门靠 SetTimer 驱动，但定时器消息必须做双重校验。
 * WM_TIMER 是异步的：松手清完状态后它还会迟到一次。老代码就是没校验，
 * 迟到的消息把状态重新置真，表现为「单击完约半秒后又莫名冒框」。
 * 现在两道闸门：
 *   ① 只在 cn_press_pending 为真时认这条消息（松手已清，迟到的无效）；
 *   ② 再问一次 GetKeyState(VK_LBUTTON)，物理键没按着就一律不认。
 * 校验不过就地 KillTimer，不留后患。
 *
 * 为什么非要定时器：Windows 只在鼠标**移动**时产生 WM_MOUSEMOVE。
 * 用户按住不动盯着屏幕，一条 MOVE 都不会来，光靠 MOVE 里的 GetTickCount
 * 现算永远等不到「按满」——「长按全失效」就是这么来的。
 */
#define CN_LONGPRESS_MS   100    /* 长按开门时长：按满这么久了才允许拖/框选 */
#define CN_LONGPRESS_TIMER 0x5F21 /* 长按开门的定时器 ID（子类里专用） */
static bool cn_press_was_selected = false; /* 按下点那一项在按下前是否选中 */
static bool cn_press_pending = false;      /* 行上按下、还没松手 */
static DWORD cn_press_tick = 0;            /* 按下时刻 GetTickCount() */
static HWND cn_press_list = NULL;          /* 哪一侧列表正在按住 */
/*
 * 长按已开门：按满 CN_LONGPRESS_MS，此后位移超阈值即可拖/框选。
 * 这个标志是「拖拽意图已确认」，不是「框选已开始」—— 用户可能按满
 * 了却不动，那仍然什么都不该发生。
 */
static bool cn_longpress_static = false;

/*
 * 收掉「按下 / 长按」这一整套状态。
 *
 * 所有收尾路径（松手、捕获被抢、框选结束、空白起手覆盖）都必须走这里，
 * 不能各自手写三行赋值 —— 漏掉任何一处，残留的 cn_press_pending 会让
 * 下一次鼠标移动被误判成「还按着」，框选就会在单击之后莫名冒出来。
 * 停定时器也在这里，一处不漏。
 */
static void cn_press_reset(void)
{
    if (cn_press_list && cn_press_pending)
        KillTimer(cn_press_list, CN_LONGPRESS_TIMER);
    cn_press_list = NULL;
    cn_press_was_selected = false;
    cn_press_pending = false;
    cn_press_tick = 0;
    cn_longpress_static = false;
}

/*
 * 手势诊断日志：只有环境变量 CN_GESTURE_LOG 指向某个文件时才写。
 *
 * 长按延迟这类问题靠读代码推不出来 —— 消息到底几时到、定时器几时响，
 * 只有实测时间戳能说明。日志按真实时间轴记下每次按下 / 定时器 / 开门 /
 * 移动 / 动作，事后一眼就能看出卡在哪一段。
 *
 * 默认关闭：环境变量不设时 statics 里没任何 IO，正常使用零开销。
 */
static FILE *cn_glog_fp = NULL;
static bool cn_glog_tried = false;

static void cn_glog(const char *fmt, ...)
{
    SYSTEMTIME st;
    va_list ap;

    if (!cn_glog_tried) {
        const char *path = getenv("CN_GESTURE_LOG");

        cn_glog_tried = true;
        if (path && *path)
            cn_glog_fp = fopen(path, "a");
    }
    if (!cn_glog_fp)
        return;
    GetLocalTime(&st);
    fprintf(cn_glog_fp, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(cn_glog_fp, fmt, ap);
    va_end(ap);
    fputc('\n', cn_glog_fp);
    fflush(cn_glog_fp);
}
/*
 * 本次拖放是不是窗口内部发起的。
 *
 * 只有自己发起的拖放才带方向语义：拖到哪个列表就是往哪个方向传，
 * 于是「本地拖到本地」会被当成一次下载，源与目标同在一个目录，
 * 白白弹覆盖询问 —— 这种同侧拖动必须直接忽略。从资源管理器拖进来
 * 的是外部数据，源目录天然不同于目标目录，不受这条限制。
 */
static bool cn_drag_ours = false;
static bool cn_drag_ours_remote = false;   /* 自己发起时，源是不是远端 */

/*
 * 自绘橡皮筋框选。
 *
 * 原生 ListView 只在空白处按下拖动才出框选；项上按下拖动是「拖拽项」
 * 手势。而我们把项上拖动接成了 OLE 拖放，框选在项上就完全出不来了。
 * 现按资源管理器语义区分：从已选中项拖 = 拖放文件，从未选中项拖 =
 * 框选。框选自己画（SetCapture + DrawFocusRect 橡皮筋 + 实时圈选）。
 */
static bool cn_marquee_active = false;
static HWND cn_marquee_list = NULL;
static POINT cn_marquee_start;      /* 起点与当前橡皮筋矩形，客户区坐标 */
static POINT cn_marquee_pt;
static RECT cn_marquee_last;        /* 上次画出去的框，用于 XOR 擦除 */
static bool cn_marquee_last_valid = false;
/*
 * 本次按下是不是落在列表空白处。
 *
 * 空白起手与项上起手都要能框选：项上起手按「已选中 / 未选中」分流到拖放
 * 或框选；空白起手只有框选一种语义（资源管理器行为）。
 *
 * 之前只在项上置 cn_drag_active，空白起手时整个自绘框选路径根本没进，
 * 交给 comctl32 自带的框选去处理 —— 而它松手时会把选中清掉，表现就是
 * 「拖的时候有高亮、一松手全没了」。
 */
static bool cn_drag_from_blank = false;

/* ----------------------------------------------------------------------
 * OLE 拖放：窗口内左右互拖。
 *
 * WM_DROPFILES 只能「从资源管理器拖进来」，做不到在窗口内部把文件从
 * 一个列表拖到另一个列表 —— 那是 OLE 的地盘：源侧给 IDataObject，
 * 目标侧实现 IDropTarget，中间由 ole32 牵线。
 *
 * 这里只做「窗口内互拖」这一层，且只发布 CF_HDROP（文件名列表）：
 *   - 拖本地 → 远端：源是右侧列表，落点是左侧远端列表，触发上传；
 *   - 拖远端 → 本地：源是左侧列表，落点是右侧本地列表，触发下载。
 * 落到列表后统一走 cn_transfer_entries，与按钮传输完全同一条路径，
 * 覆盖询问、暂停、终止、进度全都跟着一致。
 *
 * 拖出到资源管理器（把远端文件存到桌面）不在这一层：远端文件本地无
 * 实体，得靠系统回调落点再异步下载，属于另一套机制，尚未实现。
 */

typedef struct CnDropSrc {
    IDropSource iface;
    LONG ref;
} CnDropSrc;

typedef struct CnDataObj {
    IDataObject iface;
    LONG ref;
    char **names;       /* 文件名（UTF-8），所有权归本对象 */
    size_t count;
    bool *dirs;
} CnDataObj;

/*
 * 自己发起的那批数据的原始对象。
 *
 * 落点要用它，而不是从 CF_HDROP 反解 —— 远端文件在本地无实体，反解
 * 出来的路径查不到属性，目录会被当成文件去打开，于是报「无法打开本地
 * 文件」。只在 DoDragDrop 期间有效，返回后立刻清掉。
 */
static CnDataObj *cn_drag_source_obj = NULL;

typedef struct CnDropTgt {
    IDropTarget iface;
    LONG ref;
    HWND list;
    bool remote_side;   /* 本列表是否远端那侧 */
    bool ok;            /* 最近一次 DragEnter 是否接受 */
} CnDropTgt;

static CnDropTgt *cn_drop_local = NULL;    /* 本地列表上的拖放目标 */
static CnDropTgt *cn_drop_remote = NULL;   /* 远端列表上的拖放目标 */

/* ---- IDataObject ---- */

static HRESULT cn_do_QueryInterface(IDataObject *p, const IID *riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) {
        *out = p;
        p->lpVtbl->AddRef(p);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG cn_do_AddRef(IDataObject *p)
{
    return (ULONG)InterlockedIncrement(&((CnDataObj *)p)->ref);
}

static ULONG cn_do_Release(IDataObject *p)
{
    CnDataObj *d = (CnDataObj *)p;
    LONG r = InterlockedDecrement(&d->ref);
    if (r == 0) {
        for (size_t i = 0; i < d->count; i++)
            sfree(d->names[i]);
        sfree(d->names);
        sfree(d->dirs);
        sfree(d);
    }
    return (ULONG)r;
}

static HGLOBAL cn_hdrop_from(const CnDataObj *d)
{
    size_t chars = 1;
    wchar_t *wbuf;
    size_t w = 0;
    HGLOBAL h;
    DROPFILES *df;

    for (size_t i = 0; i < d->count; i++) {
        wchar_t *wn = dup_mb_to_wc(CP_UTF8, d->names[i]);
        chars += wcslen(wn) + 1;
        sfree(wn);
    }
    wbuf = snewn(chars, wchar_t);
    for (size_t i = 0; i < d->count; i++) {
        wchar_t *wn = dup_mb_to_wc(CP_UTF8, d->names[i]);
        size_t l = wcslen(wn);
        memcpy(wbuf + w, wn, l * sizeof(wchar_t));
        w += l;
        wbuf[w++] = L'\0';
        sfree(wn);
    }
    wbuf[w] = L'\0';

    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
                    sizeof(DROPFILES) + (w + 1) * sizeof(wchar_t));
    if (!h) {
        sfree(wbuf);
        return NULL;
    }
    df = (DROPFILES *)GlobalLock(h);
    if (!df) {
        GlobalFree(h);
        sfree(wbuf);
        return NULL;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy((char *)df + sizeof(DROPFILES), wbuf, (w + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    sfree(wbuf);
    return h;
}

static HRESULT cn_do_GetData(IDataObject *p, FORMATETC *fmt, STGMEDIUM *med)
{
    CnDataObj *d = (CnDataObj *)p;
    HGLOBAL h;

    if (!fmt || !med)
        return E_INVALIDARG;
    memset(med, 0, sizeof(*med));
    if (fmt->cfFormat != CF_HDROP || !(fmt->tymed & TYMED_HGLOBAL))
        return DV_E_FORMATETC;
    h = cn_hdrop_from(d);
    if (!h)
        return E_OUTOFMEMORY;
    med->tymed = TYMED_HGLOBAL;
    med->hGlobal = h;
    med->pUnkForRelease = NULL;
    return S_OK;
}

static HRESULT cn_do_QueryGetData(IDataObject *p, FORMATETC *fmt)
{
    (void)p;
    if (!fmt)
        return E_INVALIDARG;
    return (fmt->cfFormat == CF_HDROP && (fmt->tymed & TYMED_HGLOBAL))
           ? S_OK : DV_E_FORMATETC;
}

static HRESULT cn_do_GetDataHere(IDataObject *p, FORMATETC *f, STGMEDIUM *m)
{ (void)p; (void)f; (void)m; return E_NOTIMPL; }
static HRESULT cn_do_GetCanonical(IDataObject *p, FORMATETC *f, FORMATETC *o)
{ (void)p; (void)f; if (o) o->ptd = NULL; return E_NOTIMPL; }
static HRESULT cn_do_SetData(IDataObject *p, FORMATETC *f, STGMEDIUM *m, BOOL r)
{ (void)p; (void)f; (void)m; (void)r; return E_NOTIMPL; }
static HRESULT cn_do_EnumFormatEtc(IDataObject *p, DWORD dir, IEnumFORMATETC **o)
{ (void)p; (void)dir; if (o) *o = NULL; return E_NOTIMPL; }
static HRESULT cn_do_DAdvise(IDataObject *p, FORMATETC *f, DWORD a,
                             IAdviseSink *s, DWORD *c)
{ (void)p; (void)f; (void)a; (void)s; if (c) *c = 0; return E_NOTIMPL; }
static HRESULT cn_do_DUnadvise(IDataObject *p, DWORD c)
{ (void)p; (void)c; return E_NOTIMPL; }
static HRESULT cn_do_EnumDAdvise(IDataObject *p, IEnumSTATDATA **o)
{ (void)p; if (o) *o = NULL; return E_NOTIMPL; }

static const IDataObjectVtbl cn_do_vtbl = {
    cn_do_QueryInterface, cn_do_AddRef, cn_do_Release,
    cn_do_GetData, cn_do_GetDataHere, cn_do_QueryGetData,
    cn_do_GetCanonical, cn_do_SetData, cn_do_EnumFormatEtc,
    cn_do_DAdvise, cn_do_DUnadvise, cn_do_EnumDAdvise,
};

/* ---- IDropSource ---- */

static HRESULT cn_ds_QueryInterface(IDropSource *p, const IID *riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropSource)) {
        *out = p;
        p->lpVtbl->AddRef(p);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG cn_ds_AddRef(IDropSource *p)
{
    return (ULONG)InterlockedIncrement(&((CnDropSrc *)p)->ref);
}

static ULONG cn_ds_Release(IDropSource *p)
{
    CnDropSrc *d = (CnDropSrc *)p;
    LONG r = InterlockedDecrement(&d->ref);
    if (r == 0)
        sfree(d);
    return (ULONG)r;
}

static HRESULT cn_ds_QueryContinueDrag(IDropSource *p, BOOL escape, DWORD keys)
{
    (void)p;
    if (escape)
        return DRAGDROP_S_CANCEL;
    /* 左键松开即落下；中途按 Esc 已在上一条拦下。 */
    return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
}

static HRESULT cn_ds_GiveFeedback(IDropSource *p, DWORD effect)
{
    (void)p; (void)effect;
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

static const IDropSourceVtbl cn_ds_vtbl = {
    cn_ds_QueryInterface, cn_ds_AddRef, cn_ds_Release,
    cn_ds_QueryContinueDrag, cn_ds_GiveFeedback,
};

/* ---- IDropTarget ---- */

static HRESULT cn_dt_QueryInterface(IDropTarget *p, const IID *riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *out = p;
        p->lpVtbl->AddRef(p);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG cn_dt_AddRef(IDropTarget *p)
{
    return (ULONG)InterlockedIncrement(&((CnDropTgt *)p)->ref);
}

static ULONG cn_dt_Release(IDropTarget *p)
{
    CnDropTgt *d = (CnDropTgt *)p;
    LONG r = InterlockedDecrement(&d->ref);
    if (r == 0) {
        if (cn_drop_local == d)
            cn_drop_local = NULL;
        if (cn_drop_remote == d)
            cn_drop_remote = NULL;
        sfree(d);
    }
    return (ULONG)r;
}

/* 取剪贴板格式里 DROPFILES 的文件名列表（UTF-8）。 */
static char **cn_hdrop_names(HGLOBAL h, size_t *count, bool **dirs)
{
    DROPFILES *df = (DROPFILES *)GlobalLock(h);
    char **names = NULL;
    bool *d = NULL;
    size_t n = 0, cap = 0;
    const wchar_t *p;

    *count = 0;
    if (dirs) *dirs = NULL;
    if (!df)
        return NULL;
    p = (const wchar_t *)((const char *)df + df->pFiles);

    while (*p) {
        const wchar_t *base = wcsrchr(p, L'\\');
        if (!base) base = wcsrchr(p, L'/');
        base = base ? base + 1 : p;
        if (*base) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                {
                    char **nn = snewn(cap, char *);
                    bool *nd = snewn(cap, bool);
                    if (names) memcpy(nn, names, n * sizeof(char *));
                    if (d) memcpy(nd, d, n * sizeof(bool));
                    sfree(names); sfree(d);
                    names = nn; d = nd;
                }
            }
            names[n] = dup_wc_to_mb(CP_UTF8, base, "?");
            /*
             * 目录标记必须按真实来源判断，不能一律写 false。
             *
             * 本地路径好办：问文件系统。远端路径本地查不到，只能信任
             * 拖放源给的 dirs（cn_selected_all 从列表读出的准确标记）——
             * 所以窗口自己发起的拖放根本不该走这个函数，见 cn_dt_Drop
             * 里的处理：自己的数据直接用对象里存的 names/dirs，只有
             * 外部拖入才来这里解析路径。
             */
            {
                wchar_t wfull[1024];
                wcsncpy(wfull, p, lenof(wfull) - 1);
                wfull[lenof(wfull) - 1] = L'\0';
                {
                    DWORD attr = GetFileAttributesW(wfull);
                    d[n] = (attr != INVALID_FILE_ATTRIBUTES &&
                            (attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
                }
            }
            n++;
        }
        p += wcslen(p) + 1;
    }
    GlobalUnlock(h);

    if (dirs) *dirs = d; else sfree(d);
    *count = n;
    return names;
}

static HRESULT cn_dt_DragEnter(IDropTarget *p, IDataObject *obj, DWORD key,
                              POINTL pt, DWORD *effect)
{
    CnDropTgt *t = (CnDropTgt *)p;
    FORMATETC fmt;
    (void)key; (void)pt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.cfFormat = CF_HDROP;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;

    t->ok = (obj && SUCCEEDED(obj->lpVtbl->QueryGetData(obj, &fmt)));
    if (effect)
        *effect = t->ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return t->ok ? S_OK : E_UNEXPECTED;
}

static HRESULT cn_dt_DragOver(IDropTarget *p, DWORD key, POINTL pt, DWORD *effect)
{
    CnDropTgt *t = (CnDropTgt *)p;
    (void)key; (void)pt;
    if (effect)
        *effect = t->ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT cn_dt_DragLeave(IDropTarget *p)
{
    CnDropTgt *t = (CnDropTgt *)p;
    t->ok = false;
    return S_OK;
}

static HRESULT cn_dt_Drop(IDropTarget *p, IDataObject *obj, DWORD key,
                          POINTL pt, DWORD *effect)
{
    CnDropTgt *t = (CnDropTgt *)p;
    FORMATETC fmt;
    STGMEDIUM med;
    SftpWin *sw = the_sftpwin;
    char **names;
    bool *dirs;
    size_t count = 0;
    bool to_remote = t->remote_side;
    (void)key; (void)pt;

    if (effect)
        *effect = DROPEFFECT_NONE;
    if (!obj || !sw || sw->busy)
        return E_UNEXPECTED;

    memset(&fmt, 0, sizeof(fmt));
    fmt.cfFormat = CF_HDROP;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;

    memset(&med, 0, sizeof(med));
    if (FAILED(obj->lpVtbl->GetData(obj, &fmt, &med)))
        return E_UNEXPECTED;

    /*
     * 自己发起的拖放：直接取拖放对象里的原始清单。
     *
     * 为什么要绕开 HDROP 解析：远端文件在本地没有实体，从 HDROP 里的
     * 名字反推「是不是目录」只能问文件系统，远端目录必然查不到，会被
     * 当成普通文件去打开，于是报「无法打开本地文件」。对象里的 dirs
     * 是从列表读出来的准确标记，远端也一样准，用它才对。
     */
    if (cn_drag_ours && cn_drag_source_obj) {
        CnDataObj *o = cn_drag_source_obj;
        names = snewn(o->count, char *);
        dirs = snewn(o->count, bool);
        for (size_t i = 0; i < o->count; i++) {
            names[i] = dupstr(o->names[i]);
            dirs[i] = o->dirs ? o->dirs[i] : false;
        }
        count = o->count;
    } else {
        names = cn_hdrop_names(med.hGlobal, &count, &dirs);
    }
    ReleaseStgMedium(&med);

    if (!names || count == 0) {
        sfree(names);
        sfree(dirs);
        return E_UNEXPECTED;
    }

    /*
     * 同一侧拖动不做事。
     *
     * 窗口内部拖放的方向是按落点定的：落在远端列表就上传、落在本地
     * 列表就下载。可要是从本地列表拖回本地列表，方向就成了「下载到
     * 本地」，源目录和目标目录是同一个，于是弹覆盖询问、点了还真的
     * 原地覆盖一遍 —— 用户只是手滑，不该产生任何写操作。
     *
     * 判据用「本次拖放是不是自己发起的」+「源和目标是否同侧」：只有
     * 自己发起的才有同侧可言；外部拖进来的数据源目录天然不同。
     */
    if (cn_drag_ours && cn_drag_ours_remote == t->remote_side) {
        cn_set_status(sw, "源和目标是同一侧，未做任何操作。");
        if (effect)
            *effect = DROPEFFECT_NONE;
        for (size_t i = 0; i < count; i++)
            sfree(names[i]);
        sfree(names);
        sfree(dirs);
        return S_OK;
    }

    /*
     * 方向由落点决定：落在远端列表上就是上传，落在本地列表上就是下载。
     * 与「拖到哪个面板都一样」的 WM_DROPFILES 语义不同 —— 这里是
     * 两个列表互拖，方向必须跟着落点走。
     */
    {
        char *msg = dupprintf("已拖入 %u 项，开始%s…", (unsigned)count,
                              to_remote ? "上传" : "下载");
        cn_set_status(sw, msg);
        sfree(msg);
    }
    cn_transfer_entries(sw, names, NULL, dirs, count, to_remote);

    if (effect)
        *effect = DROPEFFECT_COPY;
    return S_OK;
}

static const IDropTargetVtbl cn_dt_vtbl = {
    cn_dt_QueryInterface, cn_dt_AddRef, cn_dt_Release,
    cn_dt_DragEnter, cn_dt_DragOver, cn_dt_DragLeave, cn_dt_Drop,
};

/*
 * 从列表里把选中项打包成拖放源，然后进入 OLE 拖放循环。
 * 返回 true 表示真的发起了拖放（而不是「没选中，什么都没做」）。
 */
static bool cn_start_drag(SftpWin *sw, HWND list, bool from_remote)
{
    CnDataObj *obj;
    CnDropSrc *src;
    DWORD effect = DROPEFFECT_NONE;
    size_t count = 0;
    bool *dirs = NULL;
    char **names;
    HRESULT hr;

    names = cn_selected_all(list, &count, &dirs, from_remote);
    if (!names || count == 0) {
        sfree(names);
        sfree(dirs);
        return false;
    }

    obj = snew(CnDataObj);
    memset(obj, 0, sizeof(*obj));
    /*
     * 虚表本身是 const（谁都不该改它），但 lpVtbl 字段的类型是非 const
     * 指针，直接赋值会丢 const 限定。这里显式转一道：不是为了改它，
     * 只是让类型对上，顺手消掉编译器的 -Wdiscarded-qualifiers。
     */
    obj->iface.lpVtbl = (IDataObjectVtbl *)&cn_do_vtbl;
    obj->ref = 1;
    obj->names = names;
    obj->count = count;
    obj->dirs = dirs;

    src = snew(CnDropSrc);
    memset(src, 0, sizeof(*src));
    src->iface.lpVtbl = (IDropSourceVtbl *)&cn_ds_vtbl;
    src->ref = 1;

    /*
     * 打上「自己发起」的标记。拖放目标靠它判断同侧拖动：OLE 拖放
     * 过程中事件可能插进来，标记必须在 DoDragDrop 之前设好、之后
     * 立刻清掉，免得外部拖入被误当成自己的。
     */
    cn_drag_ours = true;
    cn_drag_ours_remote = from_remote;
    cn_drag_source_obj = obj;

    hr = DoDragDrop(&obj->iface, &src->iface, DROPEFFECT_COPY, &effect);

    cn_drag_source_obj = NULL;
    /*
     * cn_drag_ours 不能在这里就清。
     *
     * 系统把内部拖动的那条 WM_DROPFILES 是 post 进队列的，等
     * DoDragDrop 返回、消息泵转起来才派发到列表。此刻若已经清零，
     * 那条消息就会被当成「外部拖入」放行，绕开同侧判断再传一遍。
     * 真正的清理交给消费它的那一方（见 cn_list_proc 的 WM_DROPFILES
     * 分支），以及下一次拖拽起点重设 —— 拖拽起点会先把标志复位。
     */

    src->iface.lpVtbl->Release(&src->iface);
    obj->iface.lpVtbl->Release(&obj->iface);
    (void)hr;
    (void)sw;
    return true;
}

/* 给一个列表注册为 OLE 拖放目标。 */
static bool cn_register_drop_target(HWND list, bool remote_side)
{
    CnDropTgt *t = snew(CnDropTgt);
    memset(t, 0, sizeof(*t));
    t->iface.lpVtbl = (IDropTargetVtbl *)&cn_dt_vtbl;
    t->ref = 1;
    t->list = list;
    t->remote_side = remote_side;
    if (remote_side)
        cn_drop_remote = t;
    else
        cn_drop_local = t;
    if (FAILED(RegisterDragDrop(list, &t->iface))) {
        if (remote_side) cn_drop_remote = NULL; else cn_drop_local = NULL;
        sfree(t);
        return false;
    }
    return true;
}


/*
 * 菜单内容的构建单独拎出来：界面自检要能查「三项都在、无选中时删/改
 * 是灰的、作用对象灰字对不对」，而这些只能从 HMENU 上读，没法通过
 * 真的弹菜单去验（弹出来就卡住自检了）。调用方负责 DestroyMenu。
 */
static HMENU cn_build_list_menu(SftpWin *sw, bool remote)
{
    HMENU menu = CreatePopupMenu();
    HWND list = remote ? sw->remote_list : sw->local_list;
    char *hint;

    if (!menu)
        return NULL;

    AppendMenuW(menu, MF_STRING, IDM_MKDIR, L"新建目录(&N)");
    AppendMenuW(menu, MF_STRING, IDM_DELETE, L"删除(&D)");
    AppendMenuW(menu, MF_STRING, IDM_RENAME, L"重命名(&R)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    /* 「刷新」从顶栏按钮搬来：同样是对某一侧的操作，放一起吃。 */
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"刷新(&F)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    /* 末尾一行灰字标明作用对象，省得用户点了才发现动错面板。 */
    hint = dupprintf("作用于%s面板", remote ? "远端" : "本地");
    {
        wchar_t *whint = dup_mb_to_wc(DEFAULT_CODEPAGE, hint);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, whint);
        sfree(whint);
    }
    sfree(hint);

    /* 没有选中项时，删除与重命名没意义，灰掉而不是让它报错。 */
    if (ListView_GetNextItem(list, -1, LVNI_SELECTED) < 0) {
        EnableMenuItem(menu, IDM_DELETE, MF_BYCOMMAND | MF_GRAYED);
        EnableMenuItem(menu, IDM_RENAME, MF_BYCOMMAND | MF_GRAYED);
    }

    return menu;
}

/*
 * 列表右键菜单。
 *
 * 三个文件操作的入口。之所以挂在列表上而不是工具栏：右键点哪一栏，
 * 操作就属于哪一栏，归属由消息来源直接给出，不需要任何焦点探测。
 *
 * 右键点在某行上、而该行没选中时，先把选中切到这一行 —— 否则用户
 * 右键甲文件再点「删除」，删掉的是之前选中的乙文件，这种事故没法
 * 用「手滑」解释。
 */
static void cn_list_context_menu(HWND list, int sx, int sy)
{
    SftpWin *sw = (SftpWin *)GetWindowLongPtrW(GetParent(list), GWLP_USERDATA);
    bool remote = (sw && list == sw->remote_list);
    HMENU menu;
    POINT pt;
    int cmd;

    if (!sw || sw->busy)
        return;

    /*
     * 键盘唤出（Shift+F10 / 菜单键）时坐标是 -1，改用选中行位置。
     *
     * ⚠️ WM_CONTEXTMENU 的 lparam 本来就是**屏幕坐标**：鼠标右键给的是
     * 光标屏幕位置，键盘唤出给 (-1,-1)。早先这里又做了一次
     * ClientToScreen，等于把窗口自身偏移加了第二遍，菜单于是飞到窗口
     * 右下方 —— 偏出去的量正好是客户区在屏幕上的位置。
     */
    if (sx < 0 && sy < 0) {
        int idx = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        if (idx >= 0) {
            RECT rc;
            ListView_GetItemRect(list, idx, &rc, LVIR_BOUNDS);
            pt.x = (rc.left + rc.right) / 2;
            pt.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(list, &pt);
        } else {
            RECT crc;
            GetClientRect(list, &crc);
            pt.x = crc.left + 8;
            pt.y = crc.top + 8;
            ClientToScreen(list, &pt);
        }
    } else {
        /* 已是屏幕坐标，直接用作弹出点；HitTest 另转客户区。 */
        pt.x = sx;
        pt.y = sy;
    }

    /*
     * 点到的行不在选中集里，就把选中切换到它。用 LVHITTESTINFO 带上
     * LVHT_ONITEM 判断，点到空白处不误清选中。
     */
    {
        POINT cp = pt;
        LVHITTESTINFO ht;
        ScreenToClient(list, &cp);
        memset(&ht, 0, sizeof(ht));
        ht.pt = cp;
        if (ListView_HitTest(list, &ht) >= 0 &&
            (ht.flags & LVHT_ONITEM) &&
            !(ListView_GetItemState(list, ht.iItem, LVIS_SELECTED) &
              LVIS_SELECTED)) {
            ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(list, ht.iItem, LVIS_SELECTED,
                                  LVIS_SELECTED);
        }
    }

    menu = cn_build_list_menu(sw, remote);
    if (!menu)
        return;

    /*
     * TPM_RETURNCMD 直接拿命令号，省掉 WM_COMMAND 回绕。菜单必须在
     * 之后销毁，顺序反了句柄就野了。
     */
    cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD |
                              TPM_NONOTIFY, pt.x, pt.y, 0, sw->hwnd, NULL);
    DestroyMenu(menu);

    switch (cmd) {
      case IDM_MKDIR:
        cn_do_mkdir(sw, remote);
        break;
      case IDM_DELETE:
        cn_do_delete(sw, remote);
        break;
      case IDM_RENAME:
        cn_do_rename(sw, remote);
        break;
      case IDM_REFRESH:
        if (remote)
            cn_refresh_remote(sw);
        else
            cn_refresh_local(sw);
        break;
    }
}

/* 这个 header 是不是两个列表之一。拖列宽的通知要靠它过滤。 */
static bool cn_is_our_header(HWND header)
{
    if (!the_sftpwin || !header)
        return false;
    return header == (HWND)SendMessageW(the_sftpwin->remote_list,
                                        LVM_GETHEADER, 0, 0) ||
           header == (HWND)SendMessageW(the_sftpwin->local_list,
                                        LVM_GETHEADER, 0, 0);
}

/*
 * 夹紧某一列的目标宽度：总宽不许超过列表客户区。
 *
 * 另外两列各留一个下限（大小列要装得下「5.3 KB」，时间列要装得下
 * 「YYYY-MM-DD HH:MM」），本列最多吃到「客户区 − 那两个下限」。三列
 * 最小合计都放不下时按比例分，绝不返回一个会让总宽溢出的值。
 */
static int cn_clamp_column(SftpWin *sw, HWND list, int col, int want)
{
    RECT cr;
    int avail, min_size, min_time, min_self, others, cap;
    int widths[3];

    (void)sw;

    if (cn_in_fit || !list || col < 0 || col > 2)
        return want;

    GetClientRect(list, &cr);
    avail = cr.right;
    /*
     * 与 cn_fit_columns_ex 同口径：按「窗口宽 − 竖滚动条」算，避免滚动条
     * 异步出现导致可用宽度被高估、列宽超界。
     */
    {
        RECT wr;
        GetWindowRect(list, &wr);
        avail = (wr.right - wr.left) - 2 * GetSystemMetrics(SM_CXBORDER) -
                GetSystemMetrics(SM_CXVSCROLL);
    }
    avail -= 2;
    if (avail <= 0)
        return want;

    for (int c = 0; c < 3; c++)
        widths[c] = (int)SendMessageW(list, LVM_GETCOLUMNWIDTH, c, 0);

    /*
     * 另两列保持现状（用户没动它们），只夹本列 —— 这样拖名称列时另外
     * 两列不会莫名跳动。它们已占的宽度就是本列的上限来源。
     */
    others = 0;
    for (int c = 0; c < 3; c++)
        if (c != col)
            others += widths[c];

    cap = avail - others;

    /* 本列自己的下限，别被拖成 0 宽看不见。 */
    min_size = 76;
    min_time = 150;
    min_self = (col == 1) ? min_size : (col == 2) ? min_time : 40;

    if (cap < min_self) {
        /*
         * 另两列已经占满，本列连下限都塞不下。原样放行就是直接溢出，
         * 所以按比例把另两列一起压到「客户区 − 本列下限」以内。
         */
        int other[2], n = 0, rest, a, b, sa, sb;

        if (avail < 40 + 76 + 150) {
            /* 客户区实在太小，三列均分。 */
            return avail / 3;
        }

        for (int c = 0; c < 3; c++)
            if (c != col)
                other[n++] = c;

        rest = avail - min_self;
        a = widths[other[0]];
        b = widths[other[1]];

        if (a + b <= rest)
            return min_self;

        if (a + b <= 0) {
            sa = sb = rest / 2;
        } else {
            sa = (int)((long)rest * a / (a + b));
            sb = rest - sa;
        }

        /* 另两列不低于各自下限，缺口由对方让。 */
        if (other[0] == 1 && sa < 76) {
            sb -= (76 - sa);
            sa = 76;
        } else if (other[0] == 2 && sa < 150) {
            sb -= (150 - sa);
            sa = 150;
        }
        if (other[1] == 1 && sb < 76) {
            sa -= (76 - sb);
            sb = 76;
        } else if (other[1] == 2 && sb < 150) {
            sa -= (150 - sb);
            sb = 150;
        }
        if (sa < 0)
            sa = 0;
        if (sb < 0)
            sb = 0;

        SendMessageW(list, LVM_SETCOLUMNWIDTH, other[0], sa);
        SendMessageW(list, LVM_SETCOLUMNWIDTH, other[1], sb);
        return min_self;
    }

    if (want > cap)
        want = cap;
    if (want < min_self)
        want = min_self;
    return want;
}

/*
 * 判断列表里的某个客户区坐标是否落在某一行上。
 *
 * 抽成独立函数是为了自检能直接验判定逻辑：自检里若真去
 * SendMessageW(WM_LBUTTONDOWN) 走完整子类过程，CallWindowProcW 调默认
 * 过程时会卡死（自检窗口未 ShowWindow、没有真实输入队列，默认过程在
 * 处理鼠标按下时阻塞，实测永不返回）。
 */
static bool cn_list_point_on_item(HWND list, int x, int y)
{
    LVHITTESTINFO ht;

    memset(&ht, 0, sizeof(ht));
    ht.pt.x = x;
    ht.pt.y = y;
    return ListView_HitTest(list, &ht) >= 0 && (ht.flags & LVHT_ONITEM);
}

/* 橡皮筋矩形：起点到当前点的规范矩形。 */
static RECT cn_marquee_rect(void)
{
    RECT r;

    r.left = cn_marquee_start.x < cn_marquee_pt.x ? cn_marquee_start.x
                                                  : cn_marquee_pt.x;
    r.top = cn_marquee_start.y < cn_marquee_pt.y ? cn_marquee_start.y
                                                 : cn_marquee_pt.y;
    r.right = cn_marquee_start.x < cn_marquee_pt.x ? cn_marquee_pt.x
                                                   : cn_marquee_start.x;
    r.bottom = cn_marquee_start.y < cn_marquee_pt.y ? cn_marquee_pt.y
                                                    : cn_marquee_start.y;
    return r;
}

/* 圈选：与橡皮筋相交的行选中，不相交的取消。只对状态变化行调 Set。 */
static void cn_marquee_apply(HWND hwnd, const RECT *r)
{
    int count = ListView_GetItemCount(hwnd);
    RECT dummy;

    for (int i = 0; i < count; i++) {
        RECT ir;
        bool hit, sel;

        ListView_GetItemRect(hwnd, i, &ir, LVIR_BOUNDS);
        hit = !(ir.right < r->left || ir.left > r->right ||
                ir.bottom < r->top || ir.top > r->bottom);
        sel = (ListView_GetItemState(hwnd, i, LVIS_SELECTED) &
               LVIS_SELECTED) != 0;
        if (hit != sel) {
            ListView_SetItemState(hwnd, i, hit ? LVIS_SELECTED : 0,
                                  LVIS_SELECTED);
        }
    }
    (void)dummy;
}

/* 开始框选：接管鼠标。 */
static void cn_marquee_begin(HWND hwnd, const POINT *pt)
{
    RECT r;

    cn_marquee_active = true;
    cn_marquee_list = hwnd;
    cn_marquee_start = *pt;
    cn_marquee_pt = *pt;
    cn_marquee_last_valid = false;
    SetCapture(hwnd);
    /*
     * 起点即当前点，矩形零面积，与任何行都不相交 —— 等价于「单击空白 =
     * 取消全部选中」。放在这里而不是各调用点各写一遍：从空白起手和从
     * 未选中项起手（按下时默认过程已把它选中）都需要这个初始清空。
     */
    r = cn_marquee_rect();
    cn_marquee_apply(hwnd, &r);
    if (the_sftpwin)
        cn_update_transfer_buttons(the_sftpwin);
}

/*
 * 逐帧更新：更新圈选 + 请求重画橡皮筋。
 *
 * 这里不用 DrawFocusRect 的 XOR 自擦除，原因有两个：
 *   ① 中间那次 cn_marquee_apply 会改变选中状态，ListView 随之重绘，
 *      XOR 擦除时底图已经不是当初画框时那张，擦不干净；
 *   ② 每帧依赖「先擦旧框再画新框」的严格对称，只要有一帧被漏掉
 *      （比如消息合并、重入），残留框线就会永久留在列表上。
 *
 * 改为：矩形变了就 InvalidateRect 旧框与新框的并集，让列表按正常流
 * 重画一遍底图，橡皮筋在 WM_PAINT 的最后统一画一次。底图始终是干净
 * 的，框线永远不会叠出来。
 */
static void cn_marquee_update(HWND hwnd, const POINT *pt)
{
    RECT r, dirty;

    cn_marquee_pt = *pt;
    r = cn_marquee_rect();
    cn_marquee_apply(hwnd, &r);

    /*
     * 脏区取「旧框 ∪ 新框」，再把橡皮筋那几像素外扩一下，保证画框
     * 线条本身被完整覆盖。旧框已画在客户区上，不并进来就擦不掉。
     */
    if (cn_marquee_last_valid) {
        UnionRect(&dirty, &cn_marquee_last, &r);
    } else {
        dirty = r;
    }
    InflateRect(&dirty, 2, 2);
    InvalidateRect(hwnd, &dirty, FALSE);

    cn_marquee_last = r;
    cn_marquee_last_valid = true;
}

/*
 * 结束框选：清橡皮筋、释放鼠标、刷按钮。
 *
 * 收尾必须整窗作废并立即重绘，不能只作废旧框那块脏区：
 *
 *   框是用 PS_DOT 画的 1px 点线，落在一堆白色行间空隙上。只作废框四周
 *   两像素的话，列表重绘时会走它自己的「只画脏区」优化，点线压在行边界
 *   或滚动条边缘的那几个像素未必被覆盖到；更糟的是 Windows 对
 *   InvalidateRect(..., FALSE) 不擦背景，被覆盖的只是「重画出来的部分」，
 *   边界上那一条就永久留下来了。主人看到的「单击文件也残留框线」，绝大部分
 *   就是这类半覆盖的残线。
 *
 *   RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN 让列表连背景一起整块重
 *   画，UpdateWindow 立刻同步刷出去，不留一帧过渡。窗口只有几百像素见方，
 *   这点重绘开销完全可以忽略。
 */
static void cn_marquee_end(HWND hwnd, bool apply_final)
{
    if (hwnd == cn_marquee_list && cn_marquee_last_valid) {
        /*
         * 先把状态清干净再重绘：WM_PAINT 里的绘制条件依赖
         * cn_marquee_active / cn_marquee_last_valid，早一步清掉，
         * 重绘出来的底图里就不会再带那条框。
         */
        cn_marquee_active = false;
        cn_marquee_last_valid = false;
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                         RDW_UPDATENOW);
    }
    if (GetCapture() == hwnd)
        ReleaseCapture();
    cn_marquee_active = false;
    cn_marquee_list = NULL;
    cn_marquee_last_valid = false;
    /*
     * 按下/长按状态在这里一并收掉：捕获被弹窗之类抢走时不会收到 UP，
     * 只靠 UP 分支清会漏掉这条路径，下一轮按下就会带着上一轮的
     * 「按下前是否选中」和长按门状态误判分流。
     */
    cn_press_reset();
    if (apply_final && the_sftpwin)
        cn_update_transfer_buttons(the_sftpwin);
}

/*
 * 清掉「另一侧」列表的选中。
 *
 * 两侧列表都开了 LVS_SHOWSELALWAYS：失去焦点的列表把选中行画成灰色而
 * 不是消失。于是选中本地某个文件、再去点远端列表，本地那行仍是灰底，
 * 上传按钮也还亮着，用户看到的是「两边都选着、按钮也不知道在传谁」。
 *
 * 这里在「某一侧被点击 / 获得焦点」时把对侧选中清掉，保证任一时刻只有
 * 一侧有选中，按钮状态与视觉一致。
 */
static void cn_drop_other_side_selection(HWND hwnd)
{
    HWND other;

    if (!the_sftpwin)
        return;
    if (hwnd == the_sftpwin->local_list)
        other = the_sftpwin->remote_list;
    else if (hwnd == the_sftpwin->remote_list)
        other = the_sftpwin->local_list;
    else
        return;

    if (other && ListView_GetNextItem(other, -1, LVNI_SELECTED) >= 0) {
        ListView_SetItemState(other, -1, 0, LVIS_SELECTED);
        cn_update_transfer_buttons(the_sftpwin);
    }
}

static LRESULT CALLBACK cn_list_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam)
{
    /*
     * 长按开门定时器。
     *
     * Windows 只在鼠标移动时发 WM_MOUSEMOVE，用户按住不动就一条消息都
     * 没有，光靠 MOVE 里现算 GetTickCount 永远等不到「按满」。所以按下
     * 时挂一个一次性定时器，到点直接把门打开。
     *
     * ⚠️ 必须双重校验，否则老毛病复发：
     *   ① 本次按下还在（cn_press_pending 且窗口对得上）—— 松手时已
     *      cn_press_reset，迟到的消息在这里被吃掉；
     *   ② 物理左键此刻真的还按着 —— 有些路径（模态循环、弹窗）会让
     *      UP 丢在路上，只看标志位不够，必须问系统。
     * 任一不成立就地 KillTimer，绝不让它把状态重新置真。
     */
    if (msg == WM_TIMER && wparam == CN_LONGPRESS_TIMER) {
        bool still_down = (GetKeyState(VK_LBUTTON) & 0x8000) != 0;

        KillTimer(hwnd, CN_LONGPRESS_TIMER);
        if (!cn_press_pending || cn_press_list != hwnd || !still_down) {
            /* 迟到的或者已经无关的消息：顺手把残留状态收干净。 */
            if (cn_press_list == hwnd && (!cn_press_pending || !still_down))
                cn_press_reset();
            return 0;
        }
        cn_longpress_static = true;   /* 门开了：此后位移超阈值即可动作 */
        cn_glog("TIMER 开门 held=%lu",
                (unsigned long)(GetTickCount() - cn_press_tick));
        /*
         * ⚠️ 开门之后必须主动补一条 MOVE，否则动作要等到用户下次动手。
         *
         * 这是「第一段位移永远不触发、停一下再动才触发」的根因：
         *   按下后立刻拖 → 第一段位移全程在门限内，每条 MOVE 都被
         *   「门没开」丢弃；用户停住不松手，定时器到点把门打开，可此时
         *   系统一条 MOVE 都不发，动作根本没机会执行；等用户再动第二段，
         *   门已开、累计位移早超阈值，于是「必定触发」。
         *
         * 补发一条带 MK_LBUTTON 的 MOVE，坐标取当前光标位置，动作就在
         * 正常的 MOVE 分支里执行 —— 不在这里直接调 cn_start_drag，
         * 免得在 WM_TIMER 里进 DoDragDrop 模态循环。
         * 若用户按满了却没动，这条 MOVE 的位移在阈值内，会被无害丢弃。
         */
        {
            POINT pt;
            RECT rc;

            /*
             * 不要求光标仍在客户区内：用户拖得快，可能已经冲出列表边界，
             * 那恰恰说明位移肯定超阈值了，更该触发。只要能算客户区坐标
             * 就补发；算不出来（窗口正在销毁之类）就跳过，等下次真实
             * MOVE 再裁决。
             */
            if (GetCursorPos(&pt) && GetClientRect(hwnd, &rc) &&
                ScreenToClient(hwnd, &pt)) {
                PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON,
                             MAKELPARAM((short)pt.x, (short)pt.y));
                cn_glog("TIMER 补发 MOVE (%d,%d)", (int)pt.x, (int)pt.y);
            }
        }
        return 0;
    }
    if (msg == WM_DROPFILES) {
        /*
         * 同一个列表窗口上并存着两条拖放通道：
         *   - RegisterDragDrop（OLE）：窗口内部互拖走这条；
         *   - DragAcceptFiles（WM_DROPFILES）：外部拖入本来走这条。
         *
         * 一次内部拖动会被两边同时收到：OLE 那条已经按落点判过方向、
         * 该拦的也拦了，紧接着系统又补一条 WM_DROPFILES 过来，绕开
         * 全部判断再传一遍 —— 表现就是「同侧拖动还是弹覆盖」。
         *
         * 所以只要本次拖放是自己发起的，就把 WM_DROPFILES 直接丢掉：
         * 它已经被 OLE 那条处理过，不能重来。外部拖入时 cn_drag_ours
         * 为假，照常放行。
         */
        if (cn_drag_ours) {
            /* 这条是内部拖动自己那条的迟到副本，吃掉它并收尾。 */
            cn_drag_ours = false;
            DragFinish((HDROP)wparam);
            return 0;
        }
        /*
         * 主窗口句柄必须现取，不能存在列表的 GWLP_USERDATA 里。
         *
         * 那个槽位归 comctl32 的 ListView 自己用：写进去的值会被它的
         * 窗口过程覆盖，等 WM_DROPFILES 来读时拿到的是垃圾指针 ——
         * 表现就是拖文件进来毫无反应（main 为假，直接 DragFinish 丢掉），
         * 而且顺带把 ListView 的内部状态踩了。列表是主窗口的直接子窗口，
         * GetParent 就能拿到，不必存。
         */
        HWND main = GetParent(hwnd);
        if (!main)
            main = the_sftpwin ? the_sftpwin->hwnd : NULL;
        if (main) {
            SendMessageW(main, WM_DROPFILES, wparam, 0);
        } else {
            /* 主窗口句柄没拿到，自己收尾，别把 HDROP 泄了。 */
            DragFinish((HDROP)wparam);
        }
        return 0;
    }
    if (msg == WM_CONTEXTMENU) {
        /* 坐标按有符号取：键盘唤出时 lparam 是 0xFFFFFFFF。 */
        int sx = (short)LOWORD(lparam);
        int sy = (short)HIWORD(lparam);
        cn_list_context_menu(hwnd, sx, sy);
        return 0;
    }
    /*
     * 点空白处取消选中。
     *
     * 列表带了 LVS_SHOWSELALWAYS，系统默认行为是「失去焦点也照样显示
     * 选中」，点空白区域并不会清掉选中 —— 于是中间的上传/下载按钮一直
     * 亮着，用户没法用鼠标表达「我不选了」。
     *
     * 顺序很重要：必须先把消息交给默认过程，再清选中。反过来做的话，
     * 默认过程在处理 WM_LBUTTONDOWN 时会把选中状态重新设回去，等于白清
     * （自检只发 DOWN 不跑完整流程，看不出这个坑，真实点击才暴露）。
     * 清完再显式刷一次按钮，不依赖 LVN_ITEMCHANGED 是否被派发。
     */
    if (msg == WM_LBUTTONDOWN) {
        bool on_item = cn_list_point_on_item(hwnd, (short)LOWORD(lparam),
                                             (short)HIWORD(lparam));
        /*
         * 记录「按下点那一项在按下前是否已选中」。
         *
         * ⚠️ 必须在下面 CallWindowProcW 之前读。ListView 默认过程处理
         * WM_LBUTTONDOWN 时会把按下的项选中，等 MOVE 分流时再来问，答案
         * 永远是「选中」—— ②已选中拖动=拖放 / ③未选中拖动=框选 就分不开，
         * 全部退化成一个分支。
         *
         * 用「按下点是否落在当前任一选中项上」而不是「这一项自身是否
         * 选中」，是为了多选场景：选中 3 个文件后按住其中一个拖动，语义
         * 是「拖走这 3 个」，此时按下点所在项必然在选中集合里。
         */
        {
            LVHITTESTINFO hti;
            int hit = -1;

            memset(&hti, 0, sizeof(hti));
            hti.pt.x = (short)LOWORD(lparam);
            hti.pt.y = (short)HIWORD(lparam);
            if (ListView_SubItemHitTest(hwnd, &hti) >= 0)
                hit = hti.iItem;
            cn_press_was_selected =
                hit >= 0 &&
                (ListView_GetItemState(hwnd, hit, LVIS_SELECTED) &
                 LVIS_SELECTED) != 0;
        }
        /*
         * 新的一次按下 = 上一轮拖放彻底结束，迟到的 WM_DROPFILES 已经
         * 没机会再来了，这时把标志复位，避免它一直挂着把后面的外部
         * 拖入也误吞掉。
         */
        cn_drag_ours = false;
        /*
         * 点了这一侧 = 用户换边了，对侧选中一律让位。
         *
         * 必须在这里做，不能只靠 WM_SETFOCUS：从另一侧点过来时焦点本来
         * 就已经在这个列表上（点空白/点行都不改变焦点归属），SETFOCUS
         * 未必再来一次。放在 DOWN 里最稳，两条起手路径（行上、空白）
         * 都覆盖到。
         */
        cn_drop_other_side_selection(hwnd);
        if (!on_item) {
            /*
             * 空白起手：先只记录起点，等鼠标真的动起来再开始框选。
             *
             * 之前这里在按下瞬间就 cn_marquee_begin，后果是「单击空白」
             * 也会走一遍框选开始/结束 —— 界面上闪过一条零面积框，而且
             * 只要收尾路径有任何一条没走到（点击被合并、窗口重绘被打断），
             * 那条框就永久留在列表上。主人反馈的「单击文件也触发框选并
             * 残留框线」就是这么来的。
             *
             * 现在的分工：
             *   - 按下：只清旧选中（等价「点空白 = 不选了」）+ 记起点；
             *   - 移动超阈值：才 cn_marquee_begin，进入真正的框选；
             *   - 直接松手没动：什么都不用收尾，因为压根没画过。
             *
             * 同样不把 DOWN 交给 comctl32：它一进自己的框选状态，我们
             * 扣下后续 MOVE/UP 之后它永远收不到收尾，松手就会清选中。
             */
            SetFocus(hwnd);
            cn_drag_active = true;
            cn_drag_from_blank = true;
            cn_drag_from_list = hwnd;
            cn_drag_start.x = (short)LOWORD(lparam);
            cn_drag_start.y = (short)HIWORD(lparam);
            /* 空白起手没有单击语义：拖动即框选，不受长按闸门约束。 */
            cn_press_reset();
            /* 点空白即「取消选择」，语义与系统列表一致。 */
            ListView_SetItemState(hwnd, -1, 0, LVIS_SELECTED);
            if (the_sftpwin)
                cn_update_transfer_buttons(the_sftpwin);
            return 0;
        }
        {
            LRESULT r = CallWindowProcW(cn_list_oldproc, hwnd, msg, wparam,
                                        lparam);
            /* 记下按下点：后续 MOUSEMOVE 靠它判断够不够拖拽阈值。 */
            cn_drag_active = true;
            cn_drag_from_blank = false;
            cn_drag_from_list = hwnd;
            cn_drag_start.x = (short)LOWORD(lparam);
            cn_drag_start.y = (short)HIWORD(lparam);
            /*
             * 记下按下时刻并挂长按定时器。
             *
             * 时间戳用于 MOVE 里现算（连续移动时不必等定时器）；定时器
             * 用于「按住完全不动」的场景 —— 那时系统一条 MOVE 都不发，
             * 只有定时器能把门打开。两者互补，谁先到都行。
             */
            cn_press_pending = true;
            cn_press_tick = GetTickCount();
            cn_press_list = hwnd;
            cn_longpress_static = false;
            SetTimer(hwnd, CN_LONGPRESS_TIMER, CN_LONGPRESS_MS, NULL);
            cn_glog("DOWN 行上 起点=(%d,%d) 门限=%dms",
                    (int)cn_drag_start.x, (int)cn_drag_start.y,
                    CN_LONGPRESS_MS);
            return r;
        }
    }
    /*
     * 拖拽起点：长按按满后，位移超系统阈值才分流。
     *
     *   ① 未选中项 + 长按满 + 拖动 → 框选
     *   ② 已选中项 + 长按满 + 拖动 → 拖放这批文件
     *   ③ 单击（长按没按满就松手）→ 由 ListView 默认过程完成选中
     *
     * 长按没按满时 MOVE 全被丢弃，所以单击期间的手抖绝不会冒框 —— 这是
     * 「单击只选中」的保证，详见下面 MOVE 分支的说明。
     *
     * 分流依据是「按下前的选中状态」（cn_press_was_selected，在 DOWN 里
     * 抢在默认过程之前记录）：选中的东西拖动 = 把这批文件拖走，没选中的
     * 东西上拖动 = 拉框选一片。
     *
     * 阈值取 SM_CXDRAG / SM_CYDRAG，和资源管理器一致 —— 手抖一两像素
     * 不该误触发动作。DoDragDrop 是模态循环，期间会抽消息，返回后才
     * 继续，所以不能在这里做别的事。
     */
    if (msg == WM_MOUSEMOVE && cn_drag_active && the_sftpwin) {
        int dx = (short)LOWORD(lparam) - cn_drag_start.x;
        int dy = (short)HIWORD(lparam) - cn_drag_start.y;
        int tx = GetSystemMetrics(SM_CXDRAG);
        int ty = GetSystemMetrics(SM_CYDRAG);

        /*
         * 左键必须真的还按着，否则立刻丢弃整套按下状态。
         *
         * 这是「单击结束后再移动，过一会儿突显框选」的根因所在：老代码
         * 只看 cn_drag_active 这个自维护的标志，从不问 wparam 里的
         * MK_LBUTTON。松手那条 UP 但凡有一个路径没送到列表（模态循环、
         * 弹窗抢捕获），标志就残留为真；此后随便晃一下鼠标都能进这段，
         * 此时 held 早已超过长按时长，门一开、位移一超阈值，框选就这么
         * 凭空冒出来了。
         *
         * 加上这道校验，标志残留也无害 —— 物理键一松，一切归零。
         */
        if (!(wparam & MK_LBUTTON)) {
            cn_press_reset();
            cn_drag_active = false;
            cn_drag_from_blank = false;
            return 0;
        }
        /*
         * 空白起手：不受长按闸门约束，拖动超阈值即框选。
         *
         * 空白处没有「单击选中」可言（单击空白只是取消选择，由 DOWN
         * 分支当场完成），所以这里不需要用时长换确定性，直接响应。
         */
        if (cn_drag_from_blank) {
            if (abs(dx) > tx || abs(dy) > ty) {
                cn_drag_active = false;
                cn_drag_from_blank = false;
                cn_marquee_begin(hwnd, &cn_drag_start);
                return 0;
            }
            return 0;
        }
        /*
         * 行上起手：先用时长开门，再用位移裁决。
         *
         * 开门有两条互补的路：
         *   ① WM_TIMER（主路）：按住完全不动时只有它能到点 —— 系统在
         *      鼠标静止时不发 MOVE，光靠现算永远等不到。
         *   ② 这里现算（补充路）：用户按下就一直在移动，MOVE 连绵不断，
         *      定时器消息可能排在消息队列里，现算能更早开门，跟手。
         *
         * 门开了之后才看位移：超阈值才真的动作，没超就继续等。按满了
         * 却不动，说明用户只是手停在鼠标上，什么都不该发生。
         */
        if (cn_press_pending) {
            DWORD held = (DWORD)(GetTickCount() - cn_press_tick);

            if (!cn_longpress_static && held >= CN_LONGPRESS_MS) {
                cn_longpress_static = true;
                cn_glog("MOVE 现算开门 held=%lu dx=%d dy=%d",
                        (unsigned long)held, dx, dy);
            }
        }
        if (!cn_longpress_static)
            return 0;   /* 门没开（还没按满）：位移再大也不动作 */
        if (abs(dx) > tx || abs(dy) > ty) {
            bool from_remote = (hwnd == the_sftpwin->remote_list);
            bool was_sel = cn_press_was_selected;

            cn_drag_active = false;   /* 先清，避免重入 */
            cn_press_reset();
            /*
             * 起点用按下时记录的 cn_drag_start，不能用当前点 —— 当前点
             * 已经越过阈值了，拿它当起点会吃掉框选范围的头部一截（用户
             * 明明从更上方起手，那几行圈不到）。
             */
            if (!was_sel) {
                /* ③ 未选中项起手拖动 → 框选。 */
                cn_glog("ACTION 框选 dx=%d dy=%d", dx, dy);
                cn_marquee_begin(hwnd, &cn_drag_start);
                return 0;
            }
            /* ② 已选中项起手拖动 → 拖放这批文件。 */
            cn_glog("ACTION 拖放 dx=%d dy=%d", dx, dy);
            cn_start_drag(the_sftpwin, hwnd, from_remote);
            return 0;
        }
        return 0;
    }
    /* 框选进行中：更新橡皮筋与圈选。 */
    if (msg == WM_MOUSEMOVE && cn_marquee_active && hwnd == cn_marquee_list) {
        POINT pt;

        pt.x = (short)LOWORD(lparam);
        pt.y = (short)HIWORD(lparam);
        cn_marquee_update(hwnd, &pt);
        return 0;
    }
    /*
     * 列宽总闸：无论谁（用户拖 header、代码调 LVM_SETCOLUMNWIDTH）想改
     * 列宽，都在这里先夹紧，保证三列之和永不超过列表客户区宽度，横向
     * 滚动条从源头上就不该出现。
     *
     * 拦截消息本身而不是依赖 HDN_ENDTRACK：实测 LVM_SETCOLUMNWIDTH 根本
     * 不发那条通知，名称列被设到 900 时总宽直接冲到 1216（客户区才
     * 578），横向滚动条照出。拦消息是唯一覆盖全部改宽路径的办法。
     */
    if (msg == LVM_SETCOLUMNWIDTH && the_sftpwin) {
        int col = (int)wparam;
        int want = (int)(short)LOWORD(lparam);
        int clamped = cn_clamp_column(the_sftpwin, hwnd, col, want);
        return CallWindowProcW(cn_list_oldproc, hwnd, msg, (WPARAM)col,
                               (LPARAM)(short)clamped);
    }
    /*
     * 拖动列宽仍要收尾：header 拖动是内部改宽，不走上面那条消息，所以
     * 松手后再复查一次。cn_fit_columns_ex 在合规时不做改动。
     */
    if (msg == WM_LBUTTONUP && the_sftpwin) {
        cn_drag_active = false;
        /*
         * 空白起手的标志也要在这里复位：单击空白（没越过拖拽阈值）
         * 不会走到框选，也就没有 cn_marquee_end 帮它清，留着这个标志
         * 会让下一次在行上按下时被误判成「空白起手」。
         */
        cn_drag_from_blank = false;
        /*
         * 阶段三：按前面留下的状态做最终裁决。
         *
         * 框选已激活 → 走下面的 cn_marquee_end 正常收尾（保留选中）；
         * 框选没激活 → 交给默认过程，它就是「单击选中」那条路。
         * 只此两条：长按没按满就松手必然是单击，按满了但没动也是单击。
         */
        cn_press_reset();
        if (cn_marquee_active && hwnd == cn_marquee_list) {
            /* 框选松手：到此为止，不把 UP 交给默认过程（免得又改选中）。 */
            cn_marquee_end(hwnd, true);
            return 0;
        }
        LRESULT r = CallWindowProcW(cn_list_oldproc, hwnd, msg, wparam,
                                    lparam);
        cn_fit_columns_ex(the_sftpwin, true);
        return r;
    }
    if (msg == WM_CAPTURECHANGED && cn_marquee_active &&
        hwnd == cn_marquee_list) {
        /* 捕获被别人抢走（弹窗之类）：框选就地收场，别让橡皮筋挂着。 */
        cn_marquee_end(hwnd, true);
        return 0;
    }
    /*
     * 焦点落到某一侧列表 = 用户切换了操作对象，另一侧选中要让位。
     *
     * 键盘 Tab / 程序化 SetFocus 也会走到这里（鼠标点击的另一条路径由
     * WM_LBUTTONDOWN 兜），两条路都清，保证任一时刻只有一侧有选中。
     */
    if (msg == WM_SETFOCUS)
        cn_drop_other_side_selection(hwnd);
    /*
     * 橡皮筋在 WM_PAINT 的最后画：先让 ListView 按正常流程重画底图
     * （行、选中高亮都刷新），再在最上层补一条点线框。
     *
     * 必须在默认过程之后画，否则会被底图盖掉。这里也不走 DrawFocusRect
     * 的 XOR 语义，改用 SetROP2(R2_COPYPEN) 直接描点线 —— XOR 的擦除
     * 依赖底图与画框时完全一致，一旦中间有重绘就擦不掉，表现为框线
     * 越拖越多、松手还留一片。
     */
    if (msg == WM_PAINT && cn_marquee_active && hwnd == cn_marquee_list &&
        cn_marquee_last_valid) {
        LRESULT r = CallWindowProcW(cn_list_oldproc, hwnd, msg, wparam,
                                    lparam);
        HDC hdc = GetDC(hwnd);
        HPEN pen = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
        HGDIOBJ old_pen = SelectObject(hdc, pen);
        HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        int old_rop = SetROP2(hdc, R2_COPYPEN);
        Rectangle(hdc, cn_marquee_last.left, cn_marquee_last.top,
                  cn_marquee_last.right, cn_marquee_last.bottom);
        SetROP2(hdc, old_rop);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);
        ReleaseDC(hwnd, hdc);
        return r;
    }
    return CallWindowProcW(cn_list_oldproc, hwnd, msg, wparam, lparam);
}

static void cn_init_controls(SftpWin *sw)
{
    HINSTANCE inst = GetModuleHandle(NULL);

    sw->local_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                                      L"", WS_CHILD | WS_VISIBLE |
                                      ES_AUTOHSCROLL | ES_READONLY,
                                      0, 0, 0, 0, sw->hwnd, (HMENU)IDC_LOCAL_PATH,
                                      inst, NULL);
    sw->remote_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                                       L"", WS_CHILD | WS_VISIBLE |
                                       ES_AUTOHSCROLL | ES_READONLY,
                                       0, 0, 0, 0, sw->hwnd, (HMENU)IDC_REMOTE_PATH,
                                       inst, NULL);

    /*
     * 列表行图标要在建列表之前备好：列表建完立刻要挂 image list，
     * 晚一步的话首批填充出来的行就没有图标。
     */
    cn_build_icon_list(sw);

    sw->local_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                      WS_CHILD | WS_VISIBLE | LVS_REPORT |
                                      LVS_SHOWSELALWAYS,
                                      0, 0, 0, 0, sw->hwnd, (HMENU)IDC_LOCAL_LIST,
                                      inst, NULL);
    sw->remote_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | LVS_REPORT |
                                       LVS_SHOWSELALWAYS,
                                       0, 0, 0, 0, sw->hwnd, (HMENU)IDC_REMOTE_LIST,
                                       inst, NULL);

    for (int i = 0; i < 2; i++) {
        HWND list = i ? sw->remote_list : sw->local_list;
        ListView_SetExtendedListViewStyle(list,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

        /*
         * 两个列表共用一份 image list。挂在 LVSIL_SMALL 上 ——
         * 详细信息视图用的就是小图标槽位，LVSIL_NORMAL 是给大图标
         * 视图的，挂那边在这儿不生效。
         */
        if (sw->imglist)
            ListView_SetImageList(list, sw->imglist, LVSIL_SMALL);

        /*
         * 列头文字必须走 W 版封装，否则「名称」会被当成 ANSI 读成乱码。
         * 后两列内容右对齐：数字与时间戳靠右读起来才是对齐的，左对齐会
         * 让「706 B」和「1.2 MB」的小数点全错位。
         */
        cn_list_insert_column(list, 0, 210, L"名称", LVCFMT_LEFT);
        cn_list_insert_column(list, 1, 80, L"大小", LVCFMT_RIGHT);
        cn_list_insert_column(list, 2, 130, L"修改时间", LVCFMT_RIGHT);

        /* 内容对齐定了，再把三个列头统一改成居中。 */
        for (int c = 0; c < 3; c++)
            cn_list_center_header(list, c);

        /*
         * 拖放接受面挂列表控件。主窗口句柄不再存 GWLP_USERDATA ——
         * 那个槽位归 comctl32 的 ListView 内部使用，写进去会踩掉它
         * 自己的状态，子类过程改走 GetParent 现取（见 cn_list_proc）。
         */
        DragAcceptFiles(list, TRUE);
        /* 同一个列表还要当 OLE 拖放目标，才能接收窗口内部互拖。 */
        cn_register_drop_target(list, i ? true : false);
        if (!cn_list_oldproc)
            cn_list_oldproc = (WNDPROC)SetWindowLongPtrW(
                list, GWLP_WNDPROC, (LONG_PTR)cn_list_proc);
        else
            SetWindowLongPtrW(list, GWLP_WNDPROC, (LONG_PTR)cn_list_proc);
    }

    sw->btn_up = CreateWindowW(L"BUTTON", L"↑ 返回",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 0, 0, sw->hwnd, (HMENU)IDC_BTN_UP,
                                inst, NULL);
    sw->btn_up_local = CreateWindowW(L"BUTTON", L"↑ 返回",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 0, 0, 0, sw->hwnd,
                                      (HMENU)IDC_BTN_UP_LOCAL, inst, NULL);
    /* cn: 本地栏「桌面」快捷键，放在「↑ 返回」左侧。 */
    sw->btn_desktop_local = CreateWindowW(L"BUTTON", L"桌面",
                                           WS_CHILD | WS_VISIBLE |
                                           BS_PUSHBUTTON,
                                           0, 0, 0, 0, sw->hwnd,
                                           (HMENU)IDC_BTN_DESKTOP, inst,
                                           NULL);
    /* 远端在左：上传方向从右往左，下载从左往右，箭头跟着方位走。 */
    sw->btn_to_remote = CreateWindowW(L"BUTTON", L"← 上传",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       0, 0, 0, 0, sw->hwnd,
                                       (HMENU)IDC_BTN_TO_REMOTE, inst, NULL);
    sw->btn_to_local = CreateWindowW(L"BUTTON", L"下载 →",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 0, 0, 0, sw->hwnd,
                                      (HMENU)IDC_BTN_TO_LOCAL, inst, NULL);
    /*
     * 暂停 / 续传是同一个按钮，文字随状态切换；终止是另一个。
     * 两者默认都灰着，只在传输期间启用（见 cn_set_xfer_state）。
     */
    sw->btn_pause = CreateWindowW(L"BUTTON", L"暂停",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   0, 0, 0, 0, sw->hwnd, (HMENU)IDC_BTN_PAUSE,
                                   inst, NULL);
    sw->btn_stop = CreateWindowW(L"BUTTON", L"终止",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 0, 0, sw->hwnd, (HMENU)IDC_BTN_STOP,
                                  inst, NULL);
    EnableWindow(sw->btn_pause, FALSE);
    EnableWindow(sw->btn_stop, FALSE);


    sw->progress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                    WS_CHILD | WS_VISIBLE,
                                    0, 0, 0, 0, sw->hwnd, (HMENU)IDC_PROGRESS,
                                    inst, NULL);
    SendMessageW(sw->progress, PBM_SETRANGE32, 0, 1000);

    sw->status = CreateWindowExW(0, L"STATIC", L"",
                                  WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                  0, 0, 0, 0, sw->hwnd, (HMENU)IDC_STATUS,
                                  inst, NULL);

    /*
     * 统一字体：取系统「消息框字体」，这是资源管理器、对话框、菜单
     * 共用的那一套（本机是 Segoe UI / 微软雅黑，9pt 上下）。
     *
     * 原来用的是 GetStockObject(DEFAULT_GUI_FONT)，看着「跟系统一致」
     * 其实不然 —— 这个 stock 对象在中文 Windows 上映射成 SimSun 点阵
     * 宋体，实测 lfHeight = -24（即 24 物理像素），比资源管理器大整整
     * 一倍，且点阵字体在高 DPI 下边缘发糊。直接后果是名称列被字体撑
     * 爆、「2026-09-16 14:07」一个时间戳就要 192px，一行显示不下几个
     * 字。主人拿资源管理器截图对比，一眼就看出字号差一号。
     *
     * SPI_GETNONCLIENTMETRICS 拿到的 lfMessageFont 才是系统真正在用的
     * UI 字体，跟随用户的「缩放与布局」设置走，不写死字号也就不会在
     * 别的机器上又偏大偏小。
     */
    {
        NONCLIENTMETRICSW ncm;
        HFONT f = NULL;

        memset(&ncm, 0, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm),
                                  &ncm, 0))
            f = CreateFontIndirectW(&ncm.lfMessageFont);
        if (f) {
            sw->font = f;
            sw->font_owned = true;
        } else {
            /* 兜底用 stock 对象，它归系统所有，销毁时不能删。 */
            sw->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            sw->font_owned = false;
        }
    }
    {
        HWND all[] = { sw->local_path, sw->remote_path, sw->local_list,
                       sw->remote_list, sw->btn_up,
                       sw->btn_up_local, sw->btn_desktop_local,
                       sw->btn_to_remote, sw->btn_to_local,
                       sw->btn_pause, sw->btn_stop,
                       sw->status };
        for (size_t i = 0; i < lenof(all); i++)
            SendMessageW(all[i], WM_SETFONT, (WPARAM)sw->font, TRUE);
    }
}

/* 前置声明：WM_DESTROY 在定义之前就要用（见本文件靠后的定义）。 */
static void cn_sftpwin_dispose(SftpWin *sw);

static LRESULT CALLBACK cn_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
                                   LPARAM lparam)
{
    SftpWin *sw = (SftpWin *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    /*
     * 网络事件必须最先处理，而且不能依赖 sw 是否已就绪：套接字在
     * 窗口刚建好、GWLP_USERDATA 还没写进去时就已经可能收到事件了。
     */
    if (msg == WM_NETEVENT || msg == WM_DONE_WITH_SOCKET) {
        winselgui_response(msg, wparam, lparam);
        return 0;
    }

    if (!sw) {
        if (msg == WM_CREATE)
            return 0;
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
      case WM_GETMINMAXINFO: {
          /*
           * 卡住最小尺寸：两栏各留 ~340 才装得下三列的正常宽度，加上
           * 中间列与边距。比这更窄的话名称列会被压到看不清，不如不让拖。
           */
          MINMAXINFO *mmi = (MINMAXINFO *)lparam;
          mmi->ptMinTrackSize.x = 900;
          mmi->ptMinTrackSize.y = 480;
          return 0;
      }

      case WM_SIZE:
        cn_layout(sw);
        return 0;

      /*
       * 点主窗口空白处（列表之外）取消两侧选中。
       *
       * 列表内部点空白由 cn_list_proc 处理，但本地目录一多，列表里根
       * 本没有空白可点 —— 用户只能往列表外面点。子控件的鼠标消息不会
       * 冒泡到父窗口，所以走到这里的必然是「落在窗口背景上」的点击
       * （路径框、按钮、列表都被子窗口吃掉了），可以直接清。
       *
       * 清两侧而不是只清一侧：用户点的是窗口背景，语义就是「我什么都
       * 不选了」，只清一侧反而让人猜不透。
       */
      case WM_LBUTTONDOWN:
          cn_clear_selection(sw);
          return 0;

      /*
       * 点标题栏也取消选中。
       *
       * 标题栏属于非客户区，点它来的是 WM_NCLBUTTONDOWN，上面那条
       * WM_LBUTTONDOWN 收不到。只对 HTCAPTION 生效：最小化 / 最大化 /
       * 关闭按钮以及边框拖拽的命中码一律放行，别把人家的窗口操作搅了。
       *
       * ⚠️ 自检里不许用 SendMessageW 发这条消息来验证：DefWindowProcW
       * 收到 HTCAPTION 会进入「拖动窗口」的模态循环，自检当场挂死
       * （实测卡在发完消息那一步再也不返回）。要验就调
       * cn_clear_selection，逻辑是同一个。
       */
      case WM_NCLBUTTONDOWN:
          if (wparam == HTCAPTION)
              cn_clear_selection(sw);
          return DefWindowProcW(hwnd, msg, wparam, lparam);

      case WM_COMMAND:
        switch (LOWORD(wparam)) {
          case IDC_BTN_UP:
            cn_go_up(sw, true);
            break;
          case IDC_BTN_UP_LOCAL:
            cn_go_up(sw, false);
            break;
          case IDC_BTN_DESKTOP: {
            /*
             * cn: 跳到当前用户的桌面目录。本地路径串是 CP_ACP 编码
             * （sftp_local_list / sftp_local_fopen 都按 ACP 转宽字符），
             * 这里转码必须同码页，中文用户名才不会乱码。
             */
            PWSTR wdesktop = NULL;
            char *desktop;
            if (FAILED(SHGetKnownFolderPath(&FOLDERID_Desktop, 0, NULL,
                                            &wdesktop)))
                break;
            desktop = dup_wc_to_mb(CP_ACP, wdesktop, "?");
            CoTaskMemFree(wdesktop);
            if (sw->localdir && strcmp(sw->localdir, desktop) == 0) {
                sfree(desktop);
                break;                  /* 已在桌面，不重复刷新 */
            }
            sfree(sw->localdir);
            sw->localdir = desktop;
            cn_refresh_local(sw);
            break;
          }

          case IDC_BTN_TO_REMOTE:
            cn_do_transfer(sw, true);
            break;
          case IDC_BTN_TO_LOCAL:
            cn_do_transfer(sw, false);
            break;
          case IDC_BTN_PAUSE:
            /*
             * 同一个按钮两种含义，看当前状态：
             *   RUNNING -> 请求暂停，当前文件停下并保留断点；
             *   PAUSED  -> 续传，拿存下的清单重走一遍，已传完的跳过。
             * 状态切换只走 cn_set_xfer_state，保证文字与可用性一致。
             */
            if (sw->xfer_state == CN_XFER_RUNNING) {
                if (sw->sx)
                    sftp_xfer_request_pause(sw->sx);
                cn_set_status(sw, "正在暂停…");
            } else if (sw->xfer_state == CN_XFER_PAUSED) {
                cn_resume_run(sw);
            }
            break;

          case IDC_BTN_STOP:
            /*
             * 终止：停掉当前文件，并让递归与后续条目都别走了。协议层
             * 会把半截文件删掉，符合「终止要清干净」的语义。
             */
            sw->cancel_requested = true;
            if (sw->sx)
                sftp_xfer_request_cancel(sw->sx);
            cn_set_status(sw, "正在终止…");
            break;
        }
        return 0;

      case WM_DROPFILES: {
        /*
         * 从资源管理器拖文件进来，一律上传到远端当前目录（拖到哪
         * 个面板都一样，WinSCP 也是这个语义）。目录整棵递归上传，
         * 走的是与按钮传输同一套 cn_transfer_entries，覆盖询问、
         * 取消、失败汇总全都一致。
         */
        HDROP hdrop = (HDROP)wparam;
        UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);
        char **names, **srcs;
        bool *dirs;
        UINT kept = 0;

        if (sw->busy || !sw->sx || n == 0) {
            DragFinish(hdrop);
            return 0;
        }

        names = snewn(n, char *);
        srcs = snewn(n, char *);
        dirs = snewn(n, bool);

        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(hdrop, i, NULL, 0);
            wchar_t *wpath = snewn(len + 1, wchar_t);
            wchar_t *base;
            DWORD attr;

            DragQueryFileW(hdrop, i, wpath, len + 1);
            attr = GetFileAttributesW(wpath);

            /* 本地源路径与界面同码页存，sftp_xfer_upload 内部用同
             * 一码页还原成宽字符打开；远端名是 UTF-8，纯文件名
             * 单独转一份。 */
            srcs[kept] = dup_wc_to_mb(DEFAULT_CODEPAGE, wpath, "?");
            base = wcsrchr(wpath, L'\\');
            if (!base)
                base = wcsrchr(wpath, L'/');
            base = base ? base + 1 : wpath;
            names[kept] = dup_wc_to_mb(CP_UTF8, base, "?");
            dirs[kept] = (attr != INVALID_FILE_ATTRIBUTES &&
                          (attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
            kept++;
            sfree(wpath);
        }
        DragFinish(hdrop);

        if (kept) {
            char *msg = dupprintf("已拖入 %u 项，开始上传…", (unsigned)kept);
            cn_set_status(sw, msg);
            sfree(msg);
            cn_transfer_entries(sw, names, srcs, dirs, kept, true);
        } else {
            sfree(names);
            sfree(srcs);
            sfree(dirs);
        }
        return 0;
      }

      case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lparam;
        if (nh->code == NM_DBLCLK) {
            LPNMITEMACTIVATE ia = (LPNMITEMACTIVATE)lparam;
            if (ia->iItem >= 0)
                cn_enter_dir(sw, nh->hwndFrom == sw->remote_list, ia->iItem);
            return 0;
        }
        /*
         * 选中状态变了就重算两个传输按钮。LVN_ITEMCHANGED 在选中和
         * 取消选中时都会来，且带上被改的那一项的状态位；只要选中位
         * 发生了变化就值得重算（选中项从 A 换到 B 也是两次变化）。
         *
         * 不细分 old/new 的差别，直接按「当前有没有选中」重算，逻辑
         * 更省心 —— 单选列表下这样算出来的结果永远正确。
         */
        if (nh->code == LVN_ITEMCHANGED &&
            (nh->hwndFrom == sw->local_list ||
             nh->hwndFrom == sw->remote_list)) {
            LPNMLISTVIEW nv = (LPNMLISTVIEW)lparam;
            if ((nv->uChanged & LVIF_STATE) &&
                ((nv->uOldState ^ nv->uNewState) & LVIS_SELECTED))
                cn_update_transfer_buttons(sw);
            return 0;
        }
        break;
      }

      case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

      case WM_DESTROY:
        /* 无条件回收：the_sftpwin 已置位或「打开失败」的半成品都要清。 */
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        cn_sftpwin_dispose(sw);
        /* 只关这个窗口，不动整个进程 */
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* ----------------------------------------------------------------------
 * 打开窗口。
 */

/* ----------------------------------------------------------------------
 * 打开过程中的加载动画。
 *
 * 点「文件传输」到窗口就绪之间隔着 SSH 连接 + 认证 + SFTP 握手，同步
 * 等待全程走 sftp_pump 的消息泵，所以动画用 SetTimer 驱动就能转起来。
 * 两处动画：居中弹窗（转圈 + 阶段文字）与菜单项旁的小转圈。
 */

/* 8 段式转圈：8 条径向短线，亮度沿圆周递减，frame 递增即旋转。 */
static void cn_spinner_draw(HDC hdc, int frame, int cx, int cy, int r)
{
    for (int i = 0; i < 8; i++) {
        double ang = ((frame * 45 + i * 45) % 360) * 3.14159265358979 / 180.0;
        double ca = cos(ang), sa = sin(ang);
        int x1 = cx + (int)(ca * r * 0.45);
        int y1 = cy + (int)(sa * r * 0.45);
        int x2 = cx + (int)(ca * r);
        int y2 = cy + (int)(sa * r);
        /* 头部最亮（接近黑），尾部渐隐到浅灰。 */
        int v = 60 + i * 24;
        HPEN pen = CreatePen(PS_SOLID, cn_dpi(2), RGB(v, v, v));
        HGDIOBJ old = SelectObject(hdc, pen);

        MoveToEx(hdc, x1, y1, NULL);
        LineTo(hdc, x2, y2);
        SelectObject(hdc, old);
        DeleteObject(pen);
    }
}

#define CN_LOADING_DONE_NONE  0   /* 还在转圈 */
#define CN_LOADING_DONE_OK    1   /* 成功：绿色勾 */
#define CN_LOADING_DONE_FAIL  2   /* 失败：红色叉 */

/*
 * cn: 加载动画状态收进 SftpWin（原为文件级全局 the_loading）。生命周期
 * 与窗口结构一致：打开失败就地回收 sw 时动画状态一并消失，不给
 * 多窗口扩展留共享态隐患。menuspin 窗口经 GWLP_USERDATA 反查 sw。
 */

/* 画结果符号：done 状态下替代转圈，绿色勾或红色叉。 */
static void cn_result_mark_draw(HDC hdc, const RECT *rc, const SftpWin *sw)
{
    COLORREF c = sw->loading.done == CN_LOADING_DONE_OK
        ? RGB(16, 124, 16)      /* Windows 主题绿 */
        : RGB(196, 43, 28);     /* Windows 主题红 */
    HPEN pen = CreatePen(PS_SOLID, cn_dpi(3), c);
    HGDIOBJ old = SelectObject(hdc, pen);
    int w = rc->right - rc->left, h = rc->bottom - rc->top;

    SetBkMode(hdc, TRANSPARENT);
    if (sw->loading.done == CN_LOADING_DONE_OK) {
        MoveToEx(hdc, rc->left + MulDiv(w, 22, 100),
                 rc->top + MulDiv(h, 55, 100), NULL);
        LineTo(hdc, rc->left + MulDiv(w, 42, 100),
               rc->top + MulDiv(h, 74, 100));
        LineTo(hdc, rc->left + MulDiv(w, 80, 100),
               rc->top + MulDiv(h, 28, 100));
    } else {
        MoveToEx(hdc, rc->left + MulDiv(w, 24, 100),
                 rc->top + MulDiv(h, 24, 100), NULL);
        LineTo(hdc, rc->left + MulDiv(w, 76, 100),
               rc->top + MulDiv(h, 76, 100));
        MoveToEx(hdc, rc->left + MulDiv(w, 76, 100),
                 rc->top + MulDiv(h, 24, 100), NULL);
        LineTo(hdc, rc->left + MulDiv(w, 24, 100),
               rc->top + MulDiv(h, 76, 100));
    }
    SelectObject(hdc, old);
    DeleteObject(pen);
}

/*
 * 锚点旁小转圈的过程：自己的 timer 推帧；转圈结束后定格为勾或叉。
 */
static LRESULT CALLBACK cn_menuspin_proc(HWND hwnd, UINT msg,
                                         WPARAM wparam, LPARAM lparam)
{
    /*
     * cn: 动画状态属于创建它的那个 sw（GWLP_USERDATA 反查）。
     * WM_CREATE 阶段 USERDATA 尚未写入，此时不该有任何绘制/定时器
     * 消息需要处理，直接走默认过程即可。
     */
    SftpWin *sw = (SftpWin *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
      case WM_TIMER:
        sw->loading.frame++;
        /* 每帧擦底重画：18px 小窗闪不出来，残影必须干净。 */
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;

        GetClientRect(hwnd, &rc);
        if (sw->loading.done != CN_LOADING_DONE_NONE) {
            cn_result_mark_draw(hdc, &rc, sw);
        } else {
            cn_spinner_draw(hdc, sw->loading.frame,
                            rc.right / 2, rc.bottom / 2,
                            (rc.bottom < rc.right ? rc.bottom : rc.right) / 2
                            - cn_dpi(1));
        }
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_NCHITTEST:
        /* 不抢鼠标：菜单栏照常响应点击。 */
        return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* 找 parent 菜单栏里「文件传输」项的屏幕矩形；找不到返回 false。 */
static bool cn_find_xfer_menu_item(HWND parent, RECT *out)
{
    HMENU menu = GetMenu(parent);
    int n;
    wchar_t buf[64];

    if (!menu)
        return false;
    n = GetMenuItemCount(menu);
    for (int i = 0; i < n; i++) {
        if (!GetMenuStringW(menu, i, buf, (int)lenof(buf), MF_BYPOSITION))
            continue;
        if (wcsstr(buf, L"文件传输")) {
            return GetMenuItemRect(parent, menu, i, out) != 0;
        }
    }
    return false;
}

/*
 * 配置框路径的小转圈锚点：找「PuTTY 配置」对话框里文字含「文件传输」
 * 的按钮。就地打开时配置框还开着，一定找得到；独立启动（无配置框）
 * 返回 false，小转圈自动省略。
 */
static BOOL CALLBACK cn_find_xfer_button_child(HWND hwnd, LPARAM lp)
{
    wchar_t text[64];

    if (GetWindowTextW(hwnd, text, (int)lenof(text)) &&
        wcsstr(text, L"文件传输")) {
        GetWindowRect(hwnd, (RECT *)lp);
        return FALSE;                   /* 找到了，停 */
    }
    return TRUE;
}

static bool cn_find_config_xfer_button(RECT *out)
{
    HWND dlg = NULL;
    wchar_t title[128];

    /*
     * 枚举本进程可见顶层窗口，标题含 PuTTY 的就是配置框。不按类名
     * 卡：配置框是自绘窗口（类名 PuTTYConfigBox），不是标准 #32770，
     * 按标准对话框类找永远扑空 —— 第一版就栽在这。
     */
    for (HWND w = GetTopWindow(NULL); w; w = GetNextWindow(w, GW_HWNDNEXT)) {
        DWORD pid = 0;

        if (!IsWindowVisible(w))
            continue;
        GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId())
            continue;
        if (GetWindowTextW(w, title, (int)lenof(title)) &&
            wcsstr(title, L"PuTTY")) {
            dlg = w;
            break;
        }
    }
    if (!dlg)
        return false;
    return EnumChildWindows(dlg, cn_find_xfer_button_child, (LPARAM)out)
        ? false : true;   /* 回调返回 FALSE 表示已找到并填好 out */
}

/* 建锚点旁小转圈；找不到锚点（独立启动等）就不显示。 */
static void cn_loading_create(HWND parent, SftpWin *sw)
{
    static const wchar_t spinclass[] = L"PuTTYSftpMenuSpin";
    HINSTANCE inst = GetModuleHandle(NULL);
    WNDCLASSW wc;

    memset(&sw->loading, 0, sizeof(sw->loading));

    /*
     * 锚点旁小转圈。菜单栏场景贴项右端（左边是相邻菜单项，放左侧会
     * 盖字）；配置框按钮场景放按钮左侧框外 —— 压进框内会遮住「(F)」
     * 字样，主人要求挪出去。
     */
    {
        RECT mr;
        bool on_menu = parent && cn_find_xfer_menu_item(parent, &mr);
        bool have = on_menu;

        if (!have)
            have = cn_find_config_xfer_button(&mr);
        if (have) {
            int ssize = cn_dpi(18);
            int sx = on_menu
                ? mr.right - ssize - cn_dpi(3)
                : mr.left - ssize - cn_dpi(4);
            int sy = mr.top + ((mr.bottom - mr.top) - ssize) / 2;

            if (!GetClassInfoW(inst, spinclass, &wc)) {
                memset(&wc, 0, sizeof(wc));
                wc.lpfnWndProc = cn_menuspin_proc;
                wc.hInstance = inst;
                wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
                wc.lpszClassName = spinclass;
                RegisterClassW(&wc);
            }
            /*
             * cn: TOPMOST 只许在「打开过程」这一小段时间存在。
             * 打开函数的所有返回点（成功/失败/创建失败）都必须先
             * cn_loading_destroy() 再返回，绝不能让这个 18px 置顶窗
             * 残留——2026-11-06 事故里它就是因为失败分支被模态框
             * 堵住而一直挂在全屏最前。
             */
            sw->loading.menuspin = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                spinclass, NULL, WS_POPUP,
                sx, sy, ssize, ssize, NULL, NULL, inst, NULL);
            if (sw->loading.menuspin) {
                /*
                 * cn: menuspin 的回调要取动画状态，而它属于这个 sw；
                 * 存进 GWLP_USERDATA 供 cn_menuspin_proc 反查。
                 */
                SetWindowLongPtrW(sw->loading.menuspin, GWLP_USERDATA,
                                  (LONG_PTR)sw);
                SetTimer(sw->loading.menuspin, 1, 33, NULL);
                ShowWindow(sw->loading.menuspin, SW_SHOWNOACTIVATE);
                UpdateWindow(sw->loading.menuspin);
            }
        }
    }
}

/* 转圈定格为结果符号：成功绿勾，失败红叉。 */
static void cn_loading_set_done(SftpWin *sw, bool ok)
{
    sw->loading.done = ok ? CN_LOADING_DONE_OK : CN_LOADING_DONE_FAIL;
    if (sw->loading.menuspin) {
        KillTimer(sw->loading.menuspin, 1);
        InvalidateRect(sw->loading.menuspin, NULL, TRUE);
    }
}

/*
 * 结果符号停留：让用户看清勾/叉再进行下一步。同步等待但持续泵消息
 * （配置框、浮层都还活着），不能 Sleep 把界面冻住。
 */
static void cn_loading_dwell(SftpWin *sw, int msec)
{
    DWORD start = GetTickCount();
    MSG msg;

    while (sw && sw->loading.menuspin &&
           GetTickCount() - start < (DWORD)msec) {
        DWORD wait = (DWORD)msec - (GetTickCount() - start);
        DWORD r = MsgWaitForMultipleObjects(0, NULL, FALSE, wait,
                                            QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

static void cn_loading_destroy(SftpWin *sw)
{
    if (sw->loading.menuspin) {
        KillTimer(sw->loading.menuspin, 1);
        DestroyWindow(sw->loading.menuspin);
    }
    memset(&sw->loading, 0, sizeof(sw->loading));
}

/*
 * 释放传输窗口的全部自持资源。
 *
 * 两个调用点：正常关窗（WM_DESTROY，条件 the_sftpwin == sw），以及
 * 「打开失败」的独立出口 —— 失败时窗口从未进入消息循环、the_sftpwin
 * 从未置位，不走这条就泄漏 sx / image list / 字体 / 目录串。
 */
static void cn_sftpwin_dispose(SftpWin *sw)
{
    if (the_sftpwin == sw)
        the_sftpwin = NULL;
    /* 提示条先收掉：它还拿着 sw 指针，晚于 sfree(sw) 就是悬空。 */
    cn_toast_destroy(sw);
    if (sw->sx) {
        sftp_xfer_close(sw->sx);
        sw->sx = NULL;
    }
    /*
     * image list 是我们建的，得自己销毁。列表控件只借用，
     * 不会替我们释放；漏掉就是每次开关窗口泄一份 GDI 对象。
     * 顺序上先于 sfree(sw) —— 句柄在 sw 里存着。
     */
    if (sw->imglist) {
        ImageList_Destroy(sw->imglist);
        sw->imglist = NULL;
    }
    /*
     * 字体同理由我们建的（CreateFontIndirectW），也得自己删。
     * 不能无条件删：取不到系统消息字体时会退化成 stock 对象
     * DEFAULT_GUI_FONT，那个归系统所有，删了是踩别人家东西。
     * 所以只删不是 stock 的那些 —— 这里用 store 标记区分：
     * sw->font_owned 为 true 才释放。
     */
    if (sw->font && sw->font_owned)
        DeleteObject(sw->font);
    sw->font = NULL;
    sfree(sw->localdir);
    sfree(sw->remotedir);
    sfree(sw);
}

bool sftp_window_is_open(void)
{
    return the_sftpwin != NULL;
}

/*
 * cn: 打开双栏传输窗口。
 *
 * 返回 NULL 表示成功；否则返回一份调用方须 sfree 的错误文案。
 *
 * ⚠️ 本函数及其调用链（含 sftp_xfer_open / sftp_pump）内禁止任何自带
 * 消息循环的同步模态调用（MessageBox / DialogBox / DoDragDrop 等）。
 * 早先失败分支拿「尚未 ShowWindow 的传输主窗」当 owner 弹 MessageBox，
 * 弹框不会被置前、却把模态禁用状态带进那个隐藏窗口，同时它的嵌套
 * 循环独占 GUI 线程，外层 ShinyDialogBox 的 GetMessage 循环被彻底堵死
 * —— 表现是配置框所有退出方式全部失效（2026-11-06 实测事故）。
 * 现在错误一律上交给调用方，由「本来就在跑自己消息循环的那个窗口」
 * 弹出，MessageBox 结束后事件处理器正常返回，外层循环得以继续。
 */
char *sftp_window_open(Conf *conf, HWND parent)
{
    SftpWin *sw;
    WNDCLASSW wc;
    HINSTANCE inst = GetModuleHandle(NULL);
    char *errmsg = NULL;
    static const wchar_t classname[] = CN_WINDOW_CLASS;
    /*
     * 防重入挡板：配置框就地打开时消息循环仍在转（sftp_pump 泵消息），
     * 连接期间用户可以再点一次「文件传输」，不加挡板会叠出两条连接。
     */
    static bool opening = false;

    if (the_sftpwin) {
        SetForegroundWindow(the_sftpwin->hwnd);
        return NULL;                    /* 已经开着，不算错误 */
    }
    if (opening)
        return dupstr("传输窗口正在打开中。");
    opening = true;

    /* 建连接可能要几秒，先给个光标提示，别让人以为没反应 */
    SetCursor(LoadCursor(NULL, IDC_APPSTARTING));

    /*
     * cn: sw 提前分配——加载动画状态收在 sw->loading 里，create 之前
     * 必须有结构体在手（原全局版无此约束，收编后顺序不能反）。
     */
    sw = snew(SftpWin);
    memset(sw, 0, sizeof(*sw));

    /* 加载动画：弹窗 + 菜单项旁小转圈，连接/握手全程可见。 */
    cn_loading_create(parent, sw);

    if (!GetClassInfoW(inst, classname, &wc)) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = cn_wndproc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        /*
         * 窗口类图标：不给的话标题栏与任务栏都挂系统默认图标，
         * 跟「文件传输」这个窗口对不上号。
         */
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_SFTPXFER));
        wc.lpszClassName = classname;
        RegisterClassW(&wc);
    }

    /*
     * 默认尺寸铺满工作区、贴右上角。
     *
     * 这段专治 DPI 口径不统一：进程没有 DPI 感知声明，系统会做虚拟化。
     * 实测 SPI_GETWORKAREA 报物理值 2880x1720，而 CreateWindowExW 收
     * 1421 只落地成物理 711 —— 两边差一倍，窗口又小又贴左上角。
     *
     * 所以建窗时给个占位尺寸，建好后按工作区物理坐标重摆一次：留 20px
     * 边距，右边缘与顶边贴住工作区。不猜缩放比例，任何 DPI 下都成立。
     */
    {
        RECT wa;

        /*
         * OLE 拖放必须先初始化。按单线程套间初始化即可：整个窗口都在
         * 主线程上跑，DoDragDrop 与 RegisterDragDrop 都要求这一步先做，
         * 否则 E_OUTOFMEMORY / DRAGDROP_E_NOTREGISTERED。
         */
        OleInitialize(NULL);

        sw->hwnd = CreateWindowExW(
            0, classname, L"文件传输",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1421, 931,
            parent, NULL, inst, NULL);

        /*
         * 小图标（标题栏左上角那个）得单独设：WNDCLASSW 只有 hIcon，
         * 没有 WNDCLASSEX 才有的 hIconSm。不设的话标题栏会显示一个
         * 被缩放得很难看的默认图标。
         */
        if (sw->hwnd) {
            HICON ico = LoadIconW(inst, MAKEINTRESOURCEW(IDI_SFTPXFER));
            if (ico) {
                SendMessageW(sw->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)ico);
                SendMessageW(sw->hwnd, WM_SETICON, ICON_BIG, (LPARAM)ico);
            }
        }

        if (sw->hwnd && SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            int availw = wa.right - wa.left;
            int availh = wa.bottom - wa.top;
            int physw, physh;
            HDC sdc = GetDC(NULL);
            int dpi = sdc ? GetDeviceCaps(sdc, LOGPIXELSX) : 96;

            if (sdc)
                ReleaseDC(NULL, sdc);
            if (dpi <= 0)
                dpi = 96;

            /*
             * 默认尺寸按逻辑像素给：1000x475。本进程没有 DPI 感知声明，
             * CreateWindowExW 与 SetWindowPos 收到的都是物理像素，所以
             * 这里要按桌面 DPI 折算一次 —— 200% 缩放下 1000 落地成物理
             * 2000，才是「逻辑 1000」应有的观感。
             *
             * 宽度为什么是 1000 而不是原来那个 710：三列里有两列的宽度
             * 由内容决定、跟窗口无关 —— 「YYYY-MM-DD HH:MM」这段时间戳
             * 在 100% 缩放下就要 192 物理像素，加 pad 后 206；大小列的
             * 内容下限 76、实测约 96 加 pad 后 110。两列合计 316，再给
             * 名称列留下 80 的最小可读宽度，一栏至少要 396 可用宽。
             *
             * 反推回去：可用宽 = 列表窗口宽 − 边框 − 竖滚动条 ≈ list_w
             * − 21，故 list_w 需 417；而 list_w = (客户区 − 边距 − 中间
             * 按钮列 − 间距)/2 = (客户区 − 116)/2，故客户区需 950，加
             * 上 16 边框就是窗口 966。原来 710 在 100% 缩放下客户区只有
             * 694，list_w 只剩 289，两列内容 316 根本塞不下，名称列被压
             * 到 80 的下限、显示成「s...」。710 的问题不是它「错」，而是
             * 它按 200% 屏的观感定的 —— 那种屏上物理宽 1420，绰绰有余。
             *
             * 取 1000 是给 966 那笔账留一点余量（长文件名、滚动条主题
             * 差异），同时在 1920 宽的屏上只占一半，不至于顶满。
             */
            physw = MulDiv(1000, dpi, 96);
            physh = MulDiv(475, dpi, 96);

            /* 屏幕放不下就收到工作区里，别让标题栏被顶出屏幕。 */
            if (physw > availw)
                physw = availw;
            if (physh > availh)
                physh = availh;

            SetWindowPos(sw->hwnd, NULL, wa.right - physw, wa.top, physw,
                         physh, SWP_NOZORDER | SWP_NOACTIVATE);

            /*
             * 贴边修正。思路照抄终端窗口（window.c 的 winselgui）：窗口
             * 矩形里含非客户区边框，直接按 GetWindowRect 贴边会在屏幕
             * 边缘留一条框宽的空隙。终端贴左边时把窗口左移到
             * -left_border，让客户区从 x=0 开始；这里贴右边，对称处理。
             */
            {
                POINT origin;
                RECT wr;
                int right_border;

                GetWindowRect(sw->hwnd, &wr);
                origin.x = 0;
                origin.y = 0;
                ClientToScreen(sw->hwnd, &origin);

                /*
                 * 客户区右缘 = origin.x + 客户区宽；窗口矩形右缘是
                 * wr.right。两者之差就是右侧边框宽。
                 */
                {
                    RECT cr;
                    GetClientRect(sw->hwnd, &cr);
                    right_border = wr.right - (origin.x + cr.right);
                }

                SetWindowPos(sw->hwnd, NULL,
                             wa.right - physw + right_border, wa.top, physw,
                             physh, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    if (!sw->hwnd) {
        sfree(sw);
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        cn_loading_destroy(sw);
        opening = false;
        return dupstr("无法创建传输窗口。");
    }
    SetWindowLongPtrW(sw->hwnd, GWLP_USERDATA, (LONG_PTR)sw);

    cn_init_controls(sw);
    cn_layout(sw);

    sw->localdir = dupstr("C:\\");
    cn_refresh_local(sw);

    /* 连远端 */
    cn_set_status(sw, "正在连接远端…");
    UpdateWindow(sw->hwnd);

    /*
     * cn: 失败文案只在此处汇总，绝不在这里弹框。errmsg 是 sftp_xfer_open
     * 用 dupstr 分配的，所有权直接转移到 failmsg；握手失败时 errmsg 为
     * NULL，改从 fxp_error() 取 —— 它返回的是静态缓冲（sftp.c:247），
     * 必须 dupstr 一份才能跨 DestroyWindow 存活，且可能为 NULL 需兜底。
     * failmsg 最终交给调用方 sfree，本函数不再释放。
     */
    char *failmsg = NULL;

    sw->sx = sftp_xfer_open(conf, sw->hwnd, &errmsg);
    if (!sw->sx) {
        cn_set_status(sw, errmsg ? errmsg : "连接远端失败。");
            cn_loading_set_done(sw, false);
        cn_loading_dwell(sw, 500);
        failmsg = errmsg ? errmsg : dupstr("连接远端失败。");
        errmsg = NULL;                  /* 所有权已转移，别再 sfree */
    } else if (!sftp_xfer_init(sw->sx)) {
        const char *fxerr = fxp_error();
        cn_set_status(sw, "SFTP 握手失败。");
            cn_loading_set_done(sw, false);
        cn_loading_dwell(sw, 500);
        failmsg = dupstr(fxerr ? fxerr : "SFTP 握手失败。");
    } else {
        sw->remotedir = sftp_xfer_pwd(sw->sx);
        if (!sw->remotedir)
            sw->remotedir = dupstr("/");
        cn_refresh_remote(sw);
        cn_set_status(sw, "就绪。选文件后按中间箭头传输。");
        /* 绿勾停一拍再显示传输窗口；配置框紧随其后关闭（config.c）。 */
        cn_loading_set_done(sw, true);
        cn_loading_dwell(sw, 500);
    }

    /*
     * 失败出口：传输窗不显示、the_sftpwin 不置位（is_open 返回 false，
     * 配置框得以保留供重试），整个 sw 就地回收。
     * cn_loading_destroy 必须在返回前完成，绝不能留下 TOPMOST 的 18px
     * spinner 挂在全屏最前（2026-11-06 事故的可见残留之一）。
     */
    cn_loading_destroy(sw);
    SetCursor(LoadCursor(NULL, IDC_ARROW));
    if (sw->remotedir == NULL) {
        /*
         * 连接或握手失败：remotedir 只在成功分支设置。传输窗不显示、
         * the_sftpwin 不置位（is_open 返回 false，配置框得以保留供
         * 重试），整个 sw 就地回收，重入挡板复位。
         */
        DestroyWindow(sw->hwnd);        /* WM_DESTROY → cn_sftpwin_dispose */
        opening = false;
        return failmsg ? failmsg : dupstr("打开传输窗口失败。");
    }

    sfree(failmsg);                     /* 成功分支不该有文案，保险起见 */
    cn_set_busy(sw, false);
    ShowWindow(sw->hwnd, SW_SHOW);
    SetForegroundWindow(sw->hwnd);
    the_sftpwin = sw;
    opening = false;
    return NULL;
}

/* ----------------------------------------------------------------------
 * 独立启动：没有主消息循环时自己跑一个。
 *
 * 从配置框点「文件传输」进来时，putty 还没进 WinMain 的消息循环
 * （那是给终端会话准备的），窗口建好后没人抽消息，界面会一动不动。
 * 这里补一个循环，窗口关掉就返回，调用方随即退出进程。
 */

void sftp_window_detached(Conf *conf)
{
    MSG msg;
    char *err = sftp_window_open(conf, NULL);

    if (err) {
        /*
         * 独立启动没有配置框，也不该在这里弹模态框（同样会独占唯一
         * 的 GUI 线程）。把错误文案落到日志，调用方随即 cleanup_exit。
         */
        char *outpath = sftp_selftest_outpath("sftp-open-fail.txt");
        FILE *fp = fopen(outpath, "w");
        sfree(outpath);
        if (fp) {
            fprintf(fp, "打开传输窗口失败：%s\n", err);
            fclose(fp);
        }
        sfree(err);
        return;
    }

    if (!the_sftpwin)
        return;

    /*
     * cn: 人工看图用 —— CN_TOAST_DEMO 设成文案内容，窗口一开出来就弹一条
     * 提示条，方便截图核对外观（圆角、字体、透明度、贴底位置）。
     *
     * 走的是 cn_toast_show 本身，与传输收尾时同一个函数，所以截到的就是
     * 线上真实效果。设 CN_TOAST_DEMO=1 用默认文案。
     */
    {
        const char *demo = getenv("CN_TOAST_DEMO");
        if (demo && *demo) {
            const char *text = strcmp(demo, "1")
                ? demo : "已上传 3 项到远端";
            cn_toast_show(the_sftpwin, text);
        }
    }

    while (the_sftpwin && GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_QUIT)
            break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* ----------------------------------------------------------------------
 * 界面自检：putty -sftp-win
 *
 * 无人工介入地验证一遍窗口骨架：控件都建起来了、位置在客户区内、
 * 左右两栏不重叠、中间按钮不压住列表。结果写文件（GUI 程序没有
 * stdout）。真正的外观与手感仍要人看，这里只兜住几何与存在性。
 */

static void sw_report(FILE *fp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fflush(fp);
}

static RECT sw_child_rect(HWND child)
{
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(HWND_DESKTOP, GetParent(child), (POINT *)&r, 2);
    return r;
}

static bool sw_rect_valid(HWND child, RECT *client)
{
    RECT r;
    if (!child)
        return false;
    r = sw_child_rect(child);
    if (r.right <= r.left || r.bottom <= r.top)
        return false;
    if (r.left < 0 || r.top < 0 ||
        r.right > client->right || r.bottom > client->bottom)
        return false;
    return true;
}

static bool sw_rects_overlap(RECT a, RECT b)
{
    return !(a.right <= b.left || b.right <= a.left ||
             a.bottom <= b.top || b.bottom <= a.top);
}

/* ----------------------------------------------------------------------
 * 打开失败自检：-sftp-open-fail
 *
 * 故意让 sftp_xfer_open 连不上（调用方通过命令行给一个不可达端口），
 * 把 2026-11-06「配置框被隐藏模态框堵死」事故的三条可观测特征变成
 * 机器断言：
 *   ① 底层返回非 NULL 错误文案（错误确实上交给调用方）；
 *   ② 本进程没弹过任何 sftpwin.c 的模态框；
 *   ③ 进程里不残留 PuTTYSftpXferWindow / PuTTYSftpMenuSpin / #32770
 *      窗口（spinner 与隐藏传输主窗正是事故现场的残留物）。
 * 结果写 sftp-open-fail-selftest.txt，GUI 程序没有 stdout。
 */

static int cn_fail_residual_nxfer = 0;
static int cn_fail_residual_nspin = 0;
static int cn_fail_residual_ndlg = 0;

static BOOL CALLBACK cn_fail_enum_windows(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    wchar_t cls[128];

    (void)lp;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
        return TRUE;
    if (!GetClassNameW(hwnd, cls, lenof(cls)))
        return TRUE;
    if (!wcscmp(cls, L"PuTTYSftpXferWindow"))
        cn_fail_residual_nxfer++;
    else if (!wcscmp(cls, L"PuTTYSftpMenuSpin"))
        cn_fail_residual_nspin++;
    else if (!wcscmp(cls, L"#32770"))
        cn_fail_residual_ndlg++;
    return TRUE;
}

void sftp_selftest_open_fail(Conf *conf)
{
    char *outpath = sftp_selftest_outpath("sftp-open-fail-selftest.txt");
    FILE *fp = fopen(outpath, "w");
    sfree(outpath);
    int pass = 0, fail = 0;
    char *err;

    if (!fp)
        return;

#define FAIL_CHECK(cond, desc) do {                                       \
        if (cond) { sw_report(fp, "  通过  %s\n", desc); pass++; }        \
        else { sw_report(fp, "  失败  %s\n", desc); fail++; }             \
    } while (0)

    sw_report(fp, "[打开失败自检]\n");
    cn_selftest_modal_count = 0;
    cn_fail_residual_nxfer = cn_fail_residual_nspin = cn_fail_residual_ndlg = 0;

    err = sftp_window_open(conf, NULL);

    FAIL_CHECK(err != NULL, "失败时返回非 NULL 错误文案");
    if (err) {
        sw_report(fp, "  错误文案：%s\n", err);
        sfree(err);
    }

    EnumWindows(cn_fail_enum_windows, 0);

    FAIL_CHECK(cn_selftest_modal_count == 0,
               "打开路径未弹出任何模态框");
    FAIL_CHECK(cn_fail_residual_nxfer == 0,
               "无残留 PuTTYSftpXferWindow 窗口");
    FAIL_CHECK(cn_fail_residual_nspin == 0,
               "无残留 PuTTYSftpMenuSpin 窗口");
    FAIL_CHECK(cn_fail_residual_ndlg == 0,
               "无残留 #32770 对话框");
    FAIL_CHECK(!sftp_window_is_open(), "传输窗口未进入已打开状态");

    sw_report(fp, "\n汇总：通过 %d，失败 %d —— %s\n",
              pass, fail, fail ? "FAIL" : "PASS");
    fclose(fp);
#undef FAIL_CHECK
}

void sftp_selftest_window(Conf *conf)
{
    char *outpath = sftp_selftest_outpath("sftp-window-selftest.txt");
    FILE *fp = fopen(outpath, "w");
    sfree(outpath);
    RECT client;
    int pass = 0, fail = 0;

    if (!fp)
        return;

    /* 自检要能看见握手细节，否则卡住时两眼一抹黑。 */
    sftp_selftest_enable_log("sftp-log.txt");

#define SW_CHECK(cond, desc) do {                                   \
        if (cond) { sw_report(fp, "  通过  %s\n", desc); pass++; }   \
        else { sw_report(fp, "  失败  %s\n", desc); fail++; }        \
    } while (0)

    sw_report(fp, "[界面自检]\n");

    {
        char *err = sftp_window_open(conf, NULL);
        if (err) {
            sw_report(fp, "  失败  打开传输窗口：%s\n", err);
            sfree(err);
            fclose(fp);
            return;
        }
    }

    if (!the_sftpwin) {
        sw_report(fp, "  失败  窗口未创建\n");
        fclose(fp);
        return;
    }

    {
        SftpWin *sw = the_sftpwin;

        GetClientRect(sw->hwnd, &client);
        sw_report(fp, "  客户区 %ld x %ld\n", client.right, client.bottom);

        sw_report(fp, "\n控件存在性：\n");
        SW_CHECK(sw->local_list != NULL, "本地列表");
        SW_CHECK(sw->remote_list != NULL, "远端列表");
        SW_CHECK(sw->local_path != NULL, "本地路径框");
        SW_CHECK(sw->remote_path != NULL, "远端路径框");
        SW_CHECK(sw->btn_to_remote != NULL, "上传按钮");
        SW_CHECK(sw->btn_to_local != NULL, "下载按钮");
        SW_CHECK(sw->btn_up != NULL, "远端返回按钮");
        SW_CHECK(sw->btn_up_local != NULL, "本地返回按钮");
        SW_CHECK(sw->btn_desktop_local != NULL, "本地桌面按钮");
        SW_CHECK(sw->btn_pause != NULL, "暂停/续传按钮");
        SW_CHECK(sw->btn_stop != NULL, "终止按钮");
        SW_CHECK(sw->progress != NULL, "进度条");
        SW_CHECK(sw->status != NULL, "状态栏");

        /*
         * 暂停 / 终止按钮的初始状态：必须存在、可见、但灰着。
         *
         * 这两条只在传输期间有意义，闲着的时候能点就是错的（点了没反应，
         * 用户以为程序坏了）。文字也得是「暂停」——那是运行前的默认态。
         */
        {
            wchar_t txt[32];
            GetWindowTextW(sw->btn_pause, txt, lenof(txt));
            SW_CHECK(!wcscmp(txt, L"暂停"), "暂停按钮初始文字为「暂停」");
            GetWindowTextW(sw->btn_stop, txt, lenof(txt));
            SW_CHECK(!wcscmp(txt, L"终止"), "终止按钮初始文字为「终止」");
            SW_CHECK(!IsWindowEnabled(sw->btn_pause), "暂停按钮初始为禁用");
            SW_CHECK(!IsWindowEnabled(sw->btn_stop), "终止按钮初始为禁用");
        }

        /*
         * 状态机切换：RUNNING 时「暂停」可点，PAUSED 时文字变「续传」，
         * 回到 IDLE 又灰下去。这三个状态是按钮行为的唯一依据，错一个
         * 就会出现「已暂停但按钮还能点暂停」这种自相矛盾的界面。
         */
        {
            wchar_t txt[32];
            cn_set_xfer_state(sw, CN_XFER_RUNNING);
            GetWindowTextW(sw->btn_pause, txt, lenof(txt));
            SW_CHECK(!wcscmp(txt, L"暂停") && IsWindowEnabled(sw->btn_pause) &&
                     IsWindowEnabled(sw->btn_stop),
                     "运行态：暂停/终止可点且文字为「暂停」");

            cn_set_xfer_state(sw, CN_XFER_PAUSED);
            GetWindowTextW(sw->btn_pause, txt, lenof(txt));
            SW_CHECK(!wcscmp(txt, L"续传") && IsWindowEnabled(sw->btn_pause),
                     "暂停态：按钮文字切换为「续传」且可点");
            SW_CHECK(IsWindowEnabled(sw->btn_stop),
                     "暂停态：终止仍可点");

            cn_set_xfer_state(sw, CN_XFER_IDLE);
            GetWindowTextW(sw->btn_pause, txt, lenof(txt));
            SW_CHECK(!wcscmp(txt, L"暂停") && !IsWindowEnabled(sw->btn_pause) &&
                     !IsWindowEnabled(sw->btn_stop),
                     "空闲态：两按钮复位为禁用且文字回到「暂停」");
        }
        /*
         * 窗口图标：资源没编进去或 ID 写错时 LoadIcon 返回 NULL，
         * 标题栏会悄悄退回系统默认图标 —— 界面上不报错，所以得查。
         */
        SW_CHECK(LoadIconW(GetModuleHandle(NULL),
                           MAKEINTRESOURCEW(IDI_SFTPXFER)) != NULL,
                 "文件传输窗口图标已编入资源");

        /*
         * 加载动画的菜单定位依赖：菜单栏里必须找得到「文件传输」项，
         * 否则菜单旁的小转圈没有落脚点。自检窗口本身没有菜单栏，这里
         * 挂一个临时菜单真验定位逻辑，验完摘掉。
         */
        {
            HMENU tmpmenu = CreateMenu();
            HMENU oldmenu = GetMenu(sw->hwnd);

            AppendMenuW(tmpmenu, MF_STRING, 0x7F00, L"文件(&F)");
            AppendMenuW(tmpmenu, MF_STRING, 0x7F01, L"文件传输(&F)...");
            SetMenu(sw->hwnd, tmpmenu);
            DrawMenuBar(sw->hwnd);
            {
                RECT mr;

                SW_CHECK(cn_find_xfer_menu_item(sw->hwnd, &mr) &&
                         mr.right > mr.left,
                         "菜单栏定位得到「文件传输」项");
            }
            SetMenu(sw->hwnd, oldmenu);
            DrawMenuBar(sw->hwnd);
            DestroyMenu(tmpmenu);
        }

        sw_report(fp, "\n几何检查：\n");
        SW_CHECK(sw_rect_valid(sw->local_list, &client), "本地列表在客户区内");
        SW_CHECK(sw_rect_valid(sw->remote_list, &client), "远端列表在客户区内");
        SW_CHECK(sw_rect_valid(sw->btn_to_remote, &client), "上传按钮在客户区内");
        SW_CHECK(sw_rect_valid(sw->btn_pause, &client), "暂停按钮在客户区内");
        SW_CHECK(sw_rect_valid(sw->btn_stop, &client), "终止按钮在客户区内");
        SW_CHECK(sw_rect_valid(sw->progress, &client), "进度条在客户区内");

        {
            RECT l = sw_child_rect(sw->local_list);
            RECT r = sw_child_rect(sw->remote_list);
            RECT b1 = sw_child_rect(sw->btn_to_remote);
            RECT b2 = sw_child_rect(sw->btn_to_local);

            SW_CHECK(!sw_rects_overlap(l, r), "左右列表不重叠");
            SW_CHECK(!sw_rects_overlap(b1, l) && !sw_rects_overlap(b1, r),
                     "上传按钮不压住列表");
            SW_CHECK(!sw_rects_overlap(b2, l) && !sw_rects_overlap(b2, r),
                     "下载按钮不压住列表");

            /*
             * 暂停 / 终止在中间列的底部：都在下载按钮下方，彼此不重叠，
             * 也不压住两侧列表和进度条。
             */
            {
                RECT b3 = sw_child_rect(sw->btn_pause);
                RECT b4 = sw_child_rect(sw->btn_stop);
                RECT pg = sw_child_rect(sw->progress);
                SW_CHECK(!sw_rects_overlap(b3, l) && !sw_rects_overlap(b3, r),
                         "暂停按钮不压住列表");
                SW_CHECK(!sw_rects_overlap(b4, l) && !sw_rects_overlap(b4, r),
                         "终止按钮不压住列表");
                SW_CHECK(!sw_rects_overlap(b3, b4), "暂停与终止不重叠");
                SW_CHECK(b3.top >= b2.bottom, "暂停按钮在下载按钮下方");
                SW_CHECK(b4.top >= b3.bottom, "终止按钮在暂停按钮下方");
                SW_CHECK(!sw_rects_overlap(b3, pg) && !sw_rects_overlap(b4, pg),
                         "暂停/终止不压住进度条");
                /* 与上传下载同列：左缘对齐，看着才是一列。 */
                SW_CHECK(b3.left == b1.left && b4.left == b1.left,
                         "暂停/终止与上传下载同列");
                SW_CHECK(b3.right == b1.right && b4.right == b1.right,
                         "暂停/终止与上传下载同宽");
            }

            SW_CHECK(r.right < l.left, "远端列表在本地列表左侧");

            /*
             * 返回按钮的对齐与等高。
             *
             * 远端那个贴左缘、本地那个贴右缘，两栏等宽，所以本地按钮的
             * 右边缘必须和本地路径框、本地列表的右边缘齐平 —— 早先这里
             * 多减了一个滚动条宽度，按钮右缘缩进 34px，肉眼一看就歪。
             * 等高同理：两侧走同一组 btnw/btnh，差一像素都说明有人偷偷
             * 给单侧改过参数。
             */
            {
                RECT bu = sw_child_rect(sw->btn_up);
                RECT bl = sw_child_rect(sw->btn_up_local);
                RECT lp = sw_child_rect(sw->local_path);
                RECT ll = sw_child_rect(sw->local_list);
                RECT rp = sw_child_rect(sw->remote_path);

                SW_CHECK(bu.bottom - bu.top == bl.bottom - bl.top,
                         "两侧返回按钮等高");
                SW_CHECK(bu.right - bu.left == bl.right - bl.left,
                         "两侧返回按钮等宽");
                SW_CHECK(bl.right == lp.right,
                         "[本地]返回按钮右缘与路径框右缘齐平");
                {
                    RECT bd = sw_child_rect(sw->btn_desktop_local);
                    SW_CHECK(bd.right + cn_dpi(6) == bl.left,
                             "[本地]桌面键右缘与返回键左缘间距 6px");
                    SW_CHECK(bd.bottom - bd.top == bl.bottom - bl.top,
                             "[本地]桌面键与返回键等高");
                }
                SW_CHECK(bl.right == ll.right,
                         "[本地]返回按钮右缘与列表右缘齐平");
                SW_CHECK(bu.left == rp.left,
                         "[远端]返回按钮左缘与路径框左缘齐平");
            }

            /*
             * 列宽检查：三列之和 <= 列表客户区宽度（否则常驻横向滚动条），
             * 且大小 / 修改时间两列必须容得下自己的内容（否则被截成
             * 「2026-07...」），两列都右对齐。
             */
            {
                HWND ls[2] = { sw->remote_list, sw->local_list };
                const char *tag2[2] = { "远端", "本地" };
                for (int i = 0; i < 2; i++) {
                    RECT cr;
                    int cw = 0, sum = 0, avail;
                    int w_size, w_time, need_size, need_time;
                    GetClientRect(ls[i], &cr);
                    cw = cr.right;
                    /*
                     * 与 cn_fit_columns_ex 同口径：它按「窗口宽 − 边框 −
                     * 竖滚动条」分配，不是直接拿客户区。这里也扣一遍，
                     * 否则「装不装得下」的判断会和实现错开一整个滚动条。
                     */
                    {
                        RECT wr;
                        GetWindowRect(ls[i], &wr);
                        avail = (wr.right - wr.left) -
                                2 * GetSystemMetrics(SM_CXBORDER) -
                                GetSystemMetrics(SM_CXVSCROLL) - 2;
                    }
                    for (int c = 0; c < 3; c++)
                        sum += (int)SendMessageW(ls[i], LVM_GETCOLUMNWIDTH,
                                                 c, 0);
                    w_size = (int)SendMessageW(ls[i], LVM_GETCOLUMNWIDTH, 1, 0);
                    w_time = (int)SendMessageW(ls[i], LVM_GETCOLUMNWIDTH, 2, 0);
                    /* 与实现一致：内容与列头取大者，所以列头也要量。 */
                    need_size = cn_column_content_width(ls[i], 1, L"大小");
                    need_time = cn_column_content_width(ls[i], 2,
                                                        L"修改时间");
                    sw_report(fp,
                              "  [%s] 列表宽 %d，三列之和 %d，"
                              "大小列 %d/%d，时间列 %d/%d\n",
                              tag2[i], cw, sum,
                              w_size, need_size, w_time, need_time);

                    {
                        char *d = dupprintf("[%s]列宽不溢出", tag2[i]);
                        SW_CHECK(sum <= cw, d);
                        sfree(d);
                    }
                    /*
                     * 真正的验收标准：横向滚动条不许存在。光比「三列之和
                     * <= 客户区」不够 —— 竖滚动条异步出现时客户区会变，
                     * 那条断言照样能过，横向滚动条却已经冒出来了。
                     */
                    {
                        SCROLLINFO hsi;
                        bool noh;
                        memset(&hsi, 0, sizeof(hsi));
                        hsi.cbSize = sizeof(hsi);
                        hsi.fMask = SIF_ALL;
                        noh = !GetScrollInfo(ls[i], SB_HORZ, &hsi) ||
                              hsi.nMax < (int)hsi.nPage;
                        {
                            char *d = dupprintf("[%s]无横向滚动条",
                                                tag2[i]);
                            SW_CHECK(noh, d);
                            sfree(d);
                        }
                    }
                    {
                        /*
                         * 容得下内容这条只在「窗口足够宽」时才成立。
                         *
                         * 默认窗口宽是逻辑 1000，100% 屏上两栏各约 434 物理
                         * 像素，时间戳 192 加大小的 96 之后名称列还有位置；
                         * 但主人可能把窗口拖窄，一旦可用宽跌到 396 以下就
                         * 必然压缩。所以这里先算「理想需求总宽」，只有它
                         * 装得下才要求两列不被压；装不下时退化成「按比例
                         * 压缩且不溢出」，那由上面的列宽断言覆盖。
                         */
                        int ideal;
                        need_size = cn_column_content_width(ls[i], 1, L"大小");
                        need_time = cn_column_content_width(ls[i], 2,
                                                            L"修改时间");
                        /*
                         * 理想总宽要按**实现的计算口径**估：实现取的是
                         * 「内容宽与列头文字宽的大者，再加左右各 10」。
                         * 远端列表没有文件时大小列内容是 0，实现仍会把
                         * 它撑到列头「大小」的宽度 —— 按 0 估就会误判
                         * 「装得下」，然后报一条假失败。
                         */
                        ideal = (need_size > 0 ? need_size : cn_dpi(20)) +
                                (need_time > 0 ? need_time : cn_dpi(60)) +
                                cn_dpi(40) + 2 * cn_dpi(10) * 2;
                        if (ideal <= avail) {
                            char *d = dupprintf("[%s]大小列容得下内容",
                                                tag2[i]);
                            SW_CHECK(need_size < 0 || w_size >= need_size, d);
                            sfree(d);
                            d = dupprintf("[%s]时间列容得下内容", tag2[i]);
                            SW_CHECK(need_time < 0 || w_time >= need_time, d);
                            sfree(d);
                        } else {
                            char *d = dupprintf(
                                "[%s]窄列表按比例压缩（理想 %d > 可用 %d）",
                                tag2[i], ideal, avail);
                            SW_CHECK(sum <= cw, d);
                            sfree(d);
                        }
                    }
                    /*
                     * 后两列要贴内容：列宽 = 内容/列头宽 + 左右各 10。
                     *
                     * 原来这里断言「时间列不超 210」——那是旧字体的封顶，
                     * 现在没有封顶了，改成验更本质的东西：列宽不该比内容
                     * 宽出一大截。允许 ±(列头差 + 边距) 的余量：列宽用的
                     * 是「内容与列头取大者」，列头比内容宽时列就会宽一点，
                     * 这属于正常。
                     */
                    {
                        int slack = cn_dpi(10) * 2 + cn_dpi(30);
                        char *d = dupprintf(
                            "[%s]时间列不浪费宽度（列宽 %d，内容 %d）",
                            tag2[i], w_time, need_time);
                        SW_CHECK(need_time < 0 || w_time <= need_time + slack,
                                 d);
                        sfree(d);
                        d = dupprintf(
                            "[%s]大小列不浪费宽度（列宽 %d，内容 %d）",
                            tag2[i], w_size, need_size);
                        SW_CHECK(need_size < 0 || w_size <= need_size + slack,
                                 d);
                        sfree(d);
                    }

                    /* 后两列内容必须右对齐：数字与时间戳靠右才对齐。 */
                    {
                        LVCOLUMNW col;
                        int fmt_ok = 0;
                        memset(&col, 0, sizeof(col));
                        col.mask = LVCF_FMT;
                        if (SendMessageW(ls[i], LVM_GETCOLUMNW, 1,
                                         (LPARAM)&col))
                            fmt_ok = ((col.fmt & LVCFMT_JUSTIFYMASK) ==
                                      LVCFMT_RIGHT);
                        {
                            char *d = dupprintf("[%s]大小列右对齐", tag2[i]);
                            SW_CHECK(fmt_ok, d);
                            sfree(d);
                        }

                        memset(&col, 0, sizeof(col));
                        col.mask = LVCF_FMT;
                        fmt_ok = 0;
                        if (SendMessageW(ls[i], LVM_GETCOLUMNW, 2,
                                         (LPARAM)&col))
                            fmt_ok = ((col.fmt & LVCFMT_JUSTIFYMASK) ==
                                      LVCFMT_RIGHT);
                        {
                            char *d = dupprintf("[%s]时间列右对齐", tag2[i]);
                            SW_CHECK(fmt_ok, d);
                            sfree(d);
                        }
                    }

                    /*
                     * 列头居中靠子类化 header 自绘，不再动列头的 justify
                     * 位（那会把列内容的 LVCFMT_RIGHT 一起改掉，实测右
                     * 对齐断言全红）。这里断言 header 的窗口过程确实被
                     * 换成了自绘过程。
                     */
                    {
                        HWND header = (HWND)SendMessageW(ls[i],
                                                         LVM_GETHEADER, 0, 0);
                        WNDPROC cur = header
                            ? (WNDPROC)GetWindowLongPtrW(header, GWLP_WNDPROC)
                            : NULL;
                        sw_report(fp, "  [%s] 列头句柄 %p 过程 %p\n", tag2[i],
                                  (void *)header, (void *)cur);
                        {
                            char *d = dupprintf("[%s]列头已自绘居中", tag2[i]);
                            SW_CHECK(cur != NULL &&
                                     cur == cn_header_proc_addr, d);
                            sfree(d);
                        }
                    }
                }
            }

            sw_report(fp, "  本地列表   (%ld,%ld)-(%ld,%ld)\n",
                      l.left, l.top, l.right, l.bottom);
            sw_report(fp, "  远端列表   (%ld,%ld)-(%ld,%ld)\n",
                      r.left, r.top, r.right, r.bottom);
            sw_report(fp, "  上传按钮   (%ld,%ld)-(%ld,%ld)\n",
                      b1.left, b1.top, b1.right, b1.bottom);
        }

        sw_report(fp, "\n列表内容：\n");
        sw_report(fp, "  本地条目 %d 个\n",
                  ListView_GetItemCount(sw->local_list));
        sw_report(fp, "  远端条目 %d 个\n",
                  ListView_GetItemCount(sw->remote_list));
        sw_report(fp, "  远端目录 %s\n",
                  sw->remotedir ? sw->remotedir : "(未连接)");

        SW_CHECK(ListView_GetItemCount(sw->local_list) > 0,
                 "本地列表有内容");

        /*
         * 远端目录识别。
         *
         * 这条针对的是「精简固件不返回权限位」的兼容性问题：部分设备
         * 在 READDIR 的 attrs 里只回 size/mtime，flags 不含
         * SSH_FILEXFER_ATTR_PERMISSIONS，此时若只认 S_ISDIR，目录会
         * 被当成文件。cn_remote_is_dir() 因此加了 longname 首字符兜底。
         *
         * 这里直接查协议层（再列一次目录拿原始 SftpEntry），而不是只看
         * 列表控件 —— 控件里读不到 longname，验证不到兜底路径。
         */
        sw_report(fp, "\n远端目录识别：\n");
        if (sw->sx) {
            char *em = NULL;
            size_t cnt = 0;
            SftpEntry *ents = sftp_xfer_list(sw->sx, sw->remotedir, &cnt, &em);
            if (!ents) {
                sw_report(fp, "  跳过  列目录失败：%s\n",
                          em ? em : "(无信息)");
                sfree(em);
            } else {
                size_t ndir = 0, nfile = 0;
                for (size_t i = 0; i < cnt; i++) {
                    if (ents[i].is_dir)
                        ndir++;
                    else
                        nfile++;
                }
                sw_report(fp, "  条目 %d：目录 %d、文件 %d\n",
                          (int)cnt, (int)ndir, (int)nfile);

                /*
                 * 桩服务器根目录只有一个 sftp_rtest 目录（无文件）。
                 * 有目录就必须被识别成目录 —— 权限位缺失时这条会挂，
                 * 正好兜住回归。
                 */
                if (cnt > 0) {
                    SW_CHECK(ndir > 0, "远端条目中识别出目录");
                    /*
                     * 反向验证：文件不能被误判成目录。若 longname 解析
                     * 写反（判 '-' 为目录），这条会挂。
                     */
                    SW_CHECK(nfile == 0 || ndir + nfile == cnt,
                             "目录/文件计数自洽");
                }

                /*
                 * 符号链接识别。桩在 sftp_rtest 下放了 dirlink（指向
                 * sub1 目录）、filelink（指向 top.txt）、loop/self
                 * （指向 . 的自指链接）。递归必须能认出它们并跳过目录
                 * 链接，否则会绕成环、把同一批文件传很多遍。
                 */
                sw_report(fp, "\n符号链接识别：\n");
                {
                    char *em2 = NULL;
                    size_t cnt2 = 0;
                    SftpEntry *e2 = sftp_xfer_list(sw->sx, "/sftp_rtest",
                                                   &cnt2, &em2);
                    if (!e2) {
                        sw_report(fp, "  跳过  列目录失败：%s\n",
                                  em2 ? em2 : "(无信息)");
                        sfree(em2);
                    } else {
                        size_t nlink = 0;
                        for (size_t i = 0; i < cnt2; i++)
                            if (e2[i].is_symlink)
                                nlink++;
                        sw_report(fp, "  条目 %d：链接 %d\n",
                                  (int)cnt2, (int)nlink);
                        SW_CHECK(nlink > 0, "远端条目中识别出符号链接");

                        /*
                         * LSTAT 复核：dirlink 指向目录，STAT 会跟随成
                         * 目录、LSTAT 必须报链接。这条验的是
                         * sftp_xfer_is_symlink 走的是 LSTAT 而非 STAT。
                         */
                        SW_CHECK(sftp_xfer_is_symlink(sw->sx,
                                                      "/sftp_rtest/dirlink"),
                                 "LSTAT 认出指向目录的链接");
                        SW_CHECK(sftp_xfer_is_symlink(sw->sx,
                                                      "/sftp_rtest/filelink"),
                                 "LSTAT 认出指向文件的链接");
                        /* 普通文件与普通目录不能被误判成链接 */
                        SW_CHECK(!sftp_xfer_is_symlink(sw->sx,
                                                       "/sftp_rtest/top.txt"),
                                 "普通文件不被误判为链接");
                        SW_CHECK(!sftp_xfer_is_symlink(sw->sx,
                                                       "/sftp_rtest/sub1"),
                                 "普通目录不被误判为链接");
                        sftp_entries_free(e2, cnt2);
                    }
                }
                sftp_entries_free(ents, cnt);
            }
        } else {
            sw_report(fp, "  跳过  未连接\n");
        }

        /*
         * REALPATH 与 STAT 的健壮性。
         *
         * 这两条针对的是「设备实现不完全规范」：
         *  - REALPATH 原来写死 count != 1 就失败，而它在会话建立时
         *    用来取 home 目录，一失败整个传输功能不可用；
         *  - 少数设备只实现了 LSTAT 没实现 STAT，取不到大小会让
         *    续传判断误以为远端没有文件。
         */
        sw_report(fp, "\nREALPATH / STAT 健壮性：\n");
        if (sw->sx) {
            const char *hd = sftp_xfer_homedir(sw->sx);
            SW_CHECK(hd != NULL && hd[0] != '\0',
                     "REALPATH 取到 home 目录");
            sw_report(fp, "  home = %s\n", hd ? hd : "(空)");

            {
                struct fxp_attrs at;
                bool ok = sftp_xfer_stat(sw->sx, "/sftp_rtest/top.txt", &at);
                SW_CHECK(ok, "STAT 取到普通文件属性");
                if (ok) {
                    sw_report(fp, "  top.txt 大小 = %llu\n",
                              (unsigned long long)
                              ((at.flags & SSH_FILEXFER_ATTR_SIZE)
                               ? at.size : 0));
                    SW_CHECK((at.flags & SSH_FILEXFER_ATTR_SIZE) &&
                             at.size == 12,
                             "STAT 返回的 top.txt 大小正确（12 字节）");
                }
            }
        } else {
            sw_report(fp, "  跳过  未连接\n");
        }

        /*
         * 右键菜单检查。三件事：三项命令都在；无选中时「删除 / 重命名」
         * 灰掉、有选中时恢复可用；末行灰字标的作用对象跟面板对得上。
         * 弹真菜单会卡住自检（要人点），所以只能查 HMENU 状态。
         */
        sw_report(fp, "\n列表右键菜单：\n");
        {
            bool panels[2] = { false, true };   /* 本地、远端 */
            for (int p = 0; p < 2; p++) {
                bool remote = panels[p];
                const char *tag = remote ? "远端" : "本地";
                HWND list = remote ? sw->remote_list : sw->local_list;
                HMENU menu;
                char *d = NULL;

                /* 先清空选中，验「无选中 → 灰掉」这条。 */
                ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
                menu = cn_build_list_menu(sw, remote);
                SW_CHECK(menu != NULL, remote ? "远端菜单创建" : "本地菜单创建");
                if (!menu) {
                    continue;
                }

                {
                    int n = GetMenuItemCount(menu);
                    sw_report(fp, "  [%s] 菜单项 %d 个\n", tag, n);
                    d = dupprintf("[%s]菜单项数=7", tag);
                    SW_CHECK(n == 7, d);
                    sfree(d);
                }

                d = dupprintf("[%s]菜单含新建目录", tag);
                SW_CHECK((int)GetMenuState(menu, IDM_MKDIR, MF_BYCOMMAND) != -1
                         && !(GetMenuState(menu, IDM_MKDIR, MF_BYCOMMAND)
                              & MF_GRAYED), d);
                sfree(d);

                /* 刷新从顶栏按钮搬进菜单，任何时候都可用。 */
                d = dupprintf("[%s]菜单含刷新且可用", tag);
                SW_CHECK((int)GetMenuState(menu, IDM_REFRESH, MF_BYCOMMAND) != -1
                         && !(GetMenuState(menu, IDM_REFRESH, MF_BYCOMMAND)
                              & MF_GRAYED), d);
                sfree(d);

                d = dupprintf("[%s]无选中时删除置灰", tag);
                SW_CHECK((int)GetMenuState(menu, IDM_DELETE, MF_BYCOMMAND) != -1
                         && (GetMenuState(menu, IDM_DELETE, MF_BYCOMMAND)
                             & MF_GRAYED), d);
                sfree(d);

                d = dupprintf("[%s]无选中时重命名置灰", tag);
                SW_CHECK((int)GetMenuState(menu, IDM_RENAME, MF_BYCOMMAND) != -1
                         && (GetMenuState(menu, IDM_RENAME, MF_BYCOMMAND)
                             & MF_GRAYED), d);
                sfree(d);

                /* 末行灰字：读出文字比对「作用于远端/本地面板」。 */
                {
                    wchar_t wbuf[128];
                    wbuf[0] = L'\0';
                    GetMenuStringW(menu, 6, wbuf, lenof(wbuf), MF_BYPOSITION);
                    d = dup_wc_to_mb(DEFAULT_CODEPAGE, wbuf, "?");
                    sw_report(fp, "  [%s] 作用对象提示 = %s\n", tag, d);
                    {
                        char *want = dupprintf("作用于%s面板", tag);
                        char *desc = dupprintf("[%s]作用对象提示正确", tag);
                        SW_CHECK(!strcmp(d, want), desc);
                        sfree(desc);
                        sfree(want);
                    }
                    sfree(d);
                }
                DestroyMenu(menu);

                /* 有选中时，删/改应当恢复可用。列表没条目就跳过。 */
                if (ListView_GetItemCount(list) > 0) {
                    ListView_SetItemState(list, 0, LVIS_SELECTED,
                                          LVIS_SELECTED);
                    menu = cn_build_list_menu(sw, remote);
                    if (menu) {
                        d = dupprintf("[%s]选中后删除可用", tag);
                        SW_CHECK(!(GetMenuState(menu, IDM_DELETE,
                                                MF_BYCOMMAND) & MF_GRAYED), d);
                        sfree(d);
                        d = dupprintf("[%s]选中后重命名可用", tag);
                        SW_CHECK(!(GetMenuState(menu, IDM_RENAME,
                                                MF_BYCOMMAND) & MF_GRAYED), d);
                        sfree(d);
                        DestroyMenu(menu);
                    }
                    ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
                } else {
                    sw_report(fp, "  [%s] 列表为空，跳过「选中后可用」检查\n",
                              tag);
                }
            }
        }

        /*
         * 传输按钮的启用状态：默认两个都灰，只有对应源列表选中了才亮，
         * 且两边互不影响。
         *
         * 这里直接调 cn_update_transfer_buttons 重算 —— 自检没有真实
         * 鼠标输入，LVN_ITEMCHANGED 未必会派发到窗口过程，靠模拟点击
         * 去验证会引入不确定性。直接验判定函数本身，才是这条逻辑的
         * 唯一真相来源。
         */
        {
            HWND lp = sw->local_list, rp = sw->remote_list;
            bool lp_has = lp && ListView_GetItemCount(lp) > 0;
            bool rp_has = rp && ListView_GetItemCount(rp) > 0;

            sw_report(fp, "\n传输按钮启用状态：\n");

            /* 1. 两边都不选中：都灰。 */
            if (lp)
                ListView_SetItemState(lp, -1, 0, LVIS_SELECTED);
            if (rp)
                ListView_SetItemState(rp, -1, 0, LVIS_SELECTED);
            cn_update_transfer_buttons(sw);
            SW_CHECK(!IsWindowEnabled(sw->btn_to_remote), "无选中时上传禁用");
            SW_CHECK(!IsWindowEnabled(sw->btn_to_local), "无选中时下载禁用");

            /* 2. 只选本地：上传亮、下载仍灰（互相独立）。 */
            if (lp_has) {
                ListView_SetItemState(lp, 0, LVIS_SELECTED, LVIS_SELECTED);
                cn_update_transfer_buttons(sw);
                SW_CHECK(IsWindowEnabled(sw->btn_to_remote),
                         "选中本地文件后上传可用");
                SW_CHECK(!IsWindowEnabled(sw->btn_to_local),
                         "只选本地时下载仍禁用");
                ListView_SetItemState(lp, -1, 0, LVIS_SELECTED);
            } else {
                sw_report(fp, "  本地列表为空，跳过「选中后上传可用」检查\n");
            }

            /* 3. 只选远端：下载亮、上传仍灰。 */
            if (rp_has) {
                ListView_SetItemState(rp, 0, LVIS_SELECTED, LVIS_SELECTED);
                cn_update_transfer_buttons(sw);
                SW_CHECK(IsWindowEnabled(sw->btn_to_local),
                         "选中远端文件后下载可用");
                SW_CHECK(!IsWindowEnabled(sw->btn_to_remote),
                         "只选远端时上传仍禁用");
                ListView_SetItemState(rp, -1, 0, LVIS_SELECTED);
            } else {
                sw_report(fp, "  远端列表为空，跳过「选中后下载可用」检查\n");
            }

            /* 4. 取消选中后回到禁用态。 */
            if (lp_has) {
                ListView_SetItemState(lp, 0, LVIS_SELECTED, LVIS_SELECTED);
                cn_update_transfer_buttons(sw);
                ListView_SetItemState(lp, -1, 0, LVIS_SELECTED);
                cn_update_transfer_buttons(sw);
                SW_CHECK(!IsWindowEnabled(sw->btn_to_remote),
                         "取消选中后上传恢复禁用");
            }

            /* 5. busy 期间一律禁用，哪怕有选中。 */
            if (lp_has) {
                ListView_SetItemState(lp, 0, LVIS_SELECTED, LVIS_SELECTED);
                cn_set_busy(sw, true);
                SW_CHECK(!IsWindowEnabled(sw->btn_to_remote),
                         "传输中上传禁用（即使有选中）");
                SW_CHECK(!IsWindowEnabled(sw->btn_to_local),
                         "传输中下载禁用");
                cn_set_busy(sw, false);
                SW_CHECK(IsWindowEnabled(sw->btn_to_remote),
                         "传输结束后上传恢复可用");
                ListView_SetItemState(lp, -1, 0, LVIS_SELECTED);
                cn_update_transfer_buttons(sw);
            }

            /*
             * 6. 点空白处取消选中。
             *
             * 列表带 LVS_SHOWSELALWAYS，系统默认点空白不清选中，所以
             * 在 cn_list_proc 里自己拦了 WM_LBUTTONDOWN。
             *
             * ⚠️ 这里不能真发 WM_LBUTTONDOWN 走子类过程：自检窗口没
             * ShowWindow，CallWindowProcW 调默认过程处理鼠标按下时会
             * 阻塞，实测永不返回、自检整体挂死。改为分两步验：
             *   ① cn_list_point_on_item 判定空白点确实不在条目上；
             *   ② cn_clear_selection 清完后按钮回到禁用。
             * 这两步合起来就是子类过程在那条分支里做的全部事情。
             *
             * 选远端列表来测：它只有一两个条目，下方必有成片空白；
             * 本地列表常被几十个目录项铺满，找不到真正的空白点。
             */
            if (rp_has) {
                RECT rr;
                int bx, by;
                GetClientRect(rp, &rr);
                bx = rr.left + 4;
                by = rr.bottom - 4;

                SW_CHECK(!cn_list_point_on_item(rp, bx, by),
                         "列表底部空白点判定为不在条目上");

                ListView_SetItemState(rp, 0, LVIS_SELECTED, LVIS_SELECTED);
                cn_update_transfer_buttons(sw);
                SW_CHECK(IsWindowEnabled(sw->btn_to_local),
                         "点空白前下载可用（前置条件）");
                cn_clear_selection(sw);
                SW_CHECK(ListView_GetNextItem(rp, -1, LVNI_SELECTED) < 0,
                         "点列表空白处清除选中");
                SW_CHECK(!IsWindowEnabled(sw->btn_to_local),
                         "点空白取消选中后下载禁用");
            }

            /*
             * 6b. 点列表外面的窗口背景，两侧选中都清掉。
             *
             * 本地目录项多时列表里没有空白可点，用户只能往列表外点。
             * 子控件的鼠标消息不冒泡到父窗口，所以发到主窗口的
             * WM_LBUTTONDOWN 就代表「点在背景上」。
             */
            if (lp_has || rp_has) {
                if (lp_has)
                    ListView_SetItemState(lp, 0, LVIS_SELECTED,
                                          LVIS_SELECTED);
                if (rp_has)
                    ListView_SetItemState(rp, 0, LVIS_SELECTED,
                                          LVIS_SELECTED);
                cn_update_transfer_buttons(sw);
                SW_CHECK(IsWindowEnabled(sw->btn_to_remote) &&
                             IsWindowEnabled(sw->btn_to_local),
                         "点背景前两按钮均可用（前置条件）");
                SendMessageW(sw->hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
                             MAKELPARAM(2, 2));
                SW_CHECK(ListView_GetNextItem(lp, -1, LVNI_SELECTED) < 0,
                         "点窗口背景清除本地选中");
                SW_CHECK(ListView_GetNextItem(rp, -1, LVNI_SELECTED) < 0,
                         "点窗口背景清除远端选中");
                SW_CHECK(!IsWindowEnabled(sw->btn_to_remote),
                         "点窗口背景后上传禁用");
                SW_CHECK(!IsWindowEnabled(sw->btn_to_local),
                         "点窗口背景后下载禁用");
            }

            /*
             * 6c. 点标题栏也清选中。
             *
             * 直接调 cn_clear_selection 而不是发 WM_NCLBUTTONDOWN：
             * DefWindowProcW 收到 HTCAPTION 会进入拖动窗口的模态循环，
             * 自检发完这条消息就再也不返回（实测挂死）。
             * 窗口过程里调用的就是同一个函数，逻辑等价。
             */
            if (lp_has) {
                ListView_SetItemState(lp, 0, LVIS_SELECTED, LVIS_SELECTED);
                cn_update_transfer_buttons(sw);
                SW_CHECK(IsWindowEnabled(sw->btn_to_remote),
                         "点标题栏前上传可用（前置条件）");
                cn_clear_selection(sw);
                SW_CHECK(ListView_GetNextItem(lp, -1, LVNI_SELECTED) < 0,
                         "点标题栏清除选中");
                SW_CHECK(!IsWindowEnabled(sw->btn_to_remote),
                         "点标题栏后上传禁用");
            }

            /*
             * 7. 目录读取失败时列表被清空，按钮也必须回禁用。
             *
             * 主人实测踩到的就是这个：本地目录读不出来（路径失效等），
             * 走了 cn_refresh_local 的提前返回分支，列表已清空但按钮还
             * 亮着。这里直接把 localdir 换成不存在的路径，调一次真实
             * 刷新函数，验证按钮被关掉。
             */
            if (lp_has) {
                char *saved = sw->localdir;
                char *fake = dupstr("Z:\\cn-selftest-no-such-dir-9f3a");
                sw->localdir = fake;
                ListView_SetItemState(lp, 0, LVIS_SELECTED, LVIS_SELECTED);
                cn_update_transfer_buttons(sw);
                SW_CHECK(IsWindowEnabled(sw->btn_to_remote),
                         "刷新失败前上传可用（前置条件）");
                cn_refresh_local(sw);
                SW_CHECK(ListView_GetItemCount(lp) == 0,
                         "读取失败的目录清空列表");
                SW_CHECK(!IsWindowEnabled(sw->btn_to_remote),
                         "本地目录读取失败后上传禁用");
                sw->localdir = saved;
                sfree(fake);
                /* 恢复真实目录内容，后面的窄窗口检查还要用。 */
                cn_refresh_local(sw);
            }
        }

        /*
         * 窄窗口回归：把窗口强制缩到最小尺寸附近，重排后三列之和仍不许
         * 超过列表客户区宽度。这是主人报「列表拥挤、右侧数字被截」的
         * 直接复现点 —— 早先三列固定 210+80+130=420，一旦栏宽不足就
         * 常驻横向滚动条。
         */
        sw_report(fp, "\n窄窗口回归（缩到 900x480）：\n");
        SetWindowPos(sw->hwnd, NULL, 0, 0, 900, 480,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateWindow(sw->hwnd);
        {
            HWND ls[2] = { sw->remote_list, sw->local_list };
            const char *tg[2] = { "远端", "本地" };
            RECT nc;
            GetClientRect(sw->hwnd, &nc);
            sw_report(fp, "  窄窗口客户区 %ld x %ld\n", nc.right, nc.bottom);
            for (int i = 0; i < 2; i++) {
                RECT cr;
                int cw, sum = 0;
                GetClientRect(ls[i], &cr);
                cw = cr.right;
                for (int c = 0; c < 3; c++)
                    sum += (int)SendMessageW(ls[i], LVM_GETCOLUMNWIDTH, c, 0);
                sw_report(fp, "  [%s] 列表宽 %d，三列之和 %d\n", tg[i], cw, sum);
                {
                    char *d = dupprintf("[%s]窄窗口列宽不溢出", tg[i]);
                    SW_CHECK(sum <= cw, d);
                    sfree(d);
                }
            }
            {
                RECT l = sw_child_rect(sw->local_list);
                RECT r = sw_child_rect(sw->remote_list);
                SW_CHECK(!sw_rects_overlap(l, r), "窄窗口左右列表不重叠");
            }
        }

        /*
         * 宽窗口回归：窄窗口那批只能验证「不溢出」，证明不了「装得下」。
         * 这里把窗口拉宽到两栏各能容纳完整时间戳的宽度，再重算一次列宽，
         * 确认算法在空间够的时候确实会把两列撑到内容宽度 —— 否则「窄了
         * 压缩」就成了掩盖缺陷的万能借口。
         */
        sw_report(fp, "\n宽窗口回归（拉到逻辑 1200x700）：\n");
        SetWindowPos(sw->hwnd, NULL, 0, 0, cn_dpi(1200), cn_dpi(700),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateWindow(sw->hwnd);
        {
            HWND ls[2] = { sw->remote_list, sw->local_list };
            const char *tg[2] = { "远端", "本地" };
            RECT wc;
            GetClientRect(sw->hwnd, &wc);
            sw_report(fp, "  宽窗口客户区 %ld x %ld\n", wc.right, wc.bottom);
            for (int i = 0; i < 2; i++) {
                RECT cr;
                int cw, sum = 0, w_size, w_time, need_size, need_time;
                GetClientRect(ls[i], &cr);
                cw = cr.right;
                for (int c = 0; c < 3; c++)
                    sum += (int)SendMessageW(ls[i], LVM_GETCOLUMNWIDTH, c, 0);
                w_size = (int)SendMessageW(ls[i], LVM_GETCOLUMNWIDTH, 1, 0);
                w_time = (int)SendMessageW(ls[i], LVM_GETCOLUMNWIDTH, 2, 0);
                need_size = cn_column_content_width(ls[i], 1, L"大小");
                need_time = cn_column_content_width(ls[i], 2, L"修改时间");
                sw_report(fp,
                          "  [%s] 列表宽 %d，三列之和 %d，"
                          "大小列 %d/%d，时间列 %d/%d\n",
                          tg[i], cw, sum,
                          w_size, need_size, w_time, need_time);
                {
                    char *d = dupprintf("[%s]宽窗口列宽不溢出", tg[i]);
                    SW_CHECK(sum <= cw, d);
                    sfree(d);
                }
                {
                    char *d = dupprintf("[%s]宽窗口大小列容得下内容", tg[i]);
                    SW_CHECK(need_size < 0 || w_size >= need_size, d);
                    sfree(d);
                }
                {
                    char *d = dupprintf("[%s]宽窗口时间列容得下内容", tg[i]);
                    SW_CHECK(need_time < 0 || w_time >= need_time, d);
                    sfree(d);
                }
            }
        }
    }

    /*
     * 排序自检。
     *
     * 不靠跨进程读列表文字来验证 —— 那是另一个进程里的控件，把它自身
     * 的 pszText 指针交给它写，会直接写坏目标进程的内存（实测必崩）。
     * 这里直接构造一组条目调排序函数，比对结果顺序，全在当前进程内完成。
     */
    sw_report(fp, "\n排序：\n");
    {
        /*
         * 造一批能暴露「字典序 vs 自然排序」差异的名字：
         * file2 / file10 用 strcmp 会比成 file10 < file2，自然排序才是
         * file2 在前。再混一个目录，验证名称排序时目录恒在最前。
         */
        const char *names[] = { "file10.txt", "file2.txt", "File1.txt",
                                "adir", "bdir", "z.txt" };
        const bool dirs[] = { false, false, false, true, true, false };
        const uint64_t sizes[] = { 300, 20, 100, 0, 0, 10 };
        const unsigned long times[] = { 3000, 1000, 2000, 500, 4000, 100 };

        for (int asc = 1; asc >= 0; asc--) {
            SftpEntry e[6];
            for (int i = 0; i < 6; i++) {
                e[i].name = (char *)names[i];
                e[i].is_dir = dirs[i];
                e[i].size = sizes[i];
                e[i].mtime = times[i];
            }
            cn_sort_entries(e, 6, 0, asc == 1, false);

            /*
             * 名称升序：两个目录在前（adir < bdir），随后文件按自然序
             * File1 < file2 < file10 < z（不区分大小写，数字按数值）。
             */
            if (asc) {
                SW_CHECK(e[0].is_dir && e[1].is_dir,
                         "名称升序时目录在最前");
                /* 两个目录内部按自然序：adir 在 bdir 之前。 */
                SW_CHECK(!strcmp(e[0].name, "adir") &&
                         !strcmp(e[1].name, "bdir"),
                         "名称升序目录按名称排列");
                {
                    /* file2 必须排在 file10 前面，这是自然排序的标志。 */
                    int i2 = -1, i10 = -1;
                    for (int i = 0; i < 6; i++) {
                        if (!strcmp(e[i].name, "file2.txt")) i2 = i;
                        if (!strcmp(e[i].name, "file10.txt")) i10 = i;
                    }
                    SW_CHECK(i2 >= 0 && i10 >= 0 && i2 < i10,
                             "名称升序 file2 在 file10 之前");
                }
                /*
                 * 文件段必须紧跟在目录之后，且 z.txt 排最后 —— 顺带证明
                 * 目录不是被「按名字插进来」，而是真的整段提前。
                 */
                SW_CHECK(!strcmp(e[5].name, "z.txt"),
                         "名称升序文件段末尾为 z.txt");
            } else {
                /* 降序：目录依然在前（反转只管同类），文件部分反过来。 */
                SW_CHECK(e[0].is_dir && e[1].is_dir,
                         "名称降序时目录仍在最前");
                {
                    int i2 = -1, i10 = -1;
                    for (int i = 0; i < 6; i++) {
                        if (!strcmp(e[i].name, "file2.txt")) i2 = i;
                        if (!strcmp(e[i].name, "file10.txt")) i10 = i;
                    }
                    SW_CHECK(i2 >= 0 && i10 >= 0 && i10 < i2,
                             "名称降序 file10 在 file2 之前");
                }
            }
        }

        /* 按大小升序：目录不再特殊对待，一起参与，10 最小排最前。 */
        {
            SftpEntry e[6];
            for (int i = 0; i < 6; i++) {
                e[i].name = (char *)names[i];
                e[i].is_dir = dirs[i];
                e[i].size = sizes[i];
                e[i].mtime = times[i];
            }
            cn_sort_entries(e, 6, 1, true, false);
            SW_CHECK(e[0].size == 0 && e[1].size == 0 && e[2].size == 10,
                     "大小升序按数值排列（目录不特殊）");
            cn_sort_entries(e, 6, 1, false, false);
            SW_CHECK(e[5].size == 0 && e[0].size == 300,
                     "大小降序首项为最大");
        }

        /* 按时间升序：100 最小排最前，降序则 4000 最前。 */
        {
            SftpEntry e[6];
            for (int i = 0; i < 6; i++) {
                e[i].name = (char *)names[i];
                e[i].is_dir = dirs[i];
                e[i].size = sizes[i];
                e[i].mtime = times[i];
            }
            cn_sort_entries(e, 6, 2, true, false);
            SW_CHECK(e[0].mtime == 100 && e[5].mtime == 4000,
                     "时间升序按时间排列");
            cn_sort_entries(e, 6, 2, false, false);
            SW_CHECK(e[0].mtime == 4000 && e[5].mtime == 100,
                     "时间降序首项为最新");
        }
    }

    /*
     * 点击列头的链路验证。
     *
     * 上面那批验的是排序函数本身；这里验「点一下列头，状态真的会变」，
     * 也就是子类过程、命中判定、状态切换、重读目录这一整条线。
     *
     * 之所以不用 PostMessage 模拟鼠标：坐标依赖列头的实际像素位置，
     * 列宽又会随内容变，写死坐标在不同 DPI 和不同目录内容下都不稳。
     * 直接调点击处理函数，验的是同一条业务的入口，只是省掉命中测试。
     */
    sw_report(fp, "\n列头点击：\n");
    {
        SftpWin *sw = the_sftpwin;
        HWND hl = (HWND)SendMessageW(sw->local_list, LVM_GETHEADER, 0, 0);

        /* 初始必须是名称升序。 */
        sw->sort_col_local = 0;
        sw->sort_asc_local = true;

        /* 点「大小」列：应切到按大小、升序。 */
        cn_header_click(hl, 1);
        SW_CHECK(sw->sort_col_local == 1 && sw->sort_asc_local,
                 "点大小列切到按大小升序");

        /* 再点同一列：应翻转成降序。 */
        cn_header_click(hl, 1);
        SW_CHECK(sw->sort_col_local == 1 && !sw->sort_asc_local,
                 "再点大小列翻转为降序");

        /* 点「修改时间」列：换列，方向重置为升序。 */
        cn_header_click(hl, 2);
        SW_CHECK(sw->sort_col_local == 2 && sw->sort_asc_local,
                 "换列后方向重置为升序");

        /* 点「名称」列：回到名称升序。 */
        cn_header_click(hl, 0);
        SW_CHECK(sw->sort_col_local == 0 && sw->sort_asc_local,
                 "点名称列回到名称升序");

        /*
         * 两栏状态互不干扰：动过本地栏之后，远端栏应保持原样。
         * 这是「分两栏各存一份」这个设计的核心，坏了就会一边排序
         * 把另一边也带着变。
         */
        {
            int saved = sw->sort_col_remote;
            bool savedasc = sw->sort_asc_remote;
            HWND hr = (HWND)SendMessageW(sw->remote_list, LVM_GETHEADER, 0, 0);
            cn_header_click(hl, 1);
            SW_CHECK(sw->sort_col_remote == saved &&
                     sw->sort_asc_remote == savedasc,
                     "点本地列头不影响远端排序状态");
            (void)hr;
        }
    }

    /*
     * 弹窗摆位：验证 cn_place_near 把弹窗中心对到光标，并贴边收拢。
     *
     * 输入框与提示框都拿它算坐标，所以验这个函数等于验真实路径。
     * 凡是「弹窗跟着鼠标走」的行为都汇总到这一个函数，两点行为一并
     * 测：居中对准、出屏收进工作区。
     */
    sw_report(fp, "\n弹窗摆位：\n");
    {
        RECT wa;
        int x = 0, y = 0, ok = SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

        if (ok) {
            /* 屏幕正中的一点：弹窗中心该落在这里。 */
            POINT pt;
            int w = 300, h = 200;

            pt.x = (wa.left + wa.right) / 2;
            pt.y = (wa.top + wa.bottom) / 2;
            cn_place_near(pt, w, h, &x, &y);
            {
                char *d = dupprintf(
                    "弹窗中心对准光标（中心 %d,%d，光标 %ld,%ld）",
                    x + w / 2, y + h / 2, (long)pt.x, (long)pt.y);
                SW_CHECK(abs((x + w / 2) - pt.x) <= 2 &&
                         abs((y + h / 2) - pt.y) <= 2, d);
                sfree(d);
            }

            /*
             * 光标贴左上角：弹窗不能跑出工作区，左/上边必须被收回来。
             * 这两条正是「别让弹窗看不见」的实际保障。
             */
            pt.x = wa.left;
            pt.y = wa.top;
            cn_place_near(pt, w, h, &x, &y);
            SW_CHECK(x >= wa.left, "贴左上角时光标处弹窗不出左边界");
            SW_CHECK(y >= wa.top, "贴左上角时光标处弹窗不出上边界");

            /* 光标贴右下角：右/下边同样得收进来。 */
            pt.x = wa.right;
            pt.y = wa.bottom;
            cn_place_near(pt, w, h, &x, &y);
            SW_CHECK(x + w <= wa.right, "贴右下角时弹窗不出右边界");
            SW_CHECK(y + h <= wa.bottom, "贴右下角时弹窗不出下边界");
        } else {
            sw_report(fp, "  跳过：读不到工作区\n");
        }
    }

    /*
     * 冲突弹窗：三项截断、省略号、tooltip 全量。
     *
     * 这个弹窗是人工点的，自检不能等用户，所以只验「建出来什么样子」：
     * 文字内容、tooltip 有没有挂、挂的什么文本、窗口尺寸能不能装下。
     * 建完就销毁，不跑消息循环。
     */
    /*
     * 框选消息序列（程序内同线程发消息，安全）：
     * DOWN 在首行 → MOVE 超阈值（marquee 开始）→ MOVE 圈大 → 查选中 →
     * UP → 再查。复现「拖动中能选中、松手全丢」的真实序列。
     */
    sw_report(fp, "\n框选消息序列：\n");
    {
        SftpWin *sw = the_sftpwin;
        HWND lv = sw ? (sw->remote_list ? sw->remote_list : sw->local_list)
                     : NULL;
        int cnt = lv ? ListView_GetItemCount(lv) : 0;

        if (lv && cnt >= 1) {
            int mid, after;

            #define LP(x, y) ((LPARAM)(((y) << 16) | ((x) & 0xFFFF)))
            /*
             * DOWN 的默认过程在自检环境会卡死（真实鼠标无此问题），
             * 绕开它：手动布置 DOWN 完成后的状态，再发 MOVE / UP 验
             * 框选核心。
             *
             * 五条手势规则各测一遍：
             *   A 空白起手拖动                  → 框选（不受长按闸门约束）
             *   B 未选中项 + 长按已满 + 位移超阈值 → 框选
             *   C 未选中项 + 长按没满 + 位移超阈值 → 什么都不做（闸门挡住）
             *   D 未选中项 + 长按已满 + 位移在阈值内 → 什么都不做
             *   E 按下前已选中 + 长按没满 + 小位移 → 什么都不做（单击）
             * 「长按已满 + 按下前已选中 = 拖放」不在这里测：它要进
             * DoDragDrop 模态循环，自检里跑会把整个进程挂住。
             */
            /*
             * 模拟「长按已按满」：把按下时刻往前拨 CN_LONGPRESS_MS+500，
             * MOVE 分支现算 held 时必然超过闸门。不用 sleep 干等，自检
             * 要快，而且这个字段本来就只被现减一次。
             * ⚠️ 顺带把 cn_press_list 置空：自检里没有真按下，绝不希望
             * 任何 KillTimer 落到这个列表上。
             */
            #define SET_HELD_YES()                                                  \
                do {                                                                \
                    cn_press_pending = true;                                        \
                    cn_press_tick = GetTickCount() - (CN_LONGPRESS_MS + 500);        \
                    cn_press_list = NULL;                                           \
                    cn_longpress_static = false;                                    \
                } while (0)
            /* 模拟「刚按下、还没按满」。 */
            #define SET_HELD_NO()                                                   \
                do {                                                                \
                    cn_press_pending = true;                                        \
                    cn_press_tick = GetTickCount();                                  \
                    cn_press_list = NULL;                                           \
                    cn_longpress_static = false;                                    \
                } while (0)

            /* --- A 空白起手 --- */
            cn_press_reset();
            cn_drag_active = true;
            cn_drag_from_blank = true;
            cn_drag_from_list = lv;
            cn_drag_start.x = 30;
            cn_drag_start.y = 20;
            ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);

            SendMessageW(lv, WM_MOUSEMOVE, MK_LBUTTON, LP(70, 20));
            SendMessageW(lv, WM_MOUSEMOVE, MK_LBUTTON, LP(400, 90));
            mid = ListView_GetSelectedCount(lv);
            SW_CHECK(mid > 0, "框选拖动中圈到的项被选中");
            sw_report(fp, "  A 空白起手，拖动中选中 %d 项\n", mid);
            SendMessageW(lv, WM_LBUTTONUP, 0, LP(400, 90));
            after = ListView_GetSelectedCount(lv);
            SW_CHECK(after == mid, "松手后选中保持不丢失");

            /*
             * --- B 未选中项 + 长按已满 + 位移超阈值 = 框选 ---
             *
             * 闸门与位移同时成立才出框，这条验的是「门开了以后确实能拖」。
             */
            cn_drag_active = true;
            cn_drag_from_blank = false;
            cn_press_was_selected = false;  /* 按下前该项未选中 */
            SET_HELD_YES();
            cn_drag_from_list = lv;
            cn_drag_start.x = 30;
            cn_drag_start.y = 20;
            ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);

            SendMessageW(lv, WM_MOUSEMOVE, MK_LBUTTON, LP(70, 20));
            SW_CHECK(cn_marquee_active, "长按已满且位移超阈值即出框");
            SendMessageW(lv, WM_MOUSEMOVE, MK_LBUTTON, LP(400, 90));
            mid = ListView_GetSelectedCount(lv);
            SW_CHECK(mid > 0, "未选中项拖动进入框选并圈到项");
            sw_report(fp, "  B 未选中项拖动，拖动中选中 %d 项\n", mid);
            SendMessageW(lv, WM_LBUTTONUP, 0, LP(400, 90));
            after = ListView_GetSelectedCount(lv);
            SW_CHECK(after == mid, "行上框选松手后选中保持");

            /*
             * --- C 长按没满 + 位移超阈值 = 绝不冒框 ---
             *
             * 这是长按闸门的核心断言：位移再大，只要没按满 1 秒就不许
             * 出框。上一版撤掉长按后「点一下快速划走」就会冒框，正是
             * 主人反馈的「和上版一样」那种不对味。
             */
            cn_drag_active = true;
            cn_drag_from_blank = false;
            cn_press_was_selected = false;
            SET_HELD_NO();
            cn_drag_from_list = lv;
            cn_drag_start.x = 30;
            cn_drag_start.y = 20;
            ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);

            SendMessageW(lv, WM_MOUSEMOVE, MK_LBUTTON, LP(70, 20));
            SW_CHECK(!cn_marquee_active, "长按没满时位移超阈值也不出框");
            SendMessageW(lv, WM_MOUSEMOVE, MK_LBUTTON, LP(400, 200));
            SW_CHECK(!cn_marquee_active, "长按没满时大幅位移仍不出框");
            SW_CHECK(ListView_GetSelectedCount(lv) == 0,
                     "长按没满期间选中不受影响");
            sw_report(fp, "  C 长按没满，选中 %ld 项（应为 0）\n",
                      ListView_GetSelectedCount(lv));
            cn_drag_active = false;
            cn_press_reset();

            /*
             * --- D 长按已满但没移动 = 什么都不做 ---
             *
             * 门开了不等于要动：用户按满了一直没挪鼠标，不该凭空冒个
             * 零面积框出来。必须位移也超阈值才动作。
             */
            cn_drag_active = true;
            cn_drag_from_blank = false;
            cn_press_was_selected = false;
            SET_HELD_YES();
            cn_drag_from_list = lv;
            cn_drag_start.x = 30;
            cn_drag_start.y = 20;
            ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);

            SendMessageW(lv, WM_MOUSEMOVE, MK_LBUTTON, LP(32, 21));
            SW_CHECK(!cn_marquee_active, "长按已满但位移在阈值内不出框");
            SW_CHECK(ListView_GetSelectedCount(lv) == 0,
                     "长按已满未移动时选中不受影响");
            sw_report(fp, "  D 长按已满未移动，选中 %ld 项（应为 0）\n",
                      ListView_GetSelectedCount(lv));
            cn_drag_active = false;
            cn_press_reset();

            /*
             * --- E 按下前已选中 + 长按没满 + 小位移 = 单击，不冒框 ---
             *
             * 已选中项上「点一下」必须老老实实是单击：既不能冒框走③，
             * 也不能误触拖放走②。
             */
            cn_drag_active = true;
            cn_drag_from_blank = false;
            cn_press_was_selected = true;   /* 按下前该项已选中 */
            SET_HELD_NO();
            cn_drag_from_list = lv;
            cn_drag_start.x = 30;
            cn_drag_start.y = 20;
            if (cnt > 0)
                ListView_SetItemState(lv, 0, LVIS_SELECTED, LVIS_SELECTED);
            {
                int sel0 = ListView_GetSelectedCount(lv);

                SendMessageW(lv, WM_MOUSEMOVE, MK_LBUTTON, LP(33, 21));
                SW_CHECK(!cn_marquee_active, "已选中项单击不冒框");
                SW_CHECK(ListView_GetSelectedCount(lv) == sel0,
                         "已选中项单击不改变选中");
                sw_report(fp, "  E 已选中项单击，选中 %d 项保持不变\n",
                          sel0);
            }
            cn_drag_active = false;
            cn_press_reset();
            ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);

            /*
             * --- F 松手之后再移动，绝不冒框 ---
             *
             * 主人报的「单击结束后再移动，过了零点几秒突显框选」就是这条。
             * 复现方式：先布置一套完整的「按满 + 已选中」状态（模拟松手
             * 时 UP 丢在路上、标志残留），然后发一条不带 MK_LBUTTON 的
             * MOVE —— 物理键没按着，必须立刻把状态清干净且绝不出框。
             */
            cn_drag_active = true;
            cn_drag_from_blank = false;
            cn_press_was_selected = true;
            SET_HELD_YES();
            cn_drag_from_list = lv;
            cn_drag_start.x = 30;
            cn_drag_start.y = 20;

            SendMessageW(lv, WM_MOUSEMOVE, 0, LP(400, 200));
            SW_CHECK(!cn_marquee_active, "松手后移动不冒框（无 MK_LBUTTON）");
            SW_CHECK(!cn_drag_active, "松手后移动清掉拖拽状态");
            SW_CHECK(!cn_press_pending && !cn_longpress_static,
                     "松手后移动清掉长按状态");
            sw_report(fp, "  F 松手后移动，marquee=%d drag=%d press=%d\n",
                      (int)cn_marquee_active, (int)cn_drag_active,
                      (int)cn_press_pending);
            #undef SET_HELD_YES
            #undef SET_HELD_NO

            /*
             * 列表只有 1 项时，上面 A/B 圈到 1 项是天花板，证明不了
             * 「跨多行」。明确记一笔，免得又把「只有一行的目录」当成
             * 多项框选通过——上一轮就栽在空列表假通过上。
             */
            if (cnt < 2)
                sw_report(fp, "  注意：列表仅 %d 项，未能验证跨多行框选\n",
                          cnt);
            #undef LP
        } else {
            sw_report(fp, "  跳过：列表为空\n");
        }
    }

    sw_report(fp, "\n框选橡皮筋矩形：\n");
    {
        RECT r;

        cn_marquee_start.x = 10; cn_marquee_start.y = 20;
        cn_marquee_pt.x = 3;     cn_marquee_pt.y = 45;
        r = cn_marquee_rect();
        SW_CHECK(r.left == 3 && r.top == 20 && r.right == 10 &&
                 r.bottom == 45, "左上拖到右下：矩形规范化正确");

        cn_marquee_start.x = 1; cn_marquee_start.y = 2;
        cn_marquee_pt.x = 30;   cn_marquee_pt.y = 40;
        r = cn_marquee_rect();
        SW_CHECK(r.left == 1 && r.top == 2 && r.right == 30 &&
                 r.bottom == 40, "左上到右下正向拖动矩形正确");

        cn_marquee_start.x = 50; cn_marquee_start.y = 50;
        cn_marquee_pt.x = 50;    cn_marquee_pt.y = 50;
        r = cn_marquee_rect();
        SW_CHECK(r.left == 50 && r.top == 50 && r.right == 50 &&
                 r.bottom == 50, "零尺寸矩形退化为一点");
    }

    sw_report(fp, "\n冲突汇总弹窗（原始自绘模板）：\n");
    {
        /*
         * cn: 「确认覆盖」复用确认保存那套自绘模板后，这里验的是模板层
         * 的行为：数据怎么进去（ConflictItem → CnChange 纯名字行）、
         * 三个按钮怎么排（跳出 / 取消 / 全部覆盖，最右是默认且红框）、
         * 标题栏图标是否换成警告、清单折行折叠与浮层。
         *
         * 走 cn_confirm_batch 的真实映射路径不现实（它会跑模态循环、
         * 阻塞等用户点）。所以这里照 cn_confirm_batch 内部的做法构造
         * 同一份 CnChangeList 与 CnConfirmTexts，再直接调
         * cn_confirm_save_build —— 与生产代码同一套模板、同一组参数，
         * 差别只在没跑消息循环。
         */
        SftpWin *sw = the_sftpwin;
        static const wchar_t *five[] = {
            L"a.txt", L"b.txt", L"c.txt", L"d.txt", L"e.txt"
        };
        static const wchar_t *two[] = { L"one.txt", L"two.txt" };
        CnChange cl_items[5];
        CnChangeList cl;
        CnConfirmTexts texts;
        ConfirmSaveBox cb;
        bool built;

        memset(&cb, 0, sizeof(cb));
        memset(&texts, 0, sizeof(texts));

        /* 与 cn_confirm_batch 完全一致的一组参数：五项冲突。 */
        for (size_t i = 0; i < 5; i++) {
            cl_items[i].name = dup_wc_to_mb(DEFAULT_CODEPAGE, five[i], "?");
            cl_items[i].oldval = dupstr("");
            cl_items[i].newval = dupstr("");
            cl_items[i].kind = CN_CHG_OTHER;
            /*
             * 与 cn_overwrite_ask 一致：行首一律中性圆点，不按目录 /
             * 文件区分（主人 2026-11 决定，危险度只由标题栏图标表达）。
             */
            cl_items[i].icon_id = IDI_CN_DOT;
        }
        cl.items = cl_items;
        cl.n = 5;

        texts.title = "确认覆盖";
        texts.head = "5 个同名项目已存在于目标位置：";
        texts.ok = "跳过已存在";
        texts.cancel = "取消";
        texts.default_on_right = true;
        texts.third = "全部覆盖";
        texts.icon_id = IDI_CN_WARN;
        texts.third_is_danger = true;

        built = cn_confirm_save_build(sw->hwnd, NULL, &cl, &texts, &cb);
        SW_CHECK(built, "确认覆盖弹窗（新模板）创建成功");
        if (built) {
            wchar_t t[512];

            GetWindowTextW(cb.list, t, lenof(t));

            /*
             * 纯名字行：清单里是名字本身，不带「：」，也不带箭头。
             * 这两条正是 ConflictItem 适配成 CnChange 时最容易翻车的
             * 地方（有值才加冒号、有值才画箭头）。
             */
            SW_CHECK(wcsstr(t, L"a.txt") && wcsstr(t, L"b.txt"),
                     "清单里是冲突项名字");
            SW_CHECK(!wcsstr(t, L"a.txt："), "纯名字行不带冒号");

            /* 列表文本不该出现箭头符号（U+2192）。 */
            SW_CHECK(!wcsstr(t, L"\u2192"), "纯名字行不画箭头");

            /*
             * 折叠：最多显示 CN_CONFIRM_SAVE_MAX_ITEMS 项，超出另补
             * 一行「.......」。这与「确认保存」字符串路径的口径一致
             * （那边也是取前 3 项 + 一行省略）。
             *
             * ⚠️ 判据不能用 listtxt（cb.list 里的文本）：结构化路径下
             *    listtxt 是**有意保留全量**的 —— 它是浮层全文的来源，
             *    也是自检读回内容的载体。真正决定「画几项、画不画
             *    省略号」的是 cb.draw_n 与 cb.draw_dots（绘制端照单执行）。
             */
            SW_CHECK(wcsstr(t, L"a.txt") && wcsstr(t, L"e.txt"),
                     "清单文本保留全量（浮层数据源）");
            /*
             * 五项、每项占 1 行：显示前 3 项 + 一行省略号（共占 4 行，
             * 卡片高度自适应撑得住）。早先按「总行数预算 3 行、其中扣
             * 一格给省略号」算，结果只显示 2 项 —— 主人当场问「为啥只有
             * 两项就触发了省略」。口径已改为按项数，与确认保存对齐。
             */
            sw_report(fp, "  draw_n=%d draw_dots=%d（每项数上限 %d 项）\n",
                      (int)cb.draw_n, (int)cb.draw_dots,
                      CN_CONFIRM_SAVE_MAX_ITEMS);
            SW_CHECK(cb.draw_n == CN_CONFIRM_SAVE_MAX_ITEMS,
                     "按项数上限显示前三项");
            SW_CHECK(cb.draw_dots, "超出部分补「.......」行");

            /* 折叠时必须挂浮层，且浮层里是全部五项。 */
            SW_CHECK(cb.tip.tip != NULL, "折叠时挂上了浮层");
            SW_CHECK(cb.tip.text != NULL &&
                     wcsstr(cb.tip.text, L"a.txt") &&
                     wcsstr(cb.tip.text, L"d.txt") &&
                     wcsstr(cb.tip.text, L"e.txt"),
                     "浮层里是全部五项");

            /*
             * 三个按钮的文字与位置。
             *
             * 主人 2026-11 拍板：全部覆盖在最右并且是默认按钮。
             * 槽位对应：b_ok = 跳过已存在（左）、b_cancel = 取消（中）、
             * b_third = 全部覆盖（右）。
             */
            {
                wchar_t bt[64];

                GetWindowTextW(cb.b_ok, bt, lenof(bt));
                SW_CHECK(!wcscmp(bt, L"跳过已存在"), "左按钮为「跳过已存在」");
                GetWindowTextW(cb.b_cancel, bt, lenof(bt));
                SW_CHECK(!wcscmp(bt, L"取消"), "中按钮为「取消」");
                SW_CHECK(cb.b_third != NULL, "第三个按钮存在");
                if (cb.b_third) {
                    GetWindowTextW(cb.b_third, bt, lenof(bt));
                    SW_CHECK(!wcscmp(bt, L"全部覆盖"),
                             "右按钮为「全部覆盖」");
                }
            }

            /* 位置：从左到右必须是 跳过 / 取消 / 全部覆盖。 */
            if (cb.b_third) {
                RECT r1, r2, r3;

                GetWindowRect(cb.b_ok, &r1);
                GetWindowRect(cb.b_cancel, &r2);
                GetWindowRect(cb.b_third, &r3);
                SW_CHECK(r1.left < r2.left && r2.left < r3.left,
                         "按钮从左到右为 跳过/取消/全部覆盖");
                SW_CHECK(r2.left - r1.left > cn_dpi(6) &&
                         r3.left - r2.left > cn_dpi(6),
                         "三枚按钮彼此分立不重叠");
            }

            /*
             * 默认按钮 = 最右那个（「全部覆盖」）。
             *
             * cn: 危险动作走 BS_OWNERDRAW 画红框，红框与蓝框（DEFPUSH）
             * 不能并存，所以这里不能断言 BS_DEFPUSHBUTTON 样式位。
             * 真正的判据是「回车落到哪个按钮」—— 那由
             * cn_confirm_dialog_texts 的循环按 cb.b_third 决定，与
             * 样式位无关。自检里能验的是「模板把 b_third 认成默认」：
             * third 非 NULL 即最右为默认，这一条由上面的位置断言
             * 加上本处的红框归属共同覆盖。
             */
            SW_CHECK(cb.b_danger == cb.b_third,
                     "红框（危险动作）挂在最右的「全部覆盖」上");

            /* 标题栏图标必须是警告图标，不是红色问号。 */
            {
                /*
                 * cn: WM_GETICON 拿回来的句柄与资源 ID 无法直接比对，
                 * 所以改为验「图标拿得到」+ 用同一资源再 Load 一次
                 * 比尺寸。资源 ID 是否正确由 cn_title_icon 的缓存
                 * 逻辑保证（按 ID 分槽），这里至少保证没退化成 NULL。
                 */
                HICON got = (HICON)SendMessageW(cb.dlg, WM_GETICON,
                                                ICON_SMALL, 0);
                SW_CHECK(got != NULL, "标题栏挂上了图标");
            }

            /*
             * 内容区不再有大问号：弹出「原始自绘模板」后，卡片里只有
             * 清单文字（勾叉/圆点是 DrawIconEx 画的，不是控件）。
             * 模板自身仍有 head / list 两个 STATIC（list 不可见）。
             */
            SW_CHECK(cb.qicon == NULL, "卡片里不再有大问号控件");

            DestroyWindow(cb.dlg);
            cn_confirm_save_release(&cb);
        }

        /* 两项：不折叠、不挂浮层。 */
        for (size_t i = 0; i < 5; i++) {
            sfree(cl_items[i].name);
            sfree(cl_items[i].oldval);
            sfree(cl_items[i].newval);
        }
        for (size_t i = 0; i < 2; i++) {
            cl_items[i].name = dup_wc_to_mb(DEFAULT_CODEPAGE, two[i], "?");
            cl_items[i].oldval = dupstr("");
            cl_items[i].newval = dupstr("");
            cl_items[i].kind = CN_CHG_OTHER;
            cl_items[i].icon_id = IDI_CN_DOT;
        }
        cl.n = 2;
        memset(&cb, 0, sizeof(cb));
        texts.head = "2 个同名项目已存在于目标位置：";

        built = cn_confirm_save_build(sw->hwnd, NULL, &cl, &texts, &cb);
        SW_CHECK(built, "两项冲突时弹窗创建成功");
        if (built) {
            wchar_t t[512];
            GetWindowTextW(cb.list, t, lenof(t));
            SW_CHECK(wcsstr(t, L"one.txt") && wcsstr(t, L"two.txt") &&
                     !wcsstr(t, L"......."),
                     "两项时不折叠、不出省略号");
            SW_CHECK(cb.tip.tip == NULL, "未折叠时不挂浮层");
            DestroyWindow(cb.dlg);
            cn_confirm_save_release(&cb);
        }
        for (size_t i = 0; i < 2; i++) {
            sfree(cl_items[i].name);
            sfree(cl_items[i].oldval);
            sfree(cl_items[i].newval);
        }
    }

    /*
     * 真实扫描：对着测试桩走一趟，确认「已存在的挑出来、不存在的放过」。
     *
     * 上面那段验的是弹窗长相，弹窗长得对不代表扫描没瞎猜 —— 万一
     * cn_collect_conflicts 把所有项都当冲突呢？所以这里拿桩里真实存在的
     * 名字（sftp_rtest 目录、top.txt 文件）和一个根本不存在的名字混在
     * 一起扫，看它只挑出前两个。
     */
    sw_report(fp, "\n冲突扫描（真实目录）：\n");
    {
        SftpWin *sw = the_sftpwin;

        if (!sw->sx) {
            sw_report(fp, "  跳过  未连上远端，无法验证扫描\n");
        } else {
            char *names[3];
            bool dirs[3];
            size_t nconf = 0;
            ConflictItem *conf;

            names[0] = dupstr("sftp_rtest");     /* 桩里确实有 */
            names[1] = dupstr("top.txt");        /* 在 sftp_rtest 里，问的是当前目录 */
            names[2] = dupstr("no-such-item-9f3a");
            dirs[0] = true;
            dirs[1] = false;
            dirs[2] = false;

            conf = cn_collect_conflicts(sw, names, dirs, 3, false, &nconf);

            /*
             * 当前远端目录是桩的根，根下只有 sftp_rtest；top.txt 在它
             * 里面，所以按当前目录问，只有 sftp_rtest 算存在。
             */
            SW_CHECK(nconf == 1, "只挑出真正存在的项（1 个）");
            if (nconf == 1) {
                SW_CHECK(!wcscmp(conf[0].name, L"sftp_rtest"),
                         "挑出来的正是 sftp_rtest");
                SW_CHECK(conf[0].is_dir, "目录标记正确传递");
            }
            /* 不存在的名字不得混进来。 */
            {
                bool leaked = false;
                for (size_t i = 0; i < nconf; i++)
                    if (wcsstr(conf[i].name, L"no-such-item-9f3a"))
                        leaked = true;
                SW_CHECK(!leaked, "不存在的项没有被误判为冲突");
            }

            cn_conflicts_free(conf, nconf);
            for (size_t i = 0; i < 3; i++)
                sfree(names[i]);
        }
    }

    /*
     * 两侧选中互斥：点这一侧必须把对侧选中清掉。
     *
     * 缺陷现场：选中本地一项后去点远端列表，本地那行因为
     * LVS_SHOWSELALWAYS 仍画成灰底，上传按钮也还亮着，用户看到的
     * 是「两边都选着」而不是「换边了」。
     *
     * 这里直接调 cn_drop_other_side_selection —— 它是 WM_LBUTTONDOWN
     * 与 WM_SETFOCUS 两条路径共用的唯一实现，自检调它就等于验了逻辑
     * 本身（真发 DOWN 会卡死，见 cn_list_point_on_item 的注释）。
     */
    sw_report(fp, "\n两侧选中互斥：\n");
    {
        SftpWin *sw = the_sftpwin;

        if (ListView_GetItemCount(sw->local_list) > 0 &&
            ListView_GetItemCount(sw->remote_list) > 0) {
        /* 本地选中 -> 远端应被清空 */
        ListView_SetItemState(sw->local_list, 0, LVIS_SELECTED,
                              LVIS_SELECTED);
        ListView_SetItemState(sw->remote_list, 0, LVIS_SELECTED,
                              LVIS_SELECTED);
        cn_drop_other_side_selection(sw->remote_list);
        SW_CHECK(ListView_GetNextItem(sw->local_list, -1, LVNI_SELECTED) < 0,
                 "点远端后本地选中被清空");
        SW_CHECK(ListView_GetNextItem(sw->remote_list, -1, LVNI_SELECTED) >= 0,
                 "点远端后远端自己的选中保留");

        /* 反向：远端选中 -> 本地应被清空 */
        ListView_SetItemState(sw->remote_list, 0, LVIS_SELECTED,
                              LVIS_SELECTED);
        ListView_SetItemState(sw->local_list, 0, LVIS_SELECTED,
                              LVIS_SELECTED);
        cn_drop_other_side_selection(sw->local_list);
        SW_CHECK(ListView_GetNextItem(sw->remote_list, -1, LVNI_SELECTED) < 0,
                 "点本地后远端选中被清空");
        SW_CHECK(ListView_GetNextItem(sw->local_list, -1, LVNI_SELECTED) >= 0,
                 "点本地后本地自己的选中保留");

        /* 清场，别把状态漏给后面的检查 */
        ListView_SetItemState(sw->local_list, -1, 0, LVIS_SELECTED);
        ListView_SetItemState(sw->remote_list, -1, 0, LVIS_SELECTED);
        cn_update_transfer_buttons(sw);
        } else {
            sw_report(fp, "  跳过  某一侧列表为空\n");
        }
    }

    /*
     * 传输结果提示条（toast）的文案与停留时长。
     *
     * 文案逐个穷举，因为出过一个很隐蔽的错：普通上传（目标端没有同名
     * 文件、压根没弹过冲突窗）被提示成「全部覆盖：已重新上传」。根因是
     * 拿 batch_policy 的默认值当判据，而它初值正是「覆盖」。所以这里必须
     * 有一条「没问过就绝不许出现覆盖字样」的反向断言盯着。
     */
    sw_report(fp, "\n传输结果提示条：\n");
    {
        char *t;

        t = cn_toast_xfer_text(true, 3, 0, false);
        SW_CHECK(t && strstr(t, "全部覆盖") == NULL &&
                 strstr(t, "已上传 3 项") != NULL,
                 "普通上传不出现「全部覆盖」，文案为「已上传 3 项到远端」");
        sfree(t);

        t = cn_toast_xfer_text(false, 2, 0, false);
        SW_CHECK(t && strstr(t, "全部覆盖") == NULL &&
                 strstr(t, "已从远端下载 2 项") != NULL,
                 "普通下载文案为「已从远端下载 2 项」");
        sfree(t);

        t = cn_toast_xfer_text(true, 3, 0, true);
        SW_CHECK(t && strstr(t, "全部覆盖") != NULL &&
                 strstr(t, "已重新上传 3 项") != NULL,
                 "问过冲突且选覆盖时才显示「全部覆盖」");
        sfree(t);

        t = cn_toast_xfer_text(false, 1, 0, true);
        SW_CHECK(t && strstr(t, "全部覆盖") != NULL &&
                 strstr(t, "已重新下载 1 项") != NULL,
                 "下载侧覆盖文案正确");
        sfree(t);

        t = cn_toast_xfer_text(true, 0, 4, false);
        SW_CHECK(t && strstr(t, "已跳过 4 项") != NULL &&
                 strstr(t, "全部覆盖") == NULL,
                 "全跳过时提示「已跳过 4 项」");
        sfree(t);

        t = cn_toast_xfer_text(true, 2, 5, false);
        SW_CHECK(t && strstr(t, "已上传 2 项") != NULL &&
                 strstr(t, "跳过 5 项") != NULL,
                 "混合情形同时交代上传数与跳过数");
        sfree(t);

        t = cn_toast_xfer_text(true, 0, 0, false);
        SW_CHECK(t == NULL, "一项没动时不生成提示文案");
        sfree(t);

        /*
         * 停留时长：主人要求「显示时间太长，砍短一半」。
         *
         * 原来 190 帧 ≈ 3.0 秒，现在 95 帧 ≈ 1.5 秒。上下界都卡住，免得
         * 以后谁随手改数字把它调到一闪而过或者又拖回三秒。
         */
        SW_CHECK(CN_TOAST_HOLD_FRAMES * CN_TOAST_FRAME_MS == 1520,
                 "停留时长为 1.52 秒（3.0 秒的一半）");
    }

    sw_report(fp, "\n小计：通过 %d 项，失败 %d 项\n", pass, fail);
    sw_report(fp, "完成\n");
    fclose(fp);

    if (the_sftpwin)
        DestroyWindow(the_sftpwin->hwnd);

#undef SW_CHECK
}

/* ----------------------------------------------------------------------
 * 「确认保存」弹窗自检：putty -confirm-save
 *
 * 只建窗、不跑循环，量出真实几何后销毁。覆盖主人几轮反馈里那几条
 * 纯视觉要求 —— 这类问题截图看不准，用像素断言才靠得住：
 *   1. 窗口宽度落在 300~450px 区间（默认 300，最宽 450，超宽折行）；
 *   2. 两按钮相邻成组、整组居中；
 *   3. 清单折叠成「.......」时挂上浮层，浮层配色是 #171b23 底 + 白字。
 */
void sftp_selftest_confirm_save(void)
{
    char *outpath = sftp_selftest_outpath("confirm-save-selftest.txt");
    FILE *fp = fopen(outpath, "w");
    sfree(outpath);
    int pass = 0, fail = 0;
    /* 五行改动：确保被折叠（上限三行），浮层才会挂上。 */
    const char *changes =
        "【启用终端指定 ANSI 颜色】 开 → 关\n"
        "【启用终端使用 xterm 256 色模式】 开 → 关\n"
        "【启用终端使用 24 位色】 开 → 关\n"
        "【窗口标题】 t9 → 1\n"
        "【主机名】 a → b";
    /*
     * 边界用例：极短（应被 300px 下限托住）与超长（应封顶 450px 并折行）。
     * 这两种极端以前没测过 —— 宽度公式改了几轮，全靠它们证明区间真的生效。
     */
    const char *changes_short = "【主机名】 a → b";
    const char *changes_long =
        "【启用终端使用 xterm 256 色模式并且把配色方案改成 Solarized Dark "
        "同时把字体换成等宽字体并调整行间距】 开 → 关";

#define SW_CHECK(cond, desc) do {                                   \
        if (cond) { sw_report(fp, "  通过  %s\n", desc); pass++; }   \
        else { sw_report(fp, "  失败  %s\n", desc); fail++; }        \
    } while (0)

    if (!fp)
        return;

    sw_report(fp, "[确认保存弹窗自检]\n");

    {
        ConfirmSaveBox cb;
        RECT wr, cr, lr, hr, r1, r2;

        if (!cn_confirm_save_build(NULL, changes, NULL, NULL, &cb)) {
            sw_report(fp, "  失败  弹窗未建成\n");
            fclose(fp);
            return;
        }

        GetWindowRect(cb.dlg, &wr);
        GetClientRect(cb.dlg, &cr);
        GetWindowRect(cb.list, &lr);
        GetWindowRect(cb.head, &hr);
        MapWindowPoints(HWND_DESKTOP, cb.dlg, (POINT *)&lr, 2);
        MapWindowPoints(HWND_DESKTOP, cb.dlg, (POINT *)&hr, 2);

        sw_report(fp, "  客户区 %dx%d，清单区 (%d,%d)-(%d,%d)\n",
                  cr.right, cr.bottom, lr.left, lr.top, lr.right, lr.bottom);
        /* 诊断：把清单控件的实际文本宽也量一遍，便于判断窗口为何这么宽。 */
        {
            wchar_t buf[512];
            HDC hdc = GetDC(NULL);
            GetWindowTextW(cb.list, buf, lenof(buf));
            sw_report(fp, "  清单文本宽实测：");
            if (hdc) {
                HFONT old = SelectObject(hdc, cb.font);
                const wchar_t *p = buf;
                while (*p) {
                    const wchar_t *nl = wcschr(p, L'\n');
                    int n = nl ? (int)(nl - p) : (int)wcslen(p);
                    SIZE s2;
                    if (n > 0) {
                        GetTextExtentPoint32W(hdc, p, n, &s2);
                        sw_report(fp, "%d ", s2.cx);
                    }
                    p = nl ? nl + 1 : p + n;
                    if (!nl)
                        break;
                }
                SelectObject(hdc, old);
                ReleaseDC(NULL, hdc);
            }
            sw_report(fp, "px\n");
        }

        /*
         * 1) 窗口宽度必须落在主人指定的 300~450px 区间（逻辑值经 DPI 折算），
         *    且内容右缘不能被右边框裁掉。
         *
         * ⚠️ 旧断言要求「右距恰好 50px」，那条规格已被主人作废（改成
         *    默认 300 / 最宽 450 + 自动折行）。现在右距由宽度决定，是
         *    结果而不是约束：内容窄时窗口撑到 300 下限，右距自然变大；
         *    内容宽时封顶 450 靠折行消化。所以这里判区间与不裁剪。
         */
        {
            int gap = cr.right - lr.right;
            int wlo = cn_dpi(300), whi = cn_dpi(450);
            sw_report(fp, "  客户区宽 %dpx（目标区间 %d~%dpx），内容右距 %dpx\n",
                      cr.right, wlo, whi, gap);
            SW_CHECK(cr.right >= wlo && cr.right <= whi,
                     "窗口宽度在 300~450px 区间内");
            SW_CHECK(gap >= 0, "内容右缘未被右边框裁掉");
        }

        /*
         * 2) 清单 STATIC 已改为不可见的文本载体（文字由自绘按段上色），
         *    它的控件坐标不再代表屏幕上文字的位置 —— 所以不能拿它的
         *    左缘去判「在图标右侧」。
         *
         *    真正该判的是自绘几何本身：卡片左边距 + icon_sz + 8 必须
         *    落在卡片内部，且留有足够空间画文字。这是「图标在左、
         *    文字在右」在代码层面的等价断言。
         */
        {
            int icon_x = cb.card_left + cn_dpi(14);
            int text_x = icon_x + cb.icon_sz + cn_dpi(8);
            int text_room = cb.card_right - cn_dpi(14) - text_x
                            - cb.chev_sz - cn_dpi(12);
            sw_report(fp, "  勾叉左缘 %d，文字起 %d，文字可用宽 %d，"
                      "卡片 %d..%d\n", icon_x, text_x, text_room,
                      cb.card_left, cb.card_right);
            SW_CHECK(hr.left == cb.card_left, "首行说明顶格对齐卡片左缘");
            SW_CHECK(icon_x >= cb.card_left + cn_dpi(8) &&
                     text_x > icon_x && text_room >= cn_dpi(80),
                     "勾叉在左、文字在右且留有可读宽度");
        }

        /*
         * 2a) 勾叉图标在每一行内垂直居中。
         *
         * 新版图标是自绘的，画在行中线（cy = 行顶 + row_h/2）。
         * 行高由 row_h 决定，所以「每行图标居中」在几何上等价于
         * 「卡片高度 = 行数 × row_h + 上下内边距」，且行高必须大于
         * 图标直径（否则图标会互相压）。
         */
        {
            int pad_top = lr.top - cb.card_top;
            int pad_bot = cb.card_bottom - lr.bottom;
            sw_report(fp, "  卡片 %d..%d，清单 %d..%d，上下内边距 %d/%d，"
                      "行高 %d，图标 %d\n", cb.card_top, cb.card_bottom,
                      lr.top, lr.bottom, pad_top, pad_bot,
                      cb.row_h, cb.icon_sz);
            SW_CHECK(cb.row_h > cb.icon_sz, "行高大于勾叉直径（图标不互相压）");
            SW_CHECK(pad_top >= 0 && pad_bot >= 0, "卡片完整包住清单");
            SW_CHECK(pad_top <= cb.row_h, "清单顶不超出卡片顶一行");
        }

        /*
         * 2b) 弹窗固定居中于屏幕工作区（主人指定，不再跟随鼠标）。
         *
         * 容差 2px：窗口宽高为奇数时 (sw-w)/2 取整会有 1px 偏差，
         * 再叠加 DPI 折算，给到 2px 足够严了 —— 居鼠标时偏差是几十上百像素，
         * 不会因为这点容差而误判通过。
         */
        {
            RECT wa, wr2;
            if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0) &&
                GetWindowRect(cb.dlg, &wr2)) {
                int cx_win = (wr2.left + wr2.right) / 2;
                int cy_win = (wr2.top + wr2.bottom) / 2;
                int cx_scr = (wa.left + wa.right) / 2;
                int cy_scr = (wa.top + wa.bottom) / 2;
                int dx = cx_win - cx_scr, dy = cy_win - cy_scr;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                sw_report(fp, "  弹窗中心 (%d,%d)，工作区中心 (%d,%d)，偏差 %d/%d px\n",
                          cx_win, cy_win, cx_scr, cy_scr, dx, dy);
                SW_CHECK(dx <= 2 && dy <= 2, "弹窗居中于屏幕");
            }
        }

        /*
         * 3) 两个按钮：左留白 = 按钮间距 = 右留白（主人 2026-11 要求）。
         *
         * 旧断言写死「间距 = 标准 8px + 整组居中」，那是上一版的做法。
         * 现在改成由窗口宽反解间距，三处等分，所以断言也跟着改：量三
         * 个距离，两两相差不超过 2px（像素取整允许的误差）。
         */
        GetWindowRect(cb.b_ok, &r1);
        GetWindowRect(cb.b_cancel, &r2);
        {
            /* 客户区左缘在屏幕坐标系里的位置。 */
            int border = (wr.right - wr.left - cr.right) / 2;
            int client_l = wr.left + border;
            int client_r = wr.right - border;
            int gap = r2.left - r1.right;
            int left_pad = r1.left - client_l;
            int right_pad = client_r - r2.right;
            sw_report(fp, "  按钮间距 %dpx，左右留白 %d/%d px\n",
                      gap, left_pad, right_pad);
            SW_CHECK(gap - left_pad <= 2 && gap - left_pad >= -2,
                     "按钮间距与左留白相等");
            SW_CHECK(gap - right_pad <= 2 && gap - right_pad >= -2,
                     "按钮间距与右留白相等");
            SW_CHECK(r1.top == r2.top, "两按钮同顶");
            SW_CHECK((r1.bottom - r1.top) == (r2.bottom - r2.top),
                     "两按钮同高");
        }

        /* 4) 被折叠时必须挂浮层，且浮层文本含未显示的那几行。 */
        SW_CHECK(cb.tip.tip != NULL, "改动超三行时挂上了 tooltip");
        SW_CHECK(cb.tip.text != NULL &&
                 wcsstr(cb.tip.text, L"窗口标题") &&
                 wcsstr(cb.tip.text, L"主机名"),
                 "tooltip 文本包含全部改动（含未显示的两行）");

        /*
         * 5) 浮层配色：#171b23 底 + 白字。
         *
         * ⚠️ 不能拿窗口 DC 的 GetPixel 判：WS_POPUP 未显示（或摆到屏幕外）
         *    时窗口没有有效表面，读回来恒为 0xFFFFFF，据此判「配色错」
         *    就是假失败（实测踩过）。改用 WM_PRINTCLIENT 把窗口画到内存
         *    位图上再读 —— 走的是同一个 WndProc，不依赖窗口是否可见。
         */
        if (cb.tip.tip) {
            HWND tip = cb.tip.tip;
            RECT tr;
            HDC memdc = NULL;
            HBITMAP membmp = NULL, oldbmp = NULL;

            /*
             * 4b) 浮层位置（主人指定）：在清单右侧，且上框线与内容区
             *     上框线齐平。
             *
             * 触发 cn_tip_place 之前浮层还没摆过位，坐标不可信 —— 先手动
             * 调一次（模拟鼠标移入），再量真实屏幕矩形。
             */
            {
                RECT wr_dlg, wr_tip, wr_head, wa;
                bool room_right;
                cn_tip_place(&cb.tip);
                GetWindowRect(cb.dlg, &wr_dlg);
                GetWindowRect(tip, &wr_tip);
                GetWindowRect(cb.head, &wr_head);

                /*
                 * 右侧空间不足时浮层会翻到左侧（防止出屏），那是正确行为，
                 * 不能拿「必须在右边」去判。先算屏幕右缘还够不够。
                 */
                room_right = true;
                if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
                    int tipw = wr_tip.right - wr_tip.left;
                    room_right = (wr_dlg.right + cn_dpi(8) + tipw <= wa.right);
                }
                sw_report(fp, "  浮层 (%d,%d)-(%d,%d)，弹窗右缘 %d，首行顶 %d，%s\n",
                          wr_tip.left, wr_tip.top, wr_tip.right, wr_tip.bottom,
                          wr_dlg.right, wr_head.top,
                          room_right ? "右侧有空间" : "右侧不够已翻左");
                if (room_right)
                    SW_CHECK(wr_tip.left >= wr_dlg.right, "浮层在弹窗右侧");
                else
                    SW_CHECK(wr_tip.right <= wr_dlg.left, "右侧不足时浮层翻到左侧");
                {
                    /*
                     * cn: 浮层锚点是卡片顶（tip.trigger_top = card_top），
                     * 不是首行说明顶。旧断言拿首行顶比，差出来的正好是
                     * 「首行高 + 间距」那 27px，属于量错对象。
                     */
                    POINT anchor;
                    int dy;
                    anchor.x = cb.card_left;
                    anchor.y = cb.card_top;
                    ClientToScreen(cb.dlg, &anchor);
                    dy = wr_tip.top - anchor.y;
                    if (dy < 0) dy = -dy;
                    sw_report(fp, "  浮层顶边与卡片顶边差 %dpx\n", dy);
                    SW_CHECK(dy <= 2, "浮层上框线与白卡片上框线齐平");
                }
            }

            GetClientRect(tip, &tr);
            {
                HDC screen = GetDC(NULL);
                if (screen) {
                    memdc = CreateCompatibleDC(screen);
                    if (memdc)
                        membmp = CreateCompatibleBitmap(screen, tr.right,
                                                        tr.bottom);
                    ReleaseDC(NULL, screen);
                }
            }
            if (memdc && membmp) {
                oldbmp = SelectObject(memdc, membmp);
                /* PRF_CLIENT 让系统把 WM_PRINTCLIENT 发给本窗。 */
                SendMessageW(tip, WM_PRINTCLIENT, (WPARAM)memdc,
                             PRF_CLIENT | PRF_ERASEBKGND);

                {
                    COLORREF bg = GetPixel(memdc, 2, tr.bottom - 3);
                    COLORREF bg2 = GetPixel(memdc, tr.right - 3, tr.bottom / 2);
                    sw_report(fp, "  浮层底色 0x%06X / 0x%06X（目标 0x171B23）\n",
                              (unsigned)bg, (unsigned)bg2);
                    SW_CHECK(bg == CN_TIP_BG && bg2 == CN_TIP_BG,
                             "浮层底色为 #171B23");
                }
                /*
                 * 白字：黑底上逐点找接近纯白的像素。先确认底色真是黑的
                 * 再找白字才有意义 —— 底色错时这条必然不可信。
                 */
                {
                    int found = 0;
                    for (int yy = cn_dpi(6); yy < tr.bottom - cn_dpi(6) && !found; yy++)
                        for (int xx = cn_dpi(8); xx < tr.right - cn_dpi(8); xx++) {
                            COLORREF c = GetPixel(memdc, xx, yy);
                            if (GetRValue(c) > 200 && GetGValue(c) > 200 &&
                                GetBValue(c) > 200) {
                                found = 1;
                                break;
                            }
                        }
                    SW_CHECK(found, "浮层文字为白色（黑底上找到白点）");
                }

                SelectObject(memdc, oldbmp);
            } else {
                sw_report(fp, "  跳过  建不出内存位图，配色检查跳过\n");
            }
            if (membmp)
                DeleteObject(membmp);
            if (memdc)
                DeleteDC(memdc);
        }

        DestroyWindow(cb.dlg);
        /*
         * 6) 销毁弹窗不许牵连整个进程。
         *
         * 回归断言：WM_NCDESTROY 里曾有一句 PostQuitMessage(0)，往线程
         * 消息队列塞退出指令，点「确定」「取消」都会让 putty 整个退出。
         * 窗口已销毁，这里查队列里有没有被塞进 WM_QUIT（PeekMessage 用
         * PM_REMOVE 取走它，避免污染后续自检）。
         */
        {
            MSG qm;
            BOOL got_quit = PeekMessageW(&qm, NULL, WM_QUIT, WM_QUIT,
                                         PM_REMOVE);
            SW_CHECK(!got_quit, "销毁弹窗不向消息队列投递 WM_QUIT");
        }
        cn_confirm_save_release(&cb);
    }

    /*
     * 边界用例：极短内容必须被 300px 下限托住（不能缩到按钮都放不下），
     * 超长内容必须封顶 450px 并折行（不能横向撑破窗口）。
     */
    {
        const struct {
            const char *label;
            const char *text;
        } cases[] = {
            { "极短内容（下限 300px）", changes_short },
            { "超长内容（上限 450px + 折行）", changes_long },
        };
        for (size_t ci = 0; ci < lenof(cases); ci++) {
            ConfirmSaveBox cb2;
            RECT c2, l2;
            sw_report(fp, "\n[%s]\n", cases[ci].label);
            if (!cn_confirm_save_build(NULL, cases[ci].text, NULL, NULL, &cb2)) {
                sw_report(fp, "  失败  弹窗未建成\n");
                fail++;
                continue;
            }
            GetClientRect(cb2.dlg, &c2);
            GetWindowRect(cb2.list, &l2);
            MapWindowPoints(HWND_DESKTOP, cb2.dlg, (POINT *)&l2, 2);
            sw_report(fp, "  客户区 %dx%d，清单区高 %dpx\n",
                      c2.right, c2.bottom, l2.bottom - l2.top);
            SW_CHECK(c2.right >= cn_dpi(300) && c2.right <= cn_dpi(450),
                     "宽度落在 300~450px");
            SW_CHECK(l2.right <= c2.right && l2.bottom <= c2.bottom,
                     "清单区未超出客户区");
            /* 折行有效性：超长条目若没折行，清单高度会只有一行。 */
            if (ci == 1) {
                int lh_est = c2.bottom > 0 ? (l2.bottom - l2.top) : 0;
                sw_report(fp, "  折行后清单高 %dpx\n", lh_est);
                SW_CHECK(lh_est > cn_dpi(20), "超长内容已折行（高度多于一行）");

                /*
                 * 断点必须在「】」之后：读回控件文本，确认存在一行以
                 * 「】」结尾、且下一行是改动内容（含「→」）。
                 * 若 STATIC 按字符硬折，「】」会被切到行中间，这条就挂。
                 */
                {
                    wchar_t buf[1024];
                    int split_ok = 0;
                    GetWindowTextW(cb2.list, buf, lenof(buf));
                    {
                        const wchar_t *q = buf;
                        while (*q) {
                            const wchar_t *nl2 = wcschr(q, L'\n');
                            int n2 = nl2 ? (int)(nl2 - q) : (int)wcslen(q);
                            /*
                             * 文本是 \r\n 分隔，行尾会先遇到 \r，得先剔掉
                             * 再判断最后一个字符是不是「】」。早先漏了这步，
                             * 断言恒不成立（q[n2-1] 永远是 L'\r'）。
                             */
                            while (n2 > 0 && (q[n2-1] == L'\r' ||
                                              q[n2-1] == L'\n'))
                                n2--;
                            /* 本行以「】」结尾？ */
                            int ends_bracket =
                                (n2 > 0 && q[n2-1] == L'\u3011');
                            /* 下一行含「→」？ */
                            int next_arrow = 0;
                            if (nl2) {
                                const wchar_t *q2 = nl2 + 1;
                                const wchar_t *nl3 = wcschr(q2, L'\n');
                                int n3 = nl3 ? (int)(nl3 - q2)
                                             : (int)wcslen(q2);
                                for (int k = 0; k < n3; k++)
                                    if (q2[k] == L'\u2192') {
                                        next_arrow = 1;
                                        break;
                                    }
                            }
                            if (ends_bracket && next_arrow) {
                                split_ok = 1;
                                sw_report(fp,
                                          "  断点正确：「】」结尾行 + 下一行含「→」\n");
                                break;
                            }
                            q = nl2 ? nl2 + 1 : NULL;
                        }
                    }
                    SW_CHECK(split_ok, "超长行在「】」之后断开，未拦腰切断");
                }
            }
            DestroyWindow(cb2.dlg);
            cn_confirm_save_release(&cb2);
        }
    }

    sw_report(fp, "\n小计：通过 %d 项，失败 %d 项\n", pass, fail);
    sw_report(fp, "完成\n");
    fclose(fp);

#undef SW_CHECK
}

/*
 * cn: 「退出」按钮的未保存改动拦截自检。
 *
 * 背景：配置框的「退出」按钮控件 ID 被映射成系统 IDCANCEL
 * （controls.c 里 button.iscancel 的处理），winctrl_handle_command
 * 对按钮点击返回 false，于是消息落到 Windows 对话框管理器的默认
 * 处理上直接关窗 —— 未保存的改动被无声丢弃。config.c 的
 * sessionsaver_handler 现在拦一道，走 dlg_confirm_quit。
 *
 * 这里验的是判据层：无改动必须直接放行（不弹窗、不卡住自检），
 * 有改动必须判为「需要确认」。模态弹窗本身点不了，所以只验
 * dlg_has_unsaved_changes 的结论，弹窗外观由「确认保存」那套
 * 断言覆盖（同一个 message_box 实现）。
 */
void sftp_selftest_confirm_quit(void)
{
    char *outpath = sftp_selftest_outpath("confirm-quit-selftest.txt");
    FILE *fp = fopen(outpath, "w");
    sfree(outpath);
    int pass = 0, fail = 0;

#define SW_CHECK(cond, desc) do {                                   \
        if (cond) { sw_report(fp, "  通过  %s\n", desc); pass++; }   \
        else { sw_report(fp, "  失败  %s\n", desc); fail++; }        \
    } while (0)

    if (!fp)
        return;

    sw_report(fp, "[退出按钮未保存改动自检]\n");

    {
        Conf *live = conf_new();
        Conf *base = conf_new();
        struct dlgparam dp;

        /*
         * conf_new() 只给一棵空树，任何 conf_get_* 都会撞断言
         * （utils/conf.c 里 assert(entry)）。真实配置框打开时是靠
         * load_settings / do_defaults 把每一项填满的，这里照做。
         */
        do_defaults(NULL, live);
        do_defaults(NULL, base);

        memset(&dp, 0, sizeof(dp));
        dp.data = live;
        dp.baseline = base;
        dp.track_changes = true;

        /*
         * 一、刚打开、什么都没改：必须放行，且 dlg_confirm_quit
         * 不能弹窗（弹了自检就会卡在这里）。
         */
        SW_CHECK(!dlg_has_unsaved_changes(&dp),
                 "无改动时判为「没有未保存改动」");
        SW_CHECK(dlg_confirm_quit(&dp) == CN_QUIT_DISCARD,
                 "无改动时 dlg_confirm_quit 直接放行（不弹窗）");

        /*
         * 二、改一项非连接目标设置（登录信息不进清单，所以用
         * 终端颜色这类纯本地项）。
         */
        conf_set_bool(live, CONF_xterm_256_colour, false);
        conf_set_bool(base, CONF_xterm_256_colour, true);
        SW_CHECK(dlg_has_unsaved_changes(&dp),
                 "有改动时判为「存在未保存改动」");

        /*
         * 三、改动清单必须非空且点到该项，否则弹窗给不出信息。
         */
        {
            char *changes = conf_describe_changes(base, live);
            SW_CHECK(changes && *changes,
                     "改动清单非空（弹窗有内容可显示）");
            SW_CHECK(changes && strstr(changes, "256") != NULL,
                     "改动清单点名了被改的那一项");
            sfree(changes);
        }

        /*
         * 四、改回原值：live 比较应重新判为干净，退出不再打扰。
         */
        conf_set_bool(live, CONF_xterm_256_colour, true);
        SW_CHECK(!dlg_has_unsaved_changes(&dp),
                 "改回原值后重新判为无改动（不误报）");

        conf_free(live);
        conf_free(base);
    }

    /*
     * 五、版面：退出确认现在与「确认保存」同源，三个按钮
     * 「直接退出 / 取消 / 保存并退出」。模态弹窗点不了，所以建完
     * 直接量控件：文案靠 GetWindowTextW 读回，样式位看 BS_* 位。
     */
    {
        /* cn: 直接用线上那份文案（cn_quit_texts），验的就是真实配置。 */
        ConfirmSaveBox cb;

        sw_report(fp, "\n[三按钮版面]\n");
        if (cn_confirm_save_build(NULL, "【启用终端指定 ANSI 颜色】 开 → 关",
                                  NULL, &cn_quit_texts, &cb)) {
            wchar_t buf[64];
            DWORD st_first, st_second, st_third;

            GetWindowTextW(cb.dlg, buf, lenof(buf));
            /*
             * ⚠️ 别用 %ls 直接打宽字符：报告是窄字节流（fopen "w"），
             * 宽字符转换会失败，打出来是空串，看着像标题丢了。
             * 转成 UTF-8 再打。
             */
            {
                char *title_mb = dup_wc_to_mb(CP_UTF8, buf, "?");
                sw_report(fp, "  窗口标题实测「%s」\n", title_mb);
                sfree(title_mb);
            }
            SW_CHECK(wcscmp(buf, L"确认保存") == 0,
                     "窗口标题为「确认保存」");

            GetWindowTextW(cb.b_ok, buf, lenof(buf));
            SW_CHECK(wcscmp(buf, L"直接退出") == 0,
                     "左按钮文案为「直接退出」");

            GetWindowTextW(cb.b_cancel, buf, lenof(buf));
            SW_CHECK(wcscmp(buf, L"取消") == 0,
                     "中按钮文案为「取消」");

            SW_CHECK(cb.b_third != NULL,
                     "三按钮模式下第三个按钮已创建");
            if (cb.b_third) {
                GetWindowTextW(cb.b_third, buf, lenof(buf));
                SW_CHECK(wcscmp(buf, L"保存并退出") == 0,
                         "右按钮文案为「保存并退出」");
            }

            st_first = (DWORD)GetWindowLongPtrW(cb.b_ok, GWL_STYLE);
            st_second = (DWORD)GetWindowLongPtrW(cb.b_cancel, GWL_STYLE);
            st_third = cb.b_third
                ? (DWORD)GetWindowLongPtrW(cb.b_third, GWL_STYLE) : 0;
            sw_report(fp, "  样式位 %08lx / %08lx / %08lx\n",
                      (unsigned long)st_first, (unsigned long)st_second,
                      (unsigned long)st_third);
            SW_CHECK((st_first & BS_TYPEMASK) == BS_OWNERDRAW,
                     "「直接退出」是自绘按钮（红框）");
            SW_CHECK((st_third & BS_TYPEMASK) == BS_DEFPUSHBUTTON,
                     "「保存并退出」是默认按钮（蓝框）");
            SW_CHECK((st_second & BS_TYPEMASK) == BS_PUSHBUTTON,
                     "「取消」是普通按钮（无蓝框）");

            /*
             * 三个按钮同一水平线，且左留白 = 间距1 = 间距2 = 右留白。
             *
             * cn: 主人 2026-11 要求「按键之间的距离和按键与窗口左右边界
             * 的距离相等」。旧断言查的是「整组居中」（左右留白相等），
             * 与等分不是一回事 —— 等分还要求间距也等于留白。像素取整
             * 会有 1~2px 余数，容差给 2。
             */
            {
                RECT r1, r2, r3, rc;
                int gap1, gap2, left, right;
                GetWindowRect(cb.b_ok, &r1);
                GetWindowRect(cb.b_cancel, &r2);
                GetWindowRect(cb.b_third, &r3);
                GetClientRect(cb.dlg, &rc);
                MapWindowPoints(cb.dlg, NULL, (POINT *)&rc, 2);
                gap1 = r2.left - r1.right;
                gap2 = r3.left - r2.right;
                left = r1.left - rc.left;
                right = rc.right - r3.right;
                sw_report(fp, "  按钮间距 %d/%d px，左右留白 %d/%d px\n",
                          gap1, gap2, left, right);
                SW_CHECK(r1.top == r2.top && r2.top == r3.top,
                         "三按钮同顶");
                SW_CHECK(abs(gap1 - gap2) <= 2,
                         "三按钮间距一致");
                SW_CHECK(abs(left - gap1) <= 2,
                         "左留白等于按钮间距");
                SW_CHECK(abs(right - gap2) <= 2,
                         "右留白等于按钮间距");
            }

            cn_confirm_save_release(&cb);
        } else {
            SW_CHECK(false, "三按钮弹窗构建成功");
        }
    }

    sw_report(fp, "\n小计：通过 %d 项，失败 %d 项\n", pass, fail);
    sw_report(fp, "完成\n");
    fclose(fp);

#undef SW_CHECK
}

/*
 * cn: 「连接 / 文件传输」前改动确认的自检。
 *
 * 两件事要分开验：
 *   1) 判据 —— 清单为空绝不弹窗（否则每次连接都被拦一道），有改动才弹；
 *   2) 版面 —— 换文案后弹窗仍与「确认保存」同款：两个按钮文字正确、
 *      宽度够放下「长期生效」四个字、窗口仍在 300～450px 区间。
 *
 * 模态弹窗点不了，所以按钮文案靠 GetWindowTextW 读回，尺寸靠
 * GetWindowRect 量，不依赖人工看图。
 */
void sftp_selftest_confirm_launch(void)
{
    char *outpath = sftp_selftest_outpath("confirm-launch-selftest.txt");
    FILE *fp = fopen(outpath, "w");
    sfree(outpath);
    int pass = 0, fail = 0;
    const char *changes =
        "【启用终端使用 xterm 256 色模式】 开 → 关\n"
        "【主机名】 a → b";

#define SW_CHECK(cond, desc) do {                                   \
        if (cond) { sw_report(fp, "  通过  %s\n", desc); pass++; }   \
        else { sw_report(fp, "  失败  %s\n", desc); fail++; }        \
    } while (0)

    if (!fp)
        return;

    sw_report(fp, "[连接/文件传输前改动确认自检]\n");

    /* 一、判据层：没改动必须放行，且一次弹窗都不出现。 */
    {
        Conf *live = conf_new();
        Conf *base = conf_new();
        struct dlgparam dp;

        do_defaults(NULL, live);
        do_defaults(NULL, base);
        memset(&dp, 0, sizeof(dp));
        dp.data = live;
        dp.baseline = base;
        dp.track_changes = true;

        SW_CHECK(dlg_confirm_launch(&dp) == CN_LAUNCH_GO,
                 "清单为空时直接放行（CN_LAUNCH_GO，不弹窗）");

        conf_set_bool(live, CONF_xterm_256_colour, false);
        SW_CHECK(dlg_has_unsaved_changes(&dp),
                 "有改动时进入待确认状态");

        conf_free(live);
        conf_free(base);
    }

    /* 二、版面层：换成「仅本次 / 长期」文案后，框仍要摆得下。 */
    {
        /* cn: 直接用线上那份文案（cn_launch_texts），验的就是真实配置。 */
        ConfirmSaveBox cb;

        if (!cn_confirm_save_build(NULL, changes, NULL, &cn_launch_texts, &cb)) {
            sw_report(fp, "  失败  弹窗未建成\n");
            fclose(fp);
            return;
        }

        {
            wchar_t buf[128];
            RECT wr, cr, br1, br2;

            GetWindowTextW(cb.b_ok, buf, 128);
            SW_CHECK(wcscmp(buf, L"仅本次生效") == 0,
                     "左按钮文案为「仅本次生效」");

            GetWindowTextW(cb.b_cancel, buf, 128);
            SW_CHECK(wcscmp(buf, L"长期生效") == 0,
                     "右按钮文案为「长期生效」");

            GetWindowTextW(cb.dlg, buf, 128);
            SW_CHECK(wcscmp(buf, L"确保保存方式后继续执行") == 0,
                     "窗口标题为「确保保存方式后继续执行」");

            GetWindowTextW(cb.head, buf, 128);
            {
                /* cn: 文案统一取宏，改文案不必再改断言。 */
                wchar_t *whead = dup_mb_to_wc(DEFAULT_CODEPAGE, CN_HEAD_TEXT);
                SW_CHECK(wcscmp(buf, whead) == 0, "首行文案与 CN_HEAD_TEXT 一致");
                sfree(whead);
            }

            /*
             * 「长期生效」比「确定」宽，按钮宽度必须跟着文字重算 ——
             * 若还按老文案量，字会被裁掉。
             */
            GetWindowRect(cb.b_cancel, &br2);
            {
                HDC hdc = GetDC(NULL);
                SIZE need = { 0, 0 };
                HFONT old = (HFONT)SelectObject(hdc, cb.font);
                GetTextExtentPoint32W(hdc, L"长期生效", 4, &need);
                SelectObject(hdc, old);
                ReleaseDC(NULL, hdc);
                sw_report(fp, "  「长期生效」文字宽 %dpx，按钮宽 %dpx\n",
                          (int)need.cx, (int)(br2.right - br2.left));
                SW_CHECK((br2.right - br2.left) >= (int)need.cx + cn_dpi(12),
                         "按钮宽度放得下「长期生效」（未裁字）");
            }

            GetWindowRect(cb.dlg, &wr);
            GetClientRect(cb.dlg, &cr);
            sw_report(fp, "  窗口 %dx%d，客户区 %dx%d\n",
                      (int)(wr.right - wr.left), (int)(wr.bottom - wr.top),
                      cr.right, cr.bottom);
            /*
             * cn: 判客户区宽度，不判整窗。
             * 300~450 是「内容区宽度」的规格（主人指定），加了原生
             * 标题栏后整窗比客户区多出左右边框（实测 6px），拿整窗判
             * 会误报超限 —— 内容其实正好 450。
             */
            SW_CHECK(cr.right >= cn_dpi(300) && cr.right <= cn_dpi(450),
                     "客户区宽度仍在 300～450px 区间");

            /* 两个按钮成组居中：左按钮左缘 + 右按钮右缘应对称。 */
            GetWindowRect(cb.b_ok, &br1);
            {
                int left_gap = (int)(br1.left - wr.left);
                int right_gap = (int)(wr.right - br2.right);
                sw_report(fp, "  左留白 %dpx，右留白 %dpx\n",
                          left_gap, right_gap);
                SW_CHECK(left_gap > 0 && right_gap > 0 &&
                         (left_gap - right_gap < cn_dpi(16)) &&
                         (right_gap - left_gap < cn_dpi(16)),
                         "两个按钮成组居中（左右留白基本对称）");
            }

            /*
             * 默认按钮必须在右边（「长期生效」），这是主人明确要的
             * 蓝框位置。BS_DEFPUSHBUTTON 的样式位是 0x1。
             */
            {
                LONG_PTR sok = GetWindowLongPtrW(cb.b_ok, GWL_STYLE);
                LONG_PTR scn = GetWindowLongPtrW(cb.b_cancel, GWL_STYLE);
                sw_report(fp, "  左按钮 style=0x%llx，右按钮 style=0x%llx\n",
                          (unsigned long long)sok, (unsigned long long)scn);
                SW_CHECK(!(sok & BS_DEFPUSHBUTTON),
                         "「仅本次生效」是普通按钮（无蓝框）");
                SW_CHECK((scn & BS_DEFPUSHBUTTON) != 0,
                         "「长期生效」是默认按钮（有蓝框）");
            }
        }

        DestroyWindow(cb.dlg);
        cn_confirm_save_release(&cb);
    }

    /*
     * 三、❌ 与「长期生效」的区分。两者都返回 IDCANCEL，全靠
     * closed_by_x 分辨 —— 判错就会让用户点个 ❌ 把配置写进注册表，
     * 或者反过来点了「长期生效」却没落盘。这里直接给弹窗发
     * WM_CLOSE（等价于点 ❌），读回标志。
     */
    {
        ConfirmSaveBox cb;
        if (cn_confirm_save_build(NULL, changes, NULL, NULL, &cb)) {
            SW_CHECK(!cb.closed_by_x, "新建弹窗时 closed_by_x 为假");
            SendMessageW(cb.dlg, WM_CLOSE, 0, 0);
            SW_CHECK(cb.closed_by_x, "点 ❌ 后 closed_by_x 置位（可判为取消）");
            SW_CHECK(cb.result == IDCANCEL, "点 ❌ 的返回值为 IDCANCEL");
            cn_confirm_save_release(&cb);
        } else {
            sw_report(fp, "  失败  弹窗未建成（❌ 区分用例）\n");
            fail++;
        }
    }

    sw_report(fp, "\n小计：通过 %d 项，失败 %d 项\n", pass, fail);
    sw_report(fp, "完成\n");
    fclose(fp);

#undef SW_CHECK
}
