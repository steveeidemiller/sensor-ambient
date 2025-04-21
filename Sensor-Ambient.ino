/**
 * @file Sensor-Ambient.ino
 * @brief ESP32-based sound, light and environmental sensor with MQTT, TFT display, and web server support
 * 
 * This ESP32-S3 project is designed to monitor ambient sound, light, and environmentals in areas such as attics,
 * basements, garages, etc. It provides calibrated readings and can be integrated into various home automation
 * platforms through MQTT.
 *
 * Features:
 * - SPH0645 I2S sound sensor for ambient sound level detection
 * - VEML7700 I2C light sensor for ambient light level detection
 * - Environmental monitoring using a BME280 sensor
 * - System uptime tracking
 * - MQTT integration for remote telemetry
 * - HTTP web server with a root page and Prometheus metrics endpoint
 * - TFT display for local visualization of data
 * - LiPo battery voltage monitoring
 */

//TODO: Consider converting uptimeSecondsTotal to milliseconds
//TODO: Add remote IP restrictions as an array of IP's for NGINX. Can the web server abort a connection?
//TODO: Allow MQTT to be done without cert and without user/pass, allow MQTT private key auth via define switch with client cert+key, update HTML page with mqtt:// or mqtts://

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
#include <Fonts/FreeSans12pt7b.h> // TFT display support
#include <Fonts/FreeSans9pt7b.h>  // TFT display support
#include <Adafruit_Sensor.h>      // BME280 support
#include <Adafruit_BME280.h>      // BME280 support
#include <hal/efuse_hal.h>        // Espressif ESP32 chip information

// The configuration references objects in the above libraries, so include it after those
#include <config.h>

// ESP32
char chipInformation[100]; // Chip information buffer

// Web server configuration
WebServer webServer(80);
char webStringBuffer[4096];

// MQTT configuration
//WiFiClient espClient;
WiFiClientSecure espClient; // Use WiFiClientSecure for SSL/TLS MQTT connections
PubSubClient mqttClient(espClient);
unsigned long mqttLastConnectionAttempt = 0;
char mqttStringBuffer[20];

// TFT display configuration
//char tftStringBuffer[20];
Adafruit_ST7789 display = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(TFT_SCREEN_WIDTH, TFT_SCREEN_HEIGHT);

// BME280 configuration
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
char stringBufferUptime[24];

// LiPo battery
SemaphoreHandle_t xMutexBattery; // Mutex to protect shared variables between tasks
Adafruit_MAX17048 max17048;
float batteryVoltage;

// SPH0645 I2S sound sensor
SemaphoreHandle_t xMutexSoundSensor; // Mutex to protect shared variables between tasks
int32_t soundSensorDataBuffer[I2S_DMA_BUF_LEN]; // I2S read buffer
float soundSensorSplAverage = 0; // Average sound level in dB SPL
float soundSensorSplPeak = 0; // Peak sound level in dB SPL

// VML7700 light sensor
Adafruit_VEML7700 lightSensor = Adafruit_VEML7700();
SemaphoreHandle_t xMutexLightSensor; // Mutex to protect shared variables between tasks
float lightSensorLuxAverage = 0; // Average light level in Lux
float lightSensorLuxPeak = 0; // Peak light level in Lux
float lightSensorGain = VEML7700_GAIN_1X; // Gain setting for the light sensor, read/updated from the AGC
int lightSensorIntegrationTime = VEML7700_IT_100MS; // Integration time setting for the light sensor, read/updated from the AGC

// Main loop
uint64_t lastUpdateTimeMqtt = 0; // Time of last MQTT update
uint64_t lastUpdateTimeTft = 0; // Time of last OLED update
uint64_t timer = 0; // Copy of the main uptime timer that doesn't need a semaphore

