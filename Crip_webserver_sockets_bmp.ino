#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include "SensorBMA423.hpp"
#include "WatchyDisplay.h"
#include <Fonts/FreeMonoBold9pt7b.h>

#define MENU_BTN_PIN 26
#define AP_BTN_PIN 35  // Pin to activate AP and webserver

const char *ssid = "CripTime";
const char *password = NULL;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dnsServer;

const IPAddress localIP(4, 3, 2, 1);
const IPAddress gatewayIP(4, 3, 2, 1);
const IPAddress subnetMask(255, 255, 255, 0);
const String localIPURL = "http://4.3.2.1";

unsigned long buttonPressTime = 0;
bool buttonPressed = false;
const unsigned long longPressDuration = 1000;  // 1 second
volatile bool apButtonPressed = false;         // To track AP button state

#define SENSOR_SDA 21
#define SENSOR_SCL 22
#define SENSOR_IRQ 14

// Define the I2C address for BMA423 (commonly 0x18 or 0x19)
#define BMA423_SLAVE_ADDRESS 0x18

SensorBMA423 accel;
bool sensorIRQ = false;

// Define the display pins as used in Watchy
#define DISPLAY_CS 5
#define DISPLAY_RES 9
#define DISPLAY_DC 10
#define DISPLAY_BUSY 19

// AP active
bool apActive = false;
bool previousApState = false; 

// Initialize the Watchy display
WatchyDisplay display(DISPLAY_CS, DISPLAY_DC, DISPLAY_RES, DISPLAY_BUSY);

void IRAM_ATTR onAPButtonPress() {
  Serial.println("AP button pressed");
  apButtonPressed = true;
}

void IRAM_ATTR onSensorIRQ() {
  sensorIRQ = true;
}

void listFiles() {
  Serial.println("Listing files on LittleFS:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.print("FILE: ");
    Serial.println(file.name());
    file = root.openNextFile();
  }
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len) {
    if (info->opcode == WS_TEXT) {
      data[len] = 0;
      if (strcmp((char *)data, "ping") == 0) {
        ws.textAll("pong");
      }
    }
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DATA) {
    handleWebSocketMessage(arg, data, len);
  }
}

void setUpDNSServer(DNSServer &dnsServer, const IPAddress &localIP) {
  dnsServer.setTTL(3600);
  dnsServer.start(53, "*", localIP);
}

void startSoftAccessPoint(const char *ssid, const char *password, const IPAddress &localIP, const IPAddress &gatewayIP) {
  WiFi.mode(WIFI_MODE_AP);
  WiFi.softAPConfig(localIP, gatewayIP, subnetMask);
  WiFi.softAP(ssid, password, 6, 0, 4);

  esp_wifi_stop();
  esp_wifi_deinit();
  wifi_init_config_t my_config = WIFI_INIT_CONFIG_DEFAULT();
  my_config.ampdu_rx_enable = false;
  esp_wifi_init(&my_config);
  esp_wifi_start();
  vTaskDelay(100 / portTICK_PERIOD_MS);
  apActive = true;
}

void setUpWebserver(AsyncWebServer &server, const IPAddress &localIP) {
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.on("/connecttest.txt", [](AsyncWebServerRequest *request) {
    request->redirect("http://logout.net");
  });
  server.on("/wpad.dat", [](AsyncWebServerRequest *request) {
    request->send(404);
  });

  server.on("/generate_204", [](AsyncWebServerRequest *request) {
    request->redirect(localIPURL);
  });
  server.on("/redirect", [](AsyncWebServerRequest *request) {
    request->redirect(localIPURL);
  });
  server.on("/hotspot-detect.html", [](AsyncWebServerRequest *request) {
    request->redirect(localIPURL);
  });
  server.on("/canonical.html", [](AsyncWebServerRequest *request) {
    request->redirect(localIPURL);
  });
  server.on("/success.txt", [](AsyncWebServerRequest *request) {
    request->send(200);
  });
  server.on("/ncsi.txt", [](AsyncWebServerRequest *request) {
    request->redirect(localIPURL);
  });

  server.on("/favicon.ico", [](AsyncWebServerRequest *request) {
    request->send(404);
  });

  server.on("/", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/index.html", "text/html");
    Serial.println("Served Basic HTML Page from LittleFS");
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    if (LittleFS.exists(request->url())) {
      request->send(LittleFS, request->url(), String(), false);
      Serial.println("Served " + request->url() + " from LittleFS");
    } else {
      request->redirect(localIPURL);
      Serial.print("onnotfound ");
      Serial.print(request->host());
      Serial.print(" ");
      Serial.print(request->url());
      Serial.print(" sent redirect to " + localIPURL + "\n");
    }
  });

  ws.onEvent(onEvent);
  server.addHandler(&ws);

  server.begin();
}

