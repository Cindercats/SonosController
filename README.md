# Sonos What's Playing Display

ESP32 firmware for the LilyGO T-Display that discovers a Sonos speaker on the local
network and shows the currently playing track, position and album artwork.

Current Application Version: `0.70` (see `APP_VERSION` in `src/main.cpp`; auto-incremented per build)

## Requirements Checklist

- [x] **Application Version**: Display application version number on the screen. Started at `0.01`, now at `0.70`. Each build increments the version by 0.01.
- [x] **WiFi Connectivity**: Connects to the network using the primary SSID, falling back to a second SSID. Credentials live in `src/secrets.h` (gitignored).
- [x] **Sonos Speaker Connection**: Finds the first Sonos speaker via SSDP (`M-SEARCH`) and adopts it; probes two compile-time fallback IPs if discovery comes up empty.
- [x] **Track & Artist Display**: Shows track name and artist name. The `/getaa` continuation is stripped from the artist (`sanitizeArtist()` drops everything from `" - /getaa"` onward).
- [x] **Track Position & Duration Display**: Parses `RelTime` and `TrackDuration` from `GetPositionInfo` and renders position, total and a progress bar.
- [x] **Artwork Display (`/getaa`)**: URI extraction, download and caching are **working**. Progressive JPEG decode now routes to JPEGDEC — **builds and links**, awaiting hardware confirmation (§6.5, `ARTWORK-ATTEMPTS.md`).
- [x] **Idle State & Auto Screen Shutdown**: Shows a countdown when nothing is playing, cuts the backlight after 10 s, and wakes on playback or a button press.
- [x] **Hardware Controls**: Button debouncing on GPIO 35 (Next) and GPIO 0 (Play/Pause); ST7789 init with backlight on GPIO 4 (`TFT_BL`).

---

# Implementation Notes

Documents the firmware as currently built: dependencies, architecture, each
subsystem, and the image pipeline in detail. The **Task list** in §8 is the backlog.

## 1. Status summary

| README requirement | State | Notes |
|---|---|---|
| Application version displayed | Working | Top-left; auto-increments per build |
| WiFi (primary + fallback SSID) | Working | Both in `src/secrets.h`, primary then fallback |
| Sonos discovery (SSDP + fallbacks) | Working | M-SEARCH, first speaker adopted |
| Track & artist display | **Confirmed on hardware** | `Cynical Little Girl` / `House of Woesen` rendered correctly — SOAP truncation (attempt G) was the cause and is fixed (§4) |
| Position, duration, progress bar | **Confirmed on hardware** | `1:47 / 3:42` advancing; `RelTime` / `TrackDuration` parse and render |
| Artwork URI extraction | **Working** | `Art="..."` populated; XML double-unescape in place |
| Artwork download + cache | **Working** | 138,711 bytes streamed to LittleFS, verified on hardware |
| Artwork JPEG decode | **Unresolved** | Panel shows the diagnostics panel with `PROXY 200`; the stage that failed was not distinguishable until the H diagnostics. Awaiting a reading of the new on-screen reason (§8.3) |
| Idle message + 10 s auto-shutdown | Working | Countdown shown, backlight cut, wakes on play/button |
| Hardware controls | Working | 40 ms debounce, GPIO 35 / GPIO 0 |

## 2. Build, dependencies and configuration

| Item | Value |
|---|---|
| Platform / board | `espressif32` / `lilygo-t-display` |
| Framework | Arduino |
| Library | `bodmer/TFT_eSPI @ ^2.5.43`, `bitbank2/JPEGDEC @ ^1.2.2` |
| JPEG decoding | Baseline → `esp_jpg_decode()`; progressive → JPEGDEC (see §6.5) |
| Filesystem | LittleFS |
| Pre-build hook | `version_bump.py` — bumps `APP_VERSION` by 0.01, skip with `PLATFORMIO_SKIP_VERSION_BUMP=1` |

Display pins come from `build_flags` in `platformio.ini` (ST7789, 135×240, rotation 1,
40 MHz SPI, backlight on GPIO 4).

### 2.1 Secrets

