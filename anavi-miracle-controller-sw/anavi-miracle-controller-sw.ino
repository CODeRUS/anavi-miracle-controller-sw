// -*- mode: c++; indent-tabs-mode: nil; c-file-style: "stroustrup" -*-
#include <FS.h>                   //this needs to be first, or it all crashes and burns...

// If HOME_ASSISTANT_DISCOVERY is defined, the Anavi Miracle Controller will
// publish MQTT messages that makes Home Assistant auto-discover the
// device.  See https://www.home-assistant.io/docs/mqtt/discovery/.
//
// This requires PubSubClient 2.7.

#define HOME_ASSISTANT_DISCOVERY 1

// If DEBUG is defined additional message will be printed in serial console
#undef DEBUG

// If PUBLISH_CHIP_ID is defined, the Anavi Miracle Controller will publish
// the chip ID using MQTT.  This can be considered a privacy issue,
// and is disabled by default.
#undef PUBLISH_CHIP_ID

// Should Over-the-Air upgrades be supported?  They are only supported
// if this define is set, and the user configures an OTA server on the
// wifi configuration page (or defines OTA_SERVER below).  If you use
// OTA you are strongly adviced to use signed builds (see
// https://arduino-esp8266.readthedocs.io/en/2.5.0/ota_updates/readme.html#advanced-security-signed-updates)
//
// You perform an OTA upgrade by publishing a MQTT command, like
// this:
//
//   mosquitto_pub -h mqtt-server.example.com \
//     -t cmnd/$MACHINEID/update \
//     -m '{"file": "/anavi.bin", "server": "www.example.com", "port": 8080 }'
//
// The port defaults to 80.
#define OTA_UPGRADES 1
// #define OTA_SERVER "www.example.com"

#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino
#include <ESP8266httpUpdate.h>

//needed for library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

#include <PubSubClient.h>        //https://github.com/knolleary/pubsubclient

#include <MD5Builder.h>
// For OLED display
#include <U8g2lib.h>
#include <Wire.h>
#include "Adafruit_HTU21DF.h"
#include "Adafruit_APDS9960.h"
// For BMP180
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP085_U.h>

// For FastLED
#define FASTLED_ESP8266_RAW_PIN_ORDER
#include <FastLED.h>

#define LED_PIN1    12
#define LED_PIN2    14
#define BRIGHTNESS  64
#define FRAMES_PER_SECOND  120

int numberLed1 = 10;
int numberLed2 = 10;

CRGB* leds1;
CRGB* leds2;

CRGB color1 = CRGB::Black;
CRGB color2 = CRGB::Black;
int brightnessLevel = 255;

// rotating "base color" used by many of the patterns
uint8_t gHue1 = 0;
uint8_t gHue2 = 0;

bool powerLed1 = true;
bool powerLed2 = true;
char effectLed1[32] = "rainbow";
char effectLed2[32] = "rainbow";

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

Adafruit_BMP085_Unified bmp = Adafruit_BMP085_Unified(10085);

Adafruit_HTU21DF htu = Adafruit_HTU21DF();

Adafruit_APDS9960 apds;

//Configure supported I2C sensors
const int sensorHTU21D =  0x40;
const int sensorBH1750 = 0x23;
const int sensorBMP180 = 0x77;
const int i2cDisplayAddress = 0x3c;

// Configure pins
const int pinAlarm = 16;
const int pinButton = 0;
const int pinExtra = 2;

unsigned long sensorPreviousMillis = 0;
const long sensorInterval = 10000;

unsigned long mqttConnectionPreviousMillis = millis();
const long mqttConnectionInterval = 60000;

float sensorTemperature = 0;
float sensorHumidity = 0;
uint16_t sensorAmbientLight = 0;

//define your default values here, if there are different values in config.json, they are overwritten.
char mqtt_server[40] = "iot.eclipse.org";
char mqtt_port[6] = "1883";
char workgroup[32] = "workgroup";
// MQTT username and password
char username[20] = "";
char password[20] = "";
// Configurations for number of LEDs in each strip
char configLed1[20] = "10";
char configLed2[20] = "10";
// LED type
char ledType[20] = "WS2812B";
// Color order of the LEDs: GRB, RGB
char ledColorOrder[20] = "GRB";
#ifdef HOME_ASSISTANT_DISCOVERY
char ha_name[32+1] = "";        // Make sure the machineId fits.
#endif
#ifdef OTA_UPGRADES
char ota_server[40];
#endif

// MD5 of chip ID.  If you only have a handful of miracle-controllers and use
// your own MQTT broker (instead of iot.eclips.org) you may want to
// truncate the MD5 by changing the 32 to a smaller value.
char machineId[32+1] = "";

//flag for saving data
bool shouldSaveConfig = false;

// MQTT
WiFiClient espClient;
PubSubClient mqttClient(espClient);

#ifdef OTA_UPGRADES
char cmnd_update_topic[12 + sizeof(machineId)];
#endif

char line1_topic[11 + sizeof(machineId)];
char line2_topic[11 + sizeof(machineId)];
char line3_topic[11 + sizeof(machineId)];
char cmnd_temp_coefficient_topic[14 + sizeof(machineId)];

char cmnd_led1_power_topic[49];
char cmnd_led2_power_topic[49];
char cmnd_led1_color_topic[49];
char cmnd_led2_color_topic[49];
char cmnd_reset_hue_topic[47];

char stat_led1_power_topic[50];
char stat_led2_power_topic[50];
char stat_led1_color_topic[50];
char stat_led2_color_topic[50];

// The display can fit 26 "i":s on a single line.  It will fit even
// less of other characters.
char mqtt_line1[26+1];
char mqtt_line2[26+1];
char mqtt_line3[26+1];

String sensor_line1;
String sensor_line2;
String sensor_line3;

bool need_redraw = true;

char stat_temp_coefficient_topic[14 + sizeof(machineId)];

//callback notifying us of the need to save config
void saveConfigCallback ()
{
    Serial.println("Should save config");
    shouldSaveConfig = true;
}

