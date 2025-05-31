#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME280.h>
#include <Adafruit_MPU6050.h>
#include <TinyGPS++.h>
#include <MFRC522.h>
#include <RTClib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// OLED Configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Wi-Fi Credentials
const char* ssid = "Honour's S20 FE";
const char* password = "welo003(";

// BME280
Adafruit_BME280 bme;

// MPU6050
Adafruit_MPU6050 mpu;

// GPS
HardwareSerial GPS(2);  // RX=16, TX=17
TinyGPSPlus gps;

// SD Card (VSPI)
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK 18
#define SD_CS 5
File logFile;
bool sdActive = false;

// RFID (HSPI)
#define RFID_MOSI 13
#define RFID_MISO 12
#define RFID_SCK 14
#define RC522_CS 4
#define RC522_RST 27
MFRC522 rfid(RC522_CS, RC522_RST);
bool rfidActive = false;

// RTC
RTC_DS3231 rtc;

// Buzzer
#define BUZZER_PIN 33

// Timing
const unsigned long LOG_INTERVAL = 1000;         // Log every 1s
const unsigned long RFID_CHECK_INTERVAL = 5000;  // Check RFID every 5s
const unsigned long DISPLAY_INTERVAL = 2000;     // Update display every 2s
unsigned long lastLogTime = 0;
unsigned long lastRFIDCheck = 0;
unsigned long lastDisplayUpdate = 0;

// Preferences
Preferences preferences;
  bool isLogging;
  bool displayNeedsUpdate;
  int s;
  int p;
  String c;
  String state;

// Sensor Data Structure
struct SensorData {
  float temperature;
  float humidity;
  float pressure;
  float accelX, accelY, accelZ;
  float gyroX, gyroY, gyroZ;
  double latitude, longitude;
  bool gpsValid;
  DateTime timestamp;
};

// Function Prototypes
void buzz(int frequency = 2000, int duration = 150);
void activateSD();
void activateRFID();
SensorData readSensors();
void logData(const SensorData& data);
void updateDisplay(const SensorData& data);

std::vector<String> readAndPrintRFID();
String extractText(byte* data, byte length);
void sendRequest(const std::vector<String>& data);

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  GPS.begin(9600, SERIAL_8N1, 16, 17);
  pinMode(BUZZER_PIN, OUTPUT);

  

  // Initialize Preferences
  preferences.begin("iot-device", false);

  // isLogging = preferences.putBool("isLogging", false);
  // displayNeedsUpdate = preferences.putBool("displayNeedsUpdate", true);
  // s = preferences.putInt("s", 0);
  // p = preferences.putInt("p", 0);
  // c = preferences.putString("c", "");
  // state = preferences.putString("state", "idle");

  isLogging = preferences.getBool("isLogging", false);
  displayNeedsUpdate = preferences.getBool("displayNeedsUpdate", true);
  s = preferences.getInt("s", 0);
  p = preferences.getInt("p", 0);
  c = preferences.getString("c", "");
  state = preferences.getString("state", "idle");




  // Initialize OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
    buzz(800, 400);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Initializing...");
  display.display();

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  display.println("Connecting to WiFi...");
  display.display();
  unsigned long wifiTimeout = millis() + 30000;  // 30s timeout
  while (WiFi.status() != WL_CONNECTED && millis() < wifiTimeout) {
    delay(500);
    display.print(".");
    display.display();
  }
  if (WiFi.status() == WL_CONNECTED) {
    display.println("Connected");
  } else {
    display.println("WiFi Failed");
    buzz(900, 400);
  }
  display.display();

  // Initialize BME280
  if (!bme.begin(0x76)) {
    Serial.println("BME280 not found!");
    display.println("BME280 Failed");
    display.display();
    buzz(1000, 400);
  }

  // Initialize MPU6050
  if (!mpu.begin()) {
    Serial.println("MPU6050 not found!");
    display.println("MPU6050 Failed");
    display.display();
    buzz(1100, 400);
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }

  // Initialize RTC
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
    display.println("RTC Failed");
    display.display();
    buzz(1000, 600);
  } else if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // Initialize SD Card
  activateSD();
  if (!SD.begin(SD_CS)) {
    Serial.println("SD init failed!");
    display.println("SD Failed");
    display.display();
    buzz(1200, 500);
  } else {
    logFile = SD.open("/data.json", FILE_WRITE);
    if (logFile) {
      logFile.println("{\"data\":[]}");
      logFile.close();
    }
  }

  // Initialize RFID
  activateRFID();
  rfid.PCD_Init();

  Serial.println("System Ready");
  buzz(2000, 150);
  display.clearDisplay();
  display.display();
}

