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
SemaphoreHandle_t heatMutex;
SemaphoreHandle_t roomTempMutex;
SemaphoreHandle_t internalTempMutex;

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
int heatingTimeHour1 = 0, heatingTimeHour2 = 0, heatingStartMinute = 0, heatingEndMinute = 0;  // Heating time
int chargingTimeHour1 = 0, chargingTimeHour2 = 0, chargeStartMinute = 0, chargeEndMinute = 0;  // Charging time
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
  heatMutex = xSemaphoreCreateMutex();
  roomTempMutex = xSemaphoreCreateMutex();
  internalTempMutex = xSemaphoreCreateMutex();

  if (xMutex == NULL || chargeMutex == NULL || heatMutex == NULL || roomTempMutex == NULL || internalTempMutex == NULL) {
    Serial.println("Mutex creation failed");
    while (1);
  }
  pinMode(kTypeSO, INPUT_PULLUP);  // Configure GPIO12 for MISO after boot

      WiFi.setAutoReconnect(false);
    WiFi.disconnect(true); // Disconnect from any saved networks and clear credentials in RAM
    // Create handles DNS server things for the instance of an
    
  xTaskCreatePinnedToCore(wifiTask,"WiFiTask",4096,NULL,1,&wifiTaskHandle,1); 
  xTaskCreatePinnedToCore(sendDataTask,"SendData",16384,NULL,2,&dataSendTaskHandle,1); // task to connect to webserver and monitor WiFi
  xTaskCreate(&touchInterface, "touchInterface", 4096, NULL, 1, NULL);
  xTaskCreate(&internalTemp, "internalTemp", 2000, NULL, 2, NULL);
  xTaskCreate(&heater, "heater", 3000, NULL, 1, NULL);
  xTaskCreate(&showTime,"showTime",2048, NULL, 1, NULL);
  // xTaskCreate(monitorStack, "StackMonitor", 2048, NULL, 1, NULL);

}

void loop() {} // ===============================================================================================================================

void sendDataTask(void *parameter) {
    int counter = 0;
    while (true) {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("Connected to the internet!");
            checkFlags();  // Now async
            sendBatteryUpdate();  // Now async
            Serial.print("internal temperature is: ");
            // if (xSemaphoreTake(internalTempMutex, portMAX_DELAY) == pdTRUE) {
            //     Serial.println(avgInternalTemp);
            //     xSemaphoreGive(internalTempMutex);
            // }
            // Serial.print("room temp: ");
            // if (xSemaphoreTake(roomTempMutex, portMAX_DELAY) == pdTRUE) {
            //     Serial.println(roomTemp);
            //     xSemaphoreGive(roomTempMutex);
            // }
            vTaskDelay(15000 / portTICK_PERIOD_MS);
        } else {
            connectToWiFiTask();  // Handle Wi-Fi connection
        }
    }
}

void wifiTask(void *parameter) {
    while (true) {
        dnsServer.processNextRequest();  // Handle DNS requests for captive portal
        vTaskDelay(10 / portTICK_PERIOD_MS);  // Send data every 30 seconds
    }
}

void heater(void *pvParameter) {  // responsible for heat scheduling ==================== going to need to input webserver heating status updates if user manually changes heating status
  if(xSemaphoreTake(heatMutex, portMAX_DELAY) == pdTRUE){
      turnOffHeat(); // make sure default state is to off
      xSemaphoreGive(heatMutex);
  }
  int tempInternalTemperature = 0;
    while (1) {
      if(xSemaphoreTake(internalTempMutex, portMAX_DELAY) == pdTRUE){ // creating local internal temp vairable to avoid nested mutex locks
        tempInternalTemperature = avgInternalTemp;
        xSemaphoreGive(internalTempMutex);
      }

      if(xSemaphoreTake(roomTempMutex, portMAX_DELAY) == pdTRUE){
        if (tm.tm_hour == finalStartHeating && tm.tm_min == heatingEndMinute && tm.tm_sec == 1 && roomTemp < finalTemp) { // at 19:00:01 turn on the heating
          Serial.println("turning on heating");
          if(xSemaphoreTake(heatMutex, portMAX_DELAY) == pdTRUE){
            turnOnHeat();
            xSemaphoreGive(heatMutex);
          }
        }
        xSemaphoreGive(roomTempMutex);
      }
        if(tm.tm_hour == finalEndHeating && tm.tm_min == heatingEndMinute && tm.tm_sec == 30){ // turn off at 19:00:30
          Serial.println("scheduling off for heating");
          if(xSemaphoreTake(heatMutex, portMAX_DELAY) == pdTRUE){
            turnOffHeat();
            xSemaphoreGive(heatMutex);
          }
        }
      // if(xSemaphoreTake(internalTempMutex, portMAX_DELAY) == pdTRUE){
        if(tm.tm_hour == finalStartCharging && tm.tm_min == chargeStartMinute && tm.tm_sec == 0 && tempInternalTemperature < 500){ //  checks for starting charge time
          if(xSemaphoreTake(chargeMutex, portMAX_DELAY) == pdTRUE){
            chargingState = true;
            chargeFunction();
          xSemaphoreGive(chargeMutex);
          }
        }
      //   xSemaphoreGive(internalTempMutex);
      // }

        if(tm.tm_hour == finalEndCharging && tm.tm_min == chargeEndMinute && tm.tm_sec == 2){ // checks for end charging time to toggle false
          if(xSemaphoreTake(chargeMutex, portMAX_DELAY) == pdTRUE){
            chargingState = false;
            chargeFunction();
            xSemaphoreGive(chargeMutex);
          }
        }
      vTaskDelay(750 / portTICK_PERIOD_MS);
    }
}

