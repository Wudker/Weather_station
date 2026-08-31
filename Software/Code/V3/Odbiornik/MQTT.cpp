#include "MQTT.h"
#include "Secret_keys.h"

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t MQTT_RETRY_MS = 5000;
constexpr uint16_t MQTT_BUFFER_SIZE = 768;

#define MQTT_ROOT "smartroom/weather_station"

static const char TOPIC_AVAILABILITY[]     = MQTT_ROOT "/availability";
static const char TOPIC_STATE[]            = MQTT_ROOT "/state";
static const char TOPIC_TIMESTAMP[]        = MQTT_ROOT "/timestamp";
static const char TOPIC_TEMPERATURE[]      = MQTT_ROOT "/temperature_c";
static const char TOPIC_HUMIDITY[]         = MQTT_ROOT "/humidity_pct";
static const char TOPIC_PRESSURE[]         = MQTT_ROOT "/pressure_hpa";
static const char TOPIC_WIND[]             = MQTT_ROOT "/wind_m_s";
static const char TOPIC_RAIN[]              = MQTT_ROOT "/rain_mv";
static const char TOPIC_BATTERY[]           = MQTT_ROOT "/battery_mv";
static const char TOPIC_BATTERY_PERCENT[]   = MQTT_ROOT "/battery_pct";
static const char TOPIC_SUN[]               = MQTT_ROOT "/sun_v";
static const char TOPIC_STATUS[]            =MQTT_ROOT "/status";

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
uint32_t nextMqttAttemptMs = 0;

float batteryPercent(float batteryMv)
{
    float value = ((batteryMv / 1000.0f) - 3.2f) * 100.0f;
    if (value < 0.0f) value = 0.0f;
    if (value > 100.0f) value = 100.0f;
    return value;
}

String mqttClientId()
{
    String id = "Pogodynka-";
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    id += mac;
    return id;
}

bool mqttConnect()
{
    if (WiFi.status() != WL_CONNECTED) return false;

    String clientId = mqttClientId();
    bool connected;

    if (mqttUsername != nullptr && mqttUsername[0] != '\0') {
        connected = mqttClient.connect(clientId.c_str(),
                                       mqttUsername,
                                       mqttPassword,
                                       TOPIC_AVAILABILITY,
                                       1,
                                       true,
                                       "offline");
    } else {
        connected = mqttClient.connect(clientId.c_str(),
                                       TOPIC_AVAILABILITY,
                                       1,
                                       true,
                                       "offline");
    }

    if (connected) {
        Serial.println("MQTT: polaczono");
        mqttClient.publish(TOPIC_AVAILABILITY, "online", true);
    } else {
        Serial.print("MQTT: blad polaczenia, stan ");
        Serial.println(mqttClient.state());
    }

    return connected;
}

bool publishValue(const char* topic, float value, uint8_t decimals)
{
    char payload[24];
    dtostrf(value, 0, decimals, payload);
    return mqttClient.publish(topic, payload, true);
}

} // namespace

void mqttBegin()
{
    mqttClient.setServer(mqttBroker_ip, mqttBroker_port);
    mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
    mqttClient.setKeepAlive(30);
    mqttClient.setSocketTimeout(2);
}

void mqttLoop()
{
    if (WiFi.status() != WL_CONNECTED) return;

    if (!mqttClient.connected()) {
        if ((int32_t)(millis() - nextMqttAttemptMs) < 0) return;
        nextMqttAttemptMs = millis() + MQTT_RETRY_MS;
        mqttConnect();
        return;
    }

    mqttClient.loop();
}

bool mqttConnected()
{
    return WiFi.status() == WL_CONNECTED && mqttClient.connected();
}



bool mqttPublishMeasurement(const Dane_ESP& d)
{
    if (!mqttConnected()) return false;

    const float pressureHpa = d.Pressure / 100.0f;
    const float batteryPct = batteryPercent(d.Battery_level);
    const float sunV = d.Sun_level / 1000.0f;

    char timestamp[24];
    snprintf(timestamp, sizeof(timestamp),
             "20%02d-%02d-%02dT%02d:%02d:00",
             d.Year, d.Month, d.Day, d.Hour, d.Minute);

    char json[512];
    snprintf(json, sizeof(json),
             "{\"timestamp\":\"%s\",\"temperature_c\":%.2f,"
             "\"humidity_pct\":%.2f,\"pressure_pa\":%.0f,"
             "\"pressure_hpa\":%.2f,\"wind_m_s\":%.2f,"
             "\"rain_mv\":%.0f,\"battery_mv\":%.0f,"
             "\"battery_pct\":%.1f,\"sun_mv\":%.0f,\"sun_v\":%.3f}",
             timestamp,
             d.Temperature,
             d.Humility,
             d.Pressure,
             pressureHpa,
             d.Wind_speed,
             d.Rain,
             d.Battery_level,
             batteryPct,
             d.Sun_level,
             sunV);

    bool ok = mqttClient.publish(TOPIC_STATE, json, true);
    ok = mqttClient.publish(TOPIC_TIMESTAMP, timestamp, true) && ok;
    ok = publishValue(TOPIC_TEMPERATURE, d.Temperature, 2) && ok;
    ok = publishValue(TOPIC_HUMIDITY, d.Humility, 2) && ok;
    ok = publishValue(TOPIC_PRESSURE, pressureHpa, 2) && ok;
    ok = publishValue(TOPIC_WIND, d.Wind_speed, 2) && ok;
    ok = publishValue(TOPIC_RAIN, d.Rain, 0) && ok;
    ok = publishValue(TOPIC_BATTERY, d.Battery_level, 0) && ok;
    ok = publishValue(TOPIC_BATTERY_PERCENT, batteryPct, 1) && ok;
    ok = publishValue(TOPIC_SUN, sunV, 3) && ok;

    if (ok) Serial.println("MQTT: opublikowano pomiar");
    return ok;
}
