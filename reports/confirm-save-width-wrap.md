# 「确认保存」弹窗宽度与折行改造报告

日期：2026-11-08
文件：`src/windows/sftpwin.c`（另含 `src/windows/putty.c` 自检入口）
构建：`C:/msys64/mingw64/bin/ninja.exe putty`，零警告零错误，产物 `putty.exe` 2,685,495 字节。

## 一、本轮需求

主人共提出两批修订。

第一批（窗口宽度）：

1. 窗口默认宽度 300px，最宽 450px；
2. 没到 450px 之前宽度应跟着内容延伸，不许提前折行；
3. 弹窗位置固定为屏幕正中央；
4. Tooltip 改到右边，其上框线与内容区上框线齐平。

第二批（断行策略）：

5. 内容过长触发换行时，要让「【标题】」与改动内容分开，只把改动内容换到下一行。

此前「内容区与右边框精准 50px」的要求作废。

## 二、根因：窗口宽度被逐行测量放大

### 2.1 现象

窗口宽度始终顶在上限，右侧留白巨大，与内容长度无关。前几轮反复调右距（50px、12px）均无效。

### 2.2 定位过程

加 `CN_CONFIRM_SAVE_DEBUG` 日志打印每行测量值，得到铁证：

- 最长行实际文字宽 246px；
- 各行测量值 205 / 246 / 190 / 21 px 均正常；
- 但 `needw` 汇总为 716，窗口因此永远触顶。

进一步打印字节数与字符数发现：第一行 49 字节，经 `dup_mb_to_wc` 转换后变成 83 个宽字符，量出宽度 662px。

### 2.3 根因

测量代码按 `\n` 切行，但**只把行首指针传给 `dup_mb_to_wc`，没有限定长度**：

```c
char *nl = strchr(p, '\n');
int blen = nl ? (int)(nl - p) : (int)strlen(p);
wchar_t *wl = dup_mb_to_wc(DEFAULT_CODEPAGE, p);   /* ← p 指向行首，未截断 */
```

`dup_mb_to_wc` 内部按 `strlen` 处理，于是每行实际转换的是「从该行开头到整个字符串结尾」的全部内容。第一行按剩余全文（83 字符）去量，宽度自然虚高。行数越多、越靠前的行，放大越严重。

### 2.4 修复

改用带长度参数的 `dup_mb_to_wc_c`，并先剔除行尾的 `\r`：

```c
while (blen > 0 && (p[blen-1] == '\r' || p[blen-1] == '\n'))
    blen--;
size_t wlen = 0;
wchar_t *wl = dup_mb_to_wc_c(DEFAULT_CODEPAGE, p, blen, &wlen);
GetTextExtentPoint32W(hdc, wl, (int)wlen, &s2);
```

注意 `dup_mb_to_wc_c` 第四参数类型为 `size_t *`（见 `src/misc.h:76`），不能用 `int *`。

## 三、宽度策略改造

### 3.1 旧公式

```
w = m + needw + cn_dpi(50);           // 内容宽 + 固定右距
if (w < btnneed) w = btnneed;          // 按钮兜底
if (w > cn_dpi(720)) w = cn_dpi(720);  // 上限
```

内容多宽窗口就多宽，靠 720 封顶，导致长条目一律撑满、右侧大片空白。

### 3.2 新公式

```c
int need_head = 2 * m + needw_head;            /* 首行顶格：左右各一个 m */
int need_list = lx + listw_text + m;           /* 清单：图标偏移 + 文字 + 右 m */
needw = need_head > need_list ? need_head : need_list;
w = needw + cn_dpi(12);
if (w < cn_dpi(300)) w = cn_dpi(300);   /* 默认宽度 */
if (w > cn_dpi(450)) w = cn_dpi(450);   /* 最宽封顶，超出靠断行 */
cw = w;                                  /* 客户区宽定死，供断行测量 */
```

宽度成为**区间约束**而非内容函数：窄内容用 300 下限托住（也保证两个 96px 按钮放得下），宽内容封顶 450 交由断行消化。

### 3.3 少算一个边距导致提前折行

第一版新公式写成单个 `needw` 统一加一个 `m`，清单那侧少算了一个左边距：

```
清单控件宽 = cw - 2m - 图标 - 6
放下 246px 文字需要 cw >= 328，而公式只给到 326
```

差 2px 就触发了折行，窗口才 326px、离 450 上限还远。主人指出「这个宽度才 330px，怎么就自动换行了」。修正为两条需求各自带全边距、取大者，实测宽度从 326 变为 340，246px 文字放得下，不再折行。

## 四、断行改造

### 4.1 控件样式

清单 STATIC 由 `SS_LEFTNOWORDWRAP` 改为 `SS_LEFT`：

```c
cb->list = CreateWindowExW(0, L"STATIC", wlist,
                          WS_CHILD | WS_VISIBLE | SS_LEFT,   /* 原 SS_LEFTNOWORDWRAP */
                          lx, ytop, cw - lx - m, list_h, ...);
```

`SS_LEFTNOWORDWRAP` 会直接裁掉超出部分，不折行。

### 4.2 高度计算顺序修正

原代码先按 `\n` 数行算 `list_h`，再算窗口宽度。断行行数取决于可用宽度，顺序必须反过来：先定宽，再用 `DT_CALCRECT` 实测。

```c
int lx = m + qicon_size + cn_dpi(6);
int availw = cw - lx - m;                    /* cw 已定死 */
RECT rc = { 0, 0, availw, 0 };
DrawTextW(hdc, wmeas, -1, &rc,
          DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
list_h = rc.bottom - rc.top;
```

`DT_CALCRECT | DT_WORDBREAK` 使用与 STATIC 相同的断行规则，中英混排不会算错。

### 4.3 按「】」断行（`cn_wrap_at_bracket`）

仅靠 STATIC 自动折行会把「开 → 关」拦腰切成「开 →」和「关」。改为在文本层预先断开：

```c
static const char *cn_find_bracket_close(const char *s, size_t len);
static char *cn_wrap_at_bracket(const char *listtxt, int maxw, HDC hdc, HFONT font);
```

规则：

1. 量整行宽度，**放得下就一个字不动**；
2. 放不下才断，断点只在「】」（U+3011，UTF-8 为 `E3 80 91`）之后；
3. 断后跳过「】」后的空格，避免下一行以空格开头；
4. 找不到「】」的行原样保留，交给 STATIC 兜底。

只认 U+3011 一个字符，不把其他全角括号算进来，否则遇到「（备注）」这类文本会误断。

### 4.4 顺序：先定宽，再断行

断行阈值必须是「窗口最终可用宽度」，而窗口宽度又按整行宽度算出，存在循环依赖。解法：

1. 按整行宽定窗口宽（内容宽就撑宽，上限 450）；
2. 拿定下来的可用宽去断行——放得下就不动；
3. 断行后重算 `listw_text`，宽度只会变窄，不会反过来撑破窗口。

实测超长用例 `listw_text` 从 652 降为 604，清单高度 40px 增至 57px。

### 4.5 已知边界

若「标题」本身超过可用宽度（如 604px > 368px），STATIC 仍会在标题内部折行，此时「】」会落到第二行。这属于标题自身过长，断行策略无法避免——除非再引入标题内的二级断点，当前未做。

## 五、自检改造

自检入口为 `-confirm-save`（**不是** `-confirm-save-test`），实现于 `sftp_selftest_confirm_save()`，结果写 `confirm-save-selftest.txt`。

### 5.1 旧断言作废

原断言「内容右缘到右边框约 50px」按新规格已不成立——右距现在是宽度的结果而非约束。改为：

```c
SW_CHECK(cr.right >= wlo && cr.right <= whi, "窗口宽度在 300~450px 区间内");
SW_CHECK(gap >= 0, "内容右缘未被右边框裁掉");
```

### 5.2 新增边界用例

原自检只跑一份默认数据，无法证明下限与上限真的生效。新增两组：

- `changes_short`：单条极短改动，验证 300px 下限；
- `changes_long`：超长中文条目，验证 450px 封顶与折行。

断言宽度区间、清单区不越界、超长内容折行后高度多于一行。

编译期踩坑：`static const struct` 数组成员不能用局部变量初始化（`initializer element is not constant`），去掉 `static` 即可。

## 六、实测结果

`putty.exe -confirm-save` 输出（`confirm-save-selftest.txt`）：