// Static HTML template
const char htmlHeader[] = R"EOF(
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <meta http-equiv="X-UA-Compatible" content="ie=edge">
    <meta http-equiv="refresh" content="60">
    <title>ESP32 Sensor - %s</title>
    <style>
      table.sensor {
        border-top: solid 1px black;
        border-left: solid 1px black;
      }
      table.sensor th, table.sensor td {
        border-right: solid 1px black;
        border-bottom: solid 1px black;
      }
      table.sensor th.header {
        background-color: #0000CC;
        color: white;
        font-size: 24px;
        font-weight: bold;
        padding: 5px 10px;
        text-align: center;
      }
      table.sensor th {
        text-align: left;
        padding-right: 50px;
      }
      table.sensor td {
        padding-left: 50px;
        text-align: right;
      }
      table.sensor tr.environmental {
        background-color: #E0E0FF;
      }
      table.sensor tr.system {
        background-color: #C0C0FF;
      }
      table.sensor tr.chip {
        background-color: #A0A0FF;
      }
      a, a:visited {
        color: blue;
      }
      a:hover {
        color: purple;
      }
    </style>
  </head>
  <body>
)EOF";
const char htmlFooter[] = R"EOF(
  </body>
</html>
)EOF";

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
  buffer += bytesAdded(sprintf(buffer, htmlHeader, WIFI_HOSTNAME)); // Hostname gets added to the HTML <title> inside the template header
  buffer += buffercat(buffer, "<table class=\"sensor\" cellspacing=\"0\" cellpadding=\"3\">"); // Sensor data table
  buffer += bytesAdded(sprintf(buffer, "<tr><th colspan=\"2\" class=\"header\">%s</th></tr>", WIFI_HOSTNAME)); // Network hostname
  xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing sound data (measured on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr><th>Sound Pressure Level (average)</th><td>%0.2f dB</td></tr>", soundSensorSplAverage)); // Average sound level in dB
  buffer += bytesAdded(sprintf(buffer, "<tr><th>Sound Pressure Level (peak)</th><td>%0.2f dB</td></tr>", soundSensorSplPeak)); // Peak sound level in dB
  xSemaphoreGive(xMutexSoundSensor); // Done with sound data
  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing light data (measured on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr><th>Light Level (average)</th><td>%0.2f lux</td></tr>", lightSensorLuxAverage)); // Average light level in lux
  buffer += bytesAdded(sprintf(buffer, "<tr><th>Light Level (peak)</th><td>%0.2f lux</td></tr>", lightSensorLuxPeak)); // Peak light level in lux
  buffer += bytesAdded(sprintf(buffer, "<tr><th>Light Measurement Gain</th><td>%0.3f</td></tr>", lightSensorGain)); // Measurement gain
  buffer += bytesAdded(sprintf(buffer, "<tr><th>Light Measurement Integration Time</th><td>%d</td></tr>", lightSensorIntegrationTime)); // Measurement integration time
  xSemaphoreGive(xMutexLightSensor); // Done with light data
  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data (calculated on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Sensor OK?</th><td>%d</td></tr>", (int)environmentSensorOK)); // Environment sensor OK?
  #if defined(BME280_TEMP_F)
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Temperature</th><td>%0.1f&deg; F</td></tr>", environmentTemperature)); // Environment temperature
  #else
    buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Temperature</th><td>%0.1f&deg; C</td></tr>", environmentTemperature)); // Environment temperature
  #endif
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Humidity</th><td>%0.1f%%</td></tr>", environmentHumidity)); // Environment humidiy
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"environmental\"><th>Environment Barometric Pressure</th><td>%0.1f mbar</td></tr>", environmentPressure)); // Environment barometric pressure
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data
  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data (calculated on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>Uptime</th><td>%s</td></tr>", stringBufferUptime)); // Formatted uptime
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"system\"><th>Uptime Seconds</th><td>%lld</td></tr>", uptimeSecondsTotal)); // Raw uptime in seconds
  xSemaphoreGive(xMutexUptime); // Done with uptime data
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Free Heap</th><td>%d bytes</td></tr>", ESP.getFreeHeap())); // ESP32 free heap memory, which indicates if the program still has enough memory to run effectively
  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data (calculated on a different thread)
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>Battery Voltage</th><td>%0.2f V</td></tr>", batteryVoltage)); // LiPo battery
  xSemaphoreGive(xMutexBattery); // Done with battery data
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>MQTT Server</th><td>%s mqtts://%s:%d</td></tr>", mqttClient.connected() ? "Connected to" : "Disconnected from ", MQTT_SERVER, MQTT_PORT)); // MQTT connection
  buffer += bytesAdded(sprintf(buffer, "<tr class=\"chip\"><th>IP Address</th><td>%d.%d.%d.%d</td></tr>", ip[0], ip[1], ip[2], ip[3])); // Network address
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
/*
  xSemaphoreTake(xMutexSensor, portMAX_DELAY); // Start accessing sensor data (measured on a different thread)

  // AC voltage
  buffer += buffercat(buffer, "# HELP sensor_ac_voltage Measured AC voltage\n");
  buffer += buffercat(buffer, "# TYPE sensor_ac_voltage gauge\n");
  buffer += bytesAdded(sprintf(buffer, "sensor_ac_voltage{measurement=\"RMS\"} %0.1f\n\n", sensorRmsVoltage));

  // AC power state based on the measured AC voltage
  buffer += buffercat(buffer, "# HELP sensor_ac_power_state AC power on/off state based on the measured AC voltage\n");
  buffer += buffercat(buffer, "# TYPE sensor_ac_power_state gauge\n");
  buffer += bytesAdded(sprintf(buffer, "sensor_ac_power_state %d\n\n", sensorRmsVoltage >= RMS_POWER_OFF_THRESHOLD ? 1 : 0));

  // Add up how many total seconds that the AC power was off by scanning the entire tracking array
  int secondsOff = 0;
  for (int i = 0; i < TELEMETRY_SCRAPE_WINDOW; i++)
  {
    secondsOff += sensorPowerOutageSeconds[i];
  }

  // AC power off seconds measured within the scrape window
  buffer += buffercat(buffer, "# HELP sensor_ac_power_off_seconds How much time the AC power was off during the sensor_ac_power_off_seconds_window monitoring window\n");
  buffer += buffercat(buffer, "# TYPE sensor_ac_power_off_seconds gauge\n");
  buffer += bytesAdded(sprintf(buffer, "sensor_ac_power_off_seconds %d\n\n", secondsOff));

  xSemaphoreGive(xMutexSensor); // Done with sensor data

  // The scrape window in seconds
  buffer += buffercat(buffer, "# HELP sensor_ac_power_off_seconds_window Size of the monitoring window (in seconds) for power outage counts measured in sensor_ac_power_off_seconds\n");
  buffer += buffercat(buffer, "# TYPE sensor_ac_power_off_seconds_window gauge\n");
  buffer += bytesAdded(sprintf(buffer, "sensor_ac_power_off_seconds_window %d\n\n", TELEMETRY_SCRAPE_WINDOW));
*/
  // Environmentals
  xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data (calculated on a different thread)
  if (environmentSensorOK)
  {
    #if defined(BME280_TEMP_F)
      buffer += buffercat(buffer, "# HELP environmental_temperature Environment temperature (Fahrenheit)\n");
    #else
      buffer += buffercat(buffer, "# HELP environmental_temperature Environment temperature (Celsius)\n");
    #endif
    buffer += buffercat(buffer, "# TYPE environmental_temperature gauge\n");
    buffer += bytesAdded(sprintf(buffer, "environmental_temperature %0.1f\n\n", environmentTemperature));
    buffer += buffercat(buffer, "# HELP environmental_humidity Environment humidity (RH%)\n");
    buffer += buffercat(buffer, "# TYPE environmental_humidity gauge\n");
    buffer += bytesAdded(sprintf(buffer, "environmental_humidity %0.1f\n\n", environmentHumidity));
    buffer += buffercat(buffer, "# HELP environmental_pressure Environment barometric pressure (mbar)\n");
    buffer += buffercat(buffer, "# TYPE environmental_pressure gauge\n");
    buffer += bytesAdded(sprintf(buffer, "environmental_pressure %0.1f\n\n", environmentPressure));
  }
  xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

  // Heap memory
  buffer += buffercat(buffer, "# HELP esp32_free_heap_bytes ESP32 free heap memory\n");
  buffer += buffercat(buffer, "# TYPE esp32_free_heap_bytes gauge\n");
  buffer += bytesAdded(sprintf(buffer, "esp32_free_heap_bytes %d\n\n", ESP.getFreeHeap()));

  // Battery voltage
  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data
  buffer += buffercat(buffer, "# HELP esp32_battery_voltage ESP32 LiPo battery voltage\n");
  buffer += buffercat(buffer, "# TYPE esp32_battery_voltage gauge\n");
  buffer += bytesAdded(sprintf(buffer, "esp32_battery_voltage %0.2f\n\n", batteryVoltage));
  xSemaphoreGive(xMutexBattery); // Done with battery data

  // Chip information
  buffer += buffercat(buffer, "# HELP esp32_chip_info ESP32 chip information\n");
  buffer += buffercat(buffer, "# TYPE esp32_chip_info gauge\n");
  buffer += bytesAdded(sprintf(buffer, "esp32_chip_info{version=\"%s\"} 1\n\n", chipInformation));

  // Last line must end with a line feed character
  buffer += buffercat(buffer, "\n");

  // Send the HTML response to the client
  webServer.send(200, "text/plain", webStringBuffer);
}

