#define SUPERSEDE_FONTSPEC_FOR_TESTING

#include "putty.h"
#include "storage.h"

void modalfatalbox(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

char *platform_default_s(const char *name)
{ return NULL; }
bool platform_default_b(const char *name, bool def)
{ return def; }
int platform_default_i(const char *name, int def)
{ return def; }
FontSpec *platform_default_fontspec(const char *name)
{ return fontspec_new_default(); }
Filename *platform_default_filename(const char *name)
{ return filename_from_str(""); }
char *platform_get_x_display(void) { return NULL; }

void read_random_seed(noise_consumer_t consumer) {}
void write_random_seed(void *data, int len)
{ unreachable("no random seed in this application"); }
bool have_ssh_host_key(const char *hostname, int port,
                       const char *keytype) { return false; }
int check_stored_host_key(const char *hostname, int port,
                          const char *keytype, const char *key) { return 1; }
void store_host_key(Seat *seat, const char *hostname, int port,
                    const char *keytype, const char *key)
{ unreachable("no actual host keys in this application"); }

host_ca_enum *enum_host_ca_start(void) { return NULL; }
bool enum_host_ca_next(host_ca_enum *handle, strbuf *out) { return false; }
void enum_host_ca_finish(host_ca_enum *handle) {}
host_ca *host_ca_load(const char *name) { return NULL; }

void old_keyfile_warning(void) { }

const bool share_can_be_upstream = false;
const bool share_can_be_downstream = false;

/*
 * cn: mirror the real Windows FontSpec layout. conf_describe_changes()
 * compares height as well as name, so a name-only stub here would read
 * past the allocation and report garbage font changes.
 */
struct FontSpec {
    char *name;
    bool isbold;
    int height;
    int charset;
};

FontSpec *fontspec_new(const char *name)
{
    FontSpec *f = snew(FontSpec);
    f->name = dupstr(name);
    f->isbold = false;
    f->height = 12;
    f->charset = 0;
    return f;
}

FontSpec *fontspec_new_default(void)
{
    return fontspec_new("");
}

FontSpec *fontspec_copy(const FontSpec *f)
{
    FontSpec *n = snew(FontSpec);
    n->name = dupstr(f->name);
    n->isbold = f->isbold;
    n->height = f->height;
    n->charset = f->charset;
    return n;
}

void fontspec_free(FontSpec *f)
{
    sfree(f->name);
    sfree(f);
}

void fontspec_serialise(BinarySink *bs, FontSpec *f)
{
    put_asciz(bs, f->name);
}

FontSpec *fontspec_deserialise(BinarySource *src)
{
    return fontspec_new(get_asciz(src));
}

#define MAXKEY 16

typedef enum {
    SAVE_UNSET, SAVE_S, SAVE_I, SAVE_FONTSPEC, SAVE_FILENAME
} SaveType;

typedef struct SaveItem {
    const char *key;
    SaveType type;
    union {
        char sval[4096];
        int ival;
    };
} SaveItem;

struct settings_w {
    size_t n;
    SaveItem si[MAXKEY];
};

settings_w *open_settings_w(const char *sessionname, char **errmsg)
{ return NULL; }
void close_settings_w(settings_w *sw)
{ unreachable("we don't open and close in this test program"); }
settings_r *open_settings_r(const char *sessionname)
{ return NULL; }
void close_settings_r(settings_r *sr) { }

/* Work around lack of true snprintf before VS2015 */
#if defined _WINDOWS && \
    !defined __MINGW32__ && \
    !defined __WINE__ && \
    _MSC_VER < 1900
#define snprintf _snprintf
#endif

void write_setting_s(settings_w *sw, const char *key, const char *value)
{
    for (size_t i = 0; i < sw->n; i++) {
        if (!strcmp(key, sw->si[i].key)) {
            sw->si[i].type = SAVE_S;
            snprintf(sw->si[i].sval, sizeof(sw->si[i].sval), "%s", value);
            break;
        }
    }
}

void write_setting_i(settings_w *sw, const char *key, int value)
{
    for (size_t i = 0; i < sw->n; i++) {
        if (!strcmp(key, sw->si[i].key)) {
            sw->si[i].type = SAVE_I;
            sw->si[i].ival = value;
            break;
        }
    }
}

void write_setting_fontspec(settings_w *sw, const char *key, FontSpec *fs)
{
    for (size_t i = 0; i < sw->n; i++) {
        if (!strcmp(key, sw->si[i].key)) {
            sw->si[i].type = SAVE_FONTSPEC;
            snprintf(sw->si[i].sval, sizeof(sw->si[i].sval), "%s", fs->name);
            break;
        }
    }
}

void write_setting_filename(settings_w *sw, const char *key, Filename *fn)
{
    for (size_t i = 0; i < sw->n; i++) {
        if (!strcmp(key, sw->si[i].key)) {
            sw->si[i].type = SAVE_FILENAME;
            snprintf(sw->si[i].sval, sizeof(sw->si[i].sval), "%s",
                     filename_to_str(fn));
            break;
        }
    }
}

struct settings_r {
    size_t n;
    SaveItem si[MAXKEY];
};

char *read_setting_s(settings_r *sr, const char *key)
{
    if (sr)
        for (size_t i = 0; i < sr->n; i++)
            if (!strcmp(key, sr->si[i].key) && sr->si[i].type == SAVE_S)
                return dupstr(sr->si[i].sval);
    return NULL;
}

int read_setting_i(settings_r *sr, const char *key, int defvalue)
{
    if (sr)
        for (size_t i = 0; i < sr->n; i++)
            if (!strcmp(key, sr->si[i].key) && sr->si[i].type == SAVE_I)
                return sr->si[i].ival;
    return defvalue;
}

FontSpec *read_setting_fontspec(settings_r *sr, const char *key)
{
    if (sr)
        for (size_t i = 0; i < sr->n; i++)
            if (!strcmp(key, sr->si[i].key) && sr->si[i].type == SAVE_FONTSPEC)
                return fontspec_new(sr->si[i].sval);
    return NULL;
}

Filename *read_setting_filename(settings_r *sr, const char *key)
{
    if (sr)
        for (size_t i = 0; i < sr->n; i++)
            if (!strcmp(key, sr->si[i].key) && sr->si[i].type == SAVE_FILENAME)
                return filename_from_str(sr->si[i].sval);
    return NULL;
}

void del_settings(const char *sessionname) {}

settings_e *enum_settings_start(void)
{ return NULL; }
bool enum_settings_next(settings_e *handle, strbuf *out)
{ unreachable("where did you get a settings_e from?"); }
void enum_settings_finish(settings_e *handle)
{ unreachable("where did you get a settings_e from?"); }

static int nfails = 0;

void test_str_simple(int confid, const char *saveid, const char *defexp)
{
    Conf *conf = conf_new();

    do_defaults(NULL, conf);
    const char *defgot = conf_get_str(conf, confid);
    if (0 != strcmp(defgot, defexp)) {
        printf("fail test_str_simple(%s): default = '%s', expected '%s'\n",
               saveid, defgot, defexp);
        nfails++;
    }

    for (int i = 0; i < 2; i++) {
        settings_w sw = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_UNSET,
        };
        static const char *const teststrings[] = { "foo", "bar" };
        const char *teststring = teststrings[i];
        conf_set_str(conf, confid, teststring);
        save_open_settings(&sw, conf);
        if (sw.si[0].type != SAVE_S) {
            printf("fail test_str_simple(%s): saved type = %d, expected %d\n",
                   saveid, sw.si[0].type, SAVE_S);
            nfails++;
        } else if (0 != strcmp(sw.si[0].sval, teststring)) {
            printf("fail test_str_simple(%s): "
                   "saved string = '%s', expected '%s'\n",
                   saveid, sw.si[0].sval, teststring);
            nfails++;
        }

        conf_clear(conf);
        settings_r sr = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_S,
        };
        snprintf(sr.si[0].sval, sizeof(sr.si[0].sval), "%s", teststring);
        load_open_settings(&sr, conf);
        const char *loaded = conf_get_str(conf, confid);
        if (0 != strcmp(loaded, teststring)) {
            printf("fail test_str_simple(%s): "
                   "loaded string = '%s', expected '%s'\n",
                   saveid, loaded, teststring);
            nfails++;
        }
    }
            
    conf_free(conf);
}

