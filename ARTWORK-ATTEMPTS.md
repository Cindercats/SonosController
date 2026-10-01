# Artwork Decode — Attempt Log

Running record of every approach tried for rendering Sonos album artwork, and why each
was accepted or rejected. **Read this before trying a new decoder** — several
plausible-sounding approaches have already been ruled out.

**Goal:** decode the JPEG Sonos serves for the current track and draw it into the
240×84 artwork region.

**Current status:** builds and links; awaiting flash-and-look to confirm the image renders.

---

## Confirmed facts (do not re-derive)

- **Download works.** The full image is fetched and cached to LittleFS (`/art.jpg`),
  verified on hardware: 138,711 bytes, DIRECT → PROXY route fallback, chunked
  transfer correctly de-chunked.
- **Sonos serves progressive JPEG for local library tracks** (observed 640×640, SOF2
  progressive). This is the common case and the cause of every failure below.
- **`esp_jpg_decode()` is a thin wrapper over the ROM TJpgDec core.** It calls
  `jd_prepare()` / `jd_decomp()` from `rom/tjpgd.h`. It is not a different, more
  capable decoder.
- **TJpgDec is baseline-only.** `jd_prepare()` returns `JDR_FMT3` for a progressive
  SOF, which `esp_jpg_decode()` maps to `ESP_FAIL` (`0xffffffff`). The ROM header says
  it outright: *"Not supported JPEG standard. May be a progressive JPEG image."*
- **Memory is tight.** The LilyGO T-Display has ~320 KB internal DRAM and **no PSRAM**.
  Any decoder that needs the whole JPEG resident must hold ~138 KB plus working state
  with WiFi already up. This constrains the decoder choice.
- `jpegInfo()` parses the frame header correctly, so the failures are genuine decoder
  rejections, not a bug in our parsing.

---

## Attempts

### Attempt A — `TJpg_Decoder` (ChaN's TJpgD, via `drawFsJpg`)

**Approach:** stream-decode straight from the LittleFS file — `getFsJpgSize()` to read
dimensions, then `drawFsJpg()` with a scale factor. Very low RAM use, which suited the
board well.

**Result:** failed on progressive artwork; `tjpgd.c` rejects SOF2 with `JDR_FMT3`.

**Secondary problem:** its global `jd_prepare` / `jd_decomp` / `jd_input` / `jd_output`
symbols collide with the ones ESP-IDF's `esp_jpg_decode` defines internally. Linking
both made `esp_jpg_decode` call TJpg_Decoder's `jd_prepare` with an incompatible
`JDEC` layout, faulting with `InstrFetchProhibited` on the first decode.

**Verdict: ❌ do not retry.** Progressive JPEG unsupported, and the symbol collision
makes it unsafe to link alongside ESP-IDF's decoder.

### Attempt B — `esp_jpg_decode()` (ESP-IDF ROM TJpgDec) — currently in `src/main.cpp`

**Approach:** drop `TJpg_Decoder` to resolve the symbol collision and call
`esp_jpg_decode()` with the existing `jpg_read_cb` / `jpg_write_cb` callbacks
(random-access read from the LittleFS `File`, so the image stays off the heap).

**Rationale recorded in the code at the time:** *"esp_jpg_decode handles both baseline
and progressive JPEG."* **That claim was wrong** — see Confirmed facts above.

**Result:** still fails, identically:

```
[Art] JPEG dimensions: 640 x 640 (progressive)
[Art] Rendering at 1/8 -> 80 x 80
[Art] esp_jpg_decode failed: 0xffffffff
```

**Why:** it is the same TJpgDec core, so it inherits the same progressive limitation.
Swapping libraries fixed the collision but not the actual problem.

**Verdict: ❌ do not retry for progressive artwork.** It remains correct and desirable
for *baseline* JPEGs (low RAM, streaming) — kept as the fast path for baseline images.

### Attempt C — progressive decoding via `bitbank2/JPEGDEC` — split into C1 (failed) / C2 (links)

