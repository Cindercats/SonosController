// Template for local credentials.
//
// Copy this file to src/secrets.h and fill in your own values:
//
//     cp src/secrets.example.h src/secrets.h
//
// src/secrets.h is gitignored, so your password is never published.

#pragma once

// WiFi
const char *WIFI_SSID_PRIMARY   = "YOUR_WIFI_SSID";
const char *WIFI_SSID_SECONDARY = "YOUR_FALLBACK_SSID";
const char *WIFI_PASSWORD       = "YOUR_WIFI_PASSWORD";

// Known Sonos speakers, used if SSDP discovery finds nothing.
const char *FALLBACK_SONOS_1 = "192.168.1.100";
const char *FALLBACK_SONOS_2 = "192.168.1.101";
