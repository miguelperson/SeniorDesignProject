#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include <FT6336U.h>
#include <DHT11.h>  // Include the DHT library
#include "MAX6675.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h> 
#include <set>


// Pin definitions for DHT11
#define DHTPIN 5    // Pin connected to the first DHT11 data pin
#define DHTPIN2 16  // Pin connected to the second DHT11 data pin

// Pin definitions for TFT display
#define TFT_MISO 19
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST 4  // Or set to -1 if connected to ESP32 reset line

// Pin definitions for FT6336U touch controller
#define CPT_SDA 21 // 5kohm pull up resistor
#define CPT_SCL 22 // 5kohm pull up resistor
#define CPT_RST 32 // 32 or 23?
#define CPT_INT 13

// SPI connection for k-type thermocouple
#define kTypeCS1 25  // Chip Select pin for the thermocouple
#define kTypeCS2 26  // CS pin for the second thermocouple
#define kTypeSO 12   // MISO pin for the thermocouple 19 ->12
#define kTypeSCK 14  // SCK pin for the thermocouple (use the same as HSPI_CLK) 18 -> 14

// Pin definitions for power control, instantiated as basic output pins in the set up
#define heatOn 17 // changed GPIO 35 to 17
#define heatOff 27
#define powerPin 33

#define MY_NTP_SERVER "at.pool.ntp.org"
#define MY_TZ "EST5EDT,M3.2.0,M11.1.0"

SemaphoreHandle_t xMutex;
SemaphoreHandle_t chargeMutex;

// Setting global variables
int roomTemp = 0;
int roomTempF = (roomTemp * 1.8) + 32;
int temp1 = 0;
int temp2 = 0;

int internalTemp1 = 0;
int internalTemp2 = 0;
int avgInternalTemp = 0;

// bool heatingToggle = false; // going to retire this variable
bool heatingRoom = false; // true is heating false is not heating room
bool showBattery = false; // Flag for battery

bool chargingState = false; // false means not charging

int screenStatus = 0;  // 0 - main screeen/ 1 - settings screen

int finalStartHeating = 0;
int startHeatingMin = 0;
int finalEndHeating = 0;
int endHeatingMin = 0;
int finalStartCharging = 0;
int startChargingMin = 0;
int finalEndCharging = 0;
int endChargingMin = 0;
int finalTemp = 20; // preferred room temperature

// all these variables are basically temporary
int heatingTimeHour1 = 0, heatingTimeHour2 = 0, heatingStartMinute = 0, heatingEndMinute = 1;     // Heating time
int chargingTimeHour1 = 0, chargingTimeHour2 = 0, chargeStartMinute = 0, chargeEndMinute = 1;  // Charging time
int minTemp = 20, maxTemp = 36; // Temperature range
bool isCelsius = true;                                                      // Temperature scale

// Variables to track selected field
enum SelectedField { NONE, CHARGING_TIME1, CHARGING_TIME2, HEATING_TIME1, HEATING_TIME2, TEMPERATURE_RANGE, TEMPERATURE_SCALE };
SelectedField selectedField = NONE;

// Creating two DHT11 instances, one for each sensor
DHT11 dht1(DHTPIN);
DHT11 dht2(DHTPIN2);

FT6336U ft6336u(CPT_SDA, CPT_SCL, CPT_RST, CPT_INT);
TFT_eSPI tft = TFT_eSPI();  // Create a TFT_eSPI object

MAX6675 thermoCouple1(kTypeCS1, kTypeSO, kTypeSCK);  // First thermocouple
MAX6675 thermoCouple2(kTypeCS2, kTypeSO, kTypeSCK);  // Second thermocouple

const char* apSSID = "TDES_Configuration";  // Name of the AP network
const char* apPassword = "12345678";  // Password for the AP network
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);  // IP address for the AP

// Structure to hold SSID and RSSI for networks
struct WiFiNetwork {
  String ssid;
  int rssi;
};

