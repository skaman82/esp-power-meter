/** RTC-VERSION
 * ESP32 Power Meter - Battery Monitor System. RTC Version 
 * Hardware: DFRobot Beetle ESP32-C3 @ 80MHz with INA226 current sensor
 * 
 * Features:
 * - Battery monitoring with INA226 sensor
 * - OLED display with multiple screens
 * - Web interface with real-time data
 * - MQTT integration for Home Assistant
 * - RTC time tracking
 * - Energy consumption logging
 * - Multiple battery chemistry support (Li-ion, LiPo, LiFePO4)
 */

// ===== INCLUDES =====
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <LittleFS.h>
#include <time.h>
#include <Preferences.h> 
#include <INA226_WE.h>
#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <esp_system.h>
#include "RTClib.h"

// ===== FEATURE TOGGLES =====
#define OLED                  // Enable OLED Display 
#define MQTT                  // Enable MQTT for Home Assistant
#define WEBSERVER             // Enable Web UI

// ===== DEVICE CONFIGURATION =====
struct DeviceConfig {
    float deviceCurrent = 0.013f;        // 13 mA in normal mode
    float deviceCurrentWifi = 0.036f;    // 36 mA in WiFi Mode
    byte batteryType = 2;                // 0:LiIon; 1:LiPo; 2:LiFePO4
    int cellcount = 4;                   // Battery cell count
    float batteryCapacityAh = 50.00f;    // Full battery capacity in Ah
    byte orientation = 1;                // Screen orientation
    byte screen = 0;                     // Default start screen (0-2)
    byte wifiEnabled = 1;                // WiFi active or not
    float pricePerKWh = 0.327f;          // Price per kWh
};

// ===== NETWORK CONFIGURATION =====
const char* WIFI_SSID = "airport";
const char* WIFI_PASSWORD = "xxx";
const char* AP_PASSWORD = "12345678";

// ===== GLOBAL VARIABLES =====
DeviceConfig config;
Preferences preferences;
char AP_SSID[32];
AsyncWebServer server(80);

// ===== PIN DEFINITIONS =====
#define BUTTON1_PIN 1
#define BUTTON2_PIN 2
#define I2C_ADDRESS 0x40

// ===== HARDWARE OBJECTS =====
#ifdef OLED
#include <U8g2lib.h>
U8G2_SH1107_SEEED_128X128_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
#endif

#ifdef MQTT
#include <PubSubClient.h>
const char* MQTT_SERVER = "homeassistant.local";
const int MQTT_PORT = 1883;
const char* MQTT_USER = "mqtt_user";
const char* MQTT_PASS = "mqtt_password";
WiFiClient espClient;
PubSubClient mqttClient(espClient);
#endif

RTC_DS3231 rtc;
INA226_WE ina226 = INA226_WE(I2C_ADDRESS);

// ===== TIMING VARIABLES =====
struct TimingControl {
    unsigned long previousMillis = 0;
    unsigned long lastSensorMillis = 0;
    unsigned long lastUpdateMicros = 0;
    unsigned long previousMicros = 0;
    unsigned long lastEnergySecond = 0;
    unsigned long lastMqttAttempt = 0;
    
    const unsigned long SENSOR_INTERVAL = 1000;        // 1 second
    const unsigned long INTERVAL_MICROS = 1000000;     // 1 second in microseconds
    const unsigned long MQTT_RETRY_INTERVAL = 5000;    // 5 seconds
};
TimingControl timing;

// ===== SENSOR DATA STRUCTURE =====
struct SensorData {
    float voltage = 0.0f;
    float current = 0.0f;
    float cellVoltage = 0.0f;
    float capacity = 0.0f;
    float remainingCapacityAh = 0.0f;
    int remainingCapacitymAh = 0;
    int watts = 0;
    int maxWatts = 0;
    float maxCurrent = 0.0f;
    float maxCurrentMin = 0.0f;
    byte currentDirection = 0;  // 0=idle, 1=discharging, 2=charging
    float selfConsumption = 0.0f;
    
    // Energy tracking
    float totalWh = 0.0f;
    float totalKWh = 0.0f;
    float totalPrice = 0.0f;
    float totalUsedmAh = 0.0f;
    double accumulatedWh = 0.0;
    
    // Runtime calculation
    float runtimeHours = 0.0f;
    int hours = 0;
    int minutes = 0;
};
SensorData sensorData;

// ===== TIME VARIABLES =====
struct TimeData {
    DateTime now;
    long nowSecs = 0;
    byte oldHour = 255;
    byte newHour = 0;
    char timeChars[5] = {'0', '0', ':', '0', '0'};
};
TimeData timeData;

// ===== UI CONTROL =====
struct UIControl {
    byte menu = 0;
    byte submenu = 0;
    byte menustep = 0;
    byte pressedButton = 0;
    
    // Button handling
    bool button1State = false;
    bool button2State = false;
    unsigned long button1PressTime = 0;
    unsigned long button2PressTime = 0;
    bool button1Handled = false;
    bool button2Handled = false;
    const unsigned long LONG_PRESS_TIME = 500;
    
    // Display buffers
    char batteryChars[7] = "000.00";
    char priceChars[6] = "0.000";
    
    byte saved = 0;
    byte reset = 0;
    bool statsSaved = true;
};
UIControl ui;

// ===== EEPROM ADDRESSES =====
struct EEPROMAddresses {
    static const int TYPE = 0;
    static const int CELL = 1;
    static const int CAPACITY = 2;
    static const int PRICE = 6;
    static const int REMAINING_CAP = 10;
    static const int KWH = 14;
    static const int MODE = 18;
    static const int WIFI = 19;
    static const int SCREEN = 30;
    static const int ORIENTATION = 31;
    static const int KWH_HISTORY = 32;
};

// ===== DATA ARRAYS =====
float hourlyKWh[12] = {0};          // Last 12 hours kWh values
float historyCapacity[72] = {0};     // 72 × 10min = 720min = 12h
float historyVoltage[72] = {0};      // Voltage history
float last60Amps[60] = {0};          // Last 60 seconds of amp readings
int capacityIndex = 0;
bool hasLoggedThis10Min = false;

// ===== BATTERY CURVES =====
// Li-ion battery discharge curve
const int LIION_POINTS = 11;
const float liionVoltage[LIION_POINTS]  = {4.20, 4.10, 4.00, 3.90, 3.80, 3.70, 3.60, 3.50, 3.40, 3.30, 3.20};
const float liionCapacity[LIION_POINTS] = {100,   90,   80,   70,   60,   50,   40,   30,   20,   10,    0};

// LiPo battery discharge curve
const int LIPO_POINTS = 11;
const float lipoVoltage[LIPO_POINTS]  = {4.20, 4.15, 4.05, 3.95, 3.85, 3.75, 3.65, 3.55, 3.45, 3.35, 3.30};
const float lipoCapacity[LIPO_POINTS] = {100,   95,   85,   75,   65,   55,   45,   35,   20,   10,    0};

// LiFePO4 battery discharge curve
const int LIFEPO4_POINTS = 9;
const float lifepo4Voltage[LIFEPO4_POINTS]  = {3.65, 3.45, 3.40, 3.35, 3.30, 3.25, 3.20, 3.10, 2.90};
const float lifepo4Capacity[LIFEPO4_POINTS] = {100,   95,   90,   80,   60,   40,   25,   10,    0};

