/*
 * Implementation of do_select() for network.c to use, that uses
 * WSAAsyncSelect to convert network activity into window messages,
 * for integration into a GUI event loop.
 */

#include "putty.h"
#include "tree234.h"

static void wm_netevent_callback(void *vctx);

/*
 * cn: 网络事件窗口改为「按套接字分派」。
 *
 * 原来只有一个全局 winsel_hwnd，谁最后调用 winselgui_set_hwnd 谁独占，
 * 所有套接字的 WM_NETEVENT 都发到那一个窗口。终端会话占着它，文件传输
 * 的独立连接若再覆盖它，终端就收不到自己的网络事件，整条 SSH 会僵住。
 *
 * 现在保留 winsel_hwnd 作为「默认窗口」（终端窗口 / Pageant 托盘窗口），
 * 另加一个「作用域窗口」：某段代码建连接期间把窗口压进
 * winselgui_push_hwnd，这段时间创建的套接字就绑到它，弹栈后恢复默认。
 * 每个套接字的归属记在 socket_hwnds 里，取消注册和 done_with_socket
 * 都按记录走，两边互不干扰。
 */
static HWND winsel_hwnd = NULL;
static HWND winsel_scope_hwnd = NULL;
static tree234 *socket_hwnds = NULL;
static tree234 *moribund_sockets = NULL;

static int moribund_socket_cmp(void *av, void *bv)
{
    uintptr_t a = (uintptr_t)av, b = (uintptr_t)bv;
    return a < b ? -1 : a > b ? +1 : 0;
}

struct sock_hwnd_entry {
    SOCKET skt;
    HWND hwnd;
};

static int sock_hwnd_cmp(void *av, void *bv)
{
    uintptr_t a = (uintptr_t)av, b = (uintptr_t)bv;
    return a < b ? -1 : a > b ? +1 : 0;
}

static void remember_sock_hwnd(SOCKET skt, HWND hwnd)
{
    struct sock_hwnd_entry *e;
    if (!socket_hwnds)
        socket_hwnds = newtree234(sock_hwnd_cmp);
    e = del234(socket_hwnds, (void *)(uintptr_t)skt);
    if (!e)
        e = snew(struct sock_hwnd_entry);
    e->skt = skt;
    e->hwnd = hwnd;
    add234(socket_hwnds, e);
}

static HWND lookup_sock_hwnd(SOCKET skt)
{
    struct sock_hwnd_entry *e;
    if (!socket_hwnds)
        return NULL;
    e = find234(socket_hwnds, (void *)(uintptr_t)skt, NULL);
    return e ? e->hwnd : NULL;
}

static void forget_sock_hwnd(SOCKET skt)
{
    struct sock_hwnd_entry *e;
    if (!socket_hwnds)
        return;
    e = del234(socket_hwnds, (void *)(uintptr_t)skt);
    if (e)
        sfree(e);
}

void winselgui_set_hwnd(HWND hwnd)
{
    winsel_hwnd = hwnd;
}

void winselgui_clear_hwnd(void)
{
    winsel_hwnd = NULL;
}

/* 压入 / 弹出作用域窗口。建连接期间用，退出时务必弹出。 */
void winselgui_push_hwnd(HWND hwnd)
{
    winsel_scope_hwnd = hwnd;
}

void winselgui_pop_hwnd(void)
{
    winsel_scope_hwnd = NULL;
}

/*
 * cn: 兜底窗口。
 *
 * 正常 GUI 会话里默认窗口是终端窗口，Pageant 用托盘窗口，两条路都有人
 * 设置 winsel_hwnd。但 putty -sftp-win 这类自检入口没有主窗口，连接一建
 * 就撞 assert。这里懒加载一个隐藏窗口顶上，同时负责把网络事件转回
 * winselgui_response。
 */
static HWND winsel_fallback = NULL;

static LRESULT CALLBACK winsel_fallback_wndproc(HWND hwnd, UINT msg,
                                                WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_NETEVENT || msg == WM_DONE_WITH_SOCKET) {
        winselgui_response(msg, wparam, lparam);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND winselgui_fallback_hwnd(void)
{
    static const wchar_t classname[] = L"PuTTYWinSelFallback";
    WNDCLASSW wc;
    HINSTANCE inst = GetModuleHandle(NULL);

    if (winsel_fallback && IsWindow(winsel_fallback))
        return winsel_fallback;

    if (!GetClassInfoW(inst, classname, &wc)) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = winsel_fallback_wndproc;
        wc.hInstance = inst;
        wc.lpszClassName = classname;
        if (!RegisterClassW(&wc))
            return NULL;
    }

    /*
     * 必须是普通窗口：WSAAsyncSelect 的 WM_NETEVENT 不会投递到
     * HWND_MESSAGE 的消息专用窗口（这个坑之前踩过一次）。
     */
    winsel_fallback = CreateWindowExW(0, classname, L"PuTTY 网络事件",
                                      0, 0, 0, 0, 0,
                                      NULL, NULL, inst, NULL);
    return winsel_fallback;
}

