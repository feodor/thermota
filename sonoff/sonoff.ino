/*
  sonoff.ino - Sonoff-Tasmota firmware for iTead Sonoff, Wemos and NodeMCU hardware

  Copyright (C) 2017  Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/*====================================================
  Prerequisites:
    - Change libraries/PubSubClient/src/PubSubClient.h
        #define MQTT_MAX_PACKET_SIZE 512

    - Select IDE Tools - Flash Mode: "DOUT"
    - Select IDE Tools - Flash Size: "1M (no SPIFFS)"
  ====================================================*/

#define VERSION                0x05090100   // 5.9.1

// Location specific includes
#include "sonoff.h"                         // Enumaration used in user_config.h
#include "user_config.h"                    // Fixed user configurable options
#include "user_config_override.h"           // Configuration overrides for user_config.h
#include "i18n.h"                           // Language support configured by user_config.h
#include "sonoff_template.h"                // Hardware configuration
#include "sonoff_post.h"                    // Configuration overrides for all previous includes

// Libraries
#include <PubSubClient.h>                   // MQTT
// Max message size calculated by PubSubClient is (MQTT_MAX_PACKET_SIZE < 5 + 2 + strlen(topic) + plength)
#if (MQTT_MAX_PACKET_SIZE -TOPSZ -7) < MESSZ  // If the max message size is too small, throw an error at compile time. See PubSubClient.cpp line 359
  #error "MQTT_MAX_PACKET_SIZE is too small in libraries/PubSubClient/src/PubSubClient.h, increase it to at least 512"
#endif
#include <Ticker.h>                         // RTC, HLW8012, OSWatch
#include <ESP8266WiFi.h>                    // MQTT, Ota, WifiManager
#include <ESP8266HTTPClient.h>              // MQTT, Ota
#include <ESP8266httpUpdate.h>              // Ota
#include <StreamString.h>                   // Webserver, Updater
#include <BufferString.h>
#include <ArduinoJson.h>                    // WemoHue, IRremote, Domoticz
#ifdef USE_WEBSERVER
  #include <ESP8266WebServer.h>             // WifiManager, Webserver
  #include <DNSServer.h>                    // WifiManager
#endif  // USE_WEBSERVER
#ifdef USE_DISCOVERY
  #include <ESP8266mDNS.h>                  // MQTT, Webserver
#endif  // USE_DISCOVERY
#ifdef USE_I2C
  #include <Wire.h>                         // I2C support library
#endif  // USE_I2C
#ifdef USE_SPI
  #include <SPI.h>                          // SPI support, TFT
#endif  // USE_SPI
#ifdef USE_LCD1602A
  #include <LiquidCrystal.h>
const int
		RS = 4 /* D2 */,
		EN = 0 /* D3 */,
		d4 = 15 /* D8 */,
		d5 = 13 /* D7 */,
		d6 = 12 /* D6 */,
		d7 = 14 /* D5 */;

LiquidCrystal lcd(RS, EN, d4, d5, d6, d7);

struct {
    float DS18B20_temperature;
    float DHT22_temperature;
    float DHT22_humidity;
} LcdDataExchange = {NAN, NAN, NAN};
#endif
#ifdef LEDPIN_BLINK
#define LEDPIN_BLINK_1	(3)
#define LEDPIN_BLINK_2	(1)

typedef struct  BlinkDesc {
	uint8	initstate[2];
	uint8	period[2]; //blink per second 
	uint8	state[2];
} BlinkDesc;

static uint8 LedBlinkPins[] = {LEDPIN_BLINK_1, LEDPIN_BLINK_2};

static struct BlinkDesc BlinkLed[] = {
	{ {1, 0}, { 1, 0 }, {0, 0} },
	{ {0, 1}, { 0, 1 }, {0, 0} },
	{ {1, 0}, {10, 0 }, {0, 0} },
	{ {0, 1}, { 0, 10}, {0, 0} },
	{ {1, 0}, {10, 10}, {0, 0} }
};

static BlinkDesc *BlinkLedCurrent = NULL;

#endif
#ifdef USE_OLED
#include <SH1106Wire.h>
// D1 -> SDA (GPIO5)
// D2 -> SCL (GPIO4)
static SH1106Wire display(0x3c, 5, 4);

#define NDATAT	(48)
static struct {
	uint32_t	crc;

	float temperature;
	float pressure;
	float humidity;

	float sumT;
	int	  countT;

	unsigned long firstT;
	unsigned long lastT;
	unsigned long tillLastT;

	float   dataT[NDATAT];
} ToDisplay = {
	0,
	NAN, NAN, NAN,
	0.0, 0,
	0, 0, 0,
	{
		NAN, NAN, NAN, NAN, NAN, NAN,
		NAN, NAN, NAN, NAN, NAN, NAN,
		NAN, NAN, NAN, NAN, NAN, NAN,
		NAN, NAN, NAN, NAN, NAN, NAN,
		NAN, NAN, NAN, NAN, NAN, NAN,
		NAN, NAN, NAN, NAN, NAN, NAN,
		NAN, NAN, NAN, NAN, NAN, NAN,
		NAN, NAN, NAN, NAN, NAN, NAN
	}
};

#define OLED_REDRAW_NOTHING	(0x00)
#define OLED_REDRAW_TEMP	(0x01)
#define OLED_REDRAW_OTHER	(0x02)

static struct {
	const char	*topic;
	float		*pvalue;
	uint16_t	redraw;
} Topics [] = {
	{"teodor/cabinet/tele/sonoff/DHT22_temperature", &ToDisplay.temperature, OLED_REDRAW_TEMP},
	{"teodor/pressure/bme_saloon", &ToDisplay.pressure, OLED_REDRAW_OTHER},
	{"teodor/cabinet/tele/sonoff/DHT22_humidity", &ToDisplay.humidity, OLED_REDRAW_OTHER},
	{"zavety/envout/bmet", &ToDisplay.temperature, OLED_REDRAW_TEMP},
	{"zavety/envout/bmep", &ToDisplay.pressure, OLED_REDRAW_OTHER},
	{"zavety/envout/bmeh", &ToDisplay.humidity, OLED_REDRAW_OTHER}
};

static uint16_t haveToReDraw = OLED_REDRAW_NOTHING;
#endif
#define lengthof(x)	((int)(sizeof(x)/sizeof(x[0])))
/********************************************************************************************/
// Structs
#include "settings.h"

enum TasmotaCommands {
  CMND_BACKLOG, CMND_DELAY, CMND_POWER, CMND_STATUS, CMND_POWERONSTATE, CMND_PULSETIME,
  CMND_BLINKTIME, CMND_BLINKCOUNT, CMND_SAVEDATA, CMND_SETOPTION, CMND_TEMPERATURE_RESOLUTION, CMND_HUMIDITY_RESOLUTION,
  CMND_PRESSURE_RESOLUTION, CMND_POWER_RESOLUTION, CMND_VOLTAGE_RESOLUTION, CMND_ENERGY_RESOLUTION, CMND_MODULE, CMND_MODULES,
  CMND_GPIO, CMND_GPIOS, CMND_PWM, CMND_PWMFREQUENCY, CMND_PWMRANGE, CMND_COUNTER, CMND_COUNTERTYPE,
  CMND_COUNTERDEBOUNCE, CMND_SLEEP, CMND_UPGRADE, CMND_UPLOAD, CMND_OTAURL, CMND_SERIALLOG, CMND_SYSLOG,
  CMND_LOGHOST, CMND_LOGPORT, CMND_IPADDRESS, CMND_NTPSERVER, CMND_AP, CMND_SSID, CMND_PASSWORD, CMND_HOSTNAME,
  CMND_WIFICONFIG, CMND_FRIENDLYNAME, CMND_SWITCHMODE, CMND_WEBSERVER, CMND_WEBPASSWORD, CMND_WEBLOG, CMND_EMULATION,
  CMND_TELEPERIOD, CMND_RESTART, CMND_RESET, CMND_TIMEZONE, CMND_ALTITUDE, CMND_LEDPOWER, CMND_LEDSTATE,
  CMND_CFGDUMP, CMND_I2CSCAN, CMND_EXCEPTION };
const char kTasmotaCommands[] PROGMEM =
  D_CMND_BACKLOG "|" D_CMND_DELAY "|" D_CMND_POWER "|" D_CMND_STATUS "|" D_CMND_POWERONSTATE "|" D_CMND_PULSETIME "|"
  D_CMND_BLINKTIME "|" D_CMND_BLINKCOUNT "|" D_CMND_SAVEDATA "|" D_CMND_SETOPTION "|" D_CMND_TEMPERATURE_RESOLUTION "|" D_CMND_HUMIDITY_RESOLUTION "|"
  D_CMND_PRESSURE_RESOLUTION "|" D_CMND_POWER_RESOLUTION "|" D_CMND_VOLTAGE_RESOLUTION "|" D_CMND_ENERGY_RESOLUTION "|" D_CMND_MODULE "|" D_CMND_MODULES "|"
  D_CMND_GPIO "|" D_CMND_GPIOS "|" D_CMND_PWM "|" D_CMND_PWMFREQUENCY "|" D_CMND_PWMRANGE "|" D_CMND_COUNTER "|"  D_CMND_COUNTERTYPE "|"
  D_CMND_COUNTERDEBOUNCE "|" D_CMND_SLEEP "|" D_CMND_UPGRADE "|" D_CMND_UPLOAD "|" D_CMND_OTAURL "|" D_CMND_SERIALLOG "|" D_CMND_SYSLOG "|"
  D_CMND_LOGHOST "|" D_CMND_LOGPORT "|" D_CMND_IPADDRESS "|" D_CMND_NTPSERVER "|" D_CMND_AP "|" D_CMND_SSID "|" D_CMND_PASSWORD "|" D_CMND_HOSTNAME "|"
  D_CMND_WIFICONFIG "|" D_CMND_FRIENDLYNAME "|" D_CMND_SWITCHMODE "|" D_CMND_WEBSERVER "|" D_CMND_WEBPASSWORD "|" D_CMND_WEBLOG "|" D_CMND_EMULATION "|"
  D_CMND_TELEPERIOD "|" D_CMND_RESTART "|" D_CMND_RESET "|" D_CMND_TIMEZONE "|" D_CMND_ALTITUDE "|" D_CMND_LEDPOWER "|" D_CMND_LEDSTATE "|"
  D_CMND_CFGDUMP "|" D_CMND_I2CSCAN
#ifdef DEBUG_THEO
  "|" D_CMND_EXCEPTION
#endif
  ;

enum MqttCommands {
  CMND_MQTTHOST, CMND_MQTTPORT, CMND_MQTTRETRY, CMND_STATETEXT, CMND_MQTTFINGERPRINT, CMND_MQTTCLIENT,
  CMND_MQTTUSER, CMND_MQTTPASSWORD, CMND_FULLTOPIC, CMND_PREFIX, CMND_GROUPTOPIC, CMND_TOPIC,
  CMND_BUTTONTOPIC, CMND_SWITCHTOPIC, CMND_BUTTONRETAIN, CMND_SWITCHRETAIN, CMND_POWERRETAIN, CMND_SENSORRETAIN };
const char kMqttCommands[] PROGMEM =
  D_CMND_MQTTHOST "|" D_CMND_MQTTPORT "|" D_CMND_MQTTRETRY "|" D_CMND_STATETEXT "|" D_CMND_MQTTFINGERPRINT "|" D_CMND_MQTTCLIENT "|"
  D_CMND_MQTTUSER "|" D_CMND_MQTTPASSWORD "|" D_CMND_FULLTOPIC "|" D_CMND_PREFIX "|" D_CMND_GROUPTOPIC "|" D_CMND_TOPIC "|"
  D_CMND_BUTTONTOPIC "|" D_CMND_SWITCHTOPIC "|" D_CMND_BUTTONRETAIN "|" D_CMND_SWITCHRETAIN "|" D_CMND_POWERRETAIN "|" D_CMND_SENSORRETAIN ;

const char kOptionOff[] PROGMEM = "OFF|" D_OFF "|" D_FALSE "|" D_STOP "|" D_CELSIUS ;
const char kOptionOn[] PROGMEM = "ON|" D_ON "|" D_TRUE "|" D_START "|" D_FAHRENHEIT "|" D_USER ;
const char kOptionToggle[] PROGMEM = "TOGGLE|" D_TOGGLE "|" D_ADMIN ;
const char kOptionBlink[] PROGMEM = "BLINK|" D_BLINK ;
const char kOptionBlinkOff[] PROGMEM = "BLINKOFF|" D_BLINKOFF ;

// Global variables
int baudrate = APP_BAUDRATE;                // Serial interface baud rate
byte serial_in_byte;                        // Received byte
int serial_in_byte_counter = 0;             // Index in receive buffer
byte dual_hex_code = 0;                     // Sonoff dual input flag
uint16_t dual_button_code = 0;              // Sonoff dual received code
int16_t save_data_counter;                  // Counter and flag for config save to Flash
uint8_t mqtt_retry_counter = 0;             // MQTT connection retry counter
uint8_t fallback_topic_flag = 0;            // Use Topic or FallbackTopic
unsigned long state_loop_timer = 0;         // State loop timer
int state_loop_counter = 0;                 // State per second flag
int mqtt_connection_flag = 2;               // MQTT connection messages flag
int ota_state_flag = 0;                     // OTA state flag
int ota_result = 0;                         // OTA result
byte ota_retry_counter = OTA_ATTEMPTS;      // OTA retry counter
int restart_flag = 0;                       // Sonoff restart flag
int wifi_state_flag = WIFI_RESTART;         // Wifi state flag
int uptime = 0;                             // Current uptime in hours
boolean latest_uptime_flag = true;          // Signal latest uptime
int tele_period = 0;                        // Tele period timer
byte web_log_index = 0;                     // Index in Web log buffer
byte reset_web_log_flag = 0;                // Reset web console log
byte devices_present = 0;                   // Max number of devices supported
int status_update_timer = 0;                // Refresh initial status
uint16_t pulse_timer[MAX_PULSETIMERS] = { 0 }; // Power off timer
uint16_t blink_timer = 0;                   // Power cycle timer
uint16_t blink_counter = 0;                 // Number of blink cycles
power_t blink_power;                        // Blink power state
power_t blink_mask = 0;                     // Blink relay active mask
power_t blink_powersave;                    // Blink start power save state
uint16_t mqtt_cmnd_publish = 0;             // ignore flag for publish command
power_t latching_power = 0;                 // Power state at latching start
uint8_t latching_relay_pulse = 0;           // Latching relay pulse timer
uint8_t backlog_index = 0;                  // Command backlog index
uint8_t backlog_pointer = 0;                // Command backlog pointer
uint8_t backlog_mutex = 0;                  // Command backlog pending
uint16_t backlog_delay = 0;                 // Command backlog delay
uint8_t interlock_mutex = 0;                // Interlock power command pending

#ifdef USE_MQTT_TLS
  WiFiClientSecure EspClient;               // Wifi Secure Client
#else
  WiFiClient EspClient;                     // Wifi Client
#endif
PubSubClient MqttClient(EspClient);         // MQTT Client
WiFiUDP PortUdp;                            // UDP Syslog and Alexa

power_t power = 0;                          // Current copy of Settings.power
byte syslog_level;                          // Current copy of Settings.syslog_level
uint16_t syslog_timer = 0;                  // Timer to re-enable syslog_level
byte seriallog_level;                       // Current copy of Settings.seriallog_level
uint16_t seriallog_timer = 0;               // Timer to disable Seriallog
uint8_t sleep;                              // Current copy of Settings.sleep
uint8_t stop_flash_rotate = 0;              // Allow flash configuration rotation

int blinks = 201;                           // Number of LED blinks
uint8_t blinkstate = 0;                     // LED state

uint8_t blockgpio0 = 4;                     // Block GPIO0 for 4 seconds after poweron to workaround Wemos D1 RTS circuit
uint8_t lastbutton[MAX_KEYS] = { NOT_PRESSED, NOT_PRESSED, NOT_PRESSED, NOT_PRESSED };  // Last button states
uint8_t holdbutton[MAX_KEYS] = { 0 };       // Timer for button hold
uint8_t multiwindow[MAX_KEYS] = { 0 };      // Max time between button presses to record press count
uint8_t multipress[MAX_KEYS] = { 0 };       // Number of button presses within multiwindow
uint8_t lastwallswitch[MAX_SWITCHES];       // Last wall switch states
uint8_t holdwallswitch[MAX_SWITCHES] = { 0 };  // Timer for wallswitch push button hold

mytmplt my_module;                          // Active copy of Module name and GPIOs
uint8_t pin[GPIO_MAX];                      // Possible pin configurations
power_t rel_inverted = 0;                   // Relay inverted flag (1 = (0 = On, 1 = Off))
uint8_t led_inverted = 0;                   // LED inverted flag (1 = (0 = On, 1 = Off))
uint8_t pwm_inverted = 0;                   // PWM inverted flag (1 = inverted)
uint8_t dht_flg = 0;                        // DHT configured
uint8_t hlw_flg = 0;                        // Power monitor configured
uint8_t i2c_flg = 0;                        // I2C configured
uint8_t spi_flg = 0;                        // SPI configured
uint8_t light_type = 0;                     // Light types

boolean mdns_begun = false;

uint8_t xsns_present = 0;                   // Number of External Sensors found
boolean (*xsns_func_ptr[XSNS_MAX])(byte, void*);   // External Sensor Function Pointers for simple implementation of sensors
char version[16];                           // Version string from VERSION define
char my_hostname[33];                       // Composed Wifi hostname
char mqtt_client[33];                        // Composed MQTT Clientname
char serial_in_buffer[INPUT_BUFFER_SIZE + 2]; // Receive buffer
static char mqtt_data[MESSZ];                      // MQTT publish buffer
BufferString	mqtt_msg(mqtt_data, sizeof(mqtt_data));
String web_log[MAX_LOG_LINES];              // Web log buffer
String backlog[MAX_BACKLOG];                // Command backlog

char helper_buffer[160 /* MAX(TOPSZ, 160 */ ];  // helper buffer for various usage, ie format string for sprintf_P

