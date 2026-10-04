/*
 * opcua_client.c - OPC UA client side of the CNC driver.
 *
 * Two independent sessions, each with its own UA_Client:
 *   read session  owned by read_thread: periodic batched Read -> snapshot
 *   write session owned by the driver's write thread: one call at a time
 *
 * The snapshot is handed to readers through a priority-inheriting mutex.
 * read_thread is the only writer; io_read (pool threads) are readers.
 * Each critical section is one 432-byte struct copy.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "open62541.h"
#include "opcua_client.h"
#include "cnc_log.h"

#ifndef EOK
#define EOK 0
#endif

#define RECONNECT_S 1

static opcua_cfg_t cfg;
static _Atomic int stopping;
static pthread_t   read_tid;

/* ------------------------------------------------------------------ */
/* Mappings: server browse paths -> plant fields / methods            */
/* ------------------------------------------------------------------ */

typedef enum { K_DOUBLE, K_INT32, K_MSTATE, K_TSTATE, K_STRING, K_DATETIME } kind_t;

#define F(field) offsetof(cnc_plant_t, field)

static const struct { const char *path; kind_t kind; size_t off; } var_def[] = {
    { "State/CurrentState",                  K_MSTATE,   F(state.machine_state)        },
    { "Timestamp",                           K_DATETIME, F(hdr.plant_time_ns)          },
    { "Spindle/Speed",                       K_DOUBLE,   F(spindle.speed_rpm)          },
    { "Spindle/Load",                        K_DOUBLE,   F(spindle.load_pct)           },
    { "Spindle/Torque",                      K_DOUBLE,   F(spindle.torque_nm)          },
    { "Spindle/Power",                       K_DOUBLE,   F(spindle.power_kw)           },
    { "Spindle/Temperature",                 K_DOUBLE,   F(spindle.temperature_c)      },
    { "FeedSystem/FeedRate",                 K_DOUBLE,   F(feed.rate_mm_min)           },
    { "FeedSystem/Override",                 K_DOUBLE,   F(feed.override_pct)          },
    { "FeedSystem/Position/X",               K_DOUBLE,   F(feed.x_mm)                  },
    { "FeedSystem/Position/Y",               K_DOUBLE,   F(feed.y_mm)                  },
    { "FeedSystem/Position/Z",               K_DOUBLE,   F(feed.z_mm)                  },
    { "Tool/Number",                         K_INT32,    F(tool.number)                },
    { "Tool/LifeRemaining",                  K_DOUBLE,   F(tool.life_remaining_pct)    },
    { "Tool/WearX",                          K_DOUBLE,   F(tool.wear_x_mm)             },
    { "Tool/WearZ",                          K_DOUBLE,   F(tool.wear_z_mm)             },
    { "Tool/State",                          K_TSTATE,   F(tool.tool_state)            },
    { "Vibration/X_Axis",                    K_DOUBLE,   F(vibration.x_mm_s)           },
    { "Vibration/Y_Axis",                    K_DOUBLE,   F(vibration.y_mm_s)           },
    { "Vibration/Z_Axis",                    K_DOUBLE,   F(vibration.z_mm_s)           },
    { "Vibration/Overall",                   K_DOUBLE,   F(vibration.overall_mm_s)     },
    { "Production/PartsProduced",            K_INT32,    F(production.parts_produced)  },
    { "Production/CycleTime",                K_DOUBLE,   F(production.cycle_time_s)    },
    { "Production/GoodParts",                K_INT32,    F(production.good_parts)      },
    { "Production/RejectedParts",            K_INT32,    F(production.rejected_parts)  },
    { "Production/Efficiency",               K_DOUBLE,   F(production.efficiency_pct)  },
    { "AuxiliarySystems/CoolantLevel",       K_DOUBLE,   F(aux.coolant_level_pct)      },
    { "AuxiliarySystems/CoolantTemperature", K_DOUBLE,   F(aux.coolant_temperature_c)  },
    { "AuxiliarySystems/AirPressure",        K_DOUBLE,   F(aux.air_pressure_bar)       },
    { "AuxiliarySystems/HydraulicPressure",  K_DOUBLE,   F(aux.hydraulic_pressure_bar) },
    { "Information/Manufacturer",            K_STRING,   F(info.manufacturer)          },
    { "Information/Model",                   K_STRING,   F(info.model)                 },
    { "Information/SerialNumber",            K_STRING,   F(info.serial_number)         },
};
#define N_VARS (sizeof var_def / sizeof var_def[0])

