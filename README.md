# ESP32 opencode Usage Display

Shows opencode Zen quota usage (rolling 5hr / weekly / monthly, as percentages)
on a 1.69" ST7789 SPI TFT (240x280) connected to an ESP32.

The opencode Zen API (`GET /zen/go/v1/usage`) only exposes quota **percentages**,
not token counts or dollar costs, so that's what this displays.

## Hardware

- ESP32 DevKitC / WROOM32
- ST7789 240x280 SPI TFT, 1.69"

Default wiring (edit in `platformio.ini` build_flags if yours differs):

| TFT pin | ESP32 pin |
|---|---|
| MOSI (SDA) | GPIO 23 |
| SCLK (SCL) | GPIO 18 |
| CS | GPIO 15 |
| DC | GPIO 2 |
| RST | GPIO 4 |
| BL (backlight) | GPIO 27 |
| VCC | 3.3V |
| GND | GND |

Optional: wire a button between GPIO 0 (BOOT button on most DevKitC boards) and
GND to force re-provisioning on next boot.

## Firmware design

No WiFi credentials or API keys are compiled into the firmware or stored in
this repo. On first boot (or if saved WiFi fails, or the provisioning button
is held at boot), the device starts a WiFi access point called
`OpenCode-Display`. Connect to it with your phone/laptop, a captive portal
page opens automatically (or browse to `192.168.4.1`), and enter:

- Your WiFi SSID + password
- Your opencode API token

These are saved to the ESP32's internal flash (NVS) and reused on every boot
after that. To reprovision (new WiFi network, new token), hold the button on
GPIO 0 while powering on / resetting.

## Building locally

Requires [PlatformIO](https://platformio.org/).

```
pio run -e esp32dev          # build
pio run -e esp32dev -t upload  # build + flash over USB
pio device monitor            # serial log
```

## CI/CD

`.github/workflows/release.yml` builds the firmware on every push/PR, and on
any pushed tag matching `v*` (e.g. `v1.0.0`) it publishes `firmware.bin` (plus
`bootloader.bin` / `partitions.bin`) as a GitHub Release artifact. No secrets
are required in CI since credentials are never compiled in.

To cut a release:

```
git tag v1.0.0
git push origin v1.0.0
```

Then flash the released `firmware.bin` with `esptool.py` or the PlatformIO/
Arduino IDE upload tools, no build environment required on that machine.

## Flashing a pre-built release with esptool

```
pip install esptool
esptool.py --chip esp32 --port COM3 write_flash \
  0x1000 bootloader.bin \
  0x8000 partitions.bin \
  0x10000 firmware.bin
```

Adjust `COM3` to your device's serial port. Offsets assume the default
ESP32 Arduino partition table.