// ===== FUNCTION DECLARATIONS =====
void initializeSystem();
void initializeSSID();
void initializeHardware();
void initializeEEPROM();
void initializeWiFi();
void initializeWebServer();
void initializeMQTT();

void updateSensorData();
void updateTime();
void handleButtons();
void handleEnergyLogging();
float estimateCapacity(float voltage, const float* volts, const float* caps, int size);
void saveConfigToEEPROM();
void loadConfigFromEEPROM();

#ifdef OLED
void updateDisplay();
void drawMainScreen();
void drawPowerScreen();
void drawStatsScreen();
void drawMenuScreen();
void drawCapacityEditScreen();
void drawPriceEditScreen();
void drawTimeEditScreen();
void handleButtonInput();
#endif

#ifdef MQTT
void handleMQTT();
void checkMQTTConnection();
void publishSensorData();
void publishDiscoveryMessages();
void publishDiscoveryEntity(const char* name, const char* unit, const char* deviceClass, 
                           const char* icon, const String& deviceInfo, const String& baseTopic);
#endif

void handleWebServer();
void disableWiFiServer();

// ===== MAIN SETUP FUNCTION =====
void setup() {
    Serial.begin(9600);
    Serial.println("ESP32 Power Meter Starting...");
    
    // Initialize all subsystems
    initializeSystem();
    
    Serial.println("System initialization complete!");
}

// ===== MAIN LOOP =====
void loop() {
    // Handle time-critical operations
    handleButtons();
    
    // Handle sensor reading and time updates every second
    if (millis() - timing.lastSensorMillis >= timing.SENSOR_INTERVAL) {
        timing.lastSensorMillis = millis();
        updateTime();
        updateSensorData();
        handleEnergyLogging();
    }
    
    #ifdef OLED
    handleButtonInput();
    updateDisplay();
    #endif
    
    #ifdef MQTT
    if (config.wifiEnabled && WiFi.status() == WL_CONNECTED) {
        handleMQTT();
    }
    #endif
}

// ===== SYSTEM INITIALIZATION =====
/**
 * Initialize all system components in proper order
 */
void initializeSystem() {
    // Setup pins
    pinMode(BUTTON1_PIN, INPUT_PULLUP);
    pinMode(BUTTON2_PIN, INPUT_PULLUP);
    
    // Initialize subsystems
    initializeSSID();
    initializeHardware();
    
    #ifdef OLED
    initializeEEPROM();
    #endif
    
    if (config.wifiEnabled) {
        initializeWiFi();
        
        #ifdef MQTT
        initializeMQTT();
        #endif
        
        #ifdef WEBSERVER
        initializeWebServer();
        #endif
        
        sensorData.selfConsumption = config.deviceCurrentWifi;
    } else {
        sensorData.selfConsumption = config.deviceCurrent;
    }
    
    // Initialize data arrays
    memset(last60Amps, 0, sizeof(last60Amps));
    memset(historyCapacity, 0, sizeof(historyCapacity));
    memset(historyVoltage, 0, sizeof(historyVoltage));
    memset(hourlyKWh, 0, sizeof(hourlyKWh));
}

/**
 * Generate unique SSID for access point mode
 */
void initializeSSID() {
    preferences.begin("esp32meter", false);
    
    if (!preferences.isKey("ssid")) {
        randomSeed(esp_random());
        int suffix = random(100, 1000);
        snprintf(AP_SSID, sizeof(AP_SSID), "ESP32-Meter-%d", suffix);
        preferences.putString("ssid", AP_SSID);
    } else {
        String storedSSID = preferences.getString("ssid", "ESP32-Meter");
        storedSSID.toCharArray(AP_SSID, sizeof(AP_SSID));
    }
    
    preferences.end();
    Serial.println("SSID: " + String(AP_SSID));
}

/**
 * Initialize all hardware components (I2C, INA226, RTC, OLED)
 */
void initializeHardware() {
    // Initialize I2C
    Wire.begin();
    
    // Initialize INA226 current sensor
    if (!ina226.init()) {
        Serial.println("ERROR: INA226 initialization failed!");
        return;
    }
    
    // Configure INA226 for optimal performance
    ina226.setAverage(AVERAGE_64);           // Average 64 samples for stability
    ina226.setMeasureMode(CONTINUOUS);       // Continuous measurement mode

    // Shunt resistor configuration - adjust based on your hardware
    // R002 = 2mΩ resistor, max ~41A measurement range
    // R004 = 4mΩ resistor, max ~20A measurement range  
    // R001 = 1mΩ resistor, max ~82A measurement range
    ina226.setResistorRange(0.00198, 20);    // R002 2mOhm resistor, max ~41A
    ina226.waitUntilConversionCompleted();

    Serial.println("INA226 sensor initialized successfully");
    
    // Initialize RTC
    if (!rtc.begin()) {
        Serial.println("WARNING: RTC initialization failed - using system time");
    } else {
        if (rtc.lostPower()) {
            Serial.println("RTC lost power, setting to compile time");
            rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        }
        Serial.println("RTC initialized successfully");
    }
    
    #ifdef OLED
    // Initialize OLED display
    u8g2.begin();
    setDisplayOrientation();
    Serial.println("OLED initialized successfully");
    #endif
}

#ifdef OLED
/**
 * Set display orientation based on configuration
 */
void setDisplayOrientation() {
    switch (config.orientation) {
        case 0: u8g2.setDisplayRotation(U8G2_R1); break;
        case 1: u8g2.setDisplayRotation(U8G2_R2); break;
        case 2: u8g2.setDisplayRotation(U8G2_R3); break;
        case 3: u8g2.setDisplayRotation(U8G2_R0); break;
        default: u8g2.setDisplayRotation(U8G2_R2); break;
    }
    u8g2.clear();
}
#endif

/**
 * Load configuration from EEPROM and validate values
 */