// Array to store the top 3 Wi-Fi networks
WiFiNetwork networks[3]; 

DNSServer dnsServer;
AsyncWebServer server(80);
Preferences preferences;

TaskHandle_t wifiTaskHandle = NULL; // task handles are used to delete the tasks when needed
TaskHandle_t dataSendTaskHandle = NULL;

time_t now;                          // this are the seconds since Epoch (1970) - UTC
tm tm; // more convenient way for the time

String batteryID = "TDES1";
bool localScheduleFlag = false; // would ideally need a mutex lock but only two user controlled threads are really interacting with it anyways


void setup() {
  Serial.begin(115200);  // Initialize serial communication at 115200 baud

    #ifdef ARDUINO_ARCH_ESP32
  // ESP32 seems to be a little more complex:
  configTime(0, 0, MY_NTP_SERVER);  // 0, 0 because we will use TZ in the next line
  setenv("TZ", MY_TZ, 1);            // Set environment variable with your time zone
  tzset();
  #else
  // ESP8266
  configTime(MY_TZ, MY_NTP_SERVER);    // --> for the ESP8266 only
  #endif
  
  pinMode(kTypeSO, INPUT);  // Set GPIO12 as input initially to avoid conflicts during boot
  pinMode(heatOn, OUTPUT);
  pinMode(heatOff, OUTPUT);
  pinMode(powerPin, OUTPUT);
  digitalWrite(heatOff, LOW);

  thermoCouple1.begin();
  thermoCouple2.begin();

  tft.begin();  // Initialize the TFT display
  ft6336u.begin();  // Initialize the FT6336U touch controller
  printMain();

  xMutex = xSemaphoreCreateMutex();
  chargeMutex = xSemaphoreCreateMutex();

  if (xMutex == NULL || chargeMutex == NULL) {
    Serial.println("Mutex creation failed");
    while (1);
  }
  pinMode(kTypeSO, INPUT_PULLUP);  // Configure GPIO12 for MISO after boot

      WiFi.setAutoReconnect(false);
    WiFi.disconnect(true); // Disconnect from any saved networks and clear credentials in RAM
    // Create handles DNS server things for the instance of an
    
  xTaskCreatePinnedToCore(wifiTask,"WiFiTask",4096,NULL,1,&wifiTaskHandle,1); 
  xTaskCreatePinnedToCore(sendDataTask,"SendData",10000,NULL,2,&dataSendTaskHandle,1); // task to connect to webserver and monitor WiFi
  xTaskCreate(&touchInterface, "touchInterface", 4096, NULL, 1, NULL);
  xTaskCreate(&internalTemp, "internalTemp", 2000, NULL, 2, NULL);
  xTaskCreate(&heater, "heater", 3000, NULL, 1, NULL);
  xTaskCreate(&showTime,"showTime",2048, NULL, 1, NULL);
}

void loop() {}

