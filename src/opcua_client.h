/*
 * opcua_client.h - the OPC UA client side of the driver.
 *
 *   opcua_client_get()   latest values of the whole plant (never blocks)
 *   opcua_client_call()  run one method on the machine (blocks, bounded)
 *
 * Two independent links, each its own UA_Client and session:
 *   GET   thread: periodic batched Read -> snapshot
 *   CALL  method calls, one at a time
 *
 * Nothing here knows about QNX messaging; it only blocks the calling thread.
 */
#ifndef OPCUA_CLIENT_H
#define OPCUA_CLIENT_H

#include "opcua_cnc_map.h"

typedef struct {
    const char *url;
    const char *ns_uri;
    const char *root;
    unsigned    read_period_ms;   /* was poll_period_ms / period_ms */
    unsigned    timeout_ms;
    int         read_prio;        /* was poll_prio / get_prio */
} opcua_cfg_t;

int  opcua_client_start(const opcua_cfg_t *cfg);     /* EOK or errno           */
void opcua_client_stop(void);                        /* close both sessions    */
void opcua_client_get(cnc_plant_t *out);             /* never blocks           */
int  opcua_client_call(cnc_method_t m, int32_t arg); /* EOK or errno           */

#endif /* OPCUA_CLIENT_H */
