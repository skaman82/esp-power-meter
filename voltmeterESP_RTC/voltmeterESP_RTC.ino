#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <LittleFS.h>
#include <time.h>
#include <Preferences.h>  // Include this at the top
Preferences preferences;  // Declare this globallychar AP_SSID[32];
#include "RTClib.h"
RTC_DS3231 rtc;


const char* ssid = "airport";
const char* password = "xxx";

const char* AP_PASSWORD = "12345678";
char AP_SSID[32];
// Create server on port 80
static AsyncWebServer server(80);


#include <INA226_WE.h>
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <EEPROM.h>
#include <esp_system.h>


//CONFIG START
#define AP //USES Accespoint Mode instead of joining a preset Network
float deviceCurrent = 0.017;           // 17 mA in normal mode
float deviceCurrentWifi = 0.050;       // 50 mA in WiFi Mode
float selfconsumption;

byte batteryType = 0; //0:LiIon; 1:LiPo; 2:LiFePO4;
int cellcount = 1;            //define Cellcount of the Li-Ion battery
float max_cellvolts = 4.20;
float min_cellvolts = 3.00;
float batteryCapacityAh = 0;  // Full battery capacity in Ah eg 6.6 Ah
byte orientation = 1;
byte screen = 0; //default start screen (0-2)
byte solarmode = 0;  //if in solarmode the system will check for minimum voltage (defined by "startupvoltage")
byte wifiEnabled = 0;

float startupvoltage = 16.0;



//U8G2_SSD1306_128X32_UNIVISION_1_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);   // Adafruit ESP8266/32u4/ARM Boards + FeatherWing OLED
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
//U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

//U8G2_SH1107_PIMORONI_128X128_1_HW_I2C u8g2(U8G2_R0, /* reset=*/8);
//U8G2_SH1107_SEEED_128X128_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);


//CONFIG END

#define BUTTON1_PIN 1
#define BUTTON2_PIN 2

float remainingCapacityAh = 0;
int remainingCapacitymAh = 0;
unsigned long previousMillis = 0;
const unsigned long interval = 500;  // Interval (500 ms)

const unsigned long longPressTime = 500; // milliseconds
bool button1State = false;
bool button2State = false;
unsigned long button1PressTime = 0;
unsigned long button2PressTime = 0;
bool button1Handled = false;
bool button2Handled = false;
byte pressedbt = 0;

float runtimeHours = 0;
int hours = 0;
int minutes = 0;
float totalWh = 0;
float totalKWh = 0.0;
float pricePerKWh = 0.00; // 15 cents per kWh
float totalPrice = 0;

byte menu = 0;
byte submenu = 0;
byte menustep = 0;

#define I2C_ADDRESS 0x40
unsigned long lastUpdateTime = millis(); // track last update time
unsigned long bootTime = millis();

unsigned long lastUpdateMicros = 0;
unsigned long previousMicros = 0;

const unsigned long intervalMicros = 1000000;  // 950 ms in microseconds 950000 +3.3% (30400) = 980400 - old 990100
//CALIBRATE THIS VALUE TO GET ACCURATE MAH COUNT

char batteryChars[7] = "000.00"; // 6 chars + null terminator
char priceChars[6] = "0.000";  // Format: "0,000"

byte cur_dir = 0;

int watts = 0;
int maxWatts = 0;
float Ampere = 0;
float voltage = 0;
float cellvolt = 0;
float capacity = 0;

float shuntVoltage_mV = 0.0;
float loadVoltage_V = 0.0;
float busVoltage_V = 0.0;
float current_A = 0.0;
float power_W = 0.0;
int mAmpere = 0;
float totalUsedmAh = 0;

//SETTINGS
int typeAddress = 0;
int cellAddress = 1;
int cap_Address = 2;
int price_Address = 6;
int remcap_Address = 10;
int kwh_Address = 14; //kwh produced - price can be calculated from that
int mode_Address = 18;
int wifi_Address = 19;
int screen_Address = 30; // Next safe address after existing ones
int orientation_Address = 31;
int kwhHistory_Address = 32;  // Move forward by 2 bytes to make room

byte saved = 0;
byte reset = 0;
bool statsSaved = true;


// Energy tracking
unsigned long lastEnergySecondMillis = 0;
unsigned long hourStartMillis = 0;
float accumulatedWh = 0;
float hourlyKWh[12] = {0};  // stores last 12 hours kWh values
float historyCapacity[72];  // 72 × 10min = 720min = 12h
float historyVoltage[72] = {0};
unsigned long lastCapacityMinuteMillis = 0;
int capacityIndex = 0;

float maxA = 0.0;
float maxA_min = 0.0;
float last60Amps[60];  // Stores last 60 seconds of amp readings (1 per sec)
unsigned long lastAmpSecondMillis = 0;


DateTime lastRtcUpdateTime;
DateTime lastEnergyTime;
DateTime lastHourTime;
DateTime lastCapacityTime;
char timeChars[5] = {'0', '0', ':', '0', '0'};  // e.g. 00:00
int lastLoggedMinute = -1;
int lastLoggedHour = -1;  // Track the last hour we logged
unsigned long lastRtcUpdateSecs = 0;

// Li-ion (typical)
const int liionPoints = 11;
const float liionVoltage[liionPoints]  = {4.20, 4.10, 4.00, 3.90, 3.80, 3.70, 3.60, 3.50, 3.40, 3.30, 3.20};
const float liionCapacity[liionPoints] = {100,   90,   80,   70,   60,   50,   40,   30,   20,   10,    0};

// LiPo (similar to Li-ion, but slightly different tail)
const int lipoPoints = 11;
const float lipoVoltage[lipoPoints]  = {4.20, 4.15, 4.05, 3.95, 3.85, 3.75, 3.65, 3.55, 3.45, 3.35, 3.30};
const float lipoCapacity[lipoPoints] = {100,   95,   85,   75,   65,   55,   45,   35,   20,   10,    0};

// LiFePO₄ (very flat plateau)
const int lifepo4Points = 9;
const float lifepo4Voltage[lifepo4Points]  = {3.65, 3.45, 3.40, 3.35, 3.30, 3.25, 3.20, 3.10, 2.90};
const float lifepo4Capacity[lifepo4Points] = {100,   95,   90,   80,   60,   40,   25,   10,    0};


INA226_WE ina226 = INA226_WE(I2C_ADDRESS);

void waitForVoltageReady() {
  while (true) {
    voltage = ina226.getBusVoltage_V();
    mAmpere = ina226.getCurrent_mA();

    float minma = 50;

    Serial.print("SOLARMODE ACTIVE: Minimun voltage: "); 
    Serial.println(startupvoltage); 
    if (voltage >= startupvoltage) {
      statsSaved = false; //enables savig routine when voltage drops again
      break;  // Exit loop once voltage is stable
    }
    //ESCAPE WITH Button1 press if needed
    else if (digitalRead(BUTTON1_PIN) == LOW) {
      statsSaved = false; //enables savig routine when voltage drops again
      break;  // Exit loop once voltage is stable
    }
    
    delay(500);  // Wait and retry
  }
}

