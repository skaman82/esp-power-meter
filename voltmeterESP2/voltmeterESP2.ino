/**
 * ESP32 Power Meter - Battery Monitor System
 * Hardware: DFRobot Beetle ESP32-C3 @ 80MHz with INA226 current sensor
 * 
 * Features:
 * - Battery monitoring with INA226 sensor
 * - OLED display with multiple screens
 * - Web interface with real-time data
 * - MQTT integration for Home Assistant
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

// ===== FEATURE TOGGLES =====
#define OLED                  // Enable OLED Display 
#define MQTT                  // Enable MQTT for Home Assistant
#define WEBSERVER             // Enable Web UI

// ===== DEVICE CONFIGURATION =====
struct DeviceConfig {
    // Power consumption values for accurate energy calculation
    float deviceCurrent = 0.017f;        // 17 mA in normal mode
    float deviceCurrentWifi = 0.050f;    // 50 mA in WiFi Mode
    
    // Battery configuration
    byte batteryType = 0;                // 0:LiIon; 1:LiPo; 2:LiFePO4
    int cellcount = 4;                   // Battery cell count (1-6)
    float batteryCapacityAh = 50.0f;     // Full battery capacity in Ah
    
    // UI configuration
    byte orientation = 1;                // Screen orientation (0-3)
    byte screen = 0;                     // Default start screen (0-2)
    byte wifiEnabled = 1;                // WiFi active or not
    
    // Energy pricing
    float pricePerKWh = 0.305f;          // Price per kWh for cost calculation
};

// ===== NETWORK CONFIGURATION =====
const char* WIFI_SSID = "airport";
const char* WIFI_PASSWORD = "xxx";
const char* AP_PASSWORD = "12345678";

#ifdef MQTT
const char* MQTT_SERVER = "homeassistant.local";
const int MQTT_PORT = 1883;
const char* MQTT_USER = "mqtt_user";
const char* MQTT_PASS = "mqtt_password";
#endif

// ===== GLOBAL OBJECTS =====
DeviceConfig config;
Preferences preferences;
char AP_SSID[32];

#ifdef WEBSERVER
AsyncWebServer server(80);
#endif

// ===== PIN DEFINITIONS =====
#define BUTTON1_PIN 1
#define BUTTON2_PIN 2
#define I2C_ADDRESS 0x40

// ===== HARDWARE OBJECTS =====
#ifdef OLED
#include <U8g2lib.h>
// Uncomment the right OLED for your setup
//U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
//U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
//U8G2_SH1107_PIMORONI_128X128_1_HW_I2C u8g2(U8G2_R0, /* reset=*/8);
U8G2_SH1107_SEEED_128X128_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
#endif

#ifdef MQTT
#include <PubSubClient.h>
WiFiClient espClient;
PubSubClient mqttClient(espClient);
#endif

INA226_WE ina226 = INA226_WE(I2C_ADDRESS);

// ===== TIMING CONTROL STRUCTURE =====
struct TimingControl {
    unsigned long previousMillis = 0;
    unsigned long lastSensorMillis = 0;
    unsigned long lastUpdateMicros = 0;
    unsigned long previousMicros = 0;
    unsigned long lastEnergySecondMillis = 0;
    unsigned long hourStartMillis = 0;
    unsigned long lastCapacityMinuteMillis = 0;
    unsigned long lastAmpSecondMillis = 0;
    
    #ifdef MQTT
    unsigned long lastMqttAttempt = 0;
    const unsigned long MQTT_RETRY_INTERVAL = 5000;    // 5 seconds
    #endif
    
    // Timing constants
    const unsigned long SENSOR_INTERVAL = 500;         // 500ms for display updates
    const unsigned long INTERVAL_MICROS = 1000000;     // 1 second in microseconds for energy calc
};
TimingControl timing;

// ===== SENSOR DATA STRUCTURE =====
struct SensorData {
    // Raw sensor values
    float voltage = 0.0f;
    float current = 0.0f;
    float cellVoltage = 0.0f;
    float capacity = 0.0f;
    int watts = 0;
    int mAmpere = 0;
    
    // Battery state
    float remainingCapacityAh = 0.0f;
    int remainingCapacitymAh = 0;
    byte currentDirection = 0;              // 0=idle, 1=discharging, 2=charging
    float selfConsumption = 0.0f;
    
    // Maximum values tracking
    int maxWatts = 0;
    float maxCurrent = 0.0f;
    float maxCurrentMin = 0.0f;
    
    // Energy tracking
    float totalWh = 0.0f;
    float totalKWh = 0.0f;
    float totalPrice = 0.0f;
    float totalUsedmAh = 0.0f;
    float accumulatedWh = 0.0f;
    
    // Runtime estimation
    float runtimeHours = 0.0f;
    int hours = 0;
    int minutes = 0;
};
SensorData sensorData;

// ===== UI CONTROL STRUCTURE =====
struct UIControl {
    // Menu navigation
    byte menu = 0;
    byte submenu = 0;
    byte menustep = 0;
    byte pressedButton = 0;
    
    // Button state tracking
    bool button1State = false;
    bool button2State = false;
    unsigned long button1PressTime = 0;
    unsigned long button2PressTime = 0;
    bool button1Handled = false;
    bool button2Handled = false;
    
    const unsigned long LONG_PRESS_TIME = 500;     // milliseconds
    
    // Edit mode buffers
    char batteryChars[7] = "000.00";               // Battery capacity edit buffer
    char priceChars[6] = "0.000";                  // Price edit buffer
    
    // Status flags
    byte saved = 0;
    byte reset = 0;
    bool statsSaved = true;
};
UIControl ui;

// ===== EEPROM MEMORY ADDRESSES =====
struct EEPROMAddresses {
    static const int TYPE = 0;                     // Battery type
    static const int CELL = 1;                     // Cell count
    static const int CAPACITY = 2;                 // Battery capacity (float)
    static const int PRICE = 6;                    // Price per kWh (float)
    static const int REMAINING_CAP = 10;           // Remaining capacity (float)
    static const int KWH = 14;                     // Total kWh produced (float)
    static const int MODE = 18;                    // Operating mode
    static const int WIFI = 19;                    // WiFi enabled flag
    static const int SCREEN = 30;                  // Default screen
    static const int ORIENTATION = 31;             // Screen orientation
    static const int KWH_HISTORY = 32;             // Historical kWh data
};

// ===== DATA HISTORY ARRAYS =====
float hourlyKWh[12] = {0};                        // Last 12 hours kWh values
float historyCapacity[72] = {0};                  // 72 × 10min = 720min = 12h capacity history
float historyVoltage[72] = {0};                   // 72 × 10min voltage history  
float last60Amps[60] = {0};                      // Last 60 seconds of current readings
int capacityIndex = 0;