void internalTemp(void *pvParameter){
  int localScreenStatus = 0;
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
  if(roomTemp1 > 100 || roomTemp2 > 100){ // updating room temperature value
    if(xSemaphoreTake(roomTempMutex, portMAX_DELAY) == pdTRUE){
      if(roomTemp1 > 100 && roomTemp2 < 100){ // basically if roomTemp1 is erroring out
      roomTemp = roomTemp2;
      }else if(roomTemp1 < 100 && roomTemp2 > 100){ // if roomTemp2 is erroring out
        roomTemp = roomTemp1;
      }else if(roomTemp1 > 100 && roomTemp2 > 100){
        roomTemp = 69;
      } 
      xSemaphoreGive(roomTempMutex);
    }
  } else{
      if(xSemaphoreTake(roomTempMutex,portMAX_DELAY) == pdTRUE){
        roomTemp = (roomTemp1 + roomTemp2) / 2; // average room temperature
        xSemaphoreGive(roomTempMutex);
      }
  }

  if(temp1 < 10 || temp2 < 10){ // technically I would've been fine wrapping the whole thing in one mutex, but if anything to reduce time of mutex lock being held its probably best to just have the mutex lock for each individual if block
    if(xSemaphoreTake(internalTempMutex, portMAX_DELAY) == pdTRUE){
      if(temp1 < 10 && temp2 > 10){ // if thermocouple 1 is failing
      avgInternalTemp = temp2;
      } else if(temp1 > 10 && temp2 < 10){ // if thermocouple 2 is failing
      avgInternalTemp = temp1;
      } else if(temp1 < 10 && temp2 < 10){ // if both are failing
      avgInternalTemp = 69;
      }
      xSemaphoreGive(internalTempMutex);
    }
  } else{ // this else is if both thermocouples are having correct readings
      if(xSemaphoreTake(internalTempMutex, portMAX_DELAY) == pdTRUE){
        avgInternalTemp = (internalTemp1 + internalTemp2) / 2; // average internal temperature
        xSemaphoreGive(internalTempMutex);
      }
    }
    if(xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE){
      localScreenStatus = screenStatus;
      xSemaphoreGive(xMutex);
    }

    // if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
      if(localScreenStatus == 0){
        if(xSemaphoreTake(internalTempMutex, portMAX_DELAY) == pdTRUE){
          changeInternalTemp(avgInternalTemp);
          xSemaphoreGive(internalTempMutex);
        }
        if(xSemaphoreTake(roomTempMutex, portMAX_DELAY) == pdTRUE){ // get mutex for the roomTemp global variable
          changeRoomTemp(roomTemp);
          xSemaphoreGive(roomTempMutex);
        }
      }
      // xSemaphoreGive(xMutex);

      // } else{
      //   Serial.println("Temp failed to get mutex lock");
      // }
  vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

void touchInterface(void *pvParameter) {
  int localScreenStatus = 0;

  while (1) {
    if (ft6336u.read_td_status()) {  // if touched
      int x = ft6336u.read_touch1_x();
      int y = ft6336u.read_touch1_y();
      Serial.print("FT6336U Touch Position 1: (");
      Serial.print(x);
      Serial.print(" , ");
      Serial.print(y);
      Serial.println(")");
      // if(xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE){
      //   xSemaphoreGive(xMutex);
      // }

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
            if(xSemaphoreTake(heatMutex, portMAX_DELAY) == pdTRUE){
              if(heatingRoom){
              turnOffHeat();
            } else if(!heatingRoom){
              turnOnHeat();
            }
            xSemaphoreGive(heatMutex);
            }
          }
          if(y < 440 && y > 280 && x < 277 && x > 110){
            Serial.println("toggle internal temperature");
            tft.fillCircle(350, 120, 72, TFT_BLACK);  // Clear the circle area
            showBattery = !showBattery;
            if(xSemaphoreTake(internalTempMutex, portMAX_DELAY) == pdTRUE){
              changeInternalTemp(avgInternalTemp);
              xSemaphoreGive(internalTempMutex);
            }
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
          if(xSemaphoreTake(internalTempMutex, portMAX_DELAY) == pdTRUE){
            changeInternalTemp(avgInternalTemp);
            xSemaphoreGive(internalTempMutex);
          }
          if(xSemaphoreTake(roomTempMutex, portMAX_DELAY) == pdTRUE){
            changeRoomTemp(roomTemp);
            xSemaphoreGive(roomTempMutex);
          }
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

void getSchedule(){ // gets app uploaded schedule from the database
  if(WiFi.status() == WL_CONNECTED){
    HTTPClient http;
    String serverPath = "https://sandbattery.info/TDESGetSchedule?batteryID="+batteryID;
    http.begin(serverPath);
    int httpCode = http.GET();

    if(httpCode > 0){
      String payload = http.getString();  // Get the response body
      Serial.println("Payload received: " + payload);

      if (httpCode == 404) {
        Serial.println("Battery not found (404)");
        return;  // Stop here if the battery wasn't found
      }

      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return;
      }

      if (doc.containsKey("exists") && doc["exists"] == true) {
        int heatStartHour = doc["heatStartHour"];
        int heatEndHour = doc["heatEndHour"];
        int startheatingMinute = doc["startheatingMinute"];
        int stopHeatingMinute = doc["stopHeatingMinute"];

        int startChargingHour = doc["startChargingHour"]; // 1
        int endChargingHour = doc["endChargingHour"];
        int startChargingMinute = doc["startChargingMinute"];
        int endChargingMinute = doc["endChargingMinute"];

        // Serial.println(startChargingHour+""+endChargingHour+""+startChargingMinute+""+endChargingMinute);

        finalStartHeating = heatStartHour;
        finalEndHeating = heatEndHour;
        finalStartCharging = startChargingHour;
        finalEndCharging = endChargingHour;
        heatingStartMinute = startheatingMinute;
        heatingEndMinute = stopHeatingMinute;
        chargeStartMinute = startChargingMinute;
        chargeEndMinute = endChargingMinute;
      }


    }
  }
}

void checkFlags() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String serverPath = "https://sandbattery.info/TDESToggleCheck?batteryID="+batteryID;
        // Serial.println("URL: " + serverPath);

        http.begin(serverPath);

        int httpCode = http.GET();  // No need to add headers for GET requests

        if (httpCode > 0) {
            String payload = http.getString();  // Get the response body
            Serial.println("Payload received: " + payload);

            if (httpCode == 404) {
                Serial.println("Battery not found (404)");
                return;  // Stop here if the battery wasn't found
            }

            StaticJsonDocument<512> doc;
            DeserializationError error = deserializeJson(doc, payload);

            if (error) {
                Serial.print(F("deserializeJson() failed: "));
                Serial.println(error.f_str());
                return;
            }

            // Check for the 'exists' field first
            if (doc.containsKey("exists") && doc["exists"] == true) {
                bool heatingToggleFlag = doc["heatingToggleFlag"];
                bool chargingToggleFlag = doc["chargingToggleFlag"];
                bool schedulingFlag = doc["scheduleFlag"];

                if (heatingToggleFlag) {
                  if(xSemaphoreTake(heatMutex, portMAX_DELAY) == pdTRUE){
                    if (heatingRoom) {
                        turnOffHeat();  // Turn off heating if it is on
                    } else {
                        turnOnHeat();  // Turn on heating if it is off
                    }
                    xSemaphoreGive(heatMutex);
                  }

                }

                if (chargingToggleFlag) {
                  if(xSemaphoreTake(chargeMutex, portMAX_DELAY) == pdTRUE){
                    chargingState = !chargingState;  // Toggle charging state
                    chargeFunction();
                    xSemaphoreGive(chargeMutex);
                  }
                }

                if(schedulingFlag){
                  // Serial.println("new schedule detected, getting and updating local schedule");
                  localScheduleFlag = false; // lowers local save flag just in case
                  getSchedule(); // if scheduling flag was raised then get new schedule from cloud
                }

            } else {
                Serial.println("Battery does not exist or flags not present");
            }

        } else {
            Serial.print("Error sending GET request: ");
            Serial.println(httpCode);  // Print HTTP error code
        }

        http.end();  // End HTTP connection

    } else {
        Serial.println("Wi-Fi not connected. Unable to send data.");
    }
}

void startAccessPoint() {
    WiFi.softAP(apSSID, apPassword);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    Serial.println("Access Point started");
    Serial.print("IP Address: ");
    Serial.println(WiFi.softAPIP());

    // Start DNS server to redirect all requests to the AP IP address
    dnsServer.start(DNS_PORT, "*", apIP);

    // Setup web server routes for captive portal detection and configuration page
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", generateHTML());
    });

    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", generateHTML());  // For Android captive portal
    });

    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", generateHTML());  // For iOS captive portal
    });

    server.on("/submit", HTTP_POST, [](AsyncWebServerRequest *request) {
        String ssid = request->getParam("ssid", true)->value();
        String password = request->getParam("password", true)->value();
        saveCredentials(ssid, password);
        request->send(200, "text/html", "Credentials saved. Attempting to connect to Wi-Fi...");
        delay(2000);  // Short delay before restarting
        ESP.restart();
    });
    server.begin();
    Serial.println("Web server started");
}

