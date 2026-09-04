#include <M5EPD.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

M5EPD_Canvas canvas(&M5.EPD);

struct {
  char ssid[64];
  char password[64];
} wifi_config = {};

struct {
  float temp;
  int humidity;
  String condition;
} hk_weather = {0, 0, "N/A"};

uint32_t last_weather_update = 0;
const uint32_t WEATHER_UPDATE_INTERVAL = 600000;

void drawErrorScreen(const char* error);
void enterSetupMode();
void loadWiFiConfig();
void connectToWiFi();
void fetchWeather();
void drawDualScreen(float roomTemp, float roomHum, uint32_t battMv);
String touchInputScreen(const char* title, bool isPassword);
void saveWiFiConfig();
int scanWiFiNetworks(String networks[], int maxNetworks);
int selectSSIDFromList(String networks[], int count);

void setup() {
  M5.begin();
  M5.EPD.SetRotation(0);
  M5.TP.SetRotation(0);
  M5.EPD.Clear(true);

  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== M5Paper Weather Monitor ===");

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed!");
    drawErrorScreen("SPIFFS Error");
    return;
  }

  M5.SHT30.Begin();
  canvas.createCanvas(960, 540);

  loadWiFiConfig();

  if (strlen(wifi_config.ssid) == 0) {
    Serial.println("No WiFi config found. Entering setup mode...");
    enterSetupMode();
  } else {
    connectToWiFi();
  }
}

void loop() {
  M5.SHT30.UpdateData();
  float temperature = M5.SHT30.GetTemperature();
  float humidity = M5.SHT30.GetRelHumidity();
  uint32_t battery = M5.getBatteryVoltage();

  if (millis() - last_weather_update > WEATHER_UPDATE_INTERVAL && WiFi.status() == WL_CONNECTED) {
    fetchWeather();
    last_weather_update = millis();
  }

  Serial.printf("Room: %.1f°C, %.0f%% | HK: %.1f°C, %s\n",
                temperature, humidity, hk_weather.temp, hk_weather.condition.c_str());

  drawDualScreen(temperature, humidity, battery);

  // Sleep for 60 seconds but check touch every 100ms
  for (int i = 0; i < 600; i++) {
    delay(100);

    // Check for config button tap in top-right corner (880-960, 0-60)
    if (M5.TP.available()) {
      M5.TP.update();
      if (!M5.TP.isFingerUp()) {
        tp_finger_t finger = M5.TP.readFinger(0);
        if (finger.x > 880 && finger.x < 960 && finger.y > 0 && finger.y < 60) {
          Serial.println("\n⚙️  Config button tapped! Entering setup mode...");
          delay(500);
          enterSetupMode();
          return;  // Return to restart loop after setup
        }
      }
    }
  }
}