static const char *method_path[1 + CNC_METHOD_COUNT] = {
    "Methods",
    [1 + CNC_ESTOP]          = "Methods/EmergencyStop",
    [1 + CNC_RESET_COUNTERS] = "Methods/ResetProductionCounters",
    [1 + CNC_CHANGE_TOOL]    = "Methods/ChangeTool",
};
#define N_METHOD_NODES (1 + CNC_METHOD_COUNT)

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int to_errno(UA_StatusCode s)
{
    if (s == UA_STATUSCODE_GOOD)                return EOK;
    if (s == UA_STATUSCODE_BADTIMEOUT)          return ETIMEDOUT;
    if (s == UA_STATUSCODE_BADUSERACCESSDENIED ||
        s == UA_STATUSCODE_BADNOTEXECUTABLE)    return EACCES;
    if (s == UA_STATUSCODE_BADINVALIDARGUMENT ||
        s == UA_STATUSCODE_BADTYPEMISMATCH)     return EINVAL;
    return EIO;
}

static int str_is(const UA_String *s, const char *c)
{
    size_t len = strlen(c);
    return s->length == len && memcmp(s->data, c, len) == 0;
}

static uint32_t machine_state(const UA_String *s)
{
    if (str_is(s, "Idle"))        return CNC_MACHINE_IDLE;
    if (str_is(s, "Running"))     return CNC_MACHINE_RUNNING;
    if (str_is(s, "Alarm"))       return CNC_MACHINE_ALARM;
    if (str_is(s, "Maintenance")) return CNC_MACHINE_MAINTENANCE;
    if (str_is(s, "Setup"))       return CNC_MACHINE_SETUP;
    return CNC_MACHINE_UNKNOWN;
}

static uint32_t tool_state(const UA_String *s)
{
    if (str_is(s, "New"))    return CNC_TOOL_NEW;
    if (str_is(s, "Good"))   return CNC_TOOL_GOOD;
    if (str_is(s, "Worn"))   return CNC_TOOL_WORN;
    if (str_is(s, "Broken")) return CNC_TOOL_BROKEN;
    return CNC_TOOL_UNKNOWN;
}

static UA_Client *ua_new(void)
{
    UA_Client *c = UA_Client_new();
    if (c)
        UA_Client_getConfig(c)->timeout = cfg.timeout_ms;
    return c;
}

/* Connect c and resolve n browse paths (under cfg.root) into out[]. */
static UA_StatusCode ua_open(UA_Client *c, size_t n, const char *const *paths, UA_NodeId *out)
{
    UA_String     uri = UA_STRING((char *)cfg.ns_uri);
    UA_UInt16     ns;
    UA_StatusCode st;

    st = UA_Client_connect(c, cfg.url);
    if (st == UA_STATUSCODE_GOOD)
        st = UA_Client_NamespaceGetIndex(c, &uri, &ns);
    if (st != UA_STATUSCODE_GOOD)
        return st;

    UA_BrowsePath *bp = UA_Array_new(n, &UA_TYPES[UA_TYPES_BROWSEPATH]);
    if (!bp)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    for (size_t i = 0; i < n; i++) {
        char   buf[256], *seg[8], *save;
        size_t k = 0;

        snprintf(buf, sizeof buf, "%s/%s", cfg.root, paths[i]);
        for (char *t = strtok_r(buf, "/", &save); t && k < 8; t = strtok_r(NULL, "/", &save))
            seg[k++] = t;

        bp[i].startingNode = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
        bp[i].relativePath.elements     = UA_Array_new(k, &UA_TYPES[UA_TYPES_RELATIVEPATHELEMENT]);
        bp[i].relativePath.elementsSize = k;
        for (size_t j = 0; j < k; j++) {
            UA_RelativePathElement *e = &bp[i].relativePath.elements[j];
            e->referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES);
            e->includeSubtypes = true;
            e->targetName      = UA_QUALIFIEDNAME_ALLOC(ns, seg[j]);
        }
    }

    UA_TranslateBrowsePathsToNodeIdsRequest req;
    UA_TranslateBrowsePathsToNodeIdsRequest_init(&req);
    req.browsePaths     = bp;
    req.browsePathsSize = n;
    UA_TranslateBrowsePathsToNodeIdsResponse resp =
        UA_Client_Service_translateBrowsePathsToNodeIds(c, req);

    st = resp.responseHeader.serviceResult;
    for (size_t i = 0; st == UA_STATUSCODE_GOOD && i < n; i++) {
        if (i >= resp.resultsSize || resp.results[i].statusCode != UA_STATUSCODE_GOOD ||
            resp.results[i].targetsSize < 1) {
            CNC_LOG("ERROR", "cannot resolve %s/%s", cfg.root, paths[i]);
            st = UA_STATUSCODE_BADNOMATCH;
        } else {
            UA_NodeId_copy(&resp.results[i].targets[0].targetId.nodeId, &out[i]);
        }
    }
    UA_TranslateBrowsePathsToNodeIdsResponse_clear(&resp);
    UA_TranslateBrowsePathsToNodeIdsRequest_clear(&req);   /* frees bp */
    return st;
}