void connectToWiFiTask() {
    
    preferences.begin("wifi-creds", false); // gets log in information
    String ssid = preferences.getString("ssid", "");
    String password = preferences.getString("password", "");
    preferences.end();

    WiFi.mode(WIFI_STA); // mode to connect to WiFi
    WiFi.begin(ssid.c_str(), password.c_str()); // runs to connect to the Wifi

    Serial.print("Connecting to Wi-Fi");
    int attemptCount = 0;
    while (WiFi.status() != WL_CONNECTED) { // this is the time out code for when its trying to connect
        delay(1000);
        Serial.print(".");
        if (++attemptCount > 10) {
            Serial.println("Failed to connect. Starting AP mode.");
            clearCredentials(); // if disconnects for too long will just clear the credentials
            startAccessPoint(); // Fall back to AP mode if connection fails
            // xTaskCreate(startAccessPointTask, "startAccessPointTask", 8192, NULL, 1, NULL); // Start the AP mode if connection fails

            vTaskDelete(NULL);  // Delete this task
        }
    }

    Serial.println();
    Serial.println("Connected to Wi-Fi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
}

void sendBatteryUpdate() {
  int localRoomTemp = 0;
  int localAvgInternalTemp = 0;
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String serverPath = "https://sandbattery.info/batteryUpdate";

        // Create the JSON document with a size of 512 (adjust this size based on your actual usage)
        StaticJsonDocument<512> doc;

        if(xSemaphoreTake(roomTempMutex, portMAX_DELAY) == pdTRUE){
          localRoomTemp = roomTemp;
          xSemaphoreGive(roomTempMutex);
        }

        if(xSemaphoreTake(internalTempMutex, portMAX_DELAY) == pdTRUE){
          localAvgInternalTemp = avgInternalTemp;
          xSemaphoreGive(internalTempMutex);
        }

        // Add the data to the JSON document
        doc["batteryID"] = batteryID;
        // if(xSemaphoreTake(roomTempMutex, portMAX_DELAY) == pdTRUE){
          doc["currentRoomTemp"] = localRoomTemp;
        //   xSemaphoreGive(roomTempMutex);
        // }

        // if (xSemaphoreTake(internalTempMutex, portMAX_DELAY) == pdTRUE) {
            doc["currentInternalTemp"] = localAvgInternalTemp;
        //     xSemaphoreGive(internalTempMutex);
        // }
        doc["setRoomTemp"] = finalTemp;
        doc["heatingRoom"] = heatingRoom;
        doc["ChargingBoolean"] = chargingState;

        // Only add the schedule if the localScheduleFlag is true
        if (localScheduleFlag) {
            localScheduleFlag = false;  // reset the flag after sending
            Serial.println("inside the localSchedule flag if");
            doc["finalStartHeating"] = finalStartHeating;
            doc["finalEndHeating"] = finalEndHeating;
            doc["finalStartCharging"] = finalStartCharging;
            doc["finalEndCharging"] = finalEndCharging;
            doc["startHeatingMin"] = startHeatingMin;
            doc["endHeatingMin"] = endHeatingMin;
            doc["startChargingMin"] = startChargingMin;
            doc["endChargingMin"] = endChargingMin;

        }

        // Serialize the JSON document to a string
        String jsonPayload;
        serializeJson(doc, jsonPayload);

        // Initialize the HTTP connection
        http.begin(serverPath);
        http.addHeader("Content-Type", "application/json");

        // Send the POST request with the JSON payload
        int httpResponseCode = http.POST(jsonPayload);

        // Handle the HTTP response
        if (httpResponseCode > 0) {
            String response = http.getString();
            Serial.println(httpResponseCode);
            Serial.println(response);
        } else {
            Serial.print("Error on sending POST: ");
            Serial.println(httpResponseCode);
        }

        // Close the connection
        http.end();
    } else {
        Serial.println("Wi-Fi not connected. Unable to send data.");
    }
}

