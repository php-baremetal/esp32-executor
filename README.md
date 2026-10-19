# esp32-executor

The executor task of the ESP32 firmware: one FreeRTOS task, pinned to core 0 with its stack in internal RAM,
that runs work handed over by HALs and the background samplers of devices. It exists because the PHP task
(core 1) has its stack in PSRAM: on ESP32 a task with a PSRAM stack must not run code that touches the flash
or suspends the flash cache (NVS, Wi-Fi driver calls, PHY calibration, `esp_light_sleep_start()`), so HALs run
that code here and wait for the result.

| Key | Value |
| --- | --- |
| Type | `bm-lib` (support library, C only, no PHP API) |
| Header | `executor.h` |
| ESP-IDF components | `esp_timer` (plus FreeRTOS) |
| Used by | [esp32-wifi-hal](package:php-baremetal/esp32-wifi-hal), [esp32-i2c-hal](package:php-baremetal/esp32-i2c-hal), [esp32-store-hal](package:php-baremetal/esp32-store-hal), [esp32-sys-hal](package:php-baremetal/esp32-sys-hal) |

## Install

Projects do not require it: the HALs that use it require it, and conductor installs them. A package that needs
it adds it to its own `composer.json` and includes `executor.h`:

```json
{ "require": { "php-baremetal/esp32-executor": "^1.0" } }
```

## The task

| Property | Value |
| --- | --- |
| Name | `executor` |
| Core | 0 (core 1 belongs to the PHP task) |
| Stack | 8192 bytes, internal RAM |
| Priority | 4 |
| Work queue | 16 entries |
| Started | lazily, by the first `executor_run_sync()` or `executor_poll()`; a firmware that uses neither has no task |

The task blocks on its work queue. While at least one poller is registered it wakes every FreeRTOS tick
(1 ms) instead, to keep the sampling cadence; work and samplers run one after the other on the task, never
at the same time.

## Running work: executor_run_sync()

```c
esp_err_t executor_run_sync(esp_err_t (*fn)(void *arg), void *arg, uint32_t enqueue_timeout_ms);
bool executor_on_task(void);
```

Runs `fn(arg)` on the executor and blocks the caller until it returns, then returns its result. Called from
the executor itself, it runs `fn` inline. It returns `ESP_ERR_TIMEOUT` when the work queue stays full for
`enqueue_timeout_ms`, and `ESP_ERR_INVALID_STATE` when the task cannot be created; once queued, the wait for
completion has no limit. `fn` must not call PHP or the Zend allocator: only the PHP task may.

Users: `esp32-wifi-hal` (every `esp_wifi_*`, `esp_netif_*`, `nvs_flash_*` call, 1000 ms enqueue limit),
`esp32-store-hal` (NVS), `esp32-sys-hal` (`sys_light_sleep()`), `esp32-i2c-hal` (every transaction of a bus
owned by the executor).

## Background samplers: executor_poll()

```c
executor_poller_t *executor_poll(esp_err_t (*sample)(void *dev, void *out),
                                 void *dev, size_t sample_size, uint32_t hz, size_t depth);
void   executor_poll_on_sample(executor_poller_t *p, void (*notify)(void *arg), void *arg);
bool   executor_poll_latest(executor_poller_t *p, void *out);
size_t executor_poll_drain(executor_poller_t *p, void *out, size_t max_samples);
```

| Function | Does |
| --- | --- |
| `executor_poll()` | Registers a sampler: the executor calls `sample(dev, out)` every `1000 / hz` ms (whole milliseconds, at least 1) and stores each successful sample in a ring of `depth` slots. A second call with the same `sample` and `dev` only changes the rate. Returns NULL for a size of 0 or over 64 bytes, `hz` 0, no free slot, or no memory. |
| `executor_poll_on_sample()` | Installs a callback run on the executor right after each stored sample, outside the ring lock. It must not block or touch PHP: post to a queue (ext-i2c calls `bm_events_emit()`). On return, no call of the previous callback is running. |
| `executor_poll_latest()` | Copies the most recent sample; false before the first one. |
| `executor_poll_drain()` | Copies up to `max_samples` unread samples, oldest first, and marks them read. Samples overwritten by a full ring are lost. |

`sample()` runs on core 0 and does its own bus locking. A sample that returns an error is not stored.

| Limit | Value |
| --- | --- |
| Samplers in the firmware | 8 |
| Sample size | 1 to 64 bytes |
| Ring depth | 1 to 256 samples (0 means 64; larger is clamped) |

Samplers are never removed: there is no call that stops one, and the task keeps waking every tick while any
exists, which also keeps the chip from light-sleeping for longer than a tick when the `power-save` option is
on.

## License

MIT.
