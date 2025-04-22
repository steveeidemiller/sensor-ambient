// Network configuration
const char* WIFI_HOSTNAME = "Sensor-Ambient-1"; // Network hostname for the ESP32
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

// TFT display configuration
#define TFT_SCREEN_WIDTH  240
#define TFT_SCREEN_HEIGHT 135
#define TFT_TIMEOUT       8    // After a button press, the display will be powered on for this many seconds before auto-shutoff

// BME280 configuration
#define BME280_ADDRESS 0x77
#define BME280_TEMP_F true // Set to false for Celsius
#define BME280_TEMP_ADJUSTMENT -2.25 // Degree adjustment to add to raw temperature readings to compensate for sensor heating (Assumed to be in degrees F if BME280_TEMP_F=true, or in degrees C if BME280_TEMP_F=false)

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

// General measurement configuration
#define MEASUREMENT_WINDOW 60 // Measurements will be averaged with an EMA over this period. It should be similar to the expected Prometheus scrape interval, in seconds.

// Max update delays. Outputs will be refreshed at least this often.
#define UPDATE_INTERVAL_MQTT    15
#define UPDATE_INTERVAL_TFT     1  // Must be an odd number due to the display toggling function

// MQTT configuration
const char* MQTT_SERVER   = "192.168.1.60"; // MQTT server name or IP
const int   MQTT_PORT     = 8883;           // 1883 is the default port for MQTT, 8883 is the default for MQTTS (TLS)
const char* MQTT_USER     = "MQTT user";    // Null if no authentication is required
const char* MQTT_PASSWORD = "MQTT pass";    // Null if no authentication is required
#define     MQTT_TOPIC_BASE  "home/sensors/ambient_upper_attic/"                       // Base topic string for all values from this sensor
const char* MQTT_TOPIC_SPL                  = MQTT_TOPIC_BASE "spl";                   // Current sound level in dB
const char* MQTT_TOPIC_SPL_AVERAGE          = MQTT_TOPIC_BASE "spl_average";           // Average sound level in dB
const char* MQTT_TOPIC_SPL_PEAK             = MQTT_TOPIC_BASE "spl_peak";              // Peak sound level in dB
const char* MQTT_TOPIC_LUX                  = MQTT_TOPIC_BASE "lux";                   // Current light level in Lux
const char* MQTT_TOPIC_LUX_AVERAGE          = MQTT_TOPIC_BASE "lux_average";           // Average light level in Lux
const char* MQTT_TOPIC_LUX_PEAK             = MQTT_TOPIC_BASE "lux_peak";              // Peak light level in Lux
const char* MQTT_TOPIC_LUX_GAIN             = MQTT_TOPIC_BASE "lux_measurement_gain";              // Current light sensor gain setting
const char* MQTT_TOPIC_LUX_INTEGRATION_TIME = MQTT_TOPIC_BASE "lux_measurement_integration_time";  // Current light sensor integration time (in ms)
const char* MQTT_TOPIC_MEASUREMENT_WINDOW   = MQTT_TOPIC_BASE "measurement_window";    // Measurement window size in seconds
const char* MQTT_TOPIC_UPTIME               = MQTT_TOPIC_BASE "uptime";                // Uptime
const char* MQTT_TOPIC_UPTIME_SECONDS       = MQTT_TOPIC_BASE "uptime_seconds";        // Total uptime in seconds
const char* MQTT_TOPIC_TEMPERATURE          = MQTT_TOPIC_BASE "environmental_temperature";
const char* MQTT_TOPIC_HUMIDITY             = MQTT_TOPIC_BASE "environmental_humidity";
const char* MQTT_TOPIC_PRESSURE             = MQTT_TOPIC_BASE "environmental_pressure";
const char* MQTT_TOPIC_FREE_HEAP            = MQTT_TOPIC_BASE "esp32_free_heap_bytes";  // Current ESP32 free heap size in bytes
const char* MQTT_TOPIC_BATTERY_VOLTAGE      = MQTT_TOPIC_BASE "esp32_battery_voltage";  // LiPo voltage
const char* MQTT_TOPIC_CHIP_INFO            = MQTT_TOPIC_BASE "esp32_chip_information"; // Info about the ESP32

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
