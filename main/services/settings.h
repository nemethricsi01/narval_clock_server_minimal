#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// The persisted settings. At runtime the live copy is in config.c (config_get /
// config_set); this file only loads and saves it. The index fields select from
// the choice tables in config.c.
typedef struct {
    // Network
    bool    net_dhcp;
    uint8_t net_ip[4];
    uint8_t net_mask[4];
    uint8_t net_gw[4];
    int     ntp_idx;        // 0-3 predefined server, 4 = manual (ntp_ip)
    uint8_t ntp_ip[4];      // manual NTP IP
    // Display
    int     brightness_pct; // SETTINGS_BRIGHTNESS_MIN..100
    // CAN
    int     rate_idx;       // CAN time broadcast interval; last = Never
    // NTP
    int     ntp_rate_idx;
    // Time
    int     dst_idx;        // 0=Winter, 1=Summer, 2=Auto
    int     tz_idx;         // 0=UTC-12 … 12=UTC … 24=UTC+12
    int8_t  rtc_trim;       // DS3231 aging offset register, -128..+127
} settings_t;

// Number of choices for each index field (checked against the tables in config.c).
#define SETTINGS_CAN_RATE_COUNT    11
#define SETTINGS_NTP_RATE_COUNT     5
#define SETTINGS_NTP_SERVER_COUNT   5
#define SETTINGS_DST_COUNT          3
#define SETTINGS_TZ_COUNT          25
#define SETTINGS_BRIGHTNESS_MIN    20

// Defaults: DHCP, UTC+1, DST auto, CAN 1 s, NTP 5 min, brightness 100%, pool.ntp.org
#define SETTINGS_DEFAULT ((settings_t){  \
    .net_dhcp       = true,              \
    .net_ip         = {192, 168, 1, 100},\
    .net_mask       = {255, 255, 255, 0},\
    .net_gw         = {192, 168, 1,   1},\
    .ntp_idx        = 0,                 \
    .ntp_ip         = {216, 239, 35,  0},\
    .brightness_pct = 100,               \
    .rate_idx       = 0,                 \
    .ntp_rate_idx   = 0,                 \
    .dst_idx        = 2,                 \
    .tz_idx         = 13,                \
    .rtc_trim       = 0,                 \
})

esp_err_t settings_load(settings_t *s);
esp_err_t settings_save(const settings_t *s);