void turnOnHeat() {
  heatingRoom = true;
  Serial.println("heating on");
  digitalWrite(heatOff, LOW);
  digitalWrite(heatOn, HIGH);
  heatCircle();

}

void turnOffHeat() {
  heatingRoom = false;
  digitalWrite(heatOn, LOW);
  digitalWrite(heatOff, HIGH);
  heatCircleClear();
}

void chargeFunction() {  // function checks charging state and toggles according to whats needed
    if (chargingState) { // if charging state set to true
      digitalWrite(powerPin, HIGH);
      chargeCircle();
    } else {
      digitalWrite(powerPin, LOW);
      chargeCircleClear();
    }
}

// Function to show that charging is ON
void chargeCircle() {
  tft.fillCircle(390, 18, 7, TFT_YELLOW); // Yellow circle
}

void chargeCircleClear(){
    tft.fillCircle(390, 18, 7, TFT_BLACK);

}

// Function to show that heating is ON
void heatCircle() {
  tft.fillCircle(410, 18, 7, TFT_GREEN); // Green circle
}

void heatCircleClear(){
    tft.fillCircle(410, 18, 7, TFT_BLACK);

}

// Function to alert that it is overheating
void overheatingAlert() {
  tft.fillCircle(430, 18, 7, TFT_RED); // Red circle
}

