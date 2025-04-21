# Sensor: Ambient
This ESP32-S3 project is designed to monitor ambient sound, light, and environmentals in areas such as attics, basements, garages, etc. It provides calibrated readings and can be integrated into various home automation platforms through MQTT.

## Features
- Code is designed to be used with the Arduino IDE
- Sound level monitoring. The sensor does not record audio, it measures aggregate sound levels over time.
- Light level monitoring. The sensor is not a camera, it uses a single pixel to measure light levels over time.
- Environmental monitoring for temperature, humidity and atmospheric pressure
- Secure MQTT integration with TLS, user/pass and client certificate options
- HTTP status page with detailed sensor information, environmentals and uptime tracking
- HTTP metrics endpoint for use with telemetry systems such as Prometheus
- TFT display support that shows current data, sensor uptime and network address information
- LiPo battery backup support to power the sensor through moderate power outages

## Bill of Materials
This solution is based on the ESP32-S3 Reverse TFT Feather from Adafruit. The device integrates a TFT display, a LiPo battery power and charging cicuit, and Stemma QT connectors (compatible with Qwiic).
- ESP32-S3 Reverse TFT Feather: [Adafruit #5691](https://www.adafruit.com/product/5691)
- SPH0645LM4H I2S MEMS microphone module: [Adafruit #3421](https://www.adafruit.com/product/3421)
- VEML7700 I2C light sensor module: [Adafruit #4162](https://www.adafruit.com/product/4162)
- BME280 module for collecting environmental data [Adafruit #2652](https://www.adafruit.com/product/2652)
- Two Stemma QT (or Qwiic) cables to connect the ESP32 to the VEML7700 and BME280 modules
- LiPo battery (WARNING: Be sure to check the battery cable polarity to ensure that it matches the ESP32 connector!)

## Implementation Overview
- 3D print the included device mount if desired
- Assemble the circuit and mount it on the 3D printed base
- Clone this repository
    ```bash
    git clone https://github.com/steveeidemiller/sensor-ambient.git
    ```
- TODO: ??? Use the calibration sketch to gather configuration values for the main sketch
- Copy the example configuration file in the main sketch and edit all values to suit the deployment scenario
- Using the Arduino IDE, upload the main sketch and configuration to the ESP32-S3 Feather
- Verify sensor function by using the HTTP status page and/or viewing the MQTT feed

## TODO
- Document the enclosure and link the above overview bullet to that sub-readme
- Add section: Circuit assembly, with diagrams and photos
- Add section: Calibration, with cal sketch and photos
- Add section: Configuration, with details about each of the options
- Add section: Upload to ESP32, with notes on selecting the board type, setting libraries, etc.
- Add section: Using the sensor, with screenshots of the web page, MQTT, Prometheus, etc.

## License
This project is licensed under the [MIT License](LICENSE).
