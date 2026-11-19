/**
 * Holo9 Weather V2.23 for 3.5" ESP32-S3 + ST77922 QSPI Display
 * Copyright (c) 2026 Harald Noedl MIT License.
 */

#include "Arduino.h"
#include "WiFi.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <FS.h>
using namespace fs;
#include <WebServer.h>
#include <ElegantOTA.h>        // Copyright Ayush Sharma 2026 AGPL-3.0 License
#include <TFT_eSPI.h>          // Copyright (c) 2026 Bodmer FreeBSD License
#include "time.h"
#include <Preferences.h>

#include "ST77922.h"           // Custom QSPI display driver
#include "ST77922_Touch.h"     // Custom capacitive touch driver
#include <OpenFontRender.h>    // Vector font engine *** Copyright (c) takkaO FTL License ***
#include "PlusJakarta.h"       // Embedded TTF  *** Copyright (c) Tokotype SIL Open Font License ***
#include "Holo9Presets.h"      // Load Presets (WIFI, Locations, Models etc)

#define OFR_FONT_ARRAY PlusJakarta_min_ttf
#define OFR_FONT_SIZE  PlusJakarta_min_ttf_len

// ============================================================
// HARDWARE INSTANCES
// ============================================================
ST77922 mylcd = ST77922();
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite canvas = TFT_eSprite(&tft);   // Off-screen frame buffer
ST77922_TOUCH touch;
OpenFontRender ofr;
WebServer server(80);
static bool otaServerStarted = false;

// Start ElegantOTA only after WiFi is connected. The guard keeps reconnects
// and settings changes from registering the web server more than once.
void startOTAServer() {
    if (otaServerStarted || WiFi.status() != WL_CONNECTED) {
        return;
    }
    ElegantOTA.begin(&server, OTA_USERNAME, OTA_PASSWORD);
    server.begin();
    otaServerStarted = true;
    Serial.print("OTA: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/update");
}

// Push the sprite buffer to the physical QSPI display
void pushCanvas() {
    mylcd.Fill_Colors(0, 0, mylcd.Get_Width(), mylcd.Get_Height(),
                      (uint16_t *)canvas.getPointer());
}

// ============================================================
// COLOR PALETTE (RGB565)
// ============================================================
#define MY_BLACK      0x0000
#define MY_WHITE      0xFFFF
#define MY_YELLOW     TFT_ORANGE //0xFE20
#define MY_YELLOW02   0xFFF9 
#define MY_BLUE       0x4B10
#define MY_BLUE02     0x6D1F
#define MY_BLUE04     0xAEBF
#define MY_BLUE05     0xDF5F
#define MY_GREY       0x2924
#define MY_GREY02     0x5227
#define MY_GREY03     0x734B
#define MY_LIGHTGREY  0xBDD4
#define MY_RED        0xC800

// ============================================================
// TEXT HELPERS (OpenFontRender wrappers)
// ============================================================

// Unified text draw with alignment
void drawOfrText(const String& str, int x, int y, int size, uint16_t color,
                 Align align = Align::TopLeft, uint16_t bg = MY_BLACK) {
    ofr.setCursor(x, y);
    ofr.setFontSize(size);
    ofr.setFontColor(color, bg);
    ofr.setAlignment(align);
    ofr.printf(str.c_str());
}

// Convenience wrappers for the three common alignments
inline void drawOfrString(const String& s, int x, int y, int sz, uint16_t c, uint16_t bg = MY_BLACK) {
    drawOfrText(s, x, y, sz, c, Align::TopLeft, bg);
}
inline void drawOfrCenterString(const String& s, int x, int y, int sz, uint16_t c, uint16_t bg = MY_BLACK) {
    drawOfrText(s, x, y, sz, c, Align::TopCenter, bg);
}
inline void drawOfrRightString(const String& s, int x, int y, int sz, uint16_t c, uint16_t bg = MY_BLACK) {
    drawOfrText(s, x, y, sz, c, Align::TopRight, bg);
}

// ============================================================
// WEATHER DATA
// ============================================================
struct CurrentWeather {
    float temp;
    int humidity;
    float windSpeed;
    int windDirection;
    int weatherCode;
    float pressure;
    String sunrise;   // "HH:MM"
    String sunset;    // "HH:MM"
};

struct HourlyPoint { float temp; float precip; float windSpeed; int code; };
struct DailyPoint  { float tempMax; float tempMin; float precipSum; int code; };

CurrentWeather currentWeather;
HourlyPoint hourlyForecast[120];
DailyPoint  dailyForecast[10];

int hourlyCount = 0;   // number of valid hourly entries received
int dailyCount  = 0;   // number of valid daily entries received

// ============================================================
// UI STATE
// ============================================================
enum ScreenState {
    SCREEN_CURRENT,       // rotating screen 1
    SCREEN_SENSE_CLOCK,   // rotating screen 2 (Microsoft Sense Clock-style)
    SCREEN_3DAY_HOURLY,   // rotating screen 3
    SCREEN_10DAY_DAILY,   // rotating screen 4
    SCREEN_SETTINGS
};
ScreenState currentScreen = SCREEN_CURRENT;

unsigned long lastFetchTime = 0;
unsigned long lastSuccessTime = 0;
unsigned long lastWifiAttempt = 0;
bool fetchFailed = false;
bool weatherFetchPending = false;
unsigned long lastTouchTime = 0;

// WiFi preset attempts are advanced in loop() so touch remains responsive.
static const unsigned long WIFI_CONNECT_TIMEOUT = 12000UL;
int wifiCycleOffset = 0;
int wifiAttemptIdx = 0;
String savedWifiSsid;
String savedWifiPassword;
bool savedWifiAvailable = false;
bool savedWifiAttempted = false;
bool wifiAttemptUsesSaved = false;
bool wifiAttemptActive = false;
bool wifiFailureTransitionPending = false;
unsigned long wifiFailureTransitionAt = 0;
unsigned long wifiNextRetryAt = 0;

// Forward declarations
void drawWeatherIcon(int x, int y, int size, int wmoCode, bool isNight = false);
void drawSettingsButton();
void drawCurrentScreen();
void drawSenseClockScreen();
void draw3DayScreen();
void draw10DayScreen();
void drawSettingsScreen();
bool fetchWeatherData();

