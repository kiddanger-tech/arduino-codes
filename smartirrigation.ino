
/*
============================================================
 GREEN MILE / GRIPYRAMID TEST 7 - V7
 ESP32 + LCD + JSN-SR04T + SOIL + PUMP + WIFI + SUPABASE
============================================================

HARDWARE CONNECTIONS

LCD:
  SDA -> GPIO32
  SCL -> GPIO33

JSN-SR04T:
  TRIG -> GPIO5
  ECHO -> GPIO18 (through voltage divider)

SOIL SENSOR:
  AO -> GPIO34

RELAY:
  IN -> GPIO19 (active LOW)

BUZZER:
  GPIO23

PUMP:
  Separate suitable supply through relay contacts.

V7 IMPROVEMENTS

1. Pump polling runs in its own high-priority task.
2. Pump polling interval: 1 second.
3. Sensor upload runs in a separate task.
4. Sensor upload interval: 10 seconds.
5. Improved JSON parsing with whitespace handling.
6. Shared data protected using a critical section.
7. Separate HTTP connection and response timeouts.
8. Existing LCD recovery system preserved.
9. Existing pump safety protections preserved.

SAFETY

- Pump starts OFF.
- Website must send OFF before ON is accepted.
- Maximum continuous pump runtime: 60 seconds.
- Backend timeout: 20 seconds.
- Pump cooldown: 3 seconds.
- Relay is active LOW.

============================================================
*/

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <string.h>

// ============================================================
// 1. PIN CONFIGURATION
// ============================================================

#define LCD_SDA     32
#define LCD_SCL     33

#define JSN_TRIG    5
#define JSN_ECHO    18

#define SOIL_PIN    34

#define RELAY_PIN   19
#define BUZZER_PIN  23

// ============================================================
// 2. WIFI AND SUPABASE
// ============================================================

// Enter your NEW Wi-Fi credentials locally.

const char *WIFI_SSID = "Happy";
const char *WIFI_PASS = "sandpaper";

// Current Supabase project.

const char *SB_URL =
  "https://zjebodmbvpjspuofdfbk.supabase.co";

// Use the current project's anon/publishable key.
// NEVER use the service_role key.

const char *SB_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InpqZWJvZG1idnBqc3B1b2ZkZmJrIiwicm9sZSI6ImFub24iLCJpYXQiOjE3OTU1NDgyMjcsImV4cCI6MjExMTEyNDIyN30.4hBQaKSbQ0cHYa7QW_Dkd1ehr3vgFeygrsTvWIlbT8w";

// Tables

#define READINGS_TABLE  "readings"
#define CONTROL_TABLE   "pump_control"

#define CONTROL_FILTER  "id=eq.1"

// Reading columns

#define COL_SOIL_RAW    "soil_raw"
#define COL_SOIL_PCT    "soil_pct"
#define COL_DIST        "distance_cm"
#define COL_WATER_PCT   "water_pct"
#define COL_PUMP_ON     "pump_on"

// Control columns

#define CTRL_MODE       "mode"
#define CTRL_STATE      "manual_state"
#define CTRL_MANUAL_VAL "manual"

// ============================================================
// 3. TIMING CONFIGURATION
// ============================================================

const unsigned long POST_INTERVAL_MS = 10000;

const unsigned long POLL_INTERVAL_MS = 1000;

const unsigned long SENSOR_INTERVAL_MS = 500;

// HTTP timeouts

const unsigned long HTTP_CONNECT_TIMEOUT_MS = 1500;

const unsigned long HTTP_RESPONSE_TIMEOUT_MS = 2000;

// Wi-Fi retry

const unsigned long WIFI_RETRY_MS = 10000;

// LCD recovery

const unsigned long LCD_ON_RECOVERY_MS = 6000;

const unsigned long LCD_OFF_RECOVERY_MS = 2000;

// ============================================================
// 4. SENSOR CALIBRATION
// ============================================================

