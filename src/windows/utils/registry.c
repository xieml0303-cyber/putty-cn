/*
 * Implement convenience wrappers on the awkward low-level functions
 * for accessing the Windows registry.
 */

#include "putty.h"

HKEY open_regkey_fn(bool create, bool write, HKEY hk, const char *path, ...)
{
    HKEY toret = NULL;
    bool hk_needs_close = false;
    va_list ap;
    va_start(ap, path);

    for (; path; path = va_arg(ap, const char *)) {
        HKEY hk_sub = NULL;

        DWORD access = KEY_READ | (write ? KEY_WRITE : 0);
        LONG status;
        if (create)
            status = RegCreateKeyEx(
                hk, path, 0, NULL, REG_OPTION_NON_VOLATILE,
                access, NULL, &hk_sub, NULL);
        else
            status = RegOpenKeyEx(hk, path, 0, access, &hk_sub);

        if (status != ERROR_SUCCESS)
            goto out;

        if (hk_needs_close)
            RegCloseKey(hk);
        hk = hk_sub;
        hk_needs_close = true;
    }

    toret = hk;
    hk = NULL;
    hk_needs_close = false;

  out:
    va_end(ap);
    if (hk_needs_close)
        RegCloseKey(hk);
    return toret;
}

void close_regkey(HKEY key)
{
    RegCloseKey(key);
}

void del_regkey(HKEY key, const char *name)
{
    RegDeleteKey(key, name);
}

char *enum_regkey(HKEY key, int index)
{
    size_t regbuf_size = MAX_PATH + 1;
    char *regbuf = snewn(regbuf_size, char);

    while (1) {
        LONG status = RegEnumKey(key, index, regbuf, regbuf_size);
        if (status == ERROR_SUCCESS)
            return regbuf;
        if (status != ERROR_MORE_DATA) {
            sfree(regbuf);
            return NULL;
        }
        sgrowarray(regbuf, regbuf_size, regbuf_size);
    }
}

bool get_reg_dword(HKEY key, const char *name, DWORD *out)
{
    DWORD type, size;
    /*
     * cn: 值名本身是 ASCII，但 Reg*W 要 wchar_t*，统一按 UTF-8 解一次。
     * 纯 ASCII 在 UTF-8 下逐字节等价，不存在歧义。
     */
    wchar_t *wname = decode_utf8_to_wide_string(name);
    bool toret = false;

    size = sizeof(*out);
    if (RegQueryValueExW(key, wname, 0, &type,
                         (BYTE *)out, &size) == ERROR_SUCCESS &&
        size == sizeof(*out) && type == REG_DWORD)
        toret = true;

    sfree(wname);
    return toret;
}

bool put_reg_dword(HKEY key, const char *name, DWORD value)
{
    wchar_t *wname = decode_utf8_to_wide_string(name);
    bool toret = RegSetValueExW(key, wname, 0, REG_DWORD,
                                (CONST BYTE *)&value,
                                sizeof(value)) == ERROR_SUCCESS;

    sfree(wname);
    return toret;
}

char *get_reg_sz(HKEY key, const char *name)
{
    DWORD type, size;
    wchar_t *wname, *wtoret = NULL;
    char *toret = NULL;
    DWORD size2;
    size_t nchars, got;

    /*
     * cn: 注册表里存的永远是 UTF-16，所以用 W 接口读出来就是正确的中文，
     * 跟进程代码页无关；再统一编码成 UTF-8 交给上层。此前由 A 接口写入
     * 的老数据也一并正确 —— 那只是写入口按 ACP 转成了 UTF-16，存的本身
     * 没问题。
     */
    wname = decode_utf8_to_wide_string(name);

    if (RegQueryValueExW(key, wname, 0, &type, NULL,
                         &size) != ERROR_SUCCESS || type != REG_SZ)
        goto out;                      /* not a string */

    nchars = size / sizeof(wchar_t);
    wtoret = snewn(nchars + 1, wchar_t);
    size2 = size;
    if (RegQueryValueExW(key, wname, 0, &type, (BYTE *)wtoret,
                         &size2) != ERROR_SUCCESS || type != REG_SZ)
        goto out;

    got = size2 / sizeof(wchar_t);
    if (got > nchars)
        got = nchars;                  /* 值在两次查询之间变长了，截断 */
    wtoret[got] = L'\0';               /* add an extra NUL in case
                                        * RegQueryValueExW didn't supply one */

    toret = encode_wide_string_as_utf8(wtoret);

  out:
    sfree(wtoret);
    sfree(wname);
    return toret;
}

