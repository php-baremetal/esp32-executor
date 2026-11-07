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

/*
 * The single core-0 executor task: device pollers (sample into a ring) and handed-over work (a CORE1 bus
 * runs its transactions here as sole owner). Created lazily; a firmware that uses neither pays nothing.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct executor_poller executor_poller_t;

/* Register (or update) a poller for a device. The executor calls sample(dev, out) at `hz` on core 0
 * and stores the raw sample (sample_size bytes) into a ring of `depth` slots. sample() must do its own
 * bus locking; it runs on core 0. Idempotent per (sample, dev): a second call updates the rate.
 * Returns the poller handle, or NULL on failure. */
executor_poller_t *executor_poll(esp_err_t (*sample)(void *dev, void *out),
                                 void *dev, size_t sample_size, uint32_t hz, size_t depth);

/* Pause (true) or resume (false) a poller. Paused, it takes no samples and does not count in the executor's wait; ring and unread samples stay. On resume the next sample is due now. */
void executor_poll_pause(executor_poller_t *p, bool paused);

/* Install a notify called on core 0 right after each new sample is stored (outside the ring lock).
 * It must not block or touch PHP -- a source uses it to post a cross-core message (a queue send). NULL
 * clears it. arg is passed through. On return no call of the previous notify is running or can start. */
void executor_poll_on_sample(executor_poller_t *p, void (*notify)(void *arg), void *arg);

/* Copy the most recent sample into out (sample_size bytes). Returns false if none has been taken yet. */
bool executor_poll_latest(executor_poller_t *p, void *out);

/* Copy up to max_samples unread samples into out (contiguous, sample_size bytes each), oldest first,
 * and mark them read. Returns how many were copied. */
size_t executor_poll_drain(executor_poller_t *p, void *out, size_t max_samples);

typedef struct executor_irq executor_irq_t;

/* A job run on the executor task only when asked; dev is passed to service. NULL without memory (or when all
 * slots are taken). Starts the executor lazily. */
executor_irq_t *executor_irq(esp_err_t (*service)(void *dev), void *dev);
/* From an ISR: run the job once as soon as the executor can; a second kick before it runs is merged. */
void executor_irq_kick_from_isr(executor_irq_t *j);
/* From a task: run the job once after ms (replaces a pending delayed run). */
void executor_irq_after(executor_irq_t *j, uint32_t ms);
/* paused: new kicks are dropped and so are a pending kick and a pending delayed run, also those racing the call;
 * a run already taken completes. Resume only un-pauses; on a job that is not paused it changes nothing. */
void executor_irq_pause(executor_irq_t *j, bool paused);
/* No more runs, and the job is freed. Blocks until the executor task is between services (inline when
 * called from a service, even of this job), so on return service(dev) is not running and never starts:
 * the caller may free dev. The caller removes the interrupt handler that kicks the job BEFORE cancel
 * (and waits out an ISR still running on another core): a kick after cancel touches freed memory. Pause
 * is weaker: a run already taken may still execute once after it returns. */
void executor_irq_cancel(executor_irq_t *j);

/* Run fn(arg) on the executor task and block until done, returning its result; run inline if already on
 * the task. Starts the executor lazily. A full queue for enqueue_timeout_ms returns ESP_ERR_TIMEOUT. */
esp_err_t executor_run_sync(esp_err_t (*fn)(void *arg), void *arg, uint32_t enqueue_timeout_ms);

/* True when the caller runs on the executor task. */
bool executor_on_task(void);