void drawDisplay(const char *line1, const char *line2 = "", const char *line3 = "", bool smallSize = false)
{
    // Write on OLED display
    // Clear the internal memory
    u8g2.clearBuffer();
    // Set appropriate font
    if ( true == smallSize)
    {
      u8g2.setFont(u8g2_font_ncenR10_tr);
      u8g2.drawStr(0,14, line1);
      u8g2.drawStr(0,39, line2);
      u8g2.drawStr(0,60, line3);
    }
    else
    {
      u8g2.setFont(u8g2_font_ncenR14_tr);
      u8g2.drawStr(0,14, line1);
      u8g2.drawStr(0,39, line2);
      u8g2.drawStr(0,64, line3);
    }
    // Transfer internal memory to the display
    u8g2.sendBuffer();
}

void checkDisplay()
{
    Serial.print("Mini I2C OLED Display at address ");
    Serial.print(i2cDisplayAddress, HEX);
    if (isSensorAvailable(i2cDisplayAddress))
    {
        Serial.println(": OK");
    }
    else
    {
        Serial.println(": N/A");
    }
}

void apWiFiCallback(WiFiManager *myWiFiManager)
{
    String configPortalSSID = myWiFiManager->getConfigPortalSSID();
    // Print information in the serial output
    Serial.print("Created access point for configuration: ");
    Serial.println(configPortalSSID);
    // Show information on the display
    String apId = configPortalSSID.substring(configPortalSSID.length()-5);
    String configHelper("AP ID: "+apId);
    drawDisplay("Miracle Controller", "Please configure", configHelper.c_str(), true);
}

void determineLedType()
{
    String configuredType(ledType);
    if (configuredType.equalsIgnoreCase("WS2812"))
    {
#define LED_TYPE    WS2812
        configuredType = "WS2812";
    }
    else if (configuredType.equalsIgnoreCase("WS2811"))
    {
#define LED_TYPE    WS2811
        configuredType = "WS2811";
    }
    else if (configuredType.equalsIgnoreCase("TM1804"))
    {
#define LED_TYPE    TM1804
        configuredType = "TM1804";
    }
    else if (configuredType.equalsIgnoreCase("NEOPIXEL"))
    {
#define LED_TYPE    NEOPIXEL
        configuredType = "NEOPIXEL";
    }
    else
    {
#define LED_TYPE    WS2812B
        configuredType = "WS2812B";
    }
    Serial.print("LED type: ");
    Serial.println(configuredType);
}

void determineLedColorOrder()
{
    String configuredColorOrder(ledColorOrder);
    if (configuredColorOrder.equalsIgnoreCase("RGB"))
    {
#define COLOR_ORDER RGB
        configuredColorOrder = "RGB";
    }
    else if (configuredColorOrder.equalsIgnoreCase("BRG"))
    {
#define COLOR_ORDER BRG
        configuredColorOrder = "BRG";
    }
    else
    {
#define COLOR_ORDER GRB
        configuredColorOrder = "GRB";
    }
    Serial.print("LED color order: ");
    Serial.println(configuredColorOrder);
}

void setup()
{
    // put your setup code here, to run once:
    strcpy(mqtt_line1, "");
    strcpy(mqtt_line2, "");
    strcpy(mqtt_line3, "");
    Serial.begin(115200);
    Serial.println();

    Wire.begin();
    checkDisplay();

    u8g2.begin();

    delay(10);

    //LED
    pinMode(pinAlarm, OUTPUT);
    //Button
    pinMode(pinButton, INPUT);
    // Set the extra pin to low in output mode
    pinMode(pinExtra, OUTPUT);
    digitalWrite(pinExtra, LOW);

    drawDisplay("ANAVI", "Miracle Controller", "Loading...", true);

    // Power-up safety delay and a chance for resetting the board
    waitForFactoryReset();

    // Machine ID
    calculateMachineId();

    // Set MQTT topics
    sprintf(cmnd_led1_power_topic, "cmnd/%s/led1/power", machineId);
    sprintf(cmnd_led1_color_topic, "cmnd/%s/led1/color", machineId);
    sprintf(cmnd_led2_power_topic, "cmnd/%s/led2/power", machineId);
    sprintf(cmnd_led2_color_topic, "cmnd/%s/led2/color", machineId);
    sprintf(cmnd_reset_hue_topic, "cmnd/%s/resethue", machineId);

    sprintf(stat_led1_power_topic, "stat/%s/led1/power", machineId);
    sprintf(stat_led2_power_topic, "stat/%s/led2/power", machineId);
    sprintf(stat_led1_color_topic, "stat/%s/led1/color", machineId);
    sprintf(stat_led2_color_topic, "stat/%s/led2/color", machineId);

    //read configuration from FS json
    Serial.println("mounting FS...");

    if (SPIFFS.begin())
    {
        Serial.println("mounted file system");
        if (SPIFFS.exists("/config.json")) {
            //file exists, reading and loading
            Serial.println("reading config file");
            File configFile = SPIFFS.open("/config.json", "r");
            if (configFile)
            {
                Serial.println("opened config file");
                const size_t size = configFile.size();
                // Allocate a buffer to store contents of the file.
                std::unique_ptr<char[]> buf(new char[size]);

                configFile.readBytes(buf.get(), size);
                DynamicJsonDocument json(1024);
                if (DeserializationError::Ok == deserializeJson(json, buf.get()))
                {
#ifdef DEBUG
                    // Content stored in the memory of the microcontroller contains
                    // sensitive data such as username and passwords therefore
                    // should be printed only during debugging
                    serializeJson(json, Serial);
                    Serial.println("\nparsed json");
#endif

                    strcpy(mqtt_server, json["mqtt_server"]);
                    strcpy(mqtt_port, json["mqtt_port"]);
                    strcpy(workgroup, json["workgroup"]);
                    strcpy(username, json["username"]);
                    strcpy(password, json["password"]);
                    strcpy(ledType, json["led_type"]);
                    strcpy(ledColorOrder, json["led_color_order"]);

                    numberLed1 = json["configLed1"];
                    numberLed2 = json["configLed2"];

#ifdef HOME_ASSISTANT_DISCOVERY
                    {
                        const char *s = json["ha_name"];
                        if (!s)
                            s = machineId;
                        snprintf(ha_name, sizeof(ha_name), "%s", s);
                    }
#endif
#ifdef OTA_UPGRADES
                    {
                        const char *s = json["ota_server"];
                        if (!s)
                            s = ""; // The empty string never matches.
                        snprintf(ota_server, sizeof(ota_server), "%s", s);
                    }
#endif
                }
                else
                {
                    Serial.println("failed to load json config");
                }
            }
        }
    }
    else
    {
        Serial.println("failed to mount FS");
    }
    //end read

    // Set MQTT topics
    sprintf(line1_topic, "cmnd/%s/line1", machineId);
    sprintf(line2_topic, "cmnd/%s/line2", machineId);
    sprintf(line3_topic, "cmnd/%s/line3", machineId);
    sprintf(cmnd_temp_coefficient_topic, "cmnd/%s/tempcoef", machineId);
    sprintf(stat_temp_coefficient_topic, "stat/%s/tempcoef", machineId);
#ifdef OTA_UPGRADES
    sprintf(cmnd_update_topic, "cmnd/%s/update", machineId);
#endif

    // The extra parameters to be configured (can be either global or just in the setup)
    // After connecting, parameter.getValue() will get you the configured value
    // id/name placeholder/prompt default length
    WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, sizeof(mqtt_server));
    WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, sizeof(mqtt_port));
    WiFiManagerParameter custom_workgroup("workgroup", "workgroup", workgroup, sizeof(workgroup));
    WiFiManagerParameter custom_mqtt_user("user", "MQTT username", username, sizeof(username));
    WiFiManagerParameter custom_mqtt_pass("pass", "MQTT password", password, sizeof(password));
    WiFiManagerParameter custom_led_type("ledType", "WS2812B", ledType, sizeof(ledType));
    WiFiManagerParameter custom_led_color_order("ledColorOrder", "GRB", ledColorOrder, sizeof(ledColorOrder));
    WiFiManagerParameter custom_led1("led1", "LED1", configLed1, sizeof(configLed1));
    WiFiManagerParameter custom_led2("led2", "LED2", configLed2, sizeof(configLed2));