```
[确认保存弹窗自检]
  客户区 340x163，清单区 (68,37)-(326,111)
  清单文本宽实测：205 246 190 21 px
  客户区宽 340px（目标区间 300~450px），内容右距 14px
  通过  窗口宽度在 300~450px 区间内
  通过  内容右缘未被右边框裁掉
  通过  清单在图标右侧、首行顶格
  弹窗中心 (960,520)，工作区中心 (960,520)，偏差 0/0 px
  通过  弹窗居中于屏幕
  按钮间距 8px，左右余量 70/70
  通过  两按钮间距为标准 8px
  通过  按钮组水平居中
  通过  两按钮同顶
  通过  两按钮同高
  通过  改动超三行时挂上了 tooltip
  通过  tooltip 文本包含全部改动（含未显示的两行）
  浮层 (1138,464)-(1400,561)，弹窗右缘 1133，首行顶 464，右侧有空间
  通过  浮层在弹窗右侧
  浮层顶边与内容区顶边差 0px
  通过  浮层上框线与内容区上框线齐平
  浮层底色 0x231B17 / 0x231B17（目标 0x171B23）
  通过  浮层底色为 #171B23
  通过  浮层文字为白色（黑底上找到白点）
  通过  销毁弹窗不向消息队列投递 WM_QUIT

[极短内容（下限 300px）]
  客户区 300x137，清单区高 23px
  通过  宽度落在 300~450px
  通过  清单区未超出客户区

[超长内容（上限 450px + 折行）]
  客户区 450x146，清单区高 57px
  通过  宽度落在 300~450px
  通过  清单区未超出客户区
  折行后清单高 57px
  通过  超长内容已折行（高度多于一行）
  断点正确：「】」结尾行 + 下一行含「→」
  通过  超长行在「】」之后断开，未拦腰切断

小计：通过 21 项，失败 0 项
完成
```

三组数据分别落在 340 / 300 / 450px，说明区间约束与断行均按预期生效。默认样例清单高 74px（三行，未断行）；超长条目 57px，断行后最长行宽 652→604px。

浮层底色读出 `0x231B17`，是 `COLORREF` 的 BGR 排列（红蓝对调），实际即 `#171B23`。

## 七、居中与浮层摆位

### 7.1 弹窗居中

新增 `cn_place_center`，按工作区居中（避开任务栏，否则视觉偏下）：

```c
static void cn_place_center(int w, int h, int *outx, int *outy)
{
    RECT wa;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        sw = wa.right - wa.left; sh = wa.bottom - wa.top;
        ox = wa.left; oy = wa.top;
    } else { /* 退到整屏 */ }
    *outx = ox + (sw - w) / 2;
    *outy = oy + (sh - h) / 2;
}
```

未复用 `cn_place_near`——那个是「居鼠标」语义，确认覆盖等弹窗仍在使用。

实测偏差 0/0 px（工作区 1920x1080，弹窗中心 960,520）。

### 7.2 浮层摆位模式

`CnTipState` 新增 `mode` 与 `trigger_top` 两个字段：

- `CN_TIP_BELOW`：摆在触发区下方，放不下翻上方（确认覆盖沿用，值 0 与 `memset` 默认一致）；
- `CN_TIP_RIGHT`：摆在弹窗右侧，顶边与内容区上边齐平（确认保存使用）。

横向锚点取**客户区右缘**而非清单文字右缘。早先误用 `st->hot.right`，实测浮层左缘 1112 压在弹窗右缘 1133 上；改用客户区右缘后为 1138，正常落在窗口外侧。

竖直锚点取 `trigger_top`（= 首行 STATIC 的 y，即 `m`），不是 `hot.top`——清单在问号图标下方起算，用 `hot.top` 会比内容区上框线低一截。实测顶边差 0px。

右侧空间不足时翻到左侧，自检对此单独判定（`right 不够已翻左`），不会误报。

## 八、新增可视化入口

`-confirm-save-show`：把弹窗真显示出来，供人工看图/截图。自检模式建完即销毁，截不到图。

- 默认用五行改动（触发折叠，浮层会挂上）；
- 设 `CN_CONFIRM_SAVE_LONG=1` 换成超长条目，用于目视确认断点位置。

实测超长用例渲染：标题独占一行（过长时 STATIC 仍会在标题内折行），「开 → 关」完整落在下一行，未被拦腰切断。

## 九、构建环境问题（与代码无关）

编译一度报 `FAILED: [code=1]` 且 stderr 完全为空，误导排查方向。实际原因：

`cc1.exe` 加载 `libmpfr-6.dll` 失败。该 DLL 存在于 `C:/msys64/mingw64/bin/`，但该目录不在 PATH 中，`cc1.exe` 找不到依赖，进程立即退出且错误信息被 ninja 吞掉。

单独执行 `cc1.exe --version` 可复现真实报错：

```
cc1.exe: error while loading shared libraries: libmpfr-6.dll:
cannot open shared object file: No such file or directory
```

解决：构建前把 `C:/msys64/mingw64/bin` 加入 PATH。

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd src/build && ninja putty
```

另：链接时 `putty.exe` 曾被占用（`Permission denied`），占用者是遗留的 putty 进程，结束进程后链接成功。

## 十、遗留

所有改动尚未提交。清单折叠上限仍为三行（`CN_CONFIRM_SAVE_MAX_LINES`），断行后行数增加会使窗口变高，未设高度上限。

标题自身超过可用宽度时（如 604px > 368px），STATIC 仍会在标题内部折行，「】」可能落到第二行。当前未引入标题内的二级断点。

## 七、构建环境问题（与代码无关）

编译一度报 `FAILED: [code=1]` 且 stderr 完全为空，误导排查方向。实际原因：

`cc1.exe` 加载 `libmpfr-6.dll` 失败。该 DLL 存在于 `C:/msys64/mingw64/bin/`，但该目录不在 PATH 中，`cc1.exe` 找不到依赖，进程立即退出且错误信息被 ninja 吞掉。

单独执行 `cc1.exe --version` 可复现真实报错：

```
cc1.exe: error while loading shared libraries: libmpfr-6.dll:
cannot open shared object file: No such file or directory
```

解决：构建前把 `C:/msys64/mingw64/bin` 加入 PATH。

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd src/build && ninja putty
```

---

# 追加：连接 / 文件传输前的改动生效确认

## 一、需求

点击「连接」或「文件传输」时先检查配置改动清单：

- 清单为空：不弹窗，直接按原流程执行；
- 清单不为空：弹出确认弹窗，复用「确认保存」弹窗的样式与布局，按钮文案改为
  「仅本次生效」（左）与「长期生效」（右）。

三种操作的结果：

| 操作 | 处理 | 是否继续连接 |
| --- | --- | --- |
| 仅本次生效 | 改动只对本次生效，不落盘 | 是 |
| 长期生效 | 改动写入注册表 | 是 |
| 右上角 ❌ | 只关闭弹窗，不做任何处理 | 否 |

后续优化要求：

1. 首行文案改为「当前改动尚未保存，改动清单：」；
2. 标题改为「确保保存方式后继续执行」；
3. ❌ 改为仅关闭弹窗，不继续执行连接或文件传输；
4. 「仅本次生效」按钮为普通框线，「长期生效」为蓝色默认按钮框线。

## 二、实现

### 2.1 弹窗文案参数化

`cn_confirm_save_build` 原先把标题、首行、按钮文案写死在函数体内。新增
`CnConfirmTexts` 结构体（定义在 `putty.h`，`config.c` 与 `sftpwin.c` 共用）：

```c
typedef struct CnConfirmTexts {
    const char *title;
    const char *head;
    const char *ok;
    const char *cancel;
    bool default_is_cancel;   /* 默认按钮（蓝框）是否在右边 */
} CnConfirmTexts;
```

`cn_confirm_save_build` 增加 `const CnConfirmTexts *texts` 参数，传 `NULL`
时沿用「确认保存」原有文案，因此保存弹窗行为完全不变。新增
`cn_confirm_dialog_texts()` 作为带文案的对外入口，
`cn_confirm_save_dialog()` 保留为传 `NULL` 的薄包装。

### 2.2 区分 ❌ 与「长期生效」

两者都返回 `IDCANCEL`，无法从返回值区分。在 `ConfirmSaveBox` 中新增
`closed_by_x` 标志，`cn_confirm_save_proc` 的 `WM_CLOSE` 分支置位；
`cn_confirm_dialog_texts()` 通过可选输出参数 `bool *closed_by_x` 回传。

### 2.3 默认按钮位置

`default_is_cancel` 为真时，左按钮用 `BS_PUSHBUTTON`、右按钮用
`BS_DEFPUSHBUTTON`（蓝色默认框）；回车键的落点也跟随该标志，避免
「默认按钮在右边、回车却执行左边」的不一致。

### 2.4 三态返回

`dlg_confirm_launch()` 返回 `CnLaunchChoice`：

```c
typedef enum {
    CN_LAUNCH_GO,        /* 无改动，直接继续 */
    CN_LAUNCH_ONCE,      /* 仅本次生效 */
    CN_LAUNCH_PERSIST,   /* 长期生效 */
    CN_LAUNCH_CANCEL,    /* 点 ❌，连操作一起取消 */
} CnLaunchChoice;
```

`config.c` 中「连接」与「文件传输」两处调用点，均在真正启动前插入该确认；
`CN_LAUNCH_PERSIST` 调用 `cn_persist_default_settings()` 落盘并
`dlg_mark_clean()`，`CN_LAUNCH_CANCEL` 直接 `return`，其余继续原流程。

## 三、验证

### 3.1 自检

新增 `-confirm-launch`（`sftp_selftest_confirm_launch`），共 14 项，全部通过：

