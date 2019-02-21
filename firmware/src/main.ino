// =--------------------------------------------------------------------------------= Libraries =--=

#include <FS.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_MCP9808.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_AM2320.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <math.h>


// =----------------------------------------------------------------------------= Configuration =--=

// Wifi
#define SETUP_AP_NAME                 "Setup Data Collector"
#define SETUP_AP_PASSWORD             "setupcollector"
#define WIFI_RECONNECT_TIMER          60000 // Delay for rechecking wifi if disconnected

// OTA Updates
#define OTA_PORT                      8266
#define OTA_HOSTNAME_PREFIX           "data-collector"
#define OTA_PASSWORD                  "data-collector" // consider changing to MD5 hash

// MQTT
#define MAX_CONNECTION_ATTEMPTS       3 // Number of attempts before enabling display
#define SHORT_CONNECTION_DELAY        3000 // Delay between initial connection attempts
#define LONG_CONNECTION_DELAY         120000 // Delay between attempts after max attempts
#define CONNECTING_BLINK_DELAY        500
#define MQTT_ROOT                     "logger"
#define DEFAULT_MQTT_SERVER           ""
#define MQTT_SERVER_LENGTH            40
#define DEFAULT_MQTT_PORT             "1883"
#define MQTT_PORT_LENGTH              6
#define LOCATION_LENGTH               40
#define DEFAULT_LOCATION              "unknown"

// Neopixel
#define NEOPIXEL_PIN                  14
#define NEOPIXEL_COUNT                2
#define DEFAULT_INTENSITY             0.25 // 0-1.0 as percentage of power

// Programs
#define DISPLAY_FPS                   30
#define FRAME_DELAY_MS                1000 / DISPLAY_FPS
#define BLINK_DELAY_MS                333 // 3 fps

// Buttons
#define BUTTON_PIN                    12
#define DEBOUNCE_MS                   50
#define HOLD_TIME_MS                  3000

// Sensors
#define SENSOR_READ_TIMER             60000 // 1 minute
#define LIGHT_READ_PIN                A0
#define LIGHT_RAW_RANGE               1024 / 1.1 // adjusted for voltage divider, input 1.1v
#define LIGHT_LOG_RANGE               5.0 // 3.3v = 10^5 lux
#define MCP9808_ADDRESS               0x18 // default address
#define TEMPERATURE_READ_COUNT        4 // number of temperature readings to average


// =-------------------------------------------------------------------------------= Prototypes =--=

void displayLoop(bool);
void runProgramDefault(bool);


// =----------------------------------------------------------------------------------= Globals =--=

// Programs
// Define all the programs in the correct order here.
// The first program will be the default.
int currentProgram = 0;
void (*renderFunc[])(bool) {
  runProgramDefault
};
#define PROGRAM_COUNT (sizeof(renderFunc) / sizeof(renderFunc[0]))

// Define the string name mappings for each program here for MQTT translation.
const char *programNames[] = {
  "default"
};

// WiFi Client
WiFiClient wifiClient;
bool wifiFeaturesEnabled = false;

// MQTT
char mqtt_server[MQTT_SERVER_LENGTH] = DEFAULT_MQTT_SERVER;
char mqtt_port[MQTT_PORT_LENGTH] = DEFAULT_MQTT_PORT;
char location[LOCATION_LENGTH] = DEFAULT_LOCATION;
int connectionAttempts = 0;

PubSubClient mqttClient(wifiClient);
String clientId(String(ESP.getChipId(), HEX));

// Neopixel
bool shouldRunDisplay = false;
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRBW + NEO_KHZ800);

// Display
float globalIntensity = DEFAULT_INTENSITY;

// Buttons
int buttonValue = 0;     // Value read from button
int buttonLastValue = 0; // Buffered value of the button's previous state
long buttonDownTime;     // Time the button was pressed
long buttonUpTime;       // Time the button was released
bool ignoreUp = false;   // Whether to ignore the button release because click+hold was triggered
bool hasBoot = false;    // Handle a bug where a short press is triggered on boot

// Save data flag for setup config
bool shouldSaveConfig = false;

// Sensors
Adafruit_MCP9808 mcp9808 = Adafruit_MCP9808();
Adafruit_AM2320 am2320 = Adafruit_AM2320();


// =--------------------------------------------------------------------------------= Utilities =--=

// Turn a topic suffix into a full MQTT topic for the centerpiece namespace
String makeTopic(String suffix, bool all = false) {
  if (all) {
    return String(String(MQTT_ROOT) + "/all/" + suffix);
  }
  return String(String(MQTT_ROOT) + "/" + clientId + "/" + suffix);
}

