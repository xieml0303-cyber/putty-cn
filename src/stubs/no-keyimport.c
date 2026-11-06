/*
 * Stub: the in-process key import/convert feature is Windows-GUI
 * only. Unix builds get no-op versions so the shared config-dialog
 * code links cleanly.
 */

#include "putty.h"

bool keyimport_run(void *hwnd, Filename **out)
{
    (void)hwnd;
    *out = NULL;
    return false;
}

bool keyimport_auto_convert(void *hwnd, const Filename *src, Filename **out)
{
    (void)hwnd;
    (void)src;
    *out = NULL;
    return false;
}
