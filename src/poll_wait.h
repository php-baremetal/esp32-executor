/*
   +----------------------------------------------------------------------+
   | php-baremetal: esp32-executor                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 2026 Gianfrancesco Aurecchia                           |
   +----------------------------------------------------------------------+
   | This source file is subject to the MIT license, that is bundled with |
   | this package in the file LICENSE.                                    |
   +----------------------------------------------------------------------+
   | Authors: Gianfrancesco Aurecchia <gianfri@php-baremetal.com>         |
   +----------------------------------------------------------------------+
*/

/* How long the executor may block before the next sampler is due. Pure, so it is tested on the PC. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define POLL_WAIT_FOREVER UINT32_MAX

/* Milliseconds from `now` to the earliest of the `n` due times (all in ms, wrapping like now_ms()): 0 when
 * one is already due, POLL_WAIT_FOREVER when n is 0. */
static inline uint32_t poll_wait_ms(uint32_t now, const uint32_t *due, size_t n)
{
    uint32_t wait = POLL_WAIT_FOREVER;
    for (size_t i = 0; i < n; i++) {
        int32_t left = (int32_t) (due[i] - now);
        uint32_t w = left > 0 ? (uint32_t) left : 0;
        if (w < wait) {
            wait = w;
        }
    }
    return wait;
}

/* `ms` (not POLL_WAIT_FOREVER) as a queue timeout in ticks of a `tick_hz` clock: rounded up, plus one tick.
 * A timeout of N ticks can end just after the (N-1)th tick and now_ms() truncates, so without the extra tick
 * the task often wakes just before the sampler is due and has to sleep again. */
static inline uint32_t poll_wait_ticks(uint32_t ms, uint32_t tick_hz)
{
    return (uint32_t) (((uint64_t) ms * tick_hz + 999) / 1000) + 1;
}

/* The due time after one taken at `now`: one period after the previous one, so the rate does not drift with
 * the wake-up latency, or one period after `now` when the sampler has fallen a whole period behind. */
static inline uint32_t poll_next_due(uint32_t due, uint32_t now, uint32_t period_ms)
{
    uint32_t next = due + period_ms;
    return (int32_t) (now - next) >= 0 ? now + period_ms : next;
}
