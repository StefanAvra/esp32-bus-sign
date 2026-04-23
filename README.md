# esp32-bus-sign

A tiny countdown sign for your local bus stop. An ESP32 polls a public
real-time departures API and shows the next bus on a 128×64 OLED, with a
small WS2812B LED strip acting as a visual "minutes until" indicator — one
LED per minute, colored by how likely it is that you'll make it.

Built for [VVS](https://www.vvs.de/) (Stuttgart) but the underlying EFA
rapidJSON endpoint is used by many German transport associations, so it
ports easily.

## Features

- **OLED departures.** Shows 1 or 2 distinct bus lines with minutes-until and
  clock time. Single-line view also surfaces the _following_ departure.
- **Minute-by-minute LED strip.** Each LED position represents one minute.
  LED[0] = next minute, LED[1] = in 2 min, etc.
  - `≤ 0 min` (imminent) — **pulses red** on LED[0]
  - `1 min` — red
  - `2 min` — amber
  - `≥ 3 min` — green
  - Farther than the strip can show — **blue** on the last LED
  - When two buses map to the same LED, urgency wins (imminent > red > amber > green > blue)
- **Burn-in mitigation.** Periodic pixel shift + hourly display invert.
- **Optional night window.** Turns the OLED + LEDs off between configured hours.
- **Resilient WiFi.** Auto-reconnect with stall recovery (the ESP32 driver
  occasionally wedges after a `reason=34` drop — we force-reconnect).
- **Frame-hash redraw.** OLED only updates when something actually changes.

## Hardware

Tested on a **Heltec WiFi Kit 32 V2** (ESP32 + 128×64 monochrome SSD1306
OLED on the board's `Vext` rail). Should work on other ESP32 + SSD1306
boards with small pin/init tweaks (remove `VextON()`, change the OLED
constructor).

- WS2812B LED strip on **GPIO 17**, 5 V, shared ground with the ESP32.
- Default strip length is **4 LEDs**; change `LED_COUNT` in `config.h` to
  scale up — rendering already handles arbitrary length.
- Should also work for other transports that use the same EFA API
  (U-Bahn, S-Bahn, tram, regional) — see _Other networks_ below.

## Setup

1. **Install tooling**
   - Arduino IDE (or arduino-cli)
   - ESP32 board package (Espressif)
   - Heltec ESP32 Series Dev-boards package (for the `HT_SSD1306Wire.h` driver and `Vext` / `SDA_OLED` / `SCL_OLED` / `RST_OLED` defines)
2. **Install libraries**
   - [FastLED](https://github.com/FastLED/FastLED)
   - [ArduinoJson](https://arduinojson.org/) v7
3. **Configure**
   ```sh
   cp secrets.h.example secrets.h
   ```
   Edit `secrets.h` with your WiFi credentials, then edit `config.h`:
   - `VVS_URL` — insert your stop's platform Global ID (see next section)
   - `LED_COUNT`, `LED_DATA_PIN` — match your strip + wiring
   - `TZ_STRING` — your timezone (default is Europe/Berlin with DST)
   - `NIGHT_WINDOW_ENABLED` + hours — optional "sleep" window
4. **Flash**
   Select board "Heltec WiFi Kit 32(V2)" (or your ESP32 variant), upload,
   open serial monitor at 115200 baud to see WiFi/NTP/fetch progress.

## Finding your bus stop ID

VVS stop IDs use the German **IFOPT** format:

```
de:AAAAA:BBBB:P:S
   │      │    │ └─ section (usually 1, 2, ...)
   │      │    └─── platform (0 = aggregate, 1+ = specific bay)
   │      └──────── stop ID within the municipality
   └─────────────── municipality code (08111 = Stuttgart)
```

Easiest way to find yours:

1. Open <https://www3.vvs.de/>, search for your stop, click through to the
   departures page.
2. Open your browser's developer tools → Network tab, reload the departures.
3. Look at the `XML_DM_REQUEST` call — the `name_dm` query parameter is the
   ID you want. Copy it into `VVS_URL` in `config.h`.

Alternatively, use the public stop-finder endpoint:

```
https://www3.vvs.de/mngvvs/XML_STOPFINDER_REQUEST?outputFormat=rapidJSON&type_sf=any&name_sf=<your search>
```

The platform-scoped ID shows all lines passing that specific platform
(useful if you only care about one direction). The stop-scoped ID (`P=0`)
shows everything.

> **A note on API usage**
>
> This firmware talks directly to the **public `www3.vvs.de` endpoint** that
> the VVS website uses. It is:
>
> - **Not documented or officially supported.** No SLA, no rate-limit docs.
> - **TLS verification is disabled** (`client.setInsecure()`) because
>   cert-bundle management on ESP32 is its own project. Fine for hobby use on
>   your local WiFi, not fine for anything security-sensitive.
> - **Please be polite** — the default 30 s poll interval is already gentle.
>   Don't go much faster. Don't deploy 100 of these.
>
> **For anything you plan to run long-term or publish, use the official
> [VVS Open-Data API](https://www.openvvs.de/) instead.** It requires a
> free sign-up and an API key but is officially supported, documented, and
> much less likely to change or block you.

### Other networks

The code uses EFA rapidJSON field names that are common across German
transport-association EFA servers (`transportation.number`,
`transportation.destination.name`, `departureTimeEstimated` /
`departureTimePlanned`). Pointing this firmware at **VRR, MVV, HVV,
NVBW**, etc. is usually just a matter of swapping `VVS_URL` for the
equivalent endpoint from that provider. U-Bahn, S-Bahn, tram, and regional
lines returned by the endpoint should all work without code changes.

## Roadmap/todos

- Other display (segment, eink, LED matrix?). The tiny OLED is just what I had in arms reach, but it's not ideal.
- WiFiManager captive-portal onboarding, so non-developers can set WiFi
  and stop ID without recompiling.
- Optional TLS cert pinning.
- Support for non-EFA backends (HAFAS, GTFS-RT).

## License

MIT — see [LICENSE](./LICENSE).
