/*
 * opcua_cnc_map.h - public interface of the CNC driver.
 *
 *   read()  on CNC_PATH_PLANT    -> one cnc_plant_t: every value from the same reading
 *   write() on CNC_PATH_METHODS  -> one cnc_cmd_t:   run a method on the machine
 *
 * CNC_PATH_PLANT behaves like a file holding one snapshot: read() at offset 0
 * returns it, the next read() returns 0 (EOF). To poll with an open fd, use
 *   pread(fd, &p, sizeof p, 0)   -> always the latest snapshot.
 * The buffer must be sizeof(cnc_plant_t), otherwise EINVAL.
 *
 * Commands are queued and run one after another, in order.  write() returns
 * sizeof(cnc_cmd_t) once the machine has executed the method, or -1 with errno:
 *
 *   EBUSY      the command queue is full: retry later
 *   EIO        no link to the machine
 *   ETIMEDOUT  the machine did not answer in time
 *   EACCES     the machine refused the method
 *   EINVAL     malformed command
 *   ECANCELED  the driver is shutting down
 *   EINTR      the caller was interrupted while waiting.
 *              If its command had not started yet it will not run.
 *              If it was already running on the machine it still runs to
 *              completion and cannot be undone.  Callers of non-idempotent
 *              methods (CHANGE_TOOL, ESTOP) must treat EINTR as "may or may
 *              not have executed" and must not blindly retry.
 */
#ifndef OPCUA_CNC_MAP_H
#define OPCUA_CNC_MAP_H

#include <stdint.h>

#define CNC_PATH_PLANT    "/dev/cnc/plant"     /* 0444, read only  */
#define CNC_PATH_METHODS  "/dev/cnc/methods"   /* 0220, write only */

#define CNC_STR_LEN 64

/* ------------------------------------------------------------------ */
/* States                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    CNC_MACHINE_UNKNOWN     = 0,
    CNC_MACHINE_IDLE        = 1,
    CNC_MACHINE_RUNNING     = 2,
    CNC_MACHINE_ALARM       = 3,
    CNC_MACHINE_MAINTENANCE = 4,
    CNC_MACHINE_SETUP       = 5
} cnc_machine_state_t;

typedef enum {
    CNC_TOOL_UNKNOWN = 0,
    CNC_TOOL_NEW     = 1,
    CNC_TOOL_GOOD    = 2,
    CNC_TOOL_WORN    = 3,
    CNC_TOOL_BROKEN  = 4
} cnc_tool_state_t;

/* ------------------------------------------------------------------ */
/* Plant data (read)                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t seq;              /* increases with every new reading            */
    int64_t  plant_time_ns;    /* plant "Timestamp", ns since 1970-01-01      */
    uint32_t connected;        /* 1 = driver connected to the plant           */
    uint32_t _pad;
} cnc_hdr_t;

typedef struct {
    uint32_t machine_state;    /* cnc_machine_state_t */
    uint32_t _pad;
} cnc_state_t;

typedef struct {
    double speed_rpm, load_pct, torque_nm, power_kw, temperature_c;
} cnc_spindle_t;

typedef struct {
    double rate_mm_min, override_pct, x_mm, y_mm, z_mm;
} cnc_feed_t;

typedef struct {
    int32_t  number;
    uint32_t tool_state;       /* cnc_tool_state_t */
    double   life_remaining_pct, wear_x_mm, wear_z_mm;
} cnc_tool_t;

typedef struct {
    double x_mm_s, y_mm_s, z_mm_s, overall_mm_s;
} cnc_vibration_t;

typedef struct {
    int32_t parts_produced, good_parts, rejected_parts, _pad;
    double  cycle_time_s, efficiency_pct;
} cnc_production_t;

typedef struct {
    double coolant_level_pct, coolant_temperature_c, air_pressure_bar, hydraulic_pressure_bar;
} cnc_aux_t;

typedef struct {
    char manufacturer[CNC_STR_LEN];
    char model[CNC_STR_LEN];
    char serial_number[CNC_STR_LEN];
} cnc_info_t;

/* What read() on CNC_PATH_PLANT returns. */
typedef struct {
    cnc_hdr_t        hdr;
    cnc_state_t      state;
    cnc_spindle_t    spindle;
    cnc_feed_t       feed;
    cnc_tool_t       tool;
    cnc_vibration_t  vibration;
    cnc_production_t production;
    cnc_aux_t        aux;
    cnc_info_t       info;
} cnc_plant_t;

/* ------------------------------------------------------------------ */
/* Methods (write)                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    CNC_ESTOP,
    CNC_RESET_COUNTERS,
    CNC_CHANGE_TOOL,           /* arg: tool number */
    CNC_METHOD_COUNT           /* sentinel, not a real method */
} cnc_method_t;

/* What write() on CNC_PATH_METHODS takes. */
typedef struct {
    uint32_t method;           /* cnc_method_t */
    int32_t  arg;              /* used by CNC_CHANGE_TOOL */
} cnc_cmd_t;

#endif /* OPCUA_CNC_MAP_H */