static const char* const dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* const dayNamesFull[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char* const monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Small accessors
String getLocationName() { return LOCATIONS[activeLocationIdx].name; }
String getTempSymbol()   { return (activeUnit == UNIT_METRIC) ? "C" : "F"; }
String getSpeedSymbol()  { return (activeUnit == UNIT_METRIC) ? "km/h" : "mph"; }
String getLengthSymbol()  { return (activeUnit == UNIT_METRIC) ? "mm" : "in"; }

// Apply selected location's timezone to the system clock
void updateSystemTimezone() {
    setenv("TZ", LOCATIONS[activeLocationIdx].posixTz, 1);
    tzset();
}

// Convert wind degrees to compass point
String getWindDirStr(int deg) {
    if (deg >= 338 || deg < 23)  return "N";
    if (deg < 68)  return "NE";
    if (deg < 113) return "E";
    if (deg < 158) return "SE";
    if (deg < 203) return "S";
    if (deg < 248) return "SW";
    if (deg < 293) return "W";
    return "NW";
}

// ============================================================
// TIME HELPERS
// ============================================================
struct tm getNow() {
    struct tm t = {};
    getLocalTime(&t);
    return t;
}

// "HH:MM" -> minutes since midnight, or -1 if invalid
int getMinutesFromTimeStr(const String& timeStr) {
    if (timeStr.length() < 5) return -1;
    return timeStr.substring(0, 2).toInt() * 60 + timeStr.substring(3, 5).toInt();
}

// True if the given time-of-day falls outside daylight hours
bool isNightAt(int minutesSinceMidnight, int srMin, int ssMin) {
    return (minutesSinceMidnight < srMin || minutesSinceMidnight >= ssMin);
}

// ============================================================
// PREFERENCES (persistent settings)
// ============================================================
Preferences preferences;

void saveSettings() {
    preferences.begin("holo9_settings", false);
    preferences.putInt("locIdx",   activeLocationIdx);
    preferences.putInt("modelIdx", activeModelIdx);
    preferences.putInt("wifiIdx",  activeWifiIdx);
    preferences.putInt("unitSys",  (int)activeUnit);
    preferences.end();
}

void loadSettings() {
    if (!preferences.begin("holo9_settings", true)) {
        Serial.println("BOOT: preferences open failed; keeping defaults");
        return;
    }
    activeLocationIdx = preferences.getInt("locIdx",   DEFAULT_LOCATION_IDX);
    activeModelIdx    = preferences.getInt("modelIdx", DEFAULT_MODEL_IDX);
    activeWifiIdx     = preferences.getInt("wifiIdx",  DEFAULT_WIFI_IDX);
    if (activeWifiIdx < 0 || activeWifiIdx >= NUM_WIFI) activeWifiIdx = DEFAULT_WIFI_IDX;
    activeUnit        = (UnitSystem)preferences.getInt("unitSys", (int)DEFAULT_UNIT);
    preferences.end();
}

// ============================================================
// VECTOR WEATHER ICONS (hand-drawn per WMO code)
// ============================================================
void drawWeatherIcon(int x, int y, int size, int wmoCode, bool isNight) {
    int cx = x + size / 2;
    int cy = y + size / 2;
    float scale = (float)size / 80.0;

    // Clear only the artwork envelope.  The vector geometry occupies about
    // 60/80 of the nominal coordinate cell; clearing the full `size` square
    // left a conspicuous black frame around every icon.
    int clearSize = max(1, (int)(60.0f * scale + 1.0f));
    int clearX = cx - clearSize / 2;
    int clearY = cy - clearSize / 2;
    canvas.fillRect(clearX, clearY, clearSize, clearSize, MY_BLACK);

    // Clear sky / mainly clear
    if (wmoCode == 0 || wmoCode == 1) {
        if (isNight) {
            int r = 15 * scale;
            canvas.fillSmoothCircle(cx, cy, r, MY_BLUE04);
            canvas.fillSmoothCircle(cx + (10 * scale), cy - (4 * scale), r, MY_BLACK);  // crescent cut
        } else {
            int r = 14 * scale;
            canvas.fillSmoothCircle(cx, cy, r, MY_YELLOW);
            int rayLen = 6 * scale, gap = 4 * scale;
            for (int i = 0; i < 8; i++) {   // 8 sun rays
                float a = i * (PI / 4.0);
                canvas.drawWideLine(
                    cx + cos(a) * (r + gap),       cy + sin(a) * (r + gap),
                    cx + cos(a) * (r + gap + rayLen), cy + sin(a) * (r + gap + rayLen),
                    max(2, (int)(3 * scale)), MY_YELLOW, MY_BLACK);
            }
        }
    }
    // Partly cloudy (sun/moon + small cloud)
    else if (wmoCode == 2) {
        int sunX = cx - 12 * scale, sunY = cy - 10 * scale;
        if (isNight) {
            int r = 14 * scale;
            canvas.fillSmoothCircle(sunX, sunY, r, MY_BLUE04);
            canvas.fillSmoothCircle(sunX + (10 * scale), sunY - (4 * scale), r, MY_BLACK);
        } else {
            canvas.fillSmoothCircle(sunX, sunY, 14 * scale, MY_YELLOW);
        }
        int cloudX = cx + 4 * scale, cloudY = cy + 8 * scale;
        canvas.fillSmoothCircle(cloudX - 10 * scale, cloudY, 10 * scale, MY_LIGHTGREY);
        canvas.fillSmoothCircle(cloudX, cloudY - 6 * scale, 12 * scale, MY_WHITE);
        canvas.fillSmoothCircle(cloudX + 12 * scale, cloudY, 8 * scale, MY_LIGHTGREY);
        canvas.fillSmoothRoundRect(cloudX - 18 * scale, cloudY, 36 * scale, 10 * scale, 4 * scale, MY_WHITE);
    }
    // Overcast / fog
    else if (wmoCode == 3 || wmoCode == 45 || wmoCode == 48) {
        canvas.fillSmoothCircle(cx - 12 * scale, cy, 12 * scale, MY_GREY03);
        canvas.fillSmoothCircle(cx, cy - 10 * scale, 16 * scale, MY_LIGHTGREY);
        canvas.fillSmoothCircle(cx + 14 * scale, cy, 11 * scale, MY_GREY03);
        canvas.fillSmoothRoundRect(cx - 22 * scale, cy, 44 * scale, 12 * scale, 4 * scale, MY_LIGHTGREY);
        if (wmoCode == 45 || wmoCode == 48) {   // fog lines
            canvas.drawFastHLine(cx - 20 * scale, cy + 18 * scale, 40 * scale, MY_BLUE04);
            canvas.drawFastHLine(cx - 15 * scale, cy + 22 * scale, 30 * scale, MY_BLUE04);
        }
    }
    // Rain / drizzle / showers
    else if ((wmoCode >= 51 && wmoCode <= 67) || (wmoCode >= 80 && wmoCode <= 82)) {
        canvas.fillSmoothCircle(cx - 10 * scale, cy - 6 * scale, 11 * scale, MY_GREY03);
        canvas.fillSmoothCircle(cx + 2 * scale, cy - 12 * scale, 14 * scale, MY_LIGHTGREY);
        canvas.fillSmoothCircle(cx + 14 * scale, cy - 4 * scale, 10 * scale, MY_GREY03);
        canvas.fillSmoothRoundRect(cx - 18 * scale, cy - 4 * scale, 38 * scale, 10 * scale, 4 * scale, MY_LIGHTGREY);
        int dropY = cy + 12 * scale;
        for (int i = -12; i <= 12; i += 8)   // raindrops
            canvas.drawWideLine(cx + (i * scale), dropY, cx + ((i - 4) * scale), dropY + (8 * scale),
                                max(1, (int)(2 * scale)), MY_BLUE05, MY_BLACK);
    }
    // Snow
    else if ((wmoCode >= 71 && wmoCode <= 77) || (wmoCode >= 85 && wmoCode <= 86)) {
        canvas.fillSmoothCircle(cx - 10 * scale, cy - 6 * scale, 11 * scale, MY_GREY03);
        canvas.fillSmoothCircle(cx + 2 * scale, cy - 12 * scale, 14 * scale, MY_LIGHTGREY);
        canvas.fillSmoothCircle(cx + 14 * scale, cy - 4 * scale, 10 * scale, MY_GREY03);
        canvas.fillSmoothRoundRect(cx - 18 * scale, cy - 4 * scale, 38 * scale, 10 * scale, 4 * scale, MY_LIGHTGREY);
        int snowY = cy + 14 * scale;
        canvas.fillSmoothCircle(cx - 10 * scale, snowY, 2 * scale, MY_WHITE);
        canvas.fillSmoothCircle(cx, snowY + 4 * scale, 2 * scale, MY_WHITE);
        canvas.fillSmoothCircle(cx + 10 * scale, snowY, 2 * scale, MY_WHITE);
    }
    // Thunderstorm
    else if (wmoCode >= 95 && wmoCode <= 99) {
        canvas.fillSmoothCircle(cx - 10 * scale, cy - 8 * scale, 11 * scale, MY_GREY02);
        canvas.fillSmoothCircle(cx + 2 * scale, cy - 14 * scale, 14 * scale, MY_GREY03);
        canvas.fillSmoothCircle(cx + 14 * scale, cy - 6 * scale, 10 * scale, MY_GREY02);
        canvas.fillSmoothRoundRect(cx - 18 * scale, cy - 6 * scale, 38 * scale, 10 * scale, 4 * scale, MY_GREY03);
        int lx = cx, ly = cy + 4 * scale;    // lightning bolt (two triangles)
        canvas.fillTriangle(lx + 2 * scale, ly, lx - 6 * scale, ly + 12 * scale, lx, ly + 12 * scale, MY_YELLOW);
        canvas.fillTriangle(lx, ly + 10 * scale, lx + 6 * scale, ly + 10 * scale, lx - 2 * scale, ly + 22 * scale, MY_YELLOW);
    }
    // Fallback: plain sun
    else {
        canvas.fillSmoothCircle(cx, cy, 14 * scale, MY_YELLOW);
    }
}

// Draw sunrise or sunset marker with time label
void drawSunEventIcon(int x, int y, bool isSunrise, const String& timeStr) {
    canvas.drawFastHLine(x, y + 11, 15, MY_GREY03);   // horizon line
    if (isSunrise) {
        canvas.fillSmoothCircle(x + 7, y + 11, 14, MY_YELLOW);
        canvas.fillRect(x - 7, y + 14, 30, 15, MY_BLACK);   // mask below horizon
    } else {
        canvas.fillSmoothCircle(x + 7, y + 8, 13, MY_BLUE04);
        canvas.fillSmoothCircle(x + 15, y + 6, 12, MY_BLACK);   // crescent
    }
    drawOfrString(timeStr, x + 26, y - 3, 22, MY_WHITE);
}

// ============================================================
// SPLINE GRAPH ENGINE
// Catmull-Rom spline with optional fill down to baselineY
// ============================================================
void drawSplineGraphFilled(int xPts[], int yPts[], int count, uint16_t color,
                           uint16_t fillColor, int baselineY) {
    for (int i = 0; i < count - 1; i++) {
        // Four control points for the current segment
        int p0 = (i == 0) ? 0 : i - 1;
        int p1 = i;
        int p2 = i + 1;
        int p3 = (i + 2 >= count) ? count - 1 : i + 2;

        float x0 = xPts[p0], y0 = yPts[p0];
        float x1 = xPts[p1], y1 = yPts[p1];
        float x2 = xPts[p2], y2 = yPts[p2];
        float x3 = xPts[p3], y3 = yPts[p3];

        float prevX = x1, prevY = y1;
        for (int step = 1; step <= 8; step++) {   // 8 sub-steps per segment
            float t = step / 8.0f, t2 = t * t, t3 = t2 * t;

            float qX = 0.5f * ((2.0f*x1) + (-x0 + x2)*t + (2.0f*x0 - 5.0f*x1 + 4.0f*x2 - x3)*t2 + (-x0 + 3.0f*x1 - 3.0f*x2 + x3)*t3);
            float qY = 0.5f * ((2.0f*y1) + (-y0 + y2)*t + (2.0f*y0 - 5.0f*y1 + 4.0f*y2 - y3)*t2 + (-y0 + 3.0f*y1 - 3.0f*y2 + y3)*t3);

            // Optional fill underneath the curve
            if (fillColor != MY_BLACK) {
                int startX = (int)min(prevX, qX);
                int endX   = (int)max(prevX, qX);
                for (int fillX = startX; fillX <= endX; fillX++) {
                    int fillY = (int)max(prevY, qY);
                    int fillH = baselineY - fillY;
                    if (fillH > 0) canvas.drawFastVLine(fillX, fillY, fillH, fillColor);
                }
            }
            // Draw the curve segment
            canvas.drawWideLine((int)prevX, (int)prevY, (int)qX, (int)qY, 2, color, color);
            prevX = qX; prevY = qY;
        }
    }
}

// ============================================================
// PRECIPITATION BAR HELPER
// ============================================================
void drawPrecipBar(int xCenter, float value, float maxValue, int baseY, int maxH,
                   bool imperial, int barWidth = 20, int labelSize = 15) {
    float threshold = imperial ? 0.002f : 0.05f;   // ignore trivial amounts
    if (value <= threshold) return;

    int barH = constrain((int)((value / maxValue) * maxH), 2, maxH);
    canvas.fillRect(xCenter - barWidth / 2, baseY - barH, barWidth, barH, MY_BLUE04);

    String pStr = imperial ? String(value, 2) : String(value, 1);
    drawOfrCenterString(pStr, xCenter, baseY - barH - 20, labelSize, MY_WHITE, MY_GREY02);
}

// ============================================================
// WEATHER DATA FETCH
// ============================================================
bool fetchWeatherData() {
    if (WiFi.status() != WL_CONNECTED) return false;

    updateSystemTimezone();
    configTzTime(LOCATIONS[activeLocationIdx].posixTz, ntpServer);

    HTTPClient http;
    String unitParams = (activeUnit == UNIT_IMPERIAL)
        ? "&temperature_unit=fahrenheit&precipitation_unit=inch"
        : "";

    // Build Open-Meteo request URL
    String url = "http://api.open-meteo.com/v1/forecast?"
                 "latitude="  + String(LOCATIONS[activeLocationIdx].lat, 4) +
                 "&longitude=" + String(LOCATIONS[activeLocationIdx].lon, 4) +
                 "&models="   + String(WEATHER_MODELS[activeModelIdx]) +
                 "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m,pressure_msl"
                 "&hourly=temperature_2m,precipitation,weather_code,wind_speed_10m"
                 "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,weather_code,sunrise,sunset"
                 "&forecast_days=10&timezone=" + String(LOCATIONS[activeLocationIdx].tz) +
                 "&wind_speed_unit=kmh" + unitParams;

    http.begin(url);
    http.setTimeout(10000);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("HTTP GET Failed: %d\n", httpCode);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.print("JSON Parse Failed: ");
        Serial.println(error.c_str());
        return false;
    }

    // --- Current conditions ---
    currentWeather.temp          = doc["current"]["temperature_2m"];
    currentWeather.humidity      = doc["current"]["relative_humidity_2m"];
    currentWeather.windSpeed     = doc["current"]["wind_speed_10m"];
    if (activeUnit == UNIT_IMPERIAL) currentWeather.windSpeed *= 0.621371f;
    currentWeather.windDirection = doc["current"]["wind_direction_10m"];
    currentWeather.weatherCode   = doc["current"]["weather_code"];
    currentWeather.pressure      = doc["current"]["pressure_msl"];

    // --- Sun times (extract "HH:MM" from ISO string) ---
    const char* srStr = doc["daily"]["sunrise"][0];
    currentWeather.sunrise = (srStr && strlen(srStr) >= 16) ? String(srStr).substring(11, 16) : "--:--";
    const char* ssStr = doc["daily"]["sunset"][0];
    currentWeather.sunset  = (ssStr && strlen(ssStr) >= 16) ? String(ssStr).substring(11, 16) : "--:--";

    // --- Hourly data (bounded by all four array sizes) ---
    JsonArray hTemp   = doc["hourly"]["temperature_2m"];
    JsonArray hPrecip = doc["hourly"]["precipitation"];
    JsonArray hCode   = doc["hourly"]["weather_code"];
    JsonArray hWind   = doc["hourly"]["wind_speed_10m"];

    hourlyCount = min({(int)hTemp.size(), (int)hPrecip.size(), (int)hCode.size(),
                       (int)hWind.size(), 120});
    for (int i = 0; i < hourlyCount; i++) {
        hourlyForecast[i].temp      = hTemp[i];
        hourlyForecast[i].precip    = hPrecip[i];
        hourlyForecast[i].windSpeed = hWind[i];
        hourlyForecast[i].code      = hCode[i];
    }

    // --- Daily data ---
    JsonArray dMax    = doc["daily"]["temperature_2m_max"];
    JsonArray dMin    = doc["daily"]["temperature_2m_min"];
    JsonArray dPrecip = doc["daily"]["precipitation_sum"];
    JsonArray dCode   = doc["daily"]["weather_code"];

    dailyCount = min({(int)dMax.size(), (int)dMin.size(), (int)dPrecip.size(), (int)dCode.size(), 10});
    for (int i = 0; i < dailyCount; i++) {
        dailyForecast[i].tempMax   = dMax[i];
        dailyForecast[i].tempMin   = dMin[i];
        dailyForecast[i].precipSum = dPrecip[i];
        dailyForecast[i].code      = dCode[i];

        // Prefer the midday hourly code as a more representative icon
        int middayIndex = (i * 24) + 14;
        if (middayIndex < hCode.size()) dailyForecast[i].code = hCode[middayIndex];
    }

    Serial.println("Holo9 Weather Data Refreshed!");
    return true;
}