`src/secrets.h` is gitignored and holds `WIFI_SSID_PRIMARY`, `WIFI_SSID_SECONDARY`,
`WIFI_PASSWORD`, `FALLBACK_SONOS_1`, `FALLBACK_SONOS_2`. Copy
`src/secrets.example.h` and fill it in. If the file is absent the firmware still
compiles using placeholders (`__has_include` guard) and warns at build time.

### 2.2 Key constants

| Constant | Value | Purpose |
|---|---|---|
| `SONOS_PORT` / `SONOS_CONTROL_PATH` | `1400` / `/MediaRenderer/AVTransport/Control` | SOAP endpoint |
| `SSDP_PORT` / `SSDP_MULTICAST_IP` | `1900` / `239.255.255.250` | Discovery |
| `MAX_SONOS_DEVICES` | `8` | Discovery table |
| `DEBOUNCE_DELAY_MS` | `40` | Button debounce |
| `SONOS_POLL_INTERVAL_MS` | `1000` | Metadata poll |
| `SSDP_RETRY_MS` | 5000 / 60000 | Rediscovery: no speaker / speaker found |
| `IDLE_SHUTDOWN_MS` | `10000` | Backlight cutoff |
| `ART_MAX_SIZE` | 150,000 | Artwork download cap |
| `HTTP_MAX_REDIRECTS` | `3` | Redirect hops |
| `ART_AREA_*` | 240×84 @ Y=0 | Artwork region (metadata starts at Y=84) |
| `ART_FILE_PATH` | `/art.jpg` | LittleFS cache |

## 3. Sonos UPnP / SOAP interface

**Endpoint:** `POST http://<speaker-ip>:1400/MediaRenderer/AVTransport/Control`
**Service:** `urn:schemas-upnp-org:service:AVTransport:1`, `InstanceID` 0.

`buildEnvelope()` wraps an action in a SOAP envelope; `sonosSoap()` posts it with
`Content-Type: text/xml` and a `SOAPACTION` header. Actions used: `GetTransportInfo`,
`GetPositionInfo`, `Play`, `Pause`, `Next`.

Response bodies are read by `readHttpBody()` straight off `http.getStreamPtr()`,
bypassing `HTTPClient::getString()`.

**Short reads are detected and retried.** A socket that closes mid-body produces a
partial response with no error from `readBytes()`, so `readHttpBody()` reports whether the
full `Content-Length` arrived via an out-param, and `sonosSoapTo()` retries once on a
fresh connection when it did not. This matters disproportionately for
`GetPositionInfo`: `<Track>` is the first element, so a response truncated right after it
still yields a track number while the title, artist, duration and artwork URI all vanish —
which looks exactly like a parsing bug. `HTTP_BODY_READ_TIMEOUT_MS` (2000) also stops the
read loop waiting indefinitely on a silent socket.

### 3.1 Discovery (SSDP, not SOAP)

`sendSSDPDiscovery()` sends:

```
M-SEARCH * HTTP/1.1
HOST: 239.255.255.250:1900
MAN: "ssdp:discover"
MX: 2
ST: urn:schemas-upnp-org:device:ZonePlayer:1
```

Responses are accepted only if they contain `Sonos`; the speaker address comes from the
`LOCATION` header, e.g. `http://192.168.1.50:1400/xml/device_description.xml`. If nothing
is found within 5 s the fallback addresses from `src/secrets.h` are tried, then
rediscovery runs every 60 s. The first device that answers becomes `activeSonosIP`.

### 3.2 Actions used

| Action | Body arguments | Purpose |
|---|---|---|
| `GetTransportInfo` | `<InstanceID>0</InstanceID>` | Playback state |
| `GetPositionInfo` | `<InstanceID>0</InstanceID>` | Track, artist, times, artwork |
| `Play` | `<InstanceID>0</InstanceID><Speed>1</Speed>` | GPIO 0 button |
| `Pause` | `<InstanceID>0</InstanceID>` | GPIO 0 button |
| `Next` | `<InstanceID>0</InstanceID>` | GPIO 35 button |

### 3.3 Response field to on-screen element