// ===== BATTERY DISCHARGE CURVES =====
// Li-ion battery discharge curve (typical 18650 cell)
const int LIION_POINTS = 11;
const float liionVoltage[LIION_POINTS]  = {4.20, 4.10, 4.00, 3.90, 3.80, 3.70, 3.60, 3.50, 3.40, 3.30, 3.20};
const float liionCapacity[LIION_POINTS] = {100,   90,   80,   70,   60,   50,   40,   30,   20,   10,    0};

// LiPo battery discharge curve (similar to Li-ion, slightly different tail)
const int LIPO_POINTS = 11;
const float lipoVoltage[LIPO_POINTS]  = {4.20, 4.15, 4.05, 3.95, 3.85, 3.75, 3.65, 3.55, 3.45, 3.35, 3.30};
const float lipoCapacity[LIPO_POINTS] = {100,   95,   85,   75,   65,   55,   45,   35,   20,   10,    0};

// LiFePO4 battery discharge curve (very flat plateau characteristic)
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
void updateSensorData();
void handleButtons();
void handleEnergyLogging();
void saveConfigToEEPROM();

// Battery curve functions
float estimateCapacity(float voltage, const float* volts, const float* caps, int size);
const float* getBatteryVoltageArray();
const float* getBatteryCapacityArray();
int getBatteryPointsCount();

// Utility functions
void disableWiFiServer();
void formatPriceToChars(float value);
String buildSensorDataJSON();

// OLED display functions
#ifdef OLED
void updateDisplay();
void drawBatteryScreen();
void drawPowerScreen();
void drawStatsScreen();
void drawMenuScreen();
void drawCapacityEditScreen();
void drawPriceEditScreen();
void handleButtonInput();
void setDisplayOrientation();
void drawPageIndicator(int currentPage);
void drawWiFiIcon();
void drawBatteryIcon();
#endif

// MQTT functions
#ifdef MQTT
void initializeMQTT();
void handleMQTT();
void checkMQTTConnection();
void publishSensorData();
void publishDiscoveryMessages();
void publishDiscoveryEntity(const char* name, const char* unit, const char* deviceClass, 
                           const char* icon, const String& baseTopic);
#endif

// Button handling
void handleButton(int pin, bool &state, unsigned long &pressTime, bool &handled, int buttonValue);

// ===== MAIN SETUP FUNCTION =====
/**
 * System initialization - called once at startup
 * Initializes all hardware components and subsystems in proper order
 */
void setup() {
    Serial.begin(9600);
    Serial.println("ESP32 Power Meter Starting...");
    
    // Initialize all subsystems
    initializeSystem();
    
    Serial.println("System initialization complete!");
    Serial.println("SSID: " + String(AP_SSID));
    Serial.println("Ready for operation.");
}

// ===== MAIN LOOP =====
/**
 * Main program loop - runs continuously
 * Handles time-critical operations and periodic tasks
 */
void loop() {
    // Handle button presses (time-critical)
    handleButtons();
    
    // Update sensor data and display at regular intervals
    if (millis() - timing.lastSensorMillis >= timing.SENSOR_INTERVAL) {
        timing.lastSensorMillis = millis();
        updateSensorData();
        handleEnergyLogging();
    }
    
    #ifdef OLED
    handleButtonInput();
    updateDisplay();
    #endif
    
    #ifdef MQTT
    if (config.wifiEnabled) {
        handleMQTT();
    }
    #endif
}

// ===== SYSTEM INITIALIZATION FUNCTIONS =====

/**
 * Initialize all system components in proper sequence
 * Sets up hardware, loads configuration, and starts services
 */
void initializeSystem() {
    // Setup GPIO pins
    pinMode(BUTTON1_PIN, INPUT_PULLUP);
    pinMode(BUTTON2_PIN, INPUT_PULLUP);
    
    // Initialize subsystems in order
    initializeSSID();
    initializeHardware();
    
    #ifdef OLED
    initializeEEPROM();
    #endif
    
    // Configure self-consumption based on WiFi status
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
    initializeDataArrays();
    
    // Start energy tracking
    timing.hourStartMillis = millis();
}

/**
 * Generate unique SSID for access point mode
 * Uses hardware RNG to create unique identifier
 */
void initializeSSID() {
    preferences.begin("esp32meter", false);
    
    if (!preferences.isKey("ssid")) {
        // Generate and store a new 3-digit suffix using hardware RNG
        randomSeed(esp_random());
        int suffix = random(100, 1000);
        snprintf(AP_SSID, sizeof(AP_SSID), "ESP32-Meter %d", suffix);
        preferences.putString("ssid", AP_SSID);
    } else {
        // Load previously saved SSID
        String storedSSID = preferences.getString("ssid", "ESP32-Meter");
        storedSSID.toCharArray(AP_SSID, sizeof(AP_SSID));
    }
    
    preferences.end();
}

/**
 * Initialize all hardware components (I2C, INA226, OLED)
 * Configures sensors for optimal performance
 */
void initializeHardware() {
    // Initialize I2C bus
    Wire.begin();
    
    // Initialize INA226 current sensor
    if (!ina226.init()) {
        Serial.println("ERROR: INA226 initialization failed!");
        return;
    }
    
    // Configure INA226 for optimal accuracy vs speed trade-off
    ina226.setAverage(AVERAGE_64);                    // Average 64 samples for noise reduction
    ina226.setMeasureMode(CONTINUOUS);                // Continuous measurement mode
    
    // Shunt resistor configuration - adjust based on your hardware
    // R002 = 2mΩ resistor, max ~41A measurement range
    // R004 = 4mΩ resistor, max ~20A measurement range  
    // R001 = 1mΩ resistor, max ~82A measurement range
    ina226.setResistorRange(0.00198, 20);            // 2mΩ shunt, 20A max range
    
    ina226.waitUntilConversionCompleted();
    Serial.println("INA226 sensor initialized successfully");
    
    #ifdef OLED
    // Initialize OLED display
    u8g2.begin();
    setDisplayOrientation();
    Serial.println("OLED display initialized");
    #endif
}

/**
 * Initialize data arrays with default values
 * Clears history and sets up tracking arrays
 */
void initializeDataArrays() {
    // Clear all history arrays
    memset(last60Amps, 0, sizeof(last60Amps));
    memset(historyCapacity, 0, sizeof(historyCapacity));
    memset(historyVoltage, 0, sizeof(historyVoltage));
    memset(hourlyKWh, 0, sizeof(hourlyKWh));
    
    Serial.println("Data arrays initialized");
}

#ifdef OLED
/**
 * Set display orientation based on configuration
 * Rotates display and clears screen buffer
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
 * Load configuration from EEPROM and validate all values
 * Handles corrupted or out-of-range values gracefully
 */