const int SOIL_DRY_RAW = 3200;

const int SOIL_WET_RAW = 1300;

const float TANK_FULL_CM = 25.0;

const float TANK_EMPTY_CM = 60.0;

// ============================================================
// 5. PUMP SAFETY
// ============================================================

const bool RELAY_ACTIVE_LOW = true;

const unsigned long REMOTE_MAX_RUN_MS = 60000;

const unsigned long PUMP_COOLDOWN_MS = 3000;

const unsigned long LINK_TIMEOUT_MS = 20000;

// ============================================================
// 6. GLOBAL OBJECTS
// ============================================================

LiquidCrystal_I2C *lcd = nullptr;

// Protect shared values between FreeRTOS tasks.

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// 7. SHARED SENSOR AND NETWORK STATE
// ============================================================

volatile int g_soilRaw = 0;

volatile int g_soilPct = 0;

volatile int g_waterPct = -1;

volatile float g_distCm = -1;

volatile bool g_haveReading = false;

volatile bool pumpOn = false;

volatile bool g_wifiOk = false;

volatile bool g_postOk = false;

volatile bool g_rowOn = false;

volatile unsigned long g_lastPollOkMs = 0;

// ============================================================
// 8. PUMP STATE
// ============================================================

unsigned long pumpStartMs = 0;

unsigned long lockUntil = 0;

bool remoteOn = false;

bool remoteArmed = false;

// ============================================================
// 9. BUZZER STATE
// ============================================================

bool buzzerOn = false;

unsigned long buzzerOffAt = 0;

// ============================================================
// 10. LCD RECOVERY STATE
// ============================================================

bool previousPumpState = false;

bool lcdOffRecoveryPending = false;

unsigned long lcdNextRecoveryAt = 0;

// ============================================================
// 11. SHARED DATA SNAPSHOTS
// ============================================================

struct SensorSnapshot {

  int soilRaw;

  int soilPct;

  int waterPct;

  float distance;

  bool haveReading;

  bool pumpState;
};

SensorSnapshot getSensorSnapshot() {

  SensorSnapshot s;

  portENTER_CRITICAL(&stateMux);

  s.soilRaw = g_soilRaw;

  s.soilPct = g_soilPct;

  s.waterPct = g_waterPct;

  s.distance = g_distCm;

  s.haveReading = g_haveReading;

  s.pumpState = pumpOn;

  portEXIT_CRITICAL(&stateMux);

  return s;
}

void updateSensorSnapshot(
  int soilRaw,
  int soilPct,
  float distance,
  int waterPct
) {

  portENTER_CRITICAL(&stateMux);

  g_soilRaw = soilRaw;

  g_soilPct = soilPct;

  g_distCm = distance;

  g_waterPct = waterPct;

  g_haveReading = true;

  portEXIT_CRITICAL(&stateMux);
}

void setPumpStateShared(bool state) {

  portENTER_CRITICAL(&stateMux);

  pumpOn = state;

  portEXIT_CRITICAL(&stateMux);
}

bool getPumpStateShared() {

  bool state;

  portENTER_CRITICAL(&stateMux);

  state = pumpOn;

  portEXIT_CRITICAL(&stateMux);

  return state;
}

void setNetworkStatus(bool wifi, bool post) {

  portENTER_CRITICAL(&stateMux);

  g_wifiOk = wifi;

  g_postOk = post;

  portEXIT_CRITICAL(&stateMux);
}

bool getWifiStatus() {

  bool value;

  portENTER_CRITICAL(&stateMux);

  value = g_wifiOk;

  portEXIT_CRITICAL(&stateMux);

  return value;
}

bool getPostStatus() {

  bool value;

  portENTER_CRITICAL(&stateMux);

  value = g_postOk;

  portEXIT_CRITICAL(&stateMux);

  return value;
}

