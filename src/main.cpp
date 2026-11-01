#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#include "secrets.h"

const int RED_PIN = 25;
const int YELLOW_PIN = 26;
const int GREEN_PIN = 27;

const char *BEER_TIME_URL = "https://aurora.gewis.nl/api/handler/screen/info/beer-time";

const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3"; // Europe/Amsterdam

// const unsigned long REFRESH_INTERVAL_MS = 15UL * 60UL * 1000UL;
const unsigned long REFRESH_INTERVAL_MS = 15UL * 60UL * 1000UL;
// After a failed refresh (WiFi down or request failed), try again this soon.
const unsigned long RETRY_INTERVAL_MS = 30UL * 1000UL;
const unsigned long LIGHT_CHECK_INTERVAL_MS = 1000;

// Orange is shown this many minutes before beer time.
const int WARNING_MINUTES = 15;

enum Light
{
  LIGHT_RED,
  LIGHT_ORANGE,
  LIGHT_GREEN,
};

const char *LIGHT_NAMES[] = {"red", "orange", "green"};

// Minutes since midnight; -1 until the API has returned a valid beer time.
int beerTimeMinutes = -1;
Light currentLight = LIGHT_RED;

unsigned long lastRefreshMs = 0;
unsigned long refreshDelayMs = REFRESH_INTERVAL_MS;
unsigned long lastLightCheckMs = 0;

void setLight(Light light)
{
  digitalWrite(RED_PIN, light == LIGHT_RED ? HIGH : LOW);
  digitalWrite(YELLOW_PIN, light == LIGHT_ORANGE ? HIGH : LOW);
  digitalWrite(GREEN_PIN, light == LIGHT_GREEN ? HIGH : LOW);
}

// Runs on the WiFi event task; logs why the connection dropped.
void onWiFiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
  uint8_t reason = info.wifi_sta_disconnected.reason;
  Serial.printf("WiFi disconnected, reason %u: %s\n", reason, WiFi.disconnectReasonName((wifi_err_reason_t)reason));
}

void onWiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
  Serial.printf("WiFi got IP address: %s, signal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

void connectWiFi()
{
  WiFi.onEvent(onWiFiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(onWiFiGotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to WiFi \"%s\"", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
}

// Sets the system clock from an HTTP Date header, e.g. "Thu, 22 Oct 2026 12:52:33 GMT".
bool setClockFromHttpDate(const String &date)
{
  static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monthName[4];
  int day, year, hour, minute, second;
  if (sscanf(date.c_str(), "%*3s, %d %3s %d %d:%d:%d GMT", &day, monthName, &year, &hour, &minute, &second) != 6 ||
      strlen(monthName) != 3 || year < 2000)
  {
    return false;
  }
  const char *found = strstr(MONTHS, monthName);
  if (!found || (found - MONTHS) % 3 != 0)
  {
    return false;
  }
  int month = (found - MONTHS) / 3 + 1;

  // Days since 1970-01-01 (days_from_civil, valid for years >= 0).
  int y = year - (month <= 2);
  int era = y / 400;
  int yearOfEra = y - era * 400;
  int dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  long days = era * 146097L + dayOfEra - 719468L;

  struct timeval tv = {(time_t)(days * 86400L + hour * 3600L + minute * 60L + second), 0};
  settimeofday(&tv, nullptr);
  return true;
}

// Parses "HH:MM" into minutes since midnight; returns -1 if invalid.
int parseTime(const char *text)
{
  int hour, minute;
  if (text && sscanf(text, "%d:%d", &hour, &minute) == 2 &&
      hour >= 0 && hour < 24 && minute >= 0 && minute < 60)
  {
    return hour * 60 + minute;
  }
  return -1;
}

// Returns true when a valid beer time was received.
bool fetchBeerTime()
{
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, BEER_TIME_URL);
  http.addHeader("x-api-key", AURORA_API_KEY);
  const char *headerKeys[] = {"Date"};
  http.collectHeaders(headerKeys, 1);

  int httpCode = http.GET();
  // The clock is taken from the server's Date header, so any HTTP response sets it.
  if (httpCode > 0 && !setClockFromHttpDate(http.header("Date")))
  {
    Serial.printf("No usable Date header: \"%s\"\n", http.header("Date").c_str());
  }

  bool ok = false;
  if (httpCode == HTTP_CODE_OK)
  {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    const char *beerTime = doc["beerTime"];
    int minutes = error ? -1 : parseTime(beerTime);
    if (minutes >= 0)
    {
      Serial.printf("Beer time: %s\n", beerTime);
      beerTimeMinutes = minutes;
      ok = true;
    }
    else
    {
      Serial.printf("Unexpected beer time response: %s\n", payload.c_str());
    }
  }
  else if (httpCode > 0)
  {
    Serial.printf("Beer time HTTP %d: %s\n", httpCode, http.getString().c_str());
  }
  else
  {
    Serial.printf("Beer time request failed: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  return ok;
}

void logStatus()
{
  struct tm now;
  char clock[32] = "not synced";
  if (getLocalTime(&now, 0))
  {
    strftime(clock, sizeof(clock), "%H:%M:%S %Z", &now);
  }
  if (beerTimeMinutes >= 0)
  {
    Serial.printf("Status: clock %s, beer time %02d:%02d, light %s\n", clock, beerTimeMinutes / 60,
                  beerTimeMinutes % 60, LIGHT_NAMES[currentLight]);
  }
  else
  {
    Serial.printf("Status: clock %s, beer time unknown, light %s\n", clock, LIGHT_NAMES[currentLight]);
  }
}

// Fetches the beer time if WiFi is up; otherwise asks the WiFi driver to reconnect.
// A failure schedules a quick retry instead of waiting a full refresh interval.
void refresh()
{
  bool ok = false;
  if (WiFi.status() == WL_CONNECTED)
  {
    ok = fetchBeerTime();
  }
  else
  {
    Serial.printf("WiFi not connected (status %d), reconnecting\n", WiFi.status());
    WiFi.reconnect();
  }
  refreshDelayMs = ok ? REFRESH_INTERVAL_MS : RETRY_INTERVAL_MS;
  lastRefreshMs = millis();
  logStatus();
}

// Green from beer time until the end of the day; at 00:00 the time of day wraps to 0, so it turns red again.
// Stays on the current light until both the clock and the beer time are known.
void updateLight()
{
  struct tm now;
  if (beerTimeMinutes < 0 || !getLocalTime(&now, 0))
  {
    return;
  }

  int nowMinutes = now.tm_hour * 60 + now.tm_min;
  Light light = LIGHT_RED;
  if (nowMinutes >= beerTimeMinutes)
  {
    light = LIGHT_GREEN;
  }
  else if (nowMinutes >= beerTimeMinutes - WARNING_MINUTES)
  {
    light = LIGHT_ORANGE;
  }

  if (light != currentLight)
  {
    currentLight = light;
    setLight(light);
    Serial.printf("%02d:%02d light -> %s\n", now.tm_hour, now.tm_min, LIGHT_NAMES[light]);
  }
}

void setup()
{
  Serial.begin(115200);

  pinMode(RED_PIN, OUTPUT);
  pinMode(YELLOW_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);

  setLight(currentLight);

  setenv("TZ", TIMEZONE, 1);
  tzset();

  connectWiFi();
  refresh();
  updateLight();
  lastLightCheckMs = millis();
}

void loop()
{
  if (millis() - lastRefreshMs >= refreshDelayMs)
  {
    refresh();
  }

  if (millis() - lastLightCheckMs >= LIGHT_CHECK_INTERVAL_MS)
  {
    lastLightCheckMs = millis();
    updateLight();
  }
}
