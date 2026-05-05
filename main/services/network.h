#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Initialise networking: attaches the W5500 to lwIP, registers event
 * handlers, and starts the Ethernet driver (DHCP by default).
 */
esp_err_t network_init(void);

/** Returns true once an IP address has been assigned. */
bool network_is_connected(void);

/** Returns the current IP as a dotted-decimal string, or "" if not connected. */
const char *network_get_ip_str(void);

/**
 * Switch between DHCP and static IP at runtime.
 * For static IP, also sets the IP/mask/GW and marks the link as connected.
 * Safe to call after network_init(); may be called from any task.
 */
typedef struct {
    bool    dhcp;
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
} network_ip_cfg_t;

esp_err_t network_apply_ip(const network_ip_cfg_t *cfg);