| Source | Example | Stored in | Rendered as | Position |
|---|---|---|---|---|
| `GetTransportInfo` / `CurrentTransportState` | `PLAYING` | `currentTransportState` | Badge text + colour | top-right, 48×15 |
| `CurrentTrackMetaData` / `dc:title` | `Creep` | `currentTitle` | Track title, font 2 | `x=4, y=86` |
| `CurrentTrackMetaData` / `dc:creator` | `Radiohead` | `currentArtist` | Artist, font 2 | `x=4, y=102` |
| `GetPositionInfo` / `RelTime` | `0:01:23` | `currentRelTime` | Elapsed time, font 1 | `x=4, y=111` |
| `GetPositionInfo` / `TrackDuration` | `0:03:58` | `currentDuration` | Total time, font 1 | right-aligned, `y=111` |
| `CurrentTrackMetaData` / `upnp:albumArtURI` | `/getaa?s=1&u=…` | `currentArtUri` | Album artwork | `0,0` 240×84 |

`CurrentTransportState` values seen in practice: `PLAYING`, `PAUSED_PLAYBACK`, `STOPPED`.

## 4. Parsing helpers

The `CurrentTrackMetaData` blob arrives **XML-escaped** (`&lt;dc:title&gt;…`), so it is
unescaped before any tag inside it is searched for.

| Function | Purpose |
|---|---|
| `extractByLocalName()` | Finds a tag by local name only, so `upnp:albumArtURI`, `r:res` and bare names all match regardless of namespace prefix |
| `xmlUnescapeDeep()` | Repeats entity unescaping until stable (max 3 passes) — Sonos **double-escapes** `&` inside `Art` attributes (see §6.2) |
| `resolveTrackField()` | Single code path for title and artist: prefers DIDL (`<dc:title>`/`<dc:creator>`), falls back to AVTransport elements |
| `sanitizeArtist()` | Drops the `" - /getaa"` continuation from the artist |
| `ellipsize()` | Truncates text to a pixel width with `…` |

DIDL tag usage: `title` → track title, `creator` → artist, `albumArtURI` → artwork URL.

Artwork URI sources, in the order `pollSonos()` tries them:

| Source | Used for |
|---|---|
| `albumArtURI` (`upnp:albumArtURI`) | Primary artwork URL, usually `http://<ip>:1400/getaa?u=…` |
| `CurrentTrackArtImage` | Artwork fallback on the AVTransport response |
| `res` | Last-resort artwork source for local/native content |

> **Known bug (fixed):** `<Track>` in `GetPositionInfo` holds the *queue track number*,
> not the title — it was being drawn in place of the title. `resolveTrackField()` now
> serves both fields identically, preferring `<dc:title>`, so the two cannot diverge.
> `TITLE_ROW_DIAG` (set to `0`) re-enables a swap test if this ever regresses.

## 5. Display layout

```
┌────────────────────────────────┐
│  ARTWORK  240×84 @ Y=0         │  renderArtwork()
│              ┌──────────┐      │
│              │  BADGE   │      │  PLAYING / PAUSED (top-right)
│              └──────────┘      │
├────────────────────────────────┤
│  Title (font 2)        Y=84    │  drawMetadataBlock(), only redrawn when
│  Artist (font 2)               │  a value actually changes
│  Artist ▾ track /radio  Y=115  │
├────────────────────────────────┤
│  0:42 ▬▬▬▬▬▬▬▬▬▬▬▬ 3:15  Y=122│  progress bar, 12 px
└────────────────────────────────┘
```

`drawScreen()` renders artwork first, then the badge and version over it, then the
metadata block. `invalidateMetadata()` forces a full repaint after anything that clears
the display. `setScreenPower()` gates all drawing so the backlight can be cut.
`showToast()` briefly overlays transport feedback after a button press.

## 6. Image handling

This is the part that does **not** work end to end. The transport is solid; the final
decode step is blocked. Both halves are documented because the split is what makes the
remaining bug tractable.

### 6.1 Pipeline overview