void touchInterface(void *pvParameter) {

  while (1) {
    if (ft6336u.read_td_status()) {  // if touched
      int x = ft6336u.read_touch1_x();
      int y = ft6336u.read_touch1_y();
      Serial.print("FT6336U Touch Position 1: (");
      Serial.print(x);
      Serial.print(" , ");
      Serial.print(y);
      Serial.println(")");

      if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {  // wrapping screenStatus stuff in mutex
        if (screenStatus == 0) {
          if (x < 100 && x > 0 && y > 173 && y < 320) {  // toggles charging state of battery
            if(xSemaphoreTake(chargeMutex, portMAX_DELAY) == pdTRUE){
            chargingState = !chargingState; // toggles charging state and calls charge function
            chargeFunction();
            xSemaphoreGive(chargeMutex);
            }
            Serial.println("Start/Stop Charging");
          }
          if (x < 100 && x > 0 && y > 320 && y < 480) {  // toggles heating
            Serial.println("Start/Stop Heating");
            if(heatingRoom){
              turnOffHeat();
            } else if(!heatingRoom){
              turnOnHeat();
            }
          }
          if(y < 440 && y > 280 && x < 277 && x > 110){
            Serial.println("toggle internal temperature");
            tft.fillCircle(350, 120, 72, TFT_BLACK);  // Clear the circle area
            showBattery = !showBattery;
            changeInternalTemp(avgInternalTemp);

          }
          if (x > 138 && x < 259 && y > 58 && y < 200)  // toggle external temp measurement
            // celciusTemp = !celciusTemp;
          // changeRoomTemp(roomTemp);
          Serial.println("change temperature");
        }
        if (screenStatus == 1) { // in the settings screen
          clearPreviousHighlight(); // Clear the previous highlight

          // Check if the user clicked on one of the editable fields or buttons
          if (x >= 230 && x <= 255 && y >= 195 && y <= 265) { // Charging Time
            selectedField = CHARGING_TIME1;
            highlightSelectedArea(); // Highlight without changing values
          } else if (x >= 240 && x <= 285 && y >= 325 && y <= 385) { // Charging Time
            selectedField = CHARGING_TIME2;
            highlightSelectedArea(); // Highlight without changing values
          } else if (x >= 195 && x <= 210 && y >= 195 && y <= 260) { // Heating Time
            selectedField = HEATING_TIME1;
            highlightSelectedArea(); // Highlight without changing values
          } else if (x >= 195 && x <= 215 && y >= 330 && y <= 395) { // Charging Time
            selectedField = HEATING_TIME2;
            highlightSelectedArea(); // Highlight without changing values
          } else if (x >= 155 && x <= 165 && y >= 280 && y <= 355) { // Temperature Range
            selectedField = TEMPERATURE_RANGE;
            highlightSelectedArea(); // Highlight without changing values
          } else if (x >= 115 && x <= 125 && y >= 255 && y <= 285) { // Temperature Scale Celcius to Farenheight
            selectedField = TEMPERATURE_SCALE;
            highlightSelectedArea(); // Highlight without changing values
          } else if (x >= 15 && x <= 90 && y >= 32 && y <= 155) { // Save Button
            settingSave(); // saving magic here
            Serial.println("Settings saved");
          } else if (x >= 15 && x <= 90 && y >= 180 && y <= 225) { // Up Button
            increaseValue();
            Serial.println("Up");
          } else if (x >= 15 && x <= 90 && y >= 255 && y <= 310) { // Down Button
            decreaseValue();
            Serial.println("Down");
          } else if (x >= 15 && x <= 90 && y >= 335 && y <= 450) { // Back Button
            Serial.println("Back to previous screen");
          }
        }
        if (screenStatus == 0 && x <= 100 && x >= 0 && y <= 173 && y >= 0) { // press settigns button
          screenStatus = 1;  // global variable set to settings screen
          printSettings();   // uses function to print the settings screen to the display
          Serial.println("Settings Button");
      } else if (screenStatus == 1 && y > 320 && y < 480 && x < 100) { // back button
          screenStatus = 0;
          printMain();
          changeInternalTemp(avgInternalTemp);
          changeRoomTemp(roomTemp);
      }
        // Release the mutex after modifying screenStatus
        xSemaphoreGive(xMutex);
      }
      vTaskDelay(200 / portTICK_PERIOD_MS);
    } else{
            vTaskDelay(50 / portTICK_PERIOD_MS);

    }
  }
}

