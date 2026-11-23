#include "settings.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <algorithm>

static Preferences s_prefs;
static SemaphoreHandle_t s_mux;
static const size_t MAX_WIFI = 8;

namespace {
struct Guard {
  Guard() { xSemaphoreTake(s_mux, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(s_mux); }
};

std::vector<WifiCred> loadWifi() {
  std::vector<WifiCred> v;
  JsonDocument doc;
  if (deserializeJson(doc, s_prefs.getString("wifi", "[]"))) return v;
  for (JsonObject o : doc.as<JsonArray>()) v.push_back({ o["s"] | "", o["p"] | "" });
  return v;
}

void saveWifi(const std::vector<WifiCred>& v) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& w : v) {
    JsonObject o = arr.add<JsonObject>();
    o["s"] = w.ssid;
    o["p"] = w.pass;
  }
  String s;
  serializeJson(doc, s);
  s_prefs.putString("wifi", s);
}
}  // namespace

namespace Settings {
void begin() {
  s_mux = xSemaphoreCreateMutex();
  s_prefs.begin("cfg", false);
}

std::vector<WifiCred> wifiList() { Guard g; return loadWifi(); }

void addWifi(const String& ssid, const String& pass) {
  Guard g;
  auto v = loadWifi();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const WifiCred& w) { return w.ssid == ssid; }), v.end());
  v.insert(v.begin(), { ssid, pass });
  if (v.size() > MAX_WIFI) v.resize(MAX_WIFI);
  saveWifi(v);
}

bool removeWifi(const String& ssid) {
  Guard g;
  auto v = loadWifi();
  size_t before = v.size();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const WifiCred& w) { return w.ssid == ssid; }), v.end());
  saveWifi(v);
  return v.size() != before;
}

void clearWifi() { Guard g; s_prefs.remove("wifi"); }

String spotifyClientId() { Guard g; return s_prefs.getString("sp_cid", ""); }
void setSpotifyClientId(const String& id) { Guard g; s_prefs.putString("sp_cid", id); }
String spotifyRefreshToken() { Guard g; return s_prefs.getString("sp_rt", ""); }
void setSpotifyRefreshToken(const String& t) { Guard g; s_prefs.putString("sp_rt", t); }
String spotifyUser() { Guard g; return s_prefs.getString("sp_user", ""); }
void setSpotifyUser(const String& n) { Guard g; s_prefs.putString("sp_user", n); }

void clearSpotifyLogin() {
  Guard g;
  s_prefs.remove("sp_rt");
  s_prefs.remove("sp_user");
}

void factoryReset() {
  Guard g;
  s_prefs.clear();
}
}  // namespace Settings
