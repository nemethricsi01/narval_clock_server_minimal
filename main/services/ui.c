#include "ui.h"
#include "services/clock.h"
#include "services/display.h"
#include "services/network.h"
#include "services/config.h"
#include "services/can_monitor.h"
#include "services/discovery.h"
#include "drivers/encoder.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef enum {
    SCREEN_MAIN,
    SCREEN_MENU,
    SCREEN_SETTINGS,
    SCREEN_UPDATE_RATE,
    SCREEN_NTP_UPDATE_RATE,
    SCREEN_DST,
    SCREEN_TIMEZONE,
    SCREEN_BRIGHTNESS,
    SCREEN_NETWORK,
    SCREEN_NTP,
    SCREEN_IP_EDIT,
    SCREEN_CAN_MONITOR,
    SCREEN_RTC_TRIM,
    SCREEN_DISCOVERY,
} ui_screen_t;
static ui_screen_t s_current_screen = SCREEN_MAIN;

typedef enum { IP_FIELD_IP, IP_FIELD_MASK, IP_FIELD_GW, IP_FIELD_NTP } ip_field_t;

static lv_indev_t *s_enc_indev       = NULL;
static lv_group_t *s_menu_group      = NULL;
static lv_group_t *s_settings_group  = NULL;
static lv_group_t *s_network_group   = NULL;
static lv_group_t *s_empty_group     = NULL;

// Main screen widgets
static lv_obj_t *s_main_scr          =  NULL;
static lv_obj_t *s_ip_label          =  NULL;
static lv_obj_t *s_time_label        =  NULL;
static lv_obj_t *s_date_label        =  NULL;
static lv_obj_t *s_connected_label   =  NULL;
static lv_obj_t *s_last_sync_label   =  NULL;

static ui_connection_state_t s_connection_state = UI_CONN_DISCONNECTED;
static bool                  s_ntp_error         = false;

// Menu screen widgets
static lv_obj_t *s_menu_scr       = NULL;
static lv_obj_t *s_status_label   = NULL;
static lv_obj_t *s_first_menu_btn = NULL;

// Settings screen widgets
static lv_obj_t *s_settings_scr       = NULL;
static lv_obj_t *s_first_settings_btn = NULL;

// The settings being edited on the open settings screen. Loaded with config_get()
// when the screen opens, written back by commit_settings() on Click, and simply
// dropped on cancel. The live settings are in config.c.
static settings_t s_edit;

// What the IP editor and the NTP picker restore on cancel.
static uint8_t s_ip_backup[4];
static int     s_ntp_idx_backup;
static uint8_t s_ntp_ip_backup[4];

// Update-rate picker widgets
static lv_obj_t *s_update_rate_scr    = NULL;
static lv_obj_t *s_update_rate_roller = NULL;

// NTP update-rate picker widgets
static lv_obj_t *s_ntp_update_rate_scr    = NULL;
static lv_obj_t *s_ntp_update_rate_roller = NULL;
static lv_obj_t *s_ntp_rate_hint          = NULL;

// DST picker widgets
static lv_obj_t *s_dst_scr    = NULL;
static lv_obj_t *s_dst_roller = NULL;

// Timezone picker widgets
static lv_obj_t *s_tz_scr   = NULL;
static lv_obj_t *s_tz_table = NULL;
#define TZ_ROW_H  34  // montserrat_20 (~25px) + top/bottom cell padding

// Brightness screen widgets
static lv_obj_t *s_brightness_scr   = NULL;
static lv_obj_t *s_brightness_label = NULL;
static lv_obj_t *s_brightness_bar   = NULL;

// RTC trim screen widgets
static lv_obj_t *s_rtc_trim_scr   = NULL;
static lv_obj_t *s_rtc_trim_label = NULL;
static lv_obj_t *s_rtc_trim_bar   = NULL;

// Network screen widgets
static lv_obj_t *s_network_scr    = NULL;
static lv_obj_t *s_net_mode_btn   = NULL;
static lv_obj_t *s_net_mode_label = NULL;
static lv_obj_t *s_net_ip_btn     = NULL;
static lv_obj_t *s_net_ip_label   = NULL;
static lv_obj_t *s_net_mask_btn   = NULL;
static lv_obj_t *s_net_mask_label = NULL;
static lv_obj_t *s_net_gw_btn     = NULL;
static lv_obj_t *s_net_gw_label   = NULL;
static lv_obj_t *s_net_ntp_btn    = NULL;
static lv_obj_t *s_net_ntp_label  = NULL;

// NTP picker screen widgets
static lv_obj_t *s_ntp_scr        = NULL;
static lv_obj_t *s_ntp_roller     = NULL;
static lv_obj_t *s_ntp_hint_label = NULL;
#define NTP_MANUAL  (SETTINGS_NTP_SERVER_COUNT - 1)   // last choice: manual IP

// IP edit screen widgets
static lv_obj_t *s_ip_edit_scr    = NULL;
static lv_obj_t *s_ip_title_label = NULL;
static lv_obj_t *s_ip_value_label = NULL;

// CAN monitor screen widgets and state
static lv_obj_t *s_can_scr  = NULL;
static lv_obj_t *s_can_cont = NULL;
static lv_obj_t *s_can_log  = NULL;
static char      s_can_log_text[CAN_MON_LOG_SIZE * CAN_MON_LINE_MAX];

// Find devices screen (PIC32 boards with the Announce service, see discovery.h)
static lv_obj_t *s_disc_scr  = NULL;
static lv_obj_t *s_disc_cont = NULL;
static lv_obj_t *s_disc_log  = NULL;
static char      s_disc_text[2048];
// true: auto-scroll to bottom on every refresh; false: user is browsing history
static bool      s_can_follow = true;

// IP edit state
static ip_field_t s_ip_field = IP_FIELD_IP;
static int        s_ip_octet = 0;

// Single rotation accumulator: 2 raw CW/CCW events = 1 detent = 1 step.
// Reset on every screen transition so there is never leftover state.
static int s_enc_accum = 0;

// ---------------------------------------------------------------------------
// Encoder accumulator + navigation helpers
// (all called from ui_encoder_event, which runs in the LVGL task)
// ---------------------------------------------------------------------------

static int accum_step(encoder_event_t event)
{
    if (event != ENCODER_CW && event != ENCODER_CCW) return 0;
    s_enc_accum += (event == ENCODER_CW) ? 1 : -1;
    if (s_enc_accum >= 2)  { s_enc_accum = 0; return  1; }
    if (s_enc_accum <= -2) { s_enc_accum = 0; return -1; }
    return 0;
}

static void group_nav(lv_group_t *g, int step)
{
    if (step > 0) lv_group_focus_next(g);
    else          lv_group_focus_prev(g);
}

static void group_click(lv_group_t *g)
{
    lv_obj_t *focused = lv_group_get_focused(g);
    if (focused) lv_obj_send_event(focused, LV_EVENT_CLICKED, NULL);
}

