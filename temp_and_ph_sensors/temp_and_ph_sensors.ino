/*
 * DS18B20 Temperature + pH Sensor with Dual Firestore Storage - ESP32
 * 
 * This sketch:
 * 1. Connects to WiFi
 * 2. Connects to Firebase/Firestore
 * 3. Checks for active recipe ID and user ID from sensor_control/active_config
 * 4. Reads temperature from DS18B20 sensor (skips first reading)
 * 5. Reads pH from DFRobot Gravity Analog pH Sensor
 * 6. Uploads data to Firestore:
 *    - Temperature to: users/{userId}/Recipes/{recipeId}/temperature_readings & temperature_readings
 *    - pH to: users/{userId}/Recipes/{recipeId}/ph_readings & ph_readings
 * 
 * Hardware Connections (ESP32):
 * - DS18B20 Red Wire    -> 3V3 (3.3V power pin)
 * - DS18B20 Black Wire  -> GND (Ground)
 * - DS18B20 Yellow Wire -> GPIO4
 * - pH Sensor Po (blue) -> GPIO34 (ADC1 pin)
 * - pH Sensor Power (orange) -> 3V3
 * - pH Sensor GND -> GND
 */

#include <WiFiManager.h>
#include <Firebase_ESP_Client.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include "DFRobot_PH.h"
#include <EEPROM.h>

// Provide the token generation process info
#include "addons/TokenHelper.h"
// Provide the Firestore payload printing info
#include "addons/RTDBHelper.h"

// Firebase project credentials
#define API_KEY "AIzaSyAg1IFaz-1v6wzq3MMX7W6ryMdNtOWjVy8"
#define FIREBASE_PROJECT_ID "kombucha-monitor"

// User email and password for authentication
#define USER_EMAIL "sensor@kombucha.com"
#define USER_PASSWORD "sensor1234"

// Sensor pins
#define ONE_WIRE_BUS 4  // GPIO4 for DS18B20
#define PH_PIN 34       // GPIO34 (ADC1) for pH sensor

// pH sensor calibration values
#define CAL7 1742
#define CAL4 2370

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Temperature sensor objects
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// pH sensor objects
DFRobot_PH ph;
float phSlope = (7.0 - 4.0) / (CAL7 - CAL4);   // slope (m)
float phOffset = 7.0 - (phSlope * CAL7);        // offset (c)

// Timing variables
unsigned long lastReadingTime = 0;
const unsigned long readingInterval = 2000; // Read every 5 seconds

unsigned long lastConfigCheck = 0;
const unsigned long configCheckInterval = 2000;

// Active recipe and user tracking
String currentRecipeID = "";
String currentUserID = "";

// Sensor IDs
String tempSensorID = "ds18b20_waterproof_temperature_sensor";
String phSensorID = "dfrobot_gravity_ph_sensor";

// Flag to skip first reading
bool firstReadingDone = false;

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== Kombucha Temperature & pH Monitor ===");
  Serial.println();
  
  // Initialize EEPROM for pH sensor calibration data
  EEPROM.begin(32);
  
  // Enable internal pull-up resistor on the temperature sensor data pin
  pinMode(ONE_WIRE_BUS, INPUT_PULLUP);
  
  // Initialize temperature sensor
  sensors.begin();
  Serial.print("Found ");
  Serial.print(sensors.getDeviceCount());
  Serial.println(" temperature sensor(s)");
  
  // Display pH calibration values
  Serial.println("pH Sensor Calibration:");
  Serial.print("  Slope (m): ");
  Serial.println(phSlope, 6);
  Serial.print("  Offset (c): ");
  Serial.println(phOffset, 6);
  Serial.println();
  
  // Connect to WiFi
  WiFiManager wm;
  bool res;
  res = wm.autoConnect("AutoConnectAP", "kombucha");

  if(!res) {
    Serial.println("Failed to connect");
  } 
  else {
    //if you get here you have connected to the WiFi    
    Serial.println("connected...yeey :)");
  }

  // Configure NTP time synchronization
  configTime(-18000, 3600, "pool.ntp.org", "time.nist.gov");  // EST: -5 hours (-18000 sec), DST: +1 hour (3600 sec)
  Serial.print("Synchronizing time");
  while (time(nullptr) < 100000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println("Time synchronized!");
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.print("Current time: ");
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  }
  Serial.println();

  // Configure Firebase
  config.api_key = API_KEY;
  
  // Sign in with user credentials
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  // Assign the callback function for token generation task
  config.token_status_callback = tokenStatusCallback;

  // Initialize Firebase
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Firebase initialized!");
  Serial.println("Waiting for Firebase authentication...");
  Serial.println();
  
  // Wait for Firebase to be ready
  while (!Firebase.ready()) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println("Firebase connected and authenticated!");
  Serial.println();
  
  // Check for active recipe immediately on startup
  checkCurrentRecipe();
  
  Serial.println("Starting sensor monitoring...");
  Serial.println("===================================");
  Serial.println();
}