void editTemperatureRange() {
  if (minTemp < maxTemp)
      minTemp = minTemp + 1;
  else if (minTemp > maxTemp) 
      minTemp = maxTemp; // Example logic to reset
}

void toggleTemperatureScale() {
  if (isCelsius) {
    // Convert to Fahrenheit with rounding
    minTemp = round((minTemp * 9 / 5) + 32);
    maxTemp = round((maxTemp * 9 / 5) + 32);
  } else {
    // Convert back to Celsius with rounding
    minTemp = round((minTemp - 32) * 5.0 / 9.0);
    maxTemp = round((maxTemp - 32) * 5.0 / 9.0);
  }
  isCelsius = !isCelsius; // Toggle the scale
  updateSelectedArea(); // Update the display with the new scale
}

// Function to highlight selected area without changing values
void highlightSelectedArea() {
    switch (selectedField) {
        case CHARGING_TIME1:
            tft.drawRect(190, 40, 93, 33, TFT_YELLOW);
            break;
        case CHARGING_TIME2:
            tft.drawRect(320, 40, 92, 33, TFT_YELLOW);
            break;
        case HEATING_TIME1:
            tft.drawRect(190, 85, 93, 33, TFT_YELLOW);
            break;
        case HEATING_TIME2:
            tft.drawRect(320, 85, 92, 33, TFT_YELLOW);
            break;
        case TEMPERATURE_RANGE:
            tft.drawRect(270, 130, 90, 33, TFT_YELLOW);
            break;
        case TEMPERATURE_SCALE:
            tft.drawRect(245, 172, 42, 33, TFT_YELLOW);
            break;
        default:
            break;
    }
}

void clearPreviousHighlight() {
  switch (selectedField) {
    case CHARGING_TIME1:
      tft.drawRect(190, 40, 93, 33, TFT_BLACK); // Clear charging time 1 highlight
      break;
    case CHARGING_TIME2:
      tft.drawRect(320, 40, 92, 33, TFT_BLACK); // Clear charging time 2 highlight
      break;
    case HEATING_TIME1:
      tft.drawRect(190, 85, 93, 33, TFT_BLACK); // Clear heating time 1 highlight
      break;
    case HEATING_TIME2:
      tft.drawRect(320, 85, 92, 33, TFT_BLACK); // Clear heating time 2 highlight
      break;
    case TEMPERATURE_RANGE:
      tft.drawRect(270, 130, 90, 33, TFT_BLACK); // Clear temperature range highlight
      break;
    case TEMPERATURE_SCALE:
      tft.drawRect(245, 172, 42, 33, TFT_BLACK); // Clear temperature scale highlight
      break;
    default:
      break;
  }
}


void updateSelectedArea() {
    switch (selectedField) {
        case CHARGING_TIME1:
            // Clear the area and redraw the updated time
            tft.fillRect(190, 40, 95, 33, TFT_BLACK);
            tft.setTextSize(3);
            tft.setCursor(195, 45);
            tft.print(chargingTimeHour1);
            tft.print(":");
            tft.print(chargeStartMinute);
            highlightSelectedArea(); // Ensure the area remains highlighted
            delay(50);
            break;

        case CHARGING_TIME2:
            // Clear the area and redraw the updated time
            tft.fillRect(320, 40, 95, 33, TFT_BLACK);
            tft.setTextSize(3);
            tft.setCursor(325, 45);
            tft.print(chargingTimeHour2);
            tft.print(":");
            tft.print(chargeEndMinute);
            highlightSelectedArea(); // Ensure the area remains highlighted
            delay(50);
            break;

            case HEATING_TIME1:
            // Clear the area and redraw the updated time
            tft.fillRect(190, 85, 95, 33, TFT_BLACK);
            tft.setTextSize(3);
            tft.setCursor(195, 90);
            tft.print(heatingTimeHour1);
            tft.print(":");
            tft.print(heatingStartMinute);
            highlightSelectedArea(); // Ensure the area remains highlighted
            delay(50);
            break;

        case HEATING_TIME2:
            // Clear the area and redraw the updated time
            tft.fillRect(320, 85, 95, 33, TFT_BLACK);
            tft.setTextSize(3);
            tft.setCursor(325, 90);
            tft.print(heatingTimeHour2);
            tft.print(":");
            tft.print(heatingEndMinute);
            highlightSelectedArea(); // Ensure the area remains highlighted
            delay(50);
            break;

        case TEMPERATURE_RANGE:
            // Clear the area and redraw the updated temperature range
            tft.fillRect(270, 130, 90, 33, TFT_BLACK);
            tft.setTextSize(3);
            tft.setCursor(280, 135);
            tft.print(minTemp);
            tft.print((char)247); // Degree symbol
            tft.print(isCelsius ? "C" : "F");
            highlightSelectedArea(); // Ensure the area remains highlighted
            delay(50);
            break;

        case TEMPERATURE_SCALE:
            // Clear the area and redraw the updated temperature scale
            tft.fillRect(245, 172, 42, 33, TFT_BLACK);
            tft.setTextSize(3);
            tft.setCursor(250, 180);
            tft.print((char)247); // Degree symbol
            tft.print(isCelsius ? "C" : "F");
            tft.fillRect(270, 130, 90, 33, TFT_BLACK);
            tft.setTextSize(3);
            tft.setCursor(280, 135);
            tft.print(minTemp);
            tft.print((char)247); // Degree symbol
            tft.print(isCelsius ? "C" : "F");
            highlightSelectedArea(); // Ensure the area remains highlighted
            delay(50);
            break;

        default:
            break;
    }
}

