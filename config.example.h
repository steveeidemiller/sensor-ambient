// Network configuration
const char* WIFI_HOSTNAME = "Sensor-AC-Voltage"; // Network hostname for the ESP32
const char* WIFI_SSID =     "Wifi Network";
const char* WIFI_PASSWORD = "Wifi Password";
#define   WIFI_STATIC_IP    true // True to use a static IP (specified using the following parameters), or false to use DHCP
IPAddress WIFI_SUBNET       (255, 255, 255, 0); // Only used for static IP
IPAddress WIFI_HOST         (192, 168, 1, 64);  // Static IP for this device
IPAddress WIFI_GATEWAY      (192, 168, 1, 1);   // Only used for static IP
IPAddress WIFI_PRIMARY_DNS  (192, 168, 1, 1);   // Only used for static IP
IPAddress WIFI_SECONDARY_DNS(192, 168, 1, 1);   // Only used for static IP

// IP access restrictions: Only these remote IP's will be able to access the web pages (the main/status page and the metrics page)
// NOTE: Leave this array empty to allow access from any client or all clients: IPAddress webAllowedClients[] = {};
IPAddress webAllowedClients[] = {
  //IPAddress(192, 168, 1, 1),
  //IPAddress(192, 168, 1, 1)
};

// Measurement configuration
#define RMS_ADC_PIN 6 // ADC pin number that the ZMPT101B output is connected to
#define RMS_NOISE_FLOOR 5.0 // Noise floor for the RMS calculation
#define RMS_SCALE_FACTOR 120.0 / 267.70 // Scale factor for the RMS calculation to result in 120V for typical max measurements
#define RMS_POWER_OFF_THRESHOLD 110 // If measured RMS voltage is less than this value, then power state will be published as "Off"
#define RMS_SAMPLES 4000 // Number of samples to average for the RMS calculation. Should be just enough to remove any jitter.

// OLED display configuration
#define OLED_ADDRESS     0x3D
#define OLED_SCREEN_WIDTH 128
#define OLED_SCREEN_HEIGHT 64

// BME280 configuration
#define BME280_ADDRESS 0x77
#define BME280_TEMP_F true // Set to false for Celsius
#define BME280_TEMP_ADJUSTMENT -2.25 // Degree adjustment to add to raw temperature readings to compensate for sensor heating (Assumed to be in degrees F if BME280_TEMP_F=true, or in degrees C if BME280_TEMP_F=false)

// Telemetry configuration, for Prometheus or similar
#define TELEMETRY_SCRAPE_WINDOW 100 // Maximum expected scrape interval, in seconds, with a little headroom (e.g. use 100 for an expected scrape interval of 60)

// Max update delays. Outputs will be refreshed at least this often.
#define UPDATE_INTERVAL_MQTT    15
#define UPDATE_INTERVAL_OLED    1  // Must be an odd number due to the display toggling function

// MQTT configuration
const char* MQTT_SERVER   = "192.168.1.60"; // MQTT server name or IP
const int   MQTT_PORT     = 8883;           // 1883 is the default port for MQTT, 8883 is the default for MQTTS (TLS)
const char* MQTT_USER     = "MQTT user";    // Null if no authentication is required
const char* MQTT_PASSWORD = "MQTT pass";    // Null if no authentication is required
#define     MQTT_TOPIC_BASE      "home/sensors/ac-voltage/"                       // Base topic string for all other sensor values
const char* MQTT_TOPIC_VOLTAGE         = MQTT_TOPIC_BASE "sensor_ac_voltage_rms"; // Topic to publish the voltage measurements
const char* MQTT_TOPIC_POWER_STATE     = MQTT_TOPIC_BASE "sensor_ac_power_state"; // Topic to publish whether the power is on/off based on measured voltage
const char* MQTT_TOPIC_UPTIME          = MQTT_TOPIC_BASE "uptime";                // Topic to publish uptime
const char* MQTT_TOPIC_UPTIME_SECONDS  = MQTT_TOPIC_BASE "uptime_seconds";        // Topic to publish total uptime in seconds
const char* MQTT_TOPIC_TEMPERATURE     = MQTT_TOPIC_BASE "environmental_temperature";
const char* MQTT_TOPIC_HUMIDITY        = MQTT_TOPIC_BASE "environmental_humidity";
const char* MQTT_TOPIC_PRESSURE        = MQTT_TOPIC_BASE "environmental_pressure";
const char* MQTT_TOPIC_FREE_HEAP       = MQTT_TOPIC_BASE "esp32_free_heap_bytes";  // Topic to publish the current ESP32 free heap size in bytes
const char* MQTT_TOPIC_BATTERY_VOLTAGE = MQTT_TOPIC_BASE "esp32_battery_voltage";  // LiPo voltage
const char* MQTT_TOPIC_CHIP_INFO       = MQTT_TOPIC_BASE "esp32_chip_information"; // Topic to publish info about the ESP32

// Certificate Authority for TLS connections
static const char CERT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
<Your CA cert>
-----END CERTIFICATE-----
)EOF";

// Client cert, issued by the CA
static const char CERT_CLIENT[] = R"EOF(
-----BEGIN CERTIFICATE-----
<Your client cert>
-----END CERTIFICATE-----
)EOF";

// Client cert private key
static const char CERT_CLIENT_KEY[] = R"EOF(
-----BEGIN PRIVATE KEY-----
<Your client cert key>
-----END PRIVATE KEY-----
)EOF";
