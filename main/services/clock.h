#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

/** NTP sync interval used without the DS3231 (system time drifts more than the RTC). */
#define CLOCK_NO_RTC_NTP_RATE_SEC  300

/**
 * Initialise the clock service:
 *  - Inits the DS3231, enables its 1 Hz square-wave output and seeds system
 *    time from it.
 *  - Starts clock_task on a 1 Hz tick: the DS3231's SQW interrupt, or an
 *    internal timer if the DS3231 is missing or its SQW never arrives.
 *  - clock_task starts NTP once the network has an IP (and re-queries on every
 *    new IP); each sync sets the RTC. Without the RTC, system time is the time
 *    source and NTP runs every CLOCK_NO_RTC_NTP_RATE_SEC.
 *  - Once the time is valid, sends CAN DATE and TIME frames at the set rate.
 * A missing DS3231 is not an error; check clock_has_rtc().
 */
esp_err_t clock_service_init(void);

/** True if the DS3231 answered at init. */
bool clock_has_rtc(void);

/**
 * UTC to local time: adds tz_offset hours (-12..+12) plus DST, where dst_mode is
 * 0 = Winter (never), 1 = Summer (always +1 h), 2 = Auto (EU rules for that date).
 */
struct tm clock_utc_to_local(const struct tm *utc, int tz_offset, int dst_mode);

/** Called by ui_init() once all LVGL widgets are built and safe to write. */
void clock_notify_ui_ready(void);
