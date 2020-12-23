#include <Tween.h>
#include <pt.h>
#include <timer.h>
#include <Adafruit_NeoPixel.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>
#include <WiFiManager.h>

#include "config.h"

#define ARR_SIZE(arr) ( sizeof((arr)) / sizeof((arr[0])) )

const uint16_t STRIP_PIN = D8;
const int STRIP_SIZE = 35;
const char* WIFI_SSID = CONFIG_WIFI_SSID;
const char* WIFI_PASSWORD = CONFIG_WIFI_PASSWORD;
const char* MQTT_SERVER_HOST = CONFIG_MQTT_SERVER_HOST;
const char* MQTT_TOPIC_BRIGHTNESS = "brightness";

static struct pt pt1, pt2, pt3, pt4, pt5;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(STRIP_SIZE, STRIP_PIN, NEO_GRB + NEO_KHZ800);
WiFiClient espClient;
PubSubClient client(espClient);
WiFiManager wifiManager;
ESP8266WebServer server(80);

static uint32_t RING_COLOR_BLACK = strip.Color(0, 0, 0);
static uint32_t RING_COLOR_ORANGE = strip.Color(255, 135, 0);
static uint32_t RING_COLOR_PURPLE = strip.Color(179, 0, 250);
static uint32_t RING_COLOR_GREEN = strip.Color(66, 250, 1);
static uint32_t RING_COLOR_BLUE = strip.Color(53, 103, 245);
static uint32_t RING_COLOR_RED = strip.Color(250, 0, 90);

int RING_SCENE_CONNECTING = 0;
int RING_SCENE_WAITING_FOR_CONFIGURATION = 1;
int RING_SCENE_CONNECTED = 2;

Tween::Timeline ringSceneConnectingTimeline;
double ringSceneConnectingTimelineValue = 0.f;
Tween::Timeline ringSceneWaitingForConfigurationTimeline;
double ringSceneWaitingForConfigurationTimelineValue = 0.f;
Tween::Timeline ringSceneConnectedTimeline;
double ringSceneConnectedTimelineValue = 0.f;

WiFiEventHandler wiFiEventStationModeConnectedEventHandler;

boolean wifiConnected = false;
boolean httpServerLaunched = false;
boolean mqttServerLaunched = false;
boolean configPortalLaunched = false;
int currentBrightness = 20;
int currentRingScene = RING_SCENE_CONNECTING;
char mqttMessageBuffer[100];
long lastMessage = 0;
long lastReceived = 0;
bool debug = false;

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  int brightnessRawValue = 2.55 * currentBrightness;
  strip.setBrightness(brightnessRawValue);
  strip.begin();

  if (!SPIFFS.begin()) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  wiFiEventStationModeConnectedEventHandler = WiFi.onStationModeConnected ([](const WiFiEventStationModeConnected & event) {
    configPortalLaunched = false;
  });

  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.autoConnect("dixie");

  server.serveStatic("/", SPIFFS, "/index.html");
  server.serveStatic("/style.css", SPIFFS, "/style.css");

  server.on("/brightness/set", []() {
    String brightnessValueAsString = server.arg("value");
    char charArray[brightnessValueAsString.length() + 1];
    strcpy(charArray, brightnessValueAsString.c_str());
    int value = atoi(charArray);
    setStripBrightness(value);
    server.send(204, "text/plain");
  });

  server.on("/brightness/get", []() {
    char buffer[20];
    itoa(currentBrightness, buffer, 10);
    server.send(200, "text/plain", buffer);
  });

  server.on("/reset", []() {
    wifiManager.resetSettings();
    ESP.reset();
  });

  server.begin();

  PT_INIT(&pt1);
  PT_INIT(&pt2);
  PT_INIT(&pt3);
  PT_INIT(&pt4);
  PT_INIT(&pt5);
}

void loop() {
  launchRingSceneConnectingThread(&pt1);
  launchRingSceneWaitingForConfigurationThread(&pt2);
  launchRingSceneConnectedThread(&pt3);
  launchWifiConnectionThread(&pt4);
  launchMQTTServerConnectionThread(&pt5);

  MDNS.update();
  wifiManager.process();
  server.handleClient();
}