/// Increase the value of the selected field
void increaseValue() {
  switch (selectedField) {
    case CHARGING_TIME1:
      chargeStartMinute += 15;
      if (chargeStartMinute >= 60) {
        chargeStartMinute = 0;  // Reset minutes to 0 when reaching 60
        chargingTimeHour1 = (chargingTimeHour1 + 1) % 24;  // Increase the hour
      }
      updateSelectedArea();
      break;
    case CHARGING_TIME2:
      chargeEndMinute += 15;
      if (chargeEndMinute >= 60) {
        chargeEndMinute = 0;
        chargingTimeHour2 = (chargingTimeHour2 + 1) % 24;
      }
      updateSelectedArea();
      break;
    case HEATING_TIME1:
      heatingStartMinute += 15;
      if (heatingStartMinute >= 60) {
        heatingStartMinute = 0;
        heatingTimeHour1 = (heatingTimeHour1 + 1) % 24;
      }
      updateSelectedArea();
      break;
    case HEATING_TIME2:
      heatingEndMinute += 15;
      if (heatingEndMinute >= 60) {
        heatingEndMinute = 0;
        heatingTimeHour2 = (heatingTimeHour2 + 1) % 24;
      }
      updateSelectedArea();
      break;
    case TEMPERATURE_RANGE:
      if (isCelsius) {
        // For Celsius scale, limit minTemp to maxTemp and ensure it doesn't exceed 36°C
        if (minTemp < 36) {
          minTemp++;
        }
        // Ensure maxTemp stays within the range (max 36°C)
        if (maxTemp < 36) {
          maxTemp++;
        }
      } else {
        // For Fahrenheit scale, limit minTemp to maxTemp and ensure it doesn't exceed 96°F
        if (minTemp < 96) {
          minTemp++;
        }
        // Ensure maxTemp stays within the range (max 96°F)
        if (maxTemp < 96) {
          maxTemp++;
        }
      }
      updateSelectedArea();
      break;
    case TEMPERATURE_SCALE:
      toggleTemperatureScale();
      break;
    default:
      break;
  }
}

// Decrease the value of the selected field
void decreaseValue() {
  switch (selectedField) {
    case CHARGING_TIME1:
      chargeStartMinute -= 15;
      if (chargeStartMinute < 0) {
        chargeStartMinute = 45;  // If minutes go below 0, set to 45
        chargingTimeHour1 = (chargingTimeHour1 - 1 + 24) % 24;  // Decrease the hour
      }
      updateSelectedArea();
      break;
    case CHARGING_TIME2:
      chargeEndMinute -= 15;
      if (chargeEndMinute < 0) {
        chargeEndMinute = 45;
        chargingTimeHour2 = (chargingTimeHour2 - 1 + 24) % 24;
      }
      updateSelectedArea();
      break;
    case HEATING_TIME1:
      heatingStartMinute -= 15;
      if (heatingStartMinute < 0) {
        heatingStartMinute = 45;
        heatingTimeHour1 = (heatingTimeHour1 - 1 + 24) % 24;
      }
      updateSelectedArea();
      break;
    case HEATING_TIME2:
      heatingEndMinute -= 15;
      if (heatingEndMinute < 0) {
        heatingEndMinute = 45;
        heatingTimeHour2 = (heatingTimeHour2 - 1 + 24) % 24;
      }
      updateSelectedArea();
      break;
    case TEMPERATURE_RANGE:
      if (isCelsius) {
        // For Celsius scale, limit minTemp between 20°C and maxTemp
        if (minTemp > 20) {
          minTemp--;
        }
        // Ensure maxTemp is not less than minTemp and does not exceed 36°C
        if (maxTemp > 36) {
          maxTemp--;
        }
      } else {
        // For Fahrenheit scale, limit minTemp between 68°F and maxTemp
        if (minTemp > 68) {
          minTemp--;
        }
        // Ensure maxTemp is not less than minTemp and does not exceed 96°F
        if (maxTemp > 96) {
          maxTemp--;
        }
      }
      updateSelectedArea();
      break;
    case TEMPERATURE_SCALE:
      toggleTemperatureScale();
      break;
    default:
      break;
  }
}

