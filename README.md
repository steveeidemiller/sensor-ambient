# Sensor: Ambient
This ESP32-S3 project is designed to monitor ambient temperature, humidity, barometric pressure, air quality, sound and light levels in areas such as attics, basements, garages, etc. It provides a rich data set that can be integrated into various home automation platforms through MQTT. It also offers a detailed web interface showing current sensor status and graphs of historical data. A telemetry endpoint is also available for use with observability platforms such as Prometheus.

## Features
- Code is designed to be used with the Arduino IDE
- BME680 environmental monitoring for temperature, humidity, atmospheric pressure and air quality
- SPH0645 I2S sound sensor for ambient sound level detection. The sensor does not record audio, it measures sound levels over time.
- VEML7700 I2C light sensor for ambient light level detection. The sensor is not a camera, it uses a single pixel to measure light levels over time.
- MQTT integration with TLS, user/pass and client certificate options
- NTP support for accurate system time which is also reported to MQTT for sensor online/offline detection
- HTTP status page with detailed sensor information, environmentals, system uptime tracking and historical charts
- HTTP metrics endpoint for use with telemetry systems such as Prometheus
- TFT display support that shows current data, sensor uptime and network address information
- AC power on/off sensing to detect power outages at the sensor location
- LiPo battery backup support to power the sensor through moderate power outages

## Screenshots

### Web Interface
[![Web Interface](assets/screenshot-web-interface-thumbnail.png)](assets/screenshot-web-interface.png)

### MQTT
![Web Interface](assets/screenshot-mqtt.png)

### Telemetry
![Web Interface](assets/screenshot-telemetry.png)<br/>
NOTE: This is only a sample of the complete telemetry available.

