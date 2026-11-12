#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "services/settings.h"

/*
 * Live settings: the one copy of settings_t the firmware runs on. The screen,
 * the clock and the web server read it with config_get() and change it with
 * config_set(), which also saves it to NVS and applies what needs applying.
 * Everything here is safe to call from any task.
 */

/** Loads the saved settings (defaults for anything missing). Call once, first. */
void config_init(void);

/** Copies the current settings into out. */
void config_get(settings_t *out);

/**
 * Replaces the current settings and saves them to NVS. An out-of-range field
 * keeps its current value. Applies the brightness, and the DHCP/static IP config
 * if a network field changed. Returns the NVS error if saving failed; the new
 * settings are in use either way, until the next reboot.
 */
esp_err_t config_set(const settings_t *s);

/** Changes on every config_set(), so a reader can tell the settings changed. */
uint32_t config_version(void);

/** Applies the saved static IP, if one is set (DHCP is the default at boot). */
void config_apply_network(void);

/* ---- Values derived from settings ---- */

uint32_t config_can_rate_sec(const settings_t *s);    // 0 = Never
uint32_t config_ntp_rate_sec(const settings_t *s);
int      config_tz_offset_hours(const settings_t *s); // -12..+12

/** The NTP server name, or the manual IP in dotted-decimal form. */
void config_ntp_server(const settings_t *s, char *out, size_t len);

/* ---- Labels for each choice, indexed like the settings_t fields ---- */

extern const char *const config_can_rate_labels[];    // SETTINGS_CAN_RATE_COUNT
extern const char *const config_ntp_rate_labels[];    // SETTINGS_NTP_RATE_COUNT
extern const char *const config_ntp_server_labels[];  // SETTINGS_NTP_SERVER_COUNT, last = manual IP
extern const char *const config_dst_labels[];         // SETTINGS_DST_COUNT
extern const char *const config_tz_labels[];          // SETTINGS_TZ_COUNT
