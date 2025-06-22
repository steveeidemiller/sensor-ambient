/**
 * @file  Sensor-Ambient.ino
 * @brief ESP32-based sound, light and environmental sensor with MQTT, TFT display, and web server support
 */

// Libraries
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>     // MQTT TLS support
#include <PubSubClient.h>         // MQTT support from knolleary
#include <ESPmDNS.h>              // mDNS support
#include <WebServer.h>            // HTTP web server support
#include <driver/i2s.h>           // I2S support for SPH0645 sound sensor
#include <Adafruit_MAX1704X.h>    // LiPo battery support
#include <Adafruit_VEML7700.h>    // VEML7700 ambient light sensor support
#include <Adafruit_ST7789.h>      // TFT display support
#include <Fonts/FreeSans9pt7b.h>  // TFT display support
//#include <Fonts/FreeSans12pt7b.h> // TFT display support
#include <SE_BME680.h>            // BME680 support
#include <hal/efuse_hal.h>        // Espressif ESP32 chip information
#include <time.h>                 // NTP and time support

// App configuration
#include <MeasurementTracker.h>
#include <html.h>                 // HTML templates
#include <config.h>               // The configuration references objects in the above libraries, so include it after those

// Data tracking
#define I2C_INTERVAL 6000 // Millseconds between reading I2C devices, such as the light sensor, environmental sensor, and battery monitor. This value must accomodate the max time each sensor takes for a reading, such as the VEML7700 which can take over 5 seconds, to avoid impacting the BME680 IAQ calculation.
#define MEASUREMENT_TRACKING_DATA_POINTS MEASUREMENT_WINDOW / (I2C_INTERVAL / 1000) // Number of data points to keep in MeasurementTracker() instances to achieve the desired measurement window

// ESP32
char chipInformation[100];   // Chip information buffer

// Web server
WebServer webServer(80);
char webStringBuffer[18 * 1024];

// MQTT
//WiFiClient espClient;     // For non-TLS connections
WiFiClientSecure espClient; // Use WiFiClientSecure for TLS MQTT connections
PubSubClient mqttClient(espClient);
unsigned long mqttLastConnectionAttempt = 0;

// TFT display
Adafruit_ST7789 display = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(TFT_SCREEN_WIDTH, TFT_SCREEN_HEIGHT);
int64_t displayTimer = 0; // Timestamp of the last button press used to turn on the display

// SPH0645 I2S sound sensor
SemaphoreHandle_t xMutexSoundSensor; // Mutex to protect shared variables between tasks
int32_t soundSensorDataBuffer[I2S_DMA_BUF_LEN]; // I2S read buffer
MeasurementTracker soundSensorSpl = MeasurementTracker(MEASUREMENT_TRACKING_DATA_POINTS);

// VML7700 light sensor
Adafruit_VEML7700 lightSensor = Adafruit_VEML7700();
SemaphoreHandle_t xMutexLightSensor; // Mutex to protect shared variables between tasks
MeasurementTracker lightSensorLux = MeasurementTracker(MEASUREMENT_TRACKING_DATA_POINTS);
float lightSensorGain = VEML7700_GAIN_1; // Current gain setting for the light sensor, read/updated from the AGC
int lightSensorIntegrationTime = VEML7700_IT_100MS; // Current integration time setting for the light sensor, read/updated from the AGC

// BME680
SE_BME680 bme680;
SemaphoreHandle_t xMutexEnvironmental;     // Mutex to protect shared variables between tasks
bool environmentSensorOK = false;          // True if the sensor is operational
MeasurementTracker environmentTemperature = MeasurementTracker(MEASUREMENT_TRACKING_DATA_POINTS);
MeasurementTracker environmentDewPoint    = MeasurementTracker(MEASUREMENT_TRACKING_DATA_POINTS);
MeasurementTracker environmentHumidity    = MeasurementTracker(MEASUREMENT_TRACKING_DATA_POINTS);
MeasurementTracker environmentPressure    = MeasurementTracker(MEASUREMENT_TRACKING_DATA_POINTS);
MeasurementTracker environmentIAQ         = MeasurementTracker(MEASUREMENT_TRACKING_DATA_POINTS); // 0-100% representing "bad" to "good"
int      environmentIAQAccuracy;           // 0 = unreliable, 1 = low, 2 = medium, 3 = high, 4 = very high
uint32_t environmentGasResistance;         // MOX gas resistance
float    environmentGasAccuracy;           // Accuracy of gas calibration as a percentage
int      environmentGasCalibrationStage;

// Uptime calculations
SemaphoreHandle_t xMutexUptime; // Mutex to protect shared variables between tasks
int64_t uptimeSecondsTotal, uptimeDays, uptimeHours, uptimeMinutes, uptimeSeconds;
int64_t lastUptimeSecondsTotal = 0;
char uptimeStringBuffer[24]; // Used by multiple threads

// LiPo battery and AC power state
SemaphoreHandle_t xMutexBattery; // Mutex to protect shared variables between tasks
Adafruit_MAX17048 max17048;
float batteryVoltage;
float batteryPercent;
int acPowerState; // Is set to 1 when 5V is present on the USB bus (AC power is on), and 0 when not (AC power is off)

// PSRAM historical data streams for the web page charts
#define DATA_ELEMENT_SIZE   10    // Size of one data value as numeric text, with comma delimiter
#define DATA_STREAM_COUNT   10    // There are ten data streams: sound, light, temperature, humidity, dew point, pressure, IAQ, gas resistance, gas accuracy, time
#define DATA_STREAM_SIZE    DATA_HISTORY_COUNT * DATA_ELEMENT_SIZE // Size of a single data stream in bytes
#define DATA_SET_SIZE       DATA_STREAM_SIZE * DATA_STREAM_COUNT   // Size of the entire data set in bytes
unsigned char* psramDataSet;

// Main loop
uint64_t timer = 0; // Copy of the main uptime timer that doesn't need a semaphore
uint64_t lastUpdateTimeMqtt = 0; // Time of last MQTT update
uint64_t lastUpdateTimeTft  = 0; // Time of last OLED update
uint64_t lastUpdateTimeData = 0; // Time of last data set update

// Helper function to return the current "clock", which is seconds of uptime
inline int64_t systemSeconds()
{
  return esp_timer_get_time() / 1000000;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Networking
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// WiFi setup
void setupWiFi()
{
  // Static IP configuration
  if (WIFI_STATIC_IP && !WiFi.config(WIFI_HOST, WIFI_GATEWAY, WIFI_SUBNET, WIFI_PRIMARY_DNS, WIFI_SECONDARY_DNS))
  {
    Serial.println("WiFi: Failed to configure static IP");
    //TODO: error handling
  }

  // Connect
  WiFi.setHostname(WIFI_HOSTNAME);
  Serial.print("WiFi: Connecting to "); Serial.print(WIFI_SSID); Serial.print(" ..");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print('.');
    delay(250);
  }
  Serial.println();
  Serial.print("WiFi: IP address "); Serial.println(WiFi.localIP());
}