void initializeEEPROM() {
    EEPROM.begin(512);
    Serial.println("Loading configuration from EEPROM...");
    
    // Load and validate battery configuration
    config.batteryType = EEPROM.read(EEPROMAddresses::TYPE);
    if (config.batteryType > 2) {
        config.batteryType = 0;  // Default to Li-ion
    }
    
    config.cellcount = EEPROM.read(EEPROMAddresses::CELL);
    if (config.cellcount == 0 || config.cellcount > 6) {
        config.cellcount = 1;    // Default to 1S
    }
    
    // Load and validate float values with NaN checking
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
    
    // Load sensor data with validation
    EEPROM.get(EEPROMAddresses::REMAINING_CAP, sensorData.remainingCapacityAh);
    if (isnan(sensorData.remainingCapacityAh) || sensorData.remainingCapacityAh < 0 || sensorData.remainingCapacityAh > 1000) {
        sensorData.remainingCapacityAh = config.batteryCapacityAh;  // Default to full
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
}

/**
 * Initialize WiFi in Access Point mode or Station mode
 * Provides fallback if connection to existing network fails
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
 * Sets up connection parameters and buffer sizes
 */
void initializeMQTT() {
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setKeepAlive(60);
    mqttClient.setBufferSize(512);
    Serial.println("MQTT client initialized");
}
#endif

#ifdef WEBSERVER
/**
 * Initialize web server and set up API endpoints
 * Serves static files and provides real-time data API
 */
void initializeWebServer() {
    // Initialize LittleFS for serving web files
    if (!LittleFS.begin(true)) {
        Serial.println("Failed to mount LittleFS");
        return;
    }
    
    // Serve static files from LittleFS
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    
    // API endpoint for real-time sensor data
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
    
    // Handle 404 errors
    server.onNotFound([](AsyncWebServerRequest *request) {
        Serial.println("404 Not Found: " + request->url());
        request->send(404, "text/plain", "404: Not Found");
    });
    
    server.begin();
    Serial.println("Web server started successfully");
}
#endif

// ===== SENSOR DATA PROCESSING =====

/**
 * Read and process sensor data from INA226
 * Performs current direction detection, energy calculations, and capacity estimation
 */
void updateSensorData() {
    static const float DEADBAND_THRESHOLD = 0.01f; // 10 mA deadband to eliminate noise
    
    // Read raw values from INA226 sensor
    ina226.readAndClearFlags();
    sensorData.voltage = ina226.getBusVoltage_V();
    sensorData.current = ina226.getCurrent_A();
    sensorData.mAmpere = ina226.getCurrent_mA();
    
    // Determine current direction and normalize values
    // INA226 reports negative current during charging (depending on shunt orientation)
    if (sensorData.current < -DEADBAND_THRESHOLD) {
        sensorData.currentDirection = 2;        // Charging
        sensorData.current = -sensorData.current;
        sensorData.mAmpere = -sensorData.mAmpere;
    } else if (sensorData.current > DEADBAND_THRESHOLD) {
        sensorData.currentDirection = 1;        // Discharging
    } else {
        sensorData.currentDirection = 0;        // Idle/no significant current
        sensorData.current = 0;
        sensorData.mAmpere = 0;
    }
    
    // Calculate power (watts)
    sensorData.watts = sensorData.voltage * sensorData.current;
    if (sensorData.currentDirection == 2) {
        sensorData.watts = -sensorData.watts;   // Negative watts for charging
    }
    
    // Track maximum values since startup
    if (sensorData.current > sensorData.maxCurrent) {
        sensorData.maxCurrent = sensorData.current;
    }
    if (abs(sensorData.watts) > sensorData.maxWatts) {
        sensorData.maxWatts = abs(sensorData.watts);
    }
    
    // Perform energy calculations at precise 1-second intervals
    performEnergyCalculations();
    
    // Calculate cell voltage and battery capacity percentage
    sensorData.cellVoltage = sensorData.voltage / config.cellcount;
    sensorData.capacity = estimateCapacity(sensorData.cellVoltage, 
                                           getBatteryVoltageArray(), 
                                           getBatteryCapacityArray(), 
                                           getBatteryPointsCount());
    
    // Constrain capacity to valid range
    sensorData.capacity = constrain(sensorData.capacity, 0.0f, 100.0f);
    
    // Calculate runtime estimate
    calculateRuntimeEstimate();
}

/**
 * Perform precise energy calculations using microsecond timing
 * Updates battery capacity and total energy production
 */
void performEnergyCalculations() {
    unsigned long currentMicros = micros();
    
    // Perform calculations every second (1,000,000 microseconds)
    if (currentMicros - timing.previousMicros >= timing.INTERVAL_MICROS) {
        timing.previousMicros = currentMicros;
        
        // Apply deadband to small currents
        if (abs(sensorData.mAmpere) <= 4) {
            sensorData.mAmpere = 0;
        }
        
        // Calculate energy consumption/production with self-consumption correction
        float realCurrent = sensorData.current + sensorData.selfConsumption;
        float usedmAh = (realCurrent * 1000.0f) / 3600.0f; // mAh per second
        
        // Update remaining battery capacity
        if (sensorData.currentDirection == 1) {        // Discharging
            sensorData.remainingCapacityAh -= usedmAh / 1000.0f;
            sensorData.totalUsedmAh -= usedmAh;
        } else if (sensorData.currentDirection == 2) {  // Charging
            sensorData.remainingCapacityAh += usedmAh / 1000.0f;
            sensorData.totalUsedmAh += usedmAh;
        }
        
        // Constrain remaining capacity to valid range
        sensorData.remainingCapacityAh = constrain(sensorData.remainingCapacityAh, 
                                                   0.0f, config.batteryCapacityAh);
        sensorData.remainingCapacitymAh = sensorData.remainingCapacityAh * 1000.0f;
        
        // Auto-calibrate capacity based on voltage (after 5 second startup delay)
        if (millis() > 5000) {
            if (sensorData.capacity >= 99.9f) {
                sensorData.remainingCapacityAh = config.batteryCapacityAh;
            } else if (sensorData.capacity <= 0.5f) {
                sensorData.remainingCapacityAh = 0;
            }
        }
        
        // Calculate total energy production (only during charging)
        if (sensorData.currentDirection == 2) {
            float deltaTimeHours = (currentMicros - timing.lastUpdateMicros) / 3600000000.0f;
            sensorData.totalWh += abs(sensorData.watts) * deltaTimeHours;
            sensorData.totalKWh = sensorData.totalWh / 1000.0f;
            sensorData.totalPrice = sensorData.totalKWh * config.pricePerKWh;
        }
        timing.lastUpdateMicros = currentMicros;
    }
}

/**
 * Calculate estimated runtime based on current flow direction and rate
 * Estimates time until battery empty (discharge) or full (charge)
 */
void calculateRuntimeEstimate() {
    if (sensorData.currentDirection == 1 && sensorData.current > 0) {
        // Discharging - calculate time until empty
        sensorData.runtimeHours = sensorData.remainingCapacityAh / sensorData.current;
    } else if (sensorData.currentDirection == 2 && sensorData.current > 0) {
        // Charging - calculate time until full
        float remainingToCharge = config.batteryCapacityAh - sensorData.remainingCapacityAh;
        sensorData.runtimeHours = remainingToCharge / sensorData.current;
    } else {
        sensorData.runtimeHours = 0;  // No runtime estimate for idle state
    }
    
    // Convert to hours and minutes for display
    sensorData.hours = static_cast<int>(sensorData.runtimeHours);
    sensorData.minutes = static_cast<int>((sensorData.runtimeHours - sensorData.hours) * 60);
}

// ===== BATTERY CURVE FUNCTIONS =====

/**
 * Get appropriate battery voltage curve based on configured battery type
 * Returns pointer to voltage array for lookup table
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
 * Get appropriate battery capacity curve based on configured battery type
 * Returns pointer to capacity percentage array for lookup table
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
 * Get number of points in battery curve based on configured battery type
 * Returns array size for boundary checking
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
 * Estimate battery capacity percentage based on cell voltage using linear interpolation
 * Uses lookup tables specific to each battery chemistry for accurate estimation
 * 
 * @param voltage Cell voltage to lookup
 * @param volts Voltage lookup table
 * @param caps Capacity lookup table  
 * @param size Number of points in lookup table
 * @return Estimated capacity percentage (0-100%)
 */
float estimateCapacity(float voltage, const float* volts, const float* caps, int size) {
    // Handle edge cases
    if (voltage >= volts[0]) return 100.0f;     // Above maximum voltage
    if (voltage <= volts[size - 1]) return 0.0f; // Below minimum voltage

    // Find the two points that bracket our voltage and interpolate
    for (int i = 0; i < size - 1; i++) {
        if (voltage <= volts[i] && voltage >= volts[i + 1]) {
            float v1 = volts[i];
            float v2 = volts[i + 1];
            float c1 = caps[i];
            float c2 = caps[i + 1];
            
            // Linear interpolation between the two points
            return c1 + ((voltage - v1) * (c2 - c1) / (v2 - v1));
        }
    }
    return 0.0f; // Fallback
}

// ===== ENERGY LOGGING AND HISTORY =====

/**
 * Handle energy logging and historical data tracking
 * Updates hourly energy production and 10-minute capacity/voltage history
 */
void handleEnergyLogging() {
    // Update 60-second current history every second
    if (millis() - timing.lastAmpSecondMillis >= 1000) {
        timing.lastAmpSecondMillis = millis();
        
        // Shift array left and add new current reading
        for (int i = 0; i < 59; i++) {
            last60Amps[i] = last60Amps[i + 1];
        }
        last60Amps[59] = sensorData.current;
        
        // Calculate maximum current in last 60 seconds
        sensorData.maxCurrentMin = 0.0f;
        for (int i = 0; i < 60; i++) {
            if (last60Amps[i] > sensorData.maxCurrentMin) {
                sensorData.maxCurrentMin = last60Amps[i];
            }
        }
    }
    
    // Accumulate energy per second (only during charging)
    if (millis() - timing.lastEnergySecondMillis >= 1000) {
        timing.lastEnergySecondMillis = millis();
        
        if (sensorData.currentDirection == 2) {
            sensorData.accumulatedWh += abs(sensorData.watts) / 3600.0; // Convert W⋅s to Wh
        }
    }
    
    // Log hourly energy production
    if (millis() - timing.hourStartMillis >= 3600000UL) { // 1 hour
        timing.hourStartMillis += 3600000UL;
        
        // Shift hourly array left (drop oldest hour)
        for (int i = 0; i < 11; i++) {
            hourlyKWh[i] = hourlyKWh[i + 1];
        }
        
        // Store accumulated charging energy for past hour
        hourlyKWh[11] = sensorData.accumulatedWh / 1000.0; // Convert Wh to kWh
        sensorData.accumulatedWh = 0; // Reset for next hour
    }
    
    // Log capacity and voltage every 10 minutes
    if (millis() - timing.lastCapacityMinuteMillis >= 600000UL) { // 10 minutes
        timing.lastCapacityMinuteMillis += 600000UL;
        
        // Shift capacity history array left
        for (int i = 0; i < 71; i++) {
            historyCapacity[i] = historyCapacity[i + 1];
        }
        historyCapacity[71] = sensorData.capacity;
        
        // Shift voltage history array left
        for (int i = 0; i < 71; i++) {
            historyVoltage[i] = historyVoltage[i + 1];
        }
        historyVoltage[71] = sensorData.voltage;
    }
}

// ===== BUTTON HANDLING =====

/**
 * Handle button press detection for both buttons
 * Calls individual button handler for each button
 */
void handleButtons() {
    handleButton(BUTTON1_PIN, ui.button1State, ui.button1PressTime, ui.button1Handled, 1);
    handleButton(BUTTON2_PIN, ui.button2State, ui.button2PressTime, ui.button2Handled, 2);
}

/**
 * Generic button handler with debouncing and long press detection
 * Detects press/release events and calculates press duration
 * 
 * @param pin GPIO pin number
 * @param state Button state tracking variable
 * @param pressTime Press start time tracking variable  
 * @param handled Press handled flag
 * @param buttonValue Button identifier (1 or 2)
 */
void handleButton(int pin, bool &state, unsigned long &pressTime, bool &handled, int buttonValue) {
    bool isPressed = digitalRead(pin) == LOW; // Active low (INPUT_PULLUP)

    if (isPressed && !state) {
        // Button just pressed - record press start time
        state = true;
        pressTime = millis();
        handled = false;
    }

    if (!isPressed && state) {
        // Button just released - determine press type
        state = false;
        unsigned long duration = millis() - pressTime;

        if (duration >= ui.LONG_PRESS_TIME) {
            ui.pressedButton = buttonValue * 11; // Long press: 1→11, 2→22
        } else {
            ui.pressedButton = buttonValue;      // Short press: 1 or 2
        }
        handled = true;
    }
}

// ===== CONFIGURATION MANAGEMENT =====

/**
 * Save current configuration to EEPROM
 * Writes all configuration parameters and current state to persistent storage
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
 * Gracefully shuts down network services and updates configuration
 */
void disableWiFiServer() {
    #ifdef WEBSERVER
    server.end();
    #endif
    
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    config.wifiEnabled = 0;
    
    // Save WiFi state and current statistics
    EEPROM.put(EEPROMAddresses::WIFI, config.wifiEnabled);
    EEPROM.put(EEPROMAddresses::REMAINING_CAP, sensorData.remainingCapacityAh);
    EEPROM.put(EEPROMAddresses::KWH, sensorData.totalKWh);
    EEPROM.commit();
    
    delay(500);
    Serial.println("WiFi disabled - power saving mode active");
}

/**
 * Format price value for display with comma as decimal separator
 * Converts float to string with European number format
 * 
 * @param value Price value to format
 */
void formatPriceToChars(float value) {
    char temp[7];
    snprintf(temp, sizeof(temp), "%1.3f", value);  // Format as "0.356"
    
    // Replace decimal point with comma
    for (int i = 0; i < 6; i++) {
        ui.priceChars[i] = (temp[i] == '.') ? ',' : temp[i];
    }
    ui.priceChars[5] = '\0';
}

#ifdef WEBSERVER
/**
 * Build comprehensive JSON string with all sensor data for web interface
 * Creates real-time data feed for web dashboard
 * 
 * @return JSON string containing all sensor readings and history
 */
String buildSensorDataJSON() {
    String json = "{";
    
    // Basic sensor data
    json += "\"batteryType\":" + String(config.batteryType) + ",";
    json += "\"cellcount\":" + String(config.cellcount) + ",";
    json += "\"watts\":" + String(sensorData.watts) + ",";
    json += "\"Ampere\":" + String(isnan(sensorData.current) ? 0.0 : sensorData.current, 2) + ",";
    json += "\"cur_dir\":" + String(sensorData.currentDirection) + ",";
    json += "\"voltage\":" + String(isnan(sensorData.voltage) ? 0.0 : sensorData.voltage, 2) + ",";
    json += "\"capacity\":" + String(isnan(sensorData.capacity) ? 0.0 : sensorData.capacity, 2) + ",";
    json += "\"batteryCapacityAh\":" + String(isnan(config.batteryCapacityAh) ? 0.0 : config.batteryCapacityAh, 2) + ",";
    json += "\"remainingCapacityAh\":" + String(isnan(sensorData.remainingCapacityAh) ? 0.0 : sensorData.remainingCapacityAh, 2) + ",";
    json += "\"pricePerKWh\":" + String(isnan(config.pricePerKWh) ? 0.0 : config.pricePerKWh, 3) + ",";
    json += "\"totalWh\":" + String(isnan(sensorData.totalWh) ? 0.0 : sensorData.totalWh, 2) + ",";
    json += "\"totalKWh\":" + String(isnan(sensorData.totalKWh) ? 0.0 : sensorData.totalKWh, 3) + ",";

    // Dynamic energy display unit
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

    // Hourly energy history
    json += ",\"hourlyKWh\":[";
    for (int i = 0; i < 12; i++) {
        json += String(hourlyKWh[i], 3);
        if (i < 11) json += ",";
    }
    json += "]";

    // Capacity history (10-minute intervals)
    json += ",\"historyCapacity\":[";
    for (int i = 0; i < 72; i++) {
        json += String(static_cast<int>(historyCapacity[i]));
        if (i < 71) json += ",";
    }
    json += "]";

    // Voltage history (10-minute intervals)
    json += ",\"historyVoltage\":[";
    for (int i = 0; i < 72; i++) {
        json += String(historyVoltage[i], 2);
        if (i < 71) json += ",";
    }
    json += "]";

    // Current history (1-second intervals for last 60 seconds)
    json += ",\"last60Amps\":[";
    for (int i = 0; i < 60; i++) {
        json += String(last60Amps[i], 2);
        if (i < 59) json += ",";
    }
    json += "]";

    json += "}";
    return json;
}
#endif

// ===== OLED DISPLAY FUNCTIONS =====

#ifdef OLED

    /**
    * Main display update function - routes to appropriate screen
    * Handles different menu states and screen types
    */
    void updateDisplay() {
        switch (ui.menu) {
            case 0: drawMainScreen(); break;
            case 1: drawMenuScreen(); break;
            case 2: drawCapacityEditScreen(); break;
            case 3: drawPriceEditScreen(); break;
            default: drawMainScreen(); break;
        }
    }
#endif
#ifdef OLED
    /**
    * Route to appropriate main screen based on screen selection
    * Handles three different information screens
    */
    void drawMainScreen() {
        switch (config.screen) {
            case 0: drawBatteryScreen(); break;
            case 1: drawPowerScreen(); break;
            case 2: drawStatsScreen(); break;
            default: drawBatteryScreen(); break;
        }
    }
#endif
#ifdef OLED
    /**
    * Draw battery status screen (screen 0)
    * Shows battery percentage, voltage, capacity, and charge/discharge status
    */
    void drawBatteryScreen() {
        u8g2.firstPage();
        do {
            drawPageIndicator(0);
            drawWiFiIcon();
            
            // Battery type and cell configuration
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.setCursor(0, 10);
            u8g2.print("BAT ");
            
            const char* batteryTypes[] = {"LiIon", "LiPo", "LiFePO4"};
            u8g2.print(batteryTypes[config.batteryType]);
            u8g2.print(" ");
            u8g2.print(config.cellcount);
            u8g2.println("s");
            
            // Large capacity percentage display
            u8g2.setCursor(0, 42);
            u8g2.setFont(u8g2_font_fub20_tr);
            u8g2.print(sensorData.capacity, 0);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" %");
            
            // Battery icon with capacity bar and charge/discharge indicator
            drawBatteryIcon();
            
            // Bus voltage display
            u8g2.setCursor(0, 72);
            u8g2.setFont(u8g2_font_fub20_tr);
            u8g2.print(sensorData.voltage, 1);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" V");
            
            // Remaining capacity with dynamic units
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
            
            // Current and runtime estimate
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
#endif
#ifdef OLED
    /**
    * Draw power and energy screen (screen 1)
    * Shows current power, peak power, voltage, current, and total energy
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
            
            // Current power consumption/production
            u8g2.setFont(u8g2_font_fub20_tr);
            u8g2.setCursor(0, 42);
            u8g2.print(abs(sensorData.watts));
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" W");
            
            // Peak watts since startup
            u8g2.setCursor(85, 33);
            u8g2.print(sensorData.maxWatts);
            u8g2.setCursor(85, 45);
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.println("PEAK W");
            
            // Bus voltage
            u8g2.setFont(u8g2_font_fub20_tr);
            u8g2.setCursor(0, 72);
            u8g2.print(sensorData.voltage, 1);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" V");
            
            // Current with dynamic units
            u8g2.setFont(u8g2_font_fub20_tr);
            u8g2.setCursor(0, 102);
            if (sensorData.currentDirection == 1) u8g2.print("-");
            
            if (sensorData.current > 1) {
                u8g2.print(sensorData.current, 2);
                u8g2.setFont(u8g2_font_fub11_tr);
                u8g2.println(" A");
            } else {
                u8g2.print(sensorData.mAmpere);
                u8g2.setFont(u8g2_font_fub11_tr);
                u8g2.println(" mA");
            }
            
            // Total energy produced with dynamic units
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
#endif
#ifdef OLED
    /**
    * Draw detailed statistics screen (screen 2)
    * Shows high-precision values and peak current information
    */
    void drawStatsScreen() {
        u8g2.firstPage();
        do {
            drawPageIndicator(2);
            drawWiFiIcon();
            
            // Current power information
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.setCursor(0, 10);
            u8g2.print("POWER ");
            u8g2.print(abs(sensorData.watts));
            u8g2.println(" W");
            
            // High-precision voltage
            u8g2.setFont(u8g2_font_fub20_tr);
            u8g2.setCursor(0, 42);
            u8g2.print(sensorData.voltage, 2);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" V");
            
            // Current with sign indication
            u8g2.setFont(u8g2_font_fub20_tr);
            u8g2.setCursor(0, 72);
            if (sensorData.currentDirection == 1) u8g2.print("-");
            
            if (sensorData.current > 1) {
                u8g2.print(sensorData.current, 2);
                u8g2.setFont(u8g2_font_fub11_tr);
                u8g2.println(" A");
            } else {
                u8g2.print(sensorData.mAmpere);
                u8g2.setFont(u8g2_font_fub11_tr);
                u8g2.println(" mA");
            }
            
            // Total used energy with dynamic scaling
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
            
            // Peak current since startup
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.setCursor(0, 123);
            u8g2.print("PEAK ");
            u8g2.print(sensorData.maxCurrent, 2);
            u8g2.print(" A");
            
        } while (u8g2.nextPage());
    }
