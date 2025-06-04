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
#include <bsec2.h>                // BME680 support
#include <hal/efuse_hal.h>        // Espressif ESP32 chip information
#include <time.h>                 // NTP and time support

// App configuration
#include <html.h>                 // HTML templates
#include <config.h>               // The configuration references objects in the above libraries, so include it after those

// ESP32
char chipInformation[100];   // Chip information buffer
SemaphoreHandle_t xMutexI2C; // Mutex to streamline access to I2C devices

// Web server
WebServer webServer(80);
char webStringBuffer[16 * 1024];

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

// BME680
Bsec2 bme680;
SemaphoreHandle_t xMutexEnvironmental; // Mutex to protect shared variables between tasks
bool environmentSensorReady;  // True if the sensor is stabilized and reporting non-default values
float environmentTemperature; // Always in C and converted to F on output if BME680_TEMP_F is true
float environmentDewPoint;    // Always in C and converted to F on output if BME680_TEMP_F is true
float environmentHumidity;
float environmentPressure;
float environmentIAQ;         // 0 - 500 representing "clean" to "extremely polluted"
int   environmentIAQAccuracy; // 0 = unreliable, 1 = low, 2 = medium, 3 = high accuracy
float environmentCO2;
float environmentVOC;
float environmentGasResistance;
float environmentGasPercentage;
char environmentLibraryInformation[20]; // BSEC version information buffer
bsecSensor environmentSensorList[] = {
  // Desired subscription list of BSEC outputs
  BSEC_OUTPUT_IAQ,
  //BSEC_OUTPUT_RAW_TEMPERATURE,
  BSEC_OUTPUT_RAW_PRESSURE,
  //BSEC_OUTPUT_RAW_HUMIDITY,
  BSEC_OUTPUT_RAW_GAS,
  BSEC_OUTPUT_STABILIZATION_STATUS,
  BSEC_OUTPUT_RUN_IN_STATUS,
  BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
  BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
  //BSEC_OUTPUT_STATIC_IAQ,
  BSEC_OUTPUT_CO2_EQUIVALENT,
  BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
  BSEC_OUTPUT_GAS_PERCENTAGE
  //BSEC_OUTPUT_COMPENSATED_GAS
};

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

// Historical data streams for the web page charts
#define DATA_ELEMENT_SIZE   10    // Size of one data value as numeric text, with comma delimiter
#define DATA_STREAM_COUNT   9     // There are nine data streams: sound, light, temperature, humidity, pressure, IAQ, CO2, VOC, time
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
inline char* webFormatIAQAccuracy(int a)
{
  switch (a)
  {
    case 0:  return "Unreliable";
    case 1:  return "Low";
    case 2:  return "Medium";
    default: return "High";
  }
}

