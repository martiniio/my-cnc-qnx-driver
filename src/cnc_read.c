/*
 * cnc_read.c – live tree view of /dev/cnc/plant, with commands
 *
 * Run:  ./cnc_read         live view + command keys
 *       ./cnc_read -1      one shot (no keys)
 *       ./cnc_read -b N    benchmark: N reads, latency min/avg/p99/max
 *
 * The live view refreshes every REFRESH_MS milliseconds.
 *
 * Keys (live view):
 *   e      EmergencyStop
 *   r      ResetProductionCounters
 *   1..9   ChangeTool(n)
 *   q      quit
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>

#define REFRESH_MS 100

#include "opcua_cnc_map.h"

/* ------------------------------------------------------------------ */
/* terminal state                                                      */
/* ------------------------------------------------------------------ */

static int            g_alt  = 0;
static int            g_raw  = 0;
static int            g_rows = 24;
static int            g_cols = 80;
static struct termios g_saved_tio;

static void query_term(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) g_rows = ws.ws_row;
        if (ws.ws_col > 0) g_cols = ws.ws_col;
    }
}

/* keys arrive one at a time, without Enter and without echo */
static void enter_raw(void)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &g_saved_tio) == 0) {
        t = g_saved_tio;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN]  = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        g_raw = 1;
    }
}

static void restore_term(void)
{
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tio);
        g_raw = 0;
    }
    if (g_alt) {
        fputs("\033[?1049l", stdout);
        fflush(stdout);
        g_alt = 0;
    }
}

static void on_signal(int sig)
{
    (void)sig;
    restore_term();
    _exit(0);
}

/* ------------------------------------------------------------------ */
/* width-aware line output                                             */
/* ------------------------------------------------------------------ */

/*
 * Never write more than g_cols bytes before the newline, so the terminal
 * never wraps and therefore never scrolls on us.
 */
static void out_fmt(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if (n > g_cols) n = g_cols;
    fwrite(buf, 1, (size_t)n, stdout);
    fputc('\n', stdout);
}

/* ------------------------------------------------------------------ */
/* labels                                                              */
/* ------------------------------------------------------------------ */

static const char *state_str(uint32_t s)
{
    switch (s) {
    case CNC_MACHINE_IDLE:        return "Idle";
    case CNC_MACHINE_RUNNING:     return "Running";
    case CNC_MACHINE_ALARM:       return "Alarm";
    case CNC_MACHINE_MAINTENANCE: return "Maintenance";
    case CNC_MACHINE_SETUP:       return "Setup";
    default:                      return "Unknown";
    }
}

