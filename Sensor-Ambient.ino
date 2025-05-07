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
#include <Adafruit_Sensor.h>      // BME280 support
#include <Adafruit_BME280.h>      // BME280 support
#include <hal/efuse_hal.h>        // Espressif ESP32 chip information
#include <time.h>                 // NTP and time support

// App configuration
#include <html.h>                 // HTML templates
#include <config.h>               // The configuration references objects in the above libraries, so include it after those

// ESP32
char chipInformation[100]; // Chip information buffer

// Web server
WebServer webServer(80);
char webStringBuffer[12 * 1024];
struct tm webTimeInfo; // NTP

// MQTT
//WiFiClient espClient;
WiFiClientSecure espClient; // Use WiFiClientSecure for SSL/TLS MQTT connections
PubSubClient mqttClient(espClient);
unsigned long mqttLastConnectionAttempt = 0;
char mqttStringBuffer[40];
struct tm mqttTimeInfo; // NTP

// TFT display
Adafruit_ST7789 display = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(TFT_SCREEN_WIDTH, TFT_SCREEN_HEIGHT);
int64_t displayTimer = 0; // Timestamp of the last button press used to turn on the display

// SPH0645 I2S sound sensor
SemaphoreHandle_t xMutexSoundSensor; // Mutex to protect shared variables between tasks
int32_t soundSensorDataBuffer[I2S_DMA_BUF_LEN]; // I2S read buffer
float soundSensorSpl = 0; // Current sound level in dB
float soundSensorSplAverage = 0; // Average sound level in dB, calculated using an Exponential Moving Average (EMA) over the measurement window
float soundSensorSplPeak = 0; // Peak sound level in dB, within the measurement window
float soundSensorPeakLevels[MEASUREMENT_WINDOW];
int soundSensorLastTimeIndex = 0; // Last time index into soundSensorPeakLevels[] for tracking peak sound levels during each second

// VML7700 light sensor
Adafruit_VEML7700 lightSensor = Adafruit_VEML7700();
SemaphoreHandle_t xMutexLightSensor; // Mutex to protect shared variables between tasks
float lightSensorLux = 0; // Current light level in lux
float lightSensorLuxAverage = 0; // Average light level in lux, calculated using an Exponential Moving Average (EMA) over the measurement window
float lightSensorLuxPeak = 0; // Peak light level in lux, within the measurement window
float lightSensorGain = VEML7700_GAIN_1; // Current gain setting for the light sensor, read/updated from the AGC
int lightSensorIntegrationTime = VEML7700_IT_100MS; // Current integration time setting for the light sensor, read/updated from the AGC
float lightSensorPeakLevels[MEASUREMENT_WINDOW]; // Track max light levels for each second
int lightSensorLastTimeIndex = 0; // Last time index into lightSensorPeakLevels[] for tracking peak light levels during each second

// BME280
Adafruit_BME280 bme280;
SemaphoreHandle_t xMutexEnvironmental; // Mutex to protect shared variables between tasks
bool  environmentSensorOK; // True if the sensor data is OK, false if not
float environmentTemperature;
float environmentHumidity;
float environmentPressure;

// Uptime calculations
SemaphoreHandle_t xMutexUptime; // Mutex to protect shared variables between tasks
int64_t uptimeSecondsTotal, uptimeDays, uptimeHours, uptimeMinutes, uptimeSeconds;
int64_t lastUptimeSecondsTotal = 0;
char uptimeStringBuffer[24];

// LiPo battery and AC power state
SemaphoreHandle_t xMutexBattery; // Mutex to protect shared variables between tasks
Adafruit_MAX17048 max17048;
float batteryVoltage;
float batteryPercent;
int acPowerState; // Is set to 1 when 5V is present on the USB bus (AC power is on), and 0 when not (AC power is off)

// Historical data streams for the web page charts
unsigned char* psramDataSet;
char dataFormatBuffer[DATA_ELEMENT_SIZE + 1]; // +1 for NULL terminator

// Main loop
uint64_t lastUpdateTimeMqtt = 0; // Time of last MQTT update
uint64_t lastUpdateTimeTft = 0; // Time of last OLED update
uint64_t lastUpdateTimeData = 0; // Time of last data set update
uint64_t timer = 0; // Copy of the main uptime timer that doesn't need a semaphore

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

// Web server 404 handler
void webHandler404()
{
  webServer.send(404, "text/plain", "Not found");
}

