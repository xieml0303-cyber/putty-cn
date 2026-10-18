# 中文版 PuTTY 上传 GitHub 记录

日期：2026-10-17

## 结果

- 仓库：https://github.com/xieml0303-cyber/putty-cn
- 可见性：**public**（创建时默认选了公开，MIT 开源衍生品，如需私有可在 Settings 改）
- 默认分支：main
- 提交：`151a10e`「PuTTY 0.85 中文版构建：中文界面 putty.exe + 功能测试与复现脚本」
- 本地副本：`C:\Users\Wrok\Repos\putty-cn`

## 仓库内容

```
README.md                          使用说明、已知限制、构建与测试方法
LICENCE                            MIT（来自 larryli/PuTTY 源码）
.gitignore                         排除密钥/日志/node_modules
dist/putty.exe                     1,836,539 字节，中文版 64 位
docs/2026-10-17-PuTTY功能测试.md    四项测试报告
docs/2026-10-17-PuTTY汉化.md        汉化路线与编译记录
tests/server-main.js               测试 SSH 服务器入口
tests/server-lib.js                测试 SSH 服务器实现
tests/gen-ppk.js                   生成 ed25519 PPK v3
tests/test-putty.ps1               四项测试驱动
tests/quick-test.ps1
tests/read-error.ps1
tests/package.json
```

## 未上传（有意排除）

- 全部私钥：`id_rsa`、`id_ed25519`、`id_ed25519.ppk`、`host_ed25519.pem`
- `node_modules/`、会话日志、`authorized_keys`、`fingerprint.txt`
- larryli 完整源码（体积大，上游已有）

## 认证与网络

- gh CLI 未安装，无 GH_TOKEN；使用 Windows 凭据管理器中的 `git:https://github.com`（用户 xieml0303-cyber）通过 GitHub API 创建仓库。
- github.com 直连不稳定（多次 Connection reset / 14 秒超时），push 走系统代理 `127.0.0.1:7877` 成功。代理仅配置在 `C:\Users\Wrok\Repos\putty-cn\.git\config`，未改全局配置。
- 首次 push 在后台任务中返回 exit 0 但实际未成功（远程 SIZE_KB=0），已发现并重推；最终以 API 查询 contents 确认文件存在。

## 验证

- API `GET /repos/.../commits`：1 个提交，SHA 与本地一致。
- API `GET /contents/dist`：`putty.exe` 1836539 字节，SHA `1de555b6`。
- 根目录文件：`.gitignore`、`LICENCE`、`README.md`、`dist/`、`docs/`、`tests/`。

## 说明

push 前安全扫描未返回完整结论（scanner_no_output），本次按兼容策略放行；仓库内容为已公开的 MIT 源码衍生二进制与测试脚本，无密钥。未对仓库安全性作保证，如需可另行完整审计。
