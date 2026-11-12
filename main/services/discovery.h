#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * Finds devices running the Microchip TCP/IP stack's Announce service (PIC32
 * boards), like find_pic32.py: while a search runs, broadcasts
 * "Discovery: Who is out there?" to UDP 30303 once a second and collects the
 * replies (name, MAC and an optional note). The stack's own power-up and DHCP
 * announcements arrive on the same port and are collected too.
 *
 * This device also takes part as "NARV_CLK_SERV": it answers every query
 * (searching or not) and broadcasts an announcement whenever it gets a new IP.
 *
 * Call after esp_netif_init(). Returns an error instead of aborting.
 */
esp_err_t discovery_init(void);

/** Clears the list and starts searching (the first query goes out at once). */
void discovery_start(void);

/** Stops searching; replies are ignored until the next discovery_start(). */
void discovery_stop(void);

/** True if a device was added since the last discovery_snapshot(). */
bool discovery_has_new_data(void);

/**
 * Renders the devices found so far into out, two lines each:
 *   192.168.1.50  MCHPBOARD
 *       00-04-A3-12-34-56  note
 * Returns the number of devices. Thread-safe.
 */
int discovery_snapshot(char *out, size_t len);
