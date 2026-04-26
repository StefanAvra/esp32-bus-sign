#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <FastLED.h>
#include "HT_SSD1306Wire.h"

#include "config.h"
#include "secrets.h"

// ============================================================
// Types
// ============================================================

enum Status : uint8_t { STATUS_OK, STATUS_NO_DEPARTURES, STATUS_API_FAIL, STATUS_WIFI_DOWN };

#define MAX_EVENTS 10
#define MAX_LINES   4

struct Departure {
  time_t tsUtc;
  char   line[8];
  char   destination[32];
};

// One "row" per distinct bus line — derived from events for rendering.
struct LineView {
  char   line[8];
  char   destination[32];
  time_t nextTs;        // earliest upcoming departure for this line
  int    minsUntil;     // minutes until nextTs (< 0 allowed for imminent)
};

struct State {
  Departure events[MAX_EVENTS];
  int       count        = 0;
  int       failCount    = 0;
  uint8_t   shiftIdx     = 0;
  bool      inNightMode  = false;
  unsigned long lastTickMs      = 0;
  unsigned long lastFetchMs     = 0;
  unsigned long lastShiftMs     = 0;
  unsigned long disconnectedMs  = 0;   // 0 while connected; else millis of first drop
};

// Pixel-shift cycle for OLED burn-in mitigation.
static const uint8_t SHIFT_PATTERN[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };

// ============================================================
// Globals
// ============================================================

// OLED: addr, freq, SDA, SCL, geometry, RST — pins come from the Heltec board variant.
static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

State  state;
CRGB   leds[LED_COUNT];
uint8_t lastFrameHash = 0xFF;  // forces first draw
bool   ntpSynced = false;
unsigned long lastWifiLogMs = 0;

// Cached per-line view, refreshed on the 1 Hz tick and consumed by the
// presentation layer (renderLeds runs every loop iteration for smooth pulse).
static LineView gViews[MAX_LINES];
static int      gNViews = 0;

// ============================================================
// Forward declarations
// ============================================================

void   VextON();
void   showSplash(const char* msg);
void   connectWiFi();
bool   waitForNtp(uint32_t timeoutMs);
bool   fetchDepartures();
time_t parseIso8601(const char* s);
int    buildLineViews(LineView* out, int max);
Status computeStatus(int lineCount);
bool   isNightMode();
void   renderOled();
void   renderLeds();
uint8_t crc8(const uint8_t* data, size_t len);

// ============================================================
// setup / loop
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println("=== esp32-bus-sign boot ===");

  // Power the OLED rail (Vext, active LOW on Heltec V2) BEFORE display.init().
  VextON();
  delay(100);
  display.init();
  display.setContrast(OLED_CONTRAST);

  FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(leds, LED_COUNT);
  FastLED.setBrightness(LED_BRIGHTNESS);
  FastLED.clear(true);

  showSplash("Starting...");

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    showSplash("Syncing time...");
    configTzTime(TZ_STRING, NTP_SERVER);
    if (waitForNtp(10000)) {
      ntpSynced = true;
      showSplash("Fetching...");
      fetchDepartures();
    } else {
      Serial.println("NTP sync failed");
    }
  }

  state.lastTickMs    = millis();
  state.lastFetchMs   = millis();
  state.lastShiftMs   = millis();
}

