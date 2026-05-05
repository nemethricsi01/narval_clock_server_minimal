#pragma once

#include "esp_err.h"
#include "esp_eth.h"

/**
 * Initialise the W5500 Ethernet MAC/PHY over SPI.
 * Installs the driver and sets a locally-administered MAC address.
 * Does NOT start the interface — call esp_eth_start() separately.
 *
 * @param[out] out_handle  Populated with the eth driver handle.
 */
esp_err_t eth_w5500_init(esp_eth_handle_t *out_handle);
