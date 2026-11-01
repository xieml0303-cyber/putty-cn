#include "putty.h"

#define CONF_ENUM(name, ...)                                            \
    static const ConfSaveEnumValue conf_enum_values_##name[] = {        \
        __VA_ARGS__                                                     \
    }; const ConfSaveEnumType conf_enum_##name = {                      \
        .values = conf_enum_values_##name,                              \
        .nvalues = lenof(conf_enum_values_##name),                      \
    };

#define VALUE(eval, sval) { eval, sval, false }
#define VALUE_OBSOLETE(eval, sval) { eval, sval, true }

#include "conf-enums.h"

bool conf_enum_map_to_storage(const ConfSaveEnumType *etype,
                              int confval, int *storageval_out)
{
    for (size_t i = 0; i < etype->nvalues; i++)
        if (!etype->values[i].obsolete &&
            etype->values[i].confval == confval) {
            *storageval_out = etype->values[i].storageval;
            return true;
        }
    return false;
}

bool conf_enum_map_from_storage(const ConfSaveEnumType *etype,
                                int storageval, int *confval_out)
{
    for (size_t i = 0; i < etype->nvalues; i++)
        if (etype->values[i].storageval == storageval) {
            *confval_out = etype->values[i].confval;
            return true;
        }
    return false;
}

#define CONF_OPTION(id, ...) { __VA_ARGS__ },
#define VALUE_TYPE(x) .value_type = CONF_TYPE_ ## x
#define SUBKEY_TYPE(x) .subkey_type = CONF_TYPE_ ## x
#define DEFAULT_INT(x) .default_value.ival = x
#define DEFAULT_STR(x) .default_value.sval = x
#define DEFAULT_BOOL(x) .default_value.bval = x
#define SAVE_KEYWORD(x) .save_keyword = x
#define STORAGE_ENUM(x) .storage_enum = &conf_enum_ ## x
#define SAVE_CUSTOM .save_custom = true
#define LOAD_CUSTOM .load_custom = true
#define NOT_SAVED .not_saved = true

const ConfKeyInfo conf_key_info[] = {
    #include "conf.h"
};

/*
 * cn: The connection targets (host, port, protocol) are deliberately
 * NOT persisted by the save button — they are written as pristine
 * defaults (empty host, SSH / 22) and the startup box resets them.
 * They therefore do not count as "unsaved changes": neither the
 * dirty gate nor the change list takes them into account.
 */
static bool cn_is_connection_target(size_t key)
{
    return key == CONF_host || key == CONF_port ||
        key == CONF_protocol;
}

/*
 * cn: Compare two configurations field by field over conf_key_info.
 * Skips options that are never persisted (not_saved, save_custom,
 * mappings with subkeys) and the connection targets. Returns true if
 * identical.
 */
bool conf_equal(const Conf *a, const Conf *b)
{
    for (size_t key = 0; key < N_CONFIG_OPTIONS; key++) {
        const ConfKeyInfo *info = &conf_key_info[key];
        if (info->save_custom || info->not_saved)
            continue;
        if (info->subkey_type != CONF_TYPE_NONE)
            continue;
        if (cn_is_connection_target(key))
            continue;

        switch (info->value_type) {
          case CONF_TYPE_STR: {
            const char *sa = conf_get_str(a, key);
            const char *sb = conf_get_str(b, key);
            if (strcmp(sa, sb) != 0)
                return false;
            break;
          }
          case CONF_TYPE_STR_AMBI: {
            bool a_utf8, b_utf8;
            const char *sa = conf_get_str_ambi(a, key, &a_utf8);
            const char *sb2 = conf_get_str_ambi(b, key, &b_utf8);
            if (strcmp(sa, sb2) != 0)
                return false;
            break;
          }
          case CONF_TYPE_INT:
            if (conf_get_int(a, key) != conf_get_int(b, key))
                return false;
            break;
          case CONF_TYPE_BOOL:
            if (conf_get_bool(a, key) != conf_get_bool(b, key))
                return false;
            break;
          case CONF_TYPE_FILENAME:
            if (strcmp(conf_get_filename(a, key)->utf8path,
                       conf_get_filename(b, key)->utf8path) != 0)
                return false;
            break;
          case CONF_TYPE_FONT:
            if (strcmp(conf_get_fontspec(a, key)->name,
                       conf_get_fontspec(b, key)->name) != 0 ||
                conf_get_fontspec(a, key)->height !=
                conf_get_fontspec(b, key)->height)
                return false;
            break;
          default:
            break;
        }
    }
    return true;
}