#endif
#ifdef OLED
    /**
    * Draw page indicator dots at top of screen
    * Shows which screen is currently active
    * 
    * @param currentPage Currently active page number (0-2)
    */
    void drawPageIndicator(int currentPage) {
        for (int i = 0; i < 3; i++) {
            if (i == currentPage) {
                u8g2.drawBox(92 + i * 8, 4, 5, 5);      // Filled box for active page
            } else {
                u8g2.drawFrame(92 + i * 8, 4, 5, 5);    // Empty frame for inactive pages
            }
        }
    }
#endif
#ifdef OLED
    /**
    * Draw WiFi status icon in top right corner
    * Shows signal strength indicator when WiFi is enabled
    */
    void drawWiFiIcon() {
        if (config.wifiEnabled) {
            // Draw WiFi signal strength bars
            u8g2.drawFrame(119, 4, 6, 1);   // Weakest signal bar
            u8g2.drawFrame(120, 6, 4, 1);   // Medium signal bar
            u8g2.drawFrame(121, 8, 2, 1);   // Strongest signal bar
        }
    }
#endif
#ifdef OLED
    /**
    * Draw battery icon with capacity bar and charge/discharge arrows
    * Visual representation of battery state and current flow direction
    */
    void drawBatteryIcon() {
        // Battery terminals
        u8g2.drawFrame(89, 21, 6, 2);      // Left terminal
        u8g2.drawFrame(114, 21, 6, 2);     // Right terminal
        
        // Battery body outline
        u8g2.drawFrame(85, 24, 39, 30);
        
        // Capacity fill bar
        const byte maxHeight = 28;
        const byte bottomY = 53;
        byte capHeight = map(sensorData.capacity, 0, 100, 0, maxHeight);
        byte capY = bottomY - capHeight;
        
        u8g2.setFontMode(0);
        u8g2.setDrawColor(2);  // XOR mode for filled bar
        u8g2.drawBox(86, capY, 37, capHeight);
        
        // Charge/discharge direction arrows
        if (sensorData.currentDirection == 1) {
            // Discharge arrows pointing up (energy leaving battery)
            u8g2.drawTriangle(99, 40, 104, 34, 109, 40);
            u8g2.drawTriangle(99, 46, 104, 40, 109, 46);
        } else if (sensorData.currentDirection == 2) {
            // Charge arrows pointing down (energy entering battery)
            u8g2.drawTriangle(99, 34, 104, 40, 109, 34);
            u8g2.drawTriangle(99, 40, 104, 46, 109, 40);
        }
        
        // Status text
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.setCursor(92, 71);
        const char* statusText[] = {"IDLE", "LOAD", "CHRG"};
        u8g2.print(statusText[sensorData.currentDirection]);
    }
