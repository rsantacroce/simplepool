#ifndef SIMPLEPOOL_CONFIG_H
#define SIMPLEPOOL_CONFIG_H

#include <stddef.h>

typedef struct {
    /* listener */
    char listen_addr[64];
    int  listen_port;
    int  max_conns;
    double initial_diff;

    /* bitcoind */
    char bitcoind_url[512];
    char bitcoind_user[128];
    char bitcoind_pass[256];
    int  bitcoind_poll_interval_ms;

    /* coinbase */
    char operator_address[128];   /* 1% (fee_bps) fee recipient */
    int  fee_bps;                 /* basis points, default 100 (=1%), cap 1000 */
    char coinbase_tag[64];

    /* sqlite */
    char db_path[512];
    int  commit_window_ms;
    int  commit_max_shares;

    /* logging */
    int  log_level;            /* 0..3 */

    /* upstream hedge — route a fraction of the fleet to an upstream pool for
     * steady income while the rest mines the operator's solo template. See
     * docs/upstream-hedge-design.md. Disabled by default (pure solo). */
    int    upstream_enabled;     /* master switch, default 0 */
    char   upstream_host[256];
    int    upstream_port;
    char   upstream_user[256];   /* our pool account, e.g. "bcaddr.worker" */
    char   upstream_pass[64];    /* usually "x" */
    double pool_fraction;        /* 0.0..1.0 of the fleet routed to the pool */
} proxy_config_t;

void proxy_config_defaults(proxy_config_t *cfg);
int  proxy_config_load(const char *path, proxy_config_t *cfg,
                       char *errbuf, size_t errlen);

#endif /* SIMPLEPOOL_CONFIG_H */