#define MAX_MQTT_ATTEMPT_COUNT	(3)
static uint8 mqtt_attempt_count = MAX_MQTT_ATTEMPT_COUNT;

static char topic_buffer[TOPSZ];


void GetMqttClient(char* output, const char* input, byte size)
{
  char *token;
  BufferString buffer(output, size);

  token = strchr(input, '%');

  if (token)
  {
	  token++;

	  while(*token && isdigit(*token))
		  token++;

	  if (!(*token == 'd' || *token == 'X' || *token == 'x'))
		  token = NULL;
	  else if (strchr(token, '%') != NULL)
		  /* % is a single in string */
		  token = NULL;
  }

  if (token)
	  buffer.sprintf(input, ESP.getChipId());
  else
	  buffer = input;
}

static void
GetTopicInternal(BufferString& fulltopic, byte prefix, char *topic)
{
  if (fallback_topic_flag) {
    fulltopic = FPSTR(kPrefixes[prefix]);
    fulltopic += F("/");
    fulltopic += mqtt_client;
  } else {
    fulltopic = Settings.mqtt_fulltopic;
    if ((0 == prefix) && (-1 == fulltopic.indexOf(F(MQTT_TOKEN_PREFIX)))) {
      fulltopic += F("/" MQTT_TOKEN_PREFIX);  // Need prefix for commands to handle mqtt topic loops
    }
    for (byte i = 0; i < 3; i++) {
      if ('\0' == Settings.mqtt_prefix[i][0]) {
		  strncpy_P(Settings.mqtt_prefix[i], kPrefixes[i],
					sizeof(Settings.mqtt_prefix[i]) - 1);
      }
    }
    fulltopic.replace(F(MQTT_TOKEN_PREFIX), Settings.mqtt_prefix[prefix]);
    fulltopic.replace(F(MQTT_TOKEN_TOPIC), topic);
  }
  fulltopic.replace(F("#"), "");
  fulltopic.replace(F("//"), "/");
  if (!fulltopic.endsWith('/')) {
    fulltopic += '/';
  }
}

const char * GetTopic_P(byte prefix, char *topic, const char* subtopic)
{
  BufferString fulltopic(topic_buffer, sizeof(topic_buffer));

  GetTopicInternal(fulltopic, prefix, topic);

  fulltopic += FPSTR(subtopic);

  return fulltopic.c_str();
}

const char * GetTopic(byte prefix, char *topic, const char* subtopic)
{
  BufferString fulltopic(topic_buffer, sizeof(topic_buffer));

  GetTopicInternal(fulltopic, prefix, topic);

  fulltopic += subtopic;

  return fulltopic.c_str();
}

char* GetStateText(byte state)
{
  if (state > 3) {
    state = 1;
  }
  return Settings.state_text[state];
}

/********************************************************************************************/

void SetLatchingRelay(power_t power, uint8_t state)
{
  power &= 1;
  if (2 == state) {           // Reset relay
    state = 0;
    latching_power = power;
    latching_relay_pulse = 0;
  }
  else if (state && !latching_relay_pulse) {  // Set port power to On
    latching_power = power;
    latching_relay_pulse = 2;  // max 200mS (initiated by stateloop())
  }
  if (pin[GPIO_REL1 +latching_power] < 99) {
    digitalWrite(pin[GPIO_REL1 +latching_power], bitRead(rel_inverted, latching_power) ? !state : state);
  }
}

void SetDevicePower(power_t rpower)
{
  uint8_t state;

  if (4 == Settings.poweronstate) {  // All on and stay on
    power = (1 << devices_present) -1;
    rpower = power;
  }
  if (Settings.flag.interlock) {     // Allow only one or no relay set
    power_t mask = 1;
    uint8_t count = 0;
    for (byte i = 0; i < devices_present; i++) {
      if (rpower & mask) {
        count++;
      }
      mask <<= 1;
    }
    if (count > 1) {
      power = 0;
      rpower = 0;
    }
  }
  if (light_type) {
    LightSetPower(bitRead(rpower, devices_present -1));
  }
  if ((SONOFF_DUAL == Settings.module) || (CH4 == Settings.module)) {
    Serial.write(0xA0);
    Serial.write(0x04);
    Serial.write(rpower &0xFF);
    Serial.write(0xA1);
    Serial.write('\n');
    Serial.flush();
  }
  else if (EXS_RELAY == Settings.module) {
    SetLatchingRelay(rpower, 1);
  }
  else {
    for (byte i = 0; i < devices_present; i++) {
      state = rpower &1;
      if ((i < MAX_RELAYS) && (pin[GPIO_REL1 +i] < 99)) {
        digitalWrite(pin[GPIO_REL1 +i], bitRead(rel_inverted, i) ? !state : state);
      }
      rpower >>= 1;
    }
  }
  HlwSetPowerSteadyCounter(2);
}

void SetLedPower(uint8_t state)
{
  if (state) {
    state = 1;
  }
  digitalWrite(pin[GPIO_LED1], (bitRead(led_inverted, 0)) ? !state : state);
}

/********************************************************************************************/

void MqttSubscribe(const char *topic)
{
  AddLog_PP(LOG_LEVEL_DEBUG, PSTR(D_LOG_MQTT D_SUBSCRIBE_TO " %s"), topic);
  MqttClient.subscribe(topic);
  MqttClient.loop();  // Solve LmacRxBlk:1 messages
}

void MqttPublishDirect(const char* topic, boolean retained)
{
  yield();

  if (Settings.flag.mqtt_enabled) {

	MqttClient.loop(); 

	yield();

    if (MqttClient.publish(topic, mqtt_data, retained)) {
      AddLog_PP(LOG_LEVEL_DEBUG, PSTR(D_LOG_MQTT "%s = %s%s"), topic, mqtt_data, (retained) ? " (" D_RETAINED ")" : "");
//      MqttClient.loop();  // Do not use here! Will block previous publishes
    } else  {
      AddLog_PP(LOG_LEVEL_INFO, PSTR(D_LOG_RESULT "failed %s = %s"), topic, mqtt_data);
    }
  } else {
    AddLog_PP(LOG_LEVEL_DEBUG, PSTR(D_LOG_RESULT "%s = %s"), strrchr(topic,'/')+1, mqtt_data);
  }

  mqtt_msg.reset();
  if (Settings.ledstate &0x04) {
    blinks++;
  }
}

void MqttPublish(const char* topic, boolean retained = false)
{
  char *me;

  if (!strcmp(Settings.mqtt_prefix[0],Settings.mqtt_prefix[1])) {
    me = strstr(topic,Settings.mqtt_prefix[0]);
    if (me == topic) {
      mqtt_cmnd_publish += 8;
    }
  }
  MqttPublishDirect(topic, retained);
}

void MqttPublish(const char* topic, bool retained, const char *formatP, ...)
{
	va_list	arglist;

	mqtt_msg.reset();

	va_start(arglist, formatP);
	mqtt_msg.vsprintf_P(FPSTR(formatP), arglist);
	va_end(arglist);

	MqttPublish(topic, retained);
}

/* helper function */
void
MqttPublishPrefixTopic_PV(uint8_t prefix, const char* subtopic, boolean retained,
						  const char *formatP, va_list arglist)
{
/* prefix 0 = cmnd using subtopic
 * prefix 1 = stat using subtopic
 * prefix 2 = tele using subtopic
 * prefix 4 = cmnd using subtopic or RESULT
 * prefix 5 = stat using subtopic or RESULT
 * prefix 6 = tele using subtopic or RESULT
 */
	BufferString topic(helper_buffer, sizeof(helper_buffer));

	if (formatP)
	{
		mqtt_msg.reset();
		mqtt_msg.vsprintf_P(FPSTR(formatP), arglist);
	}

	if ((prefix > 3) && !Settings.flag.mqtt_response)
		topic = FPSTR(S_RSLT_RESULT);
	else
		topic = FPSTR(subtopic);
	topic.toUpperCase();

	prefix &= 3;
	MqttPublish(GetTopic_P(prefix, Settings.mqtt_topic, topic.c_str()), retained);
	
}

void
MqttPublishPrefixTopic_P(uint8_t prefix, const char* subtopic, boolean retained,
						 const char *formatP = NULL, ...)
{
	va_list	arglist;

	va_start(arglist, formatP);
	MqttPublishPrefixTopic_PV(prefix, subtopic, retained, formatP, arglist);
	va_end(arglist);
}

void
MqttPublishPrefixTopic_P(uint8_t prefix, const char* subtopic,
						 const char *formatP = NULL, ...)
{
	va_list	arglist;

	va_start(arglist, formatP);
	MqttPublishPrefixTopic_PV(prefix, subtopic, false, formatP, arglist);
	va_end(arglist);
}

void MqttPublishSimple_P(const char* subtopic, const char *v)
{
	const char * topic;

	yield();

	if (Settings.flag.mqtt_enabled)
	{
		topic = GetTopic_P(2, Settings.mqtt_topic, subtopic);

		MqttClient.loop();
		yield();

		if (!MqttClient.publish(topic, v, false))
			AddLog_PP(LOG_LEVEL_INFO, PSTR(D_LOG_RESULT " failed %s = %s"), topic, v);
	}

  	mqtt_msg.reset();
}

void MqttPublishSimple_P(const char* subtopic, int v)
{
	char buf[2 + 3 * sizeof(long)];
	ltoa(v, buf, 10);
	MqttPublishSimple_P(subtopic, buf);
}

void MqttPublishSimple_P(const char* subtopic, float v)
{
	char sv[20];

	dtostrfd(v, 3, sv);
	MqttPublishSimple_P(subtopic, sv);
}

void MqttPublishSimple(const char* subtopic, const char *v)
{
	const char * topic;

	yield();

	if (Settings.flag.mqtt_enabled)
	{
		topic = GetTopic(2, Settings.mqtt_topic, subtopic);

		MqttClient.loop();
		yield();

		if (!MqttClient.publish(topic, v, false))
			AddLog_PP(LOG_LEVEL_INFO, PSTR(D_LOG_RESULT " failed %s = %s"), topic, v);
	}

  	mqtt_msg.reset();
}

void MqttPublishSimple(const char* subtopic, int v)
{
	char buf[2 + 3 * sizeof(long)];
	ltoa(v, buf, 10);
	MqttPublishSimple(subtopic, buf);
}

void MqttPublishSimple(const char* subtopic, float v)
{
	char sv[20];

	dtostrfd(v, 3, sv);
	MqttPublishSimple(subtopic, sv);
}

void MqttPublishPowerState(byte device)
{
  const char * stopic;
  char scommand[16];

  if ((device < 1) || (device > devices_present)) {
    device = 1;
  }
  GetPowerDevice(scommand, device, sizeof(scommand));
  stopic = GetTopic_P(1, Settings.mqtt_topic, (Settings.flag.mqtt_response) ? scommand : S_RSLT_RESULT);
  MqttPublish(stopic, false, PSTR("{\"%s\":\"%s\"}"), scommand, GetStateText(bitRead(power, device -1)));

  stopic = GetTopic_P(1, Settings.mqtt_topic, scommand);
  MqttPublish(stopic, Settings.flag.mqtt_power_retain, GetStateText(bitRead(power, device -1)));
}

void MqttPublishPowerBlinkState(byte device)
{
  char scommand[16];

  if ((device < 1) || (device > devices_present)) {
    device = 1;
  }

  MqttPublishPrefixTopic_P(5, S_RSLT_POWER, PSTR("{\"%s\":\"" D_BLINK " %s\"}"),
						   GetPowerDevice(scommand, device, sizeof(scommand)),
						   GetStateText(bitRead(blink_mask, device -1)));
}

void MqttConnected()
{
  const char * stopic;

  if (Settings.flag.mqtt_enabled) {

    // Satisfy iobroker (#299)
    MqttPublishPrefixTopic_P(0, S_RSLT_POWER, "");

    stopic = GetTopic_P(0, Settings.mqtt_topic, PSTR("#"));
    MqttSubscribe(stopic);
    if (strstr(Settings.mqtt_fulltopic, MQTT_TOKEN_TOPIC) != NULL) {
      stopic = GetTopic_P(0, Settings.mqtt_grptopic, PSTR("#"));
      MqttSubscribe(stopic);
      fallback_topic_flag = 1;
      stopic = GetTopic_P(0, mqtt_client, PSTR("#"));
      fallback_topic_flag = 0;
      MqttSubscribe(stopic);
    }
#ifdef USE_DOMOTICZ
    DomoticzMqttSubscribe();
#endif  // USE_DOMOTICZ
#ifdef USE_OLED
	for(byte i=0; i<lengthof(Topics); i++)
		MqttSubscribe(Topics[i].topic);
#endif
  }

  if (mqtt_connection_flag) {
    MqttPublishPrefixTopic_P(2, PSTR(D_RSLT_INFO "1"), PSTR("{\"" D_CMND_MODULE "\":\"%s\", \"" D_VERSION "\":\"%s\", \"" D_FALLBACKTOPIC "\":\"%s\", \"" D_CMND_GROUPTOPIC "\":\"%s\"}") ,
							 my_module.name, version, mqtt_client, Settings.mqtt_grptopic);
#ifdef USE_WEBSERVER
    if (Settings.webserver) {
      MqttPublishPrefixTopic_P(2, PSTR(D_RSLT_INFO "2"), PSTR("{\"" D_WEBSERVER_MODE "\":\"%s\", \"" D_CMND_HOSTNAME "\":\"%s\", \"" D_CMND_IPADDRESS "\":\"%s\"}"),
							   (2 == Settings.webserver) ? D_ADMIN : D_USER, my_hostname, WiFi.localIP().toString().c_str());
    }
#endif  // USE_WEBSERVER
    MqttPublishPrefixTopic_P(2, PSTR(D_RSLT_INFO "3"), PSTR("{\"" D_RESTARTREASON "\":\"%s\"}"),
							 (GetResetReason() == "Exception") ? ESP.getResetInfo().c_str() : GetResetReason().c_str());
    if (Settings.tele_period) {
      tele_period = Settings.tele_period -9;
    }
    status_update_timer = 2;
#ifdef USE_DOMOTICZ
    DomoticzSetUpdateTimer(2);
#endif  // USE_DOMOTICZ
  }
  mqtt_connection_flag = 0;
}

void MqttReconnect()
{
  const char * stopic;

  mqtt_retry_counter = Settings.mqtt_retry;

  if (!Settings.flag.mqtt_enabled) {
    MqttConnected();
    return;
  }

#ifdef USE_EMULATION
  UdpDisconnect();
#endif  // USE_EMULATION
  if (mqtt_connection_flag > 1) {
#ifdef USE_MQTT_TLS
    AddLog_P(LOG_LEVEL_INFO, S_LOG_MQTT, PSTR(D_FINGERPRINT));
    if (!EspClient.connect(Settings.mqtt_host, Settings.mqtt_port)) {
      AddLog_PP(LOG_LEVEL_DEBUG, PSTR(D_LOG_MQTT D_TLS_CONNECT_FAILED_TO " %s:%d. " D_RETRY_IN " %d " D_UNIT_SECOND),
				Settings.mqtt_host, Settings.mqtt_port, mqtt_retry_counter);
      return;
    }
    if (EspClient.verify(Settings.mqtt_fingerprint, Settings.mqtt_host)) {
      AddLog_P(LOG_LEVEL_INFO, S_LOG_MQTT, PSTR(D_VERIFIED));
    } else {
      AddLog_P(LOG_LEVEL_DEBUG, S_LOG_MQTT, PSTR(D_INSECURE));
    }
    EspClient.stop();
    yield();
#endif  // USE_MQTT_TLS
    MqttClient.setCallback(MqttDataCallback);
    mqtt_connection_flag = 1;
    mqtt_retry_counter = 1;
    return;
  }

  AddLog_P(LOG_LEVEL_INFO, S_LOG_MQTT, PSTR(D_ATTEMPTING_CONNECTION));
#ifndef USE_MQTT_TLS
#ifdef USE_DISCOVERY
#ifdef MQTT_HOST_DISCOVERY
//  if (!strlen(MQTT_HOST)) {
  if (!strlen(Settings.mqtt_host)) {
    MdnsDiscoverMqttServer();
  }
#endif  // MQTT_HOST_DISCOVERY
#endif  // USE_DISCOVERY
#endif  // USE_MQTT_TLS

  stopic = GetTopic_P(2, Settings.mqtt_topic, S_LWT);
  mqtt_msg = FPSTR(D_OFFLINE " ");
  mqtt_msg += (long)MqttClient.state();
  AddLog_PP(LOG_LEVEL_INFO, PSTR(D_OFFLINE " %d"), MqttClient.state());
 
  MqttClient.disconnect();
  MqttClient.setServer(Settings.mqtt_host, Settings.mqtt_port);

  char *mqtt_user = NULL;
  char *mqtt_pwd = NULL;
  if (strlen(Settings.mqtt_user) > 0) {
    mqtt_user = Settings.mqtt_user;
  }
  if (strlen(Settings.mqtt_pwd) > 0) {
    mqtt_pwd = Settings.mqtt_pwd;
  }
  if (MqttClient.connect(mqtt_client, mqtt_user, mqtt_pwd, stopic, 1, true, mqtt_msg.c_str())) {
    AddLog_P(LOG_LEVEL_INFO, S_LOG_MQTT, PSTR(D_CONNECTED));
    mqtt_retry_counter = 0;
    MqttPublish(stopic, true, PSTR(D_ONLINE));
    MqttConnected();
	mqtt_attempt_count = MAX_MQTT_ATTEMPT_COUNT;
  } else {
	//status codes are documented here http://pubsubclient.knolleary.net/api.html#state
    AddLog_PP(LOG_LEVEL_INFO, PSTR(D_LOG_MQTT D_CONNECT_FAILED_TO " %s:%d, rc %d. " D_RETRY_IN " %d " D_UNIT_SECOND),
			  Settings.mqtt_host, Settings.mqtt_port, MqttClient.state(), mqtt_retry_counter);
	if (mqtt_attempt_count == 0)
	{
		RtcSettings.oswatch_blocked_loop = 3;
		RtcSettingsSave();
		ESP.reset();
	}
	mqtt_attempt_count--;
  }

  RestartWebserver();
}

