# Sonos What's Playing Display

Current Application Version: see `APP_VERSION` in `src/main.cpp` (auto-incremented per build)

## Requirements Checklist
- [ ] **Application Version**: Display application version number on the screen. Started at `0.01`, updated to `0.02`. (Each build updates version by 0.01).
- [ ] **WiFi Connectivity**: Connect to network using:
  - SSID: your network's SSID, with a fallback SSID
  - Password: set in `src/secrets.h` (not committed)
- [ ] **Sonos Speaker Connection**:
  - Automatically find the first Sonos speaker on the network via SSDP and connect to it.
  - Fallback Sonos speakers: set in `src/secrets.h`.
- [ ] **Track & Artist Display**:
  - Show the track name and artist name on the screen.
  - Exclude continuation string from artist name: everything starting at and including `' - /getaa'` is excluded.
- [ ] **Track Position & Duration Display**:
  - Extract `RelTime` (track position) and `TrackDuration` from `GetPositionInfo`.
  - Display current position, duration, and progress bar on screen.
- [ ] **Artwork Display (/getaa)**: 
  - This currently has issues - Include debug output to the terminal screen to assist in debug
  - `/getaa` returns the artwork URL for the current track.
  - Download JPEG (handling relative URLs, HTTP and HTTPS redirects) and cache in LittleFS.
  - Render JPEG scaled to fit on screen without obscuring text.
- [ ] **Idle State & Auto Screen Shutdown**:
  - If nothing is currently playing, display a message stating nothing is playing.
  - Shut down the screen (turn off backlight and sleep display) after 10 seconds.
  - Wake screen back up immediately when playback starts or an onboard button is pressed.
- [ ] **Hardware Controls**:
  - Maintain button debouncing for onboard buttons (GPIO 35 for Next Track, GPIO 0 for Play/Pause toggle).
  - Maintain ST7789 display initialization and backlight control on GPIO 4 (`TFT_BL`).

---

# Implementation Notes (appended)

Documents the firmware as currently built, its dependencies, config changes and
outstanding work. The **Task List** in section 8 is the backlog.

## 1. Status summary

| README requirement | State | Notes |
|---|---|---|
| Application version displayed | Working | Top-left; auto-increments per build |
| WiFi (primary + fallback SSID) | Working | Both set in `src/secrets.h`, primary then fallback |
| Sonos discovery (SSDP + fallbacks) | Working | M-SEARCH, first speaker adopted |
| Track & artist display | Working | Title from `<dc:title>`, artist from `<dc:creator>` (section 6.1) |
| Position, duration, progress bar | Working | `RelTime` / `TrackDuration` parse and render |
| Artwork (`/getaa`) | **Not working** | URI extraction and download both unverified on hardware |
| Idle message + 10 s auto-shutdown | Working | Countdown shown, backlight cut, wakes on play/button |
| Hardware controls | Working | 40 ms debounce, GPIO 35 / GPIO 0 |

## 2. Sonos UPnP / SOAP interface

