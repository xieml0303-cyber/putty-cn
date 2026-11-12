# PuTTY 0.85-cn5 中文版（Windows 64 位）

基于 [larryli/PuTTY](https://github.com/larryli/PuTTY)（原 Google Code `puttycn` 项目）的中文翻译源码，使用 MSYS2 + MinGW-w64 + CMake 在本地编译生成 `putty.exe`。

> 本构建只交付 `putty.exe` 本体（含内置 SFTP 双栏文件传输）。上游的 `puttygen.exe`、`plink.exe`、`pscp.exe`、`psftp.exe`、`pageant.exe`、`puttytel.exe` 已从构建与安装包中移除 —— 它们在本汉化分支中从未随 `putty.exe` 同步维护。

在汉化基线之上，本仓库针对国内网络设备运维场景做了界面与交互改造：默认 SSH 连接（主机名留空、端口 22）、「保存」不携带连接目标、串口一键切换且串口线自动匹配本机真实端口、会话页历史连接记录与目录分组（支持条目内就地改名）、配置页标题样式统一等。

> 这不是 PuTTY 官方构建，与 Simon Tatham 及 PuTTY 官方项目无关。官方 PuTTY 从未提供中文界面。

## 仓库结构

| 目录 | 内容 |
|------|------|
| `src/` | 完整源码，基线为 larryli/PuTTY master（commit `7ddf3ae`，版本 0.85），含本项目的界面与串口改造 |
| `installer/` | Inno Setup 安装包脚本（`putty-cn.iss`）与图标资源 |
| `dist/` | 本地构建产物（不入版本库）：`putty.exe`（图形界面，含 SFTP 双栏） |
| `workspace/` | 本地资料夹（不入库）：报告、设计文档、注册表备份与本机测试脚本 |

源码基线不含上游 git 历史（初始导入为快照）。需要比对上游更新时，clone [larryli/PuTTY](https://github.com/larryli/PuTTY) 后用 `diff -r` 对照。

### 相对上游的改动

| 文件 | 改动 |
|------|------|
| `config.c` | 会话页改版：历史连接记录列表、「打开(L)」按钮、保存提示弹窗；串行口改为本机真实端口下拉框；会话列表高度与滚动条；配置页标题灰底白字加粗；未选中会话时「保存」直接写入「默认设置」；会话目录分组（根层显示目录与未归类会话，双击目录进入、首行「..」返回；「新目录」按钮新建，右键菜单支持重命名/删除/移入/移出目录；目录行显示条目数；目录内发起的连接自动归入该目录） |
| `windows/config.c` | 删除「关于/帮助」按钮，调整底部按钮布局 |
| `windows/controls.c` | 新增 `dlg_info_msg()` 信息弹窗；助记符冲突由断言改为计数；只读组合框点击任意位置展开列表；面板标题自绘样式；会话列表主机名/端口分栏竖线改为逐行自绘（避免整列重绘闪烁），并导出文本区左右边界供条目内就地改名对齐 |
| `be_list.c`、`config.c` | 连接类型为三个均匀分布的单选（SSH / 串口 / Telnet），协议下拉框移除；Rlogin、SUPDUP 协议及其专属设置页整体删除（Raw 与裸 ssh 连接保留代码供命令行与 SSH 内部机制使用，不在界面出现） |
| `windows/storage.c` | 历史连接记录读写（注册表 `RecentHosts`）；串口枚举（注册表 `SERIALCOMM`，真实串口优先、蓝牙虚拟口后排） |
| `storage.h`、`dialog.h`、`windows/platform.h` | 新增串口枚举、信息弹窗、只读编辑框等接口声明 |
| `unix/storage.c`、`unix/dialog.c` | 对应接口的非 Windows 实现 |
| `conf.h`、`be_list.c` | 默认连接方式 SSH（主机名留空、端口 22），「保存」不携带连接目标；串口参数按华为、华三网络设备 Console 口的标准预置（9600、8 数据位、1 停止位、无校验、无流控）；串口线默认留空，由界面按本机端口自动匹配 |
| `version.h`、`LATEST.VER` | 版本号递增为 0.85-cn5（`TEXTVER`、`SSHVER`、`BINARY_VERSION` 同步） |
| `windows/sftpwin.c` | 文件传输窗口整体改造：远端面板移至左侧、列头居中与内容右对齐（header 子类自绘）、列宽按内容实测、文件操作迁入右键菜单、传输按钮随选中启用、目录递归传输、拖放上传、同名覆盖三按钮提示、危险按钮悬停高光 |
| `windows/cn-toast.c`、`windows/cn-toast.h` | 抽取的公共 Toast 轻提示模块（分层窗口 + 自绘圆角 + 滑入/停留/淡出），供会话窗口与文件传输窗口共用 |
| `windows/window.c`、`windows/win-gui-seat.h` | 「会话」菜单新增「重启会话」（离线可点，用于重连）；新增顶级「日志」菜单，含「开始记录日志」「停止记录日志」，操作后弹 Toast 反馈 |
| `terminal/terminal.c` | 日志写入支持中文：新增 `logtraffic_cp()` 按码点手写 UTF-8 编码，修复原实现丢弃非 ASCII 字符导致中文日志只剩空格的问题 |
| `config.c`、`dialog.c`、`dialog.h`、`windows/controls.c` | 会话页「记录日志」改为复选框；「密钥」按钮改用输入框高度（`cn_editheight`）与同排「凭据（密码）」框精确对齐；勾选后焦点交还左侧分类树，避免切页后虚线框消失 |
| `pscp.c`、`psftp.c`、`cmdgen.c`、`windows/puttygen.c`、`windows/plink.c` | 命令行工具错误提示与 usage 汉化；plink 错误提示补行尾换行。**注**：这些程序已不参与构建，改动仅保留在源码中 |
| `terminal/terminal.c` | 终端体验改造：窗口尺寸记忆与拖动实时重排（reflow）、内容贴屏边、串口 DEL(0x7F) 退格可跨软换行、窗口标题含端口/串口号 |
| `windows/serial.c` | 串口 realhost 返回串口名供标题显示；串口错误提示翻译修正 |
| `windows/putty.c`、`windows/window.c`、`windows/console.c`、`windows/dialog.c`、`windows/network.c`、`windows/pageant.c`、`ssh/common.c`、`ssh/ca-config.c` | 界面与协议提示的翻译修正：术语统一、标点规范、语义反转与错别字修复 |
| `windows/dialog.c` | 修复串口模式「端口号／波特率」下拉框初始不显示箭头：新增 `cn_hostbox_sync_arrow()`，拦截 `CB_ADDSTRING`／`CB_DELETESTRING`／`CB_RESETCONTENT`，列表由空变非空时主动重绘按钮区（原先箭头被背景色盖掉后无还原路径） |
| `cmake/platforms/windows.cmake` | 链接 `comctl32`，供组合框与列表控件使用 |

## 安装包（推荐）

从 [Releases](https://github.com/xieml0303-cyber/putty-cn/releases/latest) 下载 `putty-cn-0.85-cn5-setup.exe`，双击运行、按中文向导安装即可：

- 装到当前用户目录 `%LOCALAPPDATA%\Programs\PuTTY-cn`，不需要管理员权限；
- 自动创建开始菜单项与桌面快捷方式，安装完可直接启动；
- 卸载走「设置 → 应用」或开始菜单里的卸载项。

安装包未做代码签名，首次运行时 Windows 可能提示「已保护你的电脑」，点「更多信息 → 仍要运行」即可。

> 安装包每次重打都会嵌入新的构建时间戳，哈希必然变化，故不在 README 中固定；请以对应 Release 页面公布的 SHA256 为准。

## 免安装版

`putty.exe` 为 0.85-cn5 构建、64 位、免安装。安装包仅含该程序；如需单文件版本，可按
[构建说明](#自行构建)自行编译，或用解压工具从安装包中取出。

下表为 0.85-cn5 构建产物的基准值，可用于核对自行构建或从安装包提取的文件：

| 文件 | 用途 | 大小（字节） | SHA256 |
|------|------|-------------|--------|
| `putty.exe` | 图形界面终端（SSH / Telnet / 串口 / SFTP 双栏） | 3,018,147 | `e7aa6b167471dd02c49d5839cc2ef4417a7d0dbd3462b49486d241ffb14537b1` |

- 仅依赖系统 DLL（ADVAPI32、COMCTL32、GDI32、IMM32、KERNEL32、SHELL32、USER32、comdlg32、msvcrt、ole32），无需 MinGW 运行库。
- `putty.exe` 启动后窗口标题为「PuTTY 配置」（英文版为 "PuTTY Configuration"）。
- **哈希表的正确用法**：PE 头嵌入链接时间戳，同一份源码两次构建的产物哈希必然不同（实测已验证），因此哈希只用于**校验下载完整性**，不能用于判断「是不是最新版」；判断版本请用 exe 文件属性里的版本信息。
- **二进制不入库**：`dist/` 已加入 `.gitignore`。入库等于每次重建都多存一份无法复用的对象，故仓库只保留源码与上表基准值，发布产物统一走 Releases。

## 串口默认配置

本构建为串口连接预置了华为、华三网络设备 Console 口的标准参数（点选「串口」后生效）：

| 参数 | 默认值 |
|------|--------|
| 串口线 | 自动匹配本机真实端口（真实串口优先，蓝牙虚拟口后排） |
| 波特率 | 9600 |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验 | 无 (None) |
| 流控 | 无 (None) |

打开程序默认是 **SSH** 连接（主机名留空、端口 22）。需要串口时点选「串口」单选按钮，连接参数即按上表预置。「串行口(N)」是从注册表 `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM` 枚举出的本机端口下拉框：

- 当前配置的端口不存在时（例如从别的机器带过来的 `COM1`），会自动回退到列表第一项，不会显示一个打不开的端口；
- 下拉框为只读，只能从列表选择，避免手输出错；
- 点击输入框任意位置即可展开列表，不必精确点右侧箭头。

需要 SSH 时点选「SSH」单选按钮，原「主机名称」输入框恢复为可编辑状态。

## 保存配置

PuTTY 不会自动保存设置。改完参数后需要点窗口右下角的「保存」按钮：

- 左侧会话列表未选中任何会话时，点「保存」会直接存入「默认设置」，下次启动沿用；
- 也可以先在左侧选中「默认设置」（或输入新会话名）再保存。

原版 PuTTY 在未选中会话时点「保存」只会「嘟」一声、不做任何提示；本构建改为直接保存到「默认设置」，避免设置改完却丢失。

## 会话目录分组

左侧会话列表支持按目录整理历史连接，行为对齐资源管理器：

| 操作 | 效果 |
|------|------|
| 「新目录」按钮 | 在列表末尾新建目录并就地命名，回车确认、Esc 保留默认名 |
| 双击目录行 | 进入该目录，列表首行显示「..」，其下只列该目录的会话 |
| 双击「..」 | 返回根层，光标停回刚才的目录行 |
| 右键目录行 | 进入目录 / 重命名目录 / 删除目录 |
| 右键会话行 | 重命名 / 移入目录（子菜单）/ 移出目录（仅在目录内出现） |

- 根层只显示目录行与**未归类**的会话，已归类的会话不再重复出现在根层；目录行以「▸ 名字 (n)」显示条目数，空目录不带括号。
- 目录行与会话行都支持**条目内就地改名**：编辑框直接浮在该行上，与列表同字体同底色，右边界止于端口列竖线之前，不遮挡行分隔线。回车提交、Esc 取消；目录名清空即删除该目录。
- 会话改名只改**显示名**，真实主机名与端口不变，取消显示名后恢复显示主机名。
- 在某个目录内发起连接，新记录会自动归入该目录，不必事后手动移动。
- 删除目录不会删除其中的会话，成员退回未归类状态。

目录名与归属关系保存在注册表 `HKCU\Software\SimonTatham\PuTTY\RecentFolders`，会话的目录归属与显示名分别记在 `RecentHosts` 对应条目的 `folder`、`label` 值下；旧版本写入的记录缺少这两个字段时按空值处理，可直接兼容。

## 会话日志

会话窗口新增顶级「日志」菜单，可随时开始或停止记录当前会话：

- **开始记录日志**：把本次会话的终端输出写入文件，路径见「会话 → 日志」设置页；
- **停止记录日志**：关闭当前日志文件；
- 操作生效后窗口顶部弹出轻提示确认，未实际生效时不弹（避免假反馈）。

日志文件按 UTF-8 编码写入，**中文输出可正常记录**。原版实现在写入前只放行 ASCII，中文码点会被整段丢弃、日志里只剩空格，本构建已修复。

> 运行中开日志只在本次连接内生效，不会污染已保存的默认设置。

## 重启会话

「会话」菜单新增**重启会话**。会话断开后该菜单项可用，点一下即按当前配置重连，无需关闭窗口重开。

## 功能验证

在隔离的本机回环环境（Node.js ssh2 测试服务器，监听 127.0.0.1:2222，不访问外部网络）实测：

| 编号 | 测试项 | 结果 |
|------|--------|------|
| T1 | 密码认证 + 远程命令执行 + 中文回显 | PASS |
| T2 | 公钥认证（ed25519，PPK v3）+ 命令执行 | PASS |
| T3 | 本地端口转发 `-L 18080:127.0.0.1:18081` | PASS |
| T4 | 交互式终端会话 | PASS |
| T5 | ~~Pageant 加载 ed25519 PPK + plink 无密码公钥认证~~ | 不适用（相关程序已移除构建） |

界面汉化与串口改造未影响协议层行为。测试记录见 `workspace/docs/2026-10-17-PuTTY功能测试.md`，串口改造见 `workspace/docs/2026-10-18-串口默认值自动匹配.md`。

### 单元测试与 CI

本仓库自带 GitHub Actions CI（`.github/workflows/ci.yml`），在 `windows-latest` 上用 MSYS2 + MinGW-w64 编译并运行 `ctest`。当前状态：

- `ctest` **9/9 全部通过**；
- 文件传输窗口自检 207 项全通过、0 失败。

此前 `test_cert_expr` 与 `test_terminal` 长期红灯，原因分别是「断言写死了英文报错、而本分支已汉化」与「用例假设远端可改标题、而本分支默认禁用」。两处均已修正，改动只涉及测试代码，不影响 `putty.exe` 行为。

## 已知限制

1. **GUI 版不支持 `-batch`**。该选项属于 plink，传给 `putty.exe` 会弹「命令行错误」。
2. **`putty host command` 写法有陷阱**。`putty 127.0.0.1 "echo hello"` 会把 `"echo hello"` 当作端口号（atoi 得 0），报「无法分配请求的地址」。远程命令请用 `-m <命令文件>` 传入。
3. **只认 PuTTY 原生 `.ppk` 私钥**。OpenSSH 新格式与旧 PEM 格式均无法加载；可用 `workspace/tests/gen-ppk.js` 生成 ed25519 PPK v3，或用官方 PuTTYgen 导入转换后指向该文件。
4. **串口下拉框只读**，只能从本机枚举到的端口中选择，不能手输 `\\.\COM10` 这类设备路径；本机没有任何串口时列表为空。Unix 版不受影响。
5. **只提供 `putty.exe`**。`puttygen`、`plink`、`pscp`、`psftp`、`pageant`、`puttytel` 已从构建与安装包中移除。如需这些程序，可用官方 PuTTY 0.85 对应版本，或按上方构建说明把 CMake 里的对应目标加回来自行编译（源码仍在树上）。

## 自行构建

```bash
# MSYS2（MinGW-w64 环境）中
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make perl
cd src
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --target putty -j
```

产物为 `src/build/` 下的同名 exe。发布时复制到 `dist/` 便于本地取用，但 `dist/*.exe`
已在 `.gitignore` 中，二进制不随源码提交；对外分发走 GitHub Releases 上传安装包。
本项目的构建产物由上述流程产出（gcc 16.2.0 / cmake 4.4.4 / ninja）。

## 复现测试

```bash
cd tests
npm install
node server-main.js          # 终端 A：启动测试服务器
powershell -File test-putty.ps1   # 终端 B：运行四项测试
```

测试脚本仅用于本机回环验证，不会连接外部主机。脚本以 `$PSScriptRoot` 定位自身目录，可随仓库移动。

## 文档索引

- 汉化过程：`workspace/docs/2026-10-17-PuTTY汉化.md`
- 翻译质量修复：`workspace/reports/2026-10-18-翻译质量修复与回归报告.md`
- 串口改造：`workspace/docs/2026-10-18-串口默认值自动匹配.md`、`workspace/docs/2026-10-18-串口下拉框.md`
- 会话页与配置页：`workspace/docs/2026-10-18-会话页历史记录改版.md`、`workspace/docs/2026-10-18-配置窗口标题样式.md`
- 命令行工具汉化：`workspace/reports/2026-10-18-命令行工具汉化完成报告.md`
- 风险扫描与整改：`workspace/reports/风险扫描-putty-cn-20261018.md`、`workspace/reports/2026-10-18-M3-M4-M6整改与完整审计.md`

## 许可证

PuTTY 及其衍生代码遵循 MIT 许可证，见 [`LICENCE`](LICENCE)。中文翻译版权归 larryli/PuTTY 项目贡献者所有。