#endif
#ifdef OLED
    /**
    * Draw configuration menu screen
    * Shows all configurable parameters with navigation highlighting
    */
    void drawMenuScreen() {
        u8g2.firstPage();
        do {
            u8g2.setFont(u8g2_font_7x13B_tr);
            u8g2.setFontMode(0);
            u8g2.setDrawColor(2);
            
            if (ui.menustep <= 5) {
                // Page 1 of menu - basic configuration
                drawMenuItem(0, "Battery", getBatteryTypeString());
                drawMenuItem(1, "Cell Count", String(config.cellcount) + "s");
                drawMenuItem(2, "Capacity", String(config.batteryCapacityAh, 1) + " Ah");
                drawMenuItem(3, "Est c Cap", String(sensorData.remainingCapacityAh, 1) + " Ah");
                drawMenuItem(4, "Capacity to 0", "");
                drawMenuItem(5, "Capacity to full", "");
            } else {
                // Page 2 of menu - advanced settings
                drawMenuItem(6, "Price/kWh", String(config.pricePerKWh, 3));
                drawMenuItem(7, "Reset Stats", ui.reset ? "OK" : "");
                drawMenuItem(8, "Save & Exit", "");
                
                // Show network information
                u8g2.setFont(u8g2_font_6x10_tr);
                u8g2.setCursor(5, 110);
                u8g2.print("SSID ");
                u8g2.print(AP_SSID);
                u8g2.setCursor(5, 124);
                u8g2.print("IP ");
                if (WiFi.status() == WL_CONNECTED) {
                    u8g2.print(WiFi.localIP());
                } else {
                    u8g2.print("192.168.4.1");
                }
                #ifdef MQTT
                u8g2.print(" MQTT");
                #endif
                
                #endif
            }
            
            // Highlight selected menu item
            int displayStep = ui.menustep > 5 ? ui.menustep - 6 : ui.menustep;
            u8g2.drawBox(0, displayStep * 21, 128, 20);
            
        } while (u8g2.nextPage());
    }