// Helper to convert __TIME__ (e.g. "14:33:12") to a unique number
int getCompileTimeSeed() {
  const char* timeStr = __TIME__; // "HH:MM:SS"
  int seed = 0;
  seed += (timeStr[0] - '0') * 36000;
  seed += (timeStr[1] - '0') * 3600;
  seed += (timeStr[3] - '0') * 600;
  seed += (timeStr[4] - '0') * 60;
  seed += (timeStr[6] - '0') * 10;
  seed += (timeStr[7] - '0');
  return seed % 900 + 100; // Ensure it's 3-digit
}



void setup() {

  // GENERATING SSID Name:
preferences.begin("esp32meter", false); // Open or create namespace

  if (!preferences.isKey("ssid")) {
    // Generate and store a new 3-digit suffix
    randomSeed(esp_random()); // Use ESP32's hardware RNG
    int suffix = random(100, 1000); // 100 to 999

    snprintf(AP_SSID, sizeof(AP_SSID), "ESP32-Meter %d", suffix);
    preferences.putString("ssid", AP_SSID);  // Save the SSID
  } else {
    // Load previously saved SSID
    String storedSSID = preferences.getString("ssid", "ESP32-Meter");
    storedSSID.toCharArray(AP_SSID, sizeof(AP_SSID));
  }

  preferences.end();



  Serial.begin(9600);  // Serielle Verbindung starten, damit die Daten am Seriellen Monitor angezeigt werden.

  EEPROM.begin(512); // Required for ESP32 (allocate flash space)

  pinMode(BUTTON1_PIN, INPUT_PULLUP);
  pinMode(BUTTON2_PIN, INPUT_PULLUP);

  Wire.begin();
  //Wire.setClock(400000UL);
  //u8g2.setBusClock(400000UL);

  ina226.init();
  Serial.println("INIT");  // shows the voltage measured


  /* Set Number of measurements for shunt and bus voltage which shall be averaged
  * Mode *     * Number of samples *
  AVERAGE_1            1 (default)
  AVERAGE_4            4
  AVERAGE_16          16
  AVERAGE_64          64
  AVERAGE_128        128
  AVERAGE_256        256
  AVERAGE_512        512
  AVERAGE_1024      1024
  */
  ina226.setAverage(AVERAGE_64);  // choose mode and uncomment for change of default

  /* Set conversion time in microseconds
     One set of shunt and bus voltage conversion will take: 
     number of samples to be averaged x conversion time x 2
     
     * Mode *         * conversion time *
     CONV_TIME_140          140 µs
     CONV_TIME_204          204 µs
     CONV_TIME_332          332 µs
     CONV_TIME_588          588 µs
     CONV_TIME_1100         1.1 ms (default)
     CONV_TIME_2116       2.116 ms
     CONV_TIME_4156       4.156 ms
     CONV_TIME_8244       8.244 ms  
  */

  // shunt max values
  //R004 > 20A (too hot at 15A)
  //R002 > 40A 
  //R001 > 80A
  //0m05 > 160A (needs a big deadband jumps about 100ma)
  
    //ina226.setConversionTime(CONV_TIME_588); //choose conversion time and uncomment for change of default

  //ina226.setResistorRange(0.0098, 20);   // R010 1 Ohm resistor > 0,0819175 / 0,0010 = 8,3 A max
  //ina226.setResistorRange(0.004, 20);   // R004 4 MOhm resistor > 0,0819175 / 0,004 = 20,479375 A max
  ina226.setResistorRange(0.00198, 20);   // R002 2 MOhm resistor > 0,0819175 / 0,002 = 40,95875 A max
  //ina226.setResistorRange(0.00099, 40);   // R001 1 MOhm resistor > 0,0819175 / 0,001 = 81,9175 A max

  //ina226.setResistorRange(0.000515,40.0); // 0.0005 > 05m
  ina226.waitUntilConversionCompleted();  //if you comment this line the first data might be zero



//RTC INIT
 
 // if (! rtc.begin()) {
 //   Serial.println("Couldn't find RTC");
 //   Serial.flush();
 //   abort();
 // }

  rtc.begin();

  if (rtc.lostPower()) {
    Serial.println("RTC lost power, let's set the time!");
    // When time needs to be set on a new device, or after a power loss, the
    // following line sets the RTC to the date & time this sketch was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    // This line sets the RTC with an explicit date & time, for example to set
    // January 21, 2014 at 3am you would call:
    // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
  }

  // When time needs to be re-set on a previously configured device, the
  // following line sets the RTC to the date & time this sketch was compiled
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  // This line sets the RTC with an explicit date & time, for example to set
  // January 21, 2014 at 3am you would call:
  // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));


DateTime now = rtc.now();
lastRtcUpdateTime = now;
lastEnergyTime = now;
lastLoggedHour = now.hour();
lastLoggedMinute = now.minute();




// READ FROM EEPROM

//SETTINGS
Serial.println("Getting COFIG:"); 

       batteryType = EEPROM.read(typeAddress); //batteryType

// Check for correct data
      if (batteryType > 2) { //check for valid batery type
        batteryType = 0;
      }
      
  //UPDATE BATTERY LIMITS
  if (batteryType == 0) { //LiIon
      max_cellvolts = 4.20;
      min_cellvolts = 3.00;
  }
  else if (batteryType == 1) { //LiPo
      max_cellvolts = 4.20;
      min_cellvolts = 3.30;
  }
  else if (batteryType == 2) { //LiFePO4
      max_cellvolts = 3.65;
      min_cellvolts = 2.50;
  }

       cellcount = EEPROM.read(cellAddress); //cellcount
       //max_cellvolts = EEPROM.read(max_cellAddress);
       //min_cellvolts = EEPROM.read(min_cellAddress);
       batteryCapacityAh = EEPROM.get(cap_Address, batteryCapacityAh); //Capacity  > 999.9 Ah
       pricePerKWh = EEPROM.get(price_Address, pricePerKWh); //Price per kWh > 0,00 ct

      if ((cellcount == 0) || (cellcount > 6)) { //check for valid cell count
        cellcount = 1;
      }
// If value is out of bounds, initialize it
if (isnan(pricePerKWh) || pricePerKWh < 0 || pricePerKWh > 1000) {
  pricePerKWh = 0.000;  // //DEFAULT VALUE
  EEPROM.put(price_Address, pricePerKWh);
  EEPROM.commit();   // Also required on ESP32 to save changes to flash
}
      
 // If value is out of bounds, initialize it
if (isnan(batteryCapacityAh) || batteryCapacityAh < 0 || batteryCapacityAh > 1000) {
  batteryCapacityAh = 1.0;  // //DEFAULT VALUE
  EEPROM.put(cap_Address, batteryCapacityAh);
  EEPROM.commit();   // Also required on ESP32 to save changes to flash
}

 solarmode = EEPROM.read(mode_Address); //SolarMode 0/1
 if (solarmode > 1) {
        solarmode = 0; //DEFAULT VALUE
        EEPROM.put(mode_Address, solarmode); //Correct 
        EEPROM.commit();   // Also required on ESP32 to save changes to flash
      }
      if (solarmode == 1) {
        screen = 1; //SET THE STATRUP SCREEN VIEW
      }

wifiEnabled = EEPROM.read(wifi_Address); //WiFimode 0/1
      if (wifiEnabled > 1) {
        wifiEnabled = 0; //DEFAULT VALUE
        EEPROM.put(wifi_Address, wifiEnabled); //Correct 
        EEPROM.commit();   // Also required on ESP32 to save changes to flash
      }

screen = EEPROM.read(screen_Address);
if (screen > 2){ screen = 0; }
orientation = EEPROM.read(orientation_Address);
if (orientation > 3){ orientation = 0; }



Serial.print("batteryType "); 
Serial.println(batteryType); 
Serial.print("cellcount "); 
Serial.println(cellcount); 
Serial.print("batteryCapacityAh "); 
Serial.println(batteryCapacityAh); 
Serial.print("pricePerKWh "); 
Serial.println(pricePerKWh); 
Serial.print("Solarmode "); 
Serial.println(solarmode);
Serial.print("wifiEnabled "); 
Serial.println(wifiEnabled); 
Serial.print("orientation "); 
Serial.println(orientation); 
Serial.print("screen "); 
Serial.println(screen); 



//STATS
remainingCapacityAh = EEPROM.get(remcap_Address, remainingCapacityAh);

 // If value is out of bounds, initialize it
if (isnan(remainingCapacityAh) || remainingCapacityAh < 0 || remainingCapacityAh > 1000) {
  remainingCapacityAh = 1.0;  // reasonable default
  EEPROM.put(remcap_Address, remainingCapacityAh);
  EEPROM.commit();   // Also required on ESP32 to save changes to flash
}

  remainingCapacitymAh = remainingCapacityAh * 1000.0;

  totalKWh = EEPROM.get(kwh_Address, totalKWh);

  // Reset if invalid (e.g., negative, NaN, or absurdly high)
if (isnan(totalKWh) || totalKWh < 0 || totalKWh > 100000) {
  Serial.println("EEPROM value invalid. Resetting to 0.");
  totalKWh = 0.0;
  EEPROM.put(kwh_Address, totalKWh);
  EEPROM.commit();
}



  //get ENERGY STATS



  Serial.println("Getting STATS:"); 
  Serial.print("remainingCapacityAh ");
  Serial.println(remainingCapacityAh); 
  Serial.print("totalKWh ");
  Serial.println(totalKWh); 
    

  //pricePerKWh = 0.39;
  //batteryCapacityAh = 100.0;
  //remainingCapacityAh = batteryCapacityAh;  // Initialize as fully charged
  //totalKWh = 0.5;

  totalWh = totalKWh * 1000.0;
  totalPrice = totalKWh * pricePerKWh;

  //CHECK FOR WOLTAGE IF SOLAR MODE IS ACTIVE

  if (solarmode == 1) {
    selfconsumption = deviceCurrentWifi;
  waitForVoltageReady();  // Wait until voltage >= definedStartVoltage
  }

  u8g2.begin();
  u8g2.setDisplayRotation(U8G2_R1);
  //u8g2.setContrast(255);

  if (orientation == 0) {
      u8g2.setDisplayRotation(U8G2_R1);
      u8g2.clear();
    } 
    else if (orientation == 1) {
      u8g2.setDisplayRotation(U8G2_R2);
      u8g2.clear();
    } 
    else if (orientation == 2) {
      u8g2.setDisplayRotation(U8G2_R3);
      u8g2.clear();
    }
    else if (orientation == 3) {
      u8g2.setDisplayRotation(U8G2_R0);
      u8g2.clear();
    } 



///SERVER INIT
// Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("Failed to mount LittleFS");
    return;
  }
  Serial.println("LittleFS mounted successfully.");



