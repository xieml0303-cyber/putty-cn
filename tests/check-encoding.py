# -*- coding: utf-8 -*-
"""控制台编码自检与修复建议。

用法：python tests/check-encoding.py

背景：本机 Git Bash 下 Python 为 UTF-8（utf8_mode=1），而 PowerShell
控制台默认 gb2312。两者混用时中文会乱码；再叠加 `iconv -f GBK` 会把
UTF-8 字节按 GBK 解码，输出彻底损坏。
"""
import sys
import os
import subprocess


def report():
    print("Python 版本       :", sys.version.split()[0])
    print("stdout 编码       :", sys.stdout.encoding)
    print("stdin 编码        :", sys.stdin.encoding)
    print("文件系统编码      :", sys.getfilesystemencoding())
    print("UTF-8 模式        :", sys.flags.utf8_mode)
    print("PYTHONIOENCODING  :", os.environ.get("PYTHONIOENCODING", "(未设置)"))
    print()

    out = sys.stdout.encoding or ""
    if out.lower().replace("-", "") in ("utf8", "utf8mb4"):
        print("判定：Python 输出为 UTF-8。")
        print("结论：不要对输出再套 iconv 或 chcp，直接打印即可。")
    else:
        print("判定：Python 输出不是 UTF-8（%s）。" % out)
        print("建议：设 PYTHONIOENCODING=utf-8，或运行前执行 chcp 65001。")


def check_powershell():
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "[Console]::OutputEncoding.WebName"],
            capture_output=True, text=True, timeout=15)
        enc = r.stdout.strip()
        print()
        print("PowerShell 控制台编码 :", enc or "(读取失败)")
        if enc and enc.lower() not in ("utf-8", "utf8"):
            print("建议：脚本首行加 [Console]::OutputEncoding = "
                  "[System.Text.Encoding]::UTF8")
            print("      或在 PowerShell 里先执行 chcp 65001")
    except Exception as e:
        print()
        print("PowerShell 检测跳过：", e)


if __name__ == "__main__":
    report()
    check_powershell()