#ifdef HOME_ASSISTANT_DISCOVERY
    WiFiManagerParameter custom_mqtt_ha_name("ha_name", "Device name for Home Assistant", ha_name, sizeof(ha_name));
#endif
#ifdef OTA_UPGRADES
    WiFiManagerParameter custom_ota_server("ota_server", "OTA server", ota_server, sizeof(ota_server));
#endif

    char htmlMachineId[200];
    sprintf(htmlMachineId,"<p style=\"color: red;\">Machine ID:</p><p><b>%s</b></p><p>Copy and save the machine ID because you will need it to control the device.</p>", machineId);
    WiFiManagerParameter custom_text_machine_id(htmlMachineId);

    //WiFiManager
    //Local intialization. Once its business is done, there is no need to keep it around
    WiFiManager wifiManager;

    //set config save notify callback
    wifiManager.setSaveConfigCallback(saveConfigCallback);

    //add all your parameters here
    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_port);
    wifiManager.addParameter(&custom_workgroup);
    wifiManager.addParameter(&custom_mqtt_user);
    wifiManager.addParameter(&custom_mqtt_pass);
    wifiManager.addParameter(&custom_led_type);
    wifiManager.addParameter(&custom_led_color_order);
    wifiManager.addParameter(&custom_led1);
    wifiManager.addParameter(&custom_led2);
#ifdef HOME_ASSISTANT_DISCOVERY
    wifiManager.addParameter(&custom_mqtt_ha_name);
#endif
#ifdef OTA_UPGRADES
    wifiManager.addParameter(&custom_ota_server);
#endif
    wifiManager.addParameter(&custom_text_machine_id);

    //reset settings - for testing
    //wifiManager.resetSettings();

    //set minimu quality of signal so it ignores AP's under that quality
    //defaults to 8%
    //wifiManager.setMinimumSignalQuality();

    //sets timeout until configuration portal gets turned off
    //useful to make it all retry or go to sleep
    //in seconds
    wifiManager.setTimeout(300);

    digitalWrite(pinAlarm, HIGH);
    drawDisplay("Connecting...", WiFi.SSID().c_str());

    //fetches ssid and pass and tries to connect
    //if it does not connect it starts an access point
    //and goes into a blocking loop awaiting configuration
    wifiManager.setAPCallback(apWiFiCallback);
    // Append the last 5 character of the machine id to the access point name
    String apId(machineId);
    apId = apId.substring(apId.length() - 5);
    String accessPointName = "ANAVI Miracle Controller " + apId;
    if (!wifiManager.autoConnect(accessPointName.c_str(), ""))
    {
        digitalWrite(pinAlarm, LOW);
        Serial.println("failed to connect and hit timeout");
        delay(3000);
        //reset and try again, or maybe put it to deep sleep
        ESP.reset();
        delay(5000);
    }

    //if you get here you have connected to the WiFi
    Serial.println("connected...yeey :)");
    digitalWrite(pinAlarm, LOW);

    //read updated parameters
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(workgroup, custom_workgroup.getValue());
    strcpy(username, custom_mqtt_user.getValue());
    strcpy(password, custom_mqtt_pass.getValue());
    strcpy(ledType, custom_led_type.getValue());
    strcpy(ledColorOrder, custom_led_color_order.getValue());
    int saveLed1 = atoi(custom_led1.getValue());
    if (0 > saveLed1)
    {
      saveLed1 = 10;
    }
    int saveLed2 = atoi(custom_led2.getValue());
    if (0 > saveLed2)
    {
      saveLed2 = 10;
    }

#ifdef HOME_ASSISTANT_DISCOVERY
    strcpy(ha_name, custom_mqtt_ha_name.getValue());
#endif
#ifdef OTA_UPGRADES
    strcpy(ota_server, custom_ota_server.getValue());