void loop() {
  unsigned long now = millis();

  // 1 Hz tick: state refresh + OLED redraw (presentation runs every loop).
  if (now - state.lastTickMs >= TICK_MS) {
    state.lastTickMs = now;

    bool night = isNightMode();
    if (night != state.inNightMode) {
      state.inNightMode = night;
      if (night) {
        if (NIGHT_OLED_CONTRAST == 0) display.displayOff();
        else                          display.setContrast(NIGHT_OLED_CONTRAST);
        FastLED.setBrightness(NIGHT_LED_BRIGHTNESS);
      } else {
        if (NIGHT_OLED_CONTRAST == 0) display.displayOn();
        display.setContrast(OLED_CONTRAST);
        FastLED.setBrightness(LED_BRIGHTNESS);
        state.lastFetchMs = 0;        // refresh right after waking
        lastFrameHash     = 0xFF;
      }
    }

    if (!(night && NIGHT_OLED_CONTRAST == 0)) {
      gNViews = buildLineViews(gViews, MAX_LINES);
      renderOled();
    }
  }

  // Every loop: LED presentation — keeps beatsin8() animating smoothly.
  // When dimmed, FastLED.setBrightness scales the output; at 0 the strip is dark.
  if (!(state.inNightMode && NIGHT_OLED_CONTRAST == 0)) {
    renderLeds();
  }

  // WiFi: auto-reconnect occasionally wedges after a reason=34 drop.
  // Force a reconnect if we've been disconnected for > 15 s.
  if (WiFi.status() != WL_CONNECTED) {
    if (state.disconnectedMs == 0) state.disconnectedMs = now;
    if (now - state.disconnectedMs > 15000) {
      Serial.println("[wifi] auto-reconnect stalled, forcing WiFi.begin()");
      WiFi.disconnect();
      delay(100);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      state.disconnectedMs = now;   // restart the timer; try again in 15 s
    }
  } else {
    state.disconnectedMs = 0;
  }

  if (now - lastWifiLogMs > 10000) {
    lastWifiLogMs = now;
    wl_status_t ws = WiFi.status();
    Serial.printf("WiFi status=%d ip=%s rssi=%d heap=%u\n",
                  (int)ws,
                  WiFi.localIP().toString().c_str(),
                  (int)WiFi.RSSI(),
                  (unsigned)ESP.getFreeHeap());
  }

  // Late NTP sync if WiFi came up after setup()
  if (!ntpSynced && WiFi.status() == WL_CONNECTED) {
    Serial.println("Late NTP sync...");
    configTzTime(TZ_STRING, NTP_SERVER);
    if (waitForNtp(5000)) {
      ntpSynced = true;
      state.lastFetchMs = 0;  // fetch ASAP
      Serial.println("NTP synced");
    }
  }

  // 30 s fetch (skipped when OLED is fully off)
  const bool oledFullyOff = state.inNightMode && NIGHT_OLED_CONTRAST == 0;
  if (!oledFullyOff
      && WiFi.status() == WL_CONNECTED
      && (now - state.lastFetchMs >= FETCH_INTERVAL_MS || state.lastFetchMs == 0)) {
    state.lastFetchMs = now;
    fetchDepartures();
  }

  if (now - state.lastShiftMs >= SHIFT_INTERVAL_MS) {
    state.lastShiftMs = now;
    state.shiftIdx    = (state.shiftIdx + 1) % 4;
    lastFrameHash     = 0xFF;
  }

  delay(20);
}

// ============================================================
// Vext (OLED power) — active LOW on Heltec WiFi Kit 32 V2
// ============================================================

void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

// ============================================================
// WiFi + NTP
// ============================================================

// Sine-breathing blue pulse across all 4 LEDs while WiFi is connecting.
static void pulseConnecting() {
  uint8_t b = sin8(millis() >> 2);    // ~1.2 Hz smooth breath
  fill_solid(leds, LED_COUNT, CRGB(0, 0, b));
  FastLED.show();
}

// Logs every STA state change with the numeric reason code.
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[wifi] STA_CONNECTED");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[wifi] GOT_IP %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      // Reasons: 2=AUTH_EXPIRE 4=ASSOC_EXPIRE 8=ASSOC_LEAVE
      // 200=BEACON_TIMEOUT 201=NO_AP_FOUND 202=AUTH_FAIL
      // 204=HANDSHAKE_TIMEOUT 205=CONNECTION_FAIL
      Serial.printf("[wifi] DISCONNECTED reason=%d\n",
                    (int)info.wifi_sta_disconnected.reason);
      break;
    default:
      break;
  }
}

void connectWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // <- big reliability win on ESP32
  WiFi.onEvent(onWiFiEvent);
  WiFi.disconnect(true, true);
  delay(100);

  // One-shot scan: confirms the AP is on a 2.4 GHz channel and in range.
  Serial.println("WiFi scan:");
  int n = WiFi.scanNetworks(false, true);
  bool sawTarget = false;
  for (int i = 0; i < n; i++) {
    const String s = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    int ch   = WiFi.channel(i);
    Serial.printf("  %2d  %-32s  ch=%d  rssi=%d\n", i, s.c_str(), ch, rssi);
    if (s == WIFI_SSID) sawTarget = true;
  }
  Serial.printf("target '%s' visible: %s\n", WIFI_SSID, sawTarget ? "yes" : "NO");

  WiFi.setAutoReconnect(true);       // let the driver handle retries
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);   // skip WPA3 negotiation quirks
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to %s (ssidLen=%d passLen=%d) ...\n",
                WIFI_SSID, (int)strlen(WIFI_SSID), (int)strlen(WIFI_PASS));
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    pulseConnecting();
    delay(30);
  }
  FastLED.clear(true);               // stop the pulse regardless of outcome
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK: %s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("WiFi connect timed out (status=%d) - auto-reconnect keeps trying\n",
                  (int)WiFi.status());
  }
}

