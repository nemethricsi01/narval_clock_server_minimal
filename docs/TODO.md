# To-do: known fixes not done yet

Collected 2026-11-11 from a code-health review of `main/` and a comparison with `narval_videomtx`.
Line numbers are as of that date. `ui.c` lines may shift slightly; function names are given where that matters.

Already done or handled separately (not listed below): the static-IP Apply deadlock and the boot hang
with a saved static IP (fixed), and the CAN merge from `narval_videomtx` (bit timing, bus-off recovery,
stale TX slot reclaim, bus error/state logging, CAN monitor 0x30–0x3F decoder and auto-scroll).
Items marked *after CAN merge* touch files that merge changes, so recheck them first.

## Bugs and risks (most severe first)

- [x] **Missing W5500 or DS3231 causes a boot loop** — fixed 2026-11-11: init errors are returned and
  logged, the device keeps running, and the main screen shows a red "... init failed" line. Without the
  DS3231 the clock itself still doesn't run (its 1 Hz SQW drives `clock_task`); see the next item.
- [x] **No clock without the DS3231** — fixed 2026-11-12: without the RTC, `clock_task` ticks from an
  `esp_timer` aligned to system-time seconds and uses system time (set by NTP); NTP is fixed at 5 min and the
  NTP Update Rate screen shows "Fixed: no RTC". The timer also takes over if the RTC answers but its SQW
  never arrives. Until the first NTP sync there is no time, so nothing is shown or sent on CAN.
- [x] **Encoder callbacks block the esp_timer task** — fixed 2026-11-12: `on_encoder_event()` only does a
  non-blocking `xQueueSend()`; an `lv_timer` (10 ms) in the LVGL task drains the queue into `ui_encoder_event()`.
- [x] **CAN Monitor timestamps ignore Auto DST** — fixed 2026-11-12: `can_mon_snapshot()` takes the timezone
  and DST mode and converts each entry with `clock_utc_to_local()` (exported from `clock.c`).
- [x] **Placeholder text looks like real data** — fixed 2026-11-12: the main screen starts with "--:--:--",
  "----/--/--" and "IP: --" until real values arrive.
- [x] **Encoder events can arrive before `lv_init()`** — fixed 2026-11-12 by the item above: the callback no
  longer touches LVGL, and knob events queued during boot are discarded once the UI is up.
- [x] **RTC validity ignores the oscillator-stop flag** — fixed 2026-11-12: `rtc_init()` doesn't seed from the
  RTC while the flag is set (or can't be read); `on_ntp_sync()` clears it after setting the RTC.
- [x] **Static IP shown as connected with the cable unplugged** — fixed 2026-11-11: `network_apply_ip()` no
  longer sets `s_connected`; only `on_got_ip()` does, and ESP-IDF posts that for a static IP only while the
  link is up. Also fixed: changing one static IP to another failed silently until reboot (wrong
  "already stopped" error code).
- [ ] **Settings fallbacks differ from `SETTINGS_DEFAULT`** — `settings.c:36` (`tz_idx` → 12, default 13).
  Fix: fall back to the `SETTINGS_DEFAULT` field. (`rate_idx` done 2026-11-11.)
- [ ] **`nvs_set_*` results ignored** — `settings.c:53-64`. A failed write (e.g. NVS full) is lost silently;
  only the commit is checked. Fix: keep the first error and return it.
- [x] **NTP server string rewritten while SNTP uses it** — fixed 2026-11-12: `use_ntp_server()` alternates
  between two buffers, so the one SNTP points at is never rewritten.
- [ ] **RTC aging offset not retried after an I2C failure** — `clock.c:211-216`. `rtc_trim_synced` and
  `last_rtc_trim` are updated before the write, so a failed write is never retried. Fix: update them only on `ESP_OK`.

## Comments to correct

- [ ] `twai_can.h` (top comment): says "listen-only mode"; the node transmits with the self-test flag on. *After CAN merge.*
- [ ] `twai_can.c` (self-test comment): says looped-back frames are filtered in `on_rx_done()`. No such filter
  exists, and self-test doesn't loop frames back (that's `enable_loopback`). *After CAN merge.*
- [x] `clock.h:9-11`: described waiting for the Network Apply button. Rewritten 2026-11-12.
- [ ] `lcd_st7789.h:9`: says init turns the backlight on; `lcd_st7789.c` has no backlight code (`display.c` does it).
- [ ] `clock.c:98-99`: says the TWAI driver keeps a pointer to `data[]`; `can_service_send()` copies into its
  TX pool, so the `static` frames in `send_can_datetime()` aren't needed.

## To verify

- [ ] `can_monitor.c`, case `0x13`: label "L37-49" breaks the 12-LEDs-per-layer pattern of 0x10–0x1F
  (L1-12, L13-24, L25-36, …). Check against the Narval protocol spec. Same in `narval_videomtx`. *Unconfirmed.*

