# Changelog

## 1.0.2

- Fixed: while a sampler was registered the executor woke every FreeRTOS tick (1 ms), which kept the chip
  from light-sleeping with the `power-save` option on. It now blocks until the earliest sampler is due. The
  rate is kept from one due time to the next, so a late wake-up no longer stretches the period.
- Fixed: the package description said core 1; the executor runs on core 0.

## 1.0.1

- Changed: the author's contact email.

## 1.0.0

Initial release.