// Multicast DNS setup so that "hostname.local" works on the network
void setupMDNS()
{
  if (!MDNS.begin(WIFI_HOSTNAME))
  {
    Serial.println("mDNS: Error setting up responder");
  }
  else
  {
    Serial.print("mDNS: Responder started for host name "); Serial.print(WIFI_HOSTNAME); Serial.println(".local");
    MDNS.addService("https", "tcp", 443); //Advertise services
    //MDNS.addServiceText("http", "tcp", "Service", "Web"); //Advertise service properties
    //MDNS.addServiceText("http", "tcp", "Device", "ESP32"); //Advertise service properties
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Web Server
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Web helper function for using sprintf() to append to the web string buffer
inline int bytesAdded(int sprintfReturnValue)
{
  // Ignore negative/error sprintf() return values so they don't affect pointer arithmetic in the calling function
  return sprintfReturnValue > 0 ? sprintfReturnValue : 0;
}

// Web helper function to append a string to the web string buffer
inline int buffercat(char* dest, const char* source)
{
  strcpy(dest, source);
  return strlen(source);
}

// Web helper function to describe IAQ accuracy values
char* webFormatIAQAccuracy(int a)
{
  switch (a)
  {
    case 1: return "Low";
    case 2: return "Medium";
    case 3: return "High";
    case 4: return "Very high";
  }
  return "Unreliable";
}

// Web helper function to output current/min/average/max values from a MeasurementTracker instance
char* webRenderMeasurementValues(char* buffer, char* description, char* format, MeasurementTracker& measurement)
{
  buffer += buffercat(buffer, description); // Start of table row
  buffer += bytesAdded(sprintf(buffer, format, measurement.current));
  buffer += bytesAdded(sprintf(buffer, format, measurement.min));
  buffer += bytesAdded(sprintf(buffer, format, measurement.average));
  buffer += bytesAdded(sprintf(buffer, format, measurement.max));
  buffer += buffercat(buffer, "</tr>"); // Close the table row 
  return buffer;
}

// Web helper function to render the main data table (the "dashboard") at the specified buffer position
char* webRenderDashboard(char* buffer)
{
  IPAddress ip = WiFi.localIP();
  struct tm timeInfo; // NTP

  buffer += buffercat(buffer, "<table id=\"dashboard\" class=\"sensor\" cellspacing=\"0\" cellpadding=\"3\">"); // Sensor data table
  buffer += bytesAdded(sprintf(buffer, "<tr><th colspan=\"5\" class=\"header\">%s</th></tr>", WIFI_HOSTNAME)); // Network hostname
  buffer += buffercat(buffer, "<tr class=\"subheader\"><th></th><td>Current</td><td>Min</td><td>Average</td><td>Max</td></tr>");

  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data (calculated on a different thread)
  if (environmentSensorOK)
  {
    #ifdef BME680_TEMP_F
      #define WEB_UNITS "F"
    #else
      #define WEB_UNITS "C"
    #endif
    buffer = webRenderMeasurementValues(buffer, "<tr class=\"environmental\"><th>Environment Temperature</th>",         "<td>%0.1f&deg; " WEB_UNITS "</td>", environmentTemperature);
    buffer = webRenderMeasurementValues(buffer, "<tr class=\"environmental\"><th>Environment Dew Point</th>",           "<td>%0.1f&deg; " WEB_UNITS "</td>", environmentDewPoint);
    buffer = webRenderMeasurementValues(buffer, "<tr class=\"environmental\"><th>Environment Humidity</th>",            "<td>%0.1f%%</td>",                  environmentHumidity);
    buffer = webRenderMeasurementValues(buffer, "<tr class=\"environmental\"><th>Environment Barometric Pressure</th>", "<td>%0.1f mbar</td>",               environmentPressure);
    if (environmentIAQAccuracy)
    {
      buffer = webRenderMeasurementValues(buffer, "<tr class=\"environmental\"><th>Environment IAQ</th>", "<td>%0.2f%%</td>", environmentIAQ);
    }  
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment IAQ Accuracy</th><td colspan=\"4\">%d (%s)</td></tr>",             environmentIAQAccuracy, webFormatIAQAccuracy(environmentIAQAccuracy)));
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Gas Resistance</th><td colspan=\"4\">%d ohms</td></tr>",           environmentGasResistance));
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Gas Calibration Accuracy</th><td colspan=\"4\">%0.1f%%</td></tr>", environmentGasAccuracy));
  }
  else
  {
    buffer += buffercat(buffer, "<tr class=\"environmental\"><th>Environment Sensor Stabilized?</th><td colspan=\"4\">0 (No)</td></tr>");
  }
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

  xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing sound data (measured on a different thread)
  buffer = webRenderMeasurementValues(buffer, "<tr class=\"soundlight\"><th>Sound Level</th>", "<td>%0.2f dB</td>", soundSensorSpl);
  xSemaphoreGive(xMutexSoundSensor); // Done with sound data

  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing light data (measured on a different thread)
  buffer = webRenderMeasurementValues(buffer, "<tr class=\"soundlight\"><th>Light Level</th>", "<td>%0.2f lux</td>", lightSensorLux);
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"soundlight\"><th>Light Measurement Gain</th><td colspan=\"4\">%0.3f</td></tr>", lightSensorGain));
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"soundlight\"><th>Light Measurement Integration Time</th><td colspan=\"4\">%d ms</td></tr>", lightSensorIntegrationTime));
  xSemaphoreGive(xMutexLightSensor); // Done with light data

  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data (calculated on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>Measurement Window for Min/Average/Max</th><td colspan=\"4\">%d seconds</td></tr>", MEASUREMENT_WINDOW));
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>Uptime</th><td colspan=\"2\">%lld seconds</td><td colspan=\"2\">%s</td></tr>", uptimeSecondsTotal, uptimeStringBuffer));
  if (getLocalTime(&timeInfo))
  {
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>System Time</th><td colspan=\"4\">%02d/%02d/%02d %02d:%02d:%02d</td></tr>", timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_year + 1900, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec));
  }
  xSemaphoreGive(xMutexUptime); // Done with uptime data

  buffer += bytesAdded(sprintf(buffer, "<tr class=\"network\"><th>MQTT Server</th><td colspan=\"4\">%s mqtts://%s:%d</td></tr>", mqttClient.connected() ? "Connected to" : "Disconnected from ", MQTT_SERVER, MQTT_PORT));
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"network\"><th>IP Address</th><td colspan=\"4\">%d.%d.%d.%d</td></tr>", ip[0], ip[1], ip[2], ip[3]));
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"network\"><th>WiFi Signal Strength (%s)</th><td colspan=\"4\">%d dBm</td></tr>", WIFI_SSID, WiFi.RSSI()));

  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Free Heap Memory</th><td colspan=\"4\">%d bytes</td></tr>", ESP.getFreeHeap())); // ESP32 free heap memory, which indicates if the program still has enough memory to run effectively
  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data (calculated on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Battery</th><td colspan=\"4\">%0.2fV / %0.0f%%</td></tr>", batteryVoltage, batteryPercent)); // LiPo battery
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>AC Power State</th><td colspan=\"4\">%s</td></tr>", acPowerState ? "1 (On)" : "0 (Off)")); // AC power sense
  xSemaphoreGive(xMutexBattery); // Done with battery data
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Chip Information</th><td colspan=\"4\">%s</td></tr>", chipInformation));

  buffer += buffercat(buffer, "</table>"); // Sensor data table

  return buffer;
}