void test_utf8_simple(int confid, const char *saveid, const char *defexp)
{
    Conf *conf = conf_new();

    do_defaults(NULL, conf);
    const char *defgot = conf_get_utf8(conf, confid);
    if (0 != strcmp(defgot, defexp)) {
        printf("fail test_utf8_simple(%s): default = '%s', expected '%s'\n",
               saveid, defgot, defexp);
        nfails++;
    }

    for (int i = 0; i < 2; i++) {
        settings_w sw = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_UNSET,
        };
        static const char *const teststrings[] = { "foo", "bar" };
        const char *teststring = teststrings[i];
        conf_set_utf8(conf, confid, teststring);
        save_open_settings(&sw, conf);
        if (sw.si[0].type != SAVE_S) {
            printf("fail test_utf8_simple(%s): saved type = %d, expected %d\n",
                   saveid, sw.si[0].type, SAVE_S);
            nfails++;
        } else if (0 != strcmp(sw.si[0].sval, teststring)) {
            printf("fail test_utf8_simple(%s): "
                   "saved string = '%s', expected '%s'\n",
                   saveid, sw.si[0].sval, teststring);
            nfails++;
        }

        conf_clear(conf);
        settings_r sr = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_S,
        };
        snprintf(sr.si[0].sval, sizeof(sr.si[0].sval), "%s", teststring);
        load_open_settings(&sr, conf);
        const char *loaded = conf_get_utf8(conf, confid);
        if (0 != strcmp(loaded, teststring)) {
            printf("fail test_utf8_simple(%s): "
                   "loaded string = '%s', expected '%s'\n",
                   saveid, loaded, teststring);
            nfails++;
        }
    }

    conf_free(conf);
}

void test_str_ambi_simple(int confid, const char *saveid,
                          const char *defexp, bool defutf8)
{
    Conf *conf = conf_new();
    bool utf8;

    do_defaults(NULL, conf);
    const char *defgot = conf_get_str_ambi(conf, confid, &utf8);
    if (0 != strcmp(defgot, defexp) || utf8 != defutf8) {
        printf("fail test_str_ambi_simple(%s): "
               "default = '%s' (%s), expected '%s' (%s)\n",
               saveid, defgot, utf8 ? "native" : "UTF-8",
               defexp, defutf8 ? "native" : "UTF-8");
        nfails++;
    }

    for (int i = 0; i < 2; i++) {
        settings_w sw = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_UNSET,
        };
        static const char *const teststrings[] = { "foo", "bar" };
        const char *teststring = teststrings[i];
        conf_set_str(conf, confid, teststring);
        save_open_settings(&sw, conf);
        if (sw.si[0].type != SAVE_S) {
            printf("fail test_str_ambi_simple(%s): "
                   "saved type = %d, expected %d\n",
                   saveid, sw.si[0].type, SAVE_S);
            nfails++;
        } else if (0 != strcmp(sw.si[0].sval, teststring)) {
            printf("fail test_str_ambi_simple(%s): "
                   "saved string = '%s', expected '%s'\n",
                   saveid, sw.si[0].sval, teststring);
            nfails++;
        }

        conf_clear(conf);
        settings_r sr = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_S,
        };
        snprintf(sr.si[0].sval, sizeof(sr.si[0].sval), "%s", teststring);
        load_open_settings(&sr, conf);
        const char *loaded = conf_get_str_ambi(conf, confid, &utf8);
        if (0 != strcmp(loaded, teststring) || utf8) {
            printf("fail test_str_ambi_simple(%s): "
                   "loaded string = '%s' (%s), expected '%s' (native)\n",
                   saveid, loaded, utf8 ? "native" : "UTF-8", teststring);
            nfails++;
        }
    }

    conf_free(conf);
}

void test_int_simple(int confid, const char *saveid, int defexp)
{
    Conf *conf = conf_new();

    do_defaults(NULL, conf);
    int defgot = conf_get_int(conf, confid);
    if (defgot != defexp) {
        printf("fail test_int_simple(%s): default = %d, expected %d\n",
               saveid, defgot, defexp);
        nfails++;
    }

    for (int i = 0; i < 2; i++) {
        settings_w sw = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_UNSET,
        };
        static const int testints[] = { 12345, 54321 };
        int testint = testints[i];
        conf_set_int(conf, confid, testint);
        save_open_settings(&sw, conf);
        if (sw.si[0].type != SAVE_I) {
            printf("fail test_int_simple(%s): saved type = %d, expected %d\n",
                   saveid, sw.si[0].type, SAVE_I);
            nfails++;
        } else if (sw.si[0].ival != testint) {
            printf("fail test_int_simple(%s): "
                   "saved integer = %d, expected %d\n",
                   saveid, sw.si[0].ival, testint);
            nfails++;
        }

        conf_clear(conf);
        settings_r sr = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_I,
            .si[0].ival = testint,
        };
        load_open_settings(&sr, conf);
        int loaded = conf_get_int(conf, confid);
        if (loaded != testint) {
            printf("fail test_int_simple(%s): "
                   "loaded integer = %d, expected %d\n",
                   saveid, loaded, testint);
            nfails++;
        }
    }
            
    conf_free(conf);
}

void test_int_translated_internal(
    int confid, const char *saveid, bool test_save, bool test_load,
    void (*load_prepare)(settings_r *), int defexp, va_list ap)
{
    Conf *conf = conf_new();

    do_defaults(NULL, conf);
    int defgot = conf_get_int(conf, confid);
    if (defgot != defexp) {
        printf("fail test_int_translated(%s): default = %d, expected %d\n",
               saveid, defgot, defexp);
        nfails++;
    }

    int confval = va_arg(ap, int);
    while (confval != -1) {
        int storageval = va_arg(ap, int);

        if (test_save) {
            settings_w sw = {
                .n = 1,
                .si[0].key = saveid,
                .si[0].type = SAVE_UNSET,
            };
            conf_set_int(conf, confid, confval);
            save_open_settings(&sw, conf);
            if (sw.si[0].type != SAVE_I) {
                printf("fail test_int_translated(%s): "
                       "saved type = %d, expected %d\n",
                       saveid, sw.si[0].type, SAVE_I);
                nfails++;
            } else if (sw.si[0].ival != storageval) {
                printf("fail test_int_translated(%s.%d.%d): "
                       "saved integer = %d, expected %d\n",
                       saveid, confval, storageval, sw.si[0].ival, storageval);
                nfails++;
            }
        }

        if (test_load) {
            conf_clear(conf);
            settings_r sr = {
                .n = 1,
                .si[0].key = saveid,
                .si[0].type = SAVE_I,
                .si[0].ival = storageval,
            };
            if (load_prepare)
                load_prepare(&sr);
            load_open_settings(&sr, conf);
            int loaded = conf_get_int(conf, confid);
            if (loaded != confval) {
                printf("fail test_int_translated(%s.%d.%d): "
                       "loaded integer = %d, expected %d\n",
                       saveid, confval, storageval, loaded, confval);
                nfails++;
            }
        }

        confval = va_arg(ap, int);
    }
            
    conf_free(conf);
}

void test_int_translated(int confid, const char *saveid, int defexp, ...)
{
    va_list ap;
    va_start(ap, defexp);
    test_int_translated_internal(confid, saveid, true, true, NULL, defexp, ap);
    va_end(ap);
}

void test_int_translated_load_legacy(
    int confid, const char *saveid, void (*load_prepare)(settings_r *),
    int defexp, ...)
{
    va_list ap;
    va_start(ap, defexp);
    test_int_translated_internal(confid, saveid, false, true, load_prepare,
                                 defexp, ap);
    va_end(ap);
}

void test_bool_simple(int confid, const char *saveid, bool defexp)
{
    Conf *conf = conf_new();

    do_defaults(NULL, conf);
    bool defgot = conf_get_bool(conf, confid);
    if (defgot != defexp) {
        printf("fail test_bool_simple(%s): default = %d, expected %d\n",
               saveid, defgot, defexp);
        nfails++;
    }

    for (int i = 0; i < 2; i++) {
        settings_w sw = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_UNSET,
        };
        static const bool testbools[] = { false, true };
        bool testbool = testbools[i];
        conf_set_bool(conf, confid, testbool);
        save_open_settings(&sw, conf);
        if (sw.si[0].type != SAVE_I) {
            printf("fail test_bool_simple(%s): saved type = %d, expected %d\n",
                   saveid, sw.si[0].type, SAVE_I);
            nfails++;
        } else if (sw.si[0].ival != testbool) {
            printf("fail test_bool_simple(%s): "
                   "saved integer = %d, expected %d\n",
                   saveid, sw.si[0].ival, testbool);
            nfails++;
        }

        conf_clear(conf);
        settings_r sr = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_I,
            .si[0].ival = testbool,
        };
        load_open_settings(&sr, conf);
        bool loaded = conf_get_bool(conf, confid);
        if (loaded != testbool) {
            printf("fail test_bool_simple(%s): "
                   "loaded boolean = %d, expected %d\n",
                   saveid, loaded, testbool);
            nfails++;
        }
    }
            
    conf_free(conf);
}