// ============================================================
// UNIFIED SCREEN HEADER
// ============================================================
void drawScreenHeader(const char* title, uint32_t titleColor = MY_BLUE04) {
    drawOfrString(title, 12, 6, 22, titleColor);
    drawOfrCenterString(getLocationName(), 240, 6, 22, MY_WHITE);

        struct tm timeinfo;
    char timeStr[6] = "--:--";
    if (getLocalTime(&timeinfo))
        sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    if (currentScreen != SCREEN_SENSE_CLOCK)
        drawOfrRightString(timeStr, 468, 6, 22, MY_WHITE);

    canvas.fillRect(2, 32, 476, 1, MY_BLUE02);
}

// Hamburger settings button in the bottom-right corner
void drawSettingsButton() {
    int startX = 430, startY = 278;
    int lineWidth = 38, lineHeight = 4, gap = 10;
    // canvas.fillRoundRect(startX - 8, startY - 8, lineWidth + 18, 50, 5, MY_GREY);
    canvas.fillRoundRect(startX - 6, startY - 6, lineWidth + 12, 45, 4, MY_GREY);
    for (int i = 0; i < 3; i++)
        canvas.fillRoundRect(startX, startY + (i * (lineHeight + gap)),
                             lineWidth, lineHeight, 1, MY_LIGHTGREY);
}