```
[连接/文件传输前改动确认自检]
  通过  清单为空时直接放行（CN_LAUNCH_GO，不弹窗）
  通过  有改动时进入待确认状态
  通过  左按钮文案为「仅本次生效」
  通过  右按钮文案为「长期生效」
  通过  窗口标题为「确保保存方式后继续执行」
  通过  首行文案为「当前改动尚未保存，改动清单：」
  「长期生效」文字宽 48px，按钮宽 96px
  通过  按钮宽度放得下「长期生效」（未裁字）
  窗口 346x166，客户区 340x137
  通过  窗口宽度仍在 300～450px 区间
  左留白 73px，右留白 73px
  通过  两个按钮成组居中（左右留白基本对称）
  左按钮 style=0x50010000，右按钮 style=0x50010001
  通过  「仅本次生效」是普通按钮（无蓝框）
  通过  「长期生效」是默认按钮（有蓝框）
  通过  新建弹窗时 closed_by_x 为假
  通过  点 ❌ 后 closed_by_x 置位（可判为取消）
  通过  点 ❌ 的返回值为 IDCANCEL
```

样式位 `0x50010001` 的最低位即 `BS_DEFPUSHBUTTON`，证明蓝框落在右按钮。

### 3.2 回归

| 测试 | 结果 |
| --- | --- |
| `-confirm-save` | 21 项通过，0 失败 |
| `-confirm-quit` | 6 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `test_conf.exe` | 退出码 0 |

### 3.3 目视确认

`-confirm-launch-show` 截图确认：标题「确保保存方式后继续执行」，首行
「当前改动尚未保存，改动清单：」，清单两行，左按钮「仅本次生效」为普通
框线，右按钮「长期生效」为蓝色默认框线，窗口 306×166。

## 四、已知取舍

- `default_is_cancel` 的语义是「默认按钮是右按钮」，命名从字段角度看略绕，
  但只在两处赋值，已加注释说明。
- `sftp_show_confirm_launch()` 内的文案是 `dlg_confirm_launch()` 中那份的
  副本，改一处须同步另一处，已在注释中标明。

---

# 追加：退出确认与标题栏 X 统一（三按钮）

## 一、问题

未保存改动时，两条退出路径给出两个完全不同的界面：

| 入口 | 实现 | 界面 |
| --- | --- | --- |
| 标题栏 ❌ | `IDD_SAVECHANGES` 资源模板 + `SaveChangesProc` | 三按钮、另一套字号、EDITTEXT 清单 |
| 底部「退出」按钮 | `dlg_confirm_quit` → `message_box` | 系统 MessageBox，确定/取消 |

主人截图对比后要求：删掉旧模板，两条路径统一到「确认保存」那套自绘弹窗，
标题改为「确认保存」，三个按钮的功能全部保留。

## 二、实现

### 2.1 自绘弹窗支持第三个按钮

`CnConfirmTexts` 新增 `third` 字段（**必须留在结构体末尾**，因为现有调用方
都用位置初始化，插到中间会让末尾的 `default_on_right` 静默错位）。

三按钮模式下：

| 位置 | 文案 | 样式 | 返回 |
| --- | --- | --- | --- |
| 左 | 直接退出 | `BS_OWNERDRAW`（红框红字自绘） | `IDOK` |
| 中 | 取消 | `BS_PUSHBUTTON` | `IDCANCEL` |
| 右 | 保存并退出 | `BS_DEFPUSHBUTTON`（蓝框默认） | `IDYES` |

`WM_DRAWITEM` 新增红框绘制，画法与旧模板一致（`FrameRect` + 2px 红笔 +
选中态浅红底）。回车落点改为优先第三按钮。

按钮宽度测量提前到「定窗口宽」阶段，`bw[3]` / `bw_n` 复用，避免同一套测量
写两遍算出两个答案；三按钮组总宽也参与窗口宽度下限计算。

### 2.2 返回值改为三态

`dlg_confirm_quit` 从 `bool` 改为 `CnQuitChoice`：

```c
typedef enum {
    CN_QUIT_DISCARD,   /* 直接退出：放弃改动关窗 */
    CN_QUIT_SAVE,      /* 保存并退出：落盘后关窗 */
    CN_QUIT_STAY,      /* 取消：留在配置框 */
} CnQuitChoice;
```

两个调用点（`config.c` 的退出按钮、`dialog.c` 的 `WM_CLOSE`）都改为 switch，
`CN_QUIT_SAVE` 分支先落盘、失败则留在框内并报错。

### 2.3 删除旧模板

删除内容：`IDD_SAVECHANGES` 资源模板、`SaveChangesProc`（142 行）、
`ask_save_changes`、`IDD_SAVECHANGES` / `IDC_SAVE_EXIT` / `IDC_DISCARD_EXIT` /
`IDC_CHANGE_LIST` / `IDC_CHANGE_NOTE` 五个宏（编号留空不复用，避免与旧资源
撞车）。源码中已无任何代码引用，仅注释保留历史说明。

### 2.4 文案收敛为唯一定义处

| 常量 | 定义 | 使用方 |
| --- | --- | --- |
| `cn_launch_texts` | `sftpwin.c` | `dlg_confirm_launch`、`sftp_show_confirm_launch`、自检 |
| `cn_quit_texts` | `sftpwin.c` | `dlg_confirm_quit`、`sftp_show_confirm_quit`、自检 |

`default_is_cancel` 更名为 `default_on_right`，字段名改为描述位置而非按钮
语义（按钮语义随调用方变化，只有位置稳定）。

## 三、验证

### 3.1 自检（52 项，全过）

| 自检 | 结果 |
| --- | --- |
| `-confirm-save` | 21 项通过，0 失败 |
| `-confirm-quit` | 17 项通过，0 失败（原 6 项 + 新增 11 项三按钮版面） |
| `-confirm-launch` | 14 项通过，0 失败 |
| `test_conf.exe` | 退出码 0 |

退出自检新增项覆盖：标题「确认保存」、三个按钮文案、`BS_OWNERDRAW` /
`BS_DEFPUSHBUTTON` / `BS_PUSHBUTTON` 样式位、三按钮同顶、间距一致（8/8px）、
整组水平居中（左右余量 20/20px）。

### 3.2 改造中修复的两个自身回归

1. **蓝框丢失**：右按钮误接 `st_second` 槽位，而该槽位未设任何 `PUSH` 样式位，
   「长期生效」的蓝框静默消失（自检「是默认按钮」失败，style 两个都是
   `0x50010000`）。改为 `st_first` / `st_second` / `st_third` 严格对应左/中/右。
2. **退出自检假失败**：断言仍是 `dlg_confirm_quit(&dp) == true`，而枚举首值
   `CN_QUIT_DISCARD` 为 0、`true` 为 1，恒不成立。改为与 `CN_QUIT_DISCARD` 比较。

### 3.3 端到端（真实点击，非模拟）

- 打开配置框 → 点「串口」单选钮制造改动（「保存」按钮由灰转亮，确认
  `conf` 真的变了）→ 点「退出」：弹出三按钮框，标题「确认保存」，红框
  「直接退出」/ 普通「取消」/ 蓝框「保存并退出」，窗口 350×166。
- 点「取消」：弹窗关闭，配置框保留（句柄不变）。
- 记录：用 `SetWindowTextW` 直接改端口控件**不产生改动**（绕过控件
  `EVENT_VALIDATE`，`conf` 未更新，「保存」按钮仍是灰的），此时点「退出」
  直接关窗属正确行为。此项曾误判为 bug，实测排除。

## 四、新增入口

`-confirm-quit-show`：把三按钮退出确认真显示出来供人工看图。自检只能量
样式位，红框与蓝框画得对不对必须看图。

## 五、图标垂直居中（最终版）

### 5.1 问题

首版内容区高度取 `max(清单高, 图标高)`，图标与清单都从 `ytop` 顶对齐：

```c
h = m + lh + cn_dpi(6) + (list_h > qicon_size ? list_h : qicon_size)
    + cn_dpi(12) + cn_dpi(26) + m;
```

清单只有一行时（高 23px < 图标 48px），内容区高度由图标决定，图标底边
正好落在灰带顶线上。截图像素实测：

| 位置 | y |
| --- | --- |
| 首行文字最低墨迹 | 53 |
| 图标可见顶部 | 63 |
| 图标可见底部 | 110 |
| 灰带顶线 | 111 |

上方视觉间距 9px（`cn_dpi(6)` 控件距 + 字形下伸留白），下方 0px。

### 5.2 第一次尝试（写死下边距，已废弃）

在高度公式末尾补 `cn_dpi(8)`。自检断言「内容区底边到灰带顶线 >= 8px」
通过，但断言本身量错了对象：五行清单高 74px > 图标 48px，内容区底部
由清单决定，图标底边离灰带还有 34px，读出来像「间距失控」。

写死下边距的另一个问题是：清单从一行变五行时，图标上下的留白比例
不变（都是 0 / 8），图标始终贴着内容区顶部，视觉上偏上。

### 5.3 最终方案：图标在内容区垂直居中

主人指定「上下边距随内容区域同步增大」，改为：

```c
content_h = list_h > qicon_size ? list_h : qicon_size;
h = m + lh + cn_dpi(6) + content_h + cn_dpi(8) + cb->band_h;
/* 创建图标时 */
int icon_y = ytop + (content_h - qicon_size) / 2;
```

清单贴 `ytop` 起排，图标在 `content_h` 内居中。效果：