// ============ WiFi NETWORK SCANNING ============
int scanWiFiNetworks(String networks[], int maxNetworks) {
  canvas.fillCanvas(0);
  canvas.setTextSize(3);
  canvas.setTextColor(15);
  canvas.drawString("Scanning WiFi...", 200, 250);
  canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

  Serial.println("Scanning WiFi networks...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int networkCount = WiFi.scanNetworks();
  Serial.printf("Found %d networks\n", networkCount);

  int added = 0;
  for (int i = 0; i < networkCount && added < maxNetworks; i++) {
    networks[added] = WiFi.SSID(i);
    Serial.printf("  %d: %s (RSSI: %d)\n", added, networks[added].c_str(), WiFi.RSSI(i));
    added++;
  }

  return added;
}

// ============ SSID SELECTION SCREEN ============
int selectSSIDFromList(String networks[], int count) {
  int itemsPerPage = 5;
  int startIdx = 0;
  int endIdx = min(startIdx + itemsPerPage, count);

  canvas.fillCanvas(0);
  canvas.setTextSize(3);
  canvas.setTextColor(15);
  canvas.drawString("Select WiFi Network", 100, 20);
  canvas.drawFastHLine(40, 70, 880, 15);

  for (int i = startIdx; i < endIdx; i++) {
    int yPos = 100 + (i - startIdx) * 80;
    canvas.drawRect(50, yPos, 860, 70, 15);
    canvas.setTextSize(2);
    canvas.setTextColor(15);
    canvas.drawString(networks[i], 70, yPos + 20);
  }

  // Add CANCEL button
  canvas.setTextSize(2);
  canvas.drawString("Tap network to select | Long wait to cancel", 40, 500);
  canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

  Serial.println("Waiting for SSID selection...");
  Serial.println("Expected touch zones:");
  for (int i = 0; i < endIdx - startIdx; i++) {
    int yPos = 100 + i * 80;
    Serial.printf("  Network %d: y=%d to y=%d\n", i, yPos, yPos + 70);
  }

  uint32_t lastTouchTime = 0;
  uint32_t touchStartTime = 0;
  bool touchActive = false;

  while (true) {
    delay(50);

    if (M5.TP.available()) {
      M5.TP.update();

      if (!M5.TP.isFingerUp()) {
        uint32_t now = millis();

        tp_finger_t finger = M5.TP.readFinger(0);
        int x = finger.x;
        int y = finger.y;

        // First touch - record time
        if (!touchActive) {
          touchActive = true;
          touchStartTime = now;
          lastTouchTime = now;
          Serial.printf("Touch started at x=%d, y=%d\n", x, y);
        }

        // Check if held too long (cancel)
        if (now - touchStartTime > 3000) {
          Serial.println("Long hold detected, canceling WiFi selection");
          return -1;
        }

        // Quick tap - select network
        if (now - lastTouchTime > 300) {
          Serial.printf("Checking touch at x=%d, y=%d against network boxes\n", x, y);

          for (int i = startIdx; i < endIdx; i++) {
            int yPos = 100 + (i - startIdx) * 80;
            Serial.printf("  Checking against network %d (y=%d to %d)... ", i, yPos, yPos + 70);

            if (x > 50 && x < 910 && y > yPos && y < yPos + 70) {
              Serial.printf("MATCH! ✓\n");
              Serial.printf("✓ Selected SSID: %s (index %d)\n", networks[i].c_str(), i);
              delay(500);
              return i;
            } else {
              Serial.println("no");
            }
          }

          lastTouchTime = now;
        }
      } else {
        touchActive = false;
      }
    }
  }
}

// ============ TOUCH KEYBOARD INPUT (QWERTY - IMPROVED) ============
String touchInputScreen(const char* title, bool isPassword) {
  String input = "";
  bool done = false;
  uint32_t lastTouchTime = 0;
  const uint32_t DEBOUNCE_MS = 250;

  int pressedKeyX = -1, pressedKeyY = -1;  // For visual feedback

  // QWERTY layout (larger keyboard)
  const char* row1 = "qwertyuiop";
  const char* row2 = "asdfghjkl";
  const char* row3 = "zxcvbnm";
  const char* row4 = "0123456789.-@ ";

  while (!done) {
    canvas.fillCanvas(0);

    // Title
    canvas.setTextSize(3);
    canvas.setTextColor(15);
    canvas.drawString(title, 40, 10);

    // Input display (show actual text)
    canvas.setTextSize(4);
    String displayInput = input.length() > 0 ? input : "Enter password...";
    canvas.drawString(displayInput, 40, 70);
    canvas.drawFastHLine(40, 120, 880, 15);

    // Larger keyboard layout
    int keyW = 86, keyH = 60;
    int startX = 40;
    int y = 145;

    // Row 1: QWERTY
    for (int i = 0; i < strlen(row1); i++) {
      int x = startX + i * keyW;
      bool isPressed = (pressedKeyX == x && pressedKeyY == y);

      if (isPressed) {
        canvas.fillRect(x, y, keyW - 2, keyH - 2, 15);  // Filled (black)
        canvas.setTextColor(0);  // Text in white
      } else {
        canvas.drawRect(x, y, keyW - 2, keyH - 2, 15);  // Outline
        canvas.setTextColor(15);  // Text in black
      }

      canvas.setTextSize(2);
      canvas.drawString(String(row1[i]), x + 32, y + 18);
    }

    // Row 2: ASDF (offset)
    y += keyH;
    for (int i = 0; i < strlen(row2); i++) {
      int x = startX + 43 + i * keyW;
      bool isPressed = (pressedKeyX == x && pressedKeyY == y);

      if (isPressed) {
        canvas.fillRect(x, y, keyW - 2, keyH - 2, 15);
        canvas.setTextColor(0);
      } else {
        canvas.drawRect(x, y, keyW - 2, keyH - 2, 15);
        canvas.setTextColor(15);
      }

      canvas.setTextSize(2);
      canvas.drawString(String(row2[i]), x + 32, y + 18);
    }

    // Row 3: ZXCV (offset)
    y += keyH;
    for (int i = 0; i < strlen(row3); i++) {
      int x = startX + 86 + i * keyW;
      bool isPressed = (pressedKeyX == x && pressedKeyY == y);

      if (isPressed) {
        canvas.fillRect(x, y, keyW - 2, keyH - 2, 15);
        canvas.setTextColor(0);
      } else {
        canvas.drawRect(x, y, keyW - 2, keyH - 2, 15);
        canvas.setTextColor(15);
      }

      canvas.setTextSize(2);
      canvas.drawString(String(row3[i]), x + 32, y + 18);
    }

    // Row 4: Numbers
    y += keyH;
    for (int i = 0; i < strlen(row4); i++) {
      int x = startX + i * keyW;
      bool isPressed = (pressedKeyX == x && pressedKeyY == y);

      if (isPressed) {
        canvas.fillRect(x, y, keyW - 2, keyH - 2, 15);
        canvas.setTextColor(0);
      } else {
        canvas.drawRect(x, y, keyW - 2, keyH - 2, 15);
        canvas.setTextColor(15);
      }

      canvas.setTextSize(2);
      if (row4[i] == ' ') {
        canvas.drawString("SPC", x + 24, y + 18);
      } else {
        canvas.drawString(String(row4[i]), x + 32, y + 18);
      }
    }

    // Bottom action buttons (larger)
    int btnY = 410;
    int btnW = 210, btnH = 60;
    canvas.setTextSize(2);
    canvas.setTextColor(15);

    canvas.drawRect(40, btnY, btnW, btnH, 15);
    canvas.drawString("BACKSPACE", 65, btnY + 18);

    canvas.drawRect(260, btnY, btnW, btnH, 15);
    canvas.drawString("CLEAR", 315, btnY + 18);

    canvas.drawRect(480, btnY, btnW, btnH, 15);
    canvas.drawString("DONE", 535, btnY + 18);

    canvas.drawRect(700, btnY, btnW, btnH, 15);
    canvas.drawString("CANCEL", 745, btnY + 18);

    canvas.pushCanvas(0, 0, UPDATE_MODE_DU4);

    pressedKeyX = -1;  // Reset pressed key
    pressedKeyY = -1;

    // Touch detection
    if (M5.TP.available()) {
      M5.TP.update();

      if (!M5.TP.isFingerUp()) {
        uint32_t now = millis();
        if (now - lastTouchTime < DEBOUNCE_MS) {
          delay(20);
          continue;
        }

        tp_finger_t finger = M5.TP.readFinger(0);
        int tx = finger.x;
        int ty = finger.y;
        lastTouchTime = now;

        bool keyPressed = false;

        // Check Row 1
        int y = 145;
        for (int i = 0; i < strlen(row1); i++) {
          int x = startX + i * keyW;
          if (tx > x && tx < x + keyW - 2 && ty > y && ty < y + keyH - 2) {
            if (input.length() < 60) input += row1[i];
            pressedKeyX = x;
            pressedKeyY = y;
            keyPressed = true;
            break;
          }
        }

        // Check Row 2
        if (!keyPressed) {
          y += keyH;
          for (int i = 0; i < strlen(row2); i++) {
            int x = startX + 43 + i * keyW;
            if (tx > x && tx < x + keyW - 2 && ty > y && ty < y + keyH - 2) {
              if (input.length() < 60) input += row2[i];
              pressedKeyX = x;
              pressedKeyY = y;
              keyPressed = true;
              break;
            }
          }
        }

        // Check Row 3
        if (!keyPressed) {
          y += keyH;
          for (int i = 0; i < strlen(row3); i++) {
            int x = startX + 86 + i * keyW;
            if (tx > x && tx < x + keyW - 2 && ty > y && ty < y + keyH - 2) {
              if (input.length() < 60) input += row3[i];
              pressedKeyX = x;
              pressedKeyY = y;
              keyPressed = true;
              break;
            }
          }
        }

        // Check Row 4
        if (!keyPressed) {
          y += keyH;
          for (int i = 0; i < strlen(row4); i++) {
            int x = startX + i * keyW;
            if (tx > x && tx < x + keyW - 2 && ty > y && ty < y + keyH - 2) {
              if (row4[i] == ' ') {
                if (input.length() < 60) input += ' ';
              } else {
                if (input.length() < 60) input += row4[i];
              }
              pressedKeyX = x;
              pressedKeyY = y;
              keyPressed = true;
              break;
            }
          }
        }

        // Check Bottom Buttons
        if (!keyPressed) {
          int btnY = 410;
          int btnW = 210, btnH = 60;

          if (tx > 40 && tx < 40 + btnW && ty > btnY && ty < btnY + btnH) {
            if (input.length() > 0) input = input.substring(0, input.length() - 1);
          } else if (tx > 260 && tx < 260 + btnW && ty > btnY && ty < btnY + btnH) {
            input = "";
          } else if (tx > 480 && tx < 480 + btnW && ty > btnY && ty < btnY + btnH) {
            done = true;
          } else if (tx > 700 && tx < 700 + btnW && ty > btnY && ty < btnY + btnH) {
            return "";
          }
        }
      }
    }

    delay(20);
  }

  return input;
}

// ============ SETUP MODE ============
void enterSetupMode() {
  Serial.println("\n=== ENTERING WIFI SETUP MODE ===");

  String networks[20];
  int networkCount = scanWiFiNetworks(networks, 20);

  if (networkCount == 0) {
    canvas.fillCanvas(0);
    canvas.setTextSize(3);
    canvas.setTextColor(15);
    canvas.drawString("No networks found", 150, 250);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    delay(3000);
    connectToWiFi();  // Try to reconnect to previous WiFi
    return;
  }

  Serial.printf("Found %d networks, waiting for selection...\n", networkCount);
  int selectedIdx = selectSSIDFromList(networks, networkCount);

  if (selectedIdx < 0 || selectedIdx >= networkCount) {
    Serial.println("❌ Setup canceled at SSID selection, reconnecting to previous WiFi...");

    // Display reconnecting message
    canvas.fillCanvas(0);
    canvas.setTextSize(3);
    canvas.setTextColor(15);
    canvas.drawString("Reconnecting WiFi...", 150, 250);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

    Serial.println("📶 Calling connectToWiFi()...");
    connectToWiFi();

    Serial.println("📡 Calling fetchWeather()...");
    fetchWeather();

    Serial.println("✅ Setup canceled - WiFi restored");
    return;
  }

  String ssid = networks[selectedIdx];
  Serial.printf("Selected SSID: %s\n", ssid.c_str());

  String password = touchInputScreen("Enter WiFi Password", true);
  Serial.printf("🔐 Password returned from keyboard: length=%d\n", password.length());
  Serial.printf("   Raw password: [%s]\n", password.c_str());

  if (password.length() == 0) {
    Serial.println("❌ No password entered, canceling setup");

    // Display reconnecting message
    canvas.fillCanvas(0);
    canvas.setTextSize(3);
    canvas.setTextColor(15);
    canvas.drawString("Reconnecting WiFi...", 150, 250);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

    Serial.println("📶 Calling connectToWiFi()...");
    connectToWiFi();

    Serial.println("📡 Calling fetchWeather()...");
    fetchWeather();

    Serial.println("✅ Setup canceled - WiFi restored");
    return;
  } else {
    Serial.printf("⚠️  Password NOT empty (length=%d), proceeding with setup\n", password.length());
  }

  Serial.printf("Password length: %d\n", password.length());

  // Save config
  ssid.toCharArray(wifi_config.ssid, sizeof(wifi_config.ssid));
  password.toCharArray(wifi_config.password, sizeof(wifi_config.password));

  Serial.printf("Config in memory: SSID=%s, PWD=%s\n", wifi_config.ssid, wifi_config.password);

  saveWiFiConfig();

  Serial.println("✓ Config saved! Reconnecting...");
  canvas.fillCanvas(0);
  canvas.setTextSize(3);
  canvas.setTextColor(15);
  canvas.drawString("Config Saved!", 200, 200);
  canvas.setTextSize(2);
  canvas.drawString("Reconnecting in 3 seconds...", 150, 300);
  canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

  delay(3000);
  connectToWiFi();
}

void saveWiFiConfig() {
  File file = SPIFFS.open("/wifi_config.txt", FILE_WRITE);
  if (file) {
    file.printf("%s\n%s\n",
                wifi_config.ssid,
                wifi_config.password);
    file.close();
    Serial.println("Config written to SPIFFS");
  }
}

void loadWiFiConfig() {
  if (!SPIFFS.exists("/wifi_config.txt")) {
    Serial.println("No config file found");
    return;
  }

  File file = SPIFFS.open("/wifi_config.txt", FILE_READ);
  if (file) {
    file.readStringUntil('\n').toCharArray(wifi_config.ssid, sizeof(wifi_config.ssid));
    file.readStringUntil('\n').toCharArray(wifi_config.password, sizeof(wifi_config.password));
    file.close();
    Serial.printf("Loaded config: SSID=%s\n", wifi_config.ssid);
  }
}

void connectToWiFi() {
  Serial.printf("Connecting to WiFi: %s\n", wifi_config.ssid);

  canvas.fillCanvas(0);
  canvas.setTextSize(3);
  canvas.setTextColor(15);
  canvas.drawString("Connecting WiFi...", 150, 200);
  canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

  // Reset WiFi completely
  WiFi.mode(WIFI_OFF);
  delay(500);

  WiFi.mode(WIFI_STA);
  delay(500);

  WiFi.begin(wifi_config.ssid, wifi_config.password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {  // Increased attempts
    delay(250);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n✓ WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    configTime(28800, 0, "pool.ntp.org");
    delay(1000);  // Extra wait for connection to stabilize
    fetchWeather();
  } else {
    Serial.println("\n✗ WiFi connection failed!");
    drawErrorScreen("WiFi Failed");
  }
}

void fetchWeather() {
  if (!WiFi.isConnected()) {
    Serial.println("❌ WiFi not connected, skipping weather update");
    return;
  }

  delay(500);

  Serial.println("\n🌐 === FETCHING WEATHER ===");
  Serial.printf("🕐 Time: %lu\n", millis());
  Serial.printf("📶 WiFi Signal: %d dBm\n", WiFi.RSSI());

  HTTPClient http;
  String url = "https://wttr.in/Hong%20Kong?format=j1";

  Serial.printf("🔗 Fetching from: %s\n", url.c_str());

  http.begin(url);
  int httpCode = http.GET();

  Serial.printf("📊 HTTP Response: %d\n", httpCode);

  if (httpCode == 200) {
    String payload = http.getString();
    Serial.printf("📦 Payload size: %d bytes\n", payload.length());

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      JsonArray current = doc["current_condition"];
      if (current.size() > 0) {
        float newTemp = current[0]["temp_C"];
        int newHumidity = current[0]["humidity"];
        String newCondition = String((const char*)current[0]["weatherDesc"][0]["value"]);

        // Check if data changed
        if (newTemp != hk_weather.temp || newHumidity != hk_weather.humidity) {
          Serial.println("✅ NEW WEATHER DATA RECEIVED!");
        } else {
          Serial.println("⚠️  Same data as before");
        }

        hk_weather.temp = newTemp;
        hk_weather.humidity = newHumidity;
        hk_weather.condition = newCondition;

        Serial.printf("🌡️  Weather: %.0f°C (%s), 💧 Humidity: %d%%\n",
                      hk_weather.temp, hk_weather.condition.c_str(), hk_weather.humidity);
      }
    } else {
      Serial.printf("❌ JSON parse error: %s\n", error.c_str());
    }
  } else {
    Serial.printf("❌ HTTP error: %d\n", httpCode);
  }

  http.end();
  Serial.println("✅ === WEATHER FETCH COMPLETE ===\n");
}

void drawDualScreen(float roomTemp, float roomHum, uint32_t battMv) {
  canvas.fillCanvas(0);

  // WiFi config icon in top-right (tap zone 880-960, 0-60)
  canvas.drawRect(880, 0, 80, 60, 15);

  // Draw WiFi info
  canvas.setTextSize(1);
  canvas.setTextColor(15);
  if (WiFi.isConnected()) {
    // Show SSID (first 8 chars) and RSSI
    String ssidShort = String(wifi_config.ssid).substring(0, 8);
    int rssi = WiFi.RSSI();
    canvas.drawString(ssidShort.c_str(), 885, 8);
    canvas.drawString(String(rssi) + "dB", 885, 25);
    canvas.drawString("WiFi", 885, 42);
  } else {
    canvas.drawString("No WiFi", 885, 20);
  }

  canvas.setTextSize(3);
  canvas.setTextColor(15);
  canvas.drawString("Room Monitor", 40, 20);
  canvas.drawFastHLine(40, 70, 880, 15);

  // LEFT SIDE: Room Data
  canvas.setTextSize(6);
  canvas.drawString(String(roomTemp, 1) + "C", 60, 120);

  canvas.setTextSize(3);
  canvas.drawString("Room Temp", 60, 230);

  canvas.setTextSize(5);
  canvas.drawString(String(roomHum, 0) + "%", 60, 310);

  canvas.setTextSize(3);
  canvas.drawString("Humidity", 60, 420);

  // Divider
  canvas.drawFastVLine(480, 100, 380, 15);

  // RIGHT SIDE: Hong Kong Weather
  if (hk_weather.temp > -100) {
    canvas.setTextSize(6);
    canvas.drawString(String(hk_weather.temp, 0) + "C", 520, 120);

    canvas.setTextSize(3);
    canvas.drawString("Hong Kong", 520, 230);

    canvas.setTextSize(4);
    canvas.drawString(hk_weather.condition, 520, 300);

    canvas.setTextSize(2);
    canvas.drawString("Hum: " + String(hk_weather.humidity) + "%", 520, 380);
  } else {
    canvas.setTextSize(3);
    canvas.drawString("Loading...", 520, 200);
  }

  // Footer
  canvas.setTextSize(2);
  canvas.drawString("Batt: " + String(battMv / 1000.0, 2) + "V", 40, 500);

  if (WiFi.isConnected()) {
    canvas.drawString("WiFi: OK", 400, 500);
  } else {
    canvas.drawString("WiFi: Offline", 400, 500);
  }

  canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawErrorScreen(const char* error) {
  canvas.fillCanvas(0);
  canvas.setTextSize(4);
  canvas.setTextColor(15);
  canvas.drawString("ERROR", 350, 150);
  canvas.setTextSize(3);
  canvas.drawString(error, 250, 250);
  canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}