#endif

    //save the custom parameters to FS
    if (shouldSaveConfig)
    {
        Serial.println("saving config");
        DynamicJsonDocument json(1024);
        json["mqtt_server"] = mqtt_server;
        json["mqtt_port"] = mqtt_port;
        json["workgroup"] = workgroup;
        json["username"] = username;
        json["password"] = password;
        json["led_type"] = ledType;
        json["led_color_order"] = ledColorOrder;
        numberLed1 = saveLed1;
        numberLed2 = saveLed2;
        json["configLed1"] = numberLed1;
        json["configLed2"] = numberLed2;
#ifdef HOME_ASSISTANT_DISCOVERY
        json["ha_name"] = ha_name;
#endif
#ifdef OTA_UPGRADES
        json["ota_server"] = ota_server;
#endif

        File configFile = SPIFFS.open("/config.json", "w");
        if (!configFile)
        {
            Serial.println("failed to open config file for writing");
        }

        serializeJson(json, Serial);
        Serial.println("");
        serializeJson(json, configFile);
        configFile.close();
        //end save
    }

    // Dynamically assign number of LEDs in both strips
    // This object will be used during the whole tile while the Arduino sketch
    // is running therefore delete is not invoked
    leds1 = new CRGB[numberLed1];
    leds2 = new CRGB[numberLed2];
    // Determine LED type and color order for both strips based on the saved configuration
    determineLedType();
    determineLedColorOrder();

    FastLED.addLeds<LED_TYPE, LED_PIN1, COLOR_ORDER>(leds1, numberLed1).setCorrection( TypicalLEDStrip );
    FastLED.addLeds<LED_TYPE, LED_PIN2, COLOR_ORDER>(leds2, numberLed2).setCorrection( TypicalLEDStrip );
    FastLED.setBrightness(  BRIGHTNESS );
    // Turn on lights
    rainbow(leds1, gHue1, numberLed1);
    rainbow(leds2, gHue2, numberLed2);
    FastLED.show();
    delay(100);
    Serial.println("LED strips have been initialized!");

    Serial.println("local ip");
    Serial.println(WiFi.localIP());
    drawDisplay("Connected!", "Local IP:", WiFi.localIP().toString().c_str());
    delay(2000);

    // Sensors
    htu.begin();
    bmp.begin();

    // MQTT
    Serial.print("MQTT Server: ");
    Serial.println(mqtt_server);
    Serial.print("MQTT Port: ");
    Serial.println(mqtt_port);
    // Print MQTT Username
    Serial.print("MQTT Username: ");
    Serial.println(username);
    // Hide password from the log and show * instead
    char hiddenpass[20] = "";
    for (size_t charP=0; charP < strlen(password); charP++)
    {
        hiddenpass[charP] = '*';
    }
    hiddenpass[strlen(password)] = '\0';
    Serial.print("MQTT Password: ");
    Serial.println(hiddenpass);
#ifdef HOME_ASSISTANT_DISCOVERY
    Serial.print("Home Assistant sensor name: ");
    Serial.println(ha_name);
#endif
#ifdef OTA_UPGRADES
    if (ota_server[0] != '\0')
    {
        Serial.print("OTA server: ");
        Serial.println(ota_server);
    }
    else
    {
#  ifndef OTA_SERVER
        Serial.println("No OTA server");
#  endif
    }

#  ifdef OTA_SERVER
    Serial.print("Hardcoded OTA server: ");
    Serial.println(OTA_SERVER);
#  endif

#endif

    const int mqttPort = atoi(mqtt_port);
    mqttClient.setServer(mqtt_server, mqttPort);
    mqttClient.setCallback(mqttCallback);

    mqttReconnect();

    Serial.println("");
    Serial.println("-----");
    Serial.print("Machine ID: ");
    Serial.println(machineId);
    Serial.println("-----");
    Serial.println("");

    setupADPS9960();
}

void setupADPS9960()
{
    if(apds.begin())
    {
        //gesture mode will be entered once proximity mode senses something close
        apds.enableProximity(true);
        apds.enableGesture(true);
    }
}

void waitForFactoryReset()
{
    Serial.println("Press button within 4 seconds for factory reset...");
    for (int iter = 0; iter < 40; iter++)
    {
        digitalWrite(pinAlarm, HIGH);
        delay(50);
        if (false == digitalRead(pinButton))
        {
            factoryReset();
            return;
        }
        digitalWrite(pinAlarm, LOW);
        delay(50);
        if (false == digitalRead(pinButton))
        {
            factoryReset();
            return;
        }
    }
}

void factoryReset()
{
    if (false == digitalRead(pinButton))
    {
        Serial.println("Hold the button to reset to factory defaults...");
        bool cancel = false;
        for (int iter=0; iter<30; iter++)
        {
            digitalWrite(pinAlarm, HIGH);
            delay(100);
            if (true == digitalRead(pinButton))
            {
                cancel = true;
                break;
            }
            digitalWrite(pinAlarm, LOW);
            delay(100);
            if (true == digitalRead(pinButton))
            {
                cancel = true;
                break;
            }
        }
        if (false == digitalRead(pinButton) && !cancel)
        {
            digitalWrite(pinAlarm, HIGH);
            Serial.println("Disconnecting...");
            WiFi.disconnect();

            // NOTE: the boot mode:(1,7) problem is known and only happens at the first restart after serial flashing.

            Serial.println("Restarting...");
            // Clean the file system with configurations
            SPIFFS.format();
            // Restart the board
            ESP.restart();
        }
        else
        {
            // Cancel reset to factory defaults
            Serial.println("Reset to factory defaults cancelled.");
            digitalWrite(pinAlarm, LOW);
        }
    }
}

