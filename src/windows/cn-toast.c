/*
 * cn-toast.c: 会话窗口与文件传输窗口共用的轻提示条（toast）。
 *
 * 一批操作收尾后，在窗口边上浮出一条深色圆角提示：滑入 -> 停留 -> 淡出，
 * 约 1.5 秒后自己消失，不打断任何操作。
 *
 * 这套实现原本长在 sftpwin.c 里，只服务文件传输。会话窗口的「重启会话 /
 * 开始记录日志 / 停止记录日志」也要给一句即时确认，于是抽出来共用 ——
 * 圆角、配色、节奏都只有一份，两边不会慢慢长歪。
 *
 * 几条刻意的取舍：
 *  - 不拦鼠标（HTTRANSPARENT）、不抢焦点（WS_EX_NOACTIVATE）：它浮在窗口
 *    边上，用户紧接着可能就要敲命令或点按钮，被挡一下都烦。
 *  - 用一个窗口反复显示，换文案就地改尺寸重排：连续触发时新提示顶掉
 *    旧的，不会在屏幕上叠出一摞。
 *  - 圆角靠 SetWindowRgn 裁，淡入淡出靠分层窗口的 alpha，全是 GDI 手段，
 *    不必为此引 Direct2D。
 */

#include <winsock2.h> /* 必须放在最前面 */

/*
 * 整个文件走 Unicode 窗口 API。工程全局没有定义 UNICODE，不在这里声明
 * 的话 DrawTextW / RegisterClassW 之类会按 ANSI 解析出别的名字，或者
 * 撞上宏定义的类型不符。必须写在 windows.h 之前。
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include "putty.h"
#include "cn-toast.h"

#define CN_TOAST_IN       0
#define CN_TOAST_HOLD     1
#define CN_TOAST_OUT      2

#define CN_TOAST_TIMER        0x5F31  /* 定时器 ID，只挂在提示条自己的窗口上 */
#define CN_TOAST_IN_FRAMES    10      /* 滑入淡入 160ms */
#define CN_TOAST_OUT_FRAMES   10      /* 淡出 160ms */

/* 与冲突浮层同一套深色配色，界面里深浅两种浮层不必各调一种黑。 */
#define CN_TOAST_BG    RGB(0x17, 0x1b, 0x23)
#define CN_TOAST_FG    RGB(0xff, 0xff, 0xff)
#define CN_TOAST_EDGE  RGB(0x3a, 0x42, 0x50)

/* 逻辑像素 -> 物理像素。 */
static int cn_toast_dpi(int logical)
{
    /*
     * DPI 只取一次。本进程没有 DPI 感知声明，跑在系统 DPI 虚拟化下，
     * 进程内的 DPI 恒定，缓存一份即可 —— 这个函数在摆位和测量里被反复
     * 调用，每次都 GetDC(NULL) 是白扔开销。
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

/*
 * 取绘制字体。
 *
 * 调用方给了就借用（不接管释放）；没给就自己取系统「消息框字体」——
 * 这是资源管理器、对话框、菜单共用的那一套，跟随用户的「缩放与布局」
 * 设置走。用 GetStockObject(DEFAULT_GUI_FONT) 在中文 Windows 上会映射
 * 成点阵宋体，比正常 UI 字体大一号，高分屏下边缘还发糊。
 */
static HFONT cn_toast_font(CnToast *t)
{
    if (!t->font) {
        NONCLIENTMETRICSW ncm;
        HFONT f = NULL;

        memset(&ncm, 0, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm),
                                  &ncm, 0))
            f = CreateFontIndirectW(&ncm.lfMessageFont);
        if (f) {
            t->font = f;
            t->own_font = true;
        } else {
            /* 兜底用 stock 对象，它归系统所有，销毁时不能删。 */
            t->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            t->own_font = false;
        }
    }
    return t->font;
}

/*
 * 按当前文案量出提示条客户区尺寸。
 *
 * DT_CALCRECT 用与绘制端完全相同的标志，量出来多少就是多大 —— 早先浮层
 * 那里曾用 GetTextExtentPoint32W 另算一遍，两套规则不一致导致文字被切，
 * 这里直接照浮层的正确做法来。
 */