// Check if a given topic containes the suffix and is for this or all nodes
bool topicMatch(String topic, String suffix) {
  return topic.equals(makeTopic(suffix)) || topic.equals(makeTopic(suffix, true));
}

// Note: This can be removed and the original `fmod` can be used once the following issue is closed.
// https://github.com/esp8266/Arduino/issues/612
double floatmod(double a, double b) {
  return (a - b * floor(a / b));
}

// Same as the Arduino API map() function, except for floats
float floatmap(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// Convert HSI colors to RGBW Neopixel colors
uint32_t hsi2rgbw(float H, float S, float I) {
  int r, g, b, w;
  float cos_h, cos_1047_h;

  H = floatmod(H, 360); // cycle H around to 0-360 degrees
  H = 3.14159 * H / (float)180; // Convert to radians.
  S = S > 0 ? (S < 1 ? S : 1) : 0; // clamp S and I to interval [0,1]
  I = I > 0 ? (I < 1 ? I : 1) : 0;

  if(H < 2.09439) {
    cos_h = cos(H);
    cos_1047_h = cos(1.047196667 - H);
    r = S * 255 * I / 3 * (1 + cos_h / cos_1047_h);
    g = S * 255 * I / 3 * (1 + (1 - cos_h / cos_1047_h));
    b = 0;
    w = 255 * (1 - S) * I;
  } else if(H < 4.188787) {
    H = H - 2.09439;
    cos_h = cos(H);
    cos_1047_h = cos(1.047196667 - H);
    g = S * 255 * I / 3 * (1 + cos_h / cos_1047_h);
    b = S * 255 * I / 3 * (1 + (1 - cos_h / cos_1047_h));
    r = 0;
    w = 255 * (1 - S) * I;
  } else {
    H = H - 4.188787;
    cos_h = cos(H);
    cos_1047_h = cos(1.047196667 - H);
    b = S * 255 * I / 3 * (1 + cos_h / cos_1047_h);
    r = S * 255 * I / 3 * (1 + (1 - cos_h / cos_1047_h));
    g = 0;
    w = 255 * (1 - S) * I;
  }

  return strip.Color(r, g, b, w);
}


// =-------------------------------------------------------------------------------------= MQTT =--=

void setupMQTT() {
  int port = atoi(mqtt_port);
  mqttClient.setServer(mqtt_server, port);
  mqttClient.setCallback(callback);
  connectionAttempts = 0;
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived ["); Serial.print(topic); Serial.print("] ");
  payload[length] = 0;
  String message((char *)payload);
  Serial.println(message);

  if (topicMatch(topic, "identify")) {
    sendIdentity();
  }
}

void sendIdentity() {
  mqttClient.publish(makeTopic("identity").c_str(), "online");
}

// Function to connect and reconnect as necessary to the MQTT server.
// Should be called in the loop function and it will take care if connecting.
void mqttConnect() {
  static unsigned long connectTimer = millis();
  static unsigned long updateTimer = millis();
  static bool ledOn = false;

  unsigned long connectTimeDiff = millis() - connectTimer;
  unsigned long updateTimeDiff = millis() - updateTimer;

  if (!mqttClient.connected()) {
    // While not connected, try every few seconds
    bool hasExceededMaxAttempts = connectionAttempts > MAX_CONNECTION_ATTEMPTS;
    int connectionDelay = hasExceededMaxAttempts ? LONG_CONNECTION_DELAY : SHORT_CONNECTION_DELAY;
    if (connectTimeDiff > connectionDelay) {
      Serial.print("Attempting MQTT connection... ");

      // TODO: Make this connect more async -- the 2-3 second delay blocks animation
      if (mqttClient.connect(clientId.c_str())) {
        Serial.println("connected");
        sendIdentity();

        // Subscribe to topics
        mqttClient.subscribe(makeTopic("identify", true).c_str());
        // mqttClient.subscribe(makeTopic("program").c_str());

        // Reset number of attempts and enable the display
        connectionAttempts = 0;
        enableDisplay();
      } else {
        Serial.printf(
          "failed to connect to MQTT server: rc=%d, trying again in %d seconds\n",
          mqttClient.state(),
          connectionDelay / 1000
        );

        // Enable the display if the connection attempts exceed the max allowed
        if (++connectionAttempts > MAX_CONNECTION_ATTEMPTS) {
          if (!shouldRunDisplay) {
            Serial.printf(
              "Unable to connect to MQTT server in %d attempts. Enabling display and slowing requests.\n",
              MAX_CONNECTION_ATTEMPTS
            );
          }
          enableDisplay();
        }
      }
      connectTimer = millis();
    }

    // While connecting, blink the light. Don't blink if the display is active
    if (!shouldRunDisplay && updateTimeDiff > CONNECTING_BLINK_DELAY) {
      ledOn = !ledOn;
      uint32_t color = hsi2rgbw(180, 1, ledOn ? globalIntensity : 0);
      for (int i = 0; i < NEOPIXEL_COUNT; ++i) { strip.setPixelColor(i, color); }
      strip.show();

      updateTimer = millis();
    }
  }
}


// =----------------------------------------------------------------------------------= Display =--=

void setupNeopixels() {
  strip.begin();
  disableDisplay();
  strip.show();
}

void enableDisplay() {
  shouldRunDisplay = true;
}

void disableDisplay() {
  shouldRunDisplay = false;
  strip.clear();
}

void setBrightness(int brightness) {
  // Convert brightness percentage to float 0-1.0 value
  if (brightness >= 0 && brightness < 100) {
    globalIntensity = brightness / 100.0;
    Serial.printf("Setting brightness to %d%%\n", brightness);
  }
}

void setBrightness(String brightness) {
  setBrightness(brightness.toInt());
}

void setProgram(int program) {
  if (program >= 0 && program < PROGRAM_COUNT && program != currentProgram) {
    currentProgram = program;
    Serial.printf("Setting program to %s\n", programNames[program]);
    displayLoop(true);
  }
}

void setProgram(String programName) {
  for (size_t program = 0; program < PROGRAM_COUNT; program++) {
    if (programName.equals(programNames[program])) {
      setProgram(program);
      break;
    }
  }
}

void nextProgram() {
  setProgram((currentProgram + 1) % PROGRAM_COUNT); // Next with loop around
}

void displayLoop(bool first = false) {
  (*renderFunc[currentProgram])(first);
}


// =---------------------------------------------------------------------------------= Programs =--=

// Program: Default
// Breathing white light for standby/active
void runProgramDefault(bool first) {
  static unsigned long updateTimer = millis();
  static float angleOffset = 0.0;

  unsigned long updateTimeDiff = millis() - updateTimer;
  if (first || updateTimeDiff > FRAME_DELAY_MS) {
    // Kudos: https://sean.voisen.org/blog/2011/10/breathing-led-with-arduino/
    // f(x) = e^sin(x) * 1/e -- pre-calculate constants
    float multiplier = exp(sin(radians(angleOffset))) * 0.36787;

    uint32_t color = hsi2rgbw(0, 0, globalIntensity * multiplier);
    for (int i = 0; i < NEOPIXEL_COUNT; ++i) { strip.setPixelColor(i, color); }
    strip.show();

    float degPerFrame = 360 / DISPLAY_FPS / 6; // 6 seconds
    angleOffset = floatmod(angleOffset + degPerFrame, 360);
    updateTimer = millis();
  }
}


// =----------------------------------------------------------------------------------= Buttons =--=

void setupButton() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(BUTTON_PIN, HIGH);
}