bool put_reg_sz(HKEY key, const char *name, const char *str)
{
    wchar_t *wname = decode_utf8_to_wide_string(name);
    wchar_t *wstr = decode_utf8_to_wide_string(str);
    bool toret;

    /* You have to store the trailing NUL as well */
    toret = RegSetValueExW(key, wname, 0, REG_SZ, (CONST BYTE *)wstr,
                           (DWORD)((wcslen(wstr) + 1) *
                                   sizeof(wchar_t))) == ERROR_SUCCESS;

    sfree(wstr);
    sfree(wname);
    return toret;
}

/*
 * REG_MULTI_SZ items are stored as a concatenation of NUL-terminated
 * strings, terminated in turn with an empty string, i.e. a second
 * consecutive NUL.
 *
 * We represent these in their storage format, as a strbuf - but
 * *without* the second consecutive NUL.
 *
 * So you can build up a new MULTI_SZ value in a strbuf by calling
 * put_asciz once per output string and then put_reg_multi_sz; and you
 * can consume one by initialising a BinarySource to the result of
 * get_reg_multi_sz, and then calling get_asciz on it and assuming
 * that !get_err(src) means you have a real output string.
 *
 * Also, calling strbuf_to_str on one of these will give you back a
 * bare 'char *' with the same double-NUL termination, to pass back to
 * a caller.
 */
strbuf *get_reg_multi_sz(HKEY key, const char *name)
{
    DWORD type, size;
    wchar_t *wname = decode_utf8_to_wide_string(name);
    wchar_t *wbuf = NULL;
    strbuf *toret = NULL;
    DWORD size2;
    size_t wsize, wlen, i;

    if (RegQueryValueExW(key, wname, 0, &type, NULL,
                         &size) != ERROR_SUCCESS || type != REG_MULTI_SZ)
        goto out;                      /* not a string */

    wsize = size / sizeof(wchar_t) + 1;
    wbuf = snewn(wsize, wchar_t);
    wbuf[0] = L'\0';
    size2 = size;
    if (RegQueryValueExW(key, wname, 0, &type, (BYTE *)wbuf,
                         &size2) != ERROR_SUCCESS || type != REG_MULTI_SZ)
        goto out;

    wlen = size2 / sizeof(wchar_t);
    if (wlen >= wsize)
        wlen = wsize - 1;
    wbuf[wlen] = L'\0';

    /*
     * cn: 注册表里是 UTF-16 的多串，程序内部统一用 UTF-8，所以逐段解码
     * 后按原格式（各段以 NUL 分隔）重新拼一个 strbuf。
     */
    toret = strbuf_new();
    i = 0;
    while (i < wlen && wbuf[i]) {
        size_t seglen = 0;
        while (i + seglen < wlen && wbuf[i + seglen])
            seglen++;
        char *seg = encode_wide_string_as_utf8(wbuf + i);
        put_data(toret, seg, strlen(seg));
        put_byte(toret, '\0');
        sfree(seg);
        i += seglen + 1;
    }
    /* Ensure we end with exactly one \0 */
    while (strbuf_chomp(toret, '\0'));
    put_byte(toret, '\0');

  out:
    sfree(wbuf);
    sfree(wname);
    return toret;
}

bool put_reg_multi_sz(HKEY key, const char *name, strbuf *str)
{
    wchar_t *wname = decode_utf8_to_wide_string(name);
    strbuf *wbuf = strbuf_new();
    const char *p = str->s, *end = str->s + str->len;
    const wchar_t nul = L'\0';
    bool toret;

    /*
     * cn: str 里是 UTF-8 多串（各段以 NUL 分隔，结尾一个 NUL）。逐段解码
     * 成 UTF-16 拼进 wbuf，末尾再补一个 NUL 作为列表结束标记。
     */
    while (p < end) {
        size_t seglen = 0;
        while (p + seglen < end && p[seglen])
            seglen++;
        if (seglen == 0)
            break;                     /* empty string: end of the list */
        char *seg = snewn(seglen + 1, char);
        wchar_t *wseg;
        memcpy(seg, p, seglen);
        seg[seglen] = '\0';
        wseg = decode_utf8_to_wide_string(seg);
        put_data(wbuf, wseg, wcslen(wseg) * sizeof(wchar_t));
        put_data(wbuf, &nul, sizeof(nul));
        sfree(wseg);
        sfree(seg);
        p += seglen + 1;
    }
    put_data(wbuf, &nul, sizeof(nul));

    toret = RegSetValueExW(key, wname, 0, REG_MULTI_SZ,
                           (CONST BYTE *)wbuf->s,
                           (DWORD)wbuf->len) == ERROR_SUCCESS;

    strbuf_free(wbuf);
    sfree(wname);
    return toret;
}

char *get_reg_sz_simple(HKEY key, const char *name, const char *leaf)
{
    HKEY subkey = open_regkey_ro(key, name);
    if (!subkey)
        return NULL;
    char *toret = get_reg_sz(subkey, leaf);
    RegCloseKey(subkey);
    return toret;
}