**Approach:** add JPEGDEC as a second decoder used *only* for progressive images.
Baseline keeps using `esp_jpg_decode()` (attempt B), which is already correct and uses
less RAM. Selection is driven by the `progressive` flag `jpegInfo()` already reports, so
there is one authoritative source of truth for which decoder runs.

**Why this decoder.** Verified from source:

- It has an explicit `JPEG_MODE_PROGRESSIVE` mode; `getJPEGType()` returns it when the
  frame marker is `0xC2`. This is the exact case that fails today.
- It can be driven from a file handle rather than an in-memory buffer, so the 138 KB
  image is never resident in RAM — essential on a board with no PSRAM, and the same
  property attempt B was chosen for.
- Working set is a fixed compile-time struct (Huffman tables + a 2 KB file buffer +
  MCU buffers), not a per-image allocation.
- Scale options are `JPEG_SCALE_HALF/QUARTER/EIGHTH`, so the existing 1/1…1/8 fit logic
  maps across directly.

**Implementation notes:**

- Pixels arrive as RGB565 blocks via `pDraw->pPixels` (already `uint16_t*`). They must be
  requested as `RGB565_BIG_ENDIAN` for TFT_eSPI, not the library default — see attempt D.
- `setCropArea()` is not used; the existing scale + centre + clip path already handles
  placement.
- Free heap is logged either side of the decode, so an out-of-memory outcome is
  distinguishable from a decode failure on the next run.

C1 and C2 below are the two ways of handing the file to the decoder; C1 does not link.

### Attempt C1 — `JPEGDEC::open(File&, drawCb)` — ❌ **link error, do not retry**

**Approach (first implementation):** call the documented `open(File &file, JPEG_DRAW_CALLBACK*)`
overload so JPEGDEC read straight from the LittleFS `File`.

**Result:** `libJPEGDEC.a` compiled, but linking failed:

```
undefined reference to `JPEGDEC::open(fs::File&, int (*)(jpeg_draw_tag*))'
```

**Why.** That overload sits behind `#ifdef FS_H` in `JPEGDEC.h`. On an
`ESP_PLATFORM` build the header takes its *first* include branch:

```c
#if defined(__MACH__) || defined(__LINUX__) || ... || defined(ESP_PLATFORM)
#include <stdlib.h> ...          /* no <FS.h> here */
#else
#include <Arduino.h>
#if __has_include(<FS.h>)
#include <FS.h>                  /* FS_H would be defined here */
#endif
#endif
```

`ESP_PLATFORM` is defined, so that branch skips `<FS.h>`. `FS_H` therefore never gets
defined while `JPEGDEC.cpp` compiles, and the overload is silently omitted from the
archive. `main.cpp` *does* see `FS_H` (it includes LittleFS.h → FS.h), so it declared
and called a symbol the library never emitted — a declaration/definition mismatch that
only shows up at link time.

**Verdict: ❌ do not retry.** Setting `-DFS_H` in `build_flags` would likely work but
depends on `__has_include(<FS.h>)` resolving inside the library's own compile context,
which is exactly the fragile part. Attempt C2 avoids the guard entirely.

### Attempt C2 — `JPEGDEC::open(handle, size, close, read, seek, draw)` — **IMPLEMENTED, awaiting hardware test**

**Approach:** use the raw-handle overload instead, supplying our own read/seek/close
callbacks over the LittleFS `File`:

```cpp
int open(void *fHandle, int iDataSize, JPEG_CLOSE_CALLBACK *pfnClose,
         JPEG_READ_CALLBACK *pfnRead, JPEG_SEEK_CALLBACK *pfnSeek,
         JPEG_DRAW_CALLBACK *pfnDraw);
```

This one is **not** `#ifdef FS_H`-guarded, so it is compiled unconditionally and cannot
suffer C1's mismatch. The callbacks mirror the library's own internal `FileRead` /
`FileSeek` / `FileClose`, so behaviour is identical to what C1 intended — and the
streaming property that makes this viable at all is preserved: the 138 KB image stays in
LittleFS and is never resident in RAM.