void initializeEEPROM() {
    EEPROM.begin(512);
    Serial.println("Loading configuration from EEPROM...");
    
    // Load battery configuration
    config.batteryType = EEPROM.read(EEPROMAddresses::TYPE);
    if (config.batteryType > 2) config.batteryType = 0;
    
    config.cellcount = EEPROM.read(EEPROMAddresses::CELL);
    if (config.cellcount == 0 || config.cellcount > 6) config.cellcount = 1;
    
    // Load and validate float values
    EEPROM.get(EEPROMAddresses::CAPACITY, config.batteryCapacityAh);
    if (isnan(config.batteryCapacityAh) || config.batteryCapacityAh <= 0 || config.batteryCapacityAh > 1000) {
        config.batteryCapacityAh = 1.0f;
        EEPROM.put(EEPROMAddresses::CAPACITY, config.batteryCapacityAh);
    }
    
    EEPROM.get(EEPROMAddresses::PRICE, config.pricePerKWh);
    if (isnan(config.pricePerKWh) || config.pricePerKWh < 0 || config.pricePerKWh > 1000) {
        config.pricePerKWh = 0.0f;
        EEPROM.put(EEPROMAddresses::PRICE, config.pricePerKWh);
    }
    
    // Load UI settings
    config.wifiEnabled = EEPROM.read(EEPROMAddresses::WIFI);
    if (config.wifiEnabled > 1) {
        config.wifiEnabled = 0;
        EEPROM.write(EEPROMAddresses::WIFI, config.wifiEnabled);
    }
    
    config.screen = EEPROM.read(EEPROMAddresses::SCREEN);
    if (config.screen > 2) config.screen = 0;
    
    config.orientation = EEPROM.read(EEPROMAddresses::ORIENTATION);
    if (config.orientation > 3) config.orientation = 0;
    
    // Load sensor data
    EEPROM.get(EEPROMAddresses::REMAINING_CAP, sensorData.remainingCapacityAh);
    if (isnan(sensorData.remainingCapacityAh) || sensorData.remainingCapacityAh < 0) {
        sensorData.remainingCapacityAh = config.batteryCapacityAh;
        EEPROM.put(EEPROMAddresses::REMAINING_CAP, sensorData.remainingCapacityAh);
    }
    
    EEPROM.get(EEPROMAddresses::KWH, sensorData.totalKWh);
    if (isnan(sensorData.totalKWh) || sensorData.totalKWh < 0 || sensorData.totalKWh > 100000) {
        sensorData.totalKWh = 0.0f;
        EEPROM.put(EEPROMAddresses::KWH, sensorData.totalKWh);
    }
    
    EEPROM.commit();
    
    // Calculate derived values
    sensorData.remainingCapacitymAh = sensorData.remainingCapacityAh * 1000.0f;
    sensorData.totalWh = sensorData.totalKWh * 1000.0f;
    sensorData.totalPrice = sensorData.totalKWh * config.pricePerKWh;
    
    Serial.println("Configuration loaded successfully");
    Serial.println("Battery Type: " + String(config.batteryType));
    Serial.println("Cell Count: " + String(config.cellcount));
    Serial.println("Capacity: " + String(config.batteryCapacityAh) + " Ah");
    Serial.println("Price/kWh: " + String(config.pricePerKWh));
}

/**
 * Initialize WiFi in dual mode (AP + Station)
 */
void initializeWiFi() {
    WiFi.mode(WIFI_AP_STA);
    
    // Start Access Point
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.println("Access Point: " + String(AP_SSID));
    Serial.println("AP IP: " + WiFi.softAPIP().toString());
    
    // Try to connect to existing WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500);
        Serial.print(".");
        retry++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
    } else {
        Serial.println("\nWiFi connection failed - AP mode only");
    }
}

#ifdef MQTT
/**
 * Initialize MQTT client for Home Assistant integration
 */
void initializeMQTT() {
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setKeepAlive(60);
    mqttClient.setBufferSize(512);
    Serial.println("MQTT client initialized");
}
#endif

/**
 * Initialize web server and API endpoints
 */
void initializeWebServer() {
    if (!LittleFS.begin(true)) {
        Serial.println("Failed to mount LittleFS");
        return;
    }
    
    // Serve static files
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    
    // API endpoint for sensor data
    server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = buildSensorDataJSON();
        request->send(200, "application/json", json);
    });
    
    // API endpoint for screen control
    server.on("/setScreen", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("value", true)) {
            String value = request->getParam("value", true)->value();
            config.screen = value.toInt();
            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Missing 'value'");
        }
    });
    
    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "404: Not Found");
    });
    
    server.begin();
    Serial.println("Webserver started");
}

/**
 * Build JSON string with all sensor data for web interface
 */
String buildSensorDataJSON() {
    String json = "{";
    json += "\"now\":" + String(timeData.nowSecs) + ",";
    json += "\"batteryType\":" + String(config.batteryType) + ",";
    json += "\"cellcount\":" + String(config.cellcount) + ",";
    json += "\"watts\":" + String(sensorData.watts) + ",";
    json += "\"Ampere\":" + String(sensorData.current, 2) + ",";
    json += "\"cur_dir\":" + String(sensorData.currentDirection) + ",";
    json += "\"voltage\":" + String(sensorData.voltage, 2) + ",";
    json += "\"capacity\":" + String(sensorData.capacity, 2) + ",";
    json += "\"batteryCapacityAh\":" + String(config.batteryCapacityAh, 2) + ",";
    json += "\"remainingCapacityAh\":" + String(sensorData.remainingCapacityAh, 2) + ",";
    json += "\"pricePerKWh\":" + String(config.pricePerKWh, 3) + ",";
    json += "\"totalWh\":" + String(sensorData.totalWh, 2) + ",";
    json += "\"totalKWh\":" + String(sensorData.totalKWh, 3) + ",";
    
    // Dynamic energy display
    if (abs(sensorData.totalUsedmAh) >= 1000.0f) {
        json += "\"usedEnergy\":" + String(sensorData.totalUsedmAh / 1000.0f, 2) + ",";
        json += "\"usedUnit\":\"Ah\",";
    } else {
        json += "\"usedEnergy\":" + String(sensorData.totalUsedmAh, 0) + ",";
        json += "\"usedUnit\":\"mAh\",";
    }
    
    json += "\"screen\":" + String(config.screen) + ",";
    json += "\"maxWatts\":" + String(sensorData.maxWatts) + ",";
    json += "\"maxA\":" + String(sensorData.maxCurrent, 2) + ",";
    json += "\"maxA_min\":" + String(sensorData.maxCurrentMin, 2);
    
    // Add array data
    json += ",\"hourlyKWh\":[";
    for (int i = 0; i < 12; i++) {
        json += String(hourlyKWh[i], 3);
        if (i < 11) json += ",";
    }
    json += "]";
    
    json += ",\"historyCapacity\":[";
    for (int i = 0; i < 72; i++) {
        json += String(static_cast<int>(historyCapacity[i]));
        if (i < 71) json += ",";
    }
    json += "]";
    
    json += ",\"historyVoltage\":[";
    for (int i = 0; i < 72; i++) {
        json += String(historyVoltage[i], 2);
        if (i < 71) json += ",";
    }
    json += "]";
    
    json += ",\"last60Amps\":[";
    for (int i = 0; i < 60; i++) {
        json += String(last60Amps[i], 2);
        if (i < 59) json += ",";
    }
    json += "]";
    
    json += "}";
    return json;
}

/**
 * Update time from RTC
 */
void updateTime() {
    timeData.now = rtc.now();
    timeData.nowSecs = timeData.now.unixtime();
    timeData.newHour = timeData.now.hour();
}

/**
 * Read sensor data and perform calculations
 */