#ifdef OTA_UPGRADES
void do_ota_upgrade(char *text)
{
    DynamicJsonDocument json(1024);
    auto error = deserializeJson(json, text);
    if (error)
    {
        Serial.println("No success decoding JSON.\n");
    }
    else if (!json["server"])
    {
        Serial.println("JSON is missing server\n");
    }
    else if (!json["file"])
    {
        Serial.println("JSON is missing file\n");
    }
    else
    {
        int port = 0;
        if (json.containsKey("port"))
        {
            port = json["port"];
            Serial.print("Port configured to ");
            Serial.println(port);
        }

        if (0 >= port || 65535 < port)
        {
            port = 80;
        }

        String server = json["server"];
        String file = json["file"];

        bool ok = false;
        if (ota_server[0] != '\0' && !strcmp(server.c_str(), ota_server))
            ok = true;

#  ifdef OTA_SERVER
        if (!strcmp(server.c_str(), OTA_SERVER))
            ok = true;
#  endif

        if (!ok)
        {
            Serial.println("Wrong OTA server. Refusing to upgrade.");
            return;
        }

        Serial.print("Attempting to upgrade from ");
        Serial.print(server);
        Serial.print(":");
        Serial.print(port);
        Serial.println(file);
        ESPhttpUpdate.setLedPin(pinAlarm, HIGH);
        WiFiClient update_client;
        t_httpUpdate_return ret = ESPhttpUpdate.update(update_client,
                                                       server, port, file);
        switch (ret)
        {
        case HTTP_UPDATE_FAILED:
            Serial.printf("HTTP_UPDATE_FAILD Error (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
            break;

        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("HTTP_UPDATE_NO_UPDATES");
            break;

        case HTTP_UPDATE_OK:
            Serial.println("HTTP_UPDATE_OK");
            break;
        }
    }
}
#endif

void printLedStatus()
{
    sensor_line1 = "1: ";
    sensor_line1 += effectLed1;
    Serial.println(sensor_line1);
    sensor_line2 = "2: ";
    sensor_line2 += effectLed2;
    Serial.println(sensor_line2);
}

void convertColors(StaticJsonDocument<200> data, CRGB& color, uint8_t& hue)
{
    const uint8_t r = data["color"]["r"];
    const uint8_t g = data["color"]["g"];
    const uint8_t b = data["color"]["b"];
    setColors(r, g, b, color, hue);
}

void convertBrightness(StaticJsonDocument<200> data, CRGB& color, uint8_t& hue)
{
    const uint8_t brightness = data["brightness"];
    setColors(brightness, brightness, brightness, color, hue);
}

void setColors(uint8_t r, uint8_t g, uint8_t b, CRGB& color, uint8_t& hue)
{
    color.setRGB(r, g, b);
    // Calculate hue
    CHSV hc = rgb2hsv_approximate(color);
    hue = hc.hue;
}

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    // Convert received bytes to a string
    char text[length + 1];
    snprintf(text, length + 1, "%s", payload);

    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    Serial.println(text);

    if (strcmp(topic, cmnd_led1_power_topic) == 0)
    {
        powerLed1 = strcmp(text, "ON") == 0;
        publishState(1);
    }
    else if (strcmp(topic, cmnd_led1_color_topic) == 0)
    {
        StaticJsonDocument<200> data;
        deserializeJson(data, text);

        // restart hue
        gHue1 = 0;

        // LED1

        if (data.containsKey("state"))
        {
            // Set variable power to true or false depending on the state
            powerLed1 = (data["state"] == "ON");
            // Set some default values
            color1 = CRGB::Red;
            gHue1 = 0;
            if (true == powerLed1)
            {
              strcpy(effectLed1, "solid");
            }
            else
            {
              strcpy(effectLed1, "none");
            }
        }

        if (data.containsKey("color"))
        {
            convertColors(data, color1, gHue1);
        }
        else if (data.containsKey("brightness"))
        {
            convertBrightness(data, color1, gHue1);
        }

        if (data.containsKey("effect"))
        {
            if (0 != strcmp(effectLed1, data["effect"]))
            {
                strcpy(effectLed1, data["effect"]);
                printLedStatus();
            }
        }
        // Update the content on the display
        need_redraw = true;

        publishState(1);
    }

    // LED2

    if (strcmp(topic, cmnd_led2_power_topic) == 0)
    {
        powerLed2 = strcmp(text, "ON") == 0;
        publishState(2);
    }
    else if (strcmp(topic, cmnd_led2_color_topic) == 0)
    {
        StaticJsonDocument<200> data;
        deserializeJson(data, text);

        // restart hue
        gHue2 = 0;

        if (data.containsKey("state"))
        {
            // Set variable power to true or false depending on the state
            powerLed2 = (data["state"] == "ON");
            // Set some default values
            color2 = CRGB::Red;
            gHue2 = 0;
            if (true == powerLed2)
            {
              strcpy(effectLed2, "solid");
            }
            else
            {
              strcpy(effectLed2, "none");
            }
        }

        if (data.containsKey("color"))
        {
            convertColors(data, color2, gHue2);
        }
        else if (data.containsKey("brightness"))
        {
            convertBrightness(data, color2, gHue2);
        }

        if (data.containsKey("effect"))
        {
            if (0 != strcmp(effectLed2, data["effect"]))
            {
                strcpy(effectLed2, data["effect"]);
                printLedStatus();
            }
        }
        // Update the content on the display
        need_redraw = true;

        publishState(2);
    }

    if (false == powerLed1)
    {
        strcpy(effectLed1, "off");
        need_redraw = true;
    }

    if (false == powerLed2)
    {
        strcpy(effectLed2, "off");
        need_redraw = true;
    }

    if (strcmp(topic, cmnd_reset_hue_topic) == 0)
    {
        // Reset hue and this was sync colors of both animation
        gHue1 = 0;
        gHue2 = 0;
    }

    if (strcmp(topic, line1_topic) == 0)
    {
        snprintf(mqtt_line1, sizeof(mqtt_line1), "%s", text);
        need_redraw = true;
    }

    if (strcmp(topic, line2_topic) == 0)
    {
        snprintf(mqtt_line2, sizeof(mqtt_line2), "%s", text);
        need_redraw = true;
    }

    if (strcmp(topic, line3_topic) == 0)
    {
        snprintf(mqtt_line3, sizeof(mqtt_line3), "%s", text);
        need_redraw = true;
    }

#ifdef OTA_UPGRADES
    if (strcmp(topic, cmnd_update_topic) == 0)
    {
        Serial.println("OTA request seen.\n");
        do_ota_upgrade(text);
        // Any OTA upgrade will stop the mqtt client, so if the
        // upgrade failed and we get here publishState() will fail.
        // Just return here, and we will reconnect from within the
        // loop().
        return;
    }
#endif
}

