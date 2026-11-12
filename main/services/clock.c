#include "clock.h"
#include "network.h"
#include "can.h"
#include "can_monitor.h"
#include "usb_bridge.h"
#include "board.h"
#include "services/display.h"
#include "services/ui.h"

#include "ds3231.h"
#include "driver/gpio.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <time.h>
#include <sys/time.h>
#include <string.h>

static const char *TAG = "clock";

// If no SQW edge arrives for this long, clock_task switches to the internal timer.
#define SQW_TIMEOUT_MS  2500

static i2c_dev_t          s_rtc;
static bool               s_has_rtc      = false;  // DS3231 answered at init; set before clock_task starts
static SemaphoreHandle_t  s_tick_sem;              // given once per second by the SQW ISR or s_tick_timer
static esp_timer_handle_t s_tick_timer   = NULL;   // fallback 1 Hz tick; NULL while the SQW drives the clock
static volatile bool     s_time_valid   = false;
static volatile bool     s_ui_ready     = false;
// SNTP keeps the pointer it is given, so the name in use is never rewritten in
// place: a new name goes into the other buffer first (see use_ntp_server()).
static char              s_ntp_server_buf[2][32];
static const char       *s_ntp_server = s_ntp_server_buf[0];
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

struct tm clock_utc_to_local(const struct tm *utc, int tz_offset, int dst_mode)
{
    struct tm local = *utc;
    local.tm_hour += tz_offset + effective_dst_hours(utc, dst_mode);
    local.tm_isdst = 0;
    mktime(&local);
    return local;
}

// ---------------------------------------------------------------------------
// 1 Hz tick sources: the DS3231's SQW pin, or an internal timer as fallback
// ---------------------------------------------------------------------------

static void IRAM_ATTR sqw_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_tick_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

// One-shot, re-armed to fire just after each whole second of system time, so
// the displayed and broadcast seconds change on time (NTP keeps system time right).
static void arm_tick_timer(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    esp_timer_start_once(s_tick_timer, 1000000 - tv.tv_usec + 1000);
}

static void tick_timer_cb(void *arg)
{
    xSemaphoreGive(s_tick_sem);
    arm_tick_timer();
}

static esp_err_t start_tick_timer(void)
{
    const esp_timer_create_args_t args = { .callback = tick_timer_cb, .name = "clock_1hz" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_tick_timer), TAG, "tick timer create failed");
    arm_tick_timer();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// CAN transmit — sends DATE then TIME frames every 1 Hz tick
// ---------------------------------------------------------------------------

