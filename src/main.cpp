// DCF77 transmitter — gets real time via NTP and re-broadcasts it as a
// low-power 77.5 kHz DCF77 signal for a radio-controlled clock nearby.
//
// Hardware: ESP32 GPIO -> series resistor (~470R..1k) -> ferrite antenna
// with its parallel tuning cap (salvaged from a DCF77 receiver module) -> GND.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>

#include <vector>

#include "config.h"

// ---- Hardware ----
constexpr int PIN_ANTENNA = 4; // GPIO -> resistor -> ferrite tank -> GND
constexpr int PIN_LED = 2; // onboard blue LED

// Mirror the modulation dips on the LED (nice for debugging, annoying in a
// bedroom). Overridable in config.h.
#ifndef LED_BLINK
#define LED_BLINK false
#endif

// ---- Carrier ----
constexpr uint32_t CARRIER_HZ = 77500;
constexpr uint8_t LEDC_CHANNEL = 0;
constexpr uint8_t LEDC_RES_BITS = 8;
constexpr uint32_t DUTY_ON = 128;     // 50 % duty -> full carrier amplitude
constexpr uint32_t DUTY_REDUCED = 0;  // carrier fully off during dips — more robust
                                      // than the spec's 15 % at close range, where the
                                      // receiver AGC can flatten a partial dip

static void carrier(uint32_t duty) { ledcWrite(LEDC_CHANNEL, duty); }

// ---- Time ----
// Europe/Berlin with automatic CET/CEST switching
constexpr const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";

// NTP servers, overridable in config.h. Defaults are the PTB servers,
// run by the same institute that operates the real DCF77 transmitter.
#ifndef NTP_SERVER_1
#define NTP_SERVER_1 "ptbtime1.ptb.de"
#endif
#ifndef NTP_SERVER_2
#define NTP_SERVER_2 "ptbtime2.ptb.de"
#endif
#ifndef NTP_SERVER_3
#define NTP_SERVER_3 "pool.ntp.org"
#endif

// Test mode: transmit a wrong time-of-day, starting at FAKE_HOUR:FAKE_MINUTE
// and counting up normally from there (clocks verify that consecutive frames
// increment by one minute, so a frozen time would never be accepted).
// Date, weekday and DST flag stay real. Overridable in config.h.
#ifndef FAKE_TIME
#define FAKE_TIME 0
#endif
#ifndef FAKE_HOUR
#define FAKE_HOUR 3
#endif
#ifndef FAKE_MINUTE
#define FAKE_MINUTE 33
#endif

// ---- Network ----
// Name on the network: status page at http://HOSTNAME.local/. Overridable
// in config.h.
#ifndef HOSTNAME
#define HOSTNAME "dcf77"
#endif

// Without WiFi or NTP after this long, restart and try again, instead of
// waiting forever with the carrier off.
constexpr unsigned long WIFI_TIMEOUT_S = 180;
constexpr unsigned long NTP_TIMEOUT_S = 180;

static bool frameBits[60];       // one bit per second of the current minute
static time_t frameMinute = -1;  // start-of-minute epoch the frame was built for
static char frameDesc[32] = "";  // time encoded in the current frame, for the status page
static char frameBitStr[60] = "";

static volatile bool transmitting = false;
static volatile time_t lastNtpSync = 0;
static volatile uint32_t ntpSyncs = 0;

static bool timeValid() { return time(nullptr) > 1600000000; } // after 2020

static String fmtTime(time_t epoch) {
  struct tm t;
  localtime_r(&epoch, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &t);
  return buf;
}

// ---- Event log ----
// Survives power loss, so after an outage you can still see what happened.
// Events are appended to a LittleFS file, and a heartbeat timestamp goes to
// NVS every HEARTBEAT_S, so the next boot knows roughly when power was lost.
//
// Flash writes stall both CPU cores for up to tens of milliseconds, enough
// to stretch a 100 ms dip into an ambiguous one. While transmitting, events
// are therefore queued in RAM and written during second 59, which has no dip.
constexpr const char *LOG_FILE = "/log.txt";
constexpr const char *LOG_FILE_OLD = "/log.old.txt";
constexpr size_t LOG_MAX_BYTES = 32 * 1024; // then rotate to LOG_FILE_OLD
constexpr size_t LOG_QUEUE_MAX = 32;
constexpr unsigned long HEARTBEAT_S = 600;

static SemaphoreHandle_t queueMutex; // guards logQueue, held only briefly
static SemaphoreHandle_t fileMutex;  // guards the log files
static std::vector<String> logQueue;
static unsigned long logDropped = 0;
static bool fsOk = false;
static Preferences prefs;