/********************************************************************************************/

boolean MqttCommand(boolean grpflg, char *type, uint16_t index, char *dataBuf, uint16_t data_len, int16_t payload, uint16_t payload16)
{
  char command [CMDSZ];
  boolean serviced = true;
  char stemp1[TOPSZ];
  char scommand[CMDSZ];
  uint16_t i;

  int command_code = GetCommandCode(command, sizeof(command), type, kMqttCommands);
  if (CMND_MQTTHOST == command_code) {
    if ((data_len > 0) && (data_len < sizeof(Settings.mqtt_host))) {
      strlcpy(Settings.mqtt_host, (!strcmp(dataBuf,"0")) ? "" : (1 == payload) ? MQTT_HOST : dataBuf, sizeof(Settings.mqtt_host));
      restart_flag = 2;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.mqtt_host);
  }
  else if (CMND_MQTTPORT == command_code) {
    if (payload16 > 0) {
      Settings.mqtt_port = (1 == payload16) ? MQTT_PORT : payload16;
      restart_flag = 2;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.mqtt_port);
  }
  else if (CMND_MQTTRETRY == command_code) {
    if ((payload >= MQTT_RETRY_SECS) && (payload < 32001)) {
      Settings.mqtt_retry = payload;
      mqtt_retry_counter = Settings.mqtt_retry;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.mqtt_retry);
  }
  else if ((CMND_STATETEXT == command_code) && (index > 0) && (index <= 4)) {
    if ((data_len > 0) && (data_len < sizeof(Settings.state_text[0]))) {
      for(i = 0; i <= data_len; i++) {
        if (dataBuf[i] == ' ') {
          dataBuf[i] = '_';
        }
      }
      strlcpy(Settings.state_text[index -1], dataBuf, sizeof(Settings.state_text[0]));
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_SVALUE), command, index, GetStateText(index -1));
  }
#ifdef USE_MQTT_TLS
  else if (CMND_MQTTFINGERPRINT == command_code) {
    if ((data_len > 0) && (data_len < sizeof(Settings.mqtt_fingerprint))) {
      strlcpy(Settings.mqtt_fingerprint, (!strcmp(dataBuf,"0")) ? "" : (1 == payload) ? MQTT_FINGERPRINT : dataBuf, sizeof(Settings.mqtt_fingerprint));
      restart_flag = 2;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.mqtt_fingerprint);
  }
#endif
  else if ((CMND_MQTTCLIENT == command_code) && !grpflg) {
    if ((data_len > 0) && (data_len < sizeof(Settings.mqtt_client))) {
      strlcpy(Settings.mqtt_client, (1 == payload) ? MQTT_CLIENT_ID : dataBuf, sizeof(Settings.mqtt_client));
      restart_flag = 2;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.mqtt_client);
  }
  else if (CMND_MQTTUSER == command_code) {
    if ((data_len > 0) && (data_len < sizeof(Settings.mqtt_user))) {
      strlcpy(Settings.mqtt_user, (!strcmp(dataBuf,"0")) ? "" : (1 == payload) ? MQTT_USER : dataBuf, sizeof(Settings.mqtt_user));
      restart_flag = 2;
    }
   mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.mqtt_user);
  }
  else if (CMND_MQTTPASSWORD == command_code) {
    if ((data_len > 0) && (data_len < sizeof(Settings.mqtt_pwd))) {
      strlcpy(Settings.mqtt_pwd, (!strcmp(dataBuf,"0")) ? "" : (1 == payload) ? MQTT_PASS : dataBuf, sizeof(Settings.mqtt_pwd));
      restart_flag = 2;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.mqtt_pwd);
  }
  else if (CMND_FULLTOPIC == command_code) {
    if ((data_len > 0) && (data_len < sizeof(Settings.mqtt_fulltopic))) {
      MakeValidMqtt(1, dataBuf);
      if (!strcmp(dataBuf, mqtt_client)) {
        payload = 1;
      }
      strlcpy(stemp1, (1 == payload) ? MQTT_FULLTOPIC : dataBuf, sizeof(stemp1));
      if (strcmp(stemp1, Settings.mqtt_fulltopic)) {
		// Offline or remove previous retained topic
        MqttPublishPrefixTopic_P(2, PSTR(D_LWT), true, (Settings.flag.mqtt_offline) ? S_OFFLINE : "");
        strlcpy(Settings.mqtt_fulltopic, stemp1, sizeof(Settings.mqtt_fulltopic));
        restart_flag = 2;
      }
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.mqtt_fulltopic);
  }
  else if ((CMND_PREFIX == command_code) && (index > 0) && (index <= 3)) {
    if ((data_len > 0) && (data_len < sizeof(Settings.mqtt_prefix[0]))) {
      MakeValidMqtt(0, dataBuf);
      strlcpy(Settings.mqtt_prefix[index -1], (1 == payload) ? (1==index)?SUB_PREFIX:(2==index)?PUB_PREFIX:PUB_PREFIX2 : dataBuf, sizeof(Settings.mqtt_prefix[0]));
//      if (Settings.mqtt_prefix[index -1][strlen(Settings.mqtt_prefix[index -1])] == '/') Settings.mqtt_prefix[index -1][strlen(Settings.mqtt_prefix[index -1])] = 0;
      restart_flag = 2;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_SVALUE), command, index, Settings.mqtt_prefix[index -1]);
  }
  else if (CMND_GROUPTOPIC == command_code) {
    if ((data_len > 0) && (data_len < sizeof(Settings.mqtt_grptopic))) {
      MakeValidMqtt(0, dataBuf);
      if (!strcmp(dataBuf, mqtt_client)) {
        payload = 1;
      }
      strlcpy(Settings.mqtt_grptopic, (1 == payload) ? MQTT_GRPTOPIC : dataBuf, sizeof(Settings.mqtt_grptopic));
      restart_flag = 2;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.mqtt_grptopic);
  }
  else if ((CMND_TOPIC == command_code) && !grpflg) {
    if ((data_len > 0) && (data_len < sizeof(Settings.mqtt_topic))) {
      MakeValidMqtt(0, dataBuf);
      if (!strcmp(dataBuf, mqtt_client)) {
        payload = 1;
      }
      strlcpy(stemp1, (1 == payload) ? MQTT_TOPIC : dataBuf, sizeof(stemp1));
      if (strcmp(stemp1, Settings.mqtt_topic)) {
		// Offline or remove previous retained topic
        MqttPublishPrefixTopic_P(2, PSTR(D_LWT), true, (Settings.flag.mqtt_offline) ? S_OFFLINE : "");
        strlcpy(Settings.mqtt_topic, stemp1, sizeof(Settings.mqtt_topic));
        restart_flag = 2;
      }
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.mqtt_topic);
  }
  else if ((CMND_BUTTONTOPIC == command_code) && !grpflg) {
    if ((data_len > 0) && (data_len < sizeof(Settings.button_topic))) {
      MakeValidMqtt(0, dataBuf);
      if (!strcmp(dataBuf, mqtt_client)) {
        payload = 1;
      }
      strlcpy(Settings.button_topic, (!strcmp(dataBuf,"0")) ? "" : (1 == payload) ? Settings.mqtt_topic : dataBuf, sizeof(Settings.button_topic));
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.button_topic);
  }
  else if (CMND_SWITCHTOPIC == command_code) {
    if ((data_len > 0) && (data_len < sizeof(Settings.switch_topic))) {
      MakeValidMqtt(0, dataBuf);
      if (!strcmp(dataBuf, mqtt_client)) {
        payload = 1;
      }
      strlcpy(Settings.switch_topic, (!strcmp(dataBuf,"0")) ? "" : (1 == payload) ? Settings.mqtt_topic : dataBuf, sizeof(Settings.switch_topic));
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.switch_topic);
  }
  else if (CMND_BUTTONRETAIN == command_code) {
    if ((payload >= 0) && (payload <= 1)) {
      strlcpy(Settings.button_topic, Settings.mqtt_topic, sizeof(Settings.button_topic));
      if (!payload) {
        for(i = 1; i <= MAX_KEYS; i++) {
          send_button_power(0, i, 9);  // Clear MQTT retain in broker
        }
      }
      Settings.flag.mqtt_button_retain = payload;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, GetStateText(Settings.flag.mqtt_button_retain));
  }
  else if (CMND_SWITCHRETAIN == command_code) {
    if ((payload >= 0) && (payload <= 1)) {
      strlcpy(Settings.button_topic, Settings.mqtt_topic, sizeof(Settings.button_topic));
      if (!payload) {
        for(i = 1; i <= MAX_SWITCHES; i++) {
          send_button_power(1, i, 9);  // Clear MQTT retain in broker
        }
      }
      Settings.flag.mqtt_switch_retain = payload;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, GetStateText(Settings.flag.mqtt_switch_retain));
  }
  else if (CMND_POWERRETAIN == command_code) {
    if ((payload >= 0) && (payload <= 1)) {
      if (!payload) {
        for(i = 1; i <= devices_present; i++) {  // Clear MQTT retain in broker
          MqttPublish(GetTopic_P(1, Settings.mqtt_topic, GetPowerDevice(scommand, i, sizeof(scommand))),
					  Settings.flag.mqtt_power_retain, "");
        }
      }
      Settings.flag.mqtt_power_retain = payload;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, GetStateText(Settings.flag.mqtt_power_retain));
  }
  else if (CMND_SENSORRETAIN == command_code) {
    if ((payload >= 0) && (payload <= 1)) {
      if (!payload) {
        MqttPublishPrefixTopic_P(2, PSTR(D_RSLT_SENSOR), Settings.flag.mqtt_sensor_retain, "");
        MqttPublishPrefixTopic_P(2, PSTR(D_RSLT_ENERGY), Settings.flag.mqtt_sensor_retain, "");
      }
      Settings.flag.mqtt_sensor_retain = payload;
    }
    mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, GetStateText(Settings.flag.mqtt_sensor_retain));
  }

#ifdef USE_DOMOTICZ
  else if (DomoticzCommand(type, index, dataBuf, data_len, payload)) {
    // Serviced
  }
#endif  // USE_DOMOTICZ
  else {
    serviced = false;
  }
  return serviced;
}

/********************************************************************************************/