static void ua_close(UA_Client **c, UA_NodeId *nodes, size_t n)
{
    if (!*c)
        return;
    for (size_t i = 0; i < n; i++)
        UA_NodeId_clear(&nodes[i]);
    UA_Client_disconnect(*c);
    UA_Client_delete(*c);
    *c = NULL;
}

/* ------------------------------------------------------------------ */
/* Snapshot (read session)                                            */
/*                                                                    */
/* Published by read_thread (twice a second), copied by io_read.      */
/* A plain priority-inheriting mutex: readers do take a lock, but the */
/* critical section is one 432-byte copy, so contention is bounded.   */

static cnc_plant_t      snap;
static pthread_mutex_t  snap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t         snap_gen;   /* hdr.seq, only touched by publish() */

void opcua_client_get(cnc_plant_t *out)
{
    pthread_mutex_lock(&snap_lock);
    *out = snap;
    pthread_mutex_unlock(&snap_lock);
}

static void publish(const cnc_plant_t *s)
{
    pthread_mutex_lock(&snap_lock);
    snap = *s;
    snap.hdr.seq = ++snap_gen;
    pthread_mutex_unlock(&snap_lock);
}

static UA_StatusCode read_all(UA_Client *c, const UA_NodeId *node, cnc_plant_t *s)
{
    UA_ReadValueId rv[N_VARS];
    UA_ReadRequest req;

    for (size_t i = 0; i < N_VARS; i++) {
        UA_ReadValueId_init(&rv[i]);
        rv[i].nodeId      = node[i];
        rv[i].attributeId = UA_ATTRIBUTEID_VALUE;
    }
    UA_ReadRequest_init(&req);
    req.nodesToRead     = rv;
    req.nodesToReadSize = N_VARS;

    UA_ReadResponse resp = UA_Client_Service_read(c, req);
    UA_StatusCode   st   = resp.responseHeader.serviceResult;
    if (st == UA_STATUSCODE_GOOD && resp.resultsSize != N_VARS)
        st = UA_STATUSCODE_BADUNEXPECTEDERROR;

    for (size_t i = 0; st == UA_STATUSCODE_GOOD && i < N_VARS; i++) {
        const UA_Variant *v   = &resp.results[i].value;
        char             *dst = (char *)s + var_def[i].off;

        if (!resp.results[i].hasValue || UA_Variant_isEmpty(v)) {
            st = UA_STATUSCODE_BADNODATA;
            break;
        }
        const UA_DataType *want =
            var_def[i].kind == K_DOUBLE   ? &UA_TYPES[UA_TYPES_DOUBLE]   :
            var_def[i].kind == K_INT32    ? &UA_TYPES[UA_TYPES_INT32]    :
            var_def[i].kind == K_DATETIME ? &UA_TYPES[UA_TYPES_DATETIME] :
                                            &UA_TYPES[UA_TYPES_STRING];
        if (v->type != want) {
            CNC_LOG("WARN", "type mismatch on %s (got %s)", var_def[i].path,
                    v->type ? v->type->typeName : "?");
            st = UA_STATUSCODE_BADTYPEMISMATCH;
            break;
        }
        switch (var_def[i].kind) {
        case K_DOUBLE:   memcpy(dst, v->data, sizeof(double));  break;
        case K_INT32:    memcpy(dst, v->data, sizeof(int32_t)); break;
        case K_MSTATE:   *(uint32_t *)dst = machine_state(v->data); break;
        case K_TSTATE:   *(uint32_t *)dst = tool_state(v->data);    break;
        case K_DATETIME: *(int64_t *)dst =
                             (*(UA_DateTime *)v->data - UA_DATETIME_UNIX_EPOCH) * 100;
                         break;
        case K_STRING: {
            const UA_String *str = v->data;
            size_t len = str->length < CNC_STR_LEN - 1 ? str->length : CNC_STR_LEN - 1;
            memcpy(dst, str->data, len);
            dst[len] = '\0';
            break;
        }
        }
    }
    UA_ReadResponse_clear(&resp);
    return st;
}

