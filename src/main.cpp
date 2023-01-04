#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Timezone.h>
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_GFX.h>
#include <TimeLib.h>
#include <Scheduler.h>
#include <RemoteDebug.h>

#define OTA_INTERVAL 500
#define WEB_SERVER_INTERVAL 500
#define REMOTE_DEBUG_INTERVAL 500

#define SCROLL_DELAY 50
#define MESSAGE_DELAY 1000

#define DISPLAY_WIDTH 32
#define DISPLAY_HEIGHT 8
#define LED_PIN 2

#define BRIGHTNESS 2
#define SCROLL_SPEED 18.0

#define MAX_MESSAGES 5

struct Message {
  String label;
  String text;
  uint16_t color;
  bool enabled;
};

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

// Central European Time (Frankfurt, Paris)
TimeChangeRule CEST = {"CEST", Last, Sun, Mar, 2, 120}; // Central European Summer Time
TimeChangeRule CET = {"CET ", Last, Sun, Oct, 3, 60}; // Central European Standard Time
Timezone CE(CEST, CET);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

RemoteDebug Debug;

ESP8266WebServer server(80);

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(DISPLAY_WIDTH, DISPLAY_HEIGHT, LED_PIN,
  NEO_MATRIX_TOP     + NEO_MATRIX_LEFT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
  NEO_GRB            + NEO_KHZ800);

uint16_t displayColor = matrix.Color(74, 171, 255);
int displayBrightness = BRIGHTNESS;

String alert;
uint16_t alertColor = matrix.Color(255, 101, 74);
unsigned long alertTimeout = 0;

Message messages[MAX_MESSAGES];

char charBuffer[256];

time_t getNtpTime() {
  timeClient.update();
  return CE.toLocal(timeClient.getEpochTime());
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
      delay(OTA_INTERVAL);
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
      delay(REMOTE_DEBUG_INTERVAL);
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

void handleSetAlert() {
  debugV("setting alert");
  if(server.hasArg("text")) {
    debugV("alert arg=%s", server.arg("text").c_str());
    alert = String(server.arg("text"));
    debugV("alert=%s", alert.c_str());
    if(server.hasArg("timeout")) {
      alertTimeout = millis() + server.arg("timeout").toInt();
      debugV("timeout=%d", alertTimeout);
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
    debugV("setting message at %d: %s, %s", index, label.c_str(), text.c_str());
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
      delay(WEB_SERVER_INTERVAL);
    }
} web_server_task;

void setupWebServer() {
  server.on("/brightness", handleGetSetBrightness);
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

bool getMessageCount() {
  int count = 0;
  for (int i=0; i<MAX_MESSAGES; i++) {
    if (messages[i].enabled) {
      count++;
    }
  }
  return count;
}

class DisplayTask : public Task {
protected:
    void loop() {
      for (int i=0; i<7; i++) {
        bool animate = getMessageCount() > 0 && i == 0;
        displayClock(animate);
        if (alert.length() > 0) {
          displayAlerts();
        }
      }
      if (getMessageCount() > 0) {
        displayMessages();
      }
    }
private:
  void displayClock(bool animate) {
      matrix.setBrightness(displayBrightness);
      matrix.setTextColor(displayColor);
      sprintf(charBuffer, "%02d:%02d", hour(), minute());
      if (animate) {
        for (int j=-DISPLAY_HEIGHT; j<=0; j++) {
          matrix.fillScreen(0);
          matrix.setCursor(1, j);
          matrix.print(charBuffer);
          matrix.show();
          delay(SCROLL_DELAY);
        }
      } else {
          matrix.fillScreen(0);
          matrix.setCursor(1, 0);
          matrix.print(charBuffer);
          matrix.show();
          delay(500);
      }
      matrix.fillScreen(0);
      matrix.setCursor(1, 0);
      sprintf(charBuffer, "%02d %02d", hour(), minute());
      matrix.print(charBuffer);
      matrix.show();
      delay(500);
  }

  void displayMessages() {
    for (int i=0; i<MAX_MESSAGES; i++) {
      if (messages[i].enabled) {
        matrix.setTextColor(messages[i].color);
        matrix.setBrightness(displayBrightness);
        for (int j=DISPLAY_HEIGHT; j>=0; j--) {
          matrix.fillScreen(0);
          matrix.setCursor(1, j);
          matrix.print(messages[i].label);
          matrix.show();
          delay(SCROLL_DELAY);
        }
        delay(MESSAGE_DELAY);
        for (int j=DISPLAY_HEIGHT; j>=0; j--) {
          matrix.fillScreen(0);
          matrix.setCursor(1, j);
          matrix.print(messages[i].text);
          matrix.show();
          delay(SCROLL_DELAY);
        }
        delay(MESSAGE_DELAY);
      }
    }
  }

  void displayAlerts() {
    debugV("showing alert=%s", alert.c_str());
    int alertWidth = getTextWidth(alert);
    debugV("alertWidth=%d", alertWidth);
    while(true) {
      if (alertTimeout > 0 && alertTimeout < millis()) {
        alert = String();
        alertTimeout = 0;
        debugV("alert expired - exit");
        return;
      }
      for (int i=DISPLAY_WIDTH; i + alertWidth >= 0; i--) {
        if (alert.length() == 0) {
          debugV("no defined alert - exit");
          return;
        }
        matrix.setBrightness(displayBrightness);
        matrix.setTextColor(alertColor);
        matrix.fillScreen(0);
        matrix.setCursor(i, 0);
        matrix.print(alert);
        matrix.show();
        delay(SCROLL_DELAY);
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
