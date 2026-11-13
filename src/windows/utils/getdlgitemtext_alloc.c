/*
 * Handy wrappers around GetDlgItemText (A and W) which don't make you
 * invent an arbitrary length limit on the output string. Returned
 * string is dynamically allocated; caller must free.
 */

#include <wchar.h>

#include "putty.h"

char *GetDlgItemText_alloc(HWND hwnd, int id)
{
    /*
     * puTTY-cn: this program's internal string encoding is UTF-8, so
     * the narrow version of this function reads the control's text as
     * wide characters and converts to UTF-8 explicitly, instead of
     * relying on the ANSI API and the process code page (which would
     * mangle non-ASCII text on a CP936 system).
     */
    wchar_t *wtext = GetDlgItemTextW_alloc(hwnd, id);
    char *text = encode_wide_string_as_utf8(wtext);
    sfree(wtext);
    return text;
}

wchar_t *GetDlgItemTextW_alloc(HWND hwnd, int id)
{
    HWND item = GetDlgItem(hwnd, id);
    size_t size = GetWindowTextLengthW(item) + 1;
    wchar_t *text = snewn(size, wchar_t);
    GetWindowTextW(item, text, size);
    return text;
}