if (wifiEnabled == 1) {

#ifdef AP
  // Access Point Mode
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  delay(100);
  Serial.println("Access Point started");
  Serial.println("IP Address: " + WiFi.softAPIP().toString());
#else
  // Station Mode - connect to existing Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected.");
    Serial.println("IP Address: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nFailed to connect to WiFi.");
    return;
  }
#endif

  // Serve static files from LittleFS
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");


  // API Endpoints
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request) {
  
    String json = "{";
    DateTime now = rtc.now();  // get current RTC time
    long nowSecs = now.unixtime(); // Convert to Unix timestamp (seconds since 1970)
    json += "\"now\":" + String(nowSecs) + ",";   // Include timestamp in response
    json += "\"batteryType\":\"" + String(batteryType) + "\",";
    json += "\"cellcount\":" + String(cellcount) + ",";
    json += "\"watts\":" + String(watts) + ",";
    json += "\"Ampere\":" + String(isnan(Ampere) ? 0.0 : Ampere, 2) + ",";
    json += "\"cur_dir\":\"" + String(cur_dir) + "\",";
    json += "\"voltage\":" + String(isnan(voltage) ? 0.0 : voltage, 2) + ",";
    json += "\"capacity\":" + String(isnan(capacity) ? 0.0 : capacity, 2) + ",";
    json += "\"batteryCapacityAh\":" + String(isnan(batteryCapacityAh) ? 0.0 : batteryCapacityAh, 2) + ",";
    json += "\"remainingCapacityAh\":" + String(isnan(remainingCapacityAh) ? 0.0 : remainingCapacityAh, 2) + ",";
    json += "\"pricePerKWh\":" + String(isnan(pricePerKWh) ? 0.0 : pricePerKWh, 3) + ",";
    json += "\"totalWh\":" + String(isnan(totalWh) ? 0.0 : totalWh, 2) + ",";
    json += "\"totalKWh\":" + String(isnan(totalKWh) ? 0.0 : totalKWh, 3) + ",";

    // Dynamic used energy
    if (abs(totalUsedmAh) >= 1000.0) {
      json += "\"usedEnergy\":" + String(totalUsedmAh / 1000.0, 2) + ",";
      json += "\"usedUnit\":\"Ah\",";
    } else {
      json += "\"usedEnergy\":" + String(totalUsedmAh, 0) + ",";
      json += "\"usedUnit\":\"mAh\",";
    }

    json += "\"screen\":" + String(screen) + ",";

    // hourlyKWh
    json += "\"hourlyKWh\":[";
    for (int i = 0; i < 12; i++) {
      json += String(hourlyKWh[i], 3);
      if (i < 11) json += ",";
    }
    json += "]";

    // historyCapacity
    json += ",\"historyCapacity\":[";
    for (int i = 0; i < 72; i++) {
      json += String(static_cast<int>(historyCapacity[i]));
      if (i < 71) json += ",";
    }
    json += "]";

    // historyVoltage
    json += ",\"historyVoltage\":[";
    for (int i = 0; i < 72; i++) {
      json += String(historyVoltage[i], 2);
      if (i < 71) json += ",";
    }
    json += "]";

    json += ",\"maxA\":" + String(maxA, 2);
    json += ",\"maxA_min\":" + String(maxA_min, 2);

    json += ",\"last60Amps\":[";
for (int i = 0; i < 60; i++) {
  json += String(last60Amps[i], 2);
  json += (i < 59) ? "," : "";
}
json += "]";

    json += ",\"maxWatts\":" + String(maxWatts);
    json += "}";

    request->send(200, "application/json", json);
  });

  server.on("/setScreen", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("value", true)) {
      String value = request->getParam("value", true)->value();
      screen = value.toInt();
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing 'value'");
    }
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    Serial.println("404 Not Found: " + request->url());
    request->send(404, "text/plain", "404: Not Found");
  });

  server.begin();

  selfconsumption = deviceCurrentWifi;

}
else {
  selfconsumption = deviceCurrent;
}


//ENERGY TRACKING
  hourStartMillis = millis();

