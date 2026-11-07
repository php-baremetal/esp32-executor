# Changelog

## 1.1.1

- Added: executor_irq(), a job run on the executor task only when asked (executor_irq_kick_from_isr() from an interrupt, executor_irq_after() for a delayed run), with pause and cancel (cancel waits out a running service, so the caller may free its dev afterwards). The interrupt-side kick builds its queue item field by field, so it calls no memset from interrupt context.

## 1.1.0

- Added: executor_poll_pause(), which pauses a poller (no samples, no wake-ups) and resumes it.

## 1.0.2

- Fixed: while a sampler was registered the executor woke every FreeRTOS tick (1 ms), which kept the chip
  from light-sleeping with the `power-save` option on. It now blocks until the earliest sampler is due. The
  rate is kept from one due time to the next, so a late wake-up no longer stretches the period.
- Fixed: the package description said core 1; the executor runs on core 0.

## 1.0.1

- Changed: the author's contact email.

## 1.0.0

Initial release.
