# ESPArtNetNode Resume Notes (2026-02-25)

## Current known-good setup

- Board: ESP32-S3-WROOM-1 N8R2
- USB network mode: USB-NCM
- Art-Net UDP port: 6454
- ESP USB-NCM IP: 192.168.4.1
- Host USB adapter IP (typically): 192.168.4.2

## LED output settings (confirmed working)

- External LED data GPIO: 18
- LED model/timing: WS2812 (required for Qanba buttons on this setup)
- Color format: GRB
- Output inversion: OFF
- Drive strength: strongest (GPIO_DRIVE_CAP_3)
- Configured LED count: 12 (safe to use even if fewer are connected)

## Qanba harness findings

- Harness wiring: GND / DATA / VCC into chain, per-button pins GND / DIN / VCC / DOUT
- Symptom was "white latch" on boot with non-working control.
- Working solution was switching timing from WS2811 to WS2812 and moving data pin to GPIO18.

## Boot behavior currently enabled

- Startup self-test remains enabled:
  - Full-strip color cycle: red -> green -> blue -> white -> off
  - Then walking single white pixel across configured LED count

## Sender defaults (tools/artnet_send.py)

- Default target: 192.168.4.1
- Default channels: 36 (12 RGB LEDs)

## Useful send commands

- Basic rainbow (uses defaults):
  - python tools/artnet_send.py --pattern rainbow --fps 20 --count 120

- Force source binding to USB adapter (useful on multi-NIC hosts):
  - python tools/artnet_send.py --target 192.168.4.1 --source-ip 192.168.4.2 --pattern rainbow --fps 20 --count 120

- 2-LED explicit test sequence (R/G/B/off):
  - python tools/artnet_send.py --target 192.168.4.1 --source-ip 192.168.4.2 --pattern solid --rgb 255,0,0 --channels 6 --fps 20 --count 60
  - python tools/artnet_send.py --target 192.168.4.1 --source-ip 192.168.4.2 --pattern solid --rgb 0,255,0 --channels 6 --fps 20 --count 60
  - python tools/artnet_send.py --target 192.168.4.1 --source-ip 192.168.4.2 --pattern solid --rgb 0,0,255 --channels 6 --fps 20 --count 60
  - python tools/artnet_send.py --target 192.168.4.1 --source-ip 192.168.4.2 --pattern solid --rgb 0,0,0 --channels 6 --fps 20 --count 60

## Next planned improvements

- Optional idle animation mode when no Art-Net frames are received
- Automatic handoff to Art-Net when packets arrive
- Optional disable switch for startup self-test once wiring is finalized

## Notes about monitor issues

- Flashing on COM14 works reliably.
- Some idf.py monitor invocations failed in shell; direct monitor script invocation worked earlier.
- If needed later, focus on functionality first, monitor tooling can be normalized separately.
