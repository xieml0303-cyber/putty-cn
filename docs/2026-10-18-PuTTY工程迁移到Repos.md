# PuTTY 改造工程迁移到 C:\Users\Wrok\Repos

日期：2026-10-18
状态：已完成（本地提交，未推送）

## 一、迁移前盘点

### C:\Users\Wrok\Repos\putty-cn（已存在）

产物仓库，**不含源码**：

| 路径 | 内容 |
|------|------|
| `dist/putty.exe` | 1,836,489 字节（2026-10-18 01:00 构建，落后于串口改造） |
| `docs/` | 2 篇：汉化过程、功能测试 |
| `tests/` | Node.js ssh2 回环测试服务器 + 4 项 PowerShell 测试 |
| `README.md` / `LICENCE` / `.gitignore` | 项目说明、MIT 许可、构建产物忽略规则 |
| git | 3 次提交，远程 `https://github.com/xieml0303-cyber/putty-cn.git` |

### 源码实际位置（迁移前）

`C:\Users\Wrok\.zcode\workspace\default\.workbuddy\putty-cn-src\PuTTY-master`

- 43M（含 build），不含 build 20M / 1267 个文件
- 上游 larryli/PuTTY master 的 zip 解压副本，**无 .git**
- 被改造过的文件 10 个：`config.c`、`conf.h`、`dialog.h`、`storage.h`、`be_list.c`、`windows/{config,controls,storage}.c`、`unix/{dialog,storage}.c`
- 风险：位于工作区临时目录，不在版本控制内，长期工程随时可能丢失

## 二、迁移动作

1. 复制源码到 `C:\Users\Wrok\Repos\putty-cn\src\`，排除 `build/` 与 `.mimosa/`（Mimosa 扫描器会话状态，9.2M / 574 文件，首次提交误带入后已剔除并加入 .gitignore）
2. 搬迁 7 篇历史报告到 `docs/`（0.85 下载、仓库上传、按钮调整、串口与保存优化、串口下拉框、列表高度与滚动条、会话页历史记录改版），现共 9 篇
3. README 新增「仓库结构」与「相对上游的改动」两节，构建命令改为使用本仓库 `src/`
4. `dist/putty.exe` 更新为最新构建（1,840,077 字节），README 字节数同步
5. 提交：`3ad8e5b` 导入完整源码（702 文件 / 266,882 行，基线 larryli/PuTTY `7ddf3ae` 0.85）、`18f9eab` 更新产物

## 三、验证

- 新位置 CMake 配置成功（17.9s，ConPTY 不可用属预期，不影响 putty.exe）
- Ninja 构建 288/288 目标全部成功，产出 `src/build/putty.exe` 1,840,077 字节
- 与已部署版本 `C:\Users\Wrok\AppData\Local\Programs\PuTTY\putty.exe` 字节数一致；SHA256 不同仅因 PE 头编译时间戳
- 工作区 `git status` 干净

## 四、待定夺

1. **是否推送 GitHub**：源码 20M，仓库体积会明显增大；本地两个提交尚未 push
2. **旧源码目录处置**：`.workbuddy\putty-cn-src\PuTTY-master` 暂保留作备份，确认新位置构建无误后可删
3. `docs/` 中历史报告含本地回环地址与代理地址（127.0.0.1:7877），无凭据，推送前可复核

## 五、后续约定

- 今后改代码一律在 `C:\Users\Wrok\Repos\putty-cn\src` 进行，构建产物在 `src/build/`，发布时复制到 `dist/`
- 每轮改动提交一次，提交信息写清改了什么；推送前跑 Mimosa 完整扫描