//You can prefill the arrays with 0s
  for (int i = 0; i < 60; i++) {
  last60Amps[i] = 0.0;
  }
for (int i = 0; i < 72; i++) {
  historyCapacity[i] = 0.0;
}
  for (int i = 0; i < 12; i++) {
      hourlyKWh[i] = 0.000;
  }



}


void formatPriceToChars(float value) {
  char temp[7];
  snprintf(temp, sizeof(temp), "%1.3f", value);  // "0.356"
  for (int i = 0; i < 6; i++) {
    priceChars[i] = (temp[i] == '.') ? ',' : temp[i];
  }
  priceChars[5] = '\0';
}


void disableWiFiServer() {
  server.end();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiEnabled = 0;
  EEPROM.put(wifi_Address, wifiEnabled);  // total Wh produced

  //save wifi state
  EEPROM.commit();   // Also required on ESP32 to save changes to flash

  delay(500);
  //Serial.println("Restarting...");
}


float estimateCapacity(float voltage, const float* volts, const float* caps, int size) {
  if (voltage >= volts[0]) return 100.0;
  if (voltage <= volts[size - 1]) return 0.0;

  for (int i = 0; i < size - 1; i++) {
    if (voltage <= volts[i] && voltage >= volts[i + 1]) {
      float v1 = volts[i];
      float v2 = volts[i + 1];
      float c1 = caps[i];
      float c2 = caps[i + 1];
      return c1 + ((voltage - v1) * (c2 - c1) / (v2 - v1)); // linear interpolation
    }
  }
  return 0.0;
}


void sensorread() {
  DateTime now = rtc.now();
  unsigned long nowSecs = now.unixtime(); // Unix time in seconds


  ina226.readAndClearFlags();
  voltage = ina226.getBusVoltage_V();
  Ampere = ina226.getCurrent_A();
  mAmpere = ina226.getCurrent_mA();
  watts = voltage * Ampere;

  const float deadbandThreshold = 0.01;

  // Determine direction and normalize
  if (Ampere < -deadbandThreshold) {
    cur_dir = 2; // Charging
    Ampere = -Ampere;
    watts = -watts;
    mAmpere = -mAmpere;
  } else if (Ampere > deadbandThreshold) {
    cur_dir = 1; // Discharging
  } else {
    cur_dir = 0; // Idle
    Ampere = 0;
    mAmpere = 0;
  }

  float real_Ampere = Ampere + selfconsumption;
  int real_mAmpere = real_Ampere * 1000.0;

  if (Ampere > maxA) maxA = Ampere;
  if (watts > maxWatts) maxWatts = watts;

  // Run energy calculations once per second
if ((nowSecs - lastRtcUpdateSecs) >= 1) {
  lastRtcUpdateSecs = nowSecs;
  float deltaTimeHours = (now - lastRtcUpdateTime).totalseconds() / 3600.0;
    lastRtcUpdateTime = now;

    if (abs(mAmpere) <= 4) mAmpere = 0;

    float usedmAh = (real_Ampere * 1000.0) / 3600.0;

    if (cur_dir == 1) {
      remainingCapacityAh -= usedmAh / 1000.0;
      totalUsedmAh -= usedmAh;
    } else if (cur_dir == 2) {
      remainingCapacityAh += usedmAh / 1000.0;
      totalUsedmAh += usedmAh;
    }

    remainingCapacityAh = constrain(remainingCapacityAh, 0.0, batteryCapacityAh);
    if (capacity >= 99.9) remainingCapacityAh = batteryCapacityAh;
    else if (capacity <= 0.5) remainingCapacityAh = 0;

    remainingCapacitymAh = remainingCapacityAh * 1000.0;

    if (cur_dir == 2) {
      totalWh += watts * deltaTimeHours;
      totalKWh = totalWh / 1000.0;
      totalPrice = totalKWh * pricePerKWh;
            
            Serial.print("totalWh:"); //debug
            Serial.println(totalWh); //debug
            Serial.print("totalKWh:"); //debug
            Serial.println(totalKWh); //debug
            Serial.print("totalUsedmAh:"); //debug
            Serial.println(totalUsedmAh); //debug

    }

cellvolt = voltage / cellcount;

// Lookup capacity from appropriate curve
switch (batteryType) {
  case 0: // Li-ion
    capacity = estimateCapacity(cellvolt, liionVoltage, liionCapacity, liionPoints);
    break;
  case 1: // LiPo
    capacity = estimateCapacity(cellvolt, lipoVoltage, lipoCapacity, lipoPoints);
    break;
  case 2: // LiFePO4
    capacity = estimateCapacity(cellvolt, lifepo4Voltage, lifepo4Capacity, lifepo4Points);
    break;
  default:
    capacity = 0.0;
    break;
}

capacity = constrain(capacity, 0.0, 100.0);

    if (cur_dir == 1) {
      runtimeHours = (Ampere > 0) ? (remainingCapacityAh / Ampere) : -1;
    } else if (cur_dir == 2) {
      runtimeHours = (Ampere > 0) ? ((batteryCapacityAh - remainingCapacityAh) / Ampere) : -1;
    } else {
      runtimeHours = 0;
    }

    hours = static_cast<int>(runtimeHours);
    minutes = static_cast<int>((runtimeHours - hours) * 60);
  }

  // Update amp tracking once per second
  if ((now - lastEnergyTime).totalseconds() >= 1) {
    lastEnergyTime = now;

    for (int i = 0; i < 59; i++) {
      last60Amps[i] = last60Amps[i + 1];
    }
    last60Amps[59] = Ampere;

    maxA_min = 0.0;
    for (int i = 0; i < 60; i++) {
      if (last60Amps[i] > maxA_min) {
        maxA_min = last60Amps[i];
      }
    }

    if (cur_dir == 2) {
      accumulatedWh += watts / 3600.0;
    }
  }

// Update hourly energy history every full hour (e.g., at 01:00:00, 02:00:00, etc.)
if (now.minute() == 0 && now.second() <= 1 && lastLoggedHour != now.hour()) {

  lastLoggedHour = now.hour();

  // Shift older entries left
  for (int i = 0; i < 11; i++) {
    hourlyKWh[i] = hourlyKWh[i + 1];
  }

  // Log new hourly value (convert Wh → kWh)
  hourlyKWh[11] = accumulatedWh / 1000.0;

  Serial.print("HOURMARK:"); //debug
  Serial.println(accumulatedWh); //debug


  // Reset energy counter for next hour
  accumulatedWh = 0;
}

  // Update 10-minute capacity/voltage history
 if (now.minute() % 10 == 0 && now.second() == 0 && lastLoggedMinute != now.minute()) {
  lastLoggedMinute = now.minute();

    Serial.println("10 MIN MARK"); //debug


 // Shift arrays left
    for (int i = 0; i < 71; i++) {
      historyCapacity[i] = historyCapacity[i + 1];
      historyVoltage[i] = historyVoltage[i + 1];
    }

    historyCapacity[71] = capacity;
    historyVoltage[71] = voltage;

  }


}




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

    if (duration >= longPressTime) {
      //Serial.println(buttonValue * 11); // long press: 1→11, 2→22
      pressedbt = buttonValue * 11;
    } else {
      //Serial.println(buttonValue); // short press: 1 or 2
      pressedbt = buttonValue;
    }
    handled = true;
  }

}