// Web server 404 handler
void webHandler404()
{
  webServer.send(404, "text/plain", "Not found");
}

// Web server "/" GET handler (for root/home page)
void webHandlerRoot()
{
  // Build the HTML response in the web string buffer
  char* buffer = webStringBuffer; // "buffer" will be used to walk through the "webStringBuffer" work area using pointer arithmetic
  buffer += bytesAdded(sprintf(buffer, htmlHeader, WIFI_HOSTNAME, systemSeconds(), BME680_TEMP_F ? "F" : "C")); // Hostname gets added to the HTML <title> inside the template header, and the ESP32 current seconds counter and temperature units are used by JavaScript for the charts
  buffer  = webRenderDashboard(buffer);
  buffer += buffercat(buffer, htmlFooter); // HTML template footer

  // Send the HTML response to the client
  webServer.send(200, "text/html", webStringBuffer);
}

// Web server "/dashboard" GET handler (for AJAX updates on the main interface)
void webHandlerDashboard()
{
  webRenderDashboard(webStringBuffer);
  webServer.send(200, "text/plain", webStringBuffer);
}

// Helper functions to append one Prometheus style metric to the web buffer, such as the following where name="light_level_lux", description="Light level (current)", format="0.2f", metric=65.0F:
//    # HELP light_level_lux Light level (current)
//    # TYPE light_level_lux gauge
//    light_level_lux 65.00
char* webAppendMetric(char* buffer, char* name, char* description, char* format, float metric)
{
  buffer += buffercat(buffer, "# HELP "); buffer += buffercat(buffer, name); buffer += buffercat(buffer, " "); buffer += buffercat(buffer, description); buffer += buffercat(buffer, "\n");
  buffer += buffercat(buffer, "# TYPE "); buffer += buffercat(buffer, name); buffer += buffercat(buffer, " gauge\n");
  buffer += buffercat(buffer, name); buffer += buffercat(buffer, " "); buffer += bytesAdded(sprintf(buffer, format, metric)); buffer += buffercat(buffer, "\n\n");
  return buffer;
}

// Web server "/metrics" GET handler (for Prometheus and similar telemetry tools)
// Reference: https://github.com/prometheus/docs/blob/main/content/docs/instrumenting/exposition_formats.md
void webHandlerMetrics()
{
  // Build the HTML response in the web string buffer
  char* buffer = webStringBuffer; // "buffer" will be used to walk through the "webStringBuffer" work area using pointer arithmetic

  // Environmentals
  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data (calculated on a different thread)
  if (environmentSensorOK)
  {
    #ifdef BME680_TEMP_F
      #define SCRAPE_UNITS "(F)"
    #else
      #define SCRAPE_UNITS "(C)"
    #endif
    buffer = webAppendMetric(buffer, "environmental_temperature",           "Environment temperature " SCRAPE_UNITS " (current)", "%0.1f", environmentTemperature.current);
    buffer = webAppendMetric(buffer, "environmental_temperature_min",       "Environment temperature " SCRAPE_UNITS " (min)",     "%0.1f", environmentTemperature.min);
    buffer = webAppendMetric(buffer, "environmental_temperature_average",   "Environment temperature " SCRAPE_UNITS " (average)", "%0.1f", environmentTemperature.average);
    buffer = webAppendMetric(buffer, "environmental_temperature_max",       "Environment temperature " SCRAPE_UNITS " (max)",     "%0.1f", environmentTemperature.max);
    buffer = webAppendMetric(buffer, "environmental_dew_point",             "Environment calculated dew point " SCRAPE_UNITS " (current)", "%0.1f", environmentDewPoint.current);
    buffer = webAppendMetric(buffer, "environmental_dew_point_min",         "Environment calculated dew point " SCRAPE_UNITS " (min)",     "%0.1f", environmentDewPoint.min);
    buffer = webAppendMetric(buffer, "environmental_dew_point_average",     "Environment calculated dew point " SCRAPE_UNITS " (average)", "%0.1f", environmentDewPoint.average);
    buffer = webAppendMetric(buffer, "environmental_dew_point_max",         "Environment calculated dew point " SCRAPE_UNITS " (max)",     "%0.1f", environmentDewPoint.max);
    buffer = webAppendMetric(buffer, "environmental_humidity",              "Environment humidity (RH%) (current)", "%0.1f", environmentHumidity.current);
    buffer = webAppendMetric(buffer, "environmental_humidity_min",          "Environment humidity (RH%) (min)",     "%0.1f", environmentHumidity.min);
    buffer = webAppendMetric(buffer, "environmental_humidity_average",      "Environment humidity (RH%) (average)", "%0.1f", environmentHumidity.average);
    buffer = webAppendMetric(buffer, "environmental_humidity_max",          "Environment humidity (RH%) (max)",     "%0.1f", environmentHumidity.max);
    buffer = webAppendMetric(buffer, "environmental_pressure_mbar",         "Environment barometric pressure (current)", "%0.1f", environmentPressure.current);
    buffer = webAppendMetric(buffer, "environmental_pressure_mbar_min",     "Environment barometric pressure (min)",     "%0.1f", environmentPressure.min);
    buffer = webAppendMetric(buffer, "environmental_pressure_mbar_average", "Environment barometric pressure (average)", "%0.1f", environmentPressure.average);
    buffer = webAppendMetric(buffer, "environmental_pressure_mbar_max",     "Environment barometric pressure (max)",     "%0.1f", environmentPressure.max);

    // IAQ metrics
    if (environmentIAQAccuracy)
    {
      buffer = webAppendMetric(buffer, "environmental_iaq",         "Environment IAQ (0-100%, 0%=bad, 100%=good) (current)", "%0.2f", environmentIAQ.current);
      buffer = webAppendMetric(buffer, "environmental_iaq_min",     "Environment IAQ (0-100%, 0%=bad, 100%=good) (min)",     "%0.2f", environmentIAQ.min);
      buffer = webAppendMetric(buffer, "environmental_iaq_average", "Environment IAQ (0-100%, 0%=bad, 100%=good) (average)", "%0.2f", environmentIAQ.average);
      buffer = webAppendMetric(buffer, "environmental_iaq_max",     "Environment IAQ (0-100%, 0%=bad, 100%=good) (max)",     "%0.2f", environmentIAQ.max);
    }
    buffer = webAppendMetric(buffer, "environmental_iaq_accuracy",             "Environment IAQ accuracy (0=unreliable, 1=low, 2=medium, 3=high, 4=very high)", " %0.0f", (float)environmentIAQAccuracy);
    buffer = webAppendMetric(buffer, "environmental_gas_resistance_ohms",      "Environment gas resistance", " %0.0f", (float)environmentGasResistance);
    buffer = webAppendMetric(buffer, "environmental_gas_calibration_accuracy", "Environment gas calibration accuracy (0-100%, 0%=bad, 100%=good)", " %0.1f", environmentGasAccuracy);
  }
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

  // Sound level
  xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing sound data (measured on a different thread)
  buffer = webAppendMetric(buffer, "sound_level_db",         "Sound pressure level (current)", "%0.2f", soundSensorSpl.current);
  buffer = webAppendMetric(buffer, "sound_level_db_min",     "Sound pressure level (min)",     "%0.2f", soundSensorSpl.min);
  buffer = webAppendMetric(buffer, "sound_level_db_average", "Sound pressure level (average)", "%0.2f", soundSensorSpl.average);
  buffer = webAppendMetric(buffer, "sound_level_db_max",     "Sound pressure level (max)",     "%0.2f", soundSensorSpl.max);
  xSemaphoreGive(xMutexSoundSensor); // Done with sound data

  // Light level
  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing light data (measured on a different thread)
  buffer = webAppendMetric(buffer, "light_level_lux",         "Light level (current)", "%0.2f", lightSensorLux.current);
  buffer = webAppendMetric(buffer, "light_level_lux_min",     "Light level (min)",     "%0.2f", lightSensorLux.min);
  buffer = webAppendMetric(buffer, "light_level_lux_average", "Light level (average)", "%0.2f", lightSensorLux.average);
  buffer = webAppendMetric(buffer, "light_level_lux_max",     "Light level (max)",     "%0.2f", lightSensorLux.max);
  buffer = webAppendMetric(buffer, "light_level_measurement_gain", "Light measurement gain", " %0.3f", lightSensorGain);
  buffer = webAppendMetric(buffer, "light_level_measurement_integration_time_ms", "Light measurement integration time", " %0.0f", (float)lightSensorIntegrationTime);
  xSemaphoreGive(xMutexLightSensor); // Done with light data

  // Measurement window
  buffer = webAppendMetric(buffer, "measurement_window_seconds", "Measurement Window for min/average/max calculations", " %0.0f", (float)MEASUREMENT_WINDOW);

  // WiFi signal strength
  buffer += buffercat(buffer, "# HELP esp32_wifi_signal_strength ESP32 WiFi signal strength\n");
  buffer += buffercat(buffer, "# TYPE esp32_wifi_signal_strength gauge\n");
  buffer += bytesAdded(sprintf(buffer, "esp32_wifi_signal_strength{SSID=\"%s\"} %d\n\n", WIFI_SSID, WiFi.RSSI()));

  // Free heap memory
  buffer = webAppendMetric(buffer, "esp32_free_heap_bytes", "ESP32 free heap memory", " %0.0f", (float)ESP.getFreeHeap());

  // Battery data and AC power on/off state
  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data
  buffer = webAppendMetric(buffer, "esp32_battery_voltage", "ESP32 LiPo battery voltage", " %0.2f", batteryVoltage);
  buffer = webAppendMetric(buffer, "esp32_battery_percent", "ESP32 LiPo battery percent", " %0.2f", batteryPercent);
  buffer = webAppendMetric(buffer, "esp32_ac_power_state", "ESP32 AC power state", " %0.0f", (float)acPowerState);
  xSemaphoreGive(xMutexBattery); // Done with battery data

  // Chip information
  buffer += buffercat(buffer, "# HELP esp32_chip_information ESP32 chip information\n");
  buffer += buffercat(buffer, "# TYPE esp32_chip_information gauge\n");
  buffer += bytesAdded(sprintf(buffer, "esp32_chip_information{version=\"%s\"} 1\n\n", chipInformation));

  // Last line must end with a line feed character
  buffer += buffercat(buffer, "\n");

  // Send the HTML response to the client
  webServer.send(200, "text/plain", webStringBuffer);
}

