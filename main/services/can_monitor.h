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
 * Register the callback that fires (via lv_async_call) whenever new data arrives.
 * The callback runs in the LVGL task — LVGL APIs may be called freely inside it.
 */
void can_mon_set_notify(void (*fn)(void));

/**
 * Render the full log (oldest first) into out[0..out_len-1] as a null-terminated string.
 * local_offset_hours: TZ + DST hours to add to stored UTC timestamps (computed by caller).
 * Returns the number of bytes written (excluding the null terminator). Thread-safe.
 */
int can_mon_snapshot(char *out, size_t out_len, int local_offset_hours);

