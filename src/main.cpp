#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>

#include "secrets.h"

TFT_eSPI tft = TFT_eSPI();

constexpr uint32_t REFRESH_INTERVAL = 30000;
constexpr uint32_t UI_UPDATE_INTERVAL = 1000;

unsigned long lastRefresh = 0;
unsigned long lastUiUpdate = 0;

float currentCpu = 0;
float currentRam = 0;
float currentDisk = 0;
float currentDbLatency = 0;

bool currentDbOk = false;
bool haveStats = false;

const char* TITLE = "MANAGEME";


// Helpers
uint16_t metricColor(float value) {
    if (value >= 90)
        return TFT_RED;

    if (value >= 75)
        return TFT_ORANGE;

    return TFT_GREEN;
}


void drawBar(
    int x,
    int y,
    int width,
    int height,
    float value,
    uint16_t color
) {
    value = constrain(value, 0, 100);

    tft.drawRoundRect(x, y, width, height, 2, TFT_DARKGREY);

    int fillWidth = (width - 2) * value / 100.0;

    if (fillWidth > 0) {
        tft.fillRect(x + 1, y + 1, fillWidth, height - 2, color);
    }
}


void drawMetric(const char* name, float value, int y) {
    uint16_t color = metricColor(value);

    // Metric label
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);

    tft.setCursor(8, y);
    tft.print(name);

    // Progress bar
    drawBar(42, y, 135, 10, value, color);

    // Percentage
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(184, y);

    tft.printf("%3.0f%%", value);
}


// Screen states
void drawLoadingScreen() {
    tft.fillScreen(TFT_BLACK);

    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(2);

    tft.setCursor(50, 40);
    tft.println("MANAGEME");

    tft.setTextColor(TFT_DARKGREY);
    tft.setTextSize(1);

    tft.setCursor(72, 75);
    tft.println("Connecting...");
}


void drawErrorScreen(const char* message) {
    tft.fillScreen(TFT_BLACK);

    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(2);

    tft.setCursor(10, 15);
    tft.println("SERVER ERROR");

    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);

    tft.setCursor(10, 50);
    tft.println(message);

    tft.setCursor(10, 100);

    if (WiFi.status() == WL_CONNECTED) {
        int rssi = WiFi.RSSI();

        const char* signalStrength;

        if (rssi >= -50) {
            signalStrength = "Excellent";
        } else if (rssi >= -60) {
            signalStrength = "Good";
        } else if (rssi >= -70) {
            signalStrength = "Fair";
        } else {
            signalStrength = "Weak";
        }

        tft.printf("WiFi: %s", signalStrength);
    } else {
        tft.print("WiFi disconnected");
    }
}


// Main dashboard
void drawDashboard(
    float cpu,
    float ram,
    float disk,
    bool dbOk,
    float dbLatency
) {
    tft.fillScreen(TFT_BLACK);

    // Header
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);

    tft.setCursor(6, 5);
    tft.print(TITLE);

    // Status dot
    if (dbOk) {
        tft.fillCircle(205, 10, 4, TFT_GREEN);

        tft.setTextColor(TFT_GREEN);
        tft.setTextSize(1);
        tft.setCursor(213, 7);
        tft.print("OK");
    } else {
        tft.fillCircle(205, 10, 4, TFT_RED);

        tft.setTextColor(TFT_RED);
        tft.setTextSize(1);
        tft.setCursor(213, 7);
        tft.print("ERR");
    }

    tft.drawFastHLine(5, 25, 230, TFT_DARKGREY);

    // Metrics
    drawMetric("CPU", cpu, 36);
    drawMetric("RAM", ram, 55);
    drawMetric("DISK", disk, 74);

    // Footer
    tft.drawFastHLine(5, 94, 230, TFT_DARKGREY);
    tft.setTextSize(1);

    // Database
    tft.setCursor(8, 105);

    if (dbOk) {
        tft.setTextColor(TFT_GREEN);
        tft.print("DB ");

        tft.setTextColor(TFT_WHITE);
        tft.printf("%.1fms", dbLatency);
    } else {
        tft.setTextColor(TFT_RED);
        tft.print("DB DOWN");
    }

    // WiFi RSSI
    tft.setTextColor(TFT_LIGHTGREY);
    tft.setCursor(155, 105);

    tft.printf("WiFi %d", WiFi.RSSI());
}


void drawUpdatedTime() {
    tft.fillRect(5, 120, 150, 12, TFT_BLACK);

    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(8, 122);

    unsigned long secondsAgo = (millis() - lastRefresh) / 1000;

    tft.printf("Updated %lus ago", secondsAgo);
}


// WiFi
bool connectWiFi() {
    WiFi.mode(WIFI_STA);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(300);
    }

    return WiFi.status() == WL_CONNECTED;
}


// API
bool fetchStats() {
    if (WiFi.status() != WL_CONNECTED) {
        if (!connectWiFi()) {
            drawErrorScreen("Cannot connect to WiFi");
            return false;
        }
    }

    HTTPClient http;

    http.begin(MONITOR_URL);

    String authHeader = "Bearer ";
    authHeader += MONITOR_TOKEN;

    http.addHeader("Authorization", authHeader);

    int status = http.GET();

    if (status != HTTP_CODE_OK) {
        Serial.printf("HTTP error: %d\n", status);

        char message[40];

        snprintf(message, sizeof(message), "HTTP error: %d", status);

        drawErrorScreen(message);
        http.end();

        return false;
    }

    JsonDocument doc;

    DeserializationError error = deserializeJson(doc, http.getStream());

    if (error) {
        Serial.print("JSON error: ");
        Serial.println(error.c_str());

        drawErrorScreen("Invalid JSON response");
        http.end();

        return false;
    }

    currentCpu = doc["server"]["cpu"] | 0.0;
    currentRam = doc["server"]["ram"] | 0.0;
    currentDisk = doc["server"]["disk"] | 0.0;

    const char* dbStatus = doc["database"]["status"] | "error";

    currentDbLatency = doc["database"]["latency_ms"] | 0.0;
    currentDbOk = strcmp(dbStatus, "ok") == 0;

    haveStats = true;
    lastRefresh = millis();
    
    http.end();

    drawDashboard(currentCpu, currentRam, currentDisk, currentDbOk, currentDbLatency);

    return true;
}


void setup() {
    Serial.begin(115200);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

    tft.init();
    tft.setRotation(1);

    drawLoadingScreen();

    if (!connectWiFi()) {
        drawErrorScreen("WiFi connection failed");
        return;
    }

    fetchStats();
}


void loop() {
    unsigned long now = millis();

    if (now - lastRefresh >= REFRESH_INTERVAL) {
        fetchStats();
    }

    if (haveStats && now - lastUiUpdate >= UI_UPDATE_INTERVAL) {
        lastUiUpdate = now;
        drawUpdatedTime();
    }

    delay(100);
}
