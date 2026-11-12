#include "discovery.h"
#include "network.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "discovery";

#define ANNOUNCE_PORT      30303
#define ANNOUNCE_NAME      "NARV_CLK_SERV"   // how this device shows up in find_pic32.py
#define QUERY              "Discovery: Who is out there?"
#define QUERY_INTERVAL_US  (1000 * 1000)
#define MAX_DEVICES        16
#define TASK_STACK         3072
#define TASK_PRIORITY      2

typedef struct {
    uint32_t ip;        // as in esp_ip4_addr_t
    char     name[24];
    char     mac[18];   // "00-04-A3-12-34-56"; "" for an unrecognised reply
    char     note[40];
} device_t;

static device_t          s_dev[MAX_DEVICES];
static int               s_count;
static SemaphoreHandle_t s_mutex;
static volatile bool     s_searching;
static volatile bool     s_query_now;   // send a query on the next loop, not after the interval
static volatile bool     s_new_data;

// Copies the next "\r\n"-terminated line at *p (or the rest, up to end) into
// out, trimmed, with non-printable characters replaced; advances *p past it.
static void next_line(const char **p, const char *end, char *out, size_t len)
{
    const char *s = *p, *e = s;
    while (e < end && !(e[0] == '\r' && e + 1 < end && e[1] == '\n')) e++;
    *p = (e < end) ? e + 2 : end;

    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    size_t n = 0;
    for (; s < e && n + 1 < len; s++) out[n++] = isprint((unsigned char)*s) ? *s : '?';
    out[n] = '\0';
}

static bool is_mac(const char *s)   // "xx-xx-xx-xx-xx-xx"
{
    if (strlen(s) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2 ? s[i] != '-' : !isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static void add_device(const device_t *d)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool known = false;
    for (int i = 0; i < s_count && !known; i++) {
        known = s_dev[i].ip == d->ip && strcmp(s_dev[i].mac, d->mac) == 0;
    }
    bool added = !known && s_count < MAX_DEVICES;
    if (added) {
        s_dev[s_count++] = *d;
        s_new_data = true;
    }
    xSemaphoreGive(s_mutex);

    if (added) {
        esp_ip4_addr_t ip = { .addr = d->ip };
        ESP_LOGI(TAG, IPSTR "  %s  %s  %s", IP2STR(&ip), d->name, d->mac, d->note);
    }
}

// An Announce reply is "name\r\nMAC\r\n[note]". Anything else is listed as
// unrecognised, like find_pic32.py does.
static void handle_packet(uint32_t ip, const char *data, int len)
{
    device_t d = { .ip = ip };
    char mac[24];
    const char *p = data, *end = data + len;
    next_line(&p, end, d.name, sizeof(d.name));
    next_line(&p, end, mac, sizeof(mac));
    if (is_mac(mac)) {
        strcpy(d.mac, mac);
        next_line(&p, end, d.note, sizeof(d.note));
    } else {
        p = data;
        next_line(&p, end, d.note, sizeof(d.note));   // show how it starts
        strcpy(d.name, "(unrecognised reply)");
    }
    add_device(&d);
}

// This device's own Announce packet, in the Microchip stack's format: name and
// MAC, plus the note the stack adds to its unsolicited DHCP/power announcements.
static void send_announce(int sock, const struct sockaddr_in *to, bool event)
{
    uint8_t m[6];
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!netif || esp_netif_get_mac(netif, m) != ESP_OK) return;

    char pkt[80];
    int len = snprintf(pkt, sizeof(pkt), ANNOUNCE_NAME "\r\n%02X-%02X-%02X-%02X-%02X-%02X\r\n%s",
                       m[0], m[1], m[2], m[3], m[4], m[5], event ? "DHCP/Power event occurred" : "");
    if (sendto(sock, pkt, len, 0, (const struct sockaddr *)to, sizeof(*to)) < 0) {
        ESP_LOGD(TAG, "announce not sent: errno %d", errno);
    }
}

static void discovery_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    int64_t next_query = 0;
    uint32_t last_ip = 0;
    char buf[256];
    const struct sockaddr_in broadcast = {
        .sin_family      = AF_INET,
        .sin_port        = htons(ANNOUNCE_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    while (1) {
        // Announce once per new IP, as the Microchip stack does on DHCP/power events.
        uint32_t my_ip = network_get_ip();
        if (my_ip && my_ip != last_ip) {
            send_announce(sock, &broadcast, true);
            ESP_LOGI(TAG, "announced as " ANNOUNCE_NAME);
        }
        last_ip = my_ip;

        if (s_searching && network_is_connected() &&
            (s_query_now || esp_timer_get_time() >= next_query)) {
            s_query_now = false;
            next_query  = esp_timer_get_time() + QUERY_INTERVAL_US;
            if (sendto(sock, QUERY, strlen(QUERY), 0, (const struct sockaddr *)&broadcast, sizeof(broadcast)) < 0) {
                ESP_LOGD(TAG, "query not sent: errno %d", errno);
            }
        }

        // Times out after 100 ms (SO_RCVTIMEO), so queries keep their pace.
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0 || from.sin_addr.s_addr == my_ip) continue;   // nothing, or our own packet

        if (n >= 9 && memcmp(buf, "Discovery", 9) == 0) {
            send_announce(sock, &from, false);   // someone is searching: answer them directly
        } else if (s_searching) {
            handle_packet(from.sin_addr.s_addr, buf, n);
        }
    }
}

esp_err_t discovery_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex, ESP_ERR_NO_MEM, TAG, "mutex create failed");

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(sock >= 0, ESP_FAIL, TAG, "socket failed: errno %d", errno);

    int on = 1;
    struct timeval timeout = { .tv_sec = 0, .tv_usec = 100 * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // The local port must be 30303: the Announce service sends its replies,
    // and its power-up/DHCP announcements, there.
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(ANNOUNCE_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind to UDP %d failed: errno %d", ANNOUNCE_PORT, errno);
        close(sock);
        return ESP_FAIL;
    }

    if (xTaskCreate(discovery_task, "discovery", TASK_STACK, (void *)(intptr_t)sock,
                    TASK_PRIORITY, NULL) != pdPASS) {
        close(sock);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready (UDP %d)", ANNOUNCE_PORT);
    return ESP_OK;
}

void discovery_start(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_count    = 0;
    s_new_data = true;
    xSemaphoreGive(s_mutex);
    s_query_now = true;
    s_searching = true;
}

void discovery_stop(void)
{
    s_searching = false;
}

bool discovery_has_new_data(void)
{
    return s_new_data;
}

// vsnprintf-append that stops cleanly once out is full.
static void __attribute__((format(printf, 4, 5)))
append(char *out, size_t len, size_t *n, const char *fmt, ...)
{
    if (*n >= len) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + *n, len - *n, fmt, ap);
    va_end(ap);
    if (w > 0) *n += (size_t)w;
}

int discovery_snapshot(char *out, size_t len)
{
    size_t n = 0;
    if (len) out[0] = '\0';

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_new_data = false;
    for (int i = 0; i < s_count; i++) {
        const device_t *d = &s_dev[i];
        esp_ip4_addr_t ip = { .addr = d->ip };
        append(out, len, &n, IPSTR "  %s\n", IP2STR(&ip), d->name);
        if (d->mac[0] || d->note[0]) {
            append(out, len, &n, "    %s%s%s\n", d->mac, (d->mac[0] && d->note[0]) ? "  " : "", d->note);
        }
    }
    int count = s_count;
    xSemaphoreGive(s_mutex);
    return count;
}