bool waitForNtp(uint32_t timeoutMs) {
  unsigned long start = millis();
  while (time(nullptr) < 1700000000L && millis() - start < timeoutMs) {
    delay(200);
  }
  return time(nullptr) >= 1700000000L;
}

// ============================================================
// VVS fetch
// ============================================================

bool fetchDepartures() {
  // Step 1: DNS — separates name-resolution failures from TLS failures.
  IPAddress ip;
  if (!WiFi.hostByName("www3.vvs.de", ip)) {
    Serial.println("DNS lookup for www3.vvs.de failed");
    state.failCount++;
    return false;
  }
  Serial.printf("DNS www3.vvs.de -> %s (rssi=%d, heap=%u)\n",
                ip.toString().c_str(),
                (int)WiFi.RSSI(),
                (unsigned)ESP.getFreeHeap());

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, VVS_URL)) {
    state.failCount++;
    Serial.println("http.begin failed");
    return false;
  }

  unsigned long t0 = millis();
  int code = http.GET();
  unsigned long elapsed = millis() - t0;
  if (code != 200) {
    // -1 conn refused, -2 send hdr, -3 send payload, -4 not connected,
    // -5 conn lost, -11 read timeout. Positive = HTTP response code.
    Serial.printf("HTTP code=%d after %lu ms (rssi=%d)\n",
                  code, elapsed, (int)WiFi.RSSI());
    http.end();
    state.failCount++;
    return false;
  }

  // Buffer the whole body before parsing. Streaming the parser off a slow
  // TLS socket was hitting IncompleteInput on mid-transfer stalls.
  String body = http.getString();
  Serial.printf("HTTP 200, body=%u bytes (total %lu ms)\n",
                (unsigned)body.length(), millis() - t0);
  http.end();

  JsonDocument filter;
  filter["stopEvents"][0]["departureTimeEstimated"]               = true;
  filter["stopEvents"][0]["departureTimePlanned"]                 = true;
  filter["stopEvents"][0]["transportation"]["number"]             = true;
  filter["stopEvents"][0]["transportation"]["destination"]["name"]= true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body,
                                             DeserializationOption::Filter(filter));

  if (err) {
    Serial.printf("JSON: %s\n", err.c_str());
    state.failCount++;
    return false;
  }

  JsonArray events = doc["stopEvents"].as<JsonArray>();
  int n = 0;
  for (JsonObject ev : events) {
    if (n >= MAX_EVENTS) break;
    const char* number = ev["transportation"]["number"] | "";
    const char* dest   = ev["transportation"]["destination"]["name"] | "";
    const char* t      = ev["departureTimeEstimated"] | ev["departureTimePlanned"] | "";
    if (!number || !*number) continue;
    if (!t      || !*t)      continue;
    time_t ts = parseIso8601(t);
    if (ts == 0) continue;
    state.events[n].tsUtc = ts;
    strncpy(state.events[n].line,        number, sizeof(state.events[n].line) - 1);
    state.events[n].line[sizeof(state.events[n].line) - 1] = 0;
    strncpy(state.events[n].destination, dest,   sizeof(state.events[n].destination) - 1);
    state.events[n].destination[sizeof(state.events[n].destination) - 1] = 0;
    n++;
  }
  state.count     = n;
  state.failCount = 0;
  Serial.printf("Fetched %d events\n", n);
  return true;
}

// ============================================================
// Time math
// ============================================================

// Convert a struct tm interpreted as UTC to a Unix timestamp, without
// touching the global TZ state. Uses Howard Hinnant's civil-from-days
// algorithm; valid for any date in tm's representable range.
static time_t utcTmToTime(const struct tm* tm) {
  int year  = tm->tm_year + 1900;
  int month = tm->tm_mon + 1;
  int day   = tm->tm_mday;

  if (month <= 2) { year--; month += 12; }
  int era = (year >= 0 ? year : year - 399) / 400;
  unsigned yoe = (unsigned)(year - era * 400);
  unsigned doy = (153U * (month - 3) + 2) / 5 + day - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + (long)doe - 719468;

  return (time_t)days * 86400
       + (time_t)tm->tm_hour * 3600
       + (time_t)tm->tm_min  * 60
       + (time_t)tm->tm_sec;
}