void calculateMachineId()
{
    MD5Builder md5;
    md5.begin();
    char chipId[25];
    sprintf(chipId,"%d",ESP.getChipId());
    md5.add(chipId);
    md5.calculate();
    md5.toString().toCharArray(machineId, sizeof(machineId));
}

void mqttReconnect()
{
    char clientId[18 + sizeof(machineId)];
    snprintf(clientId, sizeof(clientId), "anavi-miracle-controller-%s", machineId);

    // Loop until we're reconnected
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        Serial.print("Attempting MQTT connection...");
        // Attempt to connect
        if (true == mqttClient.connect(clientId, username, password))
        {
            Serial.println("connected");

            // Subscribe to MQTT topics

            // LED1
            mqttClient.subscribe(cmnd_led1_power_topic);
            mqttClient.subscribe(cmnd_led1_color_topic);
            // LED2
            mqttClient.subscribe(cmnd_led2_power_topic);
            mqttClient.subscribe(cmnd_led2_color_topic);
            // Topic to reset hue
            mqttClient.subscribe(cmnd_reset_hue_topic);

            mqttClient.subscribe(line1_topic);
            mqttClient.subscribe(line2_topic);
            mqttClient.subscribe(line3_topic);
            mqttClient.subscribe(cmnd_temp_coefficient_topic);
#ifdef OTA_UPGRADES
            mqttClient.subscribe(cmnd_update_topic);
#endif

#ifdef HOME_ASSISTANT_DISCOVERY
            // Publish discovery messages
            publishDiscoveryState();
#endif

            // Publish initial status of both LED strips
            publishState(1);
            publishState(2);
            break;

        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" try again in 5 seconds");
            // Wait 5 seconds before retrying
            delay(5000);
        }
    }
}

#ifdef HOME_ASSISTANT_DISCOVERY

bool publishSensorDiscovery(const char *config_key,
                            const char *device_class,
                            const char *name_suffix,
                            const char *state_topic,
                            const char *unit,
                            const char *value_template)
{
    static char topic[48 + sizeof(machineId)];

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/%s/config", machineId, config_key);

    DynamicJsonDocument json(1024);
    json["device_class"] = device_class;
    json["name"] = String(ha_name) + " " + name_suffix;
    json["unique_id"] = String("anavi-") + machineId + "-" + config_key;
    json["state_topic"] = String(workgroup) + "/" + machineId + state_topic;
    json["unit_of_measurement"] = unit;
    json["value_template"] = value_template;

    json["device"]["identifiers"] = machineId;
    json["device"]["manufacturer"] = "ANAVI Technology";
    json["device"]["model"] = "ANAVI Miracle Controller";
    json["device"]["name"] = ha_name;
    json["device"]["sw_version"] = ESP.getSketchMD5();

    JsonArray connections = json["device"].createNestedArray("connections").createNestedArray();
    connections.add("mac");
    connections.add(WiFi.macAddress());

    Serial.print("Home Assistant discovery topic: ");
    Serial.println(topic);

    int payload_len = measureJson(json);
    if (!mqttClient.beginPublish(topic, payload_len, true))
    {
        Serial.println("beginPublish failed!\n");
        return false;
    }

    if (serializeJson(json, mqttClient) != payload_len)
    {
        Serial.println("writing payload: wrong size!\n");
        return false;
    }

    if (!mqttClient.endPublish())
    {
        Serial.println("endPublish failed!\n");
        return false;
    }

    return true;
}

bool publishLightDiscovery(int ledId)
{
    DynamicJsonDocument json(1024);

    static char topic[48 + sizeof(machineId)];
    snprintf(topic, sizeof(topic),
             "homeassistant/light/%s/led%d/config",  machineId, ledId);

    json["schema"] = "json";
    json["brightness"] = true;
    json["rgb"] = true;
    json["effect"] = true;
    JsonArray effects = json.createNestedArray("effect_list");
    effects.add("solid");
    effects.add("rainbow");
    effects.add("sinelon");
    effects.add("confetti");
    effects.add("bpm");
    effects.add("juggle");

    String deviceSuffix = machineId + String("-led") + String(ledId);

    json["name"] = String(ha_name) + String(" ANAVI Miracle Controller LED") + String(ledId);
    json["unique_id"] = String("anavi-") + deviceSuffix;
    json["command_topic"] = String("cmnd/") + machineId + String("/led") + String(ledId) + String("/color");
    json["state_topic"] = String("stat/") + machineId + String("/led") + String(ledId) + String("/#");

    json["device"]["identifiers"] = deviceSuffix;
    json["device"]["manufacturer"] = "ANAVI Technology";
    json["device"]["model"] = "ANAVI Miracle Controller";
    json["device"]["name"] = ha_name;
    json["device"]["sw_version"] = ESP.getSketchMD5();

    JsonArray connections = json["device"].createNestedArray("connections").createNestedArray();
    connections.add("mac");
    connections.add(WiFi.macAddress());

    Serial.print("Home Assistant discovery topic: ");
    Serial.println(topic);

    int payload_len = measureJson(json);
    if (!mqttClient.beginPublish(topic, payload_len, true))
    {
        Serial.println("beginPublish failed!\n");
        return false;
    }

    if (serializeJson(json, mqttClient) != payload_len)
    {
        Serial.println("writing payload: wrong size!\n");
        return false;
    }

    if (!mqttClient.endPublish())
    {
        Serial.println("endPublish failed!\n");
        return false;
    }

    return true;
}

void publishDiscoveryState()
{
    publishLightDiscovery(1);
    publishLightDiscovery(2);

    String homeAssistantTempScale = "°C";

    if (isSensorAvailable(sensorHTU21D))
    {
        publishSensorDiscovery("temp",
                               "temperature",
                               "Temperature",
                               "temperature",
                               homeAssistantTempScale.c_str(),
                               "{{ value_json.temperature | round(1) }}");

        publishSensorDiscovery("humidity",
                               "humidity",
                               "Humidity",
                               "humidity",
                               "%",
                               "{{ value_json.humidity | round(0) }}");
    }

    if (isSensorAvailable(sensorBH1750))
    {
        publishSensorDiscovery("light",
                       "illuminance",
                       "Light",
                       "light",
                       "Lux",
                       "{{ value_json.light }}");
    }
}

