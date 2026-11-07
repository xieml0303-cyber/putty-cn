#include "putty.h"

bool agent_exists(void) { return false; }
Socket *agent_connect(Plug *plug) {
    return new_error_socket_fmt(
        plug, "此程序不包含实际网络功能");
}
void agent_cancel_query(agent_pending_query *pq) {}
agent_pending_query *agent_query(
    strbuf *query, void **out, int *outlen,
    void (*callback)(void *, void *, int), void *callback_ctx) {return NULL;}
