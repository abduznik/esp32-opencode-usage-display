#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <vector>

static const uint32_t POLL_INTERVAL_MS = 5UL * 60UL * 1000UL; // 5 minutes per provider
static const uint32_t DEFAULT_SLIDE_SECONDS = 8;
static const int MAX_PROVIDERS = 8;

// Hold this pin LOW at boot to wipe saved WiFi and re-run the captive-portal
// setup (e.g. wire a button between this pin and GND).
static const int PROVISION_BUTTON_PIN = 0; // BOOT button on most DevKitC boards

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
WiFiManager wm;
WebServer server(80);

// --- Provider model ---------------------------------------------------
// Every provider currently speaks the same "3-bucket percent" shape as
// opencode's /zen/go/v1/usage (rolling/weekly/monthly percentages). The
// `type` field exists so a future provider with a different response shape
// can be added without changing the storage format.

struct Quota {
  bool ok = false;
  int percent = -1;
};

struct UsageData {
  Quota rolling;
  Quota weekly;
  Quota monthly;
  bool valid = false;
};

struct Provider {
  String id;       // stable short id, e.g. "p1"
  String name;      // display name, e.g. "opencode"
  String type;      // "opencode_percent3" (only type for now)
  String apiKey;
  bool enabled = true;

  UsageData lastGood;
  unsigned long lastPollAt = 0;
  bool everPolled = false;
};

std::vector<Provider> providers;
uint32_t slideSeconds = DEFAULT_SLIDE_SECONDS;
size_t slideIndex = 0;
unsigned long lastSlideAt = 0;

// --- Persistence --------------------------------------------------------

String nextProviderId() {
  prefs.begin("cfg", false);
  uint32_t n = prefs.getUInt("nextid", 1);
  prefs.putUInt("nextid", n + 1);
  prefs.end();
  char buf[16];
  snprintf(buf, sizeof(buf), "p%lu", (unsigned long)n);
  return String(buf);
}

void saveProviders() {
  JsonDocument doc;
  JsonArray arr = doc["providers"].to<JsonArray>();
  for (auto &p : providers) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = p.id;
    o["name"] = p.name;
    o["type"] = p.type;
    o["apiKey"] = p.apiKey;
    o["enabled"] = p.enabled;
  }
  doc["slideSeconds"] = slideSeconds;

  String out;
  serializeJson(doc, out);

  prefs.begin("cfg", false);
  prefs.putString("providers_json", out);
  prefs.end();
}

void loadProviders() {
  providers.clear();

  prefs.begin("cfg", true);
  String raw = prefs.getString("providers_json", "");
  prefs.end();

  if (raw.length() == 0) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, raw);
  if (err) {
    Serial.printf("Failed to parse saved providers: %s\n", err.c_str());
    return;
  }

  slideSeconds = doc["slideSeconds"] | DEFAULT_SLIDE_SECONDS;

  JsonArray arr = doc["providers"];
  for (JsonObject o : arr) {
    Provider p;
    p.id = o["id"] | "";
    p.name = o["name"] | "";
    p.type = o["type"] | "opencode_percent3";
    p.apiKey = o["apiKey"] | "";
    p.enabled = o["enabled"] | true;
    if (p.id.length() > 0) {
      providers.push_back(p);
    }
  }
}

// --- Display helpers ------------------------------------------------------

void drawMessage(const char* line1, const char* line2 = nullptr) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(line1, tft.width() / 2, tft.height() / 2 - (line2 ? 10 : 0), 2);
  if (line2) {
    tft.drawString(line2, tft.width() / 2, tft.height() / 2 + 14, 2);
  }
}

// --- WiFi provisioning ------------------------------------------------

void runProvisioning(bool forcePortal) {
  wm.setConfigPortalTimeout(180);

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
}

// --- Fetching usage -----------------------------------------------------

