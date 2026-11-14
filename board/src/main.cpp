#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include "secrets.h"

// ---------- CONFIG: edit these ----------
#define DHT_PIN 15
#define LED_PIN 2
#define MQTT_NAME "dylan"  // same as MQTT_NAME in api/.env
static const char* API_URL = "http://iot-dbeckford.canadacentral.cloudapp.azure.com/readings";
// ----------------------------------------

static const char* WIFI_SSID = "Wokwi-GUEST";
static const char* WIFI_PASS = "";
static const char* BROKER_HOST = "135.232.203.144";
static const uint16_t BROKER_PORT = 1883;

static const char* TOPIC_SETTINGS = "ecse3038/" MQTT_NAME "/fan/settings";
static const char* TOPIC_STATE = "ecse3038/" MQTT_NAME "/fan/state";
static const char* TOPIC_STATUS = "ecse3038/" MQTT_NAME "/status";

static const unsigned long READ_INTERVAL_MS = 10000;
static const unsigned long MQTT_RETRY_MS = 5000;

DHT dht(DHT_PIN, DHT22);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

String macAddr;

// fan state
bool haveSettings = false;
String fanMode = "off";
float maxHumidity = 100.0;
float lastHumidity = NAN;
bool fanOn = false;
bool statePending = false;  // fan changed while MQTT was down

unsigned long lastRead = 0;
unsigned long lastMqttTry = 0;

// ---------- publish fan state (B5) ----------
void publishState() {
  if (!mqtt.connected()) {
    statePending = true;  // send on reconnect
    return;
  }
  statePending = false;
  mqtt.publish(TOPIC_STATE, fanOn ? "on" : "off", true);  // retained
}

// ---------- fan logic (B4, B5) ----------
void applyFan() {
  bool want = false;
  if (haveSettings) {
    if (fanMode == "on") {
      want = true;
    } else if (fanMode == "off") {
      want = false;
    } else if (fanMode == "auto") {
      want = !isnan(lastHumidity) && lastHumidity > maxHumidity;
    }
  }

  digitalWrite(LED_PIN, want ? HIGH : LOW);

  if (want != fanOn) {
    fanOn = want;
    Serial.printf("Fan -> %s\n", fanOn ? "on" : "off");
    publishState();  // only when the fan changes
  }
}

// ---------- MQTT callback (B3) ----------
void onMessage(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_SETTINGS) != 0) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println("Settings refused: not valid JSON");
    return;
  }
  if (!doc.is<JsonObject>()) {
    Serial.println("Settings refused: not a JSON object");
    return;
  }

  JsonVariant modeV = doc["mode"];
  JsonVariant maxV = doc["max_humidity"];
  if (modeV.isNull() || maxV.isNull()) {
    Serial.println("Settings refused: missing field");
    return;
  }
  if (!modeV.is<const char*>() || !maxV.is<float>()) {
    Serial.println("Settings refused: wrong field type");
    return;
  }

  String mode = modeV.as<String>();
  if (mode != "auto" && mode != "on" && mode != "off") {
    Serial.println("Settings refused: bad mode");
    return;
  }

  fanMode = mode;
  maxHumidity = maxV.as<float>();
  haveSettings = true;
  Serial.printf("Settings accepted: mode=%s max_humidity=%.1f\n", fanMode.c_str(), maxHumidity);

  applyFan();  // react immediately
}

// ---------- MQTT connect (B2) ----------
void mqttConnect() {
  String clientId = "fan-" + macAddr;
  clientId.replace(":", "");

  // will: offline, retained, QoS 1
  bool ok = mqtt.connect(clientId.c_str(), nullptr, nullptr,
                         TOPIC_STATUS, 1, true, "offline");
  if (ok) {
    Serial.println("MQTT connected");
    mqtt.publish(TOPIC_STATUS, "online", true);  // retained
    mqtt.subscribe(TOPIC_SETTINGS);
    if (statePending) publishState();  // fan changed while offline
  } else {
    Serial.printf("MQTT connect failed, rc=%d\n", mqtt.state());
  }
}

// ---------- WiFi ----------
void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println(" connected");
}

// ---------- POST reading (B1) ----------
void postReading(float humidity) {
  if (WiFi.status() != WL_CONNECTED) return;

  JsonDocument doc;
  doc["mac"] = macAddr;
  doc["humidity"] = humidity;
  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(API_URL);
  http.setTimeout(3000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", DEVICE_KEY);
  int code = http.POST(body);
  Serial.printf("POST /readings %s -> %d\n", body.c_str(), code);
  http.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  dht.begin();

  wifiConnect();
  macAddr = WiFi.macAddress();
  Serial.print("MAC: ");
  Serial.println(macAddr);

  mqtt.setServer(BROKER_HOST, BROKER_PORT);
  mqtt.setBufferSize(512);
  mqtt.setCallback(onMessage);
  mqttConnect();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnect();
  }

  if (!mqtt.connected()) {
    if (millis() - lastMqttTry >= MQTT_RETRY_MS) {
      lastMqttTry = millis();
      mqttConnect();
    }
  } else {
    mqtt.loop();
  }

  if (lastRead == 0 || millis() - lastRead >= READ_INTERVAL_MS) {
    lastRead = (lastRead == 0) ? millis() : lastRead + READ_INTERVAL_MS;  // fixed schedule
    float h = dht.readHumidity();
    if (isnan(h)) {
      Serial.println("DHT read failed");
    } else {
      lastHumidity = h;
      postReading(h);
      applyFan();
    }
  }
}