void buttonLoop() {
  // Read the state of the button
  buttonValue = digitalRead(BUTTON_PIN);

  // Test for button pressed and store the down time
  if (buttonValue == LOW && buttonLastValue == HIGH && (millis() - buttonUpTime) > long(DEBOUNCE_MS)) {
    buttonDownTime = millis();
  }

  // Test for button release and store the up time
  if (buttonValue == HIGH && buttonLastValue == LOW && (millis() - buttonDownTime) > long(DEBOUNCE_MS)) {
    if (ignoreUp == false) {
      if (hasBoot) {
        nextProgram();
      } else {
        hasBoot = true;
      }
    } else {
      ignoreUp = false;
    }
    buttonUpTime = millis();
  }

  // Test for button held down for longer than the hold time
  if (buttonValue == LOW && (millis() - buttonDownTime) > long(HOLD_TIME_MS)) {
    ignoreUp = true;
    buttonDownTime = millis();
    wifiCaptivePortal();
  }

  buttonLastValue = buttonValue;
}


// =----------------------------------------------------------------------------------= Sensors =--=

void setupSensors() {
  pinMode(LIGHT_READ_PIN, INPUT_PULLUP);

  // Mode Resolution SampleTime
  //  0    0.5°C       30 ms
  //  1    0.25°C      65 ms
  //  2    0.125°C     130 ms
  //  3    0.0625°C    250 ms
  mcp9808.begin(MCP9808_ADDRESS);
  mcp9808.setResolution(3);
  am2320.begin();
}