// Web server setup 
void setupWebserver()
{
  Serial.println("Web: Starting server");

  // Set URI handlers
  webServer.on("/", webHandlerRoot);
  webServer.on("/metrics", webHandlerMetrics);
  webServer.onNotFound(webHandler404);

  // Start the server
  webServer.begin();
}

// Setup MQTT connection
void setupMQTT()
{
  espClient.setCACert(CERT_CA);
  //espClient.setCertificate(const char *client_ca); // Client certificate
  //espClient.setPrivateKey(const char *private_key); // Client certificate key
  //espClient.setInsecure(); //TODO: Add switch to allow for testing
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
}

// Connect/Reconnect to the MQTT server
void mqttConnect()
{
  // Wait a few seconds between connection attempts
  if (mqttLastConnectionAttempt == 0 || millis() - mqttLastConnectionAttempt >= 5000)
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

// TFT ST7789 display setup
void setupTFT()
{
  // Initialize the display
  Serial.println("TFT: Configuring display");
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  display.init(TFT_SCREEN_HEIGHT, TFT_SCREEN_WIDTH);
  
  // Setup defaults
  display.setRotation(3);
  canvas.setFont(&FreeSans12pt7b);
  canvas.setTextColor(ST77XX_WHITE); // White text

  // Allow the display time to initialize after powering on
  delay(100);
}

// Update the TFT display
void updateDisplay(float temperature, float humidity, float pressure, float battery)
{
  IPAddress ip = WiFi.localIP();
  int16_t  x1, y1;
  uint16_t w, h;
/*
  // Start with a clear buffer
  oled.clearDisplay();

  // Render the sensor RMS voltage in a large font on the top of the display
  oled.setFont(&FreeSans18pt7b); // Larger font for the voltage
  sprintf(oledStringBuffer, "%0.1f V", voltage);
  oled.getTextBounds(oledStringBuffer, 0, 0, &x1, &y1, &w, &h); // Calculate the size of the voltage string
  oled.setCursor((OLED_SCREEN_WIDTH - w) / 2, h - 2);
  oled.print(oledStringBuffer);

  // Default font (smaller than the voltage font) for environmentals and the IP address
  oled.setFont();

  // Render system uptime and battery data, toggling between the two every other second
  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data
  if (uptimeSeconds % 2)
  {
    // Show the system uptime in days/hours/minutes/seconds format
    sprintf(oledStringBuffer, "%s", stringBufferUptime);
  }
  else
  {
    // Show the LiPo battery voltage
    sprintf(oledStringBuffer, "Battery: %0.2f V", battery);
  }
  xSemaphoreGive(xMutexUptime); // Done with uptime data
  oled.getTextBounds(oledStringBuffer, 0, 0, &x1, &y1, &w, &h); // Calculate the size of the string
  oled.setCursor((OLED_SCREEN_WIDTH - w) / 2, OLED_SCREEN_HEIGHT - 29);
  oled.print(oledStringBuffer);

  // Render environmental data
  if (environmentOK)
  {
    #if defined(BME280_TEMP_F)
      sprintf(oledStringBuffer, "%0.0fF %0.0f%% %0.0f mbar", temperature, humidity, pressure);
    #else
      sprintf(oledStringBuffer, "%0.0fC %0.0f%% %0.0f mbar", temperature, humidity, pressure);
    #endif
  }
  else
  {
    sprintf(oledStringBuffer, "%s", "BME280 offline");
  }
  oled.getTextBounds(oledStringBuffer, 0, 0, &x1, &y1, &w, &h); // Calculate the size of the string
  oled.setCursor((OLED_SCREEN_WIDTH - w) / 2, OLED_SCREEN_HEIGHT - 18);
  oled.print(oledStringBuffer);

  // Render the IP address
  sprintf(oledStringBuffer, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  oled.getTextBounds(oledStringBuffer, 0, 0, &x1, &y1, &w, &h); // Calculate the size of the string
  oled.setCursor((OLED_SCREEN_WIDTH - w) / 2, OLED_SCREEN_HEIGHT - 7);
  oled.print(oledStringBuffer);

  // Show the rendered buffer
  oled.display();
  */
}

// Setup the environmental sensor
void setupEnvironmentalSensor()
{
  Serial.println("BME280: Initializing");
  delay(100); // Allow the sensor time to initialize after powering on
  if (!bme280.begin(BME280_ADDRESS))
  {
    Serial.println("BME280: Configuration failed");
  }
}

// Environmental data measurement
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
      Serial.println("BME280: NaN detected - Resetting sensor");
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
        // Smooth values with a 20-sample moving average
        environmentTemperature = (environmentTemperature * 19 + t) / 20;
        environmentHumidity = (environmentHumidity * 19 + h) / 20;
        environmentPressure = (environmentPressure * 19 + p) / 20;
      }
      environmentSensorOK = true;
      xSemaphoreGive(xMutexEnvironmental); // Done with environmental data
    }
  }
  else
  {
    // Reset the BME280
    Serial.printf("BME280 data error: t=%0.1f h=%0.1f p=%0.1f", t, h, p); Serial.println();
    setupEnvironmentalSensor();
    xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing the environmental data
    environmentSensorOK = false;
    xSemaphoreGive(xMutexEnvironmental); // Done with environmental data
  }
}

