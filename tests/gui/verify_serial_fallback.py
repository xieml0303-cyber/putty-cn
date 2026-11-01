# -*- coding: utf-8 -*-
"""H3 验证：串口自动回退的落盘保护与用户选择落盘。

每场景改注册表「默认设置」的 SerialLine 后启动真实 putty.exe 开配置框：
  A 原值 COM1（本机不存在）→ 开框自动回退 COM3 → 点保存 → 注册表保留 COM1
  B 同 A，再真实点击下拉框选 COM3（用户明确选择）→ 保存 → 注册表 COM3
  C 原值 COM4（蓝牙）→ 开框回退到物理口 → 点保存 → 注册表保留 COM4
  D 原值 COM3（本机存在）→ 开框不回退 → 保存 → 注册表 COM3（回归验证）
显示标注「（物理，自动匹配）」以截图佐证（跨进程读 combo 文本必空，见
win-gui-verify 坑清单第 5 条）。
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time
import winreg

import win32gui
import win32process
from PIL import Image

PUTTY = os.environ.get(
    "PUTTY_EXE",
    os.path.join(os.path.dirname(__file__), "..", "..", "src", "build", "putty.exe"))
SESSION_KEY = (r"Software\SimonTatham\PuTTY\Sessions"
               r"\%E9%BB%98%E8%AE%A4%E8%AE%BE%E7%BD%AE")
SHOT = os.path.join(os.path.dirname(__file__), "cn-fallback-sceneA.png")

BM_CLICK = 0x00F5
WM_CLOSE = 0x0010
WM_COMMAND = 0x0111
CB_GETCOUNT = 0x0146
LEFTDOWN, LEFTUP = 0x0002, 0x0004

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.windll.kernel32

_sm = user32.SendMessageW
_sm.restype = ctypes.c_ssize_t
_sm.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t,
                ctypes.c_ssize_t]

_pm = user32.PostMessageW
_pm.restype = ctypes.c_ssize_t
_pm.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t,
                ctypes.c_ssize_t]


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR),
        ("lpTitle", wt.LPWSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD),
        ("dwXSize", wt.DWORD), ("dwYCountChars" if False else "dwYSize", wt.DWORD),
        ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD),
        ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD),
        ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
        ("lpReserved2", ctypes.c_void_p), ("hStdInput", wt.HANDLE),
        ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE),
    ]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
        ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD),
    ]


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


def spawn(cmdline):
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(si)
    pi = PROCESS_INFORMATION()
    ok = kernel32.CreateProcessW(
        None, ctypes.c_wchar_p(cmdline), None, None, False, 0, None, None,
        ctypes.byref(si), ctypes.byref(pi))
    if not ok:
        raise OSError("CreateProcess failed: %d" % ctypes.get_last_error())
    return pi


def stop(pi):
    kernel32.TerminateProcess(pi.hProcess, 0)
    kernel32.CloseHandle(pi.hThread)
    kernel32.CloseHandle(pi.hProcess)


def is_alive(pi):
    code = wt.DWORD()
    kernel32.GetExitCodeProcess(pi.hProcess, ctypes.byref(code))
    return code.value == 259


def wait_for(fn, timeout=10.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return None


results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + (("  -> " + detail) if detail else ""))


def reg_get():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, SESSION_KEY, 0,
                            winreg.KEY_READ) as k:
            try:
                v, _ = winreg.QueryValueEx(k, "SerialLine")
            except FileNotFoundError:
                v = None
            try:
                p, _ = winreg.QueryValueEx(k, "Protocol")
            except FileNotFoundError:
                p = None
            return v, p
    except FileNotFoundError:
        return None, None


def reg_set(value):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, SESSION_KEY, 0,
                        winreg.KEY_SET_VALUE) as k:
        winreg.SetValueEx(k, "SerialLine", 0, winreg.REG_SZ, value)
        winreg.SetValueEx(k, "Protocol", 0, winreg.REG_SZ, "serial")


def children(hwnd):
    out = []
    h = 0
    while True:
        h = win32gui.FindWindowEx(hwnd, h, None, None)
        if not h:
            return out
        out.append(h)


def find_button(dlg, prefix):
    for h in children(dlg):
        if (win32gui.GetClassName(h) == "Button"
                and win32gui.GetWindowText(h).startswith(prefix)):
            return h
    return None


def find_serial_combo(dlg):
    """会话页有两个 ComboBox（串口选择器 + 「其他:」协议框）。
    串口框在窗口上方：取 top 最小的那个。"""
    best = None
    best_top = None
    for h in children(dlg):
        if win32gui.GetClassName(h) == "ComboBox":
            r = RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            if best is None or r.top < best_top:
                best, best_top = h, r.top
    return best


def screenshot(hwnd, path):
    r = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdc = user32.GetWindowDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, 2)

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long),
                    ("biHeight", ctypes.c_long), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                    ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bi = BIH()
    bi.biSize = ctypes.sizeof(BIH)
    bi.biWidth, bi.biHeight = w, -h
    bi.biPlanes, bi.biBitCount = 1, 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert(
        "RGB").save(path)
    print("screenshot saved:", path, w, "x", h)


def real_click(x, y):
    user32.SetCursorPos(int(x), int(y))
    time.sleep(0.08)
    user32.mouse_event(LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(LEFTUP, 0, 0, 0, 0)


def open_config_box():
    pi = spawn('"%s"' % PUTTY)
    dlg = wait_for(lambda: win32gui.FindWindow("PuTTYConfigBox", None), 15)
    if dlg:
        _tid, winpid = win32process.GetWindowThreadProcessId(dlg)
        if winpid != pi.dwProcessId:
            print("FAIL 命中了别的 PuTTY 实例窗口")
            return pi, None
        time.sleep(1.0)
        user32.SetForegroundWindow(dlg)
        time.sleep(0.3)
    return pi, dlg


def dismiss_info_box():
    """保存成功后的 MessageBox（#32770、标题 PuTTY），关闭它。"""
    mb = wait_for(lambda: win32gui.FindWindow("#32770", "PuTTY"), 5)
    if mb:
        _pm(mb, WM_CLOSE, 0, 0)
        time.sleep(0.4)
    return mb is not None


