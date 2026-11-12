#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"
#include "drivers/encoder.h"

/**
 * Create all screens, set up groups, and load the main screen.
 * Must be called inside display_lock() / display_unlock(), after config_init().
 * The settings screens read and write the live settings in config.c.
 */
esp_err_t ui_init(lv_indev_t *encoder_indev);

/** Open the menu and reset focus to the first item (entry from long-press). */
void ui_open_menu(void);

/** Return to the menu, preserving the current focus (exit from a submenu). */
void ui_show_menu(void);

/** Switch back to the main screen. */
void ui_show_main(void);

/** Deliver one raw encoder event to the UI. Call from the LVGL task only. */
void ui_encoder_event(encoder_event_t event);

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
