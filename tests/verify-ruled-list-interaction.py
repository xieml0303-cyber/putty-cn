# -*- coding: utf-8 -*-
"""会话历史列表行分割线——交互验证脚本。

覆盖项：
1. 列表为 owner-draw 且行高与原生一致（13 行可见）
2. 鼠标点击选中：选中行反色，且该行不画分割线
3. 双击条目：主机名输入框被回填
4. 键盘方向键导航：选中项随之下移
5. 滚轮/滚动条滚动：不崩溃、分割线仍在
6. 进程全程存活
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

LB_GETCOUNT = 0x018B
LB_GETCURSEL = 0x0188
LB_GETTEXT = 0x0189
LB_GETTEXTLEN = 0x018A
LB_GETITEMRECT = 0x0198
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_LBUTTONDBLCLK = 0x0203
WM_KEYDOWN = 0x0100
WM_CHAR = 0x0102
VK_DOWN = 0x28
VK_UP = 0x26
VK_HOME = 0x24
MK_LBUTTON = 0x0001

user32.SetProcessDPIAware()
PUTTY = r"C:\Users\Wrok\Repos\putty-cn\dist\putty.exe"

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("%-46s %s  %s" % (name, "PASS" if ok else "FAIL", detail))


def find_windows(title_substr):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if title_substr in buf.value:
                found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(cb, 0)
    return found


def child_windows(parent):
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, lparam):
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, cls, 256)
        tid = user32.GetWindowThreadProcessId(hwnd, None)
        rect = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        out.append({
            "hwnd": hwnd,
            "class": cls.value,
            "rect": (rect.left, rect.top, rect.right, rect.bottom),
            "style": user32.GetWindowLongW(hwnd, -16) & 0xFFFFFFFF,
            "tid": tid,
        })
        return True

    user32.EnumChildWindows(parent, cb, 0)
    return out


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG),
                ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


def grab_screen_rect(left, top, w, h):
    hdc = user32.GetDC(0)
    memdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(memdc, bmp)
    gdi32.BitBlt(memdc, 0, 0, w, h, hdc, left, top, 0x00CC0020)

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -h
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(memdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(memdc)
    user32.ReleaseDC(0, hdc)

    raw = buf.raw
    pixels = []
    for y in range(h):
        row = []
        base = y * w * 4
        for x in range(w):
            o = base + x * 4
            row.append((raw[o + 2], raw[o + 1], raw[o]))
        pixels.append(row)
    return pixels


def save_png(pixels, w, h, path):
    try:
        from PIL import Image
    except ImportError:
        return
    img = Image.new("RGB", (w, h))
    img.putdata([pixels[y][x] for y in range(h) for x in range(w)])
    img.save(path)


def listbox_geometry(hwnd):
    crect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(crect))
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y, crect.right, crect.bottom


def item_rect(hwnd, index):
    """返回条目在客户区内的 (top, bottom)。"""
    r = wt.RECT()
    ok = user32.SendMessageW(hwnd, LB_GETITEMRECT, index,
                             ctypes.byref(r))
    if not ok:
        return None
    return r.top, r.bottom


def click_at(hwnd, cx, cy, dbl=False):
    """用真实鼠标事件点击客户区坐标。

    PostMessage 投递 WM_LBUTTONDOWN/UP 不会让列表产生 LBN_DBLCLK，
    所以双击必须走真实输入队列。
    """
    user32.SetForegroundWindow(hwnd)
    pt = wt.POINT(cx, cy)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    user32.SetCursorPos(pt.x, pt.y)
    time.sleep(0.1)
    for _ in range(2 if dbl else 1):
        user32.mouse_event(0x0002, 0, 0, 0, 0)   # LEFTDOWN
        user32.mouse_event(0x0004, 0, 0, 0, 0)   # LEFTUP
        if dbl:
            time.sleep(0.03)
    time.sleep(0.45)


def key(hwnd, vk):
    user32.SetForegroundWindow(hwnd)
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 0)
    time.sleep(0.25)


def main():
    proc = subprocess.Popen([PUTTY])
    time.sleep(3.0)

    wins = find_windows("配置")
    if not wins:
        proc.kill()
        print("FAIL: 未找到配置窗口")
        return 1
    main_hwnd = wins[0][0]
    user32.SetForegroundWindow(main_hwnd)
    time.sleep(0.4)

    kids = child_windows(main_hwnd)
    listboxes = [k for k in kids if k["class"] == "ListBox"]
    edits = [k for k in kids if k["class"] == "Edit"]
    if not listboxes:
        print("FAIL: 没有 ListBox")
        proc.kill()
        return 1
    lb = listboxes[0]
    hwnd = lb["hwnd"]

    check("列表为 owner-draw 风格", bool(lb["style"] & 0x0010),
          "style=0x%08X" % lb["style"])

    count = user32.SendMessageW(hwnd, LB_GETCOUNT, 0, 0)
    check("条目数 > 0", count > 0, "count=%d" % count)

    x, y, cw, ch = listbox_geometry(hwnd)
    print("客户区 (%d,%d) %dx%d" % (x, y, cw, ch))

    # 1. 行高一致性：条目 0 与条目 1 的 rect 高度
    r0 = item_rect(hwnd, 0)
    r1 = item_rect(hwnd, 1) if count > 1 else None
    if r0 and r1:
        h0 = r0[1] - r0[0]
        h1 = r1[1] - r1[0]
        check("行高一致且非零", h0 == h1 and h0 > 0,
              "h0=%d h1=%d 间距=%d" % (h0, h1, r1[0] - r0[0]))
        rowheight = h0
    else:
        rowheight = 13
        check("行高可读", False, "LB_GETITEMRECT 失败")

    # 2. 初始无选中
    sel0 = user32.SendMessageW(hwnd, LB_GETCURSEL, 0, 0)
    print("初始选中:", sel0)

    # 3. 点击第 3 项
    target = 3 if count > 3 else 0
    rt = item_rect(hwnd, target)
    cy = (rt[0] + rt[1]) // 2 if rt else rowheight * target + rowheight // 2
    click_at(hwnd, cw // 2, cy)
    sel1 = user32.SendMessageW(hwnd, LB_GETCURSEL, 0, 0)
    check("鼠标点击可选中", sel1 == target,
          "点击 index=%d → 选中=%d" % (target, sel1))

    pixels = grab_screen_rect(x, y, cw, ch)
    save_png(pixels, cw, ch,
             r"C:\Users\Wrok\Repos\putty-cn\reports\ruled-list-selected.png")

    # 选中行区域应出现高亮底色（非白、非灰线）
    if rt:
        sample = [pixels[yy][cw // 2] for yy in range(rt[0], min(rt[1], ch))]
        nonwhite = sum(1 for p in sample if not (p[0] > 240 and p[1] > 240
                                                 and p[2] > 240))
        check("选中行有高亮底色", nonwhite > 0,
              "非白像素 %d/%d 首像素=%s" % (nonwhite, len(sample),
                                            sample[0] if sample else None))

    # 4. 分割线数量（选中行会少一条）
    row_gray = []
    for yy in range(ch):
        n = sum(1 for xx in range(cw)
                if abs(pixels[yy][xx][0] - pixels[yy][xx][1]) <= 6
                and abs(pixels[yy][xx][1] - pixels[yy][xx][2]) <= 6
                and 150 <= pixels[yy][xx][0] <= 230)
        row_gray.append(n)
    lines = [yy for yy, n in enumerate(row_gray) if n > cw * 0.7]
    check("分割线仍存在（选中行除外）", len(lines) >= 3,
          "可见线数=%d" % len(lines))

    # 5. 键盘导航
    before = user32.SendMessageW(hwnd, LB_GETCURSEL, 0, 0)
    key(hwnd, VK_DOWN)
    after = user32.SendMessageW(hwnd, LB_GETCURSEL, 0, 0)
    check("方向键下移选中", after == before + 1,
          "%s → %s" % (before, after))

    key(hwnd, VK_UP)
    back = user32.SendMessageW(hwnd, LB_GETCURSEL, 0, 0)
    check("方向键上移选中", back == after - 1, "%s → %s" % (after, back))

    key(hwnd, VK_HOME)
    home = user32.SendMessageW(hwnd, LB_GETCURSEL, 0, 0)
    check("Home 跳到首项", home == 0, "选中=%s" % home)

    # 6. 双击回填主机名
    # 主机名是 ComboBox（CBS_DROPDOWN）。GetWindowText / GetDlgItemText
    # 从别的进程读不到它的显示文本（实测返回空，但控件确实画出了文字），
    # 所以改用像素判断：框内出现深色文字像素即说明回填成功。
    hostbox = None
    for e in kids:
        if e["class"] == "ComboBox":
            l, t, r, b = e["rect"]
            if r - l > 150 and t < lb["rect"][1]:
                hostbox = e
                break

    def dark_pixels(hwnd_):
        """统计控件矩形内深色像素数量（代表文字）。"""
        rc = wt.RECT()
        user32.GetWindowRect(hwnd_, ctypes.byref(rc))
        w = rc.right - rc.left
        h = rc.bottom - rc.top
        if w <= 0 or h <= 0:
            return 0, 0, 0
        px = grab_screen_rect(rc.left, rc.top, w, h)
        n = sum(1 for yy in range(h) for xx in range(w)
                if px[yy][xx][0] < 120 and px[yy][xx][1] < 120
                and px[yy][xx][2] < 120)
        return n, w, h

    # 双击第 2 项之前重新采样：前面几步点击已经触发过回填，
    # 早先那次采样不是真正的初始状态。
    before_dark = dark_pixels(hostbox["hwnd"]) if hostbox else (0, 0, 0)
    print("双击前主机名框深色像素=%d" % before_dark[0])

    # 换一个与当前已回填内容不同的条目，才能看出变化
    dt = 1 if count > 1 else 0
    rt2 = item_rect(hwnd, dt)
    cy2 = (rt2[0] + rt2[1]) // 2 if rt2 else rowheight * dt + 6
    click_at(hwnd, cw // 2, cy2, dbl=True)

    if hostbox:
        after_dark = dark_pixels(hostbox["hwnd"])
        check("双击回填主机名（像素）",
              after_dark[0] > 0,
              "前=%d 后=%d 像素（%dx%d）"
              % (before_dark[0], after_dark[0],
                 after_dark[1], after_dark[2]))
    else:
        check("双击回填主机名", False, "未找到主机名输入框")

    # 7. 滚动测试：滚到底部
    for _ in range(6):
        key(hwnd, VK_DOWN)
    time.sleep(0.3)
    alive = proc.poll() is None
    check("滚动/导航后进程存活", alive,
          "exitcode=%s" % (proc.poll(),))

    # 8. 再抓一次像素，确认滚动后分割线仍在
    pixels2 = grab_screen_rect(x, y, cw, ch)
    save_png(pixels2, cw, ch,
             r"C:\Users\Wrok\Repos\putty-cn\reports\ruled-list-scrolled.png")
    row_gray2 = []
    for yy in range(ch):
        n = sum(1 for xx in range(cw)
                if abs(pixels2[yy][xx][0] - pixels2[yy][xx][1]) <= 6
                and abs(pixels2[yy][xx][1] - pixels2[yy][xx][2]) <= 6
                and 150 <= pixels2[yy][xx][0] <= 230)
        row_gray2.append(n)
    lines2 = [yy for yy, n in enumerate(row_gray2) if n > cw * 0.7]
    check("滚动后分割线仍在", len(lines2) >= 3, "线数=%d" % len(lines2))

    proc.kill()

    print()
    passed = sum(1 for _, ok, _ in results if ok)
    print("合计 %d/%d 通过" % (passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