bool fetchOpencodeUsage(const String &apiKey, UsageData &out) {
  if (WiFi.status() != WL_CONNECTED || apiKey.length() == 0) return false;

  WiFiClientSecure client;
  client.setInsecure(); // TODO: pin the real cert for production use

  HTTPClient https;
  if (!https.begin(client, "https://opencode.ai/zen/go/v1/usage")) {
    Serial.println("HTTPClient begin() failed");
    return false;
  }

  https.addHeader("Authorization", String("Bearer ") + apiKey);
  https.addHeader("Accept", "application/json");
  https.setTimeout(10000);

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("opencode usage GET failed, HTTP code: %d\n", httpCode);
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
    q.percent = obj["percent"] | -1;
    q.ok = (q.percent >= 0);
  };

  readQuota("rolling", out.rolling);
  readQuota("weekly", out.weekly);
  readQuota("monthly", out.monthly);

  out.valid = out.rolling.ok || out.weekly.ok || out.monthly.ok;
  return out.valid;
}

bool fetchUsageForProvider(Provider &p) {
  if (p.type == "opencode_percent3") {
    return fetchOpencodeUsage(p.apiKey, p.lastGood);
  }
  Serial.printf("Unknown provider type: %s\n", p.type.c_str());
  return false;
}

// --- Screen rendering -------------------------------------------------

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

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  tft.setTextFont(4);
  tft.drawString(pctStr, barX + barW, barY + barH + 6, 4);
  tft.setTextFont(1);
}

void drawNoProviders() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("No providers configured", tft.width() / 2, tft.height() / 2 - 20, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Open in browser:", tft.width() / 2, tft.height() / 2 + 10, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(WiFi.localIP().toString(), tft.width() / 2, tft.height() / 2 + 32, 4);
}

void drawProviderScreen(const Provider &p, bool wifiOk) {
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(p.name, tft.width() / 2, 8, 2);

  if (!wifiOk) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("WiFi disconnected", tft.width() / 2, 30, 2);
  }

  int top = 40;
  int bottom = p.lastGood.valid ? tft.height() : tft.height() - 20;
  int rowSpacing = (bottom - top) / 3;

  int y = top;
  drawRow(y, "Rolling (5hr)", p.lastGood.rolling);
  y += rowSpacing;
  drawRow(y, "Weekly", p.lastGood.weekly);
  y += rowSpacing;
  drawRow(y, "Monthly", p.lastGood.monthly);

  if (!p.lastGood.valid) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setTextDatum(BC_DATUM);
    tft.drawString("Waiting for data...", tft.width() / 2, tft.height() - 8, 2);
  }

  // Small footer showing the config page IP, so it's always discoverable.
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextDatum(BL_DATUM);
  tft.drawString(WiFi.localIP().toString(), 4, tft.height() - 2, 1);
}

std::vector<Provider*> enabledProviders() {
  std::vector<Provider*> out;
  for (auto &p : providers) {
    if (p.enabled) out.push_back(&p);
  }
  return out;
}

void renderCurrentSlide(bool wifiOk) {
  auto shown = enabledProviders();
  if (shown.empty()) {
    drawNoProviders();
    return;
  }
  if (slideIndex >= shown.size()) slideIndex = 0;
  drawProviderScreen(*shown[slideIndex], wifiOk);
}