void setWebsiteCommand(bool state) {

  portENTER_CRITICAL(&stateMux);

  g_rowOn = state;

  portEXIT_CRITICAL(&stateMux);
}

bool getWebsiteCommand() {

  bool value;

  portENTER_CRITICAL(&stateMux);

  value = g_rowOn;

  portEXIT_CRITICAL(&stateMux);

  return value;
}

void setLastPollTime(unsigned long timeValue) {

  portENTER_CRITICAL(&stateMux);

  g_lastPollOkMs = timeValue;

  portEXIT_CRITICAL(&stateMux);
}

unsigned long getLastPollTime() {

  unsigned long value;

  portENTER_CRITICAL(&stateMux);

  value = g_lastPollOkMs;

  portEXIT_CRITICAL(&stateMux);

  return value;
}

// ============================================================
// 12. LCD I2C SCANNER
// ============================================================

byte scanI2C() {

  byte first = 0;

  for (byte addr = 1; addr < 127; addr++) {

    Wire.beginTransmission(addr);

    if (Wire.endTransmission() == 0) {

      Serial.printf(
        "Found I2C device at 0x%02X\n",
        addr
      );

      if (first == 0) {
        first = addr;
      }
    }
  }

  return first;
}

// ============================================================
// 13. LCD FIELD WRITER
// ============================================================

void printLcdField(
  int col,
  int row,
  const String &value,
  int width
) {

  if (lcd == nullptr) return;

  String padded = value;

  while ((int)padded.length() < width) {
    padded += " ";
  }

  lcd->setCursor(col, row);

  lcd->print(padded.substring(0, width));
}

// ============================================================
// 14. LCD ROW 1
// ============================================================

void lcdDrawRow1() {

  if (lcd == nullptr) return;

  SensorSnapshot s = getSensorSnapshot();

  String line;

  if (s.distance < 0) {

    line = "D:---";

  } else {

    line = "D:" + String(s.distance, 0) + "cm";
  }

  line += " S:" + String(s.soilRaw);

  char linkStatus =
    !getWifiStatus()
      ? '-'
      : (getPostStatus() ? 'W' : 'w');

  printLcdField(0, 0, line, 15);

  printLcdField(15, 0, String(linkStatus), 1);
}

// ============================================================
// 15. LCD ROW 2
// ============================================================

void lcdDrawRow2() {

  if (lcd == nullptr) return;

  bool currentPump = getPumpStateShared();

  String pumpText =
    currentPump ? "PUMP:ON " : "PUMP:OFF";

  String remoteText =
    remoteOn ? "REMOTE" : "";

  printLcdField(0, 1, pumpText, 9);

  printLcdField(9, 1, remoteText, 7);
}

// ============================================================
// 16. COMPLETE LCD REDRAW
// ============================================================

void lcdRedrawAll() {

  if (lcd == nullptr) return;

  lcdDrawRow1();

  lcdDrawRow2();
}

// ============================================================
// 17. LCD CONTROLLER RECOVERY
// ============================================================

void lcdRecover() {

  if (lcd == nullptr) return;

  Serial.println(
    "LCD: Starting controller recovery..."
  );

  lcd->init();

  lcd->backlight();

  lcd->clear();

  lcdRedrawAll();

  Serial.println(
    "LCD: Recovery completed."
  );
}

// ============================================================
// 18. LCD RECOVERY SCHEDULER
// ============================================================

void lcdRecoveryTask(unsigned long now) {

  if (lcd == nullptr) return;

  bool currentPump = getPumpStateShared();

  if (currentPump != previousPumpState) {

    previousPumpState = currentPump;

    if (currentPump) {

      lcdOffRecoveryPending = false;

      lcdNextRecoveryAt =
        now + LCD_ON_RECOVERY_MS;

      Serial.println(
        "LCD: Pump ON - recovery every 6 seconds."
      );

    } else {

      lcdOffRecoveryPending = true;

      lcdNextRecoveryAt =
        now + LCD_OFF_RECOVERY_MS;

      Serial.println(
        "LCD: Pump OFF - one recovery scheduled."
      );
    }
  }

  if (currentPump) {

    if (
      (long)(now - lcdNextRecoveryAt) >= 0
    ) {

      lcdRecover();

      lcdNextRecoveryAt =
        now + LCD_ON_RECOVERY_MS;
    }

    return;
  }

  if (
    lcdOffRecoveryPending &&
    (long)(now - lcdNextRecoveryAt) >= 0
  ) {

    lcdRecover();

    lcdOffRecoveryPending = false;
  }
}