// Web server "/" GET handler (for root/home page)
void webHandlerRoot()
{
  IPAddress ip = WiFi.localIP();

  // Build the HTML response in the web string buffer
  char* buffer = webStringBuffer; // "buffer" will be used to walk through the "webStringBuffer" work area using pointer arithmetic
  buffer += bytesAdded(sprintf(buffer, htmlHeader, WIFI_HOSTNAME, esp_timer_get_time() / 1000000)); // Hostname gets added to the HTML <title> inside the template header, and the ESP32 current seconds counter is used by JavaScript for the charts
  buffer += buffercat(buffer, "<table class=\"sensor\" cellspacing=\"0\" cellpadding=\"3\">"); // Sensor data table
  buffer += bytesAdded(sprintf(buffer, "<tr><th colspan=\"2\" class=\"header\">%s</th></tr>", WIFI_HOSTNAME)); // Network hostname

  xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing sound data (measured on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr><th>Sound Level (current)</th><td>%0.2f dB</td></tr>", soundSensorSpl)); // Current sound level in dB
  buffer += bytesAdded(sprintf(buffer, "<tr><th>Sound Level (average)</th><td>%0.2f dB</td></tr>", soundSensorSplAverage)); // Average sound level in dB
  buffer += bytesAdded(sprintf(buffer, "<tr><th>Sound Level (peak)</th><td>%0.2f dB</td></tr>", soundSensorSplPeak)); // Peak sound level in dB
  xSemaphoreGive(xMutexSoundSensor); // Done with sound data

  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing light data (measured on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"light\"><th>Light Level (current)</th><td>%0.2f lux</td></tr>", lightSensorLux)); // Current light level in lux
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"light\"><th>Light Level (average)</th><td>%0.2f lux</td></tr>", lightSensorLuxAverage)); // Average light level in lux
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"light\"><th>Light Level (peak)</th><td>%0.2f lux</td></tr>", lightSensorLuxPeak)); // Peak light level in lux
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"light\"><th>Light Measurement Gain</th><td>%0.3f</td></tr>", lightSensorGain)); // Current lux measurement gain
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"light\"><th>Light Measurement Integration Time</th><td>%d ms</td></tr>", lightSensorIntegrationTime)); // Current measurement integration time
  xSemaphoreGive(xMutexLightSensor); // Done with light data

  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data (calculated on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Sensor OK?</th><td>%s</td></tr>", environmentSensorOK ? "1 (Yes)" : "0 (No)")); // Environment sensor OK?
  #if defined(BME280_TEMP_F)
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Temperature</th><td>%0.1f&deg; F</td></tr>", environmentTemperature)); // Environment temperature
  #else
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Temperature</th><td>%0.1f&deg; C</td></tr>", environmentTemperature)); // Environment temperature
  #endif
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Humidity</th><td>%0.1f%%</td></tr>", environmentHumidity)); // Environment humidiy
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Barometric Pressure</th><td>%0.1f mbar</td></tr>", environmentPressure)); // Environment barometric pressure
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data (calculated on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>Measurement Window for Average/Peak Calculations</th><td>%d seconds</td></tr>", MEASUREMENT_WINDOW)); // Formatted uptime
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>Uptime</th><td>%lld seconds</td></tr>", uptimeSecondsTotal)); // Raw uptime in seconds
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>Uptime Detail</th><td>%s</td></tr>", uptimeStringBuffer)); // Formatted uptime
  xSemaphoreGive(xMutexUptime); // Done with uptime data

  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Free Heap Memory</th><td>%d bytes</td></tr>", ESP.getFreeHeap())); // ESP32 free heap memory, which indicates if the program still has enough memory to run effectively

  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data (calculated on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Battery</th><td>%0.2fV / %0.0f%%</td></tr>", batteryVoltage, batteryPercent)); // LiPo battery
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>AC Power State</th><td>%s</td></tr>", acPowerState ? "1 (On)" : "0 (Off)")); // AC power sense
  xSemaphoreGive(xMutexBattery); // Done with battery data

  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>MQTT Server</th><td>%s mqtts://%s:%d</td></tr>", mqttClient.connected() ? "Connected to" : "Disconnected from ", MQTT_SERVER, MQTT_PORT)); // MQTT connection
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>IP Address</th><td>%d.%d.%d.%d</td></tr>", ip[0], ip[1], ip[2], ip[3])); // Network address
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>WiFi Signal Strength (%s)</th><td>%d dBm</td></tr>", WIFI_SSID, WiFi.RSSI())); // Network address
  if (getLocalTime(&webTimeInfo))
  {
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>System Time</th><td>%02d/%02d/%02d %02d:%02d:%02d</td></tr>", timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_year + 1900, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec));
  }
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Chip Information</th><td>%s</td></tr>", chipInformation)); // ESP32 chipset information
  buffer += buffercat(buffer, "</table>"); // Sensor data table
  buffer += buffercat(buffer, htmlFooter); // HTML template footer

  // Send the HTML response to the client
  webServer.send(200, "text/html", webStringBuffer);
}

// Web server "/metrics" GET handler (for Prometheus and similar telemetry tools)
// Reference: https://github.com/prometheus/docs/blob/main/content/docs/instrumenting/exposition_formats.md
void webHandlerMetrics()
{
  // Build the HTML response in the web string buffer
  char* buffer = webStringBuffer; // "buffer" will be used to walk through the "webStringBuffer" work area using pointer arithmetic

  // Sound level
  xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing sound data (measured on a different thread)
  buffer += buffercat(buffer, "# HELP sound_level_db Sound pressure level (current)\n");
  buffer += buffercat(buffer, "# TYPE sound_level_db gauge\n");
  buffer += bytesAdded(sprintf(buffer, "sound_level_db %0.2f\n\n", soundSensorSpl));
  buffer += buffercat(buffer, "# HELP sound_level_db_average Sound pressure level (average)\n");
  buffer += buffercat(buffer, "# TYPE sound_level_db_average gauge\n");
  buffer += bytesAdded(sprintf(buffer, "sound_level_db_average %0.2f\n\n", soundSensorSplAverage));
  buffer += buffercat(buffer, "# HELP sound_level_db_peak Sound pressure level (peak)\n");
  buffer += buffercat(buffer, "# TYPE sound_level_db_peak gauge\n");
  buffer += bytesAdded(sprintf(buffer, "sound_level_db_peak %0.2f\n\n", soundSensorSplPeak));
  xSemaphoreGive(xMutexSoundSensor); // Done with sound data

  // Light level
  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing light data (measured on a different thread)
  buffer += buffercat(buffer, "# HELP light_level_lux Light level (current)\n");
  buffer += buffercat(buffer, "# TYPE light_level_lux gauge\n");
  buffer += bytesAdded(sprintf(buffer, "light_level_lux %0.2f\n\n", lightSensorLux));
  buffer += buffercat(buffer, "# HELP light_level_lux_average Light level (average)\n");
  buffer += buffercat(buffer, "# TYPE light_level_lux_average gauge\n");
  buffer += bytesAdded(sprintf(buffer, "light_level_lux_average %0.2f\n\n", lightSensorLuxAverage));
  buffer += buffercat(buffer, "# HELP light_level_lux_peak Light level (peak)\n");
  buffer += buffercat(buffer, "# TYPE light_level_lux_peak gauge\n");
  buffer += bytesAdded(sprintf(buffer, "light_level_lux_peak %0.2f\n\n", lightSensorLuxPeak));
  buffer += buffercat(buffer, "# HELP light_level_measurement_gain Light measurement gain\n");
  buffer += buffercat(buffer, "# TYPE light_level_measurement_gain gauge\n");
  buffer += bytesAdded(sprintf(buffer, "light_level_measurement_gain %0.3f\n\n", lightSensorGain));
  buffer += buffercat(buffer, "# HELP light_level_measurement_integration_time_ms Light measurement integration time\n");
  buffer += buffercat(buffer, "# TYPE light_level_measurement_integration_time_ms gauge\n");
  buffer += bytesAdded(sprintf(buffer, "light_level_measurement_integration_time_ms %d\n\n", lightSensorIntegrationTime));
  xSemaphoreGive(xMutexLightSensor); // Done with light data

  // Environmentals
  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data (calculated on a different thread)
  if (environmentSensorOK)
  {
    #if defined(BME280_TEMP_F)
      buffer += buffercat(buffer, "# HELP environmental_temperature Environment temperature (F)\n");
    #else
      buffer += buffercat(buffer, "# HELP environmental_temperature Environment temperature (C)\n");
    #endif
    buffer += buffercat(buffer, "# TYPE environmental_temperature gauge\n");
    buffer += bytesAdded(sprintf(buffer, "environmental_temperature %0.1f\n\n", environmentTemperature));
    buffer += buffercat(buffer, "# HELP environmental_humidity Environment humidity (RH%)\n");
    buffer += buffercat(buffer, "# TYPE environmental_humidity gauge\n");
    buffer += bytesAdded(sprintf(buffer, "environmental_humidity %0.1f\n\n", environmentHumidity));
    buffer += buffercat(buffer, "# HELP environmental_pressure_mbar Environment barometric pressure\n");
    buffer += buffercat(buffer, "# TYPE environmental_pressure_mbar gauge\n");
    buffer += bytesAdded(sprintf(buffer, "environmental_pressure_mbar %0.1f\n\n", environmentPressure));
  }
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

  // Measurement window
  buffer += buffercat(buffer, "# HELP measurement_window_seconds Measurement window for average/peak calculations\n");
  buffer += buffercat(buffer, "# TYPE measurement_window_seconds gauge\n");
  buffer += bytesAdded(sprintf(buffer, "measurement_window_seconds %d\n\n", MEASUREMENT_WINDOW));

  // Free heap memory
  buffer += buffercat(buffer, "# HELP esp32_free_heap_bytes ESP32 free heap memory\n");
  buffer += buffercat(buffer, "# TYPE esp32_free_heap_bytes gauge\n");
  buffer += bytesAdded(sprintf(buffer, "esp32_free_heap_bytes %d\n\n", ESP.getFreeHeap()));

  // Battery data and AC power on/off state
  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data
  buffer += buffercat(buffer, "# HELP esp32_battery_voltage ESP32 LiPo battery voltage\n");
  buffer += buffercat(buffer, "# TYPE esp32_battery_voltage gauge\n");
  buffer += bytesAdded(sprintf(buffer, "esp32_battery_voltage %0.2f\n\n", batteryVoltage));
  buffer += buffercat(buffer, "# HELP esp32_battery_percent ESP32 LiPo battery percent\n");
  buffer += buffercat(buffer, "# TYPE esp32_battery_percent gauge\n");
  buffer += bytesAdded(sprintf(buffer, "esp32_battery_percent %0.2f\n\n", batteryPercent));
  buffer += buffercat(buffer, "# HELP esp32_ac_power_state ESP32 AC power state\n");
  buffer += buffercat(buffer, "# TYPE esp32_ac_power_state gauge\n");
  buffer += bytesAdded(sprintf(buffer, "esp32_ac_power_state %d\n\n", acPowerState));
  xSemaphoreGive(xMutexBattery); // Done with battery data

  // WiFi signal strength
  buffer += buffercat(buffer, "# HELP esp32_wifi_signal_strength ESP32 WiFi signal strength\n");
  buffer += buffercat(buffer, "# TYPE esp32_wifi_signal_strength gauge\n");
  buffer += bytesAdded(sprintf(buffer, "esp32_wifi_signal_strength{SSID=\"%s\"} %d\n\n", WIFI_SSID, WiFi.RSSI()));

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

  // Set URI handlers
  webServer.on("/", webHandlerRoot);
  webServer.on("/metrics", webHandlerMetrics);
  webServer.on("/data", webHandlerData);
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
      Serial.println(mqttClient.state());
    }
  }
}