| 清单 | 内容区高 | 图标上下留白 |
| --- | --- | --- |
| 1 行（23px） | 48px（图标决定） | 0 / 0 |
| 5 行（74px） | 74px（清单决定） | 13 / 13 |

留白随内容增高同步变大，两个极端（图标贴顶 / 图标贴底）都不会出现。

内容区底边与灰带之间另留 `cn_dpi(8)`，与按钮带的上边距 `cn_dpi(12)`
形成层次，避免内容直接压到分隔线。

### 5.4 自检断言

改为验证居中而非固定间距：

```
图标 y=50..98，内容区 y=37..111，上下留白 13/13px
通过  图标在内容区垂直居中（上下留白相等）
通过  内容区底边与灰带之间留出 >=8px
```

容差 1px：`(content_h - qicon_size) / 2` 整除取整，奇数高度会有 1px 偏差。

### 5.5 确认覆盖弹窗同步

`cn_conflict_dialog_create` 里图标原本也是顶对齐（注释写「40px 已高于
行高，顶对齐列表首行」），按同一口径改为居中，两处弹窗视觉一致。

### 5.6 回归

| 自检 | 结果 |
| --- | --- |
| `-confirm-save` | 23 项通过，0 失败 |
| `-confirm-quit` | 17 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `test_conf.exe` | 退出码 0 |

截图核对：图标位于清单左侧垂直居中，上下留白肉眼等宽。

---

# 附录：照设计图重做视觉（2026-11-08 第三轮）

## A.1 需求

按主人提供的设计图重做「确认保存」弹窗外观：

1. 浅灰底 + 白色圆角卡片包住改动清单；
2. 清单每行行首加圆形勾/叉图标 —— 涉及关闭、禁用打叉，涉及打开、
   切换等动作打勾；
3. 选项名与旧值同色，箭头与新值同色；
4. 行尾加装饰折角（橙色）；
5. 按钮改为蓝框确定 + 灰框取消。

**追加修订（主人明确否掉的部分）**：标题栏、右上角 ❌、窗口边框
一律用回系统原生，不许自绘。

## A.2 数据结构改造

`conf_describe_changes` 原本只输出纯字符串 `【选项名】 旧值 → 新值`，
要分段上色就必须结构化。解析字符串不可行 —— 值本身可能含「→」或
「】」（自定义标题、主机名、端口转发都有），会切错。

改为先收集结构、再拼字符串，两个版本共用同一份数据：

```c
typedef struct CnChangeItem {
    char *name, *oldval, *newval;
    CnChangeKind kind;      /* 打勾 / 打叉 */
} CnChangeItem;
```

字符串版输出逐字节保持不变，`test_conf` 的既有断言全部照旧通过 ——
这也是判断重构没走形的依据。新增、删除两类子键的格式曾弄反两次
（原格式只在旧值一侧带子键前缀，新值一侧只写裸值），均由
`test_conf` 当场抓出。

## A.3 自绘范围：只画客户区

窗口改用 `WS_POPUP | WS_CAPTION | WS_SYSMENU`，标题栏、关闭按钮、
边框全部交回系统。相应地删除：

| 删除项 | 原用途 |
| --- | --- |
| `cn_apply_round_region` | 窗口圆角裁剪 |
| `cn_draw_close_x` | 自绘标题栏右上角大 X |
| `font_title` 字段 | 自绘标题的大号粗体 |
| `CN_CLR_TITLE` 宏 | 自绘标题的深蓝 |
| `WM_NCHITTEST` 分支 | 自绘标题栏拖动 |
| `WM_LBUTTONUP` 命中判定 | 自绘 X 的点击 |
| `close_cx/close_cy/close_hit/close_hot` | 自绘 X 的几何与悬停 |

`title_h` 字段保留但恒为 0，客户区从首行说明直接开始。窗口尺寸改用
`AdjustWindowRectEx` 按客户区目标尺寸反推，否则加了标题栏后内容会被
挤掉一截。

## A.4 白卡片背景曾整体缺失

首版截图整窗只有灰底 `#F7F7F5`，白卡片完全没画出来。

根因：`cb->card_left` / `card_right` 两个字段声明后从未赋值，一直是 0，
`RoundRect` 退化成一个零面积图形，画了等于没画。补上赋值即修复：

```c
cb->card_left = m;
cb->card_right = cw - m;
```

## A.5 截图通道：外部脚本截不到窗口

本机环境下外部 PowerShell 完全枚举不到本进程的顶层窗口
（`EnumWindows` 返回 0 条，连主窗都没有），`FindWindowW` 与
`PrintWindow` 一并失效，因此截不到图。

改为让窗口自己渲染自己，由环境变量触发：

```
CN_CONFIRM_SAVE_SHOT=<输出 bmp 路径> putty.exe -confirm-save-show
```

在 `WM_PAINT` 首帧之后**投递**一条自定义消息再截图（不能直接在
`WM_PAINT` 里调 `PrintWindow`，那是重入），存成 24 位 BMP 后自行关窗。

渲染方式踩了三次坑：

1. `WM_PRINT + PRF_CHILDREN`：子控件全被画到 DC 原点，STATIC 与按钮
   挤在左上角；
2. 手动 `SetViewportOrgEx` 平移子控件：按钮依然空白；
3. `BitBlt` 窗口 DC：只拿得到客户区，标题栏与按钮都没有。

最终方案分两层：非客户区交给 `DefWindowProc` 的 `WM_PRINT`，客户区
自己画，子控件按 `GetWindowRect` 相对窗口左上角的偏移逐个平移渲染。
按钮句柄与坐标已单独打印验证（`ok=0x52056c by=217`），确认不是控件
没建出来。

## A.6 自检断言重写

新版删掉了 `qicon` 控件（勾叉改为自绘在每一行内），旧断言仍在拿它
取坐标，`GetWindowRect(NULL)` 得到垃圾值（y=32355），必然失败。
三项失败全部是断言过时，不是版面画错：

| 旧断言 | 问题 | 新断言 |
| --- | --- | --- |
| 清单在图标右侧、首行顶格 | 用 `lr.left - lr.left` 自比，恒真；且清单 STATIC 已改为不可见文本载体，坐标不代表屏幕位置 | 判自绘几何：勾叉左缘 ≥ 卡片左缘 + 8，文字起于图标右侧且可用宽 ≥ 80px |
| 图标在内容区垂直居中 | 取 `cb.qicon` 坐标，控件已不存在 | 判行高 > 勾叉直径、卡片完整包住清单、清单顶不超卡片顶一行 |
| 浮层上框线与内容区上框线齐平 | 拿首行说明顶比，但浮层锚点是卡片顶 | 改用 `cb.card_top` 换算到屏幕坐标后比 |

## A.7 最终验证

| 项目 | 结果 |
| --- | --- |
| `-confirm-save` | 25 项通过，0 失败 |
| `-confirm-quit` | 17 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `test_conf.exe` | 退出码 0 |
| 编译 | 零警告零错误 |

截图核对（客户区 338×257，整窗 344×286）：原生标题栏含问号图标与
关闭按钮；首行说明顶格；白卡片圆角 `#FFFFFF` 带 `#E4E4E1` 描边；
三行清单每行行首圆形叉、选项名与旧值 `#4C4B49`、箭头与新值
`#00307C` 加粗、行尾橙色折角 `#F3761A`；底部蓝框「确定」+ 灰框
「取消」整组居中。

三按钮版（`-confirm-quit-show`，350×193）同样为原生外壳，左红框
「直接退出」、中灰框「取消」、右蓝框「保存并退出」。

## A.8 问号图标：从自绘改为多尺寸 ICO 资源

### A.8.1 问题

主人目视核对后指出：卡片左侧的大问号「锯齿感很重」，并追问能否
直接用图标而不是自绘。

### A.8.2 逐条排查与尝试

| 尝试 | 结果 | 结论 |
| --- | --- | --- |
| 系统 `IDI_QHELP` 放大 | `LoadImage` 取 16/32/48/64/96/128/256 全部 `NULL` | 系统只提供小尺寸位图，无法用于 80px |
| GDI 现画（原实现） | `Ellipse` 不做抗锯齿，80px 圆环台阶明显 | 主人所指的锯齿即此 |
| 4 倍超采样 + `HALFTONE` 缩小 | 问号整体丢失，截图上完全不显示 | 内存 DIB 上 `StretchBlt` 未按预期工作，放弃 |
| 引入 GDI+ 矢量绘制 | 头文件与 `libgdiplus.a` 均在，但需改 `cmake/platforms/windows.cmake` 链接配置 | 可行性成立，但依赖成本高于收益 |
| **主人提供的位图图标 → 多尺寸 ICO** | 边缘平滑，任意 DPI 清晰 | **采用** |

### A.8.3 素材选择

主人给了两张 2048×2048 PNG，一张「扣好的透明底」一张「白色背景」。
实测两张均为 `RGB` 模式、无 alpha 通道；前者的「透明」实为棋盘格
图案本身（左上角 200×200 区域采样出 68 种交替颜色），直接使用会把
棋盘格带进界面。后者是纯白底（同区域 23 种颜色，主色 `#FFFFFF`），
与白卡片 `CN_CLR_CARD` 同色，**选用后者**。

