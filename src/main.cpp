/*
 * Sonos What's Playing Display
 *
 * ESP32 / LilyGO T-Display (ST7789) firmware that discovers a Sonos speaker
 * on the local network and shows the currently playing track.
 *
 * Implemented per README.md; hardware and software constants per config.md.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUDP.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>

// Artwork decoding uses TWO decoders, chosen by the JPEG's frame type.
//
// esp_jpg_decode() (ESP-IDF ROM TJpgDec) — BASELINE ONLY.
//   TJpg_Decoder (ChaN's port) was removed earlier: its jd_prepare/jd_decomp symbols
//   collide with the ones ESP-IDF's esp_jpg_decode defines internally, which faults
//   with InstrFetchProhibited on the first decode. But esp_jpg_decode is a thin
//   wrapper over that same ROM TJpgDec core (it calls jd_prepare/jd_decomp from
//   rom/tjpgd.h), and TJpgDec rejects a progressive frame: jd_prepare returns
//   JDR_FMT3 ("Not supported JPEG standard. May be a progressive JPEG image."),
//   which esp_jpg_decode maps to ESP_FAIL. It is kept only for baseline images,
//   where it is fast and streams the file straight from LittleFS.
//
// JPEGDEC (bitbank2) — handles PROGRESSIVE.
//   Sonos serves progressive artwork for local library tracks, so something that can
//   decode SOF2 is required. JPEGDEC has an explicit JPEG_MODE_PROGRESSIVE and, crucially,
//   can be driven from a file handle rather than an in-memory buffer, so the ~138 KB
//   image is never resident in RAM — the same low-RAM property that made esp_jpg_decode
//   attractive, on a board with ~320 KB of DRAM and no PSRAM.
//   Note it must be set to RGB565_BIG_ENDIAN for TFT_eSPI; see jpegdecDrawMCUs().
//
// See ARTWORK-ATTEMPTS.md for the full history of what was tried and rejected, and
// README.md section 6 for how the pipeline fits together.
#include <esp_jpg_decode.h>
#include <JPEGDEC.h>
#include <esp_heap_caps.h>

// std::nothrow, used when allocating the JPEGDEC decoder.
#include <new>

// WiFi credentials and the fallback Sonos addresses live in secrets.h so they
// are never committed. Copy src/secrets.example.h to src/secrets.h and fill
// it in. Without it the project still builds, but WiFi will not connect.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #warning "secrets.h not found - using placeholders. WiFi will not connect. Copy src/secrets.example.h to src/secrets.h."
  const char *WIFI_SSID_PRIMARY   = "YOUR_WIFI_SSID";
  const char *WIFI_SSID_SECONDARY = "YOUR_FALLBACK_SSID";
  const char *WIFI_PASSWORD       = "YOUR_WIFI_PASSWORD";
  const char *FALLBACK_SONOS_1    = "192.168.1.100";
  const char *FALLBACK_SONOS_2    = "192.168.1.101";
#endif

// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------

// Application version. Incremented by 0.01 on each build.
#define APP_VERSION "0.81"

// ---------------------------------------------------------------------------
// WiFi Configuration
// ---------------------------------------------------------------------------

// WIFI_SSID_PRIMARY, WIFI_SSID_SECONDARY and WIFI_PASSWORD come from
// secrets.h (see the include at the top of this file).

// Seconds to wait for each SSID before trying the next one.
#define WIFI_CONNECT_TIMEOUT_S 20

// ---------------------------------------------------------------------------
// Sonos Speaker Configuration
// ---------------------------------------------------------------------------

// FALLBACK_SONOS_1 and FALLBACK_SONOS_2 come from secrets.h.

const char *SSDP_MULTICAST_IP = "239.255.255.250";
#define SSDP_PORT 1900

// Maximum number of Sonos devices tracked during discovery.
#define MAX_SONOS_DEVICES 8

// Port serving the AVTransport SOAP control endpoint and /getaa artwork.
#define SONOS_PORT         1400
#define SONOS_CONTROL_PATH "/MediaRenderer/AVTransport/Control"

// Timeouts and buffer sizes.
#define HTTP_TIMEOUT_MS         2500
#define HTTP_CONNECT_TIMEOUT_MS 2000
#define ARTWORK_TIMEOUT_MS      8000
#define SSDP_PACKET_SIZE        1024
#define HTTP_MAX_REDIRECTS      3

// Hard ceiling on waiting for a response body once the headers are in. Without
// it, a socket that delivers nothing but also never disconnects spins forever.
#define HTTP_BODY_READ_TIMEOUT_MS 2000

// Artwork size cap. esp_jpg_decode needs the whole image resident in RAM (this
// board has 320 KB internal RAM and no PSRAM), so anything beyond this cannot be
// decoded and is discarded on download rather than cached.
#define ART_MAX_SIZE            (150 * 1024)

// ---------------------------------------------------------------------------
// Hardware Pin Assignments
// ---------------------------------------------------------------------------

// GPIO 35 is input only, so both buttons are driven active low using the
// board's external pull-ups and must not use INPUT_PULLUP.
#define PIN_BUTTON_1 35  // Next Track
#define PIN_BUTTON_2 0   // Play / Pause

// Backlight pin, supplied by the TFT_eSPI build flags in platformio.ini.
#ifndef TFT_BL
#error "TFT_BL must be defined in the platformio.ini build_flags"
#endif
#ifndef TFT_BACKLIGHT_ON
#define TFT_BACKLIGHT_ON HIGH
#endif

// ---------------------------------------------------------------------------
// Timing & Debouncing
// ---------------------------------------------------------------------------

#define DEBOUNCE_DELAY_MS      40
#define SONOS_POLL_INTERVAL_MS 1000
#define SSDP_RETRY_INTERVAL_MS 5000
#define SSDP_KEEPALIVE_MS      60000

// How long to wait before retrying an artwork download that failed, and how long
// after a track change before the first attempt is made (so the speaker is not
// hammered while it is still settling on the new track).
#define ARTWORK_RETRY_INTERVAL_MS  5000
#define ARTWORK_INITIAL_DELAY_MS   750

// How often the other known speakers are probed while the active one is idle,
// looking for whichever room is actually playing.
#define SPEAKER_SCAN_INTERVAL_MS 5000
#define IDLE_SCREEN_TIMEOUT_MS 10000
#define TOAST_DURATION_MS      1600
#define WIFI_RETRY_INTERVAL_MS 10000

// ---------------------------------------------------------------------------
// File System & Storage
// ---------------------------------------------------------------------------

#define ART_FILE_PATH  "/art.jpg"
#define LITTLEFS_LABEL "littlefs"
#define LITTLEFS_MOUNT "/littlefs"

// ---------------------------------------------------------------------------
// Display Settings
// ---------------------------------------------------------------------------

#define SCREEN_ROTATION 1

// Artwork occupies the upper portion of the screen.
#define ART_AREA_X 0
#define ART_AREA_Y 0
#define ART_AREA_W 240
#define ART_AREA_H 84

// Metadata block sits below the artwork.
// Row heights: font 2 is 16px, font 1 is 8px, so the rows must not overlap.
#define META_Y    84
#define META_H    51
#define META_X    4   // left margin for all metadata text
#define TITLE_Y   86  // font 2 -> 86..101
#define ARTIST_Y  102 // font 1 -> 102..109
#define TIME_Y    111 // font 1 -> 111..118, clear of the bar at 122

// Progress bar, drawn at the bottom of the metadata block.
#define BAR_X 4
#define BAR_Y 122
#define BAR_W 232
#define BAR_H 12

// Status badge in the top right corner.
#define BADGE_W 48
#define BADGE_H 15
#define BADGE_X 192
#define BADGE_Y 0

// Version marker in the top left corner, opposite the status badge.
#define VER_X 4
#define VER_Y 4

// Toast notification, centred near the top.
#define TOAST_W 140
#define TOAST_H 22
#define TOAST_X 50
#define TOAST_Y 28

// ---------------------------------------------------------------------------
// Title row diagnostic
// ---------------------------------------------------------------------------
// Set to 1 to render the title in the ARTIST row (identical x, y and font)
// with the two values alternating every DIAG_SWAP_MS. This was used to prove
// that <Track> carries the queue track number rather than the song title, and
// the real cause is now fixed in resolveTrackField(). Left in place, disabled,
// as a reusable check; set to 0 for normal operation.
#define TITLE_ROW_DIAG 0
#define DIAG_SWAP_MS   2000

// ---------------------------------------------------------------------------
// Diagnostics capture
// ---------------------------------------------------------------------------
//
// Why these exist: the screen symptom "title shows 1, no artist, 0:00/0:00" is
// produced by BOTH a truncated response and a parser that cannot read a
// complete one. Guessing between them is what produced a chain of speculative
// fixes that each looked reasonable and none was confirmed on hardware.
//
// So the firmware now prints, for the first SOAP_CAPTURE_COUNT polls after boot,
// the evidence needed to tell the two apart:
//   - which of the eight GetPositionInfo elements actually arrived,
//   - the HEAD and the TAIL of the body (a truncated body stops mid-element;
//     the tail makes that unmistakable),
//   - the length of the DIDL blob and whether dc:title / dc:creator /
//     upnp:albumArtURI are inside it.
//
// Zero-length DIDL means transport. Non-empty DIDL with a blank title means
// parser. Nothing else needs to be guessed.
//
// SERIAL_CONSOLE adds a command interface on the same UART, so a single build
// can answer many questions instead of one build per hypothesis.
#define SERIAL_CONSOLE     1
#define SOAP_CAPTURE_COUNT 3

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

// Debounce state for one onboard button.
struct ButtonState {
  uint8_t pin;
  const char *label;
  bool lastReading;
  bool state;
  unsigned long lastDebounceTime;
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

TFT_eSPI tft = TFT_eSPI();

WiFiUDP  ssdpUdp;
IPAddress sonosDevices[MAX_SONOS_DEVICES];
int       sonosDeviceCount = 0;
IPAddress activeSonosIP    = IPAddress(0, 0, 0, 0);

// Onboard buttons, both active low.
ButtonState btn1 = {PIN_BUTTON_1, "BTN1 (IO35)", true, false, 0};
ButtonState btn2 = {PIN_BUTTON_2, "BTN2 (IO0)", true, false, 0};

// Currently playing track.
String   currentTitle;
String   currentArtist;
String   currentArtUri;
String   currentTransportState = "STOPPED";
uint32_t currentRelTime        = 0;
uint32_t currentDuration       = 0;
bool     isPlaying             = false;

// Artwork cache. cachedArtUrl records which URL the LittleFS copy came from.
String cachedArtUrl;
bool   artNeedsRender = true;

// Artwork revision guard.
//
// Decoding is by far the slowest thing the firmware does (a progressive JPEG takes
// hundreds of milliseconds), and it runs on the main loop alongside the 1 s Sonos
// poll, button handling and the idle timer. Re-decoding when nothing has changed
// therefore risks starving all of them.
//
// artRevision is bumped only when the cached file actually changes; artDrawnRevision
// records which revision is currently on screen. renderArtwork() skips the decode
// when they match, so artNeedsRender can be set freely (toast, screen wake) without
// triggering a redundant decode.
uint32_t artRevision       = 0;
uint32_t artDrawnRevision  = 0xFFFFFFFF;  // forces the first render

// Pending artwork fetch. A track change only sets currentArtUri, so the download has to
// be driven from here and retried until it succeeds; otherwise one transient failure left
// the previous track's artwork on screen with no way to recover.
bool          artworkPending  = false;
unsigned long artworkFirstTry = 0;

// Idle state and screen power.
bool          screenPowered            = true;
bool          isIdle                   = true;
unsigned long idleStartTime            = 0;
int           lastIdleSecondsRemaining = -1;

// Toast notification.
String        toastMessage;
uint16_t      toastColor      = TFT_WHITE;
unsigned long toastExpireTime = 0;

// Timing.
unsigned long lastSonosPollTime = 0;
unsigned long lastSsdpSearchTime = 0;
unsigned long lastWifiRetryTime  = 0;
unsigned long lastSpeakerScanTime = 0;

// Set once an artist string has been seen carrying a '/getaa' continuation,
// so the condition is reported only on the first occurrence.
static bool artistContinuationLogged = false;

// Set when something outside the metadata renderer has cleared or overwritten
// the display, so drawMetadataBlock() repaints even if the values are equal.
static bool metadataDirty = true;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

void drawScreen();
void drawConnectingScreen(const char *message);
void drawNothingPlayingScreen();
void drawStatusBadge();
void drawVersion();
void drawArtworkPlaceholder();
void showToast(const char *message, uint16_t color);
void invalidateMetadata();
void setScreenPower(bool on);
bool updateButton(ButtonState &btn);
void connectWiFi();
void sendSSDPDiscovery();
void checkSSDPResponses();
void addSonosDevice(IPAddress ip);
void pollSonos();
void sendSonosAction(const char *action, const char *instanceArgs);
bool downloadArtwork();
void renderArtwork();
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *data);

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------

// Extracts the text between <tag> and </tag> from an XML fragment.
// Returns an empty string when the tag is absent.
// Reverses the XML entities Sonos emits inside text nodes. Defined below, but
// declared here because soapField() applies it.
String xmlUnescape(const String &in);

String extractXmlValue(const String &xml, const char *tag) {
  String openTag  = String("<") + tag + ">";
  String closeTag = String("</") + tag + ">";

  int start = xml.indexOf(openTag);
  if (start < 0) return String();

  start += openTag.length();
  int end = xml.indexOf(closeTag, start);
  if (end < 0) return String();

  return xml.substring(start, end);
}

// Extracts a value by LOCAL tag name, ignoring any XML namespace prefix.
//
// The DIDL metadata is full of prefixed elements such as <upnp:albumArtURI>
// and <dc:title>, and services vary in whether they prefix tags. Matching on
// the local name alone makes extraction far more tolerant.
String extractByLocalName(const String &xml, const char *localName) {
  int searchFrom = 0;

  while (searchFrom < (int)xml.length()) {
    int open = xml.indexOf('<', searchFrom);
    if (open < 0) return String();

    int close = xml.indexOf('>', open);
    if (close < 0) return String();

    String tag = xml.substring(open + 1, close);  // e.g. "upnp:albumArtURI"

    bool selfClosing = (tag.length() > 0 && tag.charAt(tag.length() - 1) == '/');
    if (selfClosing) tag = tag.substring(0, tag.length() - 1);

    // Strip any attributes, then keep the final name component.
    int space = tag.indexOf(' ');
    if (space >= 0) tag = tag.substring(0, space);

    int    colon = tag.lastIndexOf(':');
    String local = (colon >= 0) ? tag.substring(colon + 1) : tag;

    if (!selfClosing && local == localName) {
      int valueStart = close + 1;
      int valueEnd   = xml.indexOf('<', valueStart);
      if (valueEnd < 0) return String();
      return xml.substring(valueStart, valueEnd);
    }

    searchFrom = close + 1;
  }

  return String();
}


// Logs a parsed field as length, text and leading bytes, so a value that
// parses but renders as nothing can be identified from the serial log.
void logSoapField(const char *label, const String &value) {
  Serial.printf("[Sonos]   %-12s len=%3d raw=\"%s\" bytes=",
                label, value.length(), value.c_str());
  for (unsigned int i = 0; i < value.length() && i < 8; i++) {
    Serial.printf("%02X ", (unsigned char)value.charAt(i));
  }
  Serial.println();
}

// ---------------------------------------------------------------------------
// SOAP body capture (diagnostics)
// ---------------------------------------------------------------------------

// Reports whether an element is present in a body, matched on local name so a
// namespace prefix is irrelevant.
//
// This is the primitive the capture is built on: it answers "did this element
// arrive at all?", which is the question that separates a transport failure
// from a parser failure. A parser can only misread data that is present.
bool hasElement(const String &xml, const char *localName) {
  String open = String("<") + localName;

  int idx = 0;
  while ((idx = xml.indexOf(open, idx)) >= 0) {
    char after = xml.charAt(idx + open.length());
    // "<Track>" and "<TrackDuration>" both start with "<Track"; require the
    // next character to end the tag name so one element cannot mask another.
    if (after == '>' || after == '/' || after == ' ') return true;
    idx += open.length();
  }

  return false;
}

// Prints a snippet of XML on ONE log line, escaping anything unprintable.
//
// A raw dump that contains newlines, carriage returns or UTF-8 continuation
// bytes corrupts the serial log: the snippet appears to end early and the rest
// lands on unrelated lines. Escaping keeps each capture on a single greppable
// line, so "how far did the body get" is answerable without counting bytes.
void logXmlSnippet(const char *label, const String &body, int from, int len) {
  if (from < 0) from = 0;
  if (from > (int)body.length()) from = body.length();

  int end = from + len;
  if (end > (int)body.length()) end = body.length();

  Serial.printf("[Sonos] %s [%d..%d of %d]: \"", label, from, end,
                (int)body.length());

  for (int i = from; i < end; i++) {
    unsigned char c = (unsigned char)body.charAt(i);

    if (c == '\r')      Serial.print("\\r");
    else if (c == '\n') Serial.print("\\n");
    else if (c == '\t') Serial.print("\\t");
    else if (c < 32 || c > 126) Serial.printf("\\x%02X", c);
    else Serial.write((char)c);
  }

  Serial.println("\"");
}

// Logs which GetPositionInfo elements actually arrived, and the head and tail
// of the body.
//
// The tail is the important half. A body truncated mid-response stops at an
// arbitrary offset, and the last tag seen is the whole diagnosis: cut after
// <Track> and the title is a queue number with everything after it missing;
// ending in "</s:Envelope>" means the body WAS complete and the fault is
// downstream in the parser.
void logPositionInfoBody(const String &body) {
  static const char *kElements[] = {
      "Track", "TrackDuration", "TrackMetaData", "CurrentTrackMetaData",
      "TrackURI", "RelTime", "AbsTime", "RelCount"};

  Serial.printf("[Sonos] Elements present:");
  for (unsigned int i = 0; i < sizeof(kElements) / sizeof(kElements[0]); i++) {
    Serial.printf(" %s=%s", kElements[i], hasElement(body, kElements[i]) ? "Y" : "N");
  }
  Serial.println();

  bool closed = (body.indexOf("</s:Envelope>") >= 0);
  Serial.printf("[Sonos] Envelope closed: %s  (body=%d bytes)\n",
                closed ? "YES" : "NO", (int)body.length());

  logXmlSnippet("HEAD", body, 0, 260);
  logXmlSnippet("TAIL", body, body.length() - 260, 260);
}

// Logs the state of the DIDL blob and the three elements taken from it.
//
// blob=0 settles the transport-vs-parser question outright:
//   blob=0                    -> the metadata never arrived; parsing is irrelevant
//   blob>0, title missing     -> the DIDL arrived but dc:title could not be read
//   blob>0, title present     -> parsing worked; the screen is at fault
void logDidlState(const String &metaData) {
  Serial.printf("[Sonos] DIDL blob: %d bytes, dc:title=%s dc:creator=%s "
                "upnp:albumArtURI=%s\n",
                (int)metaData.length(),
                hasElement(metaData, "dc:title")   ? "Y" : "N",
                hasElement(metaData, "dc:creator") ? "Y" : "N",
                hasElement(metaData, "upnp:albumArtURI") ? "Y" : "N");

  if (metaData.length() > 0) {
    logXmlSnippet("DIDL", metaData, 0, 300);
  }
}

// Normalises a display string for the bundled bitmap fonts.
//
// The fonts built into TFT_eSPI (GLCD and Font16) only carry printable ASCII,
// glyphs 32-127; anything else is dropped silently by drawChar(). A multi-byte
// UTF-8 lead byte is worse than dropped: decodeUTF8() consumes the following
// bytes as continuation bytes, so a single smart quote (U+2019 = E2 80 99)
// swallows the ASCII text behind it and the whole line renders as nothing.
// So every byte outside 32-126 is removed here, before it reaches the display.
String sanitizeText(const String &raw) {
  String s = raw;
  s.trim();

  // A '<' means the parse overran into surrounding markup; keep only the
  // first plain-text run.
  int lt = s.indexOf('<');
  if (lt >= 0) s = s.substring(0, lt);

  String clean;
  clean.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c >= 32 && c <= 126) clean += c;
  }

  clean.trim();
  if (clean.length() > 200) clean = clean.substring(0, 200);

  return clean;
}

// Extracts a simple AVTransport element such as <Track> or <TrackArtist>.
//
// A single helper is used for every text field so that the title and the artist
// are parsed by exactly the same rules and can never behave differently from
// one another. It uses the plain "<tag>…</tag>" form first, and only if that
// yields nothing does it scan by local tag name (ignoring any namespace
// prefix). The result is unescaped and reduced to printable ASCII, which is
// all the bundled bitmap fonts can draw.
String soapField(const String &xml, const char *tag) {
  String value = extractXmlValue(xml, tag);

  if (value.length() == 0) {
    value = extractByLocalName(xml, tag);
  }

  return sanitizeText(xmlUnescape(value));
}

// Resolves one text field (title or artist) from a GetPositionInfo response.
//
// This is the single code path used for BOTH the track title and the artist,
// so the two can never diverge again:
//
//   resolveTrackField(metaData, "title",   positionBody, "Track")
//   resolveTrackField(metaData, "creator", positionBody, "TrackArtist")
//
// Order of preference:
//   1. <dc:title> / <dc:creator> inside the DIDL-Lite metadata. This is the
//      authoritative source for both fields.
//   2. <Track> / <TrackArtist> on the AVTransport response, as a fallback.
//
// The ordering matters for the title specifically: for queue and playlist
// playback Sonos puts the *queue track number* in <Track> (e.g. "3"), not the
// song title. Preferring the DIDL title is what prevents that number from being
// displayed in place of the title. <TrackArtist> does not suffer from this, but
// both fields go through the same path so their handling cannot drift apart.
String resolveTrackField(const String &metaData, const char *didlTag,
                        const String &positionBody, const char *avtTag) {
  // 1. Preferred: the DIDL-Lite metadata inside CurrentTrackMetaData.
  if (metaData.length() > 0) {
    String value = soapField(metaData, didlTag);
    if (value.length() > 0) return value;
  }

  // 2. Fallback: the AVTransport element on the position response.
  return soapField(positionBody, avtTag);
}

// Reverses the XML entities Sonos emits inside text nodes.
//
// Single pass on purpose: "&" is replaced last so that an escaped literal such
// as "&amp;lt;" is not collapsed into a real "<" by the "&lt;" rule.
String xmlUnescape(const String &in) {
  String out = in;
  out.replace("&lt;", "<");
  out.replace("&gt;", ">");
  out.replace("&quot;", "\"");
  out.replace("&apos;", "'");
  out.replace("&amp;", "&");  // must run last
  return out;
}

// Unescapes repeatedly until no entity remains.
//
// Needed because Sonos escapes the DIDL-Lite blob in <CurrentTrackMetaData>
// TWICE: the "&" that separates the /getaa query parameters is written as
// "&amp;" inside the DIDL, and the SOAP envelope then escapes the whole blob
// again, so the wire format carries "&amp;amp;u=". One pass therefore leaves a
// literal "&amp;" in the artwork URI, the speaker stops seeing the "u" parameter
// and replies 404 instead of the JPEG.
//
// Repeats only while a pass still changes the string, so text that was escaped
// just once (or not at all) is returned untouched by the extra passes.
String xmlUnescapeDeep(const String &in, int maxPasses = 3) {
  String out = in;

  for (int pass = 0; pass < maxPasses; pass++) {
    String next = xmlUnescape(out);
    if (next == out) break;  // nothing left to decode
    out = next;
  }

  return out;
}

// Converts a Sonos "H:MM:SS" (or "MM:SS") duration into seconds.
uint32_t parseTimeToSeconds(const String &time) {
  if (time.length() == 0) return 0;

  uint32_t total = 0;
  uint32_t value = 0;
  bool     seen  = false;

  for (unsigned int i = 0; i <= time.length(); i++) {
    char c = (i < time.length()) ? time.charAt(i) : ':';
    if (c >= '0' && c <= '9') {
      value = value * 10 + (c - '0');
      seen  = true;
    } else if (c == ':') {
      if (!seen) break;               // malformed, stop early
      total = total * 60 + value;     // shift the accumulated value in
      value = 0;
      seen  = false;
    } else {
      break;                          // ignore any trailing fraction
    }
  }

  return total;
}

// Formats a number of seconds as "M:SS".
String formatTime(uint32_t seconds) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(seconds / 60),
           (unsigned)(seconds % 60));
  return String(buf);
}

// Track completion as a percentage, 0-100.
int progressPercent() {
  if (currentDuration == 0) return 0;
  if (currentRelTime >= currentDuration) return 100;
  return (int)((currentRelTime * 100UL) / currentDuration);
}

// Strips the '/getaa' continuation from an artist string.
//
// Sonos sometimes returns the artist with the artwork path appended, e.g.
// "Radiohead - /getaa?id=1234". Everything from " - /getaa" onward is dropped
// so only the real artist name is shown.
String sanitizeArtist(const String &raw) {
  int idx = raw.indexOf(" - /getaa");
  if (idx >= 0) {
    if (!artistContinuationLogged) {
      Serial.printf("[Debug] Artist contained '/getaa' continuation: \"%s\"\n",
                    raw.c_str());
      artistContinuationLogged = true;
    }
    return raw.substring(0, idx);
  }

  // Also catch a bare trailing "/getaa" with no " - " separator.
  int bare = raw.indexOf("/getaa");
  if (bare >= 0) {
    return raw.substring(0, bare);
  }

  return raw;
}

// Converts a single hex digit to its value, or -1 if not a hex digit.
int hexDigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Percent-decodes a URL component ("%3a%2f%2f" -> "://").
String urlDecode(const String &in) {
  String out;
  out.reserve(in.length());

  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in.charAt(i);

    if (c == '%' && i + 2 < in.length()) {
      int hi = hexDigitValue(in.charAt(i + 1));
      int lo = hexDigitValue(in.charAt(i + 2));

      if (hi >= 0 && lo >= 0) {
        out += (char)((hi << 4) | lo);
        i += 2;
        continue;
      }
    }

    out += c;
  }

  return out;
}

// Pulls the "u=" query parameter out of a URL and percent-decodes it.
//
// A speaker-proxied artwork path looks like:
//     /getaa?s=1&u=http%3a%2f%2fi.scdn.co%2fimage%2f...
// The u= value is the ORIGINAL upstream resource, which is often a real
// absolute https URL that can be fetched directly instead of being proxied
// through the speaker. Returns an empty string when there is no usable u=.
String extractUrlParamU(const String &url) {
  int idx = 0;
  bool found = false;

  // Match "u=" only at the start of the string or after ? or &, so parameters
  // such as "sid=" are not mistaken for it.
  while ((idx = url.indexOf("u=", idx)) >= 0) {
    if (idx == 0 || url.charAt(idx - 1) == '?' || url.charAt(idx - 1) == '&') {
      found = true;
      break;
    }
    idx += 2;
  }

  if (!found) return String();

  String value = url.substring(idx + 2);

  int amp = value.indexOf('&');
  if (amp >= 0) value = value.substring(0, amp);

  return urlDecode(value);
}

// Resolves an artwork URL taken from the GetPositionInfo response.
//
// upnp:albumArtURI arrives in two forms:
//
//   Relative path  e.g. /getaa?s=1&u=...
//       Served by the speaker. The "u=" parameter carries the original
//       upstream resource, percent-encoded. When that decodes to an absolute
//       http(s) URL - typical for streaming services such as Spotify, where
//       it looks like https://i.scdn.co/image/... - it is fetched DIRECTLY,
//       which avoids a round trip through the speaker entirely. If it decodes
//       to an internal Sonos scheme (x-sonos-http:, x-file-cifs:,
//       x-rincon-mp3radio:) it is not directly fetchable and the speaker must
//       proxy it, so the address becomes:
//           http://<speaker-ip>:1400/getaa?s=1&u=...
//       Port 1400 is required; the speaker serves nothing on port 80.
//
//   Absolute URL   e.g. https://i.scdn.co/image/...
//       Used as-is. Common with some streaming services and internet radio.
//
// Protocol-relative "//host/path" is also accepted.
// Builds the speaker-proxied address for a relative /getaa path.
// Returns an empty string when the speaker is not known.
String buildSpeakerProxyUrl(const String &raw) {
  if (activeSonosIP[0] == 0) return String();

  // Port 1400 is the speaker's HTTP port; it serves both the AVTransport SOAP
  // control endpoint and /getaa. Port 80 serves nothing.
  String base = "http://" + activeSonosIP.toString() + ":" + String(SONOS_PORT);

  if (raw.startsWith("/")) return base + raw;
  return base + "/" + raw;
}

// Produces the candidate URLs to try, in order, for an artwork reference.
//
// upnp:albumArtURI arrives in two forms:
//
//   Relative path  e.g. /getaa?s=1&u=...
//       Served by the speaker. Two routes are possible:
//         (a) direct - the "u=" parameter carries the original upstream
//             resource, percent-encoded. When it decodes to an absolute http(s)
//             URL it is fetched straight from the CDN.
//         (b) proxied - http://<speaker-ip>:1400/getaa?s=1&u=...
//
//       Both are returned, direct first, because neither is reliable on its
//       own. Streaming CDNs such as Spotify's i.scdn.co commonly reject direct
//       requests with 403, or answer 302 to a short-lived signed URL, precisely
//       because the speaker is expected to supply the right headers. Meanwhile
//       the proxy route is slower and depends on the speaker being reachable.
//       Trying direct first and falling back to the proxy covers both cases
//       without needing to know which service is playing.
//
//   Absolute URL   e.g. https://i.scdn.co/image/...
//       Only one candidate: use it as-is.
//
// Protocol-relative "//host/path" is also accepted.
int artworkUrlCandidates(const String &raw, String out[], int maxOut) {
  int count = 0;

  String url = raw;
  url.trim();
  if (url.length() == 0) return 0;

  if (count < maxOut) out[count++] = url;  // already absolute

  if (url.startsWith("//") && count < maxOut) {
    out[count++] = "http:" + url;
  }

  if (!url.startsWith("http://") && !url.startsWith("https://") &&
      !url.startsWith("//")) {
    // Relative path: offer the direct upstream first, then the speaker proxy.
    String upstream = extractUrlParamU(url);
    if ((upstream.startsWith("http://") || upstream.startsWith("https://")) &&
        count < maxOut) {
      out[count++] = upstream;
    }

    String proxy = buildSpeakerProxyUrl(url);
    if (proxy.length() > 0 && count < maxOut) {
      out[count++] = proxy;
    }
  }

  return count;
}

// Returns the single best-guess URL, used for on-screen display only.
// The real fetch path uses artworkUrlCandidates() and tries each in turn.
String resolveArtworkUrl(const String &raw) {
  String candidates[2];
  int    count = artworkUrlCandidates(raw, candidates, 2);
  return (count > 0) ? candidates[0] : String();
}

// Resolves a Location header against the URL it came from, so that relative
// redirect targets keep working.
String resolveRedirect(const String &baseUrl, const String &location) {
  if (location.length() == 0) return String();

  if (location.startsWith("http://") || location.startsWith("https://")) {
    return location;
  }

  if (location.startsWith("//")) {
    int      schemeEnd = baseUrl.indexOf("://");
    String   scheme    = (schemeEnd > 0) ? baseUrl.substring(0, schemeEnd) : "http";
    return scheme + ":" + location;
  }

  // Reduce the base URL to scheme://host[:port].
  int schemeEnd = baseUrl.indexOf("://");
  if (schemeEnd < 0) return location;

  int    pathStart = baseUrl.indexOf('/', schemeEnd + 3);
  String origin    = (pathStart > 0) ? baseUrl.substring(0, pathStart) : baseUrl;

  if (location.startsWith("/")) {
    return origin + location;
  }

  return origin + "/" + location;
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

// Updates a button's debounced state, returning true when it changes.
bool updateButton(ButtonState &btn) {
  // Active low: the pin reads LOW while the button is held down.
  bool reading = (digitalRead(btn.pin) == LOW);

  if (reading != btn.lastReading) {
    btn.lastReading      = reading;
    btn.lastDebounceTime = millis();
  }

  if ((millis() - btn.lastDebounceTime) > DEBOUNCE_DELAY_MS) {
    if (reading != btn.state) {
      btn.state = reading;
      return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

// Attempts a single SSID, returning true once an IP address is assigned.
bool tryConnect(const char *ssid) {
  Serial.printf("[WiFi] Connecting to \"%s\"...\n", ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - start) < (WIFI_CONNECT_TIMEOUT_S * 1000UL)) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.printf("[WiFi] Timed out connecting to \"%s\"\n", ssid);
  WiFi.disconnect(true);
  return false;
}

// Connects to the primary SSID, falling back to the secondary one.
void connectWiFi() {
  if (tryConnect(WIFI_SSID_PRIMARY)) return;
  if (tryConnect(WIFI_SSID_SECONDARY)) return;

  Serial.println("[WiFi] Both SSIDs failed, will retry shortly.");
}

// ---------------------------------------------------------------------------
// SSDP Discovery
// ---------------------------------------------------------------------------

// Records a discovered speaker, ignoring duplicates and table overflow.
void addSonosDevice(IPAddress ip) {
  if (ip[0] == 0) return;

  for (int i = 0; i < sonosDeviceCount; i++) {
    if (sonosDevices[i] == ip) return;
  }

  if (sonosDeviceCount >= MAX_SONOS_DEVICES) {
    Serial.printf("[SSDP] Device table full, ignoring %s\n",
                  ip.toString().c_str());
    return;
  }

  sonosDevices[sonosDeviceCount++] = ip;
  Serial.printf("[SSDP] Discovered Sonos device: %s\n", ip.toString().c_str());
}

// Sends an M-SEARCH for ZonePlayer devices.
void sendSSDPDiscovery() {
  IPAddress multicast;
  multicast.fromString(SSDP_MULTICAST_IP);

  if (!ssdpUdp.beginMulticast(multicast, SSDP_PORT)) {
    Serial.println("[SSDP] Failed to join multicast group.");
    return;
  }

  const char *search =
      "M-SEARCH * HTTP/1.1\r\n"
      "HOST: 239.255.255.250:1900\r\n"
      "MAN: \"ssdp:discover\"\r\n"
      "MX: 2\r\n"
      "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
      "\r\n";

  if (ssdpUdp.beginPacket(multicast, SSDP_PORT) > 0) {
    ssdpUdp.write((const uint8_t *)search, strlen(search));
    ssdpUdp.endPacket();
    Serial.println("[SSDP] M-SEARCH sent.");
  } else {
    Serial.println("[SSDP] beginPacket failed.");
  }

  lastSsdpSearchTime = millis();
}

// Reads pending SSDP responses and adopts the first speaker found.
void checkSSDPResponses() {
  int packetSize;

  while ((packetSize = ssdpUdp.parsePacket()) > 0) {
    if (packetSize > SSDP_PACKET_SIZE) {
      ssdpUdp.read();  // drain oversized packets
      continue;
    }

    uint8_t buffer[SSDP_PACKET_SIZE];
    int     len = ssdpUdp.read(buffer, sizeof(buffer) - 1);
    if (len <= 0) continue;
    buffer[len] = '\0';

    String response((const char *)buffer);

    // Confirm this is a Sonos speaker before adopting it.
    if (response.indexOf("Sonos") < 0) continue;

    // The LOCATION header carries the device description URL, from which the
    // speaker's IP address is taken.
    int locStart = response.indexOf("LOCATION:");
    if (locStart < 0) continue;

    int urlStart = response.indexOf("http", locStart);
    if (urlStart < 0) continue;

    int urlEnd = response.indexOf("\r\n", urlStart);
    if (urlEnd < 0) urlEnd = response.length();

    String location = response.substring(urlStart, urlEnd);

    int schemeEnd = location.indexOf("://");
    if (schemeEnd < 0) continue;

    int hostStart = schemeEnd + 3;
    int hostEnd   = location.indexOf('/', hostStart);
    if (hostEnd < 0) hostEnd = location.length();

    String host = location.substring(hostStart, hostEnd);
    int    port = host.indexOf(':');
    if (port > 0) host = host.substring(0, port);

    IPAddress ip;
    if (!ip.fromString(host)) continue;

    addSonosDevice(ip);
  }
}

// ---------------------------------------------------------------------------
// Sonos SOAP
// ---------------------------------------------------------------------------

// Parses a hex chunk size, returning -1 if the token is not a valid size.
static int parseHexChunkSize(const String &token) {
  String t = token;
  t.trim();
  if (t.length() == 0) return -1;

  // Drop any chunk extension after a semicolon.
  int semi = t.indexOf(';');
  if (semi >= 0) t = t.substring(0, semi);
  t.trim();

  unsigned long value = 0;
  for (unsigned int i = 0; i < t.length(); i++) {
    char c = t.charAt(i);
    int  digit;
    if (c >= '0' && c <= '9')      digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return -1;

    value = value * 16 + digit;
    if (value > 0x7FFFFFFUL) return -1;  // implausible, treat as malformed
  }
  return (int)value;
}

// Reads a single line (up to and including '\n') into buf.
//
// WiFiClient::readStringUntil() resolves to an overload returning void on this
// Arduino core, so the line is assembled a byte at a time instead.
static int readLineRaw(WiFiClient *stream, char *buf, int maxLen) {
  int n = 0;

  while (n < maxLen - 1) {
    char c;
    if (stream->readBytes(&c, 1) != 1) break;

    buf[n++] = c;
    if (c == '\n') break;
  }

  buf[n] = '\0';
  return n;
}

// Reads a complete HTTP response body, transparently handling chunked
// transfer encoding.
//
// Sonos does not always send a usable Content-Length, and HTTPClient's
// getString() returns "" when that header is 0, so the body is pulled from
// the socket here instead.
//
// `complete` (optional) reports whether the whole body was actually read. A
// connection that drops part-way through yields a SHORT body with no error
// from readBytes(), which used to be returned as if it were complete. For
// GetPositionInfo that matters a lot: <Track> is the first element, so a
// truncated response still yields a track number while the title, artist,
// duration and artwork URI silently disappear. Callers use this to retry.
static String readHttpBody(HTTPClient &http, bool *complete = nullptr) {
  if (complete != nullptr) *complete = false;

  WiFiClient *stream = http.getStreamPtr();
  if (stream == nullptr) return String();

  String body;
  body.reserve(2048);
  uint8_t buf[256];

  // Bounded wait, so a socket that never delivers and never disconnects cannot
  // spin here forever and stall the whole main loop.
  const uint32_t deadline = millis() + HTTP_BODY_READ_TIMEOUT_MS;

  if (http.getSize() > 0) {
    // Known length: read exactly that many bytes.
    int remaining = http.getSize();

    while (remaining > 0) {
      int available = stream->available();
      if (available <= 0) {
        if (!http.connected()) break;          // peer went away: short read
        if ((int32_t)(millis() - deadline) > 0) {
          Serial.println("[HTTP] Timed out waiting for response body.");
          break;
        }
        delay(1);
        continue;
      }
      int want = (available < remaining) ? available : remaining;
      if (want > (int)sizeof(buf)) want = sizeof(buf);

      int got = stream->readBytes(buf, want);
      if (got <= 0) break;

      // concat() appends in place; building a temporary String per chunk both
      // costs an allocation each iteration and loses data if it fails.
      body.concat((const char *)buf, got);
      remaining -= got;
    }

    bool full = (remaining == 0);
    if (complete != nullptr) *complete = full;

    if (!full) {
      Serial.printf("[HTTP] Truncated body: got %u of %d bytes.\n",
                    (unsigned)body.length(), (int)http.getSize());
    }
    return body;
  }

  // No usable length: read until the peer closes, reassembling chunk framing
  // if the response turns out to be chunked.
  bool chunked        = false;
  bool inChunk        = false;
  int  chunkRemaining = 0;
  char lineBuf[128];

  while (http.connected()) {
    if (stream->available() <= 0) {
      delay(1);
      continue;
    }

    if (!chunked) {
      // Peek at the first line to decide whether this is chunked framing.
      if (readLineRaw(stream, lineBuf, sizeof(lineBuf)) == 0) break;

      String head(lineBuf);
      head.trim();
      if (head.length() == 0) continue;

      int size = parseHexChunkSize(head);
      if (size >= 0) {
        chunked        = true;
        inChunk        = true;
        chunkRemaining = size;
      } else {
        body += head;  // plain, unframed body
      }
      continue;
    }

    if (!inChunk) {
      // Consume the CRLF that terminated the previous chunk, then read the
      // next chunk header.
      readLineRaw(stream, lineBuf, sizeof(lineBuf));

      if (readLineRaw(stream, lineBuf, sizeof(lineBuf)) == 0) break;

      chunkRemaining = parseHexChunkSize(String(lineBuf));
      if (chunkRemaining < 0) break;
      inChunk = true;
      continue;
    }

    int want = stream->available();
    if (want > chunkRemaining) want = chunkRemaining;
    if (want > (int)sizeof(buf)) want = sizeof(buf);

    int got = stream->readBytes(buf, want);
    if (got <= 0) break;

    body.concat((const char *)buf, got);
    chunkRemaining -= got;

    if (chunkRemaining == 0) inChunk = false;
  }

  if (complete != nullptr) *complete = true;
  return body;
}

// ===========================================================================
// Sonos UPnP / SOAP interface
// ===========================================================================
//
// Transport
// ---------
//   Endpoint : POST http://<speaker-ip>:1400/MediaRenderer/AVTransport/Control
//   Service  : urn:schemas-upnp-org:service:AVTransport:1
//   Header   : SOAPACTION: "urn:schemas-upnp-org:service:AVTransport:1#<Action>"
//   Body     : Content-Type: text/xml; charset="utf-8"
//   Instance : <InstanceID>0</InstanceID> (the speaker's own render instance)
//
// Request envelope produced by buildEnvelope():
//
//   <?xml version="1.0" encoding="utf-8"?>
//   <s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"
//               s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
//     <s:Body>
//       <u:<Action> xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">
//         <InstanceID>0</InstanceID>
//       </u:<Action>
//     </s:Body>
//   </s:Envelope>
//
// Actions used, and what each drives on screen:
//
//   GetTransportInfo  -> CurrentTransportState -> status badge (top right)
//   GetPositionInfo   -> Track                 -> title     (y=86,  font 2)
//                     -> TrackArtist           -> artist    (y=102, font 1)
//                     -> RelTime               -> elapsed   (y=111, left)
//                     -> TrackDuration         -> total     (y=111, right)
//                     -> CurrentTrackMetaData -> artwork   (y=0..83)
//   Play  / Pause / Next -> transport control from the onboard buttons
//
// Field -> display mapping (see README.md for the full table):
//
//   <CurrentTransportState>  PLAYING | PAUSED_PLAYBACK | STOPPED
//   <Track>                  track title, sanitised to printable ASCII
//   <TrackArtist>            artist, '/getaa' continuation stripped
//   <RelTime>                H:MM:SS  -> elapsed seconds
//   <TrackDuration>          H:MM:SS  -> total seconds
//   <CurrentTrackMetaData>   XML-escaped DIDL-Lite; unescaped, then read for
//                            <upnp:albumArtURI> (artwork URL, usually
//                            http://<ip>:1400/getaa?u=...), with
//                            <dc:title>/<dc:creator> as title/artist fallbacks
//
// Speaker discovery is SSDP, not SOAP:
//
//   M-SEARCH * HTTP/1.1 -> 239.255.255.250:1900
//   ST: urn:schemas-upnp-org:device:ZonePlayer:1
//   Responses are filtered on the "Sonos" user-agent; the speaker's address is
//   taken from the LOCATION header, e.g. http://192.168.1.50:1400/xml/device_description.xml
// ===========================================================================

// Builds a SOAP envelope for an AVTransport action.
String buildEnvelope(const char *action, const char *instanceArgs) {
  String envelope;
  envelope.reserve(700);
  envelope += F("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
  envelope += F("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" ");
  envelope += F("s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">");
  envelope += F("<s:Body><u:");
  envelope += action;
  envelope += F(" xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">");
  envelope += instanceArgs;
  envelope += F("</u:");
  envelope += action;
  envelope += F("></s:Body></s:Envelope>");
  return envelope;
}

// Issues a SOAP action against a specific speaker and returns the body.
// Used to probe candidate speakers as well as the active one.
//
// A truncated response is retried rather than returned. This matters for
// GetPositionInfo specifically: <Track> is the first element in the body, so a
// short read still yields a track number while TrackDuration, CurrentTrackMetaData
// and RelTime are all cut off. The result was a screen showing the queue number
// as the title, no artist, 0:00 / 0:00, and no artwork - which looks exactly
// like a parsing bug but is a transport one.
String sonosSoapTo(const IPAddress &ip, const char *action,
                  const char *instanceArgs, bool verbose = true) {
  if (ip[0] == 0) return String();

  String url = "http://" + ip.toString() + ":" + String(SONOS_PORT) +
               SONOS_CONTROL_PATH;

  String soapAction = "\"urn:schemas-upnp-org:service:AVTransport:1#";
  soapAction += action;
  soapAction += "\"";

  for (int attempt = 1; attempt <= 2; attempt++) {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setUserAgent("SonosDisplay/" APP_VERSION);

    if (!http.begin(client, url)) {
      if (verbose) Serial.println("[Debug] SOAP begin() failed.");
      return String();
    }

    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPACTION", soapAction);

    int     status  = http.POST(buildEnvelope(action, instanceArgs));
    String  body;
    bool    complete = false;

    if (status == 200) {
      int contentLength = http.getSize();
      body = readHttpBody(http, &complete);

      if (verbose) {
        Serial.printf("[Debug] %s -> HTTP %d, Content-Length=%d, received=%d bytes%s\n",
                      action, status, contentLength, body.length(),
                      complete ? "" : " (TRUNCATED)");
      }

      http.end();

      if (complete || body.length() == 0) return body;

      // Short read: close the socket and try once more on a fresh connection.
      if (verbose) {
        Serial.printf("[Debug] %s body truncated, retrying (%d/2).\n",
                      action, attempt);
      }
      delay(50);
      continue;
    }

    if (verbose) Serial.printf("[Debug] %s failed: HTTP %d\n", action, status);
    http.end();
    return body;
  }

  return String();
}

// Issues a SOAP action against the active speaker and returns the body.
String sonosSoap(const char *action, const char *instanceArgs) {
  return sonosSoapTo(activeSonosIP, action, instanceArgs, true);
}

// Sends a playback control action (Play, Pause, Next, ...).
void sendSonosAction(const char *action, const char *instanceArgs) {
  if (activeSonosIP[0] == 0) {
    Serial.println("[Debug] No active Sonos IP, ignoring action.");
    return;
  }

  String url = "http://" + activeSonosIP.toString() + ":" + String(SONOS_PORT) +
               SONOS_CONTROL_PATH;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setUserAgent("SonosDisplay/" APP_VERSION);

  if (!http.begin(client, url)) return;

  String soapAction = "\"urn:schemas-upnp-org:service:AVTransport:1#";
  soapAction += action;
  soapAction += "\"";

  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPACTION", soapAction);

  int status = http.POST(buildEnvelope(action, instanceArgs));
  Serial.printf("[Debug] Action %s -> HTTP %d\n", action, status);
  http.end();
}

// ---------------------------------------------------------------------------
// Artwork
// ---------------------------------------------------------------------------

// Artwork diagnostics, surfaced on screen in a large font so the values can
// be read from a photograph of the display as well as from the serial log.
int    artLastStatus  = 0;      // HTTP status, 0 = not attempted
String artLastRoute;            // "DIRECT", "PROXY" or "UNRESOLVED"
String artLastHost;             // host the request went to
String artLastNote;             // short reason text

// Short reason the last stage of the artwork pipeline failed, or "" if none did.
//
// Separate from artLastNote because HTTP 200 only means the speaker answered; it
// says nothing about whether a complete JPEG arrived or whether it decoded. The
// panel used to prefer the status code, so a missing-EOI body and a decode
// failure BOTH displayed as "200" and were indistinguishable in a photograph -
// which is how one attempt got logged as "PROXY 200" with no stated cause.
// The failure reason now takes priority over the status code on screen.
String artDownloadError;        // download stage: "no EOI", "partial", ...
String artRenderError;          // render stage:  "no header", "decode fail", ...

// A write-only Stream that forwards everything into an open LittleFS file,
// capped at a maximum size.
//
// Used as the destination for HTTPClient::writeToStream(), which is the
// framework's own de-chunking read path. Buffering is 512 bytes, so the body is
// never held in RAM in full.
class FileSink : public Stream {
 public:
  // "_capLimit": a plain "maxBytes" is safer than _max, because <windows.h> style
  // headers define max() as a macro and it would rewrite the identifier.
  FileSink(File &file, size_t capLimit) : _file(file), _capLimit(capLimit) {}

  // Stream interface. Only writes are ever used by writeToStream(); the read
  // side is a sink and returns nothing, which is what Print expects.
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() {}

  size_t write(uint8_t c) override {
    if (_written >= _capLimit) return 0;  // full: report a short write
    if (_file.write(&c, 1) != 1) return 0;
    _written++;
    return 1;
  }

  size_t write(const uint8_t *data, size_t len) override {
    size_t room = (_written < _capLimit) ? (_capLimit - _written) : 0;
    if (len > room) len = room;
    if (len == 0) return 0;

    size_t n = _file.write(data, len);

    // Track the final two bytes here rather than seeking back to read them: the
    // cache is opened FILE_WRITE ("w"), so reading from it is not dependable.
    for (size_t i = 0; i < n; i++) {
      _prev = _last;
      _last = data[i];
    }

    _written += n;
    return n;
  }

  size_t written() const { return _written; }

  // True when the stream ended on the JPEG end-of-image marker (0xFFD9), which
  // proves the body arrived complete.
  bool endedWithEoi() const { return _written >= 2 && _prev == 0xFF && _last == 0xD9; }

 private:
  File   &_file;
  size_t  _capLimit;
  size_t  _written = 0;
  uint8_t _prev    = 0;
  uint8_t _last    = 0;
};

// Attempts one artwork URL, following redirects, and writes any bytes received
// into the open file. Returns true when at least one byte was written.
static bool fetchArtworkFrom(const String &startUrl, File &artFile,
                             size_t &receivedOut) {
  receivedOut       = 0;
  bool wroteAny      = false;
  bool success       = false;
  int  redirectCount = 0;
  String url         = startUrl;

  // Both clients live for the whole request. HTTPClient keeps a reference to
  // whichever client it was given, so a client declared inside a narrower scope
  // would be destroyed while http.getStreamPtr() still pointed at it.
  WiFiClientSecure secureClient;
  WiFiClient       plainClient;

  while (redirectCount <= HTTP_MAX_REDIRECTS) {
    bool https = url.startsWith("https://");
    if (https) {
      // Streaming providers serve artwork over TLS; certificate validation is
      // skipped because the image is only rendered locally.
      secureClient.setInsecure();
    }

    HTTPClient http;
    http.setTimeout(ARTWORK_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setUserAgent("SonosDisplay/" APP_VERSION);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setReuse(false);

    if (https) {
      http.begin(secureClient, url);
    } else {
      http.begin(plainClient, url);
    }

    Serial.printf("[Art]   GET (hop %d, %s)...\n", redirectCount + 1,
                  https ? "https" : "http");

    int status = http.GET();
    artLastStatus = status;
    Serial.printf("[Art]   HTTP status: %d\n", status);

    if (status == 301 || status == 302 || status == 303 || status == 307 ||
        status == 308) {
      String location = http.getLocation();
      http.end();

      if (location.length() == 0) {
        Serial.println("[Art]   Redirect with no Location, giving up.");
        break;
      }

      String next = resolveRedirect(url, location);
      Serial.printf("[Art]   Redirect %d -> %s\n", status, next.c_str());

      url = next;
      redirectCount++;
      continue;
    }

    if (status != 200) {
      artLastNote = "http " + String(status);
      Serial.printf("[Art]   Failed: %s\n",
                    HTTPClient::errorToString(status).c_str());
      http.end();
      break;
    }

    int contentLength = http.getSize();
    Serial.printf("[Art]   Content-Length: %d\n", contentLength);

    if (contentLength > ART_MAX_SIZE) {
      Serial.println("[Art]   Artwork too large, skipping.");
      http.end();
      break;
    }

    // Stream the body straight to LittleFS to keep RAM use low.
    //
    // The body MUST be read through HTTPClient::getStream() and written with
    // writeToStream(), not pulled off getStreamPtr() directly. The speaker
    // answers with "Transfer-Encoding: chunked" and sends no Content-Length, so
    // the raw socket also delivers the chunk-size lines ("1000\r\n" etc).
    // Copying those verbatim stores chunk headers inside the JPEG and the
    // decoder then fails on a file that is otherwise a valid download - which
    // is what produced "getJpgSize failed" after a reported HTTP 200.
    // writeToStream() is the framework's own de-chunking path.
    FileSink sink(artFile, ART_MAX_SIZE);
    int       copied = http.writeToStream(&sink);

    if (copied < 0) {
      Serial.printf("[Art]   Body read failed: %d\n", copied);
      artLastNote = "read error";
      http.end();
      break;
    }

    receivedOut = sink.written();
    wroteAny    = (receivedOut > 0);

    Serial.printf("[Art]   Wrote %u bytes\n", (unsigned)receivedOut);

    // A JPEG must end with the EOI marker (0xFFD9); anything shorter is a
    // truncated body that would render as noise, so treat it as a failure
    // rather than caching a broken file.
    if (wroteAny && !sink.endedWithEoi()) {
      Serial.println("[Art]   Body is not a complete JPEG (no EOI marker).");
      artLastNote    = "truncated";
      artDownloadError = "no EOI";
      http.end();
      break;
    }

    http.end();

    success = (receivedOut > 0);
    break;
  }

  if (!success && !wroteAny && artLastNote.length() == 0) {
    artLastNote = "no data";
  }

  return success;
}

// Attempts to download and cache the artwork for currentArtUri.
//
// Returns true when the cached file on disk holds the current track's artwork.
// A false return is retryable: the caller must keep trying, because the only other
// trigger for a download is the artwork URI changing. Previously a single failed
// attempt (a transient timeout, or the speaker not yet ready after a track change)
// left the OLD cached image in place with no way to recover, so the display showed
// the previous track's cover indefinitely.
bool downloadArtwork() {
  if (currentArtUri.length() == 0) return false;

  String candidates[2];
  int    count = artworkUrlCandidates(currentArtUri, candidates, 2);

  if (count == 0) {
    artLastStatus = 0;
    artLastRoute  = "UNRESOLVED";
    artLastHost   = "?";
    artLastNote   = "no speaker IP";
    Serial.println("[Art] Could not resolve artwork URL.");
    return false;
  }

  if (candidates[0] == cachedArtUrl) {
    // Already cached on disk. Only ask for a render if that cached file has not been
    // decoded yet (or the screen was cleared since); otherwise this would trigger a
    // pointless slow decode on every poll.
    if (artDrawnRevision != artRevision) artNeedsRender = true;
    return true;
  }

  artLastStatus = 0;
  artLastNote   = "";
  // Cleared at the start of every real attempt so a stale reason from a previous
  // track cannot be shown next to this track's status code.
  artDownloadError = "";

  Serial.println("[Art] ====================================");
  Serial.printf("[Art] Candidate routes: %d\n", count);

  File artFile = LittleFS.open(ART_FILE_PATH, FILE_WRITE);
  if (!artFile) {
    Serial.println("[Art] Failed to open /art.jpg for writing.");
    return false;
  }

  bool   success  = false;
  bool   wroteAny = false;
  size_t received = 0;
  String usedUrl;

  for (int i = 0; i < count; i++) {
    String url = candidates[i];

    // Label the route for the log and the on-screen panel.
    artLastRoute = (url.startsWith("http://" + activeSonosIP.toString()))
                       ? "PROXY"
                       : "DIRECT";
    {
      int schemeEnd = url.indexOf("://");
      if (schemeEnd >= 0) {
        int hostStart = schemeEnd + 3;
        int hostEnd   = url.indexOf('/', hostStart);
        if (hostEnd < 0) hostEnd = url.length();
        artLastHost = url.substring(hostStart, hostEnd);
      } else {
        artLastHost = "?";
      }
    }

    Serial.printf("[Art] Trying route %d/%d: %s via %s\n", i + 1, count,
                  artLastRoute.c_str(), artLastHost.c_str());
    Serial.printf("[Art]   URL: %s\n", url.c_str());

    received = 0;
    if (fetchArtworkFrom(url, artFile, received)) {
      success  = true;
      wroteAny = (received > 0);
      usedUrl  = url;
      break;
    }

    Serial.println("[Art]   Route yielded no data, trying next.");
    artLastNote = "";
  }

  artFile.close();

  if (success) {
    cachedArtUrl   = usedUrl;
    artNeedsRender = true;
    artRevision++;  // cached file changed, so a re-decode is genuinely needed
    artLastNote    = String(received) + " bytes";
    Serial.printf("[Art] Cached via %s (%u bytes)\n", artLastRoute.c_str(),
                  (unsigned)received);
  } else {
    // Partial or empty download: discard so a truncated JPEG is never drawn.
    // The cachedArtUrl is deliberately NOT updated, so the next retry will fetch again.
    LittleFS.remove(ART_FILE_PATH);
    if (wroteAny) {
      artLastNote = "partial";
      Serial.println("[Art] Partial download, cached file removed.");
    } else {
      if (artLastNote.length() == 0) artLastNote = "no data";
      Serial.printf("[Art] All %d route(s) failed, will retry.\n", count);
    }
  }

  Serial.println("[Art] ====================================");
  return success;
}

// TJpg_Decoder callback: pushes each decoded block into the artwork area.
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *data) {
  // Clip against the artwork region so nothing spills into the text area.
  int32_t dx = x;
  int32_t dy = y;
  int32_t dw = w;
  int32_t dh = h;

  if (dx < ART_AREA_X) { dw += dx - ART_AREA_X; dx = ART_AREA_X; }
  if (dy < ART_AREA_Y) { dh += dy - ART_AREA_Y; dy = ART_AREA_Y; }
  if (dx + dw > ART_AREA_X + ART_AREA_W) { dw = ART_AREA_X + ART_AREA_W - dx; }
  if (dy + dh > ART_AREA_Y + ART_AREA_H) { dh = ART_AREA_Y + ART_AREA_H - dy; }

  if (dw <= 0 || dh <= 0) return true;  // fully clipped

  // Advance the source pointer past any horizontally clipped pixels.
  if (x < ART_AREA_X) data += (ART_AREA_X - x);

  tft.pushImage(dx, dy, dw, dh, data);
  return true;
}

// ---------------------------------------------------------------------------
// JPEG decoding
//
// Two decoders, selected by the frame type reported by jpegInfo():
//
//   baseline    -> esp_jpg_decode()  (ESP-IDF ROM TJpgDec). Fast and streams the
//                  cached file through a random-access reader, so peak RAM is the
//                  decoder's own working set rather than the size of the image.
//   progressive -> JPEGDEC. Required because Sonos serves progressive artwork for
//                  local library tracks and TJpgDec rejects SOF2 outright with
//                  JDR_FMT3 ("may be a progressive JPEG image"). JPEGDEC is also
//                  fed the file through open(File&, ...), so the image stays on
//                  LittleFS instead of being pulled into RAM.
//
// Both paths converge on tft_output(), which clips to the artwork region, so
// neither decoder can overwrite the text below.
// ---------------------------------------------------------------------------

// Offset applied to progressive renders, which are decoded from the image's own
// top-left corner rather than at a caller-supplied position.
static int32_t artDestX = 0;
static int32_t artDestY = 0;

// esp_jpg_decode's reader is random access: it is handed an offset and a length
// and served synchronously, so the JPEG never has to be resident in RAM as one
// block. That matters here, because this ESP32 has ~320 KB of internal DRAM, no
// PSRAM, and only about 70 KB of BSS headroom left -- far less than the 131 KB
// a local-library cover image occupies. Pointing the reader at the cached file
// keeps peak RAM to the decoder's own working set.
struct JpgSource {
  File   *file;   // cached JPEG, positioned on demand
  size_t  size;   // total bytes available
};

static size_t jpg_read_cb(void *arg, size_t index, uint8_t *buf, size_t len) {
  JpgSource *src = static_cast<JpgSource *>(arg);
  if (!src || !src->file || !buf) return 0;

  if (index >= src->size) return 0;  // nothing left
  size_t avail = src->size - index;
  if (len > avail) len = avail;
  if (len == 0) return 0;

  if (!src->file->seek(index)) return 0;

  // Loop, because the filesystem may satisfy a large request with a short read.
  // Returning a short count here is what produced the old "Read Fail at
  // 6/131716"; the decoder treats it as a hard error.
  size_t done = 0;
  while (done < len) {
    int got = src->file->read(buf + done, len - done);
    if (got <= 0) break;  // error or EOF
    done += (size_t)got;
  }
  return done;
}

// Writer callback: receives one decoded block as RGB565 bytes.
//
// esp_jpg_decode emits blocks at image coordinates starting at 0,0, so the
// centred destination offset is added here before the shared clipping/drawing
// helper is used.
static bool jpg_write_cb(void *arg, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        uint8_t *data) {
  (void)arg;
  return tft_output(static_cast<int16_t>(x + artDestX), static_cast<int16_t>(y + artDestY),
                    w, h, reinterpret_cast<uint16_t *>(data));
}

// Maps the 1/1..1/8 reduction factor onto esp_jpg's scale enum.
static jpg_scale_t scaleIdxEsp(uint8_t factor) {
  switch (factor) {
    case 2:  return JPG_SCALE_2X;
    case 4:  return JPG_SCALE_4X;
    case 8:  return JPG_SCALE_8X;
    default: return JPG_SCALE_NONE;
  }
}

// Reads the JPEG's start-of-frame marker and reports whether it is progressive,
// along with the frame width and height. Returns false when no SOF is found.
//
// Markers are walked strictly in order: the two bytes after a marker are its
// segment length, so skipping that many bytes lands exactly on the next marker
// and a marker split across two reads can never be misread.
static bool jpegInfo(File &f, bool &progressive, uint16_t &w, uint16_t &h) {
  progressive = false;
  w = 0;
  h = 0;

  // Expect the start-of-image marker.
  uint8_t soi[2];
  if (f.read(soi, 2) != 2 || soi[0] != 0xFF || soi[1] != 0xD8) return false;

  while (f.position() < 64 * 1024) {
    // Find the next 0xFF that is not a fill byte.
    uint8_t b;
    do {
      if (f.read(&b, 1) != 1) return false;
    } while (b != 0xFF);

    uint8_t marker;
    do {
      if (f.read(&marker, 1) != 1) return false;
    } while (marker == 0xFF);

    // Standalone markers carry no payload.
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }
    if (marker == 0xD9) return false;  // end of image before any frame

    uint8_t lenBuf[2];
    if (f.read(lenBuf, 2) != 2) return false;
    uint16_t segLen = (uint16_t)((lenBuf[0] << 8) | lenBuf[1]);
    if (segLen < 2) return false;

    // SOF0 (baseline) through SOF15, excluding the marker-less ones.
    bool isSof = (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 &&
                 marker != 0xC8 && marker != 0xCC;
    if (isSof) {
      if (segLen < 8) return false;

      uint8_t frame[7];  // precision, height(2), width(2), components, ...
      if (f.read(frame, sizeof(frame)) != sizeof(frame)) return false;

      progressive = (marker == 0xC2);
      h            = (uint16_t)((frame[1] << 8) | frame[2]);
      w            = (uint16_t)((frame[3] << 8) | frame[4]);
      return (w > 0 && h > 0);
    }

    // Skip the rest of this segment's payload.
    if (!f.seek(f.position() + (segLen - 2))) return false;
  }

  return false;
}

// Decodes the cached JPEG with esp_jpg_decode (BASELINE only) and draws it.
// Returns the esp_err_t from esp_jpg_decode.
//
// This cannot handle a progressive frame: jd_prepare() returns JDR_FMT3, which
// esp_jpg_decode maps to ESP_FAIL. renderArtwork() routes progressive files to
// renderCachedJpegProgressive() instead.
static esp_err_t renderCachedJpeg(const char *path, jpg_scale_t scale) {
  File f = LittleFS.open(path, "r");
  if (!f) return ESP_FAIL;

  size_t size = (size_t)f.size();
  if (size == 0) {
    f.close();
    return ESP_FAIL;
  }

  // The file stays open for the whole decode and is served to the decoder
  // through the random-access reader, so peak RAM is the decoder's own working
  // set rather than the size of the image.
  JpgSource src   = {&f, size};
  esp_err_t err   = esp_jpg_decode(size, scale, jpg_read_cb, jpg_write_cb, &src);

  f.close();
  return err;
}

// JPEGDEC file callbacks over a LittleFS File.
//
// The library's convenience overloads are deliberately not used:
//   - open(File&, cb) sits behind #ifdef FS_H. On an ESP_PLATFORM build JPEGDEC.h
//     takes its first include branch, which does not include <FS.h>, so FS_H is never
//     defined while JPEGDEC.cpp compiles. The overload is therefore omitted from
//     libJPEGDEC.a and the link fails with an undefined reference. That block also
//     contains a latent bug (FileClose dereferences a void* as handle->fHandle), so
//     it evidently has not been compiled on ESP_PLATFORM.
//   - openRAM/openFLASH need the whole JPEG resident in RAM. A local-library cover is
//     ~138 KB, which does not fit alongside WiFi and the display buffer on a board
//     with ~320 KB DRAM and no PSRAM.
//
// The raw-handle overload is compiled unconditionally. These callbacks mirror the
// library's own internal file adapters, so the image is streamed from LittleFS and
// never resident in RAM.
static int32_t jpegdecRead(JPEGFILE *handle, uint8_t *buffer, int32_t length) {
  if (handle == nullptr || handle->fHandle == nullptr || buffer == nullptr) return 0;
  File *f = static_cast<File *>(handle->fHandle);
  return static_cast<int32_t>(f->read(buffer, static_cast<size_t>(length)));
}

static int32_t jpegdecSeek(JPEGFILE *handle, int32_t position) {
  if (handle == nullptr || handle->fHandle == nullptr) return 0;
  File *f = static_cast<File *>(handle->fHandle);
  return static_cast<int32_t>(f->seek(static_cast<uint32_t>(position)));
}

static void jpegdecClose(void *handle) {
  if (handle == nullptr) return;
  static_cast<File *>(handle)->close();
}

// JPEGDEC draw callback.
//
// PIXEL BYTE ORDER: JPEGDEC must be set to RGB565_BIG_ENDIAN, not its
// RGB565_LITTLE_ENDIAN default. TFT_eSPI's pushImage(uint16_t*) streams the colour
// bytes most-significant-first (setSwapBytes() is false by default), so the decoder
// has to pre-swap each 16-bit value for the display to receive it correctly. Every
// JPEGDEC example that drives an SPI LCD does the same, with the comment "the LCD
// wants the 16-bit pixels in big-endian order". Leaving it little-endian swaps the
// red and blue byte of every pixel, which washes the artwork out and turns fine
// detail magenta/cyan.
//
// iWidthUsed vs iWidth: at the right-hand edge of the image a block can be narrower
// than the row pitch (iWidth). Only the first iWidthUsed pixels of each row are valid;
// the remainder is stale. Those rows are therefore pushed one at a time, because
// pushImage() assumes a contiguous w*h block and would otherwise read past the valid
// data and shear the block.
//
// artDestX/artDestY are zero on this path: JPEGDEC is given an absolute destination in
// decode(), so its blocks already carry final screen coordinates and no offset is
// added here. The shared globals are kept only so tft_output() can be reused verbatim
// for clipping — it is the same helper the baseline path uses.
static int jpegdecDrawMCUs(JPEGDRAW *pDraw) {
  if (pDraw == nullptr) return 0;

  int16_t  x    = static_cast<int16_t>(pDraw->x + artDestX);
  int16_t  y    = static_cast<int16_t>(pDraw->y + artDestY);
  uint16_t w    = static_cast<uint16_t>(pDraw->iWidth);
  uint16_t h    = static_cast<uint16_t>(pDraw->iHeight);
  uint16_t used = static_cast<uint16_t>(pDraw->iWidthUsed);

  if (used > 0 && used < w) {
    // Edge block: rows are still `w` apart, so push each valid row separately.
    for (uint16_t row = 0; row < h; row++) {
      tft_output(x, static_cast<int16_t>(y + row), used, 1,
                 pDraw->pPixels + static_cast<size_t>(row) * w);
    }
  } else {
    tft_output(x, y, w, h, pDraw->pPixels);
  }

  // Returning 1 tells JPEGDEC to keep going; 0 would abort the decode.
  return 1;
}

// Maps the 1/1..1/8 reduction factor onto JPEGDEC's scale flags.
static int scaleFlagsJpegdec(uint8_t factor) {
  switch (factor) {
    case 2:  return JPEG_SCALE_HALF;
    case 4:  return JPEG_SCALE_QUARTER;
    case 8:  return JPEG_SCALE_EIGHTH;
    default: return 0;
  }
}

// Decodes the cached PROGRESSIVE JPEG with JPEGDEC and draws it. Returns true on
// success.
//
// JPEGDEC is instantiated on the heap rather than as a static global because its
// internal state is ~18 KB. Allocating it statically would permanently consume that
// much DRAM on a board with ~320 KB and no PSRAM; heap allocation means it is only
// resident while an image is decoding.
//
// The raw-handle overload is used rather than open(File&, ...) because the latter is
// excluded from the compiled library on ESP_PLATFORM builds (see the callback notes
// above). Either way the file is read through the File, so the ~138 KB image stays in
// LittleFS instead of being pulled into RAM.
static bool renderCachedJpegProgressive(const char *path, uint8_t scaleFactor) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  if (f.size() == 0) {
    f.close();
    return false;
  }

  JPEGDEC *jpeg = new (std::nothrow) JPEGDEC();
  if (jpeg == nullptr) {
    Serial.println("[Art] Out of memory allocating JPEGDEC.");
    artRenderError = "no RAM";
    f.close();
    return false;
  }

  Serial.printf("[Art] Free heap before decode: %u bytes\n",
                (unsigned)ESP.getFreeHeap());

  bool ok = false;

  if (jpeg->open(&f, static_cast<int>(f.size()),
                 jpegdecClose, jpegdecRead, jpegdecSeek, jpegdecDrawMCUs)) {
    // Log the decoder's own view of the file so a mismatch with jpegInfo() is
    // visible, and so the frame type actually parsed is on record.
    Serial.printf("[Art] JPEGDEC opened: %d x %d, mode=%s\n",
                  jpeg->getWidth(), jpeg->getHeight(),
                  jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE ? "progressive"
                                                               : "baseline");

    // Big endian, NOT the library default. TFT_eSPI pushes the colour bytes
    // most-significant-first, so leaving this little-endian swaps the red and blue
    // byte of every pixel. See the note on jpegdecDrawMCUs().
    jpeg->setPixelType(RGB565_BIG_ENDIAN);

    // decode() takes the destination position in output (post-scale) pixels, so the
    // centred offset is computed here and the callback stays a straight pass-through.
    uint16_t outW = static_cast<uint16_t>(jpeg->getWidth()  / scaleFactor);
    uint16_t outH = static_cast<uint16_t>(jpeg->getHeight() / scaleFactor);

    // JPEGDEC positions blocks itself, so no offset is applied in the callback.
    artDestX = 0;
    artDestY = 0;

    uint16_t destX = static_cast<uint16_t>(ART_AREA_X + (ART_AREA_W - outW) / 2);
    uint16_t destY = static_cast<uint16_t>(ART_AREA_Y + (ART_AREA_H - outH) / 2);

    ok = jpeg->decode(destX, destY, scaleFlagsJpegdec(scaleFactor)) != 0;

    if (!ok) {
      Serial.printf("[Art] JPEGDEC decode failed, error=%d\n", jpeg->getLastError());
      artRenderError = "prog decode fail";
    }

    // jpeg->close() closes the File it was opened with, so f must not be closed
    // again on this path.
    jpeg->close();
  } else {
    Serial.println("[Art] JPEGDEC could not parse the JPEG header.");
    artRenderError = "no header";
    f.close();
  }

  Serial.printf("[Art] Free heap after decode: %u bytes\n",
                (unsigned)ESP.getFreeHeap());

  delete jpeg;
  return ok;
}

// Decodes and draws the cached artwork, scaled to fit the artwork area.
void renderArtwork() {
  if (!artNeedsRender) return;
  artNeedsRender = false;

  if (!screenPowered) return;

  // Skip the decode when the artwork already on screen is current. Drawing a toast or
  // waking the display sets artNeedsRender too, but neither changes the cached file, and
  // re-decoding a progressive JPEG is slow enough to stall the main loop.
  if (artDrawnRevision == artRevision) return;
  artDrawnRevision = artRevision;

  tft.fillRect(ART_AREA_X, ART_AREA_Y, ART_AREA_W, ART_AREA_H, TFT_BLACK);

  artRenderError = "";  // cleared: this render is about to decide the outcome

  if (!LittleFS.exists(ART_FILE_PATH)) {
    Serial.println("[Art] No cached artwork, drawing placeholder.");
    artRenderError = "not cached";
    drawArtworkPlaceholder();
    return;
  }

  // Inspect the file's own header for the frame size. Only the marker segment is
  // read, so this is cheap and works for progressive and baseline alike.
  File     infoFile = LittleFS.open(ART_FILE_PATH, "r");
  bool     progressive = false;
  uint16_t w = 0, h = 0;
  bool     haveInfo = false;

  if (infoFile) {
    haveInfo = jpegInfo(infoFile, progressive, w, h);
    infoFile.close();
  }

  if (!haveInfo) {
    Serial.println("[Art] Could not read JPEG frame header.");
    artRenderError = "no header";
    drawArtworkPlaceholder();
    return;
  }

  Serial.printf("[Art] JPEG dimensions: %u x %u (%s)\n", w, h,
                progressive ? "progressive" : "baseline");

  // Pick the smallest reduction factor that fits the artwork area, keeping
  // the image as large as possible. esp_jpg_decode only offers 1/1..1/8.
  //
  // Note for progressive images: JPEGDEC decodes only the first (DC) scan and forces
  // its own 1/8 scaling, so the picture it produces is a low-detail thumbnail rather
  // than a full decode, and the effective size may differ from outW/outH below. It is
  // drawn regardless so something sensible is shown; see ARTWORK-ATTEMPTS.md (attempt
  // E) for why this is not a real fix.
  uint8_t  scaleFactor = 1;
  uint16_t outW = w, outH = h;
  while ((outW > ART_AREA_W || outH > ART_AREA_H) && scaleFactor < 8) {
    scaleFactor *= 2;
    outW = w / scaleFactor;
    outH = h / scaleFactor;
  }

  if (outW > ART_AREA_W) outW = ART_AREA_W;
  if (outH > ART_AREA_H) outH = ART_AREA_H;

  Serial.printf("[Art] Rendering at 1/%u -> %u x %u\n", scaleFactor, outW, outH);

  // Both decoders consume blocks through tft_output(), which clips to the artwork
  // region. The centred destination offset differs per decoder: esp_jpg_decode emits
  // at image coordinates from 0,0 so its writer callback adds the offset, whereas
  // JPEGDEC takes the destination directly in decode(). artDestX/Y are zero for the
  // JPEGDEC path.
  //
  // Which decoder runs is decided by the frame type jpegInfo() found, not by trying
  // one and falling back: esp_jpg_decode is baseline-only and JPEGDEC handles both,
  // so progressive images must not be sent to esp_jpg_decode at all.
  bool rendered;

  if (progressive) {
    Serial.printf("[Art] Progressive frame -> JPEGDEC at 1/%u\n", scaleFactor);
    rendered = renderCachedJpegProgressive(ART_FILE_PATH, scaleFactor);
  } else {
    artDestX = ART_AREA_X + (ART_AREA_W - outW) / 2;
    artDestY = ART_AREA_Y + (ART_AREA_H - outH) / 2;

    Serial.printf("[Art] Baseline frame -> esp_jpg_decode at 1/%u\n", scaleFactor);
    rendered = (renderCachedJpeg(ART_FILE_PATH, scaleIdxEsp(scaleFactor)) == ESP_OK);
  }

  if (!rendered) {
    Serial.println("[Art] Decode failed, drawing placeholder.");
    // Distinguish the three ways a decode can fail on screen: the decoder could
    // not be allocated, could not parse the header, or parsed it and then failed.
    // They have completely different fixes, so they must not share one label.
    if (artRenderError.length() == 0) {
      artRenderError = progressive ? "prog decode fail" : "decode fail";
    }
    drawArtworkPlaceholder();
    return;
  }

  Serial.println("[Art] Render complete.");
}

// ---------------------------------------------------------------------------
// Sonos polling
// ---------------------------------------------------------------------------

// Queries the active speaker and updates the current track state.
void pollSonos() {
  if (activeSonosIP[0] == 0) return;

  // ---- GetTransportInfo ---------------------------------------------------
  // <CurrentTransportState> drives the status badge and the idle timeout:
  // PLAYING, PAUSED_PLAYBACK or STOPPED.
  String transportBody =
      sonosSoap("GetTransportInfo", "<InstanceID>0</InstanceID>");

  if (transportBody.length() == 0) {
    // Speaker did not answer; release it so rediscovery can start again.
    Serial.println("[Sonos] No response, releasing active speaker.");
    activeSonosIP = IPAddress(0, 0, 0, 0);
    return;
  }

  String state =
      xmlUnescape(extractXmlValue(transportBody, "CurrentTransportState"));
  if (state.length() > 0) currentTransportState = state;

  // ---- Follow whichever speaker is actually playing ----------------------
  //
  // A house can have several speakers, and only one of them is usually
  // playing. Whichever answered SSDP first gets adopted, which can easily be an
  // idle speaker in another room: it answers with a short response carrying no
  // track metadata, so the display shows nothing even though music is playing
  // elsewhere. When the active speaker is idle, probe the other known speakers
  // and switch to one that is playing.
  if (state == "STOPPED" && sonosDeviceCount > 1) {
    if (millis() - lastSpeakerScanTime > SPEAKER_SCAN_INTERVAL_MS) {
      lastSpeakerScanTime = millis();

      for (int i = 0; i < sonosDeviceCount; i++) {
        if (sonosDevices[i] == activeSonosIP) continue;

        String probe = sonosSoapTo(sonosDevices[i], "GetTransportInfo",
                                   "<InstanceID>0</InstanceID>", false);
        if (probe.length() == 0) continue;

        String otherState =
            xmlUnescape(extractXmlValue(probe, "CurrentTransportState"));
        if (otherState == "PLAYING" || otherState == "PAUSED_PLAYBACK") {
          Serial.printf("[Sonos] Switching to playing speaker at %s (%s)\n",
                        sonosDevices[i].toString().c_str(), otherState.c_str());
          activeSonosIP = sonosDevices[i];
          cachedArtUrl  = String();  // force an artwork fetch from the new speaker
          // Re-arm the pending fetch so the artwork is actually re-downloaded from the
          // new speaker; downloadArtwork() will otherwise short-circuit on the old cache.
          artworkPending = currentArtUri.length() > 0;
          invalidateMetadata();
          return;  // poll again on the next interval to read its track info
        }
      }
    }
  }

  // ---- GetPositionInfo ---------------------------------------------------
  // Renders the title, artist, elapsed/total times and the artwork.
  //
  //   <Track>                 -> currentTitle   (title row, font 2)
  //   <TrackArtist>           -> currentArtist  (artist row, font 1)
  //   <RelTime>               -> currentRelTime (elapsed, left of bar)
  //   <TrackDuration>         -> currentDuration(total, right of bar)
  //   <CurrentTrackMetaData>  -> albumArtURI    (artwork area, y=0..83)
  String positionBody =
      sonosSoap("GetPositionInfo", "<InstanceID>0</InstanceID>");

  if (positionBody.length() == 0) {
    Serial.println("[Sonos] GetPositionInfo returned no data.");
    return;
  }

  // Capture the body for the first few polls after boot. This replaced a
  // one-shot dump of the first 1400 bytes, which could not distinguish the two
  // candidate causes: a 1400-byte prefix of a ~1500-byte body looks identical
  // whether the response ended at 1503 bytes or was cut at 400.
  //
  // logPositionInfoBody() prints the element-presence map plus the head AND the
  // tail, so "how far did the body get" is answerable at a glance. It is capped
  // at SOAP_CAPTURE_COUNT polls because a full body dump every second is a wall
  // of text that hides the lines that matter.
  static int captureCountdown = SOAP_CAPTURE_COUNT;
  bool capturing = (captureCountdown > 0);
  if (capturing) captureCountdown--;

  if (capturing) {
    Serial.printf("[Sonos] ===== GetPositionInfo capture %d/%d =====\n",
                  SOAP_CAPTURE_COUNT - captureCountdown, SOAP_CAPTURE_COUNT);
    logPositionInfoBody(positionBody);
  }

  // Metadata first, because it is the authoritative source
  // Title and artist are extracted by the same helper with the same rules, so
  // neither field can behave differently from the other:
  //
  //   soapField(positionBody, "Track")       -> currentTitle
  //   soapField(positionBody, "TrackArtist") -> currentArtist
  // ---- Metadata first, because it is the authoritative source ----------
  //
  // GetPositionInfo's <Track> element does NOT reliably hold the song title:
  // for queue/playlist playback Sonos returns the queue track number in it
  // (e.g. "3"). The real title and artist live in the DIDL-Lite blob carried
  // by CurrentTrackMetaData as <dc:title> and <dc:creator>.
  //
  // The blob arrives XML-escaped (&lt;dc:title&gt;...), so it must be
  // unescaped before any tag inside it can be matched.
  String metaData =
      xmlUnescape(extractByLocalName(positionBody, "CurrentTrackMetaData"));

  if (metaData.length() == 0) {
    // Older firmware uses TrackMetaData instead.
    metaData = xmlUnescape(extractByLocalName(positionBody, "TrackMetaData"));
  }

  // Report the blob and the three elements taken from it. A zero-length blob
  // means the metadata never arrived and no amount of parser work will help; a
  // populated blob with a missing dc:title points at extraction instead. See
  // logDidlState() for how to read this line.
  if (capturing) logDidlState(metaData);

  // Title and artist are resolved by this one function, so neither field can
  // be handled differently from the other. The DIDL element is preferred and
  // the AVTransport element is only a fallback; for the title this ordering is
  // what stops the queue track number from being shown in its place.
  String title  = resolveTrackField(metaData, "title",  positionBody, "Track");
  String artist = resolveTrackField(metaData, "creator", positionBody, "TrackArtist");

  // The README requires any '/getaa' continuation to be excluded from the
  // artist. Applied after resolution so it cannot affect the title.
  artist = sanitizeArtist(artist);

  logSoapField("<Track>/dc:title", title);
  logSoapField("<TrackArtist>/dc:creator", artist);

  currentRelTime =
      parseTimeToSeconds(extractXmlValue(positionBody, "RelTime"));
  currentDuration =
      parseTimeToSeconds(extractXmlValue(positionBody, "TrackDuration"));

  // ---- Artwork ----------------------------------------------------------
  // Read from the same metadata blob, matched by local name so upnp:, r: and
  // unprefixed forms all match.
  //
  // xmlUnescapeDeep() is required here, not the single pass used for the text
  // fields: the artwork URI carries its own query string ("?s=1&u=..."), and that
  // "&" is escaped a second time by the SOAP envelope. Unescaping only once
  // leaves "&amp;" in the URL, the speaker cannot find the "u" parameter and
  // answers 404 - which is exactly what the PROXY 404 on the diagnostic panel
  // was reporting.
  String artUri;

  if (metaData.length() > 0) {
    artUri = xmlUnescapeDeep(extractByLocalName(metaData, "albumArtURI"));
  }

  if (artUri.length() == 0) {
    artUri = xmlUnescapeDeep(extractByLocalName(positionBody, "CurrentTrackArtImage"));
  }

  // Last resort: some local/native content exposes the image through <res>.
  // Guarded by an image-extension check, because <res> normally holds the
  // AUDIO STREAM (x-file-cifs://, x-rincon-mp3radio:// ...) and downloading
  // that as artwork would waste bandwidth and always fail to decode.
  if (artUri.length() == 0 && metaData.length() > 0) {
    String resUri = xmlUnescapeDeep(extractByLocalName(metaData, "res"));
    resUri.trim();
    if (resUri.endsWith(".jpg") || resUri.endsWith(".jpeg") ||
        resUri.endsWith(".png") || resUri.endsWith(".gif")) {
      artUri = resUri;
    } else if (resUri.length() > 0) {
      Serial.println("[Sonos] <res> is not an image, ignoring for artwork.");
    }
  }

  artUri.trim();

  Serial.printf("[Sonos] Title=\"%s\" Artist=\"%s\" RelTime=%u Duration=%u Art=\"%s\"\n",
                title.c_str(), artist.c_str(), (unsigned)currentRelTime,
                (unsigned)currentDuration, artUri.c_str());

  if (artUri.length() == 0) {
    Serial.println("[Sonos] No album art URI found in track metadata.");
  }

  // Only re-render when the track or the artwork actually changed.
  //
  // artNeedsRender must NOT be set here. A progressive JPEG is only ever decoded as a
  // DC-scan thumbnail (JPEGDEC decodes the first scan and forces 1/8 scale), which is
  // slow enough to stall the main loop. Re-arming it on every title/artist change made
  // renderArtwork() run again on each 1 s poll, starving the poll, the buttons and the
  // idle timer — the screen froze on a stale snapshot.
  //
  // The artwork re-render is triggered only by downloadArtwork(), when the cached file
  // actually changes (new URI, or a re-fetch after a speaker switch). drawScreen()
  // clears and redraws the artwork region whenever it needs to, via renderArtwork()'s
  // own screen-clearing, so a plain track change does not need a re-decode.
  if (title != currentTitle || artist != currentArtist) {
    currentTitle   = title;
    currentArtist  = artist;

    // Report the title with its length and first bytes, so a title that
    // parses but renders as nothing can be identified from the log.
    Serial.printf("[Sonos] Title changed: len=%d raw=\"%s\" bytes=",
                  currentTitle.length(), currentTitle.c_str());
    for (unsigned int i = 0; i < currentTitle.length() && i < 6; i++) {
      Serial.printf("%02X ", (unsigned char)currentTitle.charAt(i));
    }
    Serial.println();
  }

  if (artUri != currentArtUri) {
    // A new track (or a newly available artwork URI). Arm a delayed fetch rather than
    // downloading inline: immediately after a track change the speaker is often still
    // settling, and an early request frequently times out. The artwork retry below then
    // keeps trying until it succeeds.
    currentArtUri    = artUri;
    artworkPending   = artUri.length() > 0;
    artworkFirstTry  = millis() + ARTWORK_INITIAL_DELAY_MS;

    if (artUri.length() == 0) {
      Serial.println("[Sonos] No artwork URI in response for this track.");
    } else {
      // Show both forms: what the response contained, and what will actually
      // be fetched. For a relative path the two differ by scheme, host and
      // port, which is exactly what needs checking when a download fails.
      Serial.printf("[Sonos] Artwork as sent: %s\n", artUri.c_str());
      Serial.printf("[Sonos] Artwork resolved: %s\n",
                    resolveArtworkUrl(artUri).c_str());
    }
  }

  // Keep retrying until the artwork for this track is on disk. Without this, a single
  // failed attempt was permanent: the only other trigger was the URI changing, so a
  // transient timeout left the PREVIOUS track's cover on screen indefinitely.
  if (artworkPending && (long)(artworkFirstTry - millis()) <= 0) {
    if (downloadArtwork()) {
      artworkPending = false;
    } else {
      artworkFirstTry = millis() + ARTWORK_RETRY_INTERVAL_MS;
      Serial.printf("[Art] Retrying in %d ms.\n", ARTWORK_RETRY_INTERVAL_MS);
    }
  }

  bool wasPlaying = isPlaying;
  isPlaying = (currentTransportState == "PLAYING");

  if (isPlaying && !wasPlaying) {
    Serial.println("[Sonos] Playback started.");
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Shortens text to fit the given pixel width, appending an ellipsis.
String ellipsize(const String &text, int maxWidth, uint8_t font) {
  if (maxWidth <= 0) return String();
  if (tft.textWidth(text, font) <= maxWidth) return text;

  String out = text;
  while (out.length() > 1 && tft.textWidth(out + "...", font) > maxWidth) {
    out.remove(out.length() - 1);
  }
  return out + "...";
}

// Draws one metadata text row: title, artist or any future field.
//
// Title and artist both go through this one function, so neither can behave
// differently from the other. It ellipsises to the available width, and if the
// value is empty (or measures zero pixels in the requested font, which the
// bundled fonts can do for some byte sequences) it draws a short placeholder
// instead, so a missing value is always visible on screen rather than silently
// blank.
void drawTextRow(const String &text, uint8_t font, uint16_t color, int16_t y,
                 const char *placeholder) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color);

  if (text.length() == 0) {
    tft.setTextFont(1);
    tft.drawString(placeholder, META_X, y + 4);
    return;
  }

  String row = ellipsize(text, 232, font);

  tft.setTextFont(font);
  if (tft.textWidth(row, font) <= 0) {
    // Not renderable in the requested font; fall back to font 1, which has
    // proven reliable for the other metadata fields.
    Serial.printf("[Display] Row \"%s\" measured 0px in font %d, using font 1.\n",
                  placeholder, font);
    tft.setTextFont(1);
    tft.drawString(ellipsize(text, 232, 1), META_X, y + 4);
    return;
  }

  tft.drawString(row, META_X, y);
}

// Draws the playback state badge in the top right corner.
void drawStatusBadge() {
  if (!screenPowered) return;

  const char *label;
  uint16_t    color;

  if (isPlaying) {
    label = "PLAYING";
    color = TFT_GREEN;
  } else if (currentTransportState == "PAUSED_PLAYBACK") {
    label = "PAUSED";
    color = TFT_ORANGE;
  } else {
    label = "IDLE";
    color = TFT_DARKGREY;
  }

  tft.fillRect(BADGE_X, BADGE_Y, BADGE_W, BADGE_H, color);
  tft.setTextColor(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(label, BADGE_X + BADGE_W - 4, BADGE_Y + 4);
}

// Draws the application version in the top left corner.
//
// Required by README.md. Drawn over the artwork, so it is offset from the
// corner rather than placed flush against the edge.
void drawVersion() {
  if (!screenPowered) return;

  tft.setTextFont(1);
  tft.setTextColor(0x8C8C);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(String("v") + APP_VERSION, VER_X, VER_Y);
}

// Draws text wrapped to the given width, at most maxLines lines.
// Returns the Y position just below the last line drawn.
int drawWrappedText(const String &text, int x, int y, int maxWidth,
                    int maxLines, uint8_t font) {
  int lineY = y;
  int start = 0;

  while (start < (int)text.length() && maxLines > 0) {
    int len = text.length() - start;

    // Shrink until the remainder fits the available width.
    while (len > 0 &&
           tft.textWidth(text.substring(start, start + len), font) > maxWidth) {
      len--;
    }
    if (len <= 0) break;

    tft.drawString(text.substring(start, start + len), x, lineY);

    start += len;
    lineY += tft.fontHeight(font);
    maxLines--;
  }

  return lineY;
}

// Draws the artwork-area placeholder as a large-font status panel.
//
// The route, host and HTTP status are drawn in font 2 (16px) so they can be
// read from a photograph of the display, with the full URLs beneath in font 1
// for detail. A font-1-only panel was unreadable in practice.
void drawArtworkPlaceholder() {
  tft.fillRect(ART_AREA_X, ART_AREA_Y, ART_AREA_W, ART_AREA_H, TFT_BLACK);

  if (currentArtUri.length() == 0) {
    // Nothing in the response at all.
    int cx = ART_AREA_X + ART_AREA_W / 2;
    int cy = ART_AREA_Y + ART_AREA_H / 2;

    tft.fillCircle(cx, cy, 36, TFT_DARKGREY);
    tft.fillCircle(cx, cy, 20, 0x18A0);
    tft.fillCircle(cx, cy, 6, TFT_DARKGREY);

    // RGB565 (TFT_eSPI takes a 16-bit colour; 0xFF8C00 as a 24-bit literal would
    // silently truncate to 0x8C00 and lose the red component).
    tft.setTextColor(0xFD20);  // dark orange
    tft.setTextFont(2);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("NO ART URL", cx, cy + 24);
    tft.setTextFont(1);
    tft.setTextColor(0x6E6E);
    tft.drawString("none in GetPositionInfo", cx, cy + 42);
    return;
  }

  String resolved = resolveArtworkUrl(currentArtUri);

  // ---- Large-font summary -------------------------------------------
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(2);

  // Route line.
  tft.setTextColor(artLastRoute == "DIRECT" ? 0x9CFF9C : 0xFFD08C);
  tft.drawString(artLastRoute.length() > 0 ? artLastRoute : "?", 120,
                 ART_AREA_Y + 14);

  // Status line: HTTP code, or the reason there is no code.
  //
  // A failure reason takes priority over the code. HTTP 200 only means the
  // speaker answered - it is returned for a body with no EOI marker and for a
  // file that then fails to decode, so showing "200" for those made the panel
  // useless for diagnosis: three different faults rendered identically.
  String statusText;
  if (artRenderError.length() > 0)        statusText = artRenderError;
  else if (artDownloadError.length() > 0) statusText = artDownloadError;
  else if (artLastStatus > 0)             statusText = String(artLastStatus);
  else                                    statusText = artLastNote;

  if (statusText.length() == 0) statusText = "...";

  bool healthy = (artRenderError.length() == 0 && artDownloadError.length() == 0 &&
                  artLastStatus == 200);

  tft.setTextColor(healthy ? 0x9CFF9C : 0xFF8080);
  tft.drawString(ellipsize(statusText, ART_AREA_W - 8, 2), 120,
                 ART_AREA_Y + 36);

  // ---- Small-font detail --------------------------------------------
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);

  tft.setTextColor(0x6E6E);
  tft.drawString("host:", ART_AREA_X + 4, ART_AREA_Y + 52);
  tft.setTextColor(0xBDF7);
  tft.drawString(ellipsize(artLastHost, 180, 1), ART_AREA_X + 40,
                 ART_AREA_Y + 52);

  tft.setTextColor(0x6E6E);
  tft.drawString("url:", ART_AREA_X + 4, ART_AREA_Y + 64);
  tft.setTextColor(0xA800);  // mid grey
  tft.drawString(ellipsize(resolved.length() > 0 ? resolved : currentArtUri,
                           ART_AREA_W - 40, 1),
                 ART_AREA_X + 40, ART_AREA_Y + 64);

  tft.setTextColor(0x6E6E);
  tft.drawString("soap:", ART_AREA_X + 4, ART_AREA_Y + 74);
  tft.setTextColor(0x9C48);  // dim grey
  tft.drawString(ellipsize(currentArtUri, ART_AREA_W - 40, 1),
                 ART_AREA_X + 40, ART_AREA_Y + 74);
}

// Clears the display and draws the placeholder for the idle screen.
//
// The text rows are drawn afterwards by drawMetadataBlock(), so this only
// handles the full-screen clear and the artwork area.
void drawNothingPlayingScreen() {
  tft.fillScreen(TFT_BLACK);
  // The clear above wiped the artwork, so invalidate the revision guard so it will be
  // decoded again when playback resumes.
  artDrawnRevision = 0xFFFFFFFF;
  drawArtworkPlaceholder();
  invalidateMetadata();
}

// Draws the connecting / searching screen. Only repaints when the message
// changes, otherwise the whole screen would flicker on every loop pass.
void drawConnectingScreen(const char *message) {
  static String lastMessage;
  static bool   lastInit = false;

  if (lastInit && lastMessage == message) return;

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE);
  tft.drawString(message, 120, 55);

  tft.setTextFont(1);
  tft.setTextColor(0x9E9E);
  tft.drawString(String("v") + APP_VERSION, 120, 80);

  lastMessage = message;
  lastInit    = true;
  invalidateMetadata();
}

// Shows a temporary toast message near the top of the screen.
void showToast(const char *message, uint16_t color) {
  toastMessage    = message;
  toastColor      = color;
  toastExpireTime = millis() + TOAST_DURATION_MS;
  artNeedsRender  = true;  // force a repaint to show the toast
}

// Forces a full metadata repaint on the next drawScreen() call.
// Needed whenever something else clears or overwrites the display.
void invalidateMetadata() {
  metadataDirty = true;
}

// Turns the backlight and display on or off.
void setScreenPower(bool on) {
  if (on == screenPowered) return;

  screenPowered = on;
  digitalWrite(TFT_BL, on ? TFT_BACKLIGHT_ON : !TFT_BACKLIGHT_ON);

  if (on) {
    Serial.println("[Screen] Backlight on.");
    tft.fillScreen(TFT_BLACK);
    lastIdleSecondsRemaining = -1;
    // The fillScreen above wiped the artwork, so it genuinely must be decoded again
    // even though the cached file has not changed.
    artDrawnRevision = 0xFFFFFFFF;
    artNeedsRender   = true;
  } else {
    Serial.println("[Screen] Backlight off.");
    tft.fillScreen(TFT_BLACK);
  }

  invalidateMetadata();
}

// Redraws the metadata block only when something on it has changed.
//
// Clearing and repainting this region on every loop pass caused the lower
// third of the screen to flicker, so the previous values are cached and the
// block is only touched when one of them differs.
static void drawMetadataBlock() {
  static String  lastTitle;
  static String  lastArtist;
  static uint32_t lastRelTime     = 0xFFFFFFFF;
  static uint32_t lastDuration    = 0xFFFFFFFF;
  static int     lastPercent      = -1;
  static bool    lastPlaying      = false;
  static bool    lastIdle         = false;
  static int     lastCountdown    = -1;
  static bool    lastInitialised  = false;
  static String  lastToast;
  static bool    lastToastActive  = false;
#if TITLE_ROW_DIAG
  static bool    lastDiagShowTitle = true;
#endif

  bool  nothingPlaying =
      (currentTransportState == "STOPPED" || currentTitle.length() == 0);
  int   percent   = progressPercent();
  int   countdown = -1;
  bool  toastActive =
      (toastMessage.length() > 0 && millis() < toastExpireTime);

#if TITLE_ROW_DIAG
  // The title/artist swap runs every DIAG_SWAP_MS and is part of the change
  // test below, so the row is genuinely repainted on each swap rather than
  // being skipped by the early return.
  bool showTitle = ((millis() / DIAG_SWAP_MS) % 2) == 0;

  if (showTitle != lastDiagShowTitle) {
    Serial.printf("[Diag] %s\n",
                  showTitle ? "showing TITLE in artist row"
                            : "showing ARTIST in artist row");
  }
#endif

  if (nothingPlaying && isIdle && screenPowered) {
    countdown =
        (int)((IDLE_SCREEN_TIMEOUT_MS - (millis() - idleStartTime)) / 1000);
    if (countdown < 0) countdown = 0;
  }

#if TITLE_ROW_DIAG
  if (lastInitialised && !metadataDirty && showTitle == lastDiagShowTitle &&
      nothingPlaying == lastIdle && currentTitle == lastTitle &&
      currentArtist == lastArtist && currentRelTime == lastRelTime &&
      currentDuration == lastDuration && percent == lastPercent &&
      isPlaying == lastPlaying && countdown == lastCountdown &&
      toastActive == lastToastActive && toastMessage == lastToast) {
    return;  // nothing changed, leave the pixels alone
  }
  lastDiagShowTitle = showTitle;
#else
  if (lastInitialised && !metadataDirty && nothingPlaying == lastIdle &&
      currentTitle == lastTitle && currentArtist == lastArtist &&
      currentRelTime == lastRelTime && currentDuration == lastDuration &&
      percent == lastPercent && isPlaying == lastPlaying &&
      countdown == lastCountdown && toastActive == lastToastActive &&
      toastMessage == lastToast) {
    return;  // nothing changed, leave the pixels alone
  }
#endif

  metadataDirty = false;

  // Clear the whole metadata region so no stale glyphs remain.
  tft.fillRect(0, META_Y, 240, META_H, TFT_BLACK);

  if (nothingPlaying) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("Nothing playing", META_X, TITLE_Y);

    if (currentTitle.length() > 0) {
      drawTextRow(currentTitle, 1, 0x9E9E, ARTIST_Y, "");
    }

    if (countdown >= 0) {
      tft.setTextColor(0x6E6E);
      tft.setTextFont(1);
      tft.drawString(String("Screen off in ") + String(countdown) + "s", META_X,
                     TIME_Y);
    }
  } else {
#if TITLE_ROW_DIAG
    // ---- Diagnostic: title rendered in the artist row, alternating ----
    // Identical x, y and font to the artist row, so the only variable is the
    // string being drawn. A title that appears here proves the value is good
    // and the fault lies in the title row; a title that never appears proves
    // the <Track> value itself is unusable.
    drawTextRow(showTitle ? currentTitle : currentArtist,
                1, showTitle ? TFT_WHITE : 0x9E9E, ARTIST_Y,
                showTitle ? "(no title)" : "(no artist)");
#else
    // Title and artist go through the same renderer, so neither row can
    // behave differently from the other. Only the font and colour differ.
    drawTextRow(currentTitle, 2, TFT_WHITE, TITLE_Y, "(no title)");
    drawTextRow(currentArtist, 1, 0x9E9E, ARTIST_Y, "(no artist)");
#endif

    // Elapsed and total time, either side of the progress bar.
    tft.setTextColor(0xC0C0);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(formatTime(currentRelTime), BAR_X, TIME_Y);

    tft.setTextDatum(TR_DATUM);
    tft.drawString(formatTime(currentDuration), BAR_X + BAR_W, TIME_Y);

    // Progress bar track and fill.
    int fillW = (BAR_W * percent) / 100;

    tft.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, 0x1C1C);
    if (fillW > 0) {
      tft.fillRect(BAR_X, BAR_Y, fillW, BAR_H,
                   isPlaying ? TFT_CYAN : TFT_ORANGE);
    }

    // Position knob, kept inside the bar at both ends.
    if (fillW > 0) {
      tft.fillRect(BAR_X + fillW - 1, BAR_Y - 2, 3, BAR_H + 4, TFT_WHITE);
    }
  }

  // Toast, drawn last so it sits on top while it is active.
  if (toastActive) {
    tft.fillRoundRect(TOAST_X, TOAST_Y, TOAST_W, TOAST_H, 6, toastColor);
    tft.setTextColor(TFT_BLACK);
    tft.setTextFont(1);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(ellipsize(toastMessage, TOAST_W - 10, 1), 120,
                   TOAST_Y + TOAST_H / 2);
  } else if (toastMessage.length() > 0) {
    toastMessage = "";  // expired
  }

  tft.setTextDatum(TL_DATUM);

  lastTitle      = currentTitle;
  lastArtist     = currentArtist;
  lastRelTime    = currentRelTime;
  lastDuration   = currentDuration;
  lastPercent    = percent;
  lastPlaying    = isPlaying;
  lastIdle       = nothingPlaying;
  lastCountdown  = countdown;
  lastToast      = toastMessage;
  lastToastActive = toastActive;
  lastInitialised = true;
}

// Draws the full now-playing screen: artwork, metadata, progress and toast.
void drawScreen() {
  if (!screenPowered) return;

  bool nothingPlaying =
      (currentTransportState == "STOPPED" || currentTitle.length() == 0);

  if (nothingPlaying) {
    // The idle screen owns the whole display; repaint it only on change.
    static String lastIdleTitle;
    static int     lastIdleCountdown = -1;
    static bool    lastIdleInit      = false;

    int countdown =
        (int)((IDLE_SCREEN_TIMEOUT_MS - (millis() - idleStartTime)) / 1000);
    if (countdown < 0) countdown = 0;

    if (lastIdleInit && !metadataDirty && lastIdleTitle == currentTitle &&
        lastIdleCountdown == countdown) {
      return;
    }

    drawNothingPlayingScreen();  // clears the screen, sets metadataDirty
    drawStatusBadge();
    drawVersion();
    drawMetadataBlock();         // draws "Nothing playing" and the countdown

    lastIdleTitle     = currentTitle;
    lastIdleCountdown = countdown;
    lastIdleInit      = true;
    return;
  }

  // Artwork occupies the top 84 rows; render into it if it changed.
  renderArtwork();

  // Badge and version sit over the artwork, so redraw them each pass.
  drawStatusBadge();
  drawVersion();

  drawMetadataBlock();
}

// ---------------------------------------------------------------------------
// Serial console
// ---------------------------------------------------------------------------
//
// Single-key commands on the monitor UART.
//
// Every question raised during hardware bring-up used to cost a rebuild and a
// reflash, which is what turned debugging into a loop: the only way to test an
// idea was to commit it to the firmware first, so each hypothesis produced a
// build rather than an observation. These commands let the running device
// answer questions on demand, so an experiment no longer requires a change.
//
//   d  dump GetPositionInfo now (element map, head, tail, DIDL state)
//   x  delete the cached /art.jpg (it survives a reflash)
//   f  re-arm the artwork fetch for the current track
//   m  heap, WiFi RSSI and cached artwork size
//   h  this list
//
// Non-blocking by construction: it only drains bytes that have already arrived
// and never waits for more, so it cannot stall the poll, the buttons or the
// idle timer.
void handleSerialCommands() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;

    switch (c) {
      case 'd': case 'D': {
        Serial.println("[Cmd] Requesting GetPositionInfo...");
        String body = sonosSoap("GetPositionInfo", "<InstanceID>0</InstanceID>");

        if (body.length() == 0) {
          Serial.println("[Cmd] No response from the speaker.");
          break;
        }

        logPositionInfoBody(body);

        String meta =
            xmlUnescape(extractByLocalName(body, "CurrentTrackMetaData"));
        if (meta.length() == 0) {
          meta = xmlUnescape(extractByLocalName(body, "TrackMetaData"));
        }
        logDidlState(meta);
        break;
      }

      case 'x': case 'X':
        // /art.jpg outlives a reflash, so clearing it is the only way to test
        // the download path with a cold cache.
        if (LittleFS.remove(ART_FILE_PATH)) {
          Serial.println("[Cmd] Removed /art.jpg.");
        } else {
          Serial.println("[Cmd] No /art.jpg to remove.");
        }
        cachedArtUrl = String();
        artRevision++;
        break;

      case 'f': case 'F':
        // Re-arm the fetch even though the URI is unchanged, which
        // downloadArtwork() would otherwise short-circuit on.
        cachedArtUrl   = String();
        artworkPending = (currentArtUri.length() > 0);
        artworkFirstTry = millis();
        Serial.println("[Cmd] Artwork fetch re-armed.");
        break;

      case 'm': case 'M': {
        size_t cached = 0;
        if (LittleFS.exists(ART_FILE_PATH)) {
          File art = LittleFS.open(ART_FILE_PATH, "r");
          if (art) {
            cached = art.size();
            art.close();
          }
        }
        Serial.printf("[Cmd] heap=%u min=%u largest=%u rssi=%d cached=%u\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)ESP.getMaxAllocHeap(), (int)WiFi.RSSI(),
                      (unsigned)cached);
        break;
      }

      case 'h': case 'H':
        Serial.println(F("[Cmd] d=dump position info, x=erase /art.jpg, "
                         "f=re-fetch artwork, m=memory/rssi"));
        break;

      default:
        break;  // line endings and stray bytes are ignored
    }
  }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("--- LilyGO TTGO T-Display Sonos What's Playing ---");
  Serial.printf("Version: %s\n", APP_VERSION);
  Serial.println("------------------------------------------");

  // Buttons first, so the initial pin state is captured.
  pinMode(PIN_BUTTON_1, INPUT);
  pinMode(PIN_BUTTON_2, INPUT);
  btn1.lastReading = (digitalRead(PIN_BUTTON_1) == LOW);
  btn2.lastReading = (digitalRead(PIN_BUTTON_2) == LOW);
  btn1.state      = btn1.lastReading;
  btn2.state      = btn2.lastReading;
  Serial.printf("%s / %s ready\n", btn1.label, btn2.label);

  // Display and backlight.
  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(TFT_BLACK);

  // Report the geometry the driver actually settled on. The metadata layout
  // constants assume a 240x135 landscape panel, so this is worth confirming.
  Serial.printf("[Display] %dx%d rotation=%d, font1 height=%d, font2 height=%d\n",
                tft.width(), tft.height(), tft.getRotation(),
                tft.fontHeight(1), tft.fontHeight(2));

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  screenPowered = true;

  // LittleFS, formatting if the mount fails.
  if (!LittleFS.begin(LITTLEFS_LABEL)) {
    Serial.println("[LittleFS] Mount failed, formatting LittleFS...");
    LittleFS.format();
    LittleFS.begin(LITTLEFS_LABEL);
  }
  Serial.println("[LittleFS] Ready.");

  // WiFi, with the secondary SSID as a fallback.
  connectWiFi();

  drawConnectingScreen("Connecting...");

  // SSDP discovery.
  sendSSDPDiscovery();
  lastSsdpSearchTime = millis();

  Serial.println("Display, LittleFS, WiFi, and SSDP initialized.");
}

void loop() {
  // ---- Serial console ---------------------------------------------------
#if SERIAL_CONSOLE
  // First, so a command is acted on even if the rest of this pass returns
  // early (no WiFi, or still searching for a speaker).
  handleSerialCommands();
#endif

  // ---- Buttons ---------------------------------------------------------
  bool wakeRequested = false;

  if (updateButton(btn1)) {
    Serial.println("[Button 1] Next Track triggered.");
    sendSonosAction("Next", "<InstanceID>0</InstanceID>");
    showToast(">> Next Track", TFT_GREEN);
    wakeRequested = true;
  }

  if (updateButton(btn2)) {
    if (isPlaying || currentTransportState == "PAUSED_PLAYBACK") {
      Serial.println("[Button 2] Pause triggered.");
      sendSonosAction("Pause", "<InstanceID>0</InstanceID>");
      showToast("|| Pausing...", TFT_ORANGE);
    } else {
      Serial.println("[Button 2] Play triggered.");
      sendSonosAction("Play", "<InstanceID>0</InstanceID><Speed>1</Speed>");
      showToast("> Playing...", TFT_GREEN);
    }
    wakeRequested = true;
  }

  // Any button press wakes the display immediately.
  if (wakeRequested && !screenPowered) {
    setScreenPower(true);
  }

  // ---- WiFi supervision -------------------------------------------------
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiRetryTime > WIFI_RETRY_INTERVAL_MS) {
      lastWifiRetryTime = millis();
      Serial.println("[WiFi] Disconnected, retrying...");
      connectWiFi();
    }
    delay(50);
    return;
  }

  // ---- Sonos discovery -------------------------------------------------
  if (activeSonosIP[0] == 0) {
    checkSSDPResponses();

    if (sonosDeviceCount > 0) {
      // Adopt the first speaker discovered.
      activeSonosIP = sonosDevices[0];
      Serial.printf("[Sonos] Using speaker at %s\n",
                    activeSonosIP.toString().c_str());
    } else {
      // No speaker yet: search again, and try the known fallbacks so the
      // display still works when SSDP is unavailable.
      if (millis() - lastSsdpSearchTime > SSDP_RETRY_INTERVAL_MS) {
        Serial.println("[Sonos] No device found, trying fallbacks...");
        addSonosDevice(IPAddress(192, 168, 1, 29));
        addSonosDevice(IPAddress(192, 168, 1, 180));

        if (sonosDeviceCount > 0) {
          activeSonosIP = sonosDevices[0];
        }
        sendSSDPDiscovery();
      }

      if (!screenPowered) setScreenPower(true);
      drawConnectingScreen("Searching...");
      delay(100);
      return;
    }
  } else {
    // Periodic rediscovery, less often once a speaker is in use.
    if (millis() - lastSsdpSearchTime > SSDP_KEEPALIVE_MS) {
      sendSSDPDiscovery();
    }
  }

  // ---- Poll the speaker ------------------------------------------------
  if (millis() - lastSonosPollTime > SONOS_POLL_INTERVAL_MS) {
    lastSonosPollTime = millis();
    pollSonos();
  }

  // ---- Idle handling and screen power ----------------------------------
  if (isPlaying) {
    isIdle                  = false;
    idleStartTime           = millis();
    lastIdleSecondsRemaining = -1;
    if (!screenPowered) setScreenPower(true);
  } else {
    if (!isIdle) {
      // Playback just stopped; begin the idle countdown.
      isIdle       = true;
      idleStartTime = millis();
    }

    if (isIdle && screenPowered) {
      int remaining =
          (int)((IDLE_SCREEN_TIMEOUT_MS - (millis() - idleStartTime)) / 1000);
      if (remaining < 0) remaining = 0;

      if (remaining != lastIdleSecondsRemaining) {
        lastIdleSecondsRemaining = remaining;
      }

      if (millis() - idleStartTime > IDLE_SCREEN_TIMEOUT_MS) {
        setScreenPower(false);
      }
    }
  }

  // ---- Render ----------------------------------------------------------
  drawScreen();

  delay(20);
}