time_t parseIso8601(const char* s) {
  if (!s || !*s) return 0;
  struct tm tm = {};
  if (strptime(s, "%Y-%m-%dT%H:%M:%SZ", &tm) == nullptr) return 0;
  return utcTmToTime(&tm);
}

// Collapse events -> one LineView per (distinct) line, each holding the
// earliest non-stale departure. Sorted by nextTs ascending.
int buildLineViews(LineView* out, int max) {
  time_t now = time(nullptr);
  if (now < 1700000000L || state.count == 0) return 0;

  int n = 0;
  for (int i = 0; i < state.count; i++) {
    const Departure& d = state.events[i];
    // Accept departures up to 60 s in the past — bus might still be at stop.
    if (d.tsUtc < now - 60) continue;

    int idx = -1;
    for (int j = 0; j < n; j++) {
      if (strcmp(out[j].line, d.line) == 0) { idx = j; break; }
    }
    if (idx >= 0) {
      if (d.tsUtc < out[idx].nextTs) {
        out[idx].nextTs = d.tsUtc;
        strncpy(out[idx].destination, d.destination, sizeof(out[idx].destination) - 1);
        out[idx].destination[sizeof(out[idx].destination) - 1] = 0;
      }
    } else if (n < max) {
      strncpy(out[n].line,        d.line,        sizeof(out[n].line) - 1);
      out[n].line[sizeof(out[n].line) - 1] = 0;
      strncpy(out[n].destination, d.destination, sizeof(out[n].destination) - 1);
      out[n].destination[sizeof(out[n].destination) - 1] = 0;
      out[n].nextTs = d.tsUtc;
      n++;
    }
  }

  for (int i = 1; i < n; i++) {
    LineView tmp = out[i];
    int j = i - 1;
    while (j >= 0 && out[j].nextTs > tmp.nextTs) { out[j+1] = out[j]; j--; }
    out[j+1] = tmp;
  }

  for (int i = 0; i < n; i++) {
    long diff = (long)(out[i].nextTs - now);
    out[i].minsUntil = (int)(diff / 60);
  }
  return n;
}

Status computeStatus(int lineCount) {
  if (WiFi.status() != WL_CONNECTED) return STATUS_WIFI_DOWN;
  if (state.failCount >= 2)          return STATUS_API_FAIL;
  if (state.count == 0)              return STATUS_NO_DEPARTURES;
  if (lineCount == 0)                return STATUS_NO_DEPARTURES;
  return STATUS_OK;
}

bool isNightMode() {
  if (!NIGHT_WINDOW_ENABLED) return false;
  time_t now = time(nullptr);
  if (now < 1700000000L) return false;
  struct tm lt;
  localtime_r(&now, &lt);
  int nowMin   = lt.tm_hour * 60 + lt.tm_min;
  int startMin = NIGHT_START_HOUR * 60 + NIGHT_START_MIN;
  int endMin   = NIGHT_END_HOUR   * 60 + NIGHT_END_MIN;
  if (startMin == endMin) return false;
  if (startMin <  endMin) return nowMin >= startMin && nowMin < endMin;
  return nowMin >= startMin || nowMin < endMin;   // wraps midnight
}

// ============================================================
// Rendering — OLED
// ============================================================

void drawBusStopSign(int16_t xc, int16_t yc, int16_t r, uint8_t thickness) {
  display.setColor(WHITE);
  display.fillCircle(xc, yc, r);

  display.setColor(BLACK);
  
  display.fillCircle(xc, yc, r - thickness);
  
  display.setColor(WHITE);
  int16_t h_h = (int16_t)(r * 1.1);
  int16_t h_w = (int16_t)(r * 0.8);
  int16_t x_off = xc - (h_w / 2);
  int16_t y_off = yc - (h_h / 2);

  display.fillRect(x_off, y_off, thickness, h_h); // Left
  display.fillRect(x_off + h_w - thickness, y_off, thickness, h_h); // Right
  display.fillRect(x_off, yc - (thickness / 2), h_w, thickness); // Middle
}


void showSplash(const char* msg) {
  display.clear();

  // Bus stop sign on the left, title + status on the right.
  drawBusStopSign(20, 32, 16, 4);

  display.setColor(WHITE);
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(44, 14, "Bus Sign");

  display.setFont(ArialMT_Plain_10);
  display.drawString(44, 36, msg);

  display.display();
}