void buttoncheck() {


if (menu == 0) {

  if (pressedbt == 22) {
    menu = 1; //ENTER MENU SCREEN
    }

    if (pressedbt == 11) { //TURN ON/OFF WIFI
    //wifiEnabled = !wifiEnabled;
    if (wifiEnabled == 1) {
      disableWiFiServer();
      EEPROM.put(wifi_Address, wifiEnabled);  // tatal Wh produced
      EEPROM.commit();   // Also required on ESP32 to save changes to flash
    } else {
      wifiEnabled = 1;
      EEPROM.put(wifi_Address, wifiEnabled);  // tatal Wh produced

       // SAVE STATS POPUP
       EEPROM.put(remcap_Address, remainingCapacityAh); // remaining cap in Ah
       EEPROM.put(kwh_Address, totalKWh);  // tatal Wh produced

      // for (int i = 0; i < 12; i++) {
      //  EEPROM.put(kwhHistory_Address + i * sizeof(float), hourlyKWh[i]);
      //}


      EEPROM.commit();   // Also required on ESP32 to save changes to flash
      esp_restart();
 // Restart to re-enable Wi-Fi/server
    }
  }

  if (pressedbt == 2) {

        if (screen == 0) {
        u8g2.clear();
        screen = 1;
      } else if (screen == 1) {
        u8g2.clear();
        screen = 2;
      } else {
        u8g2.clear();
        screen = 0;
      }


  } else if (pressedbt == 1) { // CHANGE SCREEN ORIENTATION

    if ((orientation >= 0) && (orientation <= 2)) {
      orientation = orientation + 1;
    }
    else {
      orientation = 0;
    }

    if (orientation == 0) {
      u8g2.setDisplayRotation(U8G2_R1);
      u8g2.clear();
    } 
    else if (orientation == 1) {
      u8g2.setDisplayRotation(U8G2_R2);
      u8g2.clear();
    } 
    else if (orientation == 2) {
      u8g2.setDisplayRotation(U8G2_R3);
      u8g2.clear();
    }
    else if (orientation == 3) {
      u8g2.setDisplayRotation(U8G2_R0);
      u8g2.clear();
    } 
  }
}

else if (menu == 1) {

   if (pressedbt == 2) { //NAVIGATE MENU 
    menustep = menustep +1; 
    //set constrains
    if (menustep > 9) {
      menustep = 0;
    }
    }
   else if ((menustep == 0) && (pressedbt == 1)) { //CHANGE BAT TYPE
   
      batteryType = batteryType +1;
    //set constrains
    if (batteryType > 2) {
      batteryType = 0;
    }
    }
    else if ((menustep == 1) && (pressedbt == 1)) { //CHANGE CELL COUNT
    
      cellcount = cellcount +1;
      //set constrains
    if (cellcount > 6) {
      cellcount = 1;
    }
    }
       else if ((menustep == 2) && (pressedbt == 1)) { //SET MAX CAP
      // MAX CAP SCREEN
      snprintf(batteryChars, sizeof(batteryChars), "%06.2f", batteryCapacityAh);
      menu = 2;
      menustep = 0;
    }
       else if ((menustep == 3) && (pressedbt == 1)) { //ESTIMATE CAP based on Voltage perventage
      remainingCapacityAh = (capacity / 100.0) * batteryCapacityAh; 
    }
    else if ((menustep == 4) && (pressedbt == 1)) { //SET CURRENT CAP TO 0
      remainingCapacityAh = 0;
    }
    else if ((menustep == 5) && (pressedbt == 1)) { //SET CURRENT CAP TO 100
      remainingCapacityAh = batteryCapacityAh;
    }
     else if ((menustep == 6) && (pressedbt == 1)) { //SET PRICE PER KWH
       // PRICE SCREEN
        char temp[7];
        snprintf(temp, sizeof(temp), "%1.3f", pricePerKWh);  // "0.356"
        for (int i = 0; i < 6; i++) {
          priceChars[i] = (temp[i] == '.') ? ',' : temp[i];
        }
        priceChars[5] = '\0';

        menu = 3;
        menustep = 0;
    }
  
     else if ((menustep == 7) && (pressedbt == 1)) { //RESET STATS
       // RESET STATS 
       //remainingCapacityAh = 0;
       totalKWh = 0;
       totalWh = 0;
       totalUsedmAh = 0;
       totalPrice = 0;
       maxWatts = 0;
       maxA = 0;
       maxA_min = 0;
       //EEPROM.put(remcap_Address, remainingCapacityAh); // remaining cap in Ah
       EEPROM.put(kwh_Address, totalKWh);  // total Wh produced
       EEPROM.put(price_Address, totalPrice);  // total price 


       EEPROM.commit();   // Also required on ESP32 to save changes to flash
        

    // RESET ENERGY RECORDS
for (int i = 0; i < 12; i++) {
  hourlyKWh[i] = 0.0;
}
for (int i = 0; i < 72; i++) {
  historyCapacity[i] = 0.0;
  historyVoltage[i] = 0.0;
}
for (int i = 0; i < 60; i++) {
  last60Amps[i] = 0.0;
}

      Serial.println("Stats RESET");

       reset = 1;
    }

    else if ((menustep == 8) && (pressedbt == 1)) { //SOLAR MODE
    
     // if (solarmode == 0) {
     // solarmode = 1;
    //}
    //else {
    //  solarmode = 0;
    //}
      //ENTER TIME SUMMENU
      menu = 4;
      menustep = 0;

    }


    else if ((menustep == 9) && (pressedbt == 1)) { //EXIT MENU SCREEN

      //UPDATE BATTERY LIMITS
  if (batteryType == 0) { //LiIon
      max_cellvolts = 4.20;
      min_cellvolts = 3.00;
  }
  else if (batteryType == 1) { //LiPo
      max_cellvolts = 4.20;
      min_cellvolts = 3.30;
  }
  else if (batteryType == 2) { //LiFePO4
      max_cellvolts = 3.65;
      min_cellvolts = 2.50;
  }

//SAVE TO EPROM
       EEPROM.put(typeAddress, batteryType); //batteryType
       EEPROM.put(cellAddress, cellcount); //cellcount
       EEPROM.put(cap_Address, batteryCapacityAh); //Capacity  > 999.9 Ah
       EEPROM.put(price_Address, pricePerKWh); //Price per kWh > 0,00 ct
       EEPROM.put(mode_Address, solarmode); //solarmode setting
       EEPROM.write(screen_Address, screen);
       EEPROM.write(orientation_Address, orientation);
       EEPROM.put(remcap_Address, remainingCapacityAh); // remaining cap in Ah
       EEPROM.put(kwh_Address, totalKWh);  // tatal Wh produced

       EEPROM.commit();   // Also required on ESP32 to save changes to flash
        Serial.println("Settings saved");
        Serial.print("solarmode ");
        Serial.println(solarmode);
        Serial.print("orientation ");
        Serial.println(orientation);

    saved = 0;
    reset = 0;
    menu = 0; 
    menustep = 0;
    }
    
  }
  else if (menu == 2) { // CHAR MOD MENU CAPACITY

   if (pressedbt == 2) { //NAVIGATE CHAR 
    menustep = menustep +1; 
    
    if (menustep == 3) menustep++; // skip dot
    if (menustep > 6) menustep = 0;  
    }
    
    if ((menustep < 6) && (pressedbt == 1)) {
    char c = batteryChars[menustep];
    
    if (c >= '0' && c <= '9') {
        c++;
        if (c > '9') c = '0';
        batteryChars[menustep] = c;
    }
}

    else if ((menustep == 6) && (pressedbt == 1)) { //EXIT SCREEN

    //Update the CAP
      float newValue = atof(batteryChars);
      if (newValue > 999.99) {
          newValue = 999.99;
          dtostrf(newValue, 6, 2, batteryChars); // Reset string if over max
      }
      batteryCapacityAh = newValue;

      //Also update the remaining cap value
      remainingCapacityAh = batteryCapacityAh;

      Serial.print("Capacity UPDATED to: ");
      Serial.println(batteryCapacityAh);

      menu = 1;
      menustep = 0;
    }
    
}

else if (menu == 3) { // CHAR MOD MENU PRICE

   if (pressedbt == 2) { //NAVIGATE CHAR 
    menustep = menustep +1; 
    
    if (menustep == 1) menustep++; // skip dot
    if (menustep > 5) menustep = 0;  
    }
    
    if ((menustep < 5) && (pressedbt == 1)) {
    char &c = priceChars[menustep];
    
    if (c >= '0' && c <= '9') {
        c++;
        if (c > '9') c = '0';
        priceChars[menustep] = c;
    }
}

    else if ((menustep == 5) && (pressedbt == 1)) { 

    //Update the PRICE
      char tempStr[7];
      for (int i = 0; i < 6; i++) {
        tempStr[i] = (priceChars[i] == ',') ? '.' : priceChars[i];
      }
      float newValue = atof(tempStr);
      if (newValue > 9.999) newValue = 9.999;
      pricePerKWh = newValue;
      
      formatPriceToChars(pricePerKWh);  // Reformat string buffer
      
      Serial.print("Price UPDATED to: ");
      Serial.println(pricePerKWh);
      
      menu = 1;
      menustep = 0;
    }
    
}
else if (menu == 4) { // CHAR MOD MENU TIME

  if (pressedbt == 2) { // → Navigate character
    menustep++;

    if (menustep == 2) menustep++; // Skip colon
    if (menustep > 5) menustep = 0;
  }

  if ((menustep != 2 && menustep != 5) && pressedbt == 1) { // ↑ Increment digit
    char &c = timeChars[menustep];

    if (c >= '0' && c <= '9') {
      c++;

      if (c > '9') c = '0';

      // Range validation
      if (menustep == 0 && c > '2') c = '0'; // First hour digit max 2
      if (menustep == 1 && timeChars[0] == '2' && c > '3') c = '0'; // If H1=2, then H2 max 3
      if (menustep == 3 && c > '5') c = '0'; // First minute digit max 5
    }
  }

  else if (menustep == 5 && pressedbt == 1) { // ✅ Confirm and set RTC
    int hours = (timeChars[0] - '0') * 10 + (timeChars[1] - '0');
    int minutes = (timeChars[3] - '0') * 10 + (timeChars[4] - '0');

    if (hours >= 0 && hours <= 23 && minutes >= 0 && minutes <= 59) {
      rtc.adjust(DateTime(2025, 1, 1, hours, minutes, 0)); // Use dummy date
      Serial.print("Time updated to: ");
      Serial.print(hours);
      Serial.print(":");
      Serial.println(minutes);
    }

    // Exit to main menu
    menu = 1;
    menustep = 0;
  }
}



 if (pressedbt != 0) {
    Serial.print("Pressed button value: ");
    Serial.println(pressedbt);
    // Reset after sending
    pressedbt = 0;
  }

  //Serial.print("menustep: ");
  //Serial.println(menustep);
  //Serial.print("menu: ");
  //Serial.println(menu);


}

