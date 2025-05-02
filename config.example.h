// Network configuration
const char* WIFI_HOSTNAME = "Sensor-Ambient-1"; // Network hostname for the ESP32, such as "Sensor-Upper-Attic", and should be similar to MQTT_TOPIC_BASE below
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
#define TFT_TIMEOUT       30    // After a button press, the display will be powered on for this many seconds before auto-shutoff
#define TFT_ROTATION      0     // 0 = 0, 1 = 90, 2 = 180, 3 = 270. Reference: https://learn.adafruit.com/adafruit-gfx-graphics-library/rotating-the-display

// BME280 configuration
#define BME280_ADDRESS 0x77
#define BME280_TEMP_F true           // Set to false for Celsius
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

// AC power sensing pin
#define AC_POWER_PIN 10 // Attached to the center of the 5V/3.3V resister divider such that the pin gets 3.3V when 5V power exists on the USB bus

// General measurement configuration
#define MEASUREMENT_WINDOW 60 // Measurements will be averaged with an EMA over this period. It should be similar to the expected Prometheus scrape interval, in seconds.

// Max update delays. Outputs will be refreshed at least this often.
#define UPDATE_INTERVAL_MQTT    60
#define UPDATE_INTERVAL_TFT     1    // Must be an odd number due to the display toggling function
#define UPDATE_INTERVAL_DATA    5*60 // Number of seconds between data captures to the PSRAM buffer (5 minutes default)

// PSRAM data storage
#define DATA_HISTORY_COUNT  2016  // Number of data elements to keep per stream, with one element per UPDATE_INTERVAL_DATA (7 days default = one sample every UPDATE_INTERVAL_DATA seconds = 12 values per hour X 24 X 7)
#define DATA_ELEMENT_SIZE   10    // Size of one data value as numeric text, with delimiter
#define DATA_STREAM_COUNT   6     // There are six data streams: sound, light, temperature, humidity, pressure, time. The buffer is also NULL terminated (the +1).
#define DATA_STREAM_SIZE    DATA_HISTORY_COUNT * DATA_ELEMENT_SIZE // Size of a single data stream in bytes
#define DATA_SET_SIZE       DATA_STREAM_SIZE * DATA_STREAM_COUNT   // Size of the entire data set in bytes

// MQTT configuration
const char* MQTT_SERVER   = "192.168.1.60"; // MQTT server name or IP
const int   MQTT_PORT     = 8883;           // 1883 is the default port for MQTT, 8883 is the default for MQTTS (TLS)
const char* MQTT_USER     = "MQTT user";    // Null if no authentication is required
const char* MQTT_PASSWORD = "MQTT pass";    // Null if no authentication is required
#define     MQTT_TOPIC_BASE "home/sensors/ambient_1/" // Base topic string for all values from this sensor

// Certificate Authority for TLS connections
static const char CERT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
<Your CA cert>
-----END CERTIFICATE-----
)EOF";

/*
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
*/