// ============================================================
// SCREEN 1: CURRENT WEATHER
// ============================================================
void drawCurrentScreen() {
    canvas.fillSprite(MY_BLACK);
    drawScreenHeader("CURRENT");

    struct tm timeinfo = getNow();
    int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    int srMinutes = getMinutesFromTimeStr(currentWeather.sunrise);
    int ssMinutes = getMinutesFromTimeStr(currentWeather.sunset);
    if (srMinutes < 0) srMinutes = 6 * 60;
    if (ssMinutes < 0) ssMinutes = 20 * 60;

    bool mainIsNight = isNightAt(currentMinutes, srMinutes, ssMinutes);

    // Big icon, sun events, main temperature
    drawWeatherIcon(320, 23, 152, currentWeather.weatherCode, mainIsNight);
    drawSunEventIcon(350, 157, true,  "  " + currentWeather.sunrise);
    drawSunEventIcon(350, 185, false, "  " + currentWeather.sunset);

    String tempNum = String(currentWeather.temp, 1) + " °"; // + getTempSymbol();
    drawOfrString(tempNum, 18, 28, 80, TFT_ORANGE);

    // Detail lines
    drawOfrString("Humidity:  " + String(currentWeather.humidity) + " %%", 20, 114, 28, MY_BLUE04);
    drawOfrString("Wind:  " + String(currentWeather.windSpeed, 0) + " " + getSpeedSymbol() +
                  "  " + getWindDirStr(currentWeather.windDirection), 20, 144, 22, MY_WHITE);
    drawOfrString("Precipitation:  " + String(dailyForecast[0].precipSum, 2) + " " + getLengthSymbol(), 20, 167, 22, MY_BLUE02);
    drawOfrString("Pressure:  " + String((int)currentWeather.pressure) + " hPa", 20, 190, 22, MY_WHITE);

    canvas.fillRect(10, 220, 460, 2, MY_BLUE);

    // 7-cell forecast strip (every 4 hours)
    int currentHour = currentMinutes / 60 + 1; // start at current hr + 1
    const int stepHours = 4, cellWidth = 58, startX = 12, startY = 222;

    for (int i = 0; i < 7; i++) {
        int forecastIdx = min(currentHour + i * stepHours, hourlyCount - 1);
        if (forecastIdx < 0) forecastIdx = 0;

        int xPos = startX + i * cellWidth;
        int slotHour = (currentHour + i * stepHours) % 24;
        bool slotIsNight = isNightAt(slotHour * 60, srMinutes, ssMinutes);

        String tLabel = String(hourlyForecast[forecastIdx].temp, 0) + "°";  //use for Symbol c/F:        String(hourlyForecast[forecastIdx].temp, 0) + "°" + getTempSymbol();
        drawOfrCenterString(tLabel, xPos + 35, startY + 2, 22, MY_WHITE);

        drawWeatherIcon(xPos, startY + 23, 63, hourlyForecast[forecastIdx].code, slotIsNight);

        char timeBuf[6];
        sprintf(timeBuf, ("%02d"), slotHour);
        drawOfrCenterString(timeBuf + String(""), xPos + 30, startY + 74, 22, MY_WHITE);
        drawOfrCenterString(String("h"), xPos + 50, startY + 72, 14, MY_WHITE);
    }

    drawSettingsButton();
    pushCanvas();
}

