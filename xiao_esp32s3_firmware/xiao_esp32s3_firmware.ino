/*
 * Seeed Studio XIAO ESP32-S3 Firmware v2.1
 * Eco-Pet Care Smart Home System
 * 
 * Hardware Connections:
 * - BMP280 Sensor (I2C): SDA -> D4 (GPIO 5), SCL -> D5 (GPIO 6) [Address 0x76 or 0x77]
 * - LoadCell HX711 DT     -> D9  (GPIO 8)
 * - LoadCell HX711 SCK    -> D10 (GPIO 9)
 * - PIR Motion Sensor OUT -> D6  (GPIO 43)
 */

#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <HX711.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// =================================================================
// 1. 핀 및 센서 객체 정의
// =================================================================
#define I2C_SDA D4  // GPIO 5
#define I2C_SCL D5  // GPIO 6

const int PIN_PIR  = D6;   // GPIO 43 (PIR)
const int DOUT_PIN = D9;   // GPIO 8 (HX711 DT)
const int SCK_PIN  = D10;  // GPIO 9 (HX711 SCK)

// BMP280 객체
Adafruit_BMP280 bmp;
bool bmpAvailable = false;
uint8_t bmpAddress = 0;
float currentTempC = 0.0f;
float currentPressureHpa = 0.0f;

// HX711 로드셀 객체
HX711 scale;
float SCALE_FACTOR = 420.0f; // 기본 스케일 팩터 (필요시 조정 가능)
const float THRESHOLD_G = 200.0f;
float latestWeightGrams = 0.0f;
bool isDogPresent = false;
String dogStateStr = "ABSENT";
bool scaleReady = false;

// PIR 센서
bool isMotionDetected = false;
int motionCount = 0;
unsigned long lastMotionTime = 0;

// =================================================================
// 2. Wi-Fi & 네트워크 설정
// =================================================================
struct WifiCred {
  const char* ssid;
  const char* pass;
};

WifiCred wifiList[] = {
  {"junyeong", "10101010"},
  {"seojun", "35320300"}
};
const int wifiCount = sizeof(wifiList) / sizeof(WifiCred);

const char* mqtt_server = "10.48.38.244";
const int   mqtt_port   = 1883;
const char* topic_sensor  = "xiao/sensor";
const char* topic_control = "xiao/control";

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);

unsigned long lastSensorPublish = 0;
unsigned long lastBmpReadTime = 0;
unsigned long lastScaleReadTime = 0;

// =================================================================
// 3. I2C 버스 스캐너 및 Raw BMP280 폴백 읽기 함수
// =================================================================
uint8_t scanI2C() {
  Serial.println("\n--- [I2C Bus Scan] ---");
  uint8_t foundAddr = 0;
  byte error, address;
  int nDevices = 0;

  for (address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
      if (address == 0x76 || address == 0x77) {
        foundAddr = address;
      }
      nDevices++;
    }
  }
  if (nDevices == 0) {
    Serial.println("No I2C devices found! Check D4(SDA)/D5(SCL) wiring & power.");
  }
  Serial.println("----------------------\n");
  return foundAddr;
}

// BMP280 보정 데이터 구조체 (Raw 읽기용)
struct BMP280_Calib {
  uint16_t dig_T1;
  int16_t  dig_T2;
  int16_t  dig_T3;
} calib;

int32_t t_fine;

bool readRawBMP280Calib(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(0x88);
  if (Wire.endTransmission() != 0) return false;

  Wire.requestFrom((int)addr, 6);
  if (Wire.available() < 6) return false;

  calib.dig_T1 = Wire.read() | (Wire.read() << 8);
  calib.dig_T2 = Wire.read() | (Wire.read() << 8);
  calib.dig_T3 = Wire.read() | (Wire.read() << 8);
  return true;
}

float readRawBMP280Temp(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(0xFA); // Temperature MSB register
  if (Wire.endTransmission() != 0) return 0.0f;

  Wire.requestFrom((int)addr, 3);
  if (Wire.available() < 3) return 0.0f;

  int32_t adc_T = (Wire.read() << 12) | (Wire.read() << 4) | (Wire.read() >> 4);
  if (adc_T == 0) return 0.0f;

  int32_t var1 = ((((adc_T >> 3) - ((int32_t)calib.dig_T1 << 1))) * ((int32_t)calib.dig_T2)) >> 11;
  int32_t var2 = (((((adc_T >> 4) - ((int32_t)calib.dig_T1)) * ((adc_T >> 4) - ((int32_t)calib.dig_T1))) >> 12) * ((int32_t)calib.dig_T3)) >> 14;
  t_fine = var1 + var2;
  float T = (t_fine * 5 + 128) >> 8;
  return T / 100.0f;
}