处理步骤：

1. 按暗像素包围盒（x 333–1706，y 325–1727）定位图形主体；
2. 取中心正方形、外扩 8px 呼吸边，边长 1410，裁掉右下角「豆包 AI
   生成」水印（水印位于 y≈1967 以下，已在裁剪区外）；
3. 亮度 >235 的背景像素统一刷为纯白，消除 AI 生成噪点；
4. 按亮度反算 alpha（白→0，暗→255），生成 10 档尺寸
   （16/20/24/32/40/48/64/96/128/256）的 32 位 ICO，
   替换 `src/windows/icon_question.ico`（53,166 字节）。

### A.8.4 绘制代码

`cn_draw_big_question` 从约 80 行 GDI 绘图缩到 12 行：
`LoadImageW(IDI_QHELP, size, size)` + `DrawIconEx(DI_NORMAL)`。
ICO 各档自带 alpha，`DI_NORMAL` 会做 alpha 混合，边缘平滑由图标
本身保证，无需再套内存位图或 `AlphaBlend`。

中间曾试过「画进 32 位 DIB 再 `BitBlt`」，问号不显示 —— 该路径已删。

### A.8.5 一个容易踩的构建陷阱

`icon_question.ico` 改动后 `ninja putty.exe` **不会重新编译资源**：
CMake 生成的 `putty.rc.obj` 依赖列表里没有 `.ico` 文件，ninja 认为
资源无变化。表现是源码改了、exe 时间戳也更新了，但界面毫无变化。

证据：`windows/CMakeFiles/putty.dir/putty.rc.obj` 时间戳停留在
01:15:10，而 `.ico` 已是 15:35:22。

解法：`rm -f windows/CMakeFiles/putty.dir/putty.rc.obj` 后再
`ninja putty.exe`。重编后该 obj 从 74,870 字节增至 115,380 字节，
说明新图标确实打入。

另有一个排查弯路值得记下：用 Python `ctypes` 探测 exe 内资源时，
`LoadLibraryExW` 未设 `restype`，返回的 64 位句柄被截断成负数，
导致 `FindResourceW` 全部返回 `NULL`，一度误判「资源根本没打进去」。
补上 `restype = c_void_p` 后，205 号 `RT_GROUP_ICON` 立即命中，
16–256 各档 `LoadImage` 全部成功。

### A.8.6 验证

| 项目 | 结果 |
| --- | --- |
| `-confirm-save` | 25 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `-confirm-quit` | 17 项通过，0 失败 |
| `test_conf.exe` | 退出码 0，无输出 |
| 单行弹窗（`-confirm-quit-show`，390×210） | 问号清晰、边缘平滑 |
| 三行弹窗（`-confirm-save-show`，456×286） | 问号随卡片增高而放大，比例协调 |

问号直径仍按卡片内高八成取、夹在 `[40, 96]`；单行时卡片高度另设
下限「问号 + 上下各 12px」，避免小尺寸显得局促。

---

## A.9 清单折行：从「只折新值」改为两级折行

### A.9.1 问题

超长选项名（如「启用终端使用 xterm 256 色模式并且把配色方案改成
Solarized Dark」）整段冲出白卡片右缘，被窗口裁掉。截图可见首行末尾
只剩半个「S」。

### A.9.2 根因

`cn_draw_change_row()` 里「选项名：旧值」这一段是**无条件下一次性
画出去**的：先用 `GetTextExtentPoint32W` 量出宽度，然后 `TextOutW`
整段输出，中间没有任何与 `right_limit` 的比较。只有「新值」那一段
有折行逻辑。所以选项名本身一长，必然溢出。

此外，绘制与测高分叉过：`cn_change_row_lines()` 当时只量新值的折行，
与绘制各算一套。

### A.9.3 修改

1. 新增 `cn_layout_change_row()`：**唯一的排版器**，只算位置不画。
   输出 `CnRowLayout`（各段的起始行号、x 起点、是否段内折行）。
2. 新增 `cn_fit_prefix()`：二分求「宽度不超过 avail 的最长前缀字符数」，
   绘制与测高共用。
3. `cn_draw_change_row()` 改为只按排版结果输出文本，不再自己算折行。
4. `cn_change_row_lines()` 改为直接调排版器，删掉整套重复估算逻辑。

### A.9.4 折行规则（两级）

1. **改动项优先**：「选项名：旧值」+ 箭头先当整体量宽度。
   - 整体放得下 → 留在首行；
   - 改动项自己能占一行、但加箭头放不下 → 改动项留首行，箭头与
     新值另起一行；
   - 改动项本身比整行还宽 → 才在改动项内部按字符折行。
2. **再算新值**：改动项落位后量剩余宽度，放不下就在新值内部折行，
   续行从文字起点（`text_x`）左对齐。

### A.9.5 验证

| 项目 | 结果 |
| --- | --- |
| `-confirm-save` | 25 项通过，0 失败 |
| `-confirm-quit` | 17 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `test_conf.exe` | 退出码 0 |
| 超长样例截图（`CN_CONFIRM_SAVE_LONG=1`） | 长选项名完整折为两行，未溢出卡片 |
| 常规三行截图 | 三行均单行完整显示，无回归 |

### A.9.6 构建环境陷阱（本次排查耗时最多）

`ninja` 报 `FAILED: windows/CMakeFiles/putty.dir/sftpwin.c.obj`，但
**没有任何编译器诊断输出**，且同一条命令手工执行退出码为 0。

根因：`cc1.exe` 依赖 `libmpfr-6.dll`，而当前 shell 的 `PATH` 里
没有 `C:\msys64\mingw64\bin`。`cc1.exe` 起不来时退出码为 1 且不打印
任何信息，`cc.exe` 把这个失败原样上报，ninja 于是判定编译失败。

验证方式：直接调用 `cc1.exe` 可看到真实报错

```
cc1.exe: error while loading shared libraries: libmpfr-6.dll:
cannot open shared object file: No such file or directory
```

解法：编译前把 `C:\msys64\mingw64\bin` 加入 `PATH`：

```bash
PATH="/c/msys64/mingw64/bin:$PATH" /c/msys64/mingw64/bin/ninja.exe putty.exe
```

另注：`cmake` 与 `ninja` 均不在默认 `PATH` 中，需用绝对路径调用
（`C:/msys64/mingw64/bin/`）。

---

## A.10 字号放大 10% 与按钮带垂直居中

### A.10.1 需求

1. 字体整体增大 10%。
2. 按钮位置改为「改动内容区」与「窗口底部」之间垂直居中，以下边距
   为基准（实测约 17px）。

### A.10.2 字号放大

弹窗内共有三处按 `lfMessageFont` 创建字体：

| 位置 | 用途 |
| --- | --- |
| `font` | 正文（选项名 / 旧值 / 箭头） |
| `font_value` | 新值（加粗） |
| `meas_font` | 建窗时测折行行数用的临时加粗字体 |

三处若不同步，会出现「量的是小字号、画的是大字号」，折行行数算错、
卡片高度不够。因此新增 `cn_scale_logfont()`，按
`CN_FONT_SCALE_NUM / CN_FONT_SCALE_DEN`（11/10）缩放 `lfHeight`，
三处统一调用。

只放大字号，不换字族：字族仍是 `Microsoft YaHei UI`（来自
`lfMessageFont`），换族会在不同系统上出现中文回退跳变。

### A.10.3 按钮带垂直居中

**原实现的问题**

```c
cb->band_h = cn_dpi(16) + cn_dpi(32) + m;   /* 16 + 32 + 14 = 62 */
h = cb->card_bottom + m + cb->band_h;       /* 又多加一个 m */
by = ch - m - bh;                           /* 按钮 y */
```

三处叠加导致按钮上方空白比下方多出整整一个 `m`（14px）：

- `band_h` 里的 `32` 是按钮槽位，但按钮实际高度只有 26，多出 6px；
- `h` 又额外加了 `m`，把按钮带整体往下推；
- `by` 用 `ch - m - bh` 定位，与上面两处不自洽。

**修改后**

```c
#define CN_BTN_H 26
cb->band_h = m + cn_dpi(CN_BTN_H) + m;   /* 上留白 + 按钮 + 下留白 */
h = cb->card_bottom + cb->band_h;        /* 不再额外加 m */
by = cb->card_bottom + m;                /* 按钮在带内居中 */
```

三处用同一个 `CN_BTN_H`，上留白与下留白严格相等。

**像素实测**（`cs5.png`，456×241）：

| 项目 | 值 |
| --- | --- |
| 白卡片底边 | y = 182 |
| 按钮边框上下 | y = 199 .. 222 |
| 卡片底 → 按钮顶 | 16 px |
| 按钮底 → 窗口底 | 18 px |

差的 2px 来自边框检测把 1px 抗锯齿边缘算进按钮高度，几何计算上
上下留白均为 `m = 14px`，完全相等。

### A.10.4 文字与行首图标的垂直对齐

**问题**：字号放大后，「字往下掉」的观感更明显（主人截图指出）。

**根因**：绘制时用 `y - sz.cy / 2` 让文字居中，但 `sz.cy` 是**整个
字符框高度**（含 ascent + descent + 外部行距），而汉字墨迹并不在
字符框正中 —— ascent 里有一部分是留给拉丁字母上伸部的空白。

