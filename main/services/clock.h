#pragma once

#include "esp_err.h"

/**
 * Initialise the clock service:
 *  - Inits the DS3231 and enables 1 Hz square-wave output
 *  - Installs a rising-edge GPIO interrupt on BOARD_PIN_RTC_SQW
 *  - Starts a task that waits for the user to press Apply in the Network
 *    menu, applies DHCP or static-IP config, then starts NTP sync.
 *    On first successful sync the RTC is loaded with UTC time.
 *  - After time is valid, sends CAN DATE and TIME frames on every 1 Hz tick.
 */
esp_err_t clock_service_init(void);