// ============================================================
// 19. BUZZER
// ============================================================

void beep(unsigned long duration) {

  tone(BUZZER_PIN, 2000);

  buzzerOn = true;

  buzzerOffAt = millis() + duration;
}

void buzzerTask(unsigned long now) {

  if (
    buzzerOn &&
    (long)(now - buzzerOffAt) >= 0
  ) {

    noTone(BUZZER_PIN);

    buzzerOn = false;
  }
}

// ============================================================
// 20. RELAY CONTROL
// ============================================================

void setPump(bool on) {

  bool relayLevel =
    RELAY_ACTIVE_LOW ? !on : on;

  digitalWrite(
    RELAY_PIN,
    relayLevel ? HIGH : LOW
  );

  setPumpStateShared(on);
}

// ============================================================
// 21. WEBSITE COMMAND PROCESSING
// ============================================================

void remoteTask(unsigned long now) {

  unsigned long lastPoll =
    getLastPollTime();

  bool linkOk =
    (lastPoll != 0) &&
    ((now - lastPoll) < LINK_TIMEOUT_MS);

  if (!linkOk) {

    remoteOn = false;

    remoteArmed = false;

    return;
  }

  bool websiteCommand =
    getWebsiteCommand();

  if (!websiteCommand) {

    remoteArmed = true;

    remoteOn = false;

  } else if (remoteArmed) {

    remoteOn = true;
  }
}

// ============================================================
// 22. PUMP SAFETY TASK
// ============================================================

void pumpTask(unsigned long now) {

  bool currentPump = getPumpStateShared();

  bool want = remoteOn;

  if (currentPump) {

    if (!want) {

      setPump(false);

      lockUntil =
        now + PUMP_COOLDOWN_MS;

      Serial.println("Pump OFF");

    } else if (
      now - pumpStartMs >= REMOTE_MAX_RUN_MS
    ) {

      setPump(false);

      lockUntil =
        now + PUMP_COOLDOWN_MS;

      remoteOn = false;

      remoteArmed = false;

      beep(600);

      Serial.println(
        "Pump OFF: maximum runtime reached."
      );
    }

  } else if (
    want &&
    (long)(now - lockUntil) >= 0
  ) {

    setPump(true);

    pumpStartMs = now;

    beep(150);

    Serial.println(
      "Pump ON: website command."
    );
  }
}

// ============================================================
// 23. JSN-SR04T DISTANCE SENSOR
// ============================================================

float readDistanceCm() {

  digitalWrite(JSN_TRIG, LOW);

  delayMicroseconds(5);

  digitalWrite(JSN_TRIG, HIGH);

  delayMicroseconds(25);

  digitalWrite(JSN_TRIG, LOW);

  long us = pulseIn(
    JSN_ECHO,
    HIGH,
    35000
  );

  if (us == 0) {
    return -1;
  }

  return us * 0.0343f / 2.0f;
}

// ============================================================
// 24. SOIL SENSOR
// ============================================================

int readSoilRaw() {

  long sum = 0;

  for (int i = 0; i < 16; i++) {

    sum += analogRead(SOIL_PIN);

    delayMicroseconds(200);
  }

  return (int)(sum / 16);
}

// ============================================================
// 25. SENSOR PERCENTAGE CONVERSION
// ============================================================

