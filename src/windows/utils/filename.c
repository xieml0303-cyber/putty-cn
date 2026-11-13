/*
 * Implementation of Filename for Windows.
 */

#include <wchar.h>

#include "putty.h"

Filename *filename_from_str(const char *str)
{
    /*
     * 入参按 UTF-8 解释（本项目内部字符串统一 UTF-8），cpath 与
     * utf8path 同义，不再经过 ACP。
     */
    Filename *fn = snew(Filename);
    fn->cpath = dupstr(str);
    fn->wpath = decode_utf8_to_wide_string(fn->cpath);
    fn->utf8path = dupstr(fn->cpath);
    return fn;
}

Filename *filename_from_wstr(const wchar_t *str)
{
    Filename *fn = snew(Filename);
    fn->wpath = dupwcs(str);
    fn->cpath = encode_wide_string_as_utf8(fn->wpath);
    fn->utf8path = dupstr(fn->cpath);
    return fn;
}

Filename *filename_from_utf8(const char *ustr)
{
    Filename *fn = snew(Filename);
    fn->utf8path = dupstr(ustr);
    fn->wpath = decode_utf8_to_wide_string(fn->utf8path);
    fn->cpath = dupstr(fn->utf8path);
    return fn;
}

Filename *filename_copy(const Filename *fn)
{
    Filename *newfn = snew(Filename);
    newfn->cpath = dupstr(fn->cpath);
    newfn->wpath = dupwcs(fn->wpath);
    newfn->utf8path = dupstr(fn->utf8path);
    return newfn;
}

const char *filename_to_str(const Filename *fn)
{
    /* cpath 现在就是 UTF-8，与内部字符串约定一致。 */
    return fn->cpath;
}

const wchar_t *filename_to_wstr(const Filename *fn)
{
    return fn->wpath;
}

bool filename_equal(const Filename *f1, const Filename *f2)
{
    /* wpath is primary: two filenames refer to the same file if they
     * have the same wpath */
    return !wcscmp(f1->wpath, f2->wpath);
}

bool filename_is_null(const Filename *fn)
{
    return !*fn->wpath;
}

void filename_free(Filename *fn)
{
    sfree(fn->wpath);
    sfree(fn->cpath);
    sfree(fn->utf8path);
    sfree(fn);
}

void filename_serialise(BinarySink *bs, const Filename *f)
{
    put_asciz(bs, f->utf8path);
}
Filename *filename_deserialise(BinarySource *src)
{
    const char *utf8 = get_asciz(src);
    return filename_from_utf8(utf8);
}

char filename_char_sanitise(char c)
{
    if (strchr("<>:\"/\\|?*", c))
        return '.';
    return c;
}

FILE *f_open(const Filename *fn, const char *mode, bool isprivate)
{
#ifdef LEGACY_WINDOWS
    /* Fallback for legacy pre-NT windows, where as far as I can see
     * _wfopen just doesn't work at all */
    init_winver();
    if (osPlatformId == VER_PLATFORM_WIN32_WINDOWS ||
        osPlatformId == VER_PLATFORM_WIN32s)
        return fopen(fn->cpath, mode);
#endif

    wchar_t *wmode = dup_mb_to_wc(CP_UTF8, mode);
    FILE *fp = _wfopen(fn->wpath, wmode);
    sfree(wmode);
    return fp;
}
