/*
 * PuTTY: 导入 OpenSSH / ssh.com 等外部格式私钥并转存为 PPK。
 *
 * 这是 puttygen「载入 + 保存私钥」流程的进程内简化版：
 * 不含密钥生成，只做格式转换。核心逻辑全部复用共享代码
 * （import.c 的解析与 sshpubk.c 的保存），本文件只负责
 * Windows GUI 胶水：文件选择、口令输入框、结果提示。
 *
 * 两条入口共用同一套转换内核：
 *   keyimport_run()          证书页「导入并转换密钥」按钮，
 *                            选一次文件 → 同目录自动生成 .ppk，
 *                            把生成的路径交回调用方回填输入框；
 *   keyimport_auto_convert() 私钥文件框直接选中外部格式私钥时
 *                            由 conf_filesel_handler 调用。
 */

#include <windows.h>
#include <stdio.h>

#include "putty.h"
#include "ssh.h"
#include "putty-rc.h"
#include "platform.h"

/* 口令对话框的上下文 */
struct keyimport_pass_ctx {
    char *pass;    /* malloc 出来的 UTF-8 口令，NULL 表示未输入 */
};

/*
 * 可导入的格式清单（key_type 的返回值）。
 * PPK / 公钥文件不列——PPK 本来就是 PuTTY 原生格式无需转换，
 * 公钥文件不含私钥部分，转了也没法认证。
 *
 * 这里不能列 SSH_KEYTYPE_SSH1：import_possible() 对它返回 false，
 * 且 import_ssh2_s() 也没有 SSH-1 分支（import.c 里 "There are no
 * known foreign SSH-1 key formats"），列进来只会成为永远走不通的
 * 死条目——keyimport_lookup() 会在 import_possible 那关把它挡掉。
 */
static const struct keyimport_format {
    int type;
    const char *desc;
} importable_formats[] = {
    { SSH_KEYTYPE_OPENSSH_PEM, "OpenSSH SSH-2 私钥 (旧 PEM 格式)" },
    { SSH_KEYTYPE_OPENSSH_NEW, "OpenSSH SSH-2 私钥 (新格式)" },
    { SSH_KEYTYPE_SSHCOM,      "ssh.com SSH-2 私钥" },
};

/*
 * 判定文件是否属于可导入格式，是则返回其条目下标，否则返回 NULL。
 */
static const struct keyimport_format *keyimport_lookup(const Filename *fn)
{
    int type = key_type(fn);
    size_t i;

    for (i = 0; i < lenof(importable_formats); i++)
        if (importable_formats[i].type == type)
            break;
    if (i == lenof(importable_formats) || !import_possible(type))
        return NULL;
    return &importable_formats[i];
}

/*
 * 返回 src 同目录同名、扩展名为 .ppk 的新路径（Filename 对象）。
 * 这就是「在所选私钥所在文件夹下生成同名 ppk」的实现。
 */
static Filename *keyimport_derive_ppk_path(const Filename *src)
{
    const char *path = filename_to_str(src);
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '\\');
    size_t baselen = (dot && dot > slash) ? (size_t)(dot - path)
                                          : strlen(path);
    char *defpath = dupprintf("%.*s.ppk", (int)baselen, path);
    Filename *ret = filename_from_str(defpath);
    sfree(defpath);
    return ret;
}

/*
 * 口令对话框过程（资源模板 IDD_KEYPASS，见 putty-common.rc2）。
 * 编辑框控件 ID 201。
 */
INT_PTR CALLBACK keyimport_passproc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam)
{
    struct keyimport_pass_ctx *ctx;

    switch (msg) {
      case WM_INITDIALOG:
        SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)lParam);
        SetFocus(GetDlgItem(hwnd, 201));
        return FALSE;
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK: {
            ctx = (struct keyimport_pass_ctx *)GetWindowLongPtr(hwnd, DWLP_USER);
            wchar_t *w = GetDlgItemTextW_alloc(hwnd, 201);
            ctx->pass = encode_wide_string_as_utf8(w);
            sfree(w);
            EndDialog(hwnd, IDOK);
            return TRUE;
          }
          case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* 弹出口令输入框，返回 UTF-8 口令（用户取消时返回 NULL） */
static char *keyimport_ask_passphrase(HWND hwnd)
{
    struct keyimport_pass_ctx ctx = { NULL };
    INT_PTR ret = DialogBoxParam(GetModuleHandle(NULL),
                                 MAKEINTRESOURCE(IDD_KEYPASS), hwnd,
                                 keyimport_passproc, (LPARAM)&ctx);
    return (ret == IDOK) ? ctx.pass : NULL;
}

/*
 * 把 src 导入并保存为 outfn。成功返回 NULL，失败返回静态描述字符串。
 * 口令、PRNG 生命周期等易错细节都收在这里，两条入口共用。
 */