// =================================================================
// 4. HTTP API 및 웹페이지 핸들러
// =================================================================
void handleApiStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");

  StaticJsonDocument<512> doc;
  JsonObject data = doc.createNestedObject("data");
  data["temp"] = currentTempC;
  data["temperature"] = currentTempC;
  data["bmp280_temp"] = currentTempC;
  data["pressure"] = currentPressureHpa;
  data["weight"] = latestWeightGrams;
  data["motionDetected"] = isMotionDetected;
  data["dogPresent"] = isDogPresent;
  data["dogState"] = dogStateStr;
  data["deviceStatus"] = "ONLINE";
  data["ip"] = WiFi.localIP().toString();

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Eco-Pet ESP32-S3</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;background:#0b0c10;color:#fff;padding:2rem;text-align:center;}";
  html += ".card{background:rgba(255,255,255,0.05);border:1px solid rgba(255,255,255,0.1);padding:1.5rem;border-radius:15px;max-width:400px;margin:auto;}";
  html += ".val{font-size:2.2rem;color:#10b981;font-weight:bold;margin:0.5rem 0;}</style></head><body>";
  html += "<div class='card'><h2>🌱 Eco-Pet ESP32-S3</h2>";
  html += "<p>🌡️ BMP280 온도 센서</p>";
  html += "<div class='val'>" + String(currentTempC, 1) + " °C</div>";
  html += "<p>⚖️ 방석 무게: " + String(latestWeightGrams, 1) + " g</p>";
  html += "<p>🏃 PIR 움직임: " + String(isMotionDetected ? "감지됨 🏃" : "없음 💤") + "</p>";
  html += "<p>🌐 IP: " + WiFi.localIP().toString() + "</p>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void reconnectMqtt() {
  if (mqttClient.connected()) return;
  static unsigned long lastAttempt = 0;
  if (millis() - lastAttempt < 5000) return;
  lastAttempt = millis();

  String clientId = "XIAO_S3_";
  clientId += String(random(0xffff), HEX);
  if (mqttClient.connect(clientId.c_str())) {
    mqttClient.subscribe(topic_control);
    Serial.println("[MQTT] Connected to Broker!");
  }
}

// =================================================================
// 5. Setup & Loop
// =================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=================================");
  Serial.println("🌱 Eco-Pet XIAO ESP32-S3 Booting...");
  Serial.println("=================================");

  pinMode(PIN_PIR, INPUT);

  // 1. I2C Wire 초기화 (D4=SDA/GPIO 5, D5=SCL/GPIO 6)
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(100);

  // I2C 스캔 수행
  bmpAddress = scanI2C();

  // 2. BMP280 온도 센서 초기화 (Adafruit + Raw Fallback)
  if (bmpAddress != 0) {
    if (bmp.begin(bmpAddress)) {
      bmpAvailable = true;
      bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                      Adafruit_BMP280::SAMPLING_X2,
                      Adafruit_BMP280::SAMPLING_X16,
                      Adafruit_BMP280::FILTER_X16,
                      Adafruit_BMP280::STANDBY_MS_500);
      Serial.println("[BMP280] Adafruit_BMP280 initialized successfully!");
    } else {
      Serial.println("[BMP280] Adafruit_BMP280 init failed, attempting Raw I2C Calib...");
      if (readRawBMP280Calib(bmpAddress)) {
        bmpAvailable = true;
        Serial.println("[BMP280] Raw I2C Calibration loaded!");
      }
    }
  } else {
    // 0x76 기본 재시도
    if (bmp.begin(0x76)) {
      bmpAvailable = true;
      bmpAddress = 0x76;
      Serial.println("[BMP280] Initialized at 0x76!");
    } else if (bmp.begin(0x77)) {
      bmpAvailable = true;
      bmpAddress = 0x77;
      Serial.println("[BMP280] Initialized at 0x77!");
    }
  }

  // 3. HX711 로드셀 센서 초기화 (Official HX711 Library)
  Serial.println("[HX711] Initializing scale on D9 (DT) & D10 (SCK)...");
  scale.begin(DOUT_PIN, SCK_PIN);
  
  unsigned long scaleStart = millis();
  while (!scale.is_ready() && (millis() - scaleStart < 2000)) {
    delay(50);
  }
  
  if (scale.is_ready()) {
    scaleReady = true;
    scale.set_scale(SCALE_FACTOR);
    scale.tare(10); // 10회 평균 영점 잡기
    Serial.println("[HX711] Scale ready & Tare completed!");
  } else {
    Serial.println("[HX711] WARNING: Scale not ready. Check D9/D10 wiring & power.");
  }

  // 4. Wi-Fi 연결
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  bool connected = false;
  for (int i = 0; i < wifiCount && !connected; i++) {
    Serial.print("[WiFi] Trying SSID: ");
    Serial.println(wifiList[i].ssid);
    WiFi.disconnect();
    delay(100);
    WiFi.begin(wifiList[i].ssid, wifiList[i].pass);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      Serial.println("\n[WiFi] Connected!");
      Serial.print("[WiFi] IP Address: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("\n[WiFi] Connection timed out.");
    }
  }

  // 5. WebServer & MQTT
  server.on("/", handleRoot);
  server.on("/api/status", handleApiStatus);
  server.enableCORS(true);
  server.begin();
  Serial.println("[HTTP] WebServer started on port 80");

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(512);
}