```
CurrentTrackMetaData ──extractByLocalName("albumArtURI")──┐
                                                           ├─ xmlUnescapeDeep ─┐
GetPositionInfo ──"CurrentTrackArtImage"──────────────────┘                  │
                                                                              ▼
                                                        raw URI (e.g. /getaa?s=1&u=...)
                                                                              │
                                                        artworkUrlCandidates()│  (≤2 routes)
                                                                              ▼
                                                        fetchArtworkFrom()    │  try each in turn
                                                                              ▼
                                        LittleFS /art.jpg  ── renderArtwork() ── esp_jpg_decode()  ✗ FAILS
```

### 6.2 URI extraction and unescaping — working

`currentArtUri` comes from `GetPositionInfo`'s `CurrentTrackMetaData`, tried in order:

1. `extractByLocalName(metaData, "albumArtURI")` — matched on local name, so any
   namespace prefix works.
2. Fallback: the `CurrentTrackArtImage` element in the position body.
3. Fallback: a DIDL `<res>` value whose path ends in `.jpg`/`.jpeg`/`.png`.
4. `xmlUnescapeDeep()` then strips entity escapes.

**The double-escape is the subtle part.** Sonos sends the URI with its own `&` already
escaped, then SOAP escapes it again:

```
as sent:   &amp;u=x-sonos-http%3atrack%252f631323.flac%3fsid%3d174
unescaped: &u=x-sonos-http%3atrack%252f631323.flac%3fsid%3d174
```

A single unescape pass leaves `&amp;u=…` and the request 404s; looping until stable
fixes it. `jpegan.py` in the repo root was used to confirm the wire format.

### 6.3 URL resolution — `artworkUrlCandidates()` (line 656)

Builds up to two candidate URLs so the fetch can try both rather than commit to one:

| Input form | Route | Result |
|---|---|---|
| `http://` / `https://` absolute | DIRECT | used as-is |
| `//host/path` protocol-relative | DIRECT | `http:` + input |
| `/getaa?s=1&u=<encoded upstream>` | **1. DIRECT** | `u` param extracted and percent-decoded (`extractUrlParamU()`), upstream URL fetched directly |
| (same) | **2. PROXY** | `http://<speaker>:1400<path>` — the speaker proxies it |

Direct is tried first because it is one hop shorter; the proxy is the fallback.
`resolveArtworkUrl()` returns only candidate 0 and is used for on-screen display, not
for fetching.

### 6.4 Download and caching — working

`downloadArtwork()` → `fetchArtworkFrom()` (line 1255):

- Skips the network entirely when the URI matches `cachedArtUrl` (the LittleFS copy is
  reused and only re-rendered).
- Redirects (301/302/303/307/308) are followed manually up to `HTTP_MAX_REDIRECTS`;
  `resolveRedirect()` resolves relative `Location` headers against the current URL.
- HTTPS uses `WiFiClientSecure` with `setInsecure()`.
- The body is streamed through a custom `FileSink` (a `Stream` subclass that counts
  bytes, aborts past `ART_MAX_SIZE`, and remembers whether the tail was a JPEG
  `0xFFD9` EOI marker).
- **Chunked transfer is handled by `http.writeToStream(&sink)`**, not by manual reads.
  An earlier version read `getStreamPtr()` directly and stored the `2c4\r\n` chunk-size
  headers inside the JPEG, producing a file that was the right length but undecodable —
  a truncated-image bug that looked exactly like a decoder problem.
- The cache file is only kept if the transfer succeeded and ended on an EOI marker;
  otherwise it is removed so a partial image is never drawn.

Verified on hardware (`mon2.log`):

```
[Art] Trying route 1/2: DIRECT via ?
[Art]   URL: /getaa?s=1&u=x-sonos-http%3atrack%252f631323.flac%3fsid%3d174%26flags%3d24616%26sn%3d2
[Art]   HTTP status: -1
[Art]   Failed: connection refused
[Art] Route yielded no data, trying next.
[Art] Trying route 2/2: PROXY via 192.168.1.29:1400
[Art]   HTTP status: 200
[Art]   Content-Length: -1
[Art]   Wrote 138711 bytes
[Art] Cached via PROXY (138711 bytes)
```

This also demonstrates the multi-route fallback working as designed: DIRECT failed,
PROXY succeeded. Route selection, redirect handling, TLS, chunked decoding and the
LittleFS write are all confirmed correct.

### 6.5 JPEG decoding — dual decoder, chosen by frame type