def serial_port_count():
    """本机串口总数（SERIALCOMM 值个数），即下拉列表项数。"""
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                        r"HARDWARE\DEVICEMAP\SERIALCOMM", 0,
                        winreg.KEY_READ) as k:
        return winreg.QueryInfoKey(k)[1]


def serial_ports():
    """本机全部串口名（SERIALCOMM 值数据）。"""
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                        r"HARDWARE\DEVICEMAP\SERIALCOMM", 0,
                        winreg.KEY_READ) as k:
        n = winreg.QueryInfoKey(k)[1]
        out = []
        for i in range(n):
            _, val, _ = winreg.EnumValue(k, i)
            out.append(val)
    return out


def first_real_port():
    """列表第 0 项（fill_serial_port_list 的回退目标）= 用户真实点击可选的端口。"""
    return serial_ports()[0]


def find_dropdown_popup(combo_rect):
    """在 combo 正下方找宽度相近的可见 popup（下拉列表被自绘，
    不是标准 ComboLBox，按几何特征找）。"""
    hits = []

    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        cls = win32gui.GetClassName(hwnd)
        if cls == "PuTTYConfigBox":
            return True
        r = RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        w, hgt = r.right - r.left, r.bottom - r.top
        if (abs(r.left - combo_rect.left) < 40
                and r.top >= combo_rect.bottom - 2
                and 120 < hgt < 400 and w > 150):
            hits.append((hwnd, r))
        return True

    win32gui.EnumWindows(cb, None)
    return hits[0] if hits else None


