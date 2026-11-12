#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

#include "services/settings.h"
#include "services/network.h"
#include "services/display.h"
#include "services/can.h"
#include "services/can_monitor.h"
#include "services/ui.h"
#include "services/clock.h"
#include "services/usb_bridge.h"
#include "drivers/twai_can.h"
#include "drivers/encoder.h"

static const char *TAG = "main";

// ---------------------------------------------------------------------------
// Encoder → LVGL bridge
//
// encoder_read_cb always reports RELEASED / diff=0.  LVGL owns no encoder
// state, so it can never fire spurious clicks when groups are switched.
// All encoder logic lives in ui_encoder_event (runs in the LVGL task via
// lv_async_call, so LVGL widgets may be touched freely there).
// ---------------------------------------------------------------------------

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->enc_diff = 0;
    data->state    = LV_INDEV_STATE_RELEASED;
}

static void on_encoder_event(encoder_event_t event, void *user_data)
{
    display_lock();
    lv_async_call(ui_encoder_event, (void *)(uintptr_t)event);
    display_unlock();
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load settings before any subsystem that depends on them.
    settings_t cfg;
    settings_load(&cfg);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // A missing W5500 or DS3231 must not reboot-loop the device: log the error,
    // keep going without that chip, and show it on screen once the UI is up.
    esp_err_t eth_err = network_init();
    if (eth_err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet unavailable (%s), continuing without it", esp_err_to_name(eth_err));
    }

    // Apply static IP immediately if configured; DHCP is already the netif default.
    if (eth_err == ESP_OK && !cfg.net_dhcp) {
        network_ip_cfg_t net_cfg = { .dhcp = false };
        memcpy(net_cfg.ip,   cfg.net_ip,   4);
        memcpy(net_cfg.mask, cfg.net_mask, 4);
        memcpy(net_cfg.gw,   cfg.net_gw,   4);
        network_apply_ip(&net_cfg);
    }

    can_mon_init();   // must be before any task that calls can_mon_push

    twai_node_handle_t can_node = NULL;
    ESP_ERROR_CHECK(twai_can_init(&can_node));
    ESP_ERROR_CHECK(can_service_init(can_node));

    esp_err_t usb_err = usb_bridge_init();   // CAN <-> USB on the built-in USB port
    if (usb_err != ESP_OK) {
        ESP_LOGE(TAG, "USB bridge unavailable (%s), continuing without it", esp_err_to_name(usb_err));
    }

    // The clock task is driven by the DS3231's 1 Hz output, so without the RTC
    // there is no clock, NTP or CAN time broadcast; the UI still works.
    esp_err_t rtc_err = clock_service_init();
    if (rtc_err != ESP_OK) {
        ESP_LOGE(TAG, "RTC unavailable (%s), clock disabled", esp_err_to_name(rtc_err));
    }

    ESP_ERROR_CHECK(encoder_init(on_encoder_event, NULL));

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(display_init(&disp));

    display_lock();

    lv_indev_t *enc_indev = lv_indev_create();
    lv_indev_set_type(enc_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(enc_indev, encoder_read_cb);

    ESP_ERROR_CHECK(ui_init(enc_indev, &cfg));
    ui_show_init_errors(eth_err != ESP_OK, rtc_err != ESP_OK);

    display_unlock();
}
