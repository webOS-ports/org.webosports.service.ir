# org.webosports.service.ir

LuneOS service for the infrared transmitter (IR blaster). `irblasterd` puts it
on the bus with the same model as Android's ConsumerIrManager: a carrier
frequency plus alternating mark/space durations in microseconds. Protocol
encoding (NEC, RC5, ...) is the caller's job.

Backends, picked at start-up:

- `sec_ir` - Samsung's iCE40 FPGA driver (`/sys/class/sec/sec_ir`), e.g. the
  Galaxy Tab Pro 10.1 (SM-T520) and other Exynos 5420 tablets
- `lirc` - mainline `/dev/lirc0` (gpio-ir-tx, ir-spi, pwm-ir-tx, USB)

## API

`luna://org.webosports.service.ir/getStatus` (subscribable)

    {"returnValue": true, "available": true, "backend": "sec_ir", "maxDurations": 1000}

`luna://org.webosports.service.ir/transmit`

    luna-send -n 1 luna://org.webosports.service.ir/transmit \
        '{"frequency": 38000, "pattern": [9000, 4500, 560, 560, ...]}'

    {"returnValue": true, "acknowledged": true}

The pattern starts with a mark. The reply comes once the burst has been sent;
transmissions are queued (up to 8 behind the one being sent, then "busy") and
sent one at a time. `acknowledged` says whether the hardware confirmed the
burst: lirc always does, the sec_ir FPGA does for real remote codes but not
for some very short synthetic bursts it nonetheless sends.

Limits, enforced for every backend: carrier 15-500 kHz, 1-1000 durations,
each at most 65535 carrier cycles (as the sec_ir driver counts them), a whole
burst at most 5 seconds and, on sec_ir, at most a page of text.

Both methods are in the `ir.operation` group.

## Checks

- `tests/static-analysis.sh`: gcc -fanalyzer (-O0 and -O2), the clang static
  analyzer with its alpha unix/security checkers, clang-tidy, cppcheck,
  flawfinder and sparse. Any finding fails.
- `tests/harness/run.sh`: builds the real `src/main.c` against stand-ins for
  luna-service2 and pbnjson and runs it under ASan+UBSan (gcc and clang), TSan
  and valgrind memcheck - malformed input, the limits, write faults, a flood,
  20000 fuzzed requests, and shutdown with work queued.