#endif

void publishState(int ledId)
{
    DynamicJsonDocument json(1024);

    bool power;
    CRGB color;
    char effectLed[32];
    char topicPower[50];
    char topicColor[50];
    if (1 == ledId)
    {
      power = powerLed1;
      color = color1;
      strcpy(effectLed, effectLed1);
      strcpy(topicPower, stat_led1_power_topic);
      strcpy(topicColor, stat_led1_color_topic);
    }
    else
    {
      power = powerLed2;
      color = color2;
      strcpy(effectLed, effectLed2);
      strcpy(topicPower, stat_led2_power_topic);
      strcpy(topicColor, stat_led2_color_topic);
    }

    const char* state = power ? "ON" : "OFF";
    json["state"] = state;
    json["brightness"] = brightnessLevel;
    json["effect"] = effectLed;

    json["color"]["r"] = power ? color.red : 0;
    json["color"]["g"] = power ? color.green : 0;
    json["color"]["b"] = power ? color.blue : 0;

    int payloadLength = measureJson(json);
    if (mqttClient.beginPublish(topicColor, payloadLength, true))
    {
        if (serializeJson(json, mqttClient) == payloadLength)
        {
            mqttClient.endPublish();
        }
    }

    char payload[150];
    serializeJson(json, payload);

    Serial.print("[");
    Serial.print(topicColor);
    Serial.print("] ");
    Serial.println(payload);

    Serial.print("[");
    Serial.print(topicPower);
    Serial.print("] ");
    Serial.println(state);
    mqttClient.publish(topicPower, state, true);
}

void publishSensorData(const char* subTopic, const char* key, const float value)
{
    StaticJsonDocument<100> json;
    json[key] = value;
    char payload[100];
    serializeJson(json, payload);
    char topic[200];
    sprintf(topic,"%s/%s/%s", workgroup, machineId, subTopic);
    mqttClient.publish(topic, payload, true);
}

void publishSensorData(const char* subTopic, const char* key, const String& value)
{
    StaticJsonDocument<100> json;
    json[key] = value;
    char payload[100];
    serializeJson(json, payload);
    char topic[200];
    sprintf(topic,"%s/%s/%s", workgroup, machineId, subTopic);
    mqttClient.publish(topic, payload, true);
}

bool isSensorAvailable(int sensorAddress)
{
    // Check if I2C sensor is present
    Wire.beginTransmission(sensorAddress);
    return 0 == Wire.endTransmission();
}

void handleHTU21D()
{
    // Check if temperature has changed
    const float tempTemperature = htu.readTemperature();
    if (1 <= abs(tempTemperature - sensorTemperature))
    {
        // Print new temprature value
        sensorTemperature = tempTemperature;
        Serial.print("Temperature: ");
        Serial.print(sensorTemperature);
        Serial.println("C");

        // Publish new temperature value through MQTT
        publishSensorData("temperature", "temperature", sensorTemperature);
    }

    // Check if humidity has changed
    const float tempHumidity = htu.readHumidity();
    if (1 <= abs(tempHumidity - sensorHumidity))
    {
        // Print new humidity value
        sensorHumidity = tempHumidity;
        Serial.print("Humidity: ");
        Serial.print(sensorHumidity);
        Serial.println("%");

        // Publish new humidity value through MQTT
        publishSensorData("humidity", "humidity", sensorHumidity);
    }
}

void sensorWriteData(int i2cAddress, uint8_t data)
{
    Wire.beginTransmission(i2cAddress);
    Wire.write(data);
    Wire.endTransmission();
}

void handleBH1750()
{
    //Wire.begin();
    // Power on sensor
    sensorWriteData(sensorBH1750, 0x01);
    // Set mode continuously high resolution mode
    sensorWriteData(sensorBH1750, 0x10);

    uint16_t tempAmbientLight;

    Wire.requestFrom(sensorBH1750, 2);
    tempAmbientLight = Wire.read();
    tempAmbientLight <<= 8;
    tempAmbientLight |= Wire.read();
    // s. page 7 of datasheet for calculation
    tempAmbientLight = tempAmbientLight/1.2;

    if (1 <= abs(tempAmbientLight - sensorAmbientLight))
    {
        // Print new brightness value
        sensorAmbientLight = tempAmbientLight;
        Serial.print("Light: ");
        Serial.print(tempAmbientLight);
        Serial.println("Lux");

        // Publish new brightness value through MQTT
        publishSensorData("light", "light", sensorAmbientLight);
    }
}

void detectGesture()
{
    //read a gesture from the device
    const uint8_t gestureCode = apds.readGesture();
    // Skip if gesture has not been detected
    if (0 == gestureCode)
    {
        return;
    }
    String gesture = "";
    switch(gestureCode)
    {
    case APDS9960_DOWN:
        gesture = "down";
        break;
    case APDS9960_UP:
        gesture = "up";
        break;
    case APDS9960_LEFT:
        gesture = "left";
        break;
    case APDS9960_RIGHT:
        gesture = "right";
        break;
    }
    Serial.print("Gesture: ");
    Serial.println(gesture);
    // Publish the detected gesture through MQTT
    publishSensorData("gesture", "gesture", gesture);
}

void handleBMP()
{
  sensors_event_t event;
  bmp.getEvent(&event);
  if (!event.pressure)
  {
    // BMP180 sensor error
    return;
  }
  Serial.print("BMP180 Pressure: ");
  Serial.print(event.pressure);
  Serial.println(" hPa");
  float temperature;
  bmp.getTemperature(&temperature);
  Serial.print("BMP180 Temperature: ");
  Serial.print(temperature);
  Serial.println(" C");
  // For accurate results replace SENSORS_PRESSURE_SEALEVELHPA with the current SLP
  float seaLevelPressure = SENSORS_PRESSURE_SEALEVELHPA;
  float altitude;
  altitude = bmp.pressureToAltitude(seaLevelPressure, event.pressure, temperature);
  Serial.print("BMP180 Altitude: ");
  Serial.print(altitude);
  Serial.println(" m");

  // Publish new pressure values through MQTT
  publishSensorData("BMPpressure", "BMPpressure", event.pressure);
  publishSensorData("BMPtemperature", "BMPtemperature", temperature);
  publishSensorData("BMPaltitude", "BMPaltitude", altitude);
}