void loop() {
  unsigned long currentMillis = millis();

  // Process GPS data
  while (GPS.available()) {
    gps.encode(GPS.read());
  }

  // Check RFID
  if (currentMillis - lastRFIDCheck >= RFID_CHECK_INTERVAL) {
    lastRFIDCheck = currentMillis;
    activateRFID();

    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      displayNeedsUpdate = true;
      buzz(isLogging ? 2000 : 800, 300);

      std::vector<String> data = readAndPrintRFID();
      sendRequest(data);

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
  }

  // Log sensor data
  if (isLogging && (currentMillis - lastLogTime >= LOG_INTERVAL)) {
    lastLogTime = currentMillis;
    SensorData data = readSensors();
    logData(data);
    displayNeedsUpdate = true;
  }

  // Update display
  if (displayNeedsUpdate && (currentMillis - lastDisplayUpdate >= DISPLAY_INTERVAL)) {
    lastDisplayUpdate = currentMillis;
    SensorData data = readSensors();
    updateDisplay(data);
    displayNeedsUpdate = false;
  }

  delay(1);
}

void buzz(int frequency, int duration) {
  tone(BUZZER_PIN, frequency, duration);
  delay(duration);
  noTone(BUZZER_PIN);
  delay(50);
}

void activateSD() {
  if (!sdActive) {
    SPI.end();
    delay(10);  // Ensure SPI is fully stopped
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sdActive = true;
    rfidActive = false;
  }
}

void activateRFID() {
  if (!rfidActive) {
    SPI.end();
    delay(10);  // Ensure SPI is fully stopped
    SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RC522_CS);
    rfid.PCD_Init();
    rfidActive = true;
    sdActive = false;
  }
}

SensorData readSensors() {
  SensorData data;
  data.temperature = bme.readTemperature();
  data.humidity = bme.readHumidity();
  data.pressure = bme.readPressure() / 100.0F;

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  data.accelX = a.acceleration.x;
  data.accelY = a.acceleration.y;
  data.accelZ = a.acceleration.z;
  data.gyroX = g.gyro.x;
  data.gyroY = g.gyro.y;
  data.gyroZ = g.gyro.z;

  data.gpsValid = gps.location.isValid();
  data.latitude = data.gpsValid ? gps.location.lat() : 0.0;
  data.longitude = data.gpsValid ? gps.location.lng() : 0.0;

  data.timestamp = rtc.now();
  return data;
}

void logData(const SensorData& data) {
    activateSD();
    
    // First, read existing data
    JsonDocument doc;
    File readFile = SD.open("/data.json", FILE_READ);
    if (readFile) {
        DeserializationError error = deserializeJson(doc, readFile);
        readFile.close();
        
        if (error) {
            Serial.print("Deserialization failed: ");
            Serial.println(error.c_str());
            // Initialize empty document if reading failed
            doc.clear();
            doc["data"] = JsonArray();
        }
    } else {
        // Initialize empty document if file doesn't exist
        doc["data"] = JsonArray();
    }

    // Add new data
    JsonObject logEntry = doc["data"].createNestedObject();
    logEntry["product_id"] = p;
    logEntry["lat"] = data.latitude;
    logEntry["long"] = data.longitude;
    logEntry["temp"] = data.temperature;
    logEntry["humid"] = data.humidity;
    logEntry["pressure"] = data.pressure;
    logEntry["accelX"] = data.accelX;
    logEntry["accelY"] = data.accelY;
    logEntry["accelZ"] = data.accelZ;
    logEntry["gyroX"] = data.gyroX;
    logEntry["gyroY"] = data.gyroY;
    logEntry["gyroZ"] = data.gyroZ;

      HTTPClient http;
      http.begin("https://tracui.pxxl.tech/api/logs");
      http.addHeader("Content-Type", "application/json");

      String jsonData;
      
      serializeJson(logEntry, jsonData);
      Serial.println(p);

      Serial.println(jsonData);
      int httpCode = http.POST(jsonData);
      if (httpCode > 0) {
        String response = http.getString();
        Serial.println("Raw JSON:");
        Serial.println(response);

        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, response);

        Serial.println(httpCode);

        if (!error) {
          Serial.println("Logged");

        } else {
          Serial.println("No LOGS");

        }
      } else {
        Serial.println("Yello");
      }
      http.end();

    // Write back to file - use FILE_WRITE which automatically truncates
    File writeFile = SD.open("/data.json", FILE_WRITE);
    if (writeFile) {
        serializeJsonPretty(doc, writeFile);  // Using pretty print for readability
        writeFile.close();
        Serial.println("Data logged successfully");
    } else {
        Serial.println("Failed to open file for writing");
        buzz(400, 300);
    }




}

