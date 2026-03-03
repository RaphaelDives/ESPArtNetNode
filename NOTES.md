# ESPArtNetNode Resume Notes

This file is intended as a portable handoff for future work (different machine, date, or assistant).

## Project purpose

- ESP32 USB-NCM Art-Net receiver driving addressable LEDs.
- Art-Net UDP port: `6454`.
- Current app supports target profiles for **ESP32-S2** and **ESP32-S3**.

## Current behavior (baseline)

- Receives Art-Net DMX data and maps channels to RGB LED output.
- Startup self-test is enabled:
  - color cycle `red -> green -> blue -> white -> off`
  - then a walking single white pixel test
- Idle mode is implemented and activates after timeout when no mapped Art-Net frames are received.

## Known-good configuration (last validated)

- Hardware used: ESP32-S3-WROOM-1 N8R2.
- USB network mode: USB-NCM.
- Device IP on USB link: `192.168.4.1`.
- Typical host adapter IP: `192.168.4.2` (host-side may vary).
- LED setup:
  - model/timing: `WS2812`
  - color format: `GRB`
  - output inversion: `OFF`
  - drive strength: `GPIO_DRIVE_CAP_3`
  - configured LED count: `12`

## Target profile mapping in code

Defined in `main/main.c`:

- **ESP32-S2 profile**
  - `OUTPUT_LED_STRIP_GPIO = 16`
  - `STATUS_LED_GPIO = 15`
  - `STATUS_LED_USE_GPIO = 1`
- **ESP32-S3 profile**
  - `OUTPUT_LED_STRIP_GPIO = 18`
  - `STATUS_LED_GPIO = 48`
  - `STATUS_LED_USE_GPIO = 0`

## How to configure per target (S3 vs S2)

Use these as resume steps before build/flash when switching hardware.

### ESP32-S3 (last validated)

1. Set build target to `esp32s3`.
2. In menuconfig, keep TinyUSB network mode on **NCM**.
3. Flash and monitor.
4. Verify host gets USB-NCM adapter on subnet `192.168.4.x` and test Art-Net send to `192.168.4.1`.

Expected profile behavior in code:

- Output LED data pin: `GPIO18`
- Status LED pin: `GPIO48` (status GPIO mode disabled by default)
- LED timing/model: `WS2812`

### ESP32-S2

1. Set build target to `esp32s2`.
2. In menuconfig, keep TinyUSB network mode on **NCM** (if available for board/IDF version).
3. Flash and monitor.
4. Verify Art-Net reception over USB network and LED output on the S2 profile pin.

Expected profile behavior in code:

- Output LED data pin: `GPIO16`
- Status LED pin: `GPIO15` (`STATUS_LED_USE_GPIO = 1`)
- LED timing/model: `WS2812`

### Notes when switching between S3 and S2

- After changing target, do a clean rebuild to avoid stale artifacts.
- `sdkconfig` is local/ignored in this repo; regenerate via menuconfig on each machine as needed.
- If output is wrong after a target switch, verify the selected target first, then pin wiring.

## Sender script defaults

`tools/artnet_send.py` defaults are tuned for the baseline:

- default target: `192.168.4.1`
- default channels: `36` (12 RGB LEDs)

Useful examples:

- Basic rainbow:
  - `python tools/artnet_send.py --pattern rainbow --fps 20 --count 120`
- With explicit source bind (multi-NIC host):
  - `python tools/artnet_send.py --target 192.168.4.1 --source-ip 192.168.4.2 --pattern rainbow --fps 20 --count 120`

## Environment and tooling notes

- Serial ports are machine-dependent (COM number varies); do not rely on a fixed COM port in docs or scripts.
- If monitor tooling behaves inconsistently, prioritize build/flash/functionality first, then normalize monitor command usage.

## Suggested first checks when resuming work

1. Confirm selected ESP-IDF target (`esp32s2` vs `esp32s3`).
2. Build and flash successfully.
3. Verify USB-NCM interface appears on host and has expected IP subnet.
4. Send a short rainbow pattern from `tools/artnet_send.py`.
5. If LEDs misbehave, re-check timing model (`WS2812`) and output GPIO for the active profile.

## Candidate next improvements

- Add optional runtime switch to disable startup self-test.
- Improve idle animation customization.
- Add brief troubleshooting section in `README.md` for host network adapter/IP binding.
