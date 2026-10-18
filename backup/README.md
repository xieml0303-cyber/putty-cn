# backup/ 说明

本目录保留改造前后的配置快照，用于回滚与比对。**这些 `.reg` 文件仅为本机默认设置的导出快照，不含任何凭据。**

| 文件 | 内容 | 是否含凭据 |
|------|------|-----------|
| `putty-default-settings.reg` | 改造前（英文原版）的 PuTTY 默认设置 | 否（`HostName`、`UserName`、`PublicKeyFile`、`ProxyPassword` 均为空） |
| `putty-default-settings-cn.reg` | 中文版首次改造后的默认设置 | 否（同上） |
| `reg-session-127.0.0.1.reg` | 本机回环测试会话（`127.0.0.1`） | 否（无密码、无私钥路径） |

二进制备份（`putty.exe.orig-0.85-en`、`putty.exe.cn-0.85-prev.bak`）仅保存在本地工作区，已通过 `.gitignore` 排除在版本库之外；需要时可用 `src/` 源码按 README 的「自行构建」步骤重新生成。