void updateSensorData() {
    static const float DEADBAND_THRESHOLD = 0.01f; // 10 mA deadband
    
    // Read raw sensor values
    ina226.readAndClearFlags();
    sensorData.voltage = ina226.getBusVoltage_V();
    sensorData.current = ina226.getCurrent_A();
    float mAmpere = ina226.getCurrent_mA();
    
    // Determine current direction and normalize values
    if (sensorData.current < -DEADBAND_THRESHOLD) {
        sensorData.currentDirection = 2; // Charging
        sensorData.current = -sensorData.current;
        mAmpere = -mAmpere;
    } else if (sensorData.current > DEADBAND_THRESHOLD) {
        sensorData.currentDirection = 1; // Discharging
    } else {
        sensorData.currentDirection = 0; // Idle
        sensorData.current = 0;
        mAmpere = 0;
    }
    
    // Calculate power
    sensorData.watts = sensorData.voltage * sensorData.current;
    if (sensorData.currentDirection == 2) {
        sensorData.watts = -sensorData.watts; // Negative for charging
    }
    
    // Update maximum values
    if (sensorData.current > sensorData.maxCurrent) {
        sensorData.maxCurrent = sensorData.current;
    }
    if (abs(sensorData.watts) > sensorData.maxWatts) {
        sensorData.maxWatts = abs(sensorData.watts);
    }
    
    // Perform energy calculations every second (approximately)
    unsigned long currentMicros = micros();
    if (currentMicros - timing.previousMicros >= timing.INTERVAL_MICROS) {
        timing.previousMicros = currentMicros;

         // Small deadband for milli-amp readings
        if (abs(mAmpere) <= 4) mAmpere = 0;

        // Calculate energy consumption/production
        float realCurrent = sensorData.current + sensorData.selfConsumption;
        float usedmAh = (realCurrent * 1000.0f) / 3600.0f; // mAh per second
        
        // Update remaining capacity
        if (sensorData.currentDirection == 1) { // Discharging
            sensorData.remainingCapacityAh -= usedmAh / 1000.0f;
            sensorData.totalUsedmAh -= usedmAh;
        } else if (sensorData.currentDirection == 2) { // Charging
            sensorData.remainingCapacityAh += usedmAh / 1000.0f;
            sensorData.totalUsedmAh += usedmAh;
        }
        
        // Constrain capacity to valid range
        sensorData.remainingCapacityAh = constrain(sensorData.remainingCapacityAh, 
                                                   0.0f, config.batteryCapacityAh);
        sensorData.remainingCapacitymAh = sensorData.remainingCapacityAh * 1000.0f;
        
        // Auto-calibrate capacity based on voltage (after 5 seconds uptime)
        if (millis() > 5000) {
            if (sensorData.capacity >= 99.9f) {
                sensorData.remainingCapacityAh = config.batteryCapacityAh;
            } else if (sensorData.capacity <= 0.5f) {
                sensorData.remainingCapacityAh = 0;
            }
        }
        
        // Calculate total energy production (charging only)
        if (sensorData.currentDirection == 2) {
            float deltaTimeHours = (currentMicros - timing.lastUpdateMicros) / 3600000000.0f;
            sensorData.totalWh += abs(sensorData.watts) * deltaTimeHours;
            sensorData.totalKWh = sensorData.totalWh / 1000.0f;
            sensorData.totalPrice = sensorData.totalKWh * config.pricePerKWh;
        }
        timing.lastUpdateMicros = currentMicros;
    }
    
    // Calculate cell voltage and capacity percentage
    sensorData.cellVoltage = sensorData.voltage / config.cellcount;
    sensorData.capacity = estimateCapacity(sensorData.cellVoltage, 
                                           getBatteryVoltageArray(), 
                                           getBatteryCapacityArray(), 
                                           getBatteryPointsCount());
    
    // Calculate runtime estimate
    calculateRuntimeEstimate();
}

/**
 * Get appropriate battery voltage array based on battery type
 */
const float* getBatteryVoltageArray() {
    switch (config.batteryType) {
        case 0: return liionVoltage;
        case 1: return lipoVoltage;
        case 2: return lifepo4Voltage;
        default: return liionVoltage;
    }
}

/**
 * Get appropriate battery capacity array based on battery type
 */
const float* getBatteryCapacityArray() {
    switch (config.batteryType) {
        case 0: return liionCapacity;
        case 1: return lipoCapacity;
        case 2: return lifepo4Capacity;
        default: return liionCapacity;
    }
}

/**
 * Get number of points in battery curve based on battery type
 */
int getBatteryPointsCount() {
    switch (config.batteryType) {
        case 0: return LIION_POINTS;
        case 1: return LIPO_POINTS;
        case 2: return LIFEPO4_POINTS;
        default: return LIION_POINTS;
    }
}

/**
 * Estimate battery capacity based on cell voltage using linear interpolation
 */
float estimateCapacity(float voltage, const float* volts, const float* caps, int size) {
    if (voltage >= volts[0]) return 100.0f;
    if (voltage <= volts[size - 1]) return 0.0f;

    for (int i = 0; i < size - 1; i++) {
        if (voltage <= volts[i] && voltage >= volts[i + 1]) {
            float v1 = volts[i];
            float v2 = volts[i + 1];
            float c1 = caps[i];
            float c2 = caps[i + 1];
            return c1 + ((voltage - v1) * (c2 - c1) / (v2 - v1));
        }
    }
    return 0.0f;
}

/**
 * Calculate estimated runtime based on current consumption/charging
 */
void calculateRuntimeEstimate() {
    if (sensorData.currentDirection == 1 && sensorData.current > 0) {
        // Discharging - time until empty
        sensorData.runtimeHours = sensorData.remainingCapacityAh / sensorData.current;
    } else if (sensorData.currentDirection == 2 && sensorData.current > 0) {
        // Charging - time until full
        float remainingToCharge = config.batteryCapacityAh - sensorData.remainingCapacityAh;
        sensorData.runtimeHours = remainingToCharge / sensorData.current;
    } else {
        sensorData.runtimeHours = 0;
    }
    
    sensorData.hours = static_cast<int>(sensorData.runtimeHours);
    sensorData.minutes = static_cast<int>((sensorData.runtimeHours - sensorData.hours) * 60);
}

/**
 * Handle energy logging and history tracking
 */
void handleEnergyLogging() {
    // Update 60-second current history
    if (timeData.nowSecs != timing.lastEnergySecond) {
        timing.lastEnergySecond = timeData.nowSecs;
        
        // Shift array left and add new value
        for (int i = 0; i < 59; i++) {
            last60Amps[i] = last60Amps[i + 1];
        }
        last60Amps[59] = sensorData.current;
        
        // Calculate 60-second maximum
        sensorData.maxCurrentMin = 0.0f;
        for (int i = 0; i < 60; i++) {
            if (last60Amps[i] > sensorData.maxCurrentMin) {
                sensorData.maxCurrentMin = last60Amps[i];
            }
        }
    }
    
    // Accumulate energy while charging
    if (sensorData.currentDirection == 2) {
        sensorData.accumulatedWh += abs(sensorData.watts) / 3600.0; // W to Wh per second
    }
    
    // Log every 10 minutes
    DateTime now = rtc.now();
    if (now.minute() % 10 == 0 && now.second() == 0) {
        if (!hasLoggedThis10Min) {
            hasLoggedThis10Min = true;
            
            // Shift arrays left
            for (int i = 0; i < 71; i++) {
                historyCapacity[i] = historyCapacity[i + 1];
                historyVoltage[i] = historyVoltage[i + 1];
            }
            
            // Add new values
            historyCapacity[71] = sensorData.capacity;
            historyVoltage[71] = sensorData.voltage;
        }
    } else {
        hasLoggedThis10Min = false;
    }
    
    // Log energy hourly
    if (timeData.newHour != timeData.oldHour) {
        timeData.oldHour = timeData.newHour;
        
        // Shift hourly array left
        for (int i = 0; i < 11; i++) {
            hourlyKWh[i] = hourlyKWh[i + 1];
        }
        
        // Log accumulated charging energy for past hour
        hourlyKWh[11] = sensorData.accumulatedWh / 1000.0; // Wh to kWh
        sensorData.accumulatedWh = 0; // Reset for next hour
    }
}

