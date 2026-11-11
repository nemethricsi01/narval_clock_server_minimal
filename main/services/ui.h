#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"
#include "services/settings.h"

/**
 * Create all screens, set up groups, and load the main screen.
 * Must be called inside display_lock() / display_unlock().
 * Populates all widgets from the provided settings (loaded from NVS by the caller).
 */
esp_err_t ui_init(lv_indev_t *encoder_indev, const settings_t *s);

/** Open the menu and reset focus to the first item (entry from long-press). */
void ui_open_menu(void);

/** Return to the menu, preserving the current focus (exit from a submenu). */
void ui_show_menu(void);

/** Switch back to the main screen. */
void ui_show_main(void);

/**
 * Deliver one raw encoder event to the UI.  Must be called via lv_async_call
 * so it executes inside the LVGL task.
 * Usage: lv_async_call(ui_encoder_event, (void *)(uintptr_t)event);
 */
void ui_encoder_event(void *event_as_ptr);

/**
 * Show a red "... init failed" line on the main screen for hardware that
 * failed to initialise. Call after ui_init(), inside display_lock().
 */
void ui_show_init_errors(bool eth_failed, bool rtc_failed);

/* ---------- live-data update functions ---------- */
void ui_set_ip(const char *ip);
void ui_set_time(const char *time_str);
void ui_set_date(const char *date_str);
void ui_set_last_sync(const char *ntp_time, const char *can_time);

typedef enum {
    UI_CONN_DISCONNECTED = 0,
    UI_CONN_LAN,
    UI_CONN_WIFI,
} ui_connection_state_t;

void ui_set_connection_state(ui_connection_state_t state);

/** Signal an NTP sync failure (true = timed out, false = recovered). */
void ui_set_ntp_error(bool error);

/* ---------- settings readback ---------- */

/** Returns the currently selected CAN update interval in seconds. */
uint32_t ui_get_update_rate_sec(void);

/** Fills out with the configured NTP server hostname or dotted-decimal IP. */
void ui_get_ntp_server_str(char *out, size_t len);

/** Returns the currently selected NTP update interval in seconds. */
uint32_t ui_get_ntp_update_rate_sec(void);

/** Returns the DST mode: 0 = Winter, 1 = Summer, 2 = Auto (EU rules). */
int ui_get_dst_mode(void);

/** Returns the UTC timezone offset in hours: -12 to +12. */
int ui_get_tz_offset_hours(void);

/** Returns the DS3231 aging offset: -128..+127 (one LSB ≈ 0.1 ppm at 25°C). */
int8_t ui_get_rtc_trim(void);

/**
 * Network configuration as set by the user via the Network screen.
 * Read after the user presses Apply (ui_net_config_t.applied will be true).
 *
 * ntp_idx 0-3: predefined server (see NTP_SERVER_NAMES in ui.c).
 * ntp_idx   4: manual — use ntp_ip as the server address.
 */
typedef struct {
    bool    dhcp;
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
    int     ntp_idx;     // 0-3 predefined, 4 manual
    uint8_t ntp_ip[4];  // valid when ntp_idx == 4
    bool    applied;
} ui_net_config_t;

void ui_get_net_config(ui_net_config_t *out);
