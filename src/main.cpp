#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <WiFiManager.h>
#include <Preferences.h>

// opencode Zen usage endpoint — returns quota PERCENTAGES only
// (no token counts, no dollar amounts): rolling (5hr), weekly, monthly.
static const char* USAGE_URL = "https://opencode.ai/zen/go/v1/usage";

static const uint32_t POLL_INTERVAL_MS = 5UL * 60UL * 1000UL; // 5 minutes

// Hold this pin LOW at boot to wipe saved WiFi/token and re-run the
// captive-portal setup (e.g. wire a button between this pin and GND).
static const int PROVISION_BUTTON_PIN = 0; // BOOT button on most DevKitC boards

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
WiFiManager wm;

char apiTokenBuf[128] = "";
WiFiManagerParameter apiTokenParam("apitoken", "opencode API token", "", sizeof(apiTokenBuf) - 1);

struct Quota {
  bool ok = false;
  int percent = -1;
  String status;
};

struct UsageData {
  Quota rolling;
  Quota weekly;
  Quota monthly;
  bool valid = false;
};

UsageData lastGood;
unsigned long lastPollAt = 0;
bool firstDrawDone = false;
String apiToken;

void drawMessage(const char* line1, const char* line2 = nullptr) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(line1, tft.width() / 2, tft.height() / 2 - (line2 ? 10 : 0), 2);
  if (line2) {
    tft.drawString(line2, tft.width() / 2, tft.height() / 2 + 14, 2);
  }
}

void loadApiToken() {
  prefs.begin("opencode", true);
  apiToken = prefs.getString("token", "");
  prefs.end();
}

void saveApiToken(const String &token) {
  prefs.begin("opencode", false);
  prefs.putString("token", token);
  prefs.end();
}

void saveApiTokenCallback() {
  String token = apiTokenParam.getValue();
  token.trim();
  if (token.length() > 0) {
    saveApiToken(token);
    apiToken = token;
  }
}

void runProvisioning(bool forcePortal) {
  loadApiToken();
  apiTokenParam.setValue(apiToken.c_str(), sizeof(apiTokenBuf) - 1);

  wm.addParameter(&apiTokenParam);
  wm.setSaveParamsCallback(saveApiTokenCallback);
  wm.setConfigPortalTimeout(180);

  // Only shown once WiFiManager actually gives up on saved credentials and
  // opens the setup AP — not while it's still trying to reconnect, which
  // can take 10-15s on its own and previously looked identical to "needs
  // setup" even though it wasn't.
  wm.setAPCallback([](WiFiManager*) {
    drawMessage("Join WiFi: OpenCode-Display", "Then open 192.168.4.1");
  });

  if (forcePortal) {
    drawMessage("Join WiFi: OpenCode-Display", "Then open 192.168.4.1");
  } else {
    drawMessage("Connecting to WiFi...", "Please wait");
  }

  bool connected;
  if (forcePortal) {
    connected = wm.startConfigPortal("OpenCode-Display");
  } else {
    connected = wm.autoConnect("OpenCode-Display");
  }

  if (!connected) {
    drawMessage("Setup timed out", "Restarting...");
    delay(3000);
    ESP.restart();
  }

  loadApiToken();
}

bool fetchUsage(UsageData &out) {
  if (WiFi.status() != WL_CONNECTED || apiToken.length() == 0) return false;

  WiFiClientSecure client;
  client.setInsecure(); // TODO: pin the real cert for production use

  HTTPClient https;
  if (!https.begin(client, USAGE_URL)) {
    Serial.println("HTTPClient begin() failed");
    return false;
  }

  https.addHeader("Authorization", String("Bearer ") + apiToken);
  https.addHeader("Accept", "application/json");
  https.setTimeout(10000);

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("GET %s failed, HTTP code: %d\n", USAGE_URL, httpCode);
    https.end();
    return false;
  }

  String payload = https.getString();
  https.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("JSON parse failed: %s\n", err.c_str());
    return false;
  }

  JsonObject usage = doc["usage"];
  if (usage.isNull()) {
    Serial.println("Response missing 'usage' object");
    return false;
  }

  auto readQuota = [&](const char* key, Quota &q) {
    JsonObject obj = usage[key];
    if (obj.isNull()) {
      q.ok = false;
      return;
    }
    q.status = obj["status"] | "";
    q.percent = obj["percent"] | -1;
    q.ok = (q.percent >= 0);
  };

  readQuota("rolling", out.rolling);
  readQuota("weekly", out.weekly);
  readQuota("monthly", out.monthly);

  out.valid = out.rolling.ok || out.weekly.ok || out.monthly.ok;
  return out.valid;
}