// Short wait for a TX slot, so a burst from the USB bridge can't starve the
// time broadcast (a frame takes ~11 ms on the bus).
#define CAN_SEND_TIMEOUT_MS 30

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
    esp_err_t r1 = can_service_send(&date_f, CAN_SEND_TIMEOUT_MS);
    if (r1 == ESP_OK) { can_mon_push(&date_f, true); usb_bridge_forward(&date_f); }

    // TIME:  000HHHHH 00MMMMMM 00SSSSSS 00000001
    time_f.data[0] = (uint8_t)(t->tm_hour & 0x1F);
    time_f.data[1] = (uint8_t)(t->tm_min  & 0x3F);
    time_f.data[2] = (uint8_t)(t->tm_sec  & 0x3F);
    time_f.data[3] = 0x01;
    esp_err_t r2 = can_service_send(&time_f, CAN_SEND_TIMEOUT_MS);
    if (r2 == ESP_OK) { can_mon_push(&time_f, true); usb_bridge_forward(&time_f); }

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

    // SNTP has already set system time; without the RTC that's all there is to do.
    if (s_has_rtc && ds3231_set_time(&s_rtc, &t) != ESP_OK) {
        ESP_LOGE(TAG, "RTC write failed after NTP sync");
        return;
    }
    // The RTC holds good time again: clear the oscillator-stop flag so the next
    // boot trusts it (see rtc_init()).
    if (s_has_rtc && ds3231_clear_oscillator_stop_flag(&s_rtc) != ESP_OK) {
        ESP_LOGW(TAG, "couldn't clear the DS3231 oscillator-stop flag");
    }
    s_ntp_sync_utc = t_sec;
    s_time_valid   = true;
    ESP_LOGI(TAG, "synced%s: %04d-%02d-%02d %02d:%02d:%02d UTC",
             s_has_rtc ? " → RTC set" : " (no RTC, system time only)",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

// Copies name into the NTP server buffer SNTP isn't using and makes it current.
// Returns it, ready to hand to SNTP.
static const char *use_ntp_server(const char *name)
{
    char *buf = s_ntp_server_buf[s_ntp_server == s_ntp_server_buf[0] ? 1 : 0];
    strlcpy(buf, name, sizeof(s_ntp_server_buf[0]));
    s_ntp_server = buf;
    return buf;
}

// ---------------------------------------------------------------------------
// Clock task — runs forever, blocked on the 1 Hz semaphore
// ---------------------------------------------------------------------------

static void clock_task(void *arg)
{
    bool     sntp_started      = false;
    uint32_t last_ntp_rate     = 0;
    uint32_t last_can_rate     = 0;
    uint32_t can_ticks         = 0;
    uint32_t ticks_since_valid = 0;
    int8_t   last_rtc_trim     = 0;
    bool     rtc_trim_synced   = false;
    char     last_ip[16]       = "";   // IP seen on the previous tick; "" = offline

    // Tick loop: the SQW ISR or the fallback timer gives the semaphore once per second
    while (1) {
        if (xSemaphoreTake(s_tick_sem, pdMS_TO_TICKS(SQW_TIMEOUT_MS)) != pdTRUE) {
            // The DS3231 answers but its SQW never arrives (e.g. a broken SQW line):
            // keep reading the RTC, but tick from the internal timer.
            if (!s_tick_timer) {
                ESP_LOGW(TAG, "no 1 Hz SQW from the DS3231 — switching to the internal timer");
                gpio_isr_handler_remove(BOARD_PIN_RTC_SQW);
                start_tick_timer();
            }
            continue;
        }
        if (!s_ui_ready) continue;   // wait for LVGL widgets to be built

        // Snapshot all mutable UI settings in one lock window so clock_task never
        // races with the LVGL task modifying the same state on the other core.
        display_lock();
        uint32_t cur_ntp_rate = ui_get_ntp_update_rate_sec();
        uint32_t cur_can_rate = ui_get_update_rate_sec();
        int      tz_offset    = ui_get_tz_offset_hours();
        int      dst_mode     = ui_get_dst_mode();
        int8_t   cur_rtc_trim = ui_get_rtc_trim();
        char     cur_ntp_server[32];
        ui_get_ntp_server_str(cur_ntp_server, sizeof(cur_ntp_server));
        display_unlock();
        if (!s_has_rtc) cur_ntp_rate = CLOCK_NO_RTC_NTP_RATE_SEC;   // the user's choice is kept for when the RTC is back

        // Query NTP whenever a new IP arrives: the first one starts SNTP, later
        // ones (cable plugged back in, IP changed) restart it, so the time doesn't
        // wait for the next scheduled sync. The query goes out within 0-5 s
        // (CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY).
        // This must not be gated on s_time_valid: if the DS3231 has no valid time
        // yet, NTP is the only path to set it via on_ntp_sync.
        char cur_ip[16] = "";
        if (network_is_connected()) strlcpy(cur_ip, network_get_ip_str(), sizeof(cur_ip));
        bool new_ip = cur_ip[0] && strcmp(cur_ip, last_ip) != 0;
        strcpy(last_ip, cur_ip);   // "" while offline, so reconnecting with the same IP counts too

        if (new_ip && sntp_started) {
            ESP_LOGI(TAG, "new IP %s — querying NTP now", cur_ip);
            sntp_restart();
        } else if (new_ip) {
            sntp_started = true;
            use_ntp_server(cur_ntp_server);
            last_ntp_rate = cur_ntp_rate;
            ESP_LOGI(TAG, "network up (%s) — starting NTP via %s", cur_ip, s_ntp_server);
            esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
            sntp_cfg.sync_cb = on_ntp_sync;
            esp_netif_sntp_init(&sntp_cfg);
            sntp_set_sync_interval(last_ntp_rate * 1000);
        }

        // Apply NTP rate change immediately if the user updated it
        if (sntp_started && cur_ntp_rate != last_ntp_rate) {
            sntp_set_sync_interval(cur_ntp_rate * 1000);
            sntp_restart();
            last_ntp_rate = cur_ntp_rate;
        }

        // Apply NTP server change immediately if the user updated it
        if (sntp_started && strcmp(cur_ntp_server, s_ntp_server) != 0) {
            esp_sntp_setservername(0, use_ntp_server(cur_ntp_server));
            sntp_restart();
            ESP_LOGI(TAG, "NTP server → %s", s_ntp_server);
        }

        // Apply DS3231 aging offset once on startup, then on every user change.
        // ds3231_set_aging_offset also triggers a manual temperature conversion
        // so the new offset takes effect on the oscillator immediately.
        if (s_has_rtc && (!rtc_trim_synced || cur_rtc_trim != last_rtc_trim)) {
            rtc_trim_synced = true;
            last_rtc_trim   = cur_rtc_trim;
            if (ds3231_set_aging_offset(&s_rtc, cur_rtc_trim) == ESP_OK)
                ESP_LOGI(TAG, "aging offset → %d", cur_rtc_trim);
        }

        // The rest requires a valid time: seeded from the DS3231 or set by NTP
        if (!s_time_valid) continue;
        ticks_since_valid++;

        // Reset CAN counter on rate change so the new rate takes effect immediately
        if (cur_can_rate != last_can_rate) {
            can_ticks     = 0;
            last_can_rate = cur_can_rate;
        }

        // Time source: the DS3231, or system time (set by NTP, or seeded from the
        // RTC at boot) when there is no RTC or a read fails.
        struct tm utc;
        if (!s_has_rtc || ds3231_get_time(&s_rtc, &utc) != ESP_OK) {
            time_t now = time(NULL);
            gmtime_r(&now, &utc);
        }

        // Capture UTC time_t for CAN sync tracking before clock_utc_to_local touches the struct
        struct tm utc_copy = utc;
        time_t utc_ts = mktime(&utc_copy);  // ESP-IDF newlib: TZ defaults to UTC

        struct tm local = clock_utc_to_local(&utc, tz_offset, dst_mode);

        // Rate 0 is "Never": the CAN time broadcast is switched off.
        if (cur_can_rate != 0) {
            if (can_ticks == 0) {
                if (send_can_datetime(&local)) {
                    s_can_sync_utc = utc_ts;
                }
                can_ticks = cur_can_rate - 1;
            } else {
                can_ticks--;
            }
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
            struct tm ntp_local = clock_utc_to_local(&tmp_utc_tm, tz_offset, dst_mode);
            snprintf(ntp_str, sizeof(ntp_str), "%02d:%02d:%02d",
                     ntp_local.tm_hour, ntp_local.tm_min, ntp_local.tm_sec);
        } else {
            strcpy(ntp_str, "--:--:--");
        }

        if (cur_can_rate == 0) {
            strcpy(can_str, "off");
        } else if (s_can_sync_utc != 0) {
            gmtime_r((const time_t *)&s_can_sync_utc, &tmp_utc_tm);
            struct tm can_local = clock_utc_to_local(&tmp_utc_tm, tz_offset, dst_mode);
            snprintf(can_str, sizeof(can_str), "%02d:%02d:%02d",
                     can_local.tm_hour, can_local.tm_min, can_local.tm_sec);
        } else {
            strcpy(can_str, "--:--:--");
        }

        // NTP error: only meaningful once SNTP is running
        bool ntp_err = false;
        if (sntp_started) {
            if (s_ntp_sync_utc != 0) {
                ntp_err = (utc_ts - s_ntp_sync_utc) > (time_t)(last_ntp_rate * 2);
            } else {
                ntp_err = ticks_since_valid > 600;
            }
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

void clock_notify_ui_ready(void)
{
    s_ui_ready = true;
}

bool clock_has_rtc(void)
{
    return s_has_rtc;
}

// Inits the DS3231 with a 1 Hz SQW and seeds system time from it.
// Fails if the chip doesn't answer.
static esp_err_t rtc_init(void)
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

    // Seed system time from DS3231 immediately so that CAN frame timestamps
    // and the on-screen clock are valid before NTP first syncs.
    // Not trusted if the oscillator-stop flag is set (the oscillator stopped at
    // some point, e.g. the backup battery ran flat while unpowered, so the time
    // is stale; on_ntp_sync() clears it) or the year is < 2020 (never set).
    bool osc_stopped = true;   // stays true if the flag can't be read
    ds3231_get_oscillator_stop_flag(&s_rtc, &osc_stopped);
    struct tm rtc_seed;
    if (!osc_stopped && ds3231_get_time(&s_rtc, &rtc_seed) == ESP_OK && rtc_seed.tm_year >= 120) {
        struct timeval tv = { .tv_sec = mktime(&rtc_seed), .tv_usec = 0 };
        settimeofday(&tv, NULL);
        s_time_valid = true;
        ESP_LOGI(TAG, "seeded from DS3231: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 rtc_seed.tm_year + 1900, rtc_seed.tm_mon + 1, rtc_seed.tm_mday,
                 rtc_seed.tm_hour, rtc_seed.tm_min, rtc_seed.tm_sec);
    } else {
        ESP_LOGW(TAG, "DS3231 time not valid (%s) — waiting for NTP sync",
                 osc_stopped ? "oscillator had stopped, time is stale" : "never set");
    }
    return ESP_OK;
}

// Rising-edge GPIO interrupt on the DS3231's SQW pin.
static esp_err_t sqw_irq_init(void)
{
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
    return gpio_isr_handler_add(BOARD_PIN_RTC_SQW, sqw_isr, NULL);
}

esp_err_t clock_service_init(void)
{
    s_tick_sem = xSemaphoreCreateBinary();
    assert(s_tick_sem);

    // A missing DS3231 is not fatal: system time (set by NTP) becomes the time
    // source and an internal timer the 1 Hz tick.
    esp_err_t err = rtc_init();
    s_has_rtc = (err == ESP_OK);
    if (!s_has_rtc) {
        ESP_LOGW(TAG, "no DS3231 (%s) — using NTP time, synced every %d s",
                 esp_err_to_name(err), CLOCK_NO_RTC_NTP_RATE_SEC);
    }

    if (!s_has_rtc || sqw_irq_init() != ESP_OK) {
        ESP_RETURN_ON_ERROR(start_tick_timer(), TAG, "no 1 Hz tick source");
    }

    ESP_RETURN_ON_FALSE(xTaskCreate(clock_task, "clock", 4096, NULL, 4, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "clock task create failed");
    ESP_LOGI(TAG, "ready (tick: %s)", s_tick_timer ? "internal timer" : "DS3231 SQW");
    return ESP_OK;
}