void loop() {
  // Periodically check for active recipe changes
  if (millis() - lastConfigCheck >= configCheckInterval) {
    lastConfigCheck = millis();
    checkCurrentRecipe();
  }
  
  // Check if it's time to take a reading
  if (millis() - lastReadingTime >= readingInterval) {
    lastReadingTime = millis();
    
    // ===== READ TEMPERATURE =====
    sensors.requestTemperatures();
    float temperatureC = sensors.getTempCByIndex(0);
    float temperatureF = sensors.getTempFByIndex(0);
    
    // ===== READ pH =====
    int analogValue = analogRead(PH_PIN);
    float voltage = analogValue * (3.3 / 4095.0);   // ESP32 ADC voltage
    float phValue = phSlope * analogValue + phOffset;
    
    // Check if temperature reading was successful
    if (temperatureC != DEVICE_DISCONNECTED_C) {
      // Skip the first reading after startup
      if (!firstReadingDone) {
        Serial.println("First reading (skipped for stability):");
        Serial.print("  Temperature: ");
        Serial.print(temperatureC);
        Serial.print("°C  |  ");
        Serial.print(temperatureF);
        Serial.println("°F");
        Serial.print("  pH Raw ADC: ");
        Serial.print(analogValue);
        Serial.print("  |  Voltage: ");
        Serial.print(voltage, 3);
        Serial.print(" V  |  pH: ");
        Serial.println(phValue, 2);
        Serial.println("Not uploaded - waiting for stable reading");
        Serial.println();
        firstReadingDone = true;
        return; // Skip this reading
      }
      
      // Display readings
      Serial.println("Sensor Readings:");
      Serial.print("  Temperature: ");
      Serial.print(temperatureC);
      Serial.print("°C  |  ");
      Serial.print(temperatureF);
      Serial.println("°F");
      Serial.print("  pH Raw ADC: ");
      Serial.print(analogValue);
      Serial.print("  |  Voltage: ");
      Serial.print(voltage, 3);
      Serial.print(" V  |  pH: ");
      Serial.println(phValue, 2);
      Serial.println();
      
      // Upload to Firestore if ready
      if (Firebase.ready()) {
        uploadTemperatureToFirestore(temperatureC, temperatureF);
        uploadPhToFirestore(phValue, voltage, analogValue);
      }
    } else {
      Serial.println("Error: Could not read temperature data");
    }
  }
}

void checkCurrentRecipe() {
  if (!Firebase.ready()) {
    Serial.println("Firebase not ready - skipping recipe check");
    return;
  }
  
  Serial.print("Checking for active recipe... ");
  
  String docPath = "sensor_control/active_config";
  
  if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", docPath.c_str())) {
    // Parse the response
    FirebaseJson json;
    json.setJsonData(fbdo.payload().c_str());
    FirebaseJsonData result;
    
    // Try to get the active_recipe_id field
    String newRecipeID = "";
    String newUserID = "";
    
    if (json.get(result, "fields/active_recipe_id/stringValue")) {
      newRecipeID = result.stringValue;
    }
    
    // Try to get the active_user_id field
    if (json.get(result, "fields/active_user_id/stringValue")) {
      newUserID = result.stringValue;
    }
    
    // Check if both IDs are present
    if (newRecipeID != "" && newUserID != "") {
      if (newRecipeID != currentRecipeID || newUserID != currentUserID) {
        currentRecipeID = newRecipeID;
        currentUserID = newUserID;
        Serial.println("UPDATED");
        Serial.println("Active user ID: " + currentUserID);
        Serial.println("Active recipe ID: " + currentRecipeID);
      } else {
        Serial.println("NO CHANGE");
        Serial.println("Active user ID: " + currentUserID);
        Serial.println("Active recipe ID: " + currentRecipeID);
      }
    } else {
      // Missing one or both IDs
      if (currentRecipeID != "" || currentUserID != "") {
        currentRecipeID = "";
        currentUserID = "";
        Serial.println("CLEARED");
        Serial.println("No active recipe/user");
      } else {
        Serial.println("NONE");
        Serial.println("No active recipe/user set");
      }
    }
  } else {
    Serial.println("FAILED");
    Serial.println("Reason: " + fbdo.errorReason());
    Serial.println("(Document may not exist yet - waiting for Android app to set recipe)");
  }
  Serial.println();
}