// ============================================================
// SCREEN 2: SENSE CLOCK / 3-DAY SUMMARY
// ============================================================

// Draw Date Helper
void drawSenseClockDate(const struct tm& timeinfo) {
    canvas.fillRect(40, 5, 400, 25, MY_BLACK);
    char dateStr[20];
    sprintf(dateStr, "%s %d %s %d", dayNamesFull[timeinfo.tm_wday], timeinfo.tm_mday,
            monthNames[timeinfo.tm_mon], timeinfo.tm_year + 1900);
    drawOfrCenterString(dateStr, 240, 0, 25, MY_BLUE04);
}
// Draw Large Clock Helper
void drawSenseClockTime(const struct tm& timeinfo) {
    canvas.fillRect(40, 10, 400, 105, MY_BLACK);
    char timeStr[6];
    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    drawOfrCenterString(timeStr, 240, -15, 113, MY_WHITE);
}

void drawSenseClockScreen() {
    canvas.fillSprite(MY_BLACK);

    struct tm timeinfo = getNow();
    drawSenseClockTime(timeinfo);

    int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int srMinutes = getMinutesFromTimeStr(currentWeather.sunrise);
    int ssMinutes = getMinutesFromTimeStr(currentWeather.sunset);
    if (srMinutes < 0) srMinutes = 6 * 60;
    if (ssMinutes < 0) ssMinutes = 20 * 60;
    bool mainIsNight = isNightAt(currentMinutes, srMinutes, ssMinutes);

    // Today's conditions: icon, location, temperature, and humidity.
    drawWeatherIcon(50, 100, 180, currentWeather.weatherCode, mainIsNight);

    drawOfrString(String(currentWeather.temp, 1) + " °" + getTempSymbol(), 240, 111, 42, MY_YELLOW);
    drawOfrString("Humidity  " + String(currentWeather.humidity) + " %%", 240, 156, 23, MY_WHITE);
    drawOfrString("Precip  " + String(dailyForecast[0].precipSum, 2) + " " + getLengthSymbol(), 240, 181, 23, MY_BLUE02);
    drawOfrString(getLocationName(), 240, 206, 20, MY_BLUE04);
    // drawSenseClockDate(timeinfo); // Draw Date at the Top of the Screen

    // Next three daily forecast cells (dailyForecast[0] is today).
    int forecastWday = (timeinfo.tm_wday + 1) % 7;
    const int cellX[] = {30, 168, 306};
    for (int i = 0; i < 3; i++) {
        int x = cellX[i];
        int dayIdx = i + 0; // + 0 for starting today, +1 for tomorrow
        
        drawOfrCenterString(dayNames[(forecastWday + i + 6) % 7], x + 42, 297, 20, MY_BLUE04); // Re forecastWday+6 will shift dayNames to the left by one, so +0 for next day, +6 for today as first day
        if (dayIdx < dailyCount) {
            drawWeatherIcon(x + 36, 225, 90, dailyForecast[dayIdx].code, false);
            String temps = String(dailyForecast[dayIdx].tempMin, 0) + "° / " +
                           String(dailyForecast[dayIdx].tempMax, 0) + "°";
            drawOfrCenterString(temps, x + 105, 297, 20, MY_WHITE);
        } else {
            drawOfrCenterString("-- / --", x + 105, 297, 20, MY_GREY03);
        }
    }

    // drawSettingsButton();
    pushCanvas();
}

