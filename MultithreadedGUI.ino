#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include <FT6336U.h>
#include <DHT11.h>  // Include the DHT library
// #include "esp_task_wdt.h
#include "MAX6675.h"

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
#define CPT_RST 32
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

// #define TOUCH_RESET_TIMEOUT 5000


SemaphoreHandle_t xMutex;

// Setting global variables
int roomTemp = 0;
int roomTempF = (roomTemp * 1.8) + 32;
int temp1 = 0;
int temp2 = 0;
bool celciusTemp = true;

bool heatingToggle = false;
bool heatingRoom = false;

bool chargingState = false;

int screenStatus = 0;  // 0 - main screeen/ 1 - settings screen

int heatingTimeHour1 = 0, heatingTimeHour2 = 0, heatingTimeMinutes = 0;     // Heating time
int chargingTimeHour1 = 0, chargingTimeHour2 = 0, chargingTimeMinutes = 0;  // Charging time
int maxTemp = 36;                                                           // Temperature range
bool isCelsius = true;                                                      // Temperature scale

// Creating two DHT11 instances, one for each sensor
DHT11 dht1(DHTPIN);
DHT11 dht2(DHTPIN2);

FT6336U ft6336u(CPT_SDA, CPT_SCL, CPT_RST, CPT_INT);
TFT_eSPI tft = TFT_eSPI();  // Create a TFT_eSPI object

MAX6675 thermoCouple1(kTypeCS1, kTypeSO, kTypeSCK);  // First thermocouple
MAX6675 thermoCouple2(kTypeCS2, kTypeSO, kTypeSCK);  // Second thermocouple

void setup() {
  Serial.begin(115200);  // Initialize serial communication at 115200 baud
    pinMode(kTypeSO, INPUT);  // Set GPIO12 as input initially to avoid conflicts during boot

  pinMode(heatOn, OUTPUT);
  pinMode(heatOff, OUTPUT);
  pinMode(powerPin, OUTPUT);

  thermoCouple1.begin();
  thermoCouple2.begin();

    // Initialize I2C bus with the SDA and SCL pins
  // Wire.begin(CPT_SDA, CPT_SCL);

  // // Reduce the I2C speed to 100kHz for more stable communication
  // Wire.setClock(1000000);  // 100kHz I2C speed

  tft.begin();  // Initialize the TFT display

  ft6336u.begin();  // Initialize the FT6336U touch controller
  printMain();

  xMutex = xSemaphoreCreateMutex();

  if (xMutex == NULL) {
    Serial.println("Mutex creation failed");
    while (1)
      ;
  }
    pinMode(kTypeSO, INPUT_PULLUP);  // Configure GPIO12 for MISO after boot



  xTaskCreate(&touchInterface, "touchInterface", 1512, NULL, 1, NULL);
  xTaskCreate(&internalTemp, "internalTemp", 2000, NULL, 2, NULL);
  xTaskCreate(&testThread, "testThread", 3000, NULL, 2, NULL);
  xTaskCreate(&heater, "heater", 3000, NULL, 1, NULL);
}

void loop() {}

