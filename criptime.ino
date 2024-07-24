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
#define VIB_MOTOR_PIN 13

char ssid[32] = "CripTime";
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

unsigned long lastAccelUpdate = 0;
const unsigned long accelUpdateInterval = 100;  // Update every 0.1 second

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
      } else if (strcmp((char *)data, "buzz") == 0) {
        vibMotor(75, 4);  // vibrate the motor
        ws.textAll("buzzed");
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
  getSSIDFromFS();
  WiFi.mode(WIFI_MODE_AP);
  WiFi.softAPConfig(localIP, gatewayIP, subnetMask);
  WiFi.softAP(ssid, password, 6, 0, 4);
  Serial.print("softAP SSID: ");
  Serial.println(ssid);

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

  server.on("/buzz", HTTP_ANY, [](AsyncWebServerRequest *request) {
    vibMotor(75, 4);     // vibrate the motor
    request->send(200);  // send ok
  });

  /*
  handle post messages.
  store "message" post value in a file.
  returns all the messages in the response.
  */
  server.on("/message", HTTP_POST, [](AsyncWebServerRequest *request) {
    // create message.txt if it doesn't exist
    if (!LittleFS.exists("/message.txt")) {
      File file = LittleFS.open("/message.txt", "w");
      if (!file) {
        Serial.println("Failed to create file for writing");
        request->send(500, "text/plain", "Failed to create file for writing");
        return;
      }
      file.close();
    }

    if (request->hasParam("message", true)) {
      AsyncWebParameter *message = request->getParam("message", true);
      File file = LittleFS.open("/message.txt", "a"); // should exist right?
      file.print(message->value() + "\n");
      file.close();
    } else {
      Serial.println("No message param. Nothing to add.");
    }

    String response = "Messages:\n";
    File file = LittleFS.open("/message.txt", "r");
    while (file.available()) {
      response += file.readStringUntil('\n') + "\n";
    }
    file.close();
    request->send(200, "text/plain", response);
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

void vibMotor(uint8_t intervalMs, uint8_t length) {
  pinMode(VIB_MOTOR_PIN, OUTPUT);
  bool motorOn = false;
  for (int i = 0; i < length; i++) {
    motorOn = !motorOn;
    digitalWrite(VIB_MOTOR_PIN, motorOn);
    delay(intervalMs);
  }
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

void displayWatchface(bool renderServerText = false) {
  if (LittleFS.exists("/watchface.bmp")) {
    display.renderBMP("/watchface.bmp");
    Serial.println("Displayed watchface");
  }

  if (renderServerText) {
    Serial.println("Render Server active text");
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(30, 70);
    display.println("Server active");
    display.print("SSID: ");
    display.println(ssid);
  }
  display.display(true);  // full refresh to update the screen
}

void getSSIDFromFS() {
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String n = file.name();
    Serial.println(n);
    int dot = n.lastIndexOf(".");
    String ext = n.substring(dot);
    if (ext == ".ssid") {
      String hostName;
      if (n.substring(0, 1) == "/") {
        hostName = n.substring(1, dot);
      } else {
        hostName = n.substring(0, dot);
      }
      Serial.print("Using hostname: ");
      Serial.println(hostName);
      hostName.toCharArray(ssid, sizeof(ssid));
      break;  // Exit loop after finding the SSID file
    }
    file = root.openNextFile();
  }
}

void setup() {
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  while (!Serial)
    ;
  delay(2000);
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
  delay(1000);
  displayWatchface(false);

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

  unsigned long currentMillis = millis();
  if (currentMillis - lastAccelUpdate >= accelUpdateInterval) {
    lastAccelUpdate = currentMillis;

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
  }

  if (apActive != previousApState) {  // only update the display if the state changes
    previousApState = apActive;
    if (apActive) {
      displayWatchface(true);  // Display watchface with "Server active" text
    }
  }

  if (digitalRead(MENU_BTN_PIN) == HIGH) {  // Button is pressed
    if (!buttonPressed) {
      buttonPressTime = millis();
      buttonPressed = true;
    } else if (millis() - buttonPressTime >= longPressDuration) {
      Serial.println("Button held for 1 second, entering deep sleep...");
      server.end();
      apActive = false;
      WiFi.softAPdisconnect(true);
      displayWatchface();  // removes the server active text
      enterDeepSleep();
    }
  } else {
    buttonPressed = false;
  }
}