void MqttDataCallback(char* topic, byte* data, unsigned int data_len)
{
  char *str;

  if (!strcmp(Settings.mqtt_prefix[0],Settings.mqtt_prefix[1])) {
    str = strstr(topic,Settings.mqtt_prefix[0]);
    if ((str == topic) && mqtt_cmnd_publish) {
      if (mqtt_cmnd_publish > 8) {
        mqtt_cmnd_publish -= 8;
      } else {
        mqtt_cmnd_publish = 0;
      }
      return;
    }
  }

  char topicBuf[TOPSZ];
  char dataBuf[data_len+1];
  char command [CMDSZ];
  char stemp1[TOPSZ];
  char *p;
  char *type = NULL;
  byte ptype = 0;
  byte jsflg = 0;
  byte lines = 1;
  uint16_t i = 0;
  uint16_t grpflg = 0;
  uint16_t index;
  uint32_t address;

  strncpy(topicBuf, topic, sizeof(topicBuf));
  for (i = 0; i < data_len; i++) {
    if (!isspace(data[i])) {
      break;
    }
  }
  data_len -= i;
  memcpy(dataBuf, data +i, sizeof(dataBuf));
  dataBuf[sizeof(dataBuf)-1] = 0;

  mqtt_msg.reset();

  AddLog_PP(LOG_LEVEL_DEBUG_MORE,  PSTR(D_LOG_RESULT D_RECEIVED_TOPIC " %s, " D_DATA_SIZE " %d, " D_DATA " %s"),
			topicBuf, data_len, dataBuf);

#ifdef USE_OLED
  for(i=0; i<lengthof(Topics); i++) {
	if (strcmp(topicBuf, Topics[i].topic) == 0)
	{
		char	*endptr;

		*Topics[i].pvalue = strtof(dataBuf, &endptr);

		if (endptr == dataBuf) {
			AddLog_PP(LOG_LEVEL_INFO,  PSTR("could not parse '%s %s'"),
					  topicBuf, dataBuf);
			*Topics[i].pvalue = NAN;
		}

		haveToReDraw |= Topics[i].redraw;

		return;
	}
  }
#endif

#ifdef USE_DOMOTICZ
  if (Settings.flag.mqtt_enabled) {
    if (DomoticzMqttData(topicBuf, sizeof(topicBuf), dataBuf, sizeof(dataBuf))) {
      return;
    }
  }
#endif  // USE_DOMOTICZ

  grpflg = (strstr(topicBuf, Settings.mqtt_grptopic) != NULL);
  fallback_topic_flag = (strstr(topicBuf, mqtt_client) != NULL);
  type = strrchr(topicBuf, '/') +1;  // Last part of received topic is always the command (type)

  index = 1;
  if (type != NULL) {
    for (i = 0; i < strlen(type); i++) {
      type[i] = toupper(type[i]);
    }
    while (isdigit(type[i-1])) {
      i--;
    }
    if (i < strlen(type)) {
      index = atoi(type +i);
    }
    type[i] = '\0';
  }

  AddLog_PP(LOG_LEVEL_DEBUG, PSTR(D_LOG_RESULT D_GROUP " %d, " D_INDEX " %d, " D_COMMAND " %s, " D_DATA " %s"),
			grpflg, index, type, dataBuf);

  if (type != NULL) {
    if (Settings.ledstate &0x02) {
      blinks++;
    }

    if (!strcmp(dataBuf,"?")) {
      data_len = 0;
    }
    int16_t payload = -99;               // No payload
    uint16_t payload16 = 0;
    long lnum = strtol(dataBuf, &p, 10);
    if (p != dataBuf) {
      payload = (int16_t) lnum;          // -32766 - 32767
      payload16 = (uint16_t) lnum;       // 0 - 65535
    }
    backlog_delay = MIN_BACKLOG_DELAY;       // Reset backlog delay

    if ((GetCommandCode(command, sizeof(command), dataBuf, kOptionOff) >= 0) || !strcasecmp(dataBuf, Settings.state_text[0])) {
      payload = 0;
    }
    if ((GetCommandCode(command, sizeof(command), dataBuf, kOptionOn) >= 0) || !strcasecmp(dataBuf, Settings.state_text[1])) {
      payload = 1;
    }
    if ((GetCommandCode(command, sizeof(command), dataBuf, kOptionToggle) >= 0) || !strcasecmp(dataBuf, Settings.state_text[2])) {
      payload = 2;
    }
    if (GetCommandCode(command, sizeof(command), dataBuf, kOptionBlink) >= 0) {
      payload = 3;
    }
    if (GetCommandCode(command, sizeof(command), dataBuf, kOptionBlinkOff) >= 0) {
      payload = 4;
    }

    int command_code = GetCommandCode(command, sizeof(command), type, kTasmotaCommands);
    if (CMND_BACKLOG == command_code) {
      if (data_len) {
        char *blcommand = strtok(dataBuf, ";");
        while (blcommand != NULL) {
          backlog[backlog_index] = String(blcommand);
          backlog_index++;
/*
          if (backlog_index >= MAX_BACKLOG) {
            backlog_index = 0;
          }
*/
          backlog_index &= 0xF;
          blcommand = strtok(NULL, ";");
        }
        mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, D_APPENDED);
      } else {
        uint8_t blflag = (backlog_pointer == backlog_index);
        backlog_pointer = backlog_index;
        mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, blflag ? D_EMPTY : D_ABORTED);
      }
    }
    else if (CMND_DELAY == command_code) {
      if ((payload >= MIN_BACKLOG_DELAY) && (payload <= 3600)) {
        backlog_delay = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, backlog_delay);
    }
    else if ((CMND_POWER == command_code) && (index > 0) && (index <= devices_present)) {
      if ((payload < 0) || (payload > 4)) {
        payload = 9;
      }
      ExecuteCommandPower(index, payload);
      fallback_topic_flag = 0;
      return;
    }
    else if (CMND_STATUS == command_code) {
      if ((payload < 0) || (payload > MAX_STATUS)) {
        payload = 99;
      }
      PublishStatus(payload);
      fallback_topic_flag = 0;
      return;
    }
    else if ((CMND_POWERONSTATE == command_code) && (Settings.module != MOTOR)) {
      /* 0 = Keep relays off after power on
       * 1 = Turn relays on after power on
       * 2 = Toggle relays after power on
       * 3 = Set relays to last saved state after power on
       * 4 = Turn relays on and disable any relay control (used for Sonoff Pow to always measure power)
       */
      if ((payload >= 0) && (payload <= 4)) {
        Settings.poweronstate = payload;
        if (4 == Settings.poweronstate) {
          for (byte i = 1; i <= devices_present; i++) {
            ExecuteCommandPower(i, 1);
          }
        }
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.poweronstate);
    }
    else if ((CMND_PULSETIME == command_code) && (index > 0) && (index <= MAX_PULSETIMERS)) {
      if (data_len > 0) {
        Settings.pulse_timer[index -1] = payload16;  // 0 - 65535
        pulse_timer[index -1] = 0;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_NVALUE), command, index, Settings.pulse_timer[index -1]);
    }
    else if (CMND_BLINKTIME == command_code) {
      if ((payload > 2) && (payload <= 3600)) {
        Settings.blinktime = payload;
        if (blink_timer) {
          blink_timer = Settings.blinktime;
        }
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.blinktime);
    }
    else if (CMND_BLINKCOUNT == command_code) {
      if (data_len > 0) {
        Settings.blinkcount = payload16;  // 0 - 65535
        if (blink_counter) {
          blink_counter = Settings.blinkcount *2;
        }
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.blinkcount);
    }
    else if (light_type && LightCommand(type, index, dataBuf, data_len, payload)) {
      // Serviced
    }
    else if (CMND_SAVEDATA == command_code) {
      if ((payload >= 0) && (payload <= 3600)) {
        Settings.save_data = payload;
        save_data_counter = Settings.save_data;
      }
      if (Settings.flag.save_state) {
        Settings.power = power;
      }
      SettingsSave(0);
      if (Settings.save_data > 1) {
        snprintf_P(stemp1, sizeof(stemp1), PSTR(D_EVERY " %d " D_UNIT_SECOND), Settings.save_data);
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, (Settings.save_data > 1) ? stemp1 : GetStateText(Settings.save_data));
    }
    else if (((CMND_SETOPTION == command_code) && ((index >= 0) && (index <= 17))) || ((index > 31) && (index <= P_MAX_PARAM8 +31))) {
      if (index <= 31) {
        ptype = 0;   // SetOption0 .. 31
      } else {
        ptype = 1;   // SetOption32 ..
        index = index -32;
      }
      if (payload >= 0) {
        if (0 == ptype) {  // SetOption0 .. 31
          if (payload <= 1) {
            switch (index) {
              case 3:   // mqtt
              case 15:  // pwm_control
                restart_flag = 2;
              case 0:   // save_state
              case 1:   // button_restrict
              case 2:   // value_units
              case 4:   // mqtt_response
              case 8:   // temperature_conversion
              case 10:  // mqtt_offline
              case 11:  // button_swap
              case 12:  // stop_flash_rotate
              case 13:  // button_single
              case 14:  // interlock
              case 16:  // ws_clock_reverse
              case 17:  // decimal_text
                bitWrite(Settings.flag.data, index, payload);
            }
            if (12 == index) {  // stop_flash_rotate
              stop_flash_rotate = payload;
              SettingsSave(2);
            }
          }
        }
        else {  // SetOption32 ..
          switch (index) {
            case P_HOLD_TIME:
              if ((payload >= 1) && (payload <= 100)) {
                Settings.param[P_HOLD_TIME] = payload;
              }
              break;
            case P_MAX_POWER_RETRY:
              if ((payload >= 1) && (payload <= 250)) {
                Settings.param[P_MAX_POWER_RETRY] = payload;
              }
              break;
          }
        }
      }
      if (ptype) {
        snprintf_P(stemp1, sizeof(stemp1), PSTR("%d"), Settings.param[index]);
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_SVALUE), command, (ptype) ? index +32 : index, (ptype) ? stemp1 : GetStateText(bitRead(Settings.flag.data, index)));
    }
    else if (CMND_TEMPERATURE_RESOLUTION == command_code) {
      if ((payload >= 0) && (payload <= 3)) {
        Settings.flag.temperature_resolution = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.flag.temperature_resolution);
    }
    else if (CMND_HUMIDITY_RESOLUTION == command_code) {
      if ((payload >= 0) && (payload <= 3)) {
        Settings.flag.humidity_resolution = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.flag.humidity_resolution);
    }
    else if (CMND_PRESSURE_RESOLUTION == command_code) {
      if ((payload >= 0) && (payload <= 3)) {
        Settings.flag.pressure_resolution = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.flag.pressure_resolution);
    }
    else if (CMND_POWER_RESOLUTION == command_code) {
      if ((payload >= 0) && (payload <= 1)) {
        Settings.flag.wattage_resolution = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.flag.wattage_resolution);
    }
    else if (CMND_VOLTAGE_RESOLUTION == command_code) {
      if ((payload >= 0) && (payload <= 1)) {
        Settings.flag.voltage_resolution = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.flag.voltage_resolution);
    }
    else if (CMND_ENERGY_RESOLUTION == command_code) {
      if ((payload >= 0) && (payload <= 5)) {
        Settings.flag.energy_resolution = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.flag.energy_resolution);
    }
    else if (CMND_MODULE == command_code) {
      if ((payload > 0) && (payload <= MAXMODULE)) {
        payload--;
        byte new_modflg = (Settings.module != payload);
        Settings.module = payload;
        if (new_modflg) {
          for (byte i = 0; i < MAX_GPIO_PIN; i++) {
            Settings.my_gp.io[i] = 0;
          }
        }
        restart_flag = 2;
      }
      snprintf_P(stemp1, sizeof(stemp1), kModules[Settings.module].name);
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE_SVALUE), command, Settings.module +1, stemp1);
    }
    else if (CMND_MODULES == command_code) {
      for (byte i = 0; i < MAXMODULE; i++) {
        if (!jsflg) {
			mqtt_msg.reset();
			mqtt_msg.sprintf_P(FPSTR("{\"" D_CMND_MODULES "%d\":\""), lines);
        } else {
          	mqtt_msg += ','; mqtt_msg += ' ';
        }
        jsflg = 1;
		mqtt_msg += FPSTR(kModules[i].name);
        mqtt_msg.sprintf_P(FPSTR("%d (%s)"), i +1, stemp1);
        if ((mqtt_msg.length() > 200) || (i == MAXMODULE -1)) {
		  mqtt_msg += F("\"}");
          MqttPublishPrefixTopic_P(5, type);
          jsflg = 0;
          lines++;
        }
      }
      mqtt_msg.reset();
    }
    else if ((CMND_GPIO == command_code) && (index < MAX_GPIO_PIN)) {
      mytmplt cmodule;
      memcpy_P(&cmodule, &kModules[Settings.module], sizeof(cmodule));
      if ((GPIO_USER == cmodule.gp.io[index]) && (payload >= 0) && (payload < GPIO_SENSOR_END)) {
        for (byte i = 0; i < MAX_GPIO_PIN; i++) {
          if ((GPIO_USER == cmodule.gp.io[i]) && (Settings.my_gp.io[i] == payload)) {
            Settings.my_gp.io[i] = 0;
          }
        }
        Settings.my_gp.io[index] = payload;
        restart_flag = 2;
      }
	  mqtt_msg = '{';
      byte jsflg = 0;
      for (byte i = 0; i < MAX_GPIO_PIN; i++) {
        if (GPIO_USER == cmodule.gp.io[i]) {
          if (jsflg) {
			  mqtt_msg += ','; mqtt_msg += ' ';
          }
          jsflg = 1;
          snprintf_P(stemp1, sizeof(stemp1), kSensors[Settings.my_gp.io[i]]);
          mqtt_msg.sprintf_P(FPSTR("\"" D_CMND_GPIO "%d\":\"%d (%s)\""), i, Settings.my_gp.io[i], stemp1);
        }
      }
      if (jsflg) {
		  mqtt_msg += '}';
      } else {
        mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, D_NOT_SUPPORTED);
      }
    }
    else if (CMND_GPIOS == command_code) {
      for (byte i = 0; i < GPIO_SENSOR_END; i++) {
        if (!jsflg) {
			mqtt_msg.reset();
          	mqtt_msg.sprintf_P(FPSTR("{\"" D_CMND_GPIOS "%d\":\""), lines);
        } else {
          	mqtt_msg += ','; mqtt_msg += ' ';
        }
        jsflg = 1;
		mqtt_msg += FPSTR(kSensors[i]);
        mqtt_msg.sprintf_P(FPSTR("%d (%s)"), i, stemp1);
        if ((mqtt_msg.length() > 200) || (i == GPIO_SENSOR_END -1)) {
			mqtt_msg += F("\"}");
          	MqttPublishPrefixTopic_P(5, type);
          	jsflg = 0;
          	lines++;
        }
      }
      mqtt_msg.reset();
    }
    else if ((CMND_PWM == command_code) && !light_type && (index > 0) && (index <= MAX_PWMS)) {
      if ((payload >= 0) && (payload <= Settings.pwm_range) && (pin[GPIO_PWM1 + index -1] < 99)) {
        Settings.pwm_value[index -1] = payload;
        analogWrite(pin[GPIO_PWM1 + index -1], bitRead(pwm_inverted, index -1) ? Settings.pwm_range - payload : payload);
      }
      mqtt_msg = FPSTR("{\"" D_CMND_PWM "\":{");
      bool first = true;
      for (byte i = 0; i < MAX_PWMS; i++) {
        if(pin[GPIO_PWM1 + i] < 99) {
          mqtt_msg.sprintf_P(FPSTR("%s\"" D_CMND_PWM "%d\":%d"), first ? "" : ", ", i+1, Settings.pwm_value[i]);
          first = false;
        }
      }
	  mqtt_msg += '}'; mqtt_msg += '}';
    }
    else if (CMND_PWMFREQUENCY == command_code) {
      if ((1 == payload) || ((payload >= 100) && (payload <= 4000))) {
        Settings.pwm_frequency = (1 == payload) ? PWM_FREQ : payload;
        analogWriteFreq(Settings.pwm_frequency);   // Default is 1000 (core_esp8266_wiring_pwm.c)
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.pwm_frequency);
    }
    else if (CMND_PWMRANGE == command_code) {
      if ((1 == payload) || ((payload > 254) && (payload < 1024))) {
        Settings.pwm_range = (1 == payload) ? PWM_RANGE : payload;
        for (byte i = 0; i < MAX_PWMS; i++) {
          if (Settings.pwm_value[i] > Settings.pwm_range) {
            Settings.pwm_value[i] = Settings.pwm_range;
          }
        }
        analogWriteRange(Settings.pwm_range);      // Default is 1023 (Arduino.h)
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.pwm_range);
    }
    else if ((CMND_COUNTER == command_code) && (index > 0) && (index <= MAX_COUNTERS)) {
      if ((data_len > 0) && (pin[GPIO_CNTR1 + index -1] < 99)) {
        RtcSettings.pulse_counter[index -1] = payload16;
        Settings.pulse_counter[index -1] = payload16;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_NVALUE), command, index, RtcSettings.pulse_counter[index -1]);
    }
    else if ((CMND_COUNTERTYPE == command_code) && (index > 0) && (index <= MAX_COUNTERS)) {
      if ((payload >= 0) && (payload <= 1) && (pin[GPIO_CNTR1 + index -1] < 99)) {
        bitWrite(Settings.pulse_counter_type, index -1, payload &1);
        RtcSettings.pulse_counter[index -1] = 0;
        Settings.pulse_counter[index -1] = 0;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_NVALUE), command, index, bitRead(Settings.pulse_counter_type, index -1));
    }
    else if (CMND_COUNTERDEBOUNCE == command_code) {
      if ((data_len > 0) && (payload16 < 32001)) {
        Settings.pulse_counter_debounce = payload16;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.pulse_counter_debounce);
    }
    else if (CMND_SLEEP == command_code) {
      if ((payload >= 0) && (payload < 251)) {
        if ((!Settings.sleep && payload) || (Settings.sleep && !payload)) {
          restart_flag = 2;
        }
        Settings.sleep = payload;
        sleep = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE_UNIT_NVALUE_UNIT), command, sleep, (Settings.flag.value_units) ? " " D_UNIT_MILLISECOND : "", Settings.sleep, (Settings.flag.value_units) ? " " D_UNIT_MILLISECOND : "");
    }
    else if ((CMND_UPGRADE == command_code) || (CMND_UPLOAD == command_code)) {
      // Check if the payload is numerically 1, and had no trailing chars.
      //   e.g. "1foo" or "1.2.3" could fool us.
      // Check if the version we have been asked to upgrade to is higher than our current version.
      //   We also need at least 3 chars to make a valid version number string.
      if (((1 == data_len) && (1 == payload)) || ((data_len >= 3) && NewerVersion(dataBuf))) {
        ota_state_flag = 3;
        mqtt_msg.sprintf_P(F("{\"%s\":\"" D_VERSION " %s " D_FROM " %s\"}"), command, version, Settings.ota_url);
      } else {
        mqtt_msg.sprintf_P(F("{\"%s\":\"" D_ONE_OR_GT "\"}"), command, version);
      }
    }
    else if (CMND_OTAURL == command_code) {
      if ((data_len > 0) && (data_len < sizeof(Settings.ota_url)))
        strlcpy(Settings.ota_url, (1 == payload) ? OTA_URL : dataBuf, sizeof(Settings.ota_url));
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.ota_url);
    }
    else if (CMND_SERIALLOG == command_code) {
      if ((payload >= LOG_LEVEL_NONE) && (payload <= LOG_LEVEL_ALL)) {
        Settings.seriallog_level = payload;
        seriallog_level = payload;
        seriallog_timer = 0;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE_ACTIVE_NVALUE), command, Settings.seriallog_level, seriallog_level);
    }
    else if (CMND_SYSLOG == command_code) {
      if ((payload >= LOG_LEVEL_NONE) && (payload <= LOG_LEVEL_ALL)) {
        Settings.syslog_level = payload;
        syslog_level = (Settings.flag.emulation) ? 0 : payload;
        syslog_timer = 0;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE_ACTIVE_NVALUE), command, Settings.syslog_level, syslog_level);
    }
    else if (CMND_LOGHOST == command_code) {
      if ((data_len > 0) && (data_len < sizeof(Settings.syslog_host))) {
        strlcpy(Settings.syslog_host, (1 == payload) ? SYS_LOG_HOST : dataBuf, sizeof(Settings.syslog_host));
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.syslog_host);
    }
    else if (CMND_LOGPORT == command_code) {
      if (payload16 > 0) {
        Settings.syslog_port = (1 == payload16) ? SYS_LOG_PORT : payload16;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.syslog_port);
    }
    else if ((CMND_IPADDRESS == command_code) && (index > 0) && (index <= 4)) {
      if (ParseIp(&address, dataBuf)) {
        Settings.ip_address[index -1] = address;
//        restart_flag = 2;
      }
      snprintf_P(stemp1, sizeof(stemp1), PSTR(" (%s)"), WiFi.localIP().toString().c_str());
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_SVALUE_SVALUE), command, index, IPAddress(Settings.ip_address[index -1]).toString().c_str(), (1 == index) ? stemp1:"");
    }
    else if ((CMND_NTPSERVER == command_code) && (index > 0) && (index <= 3)) {
      if ((data_len > 0) && (data_len < sizeof(Settings.ntp_server[0]))) {
        strlcpy(Settings.ntp_server[index -1], (!strcmp(dataBuf,"0")) ? "" : (1 == payload) ? (1==index)?NTP_SERVER1:(2==index)?NTP_SERVER2:NTP_SERVER3 : dataBuf, sizeof(Settings.ntp_server[0]));
        for (i = 0; i < strlen(Settings.ntp_server[index -1]); i++) {
          if (Settings.ntp_server[index -1][i] == ',') {
            Settings.ntp_server[index -1][i] = '.';
          }
        }
        restart_flag = 2;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_SVALUE), command, index, Settings.ntp_server[index -1]);
    }
    else if (CMND_AP == command_code) {
      if ((payload >= 0) && (payload <= 2)) {
        switch (payload) {
        case 0:  // Toggle
          Settings.sta_active ^= 1;
          break;
        case 1:  // AP1
        case 2:  // AP2
          Settings.sta_active = payload -1;
        }
        restart_flag = 2;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE_SVALUE), command, Settings.sta_active +1, Settings.sta_ssid[Settings.sta_active]);
    }
    else if ((CMND_SSID == command_code) && (index > 0) && (index <= 2)) {
      if ((data_len > 0) && (data_len < sizeof(Settings.sta_ssid[0]))) {
        strlcpy(Settings.sta_ssid[index -1], (1 == payload) ? (1 == index) ? STA_SSID1 : STA_SSID2 : dataBuf, sizeof(Settings.sta_ssid[0]));
        Settings.sta_active = index -1;
        restart_flag = 2;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_SVALUE), command, index, Settings.sta_ssid[index -1]);
    }
    else if ((CMND_PASSWORD == command_code) && (index > 0) && (index <= 2)) {
      if ((data_len > 0) && (data_len < sizeof(Settings.sta_pwd[0]))) {
        strlcpy(Settings.sta_pwd[index -1], (1 == payload) ? (1 == index) ? STA_PASS1 : STA_PASS2 : dataBuf, sizeof(Settings.sta_pwd[0]));
        Settings.sta_active = index -1;
        restart_flag = 2;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_SVALUE), command, index, Settings.sta_pwd[index -1]);
    }
    else if ((CMND_HOSTNAME == command_code) && !grpflg) {
      if ((data_len > 0) && (data_len < sizeof(Settings.hostname))) {
        strlcpy(Settings.hostname, (1 == payload) ? WIFI_HOSTNAME : dataBuf, sizeof(Settings.hostname));
        if (strstr(Settings.hostname,"%")) {
          strlcpy(Settings.hostname, WIFI_HOSTNAME, sizeof(Settings.hostname));
        }
        restart_flag = 2;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.hostname);
    }
    else if (CMND_WIFICONFIG == command_code) {
      if ((payload >= WIFI_RESTART) && (payload < MAX_WIFI_OPTION)) {
        Settings.sta_config = payload;
        wifi_state_flag = Settings.sta_config;
        snprintf_P(stemp1, sizeof(stemp1), kWifiConfig[Settings.sta_config]);
        mqtt_msg.sprintf_P(F("{\"" D_CMND_WIFICONFIG "\":\"%s " D_SELECTED "\"}"), stemp1);
        if (WifiState() != WIFI_RESTART) {
          restart_flag = 2;
        }
      } else {
        snprintf_P(stemp1, sizeof(stemp1), kWifiConfig[Settings.sta_config]);
        mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE_SVALUE), command, Settings.sta_config, stemp1);
      }
    }
    else if ((CMND_FRIENDLYNAME == command_code) && (index > 0) && (index <= 4)) {
      if ((data_len > 0) && (data_len < sizeof(Settings.friendlyname[0]))) {
        if (1 == index) {
          snprintf_P(stemp1, sizeof(stemp1), PSTR(FRIENDLY_NAME));
        } else {
          snprintf_P(stemp1, sizeof(stemp1), PSTR(FRIENDLY_NAME "%d"), index);
        }
        strlcpy(Settings.friendlyname[index -1], (1 == payload) ? stemp1 : dataBuf, sizeof(Settings.friendlyname[index -1]));
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_SVALUE), command, index, Settings.friendlyname[index -1]);
    }
    else if ((CMND_SWITCHMODE == command_code) && (index > 0) && (index <= MAX_SWITCHES)) {
      if ((payload >= 0) && (payload < MAX_SWITCH_OPTION)) {
        Settings.switchmode[index -1] = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_INDEX_NVALUE), command, index, Settings.switchmode[index-1]);
    }
