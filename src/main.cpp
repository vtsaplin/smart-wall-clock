#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_GFX.h>
#include <TimeLib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <Scheduler.h>

#define DISPLAY_WIDTH 32
#define DISPLAY_HEIGHT 8 
#define LED_PIN 2

#define DEFAULT_BRIGHTNESS 2
#define DEFAULT_ANIMATION_SPEED 18.0

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600);

ESP8266WebServer server(80);

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(DISPLAY_WIDTH, DISPLAY_HEIGHT, LED_PIN,
  NEO_MATRIX_TOP     + NEO_MATRIX_LEFT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
  NEO_GRB            + NEO_KHZ800);

int displayColor[3] = { 74, 171, 255 };
int displayBrightness = DEFAULT_BRIGHTNESS;

int alertColor[3] = { 255, 101, 74 };

String alert;
long alertExiration = 0;

long startTime = -1;
long deltaTime = 0;
float animationPosition = 0;
float animationSpeed = DEFAULT_ANIMATION_SPEED;

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
      delay(500);
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

void handleGetSetBrightness() {
  if(server.hasArg("value")) {
    displayBrightness = server.arg("value").toInt();
  }
  server.send(200, "text/plain", String(displayBrightness));
}

void handleGetSetSpeed() {
  if(server.hasArg("value")) {
    animationSpeed = (float)server.arg("value").toInt();
  }
  server.send(200, "text/plain", String(animationSpeed));
}

void handleSetAlert() {
  if(server.hasArg("text")) {
    alert = server.arg("text");
    if(server.hasArg("validFor")) {
      alertExiration = millis() + server.arg("validFor").toInt();
    } else {
      alertExiration = 0;
    }
    server.send(200, "text/plain", "Alert set");
  }
  server.send(400, "text/plain", "Missing arguments");
}

void handleClearAlert() {
  alert = String();
  server.send(200, "text/plain", "Alert cleared");
}

class WebServerTask : public Task {
protected:
    void loop() {
      server.handleClient();
      delay(500);
    }
} web_server_task;

void setupWebServer() {
  server.on("/brightness", handleGetSetBrightness);
  server.on("/speed", handleGetSetSpeed);
  server.on("/setAlert", handleSetAlert);
  server.on("/clearAlert", handleClearAlert);
  server.begin();   
  Scheduler.start(&web_server_task);
}

int getTextWidth(String text) {
  int16_t x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  return w;
}

void showAlerts() {  
  matrix.setCursor((int)floor(animationPosition), 0);
  matrix.setTextColor(matrix.Color(alertColor[0], alertColor[1], alertColor[2]));
  matrix.print(alert);
  animationPosition -= (float)deltaTime / 1000.0 * animationSpeed;
  if (animationPosition + getTextWidth(alert) < 0) {
    animationPosition = (float)DISPLAY_WIDTH - 1;
  }
  if (alertExiration > 0 && alertExiration < millis()) {
    alert = String();
    alertExiration = 0;
  }
}

void showClock() {
    matrix.setCursor(1, 0);
    int status = timeStatus();
    if (status == timeSet) {
      matrix.setTextColor(matrix.Color(displayColor[0], displayColor[1], displayColor[2]));
    } else {
      matrix.setTextColor(matrix.Color(alertColor[0], alertColor[1], alertColor[2]));
    }
    sprintf(charBuffer, "%02d:%02d", hour(), minute());
    matrix.print(charBuffer);
}

void computeTimeDelta() {
  long currentTime = millis();
  deltaTime = currentTime - startTime;
  startTime = currentTime;
}

class DisplayTask : public Task {
protected:
    void loop() {
      computeTimeDelta();
      matrix.fillScreen(0);
      matrix.setBrightness(displayBrightness);
      if (alert.length() > 0) {
        showAlerts();
      } else {
        showClock();
      }
      matrix.show();
      delay(50);
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
  setupOta();
  setupTime();
  setupWebServer();
  setupDisplay();
  Scheduler.begin();
}

void loop() {
}