void changeInternalTemp(int newTemp) {  // meant to update the internal sand battery temperature
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
  // tft.print(newTemp);
  if (celciusTemp) {
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
  tft.fillScreen(TFT_BLACK);               // Fill the screen with black color
  tft.setTextColor(TFT_WHITE, TFT_BLACK);  // Set the text color to white with a black background

  // Draw a simple shape on the screen for testing
  tft.fillCircle(130, 120, 80, TFT_BLUE);   // Draw a blue circle in the center
  tft.drawCircle(130, 120, 81, TFT_WHITE);  // Draw a white circle line
  tft.fillCircle(130, 120, 73, TFT_BLACK);  // Draw a black circle in the center of the blue circle
  tft.drawCircle(130, 120, 74, TFT_WHITE);  // Draw a white circle line
  tft.fillCircle(350, 120, 80, TFT_RED);    // Draw a red circle in the center
  tft.drawCircle(350, 120, 81, TFT_WHITE);  // Draw a white circle line
  tft.fillCircle(350, 120, 73, TFT_BLACK);  // Draw a black circle in the center of the red circle
  tft.drawCircle(350, 120, 74, TFT_WHITE);  // Draw a white circle line

  tft.drawSmoothRoundRect(20, 225, 20, 19, 140, 75, TFT_WHITE);   // first button
  tft.drawSmoothRoundRect(170, 225, 20, 19, 140, 75, TFT_WHITE);  // second button
  tft.drawSmoothRoundRect(320, 225, 20, 19, 140, 75, TFT_WHITE);  // third button

  tft.setTextSize(2);      // Set the text size for the buttons
  tft.setCursor(43, 255);  // Set cursor position for settings
  tft.print("Settings");
  tft.setCursor(181, 245);  // Set cursor position for Start/Stop Charging
  tft.print("Start/Stop");
  tft.setCursor(195, 265);  // Set cursor position for Start/Stop Charging
  tft.print("Charging");
  tft.setCursor(332, 245);  // Set cursor position for Start/Stop Heating
  tft.print("Start/Stop");
  tft.setCursor(350, 265);  // Set cursor position for Start/Stop Heating
  tft.print("Heating");
}

void printSettings() {
  tft.fillScreen(TFT_WHITE);                         // Fill the screen with white color
  tft.fillRoundRect(5, 5, 470, 310, 25, TFT_BLACK);  // Background

  tft.drawSmoothRoundRect(70, 225, 20, 19, 140, 75, TFT_WHITE);   // First button
  tft.drawSmoothRoundRect(270, 225, 20, 19, 140, 75, TFT_WHITE);  // Second button

  //Settings title
  tft.setTextSize(2);      // Set the text size
  tft.setCursor(132, 12);  // Set cursor position for title
  tft.print("/// USER SETTINGS");

  // Editable fields
  tft.setTextSize(2);     // Set the text size
  tft.setCursor(20, 45);  // Set cursor position for charging time
  tft.print("Charging Time: ");
  tft.fillRect(200, 43, 180, 20, TFT_BLACK);
  tft.setCursor(210, 45);
  tft.print(chargingTimeHour1);
  tft.print(":");
  tft.print(chargingTimeMinutes);
  tft.print(" - ");
  tft.print(chargingTimeHour1);
  tft.print(":");
  tft.print(chargingTimeMinutes);

  tft.setCursor(20, 90);  // Set cursor position for heating time
  tft.print("Heating Time: ");
  tft.fillRect(200, 88, 150, 20, TFT_BLACK);
  tft.setCursor(210, 90);
  tft.print(heatingTimeHour1);
  tft.print(":");
  tft.print(heatingTimeMinutes);
  tft.print(" - ");
  tft.print(heatingTimeHour2);
  tft.print(":");
  tft.print(heatingTimeMinutes);

  tft.setCursor(20, 135);  // Set cursor position for temperature range
  tft.print("Ambient Temperature: ");
  tft.fillRect(270, 133, 100, 20, TFT_BLACK);
  tft.setCursor(280, 135);
  tft.print(maxTemp);
  tft.print(" ");
  tft.print(isCelsius ? "C" : "F");
  tft.print((char)247);  // Degree symbol

  tft.setCursor(20, 180);  // Set cursor position for temperature scale
  tft.print("Temperature Scale: ");
  tft.fillRect(240, 178, 50, 20, TFT_BLACK);
  tft.setCursor(250, 180);
  tft.print(isCelsius ? "C" : "F");
  tft.print((char)247);  // Degree symbol

  tft.setTextSize(2);       // Set the text size for the buttons
  tft.setCursor(118, 255);  // Set cursor position for settings
  tft.print("SAVE");
  tft.setCursor(318, 255);  // Set cursor position for Start/Stop
  tft.print("BACK");
}


void internalTemp(void *pvParameter){
  while(1){
  // Read temperature from first thermocouple
  int status1 = thermoCouple1.read();
  float temp1 = thermoCouple1.getTemperature();


  Serial.print("Thermocouple 1 - Status: ");
  Serial.print(status1);
  Serial.print(" Temperature: ");
  Serial.println(temp1);

  // Read temperature from second thermocouple

  int status2 = thermoCouple2.read();
  float temp2 = thermoCouple2.getTemperature();
  Serial.print("Thermocouple 2 - Status: ");
  Serial.print(status2);
  Serial.print(" Temperature: ");
  Serial.println(temp2);

  vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

void turnOnHeat() {
  heatingRoom = true;
  digitalWrite(heatOn, HIGH);
  delay(15000);
  digitalWrite(heatOn, LOW);
}

void turnOffHeat() {
  heatingRoom = false;
  digitalWrite(heatOff, HIGH);
  delay(15000);
  delay(10000);
  delay(10000);
  digitalWrite(heatOff, LOW);
}

void chargeFunction() {
  if (chargingState) {
    digitalWrite(powerPin, HIGH);
  } else {
    digitalWrite(powerPin, LOW);
  }
}

void heater(void *pvParameter) {  // responsible for heat scheduling
turnOffHeat(); // make sure default state is to off
  while (1) {
    if (heatingToggle) {
      if (heatingRoom) {
        Serial.println("turning on heating");
        turnOnHeat(); // TEMPORARILY DISABLED FOR THE TIME BEING PLEASE RETURN HERE SOON
      } else {
        Serial.println("turning off heating");
        turnOffHeat();
      }
    }
    heatingToggle = false;
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

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
        if (screenStatus == 0 && x <= 100 && x >= 0 && y <= 173 && y >= 0) {
          screenStatus = 1;  // global variable set to settings screen
          printSettings();   // uses function to print the settings screen to the display
          Serial.println("Settings Button");
        } else if (screenStatus == 1 && y > 280 && y < 400 && x < 100) {
          screenStatus = 0;
          printMain();
        }
        if (screenStatus == 0) {
          if (x < 100 && x > 0 && y > 173 && y < 280) {  // toggles charging state of battery
            chargingState = !chargingState;
            chargeFunction();
            Serial.println("Start/Stop Charing");
          }
          if (x < 100 && x > 0 && y > 280 && y < 480) {  // toggles heating
            Serial.println("Start/Stop Heating");
            heatingToggle = true;  // toggle so heating loop only runs once
            heatingRoom = !heatingRoom;
          }
          if (x > 138 && x < 259 && y > 58 && y < 200)  // toggle external temp measurement
            celciusTemp = !celciusTemp;
          changeRoomTemp(roomTemp);
          Serial.println("change temperature");
        }
        if (screenStatus == 1) {
          if (y > 80 && y < 200 && x < 100) {
            Serial.println("Save Button");
          }
          if (x > 240 && x < 260 && y < 243 && y > 216) {
            Serial.println("start temp");
          }
        }
        // Release the mutex after modifying screenStatus
        xSemaphoreGive(xMutex);
      }
      vTaskDelay(200 / portTICK_PERIOD_MS);
    }
  }
}

void testThread(void *pvParameter) {  // reading external temperature
                                      // esp_task_wdt_delete(NULL);
  while (1) {
    int temp1 = dht1.readTemperature();
    int temp2 = dht2.readTemperature();
    roomTemp = (temp1 + temp2) / 2;
    // Serial.println(roomTemp);
    // Serial.println(celciusTemp);

    // Serial.print("DHT #1: ");
    // Serial.println(temp1);

    // Serial.print("DHT #2: ");
    // Serial.println(temp2);

    // Print stack usage for debugging
    // UBaseType_t uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    // Serial.print("DHT High Water Mark: ");
    // Serial.println(uxHighWaterMark);

    // Lock the mutex before accessing screenStatus
    if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
      if (screenStatus == 0)
        changeRoomTemp(roomTemp);  // update the value on the screen with the average value
      // Release the mutex after reading screenStatus
      xSemaphoreGive(xMutex);
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);  // not interested in checking the temperatures constantly
  }
}
