# PuTTY 0.85 中文版（Windows 64 位）

基于 [larryli/PuTTY](https://github.com/larryli/PuTTY)（原 Google Code `puttycn` 项目）的中文翻译源码，使用 MSYS2 + MinGW-w64 + CMake 在本地编译生成的 `putty.exe`。

> 这不是 PuTTY 官方构建，与 Simon Tatham 及 PuTTY 官方项目无关。官方 PuTTY 从未提供中文界面。

## 下载

直接使用仓库中的 [`dist/putty.exe`](dist/putty.exe)（1,836,539 字节，64 位，免安装）。

- 仅依赖系统 DLL（ADVAPI32、GDI32、IMM32、KERNEL32、SHELL32、USER32、comdlg32、msvcrt、ole32），无需 MinGW 运行库。
- 启动后窗口标题为「PuTTY 配置」（英文版为 "PuTTY Configuration"）。
- 建议先校验 SHA256 再运行。

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
git clone https://github.com/larryli/PuTTY.git
cd PuTTY
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --target putty -j
```

本仓库中的二进制即由上述流程产出（gcc 16.2.0 / cmake 4.4.4 / ninja，288 个目标全部成功）。

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