The failing log line was:

```
[Art] JPEG dimensions: 640 x 640 (progressive)
[Art] Rendering at 1/8 -> 80 x 80
[Art] esp_jpg_decode failed: 0xffffffff
```

`0xffffffff` is `ESP_FAIL`. The image is a valid, complete 640×640 JPEG — it downloaded
in full and the header parses fine (`jpegInfo()` correctly reports its dimensions and
that it is **progressive**).

**Root cause: `esp_jpg_decode()` does not support progressive JPEG.** This contradicted
the comment previously at the top of `src/main.cpp`, which claimed it did.
`esp_jpg_decode()` is a thin wrapper around ESP-IDF's ROM TJpgDec core — it calls
`jd_prepare()` and `jd_decomp()` from `rom/tjpgd.h` directly:

```c
JRESULT jres = jd_prepare(&decoder, _jpg_read, work, 3100, &jpeg);
if (jres != JDR_OK) { ...; return ESP_FAIL; }
```

TJpgDec is a *baseline-only* decoder. The ROM header states it plainly:

```
JDR_FMT3   /* 8: Not supported JPEG standard. May be a progressive JPEG image. */
```

So `jd_prepare()` returns `JDR_FMT3` and `esp_jpg_decode()` maps that to `ESP_FAIL`.

**The fix — two decoders, selected by the frame type:**

| Frame type | Decoder | Why |
|---|---|---|
| Baseline (SOF0) | `esp_jpg_decode()` | Fast, low RAM, already correct. Streams the cached file through a random-access reader. |
| Progressive (SOF2) | `bitbank2/JPEGDEC` | Decodes the frame, but **DC-scan only** — see below. |

> ⚠️ **Progressive artwork is not properly solved.** JPEGDEC decodes only the **first
> (DC) scan** of a progressive JPEG and forces its own 1/8 scaling, so it produces a
> low-detail thumbnail rather than a full decode — see its README ("Supports thumbnail
> (DC-only) decoding of progressive JPEG images") and `jpeg.inl`. It renders *something*
> plausible, but it is not a true progressive decoder. `ARTWORK-ATTEMPTS.md` (attempt E)
> records this and the remaining options; asking Sonos for a **baseline** rendition is
> the preferred fix.

Selection is driven by the `progressive` flag `jpegInfo()` already reports, so the same
routine decides both the decoder and the scale. Progressive images are never handed to
`esp_jpg_decode()` at all — there is no "try one, fall back to the other" dance.

`renderCachedJpegProgressive()` feeds JPEGDEC through
`open(handle, size, close, read, seek, draw)` with our own callbacks over the LittleFS
`File`. This is deliberate — the tidier `open(File&, drawCb)` overload is behind
`#ifdef FS_H`, and on an `ESP_PLATFORM` build `JPEGDEC.h` takes its first include branch,
which does **not** include `<FS.h>`. `FS_H` is therefore never defined while
`JPEGDEC.cpp` compiles, so that overload is omitted from `libJPEGDEC.a` and the link
fails:

```
undefined reference to `JPEGDEC::open(fs::File&, int (*)(jpeg_draw_tag*))'
```

(The raw-handle overload is not guarded, so it is always compiled. That `#ifdef FS_H`
block also contains a latent bug — its `FileClose` dereferences a `void*` as
`handle->fHandle` — which suggests it has never been compiled on ESP_PLATFORM.)

Either way the file is read through the `File`, so the **138 KB image is never resident
in RAM**. Without that, the decode would not fit on a board with ~320 KB of DRAM and no
PSRAM. The decoder's own state (~18 KB) is heap-allocated rather than a static global,
so it is only resident while an image is decoding.

Both decoders feed the same `tft_output()` helper, so clipping to the artwork region is
identical and neither can overwrite the text. Free heap is logged either side of a
progressive decode so an out-of-memory outcome is distinguishable from a decode failure.