def pick_port_by_mouse(index):
    """真实点击拿焦点后用键盘完成选择（真实输入队列，触发 CBN_SELCHANGE）。"""
    dlg = win32gui.FindWindow("PuTTYConfigBox", None)
    combo = find_serial_combo(dlg)
    if not combo:
        raise RuntimeError("找不到串口 ComboBox")
    cr = RECT()
    user32.GetWindowRect(combo, ctypes.byref(cr))
    real_click((cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2)
    time.sleep(0.4)
    # 键盘选择：CBS_DROPDOWNLIST 按下方向键移动选择，回车确认；
    # 无论列表是否展开此路径都成立。
    VK_DOWN, VK_RETURN, KEYUP = 0x28, 0x0D, 2
    for _ in range(index + 1):
        user32.keybd_event(VK_DOWN, 0, 0, 0)
        user32.keybd_event(VK_DOWN, 0, KEYUP, 0)
        time.sleep(0.15)
    user32.keybd_event(VK_RETURN, 0, 0, 0)
    user32.keybd_event(VK_RETURN, 0, KEYUP, 0)
    time.sleep(0.5)
    # 断言选择真的生效（跨进程可安全读索引与列表文本）
    sel = _sm(combo, 0x0147, 0, 0)  # CB_GETCURSEL
    if sel < 0:
        raise RuntimeError("CB_GETCURSEL=%s，选择未生效" % sel)
    ln = _sm(combo, 0x0149, sel, 0)  # CB_GETLBTEXTLEN
    buf = ctypes.create_string_buffer(ln + 2)
    _sm(combo, 0x0148, sel, ctypes.cast(buf, ctypes.c_ssize_t))  # CB_GETLBTEXT
    text = buf.value.decode("utf-8", "replace")
    print("选中项确认:", text)
    return text


def run_scene(name, value, expect, do_pick=False, shot=None):
    print("\n===== %s（写入 %s，期望落盘 %s）=====" % (name, value, expect))
    reg_set(value)
    pi, dlg = open_config_box()
    check("%s: 配置框打开" % name, dlg is not None)
    if not dlg:
        stop(pi)
        return
    try:
        time.sleep(0.8)
        if shot:
            screenshot(dlg, shot)
        if do_pick:
            try:
                pick_port_by_mouse(0)
                check("%s: 真实点击选中 COM3" % name, True)
            except RuntimeError as e:
                check("%s: 真实点击选中 COM3" % name, False, str(e))
                screenshot(dlg, r"C:\Users\Wrok\Repos\putty-cn\reports\debug-sceneB-fail.png")
        btn = find_button(dlg, "保存")
        check("%s: 找到保存按钮" % name, btn is not None)
        if not btn:
            return
        _pm(btn, BM_CLICK, 0, 0)
        time.sleep(0.6)
        dismiss_info_box()
        time.sleep(0.4)
        after = reg_get()[0]
        check("%s: 注册表 SerialLine 期望 %s" % (name, expect),
              after == expect, "实际 %r" % after)
    finally:
        cancel = find_button(dlg, "取消")
        if cancel:
            _pm(cancel, BM_CLICK, 0, 0)
        else:
            _pm(dlg, WM_CLOSE, 0, 0)
        time.sleep(0.8)
        check("%s: 配置框已关闭" % name, not win32gui.IsWindow(dlg))
        if is_alive(pi):
            stop(pi)


def main():
    orig_line, orig_proto = reg_get()
    print("原始 SerialLine=%r Protocol=%r（脚本结束恢复）" % (orig_line, orig_proto))
    try:
        run_scene("场景A 回退不落盘", "COM1", "COM1",
                  shot=SHOT)
        run_scene("场景B 用户选择落盘", "COM1",
                  first_real_port(), do_pick=True)
        run_scene("场景C 蓝牙换物理保留原值", "COM4", "COM4")
        run_scene("场景D 正常路径回归", "COM3", "COM3")
    finally:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, SESSION_KEY, 0,
                            winreg.KEY_SET_VALUE) as k:
            if orig_line is None:
                try:
                    winreg.DeleteValue(k, "SerialLine")
                except OSError:
                    pass
            else:
                winreg.SetValueEx(k, "SerialLine", 0, winreg.REG_SZ, orig_line)
            if orig_proto is None:
                try:
                    winreg.DeleteValue(k, "Protocol")
                except OSError:
                    pass
            else:
                winreg.SetValueEx(k, "Protocol", 0, winreg.REG_SZ, orig_proto)
        print("\n已恢复 SerialLine=%r Protocol=%r" % (orig_line, orig_proto))

    bad = [r for r in results if not r[1]]
    print("\n合计 %d 项，失败 %d 项" % (len(results), len(bad)))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