/*
 * cn: Describe the differences between two configurations, one line
 * per changed option, formatted as "关键字: 旧值 → 新值". Used by the
 * save-confirmation and close prompts so the user can see exactly
 * what is about to be persisted.
 *
 * CONF_protocol is special-cased to display protocol names rather
 * than raw enum numbers.
 */
char *conf_describe_changes(const Conf *baseline, const Conf *current)
{
    strbuf *sb = strbuf_new();
    int nchanges = 0;

    for (size_t key = 0; key < N_CONFIG_OPTIONS; key++) {
        const ConfKeyInfo *info = &conf_key_info[key];
        if (info->save_custom || info->not_saved)
            continue;
        if (info->subkey_type != CONF_TYPE_NONE)
            continue;
        if (cn_is_connection_target(key))
            continue;

        switch (info->value_type) {
          case CONF_TYPE_STR: {
            const char *a = conf_get_str(baseline, key);
            const char *b = conf_get_str(current, key);
            if (strcmp(a, b) != 0) {
                put_fmt(sb, "%s: %s → %s\r\n", info->save_keyword,
                        *a ? a : "(空)", *b ? b : "(空)");
                nchanges++;
            }
            break;
          }
          case CONF_TYPE_STR_AMBI: {
            bool a_utf8, b_utf8;
            const char *a = conf_get_str_ambi(baseline, key, &a_utf8);
            const char *b = conf_get_str_ambi(current, key, &b_utf8);
            if (strcmp(a, b) != 0) {
                put_fmt(sb, "%s: %s → %s\r\n", info->save_keyword,
                        *a ? a : "(空)", *b ? b : "(空)");
                nchanges++;
            }
            break;
          }
          case CONF_TYPE_INT: {
            int a = conf_get_int(baseline, key);
            int b = conf_get_int(current, key);
            if (a != b) {
                put_fmt(sb, "%s: %d → %d\r\n", info->save_keyword, a, b);
                nchanges++;
            }
            break;
          }
          case CONF_TYPE_BOOL:
            if (conf_get_bool(baseline, key) !=
                conf_get_bool(current, key)) {
                put_fmt(sb, "%s: %s → %s\r\n", info->save_keyword,
                        conf_get_bool(baseline, key) ? "开" : "关",
                        conf_get_bool(current, key) ? "开" : "关");
                nchanges++;
            }
            break;
          case CONF_TYPE_FILENAME: {
            const Filename *a = conf_get_filename(baseline, key);
            const Filename *b = conf_get_filename(current, key);
            if (strcmp(a->utf8path, b->utf8path) != 0) {
                put_fmt(sb, "%s: %s → %s\r\n", info->save_keyword,
                        *a->utf8path ? a->utf8path : "(空)",
                        *b->utf8path ? b->utf8path : "(空)");
                nchanges++;
            }
            break;
          }
          case CONF_TYPE_FONT: {
            const FontSpec *a = conf_get_fontspec(baseline, key);
            const FontSpec *b = conf_get_fontspec(current, key);
            if (strcmp(a->name, b->name) != 0 || a->height != b->height) {
                put_fmt(sb, "%s: %s %d → %s %d\r\n", info->save_keyword,
                        a->name, a->height, b->name, b->height);
                nchanges++;
            }
            break;
          }
          default:
            break;
        }
    }

    if (nchanges == 0)
        return dupstr("(无可见改动)");

    return strbuf_to_str(sb);
}