#endif
#ifdef OLED
    /**
    * Draw individual menu item with title and optional value
    * 
    * @param step Menu item index
    * @param title Menu item title text
    * @param value Optional value text to display
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
    
#endif
#ifdef OLED
    /**
    * Get battery type as human-readable string
    * 
    * @return Battery chemistry name
    */
    String getBatteryTypeString() {
        const char* types[] = {"LiIon", "LiPo", "LiFePO4"};
        return String(types[config.batteryType]);
    }
#endif
#ifdef OLED
    /**
    * Draw capacity edit screen for setting battery capacity
    * Allows digit-by-digit editing with cursor highlighting
    */
    void drawCapacityEditScreen() {
        u8g2.firstPage();
        do {
            u8g2.setFont(u8g2_font_7x13B_tr);
            u8g2.setCursor(5, 10);
            u8g2.print("Capacity in Ah");
            
            // Draw editable characters (format: XXX.XX)
            for (int i = 0; i < 6; i++) {
                int x = 10 + i * 12;
                u8g2.setCursor(x, 30);
                u8g2.print(ui.batteryChars[i]);
                
                // Highlight currently selected character
                if (i == ui.menustep) {
                    u8g2.drawFrame(x - 2, 18, 12, 14);
                }
            }
            
            // Draw OK/confirm button
            int exitX = 10 + 6 * 12 + 8;
            u8g2.setCursor(exitX, 30);
            u8g2.print("OK");
            
            if (ui.menustep == 6) {
                int exitWidth = u8g2.getStrWidth("OK") + 4;
                u8g2.drawFrame(exitX - 2, 18, exitWidth, 14);
            }
            
        } while (u8g2.nextPage());
    }