int soilToPct(int raw) {

  float p =
    (float)(SOIL_DRY_RAW - raw) *
    100.0f /
    (float)(SOIL_DRY_RAW - SOIL_WET_RAW);

  return (int)constrain(p, 0.0f, 100.0f);
}

int distToWaterPct(float d) {

  float p =
    (TANK_EMPTY_CM - d) *
    100.0f /
    (TANK_EMPTY_CM - TANK_FULL_CM);

  return (int)constrain(p, 0.0f, 100.0f);
}

// ============================================================
// 26. JSON WHITESPACE HANDLER
// ============================================================

int skipJsonSpaces(const String &s, int index) {

  while (
    index < (int)s.length() &&
    (
      s[index] == ' ' ||
      s[index] == '\n' ||
      s[index] == '\r' ||
      s[index] == '\t'
    )
  ) {

    index++;
  }

  return index;
}

// ============================================================
// 27. JSON STRING READER
// ============================================================

bool jsonStr(
  const String &s,
  const char *key,
  String &out
) {

  String k =
    String("\"") + key + "\"";

  int i = s.indexOf(k);

  if (i < 0) return false;

  i += k.length();

  i = s.indexOf(':', i);

  if (i < 0) return false;

  i++;

  i = skipJsonSpaces(s, i);

  if (i >= (int)s.length()) return false;

  if (s[i] != '"') return false;

  i++;

  int j = s.indexOf('"', i);

  if (j < 0) return false;

  out = s.substring(i, j);

  return true;
}

// ============================================================
// 28. JSON BOOLEAN READER
// ============================================================

bool jsonBool(
  const String &s,
  const char *key,
  bool &out
) {

  String k =
    String("\"") + key + "\"";

  int i = s.indexOf(k);

  if (i < 0) return false;

  i += k.length();

  i = s.indexOf(':', i);

  if (i < 0) return false;

  i++;

  i = skipJsonSpaces(s, i);

  if (s.startsWith("true", i)) {

    out = true;

    return true;
  }

  if (s.startsWith("false", i)) {

    out = false;

    return true;
  }

  return false;
}

// ============================================================
// 29. SUPABASE HTTP HEADERS
// ============================================================

void addHeaders(HTTPClient &http) {

  http.addHeader("apikey", SB_KEY);

  http.addHeader(
    "Authorization",
    String("Bearer ") + SB_KEY
  );
}

// ============================================================
// 30. HTTP CLIENT CONFIGURATION
// ============================================================

void configureHttp(
  HTTPClient &http,
  WiFiClientSecure &client
) {

  // Prototype configuration.
  // setInsecure() disables certificate verification.
  // For production, use certificate validation.

  client.setInsecure();

  http.setConnectTimeout(
    HTTP_CONNECT_TIMEOUT_MS
  );

  http.setTimeout(
    HTTP_RESPONSE_TIMEOUT_MS
  );

  http.setReuse(false);
}

// ============================================================
// 31. UPLOAD SENSOR READINGS
// ============================================================