void changeInternalTemp(int newTemp) {  // meant to update the internal sand battery temperature
  if (showBattery) {
    // Display battery percentage
    float batteryPercent = (((float)newTemp - 37) / 463) * 100; // Example battery percentage
    int roundedPercent = batteryPercent;
    tft.setTextSize(4);  // Set the text size for the temperature display
    tft.setCursor(305, 105);
    tft.print(roundedPercent);
    tft.print("%");
  } else if (!showBattery) {
    tft.setTextSize(4);  // Set the text size for the temperature display
      if(isCelsius){
        tft.setCursor(300, 105);
        tft.print(newTemp);
        tft.print((char)247); // Degree symbol
        tft.print("C");
      }else{
        float temp = (newTemp * 1.8) + 32;
        tft.setCursor(300, 105);
        int temp2 = round(temp);
        tft.print(temp2);
        tft.print((char)247);  // Degree symbol
        tft.print("F");
        }
  }
}

void changeRoomTemp(int newTemp) {  // updates the room temperature variable, also checks values
  // Print the temperature to the screen in the designated area
  // For external temp
  tft.setCursor(85, 105);
  tft.setTextSize(4);  // Set the text size for the temperature display
  if (newTemp > 37) {
    tft.print("MAX");
    return;
  }

  if (isCelsius) {
    tft.print(newTemp);
    tft.print((char)247);  // Degree symbol
    tft.print("C");
  } else {
    float temp = (newTemp * 1.8) + 32;
    int temp2 = round(temp);
    tft.print(temp2);
    tft.print((char)247);  // Degree symbol
    tft.print("F");
  }
}

void printMain() {                         // prints main display
  tft.setRotation(1);                      // Set the orientation. Adjust as needed (0-3)
  tft.invertDisplay(true);                 // Invert display colors
  
  tft.fillScreen(TFT_WHITE); // Fill the screen with white color
  tft.fillRoundRect(5, 5, 470, 310, 25, TFT_BLACK); // Background
  tft.fillCircle(130, 120, 81, TFT_BLUE); // External temp circle
  tft.fillCircle(130, 120, 72, TFT_BLACK); // Inner black circle
  tft.fillCircle(350, 120, 81, TFT_RED); // Internal temp/battery circle
  tft.fillCircle(350, 120, 72, TFT_BLACK); // Inner black circle

  tft.drawSmoothRoundRect(20, 225, 20, 19, 140, 75, TFT_WHITE); // First button
  tft.drawSmoothRoundRect(170, 225, 20, 19, 140, 75, TFT_WHITE); // Second button
  tft.drawSmoothRoundRect(320, 225, 20, 19, 140, 75, TFT_WHITE); // Third button

  //Title project name
  tft.setTextSize(2); // Set the text size
  tft.setCursor(52, 12); // Set cursor position for title
  // tft.print("/// THERMAL DUNE ENERGY STORAGE");

  tft.setTextSize(2); // Set the text size for the buttons
  tft.setCursor(43, 255); // Set cursor position for settings
  tft.print("Settings");
  tft.setCursor(181, 245); // Set cursor position for Start/Stop
  tft.print("Start/Stop");
  tft.setCursor(195, 265); // Set cursor position for Charging
  tft.print("Charging");
  tft.setCursor(332, 245); // Set cursor position for Start/Stop
  tft.print("Start/Stop");
  tft.setCursor(350, 265); // Set cursor position for Heating
  tft.print("Heating");

  if(heatingRoom)
    heatCircle();
  if(chargingState)
    chargeCircle();
}

