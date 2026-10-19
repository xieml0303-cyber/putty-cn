# 汉化版覆盖 Program Files 原版 — 2026-10-19

## 背景

`C:\Program Files\PuTTY\putty.exe` 自安装以来一直是英文原版 0.85（1,341,080 字节），此前多轮改造只部署到用户目录，原因记录为「当前会话无管理员权限，Program Files 不可写」。用户本轮确认可以覆盖。

## 执行内容

1. **覆盖前核对**：`C:\Program Files\PuTTY\putty.exe` 的 md5 `07c0fc066f6a71068b70a6cf9e22bf60` 与 `backup/putty.exe.orig-0.85-en` 完全一致，确认备份就是可回滚的原版。
2. **发现 AppData 落后**：桌面快捷方式指向的 `AppData\Local\Programs\PuTTY\putty.exe` 是 10-18 23:59 的旧构建（`449c2bd1…`，1,845,780 字节），而最新构建是 10-19 21:55（`9cc7439c…`，1,848,616 字节）。先把最新构建同步进 AppData。
3. **提权覆盖 Program Files**：普通 `cp` 报 Permission denied，改用 `Start-Process cmd -Verb RunAs` 走 UAC 复制，UAC 确认后成功。
4. **验证**：三处 `src/build/putty.exe`、`dist/putty.exe`、`AppData`、`Program Files` 的 md5 全部为 `9cc7439c3ef632f85e9f727c19dc217b`；启动 `C:\Program Files\PuTTY\putty.exe`，窗口标题为「PuTTY 配置」，中文界面生效，进程正常退出。

## 影响面

- 开始菜单「PuTTY (64-bit)\PuTTY.lnk」指向 `C:\Program Files\PuTTY\putty.exe`，现在打开即为汉化版。
- 桌面快捷方式指向 AppData 那份，同样是最新汉化版。
- 两处共用同一用户注册表配置（`HKCU\Software\SimonTatham\PuTTY`），会话列表与默认设置不受影响。

## 未改动项

`C:\Program Files\PuTTY` 下的 `puttygen.exe`、`pageant.exe`、`plink.exe`、`pscp.exe`、`psftp.exe`、`putty.chm` 仍为英文原版。本次只覆盖了 GUI 主程序。

## 回滚方法

```powershell
Start-Process cmd -Verb RunAs -ArgumentList '/c copy /Y "C:\Users\Wrok\Repos\putty-cn\backup\putty.exe.orig-0.85-en" "C:\Program Files\PuTTY\putty.exe"'
```

原版备份 md5：`07c0fc066f6a71068b70a6cf9e22bf60`。

## 待定

是否把 puttygen / pageant / plink / pscp / psftp 也换成 `src/build` 里的汉化构建（命令行工具此前已汉化）。这会改变脚本调用的输出文案，需要用户确认。
