/*
 * Seeed Studio XIAO ESP32-S3 Firmware
 * Eco-Pet Care Smart Home System
 * 
 * Hardware Connections:
 * - BMP280 Sensor (I2C): SDA -> D4 (GPIO 5), SCL -> D5 (GPIO 6) [0x76 or 0x77]
 * - PIR Motion Sensor OUT -> D6 (GPIO 43)
 * - LoadCell HX711 DT     -> D9 (GPIO 8)
 * - LoadCell HX711 SCK    -> D10 (GPIO 9)
 */

#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// =================================================================
// 1. 핀 및 I2C 센서 설정
// =================================================================
#define I2C_SDA D4  // GPIO 5
#define I2C_SCL D5  // GPIO 6

const int PIN_PIR  = D6;   // GPIO 43 (PIR 모션 센서)
const int DOUT_PIN = D9;   // GPIO 8 (HX711 DT)
const int SCK_PIN  = D10;  // GPIO 9 (HX711 SCK)

Adafruit_BMP280 bmp;
bool bmpAvailable = false;
float currentTempC = 0.0f;
float currentPressureHpa = 0.0f;

// HX711 무게 센서
float SCALE_FACTOR = 100.0f;
const float THRESHOLD_G = 200.0f;
long tareValue = 0;
float latestWeightGrams = 0.0f;
bool isDogPresent = false;
String dogStateStr = "ABSENT";

// PIR 센서
bool isMotionDetected = false;
int motionCount = 0;
unsigned long lastMotionTime = 0;

// =================================================================
// 2. Wi-Fi, WebServer & MQTT 설정
// =================================================================
struct WifiCred {
  const char* ssid;
  const char* pass;
};

WifiCred wifiList[] = {
  {"seojun", "35320300"},
  {"junyeong", "10101010"}
};
const int wifiCount = sizeof(wifiList) / sizeof(WifiCred);

const char* mqtt_server = "172.20.10.2"; // 라즈베리파이 IP (기본)
const int   mqtt_port   = 1883;
const char* topic_sensor  = "xiao/sensor";
const char* topic_control = "xiao/control";

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);

unsigned long lastSensorPublish = 0;
unsigned long lastBmpReadTime = 0;

// ⚡ [Non-Blocking] HX711 수신
bool readHX711NonBlocking(long &outRaw) {
  if (digitalRead(DOUT_PIN) == HIGH) return false;

  long count = 0;
  for (int i = 0; i < 24; i++) {
    digitalWrite(SCK_PIN, HIGH);
    delayMicroseconds(1);
    count = count << 1;
    digitalWrite(SCK_PIN, LOW);
    delayMicroseconds(1);
    if (digitalRead(DOUT_PIN)) count++;
  }
  digitalWrite(SCK_PIN, HIGH);
  delayMicroseconds(1);
  digitalWrite(SCK_PIN, LOW);
  delayMicroseconds(1);

  if (count & 0x800000) count |= 0xFF000000;
  outRaw = count;
  return true;
}

void performTare() {
  long sum = 0;
  int count = 0;
  unsigned long start = millis();
  while (count < 10 && (millis() - start < 1500)) {
    long raw = 0;
    if (readHX711NonBlocking(raw)) {
      sum += raw;
      count++;
      delay(30);
    }
  }
  if (count > 0) tareValue = sum / count;
  latestWeightGrams = 0.0f;
}

// 웹 HTTP API 응답 handler (/api/status)
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

// 웹 루트 페이지 handler (/)
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Eco-Pet XIAO ESP32-S3</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;background:#0b0c10;color:#fff;padding:2rem;text-align:center;}";
  html += ".card{background:rgba(255,255,255,0.05);border:1px solid rgba(255,255,255,0.1);padding:1.5rem;border-radius:15px;max-width:400px;margin:auto;}";
  html += ".val{font-size:2rem;color:#10b981;font-weight:bold;margin:0.5rem 0;}</style></head><body>";
  html += "<div class='card'><h2>🌱 Eco-Pet ESP32-S3</h2>";
  html += "<p>🌡️ BMP280 온도 센서 (D4/D5)</p>";
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

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Eco-Pet XIAO ESP32-S3 Starting ===");

  pinMode(PIN_PIR, INPUT);
  pinMode(DOUT_PIN, INPUT_PULLUP);
  pinMode(SCK_PIN, OUTPUT);
  digitalWrite(SCK_PIN, LOW);

  // 1. I2C Wire 초기화 (D4=SDA, D5=SCL)
  Wire.begin(I2C_SDA, I2C_SCL);

  // 2. BMP280 센서 탐색 (0x76 또는 0x77)
  if (bmp.begin(0x76)) {
    bmpAvailable = true;
    Serial.println("[BMP280] Found at 0x76!");
  } else if (bmp.begin(0x77)) {
    bmpAvailable = true;
    Serial.println("[BMP280] Found at 0x77!");
  } else {
    bmpAvailable = false;
    Serial.println("[BMP280] Could NOT find sensor on 0x76/0x77");
  }

  if (bmpAvailable) {
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
  }

  // 3. Wi-Fi 연결 (자동 멀티 SSID 시도)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  bool connected = false;
  for (int i = 0; i < wifiCount && !connected; i++) {
    Serial.print("[WiFi] Trying SSID: ");
    Serial.println(wifiList[i].ssid);
    WiFi.begin(wifiList[i].ssid, wifiList[i].pass);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 15) {
      delay(300);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      Serial.println("\n[WiFi] Connected!");
      Serial.print("[WiFi] IP Address: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("\n[WiFi] Failed to connect.");
    }
  }

  performTare();

  // 4. WebServer 핸들러 설정
  server.on("/", handleRoot);
  server.on("/api/status", handleApiStatus);
  server.enableCORS(true);
  server.begin();
  Serial.println("[HTTP] WebServer started on port 80");

  // 5. MQTT 설정
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(512);
}

void loop() {
  unsigned long now = millis();

  // HTTP 웹서버 처리
  server.handleClient();

  // MQTT 클라이언트 처리
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      reconnectMqtt();
    }
    mqttClient.loop();
  }

  // 1. BMP280 온도 읽기 (1초 주기)
  if (now - lastBmpReadTime >= 1000) {
    lastBmpReadTime = now;
    if (bmpAvailable) {
      currentTempC = bmp.readTemperature();
      currentPressureHpa = bmp.readPressure() / 100.0F;
      if (isnan(currentTempC)) currentTempC = 24.5f;
    } else {
      currentTempC = 24.5f; // 센서 미검출시 기본값
    }
  }

  // 2. PIR 센서 감지
  bool motionNow = (digitalRead(PIN_PIR) == HIGH);
  if (motionNow != isMotionDetected) {
    isMotionDetected = motionNow;
    if (isMotionDetected) {
      motionCount++;
      lastMotionTime = now;
    }
  }

  // 3. HX711 로드셀 샘플링
  long currentRaw = 0;
  if (readHX711NonBlocking(currentRaw)) {
    long diff = labs(currentRaw - tareValue);
    float g = (float)diff / SCALE_FACTOR;
    if (g < 5.0f) g = 0.0f;
    latestWeightGrams = g;
  }

  // 4. 상태 판단
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

  // 5. MQTT 센서 데이터 발행 (300ms 주기)
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