## Bill of Materials
This solution is based on the ESP32-S3 Reverse TFT Feather from Adafruit. The device integrates a TFT display, a LiPo battery power and charging cicuit, and Qwiic-compatible Stemma QT connectors.
- ESP32-S3 Reverse TFT Feather: [Adafruit #5691](https://www.adafruit.com/product/5691)
- BME680 module for collecting environmental data: [Adafruit #3660](https://www.adafruit.com/product/3660)
- SPH0645LM4H I2S MEMS microphone module: [Adafruit #3421](https://www.adafruit.com/product/3421)
- VEML7700 I2C light sensor module: [Adafruit #4162](https://www.adafruit.com/product/4162)
- Two Stemma QT cables to connect the ESP32 to the VEML7700 and BME280 modules (one 50mm and one 100mm): [Adafruit #4399 and #4210](https://www.adafruit.com/product/4399)
- LiPo battery with JST PH 2.0mm connector (WARNING: Many Amazon batteries do NOT have compatible polarity. Be sure to check the battery connector polarity to ensure that it matches the ESP32 connector!)
- Two 1% precision metal film resistors for AC power sensing: one 22k and one 12k
- Small amount of 24ga solid wire for soldering devices together
- 8 M2x4mm stainless screws for mounting devices to the 3D printed mount
- 8 M3x8mm or M3x10mm stainless screws for fasting the top part of the 3D printed mount to the bottom part

## Implementation Overview
- 3D print the included mount
- Assemble the circuit and mount it on the 3D printed base
- Clone this repository
    ```bash
    git clone https://github.com/steveeidemiller/sensor-ambient.git
    ```
- Copy the `config.example.h` configuration file to `config.h` and edit all values to suit the deployment scenario
- Using the Arduino IDE, upload the main sketch and configuration to the ESP32-S3 Feather: [Flashing instructions may be a little different from other ESP32 devices](https://learn.adafruit.com/esp32-s3-reverse-tft-feather/using-with-arduino-ide)
- Verify sensor function by using the HTTP status page

## Wiring
[![Wiring Diagram](assets/wiring-diagram-thumbnail.png)](assets/wiring-diagram.png)<br/>
The BME680 and VEML7700 can be connected with the two Stemma QT cables.

Headers are not needed for this project and if used may actually interfere with the 3D mount. So, there is no need to solder them on to the devices. Instead, simply solder wires directly into the holes. The ESP32 ground pin will need to be shared with one resistor leg and one wire to ground the SPH0645LM4H module. A pair of pliers can be used to slightly flatten the end of the resistor leg so that both wires will fit into the ESP32 hole.

The LiPo battery connector MUST be checked for polarity. Many LiPo batteries do not have the correct polarity for the ESP32 module used in this project. They often have the red and black reversed. The JST connectors are designed to allow removal and reinsertion of the pings, so changing the polarity should be straightforward without damaging the connector. It is recommended to plug the battery in last, once the ESP32 has been programmed.

## Configuration Notes
The project must be properly configured before use. First, copy the `config.example.h` file to a new file named `config.h` and edit all values to suit the deployment scenario per the notes listed here. There are a lot of settings, but default values can be used for many of them.

```cpp
// Network configuration
const char* WIFI_HOSTNAME = "Sensor-Ambient-1"; // Network hostname for the ESP32, such as "Sensor-Upper-Attic", and should be similar to MQTT_TOPIC_BASE below
const char* WIFI_SSID =     "Wifi Network";
const char* WIFI_PASSWORD = "Wifi Password";
```
All three of these values MUST be changed. `WIFI_HOSTNAME` must be a unique host name on your network, and should only use letters, numbers and hyphens. Your wireless network credentials must also be specified. 

```cpp
// NTP for system time
#define NTP_TIMEZONE "CST6CDT,M3.2.0,M11.1.0" // POSIX timezone string which can include daylight savings information if desired. Reference: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.json
#define NTP_SERVER_1 "us.pool.ntp.org"        // NTP pool server within an appropriate region. Reference: https://www.ntppool.org
#define NTP_SERVER_2 "time.nist.gov"          // Alternate NTP pool, or set to NTP_SERVER_1
#define NTP_SERVER_3 "192.168.1.1"            // Alternate NTP pool, or set to NTP_SERVER_1
```
The first value specifies your time zone. Simply select an appropriate value from the provided link. The first two NTP servers can be left as a default for the US. The third NTP server can be your network gateway if it has an NTP service, otherwise any other common NTP server such as `time.cloudflare.com` or `time.google.com` can be used.

```cpp
// TFT display configuration
#define TFT_SCREEN_WIDTH  240
#define TFT_SCREEN_HEIGHT 135
#define TFT_TIMEOUT       30    // After a button press, the display will be powered on for this many seconds before auto-shutoff
#define TFT_ROTATION      2     // 0 = 0, 2 = 180. Reference: https://learn.adafruit.com/adafruit-gfx-graphics-library/rotating-the-display
```
These settings can typically remain at default values. The timeout can be adjusted to suit your preferences. And the rotation can be changed depending on the orientation chosen for mounting the ESP32.

```cpp
// BME680 configuration
#define BME680_ADDRESS 0x77
#define BME680_TEMP_F true        // Set to false for Celsius
#define BME680_TEMP_OFFSET -2.00F // Celsius temperature offset for BME680. Adjust until temperature readings stabilize and match a known reference.
#define BME680_DONCHIAN_ENABLE               true     // Whether Donchian smoothing is enabled for IAQ
#define BME680_DONCHIAN_WINDOW               800      // Donchian smoothing period in number of I2C_INTERVAL polling cycles (default is 6 seconds X this value = smoothing period in seconds)
#define BME680_DONCHIAN_TEMP_RANGE_LIMIT     2.5F     // Donchian range limit for temperature (only used in IAQ calculation)
#define BME680_DONCHIAN_HUMIDITY_RANGE_LIMIT 3.5F     // Donchian range limit for humidity (only used in IAQ calculation)
#define BME680_DONCHIAN_GAS_RANGE_LIMIT      12500.0F // Donchian range limit for gas resistance (only used in IAQ calculation)
```
This block configures the BME680. `0x77` is the default I2C address for the Adafruit module. Fahrenheit is the default temperature scale for this project and can be changed to Celsius with `BME680_TEMP_F false`. Temperature offset will need to be adjusted experimentally, but `-2.00F` degrees Celsius is a good starting point. Donchian smoothing is explained in (the SE_BME680 library)[https://github.com/steveeidemiller/SE_BME680?tab=readme-ov-file#donchian-smoothing-optional], and the default values shown here can typically be used without modification for typical residential scenarios.

```cpp
// SPH0645 I2S sound sensor configuration
#define I2S_PORT_NUM        I2S_NUM_0         // Use I2S port number 0
#define I2S_LRCK_PIN        9                 // Left/Right Clock (Word Select)
#define I2S_BCLK_PIN        6                 // Bit Clock
#define I2S_DATA_IN_PIN     5                 // Data In (DOUT from SPH0645)
#define I2S_MCLK_PIN        I2S_PIN_NO_CHANGE // MCLK pin is not used for SPH0645
#define I2S_DMA_BUF_COUNT   8                 // Number of DMA buffers
#define I2S_DMA_BUF_LEN     256               // Size of each DMA buffer in samples
#define I2S_SAMPLE_RATE     16000             // Audio sample rate (Hz)
#define I2S_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_32BIT // SPH0645 outputs data in 32-bit frames (even if only 18-24 bits are valid)
#define I2S_NUM_CHANNELS    I2S_CHANNEL_FMT_ONLY_LEFT // SPH0645 is mono, usually on the left channel
#define SPL_FACTOR          17.5 // Factor to convert the range of the samples to dB SPL (Sound Pressure Level) using the formula: SPL = FACTOR * log10(max - min), where max and min are the maximum and minimum sample values, respectively
```
All of these values can be left at defaults. The `SPL_FACTOR` may need tweaking if the reported sound levels aren't between approximately 35dB and 115dB. Buffer length and sampling rate will mostly affect the frequency bandwidth and responsiveness, but only to a minor degree.

```cpp
// AC power sensing pin
#define AC_POWER_PIN 10 // Attached to the center of the 5V/3.3V resister divider such that the pin gets 3.3V when 5V power exists on the USB bus
```
This can be left at the default if you are following the included wiring diagram. If a different input pin is used, then this value must reflect that.

```cpp
// General measurement configuration
#define MEASUREMENT_WINDOW 3600 // Seconds. Measurements will have min/average/max values calculated over this time period.
```
This value affects the time "window" within which min/average/max values are calculated. Increasing this value will consume more memory, so pay close attention to "Free Heap Memory" on the web interface when increasing this value.

```cpp
// Max update delays. Outputs will be refreshed at least this often.
#define UPDATE_INTERVAL_MQTT    60
#define UPDATE_INTERVAL_TFT     1  // Must be an odd number due to the display toggling function
#define UPDATE_INTERVAL_DATA    60 // Number of seconds between data captures to the PSRAM buffer
```
These can all be left at defaults. This project has multiple outputs and this block configures how often some of those are updated. MQTT updates default to 60 seconds, the TFT display is updated every second, and historical charts on the web interface have data points at 60 second intervals.

```cpp
// HTML history charts (PSRAM data storage)
#define DATA_HISTORY_COUNT    2880 // Number of data elements to keep per stream, with one element per UPDATE_INTERVAL_DATA
```
This can be left at its default. It controls the total amount of time "range" for the web charts. Multiply `UPDATE_INTERVAL_DATA` X `DATA_HISTORY_COUNT` to get the total range in seconds. It is suggested that 24-48 hours be used as a starting point. This value is limited only by the available PSRAM on the ESP32.

```cpp
// MQTT configuration
const char* MQTT_SERVER   = "192.168.1.60"; // MQTT server name or IP
const int   MQTT_PORT     = 8883;           // 1883 is the default port for MQTT, 8883 is the default for MQTTS (TLS)
const char* MQTT_USER     = "MQTT user";    // Null if no authentication is required
const char* MQTT_PASSWORD = "MQTT pass";    // Null if no authentication is required
#define     MQTT_TOPIC_BASE "home/sensors/ambient_1/" // Base topic string for all values from this sensor

// Certificate Authority for TLS connections
static const char CERT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
Your CA cert
-----END CERTIFICATE-----
)EOF";
```
This block configures MQTT access.

## Software Installation
TODO

### Library Dependencies
TODO

### Uploading
TODO

## 3D Printed Mount
TODO
NOTE: may not work well with the ESP variant using a w.FL antenna

# License
This project is licensed under the [MIT License](LICENSE).
