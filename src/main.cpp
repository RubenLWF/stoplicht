#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_sntp.h>
#include <time.h>
#include <ArduinoJson.h>
#include "secrets.h"

const int RED_PIN = 25;
const int YELLOW_PIN = 26;
const int GREEN_PIN = 27;

const char *BEER_TIME_URL = "https://aurora.gewis.nl/api/handler/screen/info/beer-time";

const char *NTP_SERVER = "time.gewis.nl";
const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3"; // Europe/Amsterdam

const unsigned long REFRESH_INTERVAL_MS = 15UL * 60UL * 1000UL;
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
unsigned long lastLightCheckMs = 0;

void setLight(Light light)
{
  digitalWrite(RED_PIN, light == LIGHT_RED ? HIGH : LOW);
  digitalWrite(YELLOW_PIN, light == LIGHT_ORANGE ? HIGH : LOW);
  digitalWrite(GREEN_PIN, light == LIGHT_GREEN ? HIGH : LOW);
}

void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to WiFi \"%s\"", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWiFi connected, IP address: %s\n", WiFi.localIP().toString().c_str());
}

void onTimeSynced(struct timeval *tv)
{
  struct tm local;
  localtime_r(&tv->tv_sec, &local);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local);
  Serial.printf("Time synced from %s: %s\n", NTP_SERVER, buf);
}

// Restarting SNTP sends a request right away; the result is logged by onTimeSynced.
void syncTime()
{
  configTzTime(TIMEZONE, NTP_SERVER);
}

void fetchBeerTime()
{
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, BEER_TIME_URL);
  http.addHeader("x-api-key", AURORA_API_KEY);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK)
  {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    const char *beerTime = doc["beerTime"];
    int hour, minute;
    if (!error && beerTime && sscanf(beerTime, "%d:%d", &hour, &minute) == 2 &&
        hour >= 0 && hour < 24 && minute >= 0 && minute < 60)
    {
      beerTimeMinutes = hour * 60 + minute;
      Serial.printf("Beer time: %02d:%02d\n", hour, minute);
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

  sntp_set_time_sync_notification_cb(onTimeSynced);

  connectWiFi();
  syncTime();
  fetchBeerTime();
  lastRefreshMs = millis();
  updateLight();
  lastLightCheckMs = millis();
}

void loop()
{
  if (millis() - lastRefreshMs >= REFRESH_INTERVAL_MS)
  {
    lastRefreshMs = millis();
    syncTime();
    fetchBeerTime();
  }

  if (millis() - lastLightCheckMs >= LIGHT_CHECK_INTERVAL_MS)
  {
    lastLightCheckMs = millis();
    updateLight();
  }
}
