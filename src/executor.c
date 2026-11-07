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

#include "executor.h"
#include "poll_wait.h"
#include "irq_queue.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <string.h>
#include <stdlib.h>

/* Core 1 is the PHP task's. */
#define EXECUTOR_CORE   0
#define EXECUTOR_STACK  8192
#define EXECUTOR_PRIO   4
#define MAX_POLLERS     8
#define MAX_SAMPLE      64
#define MAX_DEPTH       256
#define INTENT_QUEUE_DEPTH  16
#define MAX_IRQ_JOBS    4

/* A unit of work for the executor. fn==NULL is a wake no-op; done is given after fn runs; result gets
 * fn's return. */
typedef struct {
    esp_err_t (*fn)(void *arg);
    void             *arg;
    SemaphoreHandle_t done;
    esp_err_t        *result;
} executor_intent_t;

struct executor_poller {
    bool      active;
    volatile bool paused;    /* no samples, not counted in the wait (executor_poll_pause); set under s_mux */
    esp_err_t (*sample)(void *dev, void *out);
    void     *dev;
    size_t    sample_size;
    uint32_t  period_ms;
    uint32_t  next_due;      /* ms */
    uint8_t  *ring;
    size_t    depth;
    uint32_t  write_seq;     /* total samples written */
    uint32_t  read_seq;      /* total consumed by drain */
    void     (*notify)(void *arg);   /* called after each new sample (core 0), or NULL */
    void     *notify_arg;
    uint32_t  notify_started;        /* notifies begun (callback read under s_mux) */
    volatile uint32_t notify_done;   /* notifies finished; runs are serial on the executor task */
};

/* An on-demand job (executor_irq): runs on the executor task only when kicked or due. Cancel runs on the
 * executor task too (serial with the services), so it never overlaps a run and frees the job itself. */
struct executor_irq {
    irq_state_t st;
    esp_err_t (*service)(void *dev);
    void     *dev;
};

static executor_poller_t s_pollers[MAX_POLLERS];
/* Job slots: filled under s_mux by executor_irq(); a slot is emptied only by the executor task. */
static executor_irq_t *volatile s_irq[MAX_IRQ_JOBS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_intent_q;
static TaskHandle_t  s_task;

static uint32_t now_ms(void)
{
    return (uint32_t) (esp_timer_get_time() / 1000);
}

/* Run each job that is owed one. Only the executor task runs services and empties slots (cancel runs
 * there too), so a job is never freed while its service runs. A service may cancel its own job: nothing
 * touches j after service() returns. */
static void service_irq_jobs(void)
{
    for (int i = 0; i < MAX_IRQ_JOBS; i++) {
        executor_irq_t *j = s_irq[i];
        if (!j) {
            continue;
        }
        portENTER_CRITICAL(&s_mux);
        bool run = irq_take(&j->st, (uint64_t) esp_timer_get_time());
        portEXIT_CRITICAL(&s_mux);
        if (run) {
            j->service(j->dev);                          /* sole owner of the bus -- no lock */
        }
    }
}

static void service_pollers(void)
{
    uint32_t now = now_ms();
    for (int i = 0; i < MAX_POLLERS; i++) {
        executor_poller_t *p = &s_pollers[i];
        if (!p->active || p->paused || (int32_t) (now - p->next_due) < 0) {
            continue;
        }
        uint8_t tmp[MAX_SAMPLE];
        if (p->sample(p->dev, tmp) == ESP_OK) {          /* bus read -- outside the spinlock */
            portENTER_CRITICAL(&s_mux);
            memcpy(p->ring + (p->write_seq % p->depth) * p->sample_size, tmp, p->sample_size);
            p->write_seq++;
            void (*notify)(void *arg) = p->notify;
            void *notify_arg = p->notify_arg;
            if (notify) {
                p->notify_started++;
            }
            portEXIT_CRITICAL(&s_mux);
            if (notify) {
                notify(notify_arg);                      /* post the cross-core event -- no lock held */
                portENTER_CRITICAL(&s_mux);
                p->notify_done++;
                portEXIT_CRITICAL(&s_mux);
            }
        }
        p->next_due = poll_next_due(p->next_due, now, p->period_ms);
    }
}

/* Block until the earliest sampler is due, or until work arrives when there is none: the chip can then
 * light-sleep between samples. A poller added meanwhile nudges the queue. */
static TickType_t next_wait(void)
{
    uint32_t due[MAX_POLLERS + MAX_IRQ_JOBS];
    size_t n = 0;
    for (int i = 0; i < MAX_POLLERS; i++) {
        if (s_pollers[i].active && !s_pollers[i].paused) {
            due[n++] = s_pollers[i].next_due;
        }
    }
    uint32_t now = now_ms();
    for (int i = 0; i < MAX_IRQ_JOBS; i++) {
        executor_irq_t *j = s_irq[i];
        if (!j) {
            continue;
        }
        portENTER_CRITICAL(&s_mux);
        uint64_t d = irq_next_due(&j->st);
        portEXIT_CRITICAL(&s_mux);
        if (d != IRQ_NO_DUE) {
            due[n++] = d == 0 ? now : (uint32_t) ((d + 999) / 1000);
        }
    }
    uint32_t ms = poll_wait_ms(now, due, n);
    if (ms == POLL_WAIT_FOREVER) {
        return portMAX_DELAY;
    }
    return (TickType_t) poll_wait_ticks(ms, configTICK_RATE_HZ);
}

static void executor_task(void *arg)
{
    (void) arg;
    for (;;) {
        TickType_t wait = next_wait();
        executor_intent_t it;
        if (xQueueReceive(s_intent_q, &it, wait) == pdTRUE && it.fn) {
            esp_err_t r = it.fn(it.arg);                 /* sole owner -- no lock */
            if (it.result) {
                *it.result = r;
            }
            if (it.done) {
                xSemaphoreGive(it.done);
            }
        }
        service_pollers();
        service_irq_jobs();
    }
}

bool executor_on_task(void)
{
    return s_task && xTaskGetCurrentTaskHandle() == s_task;
}

/* Create the queue and task on first use (callers run on the PHP task, so no concurrent re-entry). */
static bool executor_ensure_started(void)
{
    if (s_task) {
        return true;
    }
    if (!s_intent_q) {
        s_intent_q = xQueueCreate(INTENT_QUEUE_DEPTH, sizeof(executor_intent_t));
        if (!s_intent_q) {
            return false;
        }
    }
    /* Stack from internal RAM: flash operations (NVS, partitions) assert on a PSRAM-stack task. */
    if (xTaskCreatePinnedToCore(executor_task, "executor", EXECUTOR_STACK, NULL,
                                EXECUTOR_PRIO, &s_task, EXECUTOR_CORE) != pdPASS) {
        s_task = NULL;
        return false;
    }
    return true;
}

esp_err_t executor_run_sync(esp_err_t (*fn)(void *arg), void *arg, uint32_t enqueue_timeout_ms)
{
    if (executor_on_task()) {
        return fn ? fn(arg) : ESP_OK;                    /* already sole owner -- run inline */
    }
    if (!executor_ensure_started()) {
        return ESP_ERR_INVALID_STATE;
    }

    StaticSemaphore_t sbuf;
    SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&sbuf);
    esp_err_t result = ESP_FAIL;
    executor_intent_t it = { .fn = fn, .arg = arg, .done = done, .result = &result };

    if (xQueueSend(s_intent_q, &it, pdMS_TO_TICKS(enqueue_timeout_ms)) != pdTRUE) {
        vSemaphoreDelete(done);
        return ESP_ERR_TIMEOUT;                          /* full queue -- backpressure */
    }
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    return result;
}