// ---------------------------------------------------------------------------
// Value-change helpers (run in LVGL task — LVGL calls are safe)
// ---------------------------------------------------------------------------

// The brightness changes while turning the knob, as a preview; a cancel puts
// the live setting back (see the SCREEN_BRIGHTNESS case).
static void brightness_refresh(void)
{
    lv_bar_set_value(s_brightness_bar, s_edit.brightness_pct, LV_ANIM_OFF);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", s_edit.brightness_pct);
    lv_label_set_text(s_brightness_label, buf);
    display_set_brightness((uint8_t)s_edit.brightness_pct);
}

static void brightness_step(int dir)
{
    s_edit.brightness_pct += dir * 5;
    if (s_edit.brightness_pct < SETTINGS_BRIGHTNESS_MIN) s_edit.brightness_pct = SETTINGS_BRIGHTNESS_MIN;
    if (s_edit.brightness_pct > 100)                     s_edit.brightness_pct = 100;
    brightness_refresh();
}

static void rtc_trim_refresh(void)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%+d", s_edit.rtc_trim);
    lv_label_set_text(s_rtc_trim_label, buf);
    lv_bar_set_value(s_rtc_trim_bar, s_edit.rtc_trim, LV_ANIM_OFF);
}

static void rtc_trim_step(int dir)
{
    int v = s_edit.rtc_trim + dir;
    s_edit.rtc_trim = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
    rtc_trim_refresh();
}

// Moves *idx by dir within 0..count-1 and shows it on the roller.
static void roller_step(lv_obj_t *roller, int *idx, int count, int dir)
{
    *idx += dir;
    if (*idx < 0)      *idx = 0;
    if (*idx >= count) *idx = count - 1;
    lv_roller_set_selected(roller, (uint32_t)*idx, LV_ANIM_OFF);
}

// Joins labels into the '\n'-separated list lv_roller_set_options() takes.
static void set_roller_options(lv_obj_t *roller, const char *const *labels, int count)
{
    char opts[192];
    size_t n = 0;
    for (int i = 0; i < count && n < sizeof(opts); i++) {
        n += snprintf(opts + n, sizeof(opts) - n, "%s%s", i ? "\n" : "", labels[i]);
    }
    lv_roller_set_options(roller, opts, LV_ROLLER_MODE_NORMAL);
}

static void tz_table_draw_cb(lv_event_t *e)
{
    lv_draw_task_t   *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
    if (base->part != LV_PART_ITEMS) return;
    if ((int)base->id1 != s_edit.tz_idx) return;

    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(task);
    if (fill) {
        fill->color = lv_palette_main(LV_PALETTE_BLUE);
        fill->opa   = LV_OPA_60;
    }
    lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(task);
    if (label) label->color = lv_color_black();
}

static void tz_table_sync(void)
{
    lv_obj_invalidate(s_tz_table);
    lv_obj_scroll_to_y(s_tz_table,
                       s_edit.tz_idx * TZ_ROW_H - TZ_ROW_H,
                       LV_ANIM_OFF);
}

static void tz_step(int dir)
{
    s_edit.tz_idx += dir;
    if (s_edit.tz_idx < 0)                  s_edit.tz_idx = 0;
    if (s_edit.tz_idx >= SETTINGS_TZ_COUNT) s_edit.tz_idx = SETTINGS_TZ_COUNT - 1;
    tz_table_sync();
}

// ---------------------------------------------------------------------------
static void can_monitor_refresh(void);

// Forward declarations for screen transitions used by callbacks / helpers
// ---------------------------------------------------------------------------

static void ui_open_settings(void);
static void ui_show_settings(void);
static void ui_show_update_rate(void);
static void ui_show_ntp_update_rate(void);
static void ui_show_dst(void);
static void ui_show_timezone(void);
static void ui_show_brightness(void);
static void ui_show_rtc_trim(void);
static void ui_open_network(void);
static void ui_show_network(void);
static void ui_open_ntp(void);
static void ui_open_ip_edit(ip_field_t field);
static void ui_open_can_monitor(void);
static void ui_open_discovery(void);

// ---------------------------------------------------------------------------
// Network screen helpers
// ---------------------------------------------------------------------------

static void network_screen_refresh(void)
{
    lv_label_set_text(s_net_mode_label,
                      s_edit.net_dhcp ? "Mode: DHCP" : "Mode: Static");

    const uint8_t *ip = s_edit.net_ip, *mask = s_edit.net_mask, *gw = s_edit.net_gw;
    char buf[48];
    snprintf(buf, sizeof(buf), "IP  : %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    lv_label_set_text(s_net_ip_label, buf);

    snprintf(buf, sizeof(buf), "Mask: %d.%d.%d.%d", mask[0], mask[1], mask[2], mask[3]);
    lv_label_set_text(s_net_mask_label, buf);

    snprintf(buf, sizeof(buf), "GW  : %d.%d.%d.%d", gw[0], gw[1], gw[2], gw[3]);
    lv_label_set_text(s_net_gw_label, buf);

    if (s_edit.net_dhcp) {
        lv_obj_add_state(s_net_ip_btn,   LV_STATE_DISABLED);
        lv_obj_add_state(s_net_mask_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s_net_gw_btn,   LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_net_ip_btn,   LV_STATE_DISABLED);
        lv_obj_remove_state(s_net_mask_btn, LV_STATE_DISABLED);
        lv_obj_remove_state(s_net_gw_btn,   LV_STATE_DISABLED);
    }

    char server[32];
    config_ntp_server(&s_edit, server, sizeof(server));
    snprintf(buf, sizeof(buf), "NTP: %s", server);
    lv_label_set_text(s_net_ntp_label, buf);
}

// ---------------------------------------------------------------------------
// IP editor helpers
// ---------------------------------------------------------------------------

// The address the IP editor is changing, inside s_edit.
static uint8_t *ip_field_ptr(void)
{
    switch (s_ip_field) {
    case IP_FIELD_IP:   return s_edit.net_ip;
    case IP_FIELD_MASK: return s_edit.net_mask;
    case IP_FIELD_GW:   return s_edit.net_gw;
    default:            return s_edit.ntp_ip;
    }
}

static void ip_edit_refresh(void)
{
    static const char *const field_names[] = {
        "IP Address", "Subnet Mask", "Gateway", "NTP Server IP"
    };
    const uint8_t *oct = ip_field_ptr();

    char title[32];
    snprintf(title, sizeof(title), "%s  [%d/4]",
             field_names[s_ip_field], s_ip_octet + 1);
    lv_label_set_text(s_ip_title_label, title);

    char val[32];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (i > 0) val[n++] = '.';
        if (i == s_ip_octet) val[n++] = '[';
        n += snprintf(val + n, (int)sizeof(val) - n, "%d", oct[i]);
        if (i == s_ip_octet) val[n++] = ']';
    }
    val[n] = '\0';
    lv_label_set_text(s_ip_value_label, val);
}