### Transport
| Item | Value |
|---|---|
| Endpoint | `POST http://<speaker-ip>:1400/MediaRenderer/AVTransport/Control` |
| Service type | `urn:schemas-upnp-org:service:AVTransport:1` |
| `SOAPACTION` header | `"urn:schemas-upnp-org:service:AVTransport:1#<Action>"` |
| `Content-Type` | `text/xml; charset="utf-8"` |
| Instance | `<InstanceID>0</InstanceID>` (the speaker's own renderer) |

### Request envelope (built by `buildEnvelope()`)
```xml
<?xml version="1.0" encoding="utf-8"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"
            s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
  <s:Body>
    <u:GetPositionInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">
      <InstanceID>0</InstanceID>
    </u:GetPositionInfo>
  </s:Body>
</s:Envelope>
```

### Actions used
| Action | Body arguments | Purpose |
|---|---|---|
| `GetTransportInfo` | `<InstanceID>0</InstanceID>` | Playback state |
| `GetPositionInfo` | `<InstanceID>0</InstanceID>` | Track, artist, times, artwork |
| `Play` | `<InstanceID>0</InstanceID><Speed>1</Speed>` | GPIO 0 button |
| `Pause` | `<InstanceID>0</InstanceID>` | GPIO 0 button |
| `Next` | `<InstanceID>0</InstanceID>` | GPIO 35 button |

### Response field to on-screen element
The authoritative mapping used by `pollSonos()` / `drawScreen()`.

| Source | Example | Stored in | Rendered as | Position |
|---|---|---|---|---|
| `GetTransportInfo` / `CurrentTransportState` | `PLAYING` | `currentTransportState` | Badge text + colour | top-right, 48x15 |
| `CurrentTrackMetaData` / `dc:title` | `Creep` | `currentTitle` | Track title, font 2 | `x=4, y=86` |
| `CurrentTrackMetaData` / `dc:creator` | `Radiohead` | `currentArtist` | Artist, font 1 | `x=4, y=102` |
| `GetPositionInfo` / `RelTime` | `0:01:23` | `currentRelTime` | Elapsed time, font 1 | `x=4, y=111` |
| `GetPositionInfo` / `TrackDuration` | `0:03:58` | `currentDuration` | Total time, font 1 | right-aligned, `y=111` |
| `CurrentTrackMetaData` / `upnp:albumArtURI` | `http://.../getaa?u=` | `currentArtUri` | Album art JPEG | `0,0` 240x84 |

`CurrentTransportState` values seen in practice: `PLAYING`, `PAUSED_PLAYBACK`,
`STOPPED`.

**Note:** `<Track>` is NOT the song title for queue/playlist playback - see 6.1.

### DIDL metadata (inside `CurrentTrackMetaData`)
The blob arrives **XML-escaped** (`&lt;dc:title&gt;...`), so the whole value is
unescaped *before* any tag inside it is searched for.

| DIDL tag (local name) | Used for |
|---|---|
| `title` (`dc:title`) | Track title (primary) |
| `creator` (`dc:creator`) | Artist (primary) |
| `albumArtURI` (`upnp:albumArtURI`) | Artwork URL, usually `http://<ip>:1400/getaa?u=...` |
| `res` | Last-resort artwork source for local/native content |
| `CurrentTrackArtImage` | Artwork fallback on the AVTransport response |

### Speaker discovery (SSDP, not SOAP)
```
M-SEARCH * HTTP/1.1
HOST: 239.255.255.250:1900
MAN: "ssdp:discover"
MX: 2
ST: urn:schemas-upnp-org:device:ZonePlayer:1
```
Responses are accepted only if they contain `Sonos`; the speaker address comes
from the `LOCATION` header, e.g.
`http://192.168.1.50:1400/xml/device_description.xml`. If nothing is found
within 5 s the fallback addresses from `src/secrets.h` are tried, then
rediscovery runs every 60 s.

## 3. Artwork pipeline
1. `albumArtURI` read from the DIDL metadata.
2. `resolveArtworkUrl()` - accepts absolute `http(s)://`, protocol-relative
   `//host`, and bare paths (resolved against the speaker).
3. `downloadArtwork()` - manual redirect handling (301/302/303/307/308, max 3
   hops), HTTPS via `WiFiClientSecure` with `setInsecure()`, streamed to
   `/art.jpg` in LittleFS in 1 KB chunks.
4. `renderArtwork()` - `TJpgDec.getFsJpgSize()` then `drawFsJpg()`, scaled by
   the smallest power-of-two factor that fits 240x84, centred, clipped to the
   artwork region so it cannot overwrite text.
5. Failed or partial downloads delete the cached file so a truncated JPEG is
   never rendered.

## 4. Configuration changes made

**`platformio.ini`** - the `build_flags` block was **not modified** (as
instructed). One key was added:

```ini
extra_scripts = pre:version_bump.py
```

**`version_bump.py`** (new file) - increments `APP_VERSION` in `src/main.cpp` by
`0.01` before each build and rewrites the source, so the new value is compiled
in. Carries `0.99 -> 1.00`. Opt out with:

```
PLATFORMIO_SKIP_VERSION_BUMP=1 pio run
```

**`src/main.cpp`** - implemented from scratch (the file previously contained only
the word `test`). All constants follow `config.md`.

## 5. Library API notes (Arduino ESP32 2.0.17 / TFT_eSPI 2.5.43)

Deviations from the obvious API, found at compile time or on hardware.

| Issue | Resolution |
|---|---|
| `TFT_DATUM_TOP_LEFT` etc. do not exist in TFT_eSPI 2.5.43 | Use `TL_DATUM`, `TC_DATUM`, `TR_DATUM` |
| `TFT_eSPI::textHeight()` does not exist | Use `fontHeight()` |
| `WiFiUDP` has no `writeMulticast()` | `beginMulticast()` -> `beginPacket(ip, port)` -> `write()` -> `endPacket()` |
| `HTTPClient::getString()` returns `""` when `Content-Length: 0` (which Sonos does send) | Custom `readHttpBody()` reads the socket directly |
| `HTTPClient` does **not** de-chunk on receive (only on send) | `readHttpBody()` sniffs for chunk framing and reassembles it |
| `WiFiClient::readStringUntil()` returns `void` on this core | `readLineRaw()` assembles a line byte-by-byte |
| `String::trim()` returns `void` (in-place) | Call as a statement, never chained |
| Bundled bitmap fonts are **ASCII-only** (glyphs 32-127) | `sanitizeText()` strips all other bytes |
| `GetPositionInfo` `<Track>` is the queue track number, not the title | Prefer `dc:title` from the DIDL metadata - see 6.1 |

## 6. Known issues

### 6.1 Track title - ROOT CAUSE FOUND AND FIXED

**Cause:** `GetPositionInfo`'s `<Track>` element does not reliably carry the
song title. For queue and playlist playback Sonos returns the **queue track
number** there (e.g. `"3"`), while the real title lives in the DIDL-Lite
metadata carried by `CurrentTrackMetaData` as `<dc:title>`.

This was found with the `TITLE_ROW_DIAG` swap test: rendering the title in the
artist row showed a *number* alternating with the artist name, proving the value
was a track number that was rendering perfectly well. The earlier theories (bad
value, unavailable font, ASCII stripping) were all wrong.

A secondary bug hid this: the DIDL fallback was guarded by
`if (title.length() == 0)`. Because `<Track>` returned a non-empty number, that
condition was never true and the fallback never ran.

**Fix:** `resolveTrackField()` is now the single code path for both the title
and the artist, so the two cannot diverge again:

```cpp
String title  = resolveTrackField(metaData, "title",   positionBody, "Track");
String artist = resolveTrackField(metaData, "creator", positionBody, "TrackArtist");
```

It prefers the DIDL element (`<dc:title>` / `<dc:creator>`) and uses the
AVTransport element only as a fallback. That ordering is what stops the track
number appearing in place of the title. Drawing remains shared via
`drawTextRow()`, so both parse and render are single-path.

`TITLE_ROW_DIAG` is retained but set to `0`; set it to `1` to re-run the swap
test if this ever regresses.

### 6.2 No artwork
Unverified on hardware. Two distinct failure points need separating:
- **URI extraction** - is `Art="..."` populated in the per-track log?
- **Download** - does the `[Art]` block report a status, then a byte count?

`Art="..."` empty means extraction; a non-empty URI plus a failed status means
download. No fix has been attempted against real output yet.

## 7. Serial debug reference

| Prefix | Meaning |
|---|---|
| `[Display]` | Geometry and font metrics at boot |
| `[WiFi]` | Connection attempts, primary then fallback |
| `[SSDP]` | M-SEARCH sent, devices discovered, table full |
| `[Sonos]` | Parsed fields per track; active speaker |
| `[Debug]` | SOAP action, HTTP status, Content-Length vs bytes received |
| `[Art]` | URL resolution, redirects, byte counts, JPEG decode result |
| `[LittleFS]` | Mount / format |
| `[Screen]` | Backlight on/off |
| `[Button 1/2]` | Button presses (Next / Play-Pause) |
| `[Diag]` | Title-row swap test (only when `TITLE_ROW_DIAG=1`) |
| `[version-bump]` | `APP_VERSION 0.12 -> 0.13` at the start of each build |

## 8. Task list (future functionality)

**Bugs**
- [x] Track title - fixed: `<Track>` holds the queue track number, not the title; now read from `<dc:title>` (see 6.1)
- [ ] Artwork not displayed - separate URI extraction from download failure (see 6.2)
- [ ] `README.md` / `config.md` still state version `0.02`; code auto-increments from `0.03`
- [ ] Checklist boxes in "Requirements Checklist" are still unticked

**Robustness**
- [ ] Retry/backoff on failed Sonos queries and artwork downloads
- [ ] Transliterate non-ASCII text instead of dropping characters
- [ ] Handle track/group changes via `AVTransport` events instead of polling only
- [ ] Deep-sleep when idle for further power saving (backlight is cut today; CPU stays awake)
- [ ] FreeRTOS task so the 1 s poll does not block drawing or button handling
- [ ] Guard against a corrupted `/art.jpg` surviving a power cut mid-write
- [ ] Remove the one-shot RAW dump once the wire format is fully confirmed

**Features**
- [ ] Volume control on a third button or long-press
- [ ] Album name line
- [ ] Select between multiple Sonos zones
- [ ] Show elapsed/total for streams with no duration
- [ ] NTP time, clock and last-updated timestamp
- [ ] Configurable WiFi/Sonos credentials (NVS) instead of compile-time constants
- [ ] OTA updates
- [ ] Brightness control rather than fixed on/off
- [ ] Screensaver / clock mode when idle