// Web helper function to render the main data table (the "dashboard") at the specified buffer position
char* webRenderDashboard(char* buffer)
{
  IPAddress ip = WiFi.localIP();
  struct tm timeInfo; // NTP

  buffer += buffercat(buffer, "<table id=\"dashboard\" class=\"sensor\" cellspacing=\"0\" cellpadding=\"3\">"); // Sensor data table
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
  if (environmentSensorReady)
  {
    #if defined(BME680_TEMP_F)
      buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Temperature</th><td>%0.1f&deg; F</td></tr>", environmentTemperature * 9.0F / 5.0F + 32.0F)); // Environment temperature
      buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Dew Point</th><td>%0.1f&deg; F</td></tr>", environmentDewPoint * 9.0F / 5.0F + 32.0F)); // Environment dew point
    #else
      buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Temperature</th><td>%0.1f&deg; C</td></tr>", environmentTemperature)); // Environment temperature
      buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Dew Point</th><td>%0.1f&deg; C</td></tr>", environmentDewPoint)); // Environment dew point
    #endif
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Humidity</th><td>%0.1f%%</td></tr>", environmentHumidity)); // Environment humidiy
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Barometric Pressure</th><td>%0.1f mbar</td></tr>", environmentPressure)); // Environment barometric pressure
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment IAQ Accuracy</th><td>%d (%s)</td></tr>", environmentIAQAccuracy, webFormatIAQAccuracy(environmentIAQAccuracy))); // Environment Indoor Air Quality accuracy
    if (environmentIAQAccuracy)
    {
      buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment IAQ</th><td>%0.2f</td></tr>",     environmentIAQ)); // Environment Indoor Air Quality (IAQ) estimate
      buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment CO2</th><td>%0.2f ppm</td></tr>", environmentCO2)); // Environment CO2 equivalent estimate
      buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment VOC</th><td>%0.2f ppm</td></tr>", environmentVOC)); // Environment Breath VOC Concentration estimate
    }  
  }
  else
  {
    //buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Sensor Stabilized?</th><td>%s</td></tr>", environmentSensorReady ? "1 (Yes)" : "0 (No)")); // Environment sensor stabilized?
    buffer += buffercat(buffer, "<tr class=\"environmental\"><th>Environment Sensor Stabilized?</th><td>0 (No)</td></tr>"); // Environment sensor stabilized?
  }
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data (calculated on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>Measurement Window for Average/Peak Calculations</th><td>%d seconds</td></tr>", MEASUREMENT_WINDOW)); // Formatted uptime
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>Uptime</th><td>%lld seconds</td></tr>", uptimeSecondsTotal)); // Raw uptime in seconds
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>Uptime Detail</th><td>%s</td></tr>", uptimeStringBuffer)); // Formatted uptime
  if (getLocalTime(&timeInfo))
  {
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>System Time</th><td>%02d/%02d/%02d %02d:%02d:%02d</td></tr>", timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_year + 1900, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec));
  }
  xSemaphoreGive(xMutexUptime); // Done with uptime data

  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Free Heap Memory</th><td>%d bytes</td></tr>", ESP.getFreeHeap())); // ESP32 free heap memory, which indicates if the program still has enough memory to run effectively

  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data (calculated on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Battery</th><td>%0.2fV / %0.0f%%</td></tr>", batteryVoltage, batteryPercent)); // LiPo battery
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>AC Power State</th><td>%s</td></tr>", acPowerState ? "1 (On)" : "0 (Off)")); // AC power sense
  xSemaphoreGive(xMutexBattery); // Done with battery data

  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>MQTT Server</th><td>%s mqtts://%s:%d</td></tr>", mqttClient.connected() ? "Connected to" : "Disconnected from ", MQTT_SERVER, MQTT_PORT)); // MQTT connection
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>IP Address</th><td>%d.%d.%d.%d</td></tr>", ip[0], ip[1], ip[2], ip[3])); // Network address
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>WiFi Signal Strength (%s)</th><td>%d dBm</td></tr>", WIFI_SSID, WiFi.RSSI())); // Network address
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Bosch BSEC Library Version</th><td>%s</td></tr>", environmentLibraryInformation)); // BME680 BSEC library version
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Chip Information</th><td>%s</td></tr>", chipInformation)); // ESP32 chipset information
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
  if (environmentSensorReady)
  {
    #if defined(BME680_TEMP_F)
      buffer += buffercat(buffer, "# HELP environmental_temperature Environment temperature (F)\n");
      buffer += buffercat(buffer, "# TYPE environmental_temperature gauge\n");
      buffer += bytesAdded(sprintf(buffer, "environmental_temperature %0.1f\n\n", environmentTemperature * 9.0F / 5.0F + 32.0F));
      buffer += buffercat(buffer, "# HELP environmental_dew_point Environment dew point (F)\n");
      buffer += buffercat(buffer, "# TYPE environmental_dew_point gauge\n");
      buffer += bytesAdded(sprintf(buffer, "environmental_dew_point %0.1f\n\n", environmentDewPoint * 9.0F / 5.0F + 32.0F));
    #else
      buffer += buffercat(buffer, "# HELP environmental_temperature Environment temperature (C)\n");
      buffer += buffercat(buffer, "# TYPE environmental_temperature gauge\n");
      buffer += bytesAdded(sprintf(buffer, "environmental_temperature %0.1f\n\n", environmentTemperature));
      buffer += buffercat(buffer, "# HELP environmental_dew_point Environment calculated dew point (C)\n");
      buffer += buffercat(buffer, "# TYPE environmental_dew_point gauge\n");
      buffer += bytesAdded(sprintf(buffer, "environmental_dew_point %0.1f\n\n", environmentDewPoint));
    #endif
    buffer += buffercat(buffer, "# HELP environmental_humidity Environment humidity (RH%)\n");
    buffer += buffercat(buffer, "# TYPE environmental_humidity gauge\n");
    buffer += bytesAdded(sprintf(buffer, "environmental_humidity %0.1f\n\n", environmentHumidity));
    buffer += buffercat(buffer, "# HELP environmental_pressure_mbar Environment barometric pressure\n");
    buffer += buffercat(buffer, "# TYPE environmental_pressure_mbar gauge\n");
    buffer += bytesAdded(sprintf(buffer, "environmental_pressure_mbar %0.1f\n\n", environmentPressure));

    // IAQ metrics
    buffer += buffercat(buffer, "# HELP environmental_iaq_accuracy Environment IAQ accuracy (0=unreliable, 1=low, 2=medium, 3=high)\n");
    buffer += buffercat(buffer, "# TYPE environmental_iaq_accuracy gauge\n");
    buffer += bytesAdded(sprintf(buffer, "environmental_iaq_accuracy %d\n\n", environmentIAQAccuracy));
    if (environmentIAQAccuracy)
    {
      buffer += buffercat(buffer, "# HELP environmental_iaq Environment IAQ (0=clean, 50=good, 200=polluted, 500=extremely polluted)\n");
      buffer += buffercat(buffer, "# TYPE environmental_iaq gauge\n");
      buffer += bytesAdded(sprintf(buffer, "environmental_iaq %0.2f\n\n", environmentIAQ));
      buffer += buffercat(buffer, "# HELP environmental_co2_ppm Environment calculated CO2 equivalent estimate (500=normal)\n");
      buffer += buffercat(buffer, "# TYPE environmental_co2_ppm gauge\n");
      buffer += bytesAdded(sprintf(buffer, "environmental_co2_ppm %0.2f\n\n", environmentCO2));
      buffer += buffercat(buffer, "# HELP environmental_voc_ppm Environment calculated breath-VOC concentration estimate (< 1.0 is good)\n");
      buffer += buffercat(buffer, "# TYPE environmental_voc_ppm gauge\n");
      buffer += bytesAdded(sprintf(buffer, "environmental_voc_ppm %0.2f\n\n", environmentVOC));
    }
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

  // BSEC library version
  buffer += buffercat(buffer, "# HELP esp32_bsec_library_version Bosch BSEC library version\n");
  buffer += buffercat(buffer, "# TYPE esp32_bsec_library_version gauge\n");
  buffer += bytesAdded(sprintf(buffer, "esp32_bsec_library_version{version=\"%s\"} 1\n\n", environmentLibraryInformation));

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

  // Sound level
  xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing the sound sensor data (calculated on a different thread)
  MQTT_PUBLISH(MQTT_TOPIC_BASE "sound_level_db",         "%0.2f", soundSensorSpl);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "sound_level_db_average", "%0.2f", soundSensorSplAverage);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "sound_level_db_peak",    "%0.2f", soundSensorSplPeak);
  xSemaphoreGive(xMutexSoundSensor); // Done with sound sensor data

  // Light level
  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing the light sensor data
  MQTT_PUBLISH(MQTT_TOPIC_BASE "light_level_lux",         "%0.2f", lightSensorLux);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "light_level_lux_average", "%0.2f", lightSensorLuxAverage);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "light_level_lux_peak",    "%0.2f", lightSensorLuxPeak);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "light_level_measurement_gain", "%0.3f", lightSensorGain);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "light_level_measurement_integration_time_ms", "%d", lightSensorIntegrationTime);
  xSemaphoreGive(xMutexLightSensor); // Done with light sensor data

  // Environmentals
  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
  if (environmentSensorReady)
  {
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_temperature",   "%0.1f", BME680_TEMP_F ? environmentTemperature * 9.0F / 5.0F + 32.0F : environmentTemperature);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_dew_point",     "%0.1f", BME680_TEMP_F ? environmentDewPoint    * 9.0F / 5.0F + 32.0F : environmentDewPoint);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_humidity",      "%0.1f", environmentHumidity);
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_pressure_mbar", "%0.1f", environmentPressure);

    // IAQ metrics
    MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_iaq_accuracy", "%d", environmentIAQAccuracy);
    if (environmentIAQAccuracy)
    {
      MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_iaq",     "%0.2f", environmentIAQ);
      MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_co2_ppm", "%0.2f", environmentCO2);
      MQTT_PUBLISH(MQTT_TOPIC_BASE "environmental_voc_ppm", "%0.2f", environmentVOC);
    }
  }
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

  // Measurement window
  MQTT_PUBLISH(MQTT_TOPIC_BASE "measurement_window_seconds", "%d", MEASUREMENT_WINDOW);

  // Uptime information
  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data
  MQTT_PUBLISH(MQTT_TOPIC_BASE "uptime_seconds", "%lld", timer);
  mqttClient.publish(MQTT_TOPIC_BASE "uptime_detail", uptimeStringBuffer, true);
  xSemaphoreGive(xMutexUptime); // Done with uptime data

  // Free heap memory
  MQTT_PUBLISH(MQTT_TOPIC_BASE "esp32_free_heap_bytes", "%d", ESP.getFreeHeap());

  // Battery data and AC power on/off state
  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data
  MQTT_PUBLISH(MQTT_TOPIC_BASE "esp32_battery_voltage", "%0.2f", batteryVoltage);
  MQTT_PUBLISH(MQTT_TOPIC_BASE "esp32_battery_percent", "%0.2f", batteryPercent);
  mqttClient.publish(MQTT_TOPIC_BASE "esp32_ac_power_state", acPowerState ? "1" : "0", true); // 1 for "ON" or 0 for "OFF"
  xSemaphoreGive(xMutexBattery); // Done with battery data

  // WiFi signal strength
  MQTT_PUBLISH(MQTT_TOPIC_BASE "esp32_wifi_signal_strength_dbm", "%d", WiFi.RSSI());

  // NTP system time
  if (getLocalTime(&timeInfo))
  {
    sprintf(mqttStringBuffer, "%02d/%02d/%d %02d:%02d:%02d", timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_year + 1900, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
    mqttClient.publish(MQTT_TOPIC_BASE "esp32_system_time", mqttStringBuffer, true);
  }

  // BME680 library information
  mqttClient.publish(MQTT_TOPIC_BASE "esp32_bsec_library_version", environmentLibraryInformation, true);

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

// Add current sensor values to the end of each data stream
void updateDataSet()
{
  if (psramDataSet)
  {
    // Shift all data streams down by one element. The updates below will overwrite the last element in each stream.
    memmove(psramDataSet, psramDataSet + DATA_ELEMENT_SIZE, DATA_SET_SIZE - DATA_ELEMENT_SIZE);

    xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing the sound sensor data
    addDataElement(0, soundSensorSpl);
    xSemaphoreGive(xMutexSoundSensor); // Done with sound sensor data

    xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing the light sensor data
    addDataElement(1, lightSensorLux);
    xSemaphoreGive(xMutexLightSensor); // Done with light sensor data

    xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
    addDataElement(2, environmentTemperature); // Always in Celsius
    addDataElement(3, environmentHumidity);
    addDataElement(4, environmentPressure);
    addDataElement(5, environmentIAQ);
    addDataElement(6, environmentCO2);
    addDataElement(7, environmentVOC);
    xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

    addDataElement(8, timer); // Time index
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
  canvas.printf("%0.2f    %0.2f    %0.2f dB", soundSensorSpl, soundSensorSplAverage, soundSensorSplPeak);
  canvas.println();
  xSemaphoreGive(xMutexSoundSensor); // Done with sound data

  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing light data (measured on a different thread)
  canvas.setTextColor(ST77XX_YELLOW);
  formatLux(formatBuffer, lightSensorLux);        canvas.print(formatBuffer); canvas.print("  ");
  formatLux(formatBuffer, lightSensorLuxAverage); canvas.print(formatBuffer); canvas.print("  ");
  formatLux(formatBuffer, lightSensorLuxPeak);    canvas.print(formatBuffer); canvas.print(" lux");
  canvas.println();
  xSemaphoreGive(xMutexLightSensor); // Done with light data

  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data (calculated on a different thread)
  canvas.setTextColor(ST77XX_GREEN);
  if (seconds % 2)
  {
    if (environmentVOC >= 100.0F)
    {
      canvas.printf("IAQ %0.1f VOC %0.0f ppm", environmentIAQ, environmentVOC);
    }
    else
    {
      canvas.printf("IAQ %0.1f VOC %0.1f ppm", environmentIAQ, environmentVOC);
    }
  }
  else
  {
    #if defined(BME680_TEMP_F)
      canvas.printf("%0.1fF   %0.1f%%   %0.0f mbar", environmentTemperature * 9.0F / 5.0F + 32.0F, environmentHumidity, environmentPressure);
    #else
      canvas.printf("%0.1fC   %0.1f%%   %0.0f mbar", environmentTemperature, environmentHumidity, environmentPressure);
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

// Report any BME680 errors to the console
void logEnvironmentSensorErrors()
{
  if (bme680.status < BSEC_OK)
  {
      Serial.print("Environmentals: BSEC error code "); Serial.println(bme680.status);
  }
  else if (bme680.status > BSEC_OK)
  {
      Serial.print("Environmentals: BSEC warning code "); Serial.println(bme680.status);
  }
  if (bme680.sensor.status < BME68X_OK)
  {
      Serial.print("Environmentals: BME68X error code "); Serial.println(bme680.sensor.status);
  }
  else if (bme680.sensor.status > BME68X_OK)
  {
      Serial.print("Environmentals: BME68X warning code "); Serial.println(bme680.sensor.status);
  }
}

// Setup the BME680 sensor
void setupEnvironmentalSensor()
{
  Serial.println("Environmentals: Initializing BME680");
  
  // Initialize
  Wire.begin(); // Required for bme680.begin()
  if (!bme680.begin(BME680_ADDRESS, Wire))
  {
    logEnvironmentSensorErrors();
  }

  // Temperature correction
  bme680.setTemperatureOffset(BME680_TEMP_OFFSET);

  // Subscribe to the desired BSEC outputs
  if (!bme680.updateSubscription(environmentSensorList, ARRAY_LEN(environmentSensorList), BSEC_SAMPLE_RATE_CONT)) // Continuous sampling, about every second
  {
    logEnvironmentSensorErrors();
  }

  // The callback function will be responsible for receving BSEC data at the specified sample rate
  bme680.attachCallback(bme680Callback);

  // BSEC library information
  sprintf(environmentLibraryInformation, "%d.%d.%d.%d", bme680.version.major, bme680.version.minor, bme680.version.major_bugfix, bme680.version.minor_bugfix);
}

// Environmental data measurement callback for the BME680 BSEC library
void bme680Callback(const bme68xData data, const bsecOutputs outputs, Bsec2 bsec)
{
  float t, h, p, iaq, gasRaw, gasPercent, co2, voc;
  int iaqAccuracy, stabilized, runIn;

  if (outputs.nOutputs == ARRAY_LEN(environmentSensorList))
  {
    // Read all outputs
    for (int i = 0; i < outputs.nOutputs; i++)
    {
      const bsecData output  = outputs.output[i];
      switch (output.sensor_id)
      {
          case BSEC_OUTPUT_IAQ:
              iaq = output.signal; // 0 - 500 representing "clean" to "extremely polluted"
              iaqAccuracy = (int)output.accuracy; // 0 = unreliable, 1 = low, 2 = medium, 3 = high accuracy
              break;
          //case BSEC_OUTPUT_RAW_TEMPERATURE:
          //    break;
          case BSEC_OUTPUT_RAW_PRESSURE:
              p = output.signal; // mbar
              break;
          //case BSEC_OUTPUT_RAW_HUMIDITY:
          //    break;
          case BSEC_OUTPUT_RAW_GAS:
              gasRaw = output.signal; // Ohms
              break;
          case BSEC_OUTPUT_STABILIZATION_STATUS:
              stabilized = (int)output.signal; // 0 = ongoing stabilization, 1 = stabilized
              break;
          case BSEC_OUTPUT_RUN_IN_STATUS:
              runIn = (int)output.signal; // 0 = ongoing stabilization, 1 = stabilized
              break;
          case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
              t = output.signal; // Celsius
              break;
          case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
              h = output.signal; // RH%
              break;
          //case BSEC_OUTPUT_STATIC_IAQ:
          //    break;
          case BSEC_OUTPUT_CO2_EQUIVALENT:
              co2 = output.signal; // ppm
              break;
          case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
              voc = output.signal; // ppm
              break;
          case BSEC_OUTPUT_GAS_PERCENTAGE:
              gasPercent = output.signal; // Percentage of min and max filtered gas value
              break;
          //case BSEC_OUTPUT_COMPENSATED_GAS:
          //    break;
          //default:
          //    break;
      }
    }

    // If the sensor is stabilized then copy its readings into the global variables
    xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
    if (runIn && stabilized)
    {
      environmentSensorReady = true;
      if (environmentPressure == 0)
      {
        // Initialize
        environmentTemperature = t;
        environmentHumidity = h;
        environmentPressure = p;
      }
      else
      {
        // Smooth values with an EMA moving average
        environmentTemperature = (environmentTemperature * (MEASUREMENT_WINDOW - 1) + t) / MEASUREMENT_WINDOW;
        environmentHumidity    = (environmentHumidity    * (MEASUREMENT_WINDOW - 1) + h) / MEASUREMENT_WINDOW;
        environmentPressure    = (environmentPressure    * (MEASUREMENT_WINDOW - 1) + p) / MEASUREMENT_WINDOW;
      }
      environmentIAQAccuracy = iaqAccuracy; // This should not be smoothed
      if (iaqAccuracy > 0 && iaq != 50.0F)
      {
        if (environmentGasResistance == 0)
        {
          // Initialize
          environmentIAQ = iaq;
          environmentCO2 = co2;
          environmentVOC = voc;
          environmentGasResistance = gasRaw;
          environmentGasPercentage = gasPercent;
        }
        else
        {
          // Smooth values with an EMA moving average
          environmentIAQ = (environmentIAQ * (MEASUREMENT_WINDOW - 1) + iaq) / MEASUREMENT_WINDOW;
          environmentCO2 = (environmentCO2 * (MEASUREMENT_WINDOW - 1) + co2) / MEASUREMENT_WINDOW;
          environmentVOC = (environmentVOC * (MEASUREMENT_WINDOW - 1) + voc) / MEASUREMENT_WINDOW;
          environmentGasResistance = (environmentGasResistance * (MEASUREMENT_WINDOW - 1) + gasRaw) / MEASUREMENT_WINDOW;
          environmentGasPercentage = (environmentGasPercentage * (MEASUREMENT_WINDOW - 1) + gasPercent) / MEASUREMENT_WINDOW;
        }
      }

      // Dew point calculation using the Magnus formula. Reference: https://en.wikipedia.org/wiki/Dew_point#Calculating_the_dew_point
      // NOTE: This is effectively "smoothed" due to reliance on the smoothed temperature and humidity values
      float magnusGammaTRH = (float)log(environmentHumidity / 100.0F) + 17.625F * environmentTemperature / (243.04F + environmentTemperature);
      environmentDewPoint = 243.04F * magnusGammaTRH / (17.625F - magnusGammaTRH); // Always in C and converted to F on output if BME680_TEMP_F is true
    }
    else
    {
      environmentSensorReady = false;
      //Serial.printf("BME680: time = %lld, run in = %d, stabilized = %d", systemSeconds(), runIn, stabilized); Serial.println();
    }
    xSemaphoreGive(xMutexEnvironmental); // Done with environmental data
  }
}

//
void measureEnvironmentals(void *parameter)
{
  // Setup the BME680 sensor
  //delay(1000); // Non-blocking delay on ESP32, in milliseconds
  //setupEnvironmentalSensor();

  // Infinite loop since this is a separate task from the main thread
  while(true)
  {
    xSemaphoreTake(xMutexI2C, portMAX_DELAY); // Start accessing I2C devices
    bme680.run();
    xSemaphoreGive(xMutexI2C); // Done with I2C devices
    delay(7);
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
  xSemaphoreTake(xMutexI2C, portMAX_DELAY); // Start accessing I2C devices
  float lux = lightSensor.readLux(VEML_LUX_AUTO);
  uint8_t sensorGain = lightSensor.getGain();
  uint8_t sensorIntegrationTime = lightSensor.getIntegrationTime();
  xSemaphoreGive(xMutexI2C); // Done with I2C devices

  // Translate the gain and integration time values
  float gain; 
  switch (sensorGain)
  {
    case VEML7700_GAIN_2:   gain = 2;    break;
    case VEML7700_GAIN_1:   gain = 1;    break;
    case VEML7700_GAIN_1_4: gain = .25;  break;
    case VEML7700_GAIN_1_8: gain = .125; break;
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

  // Track peak levels
  int timeIndex = systemSeconds() % MEASUREMENT_WINDOW; // The time index rotates through the measurement window at the rate of one slot per second
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
  float minLux = lightSensorPeakLevels[0], peakLux = 0;
  for (int i = 0; i < MEASUREMENT_WINDOW; i++)
  {
    minLux  = min(minLux,  lightSensorPeakLevels[i]);
    peakLux = max(peakLux, lightSensorPeakLevels[i]);
  }

  // Detect sensor failure
  /*
  if (minLux == peakLux && timeIndex >= MEASUREMENT_WINDOW * 9 / 10) // When the sensor gets "stuck", the lux value goes constant, so the peak buffer should be full of the exact same values
  {
    Serial.println("Light: Sensor stalled");
    delay(250);
    ESP.restart(); // Soft system reset
  }
  */

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
  // Read battery voltage and percent charge
  xSemaphoreTake(xMutexI2C, portMAX_DELAY); // Start accessing I2C devices
  float v = max17048.cellVoltage();
  float p = max17048.cellPercent();
  xSemaphoreGive(xMutexI2C); // Done with I2C devices

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

// Each device is read in sequence so that a single task can be used to read all I2C data, and so the timing can be well-controlled
void readI2CDevices(void *parameter)
{
  unsigned long timer;

  // Infinite loop since this is a separate task from the main thread
  while (true)
  {
    if (millis() - timer >= 950)
    {
      // Only read these I2C devices approximately every second, but just SLIGHTLY faster to ensure that light level peak tracking gets enough hits
      measureLight();   // Measure the light level
      measureBattery(); // Read the LiPo battery voltage
      timer = millis(); // Reset the timer
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

  // Semaphore setup
  xMutexI2C           = xSemaphoreCreateMutex();
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

  // Read the BME680 data (does not work as a background task so it needs to be on the main thread)
  //xSemaphoreTake(xMutexI2C, portMAX_DELAY); // Start accessing I2C devices
  bme680.run();
  //xSemaphoreGive(xMutexI2C); // Done with I2C devices
  if (bme680.status < BSEC_OK || bme680.sensor.status < BME68X_OK)
  {
    logEnvironmentSensorErrors();
  }

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