static void ip_octet_step(int dir)
{
    uint8_t *oct = ip_field_ptr();
    int val = (int)oct[s_ip_octet] + dir;
    if (val < 0)   val = 255;
    if (val > 255) val = 0;
    oct[s_ip_octet] = (uint8_t)val;
    ip_edit_refresh();
}

static void ip_octet_next(void)
{
    if (s_ip_octet < 3) {
        s_ip_octet++;
        ip_edit_refresh();
    } else {
        ui_show_network();
    }
}

// ---------------------------------------------------------------------------
// NTP picker helpers
// ---------------------------------------------------------------------------

static void ntp_refresh(void)
{
    lv_roller_set_selected(s_ntp_roller, (uint32_t)s_edit.ntp_idx, LV_ANIM_OFF);

    lv_label_set_text(s_ntp_hint_label,
                      (s_edit.ntp_idx == NTP_MANUAL) ? "Click: edit IP  Hold: cancel"
                                                     : "Click: save  Hold: cancel");
}

static void ntp_step(int dir)
{
    s_edit.ntp_idx += dir;
    if (s_edit.ntp_idx < 0)          s_edit.ntp_idx = 0;
    if (s_edit.ntp_idx > NTP_MANUAL) s_edit.ntp_idx = NTP_MANUAL;
    ntp_refresh();
}

static void ntp_confirm(void)
{
    if (s_edit.ntp_idx == NTP_MANUAL) {
        ui_open_ip_edit(IP_FIELD_NTP);
    } else {
        ui_show_network();
    }
}

// ---------------------------------------------------------------------------
// Notice — a red message over whatever screen is showing, hidden after a while
// ---------------------------------------------------------------------------

#define NOTICE_MS  4000

static lv_obj_t   *s_notice       = NULL;
static lv_timer_t *s_notice_timer = NULL;

static void notice_hide_cb(lv_timer_t *t)
{
    lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(t);
}

static void show_notice(const char *msg)
{
    if (!s_notice) {
        // The top layer stays visible across screen changes.
        s_notice = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_font(s_notice, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_notice, lv_color_white(), 0);
        lv_obj_set_style_text_align(s_notice, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(s_notice, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_set_style_bg_opa(s_notice, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_notice, 6, 0);
        lv_obj_set_style_radius(s_notice, 4, 0);
        lv_obj_align(s_notice, LV_ALIGN_CENTER, 0, 0);
        s_notice_timer = lv_timer_create(notice_hide_cb, NOTICE_MS, NULL);
    }
    lv_label_set_text(s_notice, msg);
    lv_obj_remove_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(s_notice_timer);
    lv_timer_resume(s_notice_timer);
}

// ---------------------------------------------------------------------------
// Commit the open screen's edit to the live settings (and NVS)
// ---------------------------------------------------------------------------

// Copies only the fields the current screen edits into the live settings, so a
// change made elsewhere meanwhile (e.g. from the web page) isn't overwritten.
// Call before switching screens.
static void commit_settings(void)
{
    settings_t s;
    config_get(&s);
    switch (s_current_screen) {
    case SCREEN_UPDATE_RATE:     s.rate_idx       = s_edit.rate_idx;       break;
    case SCREEN_NTP_UPDATE_RATE: s.ntp_rate_idx   = s_edit.ntp_rate_idx;   break;
    case SCREEN_DST:             s.dst_idx        = s_edit.dst_idx;        break;
    case SCREEN_TIMEZONE:        s.tz_idx         = s_edit.tz_idx;         break;
    case SCREEN_BRIGHTNESS:      s.brightness_pct = s_edit.brightness_pct; break;
    case SCREEN_RTC_TRIM:        s.rtc_trim       = s_edit.rtc_trim;       break;
    case SCREEN_NETWORK:         // addresses and NTP server
        s.net_dhcp = s_edit.net_dhcp;
        s.ntp_idx  = s_edit.ntp_idx;
        memcpy(s.net_ip,   s_edit.net_ip,   4);
        memcpy(s.net_mask, s_edit.net_mask, 4);
        memcpy(s.net_gw,   s_edit.net_gw,   4);
        memcpy(s.ntp_ip,   s_edit.ntp_ip,   4);
        break;
    default:
        return;
    }

    // The change still applies until the next reboot; tell the user it won't last.
    esp_err_t err = config_set(&s);
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Settings NOT saved\n%s", esp_err_to_name(err));
        show_notice(msg);
    }
}

// ---------------------------------------------------------------------------
// LVGL event callbacks (fired by group_click → lv_obj_send_event)
// ---------------------------------------------------------------------------

static void on_menu_exit(lv_event_t *e)     { ui_show_main(); }
static void on_settings_back(lv_event_t *e) { ui_show_menu(); }
// Show the current IP and connection state on the main screen.
// Call from the LVGL task or with display_lock() held.
static void show_network_status(void)
{
    char ip[16], label[24];
    network_get_ip_str(ip, sizeof(ip));   // one snapshot: "" while not connected
    snprintf(label, sizeof(label), "IP:%s", ip);
    ui_set_ip(ip[0] ? label : "IP: --");
    ui_set_connection_state(ip[0] ? UI_CONN_LAN : UI_CONN_DISCONNECTED);
}

static void on_net_apply(lv_event_t *e)
{
    commit_settings();       // config_set() reapplies the network if it changed
    show_network_status();   // already in the LVGL task, no display_lock() needed
    ui_show_menu();
}
static void on_net_back(lv_event_t *e) { ui_show_menu(); }   // drops the edit
static void on_net_ip_btn(lv_event_t *e)    { ui_open_ip_edit(IP_FIELD_IP); }
static void on_net_mask_btn(lv_event_t *e)  { ui_open_ip_edit(IP_FIELD_MASK); }
static void on_net_gw_btn(lv_event_t *e)    { ui_open_ip_edit(IP_FIELD_GW); }
static void on_net_ntp_btn(lv_event_t *e)   { ui_open_ntp(); }

static void on_net_mode(lv_event_t *e)
{
    s_edit.net_dhcp = !s_edit.net_dhcp;
    network_screen_refresh();
    lv_group_focus_obj(s_net_mode_btn);
}

static void on_menu_select(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);

    switch (idx) {
    case 0: ui_open_settings(); break;
    case 1: ui_show_brightness(); break;
    case 2: ui_open_network(); break;
    case 3: ui_open_can_monitor(); break;
    case 4: ui_open_discovery(); break;
    default: {
        const char *text = lv_list_get_button_text(lv_obj_get_parent(btn), btn);
        lv_label_set_text(s_status_label, text);
        break;
    }
    }
}

static const char *s_menu_items[] = {
    LV_SYMBOL_SETTINGS " Timeserver settings",
    LV_SYMBOL_IMAGE    " Display",
    LV_SYMBOL_WIFI     " Network",
    LV_SYMBOL_WARNING  " CAN Monitor",
    LV_SYMBOL_LIST     " Find devices",
};