static int launchRingSceneConnectingThread(struct pt *pt) {
  static struct timer t;
  static boolean timelineStarted = false;

  PT_BEGIN(pt);

  if (currentRingScene != RING_SCENE_CONNECTING) {
    PT_EXIT(pt);
  }

  ringSceneConnectingTimeline.update();

  if (!timelineStarted) {
    initializeRingSceneConnectingTimeline();
    ringSceneConnectingTimeline.start();
    timelineStarted = true;
  }

  strip.fill(strip.ColorHSV(0, 0, ringSceneConnectingTimelineValue), 0, STRIP_SIZE);
  strip.show();

  if (ringSceneConnectingTimeline.size() == 0) {
    initializeRingSceneConnectingTimeline();
    timelineStarted = false;
  }

  timer_set(&t, 0.01 * CLOCK_SECOND);
  PT_WAIT_UNTIL(pt, timer_expired(&t));
  timer_reset(&t);

  PT_END(pt);
}

static int launchRingSceneWaitingForConfigurationThread(struct pt *pt) {
  static struct timer t;
  static boolean timelineStarted = false;

  PT_BEGIN(pt);

  if (currentRingScene != RING_SCENE_WAITING_FOR_CONFIGURATION) {
    PT_EXIT(pt);
  }

  ringSceneWaitingForConfigurationTimeline.update();

  if (!timelineStarted) {
    initializeRingSceneWaitingForConfigurationTimeline();
    ringSceneWaitingForConfigurationTimeline.start();
    timelineStarted = true;
  }

  strip.fill(strip.ColorHSV(47000, 235, ringSceneWaitingForConfigurationTimelineValue), 0, STRIP_SIZE);
  strip.show();

  if (ringSceneWaitingForConfigurationTimeline.size() == 0) {
    initializeRingSceneWaitingForConfigurationTimeline();
    timelineStarted = false;
  }

  timer_set(&t, 0.01 * CLOCK_SECOND);
  PT_WAIT_UNTIL(pt, timer_expired(&t));
  timer_reset(&t);

  PT_END(pt);
}

static int launchRingSceneConnectedThread(struct pt *pt) {
  static struct timer t;

  static uint32_t colors[] = {RING_COLOR_ORANGE, RING_COLOR_PURPLE, RING_COLOR_GREEN, RING_COLOR_BLUE, RING_COLOR_RED};
  static uint32_t choosenColor = colors[rand() % ARR_SIZE(colors)];

  static uint16_t lastPixelIndex = 0;
  static uint16_t currentPixelIndex = 0;

  static boolean timelineStarted = false;
  static boolean timelineEnded = false;

  PT_BEGIN(pt);

  if (currentRingScene != RING_SCENE_CONNECTED) {
    PT_EXIT(pt);
  }

  if (!timelineStarted) {
    initializeRingSceneConnectedTimeline();
    ringSceneConnectedTimeline.start();
    timelineStarted = true;
  }

  ringSceneConnectedTimeline.update();

  currentPixelIndex = (uint16_t)ringSceneConnectedTimelineValue;

  if (currentPixelIndex <= lastPixelIndex) {
    strip.fill(RING_COLOR_BLACK, currentPixelIndex, STRIP_SIZE - currentPixelIndex);
    strip.show();
  } else {
    strip.fill(choosenColor, 0, currentPixelIndex);
    strip.show();
  }

  lastPixelIndex = currentPixelIndex;

  if (ringSceneConnectedTimeline.size() == 0) {
    timelineStarted = false;
    choosenColor = colors[rand() % ARR_SIZE(colors)];
  }

  timer_set(&t, 0.01 * CLOCK_SECOND);
  PT_WAIT_UNTIL(pt, timer_expired(&t));
  timer_reset(&t);

  PT_END(pt);
}

static int launchWifiConnectionThread(struct pt * pt) {
  static struct timer t;

  PT_BEGIN(pt);

  if (configPortalLaunched) {
    PT_EXIT(pt);
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      Serial.print("Connected to SSID [");
      Serial.print(WIFI_SSID);
      Serial.print("], IP address [");
      Serial.print(WiFi.localIP());
      Serial.println("]");

      MDNS.end();
      if (!MDNS.begin("dixie")) {
        Serial.println("Error setting up MDNS responder...");
      } else {
        Serial.println("Successfully setting up MDNS responder !");
      }

      currentRingScene = RING_SCENE_CONNECTED;
      wifiConnected = true;
    }

    PT_EXIT(pt);
  } else {
    wifiConnected = false;
    currentRingScene = RING_SCENE_CONNECTING;
  }

  if (!wifiConnected) {
    Serial.println("Waiting for wifi connection...");
    timer_set(&t, 3 * CLOCK_SECOND);
    PT_WAIT_UNTIL(pt, timer_expired(&t));
    timer_reset(&t);
  }

  PT_END(pt);
}