void updateDisplay(const SensorData& data) {

  display.clearDisplay();
  display.setCursor(0, 0);

  if (isLogging) {
    display.print("T:");
    display.print(data.temperature, 1);
    display.println("C");
    display.print("H:");
    display.print(data.humidity, 0);
    display.println("%");
    display.print("P:");
    display.print(data.pressure, 0);
    display.println("hPa");

    if (data.gpsValid) {
      display.print("Lat:");
      display.println(data.latitude, 6);
      display.print("Lon:");
      display.println(data.longitude, 6);
    } else {
      display.println("No GPS Fix");
    }

    display.println("Logging: ON");
  } else if (state == "linked") {
    display.println("Product Linked");
    display.print("Code: ");
    display.println(c);
    display.println("Send to Logistics.");
  } else {
    display.println("System Ready");
    display.println("Waiting for RFID...");
    display.println("Logging: OFF");
  }

  display.display();
}

std::vector<String> readAndPrintRFID() {
  Serial.print(F("Card UID: "));
  for (byte i = 0; i < rfid.uid.size; i++) {
    Serial.print(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    Serial.print(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) Serial.print(":");
  }
  Serial.println();

  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  Serial.println(F("Text found on card:"));
  std::vector<String> texts;

  for (byte sector = 0; sector < 8; sector++) {
    MFRC522::StatusCode status = rfid.PCD_Authenticate(
      MFRC522::PICC_CMD_MF_AUTH_KEY_A, sector * 4 + 3, &key, &(rfid.uid));

    if (status != MFRC522::STATUS_OK) continue;

    for (byte blockAddr = sector * 4; blockAddr < (sector * 4) + 3; blockAddr++) {
      byte buffer[18];
      byte size = sizeof(buffer);

      status = rfid.MIFARE_Read(blockAddr, buffer, &size);
      if (status != MFRC522::STATUS_OK) continue;

      String text = extractText(buffer, 16);
      if (text.length() > 2) {
        texts.push_back(text);
      }
    }
  }

  return texts;
}

String extractText(byte* data, byte length) {
  String result = "";

  // Method 1: Look for NDEF text record
  for (byte i = 0; i < length - 5; i++) {
    if (data[i] == 0x91 && data[i + 1] == 0x01) {
      byte payloadLength = data[i + 2];
      if (i + 3 + payloadLength <= length) {
        byte langLength = data[i + 3] & 0x3F;
        for (byte j = i + 4 + langLength; j < i + 3 + payloadLength && j < length; j++) {
          if (data[j] >= 32 && data[j] <= 126) {
            result += (char)data[j];
          }
        }
        if (result.length() > 0) return result;
      }
    }
  }

  // Method 2: Look for printable characters
  for (byte i = 0; i < length; i++) {
    if (data[i] >= 32 && data[i] <= 126) {
      String temp = "";
      while (i < length && data[i] >= 32 && data[i] <= 126) {
        temp += (char)data[i];
        i++;
      }
      if (temp.length() >= 3) {
        return temp;
      }
      i--;
    }
  }

  return result;
}

void sendRequest(const std::vector<String>& data) {
  Serial.println(state);


  if (data.size() < 2) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Invalid RFID Data");
    display.display();
    return;
  }

  String idStr = data[1].substring(2);
  if (!idStr.toInt()) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Invalid RFID ID");
    display.display();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(idStr);
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("Loading:.....");
    display.display();

    if (state == "idle") {
      HTTPClient http;
      http.begin("https://tracui.pxxl.tech/api/products/scan");
      http.addHeader("Content-Type", "application/json");
      StaticJsonDocument<200> jsonDoc;
      jsonDoc["id"] = idStr.toInt();
      String jsonData;
      serializeJson(jsonDoc, jsonData);

      int httpCode = http.POST(jsonData);
      if (httpCode > 0) {
        String response = http.getString();
        Serial.println("Raw JSON:");
        Serial.println(response);

        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, response);

        if (!error) {


          if (doc["s"] == 1) {

            s = doc["s"];
            p = doc["p"];
            c = doc["c"].as<String>();
            state = "linked";

            preferences.putInt("s", s);
            preferences.putInt("p", p);
            preferences.putString("c", c);
            preferences.putString("state", state);

            display.clearDisplay();
            display.setCursor(0, 0);
            display.print("Product Linked: ");
            display.println(c);
            display.println();
            display.print("Proceed to Logistics");
            display.display();
          } else {
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("User is not a seller");
            display.display();
          }
          // Save to preferences

        } else {
          display.clearDisplay();
          display.setCursor(0, 0);
          display.println("JSON Parse Error");
          display.display();
        }
      } else {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("HTTP Request Failed");
        display.display();
        Serial.println(httpCode);
        Serial.println(http.getString());
      }
      http.end();
    } else if (state == "linked") {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.print("Loading:.....");
      display.display();
      HTTPClient http;
      http.begin("https://tracui.pxxl.tech/api/logistics/handover");
      http.addHeader("Content-Type", "application/json");
      StaticJsonDocument<200> jsonDoc;
      jsonDoc["logistics_id"] = idStr.toInt();
      jsonDoc["product_id"] = p;
      String jsonData;
      serializeJson(jsonDoc, jsonData);
      Serial.println(p);

      int httpCode = http.POST(jsonData);
      if (httpCode > 0) {
        String response = http.getString();
        Serial.println("Raw JSON:");
        Serial.println(response);

        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, response);

        Serial.println(httpCode);

        if (!error) {

          if (httpCode == 200) {
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("Product in Transit: ");
            display.print("Code: ");
            display.println(c);
            display.print("Loging has begun");
            display.display();
            delay(1000);

            isLogging = true;
            state = "logging";

            preferences.putBool("isLogging", isLogging);
            preferences.putString("state", state);
          } else if (httpCode == 400) {
            if (doc["error"] = "Insufficient balance to cover delivery fee") {
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("CUSTOMER HAS");
            display.print("INSUFFICIENT FUNDS");
            display.display();
            delay(1000);
            }else {
                          display.clearDisplay();
            display.setCursor(0, 0);
            display.println("WRONG CARD!");
            display.print("NOT AN AGENT!!");
            display.display();
            delay(1000);
            }

          } else {
            
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("WRONG CARD!");
            display.print("NOT AN AGENT!!");
            display.display();
            delay(1000);
          }


        } else {
          Serial.print("skdskdskdskds");
          display.clearDisplay();
          display.setCursor(0, 0);
          display.println("JSON Parse Error");
          display.display();
          delay(1000);
        }
      } else {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("HTTP Request Failed");
        display.display();
        Serial.println(httpCode);
        Serial.println(http.getString());
        delay(1000);
      }
      http.end();
    }else if(state = "logging"){
      display.clearDisplay();
      display.setCursor(0, 0);
      display.print("Loading:.....");
      display.display();
      HTTPClient http;
      http.begin("https://tracui.pxxl.tech/api/logistics/complete");
      http.addHeader("Content-Type", "application/json");
      StaticJsonDocument<200> jsonDoc;
      jsonDoc["customer_id"] = idStr.toInt();
      jsonDoc["product_id"] = p;
      String jsonData;
      serializeJson(jsonDoc, jsonData);

      int httpCode = http.POST(jsonData);
      if (httpCode > 0) {
        String response = http.getString();
        Serial.println("Raw JSON:");
        Serial.println(response);

        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, response);

        Serial.println(httpCode);

        if (!error) {

          if (httpCode == 200) {
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("SUCCESSFULLY");
            display.print("DELIVERD: ");
            display.println(c);
            display.print("THANK YisLoggingOU");
            display.display();
            delay(1000);

            isLogging = false;
            state = "idle";
            s=NULL;
            c="";
            p=NULL;

            
            preferences.putInt("s", s);
            preferences.putInt("p", p);
            preferences.putString("c", c);
            preferences.putString("state", state);
            preferences.putBool("isLogging", isLogging);
          } else {


            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("WRONG CARD!");
            display.print("NOT AN CONSUMER!!");
            display.display();
            delay(1000);
          }


        } else {
          Serial.print("skdskdskdskds");
          display.clearDisplay();
          display.setCursor(0, 0);
          display.println("JSON Parse Error");
          display.display();
          delay(1000);
        }
      } else {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("HTTP Request Failed");
        display.display();
        Serial.println(httpCode);
        Serial.println(http.getString());
        delay(1000);
      }
      http.end();
    }



  } else {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi Disconnected");
    display.display();
  }
}