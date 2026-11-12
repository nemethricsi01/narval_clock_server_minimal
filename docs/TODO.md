# To-do

Revised 2026-11-12. Every bug from the 2026-11-11 code review is fixed on branch `fix/static-ip-and-can`
(`git log main..fix/static-ip-and-can` has the details), as is the CAN merge from `narval_videomtx`. So are
two later ones: a failed settings save now shows a red "Settings NOT saved" notice, and the IP is stored as one
32-bit value so other tasks can't read a half-written address.
Function names are given instead of line numbers, which go stale.

## Test on hardware, then merge

Flash over the CP2102 port (COM60) and check:

- [x] **No W5500:** boot with it unplugged. Red "W5500 (LAN) init failed" on the main screen, menus work.
  Tested OK 2026-11-12.
- [x] **No DS3231:** boot with it unplugged. Red "DS3231 (RTC) failed: NTP time only"; the time appears a few
  seconds after the network gets an IP; NTP Update Rate shows "5 min" and "Fixed: no RTC". Tested OK 2026-11-12.
- [ ] **No SQW:** DS3231 connected, SQW wire off. Log says "no 1 Hz SQW from the DS3231 — switching to the
  internal timer", and the clock keeps running.
- [ ] **Oscillator stop:** DS3231 backup battery out, power cycle. Log says "oscillator had stopped, time is
  stale"; dashes until NTP syncs; after the next reboot the RTC time is used straight away.
- [ ] **Static IP:** cable unplugged, set a static IP, Apply. Shows "IP: --" and "Disconnected"; plugging the
  cable in switches to the IP and "Connected on LAN"; changing to another static IP works without a reboot.
- [ ] **NTP on a new IP:** unplug and replug the cable. Log says "new IP … — querying NTP now", and the NTP time
  in the last-sync line updates within a few seconds.
- [ ] **CAN rate "Never":** no TX TIME frames in the CAN Monitor, last-sync line shows "CAN: off"; still set after
  a reboot.
- [ ] **USB bridge:** on the native USB port, the PC receives bus frames and the clock's date/time frames, and
  frames sent from the PC appear as TX in the CAN Monitor. Check the board doesn't reset when the PC tool opens
  or closes the port (the native USB port can reset the chip through RTS/DTR).
- [ ] **Bus-off recovery:** briefly short CAN-H to CAN-L. Log shows bus-off, then recovery, and TX resumes.
- [ ] **Encoder:** spin the knob fast in the CAN Monitor while frames arrive; no skipped steps, newest lines
  visible. Knob turns during boot are ignored.
- [ ] **Boot screen:** dashes for time, date and IP until real values arrive.
- [ ] **Find devices:** Menu > Find devices lists the PIC32 boards with Announce on (IP, name, MAC), the same
  ones `find_pic32.py` finds; a board that is power-cycled while the screen is open appears too. Click clears the
  list and searches again; long-press goes back. Without a cable it says "No network".
- [ ] **Announce:** `find_pic32.py` on the PC lists `NARV_CLK_SERV` with this device's IP and MAC. With
  `find_pic32.py -t 60` running, unplugging and replugging the cable makes it appear with "DHCP/Power event
  occurred".

Then merge `fix/static-ip-and-can` into `main`.

## Open issues

- [ ] **Every unit has the same MAC** — `board.h` `BOARD_ETH_MAC` is hard-coded (02:AB:CD:12:34:56) because the
  W5500 modules have no factory MAC. Two clock servers on one network would clash, and both announce the same
  MAC. Fix: in `eth_w5500_init()`, use `esp_read_mac(mac, ESP_MAC_ETH)`, which derives a unique Ethernet MAC from
  the ESP32-S3's factory eFuse MAC.

## Comments to correct

- [ ] `twai_can.h` (top comment): says "listen-only mode"; the node transmits, with the self-test flag on.
- [ ] `twai_can.c` (self-test comment): says looped-back frames are filtered in `on_rx_done()`. No such filter
  exists, and self-test doesn't loop frames back (that's `enable_loopback`).