void test_file_simple(int confid, const char *saveid)
{
    Conf *conf = conf_new();

    do_defaults(NULL, conf);

    for (int i = 0; i < 2; i++) {
        settings_w sw = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_UNSET,
        };
        static const char *const teststrings[] = { "foo", "bar" };
        const char *teststring = teststrings[i];
        Filename *testfn = filename_from_str(teststring);
        conf_set_filename(conf, confid, testfn);
        filename_free(testfn);
        save_open_settings(&sw, conf);
        if (sw.si[0].type != SAVE_FILENAME) {
            printf("fail test_file_simple(%s): saved type = %d, expected %d\n",
                   saveid, sw.si[0].type, SAVE_FILENAME);
            nfails++;
        } else if (0 != strcmp(sw.si[0].sval, teststring)) {
            printf("fail test_file_simple(%s): "
                   "saved string = '%s', expected '%s'\n",
                   saveid, sw.si[0].sval, teststring);
            nfails++;
        }

        conf_clear(conf);
        settings_r sr = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_FILENAME,
        };
        snprintf(sr.si[0].sval, sizeof(sr.si[0].sval), "%s", teststring);
        load_open_settings(&sr, conf);
        const char *loaded = filename_to_str(conf_get_filename(conf, confid));
        if (0 != strcmp(loaded, teststring)) {
            printf("fail test_file_simple(%s): "
                   "loaded string = '%s', expected '%s'\n",
                   saveid, loaded, teststring);
            nfails++;
        }
    }
            
    conf_free(conf);
}

void test_font_simple(int confid, const char *saveid)
{
    Conf *conf = conf_new();

    do_defaults(NULL, conf);

    for (int i = 0; i < 2; i++) {
        settings_w sw = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_UNSET,
        };
        static const char *const teststrings[] = { "foo", "bar" };
        const char *teststring = teststrings[i];
        FontSpec *testfs = fontspec_new(teststring);
        conf_set_fontspec(conf, confid, testfs);
        fontspec_free(testfs);
        save_open_settings(&sw, conf);
        if (sw.si[0].type != SAVE_FONTSPEC) {
            printf("fail test_font_simple(%s): saved type = %d, expected %d\n",
                   saveid, sw.si[0].type, SAVE_FONTSPEC);
            nfails++;
        } else if (0 != strcmp(sw.si[0].sval, teststring)) {
            printf("fail test_font_simple(%s): "
                   "saved string = '%s', expected '%s'\n",
                   saveid, sw.si[0].sval, teststring);
            nfails++;
        }

        conf_clear(conf);
        settings_r sr = {
            .n = 1,
            .si[0].key = saveid,
            .si[0].type = SAVE_FONTSPEC,
        };
        snprintf(sr.si[0].sval, sizeof(sr.si[0].sval), "%s", teststring);
        load_open_settings(&sr, conf);
        const char *loaded = conf_get_fontspec(conf, confid)->name;
        if (0 != strcmp(loaded, teststring)) {
            printf("fail test_file_simple(%s): "
                   "loaded string = '%s', expected '%s'\n",
                   saveid, loaded, teststring);
            nfails++;
        }
    }

    conf_free(conf);
}

static void load_prepare_socks4(settings_r *sr)
{
    size_t pos = sr->n++;
    sr->si[pos].key = "ProxySOCKSVersion";
    sr->si[pos].type = SAVE_I;
    sr->si[pos].ival = 4;
}