// Darker fill colors keep white overlay text and dark row backgrounds
// readable — the old bright TFT_GREEN/TFT_YELLOW washed out foreground text.
uint16_t colorForPercent(int pct) {
  if (pct < 0) return TFT_DARKGREY;
  if (pct < 50) return 0x0500;  // dark green
  if (pct < 80) return 0x6B00;  // dark amber
  return 0x8000;                // dark red
}

void drawBar(int x, int y, int w, int h, int pct, uint16_t color) {
  tft.drawRect(x, y, w, h, TFT_WHITE);
  int innerW = w - 4;
  int fillW = (pct <= 0) ? 0 : (innerW * pct) / 100;
  tft.fillRect(x + 2, y + 2, innerW, h - 4, TFT_BLACK);
  if (fillW > 0) {
    tft.fillRect(x + 2, y + 2, fillW, h - 4, color);
  }
}

void drawRow(int y, const char* label, const Quota &q) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(label, 10, y, 2);

  int barX = 10;
  int barY = y + 16;
  int barW = tft.width() - 20;
  int barH = 16;

  uint16_t color = q.ok ? colorForPercent(q.percent) : TFT_DARKGREY;
  drawBar(barX, barY, barW, barH, q.ok ? q.percent : 0, color);

  char pctStr[16];
  if (q.ok) {
    snprintf(pctStr, sizeof(pctStr), "%d%%", q.percent);
  } else {
    snprintf(pctStr, sizeof(pctStr), "--");
  }

  // Large bold percentage, drawn below the bar (not overlaid) so it never
  // gets washed out against the fill color.
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  tft.setTextFont(4);
  tft.drawString(pctStr, barX + barW, barY + barH + 6, 4);
  tft.setTextFont(1);
}

void drawScreen(const UsageData &data, bool wifiOk) {
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("opencode usage", tft.width() / 2, 8, 2);

  if (!wifiOk) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("WiFi disconnected", tft.width() / 2, 30, 2);
  }

  int top = 40;
  int bottom = data.valid ? tft.height() : tft.height() - 20;
  int rowSpacing = (bottom - top) / 3;

  int y = top;
  drawRow(y, "Rolling (5hr)", data.rolling);
  y += rowSpacing;
  drawRow(y, "Weekly", data.weekly);
  y += rowSpacing;
  drawRow(y, "Monthly", data.monthly);

  if (!data.valid) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setTextDatum(BC_DATUM);
    tft.drawString("Waiting for data...", tft.width() / 2, tft.height() - 8, 2);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PROVISION_BUTTON_PIN, INPUT_PULLUP);

  tft.init();
#ifndef DISPLAY_ROTATION
#define DISPLAY_ROTATION 0
#endif
  tft.setRotation(DISPLAY_ROTATION);
  tft.fillScreen(TFT_BLACK);

  bool forcePortal = (digitalRead(PROVISION_BUTTON_PIN) == LOW);
  if (forcePortal) {
    Serial.println("Provisioning button held — forcing config portal");
  }

  runProvisioning(forcePortal);

  drawMessage("Connected!", WiFi.localIP().toString().c_str());
  delay(1000);

  if (fetchUsage(lastGood)) {
    Serial.println("Initial usage fetch OK");
  } else {
    Serial.println("Initial usage fetch failed, will retry in loop");
  }

  drawScreen(lastGood, WiFi.status() == WL_CONNECTED);
  firstDrawDone = true;
  lastPollAt = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    drawScreen(lastGood, false);
    WiFi.reconnect();
    delay(5000);
    return;
  }

  unsigned long now = millis();
  if (!firstDrawDone || now - lastPollAt >= POLL_INTERVAL_MS) {
    UsageData fresh;
    if (fetchUsage(fresh)) {
      lastGood = fresh;
      Serial.printf("Usage: rolling=%d%% weekly=%d%% monthly=%d%%\n",
                    lastGood.rolling.percent, lastGood.weekly.percent, lastGood.monthly.percent);
    } else {
      Serial.println("Usage fetch failed, keeping last known values");
    }
    drawScreen(lastGood, true);
    lastPollAt = now;
    firstDrawDone = true;
  }

  delay(1000);
}