#ifdef USE_WEBSERVER
    else if (CMND_WEBSERVER == command_code) {
      if ((payload >= 0) && (payload <= 2)) {
        Settings.webserver = payload;
      }
      if (Settings.webserver) {
        mqtt_msg.sprintf_P(F("{\"" D_CMND_WEBSERVER "\":\"" D_ACTIVE_FOR " %s " D_ON_DEVICE " %s " D_WITH_IP_ADDRESS " %s\"}"),
          (2 == Settings.webserver) ? D_ADMIN : D_USER, my_hostname, WiFi.localIP().toString().c_str());
      } else {
        mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, GetStateText(0));
      }
    }
    else if (CMND_WEBPASSWORD == command_code) {
      if ((data_len > 0) && (data_len < sizeof(Settings.web_password))) {
        strlcpy(Settings.web_password, (!strcmp(dataBuf,"0")) ? "" : (1 == payload) ? WEB_PASSWORD : dataBuf, sizeof(Settings.web_password));
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, Settings.web_password);
    }
    else if (CMND_WEBLOG == command_code) {
      if ((payload >= LOG_LEVEL_NONE) && (payload <= LOG_LEVEL_ALL)) {
        Settings.weblog_level = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.weblog_level);
    }
#ifdef USE_EMULATION
    else if (CMND_EMULATION == command_code) {
      if ((payload >= EMUL_NONE) && (payload < EMUL_MAX)) {
        Settings.flag.emulation = payload;
        restart_flag = 2;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.flag.emulation);
    }
#endif  // USE_EMULATION
#endif  // USE_WEBSERVER
    else if (CMND_TELEPERIOD == command_code) {
      if ((payload >= 0) && (payload < 3601)) {
        Settings.tele_period = (1 == payload) ? TELE_PERIOD : payload;
        if ((Settings.tele_period > 0) && (Settings.tele_period < 10)) {
          Settings.tele_period = 10;   // Do not allow periods < 10 seconds
        }
        tele_period = Settings.tele_period;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE_UNIT), command, Settings.tele_period, (Settings.flag.value_units) ? " " D_UNIT_SECOND : "");
    }
    else if (CMND_RESTART == command_code) {
      switch (payload) {
      case 1:
        restart_flag = 2;
        mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, D_RESTARTING);
        break;
      case 99:
        AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_APPLICATION D_RESTARTING));
        ESP.restart();
        break;
      default:
        mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, D_ONE_TO_RESTART);
      }
    }
    else if (CMND_RESET == command_code) {
      switch (payload) {
      case 1:
        restart_flag = 211;
        mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command , D_RESET_AND_RESTARTING);
        break;
      case 2:
        restart_flag = 212;
        mqtt_msg.sprintf_P(F("{\"" D_CMND_RESET "\":\"" D_ERASE ", " D_RESET_AND_RESTARTING "\"}"));
        break;
      default:
        mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, D_ONE_TO_RESET);
      }
    }
    else if (CMND_TIMEZONE == command_code) {
      if ((data_len > 0) && (((payload >= -13) && (payload <= 13)) || (99 == payload))) {
        Settings.timezone = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.timezone);
    }
    else if (CMND_ALTITUDE == command_code) {
      if ((data_len > 0) && ((payload >= -30000) && (payload <= 30000))) {
        Settings.altitude = payload;
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.altitude);
    }
    else if (CMND_LEDPOWER == command_code) {
      if ((payload >= 0) && (payload <= 2)) {
        Settings.ledstate &= 8;
        switch (payload) {
        case 0: // Off
        case 1: // On
          Settings.ledstate = payload << 3;
          break;
        case 2: // Toggle
          Settings.ledstate ^= 8;
          break;
        }
        blinks = 0;
        SetLedPower(Settings.ledstate &8);
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, GetStateText(bitRead(Settings.ledstate, 3)));
    }
    else if (CMND_LEDSTATE ==command_code) {
      if ((payload >= 0) && (payload < MAX_LED_OPTION)) {
        Settings.ledstate = payload;
        if (!Settings.ledstate) {
          SetLedPower(0);
        }
      }
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_NVALUE), command, Settings.ledstate);
    }
    else if (CMND_CFGDUMP == command_code) {
      SettingsDump(dataBuf);
      mqtt_msg.sprintf_P(FPSTR(S_JSON_COMMAND_SVALUE), command, D_DONE);
    }
    else if (Settings.flag.mqtt_enabled && MqttCommand(grpflg, type, index, dataBuf, data_len, payload, payload16)) {
      // Serviced
    }
    else if (hlw_flg && HlwCommand(type, index, dataBuf, data_len, payload)) {
      // Serviced
    }
    else if ((SONOFF_BRIDGE == Settings.module) && SonoffBridgeCommand(type, index, dataBuf, data_len, payload)) {
      // Serviced
    }
#ifdef USE_I2C
    else if ((CMND_I2CSCAN == command_code) && i2c_flg) {
      I2cScan(mqtt_msg);
    }
#endif  // USE_I2C
#ifdef USE_IR_REMOTE
    else if ((pin[GPIO_IRSEND] < 99) && IrSendCommand(type, index, dataBuf, data_len, payload)) {
      // Serviced
    }
#endif  // USE_IR_REMOTE
#ifdef DEBUG_THEO
    else if (CMND_EXCEPTION == command_code) {
      if (data_len > 0) {
        ExceptionTest(payload);
      }
      mqtt_msg.sprintf_P(FPSTR(_JSON_COMMAND_SVALUE), command, D_DONE);
    }
#endif  // DEBUG_THEO

	if (mqtt_msg.length() == 0)
    	mqtt_msg.sprintf_P(FPSTR("{\"" D_COMMAND "\":\"" D_ERROR "\"}"));
   	MqttPublishPrefixTopic_P(5, type);
  } else {
    blinks = 201;
    snprintf_P(topicBuf, sizeof(topicBuf), PSTR(D_COMMAND));
    MqttPublishPrefixTopic_P(5, topicBuf, PSTR("{\"" D_COMMAND "\":\"" D_UNKNOWN "\"}"));
  }

  fallback_topic_flag = 0;
}

/********************************************************************************************/

boolean send_button_power(byte key, byte device, byte state)
{
// key 0 = button_topic
// key 1 = switch_topic
// state 0 = off
// state 1 = on
// state 2 = toggle
// state 3 = hold
// state 9 = clear retain flag

  const char * stopic;
  char scommand[CMDSZ];
  boolean result = false;

  char *key_topic = (key) ? Settings.switch_topic : Settings.button_topic;
  if (Settings.flag.mqtt_enabled && MqttClient.connected() && (strlen(key_topic) != 0) && strcmp(key_topic, "0")) {
    if (!key && (device > devices_present)) {
      device = 1;
    }
    stopic = GetTopic_P(0, key_topic, GetPowerDevice(scommand, device, sizeof(scommand), key));
    if (9 == state) {
      mqtt_msg.reset();
    } else {
      if ((!strcmp(Settings.mqtt_topic, key_topic) || !strcmp(Settings.mqtt_grptopic, key_topic)) && (2 == state)) {
        state = ~(power >> (device -1)) &1;
      }
      mqtt_msg = FPSTR(GetStateText(state));
    }
#ifdef USE_DOMOTICZ
    if (!(DomoticzButton(key, device, state, mqtt_msg.length()))) {
      MqttPublishDirect(stopic, (key) ? Settings.flag.mqtt_switch_retain : Settings.flag.mqtt_button_retain);
    }
#else
    MqttPublishDirect(stopic, (key) ? Settings.flag.mqtt_switch_retain : Settings.flag.mqtt_button_retain);
#endif  // USE_DOMOTICZ
    result = true;
  }
  return result;
}

void ExecuteCommandPower(byte device, byte state)
{
// device  = Relay number 1 and up
// state 0 = Relay Off
// state 1 = Relay On (turn off after Settings.pulse_timer * 100 mSec if enabled)
// state 2 = Toggle relay
// state 3 = Blink relay
// state 4 = Stop blinking relay
// state 6 = Relay Off and no publishPowerState
// state 7 = Relay On and no publishPowerState
// state 9 = Show power state

  uint8_t publish_power = 1;
  if ((6 == state) || (7 == state)) {
    state &= 1;
    publish_power = 0;
  }
  if ((device < 1) || (device > devices_present)) {
    device = 1;
  }
  if (device <= MAX_PULSETIMERS) {
    pulse_timer[(device -1)] = 0;
  }
  power_t mask = 1 << (device -1);
  if (state <= 2) {
    if ((blink_mask & mask)) {
      blink_mask &= (POWER_MASK ^ mask);  // Clear device mask
      MqttPublishPowerBlinkState(device);
    }
    if (Settings.flag.interlock && !interlock_mutex) {  // Clear all but masked relay
      interlock_mutex = 1;
      for (byte i = 0; i < devices_present; i++) {
        power_t imask = 1 << i;
        if ((power & imask) && (mask != imask)) {
          ExecuteCommandPower(i +1, 0);
        }
      }
      interlock_mutex = 0;
    }
    switch (state) {
    case 0: { // Off
      power &= (POWER_MASK ^ mask);
      break; }
    case 1: // On
      power |= mask;
      break;
    case 2: // Toggle
      power ^= mask;
    }
    SetDevicePower(power);
#ifdef USE_DOMOTICZ
    DomoticzUpdatePowerState(device);
#endif  // USE_DOMOTICZ
    if (device <= MAX_PULSETIMERS) {
      pulse_timer[(device -1)] = (power & mask) ? Settings.pulse_timer[(device -1)] : 0;
    }
  }
  else if (3 == state) { // Blink
    if (!(blink_mask & mask)) {
      blink_powersave = (blink_powersave & (POWER_MASK ^ mask)) | (power & mask);  // Save state
      blink_power = (power >> (device -1))&1;  // Prep to Toggle
    }
    blink_timer = 1;
    blink_counter = ((!Settings.blinkcount) ? 64000 : (Settings.blinkcount *2)) +1;
    blink_mask |= mask;  // Set device mask
    MqttPublishPowerBlinkState(device);
    return;
  }
  else if (4 == state) { // No Blink
    byte flag = (blink_mask & mask);
    blink_mask &= (POWER_MASK ^ mask);  // Clear device mask
    MqttPublishPowerBlinkState(device);
    if (flag) {
      ExecuteCommandPower(device, (blink_powersave >> (device -1))&1);  // Restore state
    }
    return;
  }
  if (publish_power) {
    MqttPublishPowerState(device);
  }
}

void StopAllPowerBlink()
{
  power_t mask;

  for (byte i = 1; i <= devices_present; i++) {
    mask = 1 << (i -1);
    if (blink_mask & mask) {
      blink_mask &= (POWER_MASK ^ mask);  // Clear device mask
      MqttPublishPowerBlinkState(i);
      ExecuteCommandPower(i, (blink_powersave >> (i -1))&1);  // Restore state
    }
  }
}

void ExecuteCommand(char *cmnd)
{
  char stopic[CMDSZ];
  char svalue[INPUT_BUFFER_SIZE];
  char *start;
  char *token;

  token = strtok(cmnd, " ");
  if (token != NULL) {
    start = strrchr(token, '/');   // Skip possible cmnd/sonoff/ preamble
    if (start) {
      token = start +1;
    }
  }
  snprintf_P(stopic, sizeof(stopic), PSTR("/%s"), (token == NULL) ? "" : token);
  token = strtok(NULL, "");
//  snprintf_P(svalue, sizeof(svalue), (token == NULL) ? "" : token);  // Fails with command FullTopic home/%prefix%/%topic% as it processes %p of %prefix%
  strlcpy(svalue, (token == NULL) ? "" : token, sizeof(svalue));       // Fixed 5.8.0b
  MqttDataCallback(stopic, (byte*)svalue, strlen(svalue));
}

void PublishStatus(uint8_t payload)
{
  uint8_t option = 1;

  // Workaround MQTT - TCP/IP stack queueing when SUB_PREFIX = PUB_PREFIX
  if (!strcmp(Settings.mqtt_prefix[0],Settings.mqtt_prefix[1]) && (!payload)) {
    option++;
  }

  if ((!Settings.flag.mqtt_enabled) && (6 == payload)) {
    payload = 99;
  }
  if ((!hlw_flg) && ((8 == payload) || (9 == payload))) {
    payload = 99;
  }

  if ((0 == payload) || (99 == payload)) {
    MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS),  PSTR("{\"" D_CMND_STATUS "\":{\"" D_CMND_MODULE "\":%d, \"" D_CMND_FRIENDLYNAME "\":\"%s\", \"" D_CMND_TOPIC "\":\"%s\", \"" D_CMND_BUTTONTOPIC "\":\"%s\", \"" D_CMND_POWER "\":%d, \"" D_CMND_POWERONSTATE "\":%d, \"" D_CMND_LEDSTATE "\":%d, \"" D_CMND_SAVEDATA "\":%d, \"" D_SAVESTATE "\":%d, \"" D_CMND_BUTTONRETAIN "\":%d, \"" D_CMND_POWERRETAIN "\":%d}}"),
							 Settings.module +1, Settings.friendlyname[0], Settings.mqtt_topic, Settings.button_topic, power, Settings.poweronstate, Settings.ledstate, Settings.save_data, Settings.flag.save_state, Settings.flag.mqtt_button_retain, Settings.flag.mqtt_power_retain);
  }

  if ((0 == payload) || (1 == payload)) {
    MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS "1"), PSTR("{\"" D_CMND_STATUS D_STATUS1_PARAMETER "\":{\"" D_BAUDRATE "\":%d, \"" D_CMND_GROUPTOPIC "\":\"%s\", \"" D_CMND_OTAURL "\":\"%s\", \"" D_UPTIME "\":%d, \"" D_CMND_SLEEP "\":%d, \"" D_BOOTCOUNT "\":%d, \"" D_SAVECOUNT "\":%d, \"" D_SAVEADDRESS "\":\"%X\"}}"),
							 baudrate, Settings.mqtt_grptopic, Settings.ota_url, uptime, Settings.sleep, Settings.bootcount, Settings.save_flag, GetSettingsAddress());
  }

  if ((0 == payload) || (2 == payload)) {
    MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS "2"), PSTR("{\"" D_CMND_STATUS D_STATUS2_FIRMWARE "\":{\"" D_VERSION "\":\"%s\", \"" D_BUILDDATETIME "\":\"%s\", \"" D_BOOTVERSION "\":%d, \"" D_COREVERSION "\":\"%s\", \"" D_SDKVERSION "\":\"%s\"}}"),
							 version, GetBuildDateAndTime().c_str(), ESP.getBootVersion(), ESP.getCoreVersion().c_str(), ESP.getSdkVersion());
  }

  if ((0 == payload) || (3 == payload)) {
    MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS "3"), PSTR("{\"" D_CMND_STATUS D_STATUS3_LOGGING "\":{\"" D_CMND_SERIALLOG "\":%d, \"" D_CMND_WEBLOG "\":%d, \"" D_CMND_SYSLOG "\":%d, \"" D_CMND_LOGHOST "\":\"%s\", \"" D_CMND_LOGPORT "\":%d, \"" D_CMND_SSID "1\":\"%s\", \"" D_CMND_SSID "2\":\"%s\", \"" D_CMND_TELEPERIOD "\":%d, \"" D_CMND_SETOPTION "\":\"%08X\"}}"),
							 Settings.seriallog_level, Settings.weblog_level, Settings.syslog_level, Settings.syslog_host, Settings.syslog_port, Settings.sta_ssid[0], Settings.sta_ssid[1], Settings.tele_period, Settings.flag.data);
  }

  if ((0 == payload) || (4 == payload)) {
    MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS "4"), PSTR("{\"" D_CMND_STATUS D_STATUS4_MEMORY "\":{\"" D_PROGRAMSIZE "\":%d, \"" D_FREEMEMORY "\":%d, \"" D_HEAPSIZE "\":%d, \"" D_PROGRAMFLASHSIZE "\":%d, \"" D_FLASHSIZE "\":%d, \"" D_FLASHMODE "\":%d}}"),
							  ESP.getSketchSize()/1024, ESP.getFreeSketchSpace()/1024, ESP.getFreeHeap()/1024, ESP.getFlashChipSize()/1024, ESP.getFlashChipRealSize()/1024, ESP.getFlashChipMode());
  }

  if ((0 == payload) || (5 == payload)) {
    MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS "5"),  PSTR("{\"" D_CMND_STATUS D_STATUS5_NETWORK "\":{\"" D_CMND_HOSTNAME "\":\"%s\", \"" D_CMND_IPADDRESS "\":\"%s\", \"" D_GATEWAY "\":\"%s\", \"" D_SUBNETMASK "\":\"%s\", \"" D_DNSSERVER "\":\"%s\", \"" D_MAC "\":\"%s\", \"" D_CMND_WEBSERVER "\":%d, \"" D_CMND_WIFICONFIG "\":%d}}"),
							 my_hostname, WiFi.localIP().toString().c_str(), IPAddress(Settings.ip_address[1]).toString().c_str(), IPAddress(Settings.ip_address[2]).toString().c_str(), IPAddress(Settings.ip_address[3]).toString().c_str(), WiFi.macAddress().c_str(), Settings.webserver, Settings.sta_config);
  }

  if (((0 == payload) || (6 == payload)) && Settings.flag.mqtt_enabled) {
    MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS "6"),  PSTR("{\"" D_CMND_STATUS D_STATUS6_MQTT "\":{\"" D_CMND_MQTTHOST "\":\"%s\", \"" D_CMND_MQTTPORT "\":%d, \"" D_CMND_MQTTCLIENT D_MASK "\":\"%s\", \"" D_CMND_MQTTCLIENT "\":\"%s\", \"" D_CMND_MQTTUSER "\":\"%s\", \"MAX_PACKET_SIZE\":%d, \"KEEPALIVE\":%d}}"),
							 Settings.mqtt_host, Settings.mqtt_port, Settings.mqtt_client, mqtt_client, Settings.mqtt_user, MQTT_MAX_PACKET_SIZE, MQTT_KEEPALIVE);
  }

  if ((0 == payload) || (7 == payload)) {
    MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS "7"), PSTR("{\"" D_CMND_STATUS D_STATUS7_TIME "\":{\"" D_UTC_TIME "\":\"%s\", \"" D_LOCAL_TIME "\":\"%s\", \"" D_STARTDST "\":\"%s\", \"" D_ENDDST "\":\"%s\", \"" D_CMND_TIMEZONE "\":%d}}"),
							 GetTime(0).c_str(), GetTime(1).c_str(), GetTime(2).c_str(), GetTime(3).c_str(), Settings.timezone);
  }

  if (hlw_flg) {
    if ((0 == payload) || (8 == payload)) {
      HlwMqttStatus(mqtt_msg);
      MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS "8"));
    }

    if ((0 == payload) || (9 == payload)) {
      MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS "9"), PSTR("{\"" D_CMND_STATUS D_STATUS9_MARGIN "\":{\"" D_CMND_POWERLOW "\":%d, \"" D_CMND_POWERHIGH "\":%d, \"" D_CMND_VOLTAGELOW "\":%d, \"" D_CMND_VOLTAGEHIGH "\":%d, \"" D_CMND_CURRENTLOW "\":%d, \"" D_CMND_CURRENTHIGH "\":%d}}"),
							   Settings.hlw_pmin, Settings.hlw_pmax, Settings.hlw_umin, Settings.hlw_umax, Settings.hlw_imin, Settings.hlw_imax);
    }
  }

  if ((0 == payload) || (10 == payload)) {
    mqtt_msg.sprintf_P(F("{\"" D_CMND_STATUS D_STATUS10_SENSOR "\":"));
    MqttShowSensor();
	mqtt_msg += '}';
    MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS "10"));
  }

  if ((0 == payload) || (11 == payload)) {
    mqtt_msg.sprintf_P(F("{\"" D_CMND_STATUS D_STATUS11_STATUS "\":"));
    MqttShowState();
	mqtt_msg += '}';
    MqttPublishPrefixTopic_P(option, PSTR(D_CMND_STATUS "11"));
  }

}

