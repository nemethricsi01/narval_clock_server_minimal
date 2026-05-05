#include "services/settings.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
#define NS "settings"

esp_err_t settings_load(settings_t *s)
{
    *s = SETTINGS_DEFAULT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t v;
    size_t  len;

    if (nvs_get_u8(h, "net_dhcp",     &v) == ESP_OK) s->net_dhcp       = v != 0;
    len = 4; nvs_get_blob(h, "net_ip",   s->net_ip,   &len);
    len = 4; nvs_get_blob(h, "net_mask", s->net_mask, &len);
    len = 4; nvs_get_blob(h, "net_gw",   s->net_gw,   &len);
    if (nvs_get_u8(h, "ntp_idx",      &v) == ESP_OK) s->ntp_idx        = v < 5  ? v : 0;
    len = 4; nvs_get_blob(h, "ntp_ip",   s->ntp_ip,   &len);
    if (nvs_get_u8(h, "brightness",   &v) == ESP_OK) s->brightness_pct = (v >= 20 && v <= 100) ? v : 100;
    if (nvs_get_u8(h, "rate_idx",     &v) == ESP_OK) s->rate_idx       = v < 10 ? v : 1;
    if (nvs_get_u8(h, "ntp_rate_idx", &v) == ESP_OK) s->ntp_rate_idx   = v < 5  ? v : 0;
    if (nvs_get_u8(h, "dst_idx",      &v) == ESP_OK) s->dst_idx        = v <= 2 ? v : 0;
    if (nvs_get_u8(h, "tz_idx",       &v) == ESP_OK) s->tz_idx         = v <= 24 ? v : 12;

    nvs_close(h);
    ESP_LOGI(TAG, "loaded");
    return ESP_OK;
}

esp_err_t settings_save(const settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(h,   "net_dhcp",     s->net_dhcp ? 1 : 0);
    nvs_set_blob(h, "net_ip",       s->net_ip,    4);
    nvs_set_blob(h, "net_mask",     s->net_mask,  4);
    nvs_set_blob(h, "net_gw",       s->net_gw,    4);
    nvs_set_u8(h,   "ntp_idx",      (uint8_t)s->ntp_idx);
    nvs_set_blob(h, "ntp_ip",       s->ntp_ip,    4);
    nvs_set_u8(h,   "brightness",   (uint8_t)s->brightness_pct);
    nvs_set_u8(h,   "rate_idx",     (uint8_t)s->rate_idx);
    nvs_set_u8(h,   "ntp_rate_idx", (uint8_t)s->ntp_rate_idx);
    nvs_set_u8(h,   "dst_idx",      (uint8_t)s->dst_idx);
    nvs_set_u8(h,   "tz_idx",       (uint8_t)s->tz_idx);

    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "saved");
    else               ESP_LOGW(TAG, "commit: %s", esp_err_to_name(err));
    return err;
}
