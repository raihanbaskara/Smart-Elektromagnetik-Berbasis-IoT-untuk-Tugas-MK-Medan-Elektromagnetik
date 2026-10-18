/*
  ==============================================================================
  PROJECT       : Smart Electromagnetic Generator IoT System
  INSTITUTION   : Universitas Negeri Malang - Teknik Elektro (2026)
  COURSE        : Medan Elektromagnetik
  AUTHORS       : Raihan Nauval Aziz, Rizaldy Yakub Mahendra, Muhammad Rafi Baihaqi
  ADVISOR       : Hanif Rifai Adha, S.Pd., M.T.
  HARDWARE      : ESP32 DevKit V1, INA219, Dual Motor RS-385, L298N, 3x Relay Mod
  MQTT BROKER   : broker.emqx.io (Port 1883 TCP / 8084 WSS)
  ==============================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <ArduinoJson.h>

// ==============================================================================
// 1. KONFIGURASI JARINGAN & MQTT
// ==============================================================================
// Silakan sesuaikan SSID dan Password WiFi sesuai access point / hotspot Anda
const char* WIFI_SSID     = "Infinix NOTE 50 Pro";
const char* WIFI_PASSWORD = "00000000";

const char* MQTT_HOST     = "broker.emqx.io";
const int   MQTT_PORT     = 1883;

// Topic MQTT
const char* TOPIC_SENSOR  = "smartem/ina219/data";  // Publish telemetri generator
const char* TOPIC_CONTROL = "smartem/control";      // Subscribe perintah dashboard
const char* TOPIC_STATUS  = "smartem/status";       // LWT / Connection status

// ==============================================================================
// 2. PINOUT HARDWARE ESP32
// ==============================================================================
// Sensor INA219 (I2C)
#define PIN_SDA           21
#define PIN_SCL           22

// Driver Motor L298N (Motor 1 - Penggerak Mekanik)
#define PIN_IN1           27    // L298N Direction IN1
#define PIN_IN2           14    // L298N Direction IN2
#define PIN_ENA           25    // L298N Enable A - PWM Speed Control

// 3 Modul Relay Output (Sesuai Tabel 2 Laporan Akhir)
// Catatan: Jika menggunakan board varian pin alternatif (GPIO 17, 16, 4), cukup ubah define di bawah.
#define PIN_RELAY_1       32    // Relay 1 Output
#define PIN_RELAY_2       33    // Relay 2 Output
#define PIN_RELAY_3       26    // Relay 3 Output

// Modul Relay umumnya menggunakan logika Aktif-LOW:
// LOW = Kontak NC terbuka / NO tertutup (Relay ON / Menyala)
// HIGH = Kontak NO terbuka (Relay OFF / Padam)
#define RELAY_ACTIVE_STATE LOW
#define RELAY_IDLE_STATE   HIGH

// ==============================================================================
// 3. PARAMETER PWM & TIMING
// ==============================================================================
#define PWM_FREQ          25000 // 25 kHz (frekuensi ultrasonik agar motor tidak berdengung)
#define PWM_RES           8     // Resolusi 8-bit (nilai 0 - 255)
#define PWM_CHANNEL       0     // PWM Channel ESP32

const unsigned long PUBLISH_INTERVAL = 2000; // Interval publish telemetri (ms)
const unsigned long RECONNECT_DELAY  = 3000; // Interval percobaan reconnect MQTT (ms)

// ==============================================================================
// 4. INSTANSIASI OBJEK & VARIABEL GLOBAL
// ==============================================================================
WiFiClient      wifiClient;
PubSubClient    mqttClient(wifiClient);
Adafruit_INA219 ina219;

bool          inaReady      = false;
bool          motorState    = false;
int           motorSpeed    = 128; // Default 50% duty cycle (0-255)

bool          relayState[3] = {false, false, false};
bool          autoRelay[3]  = {false, false, false};
float         threshold[3]  = {1.0f, 2.0f, 3.0f}; // Ambang batas tegangan (Volt)

unsigned long lastPublish   = 0;
unsigned long lastReconnect = 0;
char          mqttClientId[32];

// ==============================================================================
// 5. KONTROL MOTOR DC (L298N)
// ==============================================================================
void writePWM(int pin, int value) {
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    ledcWrite(pin, value);
  #else
    ledcWrite(PWM_CHANNEL, value);
  #endif
}

void setMotor(bool on) {
  motorState = on;
  if (on) {
    digitalWrite(PIN_IN1, HIGH);
    digitalWrite(PIN_IN2, LOW);
    writePWM(PIN_ENA, motorSpeed);
  } else {
    digitalWrite(PIN_IN1, LOW);
    digitalWrite(PIN_IN2, LOW);
    writePWM(PIN_ENA, 0);
  }
  Serial.printf("[MOTOR] Status: %s | PWM: %d (%.0f%%)\n",
                on ? "RUNNING" : "STOPPED", motorSpeed, (motorSpeed / 255.0f) * 100.0f);
}

void setSpeed(int pwm) {
  motorSpeed = constrain(pwm, 0, 255);
  if (motorState) {
    writePWM(PIN_ENA, motorSpeed);
  }
  Serial.printf("[MOTOR] Kecepatan diatur: PWM %d (%.0f%%)\n",
                motorSpeed, (motorSpeed / 255.0f) * 100.0f);
}

// ==============================================================================
// 6. KONTROL RELAY (AKTIF-LOW)
// ==============================================================================
void setRelay(int index, bool on) {
  if (index < 1 || index > 3) return;
  relayState[index - 1] = on;

  int targetPin = (index == 1) ? PIN_RELAY_1 : ((index == 2) ? PIN_RELAY_2 : PIN_RELAY_3);
  digitalWrite(targetPin, on ? RELAY_ACTIVE_STATE : RELAY_IDLE_STATE);

  Serial.printf("[RELAY %d] %s (Pin GPIO %d -> %s)\n",
                index, on ? "ON" : "OFF", targetPin, on ? "LOW" : "HIGH");
}

void checkAutoRelay(float currentVoltage) {
  for (int i = 0; i < 3; i++) {
    if (!autoRelay[i]) continue;
    bool shouldBeOn = (currentVoltage >= threshold[i]);
    if (shouldBeOn != relayState[i]) {
      setRelay(i + 1, shouldBeOn);
      Serial.printf("[AUTO RELAY %d] %s (Tegangan: %.2fV %s Ambang: %.2fV)\n",
                    i + 1, shouldBeOn ? "ON" : "OFF", currentVoltage,
                    shouldBeOn ? ">=" : "<", threshold[i]);
    }
  }
}

// ==============================================================================
// 7. MQTT CALLBACK (PENERIMAAN PERINTAH)
// ==============================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<384> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print(F("[JSON] Gagal parse payload: "));
    Serial.println(err.c_str());
    return;
  }

  const char* device = doc["device"] | "";
  int   valInt       = doc["value"]  | -1;
  float valFloat     = doc["value"]  | -1.0f;

  Serial.printf("[MQTT RX] Topik: %s | Device: %s | Val: %d\n", topic, device, valInt);

  // Kontrol Motor & Kecepatan
  if (strcmp(device, "motor") == 0 || strcmp(device, "fan") == 0) {
    setMotor(valInt == 1);
  }
  else if (strcmp(device, "speed") == 0) {
    setSpeed(valInt);
  }
  // Kontrol Manual Relay
  else if (strcmp(device, "relay1") == 0) setRelay(1, valInt == 1);
  else if (strcmp(device, "relay2") == 0) setRelay(2, valInt == 1);
  else if (strcmp(device, "relay3") == 0) setRelay(3, valInt == 1);
  // Kontrol Mode Otomatis Relay
  else if (strcmp(device, "auto_relay1") == 0) {
    autoRelay[0] = (valInt == 1);
    if (doc.containsKey("threshold")) threshold[0] = doc["threshold"].as<float>();
    Serial.printf("[AUTO R1] %s | Ambang: %.2fV\n", autoRelay[0] ? "ENABLE" : "DISABLE", threshold[0]);
  }
  else if (strcmp(device, "auto_relay2") == 0) {
    autoRelay[1] = (valInt == 1);
    if (doc.containsKey("threshold")) threshold[1] = doc["threshold"].as<float>();
    Serial.printf("[AUTO R2] %s | Ambang: %.2fV\n", autoRelay[1] ? "ENABLE" : "DISABLE", threshold[1]);
  }
  else if (strcmp(device, "auto_relay3") == 0) {
    autoRelay[2] = (valInt == 1);
    if (doc.containsKey("threshold")) threshold[2] = doc["threshold"].as<float>();
    Serial.printf("[AUTO R3] %s | Ambang: %.2fV\n", autoRelay[2] ? "ENABLE" : "DISABLE", threshold[2]);
  }
  // Update Nilai Threshold Saja
  else if (strcmp(device, "thresh1") == 0) { threshold[0] = valFloat; }
  else if (strcmp(device, "thresh2") == 0) { threshold[1] = valFloat; }
  else if (strcmp(device, "thresh3") == 0) { threshold[2] = valFloat; }
  // Emergency All OFF
  else if (strcmp(device, "all_off") == 0) {
    setMotor(false);
    setRelay(1, false);
    setRelay(2, false);
    setRelay(3, false);
    Serial.println(F("[SYSTEM] EMERGENCY ALL OFF"));
  }
}

// ==============================================================================
// 8. PUBLISH TELEMETRI SENSOR (INA219)
// ==============================================================================
void publishSensor() {
  float busVoltage = 0.0f;
  float current_mA = 0.0f;
  float power_mW   = 0.0f;

  if (inaReady) {
    busVoltage = ina219.getBusVoltage_V();
    current_mA = ina219.getCurrent_mA();

    // Filter noise mendekati nol
    if (busVoltage < 0.05f)      busVoltage = 0.0f;
    if (abs(current_mA) < 2.0f)  current_mA = 0.0f;

    power_mW = busVoltage * current_mA;
  } else {
    // Percobaan inisialisasi ulang sensor jika sempat lepas
    if (ina219.begin()) {
      inaReady = true;
      ina219.setCalibration_32V_2A();
      Serial.println(F("[INA219] Berhasil terhubung kembali!"));
    }
  }

  // Evaluasi mode relay otomatis berdasarkan tegangan terkini
  checkAutoRelay(busVoltage);

  // Buat JSON payload telemetri
  StaticJsonDocument<512> doc;
  doc["voltage"]     = roundf(busVoltage * 100.0f) / 100.0f;
  doc["current"]     = roundf(current_mA * 10.0f)  / 10.0f;
  doc["power"]       = roundf(power_mW   * 10.0f)  / 10.0f;
  doc["motor"]       = motorState     ? 1 : 0;
  doc["speed"]       = motorSpeed;
  doc["relay1"]      = relayState[0]  ? 1 : 0;
  doc["relay2"]      = relayState[1]  ? 1 : 0;
  doc["relay3"]      = relayState[2]  ? 1 : 0;
  doc["auto_relay1"] = autoRelay[0]   ? 1 : 0;
  doc["auto_relay2"] = autoRelay[1]   ? 1 : 0;
  doc["auto_relay3"] = autoRelay[2]   ? 1 : 0;
  doc["thresh1"]     = threshold[0];
  doc["thresh2"]     = threshold[1];
  doc["thresh3"]     = threshold[2];
  doc["uptime"]      = millis() / 1000;
  doc["rssi"]        = WiFi.RSSI();

  char buffer[512];
  serializeJson(doc, buffer);
  bool published = mqttClient.publish(TOPIC_SENSOR, buffer);

  Serial.printf("[TELEMETRY] %s | V: %.2fV | I: %.1fmA | P: %.1fmW | Motor: %s (%d) | R: [%d,%d,%d]\n",
                published ? "OK" : "GAGAL", busVoltage, current_mA, power_mW,
                motorState ? "ON" : "OFF", motorSpeed,
                relayState[0], relayState[1], relayState[2]);
}

// ==============================================================================
// 9. KONEKSI WIFI & MQTT
// ==============================================================================
void connectWiFi() {
  WiFi.disconnect(true);
  delay(200);
  Serial.printf("[WiFi] Menghubungkan ke SSID: %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint8_t count = 0;
  while (WiFi.status() != WL_CONNECTED && count < 30) {
    delay(500);
    Serial.print(F("."));
    count++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Terhubung! IP: %s (RSSI: %d dBm)\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println(F("[WiFi] Gagal konek dalam timeout. Akan dicoba kembali di loop..."));
  }
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.printf("[MQTT] Menghubungkan ke %s:%d ...\n", MQTT_HOST, MQTT_PORT);

  String willTopic = TOPIC_STATUS;
  String willMsg   = "{\"status\":\"offline\",\"id\":\"" + String(mqttClientId) + "\"}";

  if (mqttClient.connect(mqttClientId, nullptr, nullptr,
                         willTopic.c_str(), 1, true, willMsg.c_str())) {
    Serial.println(F("[MQTT] Terhubung ke broker EMQX!"));
    mqttClient.subscribe(TOPIC_CONTROL, 1);

    // Publish status online
    String onlineMsg = "{\"status\":\"online\",\"id\":\"" + String(mqttClientId) + "\"}";
    mqttClient.publish(TOPIC_STATUS, onlineMsg.c_str(), true);
  } else {
    Serial.printf("[MQTT] Gagal konek, rc=%d. Mencoba lagi nanti...\n", mqttClient.state());
  }
}

// ==============================================================================
// 10. SETUP & INISIALISASI
// ==============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println(F("\n========================================================"));
  Serial.println(F("   SMART ELECTROMAGNETIC GENERATOR - ESP32 FIRMWARE    "));
  Serial.println(F("   Jurusan Teknik Elektro - Universitas Negeri Malang   "));
  Serial.println(F("========================================================\n"));

  // Inisialisasi GPIO Motor
  pinMode(PIN_IN1, OUTPUT); digitalWrite(PIN_IN1, LOW);
  pinMode(PIN_IN2, OUTPUT); digitalWrite(PIN_IN2, LOW);

  // Inisialisasi Hardware PWM untuk Pin ENA L298N
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    ledcAttach(PIN_ENA, PWM_FREQ, PWM_RES);
    ledcWrite(PIN_ENA, 0);
  #else
    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
    ledcAttachPin(PIN_ENA, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, 0);
  #endif

  // Inisialisasi GPIO Relay (Default HIGH = Semua Relay MATI saat boot)
  pinMode(PIN_RELAY_1, OUTPUT); digitalWrite(PIN_RELAY_1, RELAY_IDLE_STATE);
  pinMode(PIN_RELAY_2, OUTPUT); digitalWrite(PIN_RELAY_2, RELAY_IDLE_STATE);
  pinMode(PIN_RELAY_3, OUTPUT); digitalWrite(PIN_RELAY_3, RELAY_IDLE_STATE);

  // Inisialisasi Sensor INA219 (I2C)
  Wire.begin(PIN_SDA, PIN_SCL);
  delay(100);
  if (ina219.begin()) {
    inaReady = true;
    ina219.setCalibration_32V_2A();
    Serial.println(F("[INA219] Sensor terdeteksi OK (Rentang 32V / 2A)."));
  } else {
    Serial.println(F("[INA219] PERINGATAN: Sensor tidak ditemukan! Periksa wiring SDA/SCL."));
  }

  // Buat Client ID unik dari MAC Address ESP32
  uint32_t chipId = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
  snprintf(mqttClientId, sizeof(mqttClientId), "smartem_%06X", chipId);
  Serial.printf("[MQTT] Client ID: %s\n", mqttClientId);

  // Konfigurasi Buffer MQTT
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(60);

  // Koneksi Awal
  connectWiFi();
  connectMQTT();

  Serial.println(F("[SYSTEM] Inisialisasi selesai. Memulai loop sistem...\n"));
}

// ==============================================================================
// 11. MAIN LOOP
// ==============================================================================
void loop() {
  // 1. Monitor Koneksi WiFi
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // 2. Monitor Koneksi MQTT
  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastReconnect >= RECONNECT_DELAY) {
      lastReconnect = now;
      connectMQTT();
    }
  } else {
    mqttClient.loop();
  }

  // 3. Publish Telemetri secara Berkala
  unsigned long currentMillis = millis();
  if (currentMillis - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = currentMillis;
    if (mqttClient.connected()) {
      publishSensor();
    }
  }
}