void MqttShowState()
{
  char stemp1[16];

  mqtt_msg.sprintf_P(FPSTR("{\"" D_TIME "\":\"%s\", \"" D_UPTIME "\":%d"), GetDateAndTime().c_str(), uptime);
#ifdef USE_ADC_VCC
  dtostrfd((double)ESP.getVcc()/1000, 3, stemp1);
  mqtt_msg.sprintf_P(F(", \"" D_VCC "\":%s"), stemp1);
#endif
  for (byte i = 0; i < devices_present; i++) {
    mqtt_msg.sprintf_P(F(", \"%s\":\"%s\""), GetPowerDevice(stemp1, i +1, sizeof(stemp1)), GetStateText(bitRead(power, i)));
  }
  mqtt_msg.sprintf_P(F(", \"" D_WIFI "\":{\"" D_AP "\":%d, \"" D_SSID "\":\"%s\", \"" D_RSSI "\":%d, \"" D_APMAC_ADDRESS "\":\"%s\", \"FreeMem\":%d}}"),
    Settings.sta_active +1, Settings.sta_ssid[Settings.sta_active],
	WifiGetRssiAsQuality(WiFi.RSSI()), WiFi.BSSIDstr().c_str(), ESP.getFreeHeap());
}

boolean MqttShowSensor()
{
  mqtt_msg.sprintf_P(FPSTR("{\"" D_TIME "\":\"%s\""), GetDateAndTime().c_str());
  int json_data_start = mqtt_msg.length();
  for (byte i = 0; i < MAX_SWITCHES; i++) {
    if (pin[GPIO_SWT1 +i] < 99) {
      boolean swm = ((FOLLOW_INV == Settings.switchmode[i]) || (PUSHBUTTON_INV == Settings.switchmode[i]) || (PUSHBUTTONHOLD_INV == Settings.switchmode[i]));
      mqtt_msg.sprintf_P(F(", \"" D_SWITCH "%d\":\"%s\""), i +1, GetStateText(swm ^ lastwallswitch[i]));
    }
  }

  XsnsCall(FUNC_XSNS_JSON_APPEND, NULL);

  boolean json_data_available = (mqtt_msg.length() - json_data_start);
  if (strstr_P(mqtt_msg.c_str(), PSTR(D_TEMPERATURE)))
    mqtt_msg.sprintf_P(F(", \"" D_TEMPERATURE_UNIT "\":\"%c\""), TempUnit());
  if (RtcSettings.thermocontrol_duty_ratio >= 0.0 &&
	  RtcSettings.thermocontrol_duty_ratio < 1.0)
  {
	char ratio[10];

	dtostrfd(RtcSettings.thermocontrol_duty_ratio, Settings.flag.temperature_resolution, ratio);
	mqtt_msg.sprintf_P(F(", \"" D_RATIO "\":%s"), ratio);
  }

  mqtt_msg += '}';
  return json_data_available;
}

/*********************************************************************************************\
 * Temperature Control
\*********************************************************************************************/

#ifdef TEMPERATURE_CONTROL
void
ActThermoControl(float current_temperature)
{
    if (!Settings.enable_temperature_control)
        return;
    if (Ds18x20Sensors() == 0)
        return;
	if (isnan(current_temperature) && millis() < 5000)
		/* do not act immediatly after restart */ 
		return;

    uint8_t device = 1;
    bool is_power, should_poweron = true;

    is_power = ((power >> (device - 1)) & 0x01) ? true : false;

    if (isnan(current_temperature) ||
        isnan(Settings.destination_temperature) ||
        isnan(Settings.delta_temperature))
    {
        should_poweron = !Settings.inverted_temperature_control;
    }
    else if (current_temperature > Settings.destination_temperature + Settings.delta_temperature)
    {
        should_poweron = Settings.inverted_temperature_control;
    }
    else if (current_temperature < Settings.destination_temperature)
    {
        should_poweron = !Settings.inverted_temperature_control;
    }
    else
    {
        should_poweron = is_power;
    }

    if (should_poweron != is_power)
	{
		mqtt_msg.reset();
        ExecuteCommandPower(device, should_poweron ? 1 : 0);

		if (should_poweron == !Settings.inverted_temperature_control)
		{	
			// turn on even with inverted logic
			RtcSettings.thermocontrol_up_time = LocalTime();
		}
		else
		{
			if (RtcSettings.thermocontrol_down_time > 0)
			{
				float total_time, on_time;

				total_time = LocalTime() - RtcSettings.thermocontrol_down_time;
				on_time = LocalTime() - RtcSettings.thermocontrol_up_time;

				RtcSettings.thermocontrol_duty_ratio = on_time/total_time;
				RtcSettingsSave();
			}
			RtcSettings.thermocontrol_down_time = LocalTime();
		}
	}
}
#endif //TEMPERATURE_CONTROL
/********************************************************************************************/
#ifdef USE_LCD1602A
void
LCDPrint() {
	BufferString prstr(helper_buffer, sizeof(helper_buffer));
	char a[32], b[32];

	lcd.clear();

	if (isnan(LcdDataExchange.DS18B20_temperature))
		prstr = FPSTR("Inside:    NaN C");
	else {
		dtostrfd(LcdDataExchange.DS18B20_temperature, 1, a);
		prstr  = FPSTR("Inside:");
		for(byte i = prstr.length() + strlen(a) + 1; i <= 15; i++)
			prstr += ' ';
		prstr += a;
		prstr += 'C';
	}
	lcd.setCursor(0, 0);
	lcd.print(prstr.c_str());

	prstr.reset();
	if (isnan(LcdDataExchange.DHT22_temperature) ||
		isnan(LcdDataExchange.DHT22_humidity))
		prstr = FPSTR("Out: NaN%  NaN C");
	else {
		dtostrfd(LcdDataExchange.DHT22_temperature, 1, a);
		dtostrfd(LcdDataExchange.DHT22_humidity, 0, b);
		prstr  = FPSTR("Out: ");
		prstr += b;
		prstr += '%';
		for(byte i = prstr.length() + strlen(a) + 1; i <= 15; i++)
			prstr += ' ';
		prstr += a;
		prstr += 'C';
	}
	lcd.setCursor(0, 1);
	lcd.print(prstr.c_str());
}
#endif

void PerformEverySecond()
{
  if (blockgpio0) {
    blockgpio0--;
  }

  if (RtcTime.second < 3 &&
         Settings.enable_restart && Settings.restart_hour == RtcTime.hour &&
         Settings.restart_minute == RtcTime.minute &&
         (Settings.restart_weekdays & (1 << RtcTime.day_of_week)))
  {
       RtcSettings.oswatch_blocked_loop = 2;   
       RtcSettingsSave();
       ESP.reset();
  }

  for (byte i = 0; i < MAX_PULSETIMERS; i++) {
    if (pulse_timer[i] > 111) {
      pulse_timer[i]--;
    }
  }

  if (seriallog_timer) {
    seriallog_timer--;
    if (!seriallog_timer) {
      if (seriallog_level) {
        AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_APPLICATION D_SERIAL_LOGGING_DISABLED));
      }
      seriallog_level = 0;
    }
  }

  if (syslog_timer) {  // Restore syslog level
    syslog_timer--;
    if (!syslog_timer) {
      syslog_level = (Settings.flag.emulation) ? 0 : Settings.syslog_level;
      if (Settings.syslog_level) {
        AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_APPLICATION D_SYSLOG_LOGGING_REENABLED));  // Might trigger disable again (on purpose)
      }
    }
  }

#ifdef USE_DOMOTICZ
  DomoticzMqttUpdate();
#endif  // USE_DOMOTICZ

  if (status_update_timer) {
    status_update_timer--;
    if (!status_update_timer) {
      for (byte i = 1; i <= devices_present; i++) {
        MqttPublishPowerState(i);
      }
    }
  }

#ifdef TEMPERATURE_CONTROL
	  float current_temperature = NAN;
	  XsnsCall(FUNC_XSNS_READ, &current_temperature);
  	  ActThermoControl(current_temperature);
#endif

  if (Settings.tele_period) {
    tele_period++;
#ifdef LEDPIN_BLINK
	if (tele_period >= Settings.tele_period - lengthof(BlinkLed))
	{
		if (BlinkLedCurrent == NULL) {
			BlinkLedCurrent = BlinkLed;
		} else {
			BlinkLedCurrent++;
			if (BlinkLedCurrent - BlinkLed >= lengthof(BlinkLed)) {
				BlinkLedCurrent = NULL;
				for(uint8 i=0; i<lengthof(LedBlinkPins); i++)
					digitalWrite(LedBlinkPins[i], LOW);
			}
		}

		for(uint8 i=0; BlinkLedCurrent != NULL && i<lengthof(LedBlinkPins); i++) {
			if (BlinkLedCurrent->period[i] > 0) {
				BlinkLedCurrent->state[i] = BlinkLedCurrent->initstate[i];
				
				if (BlinkLedCurrent->initstate[i] > 0)
					digitalWrite(LedBlinkPins[i], HIGH);
				else
					digitalWrite(LedBlinkPins[i], LOW);
			} else
				digitalWrite(LedBlinkPins[i], LOW);
		}
	}
#endif
    if (tele_period == Settings.tele_period - 1) {
      XsnsCall(FUNC_XSNS_PREP, NULL);
    } else if (tele_period >= Settings.tele_period) {
      tele_period = 0;

/*
      mqtt_data[0] = '\0';
      MqttShowState();
      MqttPublishPrefixTopic_P(2, PSTR(D_RSLT_STATE));

      mqtt_data[0] = '\0';
      if (MqttShowSensor()) {
        MqttPublishPrefixTopic_P(2, PSTR(D_RSLT_SENSOR), Settings.flag.mqtt_sensor_retain);
#ifdef TEMPERATURE_CONTROL
  		ActThermoControl(current_temperature);
#endif
      }
*/
	  mqtt_msg.reset();

	  /* XXX Teodor */
	  XsnsCall(FUNC_XSNS_MQTT_SIMPLE, NULL);
#ifdef USE_LCD1602A
	  LCDPrint();
#endif
	  MqttPublishSimple_P(PSTR("time"), GetDateAndTime().c_str());
#ifdef TEMPERATURE_CONTROL 
	  if (!isnan(current_temperature))
	  	MqttPublishSimple_P(PSTR("temperature"), current_temperature);
	  if (RtcSettings.thermocontrol_duty_ratio >= 0.0 &&
		  RtcSettings.thermocontrol_duty_ratio <= 1.0)
	  	MqttPublishSimple_P(PSTR("ratio"), RtcSettings.thermocontrol_duty_ratio);
	  MqttPublishSimple_P(PSTR("destination"), Settings.destination_temperature); 
	  MqttPublishSimple_P(PSTR("delta"), Settings.delta_temperature);
#endif
	  MqttPublishSimple_P(PSTR("freemem"), (int)ESP.getFreeHeap()); 
	  MqttPublishSimple_P(PSTR("rssi"), WiFi.RSSI()); 
	  MqttPublishSimple_P(PSTR("wanip"), WiFi.localIP().toString().c_str()); 
	  MqttPublishSimple_P(PSTR("uptime"), uptime); 
	  MqttPublishSimple_P(PSTR("poweron"), bitRead(power, 0 /* device - 1 */ ) ? 1 : 0); 
	  MqttPublishSimple_P(PSTR("vcc"), (float)ESP.getVcc()/1000); 

      XsnsCall(FUNC_XSNS_MQTT_SHOW, NULL);
    }
  }

  if (hlw_flg) {
    HlwMarginCheck(mqtt_msg);
  }


  if ((2 == RtcTime.minute) && latest_uptime_flag) {
    latest_uptime_flag = false;
    uptime++;
    MqttPublishPrefixTopic_P(2, PSTR(D_RSLT_UPTIME), PSTR("{\"" D_TIME "\":\"%s\", \"" D_UPTIME "\":%d}"),
							 GetDateAndTime().c_str(), uptime);
  }
  if ((3 == RtcTime.minute) && !latest_uptime_flag) {
    latest_uptime_flag = true;
  }
}

/*********************************************************************************************\
 * Button handler with single press only or multi-press and hold on all buttons
\*********************************************************************************************/

void ButtonHandler()
{
  uint8_t button = NOT_PRESSED;
  uint8_t button_present = 0;
  char scmnd[20];

  uint8_t maxdev = (devices_present > MAX_KEYS) ? MAX_KEYS : devices_present;
  for (byte i = 0; i < maxdev; i++) {
    button = NOT_PRESSED;
    button_present = 0;

    if (!i && ((SONOFF_DUAL == Settings.module) || (CH4 == Settings.module))) {
      button_present = 1;
      if (dual_button_code) {
        AddLog_PP(LOG_LEVEL_DEBUG, PSTR(D_LOG_APPLICATION D_BUTTON " " D_CODE " %04X"), dual_button_code);
        button = PRESSED;
        if (0xF500 == dual_button_code) {                     // Button hold
          holdbutton[i] = (Settings.param[P_HOLD_TIME] * (STATES / 10)) -1;
        }
        dual_button_code = 0;
      }
    } else {
      if ((pin[GPIO_KEY1 +i] < 99) && !blockgpio0) {
        button_present = 1;
        button = digitalRead(pin[GPIO_KEY1 +i]);
      }
    }

    if (button_present) {
      if (SONOFF_4CHPRO == Settings.module) {
        if (holdbutton[i]) {
          holdbutton[i]--;
        }
        boolean button_pressed = false;
        if ((PRESSED == button) && (NOT_PRESSED == lastbutton[i])) {
          AddLog_PP(LOG_LEVEL_DEBUG, PSTR(D_LOG_APPLICATION D_BUTTON " %d " D_LEVEL_10), i +1);
          holdbutton[i] = STATES;
          button_pressed = true;
        }
        if ((NOT_PRESSED == button) && (PRESSED == lastbutton[i])) {
          AddLog_PP(LOG_LEVEL_DEBUG, PSTR(D_LOG_APPLICATION D_BUTTON " %d " D_LEVEL_01), i +1);
          if (!holdbutton[i]) {                           // Do not allow within 1 second
            button_pressed = true;
          }
        }
        if (button_pressed) {
          if (!send_button_power(0, i +1, 2)) {           // Execute Toggle command via MQTT if ButtonTopic is set
            ExecuteCommandPower(i +1, 2);                       // Execute Toggle command internally
          }
        }
      } else {
        if ((PRESSED == button) && (NOT_PRESSED == lastbutton[i])) {
          if (Settings.flag.button_single) {                // Allow only single button press for immediate action
            AddLog_PP(LOG_LEVEL_DEBUG, PSTR(D_LOG_APPLICATION D_BUTTON " %d " D_IMMEDIATE), i +1);
            if (!send_button_power(0, i +1, 2)) {         // Execute Toggle command via MQTT if ButtonTopic is set
              ExecuteCommandPower(i +1, 2);                     // Execute Toggle command internally
            }
          } else {
            multipress[i] = (multiwindow[i]) ? multipress[i] +1 : 1;
            AddLog_PP(LOG_LEVEL_DEBUG, PSTR(D_LOG_APPLICATION D_BUTTON " %d " D_MULTI_PRESS " %d"),
					  i +1, multipress[i]);
            multiwindow[i] = STATES /2;                   // 0.5 second multi press window
          }
          blinks = 201;
        }

        if (NOT_PRESSED == button) {
          holdbutton[i] = 0;
        } else {
          holdbutton[i]++;
          if (Settings.flag.button_single) {                // Allow only single button press for immediate action
            if (holdbutton[i] == Settings.param[P_HOLD_TIME] * (STATES / 10) * 4) {  // Button hold for four times longer
//              Settings.flag.button_single = 0;
              snprintf_P(scmnd, sizeof(scmnd), PSTR(D_CMND_SETOPTION "13 0"));  // Disable single press only
              ExecuteCommand(scmnd);
            }
          } else {
            if (holdbutton[i] == Settings.param[P_HOLD_TIME] * (STATES / 10)) {      // Button hold
              multipress[i] = 0;
              if (!Settings.flag.button_restrict) {         // No button restriction
                snprintf_P(scmnd, sizeof(scmnd), PSTR(D_CMND_RESET " 1"));
                ExecuteCommand(scmnd);
              } else {
                send_button_power(0, i +1, 3);            // Execute Hold command via MQTT if ButtonTopic is set
              }
            }
          }
        }

        if (!Settings.flag.button_single) {                 // Allow multi-press
          if (multiwindow[i]) {
            multiwindow[i]--;
          } else {
            if (!restart_flag && !holdbutton[i] && (multipress[i] > 0) && (multipress[i] < MAX_BUTTON_COMMANDS +3)) {
              boolean single_press = false;
              if (multipress[i] < 3) {                    // Single or Double press
                if ((SONOFF_DUAL == Settings.module) || (CH4 == Settings.module)) {
                  single_press = true;
                } else  {
                  single_press = (Settings.flag.button_swap +1 == multipress[i]);
                  multipress[i] = 1;
                }
              }
              if (single_press && send_button_power(0, i + multipress[i], 2)) {  // Execute Toggle command via MQTT if ButtonTopic is set
                // Success
              } else {
                if (multipress[i] < 3) {                  // Single or Double press
                  if (WifiState()) {                     // WPSconfig, Smartconfig or Wifimanager active
                    restart_flag = 1;
                  } else {
                    ExecuteCommandPower(i + multipress[i], 2);  // Execute Toggle command internally
                  }
                } else {                                  // 3 - 7 press
                  if (!Settings.flag.button_restrict) {
                    snprintf_P(scmnd, sizeof(scmnd), kCommands[multipress[i] -3]);
                    ExecuteCommand(scmnd);
                  }
                }
              }
              multipress[i] = 0;
            }
          }
        }
      }
    }
    lastbutton[i] = button;
  }
}

