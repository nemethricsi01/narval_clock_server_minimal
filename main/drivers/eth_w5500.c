#include "eth_w5500.h"
#include "board.h"
#include "driver/spi_master.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_log.h"

static const char *TAG = "eth_w5500";

esp_err_t eth_w5500_init(esp_eth_handle_t *out_handle)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num   = BOARD_PIN_ETH_MOSI,
        .miso_io_num   = BOARD_PIN_ETH_MISO,
        .sclk_io_num   = BOARD_PIN_ETH_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .mode           = 0,
        .clock_speed_hz = BOARD_ETH_CLOCK_HZ,
        .queue_size     = 20,
        .spics_io_num   = BOARD_PIN_ETH_CS,
    };

    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(BOARD_ETH_SPI_HOST, &devcfg);
    w5500_cfg.int_gpio_num   = -1;
    w5500_cfg.poll_period_ms = 10;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num   = BOARD_PIN_ETH_RST;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, out_handle));

    // W5500 modules typically ship without a factory MAC; use locally-administered one
    uint8_t mac_addr[] = BOARD_ETH_MAC;
    ESP_ERROR_CHECK(esp_eth_ioctl(*out_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}
