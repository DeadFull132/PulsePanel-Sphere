#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool configured;
    bool connected;
    int progress;           /* -1 when no data */
    int minutes_left;       /* -1 when no data */
    int nozzle_x10;         /* -1 when no data */
    int bed_x10;            /* -1 when no data */
    int layer_current;
    int layer_total;
    int tray_now;
    char state[24];
    char filename[80];
    int64_t last_report_us;
} printer_snapshot_t;

/* Stores only IP, serial and LAN code. Does not send print-control commands. */
void printer_monitor_init(void);
void printer_monitor_on_wifi(void);
void printer_monitor_offline(void);
void printer_monitor_copy(printer_snapshot_t *out);
bool printer_monitor_store(const char *address, const char *serial, const char *access_code);
bool printer_monitor_configured(void);
void printer_monitor_forget(void);
