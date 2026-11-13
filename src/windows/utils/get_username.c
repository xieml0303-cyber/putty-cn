/*
 * Implementation of get_username() for Windows.
 */

#include "putty.h"

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <security.h>

char *get_username(void)
{
    DWORD namelen;
    char *user = NULL;
    bool got_username = false;
    DECL_WINDOWS_FUNCTION(static, BOOLEAN, GetUserNameExW,
                          (EXTENDED_NAME_FORMAT, LPWSTR, PULONG));

    {
        static bool tried_usernameex = false;
        if (!tried_usernameex) {
            /* Not available on Win9x, so load dynamically */
            HMODULE secur32 = load_system32_dll("secur32.dll");
            /* If MIT Kerberos is installed, the following call to
               GET_WINDOWS_FUNCTION makes Windows implicitly load
               sspicli.dll WITHOUT proper path sanitizing, so better
               load it properly before */
            HMODULE sspicli = load_system32_dll("sspicli.dll");
            (void)sspicli; /* squash compiler warning about unused variable */
            GET_WINDOWS_FUNCTION(secur32, GetUserNameExW);
            tried_usernameex = true;
        }
    }

    if (p_GetUserNameExW) {
        /*
         * If available, use the principal -- this avoids the problem
         * that the local username is case-insensitive but Kerberos
         * usernames are case-sensitive.
         */

        /* Get the length */
        wchar_t *wuser;
        namelen = 0;
        (void) p_GetUserNameExW(NameUserPrincipal, NULL, &namelen);

        wuser = snewn(namelen, wchar_t);
        got_username = p_GetUserNameExW(NameUserPrincipal, wuser, &namelen);
        if (got_username) {
            wchar_t *p = wuser;
            while (*p && *p != L'@')
                p++;
            *p = L'\0';
            user = dup_wc_to_mb(CP_UTF8, wuser, "?");
        }
        sfree(wuser);
    }

    if (!got_username) {
        /* Fall back to local user name */
        wchar_t *wuser;
        namelen = 0;
        if (!GetUserNameW(NULL, &namelen)) {
            /*
             * Apparently this doesn't work at least on Windows XP SP2.
             * Thus assume a maximum of 256. It will fail again if it
             * doesn't fit.
             */
            namelen = 256;
        }

        wuser = snewn(namelen, wchar_t);
        got_username = GetUserNameW(wuser, &namelen);
        if (got_username)
            user = dup_wc_to_mb(CP_UTF8, wuser, "?");
        sfree(wuser);
    }

    return got_username ? user : NULL;
}