executor_poller_t *executor_poll(esp_err_t (*sample)(void *dev, void *out),
                                 void *dev, size_t sample_size, uint32_t hz, size_t depth)
{
    if (!sample || sample_size == 0 || sample_size > MAX_SAMPLE || hz == 0) {
        return NULL;
    }
    if (depth == 0) {
        depth = 64;
    }
    if (depth > MAX_DEPTH) {
        depth = MAX_DEPTH;
    }
    uint32_t period = 1000u / hz;
    if (period == 0) {
        period = 1;
    }

    /* idempotent: same (sample, dev) updates the rate */
    for (int i = 0; i < MAX_POLLERS; i++) {
        executor_poller_t *p = &s_pollers[i];
        if (p->active && p->sample == sample && p->dev == dev) {
            p->period_ms = period;
            return p;
        }
    }

    executor_poller_t *slot = NULL;
    for (int i = 0; i < MAX_POLLERS; i++) {
        if (!s_pollers[i].active) {
            slot = &s_pollers[i];
            break;
        }
    }
    if (!slot) {
        return NULL;
    }
    uint8_t *ring = calloc(depth, sample_size);
    if (!ring) {
        return NULL;
    }
    slot->sample = sample;
    slot->dev = dev;
    slot->sample_size = sample_size;
    slot->period_ms = period;
    slot->next_due = now_ms();
    slot->ring = ring;
    slot->depth = depth;
    slot->write_seq = 0;
    slot->read_seq = 0;
    slot->notify = NULL;
    slot->notify_arg = NULL;
    slot->notify_started = 0;
    slot->notify_done = 0;
    slot->paused = false;
    portENTER_CRITICAL(&s_mux);
    slot->active = true;                 /* published last */
    portEXIT_CRITICAL(&s_mux);

    executor_ensure_started();
    if (s_intent_q) {
        executor_intent_t wake = { 0 };   /* nudge an idle loop onto the poller cadence */
        xQueueSend(s_intent_q, &wake, 0);
    }
    return slot;
}