void postReading() {

  SensorSnapshot s = getSensorSnapshot();

  if (!s.haveReading) return;

  if (WiFi.status() != WL_CONNECTED) return;

  char dist[16];

  char water[16];

  char body[256];

  if (s.distance > 0) {

    snprintf(
      dist,
      sizeof(dist),
      "%.1f",
      s.distance
    );

  } else {

    strcpy(dist, "null");
  }

  if (s.waterPct >= 0) {

    snprintf(
      water,
      sizeof(water),
      "%d",
      s.waterPct
    );

  } else {

    strcpy(water, "null");
  }

  snprintf(
    body,
    sizeof(body),

    "{\"" COL_SOIL_RAW "\":%d,"
    "\"" COL_SOIL_PCT "\":%d,"
    "\"" COL_DIST "\":%s,"
    "\"" COL_WATER_PCT "\":%s,"
    "\"" COL_PUMP_ON "\":%s}",

    s.soilRaw,
    s.soilPct,
    dist,
    water,
    s.pumpState ? "true" : "false"
  );

  WiFiClientSecure client;

  HTTPClient http;

  configureHttp(http, client);

  String url =
    String(SB_URL) +
    "/rest/v1/" READINGS_TABLE;

  if (!http.begin(client, url)) {

    Serial.println(
      "POST readings: HTTP begin failed."
    );

    return;
  }

  addHeaders(http);

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  http.addHeader(
    "Prefer",
    "return=minimal"
  );

  int code = http.POST(
    (uint8_t *)body,
    strlen(body)
  );

  if (code >= 200 && code < 300) {

    setNetworkStatus(true, true);

    Serial.printf(
      "POST readings OK (%d)\n",
      code
    );

  } else {

    setNetworkStatus(true, false);

    String response =
      (code > 0)
      ? http.getString()
      : String("(no response)");

    Serial.printf(
      "POST readings FAILED (%d): %s\n",
      code,
      response.c_str()
    );
  }

  http.end();
}

// ============================================================
// 32. POLL WEBSITE PUMP COMMAND
// ============================================================

void pollPump() {

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  WiFiClientSecure client;

  HTTPClient http;

  configureHttp(http, client);

  String url =
    String(SB_URL) +
    "/rest/v1/" CONTROL_TABLE
    "?" CONTROL_FILTER
    "&select=" CTRL_MODE "," CTRL_STATE;

  if (!http.begin(client, url)) {

    Serial.println(
      "GET pump_control: HTTP begin failed."
    );

    return;
  }

  addHeaders(http);

  int code = http.GET();

  if (code != 200) {

    String response =
      (code > 0)
      ? http.getString()
      : String("(no response)");

    Serial.printf(
      "GET pump_control FAILED (%d): %s\n",
      code,
      response.c_str()
    );

    http.end();

    return;
  }

  String response = http.getString();

  http.end();

  if (response.indexOf('{') < 0) {

    Serial.println(
      "pump_control: no matching row."
    );

    setWebsiteCommand(false);

    setLastPollTime(millis());

    return;
  }

  String modeStr;

  bool state = false;

  bool modeFound =
    jsonStr(response, CTRL_MODE, modeStr);

  bool stateFound =
    jsonBool(response, CTRL_STATE, state);

  if (!modeFound || !stateFound) {

    Serial.println(
      "ERROR: Could not read pump command."
    );

    return;
  }

  bool rowOn =
    (modeStr == CTRL_MANUAL_VAL) && state;

  bool previousCommand =
    getWebsiteCommand();

  if (rowOn != previousCommand) {

    Serial.printf(
      "Website command: mode=%s state=%s\n",
      modeStr.c_str(),
      state ? "true" : "false"
    );
  }

  setWebsiteCommand(rowOn);

  setLastPollTime(millis());
}

// ============================================================
// 33. WIFI DIAGNOSTICS
// ============================================================

const char *wifiReasonText(int reason) {

  switch (reason) {

    case 2:
      return "auth expired";

    case 15:
      return "handshake timeout";

    case 201:
      return "network not found";

    case 202:
      return "authentication failed";

    case 203:
      return "association failed";

    case 204:
      return "handshake timeout";

    case 205:
      return "connection failed";

    default:
      return "other";
  }
}

void wifiScan() {

  Serial.println(
    "WiFi: scanning for networks..."
  );

  int n = WiFi.scanNetworks();

  bool found = false;

  for (int i = 0; i < n; i++) {

    String name = WiFi.SSID(i);

    bool match = (name == WIFI_SSID);

    if (match) found = true;

    Serial.printf(
      "%s%s RSSI %d dBm channel %d\n",

      match ? "-> " : "   ",

      name.c_str(),

      (int)WiFi.RSSI(i),

      (int)WiFi.channel(i)
    );
  }

  if (n <= 0) {

    Serial.println(
      "WiFi: no networks detected."
    );

  } else if (!found) {

    Serial.printf(
      "WiFi: %s not found. Check the name and 2.4 GHz connection.\n",
      WIFI_SSID
    );

  } else {

    Serial.println(
      "WiFi: target network found."
    );
  }

  WiFi.scanDelete();
}