## Dead code

| Location | What | Safe to delete? |
|---|---|---|
| `ui.h:69-86`, `ui.c:131`, `ui.c:489` (`on_net_apply`), `ui.c:1388-1397` | `ui_net_config_t`, `ui_get_net_config()`, `s_net_applied` (no callers) | yes |
| `encoder.c:103-106`, `encoder.h:26` | `encoder_get_count()`; `s_knob` can then become local | yes |
| `can.h:28` + `can.c` | `can_service_get_queue()` (no callers) | yes, *after CAN merge* |
| `can_monitor.h:22` + `can_monitor.c` | `can_mon_push_separator()` (only the `_from_ui` variant is used) | yes, *after CAN merge* |
| `ui.h:41`, `ui.c:1420` (`connection_label_refresh`) | `UI_CONN_WIFI` | yes |
| `ui.c:531-535` (`on_menu_select`) | unreachable `default:` branch; with it, `s_status_label` (`ui.c:56,595-600`) never changes from "Select an item" | maybe |
| `ui.h:16-23` | `ui_open_menu`/`ui_show_menu`/`ui_show_main` exported but only used inside `ui.c` (make static) | yes |
| `main.c:82-83` | `disp` out-parameter of `display_init()` never used (pass `NULL`) | yes |
| `Kconfig.projbuild` | empty "Example Configuration" menu | yes |
| `docs/canmon.c` | old video-matrix UI, not compiled; reference for the CAN monitor port | yes, once the merge is done |

## Config cleanup

- [ ] `sdkconfig:3205,3210,3218,3223,3228`: Montserrat 10/16/26/32/38 are enabled but only 14/20/46 are used.
  Disable them in menuconfig to save flash.
- [ ] `CONFIG_LV_USE_OBSERVER=y` (`sdkconfig:3523`, `sdkconfig.defaults`): unused (no `lv_subject`/`lv_observer` in `main/`).
- [ ] `sdkconfig.defaults`: `LV_USE_SYSMON=y` and `LV_USE_PERF_MONITOR=y` are stale (off in `sdkconfig`).
  Regenerating `sdkconfig` from defaults would put a performance overlay on the screen. Remove them.

## Code-size cleanup (optional)

About 470 lines (~14% of ~3,400 compiled lines) could go without hurting readability, mostly in `ui.c`
(about 1,440 → 970 lines). Biggest first:

- [ ] **Label/roller builder helpers** (~100): `make_label(...)` for the ~25 four-line label blocks in the
  `build_*_screen()` functions, and one `build_roller_screen(...)` for the four near-identical roller screens
  (`build_update_rate_screen`, `build_ntp_update_rate_screen`, `build_dst_screen`, `build_ntp_screen`).
- [ ] **List-button helper plus item tables** (~90): `build_menu_screen`, `build_settings_screen`,
  `build_network_screen` loop over a `{icon, text, open_fn}` table; removes the `on_settings_select` if-ladder
  and the `on_menu_select` switch.
- [ ] **One `show_screen()` helper** (~75): the 15 `ui_open_*`/`ui_show_*` transitions repeat the same four
  lines. Keep the `lv_indev_set_group()` call: focus events carry the group's indev, which drives the focus highlight.
- [ ] **Generic value picker** (~65): `brightness_step`, `rtc_trim_step`, `update_rate_step`,
  `ntp_update_rate_step`, `dst_step`, `tz_step` and their `ui_encoder_event()` cases become one
  `picker_t {int *val; int min, max, step; void (*refresh)(void);}`.
- [ ] **Use `settings_t` as the UI state** (~60): `static settings_t s_cfg, s_saved;` replaces 12 state
  variables and 12 `*_saved` copies; `save_settings()` becomes `settings_save(&s_cfg)`, `ui_init()` copies one struct.
- [ ] **CAN decoder tables** (~40): `can_monitor.c` LED-range table for 0x10–0x1F/0x30–0x3F and name tables. *After CAN merge.*
- [ ] **`clock.c` helpers** (~20): `fmt_local_hms()` for the three HH:MM:SS blocks in `clock_task` (`clock.c:246-270`),
  a `send_frame()` helper in `send_can_datetime()`, `strlcpy` for the `strncpy` pairs, and an expression
  instead of the `effective_dst_hours()` switch.
- [ ] **IP helpers** (~15): one `fmt_ip()` for the "IP:x.x.x.x" strings (`on_got_ip`, `show_network_status`,
  `network_screen_refresh`), and an `ip_field_ptr()` for the three `s_ip_field` lookups
  (`ip_edit_refresh`, `ip_octet_step`, `ui_encoder_event`).
- [ ] **Delete the dead code above** (~75).