// Light level measurement (background task)
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

  // Update light sensor data values
  xSemaphoreTake(xMutexLightSensor, portMAX_DELAY); // Start accessing the light sensor data
  lightSensorLuxAverage = (lightSensorLuxAverage * 19 + lux) / 20; // 20-sample moving average to smooth the lux reading
  //lightSensorLuxPeak = max(lux, lightSensorLuxPeak); // Peak lux value
  lightSensorGain = gain;
  lightSensorIntegrationTime = integrationTime;
  xSemaphoreGive(xMutexLightSensor); // Done with light sensor data
}

// Read the battery voltage using the MAX17048 LiPo battery monitor
void measureBattery()
{
  xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data
  batteryVoltage = (batteryVoltage * 19 + max17048.cellVoltage()) / 20; // 20-sample moving average to smooth the voltage reading
  xSemaphoreGive(xMutexBattery); // Done with battery data
}

// Read all I2C devices in sequence (background task)
void readI2CDevices(void *parameter)
{
  // Infinite loop since this is a separate task from the main thread
  while (true)
  {
    // Read the I2C devices in sequence
    measureEnvironmentals(); // Read the BME280 sensor data
    measureLight(); // Measure the light level
    measureBattery(); // Read the LiPo battery voltage

    // Non-blocking delay on ESP32, in milliseconds
    delay(950);
  }
}

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
//TODO: semaphore, 20 period EMA on sound level, etc.
void measureSound(void *parameter)
{
  // Infinite loop since this is a separate task from the main thread
  while (true)
  {
    // Read the I2S data into the buffer
    size_t bytesRead = 0;
    esp_err_t result = i2s_read(I2S_PORT_NUM,        // I2S port number
                                soundSensorDataBuffer,     // Buffer to store data
                                I2S_DMA_BUF_LEN * sizeof(int32_t), // Max bytes to read (buffer size)
                                &bytesRead,          // Number of bytes actually read
                                portMAX_DELAY);      // Wait indefinitely for data
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
          // NOTE: The 32-bit sample is unsigned, which MUST be cast to unsigned before shifting bits to avoid the sign bit carrying over to the upper bits
          int32_t s = ((unsigned int)soundSensorDataBuffer[i]) >> (32 - 18);

          // Calculate the rage of the samples
          if (s < minValue || minValue == 0)
          {
            if (s != 0) minValue = s; // Ignore zero values for the min value after initialization
          }
          if (s > maxValue) maxValue = s;
        }

        // Rough calculation of the sound pressure level (SPL) in dB based on the measured range of the samples
        float spl = SPL_FACTOR * log10(maxValue - minValue);

        // Update the sound level values
        xSemaphoreTake(xMutexSoundSensor, portMAX_DELAY); // Start accessing the sound sensor data
        soundSensorSplAverage = (soundSensorSplAverage * 19 + spl) / 20; // 20-sample moving average to smooth the SPL reading
        //soundSensorSplPeak = max(spl, soundSensorSplPeak); // Peak SPL value
        xsemaphoreGive(xMutexSoundSensor); // Done with sound sensor data
      }
    }

    //
    delay(10); // Non-blocking delay on ESP32, in milliseconds
  }
}

