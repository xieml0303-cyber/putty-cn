# GitHub 源码一致性与 0.85-cn2 Release 更新（2026-10-22）

## 一、源码一致性核对

结论：本地与 GitHub 完全一致，无任何差异。

- `git fetch origin` 后 `HEAD` 与 `origin/main` 同为 `aea36f7a275f5491c1414a4a1285ff89b263b4d1`
- `git diff --stat HEAD origin/main` 无输出
- 工作区干净，无未跟踪文件

核对前的状态是上一轮推送完成后的结果（本地 6 个滞留在本地的提交已一并推送）。版本号定义在 `src/version.h:11`：`TEXTVER "发布版 0.85-cn2"`。

## 二、Release 更新的必要性

核对发现：GitHub 上 `v0.85-cn2` Release 里的安装包是 **2026-10-18 打的旧包**，与远端记录逐字节相同（SHA256 `b4af2028…`），说明 10-18 之后没有任何重新打包动作。而源码在 10-20 至 10-22 期间做了窗口位置/尺寸修正与终端实时重排改造。

也就是说：**Release 页面挂的安装包不含这两天的改动**。若直接回答「已是最新」会误导使用者。

## 三、处理方法

1. 确认 Inno Setup 编译器：原报告记录的 `C:\Users\Wrok\Desktop\NexusOPS\.build\InnoSetup\ISCC.exe` 已随目录消失；全盘搜索定位到现用副本 `C:\Users\Wrok\AppData\Local\Programs\Python\Python313\InnoSetup\ISCC.exe`（Inno Setup 7.1.0，winget 安装记录为 `JRSoftware.InnoSetup.7`）。
2. 重新编译 `installer/putty-cn.iss`，七个程序取自 `dist/` 当前二进制（`putty.exe` 为 2026-10-22 构建，与 `src/build`、`C:\Program Files\PuTTY` 三处 MD5 一致）。
3. `gh release upload v0.85-cn2 --clobber` 替换资产。
4. 更新 Release 说明，写入本次重建说明与新校验值。
5. 新包提交入库，保持仓库产物与线上资产同源。

新安装包：`putty-cn-0.85-cn2-setup.exe`，3,191,643 字节，SHA256 `58cba505fcddbd4aa0c200c86293a414a72279ff6a67c4ba84bc4aa0a9b455fc`。

## 四、验证

- 从 Release 重新下载资产，与本地文件 SHA256 逐字节一致（防上传截断）
- `gh release view` 记录的大小/摘要与本地一致，更新时间 2026-10-22T05:44:12Z
- 最终 `HEAD` = `origin/main` = `3c721ab96229b25ccb58750b3cab39e05ce3f856`

## 五、推送受阻与处置

推送安装包时 `git push` 报 `Connection was reset` / `Could not connect to github.com:443`。排查结果：

| 目标 | 结果 |
|---|---|
| `api.github.com` | 200 正常 |
| `github.com`（DNS 20.205.243.166） | 超时 |
| 备用 IP 140.82.112.3 / 113.3 / 20.27.177.113 / 20.200.245.247 | 全部超时 |
| `ssh.github.com:443` | 超时 |
| 经 `127.0.0.1:7897`（Clash Verge 混合端口） | 200 正常 |

因此 `gh`（走 api.github.com）上传成功，而 `git push`（走 github.com）被阻断。处置方式是单次命令携带代理参数推送：

```
git -c http.proxy=http://127.0.0.1:7897 -c https.proxy=http://127.0.0.1:7897 push origin main
```

未写入 git 全局配置，避免代理未启动时所有 git 操作连带失败。上一轮推送能成功，说明阻断是间歇性的，不是配置问题。

## 六、遗留：tag 未随资产移动

tag `v0.85-cn2` 仍指向 `75e1e6c`（10-18 打包安装包的提交），比 Release 内的新资产落后 7 个提交。Release 说明中已注明安装包构建自 `aea36f7`，但严格意义上「tag 指向的源码」与「资产里的程序」不是同一次构建。

三种处理方式，需项目所有者定夺：

1. 保持现状，视 0.85-cn2 为该版本号下的一次重新构建（资产更新，tag 不动）
2. 强制移动 tag 到当前提交（`git tag -f` + `push --force`），使 tag 与资产同源，代价是移动已发布 tag
3. 打新版本号（如 0.85-cn3）另建 Release，让旧 Release 保持原样
