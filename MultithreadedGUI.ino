#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include <FT6336U.h>
#include <DHT11.h> // Include the DHT library
// #include "esp_task_wdt.h
#include "MAX6675.h"



// Pin definitions for DHT11
#define DHTPIN 5      // Pin connected to the first DHT11 data pin
#define DHTPIN2 16     // Pin connected to the second DHT11 data pin

// Pin definitions for TFT display
#define TFT_MISO 19
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4 // Or set to -1 if connected to ESP32 reset line

// Pin definitions for FT6336U touch controller
#define CPT_SDA 21
#define CPT_SCL 22
#define CPT_RST 12
#define CPT_INT 13

// SPI connection for k-type thermocouple
#define kTypeCS 25  // Chip Select pin for the thermocouple
#define kTypeSO 19  // MISO pin for the thermocouple
#define kTypeSCK 18 // SCK pin for the thermocouple (use the same as HSPI_CLK)

// Setting global variables
int roomTemp = 0;
int temp1 = 0;
int temp2 = 0;

int screenStatus = 0; // 0 - main screeen/ 1 - settings screen

// Creating two DHT11 instances, one for each sensor
DHT11 dht1(DHTPIN);
DHT11 dht2(DHTPIN2);

FT6336U ft6336u(CPT_SDA, CPT_SCL, CPT_RST, CPT_INT);
TFT_eSPI tft = TFT_eSPI(); // Create a TFT_eSPI object
MAX6675 thermoCouple(kTypeCS, kTypeSO, kTypeSCK); // Corrected MAX6675 instantiation

void setup() {
  Serial.begin(115200); // Initialize serial communication at 115200 baud

  tft.begin(); // Initialize the TFT display

  ft6336u.begin(); // Initialize the FT6336U touch controller
  printMain();

  xTaskCreate(&touchInterface, "touchInterface", 1512, NULL,1,NULL);
  xTaskCreate(&testThread, "testThread",5000, NULL, 2, NULL);
}

void loop() {}

void changeInternalTemp(int newTemp){
  
}

void changeRoomTemp(int newTemp) {
  // Print the temperature to the screen in the designated area
  // For external temp
  if(newTemp > 100)
    return;
  tft.setCursor(85, 105);
  tft.setTextSize(4); // Set the text size for the temperature display
  tft.print(newTemp);
  tft.print((char)247); // Degree symbol
  tft.print("C");
}

void printMain(){ // prints main display
    tft.setRotation(1); // Set the orientation. Adjust as needed (0-3)
  tft.invertDisplay(true); // Invert display colors
  tft.fillScreen(TFT_BLACK); // Fill the screen with black color
  tft.setTextColor(TFT_WHITE, TFT_BLACK); // Set the text color to white with a black background

  // Draw a simple shape on the screen for testing
  tft.fillCircle(130, 120, 80, TFT_BLUE); // Draw a blue circle in the center
  tft.drawCircle(130, 120, 81, TFT_WHITE); // Draw a white circle line
  tft.fillCircle(130, 120, 73, TFT_BLACK); // Draw a black circle in the center of the blue circle
  tft.drawCircle(130, 120, 74, TFT_WHITE); // Draw a white circle line
  tft.fillCircle(350, 120, 80, TFT_RED); // Draw a red circle in the center
  tft.drawCircle(350, 120, 81, TFT_WHITE); // Draw a white circle line
  tft.fillCircle(350, 120, 73, TFT_BLACK); // Draw a black circle in the center of the red circle
  tft.drawCircle(350, 120, 74, TFT_WHITE); // Draw a white circle line

  tft.drawSmoothRoundRect(20, 225, 20, 19, 140, 75, TFT_WHITE); // first button
  tft.drawSmoothRoundRect(170, 225, 20, 19, 140, 75, TFT_WHITE); // second button
  tft.drawSmoothRoundRect(320, 225, 20, 19, 140, 75, TFT_WHITE); // third button

  tft.setTextSize(2); // Set the text size for the buttons
  tft.setCursor(43, 255); // Set cursor position for settings
  tft.print("Settings");
  tft.setCursor(181, 245); // Set cursor position for Start/Stop Charging
  tft.print("Start/Stop");
  tft.setCursor(195, 265); // Set cursor position for Start/Stop Charging
  tft.print("Charging");
  tft.setCursor(332, 245); // Set cursor position for Start/Stop Heating
  tft.print("Start/Stop");
  tft.setCursor(350, 265); // Set cursor position for Start/Stop Heating
  tft.print("Heating");
}

