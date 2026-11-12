#pragma once

#include "can.h"
#include <stdbool.h>
#include <stddef.h>

// Maximum entries held in the ring buffer.
#define CAN_MON_LOG_SIZE  100
// Maximum characters per formatted line, including '\n' and the null terminator.
#define CAN_MON_LINE_MAX   96

/** Create the internal mutex. Must be called before any task that calls can_mon_push(). */
void can_mon_init(void);

/**
 * Push one frame into the ring buffer and schedule a UI refresh.
 * Safe to call from any FreeRTOS task or ISR-deferred context.
 */
void can_mon_push(const can_frame_t *f, bool is_tx);

/** Push a visual separator line ("---...---") into the ring buffer. */
void can_mon_push_separator(void);

/**
 * Push a separator from the LVGL task without triggering the poll flag.
 * Use when the caller will immediately call can_monitor_refresh() itself.
 */
void can_mon_push_separator_from_ui(void);

/**
 * Returns true (and clears the flag) if new data has been pushed since the
 * last call.  Intended for the LVGL poll timer — safe to call from the LVGL
 * task without holding display_lock separately.
 */
bool can_mon_has_new_data(void);

/**
 * Render the full log (oldest first) into out[0..out_len-1] as a null-terminated string.
 * Timestamps are shown in local time: tz_offset_hours and dst_mode as for
 * clock_utc_to_local(), applied to each entry's own date (so Auto DST is right).
 * Returns the number of bytes written (excluding the null terminator). Thread-safe.
 */
int can_mon_snapshot(char *out, size_t out_len, int tz_offset_hours, int dst_mode);