void test_simple(void)
{
    test_str_simple(CONF_host, "HostName", "");
    test_int_translated(CONF_addressfamily, "AddressFamily", ADDRTYPE_UNSPEC,
                        ADDRTYPE_UNSPEC, 0, ADDRTYPE_IPV4, 1,
                        ADDRTYPE_IPV6, 2, -1);
    test_bool_simple(CONF_warn_on_close, "WarnOnClose", true);
    test_bool_simple(CONF_tcp_nodelay, "TCPNoDelay", true);
    /* cn change: TCP keepalives on by default (2b95c76 串口配置与终端体验改造). */
    test_bool_simple(CONF_tcp_keepalives, "TCPKeepalives", true);
    test_str_simple(CONF_loghost, "LogHost", "");
    test_str_simple(CONF_proxy_exclude_list, "ProxyExcludeList", "");
    test_bool_simple(CONF_even_proxy_localhost, "ProxyLocalhost", false);
    test_str_simple(CONF_proxy_host, "ProxyHost", "proxy");
    test_int_simple(CONF_proxy_port, "ProxyPort", 80);
    test_str_simple(CONF_proxy_username, "ProxyUsername", "");
    test_str_simple(CONF_proxy_password, "ProxyPassword", "");
    test_str_simple(CONF_proxy_telnet_command, "ProxyTelnetCommand", "connect %host %port\\n");
    test_int_translated(CONF_proxy_log_to_term, "ProxyLogToTerm", FORCE_OFF,
                        FORCE_ON, 0, FORCE_OFF, 1, AUTO, 2, -1);
    test_str_ambi_simple(CONF_remote_cmd, "RemoteCommand", "", false);
    test_bool_simple(CONF_nopty, "NoPTY", false);
    test_bool_simple(CONF_compression, "Compression", false);
    test_bool_simple(CONF_ssh_prefer_known_hostkeys, "PreferKnownHostKeys", true);
    test_int_simple(CONF_ssh_rekey_time, "RekeyTime", 60);
    test_str_simple(CONF_ssh_rekey_data, "RekeyBytes", "1G");
    test_bool_simple(CONF_tryagent, "TryAgent", true);
    test_bool_simple(CONF_agentfwd, "AgentFwd", false);
    test_bool_simple(CONF_change_username, "ChangeUsername", false);
    test_file_simple(CONF_keyfile, "PublicKeyFile");
    test_file_simple(CONF_detached_cert, "DetachedCertificate");
    test_str_simple(CONF_auth_plugin, "AuthPlugin", "");
    test_bool_simple(CONF_ssh2_des_cbc, "SSH2DES", false);
    test_bool_simple(CONF_ssh_no_userauth, "SshNoAuth", false);
    test_bool_simple(CONF_ssh_no_trivial_userauth, "SshNoTrivialAuth", false);
    test_bool_simple(CONF_ssh_show_banner, "SshBanner", true);
    test_bool_simple(CONF_try_tis_auth, "AuthTIS", false);
    test_bool_simple(CONF_try_ki_auth, "AuthKI", true);
    test_bool_simple(CONF_ssh_no_shell, "SshNoShell", false);
    test_str_simple(CONF_termtype, "TerminalType", "xterm");
    test_str_simple(CONF_termspeed, "TerminalSpeed", "38400,38400");
    test_str_ambi_simple(CONF_username, "UserName", "", false);
    test_bool_simple(CONF_username_from_env, "UserNameFromEnvironment", false);
    test_bool_simple(CONF_rfc_environ, "RFCEnviron", false);
    test_bool_simple(CONF_passive_telnet, "PassiveTelnet", false);
    test_str_simple(CONF_serline, "SerialLine", "");
    test_int_simple(CONF_serspeed, "SerialSpeed", 9600);
    test_int_simple(CONF_serdatabits, "SerialDataBits", 8);
    test_int_simple(CONF_serstopbits, "SerialStopHalfbits", 2);
    test_int_translated(CONF_serparity, "SerialParity", SER_PAR_NONE,
                        SER_PAR_NONE, 0, SER_PAR_ODD, 1, SER_PAR_EVEN, 2,
                        SER_PAR_MARK, 3, SER_PAR_SPACE, 4, -1);
    /* cn change: no flow control by default (华为/华三 Console 标准). */
    test_int_translated(CONF_serflow, "SerialFlowControl", SER_FLOW_NONE,
                        SER_FLOW_NONE, 0, SER_FLOW_XONXOFF, 1,
                        SER_FLOW_RTSCTS, 2, SER_FLOW_DSRDTR, 3, -1);
    test_bool_simple(CONF_bksp_is_delete, "BackspaceIsDelete", true);
    test_bool_simple(CONF_rxvt_homeend, "RXVTHomeEnd", false);
    /* cn change: xterm-style function keys by default. */
    test_int_translated(CONF_funky_type, "LinuxFunctionKeys", FUNKY_XTERM,
                        FUNKY_TILDE, 0, FUNKY_LINUX, 1, FUNKY_XTERM, 2,
                        FUNKY_VT400, 3, FUNKY_VT100P, 4, FUNKY_SCO, 5,
                        FUNKY_XTERM_216, 6, -1);
    test_int_translated(CONF_sharrow_type, "ShiftedArrowKeys",
                        SHARROW_APPLICATION,
                        SHARROW_APPLICATION, 0, SHARROW_BITMAP, 1, -1);
    test_bool_simple(CONF_no_applic_c, "NoApplicationCursors", false);
    /* cn change: application keys disabled by default. */
    test_bool_simple(CONF_no_applic_k, "NoApplicationKeys", true);
    test_bool_simple(CONF_no_mouse_rep, "NoMouseReporting", false);
    test_bool_simple(CONF_no_remote_resize, "NoRemoteResize", false);
    test_bool_simple(CONF_no_alt_screen, "NoAltScreen", false);
    /* cn: 默认 true——标题栏恒为「主机名/IP：端口号（在线/已离线）」，
     * 云服务器 bash 的 user@host:~ 转义序列不得覆盖。 */
    test_bool_simple(CONF_no_remote_wintitle, "NoRemoteWinTitle", true);
    test_bool_simple(CONF_no_remote_clearscroll, "NoRemoteClearScroll", false);
    test_bool_simple(CONF_no_dbackspace, "NoDBackspace", false);
    test_bool_simple(CONF_no_remote_charset, "NoRemoteCharset", false);
    /* note we have no test for CONF_remote_qtitle_action because no default */
    test_bool_simple(CONF_app_cursor, "ApplicationCursorKeys", false);
    test_bool_simple(CONF_app_keypad, "ApplicationKeypad", false);
    test_bool_simple(CONF_nethack_keypad, "NetHackKeypad", false);
    test_bool_simple(CONF_telnet_keyboard, "TelnetKey", false);
    test_bool_simple(CONF_telnet_newline, "TelnetRet", true);
    test_bool_simple(CONF_alt_f4, "AltF4", true);
    test_bool_simple(CONF_alt_space, "AltSpace", false);
    test_bool_simple(CONF_alt_only, "AltOnly", false);
    test_int_translated(CONF_localecho, "LocalEcho", AUTO,
                        FORCE_ON, 0, FORCE_OFF, 1, AUTO, 2, -1);
    test_int_translated(CONF_localedit, "LocalEdit", AUTO,
                        FORCE_ON, 0, FORCE_OFF, 1, AUTO, 2, -1);
    test_bool_simple(CONF_alwaysontop, "AlwaysOnTop", false);
    test_bool_simple(CONF_fullscreenonaltenter, "FullScreenOnAltEnter", false);
    test_bool_simple(CONF_scroll_on_key, "ScrollOnKey", false);
    test_bool_simple(CONF_scroll_on_disp, "ScrollOnDisp", true);
    test_bool_simple(CONF_erase_to_scrollback, "EraseToScrollback", true);
    test_bool_simple(CONF_compose_key, "ComposeKey", false);
    test_bool_simple(CONF_ctrlaltkeys, "CtrlAltKeys", true);
    test_str_simple(CONF_wintitle, "WinTitle", "");
    /* cn change: larger scrollback for device consoles. */
    test_int_simple(CONF_savelines, "ScrollbackLines", 9999);
    test_bool_simple(CONF_dec_om, "DECOriginMode", false);
    test_bool_simple(CONF_wrap_mode, "AutoWrapMode", true);
    test_bool_simple(CONF_lfhascr, "LFImpliesCR", false);
    test_int_translated(CONF_cursor_type, "CurType", CURSOR_BLOCK,
                        CURSOR_BLOCK, 0, CURSOR_UNDERLINE, 1,
                        CURSOR_VERTICAL_LINE, 2, -1);
    test_bool_simple(CONF_blink_cur, "BlinkCur", false);
    test_int_translated(CONF_beep, "Beep", 1,
                        BELL_DISABLED, 0, BELL_DEFAULT, 1, BELL_VISUAL, 2,
                        BELL_WAVEFILE, 3, BELL_PCSPEAKER, 4, -1);
    test_int_translated(CONF_beep_ind, "BeepInd", 0,
                        B_IND_DISABLED, 0, B_IND_FLASH, 1, B_IND_STEADY, 2, -1);
    test_bool_simple(CONF_bellovl, "BellOverload", true);
    test_int_simple(CONF_bellovl_n, "BellOverloadN", 5);
    test_file_simple(CONF_bell_wavefile, "BellWaveFile");
    test_bool_simple(CONF_scrollbar, "ScrollBar", true);
    test_bool_simple(CONF_scrollbar_in_fullscreen, "ScrollBarFullScreen", false);
    test_int_translated(CONF_resize_action, "LockSize", RESIZE_TERM,
                        RESIZE_TERM, 0, RESIZE_DISABLED, 1, RESIZE_FONT, 2,
                        RESIZE_EITHER, 3, -1);
    test_bool_simple(CONF_bce, "BCE", true);
    test_bool_simple(CONF_blinktext, "BlinkText", false);
    test_bool_simple(CONF_win_name_always, "WinNameAlways", true);
    test_int_simple(CONF_width, "TermWidth", 80);
    test_int_simple(CONF_height, "TermHeight", 24);
    test_font_simple(CONF_font, "Font");
    test_int_translated(CONF_font_quality, "FontQuality", FQ_DEFAULT,
                        FQ_DEFAULT, 0, FQ_ANTIALIASED, 1, FQ_NONANTIALIASED, 2,
                        FQ_CLEARTYPE, 3, -1);
    test_bool_simple(CONF_font_varpitch, "FontVarPitch", false);
    test_file_simple(CONF_logfilename, "LogFileName");
    test_int_translated(CONF_logtype, "LogType", LGTYP_NONE,
                        LGTYP_NONE, 0, LGTYP_ASCII, 1, LGTYP_DEBUG, 2,
                        LGTYP_PACKETS, 3, LGTYP_SSHRAW, 4, -1);
    /* FIXME: this won't work because -1 is also the terminator, darn */
    test_int_translated(CONF_logxfovr, "LogFileClash", LGXF_ASK,
                        LGXF_OVR, 1, LGXF_APN, 0, LGXF_ASK, -1, -1);
    test_bool_simple(CONF_logflush, "LogFlush", true);
    test_bool_simple(CONF_logheader, "LogHeader", true);
    test_bool_simple(CONF_logomitpass, "SSHLogOmitPasswords", true);
    test_bool_simple(CONF_logomitdata, "SSHLogOmitData", false);
    test_bool_simple(CONF_hide_mouseptr, "HideMousePtr", false);
    test_bool_simple(CONF_sunken_edge, "SunkenEdge", false);
    test_int_simple(CONF_window_border, "WindowBorder", 1);
    test_str_simple(CONF_answerback, "Answerback", "PuTTY");
    test_str_simple(CONF_printer, "Printer", "");
    test_bool_simple(CONF_no_arabicshaping, "DisableArabicShaping", false);
    test_bool_simple(CONF_no_bidi, "DisableBidi", false);
    test_bool_simple(CONF_ansi_colour, "ANSIColour", true);
    test_bool_simple(CONF_xterm_256_colour, "Xterm256Colour", true);
    test_bool_simple(CONF_true_colour, "TrueColour", true);
    test_bool_simple(CONF_system_colour, "UseSystemColours", false);
    test_bool_simple(CONF_try_palette, "TryPalette", false);
    test_int_translated(CONF_mouse_is_xterm, "MouseIsXterm", 0,
                        MOUSE_COMPROMISE, 0, MOUSE_XTERM, 1,
                        MOUSE_WINDOWS, 2, -1);
    test_bool_simple(CONF_rect_select, "RectSelect", false);
    test_bool_simple(CONF_paste_controls, "PasteControls", false);
    test_bool_simple(CONF_rawcnp, "RawCNP", false);
    test_bool_simple(CONF_utf8linedraw, "UTF8linedraw", false);
    test_bool_simple(CONF_rtf_paste, "PasteRTF", false);
    test_bool_simple(CONF_mouse_override, "MouseOverride", true);
    test_bool_simple(CONF_mouseautocopy, "MouseAutocopy", CLIPUI_DEFAULT_AUTOCOPY);
    test_int_translated(CONF_vtmode, "FontVTMode", VT_UNICODE,
                        VT_XWINDOWS, 0,
                        VT_OEMANSI, 1,
                        VT_OEMONLY, 2,
                        VT_POORMAN, 3,
                        VT_UNICODE, 4,
                        -1);
    test_str_simple(CONF_line_codepage, "LineCodePage", "");
    test_bool_simple(CONF_cjk_ambig_wide, "CJKAmbigWide", false);
    test_bool_simple(CONF_utf8_override, "UTF8Override", true);
    test_bool_simple(CONF_xlat_capslockcyr, "CapsLockCyr", false);
    test_bool_simple(CONF_x11_forward, "X11Forward", false);
    test_str_simple(CONF_x11_display, "X11Display", "");
    test_int_translated(CONF_x11_auth, "X11AuthType", X11_MIT,
                        X11_NO_AUTH, 0, X11_MIT, 1, X11_XDM, 2, -1);
    test_file_simple(CONF_xauthfile, "X11AuthFile");
    test_bool_simple(CONF_lport_acceptall, "LocalPortAcceptAll", false);
    test_bool_simple(CONF_rport_acceptall, "RemotePortAcceptAll", false);
    test_bool_simple(CONF_ssh_connection_sharing, "ConnectionSharing", false);
    test_bool_simple(CONF_ssh_connection_sharing_upstream, "ConnectionSharingUpstream", true);
    test_bool_simple(CONF_ssh_connection_sharing_downstream, "ConnectionSharingDownstream", true);
    test_bool_simple(CONF_stamp_utmp, "StampUtmp", true);
    test_bool_simple(CONF_login_shell, "LoginShell", true);
    test_bool_simple(CONF_scrollbar_on_left, "ScrollbarOnLeft", false);
    test_bool_simple(CONF_shadowbold, "ShadowBold", false);
    test_font_simple(CONF_boldfont, "BoldFont");
    test_font_simple(CONF_widefont, "WideFont");
    test_font_simple(CONF_wideboldfont, "WideBoldFont");
    test_int_simple(CONF_shadowboldoffset, "ShadowBoldOffset", 1);
    test_bool_simple(CONF_crhaslf, "CRImpliesLF", false);
    test_str_simple(CONF_winclass, "WindowClass", "");
    test_int_translated(CONF_close_on_exit, "CloseOnExit", AUTO,
                        FORCE_OFF, 0, AUTO, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_proxy_dns, "ProxyDNS", AUTO,
                        FORCE_OFF, 0, AUTO, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_bold_style, "BoldAsColour", AUTO,
                        1, 0, 2, 1, 3, 2, -1);
    test_int_translated(CONF_sshbug_ignore1, "BugIgnore1", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_plainpw1, "BugPlainPW1", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_rsa1, "BugRSA1", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_ignore2, "BugIgnore2", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_derivekey2, "BugDeriveKey2", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_rsapad2, "BugRSAPad2", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_pksessid2, "BugPKSessID2", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_rekey2, "BugRekey2", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_maxpkt2, "BugMaxPkt2", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_oldgex2, "BugOldGex2", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_winadj, "BugWinadj", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_chanreq, "BugChanReq", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_dropstart, "BugDropStart", FORCE_OFF,
                        FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_filter_kexinit, "BugFilterKexinit", FORCE_OFF,
                        FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_sshbug_rsa_sha2_cert_userauth, "BugRSASHA2CertUserauth", AUTO,
                        AUTO, 0, FORCE_OFF, 1, FORCE_ON, 2, -1);
    test_int_translated(CONF_proxy_type, "ProxyMethod", PROXY_NONE,
                        PROXY_NONE, 0, PROXY_SOCKS4, 1, PROXY_SOCKS5, 2,
                        PROXY_HTTP, 3, PROXY_TELNET, 4, PROXY_CMD, 5,
                        PROXY_SSH_TCPIP, 6, PROXY_SSH_EXEC, 7,
                        PROXY_SSH_SUBSYSTEM, 8, -1);
    test_int_translated_load_legacy(
        CONF_proxy_type, "ProxyType", NULL, PROXY_NONE,
        PROXY_HTTP, 1, PROXY_SOCKS5, 2, PROXY_TELNET, 3, PROXY_CMD, 4, -1);
    test_int_translated_load_legacy(
        CONF_proxy_type, "ProxyType", load_prepare_socks4, PROXY_NONE,
        PROXY_HTTP, 1, PROXY_SOCKS4, 2, PROXY_TELNET, 3, PROXY_CMD, 4, -1);
    test_int_translated(CONF_remote_qtitle_action, "RemoteQTitleAction", TITLE_EMPTY,
                        TITLE_NONE, 0, TITLE_EMPTY, 1, TITLE_REAL, 2, -1);
    test_int_translated_load_legacy(
        CONF_remote_qtitle_action, "NoRemoteQTitle", NULL, TITLE_EMPTY,
        TITLE_REAL, 0, TITLE_EMPTY, 1, -1);
}

