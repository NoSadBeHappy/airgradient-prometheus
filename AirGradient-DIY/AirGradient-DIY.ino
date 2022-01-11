/**
 * This sketch connects an AirGradient DIY sensor to a WiFi network, and runs a
 * tiny HTTP server to serve air quality metrics to Prometheus.
 */

#include <AirGradient.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>

#include <Adafruit_BME280.h>

#include <Wire.h>
#include "SSD1306Wire.h"

#include <EEPROM.h>
#include <ArduinoJson.h>

AirGradient ag = AirGradient();

// Config ----------------------------------------------------------------------

// Optional.
const char *deviceId = "";

// Hardware options for AirGradient DIY sensor.
const bool hasPM = true;
const bool hasCO2 = true;
const bool hasSHT = false;
const bool hasBME = true;

// WiFi and IP connection info.
const char *ssid = "PleaseChangeMe";
const char *password = "PleaseChangeMe";
const int port = 9926;

// Uncomment the line below to configure a static IP address.
// #define STATIC_IP
#ifdef STATIC_IP
IPAddress staticIp(192, 168, 0, 0);
IPAddress gateway(192, 168, 0, 0);
IPAddress subnet(255, 255, 255, 0);
#endif

// Uncomment to flip the display 180 degrees.
#define FLIP_SCREEN

// Max Temp offset
#define MAX_TEMP_OFFSET 5

// The frequency of measurement updates.
const int updateFrequency = 5000;

// For housekeeping.
long lastUpdate;
int counter = 0;

// Config End ------------------------------------------------------------------

SSD1306Wire display(0x3c, SDA, SCL);
ESP8266WebServer server(port);

struct
{
  float temperatureOffset;
} permanentSettings;

Adafruit_BME280 bme; // use I2C interface
Adafruit_Sensor *bme_temp = bme.getTemperatureSensor();
Adafruit_Sensor *bme_pressure = bme.getPressureSensor();
Adafruit_Sensor *bme_humidity = bme.getHumiditySensor();

void setup()
{
  Serial.begin(9600);

  // Initializing EEPROM
  EEPROM.begin(sizeof(permanentSettings));
  EEPROM.get(0, permanentSettings);
  if (abs(permanentSettings.temperatureOffset) > MAX_TEMP_OFFSET)
  {
    // Offset > +/- MAX_TEMP_OFFSET°C we consider it has not been initialized yet
    // Initializing to 0 & storing it to EEPROM
    permanentSettings.temperatureOffset = 0;
    EEPROM.put(0, permanentSettings);
    EEPROM.commit();
  }

  // Init Display.
  display.init();
#ifdef FLIP_SCREEN
  display.flipScreenVertically();
#endif
  showTextRectangle("Init", String(ESP.getChipId(), HEX), true);

  // Enable enabled sensors.
  if (hasPM)
    ag.PMS_Init();
  if (hasCO2)
    ag.CO2_Init();
  if (hasSHT)
    ag.TMP_RH_Init(0x44);
  if (hasBME)
    bme.begin();

// Set static IP address if configured.
#ifdef STATIC_IP
  WiFi.config(staticIp, gateway, subnet);
#endif

  // Set WiFi mode to client (without this it may try to act as an AP).
  WiFi.mode(WIFI_STA);

  // Configure Hostname
  if ((deviceId != NULL) && (deviceId[0] == '\0'))
  {
    Serial.printf("No Device ID is Defined, Defaulting to board defaults");
  }
  else
  {
    wifi_station_set_hostname(deviceId);
    WiFi.setHostname(deviceId);
  }

  // Setup and wait for WiFi.
  WiFi.begin(ssid, password);
  Serial.println("");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    showTextRectangle("Trying to", "connect...", true);
    Serial.print(".");
  }

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("MAC address: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Hostname: ");
  Serial.println(WiFi.hostname());
  server.on("/", HandleRoot);
  server.on("/metrics", HandleRoot);
  server.on("/offset", HTTP_GET, GetOffset);
  server.on("/offset", HTTP_PUT, SetOffset);

  server.onNotFound(HandleNotFound);

  server.begin();
  Serial.println("HTTP server started at ip " + WiFi.localIP().toString() + ":" + String(port));
  showTextRectangle("Listening To", WiFi.localIP().toString() + ":" + String(port), true);
}

void loop()
{
  long t = millis();

  server.handleClient();
  updateScreen(t);
}

void SetOffset()
{
  StaticJsonBuffer<500> jsonBuffer;

  JsonObject &jsonBody = jsonBuffer.parseObject(server.arg("plain"));

  if (!jsonBody.success())
  {
    //Serial.println("error in parsin json body");
    server.send(400);
    return;
  }
  if (true) // We might want to include some control on the MacAddress and/or DeviceName here
  {
    permanentSettings.temperatureOffset = jsonBody['Offset'];
    EEPROM.put(0, permanentSettings);
    EEPROM.commit();
  }
}