static const char *keyimport_convert(const struct keyimport_format *fmt,
                                     const Filename *src, const Filename *outfn,
                                     HWND hwnd, bool ask_overwrite)
{
    if (ask_overwrite && open_for_write_would_lose_data(outfn)) {
        char *msg = dupprintf("目标文件已存在：\n%s\n\n是否覆盖？",
                              filename_to_str(outfn));
        int mbret = message_box(hwnd, msg, "PuTTY 导入密钥",
                                MB_YESNO | MB_ICONWARNING, true, 0);
        sfree(msg);
        if (mbret != IDYES)
            return "用户取消了覆盖操作。";
    }

    char *comment = NULL;
    bool encrypted = import_encrypted(src, fmt->type, &comment);
    sfree(comment);

    char *passphrase = NULL;
    if (encrypted) {
        passphrase = keyimport_ask_passphrase(hwnd);
        if (!passphrase)
            return "用户取消了口令输入。";
    }

    const char *err = NULL;
    ssh2_userkey *key = import_ssh2(src, fmt->type, passphrase, &err);
    if (!key) {
        char *msg = dupprintf("导入失败：%s\n\n"
                              "若密钥已加密，请确认口令是否正确。",
                              err ? err : "无法解析该私钥");
        message_box(hwnd, msg, "PuTTY 导入密钥", MB_ICONERROR, true, 0);
        sfree(msg);
        burnstr(passphrase);
        return "无法解析该私钥。";
    }
    burnstr(passphrase);                 /* 口令用完即焚 */

    /*
     * PRNG 只在 SSH 连接期间由 ssh 层激活；未连接的配置对话框阶段
     * 它尚未初始化，而 ppk_save_f 保存时要用随机数生成 salt/MAC key。
     * 只在未激活时才自行初始化并在用完后归还，避免干扰会话中的
     * 全局 PRNG 状态。
     */
    bool own_prng = false;
    if (!random_is_active()) {
        random_setup_special();
        own_prng = true;
    }

    bool saved = ppk_save_f(outfn, key, NULL, &ppk_save_default_parameters);

    if (own_prng)
        random_unref();

    ssh_key_free(key->key);
    sfree(key->comment);
    sfree(key);

    if (!saved) {
        message_box(hwnd, "保存 PPK 失败：无法写入目标文件。",
                    "PuTTY 导入密钥", MB_ICONERROR, true, 0);
        return "无法写入目标文件。";
    }
    return NULL;
}

/*
 * 按钮入口：选一次外部私钥 → 同目录自动生成同名 .ppk → 通过 *out
 * 交回生成的路径（调用方负责 filename_free），返回 true。
 * 用户取消或任何失败返回 false，*out 保持 NULL。
 *
 * parent 为父窗口句柄（以 void* 传入以保持跨平台头文件兼容）。
 */
bool keyimport_run(void *parent, Filename **out)
{
    HWND hwnd = (HWND)parent;
    *out = NULL;

    Filename *fn = request_file(hwnd, "选择要导入的私钥文件", NULL, false,
                                NULL, false, FILTER_ALL_FILES);
    if (!fn)
        return false;                    /* 用户取消 */

    const struct keyimport_format *fmt = keyimport_lookup(fn);
    if (!fmt) {
        message_box(hwnd,
                    "无法导入该文件：\n"
                    "它不是 OpenSSH / ssh.com 私钥格式。\n"
                    "(PuTTY 自家的 PPK 文件无需转换，直接在上方填写即可；"
                    "公钥文件不能用于认证。)",
                    "PuTTY 导入密钥", MB_ICONERROR, true, 0);
        filename_free(fn);
        return false;
    }

    /* 不再二次询问保存位置：直接在源文件所在目录生成同名 .ppk */
    Filename *outfn = keyimport_derive_ppk_path(fn);

    const char *fail = keyimport_convert(fmt, fn, outfn, hwnd, true);
    if (fail) {
        filename_free(outfn);
        filename_free(fn);
        return false;
    }

    char *msg = dupprintf("转换成功！\n\n"
                          "源密钥: %s\n"
                          "类型: %s\n"
                          "已保存为 PPK: %s\n"
                          "已自动填入「认证私钥文件」。",
                          filename_to_str(fn), fmt->desc,
                          filename_to_str(outfn));
    message_box(hwnd, msg, "PuTTY 导入密钥", MB_ICONINFORMATION, true, 0);
    sfree(msg);

    filename_free(fn);
    *out = outfn;                        /* 所有权移交调用方 */
    return true;
}

/* ------------------------------------------------------------------ */
/* 以下为「选文件自动转换」路径（conf_filesel_handler 对 CONF_keyfile
 * 特判调用）：用户在私钥文件框里选中外部格式私钥时，后台转成同目录
 * 同名 .ppk 并直接填入，免去手动点按钮的步骤。 */

/*
 * 自动转换：src 为用户在「认证私钥文件」框中给出的文件。若它是
 * 可导入的外部格式，经确认后转成同目录同名 .ppk，通过 *out 返回
 * （调用方负责 filename_free），返回 true；其余情况返回 false，
 * 调用方按原逻辑处理（PPK 直接使用、公钥等由认证阶段自行报错）。
 */
bool keyimport_auto_convert(void *parent, const Filename *src, Filename **out)
{
    *out = NULL;
    HWND hwnd = (HWND)parent;

    const struct keyimport_format *fmt = keyimport_lookup(src);
    if (!fmt)
        return false;                    /* 非外部格式，走原逻辑 */

    /*
     * 确认框：让用户知情并同意，同时兜住文本框逐字符编辑造成的
     * 偶发触发（中间态路径一般连不上文件，直接静默返回）。
     */
    char *ask = dupprintf(
        "检测到外部格式私钥：\n%s\n\n"
        "是否自动将其转换为 PuTTY 可用的 PPK 文件？\n"
        "(将在同目录生成同名 .ppk，原文件保持不动)",
        filename_to_str(src));
    if (message_box(hwnd, ask, "PuTTY 导入密钥",
                    MB_YESNO | MB_ICONQUESTION, true, 0) != IDYES) {
        sfree(ask);
        return false;
    }
    sfree(ask);

    Filename *outfn = keyimport_derive_ppk_path(src);
    const char *fail = keyimport_convert(fmt, src, outfn, hwnd, true);
    if (fail) {
        filename_free(outfn);
        return false;
    }

    *out = outfn;                        /* 所有权移交调用方 */
    return true;
}