void printSettings() {
  heatingTimeHour1 = finalStartHeating;
  heatingTimeHour2 = finalEndHeating;
  heatingStartMinute = startHeatingMin;
  heatingEndMinute = endHeatingMin;
  chargingTimeHour1 = finalStartCharging;
  chargingTimeHour2 = finalEndCharging;
  chargeStartMinute = startChargingMin;
  chargeEndMinute = endChargingMin; 
  minTemp = finalTemp;

  tft.fillScreen(TFT_WHITE); // Fill the screen with white color
  tft.fillRoundRect(5, 5, 470, 310, 25, TFT_BLACK); // Background
  
  tft.drawSmoothRoundRect(15, 225, 20, 19, 140, 75, TFT_WHITE); // Save button
  tft.drawSmoothRoundRect(165, 225, 20, 19, 70, 75, TFT_WHITE); // Up button
  tft.drawSmoothRoundRect(245, 225, 20, 19, 70, 75, TFT_WHITE); // Down button
  tft.drawSmoothRoundRect(325, 225, 20, 19, 140, 75, TFT_WHITE); // Back button

  // Settings title
  tft.setTextSize(2); // Set the text size
  tft.setCursor(132, 12); // Set cursor position for title
  tft.print("/// USER SETTINGS");

  // Highlight charging time field if selected
  if (selectedField == CHARGING_TIME1) {
    tft.drawRect(190, 40, 95, 33, TFT_YELLOW); // Highlight charging time
    }  else if (selectedField == CHARGING_TIME2){
        tft.drawRect(320, 40, 95, 33, TFT_YELLOW); // Highlight charging time
    }

  // Editable fields
  tft.setTextSize(2); // Set the text size
  tft.setCursor(20, 50); // Set cursor position for charging time
  tft.print("CHARGING TIME:");
  tft.setCursor(195, 45);
  tft.setTextSize(3); // Set the text size
  tft.print(chargingTimeHour1);
  tft.print(":");
  tft.print(chargeStartMinute);
  tft.setCursor(280, 45);
  tft.print(" - ");
  tft.setCursor(325, 45);
  tft.print(chargingTimeHour2);
  tft.print(":");
  tft.print(chargeEndMinute);

  // Highlight heating time field if selected
  if (selectedField == HEATING_TIME1) {
    tft.drawRect(190, 85, 95, 33, TFT_YELLOW); // Highlight heating time
  } else if (selectedField == HEATING_TIME2){
        tft.drawRect(320, 85, 95, 33, TFT_YELLOW); // Highlight charging time
    }

  tft.setTextSize(2); // Set the text size
  tft.setCursor(20, 95); // Set cursor position for heating time
  tft.print("HEATING TIME:");
  tft.setCursor(195, 90);
  tft.setTextSize(3); // Set the text size
  tft.print(heatingTimeHour1);
  tft.print(":");
  tft.print(heatingStartMinute);
  tft.setCursor(280, 90);
  tft.print(" - ");
  tft.setCursor(325, 90);
  tft.print(heatingTimeHour2);
  tft.print(":");
  tft.print(heatingEndMinute);

  // Highlight temperature range if selected
  if (selectedField == TEMPERATURE_RANGE) {
    tft.drawRect(270, 130, 90, 33, TFT_YELLOW); // Highlight temperature range
  }

  tft.setTextSize(2); // Set the text size
  tft.setCursor(20, 140); // Set cursor position for temperature range
  tft.print("AMBIENT TEMPERATURE:");
  tft.setTextSize(3); // Set the text size for the buttons
  tft.setCursor(280, 135);
  tft.print(minTemp);
  tft.print((char)247); // Degree symbol
  tft.print(isCelsius ? "C" : "F");

  // Highlight temperature scale if selected
  if (selectedField == TEMPERATURE_SCALE) {
    tft.drawRect(245, 172, 42, 33, TFT_YELLOW); // Highlight temperature scale
  }

  tft.setTextSize(2); // Set the text size
  tft.setCursor(20, 185); // Set cursor position for temperature scale
  tft.print("TEMPERATURE SCALE:");
  tft.setTextSize(3); // Set the text size for the buttons
  tft.setCursor(250, 180);
  tft.print((char)247); // Degree symbol
  tft.print(isCelsius ? "C" : "F");

  tft.setTextSize(2); // Set the text size for the buttons
  tft.setCursor(65, 255); // Set cursor position for settings
  tft.print("SAVE");
  tft.setCursor(191, 255); // Set cursor position for settings
  tft.print("UP");
  tft.setCursor(258, 255); // Set cursor position for settings
  tft.print("DOWN");
  tft.setCursor(375, 255); // Set cursor position for Start/Stop
  tft.print("BACK");
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

String generateHTML() {
    String html = "<!DOCTYPE html><html><head><title>WiFi Setup</title></head><body>";
    html += "<h1>ESP32 Wi-Fi Setup</h1>";
    html += "<form action='/submit' method='post'>";
    html += "SSID: <input type='text' name='ssid'><br>";
    html += "Password: <input type='password' name='password'><br>";
    html += "<input type='submit' value='Save'>";
    html += "</form>";
    html += "</body></html>";
    return html;
}

void saveCredentials(const String& ssid, const String& password) {
    preferences.begin("wifi-creds", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();
    Serial.println("Wi-Fi credentials saved to preferences.");
}

void clearCredentials() {
    preferences.begin("wifi-creds", false);
    preferences.remove("ssid");
    preferences.remove("password");
    preferences.end();
    Serial.println("Wi-Fi credentials cleared from preferences.");
}

void settingSave(){
  finalStartHeating = heatingTimeHour1;
  finalEndHeating = heatingTimeHour2;
  finalStartCharging = chargingTimeHour1;
  finalEndCharging = chargingTimeHour2;
  finalTemp = minTemp;
  startHeatingMin = heatingStartMinute;
  endHeatingMin = heatingEndMinute;
  startChargingMin = chargeStartMinute;
  endChargingMin = chargeEndMinute;
  localScheduleFlag = true;
  Serial.print(finalStartCharging);
  Serial.print(":");
  Serial.print(chargeStartMinute);
  Serial.print(" - ");
  Serial.print(finalEndCharging);
  Serial.print(":");
  Serial.print(chargeEndMinute);
  Serial.println(" ");
  Serial.print(finalStartHeating);
  Serial.print(":");
  Serial.print(heatingStartMinute);
  Serial.print(" - ");
  Serial.print(finalEndHeating);
  Serial.print(":");
  Serial.print(heatingEndMinute);
  Serial.println(" ");
  // Serial.println("local schedule flag set to true");
}
