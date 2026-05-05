#include "network.h"
#include "drivers/eth_w5500.h"
#include "services/display.h"
#include "services/ui.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_check.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "network";

static esp_netif_t *s_eth_netif = NULL;
static volatile bool s_connected = false;
static char          s_ip_str[16] = "";

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch ((eth_event_t)id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_connected = false;
        s_ip_str[0] = '\0';
        display_lock();
        ui_set_connection_state(UI_CONN_DISCONNECTED);
        display_unlock();
        ESP_LOGI(TAG, "link down");
        break;
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
    s_connected = true;
    ESP_LOGI(TAG, "got IP: %s", s_ip_str);

    char label[24];
    snprintf(label, sizeof(label), "IP:%s", s_ip_str);
    display_lock();
    ui_set_ip(label);
    ui_set_connection_state(UI_CONN_LAN);
    display_unlock();
}

esp_err_t network_init(void)
{
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(eth_w5500_init(&eth_handle));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    return ESP_OK;
}

bool network_is_connected(void)
{
    return s_connected;
}

const char *network_get_ip_str(void)
{
    return s_ip_str;
}

esp_err_t network_apply_ip(const network_ip_cfg_t *cfg)
{
    if (cfg->dhcp) {
        // DHCP is already started by default; restart it in case it was stopped.
        esp_err_t ret = esp_netif_dhcpc_start(s_eth_netif);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    } else {
        esp_err_t ret = esp_netif_dhcpc_stop(s_eth_netif);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

        esp_netif_ip_info_t ip = {0};
        ip.ip.addr      = ESP_IP4TOADDR(cfg->ip[0],   cfg->ip[1],   cfg->ip[2],   cfg->ip[3]);
        ip.netmask.addr = ESP_IP4TOADDR(cfg->mask[0], cfg->mask[1], cfg->mask[2], cfg->mask[3]);
        ip.gw.addr      = ESP_IP4TOADDR(cfg->gw[0],   cfg->gw[1],   cfg->gw[2],   cfg->gw[3]);
        ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_eth_netif, &ip), TAG, "set_ip_info failed");

        snprintf(s_ip_str, sizeof(s_ip_str), "%d.%d.%d.%d",
                 cfg->ip[0], cfg->ip[1], cfg->ip[2], cfg->ip[3]);
        s_connected = true;
        ESP_LOGI(TAG, "static IP: %s", s_ip_str);

        char label[24];
        snprintf(label, sizeof(label), "IP:%s", s_ip_str);
        display_lock();
        ui_set_ip(label);
        ui_set_connection_state(UI_CONN_LAN);
        display_unlock();
    }
    return ESP_OK;
}