const char *do_select(SOCKET skt, bool enable)
{
    int msg, events;
    if (enable) {
        msg = WM_NETEVENT;
        events = (FD_CONNECT | FD_READ | FD_WRITE |
                  FD_OOB | FD_CLOSE | FD_ACCEPT);
    } else {
        msg = events = 0;
    }

    if (enable) {
        /*
         * 窗口的选取顺序：
         *   1. 该套接字原来的窗口——同一连接会多次 enable（比如发送
         *      缓冲从满变空时重新武装 FD_WRITE），这时必须回原处，
         *      否则会把事件挪到别的窗口上去；
         *   2. 作用域窗口——新建连接时用；
         *   3. 默认窗口——终端会话 / Pageant 托盘。
         */
        HWND target = lookup_sock_hwnd(skt);
        if (!target)
            target = winsel_scope_hwnd;
        if (!target)
            target = winsel_hwnd;

        if (!target) {
            /*
             * cn: 兜底。三个候选都空说明这条连接建立时没人认领它
             * （典型情况：非终端程序，比如自检路径，压根没有主窗口）。
             * 造一个隐藏窗口接住它的网络事件，别让 assert 把程序打断。
             */
            target = winselgui_fallback_hwnd();
        }

        assert(target);

        if (p_WSAAsyncSelect(skt, target, msg, events) == SOCKET_ERROR)
            return winsock_error_string(p_WSAGetLastError());

        remember_sock_hwnd(skt, target);
        return NULL;
    } else {
        /*
         * 取消注册要发到当初绑定的那个窗口。查不到就传 NULL ——
         * events 为 0 时 WSAAsyncSelect 允许 hWnd 为 NULL，效果是
         * 解除该套接字的所有异步事件通知。
         *
         * 注意这里**不**清掉归属记录：network.c 会在传输过程中反复
         * enable/disable（发送缓冲满/空时重新武装 FD_WRITE），一
         * disable 就忘掉的话，再 enable 时既没有作用域也没有默认
         * 窗口可用，直接触发 assert。归属记录留到 done_with_socket
         * —— 那是套接字真正销毁的时刻。
         */
        HWND target = lookup_sock_hwnd(skt);

        if (p_WSAAsyncSelect(skt, target, msg, events) == SOCKET_ERROR)
            return winsock_error_string(p_WSAGetLastError());

        return NULL;
    }
}

struct wm_netevent_params {
    /* Used to pass data to wm_netevent_callback */
    WPARAM wParam;
    LPARAM lParam;
};

static bool callback_is_for_socket(
    void *predicate_ctx, toplevel_callback_fn_t fn, void *callback_ctx)
{
    if (fn != wm_netevent_callback)
        return false;
    struct wm_netevent_params *params =
        (struct wm_netevent_params *)callback_ctx;
    if (params->wParam != (WPARAM)(uintptr_t)predicate_ctx)
        return false;

    /* The 'struct wm_netevent_params' would have been freed by the
     * callback function wm_netevent_callback(). Now that isn't going
     * to run, so we must free it ourself. */
    sfree(callback_ctx);
    return true;
}

void done_with_socket(SOCKET skt)
{
    /*
     * cn: 通知要发到这个套接字自己的窗口。原本固定发 winsel_hwnd，
     * 传输连接的 socket 一旦绑到自己的窗口，消息就发错了地方，
     * moribund_sockets 里的记录永远清不掉，后续同类 socket 的事件
     * 会被误判为「已死」而丢弃。
     */
    HWND target = lookup_sock_hwnd(skt);

    if (!moribund_sockets)
        moribund_sockets = newtree234(moribund_socket_cmp);
    if (target)
        PostMessage(target, WM_DONE_WITH_SOCKET, (WPARAM)skt, 0);
    else if (winsel_hwnd)
        PostMessage(winsel_hwnd, WM_DONE_WITH_SOCKET, (WPARAM)skt, 0);
    add234(moribund_sockets, (void *)skt);
    delete_callbacks(callback_is_for_socket, (void *)(uintptr_t)skt);

    /* 套接字到此为止了，归属记录也没用了。 */
    forget_sock_hwnd(skt);
}

static void wm_netevent_callback(void *vctx)
{
    struct wm_netevent_params *params = (struct wm_netevent_params *)vctx;
    select_result(params->wParam, params->lParam);
    sfree(params);
}

void winselgui_response(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DONE_WITH_SOCKET) {
        del234(moribund_sockets, (void *)wParam);
        return;
    } else if (moribund_sockets &&
               find234(moribund_sockets, (void *)wParam, NULL)) {
        return;
    }

    /*
     * To protect against re-entrancy when Windows's recv()
     * immediately triggers a new WSAAsyncSelect window message, we
     * don't call select_result directly from this handler but instead
     * wait until we're back out at the top level of the message loop.
     */
    struct wm_netevent_params *params = snew(struct wm_netevent_params);
    params->wParam = wParam;
    params->lParam = lParam;
    queue_toplevel_callback(wm_netevent_callback, params);
}
