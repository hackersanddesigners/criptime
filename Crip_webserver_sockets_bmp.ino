#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include "WatchyDisplay.h"
#include <Wire.h>

const char *ssid = "Watchy";
const char *password = NULL;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dnsServer;

const IPAddress localIP(4, 3, 2, 1);
const IPAddress gatewayIP(4, 3, 2, 1);
const IPAddress subnetMask(255, 255, 255, 0);
const String localIPURL = "http://4.3.2.1";

const int buttonPin = 26; 
unsigned long buttonPressTime = 0;
bool buttonPressed = false;
const unsigned long longPressDuration = 3 * 1000;  // 3 seconds
bool setupComplete = false;

// Initialize the Watchy display
WatchyDisplay display(DISPLAY_CS, DISPLAY_DC, DISPLAY_RES, DISPLAY_BUSY);


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
}

void setUpWebserver(AsyncWebServer &server, const IPAddress &localIP) {
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.on("/connecttest.txt", [](AsyncWebServerRequest *request) { request->redirect("http://logout.net"); });
  server.on("/wpad.dat", [](AsyncWebServerRequest *request) { request->send(404); });

  server.on("/generate_204", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });
  server.on("/redirect", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });
  server.on("/hotspot-detect.html", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });
  server.on("/canonical.html", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });
  server.on("/success.txt", [](AsyncWebServerRequest *request) { request->send(200); });
  server.on("/ncsi.txt", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });

  server.on("/favicon.ico", [](AsyncWebServerRequest *request) { request->send(404); });

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

void setup() {
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  while (!Serial);
  delay(1000);
  Serial.println("\n\nCaptive Test, V0.5.0 compiled " __DATE__ " " __TIME__ " by CD_FER");
  Serial.printf("%s-%d\n\r", ESP.getChipModel(), ESP.getChipRevision());

  if (!LittleFS.begin()) {
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }
  listFiles();
  
  startSoftAccessPoint(ssid, password, localIP, gatewayIP);
  setUpDNSServer(dnsServer, localIP);
  setUpWebserver(server, localIP);

  pinMode(buttonPin, INPUT); 
  setupComplete = true;

  // Initialize the display
  display.initWatchy();

  Serial.print("\n");
  Serial.print("Startup Time:");
  Serial.println(millis());
  Serial.print("\n");

  // Display BMP image from the filesystem
  display.displayBMP("/watchface.bmp");

  // Optionally, power off the display if needed
  Serial.println("Putting display in hibernate mode...");
  display.hibernate();
  Serial.println("Setup complete.");
}

void loop() {
  dnsServer.processNextRequest();
  ws.cleanupClients();
  if (setupComplete) {
    Serial.println(digitalRead(buttonPin));
    if (digitalRead(buttonPin) == HIGH) {  // Button is pressed
      if (!buttonPressed) {
        buttonPressTime = millis();
        buttonPressed = true;
      } else if (millis() - buttonPressTime >= longPressDuration) {
        Serial.print("Button held for ");
        Serial.print((int)longPressDuration/1000);
        Serial.println(" seconds, restarting...");
        ESP.restart();
      }
    } else {
      buttonPressed = false;
    }

    delay(30);  // Yield control to avoid watchdog timeout
  }
  delay(30);
}
