# -*- coding: utf-8 -*-
"""会话历史列表行分割线验证脚本（屏幕抓图版）。

用 Win32 API 定位 dist/putty.exe 配置窗口里的会话历史 LISTBOX，
用 BitBlt 从屏幕 DC 抓真实像素，统计行间横线。
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

LB_GETCOUNT = 0x018B
LB_GETSEL = 0x00F1
LB_GETITEMRECT = 0x0198
LB_SETCURSEL = 0x0186

user32.SetProcessDPIAware()

PUTTY = r"C:\Users\Wrok\Repos\putty-cn\dist\putty.exe"


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
        rect = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        out.append({
            "hwnd": hwnd,
            "class": cls.value,
            "rect": (rect.left, rect.top, rect.right, rect.bottom),
            "style": user32.GetWindowLongW(hwnd, -16) & 0xFFFFFFFF,
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
    """从屏幕 DC 抓取指定矩形像素。"""
    hdc = user32.GetDC(0)
    memdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(memdc, bmp)
    gdi32.BitBlt(memdc, 0, 0, w, h, hdc, left, top, 0x00CC0020)  # SRCCOPY

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
            row.append((raw[o + 2], raw[o + 1], raw[o]))  # RGB
        pixels.append(row)
    return pixels


def save_png(pixels, w, h, path):
    try:
        from PIL import Image
    except ImportError:
        return False
    img = Image.new("RGB", (w, h))
    img.putdata([pixels[y][x] for y in range(h) for x in range(w)])
    img.save(path)
    return True


def main():
    proc = subprocess.Popen([PUTTY])
    time.sleep(3.0)

    wins = find_windows("配置")
    if not wins:
        proc.kill()
        print("FAIL: 未找到配置窗口")
        return 1

    main_hwnd = wins[0][0]
    print("窗口:", wins[0][1])
    user32.SetForegroundWindow(main_hwnd)
    time.sleep(0.5)

    kids = child_windows(main_hwnd)
    listboxes = [k for k in kids if k["class"] == "ListBox"]
    if not listboxes:
        print("FAIL: 主窗口内没有 ListBox")
        proc.kill()
        return 1

    lb = listboxes[0]
    hwnd = lb["hwnd"]
    left, top, right, bottom = lb["rect"]
    # 客户区坐标偏移：排除边框，取客户区
    crect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(crect))
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    cw, ch = crect.right, crect.bottom

    print("ListBox hwnd=%d style=0x%08X" % (hwnd, lb["style"]))
    print("LBS_OWNERDRAWFIXED =", bool(lb["style"] & 0x0010))
    print("客户区屏幕坐标: (%d,%d) %dx%d" % (pt.x, pt.y, cw, ch))

    count = user32.SendMessageW(hwnd, LB_GETCOUNT, 0, 0)
    print("条目数:", count)

    pixels = grab_screen_rect(pt.x, pt.y, cw, ch)
    save_png(pixels, cw, ch,
             r"C:\Users\Wrok\Repos\putty-cn\reports\ruled-list-screen.png")

    # 统计每行中灰像素（分割线）
    row_gray = []
    for y in range(ch):
        n = 0
        for x in range(cw):
            r, g, b = pixels[y][x]
            if abs(r - g) <= 6 and abs(g - b) <= 6 and 150 <= r <= 230:
                n += 1
        row_gray.append(n)

    lines = [(y, n) for y, n in enumerate(row_gray) if n > cw * 0.7]
    print("候选分割线行数:", len(lines))
    for y, n in lines[:25]:
        print("  y=%d 灰=%d/%d (%.0f%%)" % (y, n, cw, 100.0 * n / cw))

    # 行距检查：相邻分割线间隔应大致等于行高
    if len(lines) >= 2:
        gaps = [lines[i + 1][0] - lines[i][0]
                for i in range(len(lines) - 1)]
        print("分割线间隔:", gaps)

    ok = len(lines) >= 3
    print("RESULT:", "PASS" if ok else "FAIL")
    proc.kill()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