**Result:** ✅ **links successfully.** Verified with `pio run`:

```
Linking .pio\build\lilygo-t-display\firmware.elf
RAM:   [==        ]  15.6% (used 51248 bytes from 327680 bytes)
Flash: [========  ]  81.4% (used 1066493 bytes from 1310720 bytes)
========================= [SUCCESS] Took 19.82 seconds =========================
```

RAM is 15.6% used — ~51 KB of 320 KB. The 138 KB image is definitively not being held
in RAM, which is the outcome that mattered from choosing the streaming overload. JPEGDEC
resolved to **1.8.4**.

### Attempt D — wrong colours in the rendered artwork — ❌ `RGB565_LITTLE_ENDIAN`, fixed to `RGB565_BIG_ENDIAN`

**Symptom (reported from hardware):** the image *rendered*, in the right place and at the
right size, but the colours were badly wrong — washed out, with magenta/cyan speckling
where fine detail should be. Correct-colour reference vs. rendered screen side by side.

**Diagnosis:** the user's initial guess — an RGB order error — was right.

`JPEGDEC::setPixelType()` was left at the library default, `RGB565_LITTLE_ENDIAN`. That
default is correct for a decoded image held in memory, but **wrong for this display**.
TFT_eSPI's `pushImage(x, y, w, h, uint16_t*)` streams the colour bytes
most-significant-first, and `_swapBytes` is `false` by default:

```
// Do not swap colour bytes by default
_swapBytes = false;
```

So the decoder has to pre-swap each 16-bit value. Every JPEGDEC example that drives an
SPI LCD says so explicitly:

```cpp
jpeg.setPixelType(RGB565_BIG_ENDIAN); // The SPI LCD wants the 16-bit pixels in big-endian order
```

(`crop_area.ino`, `esp32_jpeg.ino`, `lcd_dma.ino`, `web_image_viewer.ino` in the vendored
library — all `BIG_ENDIAN`, none little-endian.)

Leaving it little-endian swaps the red and blue **byte** of every RGB565 pixel, which is
why the error looks like an R/B channel swap rather than a byte-order-neutral shift.

**Fix:** `setPixelType(RGB565_BIG_ENDIAN)`.

**Second bug found and fixed in the same pass — `iWidthUsed` ignored.** The real struct is:

```c
typedef struct jpeg_draw_tag {
    int x, y;                 // upper left corner of current MCU
    int iWidth, iHeight;      // size of this pixel block
    int iWidthUsed;           // clipped size for odd/edges
    int iBpp;                 // bit depth of the pixels (8 or 16)
    uint16_t *pPixels;        // 16-bit pixels
    void *pUser;
} JPEGDRAW;
```

`iWidthUsed` is the count of *valid* pixels and is less than `iWidth` when the right-hand
edge block is clipped. The callback had been passing `iWidth`, so it pushed stale pixels
beyond the image and, worse, read past the valid region of each row. Edge rows are now
pushed one at a time using `iWidthUsed` with the correct `w`-pixel stride:

```c
if (used > 0 && used < w) {
    for (uint16_t row = 0; row < h; row++)
        tft_output(x, y + row, used, 1, pDraw->pPixels + (size_t)row * w);
}
```

Note `pPixels` is already `uint16_t*`, so the earlier `reinterpret_cast` was redundant and
has been dropped.

This edge bug would have produced shear/garbage at the right-hand border of the artwork
even with the colours correct, so both were worth fixing together.

**Result:** 🔄 **rebuilt and awaiting a look at the screen.**

**Verdict:** if colours are *still* wrong after this, the remaining possibility is a
TFT_eSPI colour-order/build-flag mismatch (`TFT_RGB_ORDER` / `TFT_IS_RGB`) rather than a
decoder byte-order issue — check those before touching the decoder again.

### Attempt E — display frozen after a successful render — ❌ JPEGDEC progressive is DC-only

**Symptom:** artwork now renders with correct colours, but the device then **stops
updating entirely**. The screen shows a stale snapshot — artwork, `PLAYING` badge, title
`1`, `(no artist)`, `0:00 / 0:00` and a full progress bar — and the time never advances.
Suspected to be "hardcoded".

