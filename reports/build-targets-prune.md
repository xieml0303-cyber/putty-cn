# 构建目标精简：只交付 putty.exe

**日期**：2026-11（`feat/confirm-dialog-redesign` 分支）
**范围**：CMake 构建目标、`build.ps1`、Inno Setup 安装脚本、README、CI
**结论**：移除 `puttygen` / `plink` / `pscp` / `psftp` / `psocks` / `pageant` / `puttytel` 七个构建目标，只保留 `putty`。源码文件全部保留在树上。

---

## 1. 起因

全量构建（`ninja -C build`，从零编译）时 `puttytel.exe` 链接失败，缺 11 个符号：

```
cn_confirm_dialog_texts   cn_confirm_save_dialog
cn_launch_texts           cn_quit_texts
sftp_selftest_confirm_save    sftp_selftest_confirm_quit
sftp_selftest_confirm_launch
sftp_show_confirm_launch      sftp_show_confirm_quit
sftp_show_confirm_save        sftp_show_confirm_overwrite
```

### 1.1 是否为本次改动引入

在 HEAD（`c6ce0e4`）的干净检出上单独编译 `puttytel` 验证：

```
$ git worktree add /tmp/putty-head-check HEAD --detach
$ cmake -S /tmp/putty-head-check/src -B /tmp/headbuild -G Ninja
$ ninja -C /tmp/headbuild puttytel
NINJA_RC=1
undefined reference to `cn_confirm_dialog_texts'
... （共 10 个）
```

HEAD 上同样失败。其中 7 个符号在本分支之前就已存在，3 个（`sftp_selftest_confirm_*`）是本次弹窗改造新增。**属既有缺陷，非本次引入**，仅因以往的增量构建从未触碰 `puttytel` 目标而未暴露。

### 1.2 根因

`dialog.c` 与 `putty.c` 是 `putty` 和 `puttytel` 两个目标共用的源文件，二者都会引用弹窗入口；但弹窗的真实实现在 `sftpwin.c`，而 `sftpwin.c` 只编进 `putty` 目标。`puttytel` 通过 `stubs/no-sftp.c` 提供空实现绕开整套 SFTP，但该桩文件只覆盖了旧接口（`sftp_window_open` 等 9 个），从未补齐后续新增的弹窗入口。

补充证据：`puttytel.rc` 中连 `IDI_QHELP` 等弹窗图标资源都未定义，只有 `IDI_MAINICON` / `IDI_CFGICON`。说明该目标在本汉化分支中从未随 `putty` 同步演进。

---

## 2. 决策：移除而非修复

修复有两条路径，均被否决：

| 方案 | 评估 |
|------|------|
| 补 `no-sftp.c` 空实现 | 链接可通过，但 `dlg_confirm_quit` 在 `puttytel` 中**可达**（配置框 `WM_CLOSE` 路径，见 `windows/dialog.c:408`）。空实现会使用户点关闭时无法正常退出配置框，等于用假实现换假实现 |
| 把弹窗模板抽成独立文件 | 技术上可行（实测模板区间零 `SftpWin` 依赖，只牵 11 个绘图辅助函数 + `cn_question_icon`），但需迁移约 1800 行并改动两个目标的源文件划分，改动面远大于本次任务 |
| 直接复用 `sftpwin.c` | 否决。该文件含 201 处 SFTP/SSH 引用，等于是给纯 Telnet 客户端塞进整套 SFTP 协议栈 |

主人决定：**这些程序在本汉化分支中从未维护，全部移除，只交付 `putty` 本体。**

### 2.1 维护状态的客观数据

统计各程序的源码与资源文件中文行数：

| 程序 | 源码中文行 | rc 中文行 | 判断 |
|------|-----------|----------|------|
| `puttygen` | 131 | 23 | 汉化较完整 |
| `pageant` | 50 | 26 | 汉化较完整 |
| `plink` | 67 | 1 | 仅部分错误提示 |
| `puttytel` | 0 | 1 | 未汉化 |
| `pscp` | 0 | 1 | 未汉化 |
| `psftp` | 0 | 1 | 未汉化 |

`puttygen` 与 `pageant` 的汉化程度高于其余，此差异已向主人说明，主人确认仍全部移除。

---

## 3. 依赖核验：putty 本体是否依赖这些程序

结论：**零依赖**。三项独立证据。

### 3.1 源码级

在 `putty` 目标的全部专属源文件（`putty.c`、`window.c`、`sftpwin.c`、`sftpxfer.c`、`help.c`、`keyimport.c`）中搜索其他 exe 的文件名或路径引用，结果为空。

### 3.2 进程创建级

`window.c:3036` 是 `putty` 中唯一的 `CreateProcess` 调用，其可执行文件参数来自：

```c
GetModuleFileName(NULL, b, sizeof(b) - 1);
...
CreateProcess(b, cl, ...);
```

即**重新启动自身**（用于「复制会话」/「重启会话」），不是拉起其他 exe。

### 3.3 功能级

| 功能 | 依赖关系 |
|------|---------|
| SFTP 双栏文件传输 | 走内置 `ssh/sftp.c` + `ssh/sftpcommon.c`（见 `windows/CMakeLists.txt` 中 `putty` 目标源列表），不调用 `psftp.exe` |
| Pageant 密钥代理 | 通过共享内存 `PageantRequest%08x` 通信（`windows/agent-client.c:46`）。缺 `pageant.exe` 仅影响「本机是否有代理可连接」，不影响 putty 自身运行 |
| 密钥导入 | `keyimport.c` 编入 `putty` 目标，PPK 加载不依赖 `puttygen.exe` |

---

## 4. 改动清单

### 4.1 构建系统

| 文件 | 改动 |
|------|------|
| `src/CMakeLists.txt` | 移除 `plink`、`pscp`、`psftp`、`psocks` 四个 `add_executable` 及其 `be_list` / `target_link_libraries` / `installed_program`；原位加注释说明取舍与恢复方法 |
| `src/windows/CMakeLists.txt` | 移除 `pageant`、`puttytel`、`puttygen` 三个目标，同上加注释 |

### 4.2 构建脚本

`build.ps1` 两处：

- 默认 `-Targets` 由七个改为 `@("putty")`。
- 版本号输出原为 `& "$BuildDir\plink.exe" -V`，`plink` 移除后该行会报错。改为新增 `Get-TextVer` 函数，从 `src/version.h` 的 `TEXTVER` 宏读取，`-VersionOnly` 分支与构建结尾共用。

选择理由：`putty.exe` 是 `WIN32_EXECUTABLE`（GUI 子系统），`-V` 不会打印到控制台，即便保留也不可用。

### 4.3 安装包

`installer/putty-cn.iss`：

- `[Files]` 段移除六个 exe，只保留 `putty.exe`、`LICENCE`、`README.md`。
- `[Icons]` 段移除六个开始菜单项，保留 PuTTY、使用说明、卸载、桌面快捷方式。

### 4.4 文档与 CI

| 文件 | 改动 |
|------|------|
| `README.md` | 首段交付物说明、`dist/` 目录说明、哈希基准表（七行收为一行）、`plink -V` 版本确认方式、`T5` 功能验证项、已知限制第 2/3/5 条、构建命令 `--target` 列表 |
| `.github/workflows/ci.yml` | 两处「七个交付物」注释更新。构建步骤本就不带 `--target`（注释已说明原因），功能不受影响 |

### 4.5 产物清理与安装包重打

- 删除 `dist/` 下六个旧 exe。
- 删除 `dist/putty-cn-0.85-cn4-setup.exe` 旧包：内含六个已移除程序的安装条目，保留会误导。
- 用 ISCC 重新编译安装包，产物复制到 `dist/`。

> **勘误**：本节初版曾写「本机未安装 Inno Setup，安装包未重新打包」。该结论错误 —— 当时只检查了 `C:\Program Files (x86)\Inno Setup 6` 默认路径，未做全盘搜索。实际编译器位于 `%LOCALAPPDATA%\Programs\Python\Python313\InnoSetup\ISCC.exe`（Inno Setup 7.1.0），本机历史上数十次打包均使用该路径，`workspace/reports/2026-11-05-发布v0.85-cn4.md` 有明确记录。经主人指出后已重打包并完成安装验证。

---

## 5. 验证

### 5.1 构建

```
$ ninja -C build -t clean   # 清理 449 个产物
$ ninja -C build
NINJA_RC=0
error: 0 处
warning: 36 处
```

36 条警告已逐条核对来源，全部为 `const` 限定符丢弃（`-Wdiscarded-qualifiers`）与 `chacha20-poly1305.c:125` 的初始化字符串过长，在 HEAD 干净检出上**完全一致**，属上游既有问题。

产物列表（`src/build/*.exe`）：

```
bidi_gettype  bidi_test  putty  test_bpp  test_cert_expr  test_conf
test_decode_utf8  test_host_strfoo  test_lineedit  test_screenshot
test_split_into_argv  test_terminal  test_tree234  test_unicode_norm
test_wildcard  testcrypt
```

除单元测试程序外，可执行交付物仅 `putty.exe`。`puttygen` / `plink` / `pscp` / `psftp` / `psocks` / `pageant` / `puttytel` 均已消失。

### 5.2 自检（删除后重新全量执行）

| 自检 | 命令 | 结果 |
|------|------|------|
| 确认保存弹窗 | `putty.exe -confirm-save` | 通过 25 项，失败 0 项 |
| 确认退出弹窗 | `putty.exe -confirm-quit` | 通过 18 项，失败 0 项 |
| 连接前确认弹窗 | `putty.exe -confirm-launch` | 通过 14 项，失败 0 项 |
| 界面全量 | `putty.exe -sftp-win -P 2222 -pw ... -hostkey ... puttytest@127.0.0.1` | 通过 196 项，失败 0 项 |
| 单元测试 | `test_conf.exe` | 退出码 0 |

合计 **253 项通过，0 失败**。其中界面全量自检包含 SFTP 双栏的完整流程，直接证明移除 `psftp.exe` 未影响内置文件传输功能。

### 5.3 构建脚本

```
$ powershell -File ./build.ps1 -VersionOnly
发布版 0.85-cn4

$ powershell -File ./build.ps1
Build OK: putty
发布版 0.85-cn4
```

### 5.4 安装包编译与安装验证

编译器：`%LOCALAPPDATA%\Programs\Python\Python313\InnoSetup\ISCC.exe`（Inno Setup 7.1.0）。

```
$ ISCC.exe installer/putty-cn.iss
Successful compile (1.828 sec).
Resulting Setup program filename is:
C:\Users\Wrok\Repos\putty-cn\installer\output\putty-cn-0.85-cn4-setup.exe
```

ISCC 编译日志中的压缩清单仅三项：

```
Compressing: dist\putty.exe
Compressing: LICENCE
Compressing: README.md
```

产物大小 **3,011,695** 字节，较上一版旧包（3,257,223）减少 245,528 字节，与移除六个程序的预期一致。

**安装实测**（静默安装到临时目录后逐项核对，非仅凭编译日志）：

```
$ Start-Process putty-cn-0.85-cn4-setup.exe \
    -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART', \
                  '/DIR=C:\tmp_install_test','/LOG=C:\tmp_install_test.log' -Wait
EXIT=0
```

安装目录实际内容：

```
LICENCE              1,283
README.md           11,258
putty.exe        3,166,671
unins000.dat         3,303
unins000.exe     4,379,126
```

六个旧程序无一存在。`putty.exe` 的 SHA256 与 `src/build/putty.exe` 完全一致
（`f353d789dd5e886dd6d9126db4b41461885931db302724835d5f16fa0e97dd9a`），
且运行该安装副本执行 `-confirm-save` 自检通过 25 项、0 失败。

卸载验证：执行 `unins000.exe /VERYSILENT` 后安装目录清空，注册表
`HKCU\...\Uninstall` 下无 PuTTY 残留项。

> 打包过程踩到的坑：在 Git Bash 中直接调用安装包时，MSYS 会把 `/VERYSILENT`
> 之类的开关当成路径转换成 `C:/Program Files/Git/VERYSILENT`，导致安装程序
> 识别不到参数而卡在交互界面。须改用 PowerShell 的 `Start-Process -ArgumentList`
> 传参，或使用 `//VERYSILENT` 双斜杠写法。

### 5.5 已知遗留

- `src/unix/CMakeLists.txt` 与 `src/doc/CMakeLists.txt` 中仍有 `plink` / `pscp` / `psftp` / `puttygen` / `pageant` 相关定义。二者不参与 Windows 构建，本次未改动，避免扩大改动面。
- 七个程序的源码文件全部保留在树上。需要恢复时，把对应的 `add_executable` 段加回 CMake 即可；`puttytel` 若要恢复，还须先解决第 1.2 节的桩缺失问题。
- 安装包未做代码签名，首次运行仍会触发 SmartScreen 提示（与历史版本一致）。