// Send all data to MQTT
void updateMQTT()
{
  // Sound level
  xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing the sound sensor data (calculated on a different thread)
  sprintf(mqttStringBuffer, "%0.2f", soundSensorSpl);
  mqttClient.publish(MQTT_TOPIC_BASE "sound_level_db", mqttStringBuffer, true);
  sprintf(mqttStringBuffer, "%0.2f", soundSensorSplAverage);
  mqttClient.publish(MQTT_TOPIC_BASE "sound_level_db_average", mqttStringBuffer, true);
  sprintf(mqttStringBuffer, "%0.2f", soundSensorSplPeak);
  mqttClient.publish(MQTT_TOPIC_BASE "sound_level_db_peak", mqttStringBuffer, true);
  xSemaphoreGive(xMutexSoundSensor); // Done with sound sensor data

  // Light level
  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing the light sensor data (calculated on a different thread)
  sprintf(mqttStringBuffer, "%0.2f", lightSensorLux);
  mqttClient.publish(MQTT_TOPIC_BASE "light_level_lux", mqttStringBuffer, true);
  sprintf(mqttStringBuffer, "%0.2f", lightSensorLuxAverage);
  mqttClient.publish(MQTT_TOPIC_BASE "light_level_lux_average", mqttStringBuffer, true);
  sprintf(mqttStringBuffer, "%0.2f", lightSensorLuxPeak);
  mqttClient.publish(MQTT_TOPIC_BASE "light_level_lux_peak", mqttStringBuffer, true);
  sprintf(mqttStringBuffer, "%0.3f", lightSensorGain);
  mqttClient.publish(MQTT_TOPIC_BASE "light_level_measurement_gain", mqttStringBuffer, true);
  sprintf(mqttStringBuffer, "%d", lightSensorIntegrationTime);
  mqttClient.publish(MQTT_TOPIC_BASE "light_level_measurement_integration_time_ms", mqttStringBuffer, true);
  xSemaphoreGive(xMutexLightSensor); // Done with light sensor data

  // Environmentals
  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data (calculated on a different thread)
  if (environmentSensorOK)
  {
    sprintf(mqttStringBuffer, "%0.1f", environmentTemperature); // Regardless of F or C units, just report the number to MQTT
    mqttClient.publish(MQTT_TOPIC_BASE "environmental_temperature", mqttStringBuffer, true);
    sprintf(mqttStringBuffer, "%0.1f", environmentHumidity);
    mqttClient.publish(MQTT_TOPIC_BASE "environmental_humidity", mqttStringBuffer, true);
    sprintf(mqttStringBuffer, "%0.1f", environmentPressure);
    mqttClient.publish(MQTT_TOPIC_BASE "environmental_pressure_mbar", mqttStringBuffer, true);
  }
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

  // Measurement window
  sprintf(mqttStringBuffer, "%d", MEASUREMENT_WINDOW);
  mqttClient.publish(MQTT_TOPIC_BASE "measurement_window_seconds", mqttStringBuffer, true);

  // Uptime information
  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data
  sprintf(mqttStringBuffer, "%lld", timer);
  mqttClient.publish(MQTT_TOPIC_BASE "uptime_seconds", mqttStringBuffer, true);
  mqttClient.publish(MQTT_TOPIC_BASE "uptime_detail", uptimeStringBuffer, true);
  xSemaphoreGive(xMutexUptime); // Done with uptime data

  // Free heap memory
  sprintf(mqttStringBuffer, "%d", ESP.getFreeHeap());
  mqttClient.publish(MQTT_TOPIC_BASE "esp32_free_heap_bytes", mqttStringBuffer, true);

  // Battery data and AC power on/off state
  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data (calculated on a different thread)
  sprintf(mqttStringBuffer, "%0.2f", batteryVoltage);
  mqttClient.publish(MQTT_TOPIC_BASE "esp32_battery_voltage", mqttStringBuffer, true);
  sprintf(mqttStringBuffer, "%0.2f", batteryPercent);
  mqttClient.publish(MQTT_TOPIC_BASE "esp32_battery_percent", mqttStringBuffer, true);
  mqttClient.publish(MQTT_TOPIC_BASE "esp32_ac_power_state", acPowerState ? "1" : "0", true); // 1 for "ON" or 0 for "OFF"
  xSemaphoreGive(xMutexBattery); // Done with battery data

  // WiFi signal strength
  sprintf(mqttStringBuffer, "%d", WiFi.RSSI());
  mqttClient.publish(MQTT_TOPIC_BASE "esp32_wifi_signal_strength_dbm", mqttStringBuffer, true);

  // NTP system time
  if (getLocalTime(&mqttTimeInfo))
  {
    sprintf(mqttStringBuffer, "%02d/%02d/%d %02d:%02d:%02d", timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_year + 1900, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
    mqttClient.publish(MQTT_TOPIC_BASE "esp32_system_time", mqttStringBuffer, true);
  }

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
        memset(psramDataSet, ' ', DATA_SET_SIZE);
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
  memset(dataFormatBuffer, ' ', DATA_ELEMENT_SIZE);   // Fill with spaces
  int n = sprintf(dataFormatBuffer, "%0.2f,", value); // Format the value with delimiter and NULL terminator
  if (n > 0) dataFormatBuffer[n] = ' ';               // Remove sprintf() NULL terminator
  int dataSetIndex = stream * DATA_STREAM_SIZE + DATA_STREAM_SIZE - DATA_ELEMENT_SIZE; // Byte index of the last element in the specified data stream
  memcpy(psramDataSet + dataSetIndex, dataFormatBuffer, DATA_ELEMENT_SIZE); // Copy the new value (with delimiter) into the data set
}
void addDataElement(int stream, uint64_t value)
{
  memset(dataFormatBuffer, ' ', DATA_ELEMENT_SIZE);   // Fill with spaces
  int n = sprintf(dataFormatBuffer, "%lld,", value);  // Format the value with delimiter and NULL terminator
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

    xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing the sound sensor data (calculated on a different thread)
    addDataElement(0, soundSensorSpl);
    xSemaphoreGive(xMutexSoundSensor); // Done with sound sensor data

    xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing the light sensor data (calculated on a different thread)
    addDataElement(1, lightSensorLux);
    xSemaphoreGive(xMutexLightSensor); // Done with light sensor data

    xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data (calculated on a different thread)
    addDataElement(2, environmentTemperature);
    addDataElement(3, environmentHumidity);
    addDataElement(4, environmentPressure);
    xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

    addDataElement(5, timer); // Time index
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
  display.setRotation(TFT_ROTATION);

  // Clear the display
  display.fillScreen(ST77XX_BLACK);

  // Establish defaults
  display.setRotation(3);
  canvas.setFont(&FreeSans9pt7b);
  canvas.setTextColor(ST77XX_WHITE); // White text
}

// Helper function to format the wide range of possible lux values into a consistent 4-digit representation with variable decimal places
char formatLuxBuffer[10];
void formatLux(float lux)
{
  if (lux < 10)
  {
    sprintf(formatLuxBuffer, "%0.3f", lux);
  }
  else if (lux < 100)
  {
    sprintf(formatLuxBuffer, "%.2f", lux);
  }
  else if (lux < 1000)
  {
    sprintf(formatLuxBuffer, "%.1f", lux);
  }
  else if (lux < 10000)
  {
    sprintf(formatLuxBuffer, "%.0f", lux);
  }
  else if (lux < 100000)
  {
    sprintf(formatLuxBuffer, "%0.2fK", lux/1000); // Such as "28.00K"
  }
  else
  {
    sprintf(formatLuxBuffer, "%0.1fK", lux/1000); // Such as "102.0K"
  }
}

// Update the TFT display
void updateDisplay()
{
  canvas.fillScreen(ST77XX_BLACK);
  canvas.setFont(&FreeSans9pt7b);
  canvas.setCursor(0, 20);

  xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing sound data (measured on a different thread)
  canvas.setTextColor(ST77XX_WHITE);
  canvas.printf("%0.2f    %0.2f    %0.2f dB", soundSensorSpl, soundSensorSplAverage, soundSensorSplPeak);
  canvas.println();
  xSemaphoreGive(xMutexSoundSensor); // Done with sound data

  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing light data (measured on a different thread)
  canvas.setTextColor(ST77XX_YELLOW);
  //canvas.printf("%0.2f  %0.2f  %0.2f lux", lightSensorLux, lightSensorLuxAverage, lightSensorLuxPeak);
  formatLux(lightSensorLux);        canvas.print(formatLuxBuffer); canvas.print("  ");
  formatLux(lightSensorLuxAverage); canvas.print(formatLuxBuffer); canvas.print("  ");
  formatLux(lightSensorLuxPeak);    canvas.print(formatLuxBuffer); canvas.print(" lux");
  canvas.println();
  xSemaphoreGive(xMutexLightSensor); // Done with light data

  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data (calculated on a different thread)
  canvas.setTextColor(ST77XX_GREEN);
  #if defined(BME280_TEMP_F)
    canvas.printf("%0.1fF   %0.1f%%   %0.0f mbar", environmentTemperature, environmentHumidity, environmentPressure);
  #else
    canvas.printf("%0.1fC   %0.1f%%   %0.0f mbar", environmentTemperature, environmentHumidity, environmentPressure);
  #endif
  canvas.println();
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data (calculated on a different thread)
  canvas.setTextColor(ST77XX_MAGENTA);
  canvas.printf("Battery: %0.2fV / %0.0f%%", batteryVoltage, batteryPercent);
  canvas.println();
  xSemaphoreGive(xMutexBattery); // Done with battery data

  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data (calculated on a different thread)
  canvas.setTextColor(ST77XX_CYAN);
  if (uptimeSeconds % 2)
  {
    canvas.printf("Uptime: %s", uptimeStringBuffer);
  }
  else
  {
    IPAddress ip = WiFi.localIP();
    canvas.printf("IP Address: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  }
  //canvas.println();
  xSemaphoreGive(xMutexUptime); // Done with uptime data

  display.drawRGBBitmap(0, 0, canvas.getBuffer(), TFT_SCREEN_WIDTH, TFT_SCREEN_HEIGHT);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// BME280 Environmental Sensor
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Setup the BME280 sensor
void setupEnvironmentalSensor()
{
  Serial.println("Environmentals: Initializing BME280");
  if (!bme280.begin(BME280_ADDRESS))
  {
    Serial.println("Environmentals: BME280 configuration failed");
  }
}

// Environmental data measurement using the BME280 sensor
void measureEnvironmentals()
{
  float t, h, p;

  // Read the environmental data
  #if defined(BME280_TEMP_F)
    t = bme280.readTemperature() * 9 / 5 + 32 + BME280_TEMP_ADJUSTMENT;
  #else
    t = bme280.readTemperature() + BME280_TEMP_ADJUSTMENT;
  #endif
  h = bme280.readHumidity();
  p = bme280.readPressure() / 100.0;

  // Is there any trouble with the sensor data?
  if (!(t < -50 || t > 200 || h < 0 || h > 100 || p < 700 || p > 1300))
  {
    if (isnanf(t) || isnanf(h) || isnanf(p))
    {
      // Reset the BME280
      Serial.println("Environmentals: NaN detected - Resetting BME280");
      setupEnvironmentalSensor();
      xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
      environmentSensorOK = false;
      xSemaphoreGive(xMutexEnvironmental); // Done with environmental data
    }
    else
    {
      // Update the environmental values
      xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
      if (environmentTemperature == 0 && environmentHumidity == 0 && environmentPressure == 0)
      {
        // Initialize values
        environmentTemperature = t;
        environmentHumidity = h;
        environmentPressure = p;
      }
      else
      {
        // Smooth values with an EMA moving average
        environmentTemperature = (environmentTemperature * (MEASUREMENT_WINDOW - 1) + t) / MEASUREMENT_WINDOW;
        environmentHumidity = (environmentHumidity * (MEASUREMENT_WINDOW - 1) + h) / MEASUREMENT_WINDOW;
        environmentPressure = (environmentPressure * (MEASUREMENT_WINDOW - 1) + p) / MEASUREMENT_WINDOW;
      }
      environmentSensorOK = true;
      xSemaphoreGive(xMutexEnvironmental); // Done with environmental data
    }
  }
  else
  {
    // Reset the BME280
    Serial.printf("Environmentals: BME280 data error: t=%0.1f h=%0.1f p=%0.1f", t, h, p); Serial.println();
    setupEnvironmentalSensor();
    xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
    environmentSensorOK = false;
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
  // Read lux using the automatic method which adjusts gain and integration time as needed to obtain
  // a good reading. A non-linear correction is also applied if needed.
  float lux = lightSensor.readLux(VEML_LUX_AUTO);

  // Read the gain and integration time settings from the light sensor
  float gain; 
  switch (lightSensor.getGain())
  {
    case VEML7700_GAIN_2:   gain = 2;    break;
    case VEML7700_GAIN_1:   gain = 1;    break;
    case VEML7700_GAIN_1_4: gain = .25;  break;
    case VEML7700_GAIN_1_8: gain = .125; break;
  }
  int integrationTime;
  switch (lightSensor.getIntegrationTime())
  {
    case VEML7700_IT_25MS:  integrationTime = 25;  break;
    case VEML7700_IT_50MS:  integrationTime = 50;  break;
    case VEML7700_IT_100MS: integrationTime = 100; break;
    case VEML7700_IT_200MS: integrationTime = 200; break;
    case VEML7700_IT_400MS: integrationTime = 400; break;
    case VEML7700_IT_800MS: integrationTime = 800; break;
  }

  // Track peak levels
  unsigned long timeIndex = (millis() / 1000) % MEASUREMENT_WINDOW; // The time index rotates through the measurement window at the rate of one slot per second
  if (timeIndex != lightSensorLastTimeIndex)
  {
    // A new second/slot
    lightSensorLastTimeIndex = timeIndex;
    lightSensorPeakLevels[timeIndex] = lux; // Initial value for the current second
  }
  else
  {
    // Subsequent measurement during the same second/slot as the previous measurement
    lightSensorPeakLevels[timeIndex] = max(lightSensorPeakLevels[timeIndex], lux);
  }
  float peakLux = 0;
  for (int i = 0; i < MEASUREMENT_WINDOW; i++)
  {
    peakLux = max(peakLux, lightSensorPeakLevels[i]);
  }

  // Update light sensor data values
  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing the light sensor data
  lightSensorLux = lux; // Current lux value
  if (lightSensorLuxAverage == 0)
  {
    // Initialize values
    lightSensorLuxAverage = lux;
  }
  else
  {
    // EMA moving average to smooth the lux reading
    lightSensorLuxAverage = (lightSensorLuxAverage * (MEASUREMENT_WINDOW - 1) + lux) / MEASUREMENT_WINDOW;
  }
  lightSensorLuxPeak = peakLux;
  lightSensorGain = gain;
  lightSensorIntegrationTime = integrationTime;
  xSemaphoreGive(xMutexLightSensor); // Done with light sensor data
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
  // Start accessing the battery data
  xSemaphoreTake(xMutexBattery, portMAX_DELAY);

  // Read battery voltage
  float v = max17048.cellVoltage();
  float p = max17048.cellPercent();
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

  // Read the AC power on/off state from a digital input pin
  acPowerState = digitalRead(AC_POWER_PIN);

  // Done with battery data
  xSemaphoreGive(xMutexBattery);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Read all I2C devices in sequence (background task)
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Each device is read in sequence so that a single task can be used to read all I2C data, and so the timing can be well-controlled
void readI2CDevices(void *parameter)
{
  unsigned long timer;

  // Infinite loop since this is a separate task from the main thread
  while (true)
  {
    // Read the I2C devices in sequence
    timer = millis();
    measureEnvironmentals(); // Read the BME280 sensor data
    measureLight(); // Measure the light level
    measureBattery(); // Read the LiPo battery voltage
    timer = millis() - timer; // Elapsed time in ms, which seems to be around 711 ms

    // Only read the I2C devices approximately every second, but just SLIGHTLY faster to ensure that light and sound peak tracking gets enough hits
    if (timer < 950)
    {
      delay(950 - timer); // Non-blocking delay on ESP32, in milliseconds
    }
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

        // Rough calculation of the sound pressure level (SPL) in dB based on the measured samples. It's more like the dB range rather than an absolute dB level.
        float spl = SPL_FACTOR * log10(maxValue - minValue);

        // Track peak levels
        unsigned long timeIndex = (millis() / 1000) % MEASUREMENT_WINDOW; // The time index rotates through the measurement window at the rate of one slot per second
        if (timeIndex != soundSensorLastTimeIndex)
        {
          // A new second/slot
          soundSensorLastTimeIndex = timeIndex;
          soundSensorPeakLevels[timeIndex] = spl; // Initial value for the current second
        }
        else
        {
          // Subsequent measurement during the same second/slot as the previous measurement
          soundSensorPeakLevels[timeIndex] = max(soundSensorPeakLevels[timeIndex], spl);
        }
        float peakSpl = 0;
        for (int i = 0; i < MEASUREMENT_WINDOW; i++)
        {
          peakSpl = max(peakSpl, soundSensorPeakLevels[i]);
        }

        // Update the sound level values
        xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing the sound sensor data
        soundSensorSpl = spl; // Current SPL value
        if (soundSensorSplAverage == 0)
        {
          // Initialize values
          soundSensorSplAverage = spl;
        }
        else
        {
          // EMA moving average to smooth the SPL reading
          soundSensorSplAverage = (soundSensorSplAverage * (MEASUREMENT_WINDOW - 1) + spl) / MEASUREMENT_WINDOW;
        }
        soundSensorSplPeak = peakSpl; // Peak SPL value
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

  // Sensor setup
  delay(200); // Allow the sensor modules time to initialize after powering on
  setupTFT();
  setupEnvironmentalSensor();
  setupBatteryMonitor();
  setupSoundSensor();
  setupLightSensor();
  xMutexEnvironmental = xSemaphoreCreateMutex();
  xMutexUptime        = xSemaphoreCreateMutex();
  xMutexBattery       = xSemaphoreCreateMutex();
  xMutexSoundSensor   = xSemaphoreCreateMutex();
  xMutexLightSensor   = xSemaphoreCreateMutex();

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
    8000,             // Stack size
    NULL,             // Parameter to pass
    1,                // Task priority
    NULL              // Task handle
  );

  // Create a task for reading sound levels
  xTaskCreate(
    measureSound,   // Function to be called
    "measureSound", // Name of the task
    8000,           // Stack size
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
  // MQTT connection management
  //if (!mqttClient.connected()) connectMQTT(); else mqttClient.loop();

  // Web server request management
  webServer.handleClient();

  // Uptime calculations: How long has the ESP32 been running since it was booted up?
  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data
  bool newSecond = false;
  uptimeSecondsTotal = esp_timer_get_time() / 1000000;
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

  // Update outputs every specified update interval, and the first-time through the loop
  bool updateTft  = timer - lastUpdateTimeTft  >= UPDATE_INTERVAL_TFT; // The display can be updated more frequently than MQTT
  bool updateMqtt = timer - lastUpdateTimeMqtt >= UPDATE_INTERVAL_MQTT;
  bool updateData = timer - lastUpdateTimeData >= UPDATE_INTERVAL_DATA;
  if (updateTft || updateMqtt || updateData || lastUpdateTimeTft == 0 || lastUpdateTimeMqtt == 0 || updateData == 0)
  {
    // Update the display
    if (updateTft)
    {
      lastUpdateTimeTft = timer;
      updateDisplay();
    }

    // Update MQTT
    if (updateMqtt)
    {
      lastUpdateTimeMqtt = timer;
      updateMQTT();
    }

    // Update the data set
    if (updateData)
    {
      lastUpdateTimeData = timer;
      updateDataSet();
    }
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
  delay(40); // Non-blocking delay on ESP32, in milliseconds
}