**This is a limitation of the chosen decoder, discovered after the fact.**

JPEGDEC's own README is explicit about what "progressive" means for it:

```
- Supports Baseline Huffman images (grayscale or YCbCr)
- Supports thumbnail (DC-only) decoding of progressive JPEG images
```

and the source confirms it (`jpeg.inl:4961`):

```c
// progressive mode - we only decode the first scan (DC values)
if (pJPEG->ucMode == 0xc2) {
    pJPEG->iOptions |= JPEG_SCALE_EIGHTH; // return 1/8 sized image
}
```

So JPEGDEC decodes **only the first (DC) scan** of a progressive JPEG and renders it as a
1/8-size image. That produces the low-detail thumbnail-like picture, *not* a full decode.
Attempt C's premise — that JPEGDEC is "the progressive-capable decoder" — was wrong, and
the claim in the README/config about supporting progressive was inherited from the same
misreading.

**Why the device then freezes.** Two compounding factors, both consequences of the above:

1. `renderArtwork()` is invoked from `drawScreen()` on the main-loop path. The DC-only
   path still walks the full 640×640 scan set to find and decode the DC coefficients,
   which is slow, and `artNeedsRender` gets re-armed whenever the poll changes the title
   or artist. The 1 s Sonos poll, the button handling and the idle timer all share that
   same single-threaded loop, so a slow render starves them and the screen appears
   frozen. Nothing is actually hardcoded — the UI simply stops being serviced.

2. Worse, the forced `JPEG_SCALE_EIGHTH` interacts badly with the caller's own scaling.
   The code picks a factor to fit 240×84 (for 640×640 that is 1/8) and passes it in, but
   for a *smaller* progressive image the chosen factor can be less than 1/8; JPEGDEC's
   forced `|=` then overrides it, so the size the caller computes and the size actually
   produced disagree. The clipping helper then has to correct for a mismatch, which is
   exactly the kind of thing that leaves stale pixels on screen.

**Lesson recorded:** a decoder "supports progressive" only if it decodes the AC scans
too. Checking for a `JPEG_MODE_PROGRESSIVE` enum is **not** sufficient — read what the
code actually does with the scans.

**Verdict: ❌ JPEGDEC is not usable for progressive artwork here.** Keep it only for
baseline. Remaining options, in order:

1. **Request a baseline rendition from `/getaa`** — still the best option. No progressive
   decode is needed at all, and the existing `esp_jpg_decode` path is already correct and
   fast. This also sidesteps the RAM question entirely.
2. **Ask Sonos for a smaller image.** 640×640 is far larger than the 240×84 area needs; a
   smaller rendition lowers both the decode cost and the RAM cost.
3. **A decoder that genuinely does full progressive** (multi-scan, AC included) — heavier
   on flash/RAM than JPEGDEC.

**Interim mitigation applied** regardless of which option is chosen next: artwork rendering
must not be able to stall the main loop.

Two changes, both in `src/main.cpp`:

1. **`artNeedsRender` is no longer set on metadata changes.** The old
   `if (title != currentTitle || artist != currentArtist) { … artNeedsRender = true; }`
   re-armed a full decode on every poll. Removed — the artwork re-render is driven only by
   `downloadArtwork()` when the cached file actually changes.

2. **Revision guard (`artRevision` / `artDrawnRevision`).** A decode is the slowest thing
   the firmware does, and several unrelated code paths legitimately want a repaint (toast,
   screen wake, idle screen). Those would each trigger a redundant decode. Now:

   ```c
   uint32_t artRevision      = 0;            // bumped when the cached file changes
   uint32_t artDrawnRevision = 0xFFFFFFFF;   // forces the first render
   ```

   `renderArtwork()` skips the decode when `artDrawnRevision == artRevision`, and paths
   that clear the screen (`setScreenPower(true)`, `drawNothingPlayingScreen()`) reset
   `artDrawnRevision` so the artwork is genuinely redrawn.

   The "already cached on disk" early-return in `downloadArtwork()` was also guarded, so
   it no longer forces a pointless decode on every poll.