// ============================================================
// SCREEN 3: 72-HOUR HOURLY PREVIEW
// ============================================================
void draw3DayScreen() {
    canvas.fillSprite(MY_BLACK);
    drawScreenHeader("72 HRS");

    struct tm timeinfo = getNow();
    int wday = timeinfo.tm_wday;
    int currentHour = timeinfo.tm_hour;
    int startHour = (currentHour / 6) * 6;   // snap to a 6-hour boundary

    int srMinutes = getMinutesFromTimeStr(currentWeather.sunrise);
    int ssMinutes = getMinutesFromTimeStr(currentWeather.sunset);
    if (srMinutes < 0) srMinutes = 6 * 60;
    if (ssMinutes < 0) ssMinutes = 20 * 60;

    // X-coordinates for the 72 hourly slots
    int xPos[72], yPos[72], windYPos[72];
    for (int i = 0; i < 72; i++)
        xPos[i] = 12 + (int)(i * (456.0 / 71.0));

    // Day separators + day labels (every time we cross midnight)
    int currentDayOffset = 0;
    int prevMidnightX = 12;
    for (int i = 0; i < 72; i++) {
        int slotHour = (startHour + i) % 24;
        if (slotHour == 0 && i > 0) {
            canvas.drawFastVLine(xPos[i], 38, 270, MY_GREY02);
            int headerX = (prevMidnightX + xPos[i]) / 2;
            String dName = dayNames[(wday + currentDayOffset) % 7];
            drawOfrCenterString(dName, headerX, 35, 24, MY_BLUE04);
            prevMidnightX = xPos[i];
            currentDayOffset++;
        }
    }
    // Label for the trailing partial day
    {
        int headerX = (prevMidnightX + 468) / 2;
        String dName = (currentDayOffset == 0) ? " " : dayNames[(wday + currentDayOffset) % 7];
        drawOfrCenterString(dName, headerX, 35, 24, MY_BLUE04);
    }

    // Weather icons + hour labels every 6 hours
    for (int i = 0; i < 72; i += 6) {
        int forecastIdx = min(startHour + i, hourlyCount - 1);
        if (forecastIdx < 0) forecastIdx = 0;
        int slotHour = (startHour + i) % 24;
        bool slotIsNight = isNightAt(slotHour * 60, srMinutes, ssMinutes);

        drawWeatherIcon(xPos[i] - 23, 60, 50, hourlyForecast[forecastIdx].code, slotIsNight);

        // char hBuf[3];   // 6-hourly labels
        // sprintf(hBuf, "%02d", slotHour);
        // drawOfrCenterString(hBuf, xPos[i], 100, 20, MY_LIGHTGREY);
    }

    // Temperature graph (fixed absolute scale)
    float minT = (activeUnit == UNIT_METRIC) ? GRAPH_TEMP_MIN_C : GRAPH_TEMP_MIN_F;
    float maxT = (activeUnit == UNIT_METRIC) ? GRAPH_TEMP_MAX_C : GRAPH_TEMP_MAX_F;

    const int graphTop = 105, graphBottom = 320;
    const int graphHeight = graphBottom - graphTop;
    const int precipBaseY = 320, precipMaxH = 180;
    float maxExpectedPrecip = (activeUnit == UNIT_IMPERIAL) ? 1.9685f : 50.0f;

    //canvas.drawFastHLine(12, graphTop, 456, MY_GREY02);

    for (int i = 0; i < 72; i++) {
        int forecastIdx = min(startHour + i, hourlyCount - 1);
        if (forecastIdx < 0) forecastIdx = 0;
        float clamped = constrain(hourlyForecast[forecastIdx].temp, minT, maxT);
        yPos[i] = graphBottom - (int)(((clamped - minT) / (maxT - minT)) * graphHeight);

        float windClamped = constrain(hourlyForecast[forecastIdx].windSpeed, 0.0f, GRAPH_WIND_MAX_KM);
        windYPos[i] = graphBottom - (int)((windClamped / GRAPH_WIND_MAX_KM) * graphHeight);
    }

    // Wind curve on the same graph area; fixed 0-100 km/h scale.
    drawSplineGraphFilled(xPos, windYPos, 72, MY_GREY03, MY_BLACK, precipBaseY);
    drawOfrString("Temp (°C/F)", 9, 105, 14, MY_YELLOW);
    drawOfrString("Wind (0-" + String(GRAPH_WIND_MAX_KM, 0) + " km/h)", 9, 122, 14, MY_LIGHTGREY);

    // Yellow filled temperature curve
    drawSplineGraphFilled(xPos, yPos, 72, MY_YELLOW, MY_BLACK, precipBaseY);
 

    // 6-hour precipitation bars
    for (int i = 0; i < 72; i += 6) {
        float pSum = 0;
        for (int j = 0; j < 6 && (startHour + i + j) < hourlyCount; j++)
            pSum += hourlyForecast[startHour + i + j].precip;

        drawPrecipBar(xPos[i], pSum, maxExpectedPrecip, precipBaseY, precipMaxH,
                      activeUnit == UNIT_IMPERIAL, 20, 15);
    }

    // ---- High temperature labels (midnight-to-midnight blocks) ----
    int dayStartIdx = 0;
    for (int i = 0; i <= 72; i++) {
        int slotHour = (startHour + i) % 24;
        if ((slotHour == 0 && i > 0) || i == 72) {
            int dayEndIdx = i;
            if (dayEndIdx > dayStartIdx) {
                int highIdx = dayStartIdx;
                for (int j = dayStartIdx + 1; j < dayEndIdx; j++) {
                    if (hourlyForecast[startHour + j].temp > hourlyForecast[startHour + highIdx].temp)
                        highIdx = j;
                }
                String highStr = String(hourlyForecast[startHour + highIdx].temp, 0);
                drawOfrCenterString(highStr + "°", xPos[highIdx], yPos[highIdx] - 28, 20,
                                    MY_YELLOW, MY_GREY02);
            }
            dayStartIdx = i;
        }
    }

    // ---- Low temperature labels (noon-to-noon overnight blocks) ----
    int nightStartIdx = -1;
    for (int i = 0; i < 24; i++) {
        if ((startHour + i) % 24 == 12) { nightStartIdx = i; break; }
    }

    // Partial early-morning low before the first noon
    if (nightStartIdx > 0) {
        int lowIdx = 0;
        for (int j = 1; j < nightStartIdx; j++) {
            if (hourlyForecast[startHour + j].temp < hourlyForecast[startHour + lowIdx].temp)
                lowIdx = j;
        }
        String lowStr = String(hourlyForecast[startHour + lowIdx].temp, 0);
        drawOfrCenterString(lowStr + "°", xPos[lowIdx] - 5, yPos[lowIdx] - 28, 20, MY_BLUE04);
    }

    // Full noon-to-noon overnight lows
    if (nightStartIdx != -1) {
        for (int i = nightStartIdx; i < 60; i += 24) {
            int windowEnd = min(i + 24, 72);
            int lowIdx = i;
            for (int j = i + 1; j < windowEnd; j++) {
                if (hourlyForecast[startHour + j].temp < hourlyForecast[startHour + lowIdx].temp)
                    lowIdx = j;
            }
            String lowStr = String(hourlyForecast[startHour + lowIdx].temp, 0);
            drawOfrCenterString(lowStr + "°", xPos[lowIdx] - 5, yPos[lowIdx] - 28, 20, MY_BLUE04);
        }
    }

    pushCanvas();
}

// ============================================================
// SCREEN 4: 10-DAY DAILY PREVIEW
// ============================================================
void draw10DayScreen() {
    canvas.fillSprite(MY_BLACK);
    drawScreenHeader("10 DAYS");

    struct tm timeinfo = getNow();
    int wday = timeinfo.tm_wday;

    // Fixed absolute vertical scale (same as 72h screen)
    float globalMin = (activeUnit == UNIT_METRIC) ? GRAPH_TEMP_MIN_C : GRAPH_TEMP_MIN_F;
    float globalMax = (activeUnit == UNIT_METRIC) ? GRAPH_TEMP_MAX_C : GRAPH_TEMP_MAX_F;

    const int graphTop = 130, graphBottom = 290;
    const int graphHeight = graphBottom - graphTop;
    const int precipBaseY = 320, precipMaxH = 180;
    float maxExpectedDailyPrecip = (activeUnit == UNIT_IMPERIAL) ? 3.937f : 100.0f;

    int maxXPts[10], maxYPts[10], minXPts[10], minYPts[10];

    for (int i = 0; i < dailyCount; i++) {
        int xCenter = 12 + (int)(i * (456.0 / 9.0));

        String dName = dayNames[(wday + i) % 7];
        drawOfrCenterString(dName, xCenter, 36, 15, MY_BLUE04);

        drawWeatherIcon(xCenter - 25, 52, 50, dailyForecast[i].code);

        float tMax = constrain(dailyForecast[i].tempMax, globalMin, globalMax);
        float tMin = constrain(dailyForecast[i].tempMin, globalMin, globalMax);
        float normMax = (tMax - globalMin) / (globalMax - globalMin);
        float normMin = (tMin - globalMin) / (globalMax - globalMin);

        maxXPts[i] = xCenter;
        maxYPts[i] = graphBottom - (int)(normMax * graphHeight);
        minXPts[i] = xCenter;
        minYPts[i] = graphBottom - (int)(normMin * graphHeight);

        // Vertical divider between days
        if (i < dailyCount - 1) {
            int nextXCenter = 12 + (int)((i + 1) * (456.0 / 9.0));
            canvas.drawFastVLine((xCenter + nextXCenter) / 2, 34, 270, MY_GREY02);
        }
    }

    // Fill only the "high" curve (yellow); the "low" curve is a thin line
    drawSplineGraphFilled(maxXPts, maxYPts, dailyCount, MY_YELLOW, MY_GREY, precipBaseY);
    drawSplineGraphFilled(minXPts, minYPts, dailyCount, MY_BLUE04, 0x0001, precipBaseY);

    drawOfrString("Temp (°C/F max)", 9, 95, 14, MY_YELLOW);
    drawOfrString("Temp (°C/F min)", 9, 113, 14, MY_BLUE04);

    // Per-day precipitation bars + min/max labels
    for (int i = 0; i < dailyCount; i++) {
        int xCenter = maxXPts[i];
        drawPrecipBar(xCenter, dailyForecast[i].precipSum, maxExpectedDailyPrecip,
                      precipBaseY, precipMaxH, activeUnit == UNIT_IMPERIAL, 16, 15);

        drawOfrCenterString(String(dailyForecast[i].tempMax, 0) + "°",
                            maxXPts[i], maxYPts[i] - 23, 18, MY_YELLOW, MY_BLACK);
        drawOfrCenterString(String(dailyForecast[i].tempMin, 0) + "°",
                            minXPts[i], minYPts[i] + 6,  18, MY_BLUE04, MY_BLACK);
    }

    pushCanvas();
}