static void on_settings_select(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx == 0) ui_show_update_rate();
    if (idx == 1) ui_show_ntp_update_rate();
    if (idx == 2) ui_show_dst();
    if (idx == 3) ui_show_timezone();
    if (idx == 4) ui_show_rtc_trim();
}

// ---------------------------------------------------------------------------
// Screen builders
// ---------------------------------------------------------------------------

static void build_main_screen(void)
{
    s_main_scr = lv_obj_create(NULL);

    s_ip_label = lv_label_create(s_main_scr);
    lv_obj_set_style_text_font(s_ip_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_ip_label, "IP: --");
    lv_obj_align(s_ip_label, LV_ALIGN_TOP_LEFT, 4, 4);

    s_time_label = lv_label_create(s_main_scr);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_46, 0);
    lv_label_set_text(s_time_label, "--:--:--");   // dashes until clock_task has a valid time
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -10);

    s_date_label = lv_label_create(s_main_scr);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_date_label, "----/--/--");
    lv_obj_align(s_date_label, LV_ALIGN_CENTER, 0, 20);

    s_connected_label = lv_label_create(s_main_scr);
    lv_obj_set_style_text_font(s_connected_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_connected_label, "Disconnected");
    lv_obj_align(s_connected_label, LV_ALIGN_TOP_RIGHT, -4, 4);

    s_last_sync_label = lv_label_create(s_main_scr);
    lv_obj_set_style_text_font(s_last_sync_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_last_sync_label, "Last sync: NTP: --:--:-- CAN: --:--:--");
    lv_obj_align(s_last_sync_label, LV_ALIGN_BOTTOM_LEFT, 4, -4);
}

static void build_menu_screen(void)
{
    s_menu_scr = lv_obj_create(NULL);

    s_status_label = lv_label_create(s_menu_scr);
    lv_label_set_text(s_status_label, "Select an item");
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_status_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_t *list = lv_list_create(s_menu_scr);
    lv_obj_set_size(list, LV_PCT(100), 145);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);

    for (int i = 0; i < (int)(sizeof(s_menu_items) / sizeof(s_menu_items[0])); i++) {
        lv_obj_t *btn = lv_list_add_button(list, NULL, s_menu_items[i]);
        lv_obj_set_height(btn, LV_PCT(30));
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_20, 0);
        lv_group_add_obj(s_menu_group, btn);
        lv_obj_add_event_cb(btn, on_menu_select, LV_EVENT_CLICKED, NULL);
        if (i == 0) s_first_menu_btn = btn;
    }

    lv_obj_t *exit_btn = lv_list_add_button(list, LV_SYMBOL_HOME " ", "Exit");
    lv_obj_set_height(exit_btn, LV_PCT(30));
    lv_obj_set_style_text_font(exit_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_menu_group, exit_btn);
    lv_obj_add_event_cb(exit_btn, on_menu_exit, LV_EVENT_CLICKED, NULL);
}