void showTime(void *parameter){
  while(true){
    if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
      if(screenStatus== 0){
        time(&now); // read the current time
        localtime_r(&now, &tm);             // update the structure tm with the current time
        tft.setTextSize(2); // Set the text size
        // tft.fillRect(52, 12, 190, 30, TFT_BLACK);
        tft.setCursor(52, 12); // Set cursor position for title
        if(tm.tm_sec == 59 || tm.tm_sec == 0)
          tft.fillRect(52, 12, 300, 25, TFT_BLACK);

        tft.print(tm.tm_hour);           // hours since midnight 0-23
        tft.print(":");
        tft.print(tm.tm_min);            // minutes after the hour 0-59
        tft.print(":");
        tft.print(tm.tm_sec);            // seconds after the minute 0-61*
        tft.println();

      }
      xSemaphoreGive(xMutex);    
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

void wifiTask(void *parameter) {
    while (true) {
        dnsServer.processNextRequest();  // Handle DNS requests for captive portal
        vTaskDelay(10 / portTICK_PERIOD_MS);  // Send data every 30 seconds
    }
}

void sendDataTask(void *parameter) { // this functionn is going to handle everything webserver related
  int counter = 0;

    while (true) {
      if(WiFi.status() == WL_CONNECTED){
        Serial.println("Connected to the internet!");
        Serial.print(counter);
        ++counter;
        checkFlags();
        sendBatteryUpdate(); // sending information to webserver
        vTaskDelay(15000 / portTICK_PERIOD_MS);
      }else{ // no longer connected to the internet
        // function responsible for connecting ESP32 to internet 
        connectToWiFiTask();
      }
    }
}

void internalTemp(void *pvParameter){
  while(1){
  // Read temperature from first thermocouple
  int status1 = thermoCouple1.read();
  float temp1 = thermoCouple1.getTemperature();
  internalTemp1 = temp1;

  // Read temperature from second thermocouple
  int status2 = thermoCouple2.read();
  float temp2 = thermoCouple2.getTemperature();
  internalTemp2 = temp2;

  int roomTemp1 = dht1.readTemperature();
  int roomTemp2 = dht2.readTemperature();
  if(roomTemp1 > 100 && roomTemp2 < 100){ // basically if roomTemp1 is erroring out
    roomTemp = roomTemp2;
  }else if(roomTemp1 < 100 && roomTemp2 > 100){ // if roomTemp2 is erroring out
    roomTemp = roomTemp1;
  }else if(roomTemp1 > 100 && roomTemp2 > 100){
    roomTemp = 69;
  } else{
        roomTemp = (roomTemp1 + roomTemp2) / 2; // average room temperature
  }
  // Serial.println(roomTemp1+" "+roomTemp2);

  avgInternalTemp = (internalTemp1 + internalTemp2) / 2; // average internal temperature
    if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
      if(screenStatus == 0){
        changeInternalTemp(avgInternalTemp);
        changeRoomTemp(roomTemp);
      }
      xSemaphoreGive(xMutex);

      } else{
        Serial.println("Temp failed to get mutex lock");
      }
  vTaskDelay(750 / portTICK_PERIOD_MS);
  }
}

void heater(void *pvParameter) {  // responsible for heat scheduling ==================== going to need to input webserver heating status updates if user manually changes heating status
  turnOffHeat(); // make sure default state is to off
    while (1) {
        if (tm.tm_hour == finalStartHeating && tm.tm_min == startHeatingMin && tm.tm_sec == 1) { // at 19:00:01 turn on the heating === can add another conditional statement that checks the 
          Serial.println("turning on heating");
          turnOnHeat();
        }

        if(tm.tm_hour == finalEndHeating && tm.tm_min == endHeatingMin && tm.tm_sec == 2){ // turn off at 19:00:30
          Serial.println("scheduling off for heating");
          turnOffHeat();
        }

        if(tm.tm_hour == finalStartCharging && tm.tm_min == startChargingMin && tm.tm_sec == 1){ //  checks for starting charge time
          if(xSemaphoreTake(chargeMutex, portMAX_DELAY) == pdTRUE){
            chargingState = true;
            chargeFunction();
          xSemaphoreGive(chargeMutex);
          }
        }

        if(tm.tm_hour == finalEndCharging && tm.tm_min == endChargingMin && tm.tm_sec == 5){ // checks for end charging time to toggle false
          if(xSemaphoreTake(chargeMutex, portMAX_DELAY) == pdTRUE){
            chargingState = false;
            chargeFunction();
            xSemaphoreGive(chargeMutex);
          }
        }
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}