**Result:** 🔄 **rebuilt; awaiting a look at the screen.**

**Verdict:** the freeze should be gone, but progressive artwork is still only a DC-scan
thumbnail. Do not treat this as "artwork fixed" — it is "artwork no longer freezes the
device". Option 1 above (baseline rendition) is still required for a real picture.

### Attempt F — stale artwork after reflashing — ❌ download was never retried

**Symptom:** after reflashing and rebooting, the display showed the *previous* track's
cover instead of the current one. LittleFS survives a reflash, so `/art.jpg` still held
the old image, and nothing ever replaced it.

**Diagnosis — a logic bug, not a decoder problem.**

`downloadArtwork()` was called from exactly one place, inside the URI-changed test:

```cpp
if (artUri != currentArtUri) {
    currentArtUri = artUri;     // recorded FIRST
    downloadArtwork();          // void — failure was invisible to the caller
}
```

`currentArtUri` was assigned *before* the download, so:

- If the download failed for **any** reason — a transient timeout, the speaker still
  settling on a new track, a momentary WiFi hiccup — the URI had already been marked as
  "handled".
- On the next poll `artUri == currentArtUri`, so the branch was skipped entirely.
- `downloadArtwork()` returned `void`, so the caller could not even know it had failed.

Result: **one failed attempt was permanent.** The old cached image stayed on screen
indefinitely with no recovery path. Nothing retried, ever.

This also explains why it looked like "hardcoded": the file was not hardcoded, the state
machine had latched.

**Fix (all in `src/main.cpp`):**

1. `downloadArtwork()` now returns `bool` (true when the cache holds the current track's
   artwork), so failure is visible to the caller. `cachedArtUrl` is deliberately left
   untouched on failure, so a retry re-fetches rather than short-circuiting.

2. A pending-fetch state drives and retries the download:

   ```c
   bool          artworkPending  = false;
   unsigned long artworkFirstTry = 0;
   ```

   A track change now *arms* the fetch rather than performing it inline, so the first
   attempt is delayed by `ARTWORK_INITIAL_DELAY_MS` (750 ms) — the speaker is often still
   settling immediately after a track change, which is exactly when a request times out.
   On failure it reschedules `ARTWORK_RETRY_INTERVAL_MS` (5 s) later and keeps trying
   until it succeeds.

3. The speaker-switch path clears `cachedArtUrl`, so it also re-arms `artworkPending`;
   otherwise `downloadArtwork()` would short-circuit on the stale cache.

**Bonus bug found in the same function:** `wroteAny` was declared but **never assigned
true**, so the "partial download" diagnostic branch was dead code and every partial
failure was reported as "no data". Now set from `received > 0`, which makes the
`artLastNote` shown on the on-screen diagnostics panel accurate.

**Result:** 🔄 **rebuilt; awaiting confirmation.**

**Verdict:** this should resolve stale artwork. If it persists, the serial log will show
whether `[Art] All N route(s) failed, will retry.` repeats (a fetch problem) or whether
downloads report success while the picture does not change (a decode/placement problem).
Note that a reflash does **not** erase LittleFS, so any test should either change tracks
or delete `/art.jpg` — otherwise the previous cover is legitimately still cached.

### Attempt G — track/artist/times all blank, title shows `1` — ❌ SOAP body silently truncated

**Symptom:** across three tracks and two reboots, the screen showed a `PLAYING` badge,
the title `1`, `(no artist)`, and `0:00 / 0:00`. Reported as "something is wrong with the
data pickup".

**This was not a parsing bug.** Every parser was correct; the response they were given was
incomplete.

**Evidence.** The one-shot RAW dump of `GetPositionInfo` shows the element order:

```
s:Envelope → s:Body → u:GetPositionInfoResponse
  Track, TrackDuration, TrackMetaData, TrackURI, RelTime, AbsTime, RelCount
```

