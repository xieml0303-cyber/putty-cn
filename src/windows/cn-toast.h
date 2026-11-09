/*
 * cn-toast.h: 会话窗口与文件传输窗口共用的轻提示条（toast）。
 *
 * 一条深色圆角提示从窗口边上浮出，滑入 -> 停留 -> 淡出，约 1.5 秒后
 * 自己消失，不打断任何操作。原先这套实现只活在 sftpwin.c 里，会话窗口
 * 的「重启会话 / 开始记录日志 / 停止记录日志」也要给反馈，于是抽成公共
 * 模块 —— 两个窗口共用一份，样式改动只需改一处，不会各走各的。
 *
 * 用法：
 *
 *     CnToast toast;              // 通常内嵌在窗口结构体里，随结构体清零
 *     cn_toast_show(&toast, hwnd, font, CN_TOAST_BOTTOM, 44, "已上传 3 项");
 *     ...
 *     cn_toast_destroy(&toast);   // 窗口销毁时调，先于宿主结构释放
 *
 * 一个 CnToast 对应一条复用的提示条窗口：连续触发时新文案顶掉旧的，
 * 不会在屏幕上叠出一摞。
 */

#ifndef PUTTY_CN_TOAST_H
#define PUTTY_CN_TOAST_H

/*
 * 下面两个常量放在头文件里，是因为界面自检要拿它们卡住节奏（停留
 * 时长必须正好 1.52 秒）。其余阶段枚举、配色、帧数都是实现细节，
 * 留在 .c 里不外露。
 */
#define CN_TOAST_FRAME_MS     16      /* 约 60fps */
#define CN_TOAST_HOLD_FRAMES  95      /* 停留 ≈ 1.5 秒 */

/*
 * 贴哪条边。
 *
 * 文件传输窗口用底部：提示条落在两个列表下沿，正好在视线余光里，也
 * 不会盖住状态栏那行「完成：成功 3…」。
 *
 * 会话窗口用顶部：终端铺满整个客户区，而用户的眼睛和光标都在最底下
 * 那行提示符上 —— 压底部就是糊住正在敲的命令。贴顶部只盖住已经翻上
 * 去的旧输出，1.5 秒后自己散开，代价最小。
 */
typedef enum {
    CN_TOAST_BOTTOM,
    CN_TOAST_TOP
} CnToastPos;

typedef struct CnToast {
    HWND hwnd;              /* 提示条窗口，NULL 表示还没建过 */
    HWND owner;             /* 宿主窗口：摆位基准，也是提示条的 owner */
    HFONT font;             /* 绘制用字体；own_font 为真时由本模块释放 */
    bool own_font;          /* 字体是本模块自己建的 */
    wchar_t *text;          /* 当前文案，换一条就重设 */
    CnToastPos pos;         /* 贴边方式，随每条文案重设 */
    int gap;                /* 距该边的距离，逻辑像素 */
    int phase;              /* CN_TOAST_* */
    int tick;               /* 当前阶段已过的帧数 */
} CnToast;

/*
 * 显示一条提示（几秒后自己消失）。
 *
 * owner 传宿主窗口，提示条水平居中于它的客户区。font 传 NULL 时模块
 * 自己取系统「消息框字体」并接管释放 —— 会话窗口没有现成的 UI 字体，
 * 正好走这条路。gap 是距所贴那条边的**逻辑像素**，内部按 DPI 折算。
 * text 是 ANSI 串，内部按 DEFAULT_CODEPAGE 转宽字符。
 *
 * 无窗口环境（owner 为 NULL）直接丢掉，不建窗 —— 自检用例里那些假
 * 窗口不需要也没有屏幕。
 */
void cn_toast_show(CnToast *t, HWND owner, HFONT font,
                   CnToastPos pos, int gap, const char *text);

/*
 * 收掉提示条（窗口销毁 / 释放资源时调）。
 *
 * 必须在宿主结构体释放之前调用：提示条窗口过程通过 GWLP_USERDATA 反向
 * 持有 CnToast 指针，晚一步就是悬空。
 */
void cn_toast_destroy(CnToast *t);

#endif /* PUTTY_CN_TOAST_H */
