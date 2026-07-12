// DCF77 transmitter — gets real time via NTP and re-broadcasts it as a
// low-power 77.5 kHz DCF77 signal for a radio-controlled clock nearby.
//
// Hardware: ESP32 GPIO -> series resistor (~470R..1k) -> ferrite antenna
// with its parallel tuning cap (salvaged from a DCF77 receiver module) -> GND.

#include <Arduino.h>
#include <WiFi.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"

// ---- Hardware ----
constexpr int PIN_ANTENNA = 4; // GPIO -> resistor -> ferrite tank -> GND
constexpr int PIN_LED = 2;        // onboard blue LED
constexpr bool LED_BLINK = false; // mirror the modulation dips on the LED
                                  // (nice for debugging, annoying in a bedroom)

// ---- Carrier ----
constexpr uint32_t CARRIER_HZ = 77500;
constexpr uint8_t LEDC_CHANNEL = 0;
constexpr uint8_t LEDC_RES_BITS = 8;
constexpr uint32_t DUTY_ON = 128;     // 50 % duty -> full carrier amplitude
constexpr uint32_t DUTY_REDUCED = 0;  // carrier fully off during dips — more robust
                                      // than the spec's 15 % at close range, where the
                                      // receiver AGC can flatten a partial dip

// ---- Time ----
// Europe/Berlin with automatic CET/CEST switching
constexpr const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";

// Test mode: transmit a wrong time-of-day, starting at FAKE_HOUR:FAKE_MINUTE
// and counting up normally from there (clocks verify that consecutive frames
// increment by one minute, so a frozen time would never be accepted).
// Date, weekday and DST flag stay real. Set to 0 for normal operation.
#define FAKE_TIME 0
constexpr int FAKE_HOUR = 3;
constexpr int FAKE_MINUTE = 33;

static bool frameBits[60];       // one bit per second of the current minute
static time_t frameMinute = -1;  // start-of-minute epoch the frame was built for

static uint8_t bcd(int v) { return ((v / 10) << 4) | (v % 10); }

// Write `len` bits of `value` LSB-first into frameBits starting at `pos`.
// Returns the number of 1-bits (for parity).
static int putBits(int pos, uint8_t value, int len) {
  int ones = 0;
  for (int i = 0; i < len; i++) {
    bool b = (value >> i) & 1;
    frameBits[pos + i] = b;
    ones += b;
  }
  return ones;
}

// Build the frame transmitted during the minute starting at txMinuteStart.
// DCF77 always announces the time of the *following* minute.
static void buildFrame(time_t txMinuteStart) {
  time_t enc = txMinuteStart + 60;
#if FAKE_TIME
  // Shift real time by a fixed offset so the first transmitted frame reads
  // FAKE_HOUR:FAKE_MINUTE and subsequent frames count up normally.
  static time_t fakeOffset = 0;
  static bool offsetSet = false;
  if (!offsetSet) {
    struct tm base;
    localtime_r(&enc, &base);
    base.tm_hour = FAKE_HOUR;
    base.tm_min = FAKE_MINUTE;
    base.tm_sec = 0;
    base.tm_isdst = -1;
    fakeOffset = mktime(&base) - enc;
    offsetSet = true;
  }
  enc += fakeOffset;
#endif
  struct tm t;
  localtime_r(&enc, &t);

  memset(frameBits, 0, sizeof(frameBits));
  // 0: start of minute, always 0
  // 1-14: civil warning / weather (unused), 15: call bit,
  // 16: DST change announcement (not implemented) — all 0
  frameBits[17] = t.tm_isdst > 0;  // CEST
  frameBits[18] = t.tm_isdst <= 0; // CET
  // 19: leap second announcement, 0
  frameBits[20] = 1; // start of encoded time, always 1

  int p = putBits(21, bcd(t.tm_min), 7);
  frameBits[28] = p & 1; // even parity minutes
  p = putBits(29, bcd(t.tm_hour), 6);
  frameBits[35] = p & 1; // even parity hours
  p = putBits(36, bcd(t.tm_mday), 6);
  p += putBits(42, t.tm_wday == 0 ? 7 : t.tm_wday, 3); // 1=Mon .. 7=Sun
  p += putBits(45, bcd(t.tm_mon + 1), 5);
  p += putBits(50, bcd(t.tm_year % 100), 8);
  frameBits[58] = p & 1; // even parity date
  // 59: minute marker — no amplitude dip, value irrelevant

  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M %Z", &t);
  Serial.printf("Encoding next minute: %s | bits: ", buf);
  for (int i = 0; i < 59; i++) Serial.print(frameBits[i] ? '1' : '0');
  Serial.println();
}

static void carrier(uint32_t duty) { ledcWrite(LEDC_CHANNEL, duty); }

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);

  ledcSetup(LEDC_CHANNEL, CARRIER_HZ, LEDC_RES_BITS);
  ledcAttachPin(PIN_ANTENNA, LEDC_CHANNEL);
  carrier(0); // stay silent until we have valid time

  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
  }
  Serial.printf("\nWiFi connected, IP %s\n", WiFi.localIP().toString().c_str());

  // PTB (the institute that runs DCF77) operates public NTP servers — fitting.
  configTzTime(TZ_INFO, "ptbtime1.ptb.de", "ptbtime2.ptb.de", "pool.ntp.org");
  Serial.print("Waiting for NTP sync");
  struct tm t;
  while (!getLocalTime(&t, 1000) || t.tm_year < 120) Serial.print('.');
  Serial.println("\nTime synced, transmitting.");
  carrier(DUTY_ON);
}

void loop() {
  static time_t lastSec = 0;
  static uint32_t dipMs = 0; // dip length of the current second, 0 = none

  struct timeval tv;
  gettimeofday(&tv, nullptr);

  if (tv.tv_sec != lastSec) { // a new second has begun
    lastSec = tv.tv_sec;
    int sec = tv.tv_sec % 60;
    time_t minuteStart = tv.tv_sec - sec;
    if (minuteStart != frameMinute) {
      buildFrame(minuteStart);
      frameMinute = minuteStart;
    }
    if (sec == 59) {
      dipMs = 0; // minute marker: no dip
    } else {
      dipMs = frameBits[sec] ? 200 : 100;
      carrier(DUTY_REDUCED);
      if (LED_BLINK) digitalWrite(PIN_LED, HIGH);
    }
  }

  if (dipMs && (uint32_t)(tv.tv_usec / 1000) >= dipMs) {
    carrier(DUTY_ON);
    if (LED_BLINK) digitalWrite(PIN_LED, LOW);
    dipMs = 0;
  }
}