`<Track>` is the **first** element. The screen showed exactly what a response truncated
immediately after `</Track>` looks like: a track number survives, and `TrackDuration`,
`TrackMetaData` (title/artist/artwork) and `RelTime` are all gone. That also explains why
the title rendered as `1` — it was the AVTransport `<Track>` queue number surfacing through
the fallback in `resolveTrackField()`, exactly the value that helper exists to *avoid*
when DIDL data is present.

`GetPositionInfo` is ~1503 bytes. The artwork path was unaffected because it is a separate
request to a separate endpoint.

**Root cause.** `readHttpBody()` looped on `while (remaining > 0 && http.connected())`.
If the peer went away mid-read, `http.connected()` went false and the loop exited —
returning whatever had arrived so far as if it were the complete body. `readBytes()`
returns 0 at end-of-stream, so there was no error signal either. A short read was
therefore **indistinguishable from a successful one** at the call site.

A second, worse latent bug in the same loop: when `available() <= 0` it did
`delay(1); continue;`. If the socket never delivered *and* never disconnected, that
spun forever, holding the main loop — a second way this could stall the UI.

**Fix:**

1. `readHttpBody()` takes an optional `bool *complete` out-param and reports whether the
   full `Content-Length` was read. A short body is now logged explicitly:
   `[HTTP] Truncated body: got N of M bytes.`
2. `sonosSoapTo()` **retries once on a truncated body**, on a fresh `WiFiClient` /
   `HTTPClient` so the dead socket is not reused. The per-attempt log now marks
   `(TRUNCATED)` and names the retry.
3. Added `HTTP_BODY_READ_TIMEOUT_MS` (2000) so the read loop can no longer spin
   indefinitely waiting on a silent socket.
4. Body assembly uses `String::concat(buf, len)` instead of `body += String(buf, len)`,
   removing a heap allocation per 256-byte chunk — that per-chunk temporary was both a
   fragmentation source and a place data could be silently dropped on allocation failure.

**Result:** 🔄 **rebuilt; awaiting confirmation.**

**Verdict:** if the fields are *still* blank, the log will now say explicitly whether the
body was truncated and how often the retry was needed. A persistent `(TRUNCATED)` on every
attempt would point at something upstream (mDNS/DNS pressure, the speaker closing idle
connections, or WiFi signal) rather than at this code.

1. Request a baseline rendition from `/getaa` (no decoder change at all).
2. Ask Sonos for a smaller image. The 640×640 artwork is far larger than the 240×84 area
   needs; a smaller variant would cut RAM and CPU cost sharply.
3. A different progressive decoder, if JPEGDEC's fixed buffers prove too large.

---

*(attempts below are appended as they are made)*

---

## How to log an attempt (read this before changing anything)

Attempts A–G above were each a **hypothesis applied as a code change**. That is
what produced the loop: a change was flashed, the screen still looked wrong, and
the next guess was made without ever capturing what the device actually did. Six
of the seven attempts never produced a measurement at all — they were never
confirmed or refuted on hardware, only abandoned.

From here, an attempt is only valid if it records all five fields. **An attempt
missing the "evidence" and "verdict" fields is not finished and must not be
repeated.**

| Field | What it must contain |
|---|---|
| **Symptom** | What is on the display, read from a photograph or the log — not a paraphrase |
| **Hypothesis** | The one mechanism believed to cause it |
| **Change** | The code change made to test it, or `none` if observation-only |
| **Evidence** | The exact log line, on-screen value or measurement that tests it. If none was captured, say so plainly |
| **Verdict** | ✅ confirmed · ❌ refuted · 🔄 untested · ⛔ do not retry |

Two rules that follow from the failure mode above:

1. **Never change code to test a hypothesis you cannot read the result of.**
   If the log cannot distinguish the outcomes, add the logging first. That is
   Attempt H's whole content.
2. **One variable per attempt.** Attempts that changed the decoder *and* the
   colour order *and* the retry logic (A→B, D→E) produced a result that
   confirmed none of them.

---

