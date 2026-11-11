#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Initialise networking: attaches the W5500 to lwIP, registers event
 * handlers, and starts the Ethernet driver (DHCP by default).
 * Returns an error (instead of aborting) if the W5500 is missing or fails;
 * the rest of the device keeps working without Ethernet.
 */
esp_err_t network_init(void);

/** Returns true while the cable link is up and an IP address is in use (DHCP or static). */
bool network_is_connected(void);

/** Returns the current IP as a dotted-decimal string, or "" if not connected. */
const char *network_get_ip_str(void);

/**
 * Switch between DHCP and static IP at runtime.
 * For static IP, also sets the IP/mask/GW. network_is_connected() turns true only
 * once the cable link is up, which may be right away or later.
 * Safe to call after network_init(); may be called from any task, including
 * the LVGL task. It doesn't update the UI: the caller refreshes the screen.
 */
typedef struct {
    bool    dhcp;
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
} network_ip_cfg_t;

esp_err_t network_apply_ip(const network_ip_cfg_t *cfg);