static int launchMQTTServerConnectionThread(struct pt * pt) {
  static struct timer t;

  PT_BEGIN(pt);

  if (!wifiConnected || !CONFIG_MQTT_ENABLED) {
    PT_EXIT(pt);
  }

  if (client.connected()) {
    if (!mqttServerLaunched) {
      if (client.subscribe(MQTT_TOPIC_BRIGHTNESS)) {
        Serial.print("Sucessfully subscribed to topic [");
        Serial.print(MQTT_TOPIC_BRIGHTNESS);
        Serial.println("]");
      } else {
        Serial.print("Failed to subscribe to topic [");
        Serial.print(MQTT_TOPIC_BRIGHTNESS);
        Serial.println("]");
      }
      mqttServerLaunched = true;
      setStripBrightness(currentBrightness);
    }
    client.loop();
    timer_set(&t, 0.01 * CLOCK_SECOND);
    PT_WAIT_UNTIL(pt, timer_expired(&t));
    timer_reset(&t);
    PT_EXIT(pt);
  } else {
    mqttServerLaunched = false;
    client.setServer(MQTT_SERVER_HOST, 1883);
    client.setCallback(mqttServerCallback);
  }

  Serial.println("Waiting for MQTT server connection...");
  if (client.connect("dixie")) {
    Serial.println("Sucessfully connected to MQTT server !");
  } else {
    Serial.print("MQTT server connection error. State is : ");
    Serial.println(client.state());
    timer_set(&t, 0.5 * CLOCK_SECOND);
    PT_WAIT_UNTIL(pt, timer_expired(&t));
    timer_reset(&t);
  }

  PT_END(pt);
}

void initializeRingSceneConnectingTimeline() {
  ringSceneConnectingTimeline.add(ringSceneConnectingTimelineValue)
  .then(0)
  .then<Ease::ExpoOut>(255, 1000)
  .then<Ease::ExpoIn>(0, 1000)
  .wait(600);
}

void initializeRingSceneWaitingForConfigurationTimeline() {
  ringSceneWaitingForConfigurationTimeline.add(ringSceneWaitingForConfigurationTimelineValue)
  .then(0)
  .then(255, 500)
  .then(0, 500);
}

void initializeRingSceneConnectedTimeline() {
  ringSceneConnectedTimeline.add(ringSceneConnectedTimelineValue)
  .then(0)
  .then<Ease::BounceOut>(36, 1200)
  .then<Ease::BounceOut>(0, 1200);
}

void configModeCallback(WiFiManager *myWiFiManager) {
  configPortalLaunched = true;
  currentRingScene = RING_SCENE_WAITING_FOR_CONFIGURATION;
}

void mqttServerCallback(char* topic, byte* payload, unsigned int length) {
  int i = 0;
  for (i = 0; i < length; i++) {
    mqttMessageBuffer[i] = payload[i];
  }
  mqttMessageBuffer[i] = '\0';
  String message = String(mqttMessageBuffer);

  Serial.print("Receiving message from topic ");
  Serial.print("[");
  Serial.print(topic);
  Serial.print("], message length is [");
  Serial.print(String(length, DEC));
  Serial.print("], message content is [");
  Serial.print(message);
  Serial.print("]");
  Serial.println("");

  if (strcmp(topic, MQTT_TOPIC_BRIGHTNESS) == 0) {
    char charArray[message.length() + 1];
    strcpy(charArray, message.c_str());

    int brightnessRawValue = 2.55 * atoi(charArray);
    strip.setBrightness(brightnessRawValue);
  }
}

void setStripBrightness(int brightness) {
  Serial.print("Setting brightness [");
  Serial.print(brightness);
  Serial.println("%]");

  currentBrightness = brightness;

  int brightnessRawValue = 2.55 * brightness;
  strip.setBrightness(brightnessRawValue);

  if (mqttServerLaunched) {
    Serial.println("Publishing brightness into MQTT topic...");
    char buffer[20];
    itoa(brightness, buffer, 10);
    client.publish(MQTT_TOPIC_BRIGHTNESS, buffer);
  }
}