static const char *tool_str(uint32_t s)
{
    switch (s) {
    case CNC_TOOL_NEW:    return "New";
    case CNC_TOOL_GOOD:   return "Good";
    case CNC_TOOL_WORN:   return "Worn";
    case CNC_TOOL_BROKEN: return "Broken";
    default:              return "Unknown";
    }
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static char g_status[128] = "keys: e=estop  r=reset  1-9=change tool  q=quit";

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

/* Send one command; result and round-trip time go into the status line. */
static void send_cmd(uint32_t method, int32_t arg, const char *label)
{
    cnc_cmd_t cmd = { method, arg };
    int fd = open(CNC_PATH_METHODS, O_WRONLY);

    if (fd == -1) {
        snprintf(g_status, sizeof g_status, "%s: open: %s", label, strerror(errno));
        return;
    }
    double t0 = now_us();
    ssize_t n = write(fd, &cmd, sizeof cmd);
    double ms = (now_us() - t0) / 1e3;

    if (n == (ssize_t)sizeof cmd)
        snprintf(g_status, sizeof g_status, "%s: OK (%.1f ms)", label, ms);
    else
        snprintf(g_status, sizeof g_status, "%s: %s (%.1f ms)", label, strerror(errno), ms);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* benchmark: latency of the read path                                 */
/* ------------------------------------------------------------------ */

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int bench(int fd, long n)
{
    cnc_plant_t p;
    double     *t = malloc((size_t)n * sizeof *t);
    double      sum = 0;

    if (!t || n < 1)
        return EXIT_FAILURE;
    for (long i = 0; i < n; i++) {
        double t0 = now_us();
        if (pread(fd, &p, sizeof p, 0) != (ssize_t)sizeof p) {
            perror("pread");
            free(t);
            return EXIT_FAILURE;
        }
        t[i] = now_us() - t0;
        sum += t[i];
    }
    qsort(t, (size_t)n, sizeof *t, cmp_double);
    long i99 = (long)(n * 0.99);
    if (i99 >= n) i99 = n - 1;
    printf("%ld reads of %zu bytes: min %.1f us  avg %.1f us  p99 %.1f us  max %.1f us\n",
           n, sizeof p, t[0], sum / n, t[i99], t[n - 1]);
    free(t);
    return EXIT_SUCCESS;
}

/* Returns 0 to quit. */
static int handle_key(int c)
{
    char label[32];

    switch (c) {
    case 'q':
        return 0;
    case 'e':
        send_cmd(CNC_ESTOP, 0, "EmergencyStop");
        break;
    case 'r':
        send_cmd(CNC_RESET_COUNTERS, 0, "ResetProductionCounters");
        break;
    default:
        if (c >= '1' && c <= '9') {
            snprintf(label, sizeof label, "ChangeTool(%c)", c);
            send_cmd(CNC_CHANGE_TOOL, c - '0', label);
        }
        break;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* frame renderer                                                      */
/* ------------------------------------------------------------------ */

static void render(const cnc_plant_t *p, int with_status)
{
    query_term();               /* re-query every frame: handles resize */

    /* home, clear from cursor down; \r makes sure we're at column 0 */
    fputs("\033[H\r\033[J", stdout);

    out_fmt("plant");
    out_fmt("  header       seq=%llu conn=%u t=%lldns",
            (unsigned long long)p->hdr.seq,
            p->hdr.connected,
            (long long)p->hdr.plant_time_ns);
    out_fmt("  state        %s", state_str(p->state.machine_state));
    out_fmt("  spindle      rpm=%.1f load=%.1f%% torque=%.2fNm power=%.2fkW temp=%.1fC",
            p->spindle.speed_rpm, p->spindle.load_pct, p->spindle.torque_nm,
            p->spindle.power_kw,  p->spindle.temperature_c);
    out_fmt("  feed         rate=%.1fmm/min ovr=%.1f%% pos=%.3f,%.3f,%.3f",
            p->feed.rate_mm_min, p->feed.override_pct,
            p->feed.x_mm, p->feed.y_mm, p->feed.z_mm);
    out_fmt("  tool         num=%d state=%s life=%.1f%% wear=%.3f,%.3f",
            p->tool.number, tool_str(p->tool.tool_state),
            p->tool.life_remaining_pct, p->tool.wear_x_mm, p->tool.wear_z_mm);
    out_fmt("  vibration    x=%.3f y=%.3f z=%.3f overall=%.3f",
            p->vibration.x_mm_s, p->vibration.y_mm_s,
            p->vibration.z_mm_s, p->vibration.overall_mm_s);
    out_fmt("  production   produced=%d good=%d rej=%d cycle=%.2fs eff=%.1f%%",
            p->production.parts_produced, p->production.good_parts,
            p->production.rejected_parts, p->production.cycle_time_s,
            p->production.efficiency_pct);
    out_fmt("  auxiliary    coolant=%.1f%%@%.1fC air=%.2fbar hyd=%.2fbar",
            p->aux.coolant_level_pct, p->aux.coolant_temperature_c,
            p->aux.air_pressure_bar,  p->aux.hydraulic_pressure_bar);
    out_fmt("  info         %s %s %s",
            p->info.manufacturer, p->info.model, p->info.serial_number);

    if (with_status) {
        out_fmt("");
        out_fmt("%s", g_status);
    }

    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int once = (argc > 1 && strcmp(argv[1], "-1") == 0);

    int fd = open(CNC_PATH_PLANT, O_RDONLY);
    if (fd == -1) {
        perror(CNC_PATH_PLANT);
        return EXIT_FAILURE;
    }

    cnc_plant_t plant;

    if (argc > 2 && strcmp(argv[1], "-b") == 0) {
        int rc = bench(fd, atol(argv[2]));
        close(fd);
        return rc;
    }

    /* If stdout isn't a tty (pipe, file, IDE output pane), ANSI redraw
     * is meaningless — fall back to one plain snapshot. */
    if (!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO))
        once = 1;

    if (!once) {
        fputs("\033[?1049h\033[2J\033[H", stdout);  /* alt screen on */
        fflush(stdout);
        g_alt = 1;
        enter_raw();

        signal(SIGINT,  on_signal);
        signal(SIGTERM, on_signal);
        atexit(restore_term);

        query_term();
    }

    for (;;) {
        ssize_t n = pread(fd, &plant, sizeof plant, 0);
        if (n != (ssize_t)sizeof plant) {
            restore_term();
            if (n == -1) perror("pread");
            else         fprintf(stderr, "short read: %zd bytes\n", n);
            close(fd);
            return EXIT_FAILURE;
        }

        if (once) {
            /* one-shot: use a very wide virtual terminal, no redraw */
            g_cols = 4096;
            render(&plant, 0);
            break;
        }

        render(&plant, 1);

        /* wait for a key, at most REFRESH_MS */
        fd_set rfds;
        struct timeval tv = { 0, REFRESH_MS * 1000 };
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1 && !handle_key(c))
                break;

        }
    }

    restore_term();
    close(fd);
    return EXIT_SUCCESS;
}