/**
 * Handle button press detection with debouncing
 */
void handleButtons() {
    handleButton(BUTTON1_PIN, ui.button1State, ui.button1PressTime, ui.button1Handled, 1);
    handleButton(BUTTON2_PIN, ui.button2State, ui.button2PressTime, ui.button2Handled, 2);
}

/**
 * Generic button handler with long press detection
 */
void handleButton(int pin, bool &state, unsigned long &pressTime, bool &handled, int buttonValue) {
    bool isPressed = digitalRead(pin) == LOW;

    if (isPressed && !state) {
        // Button just pressed
        state = true;
        pressTime = millis();
        handled = false;
    }

    if (!isPressed && state) {
        // Button just released
        state = false;
        unsigned long duration = millis() - pressTime;

        if (duration >= ui.LONG_PRESS_TIME) {
            ui.pressedButton = buttonValue * 11; // Long press: 1→11, 2→22
        } else {
            ui.pressedButton = buttonValue; // Short press: 1 or 2
        }
        handled = true;
    }
}

/**
 * Save current configuration to EEPROM
 */
void saveConfigToEEPROM() {
    EEPROM.put(EEPROMAddresses::TYPE, config.batteryType);
    EEPROM.put(EEPROMAddresses::CELL, config.cellcount);
    EEPROM.put(EEPROMAddresses::CAPACITY, config.batteryCapacityAh);
    EEPROM.put(EEPROMAddresses::PRICE, config.pricePerKWh);
    EEPROM.put(EEPROMAddresses::SCREEN, config.screen);
    EEPROM.put(EEPROMAddresses::ORIENTATION, config.orientation);
    EEPROM.put(EEPROMAddresses::WIFI, config.wifiEnabled);
    EEPROM.put(EEPROMAddresses::REMAINING_CAP, sensorData.remainingCapacityAh);
    EEPROM.put(EEPROMAddresses::KWH, sensorData.totalKWh);
    EEPROM.commit();
    
    Serial.println("Configuration saved to EEPROM");
}

/**
 * Disable WiFi and web server to save power
 */
void disableWiFiServer() {
    server.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    config.wifiEnabled = 0;
    saveConfigToEEPROM();
    delay(500);
    Serial.println("WiFi disabled");
}

/**
 * Format price value for display
 */
void formatPriceToChars(float value) {
    char temp[7];
    snprintf(temp, sizeof(temp), "%1.3f", value);
    for (int i = 0; i < 6; i++) {
        ui.priceChars[i] = (temp[i] == '.') ? ',' : temp[i];
    }
    ui.priceChars[5] = '\0';
}

#ifdef OLED
/**
 * Main display update function
 */
void updateDisplay() {
    switch (ui.menu) {
        case 0: drawMainScreen(); break;
        case 1: drawMenuScreen(); break;
        case 2: drawCapacityEditScreen(); break;
        case 3: drawPriceEditScreen(); break;
        case 4: drawTimeEditScreen(); break;
        default: drawMainScreen(); break;
    }
}

/**
 * Handle button input for UI navigation
 */
void handleButtonInput() {
    if (ui.pressedButton == 0) return;
    
    switch (ui.menu) {
        case 0: handleMainScreenInput(); break;
        case 1: handleMenuScreenInput(); break;
        case 2: handleCapacityEditInput(); break;
        case 3: handlePriceEditInput(); break;
        case 4: handleTimeEditInput(); break;
    }
    
    ui.pressedButton = 0; // Reset button state
}

/**
 * Handle input on main screen
 */
void handleMainScreenInput() {
    if (ui.pressedButton == 22) { // Long press button 2 - Enter menu
        ui.menu = 1;
        ui.menustep = 0;
    } else if (ui.pressedButton == 11) { // Long press button 1 - Toggle WiFi
        if (config.wifiEnabled) {
            disableWiFiServer();
        } else {
            config.wifiEnabled = 1;
            saveConfigToEEPROM();
            esp_restart(); // Restart to enable WiFi
        }
    } else if (ui.pressedButton == 2) { // Short press button 2 - Change screen
        config.screen = (config.screen + 1) % 3;
        u8g2.clear();
    } else if (ui.pressedButton == 1) { // Short press button 1 - Change orientation
        config.orientation = (config.orientation + 1) % 4;
        setDisplayOrientation();
    }
}

/**
 * Handle input in menu screen
 */
void handleMenuScreenInput() {
    if (ui.pressedButton == 2) { // Navigate menu
        ui.menustep = (ui.menustep + 1) % 10; //10 menu items
    } else if (ui.pressedButton == 1) { // Select menu item
        handleMenuSelection();
    }
}

/**
 * Handle menu item selection
 */
void handleMenuSelection() {
    switch (ui.menustep) {
        case 0: // Change battery type
            config.batteryType = (config.batteryType + 1) % 3;
            break;
        case 1: // Change cell count
            config.cellcount = (config.cellcount % 6) + 1;
            break;
        case 2: // Set max capacity
            snprintf(ui.batteryChars, sizeof(ui.batteryChars), "%06.2f", config.batteryCapacityAh);
            ui.menu = 2;
            ui.menustep = 0;
            return;
        case 3: // Estimate capacity from voltage
            sensorData.remainingCapacityAh = (sensorData.capacity / 100.0f) * config.batteryCapacityAh;
            break;
        case 4: // Set capacity to 0
            sensorData.remainingCapacityAh = 0;
            break;
        case 5: // Set capacity to full
            sensorData.remainingCapacityAh = config.batteryCapacityAh;
            break;
        case 6: // Set price per kWh
            formatPriceToChars(config.pricePerKWh);
            ui.menu = 3;
            ui.menustep = 0;
            return;
        case 7: // Reset stats
            resetStatistics();
            ui.reset = 1;
            break;
        case 8: // Set time
            initializeTimeEditChars();
            ui.menu = 4;
            ui.menustep = 0;
            return;
        case 9: // Save and exit
            saveConfigToEEPROM();
            ui.menu = 0;
            ui.menustep = 0;
            ui.saved = 0;
            ui.reset = 0;
            break;
    }
}

/**
 * Reset all statistics
 */
void resetStatistics() {
    sensorData.totalKWh = 0;
    sensorData.totalWh = 0;
    sensorData.totalUsedmAh = 0;
    sensorData.totalPrice = 0;
    sensorData.maxWatts = 0;
    sensorData.maxCurrent = 0;
    sensorData.maxCurrentMin = 0;
    
    // Clear history arrays
    memset(hourlyKWh, 0, sizeof(hourlyKWh));
    memset(historyCapacity, 0, sizeof(historyCapacity));
    memset(historyVoltage, 0, sizeof(historyVoltage));
    memset(last60Amps, 0, sizeof(last60Amps));
    
    Serial.println("Statistics reset");
}

/**
 * Initialize time edit characters with current time
 */
void initializeTimeEditChars() {
    DateTime now = rtc.now();
    int h = now.hour();
    int m = now.minute();
    
    timeData.timeChars[0] = '0' + (h / 10);
    timeData.timeChars[1] = '0' + (h % 10);
    timeData.timeChars[2] = ':';
    timeData.timeChars[3] = '0' + (m / 10);
    timeData.timeChars[4] = '0' + (m % 10);
}

