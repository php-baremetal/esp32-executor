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

/* The state of one on-demand executor job (executor_irq()). Pure, so it is tested on the PC.
 * irq_kick() only touches atomics, so it is safe from an interrupt handler. irq_after() and irq_take()
 * touch due_us (64 bits), so a caller that uses them from several tasks serializes them (the executor
 * does, under its spinlock). All times are microseconds. */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define IRQ_NO_DUE UINT64_MAX

typedef struct {
    atomic_bool pending;     /* a run was asked for; a second request before it runs is merged */
    atomic_bool paused;      /* requests are dropped */
    atomic_bool cancelled;   /* no more runs */
    uint64_t    due_us;      /* delayed run, or IRQ_NO_DUE */
} irq_state_t;

static inline void irq_init(irq_state_t *s)
{
    atomic_init(&s->pending, false);
    atomic_init(&s->paused, false);
    atomic_init(&s->cancelled, false);
    s->due_us = IRQ_NO_DUE;
}

/* Ask for one run as soon as possible. Interrupt-safe. */
static inline __attribute__((always_inline)) void irq_kick(irq_state_t *s)
{
    if (!atomic_load(&s->paused) && !atomic_load(&s->cancelled)) {
        atomic_store(&s->pending, true);
    }
}

/* Ask for one run `ms` after `now_us`, replacing a pending delayed run. */
static inline void irq_after(irq_state_t *s, uint64_t now_us, uint32_t ms)
{
    if (!atomic_load(&s->paused) && !atomic_load(&s->cancelled)) {
        s->due_us = now_us + (uint64_t) ms * 1000u;
    }
}

static inline void irq_pause(irq_state_t *s, bool paused)
{
    if (paused) {
        atomic_store(&s->paused, true);
        atomic_store(&s->pending, false);
        s->due_us = IRQ_NO_DUE;
    } else if (atomic_load(&s->paused)) {
        /* A kick that raced the pause (it read paused==false before the pause and stored pending after it)
         * is cleared on resume, before requests are accepted again. */
        atomic_store(&s->pending, false);
        s->due_us = IRQ_NO_DUE;
        atomic_store(&s->paused, false);
    }
    /* Resume on a job that is not paused: nothing changes, a pending run or delayed run survives. */
}

static inline void irq_cancel(irq_state_t *s)
{
    atomic_store(&s->cancelled, true);
    atomic_store(&s->pending, false);
    s->due_us = IRQ_NO_DUE;
}

/* True once when a run is owed at `now_us`: an immediate request, or a delayed one that has come due.
 * Taking clears both, so a request and a delay together run once. */
static inline bool irq_take(irq_state_t *s, uint64_t now_us)
{
    if (atomic_load(&s->cancelled) || atomic_load(&s->paused)) {
        return false;
    }
    bool run = atomic_exchange(&s->pending, false);
    if (!run && s->due_us != IRQ_NO_DUE && now_us >= s->due_us) {
        run = true;
    }
    if (run) {
        s->due_us = IRQ_NO_DUE;
    }
    return run;
}

/* When the next run is owed: 0 when one is pending now, IRQ_NO_DUE when nothing is waiting. */
static inline uint64_t irq_next_due(const irq_state_t *s)
{
    if (atomic_load(&s->cancelled) || atomic_load(&s->paused)) {
        return IRQ_NO_DUE;
    }
    return atomic_load(&s->pending) ? 0 : s->due_us;
}