void test_conf_key_info(void)
{
    struct test_data {
        const char *name;
        bool got_value_type : 1;
        bool got_subkey_type : 1;
        bool got_default : 1;
        bool got_default_int : 1;
        bool got_default_str : 1;
        bool got_default_bool : 1;
        bool got_save_keyword : 1;
        bool got_storage_enum : 1;
        bool save_custom : 1;
        bool load_custom : 1;
        bool not_saved : 1;
    };

#define CONF_OPTION(id, ...) { .name = "CONF_" #id, __VA_ARGS__ },
#define VALUE_TYPE(x) .got_value_type = true
#define SUBKEY_TYPE(x) .got_subkey_type = true
#define DEFAULT_INT(x) .got_default_int = true, .got_default = true
#define DEFAULT_STR(x) .got_default_str = true, .got_default = true
#define DEFAULT_BOOL(x) .got_default_bool = true, .got_default = true
#define SAVE_KEYWORD(x) .got_save_keyword = true
#define STORAGE_ENUM(x) .got_storage_enum = true
#define SAVE_CUSTOM .save_custom = true
#define LOAD_CUSTOM .load_custom = true
#define NOT_SAVED .not_saved = true

    static const struct test_data conf_key_test_data[] = {
        #include "conf.h"
    };

    for (size_t key = 0; key < N_CONFIG_OPTIONS; key++) {
        const ConfKeyInfo *info = &conf_key_info[key];
        const struct test_data *td = &conf_key_test_data[key];

        if (!td->got_value_type) {
            fprintf(stderr, "%s: no value type\n", td->name);
            nfails++;
        }

        if (td->got_default && info->subkey_type != CONF_TYPE_NONE) {
            fprintf(stderr, "%s: is a mapping but has a default\n", td->name);
            nfails++;
        }

        if ((td->got_default_int && info->value_type != CONF_TYPE_INT) ||
            (td->got_default_str &&
             (info->value_type != CONF_TYPE_STR &&
              info->value_type != CONF_TYPE_STR_AMBI &&
              info->value_type != CONF_TYPE_UTF8)) ||
            (td->got_default_bool && info->value_type != CONF_TYPE_BOOL)) {
            fprintf(stderr, "%s: default doesn't match type\n", td->name);
            nfails++;
        }

        if (td->got_storage_enum && info->value_type != CONF_TYPE_INT) {
            fprintf(stderr, "%s: has STORAGE_ENUM but isn't an int\n",
                    td->name);
            nfails++;
        }

        if (td->not_saved) {
            if (!td->got_default && info->subkey_type == CONF_TYPE_NONE) {
                fprintf(stderr, "%s: simple unsaved setting but has no "
                        "default\n", td->name);
                nfails++;
            }

            if (td->got_save_keyword) {
                fprintf(stderr, "%s: not saved but has SAVE_KEYWORD\n",
                        td->name);
                nfails++;
            }

            if (td->save_custom) {
                fprintf(stderr, "%s: not saved but has SAVE_CUSTOM\n",
                        td->name);
                nfails++;
            }

            if (td->load_custom) {
                fprintf(stderr, "%s: not saved but has LOAD_CUSTOM\n",
                        td->name);
                nfails++;
            }

            if (td->got_storage_enum) {
                fprintf(stderr, "%s: not saved but has STORAGE_ENUM\n",
                        td->name);
                nfails++;
            }

        } else {
            if (td->load_custom && td->save_custom) {
                if (td->got_save_keyword) {
                    fprintf(stderr, "%s: no automatic save or load but has "
                            "SAVE_KEYWORD\n", td->name);
                    nfails++;
                }

                if (td->got_storage_enum) {
                    fprintf(stderr, "%s: no automatic save or load but has "
                            "STORAGE_ENUM\n", td->name);
                    nfails++;
                }
            } else {
                if (!td->got_save_keyword) {
                    fprintf(stderr, "%s: missing SAVE_KEYWORD\n", td->name);
                    nfails++;
                }
            }
        }
    }
}

/*
 * cn: 前置声明 —— 这两个辅助函数的定义放在 check_changelist 之后
 * （它们内部用到它的兄弟函数），但调用点在 test_changelist 里，
 * 位置在定义之前。C 要求先声明后使用。
 */
static void check_changelist_struct(const char *what,
                                    void (*apply)(Conf *),
                                    const char *expect_name,
                                    const char *expect_old,
                                    const char *expect_new,
                                    CnChangeKind expect_kind);
static void check_struct_matches_text(const char *what,
                                      void (*apply)(Conf *));

/*
 * cn: check that conf_describe_changes() names each changed option the
 * way the configuration UI actually labels it, and shows the option
 * text rather than a bare 开/关 or enum number for radio-button
 * settings. Each case flips one setting from the baseline and asserts
 * the exact line the save-confirmation prompt will display.
 */
