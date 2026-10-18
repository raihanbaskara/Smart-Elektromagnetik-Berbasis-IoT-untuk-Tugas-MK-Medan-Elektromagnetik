/*
 ╔══════════════════════════════════════════════════════════════════╗
 ║          SMART ELECTROMAGNETIC – ESP32                         ║
 ║  Sensor  : INA219 (SDA:21, SCL:22)                             ║
 ║  Output  : LED (GPIO26), Kipas via L298N (IN1:27, IN2:14)      ║
 ║  ENA L298N → jumper ke pin 5V di board L298N (full speed)      ║
 ╚══════════════════════════════════════════════════════════════════╝
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <ArduinoJson.h>

// ═══════════════════════════════════════════════════
//  KONFIGURASI
// ═══════════════════════════════════════════════════
const char* WIFI_SSID     = "Infinix NOTE 50 Pro";
const char* WIFI_PASSWORD = "00000000";

const char* MQTT_HOST     = "broker.emqx.io";
const int   MQTT_PORT     = 1883;

const char* TOPIC_SENSOR  = "smartem/ina219/data";
const char* TOPIC_CONTROL = "smartem/control";
const char* TOPIC_STATUS  = "smartem/status";

// ═══════════════════════════════════════════════════
//  PIN
// ═══════════════════════════════════════════════════
#define PIN_SDA       21
#define PIN_SCL       22
#define PIN_LED       26

#define PIN_L298_IN1  27    // IN1 L298N → D27
#define PIN_L298_IN2  14    // IN2 L298N → D14
// ENA L298N → jumper ke 5V di board (tidak pakai pin ESP32)

// ═══════════════════════════════════════════════════
//  INTERVAL
// ═══════════════════════════════════════════════════
const unsigned long PUBLISH_INTERVAL = 2000;
const unsigned long RECONNECT_DELAY  = 5000;

// ═══════════════════════════════════════════════════
//  VARIABEL
// ═══════════════════════════════════════════════════
WiFiClient      wifiClient;
PubSubClient    mqttClient(wifiClient);
Adafruit_INA219 ina219;

bool  ledState       = false;
bool  fanState       = false;
bool  autoLedEnabled = false;
bool  autoFanEnabled = false;
float autoLedThresh  = 4.5;
float autoFanThresh  = 3.0;

unsigned long lastPublish   = 0;
unsigned long lastReconnect = 0;
bool          inaReady      = false;
char          mqttClientId[32];

// ═══════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n╔══════════════════════════════════╗"));
  Serial.println(F("║  Smart EM  –  ESP32  Boot...     ║"));
  Serial.println(F("╚══════════════════════════════════╝"));

  // GPIO
  pinMode(PIN_LED,      OUTPUT);
  pinMode(PIN_L298_IN1, OUTPUT);
  pinMode(PIN_L298_IN2, OUTPUT);
  digitalWrite(PIN_LED,      LOW);
  digitalWrite(PIN_L298_IN1, LOW);
  digitalWrite(PIN_L298_IN2, LOW);

  // I2C + INA219
  Wire.begin(PIN_SDA, PIN_SCL);
  delay(100);
  if (ina219.begin()) {
    inaReady = true;
    ina219.setCalibration_32V_2A();
    Serial.println(F("[INA219] OK – 32V/2A"));
  } else {
    Serial.println(F("[INA219] TIDAK DITEMUKAN – cek wiring!"));
  }

  // Client ID dari MAC
  uint32_t chipId = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
  snprintf(mqttClientId, sizeof(mqttClientId), "smartem_%06X", chipId);
  Serial.printf("[MQTT] Client ID: %s\n", mqttClientId);

  // WiFi & MQTT
  connectWiFi();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(10);
  if (WiFi.status() == WL_CONNECTED) connectMQTT();

  Serial.println(F("[BOOT] Setup selesai!\n"));
}

// ═══════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[WiFi] Terputus, reconnect..."));
    connectWiFi();
    return;
  }

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastReconnect >= RECONNECT_DELAY) {
      lastReconnect = now;
      connectMQTT();
    }
  } else {
    mqttClient.loop();
  }

  unsigned long now = millis();
  if (now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;
    if (mqttClient.connected()) publishSensor();
  }
}

// ═══════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════
void connectWiFi() {
  WiFi.disconnect(true);
  delay(300);
  Serial.printf("[WiFi] Konek ke: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint8_t attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 40) {
    delay(500);
    Serial.print(".");
    attempt++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Terhubung! IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("[WiFi] Gagal! status=%d\n", (int)WiFi.status());
  }
}

// ═══════════════════════════════════════════════════
//  MQTT – Connect
// ═══════════════════════════════════════════════════
void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.printf("[MQTT] Konek ke %s:%d ...\n", MQTT_HOST, MQTT_PORT);

  String willMsg = "{\"status\":\"offline\",\"id\":\"" + String(mqttClientId) + "\"}";
  bool ok = mqttClient.connect(mqttClientId, nullptr, nullptr,
                               TOPIC_STATUS, 1, true, willMsg.c_str());
  if (ok) {
    Serial.println(F("[MQTT] Terhubung!"));
    mqttClient.subscribe(TOPIC_CONTROL, 1);
    publishStatus("online");
  } else {
    Serial.printf("[MQTT] Gagal rc=%d\n", mqttClient.state());
  }
}

// ═══════════════════════════════════════════════════
//  MQTT – CALLBACK
// ═══════════════════════════════════════════════════
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[256];
  uint16_t len = min((unsigned int)255, length);
  memcpy(msg, payload, len);
  msg[len] = '\0';
  Serial.printf("[MQTT] ↓ %s : %s\n", topic, msg);

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, msg)) { Serial.println(F("[JSON] Parse error")); return; }

  const char* device = doc["device"] | "";
  const char* cmd    = doc["cmd"]    | "";
  int         value  = doc["value"]  | -1;

  if (strcmp(device,"led") == 0 || strcmp(cmd,"led") == 0) {
    if      (value == 1) setLED(true);
    else if (value == 0) setLED(false);
    else                 setLED(!ledState);
  }
  // "relay" = Motor 1 via L298N (dari dashboard generator baru)
  else if (strcmp(device,"relay") == 0 || strcmp(device,"fan") == 0 || strcmp(cmd,"fan") == 0) {
    if      (value == 1) setFan(true);
    else if (value == 0) setFan(false);
    else                 setFan(!fanState);
  }
  else if (strcmp(device,"auto_led") == 0) {
    autoLedEnabled = (value == 1);
    if (doc.containsKey("threshold")) autoLedThresh = doc["threshold"].as<float>();
    Serial.printf("[AUTO] LED=%s thresh=%.2fV\n", autoLedEnabled?"ON":"OFF", autoLedThresh);
  }
  else if (strcmp(device,"auto_fan") == 0) {
    autoFanEnabled = (value == 1);
    if (doc.containsKey("threshold")) autoFanThresh = doc["threshold"].as<float>();
    Serial.printf("[AUTO] FAN=%s thresh=%.2fV\n", autoFanEnabled?"ON":"OFF", autoFanThresh);
  }
  else if (strcmp(cmd,"all_off") == 0) {
    setLED(false);
    setFan(false);
  }
}

// ═══════════════════════════════════════════════════
//  PUBLISH SENSOR
// ═══════════════════════════════════════════════════
void publishSensor() {
  float shuntVoltage = 0, busVoltage = 0, current_mA = 0;
  float loadVoltage  = 0, power_mW  = 0;

  if (inaReady) {
    shuntVoltage = ina219.getShuntVoltage_mV();
    busVoltage   = ina219.getBusVoltage_V();
    current_mA   = ina219.getCurrent_mA();
    power_mW     = ina219.getPower_mW();
    loadVoltage  = busVoltage + (shuntVoltage / 1000.0f);
  } else {
    if (ina219.begin()) {
      inaReady = true;
      ina219.setCalibration_32V_2A();
      Serial.println(F("[INA219] Reconnected!"));
    }
  }

  // Auto mode
  if (autoLedEnabled) {
    bool should = (loadVoltage >= autoLedThresh);
    if (should != ledState) {
      setLED(should);
      Serial.printf("[AUTO-LED] %s (%.3fV %s %.2fV)\n",
        should?"ON":"OFF", loadVoltage, should?">=":"<", autoLedThresh);
    }
  }
  if (autoFanEnabled) {
    bool should = (loadVoltage >= autoFanThresh);
    if (should != fanState) {
      setFan(should);
      Serial.printf("[AUTO-FAN] %s (%.3fV %s %.2fV)\n",
        should?"ON":"OFF", loadVoltage, should?">=":"<", autoFanThresh);
    }
  }

  // JSON
  StaticJsonDocument<384> doc;
  doc["voltage"]     = roundf(loadVoltage  * 1000.0f) / 1000.0f;
  doc["bus_voltage"] = roundf(busVoltage   * 1000.0f) / 1000.0f;
  doc["shunt_mV"]    = roundf(shuntVoltage * 1000.0f) / 1000.0f;
  doc["current"]     = roundf(current_mA   * 100.0f)  / 100.0f;
  doc["power"]       = roundf(power_mW     * 100.0f)  / 100.0f;
  doc["led"]         = ledState       ? 1 : 0;
  doc["fan"]         = fanState       ? 1 : 0;
  doc["auto_led"]    = autoLedEnabled ? 1 : 0;
  doc["auto_fan"]    = autoFanEnabled ? 1 : 0;
  doc["thresh_led"]  = autoLedThresh;
  doc["thresh_fan"]  = autoFanThresh;
  doc["uptime_s"]    = millis() / 1000;
  doc["rssi"]        = WiFi.RSSI();
  doc["ip"]          = WiFi.localIP().toString();

  char buf[384];
  serializeJson(doc, buf);

  bool ok = mqttClient.publish(TOPIC_SENSOR, buf, false);
  Serial.printf("[PUB] %s  V=%.3f  I=%.2fmA  P=%.2fmW  LED=%d  FAN=%d\n",
                ok?"OK":"FAIL", loadVoltage, current_mA, power_mW, ledState, fanState);
}

// ═══════════════════════════════════════════════════
//  PUBLISH STATUS
// ═══════════════════════════════════════════════════
void publishStatus(const char* s) {
  StaticJsonDocument<128> doc;
  doc["status"]   = s;
  doc["id"]       = mqttClientId;
  doc["ip"]       = WiFi.localIP().toString();
  doc["uptime_s"] = millis() / 1000;
  char buf[128];
  serializeJson(doc, buf);
  mqttClient.publish(TOPIC_STATUS, buf, true);
}

// ═══════════════════════════════════════════════════
//  SET LED
// ═══════════════════════════════════════════════════
void setLED(bool on) {
  ledState = on;
  digitalWrite(PIN_LED, on ? HIGH : LOW);
  Serial.printf("[LED] %s\n", on ? "ON" : "OFF");
}

// ═══════════════════════════════════════════════════
//  SET KIPAS – L298N (ON/OFF saja)
//  IN1=HIGH, IN2=LOW  → kipas menyala
//  IN1=LOW,  IN2=LOW  → kipas mati
//  ENA sudah dijumper ke 5V → selalu enable
// ═══════════════════════════════════════════════════
void setFan(bool on) {
  fanState = on;
  digitalWrite(PIN_L298_IN1, on ? HIGH : LOW);
  digitalWrite(PIN_L298_IN2, LOW);
  Serial.printf("[FAN] %s\n", on ? "ON" : "OFF");
}