void printSettings() {
  // tft.fillScreen(TFT_WHITE); // Fill the screen with white color
  tft.fillRoundRect(5, 5, 470, 310, 25, TFT_BLACK); // Background
  
  tft.drawSmoothRoundRect(70, 225, 20, 19, 140, 75, TFT_WHITE); // First button
  tft.drawSmoothRoundRect(270, 225, 20, 19, 140, 75, TFT_WHITE); // Second button

  //Settings title
  tft.setTextSize(2); // Set the text size
  tft.setCursor(132, 12); // Set cursor position for title
  tft.print("/// USER SETTINGS");

  //Setting Options
  tft.setTextSize(2); // Set the text size for the buttons
  tft.setCursor(20, 45); // Set cursor position for settings
  tft.print("Charging Time: ");
  tft.setCursor(20, 90); // Set cursor position for Start/Stop
  tft.print("Heating Time: ");
  tft.setCursor(20, 135); // Set cursor position for settings
  tft.print("Ambient Temperature: ");
  tft.setCursor(20, 180); // Set cursor position for Start/Stop
  tft.print("Temperature Scale: ");

  tft.setTextSize(2); // Set the text size for the buttons
  tft.setCursor(118, 255); // Set cursor position for settings
  tft.print("SAVE");
  tft.setCursor(318, 255); // Set cursor position for Start/Stop
  tft.print("BACK");
}

void touchInterface(void *pvParameter){
  while(1){
      if(ft6336u.read_td_status()){ // if touched
        int x = ft6336u.read_touch1_x();
        int y = ft6336u.read_touch1_y();
        Serial.print("FT6336U Touch Position 1: ("); // has the coordinates
        Serial.print(x);
        Serial.print(" , ");
        Serial.print(y);
        Serial.println(")");
        if (x >= 20 && x <= 160 && y >= 50 && y <= 125 && screenStatus == 0) { //screenStatus is a global variable, when screen status == 0 this means we're on the main menu, and when screenStatus == 1 then we are in the settings menu
          screenStatus = 1; // global variable set to settings screen
          printSettings(); // uses function to print the settings screen to the display
        }

        if(x <= 80 && x >= 0 && y <= 400 && y >= 276 && screenStatus == 1){ // screenStatus being 1 means display is currently showing settings screen, this if checks if the back button is pressed in the settings menu
          screenStatus = 0; // set screen status back to main display
          printMain(); // main screen GUI is then printed to the screen
        }


        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
  }
}

void testThread(void *pvParameter){ // this makes no fucking sense
// esp_task_wdt_delete(NULL);
  while(1){
    int temp1 = dht1.readTemperature();
    int temp2 = dht2.readTemperature();
    int avgTemp = (temp1 + temp2) /2;
    
    // Print stack usage for debugging
    UBaseType_t uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    Serial.print("DHT High Water Mark: ");
    Serial.println(uxHighWaterMark);

    // Serial.print("temp1 - ");
    // Serial.println(temp1);

    // Serial.print("temp2 - ");
    // Serial.println(temp2);

    // Serial.print("Average temperature value: ");
    // Serial.print(avgTemp);

    if(screenStatus == 0)
      changeRoomTemp(avgTemp); // update the value on the screen with the average value
    
    vTaskDelay(1000 / portTICK_PERIOD_MS); // not interested in checking the temperatures constantly
  }
}
