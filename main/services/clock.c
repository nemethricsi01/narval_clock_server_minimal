#include "clock.h"
#include "network.h"
#include "can.h"
#include "can_monitor.h"
#include "board.h"
#include "services/display.h"
#include "services/ui.h"

#include "ds3231.h"
#include "driver/gpio.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <time.h>
#include <string.h>

static const char *TAG = "clock";

static i2c_dev_t         s_rtc;
static SemaphoreHandle_t s_sqw_sem;
static volatile bool     s_time_valid   = false;
static char              s_ntp_server[32];
static volatile time_t   s_ntp_sync_utc = 0;  // frozen UTC timestamp of last NTP sync; 0 = never
static volatile time_t   s_can_sync_utc = 0;  // frozen UTC timestamp of last successful CAN send; 0 = never

// ---------------------------------------------------------------------------
// EU DST helpers
// ---------------------------------------------------------------------------

static int last_sunday_of_month(int year, int month)
{
    // day 0 of (month+1) == last day of month
    struct tm t = { .tm_year = year, .tm_mon = month + 1, .tm_mday = 0 };
    mktime(&t);
    return t.tm_mday - (int)t.tm_wday;
}

static int eu_dst_active(const struct tm *utc)
{
    int m = utc->tm_mon;
    if (m < 2 || m > 9) return 0;
    if (m > 2 && m < 9) return 1;

    int ls = last_sunday_of_month(utc->tm_year, m);
    if (m == 2) {  // March: DST starts last Sunday at 01:00 UTC
        if (utc->tm_mday < ls) return 0;
        if (utc->tm_mday == ls && utc->tm_hour < 1) return 0;
        return 1;
    }
    // October: DST ends last Sunday at 01:00 UTC
    if (utc->tm_mday > ls) return 0;
    if (utc->tm_mday == ls && utc->tm_hour >= 1) return 0;
    return 1;
}

static int effective_dst_hours(const struct tm *utc, int dst_mode)
{
    switch (dst_mode) {
    case 1:  return 1;
    case 2:  return eu_dst_active(utc);
    default: return 0;
    }
}

static struct tm utc_to_local(const struct tm *utc, int tz_offset, int dst_mode)
{
    struct tm local = *utc;
    local.tm_hour += tz_offset + effective_dst_hours(utc, dst_mode);
    local.tm_isdst = 0;
    mktime(&local);
    return local;
}

// ---------------------------------------------------------------------------
// GPIO ISR — fires on each rising edge of the 1 Hz SQW signal
// ---------------------------------------------------------------------------

static void IRAM_ATTR sqw_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_sqw_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

// ---------------------------------------------------------------------------
// CAN transmit — sends DATE then TIME frames every 1 Hz tick
// ---------------------------------------------------------------------------

static bool send_can_datetime(const struct tm *t)
{
    // Static storage: the TWAI driver stores a pointer to data[], not a copy.
    // The buffer must remain valid until the ISR reads it — static guarantees that.
    static can_frame_t date_f = { .header = { .id = 0, .dlc = 4 } };
    static can_frame_t time_f = { .header = { .id = 0, .dlc = 4 } };

    // DATE:  0YYYYYYY 0000MMMM 000DDDDD 00000010
    date_f.data[0] = (uint8_t)((t->tm_year - 100) & 0x7F);
    date_f.data[1] = (uint8_t)((t->tm_mon  + 1)   & 0x0F);
    date_f.data[2] = (uint8_t)(t->tm_mday          & 0x1F);
    date_f.data[3] = 0x02;
    esp_err_t r1 = can_service_send(&date_f, 0);
    if (r1 == ESP_OK) can_mon_push(&date_f, true);

    // TIME:  000HHHHH 00MMMMMM 00SSSSSS 00000001
    time_f.data[0] = (uint8_t)(t->tm_hour & 0x1F);
    // time_f.data[0] |= 0x60; // address 3 for testing 
    time_f.data[1] = (uint8_t)(t->tm_min  & 0x3F);
    time_f.data[2] = (uint8_t)(t->tm_sec  & 0x3F);
    time_f.data[3] = 0x01;
    esp_err_t r2 = can_service_send(&time_f, 0);
    if (r2 == ESP_OK) can_mon_push(&time_f, true);

    return (r1 == ESP_OK) && (r2 == ESP_OK);
}

