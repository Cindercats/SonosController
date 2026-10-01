# Project Configuration Settings

## Application Settings
- **Application Version**: see `APP_VERSION` in `src/main.cpp` (auto-incremented by 0.01 on each build; currently `0.70`)
- **Version Increment**: increased by 0.01 on each rebuild, via `version_bump.py` (opt out with `PLATFORMIO_SKIP_VERSION_BUMP=1`)

## WiFi Configuration
- **Primary SSID**: set in `src/secrets.h` (`WIFI_SSID_PRIMARY`)
- **Fallback SSID**: set in `src/secrets.h` (`WIFI_SSID_SECONDARY`)
- **Password**: set in `src/secrets.h` (`WIFI_PASSWORD`)

  NOTE: credentials live in `src/secrets.h`, which is gitignored. Copy
  `src/secrets.example.h` to `src/secrets.h` and fill it in.

## Sonos Speaker Configuration
- **Fallback Speaker 1**: set in `src/secrets.h` (`FALLBACK_SONOS_1`)
- **Fallback Speaker 2**: set in `src/secrets.h` (`FALLBACK_SONOS_2`)
- **SSDP Multicast IP**: `239.255.255.250` (`SSDP_MULTICAST_IP`)
- **SSDP Port**: `1900` (`SSDP_PORT`)
- **Maximum Sonos Devices Tracked**: `8` (`MAX_SONOS_DEVICES`)

## Hardware Pin Assignments
- **Button 1 (Next Track)**: GPIO 35 (`PIN_BUTTON_1`)
- **Button 2 (Play/Pause)**: GPIO 0 (`PIN_BUTTON_2`)
- **Display Backlight**: GPIO 4 (`TFT_BL`)

## Timing & Debouncing
- **Button Debounce Delay**: 40 ms (`DEBOUNCE_DELAY_MS`)
- **Sonos Polling Interval**: 1000 ms (for track updates)
- **SSDP Rediscovery Interval**: 
  - 5000 ms when no active speaker found
  - 60000 ms when active speaker found
- **Idle Screen Timeout**: 10000 ms (10 seconds of nothing playing before screen turns off)
- **Toast Notification Duration**: 1600 ms (1.6 seconds)

## File System & Storage
- **Artwork Cache Path**: `/art.jpg` (`ART_FILE_PATH`)
- **Storage System**: LittleFS (for storing downloaded artwork)

## Display Settings
- **Display Controller**: ST7789 (via TFT_eSPI library)
- **Screen Resolution**: 240x135 pixels
- **Screen Rotation**: Landscape mode (rotation 1)
- **Artwork Display Area**: Maximum 240x84 pixels (upper screen)
- **Metadata Display Area**: 51 pixels height (lower screen, starting at Y=84)
- **Progress Bar Height**: 12 pixels (at Y=122)
- **Status Badge**: Top-right corner (48x15 pixels)
- **Toast Notification**: Center-top (140x22 pixels)

## Network Timeouts & Buffers
- **HTTP Request Timeout**: 2000-2500 ms (for Sonos queries)
- **Artwork Download Timeout**: 8000 ms
- **SSDP Packet Buffer**: 1024 bytes
- **HTTP Redirect Limit**: 3 redirects (max 4 attempts total)

## Feature Status (per README.md Checklist)
- [x] Application Version Display
- [x] WiFi Connectivity (with fallback SSID)
- [x] Sonos Speaker Auto-discovery + Fallback
- [x] Track & Artist Display (with '/getaa' exclusion)
- [x] Track Position & Duration + Progress Bar
- [~] Artwork Display (/getaa) - **PARTIAL**: URI extraction, download, redirects,
      chunked-transfer handling and LittleFS caching are all verified working on hardware
      (138,711 bytes). Colour order is fixed. However JPEGDEC decodes progressive JPEGs
      **DC-scan only**, so progressive artwork renders as a low-detail thumbnail, and the
      forced 1/8 scale stalled the main loop (fixed by not re-arming renders on metadata
      changes). The proper fix is to request a baseline rendition from `/getaa`.
      See README.md section 6.5 and ARTWORK-ATTEMPTS.md.
- [x] Idle State & Auto Screen Shutdown (10s timeout)
- [x] Hardware Controls (debounced buttons, backlight control)

## Future Settings Needed (from README.md & Code Analysis)
1. **Artwork Decode** (outstanding): `esp_jpg_decode()` cannot decode the progressive
   JPEGs Sonos serves, and JPEGDEC only decodes the DC scan (a low-detail thumbnail).
   The preferred option is to request a baseline rendition from `/getaa`; the fallback
   is a progressive-capable decoder. See README.md section 6.10.

2. **Secondary WiFi SSID**: DONE - `WIFI_SSID_SECONDARY` is defined in `src/secrets.h`.

3. **Version Automation**: DONE - `version_bump.py` auto-increments `APP_VERSION` by
   0.01 on each compile.

4. **Artwork Fallback Enhancement**: Improve the vinyl placeholder or add alternative
   fallback artwork.

5. **Network Resilience**: Add retry mechanisms for failed Sonos queries and artwork downloads.

6. **Power Management**: Consider adding deep sleep modes for further power savings when idle.

---
*Configuration generated from main.cpp and README.md analysis*