void handleSensors()
{
    if (isSensorAvailable(sensorHTU21D))
    {
        handleHTU21D();
    }
    if (isSensorAvailable(sensorBH1750))
    {
        handleBH1750();
    }
    if (isSensorAvailable(sensorBMP180))
    {
      handleBMP();
    }
}

void rainbow(CRGB *leds, uint8_t gHue, int numToFill)
{
  fill_rainbow(leds, numToFill, gHue, 7);
}

void sinelon(CRGB *leds, uint8_t gHue, int numToFill)
{
  // a colored dot sweeping back and forth, with fading trails
  fadeToBlackBy( leds, numToFill, 20);
  int pos = beatsin16( 13, 0, numToFill-1 );
  leds[pos] += CHSV( gHue, 255, 192);
}

void confetti(CRGB *leds, uint8_t gHue, int numToFill)
{
  // random colored speckles that blink in and fade smoothly
  fadeToBlackBy( leds, numToFill, 10);
  int pos = random16(numToFill);
  leds[pos] += CHSV( gHue + random8(64), 200, 255);
}

void bpm(CRGB *leds, uint8_t gHue, int numToFill)
{
  // colored stripes pulsing at a defined Beats-Per-Minute (BPM)
  uint8_t BeatsPerMinute = 62;
  CRGBPalette16 palette = PartyColors_p;
  uint8_t beat = beatsin8( BeatsPerMinute, 64, 255);
  for( int i = 0; i < numToFill; i++)
  {
    leds[i] = ColorFromPalette(palette, gHue+(i*2), beat-gHue+(i*10));
  }
}

void juggle(CRGB *leds, uint8_t gHue, int numToFill)
{
  // eight colored dots, weaving in and out of sync with each other
  fadeToBlackBy( leds, numToFill, 20);
  byte dothue = 0;
  for( int i = 0; i < 8; i++)
  {
    leds[beatsin16( i+7, 0, numToFill-1 )] |= CHSV(dothue, 200, 255);
    dothue += 32;
  }
}

void processEffects(CRGB *leds, bool power, const char* effect, uint8_t hue, const CRGB& color, int numToFill)
{
    if ( (false == power) || (0 == strcmp(effect, "none")) )
    {
      // Make sure all LEDs are turned off
      fill_solid( leds, numToFill, CRGB::Black);
      return;
    }

    if ( 0 == strcmp(effect, "solid"))
    {
      fill_solid(leds, numToFill, color);
    }
    else if (0 == strcmp(effect, "rainbow"))
    {
      rainbow(leds, hue, numToFill);
    }
    else if (0 == strcmp(effect, "sinelon"))
    {
      sinelon(leds, hue, numToFill);
    }
    else if (0 == strcmp(effect, "confetti"))
    {
      confetti(leds, hue, numToFill);
    }
    else if (0 == strcmp(effect, "bpm"))
    {
      bpm(leds, hue, numToFill);
    }
    else if (0 == strcmp(effect, "juggle"))
    {
      juggle(leds, hue, numToFill);
    }
    else
    {
      // Turn off LEDs if the effect is not supported
      fill_solid( leds, numToFill, CRGB::Black);
    }
}

void loop()
{
    // put your main code here, to run repeatedly:
    mqttClient.loop();

    // Reconnect if there is an issue with the MQTT connection
    const unsigned long mqttConnectionMillis = millis();
    if ( (false == mqttClient.connected()) && (mqttConnectionInterval <= (mqttConnectionMillis - mqttConnectionPreviousMillis)) )
    {
        mqttConnectionPreviousMillis = mqttConnectionMillis;
        mqttReconnect();
    }

    // Set< animations for each LED strip
    processEffects(leds1, powerLed1, effectLed1, gHue1, color1, numberLed1);
    processEffects(leds2, powerLed2, effectLed2, gHue2, color2, numberLed2);

    FastLED.show();
    FastLED.delay(1000/FRAMES_PER_SECOND);

    // do some periodic updates
    EVERY_N_MILLISECONDS(20)
    {
      // slowly cycle the "base color" through the rainbow
      gHue1++;
      gHue2++;
      // Bring back hue in range
      if (360 <= gHue1)
      {
        gHue1 = 0;
      }
      if (360 <= gHue1)
      {
        gHue2 = 0;
      }
    }

    // Handle gestures at a shorter interval
    if (isSensorAvailable(APDS9960_ADDRESS))
    {
        detectGesture();
    }

    const unsigned long currentMillis = millis();
    if (sensorInterval <= (currentMillis - sensorPreviousMillis))
    {
        sensorPreviousMillis = currentMillis;
        handleSensors();

        printLedStatus();

        long rssiValue = WiFi.RSSI();
        String rssi = String(rssiValue) + " dBm";
        Serial.println(rssi);
        sensor_line3 = rssi;
        need_redraw = true;

        publishSensorData("wifi/ssid", "ssid", WiFi.SSID());
        publishSensorData("wifi/bssid", "bssid", WiFi.BSSIDstr());
        publishSensorData("wifi/rssi", "rssi", rssiValue);
        publishSensorData("wifi/ip", "ip", WiFi.localIP().toString());
        publishSensorData("sketch", "sketch", ESP.getSketchMD5());

#ifdef PUBLISH_CHIP_ID
        char chipid[9];
        snprintf(chipid, sizeof(chipid), "%08x", ESP.getChipId());
        publishSensorData("chipid", "chipid", chipid);
#endif
    }

    if (true == need_redraw)
    {
        drawDisplay(mqtt_line1[0] ? mqtt_line1 : sensor_line1.c_str(),
                    mqtt_line2[0] ? mqtt_line2 : sensor_line2.c_str(),
                    mqtt_line3[0] ? mqtt_line3 : sensor_line3.c_str());
        need_redraw = false;
    }

    // Press and hold the button to reset to factory defaults
    factoryReset();
}