// Check each sensor in turn
void sensorLoop() {
  static unsigned long updateTimer = millis();

  unsigned long updateTimeDiff = millis() - updateTimer;
  if (updateTimeDiff > SENSOR_READ_TIMER) {

    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["clientId"] = clientId;
    json["location"] = location;
    json["temperature"] = readTemperatureSensor();
    json["humidity"] = readHumiditySensor();
    json["lux"] = readLuxSensor();

    String output;
    json.printTo(output);
    mqttClient.publish(makeTopic("reading").c_str(), output.c_str());

    updateTimer = millis();
  }
}

float readTemperatureSensor() {
  mcp9808.wake();

  float temperature = 0;
  for(size_t i = 0; i < TEMPERATURE_READ_COUNT; i++) {
    temperature += mcp9808.readTempF();
  }
  temperature /= TEMPERATURE_READ_COUNT;

  // mcp9808.shutdown(); // drop power consumption to ~0.1 µAm, stops sampling
  return temperature;
}

float readHumiditySensor() {
  float humidity = am2320.readHumidity();
  return humidity;
}

float readLuxSensor() {
  int rawValue = analogRead(LIGHT_READ_PIN);
  return rawToLux(rawValue);
}

float rawToLux(int raw) {
  float logLux = raw * LIGHT_LOG_RANGE / LIGHT_RAW_RANGE;
  return pow(10, logLux);
}

// =-------------------------------------------------------------------------------------= WIFI =--=

void setupWifi() {
  if (WiFi.SSID() == "") {
    Serial.println("We haven't got any access point credentials, so get them now");
    wifiCaptivePortal();
  } else {
    connectWifi();
  }
}

void connectWifi() {
  // We need to check here again as this can be called multiple places
  if (WiFi.SSID() != "") {
    if (!shouldRunDisplay) {
      // Set status LED for WIFI connection start
      uint32_t color = hsi2rgbw(20, 1, globalIntensity / 2);
      for (int i = 0; i < NEOPIXEL_COUNT; ++i) { strip.setPixelColor(i, color); }
      strip.show();
    }

    // Force to station mode because if device was switched off while in access point mode it will
    // start up next time in access point mode.
    WiFi.mode(WIFI_STA);
    WiFi.waitForConnectResult();
    finalizeWifi();
  }
}

// Wifi wasn't connected for some reason, let's try again periodically
void maybeConnectWifi() {
  static unsigned long updateTimer = millis();

  unsigned long updateTimeDiff = millis() - updateTimer;
  if (updateTimeDiff > WIFI_RECONNECT_TIMER) {
    Serial.println("Attempting to reconnect to wifi...");
    connectWifi();
    updateTimer = millis();
  }
}

// Finishing steps for after wifi may be complete
void finalizeWifi() {
  if (WiFi.status() != WL_CONNECTED){
    wifiFeaturesEnabled = false;
    Serial.print("Failed to connect to wifi. ");

    if (shouldRunDisplay) {
      // Display is already running
      Serial.printf("Will try again in %d seconds.", (WIFI_RECONNECT_TIMER / 1000));
    } else {
      Serial.println("Playing failure animation then proceeding.");
      // Flash the bad news, then activate the display
      for (size_t j = 0; j < 6; j++) {
        uint32_t color = hsi2rgbw(j % 2 ? 0 : 240, 1, globalIntensity);
        for (int i = 0; i < NEOPIXEL_COUNT; ++i) { strip.setPixelColor(i, color); }
        strip.show();
        delay(125);
      }
      enableDisplay();
    }
  } else {
    wifiFeaturesEnabled = true;
    Serial.print("Connected to WiFi. Local IP: ");
    Serial.println(WiFi.localIP());
  }
}