// Draw the stylized line number block (inverted rectangle + number text).
// Returns the pixel x just past the right edge of the block.
static int drawLineBlock(int x, int y, int fontH,
                         const uint8_t* font, const char* line) {
  display.setColor(WHITE);
  display.setFont(font);
  int w   = display.getStringWidth(line);
  int boxW = w + 6;
  int boxH = fontH + 2;
  display.drawRect(x, y, boxW, boxH);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(x + 3, y, line);
  return x + boxW;
}

// Truncate `s` in place so its pixel width fits `maxW` in the current font.
static void truncateToWidth(char* s, int maxW) {
  while (display.getStringWidth(s) > maxW && s[0]) {
    s[strlen(s) - 1] = 0;
  }
}

// One compact row: [LL] destination     M min HH:MM
static void drawCompactRow(int rowY, int rowH, const LineView& v, int dx, int dy) {
  const uint8_t* font;
  int fontH;
  if (rowH >= 28)      { font = ArialMT_Plain_16; fontH = 16; }
  else                  { font = ArialMT_Plain_10; fontH = 10; }

  int blockY = rowY + (rowH - (fontH + 2)) / 2;
  int afterBlock = drawLineBlock(dx, blockY + dy, fontH, font, v.line);

  // Right-aligned "M min HH:MM"
  struct tm lt;
  localtime_r(&v.nextTs, &lt);
  int m = v.minsUntil < 0 ? 0 : v.minsUntil;
  char right[24];
  snprintf(right, sizeof(right), "%d min  %02d:%02d", m, lt.tm_hour, lt.tm_min);
  int rightW = display.getStringWidth(right);
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(127 + dx, blockY + dy, right);

  // Destination in the middle, truncated to available width.
  int destX   = afterBlock + 4;
  int destMax = (127 + dx - rightW - 4) - destX;
  char dest[32];
  strncpy(dest, v.destination, sizeof(dest));
  dest[sizeof(dest) - 1] = 0;
  truncateToWidth(dest, destMax);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(destX, blockY + dy, dest);
}

// Earliest departure of `line` strictly after `afterTs`, or 0 if none.
static time_t nextDepartureOfLineAfter(const char* line, time_t afterTs) {
  time_t best = 0;
  for (int i = 0; i < state.count; i++) {
    if (strcmp(state.events[i].line, line) != 0) continue;
    if (state.events[i].tsUtc <= afterTs) continue;
    if (best == 0 || state.events[i].tsUtc < best) best = state.events[i].tsUtc;
  }
  return best;
}

// Single-line layout: header row + big minutes number + side clock.
// When secondTs != 0, shows the following departure's clock beneath.
static void drawSingleLayout(const LineView& v, time_t secondTs, int dx, int dy) {
  // Header: [LL] destination
  const uint8_t* font = ArialMT_Plain_16;
  int afterBlock = drawLineBlock(dx, dy, 16, font, v.line);

  char dest[32];
  strncpy(dest, v.destination, sizeof(dest));
  dest[sizeof(dest) - 1] = 0;
  display.setFont(font);
  truncateToWidth(dest, 127 + dx - (afterBlock + 4));
  display.setColor(WHITE);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(afterBlock + 4, dy, dest);

  // Big minutes.
  int m = v.minsUntil < 0 ? 0 : v.minsUntil;
  char mbuf[6];
  snprintf(mbuf, sizeof(mbuf), "%d", m);
  display.setFont(ArialMT_Plain_24);
  display.drawString(2 + dx, 24 + dy, mbuf);
  display.setFont(ArialMT_Plain_10);
  display.drawString(2 + dx, 52 + dy, "min");

  // Right side: primary clock (16pt), and optional "next HH:MM" (10pt).
  struct tm lt;
  localtime_r(&v.nextTs, &lt);
  char clk[8];
  snprintf(clk, sizeof(clk), "%02d:%02d", lt.tm_hour, lt.tm_min);
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(126 + dx, 24 + dy, clk);

  if (secondTs != 0) {
    struct tm lt2;
    localtime_r(&secondTs, &lt2);
    char clk2[16];
    snprintf(clk2, sizeof(clk2), "next %02d:%02d", lt2.tm_hour, lt2.tm_min);
    display.setFont(ArialMT_Plain_10);
    display.drawString(126 + dx, 50 + dy, clk2);
  }
}

