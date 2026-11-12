/*
 * storage.h: interface defining functions for storage and recovery
 * of PuTTY's persistent data.
 */

#ifndef PUTTY_STORAGE_H
#define PUTTY_STORAGE_H

#include "defs.h"

/* ----------------------------------------------------------------------
 * Functions to save and restore PuTTY sessions. Note that this is
 * only the low-level code to do the reading and writing. The
 * higher-level code that translates an internal Conf structure into
 * a set of (key,value) pairs in their external storage format is
 * elsewhere, since it doesn't (mostly) change between platforms.
 */

/*
 * Write a saved session. The caller is expected to call
 * open_setting_w() to get a `void *' handle, then pass that to a
 * number of calls to write_setting_s() and write_setting_i(), and
 * then close it using close_settings_w(). At the end of this call
 * sequence the settings should have been written to the PuTTY
 * persistent storage area.
 *
 * A given key will be written at most once while saving a session.
 * Keys may be up to 255 characters long.  String values have no length
 * limit.
 *
 * Any returned error message must be freed after use.
 */
settings_w *open_settings_w(const char *sessionname, char **errmsg);
void write_setting_s(settings_w *handle, const char *key, const char *value);
void write_setting_i(settings_w *handle, const char *key, int value);
void write_setting_filename(settings_w *handle,
                            const char *key, Filename *value);
void write_setting_fontspec(settings_w *handle,
                            const char *key, FontSpec *font);
void close_settings_w(settings_w *handle);

/*
 * Read a saved session. The caller is expected to call
 * open_setting_r() to get a `void *' handle, then pass that to a
 * number of calls to read_setting_s() and read_setting_i(), and
 * then close it using close_settings_r().
 *
 * read_setting_s() returns a dynamically allocated string which the
 * caller must free. read_setting_filename() and
 * read_setting_fontspec() likewise return dynamically allocated
 * structures.
 *
 * If a particular string setting is not present in the session,
 * read_setting_s() can return NULL, in which case the caller
 * should invent a sensible default. If an integer setting is not
 * present, read_setting_i() returns its provided default.
 */
settings_r *open_settings_r(const char *sessionname);
char *read_setting_s(settings_r *handle, const char *key);
int read_setting_i(settings_r *handle, const char *key, int defvalue);
Filename *read_setting_filename(settings_r *handle, const char *key);
FontSpec *read_setting_fontspec(settings_r *handle, const char *key);
void close_settings_r(settings_r *handle);

/*
 * Delete a whole saved session.
 */
void del_settings(const char *sessionname);

/*
 * Enumerate all saved sessions.
 */
settings_e *enum_settings_start(void);
bool enum_settings_next(settings_e *handle, strbuf *out);
void enum_settings_finish(settings_e *handle);

/* ----------------------------------------------------------------------
 * Recent connection history: host names / IP addresses the user has
 * tried to connect to, together with the port each was last used on,
 * most recent first.
 */

struct recent_conn {
    char *host;
    int port;      /* 0 if the entry predates port tracking */
    char *username; /* 空串 = 未记录（旧条目或不适用），回填时留空 */
    /*
     * cn: 所属目录名，空串 = 未归类（直接挂在列表根部）。
     *
     * 存名字而不是目录序号：目录被删除或改名时不必回写每一个条目的
     * 索引，改名由 rename_recent_folder 一次性改写归属。
     */
    char *folder;
    /*
     * cn: 用户自己起的显示名，空串 = 照旧显示 host。
     *
     * 只影响列表里怎么画这一行，host / port / username 三个真实字段
     * 一律不动 —— 双击回填、连接目标、去重比较都还按真实值走。
     */
    char *label;
};

/* caller frees via free_recent_conns */
struct recent_conn *get_recent_conns(int *count);
void add_recent_conn(const char *host, int port, const char *username);
void del_recent_conn(const char *host, int port, const char *username);
void free_recent_conns(struct recent_conn *list, int count);

/*
 * cn: 会话目录 —— 把历史会话按 IP 归类用，只做一级（目录里不能再放
 * 目录）。
 *
 * 目录单独存一个注册表键，不能塞进 RecentHosts 下：历史列表的写入是
 * 「整键删除再重建」（见 put_recent_conns），目录跟着进去会被连带抹掉。
 */
char **get_recent_folders(int *count);
void free_recent_folders(char **list, int count);
/* 空名 / 重名 / 数量超限 → false */
bool add_recent_folder(const char *name);
/* 目录不存在或与别的目录重名 → false；同时改写其下会话的归属 */
bool rename_recent_folder(const char *oldname, const char *newname);
/* 只删目录，其下会话改回未归类（不删会话本身） */
void del_recent_folder(const char *name);
/* 把一条会话移进某个目录；folder 为空串表示移出到未归类 */
void set_recent_conn_folder(const char *host, int port,
                            const char *username, const char *folder);
/*
 * cn: 给一条会话改显示名；label 为空串表示恢复显示真实主机名。
 * 只改列表显示，不改 host/port/username，也不影响连接行为。
 */
void set_recent_conn_label(const char *host, int port,
                           const char *username, const char *label);

/* ----------------------------------------------------------------------
 * Serial ports: the serial ports actually present on this machine,
 * for the serial-port drop-down list. Caller frees via
 * free_serial_ports.
 */

char **get_serial_ports(int *count);
void free_serial_ports(char **ports, int count);
/*
 * True if the named port is a Bluetooth virtual serial port rather
 * than a wired one, so callers can prefer real hardware.
 */
bool serial_port_is_bluetooth(const char *port);

/* ----------------------------------------------------------------------
 * Functions to access PuTTY's host key database.
 */

/*
 * See if a host key matches the database entry. Return values can
 * be 0 (entry matches database), 1 (entry is absent in database),
 * or 2 (entry exists in database and is different).
 */
int check_stored_host_key(const char *hostname, int port,
                          const char *keytype, const char *key);

/*
 * Write a host key into the database, overwriting any previous
 * entry that might have been there.
 *
 * A Seat is provided for error-reporting purposes.
 */
void store_host_key(Seat *seat, const char *hostname, int port,
                    const char *keytype, const char *key);

/* ----------------------------------------------------------------------
 * Functions to access PuTTY's configuration for trusted host
 * certification authorities. This must be stored separately from the
 * saved-session data, because the whole point is to avoid having to
 * configure CAs separately per session.
 */

struct host_ca {
    char *name;
    strbuf *ca_public_key;
    char *validity_expression;
    ca_options opts;
};

host_ca_enum *enum_host_ca_start(void);
bool enum_host_ca_next(host_ca_enum *handle, strbuf *out);
void enum_host_ca_finish(host_ca_enum *handle);

host_ca *host_ca_load(const char *name);
char *host_ca_save(host_ca *);   /* NULL on success, or dynamic error msg */
char *host_ca_delete(const char *name); /* likewise */

host_ca *host_ca_new(void);  /* initialises to default settings */
void host_ca_free(host_ca *);

/* ----------------------------------------------------------------------
 * Functions to access PuTTY's random number seed file.
 */

typedef void (*noise_consumer_t) (void *data, int len);

/*
 * Read PuTTY's random seed file and pass its contents to a noise
 * consumer function.
 */
void read_random_seed(noise_consumer_t consumer);

/*
 * Write PuTTY's random seed file from a given chunk of noise.
 */
void write_random_seed(void *data, int len);

/* ----------------------------------------------------------------------
 * Cleanup function: remove all of PuTTY's persistent state.
 */
void cleanup_all(void);

#endif