// Log an event, printf-style. Safe to call from any task.
__attribute__((format(printf, 1, 2))) static void logEvent(const char *fmt, ...) {
  char msg[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char line[200];
  if (timeValid()) {
    snprintf(line, sizeof(line), "%s  %s", fmtTime(time(nullptr)).c_str(), msg);
  } else {
    unsigned long ms = millis();
    snprintf(line, sizeof(line), "uptime %lu.%03lus  %s", ms / 1000, ms % 1000, msg);
  }
  Serial.println(line);

  xSemaphoreTake(queueMutex, portMAX_DELAY);
  if (logQueue.size() < LOG_QUEUE_MAX) logQueue.push_back(line);
  else logDropped++;
  xSemaphoreGive(queueMutex);
}

// Write queued events to flash. Only call when no dip is in progress. Gives
// up (events stay queued) if the status page is reading the file right now,
// so loop() never blocks.
static void flushLog() {
  if (xSemaphoreTake(fileMutex, 0) != pdTRUE) return;

  std::vector<String> lines;
  unsigned long dropped;
  xSemaphoreTake(queueMutex, portMAX_DELAY);
  lines.swap(logQueue);
  dropped = logDropped;
  logDropped = 0;
  xSemaphoreGive(queueMutex);

  if (fsOk && (!lines.empty() || dropped)) {
    File f = LittleFS.open(LOG_FILE, FILE_APPEND);
    if (f) {
      for (auto &l : lines) f.println(l);
      if (dropped) f.printf("(%lu events dropped, queue full)\n", dropped);
      size_t size = f.size();
      f.close();
      if (size > LOG_MAX_BYTES) {
        LittleFS.remove(LOG_FILE_OLD);
        LittleFS.rename(LOG_FILE, LOG_FILE_OLD);
      }
    }
  }
  xSemaphoreGive(fileMutex);
}

static String readLogFile(const char *path) {
  String s;
  xSemaphoreTake(fileMutex, portMAX_DELAY);
  File f = LittleFS.open(path, FILE_READ);
  if (f) {
    s = f.readString();
    f.close();
  }
  xSemaphoreGive(fileMutex);
  return s;
}

static const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on (power was off)";
    case ESP_RST_BROWNOUT: return "brownout (supply voltage dipped)";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_EXT: return "reset button";
    case ESP_RST_DEEPSLEEP: return "deep sleep wakeup";
    default: return "unknown";
  }
}

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  static bool up = false;
  static uint32_t downSinceMs = 0;
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    if (downSinceMs) {
      logEvent("WiFi back after %lus, IP %s, RSSI %d dBm", (millis() - downSinceMs) / 1000,
               WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
      logEvent("WiFi connected, IP %s, RSSI %d dBm", WiFi.localIP().toString().c_str(),
               WiFi.RSSI());
    }
    up = true;
    downSinceMs = 0;
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED && up) {
    // Reconnect attempts fire this every few seconds; log only the drop.
    logEvent("WiFi lost (reason %u)", info.wifi_sta_disconnected.reason);
    up = false;
    downSinceMs = millis() | 1; // nonzero
  }
}

static void onNtpSync(struct timeval *tv) {
  lastNtpSync = tv->tv_sec;
  ntpSyncs++;
}

// ---- Status page ----
static WebServer server(80);

static void appendEscaped(String &out, const String &in) {
  for (char c : in) {
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else out += c;
  }
}

static void handleStatus() {
  time_t now = time(nullptr);
  unsigned long up = millis() / 1000;
  char buf[64];
  String s;
  s.reserve(8192);
  s += "<!doctype html><meta charset=utf-8>"
       "<meta name=viewport content='width=device-width,initial-scale=1'>"
       "<title>DCF77 transmitter</title>"
       "<style>body{font:14px/1.4 ui-monospace,monospace;margin:16px}"
       "pre{white-space:pre-wrap;word-break:break-all}</style>"
       "<h1>DCF77 transmitter</h1><pre>";
  s += "Time        " + fmtTime(now) + "\n";
  snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu", up / 86400, up / 3600 % 24, up / 60 % 60,
           up % 60);
  s += "Uptime      " + String(buf) + "\n";
  s += "Last reset  " + String(resetReason()) + "\n";
  if (WiFi.status() == WL_CONNECTED) {
    s += "WiFi        RSSI " + String(WiFi.RSSI()) + " dBm, IP " + WiFi.localIP().toString() + "\n";
  } else {
    s += "WiFi        not connected\n";
  }
  if (lastNtpSync) {
    s += "Last NTP    " + fmtTime(lastNtpSync) + " (" + String((now - lastNtpSync) / 60) +
         " min ago, " + String(ntpSyncs) + " syncs since boot)\n";
  } else {
    s += "Last NTP    never\n";
  }
  s += "Carrier     " + String(transmitting ? "on" : "off (waiting for WiFi/NTP)") + "\n";
  s += "Encoding    " + String(frameDesc) + "\n";
  s += "Bits        " + String(frameBitStr) + "\n";
  s += "Free heap   " + String(ESP.getFreeHeap()) + " bytes\n";
  s += "</pre><h2>Event log, newest first</h2><pre>";

  String log = readLogFile(LOG_FILE);
  int end = log.length();
  while (end > 0) {
    int start = log.lastIndexOf('\n', end - 2) + 1; // skip the line's own '\n'
    appendEscaped(s, log.substring(start, end));
    end = start;
  }
  s += "</pre><p><a href=/log.txt>log.txt</a> &middot; <a href=/log.old.txt>log.old.txt</a>";
  server.send(200, "text/html", s);
}