void renderOled() {
  LineView views[MAX_LINES];
  int n = buildLineViews(views, MAX_LINES);
  Status st = computeStatus(n);

  const char* statusWord = nullptr;
  switch (st) {
    case STATUS_WIFI_DOWN:     statusWord = "WIFI"; break;
    case STATUS_API_FAIL:      statusWord = "API";  break;
    case STATUS_NO_DEPARTURES: statusWord = "NONE"; break;
    default: break;
  }

  // When only one distinct line, also surface the following departure.
  time_t secondTs = (n == 1) ? nextDepartureOfLineAfter(views[0].line, views[0].nextTs) : 0;

  // Frame-hash check (redraw only on change)
  char hashBuf[144];
  int hLen = snprintf(hashBuf, sizeof(hashBuf), "%s|%u|",
                      statusWord ? statusWord : "OK",
                      (unsigned)state.shiftIdx);
  for (int i = 0; i < n && hLen < (int)sizeof(hashBuf) - 1; i++) {
    hLen += snprintf(hashBuf + hLen, sizeof(hashBuf) - hLen,
                     "%s:%d@%ld;", views[i].line,
                     views[i].minsUntil, (long)views[i].nextTs);
  }
  if (secondTs != 0 && hLen < (int)sizeof(hashBuf) - 1) {
    snprintf(hashBuf + hLen, sizeof(hashBuf) - hLen, "2@%ld;", (long)secondTs);
  }
  uint8_t h = crc8((const uint8_t*)hashBuf, strlen(hashBuf));
  if (h == lastFrameHash) return;
  lastFrameHash = h;

  int dx = SHIFT_PATTERN[state.shiftIdx][0];
  int dy = SHIFT_PATTERN[state.shiftIdx][1];

  display.clear();
  display.setColor(WHITE);

  if (statusWord) {
    display.setFont(ArialMT_Plain_16);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64 + dx, 10 + dy, "Bus Sign");
    display.drawString(64 + dx, 34 + dy, statusWord);
  } else if (n == 1) {
    drawSingleLayout(views[0], secondTs, dx, dy);
  } else {
    int rowH = 64 / n;
    for (int i = 0; i < n; i++) {
      int rowY = i * rowH;
      drawCompactRow(rowY, rowH, views[i], dx, dy);
      if (i < n - 1) {
        display.drawHorizontalLine(dx, (i + 1) * rowH - 1 + dy, 128);
      }
    }
  }

  display.display();
}

// ============================================================
// Rendering — LEDs
// ============================================================

// LED mapping — ALL departures (every returned event, not one-per-line):
//   * Each bus with 1 <= m <= LED_COUNT lights LED[m-1]. Color by urgency:
//     m == 1 -> red, m == 2 -> amber, m >= 3 -> green.
//   * Each bus with m > LED_COUNT ("above window") lights LED[LED_COUNT-1] blue.
//   * Each bus with m <= 0 pulses red on LED[0] at ~1 Hz.
//   * When two buses compete for the same LED, urgency wins:
//     imminent > red > amber > green > blue.
//   * Iterates state.events directly so 2nd/3rd departures of the same line
//     also show up on their own LED positions.
//   * Dynamic with LED_COUNT — scales to 10 later without changes.
void renderLeds() {
  FastLED.clear();

  if (computeStatus(gNViews) != STATUS_OK) {
    FastLED.show();
    return;
  }

  time_t now = time(nullptr);
  if (now < 1700000000L) { FastLED.show(); return; }

  // Per-LED priority: 0=none, 1=blue, 2=green, 3=amber, 4=red, 5=imminent.
  uint8_t pri[LED_COUNT] = {0};
  CRGB    col[LED_COUNT];

  for (int i = 0; i < state.count; i++) {
    long diff = (long)(state.events[i].tsUtc - now);
    if (diff < -60) continue;                    // stale past departure
    int m = (int)(diff / 60);

    int     idx;
    uint8_t p;
    CRGB    c;
    if (m <= 0) {
      uint8_t b = beatsin8(60, 5, LED_BRIGHTNESS); // 1 Hz, 5..brightness breath
      idx = 0;
      p   = 5;
      c   = CRGB(b, 0, 0);
    } else if (m > LED_COUNT) {
      idx = LED_COUNT - 1;
      p   = 1;
      c   = CRGB::Blue;
    } else {
      idx = m - 1;
      if (m == 1)      { p = 4; c = CRGB::Red; }
      else if (m == 2) { p = 3; c = CRGB(255, 140, 0); }
      else             { p = 2; c = CRGB::Green; }
    }
    if (p > pri[idx]) { pri[idx] = p; col[idx] = c; }
  }

  for (int i = 0; i < LED_COUNT; i++) {
    if (pri[i] > 0) leds[i] = col[i];
  }
  FastLED.show();
}

// ============================================================
// Helpers
// ============================================================

uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}
