# Stoplicht

An ESP32-powered traffic light that shows whether it's beer time yet at [GEWIS](https://gewis.nl).

| Light  | Meaning                                    |
| ------ | ------------------------------------------ |
| Red    | Not beer time yet                          |
| Orange | Beer time starts within 15 minutes         |
| Green  | It's beer time, until midnight             |

The beer time comes from the Aurora API (`/api/handler/screen/info/beer-time`) and is refreshed every 15 minutes. If WiFi is down or the request fails, it tries again after 30 seconds. The clock is set from the `Date` header of that API response, so no NTP server is needed. Times are shown in Europe/Amsterdam time.

## Hardware

A vintage 1995 Dutch traffic light, switched at 230 V by the ESP32 through a relay module.
See [docs/hardware.md](docs/hardware.md) for parts and wiring.

| Light  | GPIO | Relay |
|--------|------|-------|
| Red    | 25   | IN1   |
| Orange | 26   | IN2   |
| Green  | 27   | IN3   |

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. Copy the secrets template and fill in your WiFi credentials and Aurora API key:

   ```sh
   cp include/secrets.h.example include/secrets.h
   ```

3. Set `upload_port` and `monitor_port` in `platformio.ini` to your board's serial port, or remove them so PlatformIO detects the port itself.
4. Build, flash and open the serial monitor:

   ```sh
   pio run -t upload
   pio device monitor
   ```

The serial log (115200 baud) shows WiFi events, the beer time it fetched, and every time the light changes.
