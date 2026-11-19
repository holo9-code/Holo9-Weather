#pragma once

// ============================================================
// HOLO9 WEATHER USER CONFIGURATION
// Edit the values in this file to customize the device.
// ============================================================

// Screen brightness: 1 (min) to 10 (full)
static const int SCREEN_BRIGHTNESS = 10;
static const bool NIGHT_DIM_ENABLED = true; // set true or false to enable/disable nighttime dimming 
static const int  NIGHT_START_HOUR  = 21;
static const int  NIGHT_END_HOUR    = 7;
static const int  NIGHT_BRIGHTNESS  = 4;

// ============================================================
// OTA WEB UPDATE
// Leave both values empty to disable authentication.
// ============================================================
static const char* const OTA_USERNAME = "holo9";
static const char* const OTA_PASSWORD = "1234";

// ============================================================
// GRAPH TEMPERATURE/WIND SCALE
// Fixed absolute scale used by the 72-hour and 10-day graphs.
// ============================================================
const float GRAPH_TEMP_MIN_C = -7.0f;
const float GRAPH_TEMP_MAX_C = 38.0f;
const float GRAPH_TEMP_MIN_F = 20.0f;
const float GRAPH_TEMP_MAX_F = 100.0f;

const float GRAPH_WIND_MAX_KM = 60.0f;

// ============================================================
// LOCATION, WEATHER MODEL, AND WI-FI PRESETS
// Add/edit entries here; the sketch derives each preset count.
// ============================================================
struct LocationPreset {
    const char* name;
    float lat;
    float lon;
    const char* tz;         // URL-encoded timezone for API
    const char* posixTz;    // POSIX TZ string for local clock
};

const LocationPreset LOCATIONS[] = {
    {"Mariabrunn, Vienna", 48.20695487974676, 16.22939762373813, "Europe%2FVienna",   "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Graz, Austria",      47.077552840183884, 15.44928562714861, "Europe%2FVienna",   "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Paris, France",      48.85661,          2.35222,            "Europe%2FParis" ,   "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Berlin, Germany",    52.52000,          13.40500,           "Europe%2FBerlin",   "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Nuuk, Greenland",    64.17451,          -51.73844,          "America%2FNuuk",    "WGT2WGST,M3.5.0,M10.5.0/3"},
    {"Bangkok, Thailand",  13.75633,          100.50177,          "Asia%2FBangkok",    "ICT-7"},
    {"New York, USA",      40.71280,          -74.0060,           "America%2FNew_York","EST5EDT,M3.2.0,M11.1.0"}
};
const int NUM_LOCATIONS = sizeof(LOCATIONS) / sizeof(LOCATIONS[0]);

const char* WEATHER_MODELS[] = {
    "best_match", "icon_seamless", "geosphere_seamless", "ecmwf_ifs04", "gfs_seamless"
};
const int NUM_MODELS = sizeof(WEATHER_MODELS) / sizeof(WEATHER_MODELS[0]);

struct WifiCredential { const char* ssid; const char* password; };

const WifiCredential WIFI_PRESETS[] = {  // ****** Enter SSID and Password of up to 3 WIFI Networks ******
  
    //{"YOUR_SSID_1", "YOUR_PASSWORD_1"}
    //{"YOUR_SSID_2", "YOUR_PASSWORD_2"}
    //{"YOUR_SSID_3", "YOUR_PASSWORD_3"}
};
const int NUM_WIFI = sizeof(WIFI_PRESETS) / sizeof(WIFI_PRESETS[0]);

enum UnitSystem { UNIT_METRIC, UNIT_IMPERIAL };

// ============================================================
// ACTIVE SETTINGS / STARTUP DEFAULTS
// Change these defaults when no saved Preferences exist.
// ============================================================
static const int DEFAULT_LOCATION_IDX = 0;
static const int DEFAULT_MODEL_IDX    = 0;
static const int DEFAULT_WIFI_IDX     = 0;
static const UnitSystem DEFAULT_UNIT  = UNIT_METRIC;

static int activeLocationIdx = DEFAULT_LOCATION_IDX;
static int activeModelIdx    = DEFAULT_MODEL_IDX;
static int activeWifiIdx     = DEFAULT_WIFI_IDX;
static UnitSystem activeUnit = DEFAULT_UNIT;

static const char* const ntpServer = "pool.ntp.org";

// ============================================================
// REFRESH, SCREEN, TIME, AND DEFAULT SELECTION SETTINGS
// These are the startup defaults; touch-screen changes are saved by the sketch.
// ============================================================
static const int ROTATING_SCREEN_COUNT = 4;
static const unsigned long fetchInterval = 10 * 60 * 1000;   // 10 minutes
static const unsigned long retryInterval     = 90UL * 1000;
static const unsigned long wifiRetryInterval = 30UL * 1000;
static const unsigned long staleThreshold    = 2UL * 60 * 60 * 1000;