static void check_changelist(const char *what,
                             void (*apply)(Conf *), const char *expect)
{
    Conf *base = conf_new();
    Conf *cur = conf_new();
    do_defaults(NULL, base);
    /*
     * Copy rather than call do_defaults() twice: the platform default
     * font specs are freshly allocated each time, so two independent
     * defaults compare unequal and would show up as spurious font
     * changes in the list.
     */
    conf_copy_into(cur, base);
    apply(cur);
    char *got = conf_describe_changes(base, cur);
    if (strcmp(got, expect) != 0) {
        fprintf(stderr, "fail changelist(%s):\n  got:    %s  expect: %s",
                what, got, expect);
        nfails++;
    }
    sfree(got);
    conf_free(base);
    conf_free(cur);
}

static void cl_bksp(Conf *c) { conf_set_bool(c, CONF_bksp_is_delete, false); }
static void cl_homeend(Conf *c) { conf_set_bool(c, CONF_rxvt_homeend, true); }
static void cl_sharrow(Conf *c)
{ conf_set_int(c, CONF_sharrow_type, SHARROW_BITMAP); }
static void cl_appcursor(Conf *c) { conf_set_bool(c, CONF_app_cursor, true); }
static void cl_appkeypad(Conf *c) { conf_set_bool(c, CONF_app_keypad, true); }
static void cl_nethack(Conf *c)
{ conf_set_bool(c, CONF_nethack_keypad, true); }
/* cn: 纯字符串改动，用于验证「非开关类一律打勾」的分类。 */
static void cl_title(Conf *c) { conf_set_str(c, CONF_wintitle, "新标题"); }
static void cl_localecho(Conf *c)
{ conf_set_int(c, CONF_localecho, FORCE_ON); }
static void cl_proxydns(Conf *c) { conf_set_int(c, CONF_proxy_dns, FORCE_ON); }
static void cl_localedit(Conf *c)
{ conf_set_int(c, CONF_localedit, FORCE_OFF); }
static void cl_proxylog(Conf *c)
{ conf_set_int(c, CONF_proxy_log_to_term, AUTO); }
static void cl_sshprot(Conf *c) { conf_set_int(c, CONF_sshprot, 0); }
static void cl_proxysshsub(Conf *c)
{ conf_set_int(c, CONF_proxy_type, PROXY_SSH_SUBSYSTEM); }
static void cl_proxysshexec(Conf *c)
{ conf_set_int(c, CONF_proxy_type, PROXY_SSH_EXEC); }
static void cl_proxysshport(Conf *c)
{ conf_set_int(c, CONF_proxy_type, PROXY_SSH_TCPIP); }
static void cl_proxyhttp(Conf *c)
{ conf_set_int(c, CONF_proxy_type, PROXY_HTTP); }
static void cl_proxytelnet(Conf *c)
{ conf_set_int(c, CONF_proxy_type, PROXY_TELNET); }
static void cl_funky(Conf *c) { conf_set_int(c, CONF_funky_type, FUNKY_LINUX); }
static void cl_rectsel(Conf *c) { conf_set_bool(c, CONF_rect_select, true); }
static void cl_rfcenv(Conf *c) { conf_set_bool(c, CONF_rfc_environ, true); }
static void cl_passive(Conf *c) { conf_set_bool(c, CONF_passive_telnet, true); }
static void cl_usrenv(Conf *c)
{ conf_set_bool(c, CONF_username_from_env, true); }
static void cl_beep(Conf *c) { conf_set_int(c, CONF_beep, BELL_VISUAL); }
static void cl_beepwave(Conf *c)
{ conf_set_int(c, CONF_beep, BELL_WAVEFILE); }
static void cl_beepspk(Conf *c)
{ conf_set_int(c, CONF_beep, BELL_PCSPEAKER); }
static void cl_serparity_mark(Conf *c)
{ conf_set_int(c, CONF_serparity, SER_PAR_MARK); }
static void cl_cursor(Conf *c)
{ conf_set_int(c, CONF_cursor_type, CURSOR_VERTICAL_LINE); }
static void cl_vtmode(Conf *c) { conf_set_int(c, CONF_vtmode, VT_POORMAN); }
static void cl_vtmode_xwin(Conf *c)
{ conf_set_int(c, CONF_vtmode, VT_XWINDOWS); }
static void cl_vtmode_oemansi(Conf *c)
{ conf_set_int(c, CONF_vtmode, VT_OEMANSI); }
static void cl_vtmode_oemonly(Conf *c)
{ conf_set_int(c, CONF_vtmode, VT_OEMONLY); }
static void cl_logxfovr(Conf *c) { conf_set_int(c, CONF_logxfovr, LGXF_APN); }
static void cl_qtitle(Conf *c)
{ conf_set_int(c, CONF_remote_qtitle_action, TITLE_REAL); }
static void cl_bold(Conf *c)
{ conf_set_int(c, CONF_bold_style, BOLD_STYLE_FONT | BOLD_STYLE_COLOUR); }
static void cl_x11auth(Conf *c) { conf_set_int(c, CONF_x11_auth, X11_XDM); }
static void cl_varpitch(Conf *c)
{ conf_set_bool(c, CONF_font_varpitch, true); }
/*
 * cn: The three clipboard-action dropdowns. Their options are stored
 * with LOAD_CUSTOM/SAVE_CUSTOM, which used to make conf_equal and
 * conf_describe_changes skip them entirely — changing one left the
 * Save button grey and the change list empty.
 */
static void cl_mousepaste(Conf *c)
{ conf_set_int(c, CONF_mousepaste, CLIPUI_NONE); }
static void cl_ctrlshiftins(Conf *c)
{ conf_set_int(c, CONF_ctrlshiftins, CLIPUI_IMPLICIT); }
static void cl_ctrlshiftcv(Conf *c)
{ conf_set_int(c, CONF_ctrlshiftcv, CLIPUI_EXPLICIT); }
static void cl_rtfpaste(Conf *c)
{ conf_set_bool(c, CONF_rtf_paste, true); }
static void cl_mouseisxterm(Conf *c)
{ conf_set_int(c, CONF_mouse_is_xterm, MOUSE_XTERM); }
static void cl_sunken(Conf *c)
{ conf_set_bool(c, CONF_sunken_edge, true); }
static void cl_altonly(Conf *c)
{ conf_set_bool(c, CONF_alt_only, true); }
static void cl_resize(Conf *c)
{ conf_set_int(c, CONF_resize_action, RESIZE_FONT); }
static void cl_composekey(Conf *c)
{ conf_set_bool(c, CONF_compose_key, true); }
static void cl_ctrlalt(Conf *c)
{ conf_set_bool(c, CONF_ctrlaltkeys, false); }

