#include "services/config.h"
#include "services/display.h"
#include "services/network.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "config";

#define COUNT(a)  (sizeof(a) / sizeof((a)[0]))

// ---------------------------------------------------------------------------
// Choices. New entries go at the end, so saved indices keep their meaning.
// ---------------------------------------------------------------------------

static const uint32_t s_can_rate_sec[] = { 1, 60, 120, 300, 600, 1800, 3600, 14400, 43200, 86400, 0 };
const char *const config_can_rate_labels[] = {
    "1 sec", "1 min", "2 min", "5 min", "10 min", "30 min",
    "1 hour", "4 hours", "12 hours", "24 hours", "Never",
};

static const uint32_t s_ntp_rate_sec[] = { 300, 1800, 7200, 43200, 86400 };
const char *const config_ntp_rate_labels[] = { "5 min", "30 min", "2 hours", "12 hours", "24 hours" };

const char *const config_ntp_server_labels[] = {
    "pool.ntp.org", "time.google.com", "time.cloudflare.com", "time.windows.com", "Manual NTP IP",
};

const char *const config_dst_labels[] = { "Winter", "Summer", "Auto" };

const char *const config_tz_labels[] = {   // index 0 = UTC-12, 12 = UTC, 24 = UTC+12
    "UTC-12", "UTC-11", "UTC-10", "UTC-9",  "UTC-8",  "UTC-7",  "UTC-6",
    "UTC-5",  "UTC-4",  "UTC-3",  "UTC-2",  "UTC-1",  "UTC",    "UTC+1 (CET)",
    "UTC+2",  "UTC+3",  "UTC+4",  "UTC+5",  "UTC+6",  "UTC+7",  "UTC+8",
    "UTC+9",  "UTC+10", "UTC+11", "UTC+12",
};

_Static_assert(COUNT(s_can_rate_sec) == SETTINGS_CAN_RATE_COUNT &&
               COUNT(config_can_rate_labels) == SETTINGS_CAN_RATE_COUNT, "CAN rate tables");
_Static_assert(COUNT(s_ntp_rate_sec) == SETTINGS_NTP_RATE_COUNT &&
               COUNT(config_ntp_rate_labels) == SETTINGS_NTP_RATE_COUNT, "NTP rate tables");
_Static_assert(COUNT(config_ntp_server_labels) == SETTINGS_NTP_SERVER_COUNT, "NTP server table");
_Static_assert(COUNT(config_dst_labels) == SETTINGS_DST_COUNT, "DST table");
_Static_assert(COUNT(config_tz_labels) == SETTINGS_TZ_COUNT, "timezone table");

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static settings_t        s_cfg;
static uint32_t          s_version;
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;  // guards s_cfg and s_version
static SemaphoreHandle_t s_write_mutex;  // one config_set() at a time, so saves and applies stay in order

void config_init(void)
{
    s_write_mutex = xSemaphoreCreateMutex();
    assert(s_write_mutex);
    settings_load(&s_cfg);   // gives the defaults for anything missing or out of range
    s_version = 1;
}

void config_get(settings_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t config_version(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t v = s_version;
    portEXIT_CRITICAL(&s_lock);
    return v;
}

static int index_or(int v, int count, int fallback)
{
    return (v >= 0 && v < count) ? v : fallback;
}

static void apply_network(const settings_t *s)
{
    network_ip_cfg_t net = { .dhcp = s->net_dhcp };
    memcpy(net.ip,   s->net_ip,   4);
    memcpy(net.mask, s->net_mask, 4);
    memcpy(net.gw,   s->net_gw,   4);
    esp_err_t err = network_apply_ip(&net);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   // INVALID_STATE: no Ethernet at all
        ESP_LOGW(TAG, "network config not applied: %s", esp_err_to_name(err));
    }
}

// True if switching from a to b needs the network reconfigured.
static bool network_changed(const settings_t *a, const settings_t *b)
{
    if (a->net_dhcp != b->net_dhcp) return true;
    return !b->net_dhcp && (memcmp(a->net_ip,   b->net_ip,   4) ||
                            memcmp(a->net_mask, b->net_mask, 4) ||
                            memcmp(a->net_gw,   b->net_gw,   4));
}

esp_err_t config_set(const settings_t *in)
{
    xSemaphoreTake(s_write_mutex, portMAX_DELAY);

    settings_t old, s = *in;
    config_get(&old);
    s.ntp_idx      = index_or(s.ntp_idx,      SETTINGS_NTP_SERVER_COUNT, old.ntp_idx);
    s.rate_idx     = index_or(s.rate_idx,     SETTINGS_CAN_RATE_COUNT,   old.rate_idx);
    s.ntp_rate_idx = index_or(s.ntp_rate_idx, SETTINGS_NTP_RATE_COUNT,   old.ntp_rate_idx);
    s.dst_idx      = index_or(s.dst_idx,      SETTINGS_DST_COUNT,        old.dst_idx);
    s.tz_idx       = index_or(s.tz_idx,       SETTINGS_TZ_COUNT,         old.tz_idx);
    if (s.brightness_pct < SETTINGS_BRIGHTNESS_MIN || s.brightness_pct > 100) {
        s.brightness_pct = old.brightness_pct;
    }

    portENTER_CRITICAL(&s_lock);
    s_cfg = s;
    s_version++;
    portEXIT_CRITICAL(&s_lock);

    // The clock picks up the rest itself on its next tick.
    if (s.brightness_pct != old.brightness_pct) display_set_brightness((uint8_t)s.brightness_pct);
    if (network_changed(&old, &s)) apply_network(&s);
    esp_err_t err = settings_save(&s);

    xSemaphoreGive(s_write_mutex);
    return err;
}

void config_apply_network(void)
{
    settings_t s;
    config_get(&s);
    if (!s.net_dhcp) apply_network(&s);
}

// ---------------------------------------------------------------------------
// Derived values (s comes from config_get(), so its indices are valid)
// ---------------------------------------------------------------------------

uint32_t config_can_rate_sec(const settings_t *s)    { return s_can_rate_sec[s->rate_idx]; }
uint32_t config_ntp_rate_sec(const settings_t *s)    { return s_ntp_rate_sec[s->ntp_rate_idx]; }
int      config_tz_offset_hours(const settings_t *s) { return s->tz_idx - 12; }

void config_ntp_server(const settings_t *s, char *out, size_t len)
{
    if (s->ntp_idx < SETTINGS_NTP_SERVER_COUNT - 1) {
        snprintf(out, len, "%s", config_ntp_server_labels[s->ntp_idx]);
    } else {
        snprintf(out, len, "%d.%d.%d.%d", s->ntp_ip[0], s->ntp_ip[1], s->ntp_ip[2], s->ntp_ip[3]);
    }
}