// Web handler for chart data. Sends all data streams as a single text/plain data set.
void webHandlerData()
{
  if (psramDataSet)
  {
    webServer.send(200, "text/plain", (const char*) psramDataSet);
  }
}

// Web server setup 
void setupWebserver()
{
  Serial.println("Web: Starting server");

  // Set features and URI handlers
  webServer.enableCORS();
  webServer.on("/", webHandlerRoot);
  webServer.on("/dashboard", webHandlerDashboard);
  webServer.on("/data", webHandlerData);
  webServer.on("/metrics", webHandlerMetrics);
  webServer.onNotFound(webHandler404);

  // Start the server
  webServer.begin();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MQTT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Setup MQTT connection
void setupMQTT()
{
  espClient.setCACert(CERT_CA);
  //espClient.setCertificate(const char *CERT_CLIENT); // Client certificate
  //espClient.setPrivateKey(const char *CERT_CLIENT_KEY); // Client certificate key
  //espClient.setInsecure(); //TODO: Add switch to allow for testing
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
}

// Connect/Reconnect to the MQTT server
void connectMQTT()
{
  // Wait a few seconds between connection attempts
  if (mqttLastConnectionAttempt == 0 || millis() - mqttLastConnectionAttempt >= 15000)
  {
    mqttLastConnectionAttempt = millis();

    // Attempt the connection
    //Serial.println("MQTT: Initializing connection");
    if (mqttClient.connect(WIFI_HOSTNAME, MQTT_USER, MQTT_PASSWORD))
    {
      Serial.println("MQTT: Connected");
    }
    else
    {
      Serial.print("MQTT: Connection failed: ");
      Serial.println(mqttClient.state()); // -1=disconnected, -2=connect failed, -3=connection lost, -4=connection timeout
    }
  }
}

// Send all data to MQTT
void updateMQTT()
{
  char mqttStringBuffer[25];
  struct tm timeInfo; // NTP

  // Helper macro to format and publish a single value to MQTT
  #define MQTT_PUBLISH(topic, format, value) sprintf(mqttStringBuffer, format, value); mqttClient.publish(topic, mqttStringBuffer, true);

  // Environmentals
  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
  if (environmentSensorOK)
  {
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_temperature",           "%0.1f", environmentTemperature.current);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_temperature_min",       "%0.1f", environmentTemperature.min);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_temperature_average",   "%0.1f", environmentTemperature.average);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_temperature_max",       "%0.1f", environmentTemperature.max);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_dew_point",             "%0.1f", environmentDewPoint.current);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_dew_point_min",         "%0.1f", environmentDewPoint.min);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_dew_point_average",     "%0.1f", environmentDewPoint.average);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_dew_point_max",         "%0.1f", environmentDewPoint.max);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_humidity",              "%0.1f", environmentHumidity.current);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_humidity_min",          "%0.1f", environmentHumidity.min);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_humidity_average",      "%0.1f", environmentHumidity.average);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_humidity_max",          "%0.1f", environmentHumidity.max);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_pressure_mbar",         "%0.1f", environmentPressure.current);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_pressure_mbar_min",     "%0.1f", environmentPressure.min);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_pressure_mbar_average", "%0.1f", environmentPressure.average);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_pressure_mbar_max",     "%0.1f", environmentPressure.max);

    // IAQ metrics
    if (environmentIAQAccuracy)
    {
      MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_iaq",              "%0.2f", environmentIAQ.current);
      MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_iaq_min",          "%0.2f", environmentIAQ.min);
      MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_iaq_average",      "%0.2f", environmentIAQ.average);
      MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_iaq_max",          "%0.2f", environmentIAQ.max);
    }
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_iaq_accuracy", "%d", environmentIAQAccuracy);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_gas_resistance_ohms", "%d", environmentGasResistance);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_gas_calibration_accuracy", "%0.1f", environmentGasAccuracy);
  }
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

  // Sound level
  xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing the sound sensor data (calculated on a different thread)
  MQTT_PUBLISH(MQTT_TOPIC_BASE "sound_level_db",              "%0.2f", soundSensorSpl.current);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "sound_level_db_min",          "%0.2f", soundSensorSpl.min);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "sound_level_db_average",      "%0.2f", soundSensorSpl.average);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "sound_level_db_max",          "%0.2f", soundSensorSpl.max);
  xSemaphoreGive(xMutexSoundSensor); // Done with sound sensor data

  // Light level
  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing the light sensor data
  MQTT_PUBLISH(MQTT_TOPIC_BASE "light_level_lux",              "%0.2f", lightSensorLux.current);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "light_level_lux_min",          "%0.2f", lightSensorLux.min);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "light_level_lux_average",      "%0.2f", lightSensorLux.average);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "light_level_lux_max",          "%0.2f", lightSensorLux.max);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "light_level_measurement_gain", "%0.3f", lightSensorGain);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "light_level_measurement_integration_time_ms", "%d", lightSensorIntegrationTime);
  xSemaphoreGive(xMutexLightSensor); // Done with light sensor data

  // Measurement window
  MQTT_PUBLISH(MQTT_TOPIC_BASE "measurement_window_seconds", "%d", MEASUREMENT_WINDOW);

  // Uptime information
  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data
  MQTT_PUBLISH(MQTT_TOPIC_BASE "uptime_seconds", "%lld", timer);
  mqttClient.publish(MQTT_TOPIC_BASE "uptime", uptimeStringBuffer, true);
  xSemaphoreGive(xMutexUptime); // Done with uptime data

  // NTP system time
  if (getLocalTime(&timeInfo))
  {
    sprintf(mqttStringBuffer, "%02d/%02d/%d %02d:%02d:%02d", timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_year + 1900, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
    mqttClient.publish(MQTT_TOPIC_BASE "esp32_system_time", mqttStringBuffer, true);
  }

  // WiFi signal strength
  MQTT_PUBLISH(MQTT_TOPIC_BASE "esp32_wifi_signal_strength_dbm", "%d", WiFi.RSSI());

  // Free heap memory
  MQTT_PUBLISH(MQTT_TOPIC_BASE "esp32_free_heap_bytes", "%d", ESP.getFreeHeap());

  // Battery data and AC power on/off state
  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data
  MQTT_PUBLISH(MQTT_TOPIC_BASE "esp32_battery_voltage", "%0.2f", batteryVoltage);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "esp32_battery_percent", "%0.2f", batteryPercent);
  mqttClient.publish(MQTT_TOPIC_BASE "esp32_ac_power_state", acPowerState ? "1" : "0", true); // 1 for "ON" or 0 for "OFF"
  xSemaphoreGive(xMutexBattery); // Done with battery data

  // Chip information
  mqttClient.publish(MQTT_TOPIC_BASE "esp32_chip_information", chipInformation, true);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// PSRAM Data Storage
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Allocate PSRAM for long-term data storage
void setupPsram()
{
  if (psramFound())
  {
    if (psramInit())
    {
      psramDataSet = (unsigned char*)ps_malloc(DATA_SET_SIZE + 1); // +1 to make room for the NULL terminator
      if (psramDataSet)
      {
        memset(psramDataSet, ' ', DATA_SET_SIZE); // Fill with spaces
        psramDataSet[DATA_SET_SIZE] = 0; // NULL terminator
        Serial.print("PSRAM: Allocated "); Serial.print(DATA_SET_SIZE + 1); Serial.println(" bytes");
      }
      else
      {
        Serial.print("PSRAM: ps_malloc() failed allocating "); Serial.print(DATA_SET_SIZE + 1); Serial.println(" bytes from "); Serial.println(ESP.getFreePsram());
      }
    }
    else
    {
      Serial.println("PSRAM: Initialization failed");
    }
  }
  else
  {
    Serial.println("PSRAM: Not found");
  }
}