void draw() {
  u8g2.firstPage();
  do {
   


  


    if (screen == 0) {
      //pagination
      u8g2.drawBox(92, 4, 5, 5);
      u8g2.drawFrame(100, 4, 5, 5);
      u8g2.drawFrame(108, 4, 5, 5);

      //WIFI ICON
      if (wifiEnabled) {
      u8g2.drawFrame(119, 4, 6, 1);
      u8g2.drawFrame(120, 6, 4, 1);
      u8g2.drawFrame(121, 8, 2, 1);
      }


      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.setCursor(0, 10);
      u8g2.print("BAT ");
      if (batteryType == 0){
        u8g2.println("LiIon ");
      }
      else if (batteryType == 1) {
        u8g2.println("Lipo ");
      }
      else if (batteryType == 2) {
        u8g2.println("LiPoFe4 ");
      }
      if (cellcount == 1) {
        u8g2.println("1s");
      }
      else if (cellcount == 2) {
        u8g2.println("2s");
      }
      else if (cellcount == 3) {
        u8g2.println("3s");
      }
      else if (cellcount == 4) {
        u8g2.println("4s");
      }
      else if (cellcount == 5) {
        u8g2.println("5s");
      }
      else if (cellcount == 6) {
        u8g2.println("6s");
      }
      else {
        u8g2.println("-");
      }

      u8g2.setCursor(0, 42);
      u8g2.setFont(u8g2_font_fub20_tr);
      u8g2.print(capacity, 0);
      u8g2.setFont(u8g2_font_fub11_tr);
      u8g2.println(" %");



// Batery ICON 
      u8g2.drawFrame(89,21,6,2);
      u8g2.drawFrame(114,21,6,2);
      u8g2.drawFrame(85,24,39,30);

    

      u8g2.setFontMode(0); 
      u8g2.setDrawColor(2);



      //CAPACITY BAR
      const byte maxHeight = 28;
      const byte bottomY = 53; // This is the baseline Y-coordinate where bar starts from bottom
      byte capHeight = map(capacity, 0, 100, 0, maxHeight); // bar height depending on capacity
      byte capY = bottomY - capHeight; // top-left corner Y-position for the bar (grows upward)

      u8g2.drawBox(86, capY, 37, capHeight);


 if (cur_dir == 1) {
      //ARROW UP
      u8g2.drawTriangle(99,40, 104,34, 109,40 );
      u8g2.drawTriangle(99,46, 104,40, 109,46 );
      } else if (cur_dir == 2) {
      //ARROW DOWN
      u8g2.drawTriangle(99,34, 104,40, 109,34 );
      u8g2.drawTriangle(99,40, 104,46, 109,40 );
      } 

      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.setCursor(92,71);

       if (cur_dir == 1) {
       u8g2.print("LOAD");
           } else if (cur_dir == 2) {
       u8g2.print("CHRG");
      } else {
       u8g2.print("IDLE");
        runtimeHours = 0;  //reset counter while in idle mode
      }


     
      u8g2.setCursor(0, 72);
      u8g2.setFont(u8g2_font_fub20_tr);
      u8g2.print(voltage, 1);
      // u8g2.println("13.6");
      u8g2.setFont(u8g2_font_fub11_tr);
      u8g2.println(" V");

      u8g2.setCursor(0, 102);
     
         //Plausibility check
      if (remainingCapacityAh > 999) {
        u8g2.println("NaN");
      }
      else {
            if (remainingCapacityAh > 1) {
            u8g2.setFont(u8g2_font_fub20_tr);
            u8g2.print(remainingCapacityAh, 2);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" Ah");

          } else {
            u8g2.setFont(u8g2_font_fub20_tr);
            u8g2.print(remainingCapacitymAh);
            u8g2.setFont(u8g2_font_fub11_tr);
            u8g2.println(" mAh");
          }
      }
      

      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.setCursor(0, 123);
      if (cur_dir == 1) { u8g2.print("-"); } //LOAD
      u8g2.print(Ampere, 2);
      u8g2.println(" A ");

      if (runtimeHours >= 0) {
        u8g2.println("TIME ");
        if (hours < 10) {
          u8g2.print("0");
          u8g2.print(hours);
        } else {
          u8g2.print(hours);
        }
        u8g2.print("h ");
        if (minutes < 10) {
          u8g2.print("0");
          u8g2.print(minutes);
        } else {
          u8g2.print(minutes);
        }
        u8g2.print("m ");

      } else {
        u8g2.println("");
      }
    }


    if (screen == 1) { //SOLAR SCREEN

       //pagination
      u8g2.drawFrame(92, 4, 5, 5);
      u8g2.drawBox(100, 4, 5, 5);
      u8g2.drawFrame(108, 4, 5, 5);

      //WIFI ICON
      if (wifiEnabled) {
      u8g2.drawFrame(119, 4, 6, 1);
      u8g2.drawFrame(120, 6, 4, 1);
      u8g2.drawFrame(121, 8, 2, 1);
      }

      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.setCursor(0, 10);
      u8g2.print("COST ");
      u8g2.print(totalPrice);
      u8g2.print(" EUR");

      u8g2.setFont(u8g2_font_fub20_tr);
      u8g2.setCursor(0, 42);
      //1st row
      u8g2.print(watts);
      u8g2.setFont(u8g2_font_fub11_tr);
      u8g2.println(" W");

      //add PEAK WATTS ITEM
      u8g2.setCursor(85, 33);
      u8g2.setFont(u8g2_font_fub11_tr);
      u8g2.print(maxWatts);
      //u8g2.println(" W");
      u8g2.setCursor(85, 45);
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.println("PEAK W");


      //2nd row
      u8g2.setFont(u8g2_font_fub20_tr);
      u8g2.setCursor(0, 72);
      u8g2.print(voltage, 1);
      u8g2.setFont(u8g2_font_fub11_tr);
      u8g2.println(" V");
       //3rd row
      u8g2.setFont(u8g2_font_fub20_tr);
      u8g2.setCursor(0, 102);
      if (cur_dir == 1) { u8g2.print("-"); } //LOAD

      if (Ampere > 1) {
        u8g2.print(Ampere);
        u8g2.setFont(u8g2_font_fub11_tr);
        u8g2.println(" A");
      } else {
        u8g2.print(mAmpere);
        u8g2.setFont(u8g2_font_fub11_tr);
        u8g2.println(" mA");
      }

     // u8g2.setCursor(0, 123);
     // u8g2.setFont(u8g2_font_fub20_tr);
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.setCursor(0, 123);
      u8g2.println("TOTAL ");
      // convert to kwh if over 999wh
    if (totalWh >= 999.9) {
      u8g2.print(totalKWh, 2);
    //  u8g2.setFont(u8g2_font_fub11_tr);
      u8g2.println(" kWh ");
    }
    // print Wh
    else {
      u8g2.print(totalWh);
    //  u8g2.setFont(u8g2_font_fub11_tr);
      u8g2.println(" Wh ");
    }

      
    }


    if (screen == 2) {

       //pagination
      u8g2.drawFrame(92, 4, 5, 5);
      u8g2.drawFrame(100, 4, 5, 5);
      u8g2.drawBox(108, 4, 5, 5);

      //WIFI ICON
      if (wifiEnabled) {
      u8g2.drawFrame(119, 4, 6, 1);
      u8g2.drawFrame(120, 6, 4, 1);
      u8g2.drawFrame(121, 8, 2, 1);
      }

      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.setCursor(0, 10);
      u8g2.print("POWER ");
      u8g2.print(watts);
      u8g2.println(" W");

    //FIRST LINE
      u8g2.setFont(u8g2_font_fub20_tr);
      u8g2.setCursor(0, 42);
      //u8g2.setCursor(5, 39);
      u8g2.print(voltage, 2);
      u8g2.setFont(u8g2_font_fub11_tr);
      u8g2.println(" V");
    //second LINE
      u8g2.setFont(u8g2_font_fub20_tr);
      u8g2.setCursor(0, 72);
      
      if (cur_dir == 1) { u8g2.print("-"); } //LOAD

      if (Ampere > 1) {
        u8g2.print(Ampere);
        u8g2.setFont(u8g2_font_fub11_tr);
        u8g2.println(" A");
      } else {
        u8g2.print(mAmpere);
        u8g2.setFont(u8g2_font_fub11_tr);
        u8g2.println(" mA");

      }

      
      //3rd line
      //u8g2.setCursor(5, 95);
      u8g2.setCursor(0, 102);
      u8g2.setFont(u8g2_font_fub20_tr);
      // Used energy, dynamic scale for mAh
      if (abs(totalUsedmAh) >= 1000.0) {
      float totalUsedAh = totalUsedmAh / 1000;
      u8g2.print(totalUsedAh, 2);
      u8g2.setFont(u8g2_font_fub11_tr);
      u8g2.println(" Ah");
    } else {
      u8g2.print(totalUsedmAh, 0);
      u8g2.setFont(u8g2_font_fub11_tr);
      u8g2.println(" mAh");
    }

      //4th row
      //u8g2.setCursor(5, 123);
      //u8g2.setFont(u8g2_font_fub20_tr);
      //u8g2.print(watts);
      //u8g2.setFont(u8g2_font_fub11_tr);
      //u8g2.println(" W");

      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.setCursor(0, 123);
      u8g2.print("PEAK ");
      u8g2.print(maxA, 2);
      u8g2.print(" A");


    
      
    }

  } while (u8g2.nextPage());
}



