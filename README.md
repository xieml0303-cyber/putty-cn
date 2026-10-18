# PuTTY 0.85 中文版（Windows 64 位）

基于 [larryli/PuTTY](https://github.com/larryli/PuTTY)（原 Google Code `puttycn` 项目）的中文翻译源码，使用 MSYS2 + MinGW-w64 + CMake 在本地编译生成的 `putty.exe`。

> 这不是 PuTTY 官方构建，与 Simon Tatham 及 PuTTY 官方项目无关。官方 PuTTY 从未提供中文界面。

## 仓库结构

| 目录 | 内容 |
|------|------|
| `src/` | 完整源码，基线为 larryli/PuTTY master（commit `7ddf3ae`，版本 0.85），含本项目的界面改造 |
| `dist/` | 编译产物 `putty.exe` |
| `docs/` | 汉化过程、功能测试与各轮改造记录；`docs/screenshots/` 为报告引用的界面截图 |
| `backup/` | 改造前的原版/旧版 exe 与注册表默认设置备份 |
| `tests/` | 本机回环 SSH 测试脚本（Node.js ssh2） |

源码基线不含上游 git 历史（初始导入为快照）。需要比对上游更新时，clone [larryli/PuTTY](https://github.com/larryli/PuTTY) 后用 `diff -r` 对照。

### 相对上游的改动

| 文件 | 改动 |
|------|------|
| `config.c` | 会话页改版：历史连接记录列表、「打开(L)」按钮、保存提示弹窗、串口下拉框、会话列表高度与滚动条、「退出时关闭窗口」固定显示「从不」 |
| `windows/config.c` | 删除「关于/帮助」按钮，调整底部按钮布局 |
| `windows/controls.c` | 新增 `dlg_info_msg()` 信息弹窗接口 |
| `windows/storage.c` | 历史连接记录读写（注册表 `RecentHosts`）、本机串口枚举 |
| `unix/storage.c`、`unix/dialog.c` | 对应接口的非 Windows 实现 |
| `storage.h`、`dialog.h` | 新增接口声明 |
| `conf.h`、`be_list.c` | 默认连接方式与串口默认参数（华为/华三 Console 标准） |

## 下载

直接使用仓库中的 [`dist/putty.exe`](dist/putty.exe)（1,845,159 字节，64 位，免安装）。

- 仅依赖系统 DLL（ADVAPI32、GDI32、IMM32、KERNEL32、SHELL32、USER32、comdlg32、msvcrt、ole32），无需 MinGW 运行库。
- 启动后窗口标题为「PuTTY 配置」（英文版为 "PuTTY Configuration"）。
- 运行前建议校验 SHA256：

```text
98c9ee951aecb9f5c9b38dfbfc2f956fb896d2f17db5bf411bfe3f280c0dc4c1
```

## 串口默认配置（华为 / 华三 Console 标准）

本构建将默认连接方式设为**串口**，串口参数按华为、华三网络设备 Console 口的标准预置：

| 参数 | 默认值 |
|------|--------|
| 串口线 | COM1 |
| 波特率 | 9600 |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验 | 无 (None) |
| 流控 | 无 (None) |

打开程序即处于串口模式，接上 USB 转串口线后把「串口线」改成实际端口（如 COM3）即可连接。需要 SSH 时点选「SSH」单选按钮。

## 保存配置

PuTTY 不会自动保存设置。改完参数后需要点窗口右下角的「保存」按钮：

- 左侧会话列表未选中任何会话时，点「保存」会直接存入「默认设置」，下次启动沿用；
- 也可以先在左侧选中「默认设置」（或输入新会话名）再保存。

原版 PuTTY 在未选中会话时点「保存」只会「嘟」一声、不做任何提示；本构建改为直接保存到「默认设置」，避免设置改完却丢失。

## 功能验证

在隔离的本机回环环境（Node.js ssh2 测试服务器，监听 127.0.0.1:2222，不访问外部网络）实测：

| 编号 | 测试项 | 结果 |
|------|--------|------|
| T1 | 密码认证 + 远程命令执行 + 中文回显 | PASS |
| T2 | 公钥认证（ed25519，PPK v3）+ 命令执行 | PASS |
| T3 | 本地端口转发 `-L 18080:127.0.0.1:18081` | PASS |
| T4 | 交互式终端会话 | PASS |

界面汉化未影响协议层行为。完整报告见 [`docs/2026-10-17-PuTTY功能测试.md`](docs/2026-10-17-PuTTY功能测试.md)，汉化过程见 [`docs/2026-10-17-PuTTY汉化.md`](docs/2026-10-17-PuTTY汉化.md)。

## 已知限制

1. **GUI 版不支持 `-batch`**。该选项属于 plink，传给 `putty.exe` 会弹「命令行错误」。
2. **`putty host command` 写法有陷阱**。`putty 127.0.0.1 "echo hello"` 会把 `"echo hello"` 当作端口号（atoi 得 0），报「无法分配请求的地址」。远程命令请用 `-m <命令文件>` 传入，或改用 plink。
3. **只认 PuTTY 原生 `.ppk` 私钥**。OpenSSH 新格式与旧 PEM 格式均无法加载；可用 puttygen 图形界面导入转换，或用 [`tests/gen-ppk.js`](tests/gen-ppk.js) 生成 ed25519 PPK v3。
4. 仅编译了 `putty.exe`；`puttygen`、`plink`、`pscp`、`psftp`、`pageant` 未包含在本仓库，可自行从 larryli/PuTTY 源码构建。

## 自行构建

```bash
# MSYS2（MinGW-w64 环境）中
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make perl
cd src
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --target putty -j
```

产物为 `src/build/putty.exe`，复制到 `dist/` 即可发布。本仓库中的二进制即由上述流程产出（gcc 16.2.0 / cmake 4.4.4 / ninja，288 个目标全部成功）。

## 复现测试

```bash
cd tests
npm install
node server-main.js          # 终端 A：启动测试服务器
powershell -File test-putty.ps1   # 终端 B：运行四项测试
```

测试脚本仅用于本机回环验证，不会连接外部主机。

## 许可证

PuTTY 及其衍生代码遵循 MIT 许可证，见 [`LICENCE`](LICENCE)。中文翻译版权归 larryli/PuTTY 项目贡献者所有。