#endif
#ifdef OLED
    /**
    * Draw price edit screen for setting energy price
    * Allows editing price per kWh with European format (comma decimal)
    */
    void drawPriceEditScreen() {
        u8g2.firstPage();
        do {
            u8g2.setFont(u8g2_font_7x13B_tr);
            u8g2.setCursor(5, 10);
            u8g2.print("Price / kWh");
            
            // Draw editable characters (format: X,XXX)
            for (int i = 0; i < 5; i++) {
                int x = 10 + i * 12;
                u8g2.setCursor(x, 30);
                u8g2.print(ui.priceChars[i]);
                
                // Highlight currently selected character
                if (i == ui.menustep) {
                    u8g2.drawFrame(x - 2, 18, 12, 14);
                }
            }
            
            // Draw OK/confirm button
            int exitX = 10 + 5 * 12 + 8;
            u8g2.setCursor(exitX, 30);
            u8g2.print("OK");
            
            if (ui.menustep == 5) {
                int exitWidth = u8g2.getStrWidth("OK") + 4;
                u8g2.drawFrame(exitX - 2, 18, exitWidth, 14);
            }
            
        } while (u8g2.nextPage());
    }
#endif
#ifdef OLED
    /**
    * Handle button input for UI navigation and menu control
    * Routes button presses to appropriate handlers based on current menu state
    */
    void handleButtonInput() {
        if (ui.pressedButton == 0) return; // No button pressed
        
        switch (ui.menu) {
            case 0: handleMainScreenInput(); break;
            case 1: handleMenuScreenInput(); break;
            case 2: handleCapacityEditInput(); break;
            case 3: handlePriceEditInput(); break;
        }
        
        ui.pressedButton = 0; // Clear button state after handling
    }
#endif
#ifdef OLED
    /**
    * Handle button input on main display screens
    * Controls screen switching, orientation, and WiFi toggle
    */
    void handleMainScreenInput() {
        if (ui.pressedButton == 22) {
            // Long press button 2 - Enter configuration menu
            ui.menu = 1;
            ui.menustep = 0;
        } else if (ui.pressedButton == 11) {
            // Long press button 1 - Toggle WiFi on/off
            if (config.wifiEnabled) {
                disableWiFiServer();
            } else {
                config.wifiEnabled = 1;
                saveConfigToEEPROM();
                esp_restart(); // Restart required to enable WiFi services
            }
        } else if (ui.pressedButton == 2) {
            // Short press button 2 - Cycle through display screens
            config.screen = (config.screen + 1) % 3;
            u8g2.clear();
        } else if (ui.pressedButton == 1) {
            // Short press button 1 - Change display orientation
            config.orientation = (config.orientation + 1) % 4;
            setDisplayOrientation();
        }
    }
#endif
#ifdef OLED
    /**
    * Handle button input in configuration menu
    * Navigate menu items and select options
    */
    void handleMenuScreenInput() {
        if (ui.pressedButton == 2) {
            // Button 2 - Navigate through menu items
            ui.menustep = (ui.menustep + 1) % 9;
        } else if (ui.pressedButton == 1) {
            // Button 1 - Select/activate menu item
            handleMenuSelection();
        }
    }
#endif
#ifdef OLED
    /**
    * Handle menu item selection and execute corresponding actions
    * Processes configuration changes and special functions
    */
    void handleMenuSelection() {
        switch (ui.menustep) {
            case 0: // Change battery chemistry type
                config.batteryType = (config.batteryType + 1) % 3;
                break;
                
            case 1: // Change cell count (1S to 6S)
                config.cellcount = (config.cellcount % 6) + 1;
                break;
                
            case 2: // Edit battery capacity
                snprintf(ui.batteryChars, sizeof(ui.batteryChars), "%06.2f", config.batteryCapacityAh);
                ui.menu = 2; // Switch to capacity edit screen
                ui.menustep = 0;
                return;
                
            case 3: // Estimate current capacity from voltage
                sensorData.remainingCapacityAh = (sensorData.capacity / 100.0f) * config.batteryCapacityAh;
                break;
                
            case 4: // Set remaining capacity to 0 (empty battery)
                sensorData.remainingCapacityAh = 0;
                break;
                
            case 5: // Set remaining capacity to full
                sensorData.remainingCapacityAh = config.batteryCapacityAh;
                break;
                
            case 6: // Edit price per kWh
                formatPriceToChars(config.pricePerKWh);
                ui.menu = 3; // Switch to price edit screen
                ui.menustep = 0;
                return;
                
            case 7: // Reset all statistics
                resetStatistics();
                ui.reset = 1;
                break;
                
            case 8: // Save configuration and exit menu
                saveConfigToEEPROM();
                ui.menu = 0;
                ui.menustep = 0;
                ui.saved = 0;
                ui.reset = 0;
                break;
        }
    }