void menuscreen() {
   u8g2.firstPage();
  do {

    DateTime now = rtc.now();  // get current RTC time

    u8g2.setFont(u8g2_font_7x13B_tr);
    u8g2.setFontMode(0);
    u8g2.setDrawColor(2);

//PAGE1 
 if (menustep <= 5) {

    u8g2.setCursor(5, 14);
    u8g2.println("Battery ");
    if (batteryType == 0){
        u8g2.println("LiIon ");
      }
    else if (batteryType == 1) {
      u8g2.println("LiPo ");
    }
    else if (batteryType == 2) {
      u8g2.println("LiFePO4 ");
    }

    u8g2.setCursor(5, 35);
    u8g2.println("Cell Count ");
    if (cellcount == 1) {
      u8g2.println("1s");
    }
    else if (cellcount == 2) {
      u8g2.println("2s");
    }
    else if (cellcount == 3) {
      u8g2.println("3s");
    }
    else if (cellcount == 4) {
      u8g2.println("4s");
    }
    else if (cellcount == 5) {
      u8g2.println("5s");
    }
    else if (cellcount == 6) {
      u8g2.println("6s");
    }
    else {
      u8g2.println("-");
    }
    u8g2.setCursor(5, 56);
    u8g2.println("Capacity ");
    u8g2.println(batteryCapacityAh, 1);
    u8g2.println(" Ah");

    u8g2.setCursor(5, 77);
    u8g2.println("Est c Cap ");
    u8g2.println(remainingCapacityAh, 1);
    u8g2.println(" Ah");

    u8g2.setCursor(5, 98);
    u8g2.println("Capacity to 0");

    u8g2.setCursor(5, 119);
    u8g2.println("Capacity to full");
    //u8g2.println(batteryCapacityAh, 1);
}

else {
    u8g2.setCursor(5, 14);
    u8g2.println("Price/kWh ");
    u8g2.println(pricePerKWh, 3);

    u8g2.setCursor(5, 35);
    u8g2.println("Reset Stats ");
    if (reset == 1) { 
      u8g2.println("OK");
    }

    u8g2.setCursor(5, 56);
    u8g2.print("Set Time ");
    u8g2.print(now.hour(), DEC);
    u8g2.println(":");
    if (now.minute() < 9) {
    u8g2.print("0");
    }
    u8g2.print(now.minute(), DEC);
 
    
    u8g2.setCursor(5, 77);
    u8g2.println("Save & Exit");


    //Show SSID Name
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(5, 110);
    u8g2.print("SSID ");
    u8g2.print(AP_SSID);
    u8g2.setCursor(5, 124);
    #ifdef AP
    u8g2.print("IP 192.168.4.1");
    #endif
    #ifndef AP
    u8g2.print("IP ");
    u8g2.print(WiFi.localIP());

    #endif
}



  //u8g2.drawStr(0, 30, AP_SSID);

    if ((submenu == 0) && (menustep == 0)) {
    u8g2.drawBox(0,0,128,20);
    }
    else if ((submenu == 0) && (menustep == 1)) {
    u8g2.drawBox(0,21,128,20);
    }
     else if ((submenu == 0) && (menustep == 2)) {
    u8g2.drawBox(0,42,128,20);
    }
     else if ((submenu == 0) && (menustep == 3)) {
    u8g2.drawBox(0,63,128,20);
    }
     else if ((submenu == 0) && (menustep == 4)) {
    u8g2.drawBox(0,84,128,20);
    }
     else if ((submenu == 0) && (menustep == 5)) {
    u8g2.drawBox(0,105,128,20);
    }
    
    //NEXT PAGE STARTING AT menustep 6
    else if ((submenu == 0) && (menustep == 6)) {
    u8g2.drawBox(0,0,128,20);
    }
      else if ((submenu == 0) && (menustep == 7)) {
    u8g2.drawBox(0,21,128,20);
    }
       else if ((submenu == 0) && (menustep == 8)) {
    u8g2.drawBox(0,42,128,20);
    }
         else if ((submenu == 0) && (menustep == 9)) {
    u8g2.drawBox(0,63,128,20);
    }
    



    } while (u8g2.nextPage());
}