static bool cn_toast_measure(CnToast *t, int *out_w, int *out_h)
{
    RECT need = { 0, 0, 0, 0 };
    HDC hdc;
    HFONT old;
    int padx = cn_toast_dpi(22), pady = cn_toast_dpi(13);

    if (!t->text || !*t->text)
        return false;

    hdc = GetDC(NULL);
    if (!hdc)
        return false;
    old = SelectObject(hdc, cn_toast_font(t));
    need.right = cn_toast_dpi(4000);      /* 给够宽，只为量出单行文字宽度 */
    DrawTextW(hdc, t->text, -1, &need,
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
static void cn_toast_paint(HWND hwnd, HDC hdc, CnToast *t)
{
    RECT rc;
    HFONT old;

    if (!t || !t->text)
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

    old = SelectObject(hdc, cn_toast_font(t));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, CN_TOAST_FG);
    /* DT_SINGLELINE + 垂直居中：提示条是定高的一条，文案都是短句。 */
    DrawTextW(hdc, t->text, -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);
}

/*
 * 摆位：水平居中于宿主窗口客户区，贴着 pos 指定的那条边。
 *
 * anchor 是提示条贴着的那条边，slide 是滑入阶段额外偏移的像素数 ——
 * 从边外几像素处滑到位，比原地淡入更有「刚发生了什么」的方向感。
 * 贴底部时往下压，贴顶部时往上抬，方向都朝屏幕外。
 */
static void cn_toast_place(CnToast *t, int slide)
{
    RECT cr, tr;
    POINT pt;
    int w, h, x, y;

    if (!t->hwnd || !t->owner)
        return;
    if (!GetClientRect(t->owner, &cr) || !GetWindowRect(t->hwnd, &tr))
        return;
    w = tr.right - tr.left;
    h = tr.bottom - tr.top;

    pt.x = (cr.left + cr.right) / 2;
    pt.y = cn_toast_dpi(t->gap);
    if (t->pos == CN_TOAST_TOP)
        pt.y = cr.top + pt.y;          /* 从客户区顶部往下让出 gap */
    else
        pt.y = cr.bottom - pt.y;       /* 从客户区底部往上让出 gap */
    ClientToScreen(t->owner, &pt);

    x = pt.x - w / 2;
    if (t->pos == CN_TOAST_TOP)
        y = pt.y + slide;              /* 贴顶：滑入时更靠上，收拢到 y */
    else
        y = pt.y - h + slide;          /* 贴底：滑入时更靠下，收拢到底边 */

    SetWindowPos(t->hwnd, HWND_TOPMOST, x, y, 0, 0,
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
                             cn_toast_dpi(14), cn_toast_dpi(14));
    if (rgn)
        SetWindowRgn(hwnd, rgn, TRUE);
}

/*
 * 每帧一次：推进阶段机，更新透明度与位置。
 *
 * 三个阶段各自只数自己的帧，切换时把 tick 归零，读起来比一个全局计数
 * 加减去判断偏移清楚。
 */
static void cn_toast_tick(CnToast *t)
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

    t->tick++;

    switch (t->phase) {
      case CN_TOAST_IN:
        total = CN_TOAST_IN_FRAMES;
        if (t->tick >= total) {
            t->phase = CN_TOAST_HOLD;
            t->tick = 0;
        }
        break;
      case CN_TOAST_HOLD:
        total = hold;
        if (t->tick >= total) {
            t->phase = CN_TOAST_OUT;
            t->tick = 0;
        }
        break;
      default:                    /* CN_TOAST_OUT */
        total = CN_TOAST_OUT_FRAMES;
        if (t->tick >= total) {
            /* 淡出到头：收工。窗口留着，下次改文案再用。 */
            KillTimer(t->hwnd, CN_TOAST_TIMER);
            ShowWindow(t->hwnd, SW_HIDE);
            t->phase = CN_TOAST_IN;
            t->tick = 0;
            return;
        }
        break;
    }

    if (t->phase == CN_TOAST_IN) {
        alpha = 255 * t->tick / CN_TOAST_IN_FRAMES;
        /* 滑入：起始外偏 8 逻辑像素，随帧数线性收拢到 0。 */
        slide = cn_toast_dpi(8) * (CN_TOAST_IN_FRAMES - t->tick) /
                CN_TOAST_IN_FRAMES;
    } else if (t->phase == CN_TOAST_OUT) {
        alpha = 255 * (CN_TOAST_OUT_FRAMES - t->tick) /
                CN_TOAST_OUT_FRAMES;
    } else {
        alpha = 255;
    }

    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    SetLayeredWindowAttributes(t->hwnd, 0, (BYTE)alpha, LWA_ALPHA);
    cn_toast_place(t, slide);
    InvalidateRect(t->hwnd, NULL, TRUE);
}

