#include "usb_bridge.h"
#include "can_monitor.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "usb_bridge";

#define FRAME_START  0x55
#define FRAME_END    0xAA

// The driver drops incoming USB data when its RX buffer is full (no flow
// control), and the PC can send far faster than the bus carries (~90 frames/s).
// 4 KB holds a burst of ~370 frames.
#define USB_RX_BUF          4096
#define USB_TX_BUF          1024
#define TASK_STACK          3072
#define TASK_PRIORITY       3
#define CAN_SEND_TIMEOUT_MS 200   // wait this long for room in the CAN TX queue
#define USB_WRITE_TIMEOUT   pdMS_TO_TICKS(2)

static volatile bool s_ready = false;

void usb_bridge_forward(const can_frame_t *f)
{
    // With no host attached, don't fill the buffer with frames that would
    // arrive stale when one is plugged in.
    if (!s_ready || !usb_serial_jtag_is_connected()) return;

    uint8_t len = f->header.dlc < CAN_DATA_MAX_LEN ? f->header.dlc : CAN_DATA_MAX_LEN;
    uint8_t buf[CAN_DATA_MAX_LEN + 3];
    buf[0] = FRAME_START;
    buf[1] = len;
    memcpy(&buf[2], f->data, len);
    buf[2 + len] = FRAME_END;
    // All-or-nothing: if the PC isn't reading and the buffer is full, the whole
    // frame is dropped, never half of it.
    usb_serial_jtag_write_bytes(buf, len + 3, USB_WRITE_TIMEOUT);
}

static void send_to_can(const uint8_t *data, uint8_t len)
{
    can_frame_t f = { .header = { .id = 0x000, .dlc = len } };
    memcpy(f.data, data, len);

    esp_err_t err = can_service_send(&f, CAN_SEND_TIMEOUT_MS);
    if (err == ESP_OK) {
        can_mon_push(&f, true);
    } else {
        ESP_LOGW(TAG, "frame from USB dropped: %s", esp_err_to_name(err));
    }
}

// Parses 0x55, len, data, 0xAA from the PC. Anything else (e.g. the boot ROM's
// text after a reset) is skipped until the next 0x55.
static void usb_rx_task(void *arg)
{
    enum { WAIT_START, WAIT_LEN, RECV_DATA, WAIT_END } state = WAIT_START;
    uint8_t data[CAN_DATA_MAX_LEN];
    uint8_t len = 0, idx = 0;
    uint8_t chunk[64];

    while (1) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            switch (state) {
            case WAIT_START:
                if (b == FRAME_START) state = WAIT_LEN;
                break;
            case WAIT_LEN:
                if (b >= 1 && b <= CAN_DATA_MAX_LEN) {
                    len = b;
                    idx = 0;
                    state = RECV_DATA;
                } else {
                    state = WAIT_START;   // invalid length: resync
                }
                break;
            case RECV_DATA:
                data[idx++] = b;
                if (idx == len) state = WAIT_END;
                break;
            case WAIT_END:
                if (b == FRAME_END) send_to_can(data, len);
                state = WAIT_START;       // bad end byte: drop the frame and resync
                break;
            }
        }
    }
}

esp_err_t usb_bridge_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = USB_TX_BUF,
        .rx_buffer_size = USB_RX_BUF,
    };
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&cfg), TAG, "driver install failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(usb_rx_task, "usb_bridge", TASK_STACK, NULL, TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task create failed");
    s_ready = true;
    ESP_LOGI(TAG, "CAN <-> USB bridge ready on the built-in USB port");
    return ESP_OK;
}
