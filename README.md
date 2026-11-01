# PuTTY 0.85-cn3 中文版（Windows 64 位）

基于 [larryli/PuTTY](https://github.com/larryli/PuTTY)（原 Google Code `puttycn` 项目）的中文翻译源码，使用 MSYS2 + MinGW-w64 + CMake 在本地编译生成的 `putty.exe`、`puttygen.exe`、`plink.exe`、`pscp.exe`、`psftp.exe`、`pageant.exe`、`puttytel.exe`。

在汉化基线之上，本仓库针对国内网络设备运维场景做了界面与交互改造：默认 SSH 连接（主机名留空、端口 22）、「保存」不携带连接目标、串口一键切换且串口线自动匹配本机真实端口、会话页历史连接记录、配置页标题样式统一等。

> 这不是 PuTTY 官方构建，与 Simon Tatham 及 PuTTY 官方项目无关。官方 PuTTY 从未提供中文界面。

## 仓库结构

| 目录 | 内容 |
|------|------|
| `src/` | 完整源码，基线为 larryli/PuTTY master（commit `7ddf3ae`，版本 0.85），含本项目的界面与串口改造 |
| `dist/` | 编译产物：`putty.exe`（图形界面）、`puttygen.exe`（密钥工具）、`plink.exe`、`pscp.exe`、`psftp.exe`（命令行工具）、`pageant.exe`（密钥代理）、`puttytel.exe`（纯 Telnet） |
| `docs/` | 各轮改造的设计与验证记录；`docs/screenshots/` 为报告引用的界面截图 |
| `reports/` | 整改、审计与专项验证报告（翻译质量、命令行汉化、风险扫描、命名清理等） |
| `backup/` | 注册表默认设置快照（不含凭据，见 [`backup/README.md`](backup/README.md)）；二进制备份仅存本地，不入库 |
| `tests/` | 本机回环 SSH 测试脚本（Node.js ssh2） |

源码基线不含上游 git 历史（初始导入为快照）。需要比对上游更新时，clone [larryli/PuTTY](https://github.com/larryli/PuTTY) 后用 `diff -r` 对照。

### 相对上游的改动

| 文件 | 改动 |
|------|------|
| `config.c` | 会话页改版：历史连接记录列表、「打开(L)」按钮、保存提示弹窗；串行口改为本机真实端口下拉框；会话列表高度与滚动条；配置页标题灰底白字加粗；未选中会话时「保存」直接写入「默认设置」 |
| `windows/config.c` | 删除「关于/帮助」按钮，调整底部按钮布局 |
| `windows/controls.c` | 新增 `dlg_info_msg()` 信息弹窗；助记符冲突由断言改为计数；只读组合框点击任意位置展开列表；面板标题自绘样式 |
| `be_list.c`、`config.c` | 连接类型为三个均匀分布的单选（SSH / 串口 / Telnet），协议下拉框移除；Rlogin、SUPDUP 协议及其专属设置页整体删除（Raw 与裸 ssh 连接保留代码供命令行与 SSH 内部机制使用，不在界面出现） |
| `windows/storage.c` | 历史连接记录读写（注册表 `RecentHosts`）；串口枚举（注册表 `SERIALCOMM`，真实串口优先、蓝牙虚拟口后排） |
| `storage.h`、`dialog.h`、`windows/platform.h` | 新增串口枚举、信息弹窗、只读编辑框等接口声明 |
| `unix/storage.c`、`unix/dialog.c` | 对应接口的非 Windows 实现 |
| `conf.h`、`be_list.c` | 默认连接方式 SSH（主机名留空、端口 22），「保存」不携带连接目标；串口参数按华为、华三网络设备 Console 口的标准预置（9600、8 数据位、1 停止位、无校验、无流控）；串口线默认留空，由界面按本机端口自动匹配 |
| `version.h`、`LATEST.VER` | 版本号递增为 0.85-cn3（`TEXTVER`、`SSHVER`、`BINARY_VERSION` 同步） |
| `pscp.c`、`psftp.c`、`cmdgen.c`、`windows/puttygen.c`、`windows/plink.c` | 命令行工具错误提示与 usage 汉化；plink 错误提示补行尾换行 |
| `terminal/terminal.c` | 终端体验改造：窗口尺寸记忆与拖动实时重排（reflow）、内容贴屏边、串口 DEL(0x7F) 退格可跨软换行、窗口标题含端口/串口号 |
| `windows/serial.c` | 串口 realhost 返回串口名供标题显示；串口错误提示翻译修正 |
| `windows/putty.c`、`windows/window.c`、`windows/console.c`、`windows/dialog.c`、`windows/network.c`、`windows/pageant.c`、`ssh/common.c`、`ssh/ca-config.c` | 界面与协议提示的翻译修正：术语统一、标点规范、语义反转与错别字修复 |
| `cmake/platforms/windows.cmake` | 链接 `comctl32`，供组合框与列表控件使用 |

## 安装包（推荐）

从 [Releases](https://github.com/xieml0303-cyber/putty-cn/releases/latest) 下载 `putty-cn-0.85-cn3-setup.exe`（也可直接取仓库里的 [`dist/putty-cn-0.85-cn3-setup.exe`](dist/putty-cn-0.85-cn3-setup.exe)，SHA256 `600584a347a6e319a4a5ddc13c8c91ad177f5a4756333a3b431cfd6e5d7f4f45`），双击运行、按中文向导安装即可：

- 装到当前用户目录 `%LOCALAPPDATA%\Programs\PuTTY-cn`，不需要管理员权限；
- 自动创建开始菜单项与桌面快捷方式，安装完可直接启动；
- 卸载走「设置 → 应用」或开始菜单里的卸载项。

安装包未做代码签名，首次运行时 Windows 可能提示「已保护你的电脑」，点「更多信息 → 仍要运行」即可。

## 免安装版

`dist/` 下的七个程序均为 0.85-cn3 构建、64 位、免安装，可单独取用：

| 文件 | 用途 | 大小（字节） | SHA256 |
|------|------|-------------|--------|
| [`putty.exe`](dist/putty.exe) | 图形界面终端（SSH / Telnet / 串口） | 1,862,029 | `0f72136ee3e2f3f3bcdda0c1e816540b6f5b8118fd4bc1ad5d4c5c08938b0be7` |
| [`puttygen.exe`](dist/puttygen.exe) | 密钥生成 / 转换工具 | 912,067 | `fc647ff0016cfae690b4e00a3eccf260c02702d55fc40671fb006fd368296100` |
| [`plink.exe`](dist/plink.exe) | 命令行 SSH 客户端 | 1,418,311 | `dc6396281f6ac2d1c20b78b0c443bbc7baf0c146c84c8a0f6a0470420e6c6bc0` |
| [`pscp.exe`](dist/pscp.exe) | SCP 文件传输 | 1,416,663 | `85947512a0cedcf263beb4a91d166f7e5cd6d78dcb65d8d0b32f3f8f5cf02f61` |
| [`psftp.exe`](dist/psftp.exe) | SFTP 文件传输 | 1,432,110 | `1cdb43a0098adfac08b234cf576703c37c75eadd4a755fc217f58bffca97481a` |
| [`pageant.exe`](dist/pageant.exe) | 密钥代理（SSH 公钥认证） | 758,229 | `97b120fa1a71f6d7050e7eb95e25c228c4fbaba89629bb1eac649b42a124a24f` |
| [`puttytel.exe`](dist/puttytel.exe) | 纯 Telnet 客户端（不含 SSH） | 979,863 | `376cafd7cd892fc5607668c783a4ed7824932fa705824a5e78d4e20455ea4acd` |

- 仅依赖系统 DLL（ADVAPI32、COMCTL32、GDI32、IMM32、KERNEL32、SHELL32、USER32、comdlg32、msvcrt、ole32），无需 MinGW 运行库。
- `putty.exe` 启动后窗口标题为「PuTTY 配置」（英文版为 "PuTTY Configuration"）。
- 命令行工具可执行 `plink -V` 确认版本，输出应为 `发布版 0.85-cn3`。
- 运行前建议按上表校验 SHA256。
- **哈希表的正确用法**：PE 头嵌入链接时间戳，同一份源码两次构建的产物哈希必然不同（实测已验证），因此哈希只用于**校验下载完整性**，不能用于判断「是不是最新版」；判断版本请用 `plink -V` 输出与 exe 文件属性。

## 串口默认配置

本构建将默认连接方式设为**串口**，串口参数按华为、华三网络设备 Console 口的标准预置：

| 参数 | 默认值 |
|------|--------|
| 串口线 | 自动匹配本机真实端口（真实串口优先，蓝牙虚拟口后排） |
| 波特率 | 9600 |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验 | 无 (None) |
| 流控 | 无 (None) |

打开程序默认为 SSH 连接；需要串口时点选「串口」单选按钮，连接参数即按下表预置。「串行口(N)」是从注册表 `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM` 枚举出的本机端口下拉框：

- 当前配置的端口不存在时（例如从别的机器带过来的 `COM1`），会自动回退到列表第一项，不会显示一个打不开的端口；
- 下拉框为只读，只能从列表选择，避免手输出错；
- 点击输入框任意位置即可展开列表，不必精确点右侧箭头。

需要 SSH 时点选「SSH」单选按钮，原「主机名称」输入框恢复为可编辑状态。

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
| T5 | Pageant 加载 ed25519 PPK + plink 无密码公钥认证 | PASS |

界面汉化与串口改造未影响协议层行为。测试记录见 [`docs/2026-10-17-PuTTY功能测试.md`](docs/2026-10-17-PuTTY功能测试.md)，串口改造见 [`docs/2026-10-18-串口默认值自动匹配.md`](docs/2026-10-18-串口默认值自动匹配.md)。

## 已知限制

1. **GUI 版不支持 `-batch`**。该选项属于 plink，传给 `putty.exe` 会弹「命令行错误」。
2. **`putty host command` 写法有陷阱**。`putty 127.0.0.1 "echo hello"` 会把 `"echo hello"` 当作端口号（atoi 得 0），报「无法分配请求的地址」。远程命令请用 `-m <命令文件>` 传入，或改用 plink。
3. **只认 PuTTY 原生 `.ppk` 私钥**。OpenSSH 新格式与旧 PEM 格式均无法加载；可用 puttygen 图形界面导入转换，或用 [`tests/gen-ppk.js`](tests/gen-ppk.js) 生成 ed25519 PPK v3。
4. **串口下拉框只读**，只能从本机枚举到的端口中选择，不能手输 `\\.\COM10` 这类设备路径；本机没有任何串口时列表为空。Unix 版不受影响。
5. **`pageant.exe` 与 `puttytel.exe` 已随 `dist/` 发布**（0.85-cn3 构建）。`pageant` 为密钥代理，`puttytel` 为不含 SSH 的 Telnet 客户端；两者功能与官方 0.85 一致，仅界面汉化。命令行工具为实验性汉化，错误提示可能存在个别未翻译或格式瑕疵。

## 自行构建

```bash
# MSYS2（MinGW-w64 环境）中
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make perl
cd src
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --target putty puttygen plink pscp psftp pageant puttytel -j
```

产物为 `src/build/` 下的同名 exe，复制到 `dist/` 即可发布。本仓库中的二进制即由上述流程产出（gcc 16.2.0 / cmake 4.4.4 / ninja）。

## 复现测试

```bash
cd tests
npm install
node server-main.js          # 终端 A：启动测试服务器
powershell -File test-putty.ps1   # 终端 B：运行四项测试
```

测试脚本仅用于本机回环验证，不会连接外部主机。脚本以 `$PSScriptRoot` 定位自身目录，可随仓库移动。

## 文档索引

- 汉化过程：[`docs/2026-10-17-PuTTY汉化.md`](docs/2026-10-17-PuTTY汉化.md)
- 翻译质量修复：[`reports/2026-10-18-翻译质量修复与回归报告.md`](reports/2026-10-18-翻译质量修复与回归报告.md)
- 串口改造：[`docs/2026-10-18-串口默认值自动匹配.md`](docs/2026-10-18-串口默认值自动匹配.md)、[`docs/2026-10-18-串口下拉框.md`](docs/2026-10-18-串口下拉框.md)
- 会话页与配置页：[`docs/2026-10-18-会话页历史记录改版.md`](docs/2026-10-18-会话页历史记录改版.md)、[`docs/2026-10-18-配置窗口标题样式.md`](docs/2026-10-18-配置窗口标题样式.md)
- 命令行工具汉化：[`reports/2026-10-18-命令行工具汉化完成报告.md`](reports/2026-10-18-命令行工具汉化完成报告.md)
- 风险扫描与整改：[`reports/风险扫描-putty-cn-20261018.md`](reports/风险扫描-putty-cn-20261018.md)、[`reports/2026-10-18-M3-M4-M6整改与完整审计.md`](reports/2026-10-18-M3-M4-M6整改与完整审计.md)

## 许可证

PuTTY 及其衍生代码遵循 MIT 许可证，见 [`LICENCE`](LICENCE)。中文翻译版权归 larryli/PuTTY 项目贡献者所有。