// ---------------------------------------------------------------------------
// NTP sync callback — runs in the SNTP task context
// ---------------------------------------------------------------------------

static void on_ntp_sync(struct timeval *tv)
{
    // Round to nearest second so the DS3231 countdown aligns with the true
    // second boundary within ±500 ms, preventing a persistent 1-second lag.
    time_t t_sec = tv->tv_sec + (tv->tv_usec >= 500000 ? 1 : 0);
    struct tm t;
    gmtime_r(&t_sec, &t);

    if (ds3231_set_time(&s_rtc, &t) == ESP_OK) {
        s_ntp_sync_utc = t_sec;  // freeze the UTC sync timestamp
        s_time_valid   = true;
        ESP_LOGI(TAG, "synced → RTC set: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        ESP_LOGE(TAG, "RTC write failed after NTP sync");
    }
}

// ---------------------------------------------------------------------------
// Clock task — runs forever, blocked on the 1 Hz semaphore
// ---------------------------------------------------------------------------

static void clock_task(void *arg)
{
    // Wait for DHCP to assign an IP (default network config, no user action needed)
    ESP_LOGI(TAG, "waiting for network...");
    while (!network_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "network up, IP: %s", network_get_ip_str());

    display_lock();
    ui_get_ntp_server_str(s_ntp_server, sizeof(s_ntp_server));
    uint32_t last_ntp_rate = ui_get_ntp_update_rate_sec();
    uint32_t last_can_rate = ui_get_update_rate_sec();
    display_unlock();

    ESP_LOGI(TAG, "starting NTP sync via %s", s_ntp_server);
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
    sntp_cfg.sync_cb = on_ntp_sync;
    esp_netif_sntp_init(&sntp_cfg);
    sntp_set_sync_interval(last_ntp_rate * 1000);

    uint32_t can_ticks         = 0;   // 0 = send on first valid tick
    uint32_t ticks_since_valid = 0;

    // Tick loop: each 1 Hz rising edge gives the semaphore
    while (1) {
        xSemaphoreTake(s_sqw_sem, portMAX_DELAY);
        if (!s_time_valid) continue;
        ticks_since_valid++;

        // Snapshot all mutable UI settings in one lock window so clock_task never
        // races with the LVGL task modifying the same state on the other core.
        display_lock();
        uint32_t cur_ntp_rate = ui_get_ntp_update_rate_sec();
        uint32_t cur_can_rate = ui_get_update_rate_sec();
        int      tz_offset    = ui_get_tz_offset_hours();
        int      dst_mode     = ui_get_dst_mode();
        char     cur_ntp_server[32];
        ui_get_ntp_server_str(cur_ntp_server, sizeof(cur_ntp_server));
        display_unlock();

        // Apply NTP rate change immediately if the user updated it
        if (cur_ntp_rate != last_ntp_rate) {
            sntp_set_sync_interval(cur_ntp_rate * 1000);
            sntp_restart();
            last_ntp_rate = cur_ntp_rate;
        }

        // Apply NTP server change immediately if the user updated it
        if (strcmp(cur_ntp_server, s_ntp_server) != 0) {
            strncpy(s_ntp_server, cur_ntp_server, sizeof(s_ntp_server) - 1);
            s_ntp_server[sizeof(s_ntp_server) - 1] = '\0';
            esp_sntp_setservername(0, s_ntp_server);
            sntp_restart();
            ESP_LOGI(TAG, "NTP server → %s", s_ntp_server);
        }

        // Reset CAN counter on rate change so the new rate takes effect immediately
        if (cur_can_rate != last_can_rate) {
            can_ticks     = 0;
            last_can_rate = cur_can_rate;
        }

        struct tm utc;
        if (ds3231_get_time(&s_rtc, &utc) != ESP_OK) continue;

        // Capture UTC time_t for CAN sync tracking before utc_to_local touches the struct
        struct tm utc_copy = utc;
        time_t utc_ts = mktime(&utc_copy);  // ESP-IDF newlib: TZ defaults to UTC

        struct tm local = utc_to_local(&utc, tz_offset, dst_mode);

        if (can_ticks == 0) {
            if (send_can_datetime(&local)) {
                s_can_sync_utc = utc_ts;
            }
            can_ticks = (cur_can_rate > 1) ? cur_can_rate - 1 : 0;
        } else {
            can_ticks--;
        }

        char time_str[16], date_str[40], ntp_str[16], can_str[16];
        snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
                 local.tm_hour, local.tm_min, local.tm_sec);
        snprintf(date_str, sizeof(date_str), "%04d/%02d/%02d",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);

        // Recompute both sync times in local timezone every tick so TZ/DST changes apply immediately
        struct tm tmp_utc_tm;
        if (s_ntp_sync_utc != 0) {
            gmtime_r((const time_t *)&s_ntp_sync_utc, &tmp_utc_tm);
            struct tm ntp_local = utc_to_local(&tmp_utc_tm, tz_offset, dst_mode);
            snprintf(ntp_str, sizeof(ntp_str), "%02d:%02d:%02d",
                     ntp_local.tm_hour, ntp_local.tm_min, ntp_local.tm_sec);
        } else {
            strcpy(ntp_str, "--:--:--");
        }

        if (s_can_sync_utc != 0) {
            gmtime_r((const time_t *)&s_can_sync_utc, &tmp_utc_tm);
            struct tm can_local = utc_to_local(&tmp_utc_tm, tz_offset, dst_mode);
            snprintf(can_str, sizeof(can_str), "%02d:%02d:%02d",
                     can_local.tm_hour, can_local.tm_min, can_local.tm_sec);
        } else {
            strcpy(can_str, "--:--:--");
        }

        // NTP error: overdue if never synced after 10 min, or missed 2× the sync interval
        bool ntp_err;
        if (s_ntp_sync_utc != 0) {
            ntp_err = (utc_ts - s_ntp_sync_utc) > (time_t)(last_ntp_rate * 2);
        } else {
            ntp_err = ticks_since_valid > 600;
        }

        display_lock();
        ui_set_time(time_str);
        ui_set_date(date_str);
        ui_set_last_sync(ntp_str, can_str);
        ui_set_ntp_error(ntp_err);
        display_unlock();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t clock_service_init(void)
{
    // i2cdev subsystem (must be called once before any ds3231_* call)
    ESP_RETURN_ON_ERROR(i2cdev_init(), TAG, "i2cdev_init failed");

    // DS3231 descriptor — pullup flags must be set before ds3231_init_desc so
    // that i2c_dev_create_mutex configures the bus with them enabled.
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.cfg.sda_pullup_en = 1;
    s_rtc.cfg.scl_pullup_en = 1;
    ESP_RETURN_ON_ERROR(
        ds3231_init_desc(&s_rtc, BOARD_I2C_PORT, BOARD_PIN_RTC_SDA, BOARD_PIN_RTC_SCL),
        TAG, "ds3231_init_desc failed");

    // Configure 1 Hz square-wave output on the SQW/INT pin
    ESP_RETURN_ON_ERROR(ds3231_set_squarewave_freq(&s_rtc, DS3231_SQWAVE_1HZ),
                        TAG, "sqwave freq failed");
    ESP_RETURN_ON_ERROR(ds3231_enable_squarewave(&s_rtc),
                        TAG, "sqwave enable failed");
    ESP_LOGI(TAG, "DS3231 ready, 1 Hz SQW enabled");

    // Binary semaphore consumed by clock_task, given by ISR
    s_sqw_sem = xSemaphoreCreateBinary();
    assert(s_sqw_sem);

    // GPIO interrupt on the SQW pin, rising edge
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_RTC_SQW),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "gpio_config failed");

    // ISR service may already be installed by the button/knob components
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BOARD_PIN_RTC_SQW, sqw_isr, NULL),
                        TAG, "isr_handler_add failed");

    xTaskCreate(clock_task, "clock", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}