static void build_settings_screen(void)
{
    s_settings_scr = lv_obj_create(NULL);

    lv_obj_t *list = lv_list_create(s_settings_scr);
    lv_obj_set_size(list, LV_PCT(100), 145);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_REFRESH, "CAN Update Rate");
    lv_obj_set_height(btn, LV_PCT(30));
    lv_obj_set_user_data(btn, (void *)(intptr_t)0);
    lv_obj_set_style_text_font(btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_settings_group, btn);
    lv_obj_add_event_cb(btn, on_settings_select, LV_EVENT_CLICKED, NULL);
    s_first_settings_btn = btn;

    lv_obj_t *ntp_btn = lv_list_add_button(list, LV_SYMBOL_REFRESH, "NTP Update Rate");
    lv_obj_set_height(ntp_btn, LV_PCT(30));
    lv_obj_set_user_data(ntp_btn, (void *)(intptr_t)1);
    lv_obj_set_style_text_font(ntp_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_settings_group, ntp_btn);
    lv_obj_add_event_cb(ntp_btn, on_settings_select, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dst_btn = lv_list_add_button(list, LV_SYMBOL_SETTINGS, "Daylight Savings");
    lv_obj_set_height(dst_btn, LV_PCT(30));
    lv_obj_set_user_data(dst_btn, (void *)(intptr_t)2);
    lv_obj_set_style_text_font(dst_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_settings_group, dst_btn);
    lv_obj_add_event_cb(dst_btn, on_settings_select, LV_EVENT_CLICKED, NULL);

    lv_obj_t *tz_btn = lv_list_add_button(list, LV_SYMBOL_SETTINGS, "UTC Timezone");
    lv_obj_set_height(tz_btn, LV_PCT(30));
    lv_obj_set_user_data(tz_btn, (void *)(intptr_t)3);
    lv_obj_set_style_text_font(tz_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_settings_group, tz_btn);
    lv_obj_add_event_cb(tz_btn, on_settings_select, LV_EVENT_CLICKED, NULL);

    lv_obj_t *trim_btn = lv_list_add_button(list, LV_SYMBOL_SETTINGS, "RTC Trim");
    lv_obj_set_height(trim_btn, LV_PCT(30));
    lv_obj_set_user_data(trim_btn, (void *)(intptr_t)4);
    lv_obj_set_style_text_font(trim_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_settings_group, trim_btn);
    lv_obj_add_event_cb(trim_btn, on_settings_select, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_btn = lv_list_add_button(list, LV_SYMBOL_LEFT, "Back");
    lv_obj_set_height(back_btn, LV_PCT(30));
    lv_obj_set_style_text_font(back_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_settings_group, back_btn);
    lv_obj_add_event_cb(back_btn, on_settings_back, LV_EVENT_CLICKED, NULL);
}

static void build_update_rate_screen(void)
{
    s_update_rate_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_update_rate_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "CAN Update Rate");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_update_rate_roller = lv_roller_create(s_update_rate_scr);
    set_roller_options(s_update_rate_roller, config_can_rate_labels, SETTINGS_CAN_RATE_COUNT);
    lv_obj_set_style_text_font(s_update_rate_roller, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(s_update_rate_roller, 2, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_update_rate_roller, 2, LV_PART_SELECTED);
    lv_roller_set_visible_row_count(s_update_rate_roller, 5);
    lv_obj_set_width(s_update_rate_roller, LV_PCT(80));
    lv_obj_align(s_update_rate_roller, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *hint = lv_label_create(s_update_rate_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "Click: apply  Hold: cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_ntp_update_rate_screen(void)
{
    s_ntp_update_rate_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_ntp_update_rate_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "NTP Update Rate");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_ntp_update_rate_roller = lv_roller_create(s_ntp_update_rate_scr);
    set_roller_options(s_ntp_update_rate_roller, config_ntp_rate_labels, SETTINGS_NTP_RATE_COUNT);
    lv_obj_set_style_text_font(s_ntp_update_rate_roller, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(s_ntp_update_rate_roller, 2, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_ntp_update_rate_roller, 2, LV_PART_SELECTED);
    lv_roller_set_visible_row_count(s_ntp_update_rate_roller, 5);
    lv_obj_set_width(s_ntp_update_rate_roller, LV_PCT(80));
    lv_obj_align(s_ntp_update_rate_roller, LV_ALIGN_CENTER, 0, 6);

    s_ntp_rate_hint = lv_label_create(s_ntp_update_rate_scr);
    lv_obj_set_style_text_font(s_ntp_rate_hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_ntp_rate_hint, "Click: apply  Hold: cancel");
    lv_obj_align(s_ntp_rate_hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_dst_screen(void)
{
    s_dst_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_dst_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "Daylight Savings");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_dst_roller = lv_roller_create(s_dst_scr);
    set_roller_options(s_dst_roller, config_dst_labels, SETTINGS_DST_COUNT);
    lv_obj_set_style_text_font(s_dst_roller, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(s_dst_roller, 2, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_dst_roller, 2, LV_PART_SELECTED);
    lv_roller_set_visible_row_count(s_dst_roller, 3);
    lv_obj_set_width(s_dst_roller, LV_PCT(60));
    lv_obj_align(s_dst_roller, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *hint = lv_label_create(s_dst_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "Click: apply  Hold: cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_timezone_screen(void)
{
    s_tz_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_tz_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "UTC Timezone");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_tz_table = lv_table_create(s_tz_scr);
    lv_table_set_column_count(s_tz_table, 1);
    lv_table_set_row_count(s_tz_table, SETTINGS_TZ_COUNT);
    lv_table_set_col_width(s_tz_table, 0, 180);
    lv_obj_set_size(s_tz_table, 184, 112);
    lv_obj_set_style_text_font(s_tz_table, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_text_align(s_tz_table, LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS);
    lv_obj_align(s_tz_table, LV_ALIGN_CENTER, 0, 4);

    for (int i = 0; i < SETTINGS_TZ_COUNT; i++)
        lv_table_set_cell_value(s_tz_table, i, 0, config_tz_labels[i]);

    lv_obj_add_flag(s_tz_table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(s_tz_table, tz_table_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);

    lv_obj_t *hint = lv_label_create(s_tz_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "Click: apply  Hold: cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_brightness_screen(void)
{
    s_brightness_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_brightness_scr);
    lv_label_set_text(title, "Brightness");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Values are filled in by brightness_refresh() when the screen opens.
    s_brightness_label = lv_label_create(s_brightness_scr);
    lv_obj_set_style_text_font(s_brightness_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_brightness_label, LV_ALIGN_CENTER, 0, -14);

    s_brightness_bar = lv_bar_create(s_brightness_scr);
    lv_obj_set_size(s_brightness_bar, 280, 20);
    lv_bar_set_range(s_brightness_bar, SETTINGS_BRIGHTNESS_MIN, 100);
    lv_obj_align(s_brightness_bar, LV_ALIGN_CENTER, 0, 18);

    lv_obj_t *hint = lv_label_create(s_brightness_scr);
    lv_label_set_text(hint, "Click: apply  Hold: cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_rtc_trim_screen(void)
{
    s_rtc_trim_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_rtc_trim_scr);
    lv_label_set_text(title, "RTC Trim");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_rtc_trim_label = lv_label_create(s_rtc_trim_scr);
    lv_obj_set_style_text_font(s_rtc_trim_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_rtc_trim_label, LV_ALIGN_CENTER, 0, -14);

    s_rtc_trim_bar = lv_bar_create(s_rtc_trim_scr);
    lv_obj_set_size(s_rtc_trim_bar, 280, 20);
    lv_bar_set_mode(s_rtc_trim_bar, LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_range(s_rtc_trim_bar, -128, 127);
    lv_obj_align(s_rtc_trim_bar, LV_ALIGN_CENTER, 0, 18);

    lv_obj_t *hint = lv_label_create(s_rtc_trim_scr);
    lv_label_set_text(hint, "Click: apply  Hold: cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    rtc_trim_refresh();
}

static void build_network_screen(void)
{
    s_network_scr = lv_obj_create(NULL);

    lv_obj_t *list = lv_list_create(s_network_scr);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);

    // Mode toggle — label is child 0 (no icon)
    s_net_mode_btn = lv_list_add_button(list, NULL, "Mode: DHCP");
    lv_obj_set_height(s_net_mode_btn, 38);
    lv_obj_set_style_text_font(s_net_mode_btn, &lv_font_montserrat_20, 0);
    s_net_mode_label = lv_obj_get_child(s_net_mode_btn, 0);
    lv_group_add_obj(s_network_group, s_net_mode_btn);
    lv_obj_add_event_cb(s_net_mode_btn, on_net_mode, LV_EVENT_CLICKED, NULL);

    // IP address — label is child 0 (no icon)
    s_net_ip_btn = lv_list_add_button(list, NULL, "IP  : 192.168.1.100");
    lv_obj_set_height(s_net_ip_btn, 38);
    lv_obj_set_style_text_font(s_net_ip_btn, &lv_font_montserrat_20, 0);
    s_net_ip_label = lv_obj_get_child(s_net_ip_btn, 0);
    lv_group_add_obj(s_network_group, s_net_ip_btn);
    lv_obj_add_event_cb(s_net_ip_btn, on_net_ip_btn, LV_EVENT_CLICKED, NULL);

    // Subnet mask — label is child 0 (no icon)
    s_net_mask_btn = lv_list_add_button(list, NULL, "Mask: 255.255.255.0");
    lv_obj_set_height(s_net_mask_btn, 38);
    lv_obj_set_style_text_font(s_net_mask_btn, &lv_font_montserrat_20, 0);
    s_net_mask_label = lv_obj_get_child(s_net_mask_btn, 0);
    lv_group_add_obj(s_network_group, s_net_mask_btn);
    lv_obj_add_event_cb(s_net_mask_btn, on_net_mask_btn, LV_EVENT_CLICKED, NULL);

    // Gateway — label is child 0 (no icon)
    s_net_gw_btn = lv_list_add_button(list, NULL, "GW  : 192.168.1.1");
    lv_obj_set_height(s_net_gw_btn, 38);
    lv_obj_set_style_text_font(s_net_gw_btn, &lv_font_montserrat_20, 0);
    s_net_gw_label = lv_obj_get_child(s_net_gw_btn, 0);
    lv_group_add_obj(s_network_group, s_net_gw_btn);
    lv_obj_add_event_cb(s_net_gw_btn, on_net_gw_btn, LV_EVENT_CLICKED, NULL);

    // NTP server
    s_net_ntp_btn = lv_list_add_button(list, NULL, "NTP: pool.ntp.org");
    lv_obj_set_height(s_net_ntp_btn, 38);
    lv_obj_set_style_text_font(s_net_ntp_btn, &lv_font_montserrat_20, 0);
    s_net_ntp_label = lv_obj_get_child(s_net_ntp_btn, 0);
    lv_group_add_obj(s_network_group, s_net_ntp_btn);
    lv_obj_add_event_cb(s_net_ntp_btn, on_net_ntp_btn, LV_EVENT_CLICKED, NULL);

    // Apply
    lv_obj_t *apply_btn = lv_list_add_button(list, LV_SYMBOL_OK, "Apply");
    lv_obj_set_height(apply_btn, 38);
    lv_obj_set_style_text_font(apply_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_network_group, apply_btn);
    lv_obj_add_event_cb(apply_btn, on_net_apply, LV_EVENT_CLICKED, NULL);

    // Back
    lv_obj_t *back_btn = lv_list_add_button(list, LV_SYMBOL_LEFT, "Back");
    lv_obj_set_height(back_btn, 38);
    lv_obj_set_style_text_font(back_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_network_group, back_btn);
    lv_obj_add_event_cb(back_btn, on_net_back, LV_EVENT_CLICKED, NULL);
}

static void build_ntp_screen(void)
{
    s_ntp_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_ntp_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "NTP Server");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_ntp_roller = lv_roller_create(s_ntp_scr);
    set_roller_options(s_ntp_roller, config_ntp_server_labels, SETTINGS_NTP_SERVER_COUNT);
    lv_obj_set_style_text_font(s_ntp_roller, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(s_ntp_roller, 2, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_ntp_roller, 2, LV_PART_SELECTED);
    lv_roller_set_visible_row_count(s_ntp_roller, 5);
    lv_obj_set_width(s_ntp_roller, LV_PCT(80));
    lv_obj_align(s_ntp_roller, LV_ALIGN_CENTER, 0, 4);

    s_ntp_hint_label = lv_label_create(s_ntp_scr);
    lv_obj_set_style_text_font(s_ntp_hint_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_ntp_hint_label, "Click: save  Hold: cancel");
    lv_obj_align(s_ntp_hint_label, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void build_ip_edit_screen(void)
{
    s_ip_edit_scr = lv_obj_create(NULL);

    s_ip_title_label = lv_label_create(s_ip_edit_scr);
    lv_obj_set_style_text_font(s_ip_title_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_ip_title_label, "IP Address  [1/4]");
    lv_obj_align(s_ip_title_label, LV_ALIGN_TOP_MID, 0, 10);

    s_ip_value_label = lv_label_create(s_ip_edit_scr);
    lv_obj_set_style_text_font(s_ip_value_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_ip_value_label, "[192].168.1.100");
    lv_obj_align(s_ip_value_label, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *hint = lv_label_create(s_ip_edit_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "Rotate: change  Click: next");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void can_monitor_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_current_screen != SCREEN_CAN_MONITOR) return;
    if (!s_can_follow) return;
    if (!can_mon_has_new_data()) return;
    can_monitor_refresh();
}

static void can_monitor_refresh(void)
{
    if (s_current_screen != SCREEN_CAN_MONITOR) return;
    if (!s_can_follow) return;

    settings_t cfg;
    config_get(&cfg);
    can_mon_snapshot(s_can_log_text, sizeof(s_can_log_text), config_tz_offset_hours(&cfg), cfg.dst_idx);
    lv_label_set_text_static(s_can_log,
                             s_can_log_text[0] ? s_can_log_text : "Waiting for CAN frames...");
    // Recompute the container's content height for the new text first;
    // otherwise the scroll stops at the old bottom and hides the newest lines.
    lv_obj_update_layout(s_can_cont);
    lv_obj_scroll_to_y(s_can_cont, 100000, LV_ANIM_OFF);
}

static void build_can_monitor_screen(void)
{
    s_can_scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(s_can_scr, 0, 0);

    // Thin header bar
    lv_obj_t *hdr = lv_label_create(s_can_scr);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
    lv_label_set_text(hdr, LV_SYMBOL_WARNING " CAN Monitor  (long-press: back)");
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 4, 2);

    // Scrollable log container below the header
    int log_h = lv_display_get_vertical_resolution(NULL) - 18;
    s_can_cont = lv_obj_create(s_can_scr);
    lv_obj_set_size(s_can_cont, LV_PCT(100), log_h);
    lv_obj_align(s_can_cont, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_pad_all(s_can_cont, 2, 0);
    lv_obj_set_scroll_dir(s_can_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_can_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(s_can_cont, LV_OBJ_FLAG_SCROLL_ELASTIC);

    s_can_log = lv_label_create(s_can_cont);
    lv_obj_set_width(s_can_log, LV_PCT(100));
    lv_label_set_long_mode(s_can_log, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_can_log, &lv_font_montserrat_14, 0);
    lv_obj_align(s_can_log, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text_static(s_can_log, "Waiting for CAN frames...");
}

// ---------------------------------------------------------------------------
// Find devices screen
// ---------------------------------------------------------------------------

static void discovery_screen_refresh(void)
{
    // Leave room for the status line after the list.
    int count = discovery_snapshot(s_disc_text, sizeof(s_disc_text) - 64);
    size_t n = strlen(s_disc_text);
    if (network_is_connected()) {
        snprintf(s_disc_text + n, sizeof(s_disc_text) - n, "%sSearching... %d found  (click: start over)",
                 n ? "\n" : "", count);
    } else {
        snprintf(s_disc_text + n, sizeof(s_disc_text) - n, "%sNo network", n ? "\n" : "");
    }
    lv_label_set_text_static(s_disc_log, s_disc_text);
}

static void discovery_poll_cb(lv_timer_t *t)
{
    static bool was_up = false;
    if (s_current_screen != SCREEN_DISCOVERY) return;
    bool up = network_is_connected();
    if (discovery_has_new_data() || up != was_up) discovery_screen_refresh();
    was_up = up;
}

static void build_discovery_screen(void)
{
    s_disc_scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(s_disc_scr, 0, 0);

    lv_obj_t *hdr = lv_label_create(s_disc_scr);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
    lv_label_set_text(hdr, LV_SYMBOL_LIST " Find devices  (long-press: back)");
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 4, 2);

    s_disc_cont = lv_obj_create(s_disc_scr);
    lv_obj_set_size(s_disc_cont, LV_PCT(100), lv_display_get_vertical_resolution(NULL) - 18);
    lv_obj_align(s_disc_cont, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_pad_all(s_disc_cont, 2, 0);
    lv_obj_set_scroll_dir(s_disc_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_disc_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(s_disc_cont, LV_OBJ_FLAG_SCROLL_ELASTIC);

    s_disc_log = lv_label_create(s_disc_cont);
    lv_obj_set_width(s_disc_log, LV_PCT(100));
    lv_label_set_long_mode(s_disc_log, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_disc_log, &lv_font_montserrat_14, 0);
    lv_obj_align(s_disc_log, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text_static(s_disc_log, "");
}

// ---------------------------------------------------------------------------
// Screen transitions (all run in the LVGL task)
// ---------------------------------------------------------------------------

void ui_open_menu(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_MENU;
    lv_indev_set_group(s_enc_indev, s_menu_group);
    lv_screen_load(s_menu_scr);
    lv_group_focus_obj(s_first_menu_btn);
}

void ui_show_menu(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_MENU;
    lv_indev_set_group(s_enc_indev, s_menu_group);
    lv_screen_load(s_menu_scr);
    lv_obj_t *prev = lv_group_get_focused(s_menu_group);
    if (prev) lv_group_focus_obj(prev);
}

void ui_show_main(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_MAIN;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_main_scr);
}

static void ui_open_settings(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_SETTINGS;
    lv_indev_set_group(s_enc_indev, s_settings_group);
    lv_screen_load(s_settings_scr);
    lv_group_focus_obj(s_first_settings_btn);
}

static void ui_show_settings(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_SETTINGS;
    lv_indev_set_group(s_enc_indev, s_settings_group);
    lv_screen_load(s_settings_scr);
    lv_obj_t *prev = lv_group_get_focused(s_settings_group);
    if (prev) lv_group_focus_obj(prev);
}

static void ui_show_update_rate(void)
{
    config_get(&s_edit);
    lv_roller_set_selected(s_update_rate_roller, (uint32_t)s_edit.rate_idx, LV_ANIM_OFF);
    s_enc_accum = 0;
    s_current_screen = SCREEN_UPDATE_RATE;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_update_rate_scr);
}

static void ui_show_ntp_update_rate(void)
{
    // Without the DS3231 the clock syncs every CLOCK_NO_RTC_NTP_RATE_SEC (5 min,
    // roller index 0) whatever is set here; show that, and keep the saved choice
    // for when the RTC is back. ntp_update_rate_step() ignores the knob.
    config_get(&s_edit);
    bool fixed = !clock_has_rtc();
    lv_roller_set_selected(s_ntp_update_rate_roller, fixed ? 0 : (uint32_t)s_edit.ntp_rate_idx, LV_ANIM_OFF);
    lv_label_set_text(s_ntp_rate_hint, fixed ? "Fixed: no RTC  Click: back" : "Click: apply  Hold: cancel");

    s_enc_accum = 0;
    s_current_screen = SCREEN_NTP_UPDATE_RATE;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_ntp_update_rate_scr);
}

static void ui_show_dst(void)
{
    config_get(&s_edit);
    lv_roller_set_selected(s_dst_roller, (uint32_t)s_edit.dst_idx, LV_ANIM_OFF);
    s_enc_accum = 0;
    s_current_screen = SCREEN_DST;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_dst_scr);
}

static void ui_show_timezone(void)
{
    config_get(&s_edit);
    s_enc_accum = 0;
    s_current_screen = SCREEN_TIMEZONE;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_tz_scr);
    tz_table_sync();
}

static void ui_show_brightness(void)
{
    config_get(&s_edit);
    brightness_refresh();
    s_enc_accum = 0;
    s_current_screen = SCREEN_BRIGHTNESS;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_brightness_scr);
}

static void ui_show_rtc_trim(void)
{
    config_get(&s_edit);
    s_enc_accum = 0;
    s_current_screen = SCREEN_RTC_TRIM;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    rtc_trim_refresh();
    lv_screen_load(s_rtc_trim_scr);
}

// Entering the network screen from the menu starts a new edit; coming back from
// the IP editor or NTP picker (ui_show_network) keeps it.
static void ui_open_network(void)
{
    config_get(&s_edit);
    s_enc_accum = 0;
    s_current_screen = SCREEN_NETWORK;
    lv_indev_set_group(s_enc_indev, s_network_group);
    network_screen_refresh();
    lv_screen_load(s_network_scr);
    lv_group_focus_obj(s_net_mode_btn);
}

static void ui_show_network(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_NETWORK;
    lv_indev_set_group(s_enc_indev, s_network_group);
    network_screen_refresh();
    lv_screen_load(s_network_scr);
    lv_obj_t *prev = lv_group_get_focused(s_network_group);
    if (prev) lv_group_focus_obj(prev);
}

static void ui_open_ntp(void)
{
    s_ntp_idx_backup = s_edit.ntp_idx;
    memcpy(s_ntp_ip_backup, s_edit.ntp_ip, 4);
    s_enc_accum = 0;
    s_current_screen = SCREEN_NTP;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    ntp_refresh();
    lv_screen_load(s_ntp_scr);
}

static void ui_open_ip_edit(ip_field_t field)
{
    s_ip_field  = field;
    s_ip_octet  = 0;
    memcpy(s_ip_backup, ip_field_ptr(), 4);
    s_enc_accum = 0;
    s_current_screen = SCREEN_IP_EDIT;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    ip_edit_refresh();
    lv_screen_load(s_ip_edit_scr);
}

static void ui_open_can_monitor(void)
{
    s_enc_accum  = 0;
    s_can_follow = true;
    s_current_screen = SCREEN_CAN_MONITOR;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    can_monitor_refresh();
    lv_screen_load(s_can_scr);
}

static void ui_open_discovery(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_DISCOVERY;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    discovery_start();
    discovery_screen_refresh();
    lv_screen_load(s_disc_scr);
}

// ---------------------------------------------------------------------------
// Central encoder dispatcher — runs in the LVGL task (main.c enc_queue_drain_cb)
// ---------------------------------------------------------------------------

void ui_encoder_event(encoder_event_t event)
{
    int step = accum_step(event);

    switch (s_current_screen) {

    case SCREEN_MAIN:
        if (event == ENCODER_LONG) ui_open_menu();
        break;

    case SCREEN_MENU:
        if      (step != 0)              group_nav(s_menu_group, step);
        else if (event == ENCODER_CLICK) group_click(s_menu_group);
        break;

    case SCREEN_SETTINGS:
        if      (step != 0)              group_nav(s_settings_group, step);
        else if (event == ENCODER_CLICK) group_click(s_settings_group);
        break;

    // Value screens: turn to edit s_edit, Click commits it, Hold drops it.
    case SCREEN_UPDATE_RATE:
        if      (step != 0)               roller_step(s_update_rate_roller, &s_edit.rate_idx, SETTINGS_CAN_RATE_COUNT, step);
        else if (event == ENCODER_CLICK) { commit_settings(); ui_show_settings(); }
        else if (event == ENCODER_LONG)   ui_show_settings();
        break;

    case SCREEN_NTP_UPDATE_RATE:   // fixed at 5 min without the RTC, see ui_show_ntp_update_rate()
        if      (step != 0 && clock_has_rtc()) roller_step(s_ntp_update_rate_roller, &s_edit.ntp_rate_idx, SETTINGS_NTP_RATE_COUNT, step);
        else if (event == ENCODER_CLICK) { commit_settings(); ui_show_settings(); }
        else if (event == ENCODER_LONG)   ui_show_settings();
        break;

    case SCREEN_DST:
        if      (step != 0)               roller_step(s_dst_roller, &s_edit.dst_idx, SETTINGS_DST_COUNT, step);
        else if (event == ENCODER_CLICK) { commit_settings(); ui_show_settings(); }
        else if (event == ENCODER_LONG)   ui_show_settings();
        break;

    case SCREEN_TIMEZONE:
        if      (step != 0)               tz_step(step);
        else if (event == ENCODER_CLICK) { commit_settings(); ui_show_settings(); }
        else if (event == ENCODER_LONG)   ui_show_settings();
        break;

    case SCREEN_BRIGHTNESS:
        if      (step != 0)               brightness_step(step);
        else if (event == ENCODER_CLICK) { commit_settings(); ui_show_menu(); }
        else if (event == ENCODER_LONG) {
            settings_t cur;   // undo the preview
            config_get(&cur);
            display_set_brightness((uint8_t)cur.brightness_pct);
            ui_show_menu();
        }
        break;

    case SCREEN_RTC_TRIM:
        if      (step != 0)               rtc_trim_step(step);
        else if (event == ENCODER_CLICK) { commit_settings(); ui_show_settings(); }
        else if (event == ENCODER_LONG)   ui_show_settings();
        break;

    case SCREEN_NETWORK:
        if      (step != 0)              group_nav(s_network_group, step);
        else if (event == ENCODER_CLICK) group_click(s_network_group);
        break;

    case SCREEN_NTP:
        if      (step != 0)               ntp_step(step);
        else if (event == ENCODER_CLICK)  ntp_confirm();
        else if (event == ENCODER_LONG) {
            s_edit.ntp_idx = s_ntp_idx_backup;
            memcpy(s_edit.ntp_ip, s_ntp_ip_backup, 4);
            ui_show_network();
        }
        break;

    case SCREEN_IP_EDIT:
        if      (step != 0)               ip_octet_step(step);
        else if (event == ENCODER_CLICK)  ip_octet_next();
        else if (event == ENCODER_LONG) {
            memcpy(ip_field_ptr(), s_ip_backup, 4);
            ui_show_network();
        }
        break;

    case SCREEN_CAN_MONITOR:
        if (event == ENCODER_CW) {
            s_can_follow = false;
            lv_obj_scroll_by(s_can_cont, 0, -8, LV_ANIM_OFF);
        } else if (event == ENCODER_CCW) {
            s_can_follow = false;
            lv_obj_scroll_by(s_can_cont, 0, 8, LV_ANIM_OFF);
        } else if (event == ENCODER_CLICK) {
            s_can_follow = true;
            can_mon_push_separator_from_ui();
            can_monitor_refresh();
        } else if (event == ENCODER_LONG) {
            ui_open_menu();
        }
        break;

    case SCREEN_DISCOVERY:
        if (event == ENCODER_CW) {
            lv_obj_scroll_by(s_disc_cont, 0, -8, LV_ANIM_OFF);
        } else if (event == ENCODER_CCW) {
            lv_obj_scroll_by(s_disc_cont, 0, 8, LV_ANIM_OFF);
        } else if (event == ENCODER_CLICK) {
            discovery_start();   // clear the list and search again
            discovery_screen_refresh();
        } else if (event == ENCODER_LONG) {
            discovery_stop();
            ui_open_menu();
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t ui_init(lv_indev_t *encoder_indev)
{
    // Start the backlight at the saved level. Each settings screen loads its
    // values from config when it opens, so nothing else needs seeding here.
    settings_t cfg;
    config_get(&cfg);
    display_set_brightness((uint8_t)cfg.brightness_pct);

    s_enc_indev      = encoder_indev;
    s_empty_group    = lv_group_create();
    s_menu_group     = lv_group_create();
    s_settings_group = lv_group_create();
    s_network_group  = lv_group_create();
    lv_group_set_wrap(s_menu_group,     false);
    lv_group_set_wrap(s_settings_group, false);
    lv_group_set_wrap(s_network_group,  false);

    build_main_screen();
    build_menu_screen();
    build_settings_screen();
    build_update_rate_screen();
    build_ntp_update_rate_screen();
    build_dst_screen();
    build_timezone_screen();
    build_brightness_screen();
    build_rtc_trim_screen();
    build_network_screen();
    build_ntp_screen();
    build_ip_edit_screen();
    build_can_monitor_screen();
    lv_timer_create(can_monitor_poll_cb, 100, NULL);
    build_discovery_screen();
    lv_timer_create(discovery_poll_cb, 250, NULL);

    s_current_screen = SCREEN_MAIN;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_main_scr);

    // A static IP is applied in app_main before the UI exists, so show it now.
    show_network_status();

    // Signal clock_task that all LVGL widgets are built and ui_set_* calls are safe
    clock_notify_ui_ready();

    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

// Guarded: the network "got IP" event can fire before ui_init() builds the label.
void ui_set_ip(const char *ip)  { if (s_ip_label) lv_label_set_text(s_ip_label, ip); }
void ui_set_time(const char *t) { lv_label_set_text(s_time_label, t);  }
void ui_set_date(const char *d) { lv_label_set_text(s_date_label, d);  }

void ui_show_init_errors(bool eth_failed, bool rtc_failed)
{
    if (!eth_failed && !rtc_failed) return;

    const char *msg = (eth_failed && rtc_failed) ? "W5500 (LAN) + DS3231 (RTC) init failed"
                    : eth_failed                 ? "W5500 (LAN) init failed"
                    :                              "DS3231 (RTC) failed: NTP time only";

    // Red line between the top status labels and the clock.
    lv_obj_t *lbl = lv_label_create(s_main_scr);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_palette_main(LV_PALETTE_RED), 0);
    lv_label_set_text(lbl, msg);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 26);
}

void ui_set_last_sync(const char *ntp_time, const char *can_time)
{
    if (!s_last_sync_label) return;
    char buf[48];
    snprintf(buf, sizeof(buf), "Last sync: NTP: %s CAN: %s", ntp_time, can_time);
    lv_label_set_text(s_last_sync_label, buf);
}

static void connection_label_refresh(void)
{
    if (!s_connected_label) return;
    char buf[48];
    switch (s_connection_state) {
    case UI_CONN_LAN:
        snprintf(buf, sizeof(buf), s_ntp_error ? "LAN | NTP ERR" : "Connected on LAN");
        break;
    case UI_CONN_WIFI:
        snprintf(buf, sizeof(buf), s_ntp_error ? "WiFi | NTP ERR" : "Connected on WiFi");
        break;
    default:
        snprintf(buf, sizeof(buf), "Disconnected");
        break;
    }
    lv_label_set_text(s_connected_label, buf);
}

void ui_set_connection_state(ui_connection_state_t state)
{
    s_connection_state = state;
    connection_label_refresh();
}

void ui_set_ntp_error(bool error)
{
    if (s_ntp_error == error) return;
    s_ntp_error = error;
    connection_label_refresh();
}