void uploadTemperatureToFirestore(float tempC, float tempF) {
  // Check if there's an active recipe and user
  if (currentRecipeID == "" || currentUserID == "" || 
      currentRecipeID.length() == 0 || currentUserID.length() == 0) {
    Serial.println("Temperature Upload: No active recipe/user - skipping");
    return;
  }
  
  Serial.println("Uploading TEMPERATURE to dual collections...");

  struct tm timeinfo;
  time_t now = time(nullptr);
  getLocalTime(&timeinfo);
  
  // Create timestamp string
  char timeString[64];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  
  // Create a FirebaseJson object for the document
  FirebaseJson temperature;
  
  // Add fields to the document
  temperature.set("fields/temperature_c/doubleValue", tempC);
  temperature.set("fields/temperature_f/doubleValue", tempF);
  temperature.set("fields/sensor_id/stringValue", tempSensorID);
  temperature.set("fields/recipe_id/stringValue", currentRecipeID);
  temperature.set("fields/user_id/stringValue", currentUserID);
  temperature.set("fields/timestamp/stringValue", String(timeString));
  
  // Upload to FIRST location: users/{userId}/Recipes/{recipeId}/temperature_readings
  String recipeSpecificPath = "users/" + currentUserID + "/Recipes/" + currentRecipeID + "/temperature_readings";
  
  Serial.print("Recipe-specific... ");
  if (Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "", recipeSpecificPath.c_str(), temperature.raw())) {
    Serial.println("SUCCESS");
    Serial.printf("        Path: users/%s/Recipes/%s/temperature_readings\n", 
                  currentUserID.c_str(), currentRecipeID.c_str());
  } else {
    Serial.println("FAILED");
    Serial.println("        Reason: " + fbdo.errorReason());
  }
  Serial.println();
}

void uploadPhToFirestore(float phValue, float voltage, int rawAdc) {
  // Check if there's an active recipe and user
  if (currentRecipeID == "" || currentUserID == "" || 
      currentRecipeID.length() == 0 || currentUserID.length() == 0) {
    Serial.println("pH Upload: No active recipe/user - skipping");
    return;
  }
  
  Serial.println("Uploading pH to dual collections...");

  struct tm timeinfo;
  time_t now = time(nullptr);
  getLocalTime(&timeinfo);
  
  // Create timestamp string
  char timeString[64];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  
  // Create a FirebaseJson object for the document
  FirebaseJson phData;
  
  // Add fields to the document
  phData.set("fields/ph_value/doubleValue", phValue);
  phData.set("fields/sensor_id/stringValue", phSensorID);
  phData.set("fields/recipe_id/stringValue", currentRecipeID);
  phData.set("fields/user_id/stringValue", currentUserID);
  phData.set("fields/timestamp/stringValue", String(timeString));
  
  // Upload to FIRST location: users/{userId}/Recipes/{recipeId}/ph_readings
  String recipeSpecificPath = "users/" + currentUserID + "/Recipes/" + currentRecipeID + "/ph_readings";
  
  Serial.print("Recipe-specific... ");
  if (Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "", recipeSpecificPath.c_str(), phData.raw())) {
    Serial.println("SUCCESS");
    Serial.printf("        Path: users/%s/Recipes/%s/ph_readings\n", 
                  currentUserID.c_str(), currentRecipeID.c_str());
  } else {
    Serial.println("FAILED");
    Serial.println("        Reason: " + fbdo.errorReason());
  }
  Serial.println();
}
