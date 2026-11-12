#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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
#include "services/discovery.h"
#include "drivers/twai_can.h"
#include "drivers/encoder.h"

static const char *TAG = "main";

// ---------------------------------------------------------------------------
// Encoder → LVGL bridge
//
// encoder_read_cb always reports RELEASED / diff=0.  LVGL owns no encoder
// state, so it can never fire spurious clicks when groups are switched.
// All encoder logic lives in ui_encoder_event, which enc_queue_drain_cb calls
// in the LVGL task, so LVGL widgets may be touched freely there.
// ---------------------------------------------------------------------------

#define ENC_QUEUE_LEN       16
#define ENC_POLL_PERIOD_MS  10

static QueueHandle_t s_enc_queue;

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->enc_diff = 0;
    data->state    = LV_INDEV_STATE_RELEASED;
}

// Runs in the esp_timer task, which also polls the knob and button and drives
// the LVGL tick and the clock's fallback 1 Hz timer: never block here (waiting
// for the display lock while LVGL renders stalled all of them). If the queue
// is full the event is dropped.
static void on_encoder_event(encoder_event_t event, void *user_data)
{
    xQueueSend(s_enc_queue, &event, 0);
}

// lv_timer, so it runs in the LVGL task: hands queued encoder events to the UI.
static void enc_queue_drain_cb(lv_timer_t *timer)
{
    encoder_event_t event;
    while (xQueueReceive(s_enc_queue, &event, 0) == pdTRUE) {
        ui_encoder_event(event);
    }
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

    // Finds PIC32 boards with the Announce service (Menu > Find devices).
    esp_err_t disc_err = discovery_init();
    if (disc_err != ESP_OK) {
        ESP_LOGE(TAG, "device discovery unavailable (%s)", esp_err_to_name(disc_err));
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

    // Without the DS3231 the clock runs on an internal 1 Hz timer and NTP time.
    esp_err_t clk_err = clock_service_init();
    if (clk_err != ESP_OK) {
        ESP_LOGE(TAG, "clock service failed (%s), continuing without it", esp_err_to_name(clk_err));
    }

    s_enc_queue = xQueueCreate(ENC_QUEUE_LEN, sizeof(encoder_event_t));
    assert(s_enc_queue);
    ESP_ERROR_CHECK(encoder_init(on_encoder_event, NULL));

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(display_init(&disp));

    display_lock();

    lv_indev_t *enc_indev = lv_indev_create();
    lv_indev_set_type(enc_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(enc_indev, encoder_read_cb);

    ESP_ERROR_CHECK(ui_init(enc_indev, &cfg));
    ui_show_init_errors(eth_err != ESP_OK, !clock_has_rtc());

    xQueueReset(s_enc_queue);   // drop knob turns made during boot, before the UI was up
    lv_timer_create(enc_queue_drain_cb, ENC_POLL_PERIOD_MS, NULL);

    display_unlock();
}