void GetOffset()
{
  String message = "";
  message += "Current Temperature Offset is: ";
  message += String(permanentSettings.temperatureOffset);
  server.send(200, "text/plain", message);
}

String GenerateMetrics()
{
  String message = "";
  String idString = "{id=\"" + String(deviceId) + "\",mac=\"" + WiFi.macAddress().c_str() + "\"}";

  if (hasPM)
  {
    int stat = ag.getPM2_Raw();

    message += "# HELP pm02 Particulate Matter PM2.5 value\n";
    message += "# TYPE pm02 gauge\n";
    message += "pm02";
    message += idString;
    message += String(stat);
    message += "\n";
  }

  if (hasCO2)
  {
    int stat = ag.getCO2_Raw();

    message += "# HELP rco2 CO2 value, in ppm\n";
    message += "# TYPE rco2 gauge\n";
    message += "rco2";
    message += idString;
    message += String(stat);
    message += "\n";
  }

  if (hasSHT)
  {
    TMP_RH stat = ag.periodicFetchData();

    message += "# HELP atmp Temperature, in degrees Celsius\n";
    message += "# TYPE atmp gauge\n";
    message += "atmp";
    message += idString;
    message += String(stat.t + permanentSettings.temperatureOffset);
    message += "\n";

    message += "# HELP rhum Relative humidity, in percent\n";
    message += "# TYPE rhum gauge\n";
    message += "rhum";
    message += idString;
    message += String(stat.rh);
    message += "\n";
  }

  if (hasBME)
  {
    sensors_event_t temp_event, pressure_event, humidity_event;
    bme_temp->getEvent(&temp_event);
    bme_pressure->getEvent(&pressure_event);
    bme_humidity->getEvent(&humidity_event);

    message += "# HELP atmp Temperature, in degrees Celsius\n";
    message += "# TYPE atmp gauge\n";
    message += "atmp";
    message += idString;
    message += String(temp_event.temperature + permanentSettings.temperatureOffset);
    message += "\n";

    message += "# HELP rhum Relative humidity, in percent\n";
    message += "# TYPE rhum gauge\n";
    message += "rhum";
    message += idString;
    message += String(humidity_event.relative_humidity);
    message += "\n";

    message += "# HELP apre Athmospherique pressure, in hPa\n";
    message += "# TYPE apre gauge\n";
    message += "apre";
    message += idString;
    message += String(pressure_event.pressure);
    message += "\n";
  }

  return message;
}

void HandleRoot()
{
  server.send(200, "text/plain", GenerateMetrics());
}

void HandleNotFound()
{
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint i = 0; i < server.args(); i++)
  {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/html", message);
}

// DISPLAY
void showTextRectangle(String ln1, String ln2, boolean small)
{
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  if (small)
  {
    display.setFont(ArialMT_Plain_16);
  }
  else
  {
    display.setFont(ArialMT_Plain_24);
  }
#ifdef FLIP_SCREEN
  display.drawString(32, 16, ln1);
  display.drawString(32, 36, ln2);
#else
  display.drawString(32, 0, ln1);
  display.drawString(32, 20, ln2);
#endif
  display.display();
}

void updateScreen(long now)
{
  if ((now - lastUpdate) > updateFrequency)
  {
    // Take a measurement at a fixed interval.
    switch (counter)
    {
    case 0:
      if (hasPM)
      {
        int stat = ag.getPM2_Raw();
        showTextRectangle("Particules 2.5", String(stat), true);
      }
      break;
    case 1:
      if (hasCO2)
      {
        int stat = ag.getCO2_Raw();
        showTextRectangle("CO2", String(stat), false);
      }
      break;
    case 2:
      if (hasSHT)
      {
        TMP_RH stat = ag.periodicFetchData();
        showTextRectangle("Température", String(stat.t + permanentSettings.temperatureOffset, 1) + "C", true);
      }
      if (hasBME)
      {
        sensors_event_t temp_event;
        bme_temp->getEvent(&temp_event);
        showTextRectangle("Température", String(temp_event.temperature + permanentSettings.temperatureOffset, 1) + "C", true);
      }
      break;
    case 3:
      if (hasSHT)
      {
        TMP_RH stat = ag.periodicFetchData();
        showTextRectangle("Humidité", String(stat.rh) + "%", true);
      }
      if (hasBME)
      {
        sensors_event_t humidity_event;
        bme_humidity->getEvent(&humidity_event);
        showTextRectangle("Humidité", String(humidity_event.relative_humidity, 1) + "%", true);
      }
      break;
    case 4:
      if (hasBME)
      {
        sensors_event_t pressure_event;
        bme_pressure->getEvent(&pressure_event);
        showTextRectangle("Pression", String(pressure_event.pressure, 1) + "hPa", true);
      }
      break;
    }
    counter++;
    if (counter > 4)
      counter = 0;
    lastUpdate = millis();
  }
}