// --- Web config server --------------------------------------------------

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>opencode usage display - config</title>
<style>
  body { font-family: -apple-system, Segoe UI, Roboto, sans-serif; background:#111; color:#eee; margin:0; padding:16px; }
  h1 { font-size:1.2rem; }
  .card { background:#1c1c1c; border:1px solid #333; border-radius:8px; padding:12px; margin-bottom:12px; }
  label { display:block; font-size:0.8rem; color:#aaa; margin-top:8px; }
  input[type=text], select { width:100%; box-sizing:border-box; padding:8px; margin-top:4px; background:#0d0d0d; border:1px solid #333; color:#eee; border-radius:4px; }
  button { padding:8px 14px; border:none; border-radius:4px; background:#3a7; color:#fff; cursor:pointer; margin-top:10px; }
  button.danger { background:#a33; }
  .row { display:flex; justify-content:space-between; align-items:center; }
  .muted { color:#888; font-size:0.8rem; }
  .toggle { display:flex; align-items:center; gap:8px; margin-top:8px; }
</style>
</head>
<body>
<h1>opencode usage display</h1>
<p class="muted">Slideshow interval (seconds):
  <input type="text" id="slideSeconds" style="width:60px; display:inline-block;">
  <button onclick="saveSlide()">Save</button>
</p>

<div id="list"></div>

<div class="card">
  <h2 style="font-size:1rem;">Add provider</h2>
  <label>Name (shown on screen)</label>
  <input type="text" id="newName" placeholder="opencode">
  <label>Type</label>
  <select id="newType">
    <option value="opencode_percent3">opencode Zen (rolling/weekly/monthly %)</option>
  </select>
  <label>API key</label>
  <input type="text" id="newKey" placeholder="sk-...">
  <button onclick="addProvider()">Add</button>
</div>

<script>
async function load() {
  const res = await fetch('/api/providers');
  const data = await res.json();
  document.getElementById('slideSeconds').value = data.slideSeconds;
  const list = document.getElementById('list');
  list.innerHTML = '';
  data.providers.forEach(p => {
    const div = document.createElement('div');
    div.className = 'card';
    div.innerHTML = `
      <div class="row">
        <strong>${p.name}</strong>
        <span class="muted">${p.type}</span>
      </div>
      <label>Name</label>
      <input type="text" value="${p.name}" id="name-${p.id}">
      <label>API key</label>
      <input type="text" value="${p.apiKey}" id="key-${p.id}">
      <div class="toggle">
        <input type="checkbox" id="enabled-${p.id}" ${p.enabled ? 'checked' : ''}>
        <label style="margin:0;">Show in slideshow</label>
      </div>
      <button onclick="saveProvider('${p.id}')">Save</button>
      <button class="danger" onclick="removeProvider('${p.id}')">Remove</button>
    `;
    list.appendChild(div);
  });
}

async function addProvider() {
  const name = document.getElementById('newName').value || 'provider';
  const type = document.getElementById('newType').value;
  const apiKey = document.getElementById('newKey').value;
  await fetch('/api/providers', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({name, type, apiKey, enabled:true})
  });
  document.getElementById('newName').value = '';
  document.getElementById('newKey').value = '';
  load();
}

async function saveProvider(id) {
  const name = document.getElementById(`name-${id}`).value;
  const apiKey = document.getElementById(`key-${id}`).value;
  const enabled = document.getElementById(`enabled-${id}`).checked;
  await fetch(`/api/providers/${id}`, {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({name, apiKey, enabled})
  });
  load();
}

async function removeProvider(id) {
  if (!confirm('Remove this provider?')) return;
  await fetch(`/api/providers/${id}`, {method:'DELETE'});
  load();
}

async function saveSlide() {
  const seconds = parseInt(document.getElementById('slideSeconds').value, 10) || 8;
  await fetch('/api/slide', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({slideSeconds: seconds})
  });
}

load();
</script>
</body>
</html>
)HTML";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleGetProviders() {
  JsonDocument doc;
  JsonArray arr = doc["providers"].to<JsonArray>();
  for (auto &p : providers) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = p.id;
    o["name"] = p.name;
    o["type"] = p.type;
    o["apiKey"] = p.apiKey;
    o["enabled"] = p.enabled;
  }
  doc["slideSeconds"] = slideSeconds;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAddProvider() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }
  if ((int)providers.size() >= MAX_PROVIDERS) {
    server.send(400, "application/json", "{\"error\":\"max providers reached\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  Provider p;
  p.id = nextProviderId();
  p.name = (const char*)(doc["name"] | "provider");
  p.type = (const char*)(doc["type"] | "opencode_percent3");
  p.apiKey = (const char*)(doc["apiKey"] | "");
  p.enabled = doc["enabled"] | true;

  providers.push_back(p);
  saveProviders();

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleUpdateProvider(const String &id) {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  for (auto &p : providers) {
    if (p.id == id) {
      if (doc["name"].is<const char*>()) p.name = (const char*)doc["name"];
      if (doc["apiKey"].is<const char*>()) p.apiKey = (const char*)doc["apiKey"];
      if (doc["enabled"].is<bool>()) p.enabled = doc["enabled"];
      saveProviders();
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

void handleDeleteProvider(const String &id) {
  for (size_t i = 0; i < providers.size(); i++) {
    if (providers[i].id == id) {
      providers.erase(providers.begin() + i);
      saveProviders();
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

void handleSlideConfig() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }
  uint32_t seconds = doc["slideSeconds"] | DEFAULT_SLIDE_SECONDS;
  if (seconds < 2) seconds = 2;
  if (seconds > 3600) seconds = 3600;
  slideSeconds = seconds;
  saveProviders();
  server.send(200, "application/json", "{\"ok\":true}");
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/providers", HTTP_GET, handleGetProviders);
  server.on("/api/providers", HTTP_POST, handleAddProvider);
  server.on("/api/slide", HTTP_POST, handleSlideConfig);

  // WebServer library doesn't support path params directly; match by prefix.
  server.onNotFound([]() {
    String uri = server.uri();
    if (uri.startsWith("/api/providers/")) {
      String id = uri.substring(strlen("/api/providers/"));
      if (server.method() == HTTP_POST) {
        handleUpdateProvider(id);
        return;
      }
      if (server.method() == HTTP_DELETE) {
        handleDeleteProvider(id);
        return;
      }
    }
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
}

// --- Setup / loop -------------------------------------------------------

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

  loadProviders();

  bool forcePortal = (digitalRead(PROVISION_BUTTON_PIN) == LOW);
  if (forcePortal) {
    Serial.println("Provisioning button held — forcing config portal");
  }

  runProvisioning(forcePortal);

  setupWebServer();

  drawMessage("Connected!", WiFi.localIP().toString().c_str());
  delay(1500);

  for (auto &p : providers) {
    if (p.enabled) {
      fetchUsageForProvider(p);
      p.everPolled = true;
      p.lastPollAt = millis();
    }
  }

  lastSlideAt = millis();
  renderCurrentSlide(WiFi.status() == WL_CONNECTED);
}

void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    renderCurrentSlide(false);
    WiFi.reconnect();
    delay(2000);
    return;
  }

  unsigned long now = millis();

  // Poll each enabled provider independently on its own interval.
  auto shown = enabledProviders();
  bool currentSlideUpdated = false;
  for (auto &p : providers) {
    if (!p.enabled) continue;
    if (!p.everPolled || now - p.lastPollAt >= POLL_INTERVAL_MS) {
      bool ok = fetchUsageForProvider(p);
      if (ok) {
        Serial.printf("[%s] rolling=%d%% weekly=%d%% monthly=%d%%\n",
                      p.name.c_str(), p.lastGood.rolling.percent,
                      p.lastGood.weekly.percent, p.lastGood.monthly.percent);
      } else {
        Serial.printf("[%s] usage fetch failed\n", p.name.c_str());
      }
      p.everPolled = true;
      p.lastPollAt = now;

      if (!shown.empty() && slideIndex < shown.size() && shown[slideIndex] == &p) {
        currentSlideUpdated = true;
      }
    }
  }

  // Advance the slideshow, or just refresh the current slide if its data changed.
  if (!shown.empty() && now - lastSlideAt >= slideSeconds * 1000UL) {
    slideIndex = (slideIndex + 1) % shown.size();
    lastSlideAt = now;
    renderCurrentSlide(true);
  } else if (shown.empty()) {
    renderCurrentSlide(true);
  } else if (currentSlideUpdated) {
    renderCurrentSlide(true);
  }

  delay(200);
}