/*********************************************************************************************\
 * Switch handler
\*********************************************************************************************/

void SwitchHandler()
{
  uint8_t button = NOT_PRESSED;
  uint8_t switchflag;

  for (byte i = 0; i < MAX_SWITCHES; i++) {
    if (pin[GPIO_SWT1 +i] < 99) {

      if (holdwallswitch[i]) {
        holdwallswitch[i]--;
        if (0 == holdwallswitch[i]) {
          send_button_power(1, i +1, 3);         // Execute command via MQTT
        }
      }

      button = digitalRead(pin[GPIO_SWT1 +i]);
      if (button != lastwallswitch[i]) {
        switchflag = 3;
        switch (Settings.switchmode[i]) {
        case TOGGLE:
          switchflag = 2;                // Toggle
          break;
        case FOLLOW:
          switchflag = button &1;        // Follow wall switch state
          break;
        case FOLLOW_INV:
          switchflag = ~button &1;       // Follow inverted wall switch state
          break;
        case PUSHBUTTON:
          if ((PRESSED == button) && (NOT_PRESSED == lastwallswitch[i])) {
            switchflag = 2;              // Toggle with pushbutton to Gnd
          }
          break;
        case PUSHBUTTON_INV:
          if ((NOT_PRESSED == button) && (PRESSED == lastwallswitch[i])) {
            switchflag = 2;              // Toggle with releasing pushbutton from Gnd
          }
          break;
        case PUSHBUTTONHOLD:
          if ((PRESSED == button) && (NOT_PRESSED == lastwallswitch[i])) {
            holdwallswitch[i] = Settings.param[P_HOLD_TIME] * (STATES / 10);
          }
          if ((NOT_PRESSED == button) && (PRESSED == lastwallswitch[i]) && (holdwallswitch[i])) {
            holdwallswitch[i] = 0;
            switchflag = 2;              // Toggle with pushbutton to Gnd
          }
          break;
        case PUSHBUTTONHOLD_INV:
          if ((NOT_PRESSED == button) && (PRESSED == lastwallswitch[i])) {
            holdwallswitch[i] = Settings.param[P_HOLD_TIME] * (STATES / 10);
          }
          if ((PRESSED == button) && (NOT_PRESSED == lastwallswitch[i]) && (holdwallswitch[i])) {
            holdwallswitch[i] = 0;
            switchflag = 2;             // Toggle with pushbutton to Gnd
          }
          break;
        }

        if (switchflag < 3) {
          if (!send_button_power(1, i +1, switchflag)) {  // Execute command via MQTT
            ExecuteCommandPower(i +1, switchflag);              // Execute command internally (if i < devices_present)
          }
        }

        lastwallswitch[i] = button;
      }
    }
  }
}

#ifdef USE_OLED
uint32_t calculateCRC32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffff;
  while (length--) {
    uint8_t c = *data++;
    for (uint32_t i = 0x80; i > 0; i >>= 1) {
      bool bit = crc & 0x80000000;
      if (c & i) {
        bit = !bit;
      }
      crc <<= 1;
      if (bit) {
        crc ^= 0x04c11db7;
      }
    }
  }
  return crc;
}

void
SaveToDisplay() {
	uint32_t	crc;

	ToDisplay.crc = 0;
	crc = calculateCRC32((uint8_t*)&ToDisplay, sizeof(ToDisplay));
	ToDisplay.crc = crc;

	if (!ESP.rtcUserMemoryWrite(512 - sizeof(ToDisplay), (uint32_t*)&ToDisplay, sizeof(ToDisplay)))
		AddLog_P(LOG_LEVEL_INFO, PSTR("ESP.rtcUserMemoryWrite(ToDisplay) fails"));
}

void
LoadToDisplay() {
	uint32_t	crc;
	union {
		uint32_t	crc;
		uint8_t		data[sizeof(ToDisplay)];
	} buf;

	if (!ESP.rtcUserMemoryRead(512 - sizeof(ToDisplay), (uint32_t*)buf.data, sizeof(ToDisplay))) {
		AddLog_P(LOG_LEVEL_INFO, PSTR("ESP.rtcUserMemoryRead(ToDisplay) fails"));
		return;
	}

	crc = buf.crc;
	buf.crc = 0;

	if (calculateCRC32(buf.data, sizeof(ToDisplay)) != crc) {
		AddLog_P(LOG_LEVEL_INFO, PSTR("Wrong CRC of ToDisplay"));
		return;
	}

	memcpy(&ToDisplay, buf.data, sizeof(ToDisplay));

	/* recount collecting period */
	ToDisplay.firstT = millis();
	ToDisplay.lastT = ToDisplay.firstT + ToDisplay.tillLastT;
}

void
OledShowGraph(int fromX, int toX) {
#define GRAPHWIDTH	NDATAT
#define GRAPHHEIGHT	(40)
#define XNTICS	(5)
#define YNTICS	(6)
#define XTICLENGTH	(1)
#define YTICLENGTH	(3)
	int	leftBorder = fromX + (toX - fromX - GRAPHWIDTH) / 2;
	int	rightBorder = leftBorder + GRAPHWIDTH;
	int	upperBorder = 10 + 2;
	int	lowerBorder = 64 - (10 + 2);
	float	maxT = -1e6, minT = 1e6, pperT;
	int		i, rT;
	unsigned long	tic;
	BufferString prntval(helper_buffer, sizeof(helper_buffer));

	display.drawVerticalLine(toX, 0, 64);

	display.setFont(ArialMT_Plain_10);

	display.drawHorizontalLine(leftBorder, upperBorder, GRAPHWIDTH + 1); 
	display.drawHorizontalLine(leftBorder, lowerBorder, GRAPHWIDTH + 1);
	for(i=0; leftBorder > YTICLENGTH && i<YNTICS; i++)
	{
		display.drawHorizontalLine(leftBorder - YTICLENGTH - 1,
								   upperBorder + i * GRAPHHEIGHT / (YNTICS - 1),
								   YTICLENGTH); 
		display.drawHorizontalLine(rightBorder + 2,
								   upperBorder + i * GRAPHHEIGHT / (YNTICS - 1),
								   YTICLENGTH);

	}
	for(i=0; i<XNTICS; i++)
	{
		display.drawVerticalLine(leftBorder + i * GRAPHWIDTH / (XNTICS - 1),
								 upperBorder - 1,
								 XTICLENGTH);
		display.drawVerticalLine(leftBorder + i * GRAPHWIDTH / (XNTICS - 1),
								 lowerBorder + 1,
								 XTICLENGTH);
	}
	display.setTextAlignment(TEXT_ALIGN_LEFT);
	display.drawString(0, lowerBorder, "-1d");
	display.setTextAlignment(TEXT_ALIGN_RIGHT);

	tic = millis();

	if (ToDisplay.lastT < ToDisplay.firstT) {
		// wraparound lastT was caused in one of previous loop 

		if (tic < ToDisplay.firstT)
			// millis() was wrapped too
			ToDisplay.firstT = 0;
	}

	if (haveToReDraw & OLED_REDRAW_TEMP)
	{
		// check for both last and first to correct count around wraparound
		if (tic > ToDisplay.lastT && ToDisplay.lastT >= ToDisplay.firstT) {
			if (ToDisplay.countT > 0)
				ToDisplay.dataT[lengthof(ToDisplay.dataT) - 1] =
					ToDisplay.sumT / ((float)ToDisplay.countT);
			else
				ToDisplay.dataT[lengthof(ToDisplay.dataT) - 1] = NAN;

			for(i=1; i<lengthof(ToDisplay.dataT); i++)
				ToDisplay.dataT[i-1] = ToDisplay.dataT[i];

			ToDisplay.sumT = 0;
			ToDisplay.countT = 0;
			ToDisplay.firstT = tic;
			ToDisplay.lastT = ToDisplay.firstT + (24U * 3600U * 1000U) / NDATAT;

			if (ToDisplay.lastT < ToDisplay.firstT)
				ToDisplay.tillLastT = ToDisplay.lastT; // wrapped
			else
				ToDisplay.tillLastT = ToDisplay.lastT - tic;
		}

		if (!isnan(ToDisplay.temperature)) {
			ToDisplay.sumT += ToDisplay.temperature;
			ToDisplay.countT++;
			if (ToDisplay.countT == 1)
				SaveToDisplay();
		}
	}

	if (ToDisplay.countT > 0)
		ToDisplay.dataT[lengthof(ToDisplay.dataT) - 1] =
			ToDisplay.sumT / ((float)ToDisplay.countT);
	else
		ToDisplay.dataT[lengthof(ToDisplay.dataT) - 1] = NAN;

	for(i=lengthof(ToDisplay.dataT) - 1; i>=0 && !isnan(ToDisplay.dataT[i]); i--) {
		if (maxT < ToDisplay.dataT[i])
			maxT = ToDisplay.dataT[i];
		if (minT > ToDisplay.dataT[i])
			minT = ToDisplay.dataT[i];
	}

	if (maxT <= minT)
		return;

	rT = floorf(minT);
	while(rT != 0 && rT % 5)
		rT--;
	minT = rT;
	prntval.reset();
	prntval.sprintf_P(FPSTR("%dC"), rT);
	display.drawString(rightBorder, lowerBorder, prntval.c_str());

	rT = ceilf(maxT);
	while(rT != 0 && rT % 5)
		rT++;
	maxT = rT;
	prntval.reset();
	prntval.sprintf_P(FPSTR("%dC"), rT);
	display.drawString(rightBorder, 0, prntval.c_str());

	pperT = ((float)GRAPHHEIGHT) / (maxT - minT);

	for(i = lengthof(ToDisplay.dataT) - 2; i>=0 && !isnan(ToDisplay.dataT[i]); i--) {
		int y0 = (int)round(lowerBorder - (ToDisplay.dataT[i+1] - minT) * pperT),
			y1 = (int)round(lowerBorder - (ToDisplay.dataT[i+0] - minT) * pperT);

		display.drawLine(
			leftBorder + (i+1), y0,
			leftBorder + (i+0), y1
		);
	}
}

void
OledShow() {
	char x[32];
	int l;

	display.clear();
	display.setColor(WHITE);
	display.fillRect(0,0, 128, 64);
	display.setColor(BLACK);
	display.setTextAlignment(TEXT_ALIGN_RIGHT);

	if (!isnan(ToDisplay.temperature)) {
		dtostrfd(ToDisplay.temperature, 0, x);
		l = strlen(x);
		x[l] = 'C';
		x[l+1] = '\0';

		display.setFont(ArialMT_Plain_24);
		display.drawString(128, 0, x);
		display.drawCircle(112, 4, 2);
	}

	if (!isnan(ToDisplay.humidity)) {
		dtostrfd(ToDisplay.humidity, 0, x);
		l = strlen(x);
		x[l] = '%';
		x[l+1] = '\0';

		display.setFont(ArialMT_Plain_16);
		display.drawString(128, 27, x);
	}

	if (!isnan(ToDisplay.pressure)) {
		dtostrfd(ToDisplay.pressure, 0, x);
		l = strlen(x);
		x[l + 0] = 'm';
		x[l + 1] = 'm';
		x[l + 2] = '\0';

		display.setFont(ArialMT_Plain_16);
		display.drawString(128, 46, x);
	}

	OledShowGraph(0, 70);

	display.display();
}
#endif
/*********************************************************************************************\
 * State loop
\*********************************************************************************************/

void StateLoop()
{
  power_t power_now;

  if (state_loop_timer < 1000 / STATES)
	  state_loop_timer = millis();

  state_loop_timer += (1000 / STATES);
  state_loop_counter++;

  if (STATES == state_loop_counter) {
    PerformEverySecond();
    state_loop_counter = 0;
#ifdef USE_OLED
	if (haveToReDraw != OLED_REDRAW_NOTHING) {
		OledShow();
		haveToReDraw = OLED_REDRAW_NOTHING;
	}
#endif
  }

/*-------------------------------------------------------------------------------------------*\
 * Every 0.1 second
\*-------------------------------------------------------------------------------------------*/

  if (!(state_loop_counter % (STATES/10))) {

#ifdef LEDPIN_BLINK
	if (BlinkLedCurrent != NULL)
	{
		for(uint8 i = 0; i<lengthof(LedBlinkPins); i++)
		{
			if (BlinkLedCurrent->period[i] == 10)
			{
				digitalWrite(LedBlinkPins[i],
							 BlinkLedCurrent->state[i] ? HIGH : LOW);
				BlinkLedCurrent->state[i] = !BlinkLedCurrent->state[i];
			}
		}
	}
#endif

    if (mqtt_cmnd_publish) {
      mqtt_cmnd_publish--;  // Clean up
    }

    if (latching_relay_pulse) {
      latching_relay_pulse--;
      if (!latching_relay_pulse) {
        SetLatchingRelay(0, 0);
      }
    }

    for (byte i = 0; i < MAX_PULSETIMERS; i++) {
      if ((pulse_timer[i] > 0) && (pulse_timer[i] < 112)) {
        pulse_timer[i]--;
        if (!pulse_timer[i]) {
          ExecuteCommandPower(i +1, 0);
        }
      }
    }

    if (blink_mask) {
      blink_timer--;
      if (!blink_timer) {
        blink_timer = Settings.blinktime;
        blink_counter--;
        if (!blink_counter) {
          StopAllPowerBlink();
        } else {
          blink_power ^= 1;
          power_now = (power & (POWER_MASK ^ blink_mask)) | ((blink_power) ? blink_mask : 0);
          SetDevicePower(power_now);
        }
      }
    }

    // Backlog
    if (backlog_delay) {
      backlog_delay--;
    }
    if ((backlog_pointer != backlog_index) && !backlog_delay && !backlog_mutex) {
      backlog_mutex = 1;
      ExecuteCommand((char*)backlog[backlog_pointer].c_str());
      backlog_mutex = 0;
      backlog_pointer++;
/*
    if (backlog_pointer >= MAX_BACKLOG) {
      backlog_pointer = 0;
    }
*/
      backlog_pointer &= 0xF;
    }
  }

#ifdef USE_IR_REMOTE
#ifdef USE_IR_RECEIVE
  if (pin[GPIO_IRRECV] < 99) {
    IrReceiveCheck();  // check if there's anything on IR side
  }
#endif  // USE_IR_RECEIVE
#endif  // USE_IR_REMOTE

/*-------------------------------------------------------------------------------------------*\
 * Every 0.05 second
\*-------------------------------------------------------------------------------------------*/

  ButtonHandler();
  SwitchHandler();

  if (light_type) {
    LightAnimate();
  }

/*-------------------------------------------------------------------------------------------*\
 * Every 0.2 second
\*-------------------------------------------------------------------------------------------*/

  if (!(state_loop_counter % ((STATES/10)*2))) {
    if (blinks || restart_flag || ota_state_flag) {
      if (restart_flag || ota_state_flag) {
        blinkstate = 1;   // Stay lit
      } else {
        blinkstate ^= 1;  // Blink
      }
      if ((!(Settings.ledstate &0x08)) && ((Settings.ledstate &0x06) || (blinks > 200) || (blinkstate))) {
        SetLedPower(blinkstate);
      }
      if (!blinkstate) {
        blinks--;
        if (200 == blinks) {
          blinks = 0;
        }
      }
    } else {
      if (Settings.ledstate &1) {
        boolean tstate = power;
        if ((SONOFF_TOUCH == Settings.module) || (SONOFF_T11 == Settings.module) || (SONOFF_T12 == Settings.module) || (SONOFF_T13 == Settings.module)) {
          tstate = (!power) ? 1 : 0;
        }
        SetLedPower(tstate);
      }
    }
  }

/*-------------------------------------------------------------------------------------------*\
 * Every second at 0.2 second interval
\*-------------------------------------------------------------------------------------------*/

  switch (state_loop_counter) {
  case (STATES/10)*2:
    if (ota_state_flag && (backlog_pointer == backlog_index)) {
      ota_state_flag--;
      if (2 == ota_state_flag) {
        ota_retry_counter = OTA_ATTEMPTS;
        ESPhttpUpdate.rebootOnUpdate(false);
        SettingsSave(1);  // Free flash for OTA update
      }
      if (ota_state_flag <= 0) {
#ifdef USE_WEBSERVER
        if (Settings.webserver) {
          StopWebserver();
        }
#endif  // USE_WEBSERVER
        ota_state_flag = 92;
        ota_result = 0;
        ota_retry_counter--;
        if (ota_retry_counter) {
          ota_result = (HTTP_UPDATE_FAILED != ESPhttpUpdate.update(Settings.ota_url));
          if (!ota_result) {
            ota_state_flag = 2;
          }
        }
      }
      if (90 == ota_state_flag) {     // Allow MQTT to reconnect
        ota_state_flag = 0;
        if (ota_result) {
          SetFlashModeDout();  // Force DOUT for both ESP8266 and ESP8285
          mqtt_msg = FPSTR(D_SUCCESSFUL ". " D_RESTARTING);
        } else {
          mqtt_msg = ESPhttpUpdate.getLastErrorString().c_str();
        }
        restart_flag = 2;       // Restart anyway to keep memory clean webserver
        MqttPublishPrefixTopic_P(1, PSTR(D_CMND_UPGRADE));
      }
    }
    break;
  case (STATES/10)*4:
    if (MidnightNow()) {
      CounterSaveState();
    }
    if (save_data_counter && (backlog_pointer == backlog_index)) {
      save_data_counter--;
      if (save_data_counter <= 0) {
        if (Settings.flag.save_state) {
          power_t mask = POWER_MASK;
          for (byte i = 0; i < MAX_PULSETIMERS; i++) {
            if ((Settings.pulse_timer[i] > 0) && (Settings.pulse_timer[i] < 30)) {  // 3 seconds
              mask &= ~(1 << i);
            }
          }
          if (!((Settings.power &mask) == (power &mask))) {
            Settings.power = power;
          }
        } else {
          Settings.power = 0;
        }
        SettingsSave(0);
        save_data_counter = Settings.save_data;
      }
    }
    if (restart_flag && (backlog_pointer == backlog_index)) {
      if (211 == restart_flag) {
        SettingsDefault();
        restart_flag = 2;
      }
      if (212 == restart_flag) {
        SettingsErase();
        SettingsDefault();
        restart_flag = 2;
      }
      if (Settings.flag.save_state) {
        Settings.power = power;
      } else {
        Settings.power = 0;
      }
      if (hlw_flg) {
        HlwSaveState();
      }
      CounterSaveState();
      SettingsSave(0);
      restart_flag--;
      if (restart_flag <= 0) {
        AddLog_P(LOG_LEVEL_INFO, PSTR(D_LOG_APPLICATION D_RESTARTING));
        ESP.restart();
      }
    }
    break;
  case (STATES/10)*6:
    WifiCheck(wifi_state_flag);
    wifi_state_flag = WIFI_RESTART;
    break;
  case (STATES/10)*8:
    if (WL_CONNECTED == WiFi.status()) {
      if (Settings.flag.mqtt_enabled) {
        if (!MqttClient.connected()) {
          if (!mqtt_retry_counter) {
            MqttReconnect();
          } else {
            mqtt_retry_counter--;
          }
        }
      } else {
        if (!mqtt_retry_counter) {
         MqttReconnect();
        }
      }
    }
    break;
  }
}

