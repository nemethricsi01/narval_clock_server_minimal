#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
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

/**
 * Writes the current IP as a dotted-decimal string into out (16 bytes is enough),
 * or "" if not connected. Safe from any task.
 */
void network_get_ip_str(char *out, size_t len);

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
