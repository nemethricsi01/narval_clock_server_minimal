#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    // Network
    bool    net_dhcp;
    uint8_t net_ip[4];
    uint8_t net_mask[4];
    uint8_t net_gw[4];
    int     ntp_idx;        // 0-3 predefined, 4 manual
    uint8_t ntp_ip[4];      // manual NTP IP
    // Display
    int     brightness_pct;
    // CAN
    int     rate_idx;
    // NTP
    int     ntp_rate_idx;
    // Time
    int     dst_idx;        // 0=Winter, 1=Summer, 2=Auto
    int     tz_idx;         // 0=UTC-12 … 12=UTC … 24=UTC+12
    int8_t  rtc_trim;       // DS3231 aging offset register, -128..+127
} settings_t;

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
