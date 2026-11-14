#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PRINTER_CONN_NOT_CONFIGURED = 0,
    PRINTER_CONN_CONNECTING,
    PRINTER_CONN_SUBSCRIBING,
    PRINTER_CONN_WAITING_DATA,
    PRINTER_CONN_ONLINE,
    PRINTER_CONN_TLS_ERROR,
    PRINTER_CONN_NETWORK_ERROR,
    PRINTER_CONN_INVALID_DATA
} printer_connection_state_t;

typedef struct {
    bool configured;
    bool connected;
    printer_connection_state_t connection_state;
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

/* Diagnostic state for on-device screen; does not expose LAN access code. */
const char *printer_monitor_status_text(printer_connection_state_t state);

/* Called once a second to refresh stale printers. */
void printer_monitor_poll(void);
