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
static bool          s_eth_ready = false;   // set once network_init() fully succeeded
static volatile bool s_connected = false;
static char          s_ip_str[16] = "";

// Forget the current address until on_got_ip() reports one again.
static void clear_ip(void)
{
    s_connected = false;
    s_ip_str[0] = '\0';
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch ((eth_event_t)id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        clear_ip();
        display_lock();
        ui_set_ip("IP: --");
        ui_set_connection_state(UI_CONN_DISCONNECTED);
        display_unlock();
        ESP_LOGI(TAG, "link down");
        break;
    default:
        break;
    }
}

// The only place that marks the device connected. ESP-IDF posts this for a DHCP
// lease, and for a static IP only while the cable link is up (when the IP is set
// with the link up, or later on link-up).
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
    // Errors are returned, not ESP_ERROR_CHECKed: a missing W5500 must not
    // reboot-loop the device. The caller shows the failure on screen.
    esp_eth_handle_t eth_handle = NULL;
    ESP_RETURN_ON_ERROR(eth_w5500_init(&eth_handle), TAG, "W5500 init failed");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_eth_netif, ESP_ERR_NO_MEM, TAG, "esp_netif_new failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(eth_handle)),
                        TAG, "esp_netif_attach failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL),
                        TAG, "ETH event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL),
                        TAG, "IP event register failed");

    ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle), TAG, "esp_eth_start failed");
    s_eth_ready = true;
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
    if (!s_eth_ready) return ESP_ERR_INVALID_STATE;   // Ethernet failed to init

    // Connected state is left to on_got_ip(), so an unplugged cable is never
    // reported as connected. No UI calls here: Network > Apply calls this from
    // the LVGL task, which already holds the display lock (taking it again would
    // deadlock). Callers update the screen themselves.
    if (cfg->dhcp) {
        esp_netif_dhcp_status_t st;
        ESP_RETURN_ON_ERROR(esp_netif_dhcpc_get_status(s_eth_netif, &st), TAG, "dhcpc status failed");
        if (st == ESP_NETIF_DHCP_STARTED) return ESP_OK;   // already on DHCP: keep the lease
        clear_ip();
        ESP_RETURN_ON_ERROR(esp_netif_dhcpc_start(s_eth_netif), TAG, "dhcpc start failed");
    } else {
        esp_err_t ret = esp_netif_dhcpc_stop(s_eth_netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return ret;
        clear_ip();   // before set_ip_info(): with the link up it posts "got IP" at once

        esp_netif_ip_info_t ip = {0};
        ip.ip.addr      = ESP_IP4TOADDR(cfg->ip[0],   cfg->ip[1],   cfg->ip[2],   cfg->ip[3]);
        ip.netmask.addr = ESP_IP4TOADDR(cfg->mask[0], cfg->mask[1], cfg->mask[2], cfg->mask[3]);
        ip.gw.addr      = ESP_IP4TOADDR(cfg->gw[0],   cfg->gw[1],   cfg->gw[2],   cfg->gw[3]);
        ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_eth_netif, &ip), TAG, "set_ip_info failed");
        ESP_LOGI(TAG, "static IP " IPSTR " set, active once the link is up", IP2STR(&ip.ip));
    }
    return ESP_OK;
}