// ============================================================
// SETTINGS SCREEN
// ============================================================
void drawSettingsScreen() {
    canvas.fillSprite(MY_GREY);
    drawOfrString("SETTINGS", 15, 10, 32, MY_BLUE04, MY_GREY);
    canvas.fillRect(15, 48, 450, 2, MY_BLUE);

    // Four tappable rows
    drawOfrString("Location:", 20, 56, 30, MY_BLUE04, MY_GREY);
    drawOfrString(LOCATIONS[activeLocationIdx].name, 160, 60, 25, MY_WHITE, MY_GREY);

    drawOfrString("Model:", 20, 100, 30, MY_BLUE04, MY_GREY);
    drawOfrString(WEATHER_MODELS[activeModelIdx], 160, 104, 25, MY_WHITE, MY_GREY);

    drawOfrString("WiFi:", 20, 144, 30, MY_BLUE04, MY_GREY);
    drawOfrString(WIFI_PRESETS[activeWifiIdx].ssid, 160, 148, 25, MY_WHITE, MY_GREY);

    drawOfrString("Units:", 20, 188, 30, MY_BLUE04, MY_GREY);
    String uStr = (activeUnit == UNIT_METRIC) ? "Metric (°C, km/h, mm)"
                                              : "Imperial (°F, mph, in)";
    drawOfrString(uStr, 160, 192, 25, MY_WHITE, MY_GREY);

    canvas.drawFastHLine(15, 230, 450, MY_GREY02);

    // Save button
    canvas.fillRoundRect(30, 244, 420, 50, 8, MY_BLUE);
    drawOfrCenterString("< SAVE & RECONNECT", 240, 254, 22, MY_WHITE, MY_BLUE);

    // ElegantOTA endpoint (same information as the CYD settings screen).
    drawOfrCenterString("Holo9 Weather © HN 2026 - IP: " + WiFi.localIP().toString(), 240, 300, 14, MY_GREY03, MY_GREY);

    pushCanvas();
}

// Start one preset. Subsequent presets are attempted by serviceWiFiConnection().
void beginWifiPresetAttempt(int presetIdx) {
    wifiAttemptIdx = presetIdx;
    wifiAttemptUsesSaved = false;
    wifiAttemptActive = true;
    lastWifiAttempt = millis();
    Serial.printf("Attempting WiFi SSID: %s (Index: %d)\n",
                  WIFI_PRESETS[wifiAttemptIdx].ssid, wifiAttemptIdx);
    WiFi.disconnect();
    WiFi.begin(WIFI_PRESETS[wifiAttemptIdx].ssid, WIFI_PRESETS[wifiAttemptIdx].password);
}

void beginSavedWifiAttempt() {
    wifiAttemptUsesSaved = true;
    wifiAttemptActive = true;
    lastWifiAttempt = millis();
    Serial.printf("Attempting previously saved WiFi SSID: %s\n", savedWifiSsid.c_str());
    WiFi.disconnect();
    WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
}

void beginWifiPresetCycle() {
    wifiCycleOffset = 0;
    savedWifiAttempted = false;
    wifiFailureTransitionPending = false;
    beginWifiPresetAttempt(activeWifiIdx);
}

void serviceWiFiConnection() {
    unsigned long now = millis();

    if (wifiFailureTransitionPending && now - wifiFailureTransitionAt >= 1800UL) {
        wifiFailureTransitionPending = false;
        currentScreen = SCREEN_SETTINGS;
        drawSettingsScreen();
    }

    if (wifiAttemptActive) {
        if (WiFi.status() == WL_CONNECTED) {
            wifiAttemptActive = false;
            if (!wifiAttemptUsesSaved && activeWifiIdx != wifiAttemptIdx) {
                activeWifiIdx = wifiAttemptIdx;
                saveSettings();
            }
            Serial.print("WiFi Connected! IP: ");
            Serial.println(WiFi.localIP());
            startOTAServer();
            configTzTime(LOCATIONS[activeLocationIdx].posixTz, ntpServer);
            weatherFetchPending = true;
            lastWifiAttempt = now;
            return;
        }

        if (now - lastWifiAttempt >= WIFI_CONNECT_TIMEOUT) {
            wifiAttemptActive = false;
            ++wifiCycleOffset;
            if (wifiCycleOffset < NUM_WIFI) {
                beginWifiPresetAttempt((activeWifiIdx + wifiCycleOffset) % NUM_WIFI);
            } else if (savedWifiAvailable && !savedWifiAttempted) {
                savedWifiAttempted = true;
                beginSavedWifiAttempt();
            } else {
                Serial.printf("All %d WiFi presets and saved credentials failed. Status Code: %d\n",
                              NUM_WIFI, WiFi.status());
                fetchFailed = true;
                wifiNextRetryAt = now + wifiRetryInterval;
                canvas.fillSprite(MY_BLACK);
                drawOfrCenterString("WiFi Connection Failed!", 240, 140, 32, MY_RED);
                pushCanvas();
                wifiFailureTransitionPending = true;
                wifiFailureTransitionAt = now;
            }
        }
    } else if (!wifiFailureTransitionPending && WiFi.status() != WL_CONNECTED &&
               (wifiNextRetryAt == 0 || (long)(now - wifiNextRetryAt) >= 0)) {
        // Retry the full preset list periodically after startup or link failure.
        wifiNextRetryAt = 0;
        beginWifiPresetCycle();
    }
}

void applyBrightness(int level) {
    int b = constrain(level, 1, 10);
    uint8_t duty = (b * b * 255) / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(LCD_BL, duty);
#else
    ledcWrite(0, duty);
#endif
}

int currentTargetBrightness(int hour) {
    if (!NIGHT_DIM_ENABLED) return SCREEN_BRIGHTNESS;
    bool night;
    if (NIGHT_START_HOUR > NIGHT_END_HOUR) {
        night = (hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR);
    } else {
        night = (hour >= NIGHT_START_HOUR && hour < NIGHT_END_HOUR);
    }
    return night ? NIGHT_BRIGHTNESS : SCREEN_BRIGHTNESS;
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);    
    loadSettings();
    updateSystemTimezone();

    // --- Display ---
    // Backlight PWM (brightness 0-10 from presets)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(LCD_BL, 5000, 8);      // pin, 5 kHz, 8-bit
#else
    ledcSetup(0, 5000, 8);            // channel 0, 5 kHz, 8-bit
    ledcAttachPin(LCD_BL, 0);