void test_changelist(void)
{
    /* The five the user reported. */
    check_changelist("bksp_is_delete", cl_bksp,
                     "【Backspace 回退键】 Control-? (127) → Control-H\r\n");
    check_changelist("rxvt_homeend", cl_homeend,
                     "【Home 和 End 键】 标准 → rxvt\r\n");
    check_changelist("sharrow_type", cl_sharrow,
                     "【Shift/Ctrl/Alt 配合方向键】 Ctrl 切换应用模式"
                     " → xterm 风格位图\r\n");
    check_changelist("app_cursor", cl_appcursor,
                     "【光标键初始状态】 常规 → 应用\r\n");
    check_changelist("app_keypad", cl_appkeypad,
                     "【数字小键盘初始状态】 常规 → 应用\r\n");
    /* The third state must show as NetHack, not a second 开/关 line. */
    check_changelist("nethack_keypad", cl_nethack,
                     "【数字小键盘初始状态】 常规 → NetHack\r\n");
    /* Remaining bool radios and the shared enums spelled per control. */
    check_changelist("localecho", cl_localecho,
                     "【本地回应】 自动 → 强制开\r\n");
    check_changelist("localedit", cl_localedit,
                     "【本地行编辑】 自动 → 强制关\r\n");
    check_changelist("proxy_dns", cl_proxydns,
                     "【在代理端进行 DNS 域名解析】 自动 → 是\r\n");
    /*
     * Proxy method values: the table used to carry the raw English
     * names (SSH subsystem, HTTP, Telnet) instead of the wording the
     * drop-down actually shows.
     */
    check_changelist("proxy_type_sshsub", cl_proxysshsub,
                     "【代理类型】 无 → 通过 SSH 代理调用子系统\r\n");
    check_changelist("proxy_type_sshexec", cl_proxysshexec,
                     "【代理类型】 无 → 通过 SSH 代理执行命令\r\n");
    check_changelist("proxy_type_sshport", cl_proxysshport,
                     "【代理类型】 无 → 通过 SSH 代理使用端口转发\r\n");
    check_changelist("proxy_type_http", cl_proxyhttp,
                     "【代理类型】 无 → HTTP 连接\r\n");
    check_changelist("proxy_type_telnet", cl_proxytelnet,
                     "【代理类型】 无 → Telnet（发送临时命令）\r\n");
    check_changelist("proxy_log_to_term", cl_proxylog,
                     "【在终端窗口输出代理诊断信息】 否 → 仅限会话开始前\r\n");
    check_changelist("sshprot", cl_sshprot,
                     "【首选的 SSH 协议版本】 2 → 1 (不安全)\r\n");
    check_changelist("rect_select", cl_rectsel,
                     "【默认选择模式(Alt 换档键拖放为另一种模式)】 常规 → 矩形框\r\n");
    check_changelist("rfc_environ", cl_rfcenv,
                     "【OLD_ENVIRON 歧义处理】 BSD (常用) → RFC 1408 (不常用)\r\n");
    check_changelist("passive_telnet", cl_passive,
                     "【Telnet 通讯模式】 主动 → 被动\r\n");
    check_changelist("username_from_env", cl_usrenv,
                     "【未指定用户名时】 提示 → 使用系统用户名\r\n");
    /* Enum tables realigned to the on-screen wording. */
    check_changelist("beep", cl_beep,
                     "【发生响铃时动作】 使用系统默认警告声音"
                     " → 可视响铃 (闪动窗口)\r\n");
    /*
     * The two Windows-only bell choices, whose value names used to
     * read 波形文件 / 机箱喇叭 instead of the radio labels.
     */
    check_changelist("beep_wavefile", cl_beepwave,
                     "【发生响铃时动作】 使用系统默认警告声音"
                     " → 播放指定的声音文件\r\n");
    check_changelist("beep_pcspeaker", cl_beepspk,
                     "【发生响铃时动作】 使用系统默认警告声音"
                     " → 使用 PC 扬声器鸣叫\r\n");
    check_changelist("serparity_mark", cl_serparity_mark,
                     "【校验位】 无 → 始终为 1\r\n");
    check_changelist("cursor_type", cl_cursor,
                     "【光标显示】 显示块 → 垂直线\r\n");
    check_changelist("vtmode", cl_vtmode,
                     "【线条绘制字符处理】 使用 Unicode 线条绘制码位"
                     " → 简单线条绘制（+、- 和 |）\r\n");
    /* Every remaining vtmode value, so a typo in the table shows up. */
    check_changelist("vtmode_xwin", cl_vtmode_xwin,
                     "【线条绘制字符处理】 使用 Unicode 线条绘制码位"
                     " → 字体使用 X 窗口系统编码\r\n");
    check_changelist("vtmode_oemansi", cl_vtmode_oemansi,
                     "【线条绘制字符处理】 使用 Unicode 线条绘制码位"
                     " → ANSI 和 OEM 模式下都使用字体\r\n");
    check_changelist("vtmode_oemonly", cl_vtmode_oemonly,
                     "【线条绘制字符处理】 使用 Unicode 线条绘制码位"
                     " → 仅在 OEM 模式下使用字体\r\n");
    check_changelist("logxfovr", cl_logxfovr,
                     "【要记录的日志文件已存在时】 每次询问 → 总是添加到末尾\r\n");
    check_changelist("remote_qtitle_action", cl_qtitle,
                     "【远程标题查询回应(SECURITY)】 空字符串 → 窗口标题\r\n");
    check_changelist("bold_style", cl_bold,
                     "【粗体文字的表示方式】 颜色 → 两者\r\n");
    check_changelist("x11_auth", cl_x11auth,
                     "【远程 X11 认证协议】 MIT-Magic-Cookie-1"
                     " → XDM-Authorization-1\r\n");
    check_changelist("funky_type", cl_funky,
                     "【Fn 功能键和小键盘】 Xterm R6 → Linux\r\n");
    /*
     * cn: promoted from a dialog-local flag to a real setting, so that
     * toggling it shows up here and can actually be saved.
     */
    check_changelist("font_varpitch", cl_varpitch,
                     "【启用可变宽度字体】 关 → 开\r\n");
    /*
     * cn: Defaults are 系统剪贴板 / 系统剪贴板 / 无动作 on Windows
     * (CLIPUI_DEFAULT_MOUSE and CLIPUI_DEFAULT_INS are both
     * CLIPUI_EXPLICIT, and CtrlShiftCV defaults to CLIPUI_NONE).
     */
    check_changelist("mousepaste", cl_mousepaste,
                     "【鼠标粘贴动作】 系统剪贴板 → 无动作\r\n");
    check_changelist("ctrlshiftins", cl_ctrlshiftins,
                     "【{Ctrl,Shift} + Ins】 系统剪贴板 → 上次选择的文本\r\n");
    check_changelist("ctrlshiftcv", cl_ctrlshiftcv,
                     "【Ctrl + Shift + {C,V}】 无动作 → 系统剪贴板\r\n");
    /*
     * Windows-only controls that also had no Chinese name, so the
     * change list fell back to the raw save keyword.
     */
    check_changelist("rtf_paste", cl_rtfpaste,
                     "【以 RTF 和纯文本格式复制到剪贴板】 关 → 开\r\n");
    check_changelist("mouse_is_xterm", cl_mouseisxterm,
                     "【鼠标按键动作】 混合模式 (中键扩展，右键粘贴)"
                     " → xterm (右键扩展，中键粘贴)\r\n");
    check_changelist("sunken_edge", cl_sunken,
                     "【下沉边框（略厚）】 关 → 开\r\n");
    check_changelist("alt_only", cl_altonly,
                     "【Alt 键显示系统菜单】 关 → 开\r\n");
    check_changelist("resize_action", cl_resize,
                     "【当窗口大小被改变时】 改变行列数 → 改变字体大小\r\n");
    check_changelist("compose_key", cl_composekey,
                     "【AltGr 用作 Compose 键】 关 → 开\r\n");
    check_changelist("ctrlaltkeys", cl_ctrlalt,
                     "【Ctrl+Alt 键与 AltGr 不同】 开 → 关\r\n");

    /*
     * cn: 结构化清单的分段与勾叉分类。
     *
     * 分类规则（主人指定）：新值「开/启用」→ 打勾，「关/禁用」→ 打叉，
     * 字符串与数字等非开关改动一律打勾。这里逐个钉住三种情形，
     * 界面画错勾叉时能立刻定位是分类错还是绘制错。
     *
     * ⚠️ bksp 的新值是「Control-H」这种字符串，不是「开」——
     *    它归 CN_CHG_OTHER（打勾）。要验「开」的判定得用真正把
     *    bool 写成 开/关 的选项（ctrlalt 的反向即 开→关）。
     */
    check_changelist_struct("bksp 字符串值", cl_bksp,
                            "Backspace 回退键", "Control-? (127)",
                            "Control-H", CN_CHG_OTHER);
    check_changelist_struct("ctrlalt 开→关", cl_ctrlalt,
                            "Ctrl+Alt 键与 AltGr 不同", "开", "关",
                            CN_CHG_TOGGLE_OFF);
    check_changelist_struct("enum 非开关", cl_sharrow,
                            "Shift/Ctrl/Alt 配合方向键", "Ctrl 切换应用模式",
                            "xterm 风格位图", CN_CHG_OTHER);
    check_changelist_struct("net hack 三态", cl_nethack,
                            "数字小键盘初始状态", "常规", "NetHack",
                            CN_CHG_OTHER);
    /* wintitle 默认是空串，所以旧值显示 (空)。 */
    check_changelist_struct("字符串改动", cl_title,
                            "窗口标题", "(空)", "新标题", CN_CHG_OTHER);
    /* 文本版必须与结构版逐行对应，不能各改一边。 */
    check_struct_matches_text("bksp", cl_bksp);
    check_struct_matches_text("proxy_type", cl_proxyhttp);
    check_struct_matches_text("resize_action", cl_resize);
    check_struct_matches_text("title", cl_title);
}

/*
 * cn: 验证结构化改动清单（供自绘弹窗分段上色用）。
 *
 * 重点锁两件事：
 *   1) 分段正确 —— 选项名、旧值、新值分别落在各自字段，不是从
 *      纯字符串里解析出来的（值本身含「】」或「→」时会解析错）；
 *   2) 勾叉分类正确 —— 新值是「开/启用」打勾，是「关/禁用」打叉，
 *      字符串与数字等非开关改动一律打勾（主人指定）。
 *
 * 这两条是界面正确性的根，靠人眼看截图看不出分类错没错。
 */