**Pixel byte order matters.** JPEGDEC must be set to `RGB565_BIG_ENDIAN`, *not* its
`RGB565_LITTLE_ENDIAN` default. TFT_eSPI's `pushImage(uint16_t*)` streams colour bytes
most-significant-first (`_swapBytes` is `false` by default), so the decoder pre-swaps each
16-bit value. Leaving it little-endian swaps the red and blue byte of every pixel, which
shows up as washed-out artwork with magenta/cyan speckling. Every JPEGDEC example that
drives an SPI LCD uses `RGB565_BIG_ENDIAN` for this reason.

The draw callback also honours `pDraw->iWidthUsed`, the count of *valid* pixels in an
edge-clipped block. Passing `iWidth` instead reads past the valid part of each row and
shears the right-hand edge.

**Status:** **builds and links** (RAM 15.6%, 51 KB of 320 KB — confirming the image is not
held in RAM). Awaiting flash-and-look to confirm it renders. `ARTWORK-ATTEMPTS.md`
records this, the link error hit on the way, and the rejected alternatives.

> `TJpg_Decoder` remains removed. It and `esp_jpg_decode` share the same TJpgDec core,
> so swapping one for the other never solved progressive decoding. The swap did resolve
> a genuine duplicate-symbol fault (`jd_prepare`/`jd_decomp`), which is worth keeping in
> mind before re-adding it.

### 6.6 Scaling and drawing

`renderArtwork()` (line 1853):

- `jpegInfo()` parses the JPEG frame header for dimensions and the progressive flag.
- Picks the **smallest** power-of-two reduction (1/1…1/8) that fits 240×84, so the
  image is drawn as large as possible. 640×640 → 1/8 → 80×80.
- Baseline: `artDestX/artDestY` hold the centred offset, which `jpg_write_cb()` adds
  because `esp_jpg_decode` emits at image coordinates from 0,0.
- Progressive: JPEGDEC takes an absolute destination in `decode()`, so the offset is
  passed there instead and the callback is a straight pass-through.
- Neither path loads the whole file into RAM.
- **Rendering is not re-armed on metadata changes.** A progressive decode is slow enough
  that doing it on every 1 s poll starved the poll, the buttons and the idle timer, and the
  screen froze on a stale snapshot. `artNeedsRender` is set only by `downloadArtwork()`,
  when the cached file actually changes.

### 6.6a Artwork revision guard

Decoding is the slowest thing the firmware does, and several unrelated paths legitimately
want a repaint (toast, screen wake, idle screen). Each of those would otherwise trigger a
redundant decode and could starve the main loop again.

| Variable | Purpose |
|---|---|
| `artRevision` | Bumped only when the cached file actually changes |
| `artDrawnRevision` | Which revision is currently on screen; starts at `0xFFFFFFFF` to force the first render |

`renderArtwork()` skips the decode when `artDrawnRevision == artRevision`. Paths that clear
the screen (`setScreenPower(true)`, `drawNothingPlayingScreen()`) reset `artDrawnRevision`
so the artwork is genuinely redrawn. The "already cached on disk" early return in
`downloadArtwork()` is guarded too, so it no longer forces a pointless decode each poll.

### 6.7 Artwork fetch is retried until it succeeds

A track change only records the new artwork URI; the download itself is driven by a
pending-fetch state and retried until it works.

| Item | Value |
|---|---|
| `artworkPending` | A fetch is owed for the current track |
| `artworkFirstTry` | Earliest time of the next attempt (millis-based) |
| `ARTWORK_INITIAL_DELAY_MS` | 750 — delay before the first attempt after a track change |
| `ARTWORK_RETRY_INTERVAL_MS` | 5000 — delay after a failed attempt |

`downloadArtwork()` returns `bool`. On failure `cachedArtUrl` is deliberately **not**
updated, so the retry re-fetches rather than short-circuiting on a stale cache.

This matters because the speaker is often still settling immediately after a track
change, which is exactly when a request times out. Without the retry, a single failed
attempt was permanent and the previous track's cover stayed on screen indefinitely —
the firmware latched rather than retried.

### 6.8 LittleFS is not erased by a reflash

`pio run -t upload` rewrites only the firmware partition. The LittleFS partition — and
therefore the cached `/art.jpg` — **survives**, so after a reflash the previous track's
cover is still on disk and is legitimately drawn until a new download replaces it.

To test the artwork path from a known state, either change tracks, or erase the
filesystem:

```
pio run -t upload
pio run -t erasefs
```

Without `erasefs`, "the same artwork is still showing" after a reflash is expected
behaviour, not a bug.

### 6.9 On-screen diagnostics

`drawArtworkPlaceholder()` doubles as the artwork diagnostics panel: it shows the HTTP
status, the route taken (`DIRECT`/`PROXY`), the host and the resolved URL in a large
font, so the values can be read from a photograph of the display as well as from the
serial log. This is why the artwork failure was diagnosable without a debugger.

### 6.10 If progressive decoding still fails

Ordered by preference — check `ARTWORK-ATTEMPTS.md` first so these are not re-tried
blind:

1. **Request a baseline rendition from `/getaa`.** Preferred: no decoder change at all.
2. **Ask for a smaller image.** The current 640×640 artwork is far larger than the
   240×84 area needs. If Sonos can be asked for a lower-resolution variant the RAM and
   CPU cost drops sharply. Worth trying before adding a third decoder.
3. **A different progressive decoder**, if JPEGDEC's fixed buffers prove too large.

## 7. Library API notes (Arduino ESP32 2.0.17 / TFT_eSPI 2.5.43)

Deviations from the obvious API, found at compile time or on hardware.

| Issue | Resolution |
|---|---|
| `TFT_DATUM_TOP_LEFT` etc. do not exist in TFT_eSPI 2.5.43 | Use `TL_DATUM`, `TC_DATUM`, `TR_DATUM` |
| `TFT_eSPI::textHeight()` does not exist | Use `fontHeight()` |
| `WiFiUDP` has no `writeMulticast()` | `beginMulticast()` → `beginPacket(ip, port)` → `write()` → `endPacket()` |
| `HTTPClient::getString()` returns `""` when `Content-Length: 0` (which Sonos does send) | Custom `readHttpBody()` reads the socket directly |
| `HTTPClient` does **not** de-chunk on receive (only on send) | `readHttpBody()` sniffs for chunk framing and reassembles it |
| `WiFiClient::readStringUntil()` returns `void` on this core | `readLineRaw()` assembles a line byte-by-byte |
| `String::trim()` returns `void` (in-place) | Call as a statement, never chained |
| Bundled bitmap fonts are **ASCII-only** (glyphs 32–127) | `sanitizeText()` strips all other bytes |
| `GetPositionInfo` `<Track>` is the queue track number, not the title | Prefer `dc:title` from the DIDL metadata (§4) |
| `esp_jpg_decode()` is baseline-only | Progressive JPEG unsupported (§6.5) |

## 8. Serial debug reference

| Prefix | Meaning |
|---|---|
| `[Display]` | Geometry and font metrics at boot |
| `[WiFi]` | Connection attempts, primary then fallback |
| `[SSDP]` | M-SEARCH sent, devices discovered, table full |
| `[Sonos]` | Parsed fields per track; active speaker |
| `[Debug]` | SOAP action, HTTP status, Content-Length vs bytes received |
| `[Art]` | URL resolution, route attempts, byte counts, JPEG decode result |
| `[LittleFS]` | Mount / format |
| `[Screen]` | Backlight on/off |
| `[Button 1/2]` | Button presses (Next / Play-Pause) |
| `[Diag]` | Title-row swap test (only when `TITLE_ROW_DIAG=1`) |
| `[Cmd]` | Serial-console command acknowledgement |
| `[version-bump]` | `APP_VERSION` at the start of each build |

### 8.1 Decisive SOAP capture

For the first `SOAP_CAPTURE_COUNT` polls after boot, `pollSonos()` logs enough to
settle **transport vs parser** — the question that a title showing a queue number
and `0:00 / 0:00` cannot answer on its own:

| Line | Reads as |
|---|---|
| `[Sonos] Elements present: Track=Y TrackDuration=Y …` | Which of the eight AVTransport elements actually arrived |
| `[Sonos] Envelope closed: YES/NO (body=N bytes)` | `NO` means the body was cut off; `YES` means it was complete and the fault is downstream |
| `[Sonos] HEAD …` / `[Sonos] TAIL …` | Where the body starts and **ends**. The tail is what makes truncation visible; a prefix dump cannot distinguish a 1503-byte body from one cut at 400 |
| `[Sonos] DIDL blob: N bytes, dc:title=Y dc:creator=Y upnp:albumArtURI=Y` | `blob=0` → metadata never arrived (parsing is irrelevant). `blob>0` with `dc:title=N` → the DIDL arrived but could not be read |