#endif
#ifdef OLED
    /**
    * Reset all statistical data and history
    * Clears energy counters and maximum value tracking
    */
    void resetStatistics() {
        sensorData.totalKWh = 0;
        sensorData.totalWh = 0;
        sensorData.totalUsedmAh = 0;
        sensorData.totalPrice = 0;
        sensorData.maxWatts = 0;
        sensorData.maxCurrent = 0;
        sensorData.maxCurrentMin = 0;
        
        // Clear all history arrays
        memset(hourlyKWh, 0, sizeof(hourlyKWh));
        memset(historyCapacity, 0, sizeof(historyCapacity));
        memset(historyVoltage, 0, sizeof(historyVoltage));
        memset(last60Amps, 0, sizeof(last60Amps));
        
        // Save reset values to EEPROM
        EEPROM.put(EEPROMAddresses::KWH, sensorData.totalKWh);
        EEPROM.commit();
        
        Serial.println("Statistics reset successfully");
    }

    /**
    * Handle capacity edit screen input
    * Allows digit-by-digit editing of battery capacity value
    */
    void handleCapacityEditInput() {
        if (ui.pressedButton == 2) {
            // Navigate to next character/position
            ui.menustep = (ui.menustep + 1) % 7;
            if (ui.menustep == 3) ui.menustep++; // Skip decimal point position
        } else if (ui.pressedButton == 1) {
            if (ui.menustep < 6) {
                // Edit selected digit (increment 0-9, wrap around)
                char &c = ui.batteryChars[ui.menustep];
                if (c >= '0' && c <= '9') {
                    c = (c == '9') ? '0' : c + 1;
                }
            } else {
                // Confirm button pressed - save new capacity value
                float newValue = atof(ui.batteryChars);
                if (newValue > 999.99f) newValue = 999.99f; // Clamp to maximum
                config.batteryCapacityAh = newValue;
                sensorData.remainingCapacityAh = config.batteryCapacityAh; // Reset to full
                
                Serial.println("Capacity updated to: " + String(config.batteryCapacityAh));
                
                ui.menu = 1; // Return to main menu
                ui.menustep = 0;
            }
        }
    }

    /**
    * Handle price edit screen input
    * Allows editing of price per kWh with European decimal format
    */
    void handlePriceEditInput() {
        if (ui.pressedButton == 2) {
            // Navigate to next character/position
            ui.menustep = (ui.menustep + 1) % 6;
            if (ui.menustep == 1) ui.menustep++; // Skip decimal comma position
        } else if (ui.pressedButton == 1) {
            if (ui.menustep < 5) {
                // Edit selected digit (increment 0-9, wrap around)
                char &c = ui.priceChars[ui.menustep];
                if (c >= '0' && c <= '9') {
                    c = (c == '9') ? '0' : c + 1;
                }
            } else {
                // Confirm button pressed - save new price value
                char tempStr[7];
                // Convert European format (comma) to standard format (point)
                for (int i = 0; i < 6; i++) {
                    tempStr[i] = (ui.priceChars[i] == ',') ? '.' : ui.priceChars[i];
                }
                
                float newValue = atof(tempStr);
                if (newValue > 9.999f) newValue = 9.999f; // Clamp to maximum
                config.pricePerKWh = newValue;
                
                // Recalculate total price with new rate
                sensorData.totalPrice = sensorData.totalKWh * config.pricePerKWh;
                
                formatPriceToChars(config.pricePerKWh); // Reformat display buffer
                
                Serial.println("Price updated to: " + String(config.pricePerKWh));
                
                ui.menu = 1; // Return to main menu
                ui.menustep = 0;
            }
        }
    }

#endif // OLED

// ===== MQTT FUNCTIONS =====

#ifdef MQTT

/**
 * Handle MQTT connection and data publishing
 * Manages connection state and publishes sensor data periodically
 */
void handleMQTT() {
    checkMQTTConnection();
    
    // Publish sensor data every second when connected
    static unsigned long lastPublish = 0;
    if (mqttClient.connected() && millis() - lastPublish >= 1000) {
        lastPublish = millis();
        publishSensorData();
    }
}

/**
 * Check and maintain MQTT connection to Home Assistant
 * Handles automatic reconnection with exponential backoff
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
                publishDiscoveryMessages(); // Send Home Assistant discovery
                Serial.println("Discovery messages published");
            } else {
                Serial.print("MQTT connection failed, rc=");
                Serial.print(mqttClient.state());
                Serial.println(" - retrying in 5 seconds");
            }
        }
    } else {
        mqttClient.loop(); // Process incoming messages and maintain connection
    }
}

/**
 * Publish sensor data to MQTT topics for Home Assistant
 * Sends all relevant sensor readings as separate topics
 */
void publishSensorData() {
    String baseTopic = "powermeter/" + WiFi.macAddress();
    
    // Calculate Home Assistant compatible values
    float soc = sensorData.capacity; // State of Charge percentage
    float chargingWatts = (sensorData.currentDirection == 2) ? abs(sensorData.watts) : 0;
    float dischargingWatts = (sensorData.currentDirection == 1) ? abs(sensorData.watts) : 0;
    float batteryCurrent = (sensorData.currentDirection == 1) ? -sensorData.current : sensorData.current;
    
    // Publish individual sensor values with retain flag for persistence
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
        Serial.println("MQTT sensor data published successfully");
    }
}

/**
 * Publish Home Assistant MQTT discovery configuration messages
 * Enables automatic device discovery and integration
 */
void publishDiscoveryMessages() {
    String deviceId = WiFi.macAddress();
    deviceId.replace(":", ""); // Remove colons for valid device ID
    String baseTopic = "powermeter/" + WiFi.macAddress();
    
    Serial.println("Publishing MQTT discovery messages...");
    
    // Publish discovery configuration for each sensor entity
    publishDiscoveryEntity("capacity", "%", "battery", "mdi:battery", baseTopic);
    publishDiscoveryEntity("voltage", "V", "voltage", "mdi:flash", baseTopic);
    publishDiscoveryEntity("charging_watts", "W", "power", "mdi:battery-charging", baseTopic);
    publishDiscoveryEntity("discharging_watts", "W", "power", "mdi:battery-minus", baseTopic);
    publishDiscoveryEntity("battery_current", "A", "current", "mdi:current-dc", baseTopic);
}

/**
 * Publish individual MQTT discovery entity configuration
 * Creates Home Assistant sensor entity with proper metadata
 * 
 * @param name Entity name/identifier
 * @param unit Unit of measurement
 * @param deviceClass Home Assistant device class
 * @param icon Material Design icon identifier
 * @param baseTopic MQTT topic prefix for this device
 */
void publishDiscoveryEntity(const char* name, const char* unit, const char* deviceClass, 
                           const char* icon, const String& baseTopic) {
    String deviceId = WiFi.macAddress();
    deviceId.replace(":", "");
    String discoveryTopic = "homeassistant/sensor/" + deviceId + "_" + name + "/config";
    
    // Build JSON configuration payload
    String payload = "{";
    payload += "\"name\":\"" + String(name) + "\",";
    payload += "\"stat_t\":\"" + baseTopic + "/" + name + "\",";
    payload += "\"unit_of_meas\":\"" + String(unit) + "\",";
    payload += "\"dev_cla\":\"" + String(deviceClass) + "\",";
    payload += "\"ic\":\"" + String(icon) + "\",";
    payload += "\"stat_cla\":\"measurement\",";
    payload += "\"uniq_id\":\"" + deviceId + "_" + name + "\",";
    
    // Device information for grouping in Home Assistant
    payload += "\"dev\":{";
    payload += "\"ids\":[\"" + deviceId + "\"],";
    payload += "\"mf\":\"skaman82\",";
    payload += "\"mdl\":\"ESP32 PowerMeter\",";
    payload += "\"name\":\"" + String(AP_SSID) + "\"";
    payload += "}}";
    
    // Publish discovery configuration
    bool result = mqttClient.publish(discoveryTopic.c_str(), payload.c_str(), true);
    Serial.println("Discovery for " + String(name) + ": " + (result ? "SUCCESS" : "FAILED"));
    
    delay(200); // Brief delay between publications to avoid overwhelming broker
}

#endif // MQTT

// ===== PROGRAM END =====

/**
 * Additional helper functions and utilities can be added here
 * This modular structure makes the code easy to extend and maintain
 */