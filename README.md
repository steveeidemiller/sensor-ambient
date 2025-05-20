# Sensor: Ambient
This ESP32-S3 project is designed to monitor ambient sound, light, and environmentals in areas such as attics, basements, garages, etc. It provides a rich data set of calibrated readings that can be integrated into various home automation platforms through MQTT.

## Features
- Code is designed to be used with the Arduino IDE
- SPH0645 I2S sound sensor for ambient sound level detection. The sensor does not record audio, it measures sound levels over time.
- VEML7700 I2C light sensor for ambient light level detection. The sensor is not a camera, it uses a single pixel to measure light levels over time.
- BME680 environmental monitoring for temperature, humidity, atmospheric pressure and air quality
- MQTT integration with TLS, user/pass and client certificate options
- NTP support for accurate system time which is also reported to MQTT for sensor online/offline detection
- HTTP status page with detailed sensor information, environmentals, system uptime tracking and historical charts
- HTTP metrics endpoint for use with telemetry systems such as Prometheus
- TFT display support that shows current data, sensor uptime and network address information
- AC power on/off sensing to detect power outages at the sensor location
- LiPo battery backup support to power the sensor through moderate power outages

## Bill of Materials
This solution is based on the ESP32-S3 Reverse TFT Feather from Adafruit. The device integrates a TFT display, a LiPo battery power and charging cicuit, and Stemma QT connectors.
- ESP32-S3 Reverse TFT Feather: [Adafruit #5691](https://www.adafruit.com/product/5691)
- SPH0645LM4H I2S MEMS microphone module: [Adafruit #3421](https://www.adafruit.com/product/3421)
- VEML7700 I2C light sensor module: [Adafruit #4162](https://www.adafruit.com/product/4162)
- BME680 module for collecting environmental data: [Adafruit #3660](https://www.adafruit.com/product/3660)
- Two Stemma QT cables to connect the ESP32 to the VEML7700 and BME280 modules: [Adafruit #4399 or #4210](https://www.adafruit.com/product/4399)
- LiPo battery with JST PH 2.0mm connector (WARNING: Many Amazon batteries do NOT have compatible polarity. Be sure to check the battery connector polarity to ensure that it matches the ESP32 connector!)
- Two 1% precision metal film resistors for AC power sensing: 22k and 12k

## Implementation Overview
- 3D print the included device mount if desired
- Assemble the circuit and mount it on the 3D printed base
- Clone this repository
    ```bash
    git clone https://github.com/steveeidemiller/sensor-ambient.git
    ```
- Copy the `config.example.h` configuration file to `config.h` and edit all values to suit the deployment scenario
- Using the Arduino IDE, upload the main sketch and configuration to the ESP32-S3 Feather: [Flashing instructions may be a little different from other ESP32 devices](https://learn.adafruit.com/esp32-s3-reverse-tft-feather/using-with-arduino-ide)
- Verify sensor function by using the HTTP status page

## License
This project is licensed under the [MIT License](LICENSE).