static void *read_thread(void *unused)
{
    static const char *paths[N_VARS];
    UA_NodeId   nodes[N_VARS];
    UA_Client  *read_client = NULL;
    cnc_plant_t plant;
    (void)unused;

    /* See the note in main.c: this needs PROCMGR_AID_SCHEDULE, which setuid()
     * may already have dropped.  If the call fails, we log and run on the
     * inherited policy: correct, just not the priority we asked for. */
    struct sched_param sp = { .sched_priority = cfg.read_prio };
    int prc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (prc != 0)
        CNC_LOG("WARN", "read_thread: SCHED_FIFO/%d not applied: %s",
                cfg.read_prio, strerror(prc));

    for (size_t i = 0; i < N_VARS; i++)
        paths[i] = var_def[i].path;
    memset(&plant, 0, sizeof plant);

    while (!atomic_load_explicit(&stopping, memory_order_acquire)) {
        if (!read_client) {
            read_client = ua_new();
            memset(nodes, 0, sizeof nodes);
            if (!read_client || ua_open(read_client, N_VARS, paths, nodes) != UA_STATUSCODE_GOOD) {
                ua_close(&read_client, nodes, N_VARS);
                sleep(RECONNECT_S);
                continue;
            }
            CNC_LOG("INFO", "read session up (%s)", cfg.url);
        }

        if (read_all(read_client, nodes, &plant) == UA_STATUSCODE_GOOD) {
            plant.hdr.connected = 1;
            publish(&plant);
            usleep(cfg.read_period_ms * 1000);
        } else {
            CNC_LOG("WARN", "read session down");
            ua_close(&read_client, nodes, N_VARS);
            plant.hdr.connected = 0;       /* keep the last values, mark them */
            publish(&plant);
            sleep(RECONNECT_S);
        }
    }
    ua_close(&read_client, nodes, N_VARS);  /* clean session close on shutdown */
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Method calls (write session)                                       */
/* ------------------------------------------------------------------ */

static UA_Client      *write_client;
static UA_NodeId       write_method_nodes[N_METHOD_NODES];
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;

/* Serialises method calls; held across the whole network round trip,
 * so shutdown may wait up to REQUEST_TIMEOUT_MS for it. */
int opcua_client_call(cnc_method_t m, int32_t arg)
{
    UA_Variant    in, *out = NULL;
    size_t        nout = 0, nin = (m == CNC_CHANGE_TOOL) ? 1 : 0;
    UA_StatusCode st;

    if ((unsigned)m >= CNC_METHOD_COUNT)
        return EINVAL;
    if (m == CNC_CHANGE_TOOL && (arg < 1 || arg > 99)) {
        CNC_LOG("WARN", "ChangeTool: tool number %d out of range [1,99]", arg);
        return EINVAL;
    }

    pthread_mutex_lock(&write_lock);
    if (atomic_load_explicit(&stopping, memory_order_acquire)) {
        pthread_mutex_unlock(&write_lock);
        return ECANCELED;
    }

    if (!write_client) {                   /* connect on demand */
        write_client = ua_new();
        memset(write_method_nodes, 0, sizeof write_method_nodes);
        if (!write_client || ua_open(write_client, N_METHOD_NODES, method_path, write_method_nodes)
                       != UA_STATUSCODE_GOOD) {
            ua_close(&write_client, write_method_nodes, N_METHOD_NODES);
            pthread_mutex_unlock(&write_lock);
            return EIO;
        }
        CNC_LOG("INFO", "write session up (%s)", cfg.url);
    }

    UA_Variant_setScalar(&in, &arg, &UA_TYPES[UA_TYPES_INT32]);
    st = UA_Client_call(write_client, write_method_nodes[0], write_method_nodes[1 + m],
                        nin, nin ? &in : NULL, &nout, &out);
    UA_Array_delete(out, nout, &UA_TYPES[UA_TYPES_VARIANT]);

    if (st != UA_STATUSCODE_GOOD) {
        int rc = to_errno(st);
        /* Session/connection errors: tear down, next call reconnects.
         * Method-level errors (BadUserAccessDenied, BadNotExecutable,
         * BadInvalidArgument, BadTypeMismatch): session is fine, keep it. */
        if (rc == EIO || rc == ETIMEDOUT) {
            CNC_LOG("WARN", "write session: %s -> tearing down", UA_StatusCode_name(st));
            ua_close(&write_client, write_method_nodes, N_METHOD_NODES);
        }
    }
    pthread_mutex_unlock(&write_lock);
    return to_errno(st);
}

/* ------------------------------------------------------------------ */
/* Start / stop                                                       */
/* ------------------------------------------------------------------ */

int opcua_client_start(const opcua_cfg_t *c)
{
    if (!c || !c->url)
        return EINVAL;
    cfg = *c;
    atomic_store_explicit(&stopping, 0, memory_order_release);
    return pthread_create(&read_tid, NULL, read_thread, NULL);
}

void opcua_client_stop(void)
{
    atomic_store_explicit(&stopping, 1, memory_order_release);
    pthread_join(read_tid, NULL);          /* read_thread closes its own session */
    pthread_mutex_lock(&write_lock);       /* waits for a call in progress */
    ua_close(&write_client, write_method_nodes, N_METHOD_NODES);
    pthread_mutex_unlock(&write_lock);
    CNC_LOG("INFO", "OPC UA sessions closed");
}