static void check_changelist_struct(const char *what,
                                    void (*apply)(Conf *),
                                    const char *expect_name,
                                    const char *expect_old,
                                    const char *expect_new,
                                    CnChangeKind expect_kind)
{
    Conf *base = conf_new();
    Conf *cur = conf_new();
    do_defaults(NULL, base);
    conf_copy_into(cur, base);
    apply(cur);

    CnChangeList *list = conf_describe_changes_struct(base, cur);
    if (list->n != 1) {
        fprintf(stderr, "fail structlist(%s): 条目数 %d，期望 1\n",
                what, (int)list->n);
        nfails++;
    } else {
        CnChange *it = &list->items[0];
        if (strcmp(it->name, expect_name) != 0) {
            fprintf(stderr, "fail structlist(%s): name 是「%s」，期望「%s」\n",
                    what, it->name, expect_name);
            nfails++;
        }
        if (strcmp(it->oldval, expect_old) != 0) {
            fprintf(stderr, "fail structlist(%s): old 是「%s」，期望「%s」\n",
                    what, it->oldval, expect_old);
            nfails++;
        }
        if (strcmp(it->newval, expect_new) != 0) {
            fprintf(stderr, "fail structlist(%s): new 是「%s」，期望「%s」\n",
                    what, it->newval, expect_new);
            nfails++;
        }
        if (it->kind != expect_kind) {
            fprintf(stderr, "fail structlist(%s): kind 是 %d，期望 %d\n",
                    what, (int)it->kind, (int)expect_kind);
            nfails++;
        }
    }
    cn_change_list_free(list);
    conf_free(base);
    conf_free(cur);
}

/*
 * cn: 结构化清单与文本清单必须逐行对应 —— 文本由结构拼出，若两者
 * 出现分歧说明有人只改了一边。
 */
static void check_struct_matches_text(const char *what, void (*apply)(Conf *))
{
    Conf *base = conf_new();
    Conf *cur = conf_new();
    do_defaults(NULL, base);
    conf_copy_into(cur, base);
    apply(cur);

    CnChangeList *list = conf_describe_changes_struct(base, cur);
    char *text = conf_describe_changes(base, cur);
    strbuf *sb = strbuf_new();
    for (size_t i = 0; i < list->n; i++)
        put_fmt(sb, "【%s】 %s → %s\r\n",
                list->items[i].name, list->items[i].oldval,
                list->items[i].newval);
    char *rebuilt = strbuf_to_str(sb);
    if (strcmp(rebuilt, text) != 0) {
        fprintf(stderr, "fail struct2text(%s):\n  结构拼回: %s  文本版:   %s",
                what, rebuilt, text);
        nfails++;
    }
    sfree(rebuilt);
    sfree(text);
    cn_change_list_free(list);
    conf_free(base);
    conf_free(cur);
}

/*
 * cn: Mapping-type settings (port forwardings, environment variables,
 * manual host keys) were skipped by both conf_equal and
 * conf_describe_changes, so adding a forwarding never woke the Save
 * button up and never appeared in the change list.
 */
static void check_map_change(const char *what,
                             void (*apply)(Conf *), const char *expect)
{
    Conf *base = conf_new();
    Conf *cur = conf_new();
    do_defaults(NULL, base);
    conf_copy_into(cur, base);
    apply(cur);

    if (conf_equal(base, cur)) {
        fprintf(stderr, "fail conf_equal(%s): mapping change not detected\n",
                what);
        nfails++;
    }

    char *got = conf_describe_changes(base, cur);
    if (strcmp(got, expect) != 0) {
        fprintf(stderr, "fail mapchangelist(%s):\n  got:    %s  expect: %s",
                what, got, expect);
        nfails++;
    }
    sfree(got);
    conf_free(base);
    conf_free(cur);
}

static void cl_pf_add(Conf *c)
{ conf_set_str_str(c, CONF_portfwd, "L8080", "localhost:80"); }
static void cl_pf_two(Conf *c)
{
    conf_set_str_str(c, CONF_portfwd, "L8080", "localhost:80");
    conf_set_str_str(c, CONF_portfwd, "R9090", "example.com:22");
}
static void cl_pf_change(Conf *c)
{
    conf_set_str_str(c, CONF_portfwd, "L8080", "localhost:80");
    conf_set_str_str(c, CONF_portfwd, "L8080", "localhost:8080");
}
static void cl_env_add(Conf *c)
{ conf_set_str_str(c, CONF_environmt, "FOO", "bar"); }

static void test_mappings(void)
{
    check_map_change("portfwd add", cl_pf_add,
                     "【端口转发】 L8080：(无) → localhost:80\r\n");
    check_map_change("portfwd two", cl_pf_two,
                     "【端口转发】 L8080：(无) → localhost:80\r\n"
                     "【端口转发】 R9090：(无) → example.com:22\r\n");
    check_map_change("portfwd change", cl_pf_change,
                     "【端口转发】 L8080：(无) → localhost:8080\r\n");
    check_map_change("environmt add", cl_env_add,
                     "【环境变量】 FOO：(无) → bar\r\n");

    /* Removing an entry must show as a deletion, not as no change. */
    Conf *base = conf_new();
    Conf *cur = conf_new();
    do_defaults(NULL, base);
    conf_set_str_str(base, CONF_portfwd, "L8080", "localhost:80");
    conf_copy_into(cur, base);
    conf_del_str_str(cur, CONF_portfwd, "L8080");
    if (conf_equal(base, cur)) {
        fprintf(stderr, "fail conf_equal(portfwd del): "
                "deletion not detected\n");
        nfails++;
    }
    char *got = conf_describe_changes(base, cur);
    const char *want = "【端口转发】 L8080：localhost:80 → (无)\r\n";
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "fail mapchangelist(portfwd del):\n"
                "  got:    %s  expect: %s", got, want);
        nfails++;
    }
    sfree(got);
    conf_free(base);
    conf_free(cur);
}

/*
 * cn: The save button's dirty check runs off conf_equal, so a setting
 * that conf_equal skips leaves the button permanently grey no matter
 * what the user does. Assert directly that each of the three
 * clipboard dropdowns makes two otherwise identical configs compare
 * unequal, and that an untouched copy still compares equal.
 */
static void test_conf_equal(void)
{
    Conf *base = conf_new();
    Conf *cur = conf_new();
    do_defaults(NULL, base);

    conf_copy_into(cur, base);
    if (!conf_equal(base, cur)) {
        fprintf(stderr, "fail conf_equal: identical configs reported "
                "different\n");
        nfails++;
    }

    static const struct {
        const char *what;
        int key;
        int val;
    } cases[] = {
        { "mousepaste",   CONF_mousepaste,   CLIPUI_NONE },
        { "ctrlshiftins", CONF_ctrlshiftins, CLIPUI_IMPLICIT },
        { "ctrlshiftcv",  CONF_ctrlshiftcv,  CLIPUI_EXPLICIT },
    };
    for (size_t i = 0; i < lenof(cases); i++) {
        conf_copy_into(cur, base);
        conf_set_int(cur, cases[i].key, cases[i].val);
        if (conf_equal(base, cur)) {
            fprintf(stderr, "fail conf_equal(%s): change not detected\n",
                    cases[i].what);
            nfails++;
        }
    }

    conf_free(base);
    conf_free(cur);
}

/*
 * cn: 用户名与主机/端口/协议同属「连接目标」：conf_equal（dirty 门控）
 * 与 conf_describe_changes（改动清单）都必须忽略它。锁死这条语义，
 * 防后人把 CONF_username 从 cn_is_connection_target 清单里误删。
 */
static void test_username_is_connection_target(void)
{
    Conf *base = conf_new();
    Conf *cur = conf_new();
    do_defaults(NULL, base);
    conf_copy_into(cur, base);

    conf_set_str(cur, CONF_username, "cn-selftest");

    if (!conf_equal(base, cur)) {
        fprintf(stderr, "fail username dirty: username change must not "
                "make configs unequal (it is a connection target)\n");
        nfails++;
    }

    char *changes = conf_describe_changes(base, cur);
    /*
     * cn: 无可见改动时该函数返回占位文案「(无可见改动)」而非空串；
     * 用户名变化必须与「完全没动过」产出同样的输出。
     */
    char *nochange = conf_describe_changes(base, base);
    if (!changes || !nochange || strcmp(changes, nochange) != 0) {
        fprintf(stderr, "fail username changelist: expected \"%s\", got %s",
                nochange ? nochange : "(null)",
                changes ? changes : "(null)");
        nfails++;
    }
    sfree(changes);
    sfree(nochange);

    conf_free(base);
    conf_free(cur);
}

int main(void)
{
    test_conf_key_info();
    test_simple();
    test_changelist();
    test_mappings();
    test_conf_equal();
    test_username_is_connection_target();
    return nfails != 0;
}