void enterDeepSleep() {
  Serial.println("Entering deep sleep");
  accel.disableAccelerometer();                  // Disable accelerometer before deep sleep
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_35, 1);  // Wake up on button press
  esp_deep_sleep_start();
}

void setupAccelerometer() {
  pinMode(SENSOR_IRQ, INPUT);
  attachInterrupt(digitalPinToInterrupt(SENSOR_IRQ), onSensorIRQ, RISING);

  Wire.begin(SENSOR_SDA, SENSOR_SCL);
  Wire.setClock(400000);  // Set I2C frequency to 400kHz for faster communication

  if (!accel.begin(Wire, BMA423_SLAVE_ADDRESS, SENSOR_SDA, SENSOR_SCL)) {
    Serial.println("Failed to find BMA423 - check your wiring!");
    while (1) {
      delay(1000);
    }
  }

  Serial.println("Init BMA423 Sensor success!");

  accel.configAccelerometer();
  accel.enableAccelerometer();
  accel.enablePedometer();
  accel.resetPedometer();
  accel.enableFeature(SensorBMA423::FEATURE_STEP_CNTR | SensorBMA423::FEATURE_ANY_MOTION | SensorBMA423::FEATURE_ACTIVITY | SensorBMA423::FEATURE_TILT | SensorBMA423::FEATURE_WAKEUP, true);

  accel.enablePedometerIRQ();
  accel.enableTiltIRQ();
  accel.enableWakeupIRQ();
  accel.enableAnyNoMotionIRQ();
  accel.enableActivityIRQ();
  accel.configInterrupt();
}

void displayWatchface() {
    if (apActive != previousApState) {  // only update the display if the state changes
        Serial.println("Updating display");
        previousApState = apActive;
        display.initWatchy();  // Initialize the display
        if (LittleFS.exists("/watchface.bmp")) {
            display.renderBMP("/watchface.bmp");
        }
        if (apActive) {
            display.setFont(&FreeMonoBold9pt7b);
            display.setTextColor(GxEPD_BLACK);
            display.setCursor(30, 80);
            display.println("Server active");
        }
        display.display(true);  // full refresh to update the screen
    }
}


void setup() {
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  while (!Serial)
    ;
  delay(1000);
  Serial.println("\n\nCripTime, V0.0.2 compiled " __DATE__ " " __TIME__ " by hrk");
  Serial.printf("%s-%d\n\r", ESP.getChipModel(), ESP.getChipRevision());

  if (!LittleFS.begin()) {
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }
  listFiles();

  pinMode(MENU_BTN_PIN, INPUT);  // Set button pin as input
  pinMode(AP_BTN_PIN, INPUT);    // Set AP button pin as input

  attachInterrupt(digitalPinToInterrupt(AP_BTN_PIN), onAPButtonPress, RISING);  // Attach interrupt

  // Initialize the display
  display.initWatchy();
  displayWatchface();

  // Setup Accelerometer
  setupAccelerometer();

  apActive = false;  // the ap should be active when setup is running.

  // Start the WiFi AP and Web Server if the button is pressed
  if (digitalRead(AP_BTN_PIN) == HIGH) {
    startSoftAccessPoint(ssid, password, localIP, gatewayIP);
    setUpDNSServer(dnsServer, localIP);
    setUpWebserver(server, localIP);
  } else {
    enterDeepSleep();
  }
}

void loop() {
  dnsServer.processNextRequest();

  if (sensorIRQ) {
    sensorIRQ = false;
    uint16_t status = accel.readIrqStatus();

    int16_t x, y, z;
    if (accel.getAccelerometer(x, y, z)) {
      StaticJsonDocument<200> jsonDoc;
      jsonDoc["x"] = x;
      jsonDoc["y"] = y;
      jsonDoc["z"] = z;
      String jsonString;
      serializeJson(jsonDoc, jsonString);
      ws.textAll(jsonString);
    }

    if (accel.isPedometer()) {
      uint32_t stepCounter = accel.getPedometerCounter();
      Serial.printf("Step count interrupt, step Counter:%u\n", stepCounter);
      StaticJsonDocument<200> jsonDoc;
      jsonDoc["steps"] = stepCounter;
      String jsonString;
      serializeJson(jsonDoc, jsonString);
      ws.textAll(jsonString);
    }
  }


  displayWatchface();  // Display watchface with "Server active" text

  if (digitalRead(MENU_BTN_PIN) == HIGH) {  // Button is pressed
    if (!buttonPressed) {
      buttonPressTime = millis();
      buttonPressed = true;
    } else if (millis() - buttonPressTime >= longPressDuration) {
      Serial.println("Button held for 1 second, entering deep sleep...");
      server.end();
      apActive = false;
      WiFi.softAPdisconnect(true);
      displayWatchface();
      enterDeepSleep();
    }
  } else {
    buttonPressed = false;
  }
}