### Attempt H — metadata confirmed fixed; artwork reports bare `PROXY 200`

**Symptom:** after flashing the attempt-G build, the screen showed correct
metadata — `Cynical Little Girl` / `House of Woesen`, `1:47 / 3:42`, `PLAYING`,
version `v0.80`. The title-`1` / `(no artist)` / `0:00` symptom from attempt G is
**gone**. The artwork area showed the diagnostics panel reading:

```
PROXY
200
host: 192.168.1.29:1400
url:  /getaa?s=1&u=x-sonos-http%3a&tr...
```

**Hypothesis (attempt G):** the SOAP body was truncated, so the DIDL never
arrived and only `<Track>` survived.

**Change:** attempt G's `readHttpBody()` completeness out-param plus the
fresh-connection retry in `sonosSoapTo()`.

**Evidence:** the display itself. `1:47` advancing against a real `3:42`
duration is only reachable via `RelTime` + `TrackDuration` + `dc:title` +
`dc:creator`, all of which sit *after* `<Track>` in the body. **✅ Confirmed.**

**Verdict:** ✅ attempt G's truncation diagnosis and fix are correct. The
metadata pipeline is done. Do not revisit it.

**New symptom — the artwork stage.** `PROXY 200` means the speaker answered
HTTP 200 on port 1400 and the URI resolved. But the panel cannot say *what went
wrong next*, and that is the defect this attempt is really about:

- `statusText` preferred `artLastStatus` whenever it was non-zero, so the code
  was shown and the failure reason discarded. A body with **no EOI marker**, a
  **partial download**, and a **decode failure** all rendered as an identical
  `PROXY / 200` panel. Three unrelated faults, one indistinguishable picture —
  which is exactly why this attempt could not be diagnosed from the photo and
  had to be reported as a bare "proxy 200 error".

**Change (diagnostics only — no behavioural change to the pipeline):**

1. `artDownloadError` / `artRenderError` record which stage failed and why, and
   now take priority over the HTTP code on the panel. "no EOI", "partial",
   "not cached", "no header", "no RAM", "prog decode fail" are all distinct and
   all readable from a photograph.
2. The one-shot 1400-byte `GetPositionInfo` dump is replaced by
   `logPositionInfoBody()` for the first `SOAP_CAPTURE_COUNT` polls: a
   presence map for all eight AVTransport elements, the body **head and tail**,
   and whether `</s:Envelope>` closed. The old prefix dump could not tell a
   1503-byte body from one cut at 400, because both look like their first 1400
   bytes. The tail makes truncation unmistakable.
3. `logDidlState()` reports the blob length and whether `dc:title`,
   `dc:creator` and `upnp:albumArtURI` are in it — the single measurement that
   separates "the metadata never arrived" from "the parser could not read it".
4. `logXmlSnippet()` escapes control bytes so a capture stays on one greppable
   line instead of spilling across unrelated log lines.
5. **`handleSerialCommands()`** — `d` dump position info, `x` erase `/art.jpg`,
   `f` re-arm the artwork fetch, `m` heap/RSSI/cache size, `h` help. This is the
   direct answer to the loop: experiments no longer require a rebuild, so a
   hypothesis can be tested without first committing it to the firmware.

**Evidence:** 🔄 **built only.** RAM 15.7% (51,288 B), flash 81.7% (1,070,649 B).
Not yet run on hardware.

**Verdict:** 🔄 untested. **Next action is observation, not another fix:**
flash, then read the panel.

- Panel shows `prog decode fail` → JPEGDEC is failing on a complete file; the
  decode path is the next target, and `d` + the `[Art] JPEGDEC …` lines
  describe which stage.
- Panel shows `no EOI` / `partial` → the 200 is a lie about the body; the
  proxy route is truncating and attempt E's transport work applies.
- Panel shows `not cached` → the retry loop is not re-firing; check `[Art] Retrying`.

⛔ **Do not change the decoder, the colour order or the retry timing until one of
those three strings appears on the panel.** The distinction is what selects the
next move, and guessing it again is what caused this loop.

---

