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

int internalTemp1 = 0;
int internalTemp2 = 0;
int avgInternalTemp = 0;
// bool celciusTemp = true;

bool heatingToggle = false;
bool heatingRoom = false;
bool showBattery = false; // Flag for battery

bool chargingState = false;

int screenStatus = 0;  // 0 - main screeen/ 1 - settings screen

int finalStartHeating = 0;
int finalEndHeating = 0;
int finalStartCharging = 0;
int finalEndCharging = 0;
int finalTemp = 20; // preferred room temperature

// all these variables are basically temporary
int heatingTimeHour1 = 0, heatingTimeHour2 = 0, heatingTimeMinutes = 0;     // Heating time
int chargingTimeHour1 = 0, chargingTimeHour2 = 0, chargingTimeMinutes = 0;  // Charging time
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

void setup() {
  Serial.begin(115200);  // Initialize serial communication at 115200 baud
  
  pinMode(kTypeSO, INPUT);  // Set GPIO12 as input initially to avoid conflicts during boot
  pinMode(heatOn, OUTPUT);
  pinMode(heatOff, OUTPUT);
  pinMode(powerPin, OUTPUT);

  thermoCouple1.begin();
  thermoCouple2.begin();

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
  xTaskCreate(&heater, "heater", 3000, NULL, 1, NULL);
}

void loop() {}

void editTime(int &hours) { 
  // This function will allow the user to edit the time
  hours = (hours + 1) % 24; // Example logic to increment hours
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
            tft.fillRect(190, 40, 93, 33, TFT_BLACK);
            tft.setTextSize(3);
            tft.setCursor(195, 45);
            tft.print(chargingTimeHour1);
            tft.print(":");
            tft.print("00");
            highlightSelectedArea(); // Ensure the area remains highlighted
            delay(50);
            break;

        case CHARGING_TIME2:
            // Clear the area and redraw the updated time
            tft.fillRect(320, 40, 92, 33, TFT_BLACK);
            tft.setTextSize(3);
            tft.setCursor(325, 45);
            tft.print(chargingTimeHour2);
            tft.print(":");
            tft.print("00");
            highlightSelectedArea(); // Ensure the area remains highlighted
            delay(50);
            break;

            case HEATING_TIME1:
            // Clear the area and redraw the updated time
            tft.fillRect(190, 85, 93, 33, TFT_BLACK);
            tft.setTextSize(3);
            tft.setCursor(195, 90);
            tft.print(heatingTimeHour1);
            tft.print(":");
            tft.print("00");
            highlightSelectedArea(); // Ensure the area remains highlighted
            delay(50);
            break;

        case HEATING_TIME2:
            // Clear the area and redraw the updated time
            tft.fillRect(320, 85, 92, 33, TFT_BLACK);
            tft.setTextSize(3);
            tft.setCursor(325, 90);
            tft.print(heatingTimeHour2);
            tft.print(":");
            tft.print("00");
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

// Increase the value of the selected field
void increaseValue() {
  switch (selectedField) {
    case CHARGING_TIME1:
      chargingTimeHour1 = (chargingTimeHour1 + 1) % 24;
      updateSelectedArea();
      break;
    case CHARGING_TIME2:
      chargingTimeHour2 = (chargingTimeHour2 + 1) % 24;
      updateSelectedArea();
      break;
    case HEATING_TIME1:
      heatingTimeHour1 = (heatingTimeHour1 + 1) % 24;
      updateSelectedArea();
      break;
    case HEATING_TIME2:
      heatingTimeHour2 = (heatingTimeHour2 + 1) % 24;
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
      chargingTimeHour1 = (chargingTimeHour1 - 1 + 24) % 24;
      updateSelectedArea();
      break;
    case CHARGING_TIME2:
      chargingTimeHour2 = (chargingTimeHour2 - 1 + 24) % 24;
      updateSelectedArea();
      break;
    case HEATING_TIME1:
      heatingTimeHour1 = (heatingTimeHour1 - 1 + 24) % 24;
      updateSelectedArea();
      break;
    case HEATING_TIME2:
      heatingTimeHour2 = (heatingTimeHour2 - 1 + 24) % 24;
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
  tft.print("/// THERMAL DUNE ENERGY STORAGE");

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
}

void printSettings() {
  heatingTimeHour1 = finalStartHeating;
  heatingTimeHour2 = finalEndHeating;
  chargingTimeHour1 = finalStartCharging;
  chargingTimeHour2 = finalEndCharging;
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
    tft.drawRect(190, 40, 92, 33, TFT_YELLOW); // Highlight charging time
    }  else if (selectedField == CHARGING_TIME2){
        tft.drawRect(320, 40, 92, 33, TFT_YELLOW); // Highlight charging time
    }

  // Editable fields
  tft.setTextSize(2); // Set the text size
  tft.setCursor(20, 50); // Set cursor position for charging time
  tft.print("CHARGING TIME:");
  tft.setCursor(195, 45);
  tft.setTextSize(3); // Set the text size
  tft.print(chargingTimeHour1);
  tft.print(":");
  tft.print("00");
  tft.print(" - ");
  tft.print(chargingTimeHour2);
  tft.print(":");
  tft.print("00");

  // Highlight heating time field if selected
  if (selectedField == HEATING_TIME1) {
    tft.drawRect(190, 85, 90, 33, TFT_YELLOW); // Highlight heating time
  } else if (selectedField == HEATING_TIME2){
        tft.drawRect(320, 85, 92, 33, TFT_YELLOW); // Highlight charging time
    }

  tft.setTextSize(2); // Set the text size
  tft.setCursor(20, 95); // Set cursor position for heating time
  tft.print("HEATING TIME:");
  tft.setCursor(195, 90);
  tft.setTextSize(3); // Set the text size
  tft.print(heatingTimeHour1);
  tft.print(":");
  tft.print("00");
  tft.print(" - ");
  tft.print(heatingTimeHour2);
  tft.print(":");
  tft.print("00");

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
  roomTemp = (roomTemp1 + roomTemp2) / 2; // average room temperature

  avgInternalTemp = (internalTemp1 + internalTemp2) / 2; // average internal temperature
    if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
      if(screenStatus == 0){
        changeInternalTemp(avgInternalTemp);
        changeRoomTemp(roomTemp);
      }
      xSemaphoreGive(xMutex);

      }

  vTaskDelay(500 / portTICK_PERIOD_MS);
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

void settingSave(){
  finalStartHeating = heatingTimeHour1;
  finalEndHeating = heatingTimeHour2;
  finalStartCharging = chargingTimeHour1;
  finalEndCharging = chargingTimeHour2;
  finalTemp = minTemp;
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
        if (screenStatus == 0) {
          if (x < 100 && x > 0 && y > 173 && y < 320) {  // toggles charging state of battery
            chargingState = !chargingState;
            chargeFunction();
            Serial.println("Start/Stop Charging");
          }
          if (x < 100 && x > 0 && y > 320 && y < 480) {  // toggles heating
            Serial.println("Start/Stop Heating");
            heatingToggle = true;  // toggle so heating loop only runs once
            heatingRoom = !heatingRoom;
          }
          if(y < 440 && y > 280 && x < 277 && x > 110){
            Serial.println("toggle internal temperature");
            tft.fillCircle(350, 120, 72, TFT_BLACK);  // Clear the circle area
            showBattery = !showBattery;
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