static LRESULT CALLBACK cn_toast_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam)
{
    CnToast *t = (CnToast *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
      case WM_PAINT:
        if (t && t->text) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            cn_toast_paint(hwnd, hdc, t);
            EndPaint(hwnd, &ps);
        }
        return 0;
      /* 自检截图用：不接这条，WM_PRINTCLIENT 落到默认过程，位图全白。 */
      case WM_PRINTCLIENT:
        cn_toast_paint(hwnd, (HDC)wparam, t);
        return 0;
      case WM_ERASEBKGND:
        return 1;               /* WM_PAINT 里整片重画，不再擦一遍闪一下 */
      case WM_NCHITTEST:
        return HTTRANSPARENT;   /* 鼠标穿透：底下的终端和按钮照常能点 */
      case WM_TIMER:
        if (wparam == CN_TOAST_TIMER && t)
            cn_toast_tick(t);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/*
 * 建提示条窗口（首次调用时）。文本为空直接返回 false。
 *
 * owner 传宿主窗口：这样宿主最小化 / 关闭时系统会一并处理掉提示条，
 * 不至于留一条黑卡片孤零零浮在桌面上。
 */
static bool cn_toast_create(CnToast *t)
{
    static const wchar_t classname[] = L"PuTTYCnToast";
    HINSTANCE inst = GetModuleHandle(NULL);
    WNDCLASSW wc;
    int w, h;

    if (!cn_toast_measure(t, &w, &h))
        return false;

    if (!GetClassInfoW(inst, classname, &wc)) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = cn_toast_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = classname;
        RegisterClassW(&wc);
    }

    t->hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_LAYERED | WS_EX_TRANSPARENT,
        classname, NULL, WS_POPUP, 0, 0, w, h, t->owner, NULL, inst, NULL);
    if (!t->hwnd)
        return false;

    SetWindowLongPtrW(t->hwnd, GWLP_USERDATA, (LONG_PTR)t);
    cn_toast_round(t->hwnd);
    return true;
}

void cn_toast_show(CnToast *t, HWND owner, HFONT font,
                   CnToastPos pos, int gap, const char *text)
{
    wchar_t *w;
    int nw, nh;

    if (!t || !owner || !text || !*text)
        return;

    w = dup_mb_to_wc(DEFAULT_CODEPAGE, text);
    if (!w || !*w) {
        sfree(w);
        return;
    }
    sfree(t->text);
    t->text = w;

    /*
     * 字体与宿主窗口：会话窗口每次触发都传同一份，但文件传输窗口可能
     * 换了主题重建字体，这里跟着最新值走。
     */
    if (font && font != t->font) {
        if (t->own_font)
            DeleteObject(t->font);
        t->font = font;
        t->own_font = false;
    }
    t->owner = owner;
    t->pos = pos;
    t->gap = gap;

    if (!t->hwnd && !cn_toast_create(t)) {
        sfree(t->text);
        t->text = NULL;
        return;
    }

    /*
     * 文案换长了就得重排尺寸，否则新字会被旧窗宽切掉。尺寸一样时这一步
     * 也走，代码少一条分支，代价只是两次 SetWindowPos。
     */
    if (cn_toast_measure(t, &nw, &nh)) {
        RECT tr;
        if (GetWindowRect(t->hwnd, &tr))
            if (tr.right - tr.left != nw || tr.bottom - tr.top != nh)
                SetWindowPos(t->hwnd, NULL, 0, 0, nw, nh,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        cn_toast_round(t->hwnd);
    }

    /* 从滑入重新开始：连续两次触发之间不会出现「刚显示就淡出」的怪相。 */
    t->phase = CN_TOAST_IN;
    t->tick = 0;
    SetLayeredWindowAttributes(t->hwnd, 0, 0, LWA_ALPHA);
    cn_toast_place(t, cn_toast_dpi(8));
    SetTimer(t->hwnd, CN_TOAST_TIMER, CN_TOAST_FRAME_MS, NULL);
    InvalidateRect(t->hwnd, NULL, TRUE);
}

void cn_toast_destroy(CnToast *t)
{
    if (!t)
        return;
    if (t->hwnd) {
        KillTimer(t->hwnd, CN_TOAST_TIMER);
        SetWindowLongPtrW(t->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(t->hwnd);
        t->hwnd = NULL;
    }
    sfree(t->text);
    t->text = NULL;
    /* 只删自己建的字体；借用调用方的那份不归我们管。 */
    if (t->own_font && t->font)
        DeleteObject(t->font);
    t->font = NULL;
    t->own_font = false;
    t->owner = NULL;
}