/********************************************************************************************/

void SerialInput()
{
  while (Serial.available()) {
    yield();
    serial_in_byte = Serial.read();

/*-------------------------------------------------------------------------------------------*\
 * Sonoff dual 19200 baud serial interface
\*-------------------------------------------------------------------------------------------*/

    if (dual_hex_code) {
      dual_hex_code--;
      if (dual_hex_code) {
        dual_button_code = (dual_button_code << 8) | serial_in_byte;
        serial_in_byte = 0;
      } else {
        if (serial_in_byte != 0xA1) {
          dual_button_code = 0;                    // 0xA1 - End of Sonoff dual button code
        }
      }
    }
    if (0xA0 == serial_in_byte) {              // 0xA0 - Start of Sonoff dual button code
      serial_in_byte = 0;
      dual_button_code = 0;
      dual_hex_code = 3;
    }

/*-------------------------------------------------------------------------------------------*\
 * Sonoff bridge 19200 baud serial interface
\*-------------------------------------------------------------------------------------------*/

    if (SonoffBridgeSerialInput()) {
      serial_in_byte_counter = 0;
      Serial.flush();
      return;
    }

/*-------------------------------------------------------------------------------------------*/

    if (serial_in_byte > 127) {                // binary data...
      serial_in_byte_counter = 0;
      Serial.flush();
      return;
    }
    if (isprint(serial_in_byte)) {
      if (serial_in_byte_counter < INPUT_BUFFER_SIZE) {  // add char to string if it still fits
        serial_in_buffer[serial_in_byte_counter++] = serial_in_byte;
      } else {
        serial_in_byte_counter = 0;
      }
    }

/*-------------------------------------------------------------------------------------------*\
 * Sonoff SC 19200 baud serial interface
\*-------------------------------------------------------------------------------------------*/

    if (serial_in_byte == '\x1B') {            // Sonoff SC status from ATMEGA328P
      serial_in_buffer[serial_in_byte_counter] = 0;  // serial data completed
      SonoffScSerialInput(serial_in_buffer);
      serial_in_byte_counter = 0;
      Serial.flush();
      return;
    }

/*-------------------------------------------------------------------------------------------*/

    else if (serial_in_byte == '\n') {
      serial_in_buffer[serial_in_byte_counter] = 0;  // serial data completed
      seriallog_level = (Settings.seriallog_level < LOG_LEVEL_INFO) ? LOG_LEVEL_INFO : Settings.seriallog_level;
      AddLog_PP(LOG_LEVEL_INFO, PSTR(D_LOG_COMMAND "%s"), serial_in_buffer);
      ExecuteCommand(serial_in_buffer);
      serial_in_byte_counter = 0;
      Serial.flush();
      return;
    }
  }
}

/********************************************************************************************/

void GpioInit()
{
  uint8_t mpin;
  mytmplt def_module;

  if (!Settings.module || (Settings.module >= MAXMODULE)) {
    Settings.module = MODULE;
  }

  memcpy_P(&def_module, &kModules[Settings.module], sizeof(def_module));
  strlcpy(my_module.name, def_module.name, sizeof(my_module.name));
  for (byte i = 0; i < MAX_GPIO_PIN; i++) {
    if (Settings.my_gp.io[i] > GPIO_NONE) {
      my_module.gp.io[i] = Settings.my_gp.io[i];
    }
    if ((def_module.gp.io[i] > GPIO_NONE) && (def_module.gp.io[i] < GPIO_USER)) {
      my_module.gp.io[i] = def_module.gp.io[i];
    }
  }

  for (byte i = 0; i < GPIO_MAX; i++) {
    pin[i] = 99;
  }
  for (byte i = 0; i < MAX_GPIO_PIN; i++) {
    mpin = my_module.gp.io[i];

    if (mpin) {
      if ((mpin >= GPIO_REL1_INV) && (mpin < (GPIO_REL1_INV + MAX_RELAYS))) {
        bitSet(rel_inverted, mpin - GPIO_REL1_INV);
        mpin -= (GPIO_REL1_INV - GPIO_REL1);
      }
      else if ((mpin >= GPIO_LED1_INV) && (mpin < (GPIO_LED1_INV + MAX_LEDS))) {
        bitSet(led_inverted, mpin - GPIO_LED1_INV);
        mpin -= (GPIO_LED1_INV - GPIO_LED1);
      }
      else if ((mpin >= GPIO_PWM1_INV) && (mpin < (GPIO_PWM1_INV + MAX_PWMS))) {
        bitSet(pwm_inverted, mpin - GPIO_PWM1_INV);
        mpin -= (GPIO_PWM1_INV - GPIO_PWM1);
      }
#ifdef USE_DHT
      else if ((mpin >= GPIO_DHT11) && (mpin <= GPIO_DHT22)) {
        if (DhtSetup(i, mpin)) {
          dht_flg = 1;
          mpin = GPIO_DHT11;
        } else {
          mpin = 0;
        }
      }
#endif  // USE_DHT
    }
    if (mpin) {
      pin[mpin] = i;
    }
  }

  if (2 == pin[GPIO_TXD]) {
    Serial.set_tx(2);
  }

  analogWriteRange(Settings.pwm_range);      // Default is 1023 (Arduino.h)
  analogWriteFreq(Settings.pwm_frequency);   // Default is 1000 (core_esp8266_wiring_pwm.c)

#ifdef USE_I2C
  i2c_flg = ((pin[GPIO_I2C_SCL] < 99) && (pin[GPIO_I2C_SDA] < 99));
  if (i2c_flg) {
    Wire.begin(pin[GPIO_I2C_SDA], pin[GPIO_I2C_SCL]);
  }
#endif  // USE_I2C

  devices_present = 1;
  if (Settings.flag.pwm_control) {
    light_type = LT_BASIC;
    for (byte i = 0; i < MAX_PWMS; i++) {
      if (pin[GPIO_PWM1 +i] < 99) {
        light_type++;                        // Use Dimmer/Color control for all PWM as SetOption15 = 1
      }
    }
  }
  if (SONOFF_BRIDGE == Settings.module) {
    baudrate = 19200;
  }
  if (SONOFF_DUAL == Settings.module) {
    devices_present = 2;
    baudrate = 19200;
  }
  else if (CH4 == Settings.module) {
    devices_present = 4;
    baudrate = 19200;
  }
  else if (SONOFF_SC == Settings.module) {
    devices_present = 0;
    baudrate = 19200;
  }
  else if ((H801 == Settings.module) || (MAGICHOME == Settings.module) || (ARILUX == Settings.module)) {  // PWM RGBCW led
    if (!Settings.flag.pwm_control) {
      light_type = LT_BASIC;                 // Use basic PWM control if SetOption15 = 0
    }
  }
  else if (SONOFF_BN == Settings.module) {   // PWM Single color led (White)
    light_type = LT_PWM1;
  }
  else if (SONOFF_LED == Settings.module) {  // PWM Dual color led (White warm and cold)
    light_type = LT_PWM2;
  }
  else if (AILIGHT == Settings.module) {     // RGBW led
    light_type = LT_RGBW;
  }
  else if (SONOFF_B1 == Settings.module) {   // RGBWC led
    light_type = LT_RGBWC;
  }
  else {
    if (!light_type) {
      devices_present = 0;
    }
    for (byte i = 0; i < MAX_RELAYS; i++) {
      if (pin[GPIO_REL1 +i] < 99) {
        pinMode(pin[GPIO_REL1 +i], OUTPUT);
        devices_present++;
      }
    }
  }
  for (byte i = 0; i < MAX_KEYS; i++) {
    if (pin[GPIO_KEY1 +i] < 99) {
      pinMode(pin[GPIO_KEY1 +i], (16 == pin[GPIO_KEY1 +i]) ? INPUT_PULLDOWN_16 : INPUT_PULLUP);
    }
  }
  for (byte i = 0; i < MAX_LEDS; i++) {
    if (pin[GPIO_LED1 +i] < 99) {
      pinMode(pin[GPIO_LED1 +i], OUTPUT);
      digitalWrite(pin[GPIO_LED1 +i], bitRead(led_inverted, i));
    }
  }
  for (byte i = 0; i < MAX_SWITCHES; i++) {
    if (pin[GPIO_SWT1 +i] < 99) {
      pinMode(pin[GPIO_SWT1 +i], (16 == pin[GPIO_SWT1 +i]) ? INPUT_PULLDOWN_16 :INPUT_PULLUP);
      lastwallswitch[i] = digitalRead(pin[GPIO_SWT1 +i]);  // set global now so doesn't change the saved power state on first switch check
    }
  }

#ifdef USE_WS2812
  if (!light_type && (pin[GPIO_WS2812] < 99)) {  // RGB led
    devices_present++;
    light_type = LT_WS2812;
  }
#endif  // USE_WS2812
  if (light_type) {                           // Any Led light under Dimmer/Color control
    LightInit();
  } else {
    for (byte i = 0; i < MAX_PWMS; i++) {
      if (pin[GPIO_PWM1 +i] < 99) {
        pinMode(pin[GPIO_PWM1 +i], OUTPUT);
        analogWrite(pin[GPIO_PWM1 +i], bitRead(pwm_inverted, i) ? Settings.pwm_range - Settings.pwm_value[i] : Settings.pwm_value[i]);
      }
    }
  }

  if (EXS_RELAY == Settings.module) {
    SetLatchingRelay(0,2);
    SetLatchingRelay(1,2);
  }
  SetLedPower(Settings.ledstate &8);

#ifdef USE_IR_REMOTE
  if (pin[GPIO_IRSEND] < 99) {
    IrSendInit();
  }
#ifdef USE_IR_RECEIVE
  if (pin[GPIO_IRRECV] < 99) {
    IrReceiveInit();
  }
#endif  // USE_IR_RECEIVE
#endif  // USE_IR_REMOTE

  hlw_flg = ((pin[GPIO_HLW_SEL] < 99) && (pin[GPIO_HLW_CF1] < 99) && (pin[GPIO_HLW_CF] < 99));

  XSnsInit();
}

extern "C" {
extern struct rst_info resetInfo;
}

void setup()
{
  byte idx;

  Serial.begin(baudrate);
  delay(10);
  Serial.println();
  seriallog_level = LOG_LEVEL_INFO;  // Allow specific serial messages until config loaded

  snprintf_P(version, sizeof(version), PSTR("%d.%d.%d"), VERSION >> 24 & 0xff, VERSION >> 16 & 0xff, VERSION >> 8 & 0xff);
  if (VERSION & 0x1f) {
    idx = strlen(version);
    version[idx] = 96 + (VERSION & 0x1f);
    version[idx +1] = 0;
  }
  SettingsLoad();
  SettingsDelta();

  OsWatchInit();

  seriallog_level = Settings.seriallog_level;
  seriallog_timer = SERIALLOG_TIMER;
#ifndef USE_EMULATION
  Settings.flag.emulation = 0;
#endif  // USE_EMULATION
  syslog_level = (Settings.flag.emulation) ? 0 : Settings.syslog_level;
  stop_flash_rotate = Settings.flag.stop_flash_rotate;
  save_data_counter = Settings.save_data;
  sleep = Settings.sleep;

  Settings.bootcount++;
  AddLog_PP(LOG_LEVEL_DEBUG, PSTR(D_LOG_APPLICATION D_BOOT_COUNT " %d"), Settings.bootcount);

  GpioInit();

  if (Serial.baudRate() != baudrate) {
    if (seriallog_level) {
      AddLog_PP(LOG_LEVEL_INFO, PSTR(D_LOG_APPLICATION D_SET_BAUDRATE_TO " %d"), baudrate);
    }
    delay(100);
    Serial.flush();
    Serial.begin(baudrate);
    delay(10);
    Serial.println();
  }

  if (strstr(Settings.hostname, "%")) {
    strlcpy(Settings.hostname, WIFI_HOSTNAME, sizeof(Settings.hostname));
    snprintf_P(my_hostname, sizeof(my_hostname)-1, Settings.hostname, Settings.mqtt_topic, ESP.getChipId() & 0x1FFF);
  } else {
    snprintf_P(my_hostname, sizeof(my_hostname)-1, Settings.hostname);
  }
  WifiConnect();

  GetMqttClient(mqtt_client, Settings.mqtt_client, sizeof(mqtt_client));

  if (MOTOR == Settings.module) {
    Settings.poweronstate = 1;  // Needs always on else in limbo!
  }
  if (4 == Settings.poweronstate) {  // Allways on
    SetDevicePower(1);
  } else {
    if ((resetInfo.reason == REASON_DEFAULT_RST) || (resetInfo.reason == REASON_EXT_SYS_RST)) {
      switch (Settings.poweronstate) {
      case 0:  // All off
        power = 0;
        SetDevicePower(power);
        break;
      case 1:  // All on
        power = (1 << devices_present) -1;
        SetDevicePower(power);
        break;
      case 2:  // All saved state toggle
        power = Settings.power & (((1 << devices_present) -1) ^ POWER_MASK);
        if (Settings.flag.save_state) {
          SetDevicePower(power);
        }
        break;
      case 3:  // All saved state
        power = Settings.power & ((1 << devices_present) -1);
        if (Settings.flag.save_state) {
          SetDevicePower(power);
        }
        break;
      }
    } else {
      power = Settings.power & ((1 << devices_present) -1);
      if (Settings.flag.save_state) {
        SetDevicePower(power);
      }
    }
  }

  // Issue #526 and #909
  for (byte i = 0; i < devices_present; i++) {
    if ((i < MAX_RELAYS) && (pin[GPIO_REL1 +i] < 99)) {
      bitWrite(power, i, digitalRead(pin[GPIO_REL1 +i]) ^ bitRead(rel_inverted, i));
    }
    if ((i < MAX_PULSETIMERS) && bitRead(power, i)) {
      pulse_timer[i] = Settings.pulse_timer[i];
    }
  }

  blink_powersave = power;

  if (SONOFF_SC == Settings.module) {
    SonoffScInit();
  }

  RtcInit();

#ifdef USE_LCD1602A
	lcd.begin(16, 2);

	lcd.setCursor(0, 0);
	lcd.print("hello,          ");
	lcd.setCursor(0, 1);
	lcd.print("          world!");
#endif
#ifdef LEDPIN_BLINK
	for(uint8 i = 0; i<lengthof(LedBlinkPins); i++) {
		pinMode(LedBlinkPins[i], OUTPUT);
		digitalWrite(LedBlinkPins[i], LOW);
	}
#endif
#ifdef USE_OLED
	display.init();
	display.flipScreenVertically();
	haveToReDraw = OLED_REDRAW_OTHER;
#endif
  AddLog_PP(LOG_LEVEL_INFO, PSTR(D_PROJECT " %s %s (" D_CMND_TOPIC " %s, " D_FALLBACK " %s, " D_CMND_GROUPTOPIC " %s) " D_VERSION " %s"),
			PROJECT, Settings.friendlyname[0], Settings.mqtt_topic, mqtt_client, Settings.mqtt_grptopic, version);
}

void loop()
{
  OsWatchLoop();

#ifdef USE_WEBSERVER
  PollDnsWebserver();
#endif  // USE_WEBSERVER

#ifdef USE_EMULATION
  if (Settings.flag.emulation) {
    PollUdp();
  }
#endif  // USE_EMULATION

  if (millis() >= state_loop_timer) {
    StateLoop();
  }

  if (Settings.flag.mqtt_enabled) {
    MqttClient.loop();
  }
  if (Serial.available()){
    SerialInput();
  }

//  yield();     // yield == delay(0), delay contains yield, auto yield in loop
  delay(1);  // https://github.com/esp8266/Arduino/issues/2021
}