void executor_poll_pause(executor_poller_t *p, bool paused)
{
    if (!p) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    if (!paused && p->paused) {
        p->next_due = now_ms();          /* the next sample is due now */
    }
    p->paused = paused;
    portEXIT_CRITICAL(&s_mux);
    if (!paused && s_intent_q) {
        executor_intent_t wake = { 0 };   /* the task may be blocked with no deadline: nudge it */
        xQueueSend(s_intent_q, &wake, 0);
    }
}

void executor_poll_on_sample(executor_poller_t *p, void (*notify)(void *arg), void *arg)
{
    portENTER_CRITICAL(&s_mux);
    p->notify_arg = arg;
    p->notify = notify;      /* published after its arg */
    uint32_t begun = p->notify_started;
    portEXIT_CRITICAL(&s_mux);
    /* Notifies begun before the swap may still run the previous callback: wait only for those (later
     * ones read the new callback), so on return nothing the old callback uses is read any more. The
     * executor task itself is never inside a notify here. */
    if (!executor_on_task()) {
        while ((int32_t) (p->notify_done - begun) < 0) {
            vTaskDelay(1);
        }
    }
}

bool executor_poll_latest(executor_poller_t *p, void *out)
{
    bool got = false;
    portENTER_CRITICAL(&s_mux);
    if (p->write_seq > 0) {
        uint32_t idx = (p->write_seq - 1) % p->depth;
        memcpy(out, p->ring + idx * p->sample_size, p->sample_size);
        got = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return got;
}

size_t executor_poll_drain(executor_poller_t *p, void *out, size_t max_samples)
{
    size_t n = 0;
    portENTER_CRITICAL(&s_mux);
    uint32_t oldest = (p->write_seq > p->depth) ? (p->write_seq - p->depth) : 0;
    if (p->read_seq < oldest) {
        p->read_seq = oldest;                /* overwritten samples are lost */
    }
    while (p->read_seq < p->write_seq && n < max_samples) {
        uint32_t idx = p->read_seq % p->depth;
        memcpy((uint8_t *) out + n * p->sample_size, p->ring + idx * p->sample_size, p->sample_size);
        p->read_seq++;
        n++;
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

/* Nudge the task out of its wait from a task. */
static void irq_wake(void)
{
    if (s_intent_q) {
        executor_intent_t wake = { 0 };
        xQueueSend(s_intent_q, &wake, 0);
    }
}

executor_irq_t *executor_irq(esp_err_t (*service)(void *dev), void *dev)
{
    if (!service || !executor_ensure_started()) {        /* start first: a placed job always has a task */
        return NULL;
    }
    executor_irq_t *j = calloc(1, sizeof(*j));
    if (!j) {
        return NULL;
    }
    irq_init(&j->st);
    j->service = service;
    j->dev = dev;
    bool placed = false;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_IRQ_JOBS; i++) {
        if (!s_irq[i]) {
            s_irq[i] = j;                                /* published last: fully built above */
            placed = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (!placed) {
        free(j);
        return NULL;
    }
    return j;
}

/* fn == NULL: a wake no-op. In DRAM and sent by address (the queue copies it), so the ISR builds nothing. */
static const DRAM_ATTR executor_intent_t k_wake_item = { 0 };

/* ISR-safe: atomics and the FromISR queue send only. The handler must be removed before executor_irq_cancel(),
 * which frees the job. */
void IRAM_ATTR executor_irq_kick_from_isr(executor_irq_t *j)
{
    if (!j) {
        return;
    }
    irq_kick(&j->st);
    if (s_intent_q) {
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(s_intent_q, &k_wake_item, &woken);    /* full: the task is busy and will see pending */
        if (woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

void executor_irq_after(executor_irq_t *j, uint32_t ms)
{
    if (!j) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    irq_after(&j->st, (uint64_t) esp_timer_get_time(), ms);
    portEXIT_CRITICAL(&s_mux);
    irq_wake();
}

void executor_irq_pause(executor_irq_t *j, bool paused)
{
    if (!j) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    irq_pause(&j->st, paused);
    portEXIT_CRITICAL(&s_mux);
}

/* Runs on the executor task: no service is running (or it is the caller itself, returning next). */
static esp_err_t irq_cancel_on_task(void *arg)
{
    executor_irq_t *j = arg;
    portENTER_CRITICAL(&s_mux);
    irq_cancel(&j->st);
    for (int i = 0; i < MAX_IRQ_JOBS; i++) {
        if (s_irq[i] == j) {
            s_irq[i] = NULL;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    free(j);
    return ESP_OK;
}

void executor_irq_cancel(executor_irq_t *j)
{
    if (!j) {
        return;
    }
    /* Serialized with the services on the executor task, inline when called from one (no deadlock). On
     * return service(dev) is not running and never runs again. A full queue only delays us: retry. */
    while (executor_run_sync(irq_cancel_on_task, j, 1000) == ESP_ERR_TIMEOUT) {
    }
}