用 `tmprobe.exe` 实测该字体（`Microsoft YaHei UI`，放大后）：

```
lfHeight = -13
tmHeight = 19   tmAscent = 15   tmDescent = 4
sz.cy    = 19
sz.cy / 2            = 9
(tmAscent - tmDescent) / 2 = 5
差 = 4 px
```

正好等于截图里量到的文字偏低量（图标墨迹 88..109，文字墨迹
80..109，底边齐平、顶边差 8px，中心差 4px）。

**修改**：新增 `cn_text_baseline_for_center()`，用真实度量算基线：

```c
baseline_y = cy + (tmAscent - tmDescent) / 2;
SetTextAlign(hdc, TA_LEFT | TA_BASELINE);
TextOutW(hdc, x, baseline_y, ...);
SetTextAlign(hdc, TA_LEFT | TA_TOP);
```

这样文字的**视觉中心**（而非字符框中心）与行首图标的圆心对齐。

### A.10.5 验证

| 项目 | 结果 |
| --- | --- |
| `-confirm-save` | 25 项通过，0 失败 |
| `-confirm-quit` | 17 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `test_conf.exe` | 退出码 0 |
| 两按钮弹窗截图 | 字号变大、按钮上下留白均衡、文字与图标齐平 |
| 三按钮弹窗截图 | 同上，红框「直接退出」与蓝框「保存并退出」正常 |
| 超长内容截图 | 折行正确，两行文字均与图标对齐 |

## A.11 排版器统一：箭头位置由排版结果给出，绘制端不再重算

### A.11.1 问题

A.9 的两级折行上线后，超长项虽然能折行了，但箭头（`→`）位置仍然错：
绘制端画完旧值后，自己再用 `GetTextExtentPoint32W` + `cn_fit_prefix`
走一遍折行循环去推断「旧值最后停在哪一行、哪个 x」，再据此画箭头。
这段推断逻辑与排版器 `cn_layout_change_row` 的规则不是同一份代码，
两边的折行点、行高、可用宽度边界必然存在细微分歧，表现为箭头跑到
上一行末尾、或者跑到行首、或者干脆压在新值上。

### A.11.2 根因

同一件事被算了两遍，而且第二遍是「照着第一遍的规则重写一份」。
排版器已经把每个段的起始行 `seg_line[]` 与起始 x `seg_x[]` 算出来了，
绘制端却不用，非要自己再推一遍旧值的结束位置 —— 属于典型的
「双份真相」（two sources of truth）。

### A.11.3 修改

`cn_layout_change_row` 的输出结构新增 `arrow_x` 与 `arrow_line`：
排版器在排完段 1（旧值）后，直接在当前光标位置记下箭头应画在哪，
必要时先换行。绘制端 `cn_draw_change_row` 的段 1 分支改为：

```c
if (i == 1) {
    static const wchar_t arrow[] = L" → ";
    int liney = cy + lay.arrow_line * line_h;
    int by = cn_text_baseline_for_center(hdc, liney);

    SelectObject(hdc, font);
    SetTextColor(hdc, CN_CLR_ACCENT);
    SetTextAlign(hdc, TA_LEFT | TA_BASELINE);
    TextOutW(hdc, text_x + lay.arrow_x, by, arrow, 3);
    SetTextAlign(hdc, TA_LEFT | TA_TOP);
}
```

原先那段约 45 行的「重算旧值结束位置」循环整体删除。现在箭头位置
只有一个来源，绘制端与测量端（`cn_change_row_lines`）共用同一份
`cn_layout_change_row` 结果，不可能再分叉。

### A.11.4 验证

| 项目 | 结果 |
| --- | --- |
| 编译 | 通过，`-Wall -Wextra` 无 unused 警告 |
| `-confirm-save` | 25 项通过，0 失败 |
| `-confirm-quit` | 17 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `test_conf.exe` | 退出码 0 |
| 常规截图（`-confirm-save-show`） | 三项清单箭头紧跟旧值，蓝色，位置正确 |
| 超长截图（`CN_CONFIRM_SAVE_LONG=1`） | 长项折成两行，续行从文字起点对齐，箭头随旧值落在第二行 |
| 三按钮截图（`-confirm-quit-show`） | 单行清单居中，红框/蓝框/省略号预留行均正常 |

### A.11.5 省略号预留行

四行以上改动折叠时，卡片高度 `nrows` 原先只累加清单项行数，
没给省略号预留一行，导致卡片画满后省略号被 `y + row_h > card_bottom`
条件挡掉、完全不显示。已在折叠分支给 `nrows` 加 1。
实测四行改动场景：显示三行 + 一行 `.......`，卡片高度正好容下。

### A.11.6 截图入口备忘

超长场景没有独立命令行开关，用环境变量：

```
CN_CONFIRM_SAVE_SHOT=out.bmp CN_CONFIRM_SAVE_LONG=1 putty.exe -confirm-save-show
```

`-confirm-save-wrap-show` 之类的名字并不存在，写错会被 PuTTY 以
「未知选项」弹窗拒绝。

## A.12 首行文案改冒号 + 清单在卡片内垂直居中

### A.12.1 需求

1. 首行说明末尾的句号改为冒号。
2. 清单行（连同行首勾叉图标）在卡片内垂直居中，不再贴着上沿。

### A.12.2 文案

`CN_HEAD_TEXT` 由「……请核对后保存或撤销。」改为「……请核对后保存或撤销：」。
该宏是三处引用（绘制、测宽、自检断言）的唯一来源，改一处即全局生效，
自检里那条 `wcscmp(buf, whead) == 0` 的断言仍通过，说明没有漏改的硬编码。

### A.12.3 居中实现

原先清单从 `card_top + cn_dpi(10)` 起画，卡片高度却是按内容算的，
多出来的空间全堆在下方，单行时表现为「文字贴上沿、下面空一片」。

现在绘制前先模拟一遍与绘制循环**完全相同**的判据
（`y + row_h > card_bottom - cn_dpi(6)` 就停），算出真正会画出来的
内容总高，再按 `(卡片内高 - 内容总高) / 2` 定起点，并夹一个
`cn_dpi(6)` 的下限防止内容顶到圆角。

两个容易踩的点：

- 模拟起点必须与绘制起点一致，否则算出的可用空间和实际不符。
  这里两边都用 `card_top + cn_dpi(10)` 作为判据的初始 y。
- 折叠时的省略行不是 `changes` 里的一项，必须单独加进内容总高，
  否则居中会按「少一行」算，整块偏上。

### A.12.4 省略号的基线对齐

省略行原用 `y + row_h / 2 - sz.cy / 2` 定位，与正文的
`cn_text_baseline_for_center()` 不是同一套算法，视觉上比其他行偏高、
和最后一项贴得过近。已改为与正文一致的基线对齐：

```c
int by = cn_text_baseline_for_center(hdc, y + cb->row_h / 2);
SetTextAlign(hdc, TA_LEFT | TA_BASELINE);
TextOutW(hdc, rowx + cb->icon_sz + cn_dpi(8), by, dots, 7);
```

### A.12.5 验证

代码内打印实测（临时加 `CN_CONFIRM_SAVE_DEBUG` 输出，验完已删除）：

```
[vcenter] card=43..191 h=148 content=132 top=51 上留=8 下留=8
```

上下留白各 8px，完全对称。

截图逐像素量测（卡片白色区纵向范围 vs 文字墨迹范围）：

| 场景 | 卡片 | 上留白 | 下留白 |
| --- | --- | --- | --- |
| 单行（三按钮） | 70..131 | 24 | 26 |
| 三行 + 省略 | 43..191 | 8 | 8 |
| 超长折行 | 131..248 | 17 | 18 |

单行那组上下差 2px，来自 22px 勾叉图标的抗锯齿边缘被计入墨迹；
图标与文字的垂直中心实测完全一致（均为 99.5）。

| 回归项 | 结果 |
| --- | --- |
| `-confirm-save` | 25 项通过，0 失败 |
| `-confirm-quit` | 17 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `test_conf.exe` | 退出码 0 |



## A.13 「关」落单与省略号消失（2026-11 修复）

### A.13.1 现象

主人截图（三项改动，第二项折行）暴露两个问题：

1. 第二项行末孤零零一个「关」，下一行只有「→ 开」；
2. 该出现省略号的位置什么都没有。

### A.13.2 「关」落单：旧值与箭头分两段排

`cn_layout_change_row()` 原先对段 1（旧值）和箭头分别处理：旧值只要
在行末剩余宽度里放得下就留在本行，箭头放不下才换行。一个「关」字
的宽度远小于「→ 开」，于是旧值赖在上一行、箭头独自下移。

读起来像选项名结尾带个「关」，正是主人指出的「只包含到『：』」
要求没落实到位。

修法：把「旧值 → 新值」整段当**不可分整体**。当前行放不下、但整段
单独一行放得下时，三者一起下移；只有旧值本身超过整行宽度（超长文件
路径）才退回段内逐字符折行。

