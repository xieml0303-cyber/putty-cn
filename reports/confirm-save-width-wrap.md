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