- [ ] `lcd_st7789.h`: says init turns the backlight on; `display.c` does that.
- [ ] `clock.c` `send_can_datetime()`: says the TWAI driver keeps a pointer to `data[]`; `can_service_send()`
  copies into its TX pool, so the frames needn't be `static`.

## To verify

- [ ] `can_monitor.c`, case `0x13`: label "L37-49" breaks the 12-LEDs-per-layer pattern of 0x10–0x1F
  (L1-12, L13-24, L25-36, …). Check against the Narval protocol spec. Same in `narval_videomtx`.

## Dead code

| Location | What | Safe to delete? |
|---|---|---|
| `ui.h`, `ui.c` | `ui_net_config_t`, `ui_get_net_config()`, `s_net_applied` (no callers) | yes |
| `encoder.h`, `encoder.c` | `encoder_get_count()`; `s_knob` can then become local | yes |
| `can.h`, `can.c` | `can_service_get_queue()` (no callers) | yes |
| `can_monitor.h`, `can_monitor.c` | `can_mon_push_separator()` (only the `_from_ui` variant is used) | yes |
| `ui.h`, `ui.c` `connection_label_refresh()` | `UI_CONN_WIFI` | yes |
| `ui.c` `on_menu_select()` | unreachable `default:` branch; with it, `s_status_label` never changes from "Select an item" | maybe |
| `ui.h` | `ui_open_menu`/`ui_show_menu`/`ui_show_main` exported but only used inside `ui.c` (make static) | yes |
| `main.c` | `disp` out-parameter of `display_init()` never used (pass `NULL`) | yes |
| `Kconfig.projbuild` | empty "Example Configuration" menu | yes |
| `docs/canmon.c` | old video-matrix UI, not compiled; was the reference for the CAN monitor port | yes, the merge is done |

## Config cleanup

- [ ] Montserrat 10/16/26/32/38 are enabled in `sdkconfig` but only 14/20/46 are used. Disable them in
  menuconfig to save flash.
- [ ] `CONFIG_LV_USE_OBSERVER=y` (`sdkconfig`, `sdkconfig.defaults`): unused (no `lv_subject`/`lv_observer` in `main/`).
- [ ] `sdkconfig.defaults`: `LV_USE_SYSMON=y` and `LV_USE_PERF_MONITOR=y` are stale (off in `sdkconfig`).
  Regenerating `sdkconfig` from defaults would put a performance overlay on the screen. Remove them.

## Code-size cleanup (optional)

About 460 lines (~11% of the ~4,050 lines in `main/`) could go without hurting readability, mostly in `ui.c`
(about 1,470 → 1,000 lines). Biggest first:

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
  `picker_t {int *val; int min, max, step; void (*refresh)(void);}`. Keep the "Fixed: no RTC" lock on the NTP rate.
- [ ] **Use `settings_t` as the UI state** (~60): `static settings_t s_cfg, s_saved;` replaces 12 state
  variables and 12 `*_saved` copies; `save_settings()` becomes `settings_save(&s_cfg)`, `ui_init()` copies one struct.
- [ ] **CAN decoder tables** (~40): `can_monitor.c` LED-range table for 0x10–0x1F/0x30–0x3F and name tables.
- [ ] **`clock.c` helpers** (~15): `fmt_local_hms()` for the three HH:MM:SS blocks in `clock_task`, a
  `send_frame()` helper in `send_can_datetime()`, and an expression instead of the `effective_dst_hours()` switch.
- [ ] **IP helpers** (~15): one `fmt_ip()` for the "IP:x.x.x.x" strings (`on_got_ip`, `show_network_status`,
  `network_screen_refresh`), and an `ip_field_ptr()` for the three `s_ip_field` lookups
  (`ip_edit_refresh`, `ip_octet_step`, `ui_encoder_event`).
- [ ] **Delete the dead code above** (~75).