```c
int valw = oldw + arroww + neww;   /* 「旧值 → 新值」整段宽度 */
if (olen > 0 && cursor_x + valw <= avail) {
    /* 原地排 */
} else if (olen > 0 && cursor_x > 0 && valw <= avail) {
    /* 整段下移，arrow_x = oldw */
} else {
    /* 超长旧值：段内折行 */
}
```

### A.13.3 省略号消失：三处判据互相打架

| 环节 | 原判据 |
| --- | --- |
| 卡片高度 | `nrows`（各项折行行数之和）+ `truncated ? 1 : 0` |
| 居中排版 | 模拟累计内容高，含省略行 |
| 绘制循环 | 逐行判 `y + row_h > card_bottom - 6` 就 break |

居中阶段已经把省略号的位置算进内容高，绘制走到省略号时 `y` 恰好顶到
内容底，那个 `-6` 的余量让它判定「放不下」，直接跳过。卡片下方还空着
一行，省略号却没了。

改为**一次算定**：先算总预算 `cap = (cardh - 2*pad) / row_h`，有折叠时
先扣掉省略行那一格，剩下的才轮到项；结果存进 `cb->draw_n` /
`cb->draw_dots`，绘制阶段照单执行，不再二次判断。

### A.13.4 卡片高度算少一行：两个坐标系混用

`nrows` 由 `cn_change_row_lines()` 量出，但量的时候用的 `rowx` 与绘制
端不同源：

| | 公式 | 实测值 |
| --- | --- | --- |
| 测量端（修前） | `14 + QICON_MAX + 14` | 124 |
| 绘制端 | `card_left + 14 + qicon_sz + 14` | 138 |

测量端漏了开头的 `m`（`card_left`），可用宽度比实际宽 14px，行数因此
少算一行。卡片高度按少一行算，绘制时多出来的那一行把省略号挤出预算。

修法：测量端改用与绘制端同一个表达式，`rowx = m + 14 + QICON_MAX + 14`。
问号直径取上限 `CN_QICON_MAX` 而非实际值（实际值依赖卡片高度，存在
循环依赖），方向安全：预留只会更宽，量出的行数不会少于实际。

### A.13.5 验证

四个用例截图逐一目视 + 代码内打印核对：

| 用例 | 项数 | 折行后行数 | 卡片高 | 显示项 | 省略号 |
| --- | --- | --- | --- | --- | --- |
| 主人同款（三项，第二项折行） | 3 | 4 | 148 | 3 / 3 | 无（未截断） |
| 主人同款 + 第四项 | 4 | 4 + 1 | 181 | 3 / 4 | 有 |
| 超长选项名 | 5 | 4 + 1 | 181 | 3 / 5 | 有 |
| 默认五行 demo | 5 | 3 + 1 | 148 | 3 / 5 | 有 |

主人要求的「三项必须完整显示、高度自适应增加」在前两行成立：三项
折行占 4 行时卡片自动从 148 涨到 181，不靠省略号挤掉内容。

| 回归项 | 结果 |
| --- | --- |
| `-confirm-save` | 25 项通过，0 失败 |
| `-confirm-quit` | 17 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `test_conf.exe` | 退出码 0 |

### A.13.6 新增复现入口

`sftp_show_confirm_save()` 增加两组与主人截图同款的样例，供后续回归：

| 环境变量 | 内容 |
| --- | --- |
| `CN_CONFIRM_SAVE_OWNER3=1` | 三项，第二项折行（主人截图原样） |
| `CN_CONFIRM_SAVE_OWNER=1` | 四项，第二项折行（触发折叠） |
| `CN_CONFIRM_SAVE_LONG=1` | 超长选项名（原有） |

## A.14 按钮三处距离等分（2026-11）

### A.14.1 需求

「按键之间的距离和按键与窗口左右边界的距离相等」——即

```
左留白 = 按钮间距 = 右留白
```

### A.14.2 原实现

整组按总宽居中、相邻间距固定 `cn_dpi(8)`：

```c
total = bw[0];
for (i = 1; i < bw_n; i++) total += gap + bw[i];   /* gap = 8 */
bx = (cw - total) / 2;
```

左右留白相等（居中成立），但随窗口变宽远大于 8px，三处距离不等。
窗口宽 450、两按钮总宽 192 时，左右留白各 129、中间只有 8。

### A.14.3 新实现

由窗口宽反解间距，`n` 个按钮把客户区分成 `n+1` 段空白：

```c
slots  = bw_n + 1;
blanks = cw - total;          /* 所有按钮宽之和已扣掉 */
base   = blanks / slots;
rem    = blanks - base * slots;
seg_w[i] = base + (i < rem ? 1 : 0);   /* 余数从左往右逐段 +1 */
```

创建按钮时按段推进（`cx += bw[i] + seg_w[i+1]`），不再用固定 gap。

### A.14.4 余数必须摊掉

整数除法剩下的零头若直接丢弃，会全部落在右留白上：

| 场景 | cw | 按钮总宽 | 空白 | 截断后 | 实测 |
| --- | --- | --- | --- | --- | --- |
| 两按钮 | 450 | 192 | 258 | 258/3 = 86 整 | 86 / 86 / 86 |
| 三按钮 | 415 | 288 | 127 | 127/4 = 31 余 3 | 修前 31/31/34 |

三按钮时右留白多 3px，肉眼能看出偏。把余数按「从左往右逐段 +1」
摊开后为 32/32/32/31，最大偏差 1px，视觉上完全相等。

间距另设下限 `cn_dpi(6)`：窗口极窄时 `base` 可能算出 0 或负数，
按钮会贴在一起。

### A.14.5 自检断言同步

两处断言原本写死了旧规则，必须跟着改，否则量的是过时的设计：

| 位置 | 旧断言 | 新断言 |
| --- | --- | --- |
| 两按钮（`-confirm-save`） | `gap == cn_dpi(8)` + 左右留白相等 | 间距与左留白相等、间距与右留白相等 |
| 三按钮（`-confirm-quit`） | `abs(left - right) <= 2`（整组居中） | 左留白等于间距、右留白等于间距 |

容差统一取 2px，覆盖像素取整与余数分配。

### A.14.6 验证

| 场景 | 间距 | 左留白 | 右留白 |
| --- | --- | --- | --- |
| 两按钮，cw=450 | 86 | 86 | 86 |
| 两按钮，cw=300 | 36 | 36 | 36 |
| 三按钮，cw=415 | 32 / 32 | 32 | 31 |

| 回归项 | 结果 |
| --- | --- |
| `-confirm-save` | 25 项通过，0 失败 |
| `-confirm-quit` | 18 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `test_conf.exe` | 退出码 0 |

## A.15 删除卡片内大问号（2026-11）

### A.15.1 需求

「把大问号图标删除，仅保留标题栏的小问号图标」，并明确「不仅仅是
确认保存的弹窗，所有同一套模板的弹窗都一并改动」。

### A.15.2 影响范围

`cn_confirm_save_paint()` / `cn_confirm_save_build()` 是两个弹窗**共用**
的绘制与构建函数，三个入口都汇到它：

| 入口 | 弹窗 | 形态 |
| --- | --- | --- |
| `cn_confirm_save_dialog()` | 确认保存 | 两按钮 |
| `cn_confirm_save_dialog()`（三按钮） | 退出前确认 | 三按钮 |
| `cn_confirm_dialog_texts()` | 连接 / 文件传输前确认 | 两按钮，默认在右 |

改共用函数，三处同时生效，无需分别处理。

「确认覆盖」弹窗（`cn_conflict_proc` / `PuTTYSftpConflictBox`）是**另一套**
实现，其问号是 48px 的 STATIC 控件（`cb->qicon`），不在本次范围内，
未改动。

### A.15.3 改动清单

去掉的不只是绘制调用 —— 大问号占的横向空间串在五条路径上，只删一处
会导致测量与绘制不一致（本报告 A.13.4 已因此踩过一次坑）：

| 位置 | 改动 |
| --- | --- |
| `cn_confirm_save_paint()` | 删掉 `cn_draw_big_question()` 调用块 |
| 绘制端 `rowx` | `card_left + 14 + qicon_sz + 14` → `card_left + 14` |
| 构建端宽度预留 | `row_deco` 去掉 `qicon_reserve + 14` 两段 |
| 测量端 `rowx` | `m + 14 + QICON_MAX + 14` → `m + 14`（与绘制端同一表达式） |
| 卡片高度下限 | 删掉 `min_cardh = QICON_MIN + 24` 的撑高逻辑 |
| 浮层命中区 | `hot.left` 去掉 `qicon_sz + 14` |
| 删除的死代码 | `cn_draw_big_question()` 函数体、`CN_QICON_MIN/MAX` 宏、`ConfirmSaveBox.qicon_sz` 字段 |

### A.15.4 副作用（正面）

横向空间不再被占用，窗口宽度与折行数一起受益：

| 项目 | 改前 | 改后 |
| --- | --- | --- |
| 确认保存窗口宽（三行样例） | 456px | 386px |
| 第二项「禁用服务器发送 ^? 时破坏性回退删除」 | 折成两行 | 一行放下 |
| 卡片高度下限 | 64px（单行时被撑高） | 由行数决定 |

单行弹窗不再被撑到 64px 高、下面空一片。

### A.15.5 验证

