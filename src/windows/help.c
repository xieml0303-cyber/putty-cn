/*
 * help.c: centralised functions to launch Windows HTML Help files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "putty.h"
#include "putty-rc.h"

#ifdef NO_HTMLHELP

/* If htmlhelp.h is not available, we can't do any of this at all */
bool has_help(void) { return false; }
void init_help(void) { }
void shutdown_help(void) { }
void launch_help(HWND hwnd, const char *topic) { }
void quit_help(HWND hwnd) { }

#else

#include <htmlhelp.h>

static char *chm_path = NULL;
static bool chm_created_by_us = false;

/* chm_path 内部是 UTF-8，删除时显式转宽字符。 */
static void delete_chm_file(const char *path)
{
    if (!path)
        return;
    wchar_t *wpath = dup_mb_to_wc(CP_UTF8, path);
    DeleteFileW(wpath);
    sfree(wpath);
}

static bool requested_help;
DECL_WINDOWS_FUNCTION(static, HWND, HtmlHelpW, (HWND, LPCWSTR, UINT, DWORD_PTR));

static HRSRC chm_hrsrc;
static DWORD chm_resource_size = 0;
static const void *chm_resource = NULL;

int has_embedded_chm(void)
{
    static bool checked = false;
    if (!checked) {
        checked = true;

        chm_hrsrc = FindResource(
            NULL, MAKEINTRESOURCE(ID_CUSTOM_CHMFILE),
            MAKEINTRESOURCE(TYPE_CUSTOM_CHMFILE));
    }
    return chm_hrsrc != NULL ? 1 : 0;
}

static bool find_chm_resource(void)
{
    static bool checked = false;
    if (checked)       /* we've been here already */
        goto out;
    checked = true;

    /*
     * Look for a CHM file embedded in this executable as a custom
     * resource.
     */
    if (!has_embedded_chm())    /* set up chm_hrsrc and check if it's NULL */
        goto out;

    chm_resource_size = SizeofResource(NULL, chm_hrsrc);
    if (chm_resource_size == 0)
        goto out;

    HGLOBAL chm_hglobal = LoadResource(NULL, chm_hrsrc);
    if (chm_hglobal == NULL)
        goto out;

    chm_resource = (const uint8_t *)LockResource(chm_hglobal);

  out:
    return chm_resource != NULL;
}

static bool load_chm_resource(void)
{
    bool toret = false;
    char *filename = NULL;
    char *tempdir = NULL;
    HANDLE filehandle = INVALID_HANDLE_VALUE;
    bool created = false;

    static bool tried_to_load = false;
    if (tried_to_load)
        goto out;
    tried_to_load = true;

    /*
     * We've found it! Now write it out into a separate file, so that
     * htmlhelp.exe can handle it.
     */

    /* GetTempPath is documented as returning a size of up to
     * MAX_PATH+1 which does not count the NUL */
    wchar_t wtempdir[MAX_PATH + 2];
    if (GetTempPathW(lenof(wtempdir), wtempdir) == 0)
        goto out;
    tempdir = dup_wc_to_mb(CP_UTF8, wtempdir, "?");

    unsigned long pid = GetCurrentProcessId();

    for (uint64_t counter = 0;; counter++) {
        filename = dupprintf(
            "%s\\putty_%lu_%"PRIu64".chm", tempdir, pid, counter);
        wchar_t *wfilename = dup_mb_to_wc(CP_UTF8, filename);
        filehandle = CreateFileW(
            wfilename, GENERIC_WRITE, FILE_SHARE_READ,
            NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        sfree(wfilename);

        if (filehandle != INVALID_HANDLE_VALUE)
            break;                     /* success! */

        if (GetLastError() != ERROR_FILE_EXISTS)
            goto out;                  /* failed for some other reason! */

        sfree(filename);
        filename = NULL;
    }
    created = true;

    const uint8_t *p = (const uint8_t *)chm_resource;
    for (DWORD pos = 0; pos < chm_resource_size ;) {
        DWORD to_write = chm_resource_size - pos;
        DWORD written = 0;

        if (!WriteFile(filehandle, p + pos, to_write, &written, NULL))
            goto out;
        pos += written;
    }

    chm_path = filename;
    filename = NULL;
    chm_created_by_us = true;
    toret = true;

  out:
    if (created && !toret)
        delete_chm_file(filename);
    sfree(filename);
    sfree(tempdir);
    if (filehandle != INVALID_HANDLE_VALUE)
        CloseHandle(filehandle);
    return toret;
}

static bool find_chm_from_installation(void)
{
    static const char *const reg_paths[] = {
        "Software\\SimonTatham\\PuTTY64\\CHMPath",
        "Software\\SimonTatham\\PuTTY\\CHMPath",
    };

    for (size_t i = 0; i < lenof(reg_paths); i++) {
        char *filename = get_reg_sz_simple(
            HKEY_LOCAL_MACHINE, reg_paths[i], NULL);

        if (filename) {
            chm_path = filename;
            chm_created_by_us = false;
            return true;
        }
    }

    return false;
}

void init_help(void)
{
    /* Just in case of multiple calls */
    static bool already_called = false;
    if (already_called)
        return;
    already_called = true;

    /*
     * Don't even try looking for the CHM file if we can't even find
     * the HtmlHelp() API function.
     */
    HINSTANCE dllHH = load_system32_dll("hhctrl.ocx");
    GET_WINDOWS_FUNCTION(dllHH, HtmlHelpW);
    if (!p_HtmlHelpW) {
        FreeLibrary(dllHH);
        return;
    }

    /*
     * If there's a CHM file embedded in this executable, we should
     * use that as the first choice.
     */
    if (find_chm_resource())
        return;

    /*
     * Otherwise, try looking for the CHM in the location that the
     * installer marked in the registry.
     */
    if (find_chm_from_installation())
        return;
}

void shutdown_help(void)
{
    if (chm_path && chm_created_by_us) {
        p_HtmlHelpW(NULL, NULL, HH_CLOSE_ALL, 0);
        delete_chm_file(chm_path);
    }
    sfree(chm_path);
    chm_path = NULL;
    chm_created_by_us = false;
}

bool has_help(void)
{
    return chm_path != NULL || chm_resource != NULL;
}

void launch_help(HWND hwnd, const char *topic)
{
    if (!chm_path && chm_resource) {
        /*
         * If we've been called without already having a file name for
         * the CHM file, that might be because we've located it in our
         * resource section but not written it to a temp file yet. Do
         * so now, on first use.
         */
        load_chm_resource();
    }

    /* If we _still_ don't have a CHM pathname, we just can't display help. */
    if (!chm_path)
        return;

    if (topic) {
        char *fname = dupprintf(
            "%s::/%s.html>main", chm_path, topic);
        wchar_t *wfname = dup_mb_to_wc(CP_UTF8, fname);
        p_HtmlHelpW(hwnd, wfname, HH_DISPLAY_TOPIC, 0);
        sfree(wfname);
        sfree(fname);
    } else {
        wchar_t *wpath = dup_mb_to_wc(CP_UTF8, chm_path);
        p_HtmlHelpW(hwnd, wpath, HH_DISPLAY_TOPIC, 0);
        sfree(wpath);
    }
    requested_help = true;
}

void quit_help(HWND hwnd)
{
    if (requested_help)
        p_HtmlHelpW(NULL, NULL, HH_CLOSE_ALL, 0);
    if (chm_path && chm_created_by_us)
        delete_chm_file(chm_path);
}

#endif /* NO_HTMLHELP */
