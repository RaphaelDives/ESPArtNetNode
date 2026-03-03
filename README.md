# ESPArtNetNode

ESP32-S3 USB-NCM Art-Net receiver with addressable LED output.

## Status

Working baseline is confirmed on this project and documented in detail in [NOTES.md](NOTES.md).

## Quick Start

1. Build and flash via ESP-IDF extension commands.
2. Connect host over USB and ensure the USB network adapter is up.
3. Send Art-Net to `192.168.4.1:6454`.

Example sender command:

```powershell
python tools/artnet_send.py --target 192.168.4.1 --pattern rainbow --fps 20 --count 120
```

If your PC has multiple network adapters, bind source IP explicitly:

```powershell
python tools/artnet_send.py --target 192.168.4.1 --source-ip 192.168.4.2 --pattern rainbow --fps 20 --count 120
```

## Current Working LED Config

- Output GPIO: `18`
- LED timing/model: `WS2812`
- LED count: `12`
- Sender default channels: `36`

## Notes

- Resume and troubleshooting details: [NOTES.md](NOTES.md)
- Known monitor-tooling caveat is also tracked in [NOTES.md](NOTES.md)