// Main setup
void setup()
{
  // Serial port for debugging purposes
  Serial.begin(115200);

  // Core features
  setupWiFi();
  setupMDNS();
  setupWebserver();
  setupMQTT();
  setupTFT();
  setupEnvironmentalSensor();
  max17048.begin();
  lightSensor.begin();
  xMutexEnvironmental = xSemaphoreCreateMutex();
  xMutexUptime        = xSemaphoreCreateMutex();
  xMutexBattery       = xSemaphoreCreateMutex();
  xMutexSoundSensor   = xSemaphoreCreateMutex();
  xMutexLightSensor   = xSemaphoreCreateMutex();

  // Chip information
  sprintf(chipInformation, "%s %s (revison v%d.%d), %dMHz", ESP.getChipModel(), ESP.getCoreVersion(), efuse_hal_get_major_chip_version(), efuse_hal_get_minor_chip_version(), ESP.getCpuFreqMHz());

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

// Main loop
void loop()
{
  // MQTT connection management
  if (!mqttClient.connected())
  {
    mqttConnect();
  }
  else
  {
    mqttClient.loop();
  }

  // Web server request management
  webServer.handleClient();

  // Uptime calculations: How long has the ESP32 been running since it was booted up?
  xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data (calculated on a different thread)
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
    sprintf(stringBufferUptime, "%lldd %lldh %lldm %llds", uptimeDays, uptimeHours, uptimeMinutes, uptimeSeconds);
    lastUptimeSecondsTotal = uptimeSecondsTotal;
  }
  timer = uptimeSecondsTotal; // Copy the timer so it can be used without the semaphore
  xSemaphoreGive(xMutexUptime); // Done with uptime data

  // Update outputs every specified update interval, and the first-time through the loop
  bool updateMqtt = timer - lastUpdateTimeMqtt >= UPDATE_INTERVAL_MQTT;
  bool updateTft  = timer - lastUpdateTimeTft  >= UPDATE_INTERVAL_TFT; // The display can be updated more frequently than MQTT
  if (updateMqtt || updateTft || lastUpdateTimeMqtt == 0 || lastUpdateTimeTft == 0)
  {
    // Quickly copy the environmentals so the semaphore isn't held during time consuming operations
    xSemaphoreTake(xMutexEnvironmental, portMAX_DELAY); // Start accessing environmental data (measured on a different thread)
    float environmentOK = environmentSensorOK;
    float temperature = environmentTemperature;
    float humidity = environmentHumidity;
    float pressure = environmentPressure;
    xSemaphoreGive(xMutexEnvironmental); // Done with environmental data

    // Quickly copy the battery data so the semaphore isn't held during time consuming operations
    xSemaphoreTake(xMutexBattery, portMAX_DELAY); // Start accessing the battery data (calculated on a different thread)
    float battery = batteryVoltage;
    xSemaphoreGive(xMutexBattery); // Done with battery data

    // Update the TFT display
    if (updateTft)
    {
      lastUpdateTimeTft = timer;
      updateDisplay(temperature, humidity, pressure, battery);
    }

    // Update MQTT
    if (updateMqtt)
    {
      lastUpdateTimeMqtt = timer;

      // Send sound level information to MQTT
      sprintf(mqttStringBuffer, "%0.2f dB", soundSensorSplAverage);
      mqttClient.publish(MQTT_TOPIC_SPL_AVERAGE, mqttStringBuffer, true);
      sprintf(mqttStringBuffer, "%0.2f dB", soundSensorSplPeak);
      mqttClient.publish(MQTT_TOPIC_SPL_PEAK, mqttStringBuffer, true);

      // Send light level information to MQTT
      sprintf(mqttStringBuffer, "%0.2f lux", lightSensorLuxAverage);
      mqttClient.publish(MQTT_TOPIC_LUX_AVERAGE, mqttStringBuffer, true);
      sprintf(mqttStringBuffer, "%0.2f lux", lightSensorLuxPeak);
      mqttClient.publish(MQTT_TOPIC_LUX_PEAK, mqttStringBuffer, true);
      sprintf(mqttStringBuffer, "%0.3f", lightSensorGain);
      mqttClient.publish(MQTT_TOPIC_LUX_GAIN, mqttStringBuffer, true);
      sprintf(mqttStringBuffer, "%d ms", lightSensorIntegrationTime);
      mqttClient.publish(MQTT_TOPIC_LUX_INTEGRATION_TIME, mqttStringBuffer, true);

      // Send uptime information to MQTT
      xSemaphoreTake(xMutexUptime, portMAX_DELAY); // Start accessing the uptime data (calculated on a different thread)
      mqttClient.publish(MQTT_TOPIC_UPTIME, stringBufferUptime, true);
      sprintf(mqttStringBuffer, "%lld", timer);
      mqttClient.publish(MQTT_TOPIC_UPTIME_SECONDS, mqttStringBuffer, true);
      xSemaphoreGive(xMutexUptime); // Done with uptime data

      // Send the environmentals to MQTT
      if (environmentOK)
      {
        #if defined(BME280_TEMP_F)
          sprintf(mqttStringBuffer, "%0.1f F", temperature);
        #else
          sprintf(mqttStringBuffer, "%0.1f C", temperature);
        #endif
        mqttClient.publish(MQTT_TOPIC_TEMPERATURE, mqttStringBuffer, true);
        sprintf(mqttStringBuffer, "%0.1f%%", humidity);
        mqttClient.publish(MQTT_TOPIC_HUMIDITY, mqttStringBuffer, true);
        sprintf(mqttStringBuffer, "%0.1f mbar", pressure);
        mqttClient.publish(MQTT_TOPIC_PRESSURE, mqttStringBuffer, true);
      }

      // Send free heap size to MQTT
      sprintf(mqttStringBuffer, "%d", ESP.getFreeHeap());
      mqttClient.publish(MQTT_TOPIC_FREE_HEAP, mqttStringBuffer, true);

      // Send battery voltage to MQTT
      sprintf(mqttStringBuffer, "%0.2f V", battery);
      mqttClient.publish(MQTT_TOPIC_BATTERY_VOLTAGE, mqttStringBuffer, true);

      // Send chip info to MQTT
      mqttClient.publish(MQTT_TOPIC_CHIP_INFO, chipInformation, true);
    }
  }

  // Yield to other tasks, and slow down the main loop a little
  delay(40); // Non-blocking delay on ESP32, in milliseconds
}