void capscreen() {
   u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_7x13B_tr);
    u8g2.setCursor(5, 10);
    u8g2.print("Capacity in Ah");



    for (int i = 0; i < 6; i++) {
    int x = 10 + i * 12;
    u8g2.setCursor(x, 30);
    u8g2.print(batteryChars[i]);

    if (i == menustep) {  // Highlight selected character
      u8g2.drawFrame(x - 2, 18, 12, 14);
    }
  }
 
// Draw EXIT option at end
int exitX = 10 + 6 * 12 + 8; // some spacing after last char
u8g2.setCursor(exitX, 30);
u8g2.print("OK");

if (menustep == 6) {
    int exitWidth = u8g2.getStrWidth("OK") + 4;
    u8g2.drawFrame(exitX - 2, 18, exitWidth, 14);
}

    } while (u8g2.nextPage());
}



void pricescreen() {
   u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_7x13B_tr);
    u8g2.setCursor(5, 10);
    u8g2.print("Price / kW");


   for (int i = 0; i < 5; i++) {
    int x = 10 + i * 12;
    u8g2.setCursor(x, 30);
    u8g2.print(priceChars[i]);

    if (i == menustep) {
      u8g2.drawFrame(x - 2, 18, 12, 14);
    }
  }

  // EXIT
  int exitX = 10 + 5 * 12 + 8;
  u8g2.setCursor(exitX, 30);
  u8g2.print("OK");

  if (menustep == 5) {
    int exitWidth = u8g2.getStrWidth("OK") + 4;
    u8g2.drawFrame(exitX - 2, 18, exitWidth, 14);
  }


    } while (u8g2.nextPage());
}


void timescreen() {
  // Populate timeChars[] with current time if this is the first call
  static bool initialized = false;
  if (!initialized) {
    DateTime now = rtc.now();
    int h = now.hour();
    int m = now.minute();

    timeChars[0] = '0' + (h / 10);
    timeChars[1] = '0' + (h % 10);
    timeChars[2] = ':'; // colon separator
    timeChars[3] = '0' + (m / 10);
    timeChars[4] = '0' + (m % 10);

    initialized = true;
  }

  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_7x13B_tr);
    u8g2.setCursor(5, 10);
    u8g2.print("Set Time (24h)");

    // Draw HH:MM characters
    for (int i = 0; i < 5; i++) {
      int x = 15 + i * 12;

      if (i == 2) {
        u8g2.setCursor(x, 30);
        u8g2.print(":");
        continue;
      }

      u8g2.setCursor(x, 30);
      u8g2.print(timeChars[i]);

      if (i == menustep) {
        u8g2.drawFrame(x - 2, 18, 12, 14); // Highlight digit
      }
    }

    // Draw EXIT button (as 6th step)
    if (menustep == 5) {
      u8g2.drawFrame(90, 20, 30, 16); // frame around "EXIT"
    }
    u8g2.setCursor(94, 32);
    u8g2.print("OK");

  } while (u8g2.nextPage());
}

unsigned long lastSensorMillis = 0;


void loop() {
  handleButton(BUTTON1_PIN, button1State, button1PressTime, button1Handled, 1);
  handleButton(BUTTON2_PIN, button2State, button2PressTime, button2Handled, 2);
  
if (millis() - lastSensorMillis >= 1000) {
    lastSensorMillis = millis();
    sensorread();  // call once per second
  }
  buttoncheck();

if (solarmode == 1) {





 if (voltage > startupvoltage) {  //DO USUAL STUFF IF VOLTAGE IS ABOVE startupvoltage
  if (menu == 0) {
  draw();
  }
  else if (menu == 1) {
  menuscreen();
  }
  else if (menu == 2) {
  capscreen();
  }
   else if (menu == 3) {
  pricescreen();
  }
    else if (menu == 4) {
  timescreen();
  }
 }
 else { // CLEAR SCREEN if voltage drops startupvoltage
  if (menu == 0) {
  // clear screen
    u8g2.clear();
  }
  else if (menu == 1) {
  menuscreen();
  }
  else if (menu == 2) {
  capscreen();
  }
   else if (menu == 3) {
  pricescreen();
  }
   else if (menu == 4) {
  timescreen();
  }
 }

}
else if (solarmode == 0) { //DO USUAL STUFF
  if (menu == 0) {
  draw();
  }
  else if (menu == 1) {
  menuscreen();
  }
  else if (menu == 2) {
  capscreen();
  }
   else if (menu == 3) {
  pricescreen();
  }
   else if (menu == 4) {
  timescreen();
  }
}





}