// Overloaded helper function to format a data value and put it at the end of the specified data stream (overwriting the end value/slot)
void addDataElement(int stream, float value)
{
  char dataFormatBuffer[DATA_ELEMENT_SIZE + 1]; // +1 for NULL terminator
  memset(dataFormatBuffer, ' ', DATA_ELEMENT_SIZE);   // Fill with spaces
  int n = sprintf(dataFormatBuffer, "%0.2f,", value); // Format the value with delimiter and NULL terminator
  if (n > 0) dataFormatBuffer[n] = ' ';               // Remove sprintf() NULL terminator
  int dataSetIndex = stream * DATA_STREAM_SIZE + DATA_STREAM_SIZE - DATA_ELEMENT_SIZE; // Byte index of the last element in the specified data stream
  memcpy(psramDataSet + dataSetIndex, dataFormatBuffer, DATA_ELEMENT_SIZE); // Copy the new value (with delimiter) into the data set
}
void addDataElement(int stream, uint64_t value)
{
  char dataFormatBuffer[DATA_ELEMENT_SIZE + 1]; // +1 for NULL terminator
  memset(dataFormatBuffer, ' ', DATA_ELEMENT_SIZE);   // Fill with spaces
  int n = sprintf(dataFormatBuffer, "%lld,", value);  // Format the value with delimiter and NULL terminator
  if (n > 0) dataFormatBuffer[n] = ' ';               // Remove sprintf() NULL terminator
  int dataSetIndex = stream * DATA_STREAM_SIZE + DATA_STREAM_SIZE - DATA_ELEMENT_SIZE; // Byte index of the last element in the specified data stream
  memcpy(psramDataSet + dataSetIndex, dataFormatBuffer, DATA_ELEMENT_SIZE); // Copy the new value (with delimiter) into the data set
}
void addNullDataElement(int stream)
{
  char dataFormatBuffer[DATA_ELEMENT_SIZE + 1]; // +1 for NULL terminator
  memset(dataFormatBuffer, ' ', DATA_ELEMENT_SIZE);   // Fill with spaces
  int n = sprintf(dataFormatBuffer, "%s,", "null");   // Format the JavaScript NULL value with delimiter and NULL terminator
  if (n > 0) dataFormatBuffer[n] = ' ';               // Remove sprintf() NULL terminator
  int dataSetIndex = stream * DATA_STREAM_SIZE + DATA_STREAM_SIZE - DATA_ELEMENT_SIZE; // Byte index of the last element in the specified data stream
  memcpy(psramDataSet + dataSetIndex, dataFormatBuffer, DATA_ELEMENT_SIZE); // Copy the new value (with delimiter) into the data set
}

