// Flags

// Enable debug info from IoT library
// #define ENABLE_IOT_DEBUG

//#define SGP30


// Libraries
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <MQTTClient.h>
#include <EEPROM.h>
#include <SPIFFS.h>
#include "secrets.h" // credentials
#include <time.h>
#include <ctype.h> // for tolower

#include <M5Unified.h>
#ifdef SGP30
#include <Adafruit_SGP30.h>
Adafruit_SGP30 sgp;
#else
#include <M5Unified.h>
#include "M5UnitENV.h"
SHT3X sht3x;
#endif

String shadowSubscribeTopic = "";
String shadowPublishTopic = "";
const char *topicSubscribeC;
const char *topicPublishC;

const int BOOT_BUTTON_PIN = 0; // GPIO0 for most ESP32 dev kits

// Define the size of the MQTT message buffer
const int bufferSize = 1024 * 23; // 23552 bytes

// MQTT functions
void connectWiFi();
void connectAWS();
void messageHandlerIoT(String &topic, String &payload);

void initialiseSensor();
void sendSensor();
float readTemperature();
float readHumidity();

WiFiClientSecure net = WiFiClientSecure();
MQTTClient client = MQTTClient(bufferSize);

unsigned long lastRead = 0;
unsigned long lastPublish = 0;

void setup()
{
  Serial.begin(115200);

  //Wire.begin(0, 26); // SDA, SCL for M5StickC
  M5.begin();

  M5.Lcd.setRotation(3);
  M5.Lcd.fillScreen(WHITE);
  M5.Lcd.setTextColor(BLACK, WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 10);

  M5.Lcd.println("Starting");

  initialiseSensor();

  connectWiFi();
  M5.Lcd.println("WiFi connected");

  // Synchronize time, for Certificate validation
  configTime(0, 0, "pool.ntp.org"); // Initialize and synchronize time with NTP server
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to obtain time");
    return;
  }
  char timeString[50];
  strftime(timeString, sizeof(timeString), "%A, %B %d %Y %H:%M:%S", &timeinfo);
  Serial.println(timeString);

  shadowSubscribeTopic = String("$aws/things/") + THINGNAME + "/shadow/update/delta";
  shadowPublishTopic = String("$aws/things/") + THINGNAME + "/shadow/update";
  topicSubscribeC = shadowSubscribeTopic.c_str();
  topicPublishC = shadowPublishTopic.c_str();

  connectAWS();
  M5.Lcd.println("AWS connected");
}

void loop()
{
  if (client.connected())
    client.loop();
  if (millis() - lastRead >= 2000) {
    lastRead = millis();
    sendSensor();
  }
}

void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println("Connecting to Wi-Fi: " + String(WIFI_SSID));

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");

  Serial.print("WiFi connected. IP Address: ");
  Serial.println(WiFi.localIP());
}

void connectAWS()
{
  // CA Cert is static
  net.setCACert(AWS_CERT_CA);

  // Configure WiFiClientSecure to use the AWS IoT device credentials
  net.setCertificate(IOT_CERT);
  net.setPrivateKey(IOT_KEY);

  // Connect to the MQTT broker on the AWS endpoint we defined earlier
  client.begin(AWS_IOT_ENDPOINT, 8883, net);
  client.setCleanSession(true);

  client.onMessage(messageHandlerIoT);

  Serial.print("Connecting to AWS IOT");

  int retryCount = 0;
  while (!client.connect(AWS_IOT_ENDPOINT) && retryCount < 3)
  {
    if (retryCount++ < 3)
    {
      Serial.print(".");
      delay(500);
    }
  }
  Serial.println("");

  if (!client.connected())
  {
    Serial.println("AWS IoT Timeout! Restarting.");
    delay(1000);
    ESP.restart();
    return;
  }

  // Subscribe to shadow topics
  Serial.println("Connected directly to IoT Core, subscribing to shadow topic");
  client.subscribe(topicSubscribeC);

  Serial.println("AWS IoT Connected!");
}

void messageHandlerIoT(String &topic, String &payload)
{
  if (payload != NULL && topic != NULL)
  {
    Serial.print("Received Message: ");
    Serial.println(payload);
    Serial.print("On Topic: ");
    Serial.println(topic);

      M5.Lcd.fillScreen(WHITE);
      M5.Lcd.setCursor(10, 10);
      M5.Lcd.println(payload);
  }
}

void sendSensor()
{
  // Send current button state
  StaticJsonDocument<200> doc;
  #ifdef SGP30
  float temperature = readTemperature();
  doc["temperature"] = temperature;
  doc["humidity"] = temperature;
  #else
  float temperature = readTemperature();
  doc["temperature"] = temperature;
  float humidity = readHumidity();
  doc["humidity"] = humidity;
  #endif
  
  String payload;
  serializeJson(doc, payload);

  // publish payload
  const char *topic_c = shadowPublishTopic.c_str();
  const char *payload_c = payload.c_str();
  unsigned int payloadLen = (unsigned int)(payload.length() + 1);

  bool success = false;

  int retryCount = 0;
  if (!client.connected())
  {
    connectAWS();
  }
  if (client.connected())
  {
    while (!success)
    {
      success = client.publish(topic_c, payload_c, payloadLen);
      if (success)
        break;
      retryCount++;
      if (retryCount == 1)
      {
        Serial.print("Failed to publish to " + shadowPublishTopic + ", retrying.");
      }
      else if (retryCount < 10)
      {
        Serial.print(".");
        delay(500);
      }
      else
      {
        break;
      }
    }
  }
  else
  {
    Serial.println("Aborting, client not connected");
  }
  if (success)
  {
    Serial.println("Sent: " + payload);
  }
  else
  {
    Serial.println("Aborting, failed to send");
  }

  M5.Lcd.fillScreen(WHITE);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.print("Temp: ");
  M5.Lcd.print(temperature);
  M5.Lcd.println(" °C");
  M5.Lcd.print("Humidity: ");
  M5.Lcd.print(humidity);
  M5.Lcd.println(" g/kg");

  M5.update();
}

void initialiseSensor() {
  #ifdef SGP30
  // begin SGP30
  if (! sgp.begin()){
    Serial.println("Sensor not found :(");
    while (1);
  }
  Serial.print("Found SGP30 serial #");
  Serial.print(sgp.serialnumber[0], HEX);
  Serial.print(sgp.serialnumber[1], HEX);
  Serial.println(sgp.serialnumber[2], HEX);

  M5.Lcd.println("SGP30 initialized");
  delay(1000);
  #else
  if (!sht3x.begin(&Wire, SHT3X_I2C_ADDR, 0, 26, 400000U)) {
      M5.Lcd.println("Sensor not found");
      while (1) {
        Serial.println("Couldn't find sensor");
        delay(1000);
      }
  }
  #endif
}


float readTemperature()
{
  #ifdef SGP30
  if (! sgp.IAQmeasure()) {
    Serial.println("Measurement failed");
    return -1.0;
  }
  return sgp.eCO2;
  #else
  if (sht3x.update()) {
    return sht3x.cTemp;
  }
  return -1.0;
  #endif
}


float readHumidity()
{
  #ifdef SGP30
  return 100.0;
  #else
  if (sht3x.update()) {
    return sht3x.humidity;
  }
  return -1.0;
  #endif
}
