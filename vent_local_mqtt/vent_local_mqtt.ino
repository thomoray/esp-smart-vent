#include <Servo.h>
#include "wifi.h"

#include <WEMOS_Motor.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <FS.h>

const int servoOutputPin = 0;
const int servoMin = 450;
const int servoMax = 2600;
const int servoSensorPin = A0;

const int servoSensorValueThreshold = 165;

const int maxClosedPosition = 0;
const int maxOpenedPosition = 180;
const int positionOffset = 7;

int openedPosition;
int closedPosition;
int currentPosition;

Servo servo;

uint8_t mqttRetryCounter = 0;

WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient mqttClient;

WiFiManagerParameter custom_mqtt_server("mqtt server", "mqtt server", mqtt_server, sizeof(mqtt_server));
WiFiManagerParameter custom_mqtt_port("mqtt port", "mqtt port", mqtt_port, sizeof(mqtt_port));
WiFiManagerParameter custom_mqtt_username("mqtt username", "mqtt username", mqtt_username, sizeof(mqtt_username));
WiFiManagerParameter custom_mqtt_password("mqtt password", "mqtt password", mqtt_password, sizeof(mqtt_password));
WiFiManagerParameter custom_mqtt_topic("mqtt topic", "mqtt_topic", mqtt_topic, sizeof(mqtt_topic));

unsigned long lastMqttConnectionAttempt = millis();
const long mqttConnectionInterval = 60000;

char identifier[24];
#define AVAILABILITY_ONLINE "online"
#define AVAILABILITY_OFFLINE "offline"
char MQTT_COMMAND_TOPIC[128];
char MQTT_STATE_TOPIC[128];
char MQTT_AVAILABILITY_TOPIC[128];

bool shouldSaveConfig = false;

void saveConfigCallback () {
  shouldSaveConfig = true;
}

void setup() {
  Serial.begin(9600);
  pinMode(INPUT, servoSensorPin);
  pinMode(OUTPUT, servoSensorPin);

  delay(3000);

  snprintf(identifier, sizeof(identifier), "VENT-%X", ESP.getChipId());

  WiFi.hostname(identifier);

  loadConfig();

  snprintf(MQTT_COMMAND_TOPIC, 127, "%s/%s", "cmd", mqtt_topic);
  snprintf(MQTT_STATE_TOPIC, 127, "%s/%s", "stat", mqtt_topic);
  snprintf(MQTT_AVAILABILITY_TOPIC, 127, "%s/%s/%s", "tele", mqtt_topic, "LWT");

  setupWifi();
  mqttClient.setServer(mqtt_server, std::strtol(mqtt_port, nullptr, 10));
  mqttClient.setKeepAlive(10);
  mqttClient.setBufferSize(2048);
  mqttClient.setCallback(mqttCallback);

  mqttReconnect();

  closedPosition = close(90, 45) + positionOffset;
  openedPosition = open(currentPosition, 90) - positionOffset;
}

void loop() {
  mqttClient.loop();

  if (!mqttClient.connected() && (mqttConnectionInterval <= (millis() - lastMqttConnectionAttempt)) )  {
    lastMqttConnectionAttempt = millis();
    mqttReconnect();
  }
}

void setupWifi() {
  wifiManager.setDebugOutput(false);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_username);
  wifiManager.addParameter(&custom_mqtt_password);
  wifiManager.addParameter(&custom_mqtt_topic);

  WiFi.hostname(identifier);
  wifiManager.autoConnect(identifier);
  mqttClient.setClient(wifiClient);

  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_username, custom_mqtt_username.getValue());
  strcpy(mqtt_password, custom_mqtt_password.getValue());
  strcpy(mqtt_topic, custom_mqtt_topic.getValue());

  if (shouldSaveConfig) {
    saveConfig();
  } else {
    //For some reason, the read values get overwritten in this function
    //To combat this, we just reload the config
    //This is most likely a logic error which could be fixed otherwise
    loadConfig();
  }
}

void resetWifiSettingsAndReboot() {
  wifiManager.resetSettings();
  delay(3000);
  ESP.restart();
}

void mqttReconnect()
{
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (mqttClient.connect(identifier, mqtt_username, mqtt_password, MQTT_AVAILABILITY_TOPIC, 1, true, AVAILABILITY_OFFLINE)) {
      mqttClient.publish(MQTT_AVAILABILITY_TOPIC, AVAILABILITY_ONLINE, true);

      mqttClient.subscribe(MQTT_COMMAND_TOPIC);
      break;
    } else {
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_COMMAND_TOPIC) == 0) {
    char payloadText[length + 1];

    snprintf(payloadText, length + 1, "%s", payload);

    if (isOpen(payloadText) || isClose(payloadText)) {
      for (int i = 0; i < 100; i++)
      {
        if (isOpen(payloadText)) {
          open(currentPosition, 90);
        } else if (isClose(payloadText)) {
          close(currentPosition, 90);
        }

        delay(10);
      }
    }
  }
}

boolean isMqttConnected() {
  return mqttClient.connected();
}

int close(int startPosition, int minDegreesTraveled) {
  Serial.println("closing");
  return turnServoUntilEndStop(startPosition, maxClosedPosition, startPosition, -1, maxClosedPosition, minDegreesTraveled);
}

int open(int startPosition, int minDegreesTraveled) {
  Serial.println("opening");
  return turnServoUntilEndStop(startPosition, startPosition, maxOpenedPosition, 1, maxOpenedPosition, minDegreesTraveled);
}

int turnServoUntilEndStop(int startPosition, int leftNumber, int rightNumber, int incrementBy, int maxPosition, int minDegreesTraveled) {
  servo.attach(servoOutputPin, servoMin, servoMax);

  for(int position = startPosition; leftNumber < rightNumber; position += incrementBy) {
    int degreesTraveled = (position - startPosition) * incrementBy;
    if (hasHitEndstopAndTurnOneDegree(position, degreesTraveled, minDegreesTraveled)) {
      return position;
    }  
  }

  return maxPosition;
}

bool hasHitEndstopAndTurnOneDegree(int position, int degreesTraveled, int minDegreesTraveled) {
  bool hasTraveledFarEnough = degreesTraveled >= minDegreesTraveled;
  int servoSensorValue = analogRead(servoSensorPin);
  bool hasHitVoltageThreshold = servoSensorValue >= servoSensorValueThreshold;

  if (hasTraveledFarEnough && hasHitVoltageThreshold) {
    servo.detach();
    return true;
  } else {
    servo.write(position);
    currentPosition = position;
  }

  delay(30); 
  return false;
}

boolean isOpen(char* payloadText) {
  return strcmp(payloadText, "open") == 0;
}

boolean isClose(char* payloadText) {
  return strcmp(payloadText, "close") == 0;
}

//boolean isOpened() {
//  return digitalRead(opened_pin) == 0;
//}
//
//boolean isClosed() {
//  return digitalRead(closed_pin) == 0;
//}