// Add current sensor values to the end of each data stream
void updateDataSet()
{
  if (psramDataSet)
  {
    // Shift all data streams down by one element. The updates below will overwrite the last element in each stream.
    memmove(psramDataSet, psramDataSet + DATA_ELEMENT_SIZE, DATA_SET_SIZE - DATA_ELEMENT_SIZE);

    xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing the sound sensor data
    addDataElement(0, soundSensorSpl.current);
    xSemaphoreGive(xMutexSoundSensor); // Done with sound sensor data

    xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing the light sensor data
    addDataElement(1, lightSensorLux.current);
    xSemaphoreGive(xMutexLightSensor); // Done with light sensor data

    xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
    addDataElement(2, environmentTemperature.current);
    addDataElement(3, environmentHumidity.current);
    addDataElement(4, environmentDewPoint.current);
    addDataElement(5, environmentPressure.current);
    if (environmentIAQAccuracy > 0 && !(environmentGasCalibrationStage <= 1 && environmentIAQ.current == 50.0F))
    {
      addDataElement(6, environmentIAQ.current);
      addDataElement(7, (float)environmentGasResistance / 1000.0F); // Convert to kiloohms to fit within the PSRAM data slot with fewer digits
      addDataElement(8, environmentGasAccuracy);
    }
    else
    {
      // Use JavaScript NULL values if the IAQ data is not ready yet (due to initialization)
      addNullDataElement(6);
      addNullDataElement(7);
      addNullDataElement(8);
    }
    xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

    addDataElement(9, timer); // Time index
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// ST7789 TFT Display
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Setup the TFT ST7789 display
void setupTFT()
{
  // Initialize the display
  Serial.println("TFT: Configuring ST7789");
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);
  display.init(TFT_SCREEN_HEIGHT, TFT_SCREEN_WIDTH);
  display.setRotation(3);

  // Clear the display
  display.fillScreen(ST77XX_BLACK);

  // Establish defaults
  canvas.setRotation(TFT_ROTATION);
  canvas.setFont(&FreeSans9pt7b);
  canvas.setTextColor(ST77XX_WHITE); // White text
}

// Helper function to format the wide range of possible lux values into a consistent 4-digit representation with variable decimal places
void formatLux(char* buffer, float lux)
{
  if (lux < 10)
  {
    sprintf(buffer, "%0.3f", lux);
  }
  else if (lux < 100)
  {
    sprintf(buffer, "%.2f", lux);
  }
  else if (lux < 1000)
  {
    sprintf(buffer, "%.1f", lux);
  }
  else if (lux < 10000)
  {
    sprintf(buffer, "%.0f", lux);
  }
  else if (lux < 100000)
  {
    sprintf(buffer, "%0.2fK", lux/1000); // Such as "28.00K"
  }
  else
  {
    sprintf(buffer, "%0.1fK", lux/1000); // Such as "102.0K"
  }
}

// Update the TFT display
void updateDisplay()
{
  char formatBuffer[40];
  int64_t seconds = systemSeconds(); // Current system clock in seconds, used to flip/flop display data

  canvas.fillScreen(ST77XX_BLACK);
  canvas.setFont(&FreeSans9pt7b);
  canvas.setCursor(0, 20);

  xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing sound data (measured on a different thread)
  canvas.setTextColor(ST77XX_WHITE);
  canvas.printf("%0.2f    %0.2f    %0.2f dB", soundSensorSpl.current, soundSensorSpl.average, soundSensorSpl.max);
  canvas.println();
  xSemaphoreGive(xMutexSoundSensor); // Done with sound data

  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing light data (measured on a different thread)
  canvas.setTextColor(ST77XX_YELLOW);
  formatLux(formatBuffer, lightSensorLux.current); canvas.print(formatBuffer); canvas.print("  ");
  formatLux(formatBuffer, lightSensorLux.average); canvas.print(formatBuffer); canvas.print("  ");
  formatLux(formatBuffer, lightSensorLux.max);     canvas.print(formatBuffer); canvas.print(" lux");
  canvas.println();
  xSemaphoreGive(xMutexLightSensor); // Done with light data

  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data (calculated on a different thread)
  canvas.setTextColor(ST77XX_GREEN);
  if (seconds % 2)
  {
    #if defined(BME680_TEMP_F)
      canvas.printf("Dew: %0.1fF   IAQ %0.1f%%", environmentDewPoint.current, environmentIAQ.current);
    #else
      canvas.printf("Dew: %0.1fC   IAQ %0.1f%%", environmentDewPoint.current, environmentIAQ.current);
    #endif
  }
  else
  {
    #if defined(BME680_TEMP_F)
      canvas.printf("%0.1fF   %0.1f%%   %0.0f mbar", environmentTemperature.current, environmentHumidity.current, environmentPressure.current);
    #else
      canvas.printf("%0.1fC   %0.1f%%   %0.0f mbar", environmentTemperature.current, environmentHumidity.current, environmentPressure.current);
    #endif
  }
  canvas.println();
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

  canvas.setTextColor(ST77XX_MAGENTA);
  if (seconds % 2)
  {
    xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data (calculated on a different thread)
    canvas.printf("Battery: %0.2fV / %0.0f%%", batteryVoltage, batteryPercent);
    xSemaphoreGive(xMutexBattery); // Done with battery data
  }
  else
  {
    xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data (calculated on a different thread)
    canvas.printf("Uptime: %s", uptimeStringBuffer);
    xSemaphoreGive(xMutexUptime); // Done with uptime data
  }
  canvas.println();

  canvas.setTextColor(ST77XX_CYAN);
  if (seconds % 2)
  {
    canvas.printf("WiFi Signal: %d dBm", WiFi.RSSI());
  }
  else
  {
    IPAddress ip = WiFi.localIP();
    canvas.printf("IP Address: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  }
  //canvas.println();

  display.drawRGBBitmap(0, 0, canvas.getBuffer(), TFT_SCREEN_WIDTH, TFT_SCREEN_HEIGHT);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// BME680 Environmental Sensor
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Setup the BME680 sensor
void setupEnvironmentalSensor()
{
  Serial.println("Environmentals: Initializing BME680");
  
  // Initialize
  if (!bme680.begin(BME680_ADDRESS))
  {
    Serial.println("Environmentals: BME680 configuration failed");
    environmentSensorOK = false;
  }
  else
  {
    environmentSensorOK = true;
  }

  // Temperature compensation, also used for humidity compensation
  bme680.setTemperatureCompensation(BME680_TEMP_OFFSET);

  // Enable Donchian smoothing to remove oscillations in IAQ due to the cycling of air conditioners, heaters, etc.
  if (BME680_DONCHIAN_ENABLE)
  {
    bme680.setDonchianSmoothing(true, BME680_DONCHIAN_WINDOW);
  }
}

// Environmental data measurement
void measureEnvironmentals()
{
  // Read environmental data
  float t = bme680.temperature_compensated; // Celsius
  float h = bme680.humidity_compensated;
  float p = bme680.pressure / 100.0F; // Convert to mbar

  // Is there any trouble with the sensor data?
  if (!(t < -100 || t > 200 || h < 0 || h > 100 || p < 700 || p > 1500))
  {
    if (isnanf(t) || isnanf(h) || isnanf(p))
    {
      // Reset the BME680
      Serial.println("Environmentals: NaN detected - Resetting BME680");
      xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
      setupEnvironmentalSensor();
      xSemaphoreGive(xMutexEnvironmental); // Done with environmental data
    }
    else
    {
      // Update the environmental values
      xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
      environmentTemperature.track(BME680_TEMP_F ? t * 9.0F / 5.0F + 32.0F : t);
      environmentHumidity.track(h);
      environmentPressure.track(p);
      environmentDewPoint.track(BME680_TEMP_F ? bme680.dew_point * 9.0F / 5.0F + 32.0F : bme680.dew_point);
      environmentIAQ.track(bme680.IAQ);
      environmentIAQAccuracy = bme680.IAQ_accuracy;
      environmentGasResistance = bme680.gas_resistance;
      environmentGasAccuracy = bme680.getGasCalibrationAccuracy();
      environmentGasCalibrationStage = bme680.getGasCalibrationStage();

      // Sensor is operational
      environmentSensorOK = true;
      xSemaphoreGive(xMutexEnvironmental); // Done with environmental data
    }
  }
  else
  {
    // Reset the BME680
    Serial.printf("Environmentals: BME680 data error: t=%0.1f h=%0.1f p=%0.1f", t, h, p); Serial.println();
    xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
    setupEnvironmentalSensor();
    xSemaphoreGive(xMutexEnvironmental); // Done with environmental data
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VEML7700 Light Sensor
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Setup the VEML7700 light sensor
void setupLightSensor()
{
  Serial.println("Light: Configuring VEML7700");

  // Initialize the VEML7700 light sensor
  if (!lightSensor.begin())
  {
    Serial.println("Light: Configuration failed");
    //while (1); // Halt execution
  }
  //lightSensor.setIntegrationTime(VEML7700_IT_100MS); // Set integration time to 100ms
  //lightSensor.setGain(VEML7700_GAIN_1X); // Set gain to 1x
  //lightSensor.setPower(VEML7700_POWER_ON); // Power on the sensor
}

// Light level measurement using the VEML7700 sensor
// Reference: https://github.com/adafruit/Adafruit_VEML7700/blob/master/examples/veml7700_autolux/veml7700_autolux.ino
void measureLight()
{
  // Read lux using the automatic method which adjusts gain and integration time as needed to obtain a good reading. A non-linear correction is also applied if needed.
  float lux = lightSensor.readLux(VEML_LUX_AUTO);
  uint8_t sensorGain = lightSensor.getGain();
  uint8_t sensorIntegrationTime = lightSensor.getIntegrationTime();

  // Translate the gain and integration time values
  float gain; 
  switch (sensorGain)
  {
    case VEML7700_GAIN_2:   gain = 2;     break;
    case VEML7700_GAIN_1:   gain = 1;     break;
    case VEML7700_GAIN_1_4: gain = 0.25;  break;
    case VEML7700_GAIN_1_8: gain = 0.125; break;
  }
  int integrationTime;
  switch (sensorIntegrationTime)
  {
    case VEML7700_IT_25MS:  integrationTime = 25;  break;
    case VEML7700_IT_50MS:  integrationTime = 50;  break;
    case VEML7700_IT_100MS: integrationTime = 100; break;
    case VEML7700_IT_200MS: integrationTime = 200; break;
    case VEML7700_IT_400MS: integrationTime = 400; break;
    case VEML7700_IT_800MS: integrationTime = 800; break;
  }

  // Update light sensor data values
  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing the light sensor data
  lightSensorLux.track(lux);
  lightSensorGain = gain;
  lightSensorIntegrationTime = integrationTime;
  xSemaphoreGive(xMutexLightSensor); // Done with light sensor data

  // Detect sensor failure
  /*
  if (lightSensorLux.min == lightSensorLux.max && the tracking data points are full) // When the sensor gets "stuck", the lux value goes constant, so the peak buffer should be full of the exact same values
  {
    Serial.println("Light: Sensor stalled");
    delay(250);
    ESP.restart(); // Soft system reset
  }
  */
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MAX17048 LiPo Battery Monitor
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Setup the MAX17048 LiPo battery monitor
void setupBatteryMonitor()
{
  Serial.println("Battery: Configuring MAX17048");

  // Initialize the MAX17048 battery monitor
  if (!max17048.begin())
  {
    Serial.println("Battery: Configuration failed");
    //while (1); // Halt execution
  }
}

// Read the battery voltage using the MAX17048 LiPo battery monitor, and ALSO read the AC power on/off state from a digital input pin
void measureBattery()
{
  // Read battery voltage and percent charge
  float v = max17048.cellVoltage();
  float p = max17048.cellPercent();

  // Update the battery and AC power data values
  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data
  if (batteryVoltage == 0)
  {
    // Initialize values
    batteryVoltage = v;
    batteryPercent = p;
  }
  else
  {
    // EMA moving average to smooth the readings
    batteryVoltage = (batteryVoltage * (MEASUREMENT_WINDOW - 1) + v) / MEASUREMENT_WINDOW;
    batteryPercent = (batteryPercent * (MEASUREMENT_WINDOW - 1) + p) / MEASUREMENT_WINDOW;
  }
  acPowerState = digitalRead(AC_POWER_PIN); // Read the AC power on/off state from a digital input pin
  xSemaphoreGive(xMutexBattery); // Done with battery data
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Read I2C devices in sequence (background task)
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Each device is read in sequence so that a single task can be used to read all I2C data without a semaphore, and so the timing can be well controlled
void readI2CDevices(void *parameter)
{
  unsigned long timer;

  // Infinite loop since this is a separate task from the main thread
  unsigned long interval;
  while (true)
  {
    if (millis() - timer >= I2C_INTERVAL)
    {
      timer = millis(); // Reset the timer
      interval = millis();
      if (bme680.performReading()) // Takes about 370ms
      {
        measureEnvironmentals(); // Read the BME680 environmentals
      }
      interval = millis() - interval;
      Serial.print("BME680 interval = "); Serial.println(interval);
      interval = millis();
      measureLight();   // Measure the light level, takes up to 5118ms whne dark due to auto gain
      interval = millis() - interval;
      Serial.print("VEML7700 interval = "); Serial.println(interval);
      interval = millis();
      measureBattery(); // Read the LiPo battery voltage, takes about 5ms
      interval = millis() - interval;
      Serial.print("Battery interval = "); Serial.println(interval);
    }
    delay(25); // Non-blocking delay on ESP32, in milliseconds
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SPH0645 I2S Sound Sensor
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Setup the SPH0645 I2S sound sensor
void setupSoundSensor()
{
  Serial.println("Sound: Configuring SPH0645");

  // Configure I2S
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), // Master mode, Receive data
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE,
    .channel_format = I2S_NUM_CHANNELS,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S, // Standard I2S protocol
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,          // Interrupt priority
    .dma_buf_count = I2S_DMA_BUF_COUNT,
    .dma_buf_len = I2S_DMA_BUF_LEN,
    .use_apll = true, // Use internal APLL clock source (false usually works for lower sample rates)
    .tx_desc_auto_clear = false, // Not used in RX mode
    .fixed_mclk = 0             // MCLK not used
  };

  // Configure I2S Pins
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_LRCK_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE, // Not transmitting data
    .data_in_num = I2S_DATA_IN_PIN
  };

  // Install and start the I2S driver
  esp_err_t err = i2s_driver_install(I2S_PORT_NUM, &i2s_config, 0, NULL);
  if (err != ESP_OK)
  {
    Serial.printf("Sound: Failed to install I2S driver: %d\n", err);
    //while (1); // Halt execution
  }

  // Set the I2S pins
  err = i2s_set_pin(I2S_PORT_NUM, &pin_config);
  if (err != ESP_OK)
  {
    Serial.printf("Sound: Failed to set I2S pins: %d\n", err);
    //while (1); // Halt execution
  }

  // Clear the DMA buffer
  i2s_zero_dma_buffer(I2S_PORT_NUM);
}

// Sound level measurement using I2S (background task)
void measureSound(void *parameter)
{
  // Infinite loop since this is a separate task from the main thread
  while (true)
  {
    // Read the I2S data into the buffer
    size_t bytesRead = 0;
    esp_err_t result = i2s_read(I2S_PORT_NUM,                      // I2S port number
                                soundSensorDataBuffer,             // Buffer to store data
                                I2S_DMA_BUF_LEN * sizeof(int32_t), // Max bytes to read (buffer size)
                                &bytesRead,                        // Number of bytes actually read
                                portMAX_DELAY);                    // Wait indefinitely for data
    if (result == ESP_OK)
    {
      if (bytesRead > 0)
      {
        // Process the samples
        int32_t minValue = 0, maxValue = 0; // Initialize min/max values
        int samplesRead = bytesRead / sizeof(int32_t); // Calculate the number of 32-bit samples read
        for (int i = 0; i < samplesRead; i++)
        {
          // SPH0645 data is in the upper 18 bits of the 32-bit sample
          // NOTE: The 32-bit sample is unsigned, which MUST be cast to unsigned before shifting bits to avoid the sign bit potentially carrying over to the upper bits
          int32_t s = ((unsigned int)soundSensorDataBuffer[i]) >> (32 - 18);

          // Calculate the rage of the samples
          if (s < minValue || minValue == 0)
          {
            if (s != 0) minValue = s; // Ignore zero values for the min value after initialization
          }
          if (s > maxValue) maxValue = s;
        }

        // Rough calculation of the sound pressure level (SPL) in dB based on the measured samples. It's more like the dB range rather than an absolute dB level, but the scaling factor helps compensate for that.
        float spl = SPL_FACTOR * log10(maxValue - minValue);

        // Update the sound level values
        xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing the sound sensor data
        soundSensorSpl.track(spl);
        xSemaphoreGive(xMutexSoundSensor); // Done with sound sensor data
      }
    }

    //
    delay(10); // Non-blocking delay on ESP32, in milliseconds
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Main Setup
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void setup()
{
  // Serial port for debugging purposes
  Serial.begin(115200);
  while (!Serial) delay(10); // Wait for the console to initialize

  // Allocate a large PSRAM buffer for long-term data storage, before any other setup that might try and use PSRAM
  setupPsram();

  // Core features
  setupWiFi();
  setupMDNS();
  setupWebserver();
  setupMQTT();
  configTzTime(NTP_TIMEZONE, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3); // NTP

  // Semaphore setup
  xMutexEnvironmental = xSemaphoreCreateMutex();
  xMutexUptime        = xSemaphoreCreateMutex();
  xMutexBattery       = xSemaphoreCreateMutex();
  xMutexSoundSensor   = xSemaphoreCreateMutex();
  xMutexLightSensor   = xSemaphoreCreateMutex();

  // Sensor setup
  delay(200); // Allow the sensor modules time to initialize after powering on
  setupTFT();
  setupEnvironmentalSensor();
  setupBatteryMonitor();
  setupSoundSensor();
  setupLightSensor();

  // Button setup
  // Reference: https://learn.adafruit.com/esp32-s3-reverse-tft-feather/factory-shipped-demo-2
  pinMode(0, INPUT_PULLUP); // D0 is different
  pinMode(1, INPUT_PULLDOWN);
  pinMode(2, INPUT_PULLDOWN);

  // AC power on/off sense pin setup
  pinMode(AC_POWER_PIN, INPUT_PULLDOWN);

  // Chip information
  sprintf(chipInformation, "%s %s (revison v%d.%d), %dMHz, %dMB Flash, %dMB PSRAM", ESP.getChipModel(), ESP.getCoreVersion(), efuse_hal_get_major_chip_version(), efuse_hal_get_minor_chip_version(), ESP.getCpuFreqMHz(), ESP.getFlashChipSize()/1024/1024, ESP.getPsramSize()/1024/1024);

  // Create a task for reading all of the I2C devices
  xTaskCreate(
    readI2CDevices,   // Function to be called
    "readI2CDevices", // Name of the task
    12000,             // Stack size
    NULL,             // Parameter to pass
    1,                // Task priority
    NULL              // Task handle
  );

  // Create a task for reading sound levels
  xTaskCreate(
    measureSound,   // Function to be called
    "measureSound", // Name of the task
    6000,           // Stack size
    NULL,           // Parameter to pass
    1,              // Task priority
    NULL            // Task handle
  );
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Main Loop
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void loop()
{
  // Uptime calculations: How long has the ESP32 been running since it was booted up?
  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data
  bool newSecond = false;
  uptimeSecondsTotal = systemSeconds();
  if (uptimeSecondsTotal > lastUptimeSecondsTotal)
  {
    newSecond = true;
    int64_t seconds = uptimeSecondsTotal;
    uptimeDays = seconds / 86400;
    seconds -= uptimeDays * 86400;
    uptimeHours = seconds / 3600;
    seconds -= uptimeHours * 3600;
    uptimeMinutes = seconds / 60;
    uptimeSeconds = seconds - uptimeMinutes * 60;
    sprintf(uptimeStringBuffer, "%lldd %lldh %lldm %llds", uptimeDays, uptimeHours, uptimeMinutes, uptimeSeconds);
    lastUptimeSecondsTotal = uptimeSecondsTotal;
  }
  timer = uptimeSecondsTotal; // Copy the timer so it can be used without the semaphore
  xSemaphoreGive(xMutexUptime); // Done with uptime data

  // MQTT connection management
  if (!mqttClient.connected()) connectMQTT(); else mqttClient.loop();

  // Web server request management
  webServer.handleClient();

  // Update outputs every specified update interval, and usually the first-time through the loop()
  bool updateTft  = timer - lastUpdateTimeTft >= UPDATE_INTERVAL_TFT;
  if (updateTft || lastUpdateTimeTft == 0)
  {
      // Update the display
      updateDisplay();
      lastUpdateTimeTft = timer;
  }
  bool updateMqtt = timer - lastUpdateTimeMqtt >= UPDATE_INTERVAL_MQTT;
  if (updateMqtt || lastUpdateTimeMqtt == 0)
  {
      // Update MQTT
      updateMQTT();
      lastUpdateTimeMqtt = timer;
  }
  bool updateData = timer - lastUpdateTimeData >= UPDATE_INTERVAL_DATA;
  if (updateData) // Don't want to capture data the first time through the loop() because there likely won't be any useful data
  {
      // Update the data set
      updateDataSet();
      lastUpdateTimeData = timer;
  }

  // Turn on the TFT display backlight for a few seconds when a button is pressed. The display is constantly updated, so buttons just turn the backlight on/off.
  if (digitalRead(0) == 0 || digitalRead(1) || digitalRead(1)) // NOTE: D0 is different
  {
    displayTimer = timer;
    digitalWrite(TFT_BACKLITE, HIGH); // Power on the TFT backlight
  }
  else if (timer - displayTimer == TFT_TIMEOUT)
  {
    digitalWrite(TFT_BACKLITE, LOW); // Power off the TFT backlight
  }

  // Yield to other tasks, and slow down the main loop a little
  delay(5); // Non-blocking delay on ESP32, in milliseconds
}