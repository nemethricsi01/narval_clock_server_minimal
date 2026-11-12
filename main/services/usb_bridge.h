#pragma once

#include "esp_err.h"
#include "services/can.h"

/**
 * CAN <-> USB bridge on the ESP32-S3's built-in USB port (USB-Serial-JTAG,
 * GPIO19/20), running alongside the clock. Logs and flashing stay on UART0.
 *
 * Byte protocol, both directions (same as esp32_canlogger_uart and
 * cherryusb_serial_device):   0x55, len (1-8), len data bytes, 0xAA
 * The CAN ID isn't carried: frames from USB are sent with ID 0x000.
 *
 * Call after can_service_init(). Returns an error instead of aborting.
 */
esp_err_t usb_bridge_init(void);

/**
 * Forward one frame to the PC. Called for frames received from the bus and for
 * the clock's own time broadcast; frames that came from USB aren't echoed.
 * Never blocks for long: drops the frame if no USB host is connected or the PC
 * isn't reading. Safe to call before usb_bridge_init() (does nothing).
 */
void usb_bridge_forward(const can_frame_t *f);
