# ESPArtNetNode

This is an ESP32-S3 USB-NCM Art-Net receiver with addressable LED output. It was build for my 23''-VPin-Table, to light up my RGB-LED Buttons.
(The official option is to use a quite expensive Teensy Board which is very rare meanwhile - and yeah I had the ESP flying around...)

You need an ESP32 with build in USB support to emulate the Network device. This code was developed on a ESP32-S3-WROOM-1 N8R2 Devkit with two USB Ports. (Used the native USB for network and the second one for debugging.)

When it seems to works like it should I moved on to a ESP32S2 WEMOS Mini S2 Board. The plan was to use one USB Port for Serial and Network Device - serial is not working yet, but the network is running and receiving ArtNet Frames und sends them to the LED Strip... ;)

## Disclaimer

This project was created with extensive use of Chat-GPT-5.3-codex, which did most of the work.
There are some notes that GPT wanted to share with the world or with its future "it" in [NOTES.md](NOTES.md).

On one hand this project is here that I will find it later again, on the other hand I never saw a project like this before, so maybe it could be of some use to someone, at least to see that it is posible! ;) (I really wonder why nobody had this idea before...)

This project is "as it is" it's just a weekend-fun-project that had some useful result. (Surprise!) - so I can't and won't give much support on anything. Sorry.
But it might be possible that I answer a question, when I find the time. ;)

## Prerequisites
1) The whole project was build against the ESP-IDF 5.5.3, so you should install this ;)
2) You also need the libraries "espressif__tinyusb", "espressif__esp_tinyusb" and "espressif__led_strip"
3) I used VS-Code with the Github agent

## Quick Start

1. Build and flash via ESP-IDF extension commands.
(Remember: After retargeting to S2/S3 you have to configure the TinyUSB Stack for the NCM Device!)
2. Connect host over USB and ensure the USB network adapter is up.
3. In Device Manager install the "Microsoft USB NCM Host Device"
4. Send Art-Net to `192.168.4.1:6454`.

## Testing

Example sender command:

```powershell
python tools/artnet_send.py --target 192.168.4.1 --pattern rainbow --fps 20 --count 120
```

If your PC has multiple network adapters, bind source IP explicitly:

```powershell
python tools/artnet_send.py --target 192.168.4.1 --source-ip 192.168.4.2 --pattern rainbow --fps 20 --count 120
```
## Current Working LED Config

ESP32S3: LED Output on GPIO18
ESP32S2: LED Output on GPIO15

LED timing/model: `WS2812`
LED count: `12` which is `36` channels in total.

## Config section in main.c

I think most of the Options at the beginning of main are self explaining. Don't touch the rest - you've bee warned! :D

## Designated use in my VPin Table

I'm running my VPin with 10 Qanba RGB Buttons (https://www.qanba.com/products/qanba-gravity-ks-rgb-led-illuminated-buttons?VariantsId=10505), with the WEMOS S2 Mini Board, and I power the LEDs directly over my USB Hub.

On my table I use the VirtualPinballX64 with the DirectOutput library (DOF R+++). (You need a proper cabinet.xml and the DOF Config Tool - good luck)

## Useful resources

1. Please check out the awesome Pinscape Build Guide by Michael Roberts: http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php
2. For the ArtNet sender check out this page: https://directoutput.github.io/DirectOutput/

## Random Notes
- ToDo: Serial Port on S2 Mini is not working (compound device)
- ToDo: Status LED on my S2 is also not working...
- Play more Pinball!
- While testing, in some situation there's a random flashing of LEDs, Maybe we should test the ArtNet Frames if the are sent "completely"?
- I like the Startup Animation (you can switch it off ;)
- I also like the Idle Mode (30 sec. after a ArtNet Frame with complete Zeros I start an RGB Animation over all Buttons)
- The arrow keys in the esp-idf configmenu, running in VSCode, are not working - use "J" and "K" instead (If I remember correctly...)
- Life is too short for cold coffee!