// gets called when WiFiManager enters configuration mode
void configModeCallback (WiFiManager *myWiFiManager) {
  uint32_t color = hsi2rgbw(240, 1, globalIntensity);
  for (int i = 0; i < NEOPIXEL_COUNT; ++i) { strip.setPixelColor(i, color); }
  strip.show();

  Serial.println("Entered config mode...");
  Serial.println(WiFi.softAPIP());

  // if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

// Callback notifying us of the need to save config
void saveConfigCallback() {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

// Fire up a captive portal to collect network and MQTT info
// This portal is blocking, so the main loop can not run during this
void wifiCaptivePortal() {
  // WiFi.disconnect(); // Reset saved WiFi networks. Good for blowing out a config/debugging

  // The extra parameters to be configured (can be either global or just in the setup). After
  // connecting, parameter.getValue() will get you the configured value:
  // id/name, placeholder/prompt, default, length
  WiFiManagerParameter config_mqtt_server("server", "MQTT Server", mqtt_server, MQTT_SERVER_LENGTH);
  WiFiManagerParameter config_mqtt_port("port", "MQTT Port", mqtt_port, MQTT_PORT_LENGTH);
  WiFiManagerParameter config_location("location", "Device Location", location, LOCATION_LENGTH);
  WiFiManager wifiManager;

  // Set callback for when connecting to previous WiFi fails, and enters Access Point mode
  wifiManager.setAPCallback(configModeCallback);

  // Set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // Add all your parameters here
  wifiManager.addParameter(&config_mqtt_server);
  wifiManager.addParameter(&config_mqtt_port);
  wifiManager.addParameter(&config_location);

  // Fire up the captive portal
  String setupAPName(String(SETUP_AP_NAME) + " " + clientId);
  if (!wifiManager.startConfigPortal(setupAPName.c_str(), SETUP_AP_PASSWORD)) {
    Serial.println("Failed to connect and hit timeout. Resetting...");

    // Reset and reboot. User can try again by holding button
    delay(3000);
    ESP.reset();
    delay(5000);
  }

  // Maybe save the custom parameters to FS
  if (shouldSaveConfig) {
    // Read updated parameters
    strlcpy(mqtt_server, config_mqtt_server.getValue(), MQTT_SERVER_LENGTH);
    strlcpy(mqtt_port, config_mqtt_port.getValue(), MQTT_PORT_LENGTH);
    strlcpy(location, config_location.getValue(), LOCATION_LENGTH);

    Serial.println("Saving config...");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["location"] = location;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("Failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    Serial.println("");
    configFile.close();
  }

  finalizeWifi();
}


// =------------------------------------------------------------------------------= File System =--=

void setupFileSystem() {
  // Clean FS, for testing
  // SPIFFS.format();

  // Read configuration from FS json
  Serial.println("Mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("Mounted file system");
    if (SPIFFS.exists("/config.json")) {
      // file exists, reading and loading
      Serial.println("Reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("Opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);

        if (json.success()) {
          Serial.println("\nParsed json");

          strlcpy(mqtt_server, json["mqtt_server"] | DEFAULT_MQTT_SERVER, MQTT_SERVER_LENGTH);
          strlcpy(mqtt_port, json["mqtt_port"] | DEFAULT_MQTT_PORT, MQTT_PORT_LENGTH);
          strlcpy(location, json["location"] | DEFAULT_LOCATION, LOCATION_LENGTH);
        } else {
          Serial.println("Failed to load json config");
        }
      }
    }
  } else {
    Serial.println("Failed to mount FS");
  }
}


// =------------------------------------------------------------------------------= OTA Updates =--=

void setupOta() {
  // ArduinoOTA.setPort(OTA_PORT); // re-add when platformio fixes bug
  ArduinoOTA.setHostname((const char *)(String(OTA_HOSTNAME_PREFIX) + "-" + clientId).c_str());
  // ArduinoOTA.setPassword(OTA_PASSWORD); // re-add when platformio fixes bug
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    strip.setPixelColor(0, hsi2rgbw(240, 1, globalIntensity));
    strip.setPixelColor(1, hsi2rgbw(120, 1, globalIntensity));
    strip.show();
    Serial.println("Start updating " + type);
    delay(100);
    Serial.end();
  });

  ArduinoOTA.onEnd([]() {
    Serial.begin(115200);
    Serial.println("\nOTA Completed.");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });

  ArduinoOTA.begin();
}

void otaLoop() {
  ArduinoOTA.handle();
}

// =---------------------------------------------------------------------------= Setup and Loop =--=

void setup() {
  Serial.begin(115200);
  delay(100);

  // Change Watchdog Timer to longer wait
  ESP.wdtDisable();
  ESP.wdtEnable(WDTO_8S);

  // Setup :allthethings:
  randomSeed(analogRead(0));
  setupSensors();
  setupNeopixels();
  setupFileSystem();
  setupButton();
  setupWifi();

  if (wifiFeaturesEnabled) {
    setupOta();
    setupMQTT();
  }
}

void loop() {
  if (shouldSaveConfig) {
    // Config was rewritten, be sure to rerun setup that uses it
    setupMQTT();
    shouldSaveConfig = false;
  }

  if (wifiFeaturesEnabled) {
    if (mqttClient.connected()) {
      otaLoop();
      mqttClient.loop();
    } else {
      mqttConnect();
    }
  } else {
    maybeConnectWifi();
  }

  sensorLoop();
  buttonLoop();
  if (shouldRunDisplay) {
    displayLoop();
  }
}
