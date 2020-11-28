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
unsigned long nextMessageMillis = 0;

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

void handleSetAlert() {
  debugV("setting alert");
  if(server.hasArg("text")) {
    alert = server.arg("text");
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
      for (int i=0; i<10; i++) {
        displayClock(getMessageCount() > 0 && i == 0);
        if (alert.length() > 0) {
          diplayAlerts();
        }
      }
      if (getMessageCount() > 0 && millis() > nextMessageMillis) {
        displayMessages();
      }
    }
private:
  void displayClock(bool animate) {
      matrix.setBrightness(displayBrightness);
      matrix.setTextColor(displayColor);
      sprintf(charBuffer, "%02d:%02d", hour(), minute());
      if (animate) {
        for (int j=DISPLAY_HEIGHT; j>=0; j--) {
          matrix.fillScreen(0);
          matrix.setCursor(1, j);
          matrix.print(charBuffer);
          matrix.show();
          delay(50);
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
          matrix.setCursor(0, j);
          matrix.print(messages[i].label);
          matrix.show();
          delay(50);
        }
        delay(1000);
        for (int j=DISPLAY_HEIGHT; j>=0; j--) {
          matrix.fillScreen(0);
          matrix.setCursor(0, j);
          matrix.print(messages[i].text);
          matrix.show();
          delay(50);
        }
        delay(2000);
      }
    }
    nextMessageMillis = millis() + MESSAGE_DELAY;
  }

  void diplayAlerts() {  
    debugV("showing alert=%s", alert.c_str());
    int textWidth = getTextWidth(alert);
    debugV("textWidth=%d", textWidth);
    while(true) {
      for (int i=DISPLAY_WIDTH; i + textWidth < 0; i--) {
        debugV("alert pos=%d", i);
        matrix.setCursor(i, 0);
        matrix.setBrightness(displayBrightness);
        matrix.setTextColor(alertColor);
        matrix.print(alert);
        matrix.show();
        delay(50);
      }
      if (alert.length() == 0) {
        debugV("no defined alert - exit");
        return;
      }
      if (alertTimeout > 0 && alertTimeout < millis()) {
        alert = String();
        alertTimeout = 0;
        debugV("alert expired - exit");
        return;
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
