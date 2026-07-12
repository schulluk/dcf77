// Copy this file to config.h and fill in your WiFi credentials.
#pragma once

#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"

// Optional: override the NTP servers (defaults: PTB servers + pool.ntp.org).
// #define NTP_SERVER_1 "ptbtime1.ptb.de"
// #define NTP_SERVER_2 "ptbtime2.ptb.de"
// #define NTP_SERVER_3 "pool.ntp.org"

// Optional: blink the onboard blue LED with the modulation dips (default: off).
// #define LED_BLINK true

// Optional: test mode. Transmit a wrong time-of-day starting at
// FAKE_HOUR:FAKE_MINUTE, counting up from there (default: off, real time).
// #define FAKE_TIME 1
// #define FAKE_HOUR 3
// #define FAKE_MINUTE 33
