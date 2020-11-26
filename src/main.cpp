#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_GFX.h>
#include <TimeLib.h>
#include <Scheduler.h>
#include <RemoteDebug.h>

#define OTA_DELAY 1000
#define WEB_SERVER_DELAY 1000
#define REMOTE_DEBUG_DELAY 1000

#define SCROLL_DELAY 50
#define CLOCK_DELAY 1000

#define DISPLAY_WIDTH 32
#define DISPLAY_HEIGHT 8 
#define LED_PIN 2

#define BRIGHTNESS 2
#define SCROLL_SPEED 18.0

#define MAX_MESSAGES 3
#define MESSAGE_DELAY 10000
#define MESSAGE_PADDING 5

struct Message {
  String label;
  String text;
  uint16_t color;
  bool enabled;
};

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600);

RemoteDebug Debug;

ESP8266WebServer server(80);

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(DISPLAY_WIDTH, DISPLAY_HEIGHT, LED_PIN,
  NEO_MATRIX_TOP     + NEO_MATRIX_LEFT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
  NEO_GRB            + NEO_KHZ800);

uint16_t displayColor = matrix.Color(74, 171, 255);
int displayBrightness = BRIGHTNESS;

uint16_t alertColor = matrix.Color(255, 101, 74);

String alert;
unsigned long alertTimeout = 0;

Message messages[MAX_MESSAGES];
unsigned long nextMessage = 0;
bool messageMode = false;

unsigned long startTime = -1;
unsigned long deltaTime = 0;

float scrollPosition = 0;
float scrollSpeed = SCROLL_SPEED;

char charBuffer[256];

time_t getNtpTime() {
  timeClient.update();
  return timeClient.getEpochTime();
}

void setupTime() {
  timeClient.begin();
  setSyncProvider(&getNtpTime);
  setSyncInterval(60*5);
}

void setupWifi() {
  Serial.begin(115200);
  Serial.println("Booting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
  Serial.println("WIFI connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

class OtaTask : public Task {
protected:
    void loop() {
      ArduinoOTA.handle();
      delay(OTA_DELAY);
    }
} ota_task;

void setupOta() {  
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASS);

  Serial.print("OTA hostname: ");
  Serial.println(OTA_HOSTNAME);

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
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

  Scheduler.start(&ota_task);
}

class RemoteDebugTask : public Task {
protected:
    void loop() {
      Debug.handle();
      delay(REMOTE_DEBUG_DELAY);
    }
} remote_debug_task;

void setupRemoteDebug() {  
  Debug.begin(OTA_HOSTNAME);
  Debug.setResetCmdEnabled(true);
  Scheduler.start(&remote_debug_task);
}

void handleGetSetBrightness() {
  if(server.hasArg("value")) {
    displayBrightness = server.arg("value").toInt();
  }
  server.send(200, "text/plain", String(displayBrightness));
}

void handleGetSetSpeed() {
  if(server.hasArg("value")) {
    scrollSpeed = (float)server.arg("value").toInt();
  }
  server.send(200, "text/plain", String(scrollSpeed));
}

void handleSetAlert() {
  if(server.hasArg("text")) {
    alert = server.arg("text");
    if(server.hasArg("timeout")) {
      alertTimeout = millis() + server.arg("timeout").toInt();
    } else {
      alertTimeout = 0;
    }
    server.send(200, "text/plain", "Alert set");
  }
  server.send(400, "text/plain", "Missing arguments");
}

void handleClearAlert() {
  alert = String();
  server.send(200, "text/plain", "Alert cleared");
}

void handleSetMessage() {
  if(server.hasArg("index") && 
     server.hasArg("label") && 
     server.hasArg("text") && 
     server.hasArg("color")) {
    int index = strtol(server.arg("index").c_str(), NULL, 10);
    String label = server.arg("label");
    String text = server.arg("text");
    unsigned long color = strtol(server.arg("color").c_str(), NULL, 16);
    messages[index] = { label, text, matrix.Color((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff), true };
    server.send(200, "text/plain", "Message set");
  }
  server.send(400, "text/plain", "Missing arguments");
}

void handleClearMessage() {
  if(server.hasArg("index")) {
    int index = strtol(server.arg("index").c_str(), NULL, 10);
    messages[index].enabled = false;
    server.send(200, "text/plain", "Message cleared");
  }
  server.send(400, "text/plain", "Missing arguments");
}

class WebServerTask : public Task {
protected:
    void loop() {
      server.handleClient();
      delay(WEB_SERVER_DELAY);
    }
} web_server_task;

void setupWebServer() {
  server.on("/brightness", handleGetSetBrightness);
  server.on("/scrollSpeed", handleGetSetSpeed);
  server.on("/setAlert", handleSetAlert);
  server.on("/clearAlert", handleClearAlert);
  server.on("/setMessage", handleSetMessage);
  server.on("/clearMessage", handleClearMessage);
  server.begin();   
  Scheduler.start(&web_server_task);
}

int getTextWidth(String text) {
  int16_t x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  return w;
}

void displayClock() {
    matrix.setCursor(1, 0);
    matrix.setTextColor(displayColor);
    const char * delim = second() % 2 ? ":" : " ";
    sprintf(charBuffer, "%02d%s%02d", hour(), delim, minute());
    matrix.print(charBuffer);
    matrix.show();
}

void resetMessageMode() {
  messageMode = false;
  nextMessage = millis() + MESSAGE_DELAY;
}

void displayMessages() {
  int totalLength = 0;
  for (int i=0; i<MAX_MESSAGES; i++) {
    if (messages[i].enabled) {
      matrix.setCursor((int)floor(scrollPosition) + totalLength, 0);
      matrix.setTextColor(messages[i].color);
      String message = messages[i].label + ":" + messages[i].text;
      matrix.print(message);
      totalLength += getTextWidth(message) + MESSAGE_PADDING;
    }
  }
  matrix.show();
  scrollPosition -= (float)deltaTime / 1000.0 * scrollSpeed;
  if (scrollPosition + totalLength < 0) {
    resetMessageMode();
  }
}

void diplayAlerts() {  
  matrix.setCursor((int)floor(scrollPosition), 0);
  matrix.setTextColor(alertColor);
  matrix.print(alert);
  matrix.show();
  scrollPosition -= (float)deltaTime / 1000.0 * scrollSpeed;
  if (scrollPosition + getTextWidth(alert) < 0) {
    scrollPosition = (float)DISPLAY_WIDTH - 1;
  }
  if (alertTimeout > 0 && alertTimeout < millis()) {
    alert = String();
    alertTimeout = 0;
  }
}

void calcTimeDelta() {
  long currentTime = millis();
  deltaTime = currentTime - startTime;
  startTime = currentTime;
}

class DisplayTask : public Task {
protected:
    void loop() {
      calcTimeDelta();
      matrix.fillScreen(0);
      matrix.setBrightness(displayBrightness);
      if (alert.length() > 0) {
        diplayAlerts();
        resetMessageMode();
        delay(SCROLL_DELAY);
      } else {
        if (!messageMode && millis() > nextMessage) {
          scrollPosition = DISPLAY_WIDTH;
          messageMode = true;
        } 
        if (messageMode) {
          displayMessages();
          delay(SCROLL_DELAY);
        } else {
          displayClock();
          delay(CLOCK_DELAY);
        }
      }
    }
} display_task;

void setupDisplay() {
  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setTextSize(1);
  Scheduler.start(&display_task);
}

void setup() {
  setupWifi();
  setupRemoteDebug();
  setupOta();
  setupTime();
  setupWebServer();
  setupDisplay();
  Scheduler.begin();
}

void loop() {
}
