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
#include <TJpg_Decoder.h>

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
#define APP_VERSION "0.35"

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
#define ART_MAX_SIZE            (256 * 1024)

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
void downloadArtwork();
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
String xmlUnescape(const String &in) {
  String out = in;
  out.replace("&lt;", "<");
  out.replace("&gt;", ">");
  out.replace("&quot;", "\"");
  out.replace("&apos;", "'");
  out.replace("&amp;", "&");  // must run last
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

// Resolves a possibly relative artwork URL against the active speaker.
// Handles absolute http/https URLs, protocol-relative "//host" URLs and
// bare paths such as "/getaa?id=...".
String resolveArtworkUrl(const String &raw) {
  String url = raw;
  url.trim();
  if (url.length() == 0) return String();

  if (url.startsWith("http://") || url.startsWith("https://")) {
    return url;
  }

  if (url.startsWith("//")) {
    return "http:" + url;
  }

  if (activeSonosIP[0] == 0) return String();

  String base = "http://" + activeSonosIP.toString();

  if (url.startsWith("/")) {
    return base + url;
  }

  return base + "/" + url;
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
static String readHttpBody(HTTPClient &http) {
  WiFiClient *stream = http.getStreamPtr();
  if (stream == nullptr) return String();

  String body;
  body.reserve(2048);
  uint8_t buf[256];

  if (http.getSize() > 0) {
    // Known length: read exactly that many bytes.
    int remaining = http.getSize();

    while (remaining > 0 && http.connected()) {
      int available = stream->available();
      if (available <= 0) {
        delay(1);
        continue;
      }
      int want = (available < remaining) ? available : remaining;
      if (want > (int)sizeof(buf)) want = sizeof(buf);

      int got = stream->readBytes(buf, want);
      if (got <= 0) break;

      body += String((const char *)buf, got);
      remaining -= got;
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

    body += String((const char *)buf, got);
    chunkRemaining -= got;

    if (chunkRemaining == 0) inChunk = false;
  }

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

// Issues a SOAP action against the active speaker and returns the body.
String sonosSoap(const char *action, const char *instanceArgs) {
  if (activeSonosIP[0] == 0) return String();

  String url = "http://" + activeSonosIP.toString() + ":" + String(SONOS_PORT) +
               SONOS_CONTROL_PATH;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setUserAgent("SonosDisplay/" APP_VERSION);

  if (!http.begin(client, url)) {
    Serial.println("[Debug] SOAP begin() failed.");
    return String();
  }

  String soapAction = "\"urn:schemas-upnp-org:service:AVTransport:1#";
  soapAction += action;
  soapAction += "\"";

  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPACTION", soapAction);

  int status = http.POST(buildEnvelope(action, instanceArgs));
  String body;

  if (status == 200) {
    int contentLength = http.getSize();
    body = readHttpBody(http);

    Serial.printf("[Debug] %s -> HTTP %d, Content-Length=%d, received=%d bytes\n",
                  action, status, contentLength, body.length());
  } else {
    Serial.printf("[Debug] %s failed: HTTP %d\n", action, status);
  }

  http.end();
  return body;
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

// Downloads the current track artwork into LittleFS.
//
// Source URL: the <upnp:albumArtURI> value read from <CurrentTrackMetaData>,
// typically http://<speaker-ip>:1400/getaa?u=<url-encoded upstream URI>.
// It may be relative (resolved against the speaker), http, or https, and
// commonly answers with a redirect to a CDN, so up to HTTP_MAX_REDIRECTS
// hops are followed manually.
//
// Verbose logging is emitted throughout to assist debugging.
void downloadArtwork() {
  if (currentArtUri.length() == 0) return;

  String url = resolveArtworkUrl(currentArtUri);
  if (url.length() == 0) {
    Serial.println("[Art] Could not resolve artwork URL.");
    return;
  }

  if (url == cachedArtUrl) {
    artNeedsRender = true;  // already cached on disk
    return;
  }

  Serial.println("[Art] ------------------------------------");
  Serial.printf("[Art] Resolved URL: %s\n", url.c_str());

  File artFile = LittleFS.open(ART_FILE_PATH, FILE_WRITE);
  if (!artFile) {
    Serial.println("[Art] Failed to open /art.jpg for writing.");
    return;
  }

  // Both clients are declared here, outside the redirect loop, and MUST live
  // for the whole request. HTTPClient keeps a reference to whichever client it
  // was given, so a client declared inside the loop's scope would be destroyed
  // while http.getStreamPtr() still pointed at it.
  WiFiClientSecure secureClient;
  WiFiClient       plainClient;

  bool               wroteAnyBytes = false;
  bool               success       = false;
  int                redirectCount = 0;

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

    Serial.printf("[Art] GET (attempt %d, %s)...\n", redirectCount + 1,
                  https ? "https" : "http");

    int status = http.GET();
    Serial.printf("[Art] HTTP status: %d\n", status);

    if (status == 301 || status == 302 || status == 303 || status == 307 ||
        status == 308) {
      String location = http.getLocation();
      http.end();

      if (location.length() == 0) {
        Serial.println("[Art] Redirect with no Location header, giving up.");
        break;
      }

      String next = resolveRedirect(url, location);
      Serial.printf("[Art] Redirect %d -> %s\n", status, next.c_str());

      url = next;
      redirectCount++;
      continue;
    }

    if (status != 200) {
      Serial.printf("[Art] Download failed: %s\n",
                    HTTPClient::errorToString(status).c_str());
      http.end();
      break;
    }

    int contentLength = http.getSize();
    Serial.printf("[Art] Content-Length: %d\n", contentLength);

    if (contentLength > ART_MAX_SIZE) {
      Serial.println("[Art] Artwork too large, skipping.");
      http.end();
      break;
    }

    // Stream the body straight to LittleFS to keep RAM use low.
    WiFiClient *stream = http.getStreamPtr();
    uint8_t     chunk[1024];
    size_t      received = 0;
    int         available;

    while (http.connected() && (available = stream->available()) > 0) {
      // Enforce the cap even when the server sent no usable Content-Length,
      // otherwise a bad response could fill the whole filesystem.
      if (received >= ART_MAX_SIZE) {
        Serial.println("[Art] Exceeded size cap mid-stream, aborting.");
        break;
      }

      int toRead = available;
      if (toRead > (int)sizeof(chunk)) toRead = sizeof(chunk);
      if ((size_t)toRead > ART_MAX_SIZE - received) {
        toRead = (int)(ART_MAX_SIZE - received);
      }

      int bytes = stream->readBytes(chunk, toRead);
      if (bytes <= 0) break;

      artFile.write(chunk, bytes);
      received += bytes;
      wroteAnyBytes = true;
    }

    http.end();

    Serial.printf("[Art] Wrote %u bytes to LittleFS\n", (unsigned)received);
    success = (received > 0);
    break;
  }

  artFile.close();

  if (success) {
    cachedArtUrl   = url;
    artNeedsRender = true;
    Serial.println("[Art] Cached successfully.");
  } else {
    // Partial or empty download: discard so a truncated JPEG is never drawn.
    LittleFS.remove(ART_FILE_PATH);
    if (wroteAnyBytes) {
      Serial.println("[Art] Partial download, cached file removed.");
    } else {
      Serial.println("[Art] Nothing downloaded.");
    }
  }

  Serial.println("[Art] ------------------------------------");
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

// Decodes and draws the cached artwork, scaled to fit the artwork area.
void renderArtwork() {
  if (!artNeedsRender) return;
  artNeedsRender = false;

  if (!screenPowered) return;

  tft.fillRect(ART_AREA_X, ART_AREA_Y, ART_AREA_W, ART_AREA_H, TFT_BLACK);

  if (!LittleFS.exists(ART_FILE_PATH)) {
    Serial.println("[Art] No cached artwork, drawing placeholder.");
    drawArtworkPlaceholder();
    return;
  }

  uint16_t w = 0, h = 0;
  JRESULT  sizeResult = TJpgDec.getFsJpgSize(&w, &h, ART_FILE_PATH, LittleFS);
  if (sizeResult != JDR_OK) {
    Serial.printf("[Art] getJpgSize failed: %d\n", (int)sizeResult);
    drawArtworkPlaceholder();
    return;
  }

  Serial.printf("[Art] JPEG dimensions: %u x %u\n", w, h);

  // Pick the smallest reduction factor that fits the artwork area, keeping
  // the image as large as possible.
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

  TJpgDec.setJpgScale(scaleFactor);
  TJpgDec.setCallback(tft_output);

  int32_t destX = ART_AREA_X + (ART_AREA_W - outW) / 2;
  int32_t destY = ART_AREA_Y + (ART_AREA_H - outH) / 2;

  JRESULT result = TJpgDec.drawFsJpg(destX, destY, ART_FILE_PATH, LittleFS);
  if (result != JDR_OK) {
    Serial.printf("[Art] drawFsJpg failed: %d\n", (int)result);
    // The file decoded as JPEG headers but could not be rendered; show the
    // URL so the source of the bad image is visible on screen.
    drawArtworkPlaceholder();
  } else {
    Serial.println("[Art] Render complete.");
  }
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

  // Dump the raw response once so the exact tag names and ordering can be
  // confirmed from the serial log when parsing needs investigating.
  static bool dumpedPositionInfo = false;
  if (!dumpedPositionInfo) {
    dumpedPositionInfo = true;
    Serial.println("========== GetPositionInfo RAW (first 1400 bytes) ==========");
    Serial.println(positionBody.substring(0, 1400));
    Serial.println("========== END RAW ==========");
  }

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
  String artUri;

  if (metaData.length() > 0) {
    artUri = extractByLocalName(metaData, "albumArtURI");
  }

  if (artUri.length() == 0) {
    artUri = xmlUnescape(extractByLocalName(positionBody, "CurrentTrackArtImage"));
  }

  // Last resort: some local/native content exposes the image through <res>.
  // Guarded by an image-extension check, because <res> normally holds the
  // AUDIO STREAM (x-file-cifs://, x-rincon-mp3radio:// ...) and downloading
  // that as artwork would waste bandwidth and always fail to decode.
  if (artUri.length() == 0 && metaData.length() > 0) {
    String resUri = xmlUnescape(extractByLocalName(metaData, "res"));
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
  if (title != currentTitle || artist != currentArtist) {
    currentTitle   = title;
    currentArtist  = artist;
    artNeedsRender = true;

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
    currentArtUri = artUri;
    Serial.printf("[Sonos] Artwork URI: %s\n", artUri.c_str());
    if (artUri.length() > 0 && artUri != cachedArtUrl) {
      downloadArtwork();
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

// Draws the artwork-area placeholder.
//
// When the track carries an artwork URL, that URL is shown on screen instead
// of the generic disc. The URL comes straight out of the GetPositionInfo
// response, so reading it on the display is the quickest way to confirm that
// extraction produced a usable address and to see which host/port/path the
// firmware is being asked to fetch.
void drawArtworkPlaceholder() {
  tft.fillRect(ART_AREA_X, ART_AREA_Y, ART_AREA_W, ART_AREA_H, TFT_BLACK);

  if (currentArtUri.length() > 0) {
    tft.setTextFont(1);
    tft.setTextColor(0xBDF7);
    tft.setTextDatum(TL_DATUM);

    tft.drawString("Artwork URL:", ART_AREA_X + 4, ART_AREA_Y + 4);

    int nextY = drawWrappedText(currentArtUri, ART_AREA_X + 4,
                                ART_AREA_Y + 16, ART_AREA_W - 8, 7, 1);

    // If the URL did not fit, note that it was truncated.
    tft.setTextColor(0x6E6E);
    tft.drawString("(truncated)", ART_AREA_X + 4,
                   nextY < ART_AREA_Y + ART_AREA_H - 10
                       ? nextY
                       : ART_AREA_Y + ART_AREA_H - 10);
    return;
  }

  // No URL at all in the response: fall back to the disc motif and say why.
  int cx = ART_AREA_X + ART_AREA_W / 2;
  int cy = ART_AREA_Y + ART_AREA_H / 2;

  tft.fillCircle(cx, cy, 36, TFT_DARKGREY);
  tft.fillCircle(cx, cy, 20, 0x18A0);
  tft.fillCircle(cx, cy, 6, TFT_DARKGREY);

  tft.setTextColor(0xFF8C00);
  tft.setTextFont(1);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("no artwork URL", cx, cy + 26);
  tft.setTextColor(0x6E6E);
  tft.drawString("in GetPositionInfo", cx, cy + 38);
}

// Clears the display and draws the placeholder for the idle screen.
//
// The text rows are drawn afterwards by drawMetadataBlock(), so this only
// handles the full-screen clear and the artwork area.
void drawNothingPlayingScreen() {
  tft.fillScreen(TFT_BLACK);
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
    artNeedsRender          = true;
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

  // Artwork decoder callback.
  TJpgDec.setCallback(tft_output);

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