static void handleRawLog() {
  String path = server.uri();
  if (path != LOG_FILE && path != LOG_FILE_OLD) {
    server.send(404, "text/plain", "not found");
    return;
  }
  server.send(200, "text/plain; charset=utf-8", readLogFile(path.c_str()));
}

// Runs on core 0 next to the WiFi stack, so serving a page never delays
// the dip timing in loop() on core 1.
static void webTask(void *) {
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void startWeb() {
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  server.on("/", handleStatus);
  server.on(LOG_FILE, handleRawLog);
  server.on(LOG_FILE_OLD, handleRawLog);
  server.begin();
  xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 0);
}

// Wait until ready() holds, logging progress. On timeout, log why and restart.
static void waitOrRestart(bool (*ready)(), unsigned long timeoutS, const char *what) {
  uint32_t start = millis();
  while (!ready()) {
    if (millis() - start > timeoutS * 1000) {
      logEvent("no %s after %lus, restarting", what, timeoutS);
      flushLog();
      delay(100);
      ESP.restart();
    }
    flushLog(); // WiFi events may have been queued meanwhile
    Serial.print('.');
    delay(1000);
  }
  Serial.println();
}

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

  strftime(frameDesc, sizeof(frameDesc), "%Y-%m-%d %H:%M %Z", &t);
  for (int i = 0; i < 59; i++) frameBitStr[i] = frameBits[i] ? '1' : '0';
  frameBitStr[59] = '\0';
  Serial.printf("Encoding next minute: %s | bits: %s\n", frameDesc, frameBitStr);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);

  ledcSetup(LEDC_CHANNEL, CARRIER_HZ, LEDC_RES_BITS);
  ledcAttachPin(PIN_ANTENNA, LEDC_CHANNEL);
  carrier(0); // stay silent until we have valid time

  setenv("TZ", TZ_INFO, 1); // for log timestamps before NTP is configured
  tzset();
  queueMutex = xSemaphoreCreateMutex();
  fileMutex = xSemaphoreCreateMutex();
  fsOk = LittleFS.begin(true); // formats on first boot
  prefs.begin("dcf77", false);

  logEvent("boot, reset reason: %s", resetReason());
  if (!fsOk) logEvent("LittleFS mount failed, event log is not persisted");
  uint32_t lastAlive = prefs.getUInt("alive", 0);
  if (lastAlive) {
    logEvent("previous run last alive at %s (heartbeat every %lu min)",
             fmtTime(lastAlive).c_str(), HEARTBEAT_S / 60);
  }
  flushLog();

  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.onEvent(onWiFiEvent);
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  waitOrRestart([] { return WiFi.status() == WL_CONNECTED; }, WIFI_TIMEOUT_S, "WiFi");
  startWeb();

  // PTB (the institute that runs DCF77) operates public NTP servers, fitting.
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  Serial.print("Waiting for NTP sync");
  waitOrRestart(timeValid, NTP_TIMEOUT_S, "NTP sync");
  logEvent("time synced %lus after boot, carrier on", millis() / 1000);
  prefs.putUInt("alive", time(nullptr));
  flushLog();
  transmitting = true;
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
      dipMs = 0; // minute marker: no dip, so flash writes can't disturb timing
      flushLog();
      static time_t lastHeartbeat = 0;
      if (tv.tv_sec - lastHeartbeat >= (time_t)HEARTBEAT_S) {
        prefs.putUInt("alive", tv.tv_sec);
        lastHeartbeat = tv.tv_sec;
      }
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