// ============================================================
// 34. WIFI MANAGER TASK
// ============================================================

void wifiManagerTask(void *) {

  unsigned long lastWifiTry = 0;

  bool wasConnected = false;

  WiFi.onEvent(
    [](arduino_event_id_t event,
       arduino_event_info_t info) {

      (void)event;

      int reason =
        info.wifi_sta_disconnected.reason;

      Serial.printf(
        "WiFi disconnected: reason %d - %s\n",
        reason,
        wifiReasonText(reason)
      );

    },
    ARDUINO_EVENT_WIFI_STA_DISCONNECTED
  );

  WiFi.persistent(false);

  WiFi.mode(WIFI_STA);

  WiFi.setSleep(false);

  WiFi.setAutoReconnect(true);

  WiFi.setHostname("GRIPYRAMID-ESP32");

  WiFi.disconnect();

  vTaskDelay(pdMS_TO_TICKS(200));

  wifiScan();

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASS
  );

  lastWifiTry = millis();

  Serial.printf(
    "WiFi connecting to %s...\n",
    WIFI_SSID
  );

  for (;;) {

    unsigned long now = millis();

    if (WiFi.status() != WL_CONNECTED) {

      setNetworkStatus(false, false);

      wasConnected = false;

      if (now - lastWifiTry >= WIFI_RETRY_MS) {

        lastWifiTry = now;

        Serial.println(
          "WiFi: retrying connection..."
        );

        WiFi.disconnect();

        WiFi.begin(
          WIFI_SSID,
          WIFI_PASS
        );
      }

      vTaskDelay(pdMS_TO_TICKS(250));

      continue;
    }

    if (!wasConnected) {

      wasConnected = true;

      setNetworkStatus(true, false);

      Serial.printf(
        "WiFi connected. IP: %s\n",
        WiFi.localIP().toString().c_str()
      );
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ============================================================
// 35. INDEPENDENT PUMP POLLING TASK
// ============================================================

void pumpPollingTask(void *) {

  unsigned long lastPoll = 0;

  for (;;) {

    unsigned long now = millis();

    if (WiFi.status() == WL_CONNECTED) {

      if (
        now - lastPoll >= POLL_INTERVAL_MS
      ) {

        lastPoll = now;

        pollPump();
      }

    } else {

      lastPoll = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ============================================================
// 36. INDEPENDENT SENSOR UPLOAD TASK
// ============================================================

void sensorUploadTask(void *) {

  unsigned long lastPost = 0;

  for (;;) {

    unsigned long now = millis();

    if (
      WiFi.status() == WL_CONNECTED &&
      now - lastPost >= POST_INTERVAL_MS
    ) {

      lastPost = now;

      postReading();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================================================
// 37. SETUP
// ============================================================

void setup() {

  // Configure relay output OFF as early as possible.

  digitalWrite(
    RELAY_PIN,
    RELAY_ACTIVE_LOW ? HIGH : LOW
  );

  pinMode(
    RELAY_PIN,
    OUTPUT
  );

  setPumpStateShared(false);

  Serial.begin(115200);

  delay(1000);

  Serial.println();

  Serial.println(
    "======================================"
  );

  Serial.println(
    "       GRIPYRAMID TEST 7 - V7"
  );

  Serial.println(
    "======================================"
  );

  Serial.println(
    "Pump starts OFF. Website-only control."
  );

  pinMode(
    BUZZER_PIN,
    OUTPUT
  );

  noTone(BUZZER_PIN);

  pinMode(
    JSN_TRIG,
    OUTPUT
  );

  pinMode(
    JSN_ECHO,
    INPUT
  );

  digitalWrite(
    JSN_TRIG,
    LOW
  );

  pinMode(
    SOIL_PIN,
    INPUT
  );

  analogSetPinAttenuation(
    SOIL_PIN,
    ADC_11db
  );

  // LCD initialization

  Wire.begin(
    LCD_SDA,
    LCD_SCL
  );

  byte address = scanI2C();

  if (address == 0) {

    Serial.println(
      "WARNING: LCD not detected."
    );

  } else {

    lcd = new LiquidCrystal_I2C(
      address,
      16,
      2
    );

    lcd->init();

    lcd->backlight();

    lcd->clear();

    printLcdField(
      0,
      0,
      "GRIPYRAMID",
      16
    );

    printLcdField(
      0,
      1,
      "Starting V7...",
      16
    );
  }

  // Startup beep

  tone(BUZZER_PIN, 1500);

  delay(200);

  noTone(BUZZER_PIN);

  delay(1300);

  if (lcd != nullptr) {
    lcd->clear();
  }

  // Start Wi-Fi manager.

  BaseType_t wifiResult =
    xTaskCreatePinnedToCore(
      wifiManagerTask,
      "wifiManager",
      8192,
      NULL,
      2,
      NULL,
      0
    );

  if (wifiResult != pdPASS) {

    Serial.println(
      "ERROR: WiFi manager task failed."
    );
  }

  // Start pump polling task.
  // Higher priority than the upload task.

  BaseType_t pollResult =
    xTaskCreatePinnedToCore(
      pumpPollingTask,
      "pumpPolling",
      8192,
      NULL,
      3,
      NULL,
      0
    );

  if (pollResult != pdPASS) {

    Serial.println(
      "ERROR: Pump polling task failed."
    );
  }

  // Start sensor upload task.

  BaseType_t uploadResult =
    xTaskCreatePinnedToCore(
      sensorUploadTask,
      "sensorUpload",
      8192,
      NULL,
      1,
      NULL,
      0
    );

  if (uploadResult != pdPASS) {

    Serial.println(
      "ERROR: Sensor upload task failed."
    );
  }

  Serial.println(
    "V7 tasks initialized."
  );
}

// ============================================================
// 38. MAIN LOOP
// ============================================================

void loop() {

  static unsigned long lastRead = 0;

  unsigned long now = millis();

  // Process website command and pump safety.

  remoteTask(now);

  pumpTask(now);

  // Buzzer

  buzzerTask(now);

  // LCD recovery

  lcdRecoveryTask(now);

  // Sensor reading

  if (
    now - lastRead >= SENSOR_INTERVAL_MS
  ) {

    lastRead = now;

    int soil = readSoilRaw();

    float distance = readDistanceCm();

    int soilPct = soilToPct(soil);

    int waterPct =
      (distance > 0)
      ? distToWaterPct(distance)
      : -1;

    updateSensorSnapshot(
      soil,
      soilPct,
      distance,
      waterPct
    );

    // Serial monitor

    if (distance < 0) {

      Serial.printf(
        "Soil raw=%d (%d%%) | JSN: no echo | Pump=%s | WiFi=%s\n",

        soil,

        soilPct,

        getPumpStateShared() ? "ON" : "OFF",

        getWifiStatus() ? "OK" : "DOWN"
      );

    } else {

      Serial.printf(
        "Soil raw=%d (%d%%) | JSN: %.1f cm (water %d%%) | Pump=%s | WiFi=%s\n",

        soil,

        soilPct,

        distance,

        waterPct,

        getPumpStateShared() ? "ON" : "OFF",

        getWifiStatus() ? "OK" : "DOWN"
      );
    }

    lcdDrawRow1();
  }

  // Keep pump status visible.

  lcdDrawRow2();

  delay(5);
}

// ============================================================
// END OF GREEN MILE / GRIPYRAMID V7
// ============================================================