/**
 * Handle capacity edit screen input
 */
void handleCapacityEditInput() {
    if (ui.pressedButton == 2) { // Navigate character
        ui.menustep = (ui.menustep + 1) % 7;
        if (ui.menustep == 3) ui.menustep++; // Skip decimal point
    } else if (ui.pressedButton == 1) {
        if (ui.menustep < 6) { // Edit digit
            char &c = ui.batteryChars[ui.menustep];
            if (c >= '0' && c <= '9') {
                c = (c == '9') ? '0' : c + 1;
            }
        } else { // Confirm and exit
            float newValue = atof(ui.batteryChars);
            if (newValue > 999.99f) newValue = 999.99f;
            config.batteryCapacityAh = newValue;
            sensorData.remainingCapacityAh = config.batteryCapacityAh;
            ui.menu = 1;
            ui.menustep = 0;
        }
    }
}

/**
 * Handle price edit screen input
 */
void handlePriceEditInput() {
    if (ui.pressedButton == 2) { // Navigate character
        ui.menustep = (ui.menustep + 1) % 6;
        if (ui.menustep == 1) ui.menustep++; // Skip decimal comma
    } else if (ui.pressedButton == 1) {
        if (ui.menustep < 5) { // Edit digit
            char &c = ui.priceChars[ui.menustep];
            if (c >= '0' && c <= '9') {
                c = (c == '9') ? '0' : c + 1;
            }
        } else { // Confirm and exit
            char tempStr[7];
            for (int i = 0; i < 6; i++) {
                tempStr[i] = (ui.priceChars[i] == ',') ? '.' : ui.priceChars[i];
            }
            float newValue = atof(tempStr);
            if (newValue > 9.999f) newValue = 9.999f;
            config.pricePerKWh = newValue;
            formatPriceToChars(config.pricePerKWh);
            ui.menu = 1;
            ui.menustep = 0;
        }
    }
}

/**
 * Handle time edit screen input
 */
void handleTimeEditInput() {
    if (ui.pressedButton == 2) { // Navigate character
        ui.menustep = (ui.menustep + 1) % 6;
        if (ui.menustep == 2) ui.menustep++; // Skip colon
    } else if (ui.pressedButton == 1) {
        if (ui.menustep != 2 && ui.menustep != 5) { // Edit digit
            char &c = timeData.timeChars[ui.menustep];
            if (c >= '0' && c <= '9') {
                c++;
                if (c > '9') c = '0';
                
                // Validate time ranges
                if (ui.menustep == 0 && c > '2') c = '0';
                if (ui.menustep == 1 && timeData.timeChars[0] == '2' && c > '3') c = '0';
                if (ui.menustep == 3 && c > '5') c = '0';
            }
        } else if (ui.menustep == 5) { // Confirm time
            int hours = (timeData.timeChars[0] - '0') * 10 + (timeData.timeChars[1] - '0');
            int minutes = (timeData.timeChars[3] - '0') * 10 + (timeData.timeChars[4] - '0');
            
            if (hours <= 23 && minutes <= 59) {
                rtc.adjust(DateTime(2025, 1, 1, hours, minutes, 0));
                Serial.println("Time updated to " + String(hours) + ":" + String(minutes));
            }
            
            ui.menu = 1;
            ui.menustep = 0;
        }
    }
}

/**
 * Draw main screen based on current screen selection
 */
void drawMainScreen() {
    switch (config.screen) {
        case 0: drawBatteryScreen(); break;
        case 1: drawPowerScreen(); break;
        case 2: drawStatsScreen(); break;
        default: drawBatteryScreen(); break;
    }
}

/**
 * Draw battery status screen (screen 0)
 */
void drawBatteryScreen() {
    u8g2.firstPage();
    do {
        drawPageIndicator(0);
        drawWiFiIcon();
        
        // Battery type and cell count
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.setCursor(0, 10);
        u8g2.print("BAT ");
        
        const char* batteryTypes[] = {"LiIon", "LiPo", "LiFePO4"};
        u8g2.print(batteryTypes[config.batteryType]);
        u8g2.print(" ");
        u8g2.print(config.cellcount);
        u8g2.println("s");
        
        // Large capacity percentage
        u8g2.setCursor(0, 42);
        u8g2.setFont(u8g2_font_fub20_tr);
        u8g2.print(sensorData.capacity, 0);
        u8g2.setFont(u8g2_font_fub11_tr);
        u8g2.println(" %");
        
        // Battery icon with capacity bar
        drawBatteryIcon();
        
        // Voltage
        u8g2.setCursor(0, 72);
        u8g2.setFont(u8g2_font_fub20_tr);
        u8g2.print(sensorData.voltage, 1);
        u8g2.setFont(u8g2_font_fub11_tr);
        u8g2.println(" V");
        
        // Remaining capacity
        u8g2.setCursor(0, 102);
        if (sensorData.remainingCapacityAh > 1) {
            u8g2.setFont(u8g2_font_fub20_tr);
            u8g2.print(sensorData.remainingCapacityAh, 2);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" Ah");
        } else {
            u8g2.setFont(u8g2_font_fub20_tr);
            u8g2.print(sensorData.remainingCapacitymAh);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" mAh");
        }
        
        // Current and runtime
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.setCursor(0, 123);
        if (sensorData.currentDirection == 1) u8g2.print("-");
        u8g2.print(sensorData.current, 2);
        u8g2.println(" A ");
        
        if (sensorData.runtimeHours >= 0) {
            u8g2.println("TIME ");
            u8g2.printf("%02dh %02dm", sensorData.hours, sensorData.minutes);
        }
        
    } while (u8g2.nextPage());
}

/**
 * Draw power screen (screen 1)
 */
void drawPowerScreen() {
    u8g2.firstPage();
    do {
        drawPageIndicator(1);
        drawWiFiIcon();
        
        // Cost information
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.setCursor(0, 10);
        u8g2.print("COST ");
        u8g2.print(sensorData.totalPrice, 2);
        u8g2.print(" EUR");
        
        // Current power
        u8g2.setFont(u8g2_font_fub20_tr);
        u8g2.setCursor(0, 42);
        u8g2.print(abs(sensorData.watts));
        u8g2.setFont(u8g2_font_fub11_tr);
        u8g2.println(" W");
        
        // Peak watts
        u8g2.setCursor(85, 33);
        u8g2.print(sensorData.maxWatts);
        u8g2.setCursor(85, 45);
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.println("PEAK W");
        
        // Voltage
        u8g2.setFont(u8g2_font_fub20_tr);
        u8g2.setCursor(0, 72);
        u8g2.print(sensorData.voltage, 1);
        u8g2.setFont(u8g2_font_fub11_tr);
        u8g2.println(" V");
        
        // Current
        u8g2.setFont(u8g2_font_fub20_tr);
        u8g2.setCursor(0, 102);
        if (sensorData.currentDirection == 1) u8g2.print("-");
        
        if (sensorData.current > 1) {
            u8g2.print(sensorData.current, 2);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" A");
        } else {
            u8g2.print(sensorData.current * 1000, 0);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" mA");
        }
        
        // Total energy
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.setCursor(0, 123);
        u8g2.println("TOTAL ");
        if (sensorData.totalWh >= 999.9f) {
            u8g2.print(sensorData.totalKWh, 2);
            u8g2.println(" kWh");
        } else {
            u8g2.print(sensorData.totalWh, 0);
            u8g2.println(" Wh");
        }
        
    } while (u8g2.nextPage());
}