三个弹窗逐一截图目视，标题栏左上角红色小问号均在（由
`icon_question.ico` + `WM_SETICON` 提供，系统绘制，与本改动无关）：

| 弹窗 | 尺寸 | 大问号 | 标题栏小问号 |
| --- | --- | --- | --- |
| 确认保存（三行） | 386×241 | 无 | 有 |
| 退出前确认（三按钮） | 350×175 | 无 | 有 |
| 连接前确认 | 311×208 | 无 | 有 |

| 回归项 | 结果 |
| --- | --- |
| `-confirm-save` | 25 项通过，0 失败 |
| `-confirm-quit` | 18 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `test_conf.exe` | 退出码 0 |

自检里仍有三条「问号图标」断言（`cb.qicon` 非空、底缘不越按钮顶缘），
属确认覆盖弹窗的 STATIC 控件，与本模板无关，保持原样。
（后续：那套 STATIC 实现已在 A.17 整体删除，这三条断言也随之重写。）

---

## A.16 确认覆盖弹窗改用「原始自绘模板」（2026-11）

### A.16.1 需求

主人原话：「将确认保存的弹窗的模板命名为原始自绘模板，把确认覆盖的
弹窗模板替换成原始自绘模板并做适配」。另经图片回复确定：三个按钮中
「全部覆盖」放最右边，并作为默认按钮。

### A.16.2 原实现为何必须整段替换

改动前「确认覆盖」走的是完全独立的一套（约 1752～2876 行）：原生
`BUTTON` + `STATIC` 控件、48px 问号图标、自己的 `ConflictBox` 状态机
与 `cn_conflict_proc` / `cn_conflict_create` / `cn_conflict_loop` /
`cn_conflict_box_done` / `cn_count_static_childs` / `cn_conflict_join`。
与「确认保存」模板在圆角卡片、分段着色、按钮带留白、悬停浮层上全都
不一致 —— 这正是两个弹窗观感不同的根源。局部对齐治不了这种结构差异，
因此整段删除后改走共享模板。

删除规模：402 行（`ConflictBox` 结构与六个函数全部移除）。

### A.16.3 模板扩展的五项

| 扩展点 | 目的 |
| --- | --- |
| `structured` 入参（`CnChangeList *`） | 不走「【】→」字符串解析，直接用结构化数据排版 |
| 纯名字行 | `oldval` / `newval` 均为空串时不拼冒号、不画箭头 |
| `CnChange.icon_id` | 行首图标按资源 ID 直填，覆盖弹窗不必借用勾/叉语义 |
| `CnConfirmTexts.icon_id` | 标题栏图标可配置，覆盖弹窗挂警告图标而非问号 |
| `CnConfirmTexts.third_is_danger` | 红框挂在指定按钮，不靠位置猜 |

关于 `third_is_danger` 的必要性：模板三按钮槽位固定是「ok / cancel /
third」，`third` 恒为最右且恒为默认。退出确认的危险项「直接退出」在
**最左**，确认覆盖的危险项「全部覆盖」在**最右** —— 同样是三按钮，
危险项恰恰在相反两侧，红框挂哪个必须显式指定。

配套增加 `ConfirmSaveBox.b_danger` 字段（`memset` 已清零，`-Werror`
不报），`WM_DRAWITEM` 据此支持最右按钮画红框红字。

### A.16.4 结构上的两个坑

**坑一：孤立箭头**。纯名字行新旧值都是空串，但排版器用 `olen > 0`
判断是否有旧值，若只判 `arrow` 是否绘制就会画出孤零零的「→」，且
`cursor_x` 无条件加箭头宽度会把后续内容推后。改法：引入 `arrow_drawn`
标志，仅在真正绘出箭头时置真，三个排版分支统一按它推进坐标。

**坑二：`icon_id` 未初始化**。`cn_change_add` 与 `cn_parse_changes_text`
都不曾给 `icon_id` 赋值，新增字段后这两个构造点会留下随机值。两处
均补 0（含义：按 `kind` 用勾/叉）。

### A.16.5 数据适配

`ConflictItem`（只有名字 + 是否目录）→ `CnChange`（名字 + 旧值 + 新值 +
行首图标），映射成纯名字行。该映射与文案集中在 `cn_overwrite_ask`，
`cn_confirm_batch` 与作图入口 `-confirm-overwrite-show` 共用同一份，
保证「截图看到的就是线上效果」。

返回值映射：`IDOK` → `CN_OVW_SKIP`（跳过已存在，最左）、`IDYES` →
`CN_OVW_OVERWRITE`（全部覆盖，最右、默认、红框）、其余（含 Esc 与标题栏
X）→ `CN_OVW_ABORT`。关闭一个「要不要覆盖」的窗绝不能被当成「全部覆盖」。

### A.16.6 折叠口径修正（主人追问后）

初次接上模板后出现：「5 个冲突项只显示 2 项就出现了省略号」。主人直接
问「为啥只有两项就触发了省略」。

根因是折叠口径写错。`CN_CONFIRM_SAVE_MAX_ITEMS`（原
`CN_CONFIRM_SAVE_MAX_LINES`）的语义是「最多显示几项」，确认保存的
字符串路径就是「取前 3 项，若还有更多则额外补一行省略」。而我把它
当成「总行数预算」，还从中扣掉一格给省略号：

```
预算 = 3 行
扣 1 行给省略号 → 只剩 2 行
每项 1 行 → 只画 2 项
```

改为按项数：

```
n <= 3  → 全部显示，不出省略号
n >  3  → 显示前 3 项 + 一行省略（共占 4 行，卡片高度自适应撑得住）
```

同时把绘制端那套「按卡片实际高度重算 cap、再逐行判 `y + row_h >
card_bottom - 6`」的旧逻辑删掉 —— 它就是历史上「省略号被跳过」的
元凶，两套判据互相打架。现在单一来源：build 阶段算定 `draw_n` /
`draw_dots`，paint 阶段照单执行，一个字都不重算。

宏同时由 `CN_CONFIRM_SAVE_MAX_LINES` 更名为 `CN_CONFIRM_SAVE_MAX_ITEMS`
（9 处引用），避免下次又被「LINES」误导。

### A.16.7 行首图标：一律中性圆点（主人决定）

初版按「目录 → 警告图标、文件 → 中性圆点」区分，理由是「覆盖目录是
连锅端」。主人追问「为什么内容区要用感叹号的小图标」，确定改为全部
中性圆点：

- 内容区只陈述「该项已存在于目标位置」，不表达危险度；
- 危险提示由标题栏那一个警告图标统一承担，内容区再挂感叹号属语义重复；
- 「同名目录已存在」本身也不构成危险动作。

目录 / 文件在清单里一视同仁，不再用图标区分。标题栏仍为
`IDI_CN_WARN`（红圈叹号）。

### A.16.8 新增看图入口

`-confirm-overwrite-show`：覆盖弹窗平时只在真实传输扫到同名项时弹出，
为截一张图跑完整传输流程太重，故单留入口。演示数据 5 项（超过 3 项
上限，可同时看到折叠与省略行）。

`CN_OVERWRITE_LONG=1` 切换为超长名字组（含一个中英混排），用于验证
折行：长项必须折成多行、卡片高度跟着长。环境变量命名与
`CN_CONFIRM_SAVE_LONG` 保持同风格。

### A.16.9 验证

四套自检合计 **253 项通过，0 失败**：

| 回归项 | 结果 |
| --- | --- |
| `-confirm-save` | 25 项通过，0 失败 |
| `-confirm-quit` | 18 项通过，0 失败 |
| `-confirm-launch` | 14 项通过，0 失败 |
| `-sftp-win`（窗口全量） | 196 项通过，0 失败 |

覆盖弹窗新增/重写的关键断言：

- 清单里是冲突项名字，且纯名字行不带冒号、不画箭头（U+2192 不出现）
- `draw_n == 3`、`draw_dots == 1`（按项数上限显示前三项 + 省略行）
- 两项时不折叠、不出省略号
- 折叠时挂上浮层，且浮层里是全部五项
- 按钮文字与从左到右顺序为 跳过已存在 / 取消 / 全部覆盖

截图目视核对（`reports/shot-overwrite.png`、`shot-overwrite-long.png`）：

- 标题栏红圈警告图标在，卡片内无大问号
- 行首全部中性圆点，目录与文件一致
- 5 项显示前 3 项 + 一行「.......」
- 三按钮间距相等，红框在最右「全部覆盖」
- 超长名字折两行，悬挂缩进对齐到文字起点（不是贴图标下），
  中英混排断行位置正常，省略行仍在

按钮带像素实测：按钮高 25px，下沿至窗口底 17px，与主人要求的
「以现在按键与窗口底部的距离为基准（大概 17px）」一致。

### A.16.10 已知取舍

- 折叠上限 3 项是沿用确认保存的既有规范，不是为覆盖弹窗单独设定。
  若日后冲突项普遍多于 3 个、希望一次看全，改
  `CN_CONFIRM_SAVE_MAX_ITEMS` 一个数字即可，卡片高度会自动跟随。
- `cn_draw_chevron` 仍无调用者，但其 `chev_sz` 参与自检几何计算，
  本轮仍未动，避免一次改动过大。