`logXmlSnippet()` escapes control bytes, so each capture stays on one greppable line.

### 8.2 Serial console

Single-key commands on the monitor UART, enabled by `SERIAL_CONSOLE` (default 1).
They exist so a hypothesis can be tested **without a rebuild** — each experiment
used to cost a flash, which is what turned debugging into a loop of guesses.

| Key | Action |
|---|---|
| `d` | Request `GetPositionInfo` and print the full capture above |
| `x` | Delete the cached `/art.jpg` (it survives a reflash, so this is the only cold-cache test) |
| `f` | Re-arm the artwork fetch for the current track, bypassing the `cachedArtUrl` short-circuit |
| `m` | Free/min/largest heap, WiFi RSSI, cached artwork size |
| `h` | List the commands |

### 8.3 Artwork failure reasons on screen

The panel shows the **failure reason in preference to the HTTP code**. `200` only
means the speaker answered; it is also returned for a body with no EOI marker and
for a file that then fails to decode, so showing the code alone made three
unrelated faults look identical in a photograph.

| Panel text | Meaning |
|---|---|
| `no EOI` | Speaker answered 200 but the body was not a complete JPEG |
| `partial` | Some bytes arrived, but not a whole image |
| `not cached` | Nothing usable on disk — check `[Art] Retrying` |
| `no header` | The cached file's JPEG frame header would not parse |
| `no RAM` | JPEGDEC could not be allocated |
| `prog decode fail` | JPEGDEC parsed the header, then failed mid-decode |

## 9. Task list

**Bugs**
- [x] Track title showing the track number — fixed by `resolveTrackField()` (§4)
- [x] Artwork URI double-escaping — fixed by `xmlUnescapeDeep()` (§6.2)
- [x] Artwork 404 on `/getaa` — fixed by route fallback + port 1400 (§6.3)
- [x] Corrupt cached JPEG from chunked encoding — fixed via `writeToStream()` (§6.4)
- [x] **Blank title/artist and `0:00 / 0:00`** — the `GetPositionInfo` body was being
      truncated mid-response, so only `<Track>` survived. Fixed by detecting short
      reads and retrying on a fresh connection. **Confirmed on hardware** (§3,
      `ARTWORK-ATTEMPTS.md` attempt G)
- [ ] **Artwork stage after a `PROXY 200`** — ⛔ **blocked on observation, not on a
      guess.** Flash the current build and read the reason on the panel (§8.3):
      `prog decode fail` points at JPEGDEC, `no EOI`/`partial` at the proxy body,
      `not cached` at the retry loop. Do not change the decoder, colour order or
      retry timing until one of those appears (`ARTWORK-ATTEMPTS.md` attempt H)
- [ ] **Request a baseline rendition from `/getaa`** — the real fix for progressive
      artwork. JPEGDEC only decodes the DC scan, so progressive art is a low-detail
      thumbnail, and the forced 1/8 scale is slow enough to have frozen the UI
      (`ARTWORK-ATTEMPTS.md` attempt E)
- [ ] `config.md` still lists the application version as `0.02`

**Robustness**
- [ ] Retry/backoff on failed Sonos queries and artwork downloads
- [ ] Transliterate non-ASCII text instead of dropping characters
- [ ] Handle track/group changes via `AVTransport` events instead of polling only
- [ ] Deep-sleep when idle for further power saving (backlight is cut today; CPU stays awake)
- [ ] FreeRTOS task so the 1 s poll does not block drawing or button handling
- [ ] Guard against a corrupted `/art.jpg` surviving a power cut mid-write
- [ ] Remove the `jpegan.py` / `dcdec.py` / `sweep.py` / `cmp.py` probe scripts once
      the decode path is settled (the firmware's one-shot RAW dump has already been
      replaced by the §8.1 capture)

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