/**
 * Draw statistics screen (screen 2)
 */
void drawStatsScreen() {
    u8g2.firstPage();
    do {
        drawPageIndicator(2);
        drawWiFiIcon();
        
        // Power information
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.setCursor(0, 10);
        u8g2.print("POWER ");
        u8g2.print(abs(sensorData.watts));
        u8g2.println(" W");
        
        // Voltage (high precision)
        u8g2.setFont(u8g2_font_fub20_tr);
        u8g2.setCursor(0, 42);
        u8g2.print(sensorData.voltage, 2);
        u8g2.setFont(u8g2_font_fub11_tr);
        u8g2.println(" V");
        
        // Current
        u8g2.setFont(u8g2_font_fub20_tr);
        u8g2.setCursor(0, 72);
        if (sensorData.currentDirection == 1) u8g2.print("-");
        
        if (sensorData.current > 1) {
            u8g2.print(sensorData.current, 2);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" A");
        } else {
            u8g2.print(sensorData.current * 1000, 0);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" mA");
        }
        
        // Used energy (dynamic scale)
        u8g2.setCursor(0, 102);
        u8g2.setFont(u8g2_font_fub20_tr);
        if (abs(sensorData.totalUsedmAh) >= 1000.0f) {
            float totalUsedAh = sensorData.totalUsedmAh / 1000.0f;
            u8g2.print(totalUsedAh, 2);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" Ah");
        } else {
            u8g2.print(sensorData.totalUsedmAh, 0);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" mAh");
        }
        
        // Peak current
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.setCursor(0, 123);
        u8g2.print("PEAK ");
        u8g2.print(sensorData.maxCurrent, 2);
        u8g2.print(" A");
        
    } while (u8g2.nextPage());
}

/**
 * Draw page indicator dots
 */
void drawPageIndicator(int currentPage) {
    for (int i = 0; i < 3; i++) {
        if (i == currentPage) {
            u8g2.drawBox(92 + i * 8, 4, 5, 5);
        } else {
            u8g2.drawFrame(92 + i * 8, 4, 5, 5);
        }
    }
}

/**
 * Draw WiFi status icon
 */
void drawWiFiIcon() {
    if (config.wifiEnabled) {
        u8g2.drawFrame(119, 4, 6, 1);
        u8g2.drawFrame(120, 6, 4, 1);
        u8g2.drawFrame(121, 8, 2, 1);
    }
}

/**
 * Draw battery icon with capacity bar and charge/discharge indicator
 */
void drawBatteryIcon() {
    // Battery terminals
    u8g2.drawFrame(89, 21, 6, 2);
    u8g2.drawFrame(114, 21, 6, 2);
    
    // Battery body
    u8g2.drawFrame(85, 24, 39, 30);
    
    // Capacity bar
    const byte maxHeight = 28;
    const byte bottomY = 53;
    byte capHeight = map(sensorData.capacity, 0, 100, 0, maxHeight);
    byte capY = bottomY - capHeight;
    
    u8g2.setFontMode(0);
    u8g2.setDrawColor(2);
    u8g2.drawBox(86, capY, 37, capHeight);
    
    // Charge/discharge arrows
    if (sensorData.currentDirection == 1) {
        // Discharge arrows (up)
        u8g2.drawTriangle(99, 40, 104, 34, 109, 40);
        u8g2.drawTriangle(99, 46, 104, 40, 109, 46);
    } else if (sensorData.currentDirection == 2) {
        // Charge arrows (down)
        u8g2.drawTriangle(99, 34, 104, 40, 109, 34);
        u8g2.drawTriangle(99, 40, 104, 46, 109, 40);
    }
    
    // Status text
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(92, 71);
    const char* statusText[] = {"IDLE", "LOAD", "CHRG"};
    u8g2.print(statusText[sensorData.currentDirection]);
}

/**
 * Draw menu screen
 */
void drawMenuScreen() {
    u8g2.firstPage();
    do {
        u8g2.setFont(u8g2_font_7x13B_tr);
        u8g2.setFontMode(0);
        u8g2.setDrawColor(2);
        
        if (ui.menustep <= 5) {
            // Page 1 of menu
            drawMenuItem(0, "Battery", getBatteryTypeString());
            drawMenuItem(1, "Cell Count", String(config.cellcount) + "s");
            drawMenuItem(2, "Capacity", String(config.batteryCapacityAh, 1) + " Ah");
            drawMenuItem(3, "Est c Cap", String(sensorData.remainingCapacityAh, 1) + " Ah");
            drawMenuItem(4, "Capacity to 0", "");
            drawMenuItem(5, "Capacity to full", "");
        } else {
            // Page 2 of menu
            drawMenuItem(6, "Price/kWh", String(config.pricePerKWh, 3));
            drawMenuItem(7, "Reset Stats", ui.reset ? "OK" : "");
            drawMenuItem(8, "Set Time", String(timeData.now.hour()) + ":" + 
                        (timeData.now.minute() < 10 ? "0" : "") + String(timeData.now.minute()));
            drawMenuItem(9, "Save & Exit", "");
            
            // Show network info
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.setCursor(5, 110);
            u8g2.print("SSID ");
            u8g2.print(AP_SSID);
            u8g2.setCursor(5, 124);
            if (WiFi.status() == WL_CONNECTED) {
                u8g2.print(WiFi.localIP());
            } else {
                u8g2.print("192.168.4.1");
            }
            #ifdef MQTT
            u8g2.print(" MQTT");
            #endif
        }
        
        // Highlight selected item
        int displayStep = ui.menustep > 5 ? ui.menustep - 6 : ui.menustep;
        u8g2.drawBox(0, displayStep * 21, 128, 20);
        
    } while (u8g2.nextPage());
}

/**
 * Draw individual menu item
 */
void drawMenuItem(int step, const String& title, const String& value) {
    int displayStep = step > 5 ? step - 6 : step;
    u8g2.setCursor(5, 14 + displayStep * 21);
    u8g2.print(title);
    if (value.length() > 0) {
        u8g2.print(" ");
        u8g2.print(value);
    }
}

/**
 * Get battery type as string
 */
String getBatteryTypeString() {
    const char* types[] = {"LiIon", "LiPo", "LiFePO4"};
    return String(types[config.batteryType]);
}

/**
 * Draw capacity edit screen
 */
void drawCapacityEditScreen() {
    u8g2.firstPage();
    do {
        u8g2.setFont(u8g2_font_7x13B_tr);
        u8g2.setCursor(5, 10);
        u8g2.print("Capacity in Ah");
        
        // Draw editable characters
        for (int i = 0; i < 6; i++) {
            int x = 10 + i * 12;
            u8g2.setCursor(x, 30);
            u8g2.print(ui.batteryChars[i]);
            
            if (i == ui.menustep) {
                u8g2.drawFrame(x - 2, 18, 12, 14);
            }
        }
        
        // Draw OK button
        int exitX = 10 + 6 * 12 + 8;
        u8g2.setCursor(exitX, 30);
        u8g2.print("OK");
        
        if (ui.menustep == 6) {
            int exitWidth = u8g2.getStrWidth("OK") + 4;
            u8g2.drawFrame(exitX - 2, 18, exitWidth, 14);
        }
        
    } while (u8g2.nextPage());
}