#endif
    applyBrightness(SCREEN_BRIGHTNESS);

    mylcd.Init();
    mylcd.Set_Rotation(1);

    // --- Off-screen sprite (matches display size) ---
    canvas.createSprite(mylcd.Get_Width(), mylcd.Get_Height());
    canvas.setSwapBytes(true);

    // --- Vector font engine ---
    ofr.setDrawer(canvas);

    // --- Load font ---
    ofr.loadFont(OFR_FONT_ARRAY, OFR_FONT_SIZE);

    // --- Splash screen ---
    canvas.fillSprite(MY_BLACK);
    drawOfrCenterString("H O L O 9   W E A T H E R", 240, 35, 32, MY_BLUE04);
    drawOfrCenterString("Connecting WiFi...", 240, 80, 22, MY_WHITE);

    // Concentric arcs as a decorative WiFi signal
    canvas.fillSmoothCircle(240, 220, 6, MY_WHITE);
    canvas.drawSmoothArc(240, 220, 22, 18, 140, 220, MY_BLUE05, true);
    canvas.drawSmoothArc(240, 220, 36, 32, 140, 220, MY_BLUE04, true);
    canvas.drawSmoothArc(240, 220, 50, 46, 140, 220, MY_BLUE02, true);
    canvas.drawSmoothArc(240, 220, 64, 60, 140, 220, MY_BLUE,   true);

    drawOfrCenterString("Holo9 Weather V2.23 © HN 2026", 240, 280, 18, MY_GREY03);
    pushCanvas();

    // --- Touch ---
    touch.init();
    touch.Set_Rotation(1);

    // --- WiFi ---
    WiFi.mode(WIFI_STA);
    // Preserve ESP32-saved credentials before preset attempts change station config.
    savedWifiSsid = WiFi.SSID();
    savedWifiPassword = WiFi.psk();
    savedWifiAvailable = savedWifiSsid.length() > 0;
    WiFi.disconnect();
    fetchFailed = true;
    weatherFetchPending = true;
    beginWifiPresetCycle();
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
    serviceWiFiConnection();

    // Start after a later WiFi reconnect as well as the initial connection.
    startOTAServer();

    // Keep the OTA web server responsive without blocking the weather UI.
    if (otaServerStarted) {
        server.handleClient();
        ElegantOTA.loop();
    }

    // 1) Tick the header clock once per minute
    static int lastMinute = -1;
    struct tm timeinfo;
    if (currentScreen != SCREEN_SETTINGS && getLocalTime(&timeinfo)) {
        if (timeinfo.tm_min != lastMinute) {
            lastMinute = timeinfo.tm_min;
            applyBrightness(currentTargetBrightness(timeinfo.tm_hour));

            if (currentScreen == SCREEN_SENSE_CLOCK) {
                drawSenseClockTime(timeinfo);
                // drawSenseClockDate(timeinfo);
                pushCanvas();
            } else {
                canvas.fillRect(400, 4, 70, 26, MY_BLACK);   // clear old time
                char timeStr[6];
                sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
                drawOfrRightString(timeStr, 468, 6, 22, MY_WHITE);
                pushCanvas();
            }
            bool stale = (lastSuccessTime == 0) || (millis() - lastSuccessTime > staleThreshold);
            if (stale && currentScreen != SCREEN_SENSE_CLOCK) {
                canvas.fillCircle(388, 17, 5, MY_RED);
                pushCanvas();
            }
        }
    }

    // 2) Periodic refresh (every 10 minutes), or after WiFi connects.
    unsigned long wait = fetchFailed ? retryInterval : fetchInterval;
    if (WiFi.status() == WL_CONNECTED &&
        (weatherFetchPending || millis() - lastFetchTime > wait)) {
        lastFetchTime = millis();
        if (fetchWeatherData()) {
            weatherFetchPending = false;
            fetchFailed = false;
            lastSuccessTime = millis();
            if (lastSuccessTime == 0) lastSuccessTime = 1;
            switch (currentScreen) {
                case SCREEN_CURRENT:     drawCurrentScreen();     break;
                case SCREEN_SENSE_CLOCK: drawSenseClockScreen(); break;
                case SCREEN_3DAY_HOURLY: draw3DayScreen();    break;
                case SCREEN_10DAY_DAILY: draw10DayScreen();   break;
                default: break;
            }
        } else {
            weatherFetchPending = false;
            fetchFailed = true;
            Serial.println("Fetch failed, retrying soon");
        }
    }

    // 3) Touch input (debounced 1/2 second)
    if (touch.Get_Touch() && (millis() - lastTouchTime > 500)) {
        lastTouchTime = millis();

        uint16_t pixelX = touch.touch.x[0];
        uint16_t pixelY = touch.touch.y[0];

        if (currentScreen == SCREEN_SETTINGS) {
            // Row taps cycle through the preset lists
            if      (pixelY >= 45  && pixelY < 85)  { activeLocationIdx = (activeLocationIdx + 1) % NUM_LOCATIONS; drawSettingsScreen(); }
            else if (pixelY >= 85  && pixelY < 130) { activeModelIdx    = (activeModelIdx    + 1) % NUM_MODELS;    drawSettingsScreen(); }
            else if (pixelY >= 130 && pixelY < 175) { activeWifiIdx     = (activeWifiIdx     + 1) % NUM_WIFI;      drawSettingsScreen(); }
            else if (pixelY >= 175 && pixelY < 230) { activeUnit = (activeUnit == UNIT_METRIC) ? UNIT_IMPERIAL : UNIT_METRIC; drawSettingsScreen(); }
            // Save & return home
            else if (pixelY >= 240) {
                saveSettings();  //*************   SAVING PREFERENCES   ***************
                canvas.fillSprite(MY_BLACK);
                drawOfrCenterString("Updating Settings...", 240, 140, 32, MY_WHITE);
                pushCanvas();

                // Return to the weather view immediately; reconnect in loop().
                bool wifiPresetChanged = WiFi.status() != WL_CONNECTED ||
                                         WiFi.SSID() != WIFI_PRESETS[activeWifiIdx].ssid;
                currentScreen = SCREEN_CURRENT;
                if (wifiPresetChanged) {
                    weatherFetchPending = true;
                    wifiNextRetryAt = 0;
                    beginWifiPresetCycle();
                    canvas.fillSprite(MY_BLACK);
                    drawOfrCenterString("Connecting WiFi...", 240, 140, 32, MY_WHITE);
                    pushCanvas();
                } else {
                    weatherFetchPending = true;
                    lastFetchTime = 0;
                    drawCurrentScreen();
                }

                // Consume the save-button touch: wait for release, then restart the debounce
                while (touch.Get_Touch()) {
                    delay(10);
                }
                lastTouchTime = millis();
            }
        }
        else {
            // Bottom-right corner -> settings
            if (pixelX >= 410 && pixelY >= 250) {
                currentScreen = SCREEN_SETTINGS;
                drawSettingsScreen();
            }
            // Anywhere else -> cycle screens
            else {
                if      (currentScreen == SCREEN_CURRENT)       currentScreen = SCREEN_SENSE_CLOCK;
                else if (currentScreen == SCREEN_SENSE_CLOCK)   currentScreen = SCREEN_3DAY_HOURLY;
                else if (currentScreen == SCREEN_3DAY_HOURLY)   currentScreen = SCREEN_10DAY_DAILY;
                else if (currentScreen == SCREEN_10DAY_DAILY)   currentScreen = SCREEN_CURRENT;

                switch (currentScreen) {
                    case SCREEN_CURRENT:     drawCurrentScreen();     break;
                    case SCREEN_SENSE_CLOCK: drawSenseClockScreen(); break;
                    case SCREEN_3DAY_HOURLY: draw3DayScreen();    break;
                    case SCREEN_10DAY_DAILY: draw10DayScreen();   break;
                    default: break;
                }
            }
        }
    }
}