void loop() {
  unsigned long now = millis();

  // HTTP 서비스
  server.handleClient();

  // MQTT 서비스
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      reconnectMqtt();
    }
    mqttClient.loop();
  }

  // 1. BMP280 온도 센서 데이터 읽기 (500ms 주기)
  if (now - lastBmpReadTime >= 500) {
    lastBmpReadTime = now;
    if (bmpAvailable) {
      float t = bmp.readTemperature();
      if (!isnan(t) && t > -40.0f && t < 85.0f) {
        currentTempC = t;
        currentPressureHpa = bmp.readPressure() / 100.0F;
      } else if (bmpAddress != 0) {
        // Raw I2C 읽기 시도
        float rawT = readRawBMP280Temp(bmpAddress);
        if (rawT > -40.0f && rawT < 85.0f) {
          currentTempC = rawT;
        }
      }
    } else {
      // 센서 재시도
      if (bmpAddress != 0 && readRawBMP280Calib(bmpAddress)) {
        bmpAvailable = true;
        currentTempC = readRawBMP280Temp(bmpAddress);
      }
    }
  }

  // 2. PIR 센서 읽기
  bool motionNow = (digitalRead(PIN_PIR) == HIGH);
  if (motionNow != isMotionDetected) {
    isMotionDetected = motionNow;
    if (isMotionDetected) {
      motionCount++;
      lastMotionTime = now;
    }
  }

  // 3. HX711 로드셀 무게 읽기 (200ms 주기)
  if (now - lastScaleReadTime >= 200) {
    lastScaleReadTime = now;
    if (scale.is_ready()) {
      scaleReady = true;
      float rawUnits = scale.get_units(1);
      if (rawUnits < 0) rawUnits = 0.0f; // 음수 방지
      latestWeightGrams = rawUnits;
    } else {
      // 핀을 다시 확인하거나 재초기화 시도
      scale.begin(DOUT_PIN, SCK_PIN);
    }
  }

  // 4. 강아지 상태 판단
  if (isMotionDetected) {
    dogStateStr = "ACTIVE";
    isDogPresent = true;
  } else if (latestWeightGrams >= THRESHOLD_G) {
    isDogPresent = true;
    if (now - lastMotionTime >= 5000) {
      dogStateStr = "SLEEPING";
    } else {
      dogStateStr = "PRESENT";
    }
  } else {
    isDogPresent = false;
    dogStateStr = "ABSENT";
  }

  // 5. MQTT 데이터 전송 (300ms 주기)
  if (now - lastSensorPublish >= 300) {
    lastSensorPublish = now;
    if (mqttClient.connected()) {
      StaticJsonDocument<512> doc;
      doc["temp"] = currentTempC;
      doc["temperature"] = currentTempC;
      doc["bmp280_temp"] = currentTempC;
      doc["weight"] = latestWeightGrams;
      doc["motionDetected"] = isMotionDetected;
      doc["dogPresent"] = isDogPresent;
      doc["dogState"] = dogStateStr;
      doc["deviceStatus"] = "ONLINE";

      char buf[512];
      serializeJson(doc, buf);
      mqttClient.publish(topic_sensor, buf);
    }
  }
}