/**
 * Draw price edit screen
 */
void drawPriceEditScreen() {
    u8g2.firstPage();
    do {
        u8g2.setFont(u8g2_font_7x13B_tr);
        u8g2.setCursor(5, 10);
        u8g2.print("Price / kWh");
        
        // Draw editable characters
        for (int i = 0; i < 5; i++) {
            int x = 10 + i * 12;
            u8g2.setCursor(x, 30);
            u8g2.print(ui.priceChars[i]);
            
            if (i == ui.menustep) {
                u8g2.drawFrame(x - 2, 18, 12, 14);
            }
        }
        
        // Draw OK button
        int exitX = 10 + 5 * 12 + 8;
        u8g2.setCursor(exitX, 30);
        u8g2.print("OK");
        
        if (ui.menustep == 5) {
            int exitWidth = u8g2.getStrWidth("OK") + 4;
            u8g2.drawFrame(exitX - 2, 18, exitWidth, 14);
        }
        
    } while (u8g2.nextPage());
}

/**
 * Draw time edit screen
 */
void drawTimeEditScreen() {
    u8g2.firstPage();
    do {
        u8g2.setFont(u8g2_font_7x13B_tr);
        u8g2.setCursor(5, 10);
        u8g2.print("Set Time (24h)");
        
        // Draw time characters (HH:MM)
        for (int i = 0; i < 5; i++) {
            int x = 15 + i * 12;
            
            if (i == 2) {
                u8g2.setCursor(x, 30);
                u8g2.print(":");
                continue;
            }
            
            u8g2.setCursor(x, 30);
            u8g2.print(timeData.timeChars[i]);
            
            if (i == ui.menustep) {
                u8g2.drawFrame(x - 2, 18, 12, 14);
            }
        }
        
        // Draw OK button
        if (ui.menustep == 5) {
            u8g2.drawFrame(90, 20, 30, 16);
        }
        u8g2.setCursor(94, 32);
        u8g2.print("OK");
        
    } while (u8g2.nextPage());
}
#endif // OLED

#ifdef MQTT
/**
 * Handle MQTT connection and data publishing
 */
void handleMQTT() {
    checkMQTTConnection();
    if (mqttClient.connected()) {
        publishSensorData();
    }
}

/**
 * Check and maintain MQTT connection
 */
void checkMQTTConnection() {
    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if (now - timing.lastMqttAttempt > timing.MQTT_RETRY_INTERVAL) {
            timing.lastMqttAttempt = now;
            
            String clientId = "PowerMeter-" + WiFi.macAddress();
            
            Serial.print("Attempting MQTT connection to ");
            Serial.print(MQTT_SERVER);
            Serial.print(":");
            Serial.println(MQTT_PORT);
            
            if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
                Serial.println("MQTT connected successfully!");
                publishDiscoveryMessages();
                Serial.println("Discovery messages sent!");
            } else {
                Serial.print("MQTT failed, rc=");
                Serial.print(mqttClient.state());
                Serial.println(" - retrying in 5 seconds");
            }
        }
    } else {
        mqttClient.loop();
    }
}

/**
 * Publish sensor data to MQTT topics
 */
void publishSensorData() {
    String baseTopic = "powermeter/" + WiFi.macAddress();
    
    // Calculate Home Assistant compatible values
    float soc = sensorData.capacity;
    float chargingWatts = (sensorData.currentDirection == 2) ? abs(sensorData.watts) : 0;
    float dischargingWatts = (sensorData.currentDirection == 1) ? abs(sensorData.watts) : 0;
    float batteryCurrent = (sensorData.currentDirection == 1) ? -sensorData.current : sensorData.current;
    
    // Publish sensor values
    mqttClient.publish((baseTopic + "/capacity").c_str(), 
                      String(soc, 1).c_str(), true);
    mqttClient.publish((baseTopic + "/voltage").c_str(), 
                      String(sensorData.voltage, 2).c_str(), true);
    mqttClient.publish((baseTopic + "/charging_watts").c_str(), 
                      String(chargingWatts, 1).c_str(), true);
    mqttClient.publish((baseTopic + "/discharging_watts").c_str(), 
                      String(dischargingWatts, 1).c_str(), true);
    mqttClient.publish((baseTopic + "/battery_current").c_str(), 
                      String(batteryCurrent, 3).c_str(), true);
    mqttClient.publish((baseTopic + "/status").c_str(), "online", true);
    
    // Debug output every 10 seconds
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 10000) {
        lastDebug = millis();
        Serial.println("Published MQTT sensor data");
    }
}

/**
 * Publish Home Assistant MQTT discovery messages
 */
void publishDiscoveryMessages() {
    String deviceId = WiFi.macAddress();
    deviceId.replace(":", "");
    String baseTopic = "powermeter/" + WiFi.macAddress();
    
    Serial.println("Publishing MQTT discovery messages...");
    
    // Publish discovery for each sensor
    publishDiscoveryEntity("capacity", "%", "battery", "mdi:battery", "", baseTopic);
    publishDiscoveryEntity("voltage", "V", "voltage", "mdi:flash", "", baseTopic);
    publishDiscoveryEntity("charging_watts", "W", "power", "mdi:battery-charging", "", baseTopic);
    publishDiscoveryEntity("discharging_watts", "W", "power", "mdi:battery-minus", "", baseTopic);
    publishDiscoveryEntity("battery_current", "A", "current", "mdi:current-dc", "", baseTopic);
}

/**
 * Publish individual MQTT discovery entity
 */
void publishDiscoveryEntity(const char* name, const char* unit, const char* deviceClass, 
                           const char* icon, const String& deviceInfo, const String& baseTopic) {
    String deviceId = WiFi.macAddress();
    deviceId.replace(":", "");
    String discoveryTopic = "homeassistant/sensor/" + deviceId + "_" + name + "/config";
    
    // Build JSON payload
    String payload = "{";
    payload += "\"name\":\"" + String(name) + "\",";
    payload += "\"stat_t\":\"" + baseTopic + "/" + name + "\",";
    payload += "\"unit_of_meas\":\"" + String(unit) + "\",";
    payload += "\"dev_cla\":\"" + String(deviceClass) + "\",";
    payload += "\"ic\":\"" + String(icon) + "\",";
    payload += "\"stat_cla\":\"measurement\",";
    payload += "\"uniq_id\":\"" + deviceId + "_" + name + "\",";
    payload += "\"dev\":{";
    payload += "\"ids\":[\"" + deviceId + "\"],";
    payload += "\"mf\":\"skaman82\",";
    payload += "\"mdl\":\"ESP PowerMeter\",";
    payload += "\"name\":\"" + String(AP_SSID) + "\"";
    payload += "}}";
    
    bool result = mqttClient.publish(discoveryTopic.c_str(), payload.c_str(), true);
    Serial.println("Discovery for " + String(name) + ": " + (result ? "SUCCESS" : "FAILED"));
    
    delay(200); // Delay between publications
}
#endif // MQTT