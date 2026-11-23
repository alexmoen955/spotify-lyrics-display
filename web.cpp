// Web interface: setup hotspot with captive portal, dashboard, live logs.
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <vector>
#include "config.h"
#include "device.h"
#include "logger.h"
#include "web_ui.h"

static WebServer server(80);
static DNSServer dns;
static volatile bool s_captiveWanted = false;
static bool s_captiveOn = false;

// ------------------------------------------------------------------ helpers --
static void sendJson(JsonDocument& doc, int code = 200) {
  String s;
  serializeJson(doc, s);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", s);
}

static void sendOk(const char* msg = "ok") {
  JsonDocument d;
  d["ok"] = true;
  d["msg"] = msg;
  sendJson(d);
}

static void redirect(const String& to) {
  server.sendHeader("Location", to);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}

static String urlEnc(const String& s) {
  static const char* hex = "0123456789ABCDEF";
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

// Is this request for us, or a phone's "is there internet?" probe on the hotspot?
static bool isForeignHost() {
  String host = server.hostHeader();
  int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  if (host.length() == 0) return false;
  if (host == WiFi.softAPIP().toString() || host == WiFi.localIP().toString()) return false;
  if (host.equalsIgnoreCase(String(DEVICE_HOSTNAME) + ".local")) return false;
  return true;
}

// ----------------------------------------------------------------- Wi-Fi scan --
static String s_scanCache = "[]";
static uint32_t s_scanStarted = 0;

static void handleScan() {
  int n = WiFi.scanComplete();
  bool scanning = false;
  if (n >= 0) {
    struct Net { String ssid; int rssi; bool lock; };
    std::vector<Net> nets;
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      auto it = std::find_if(nets.begin(), nets.end(), [&](const Net& x) { return x.ssid == s; });
      if (it == nets.end()) nets.push_back({ s, WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN });
      else if (WiFi.RSSI(i) > it->rssi) it->rssi = WiFi.RSSI(i);
    }
    WiFi.scanDelete();
    std::sort(nets.begin(), nets.end(), [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
    JsonDocument d;
    JsonArray arr = d.to<JsonArray>();
    for (auto& x : nets) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = x.ssid;
      o["rssi"] = x.rssi;
      o["lock"] = x.lock;
    }
    s_scanCache = "";
    serializeJson(d, s_scanCache);
  } else if (n == WIFI_SCAN_RUNNING) {
    scanning = true;
  } else if (server.hasArg("refresh") || millis() - s_scanStarted > 15000 || s_scanStarted == 0) {
    if (WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING) {
      s_scanStarted = millis();
      scanning = true;
    }
  }
  String out = String("{\"scanning\":") + (scanning ? "true" : "false") + ",\"networks\":" + s_scanCache + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

// ------------------------------------------------------------------- routes --
static void handleRoot() {
  if (s_captiveOn && isForeignHost()) { redirect("http://" + WiFi.softAPIP().toString() + "/"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
  JsonDocument d;
  deviceStatus(d);
  sendJson(d);
}

static void handleLogs() {
  uint32_t from = server.hasArg("from") ? strtoul(server.arg("from").c_str(), nullptr, 10) : 0;
  String text;
  uint32_t next = LOG.readSince(from, text);
  server.sendHeader("X-Log-Next", String(next));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", text);
}

static void handleArt() {
  File f = LittleFS.open("/art.jpg", "r");
  if (!f) { server.send(404, "text/plain", "no art"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, "image/jpeg");
  f.close();
}

static void handleSpotifyLogin() {
  String url = deviceSpotifyLoginUrl("http://" + server.hostHeader());
  if (url.length() == 0) { redirect("/"); return; }
  redirect(url);
}

static void handleSpotifyCallback() {
  String msg;
  bool ok = deviceSpotifyCallback(server.arg("code"), server.arg("state"), server.arg("error"), msg);
  redirect(ok ? String("/?spotify=connecting") : "/?spotify_error=" + urlEnc(msg));
}

static void handleNotFound() {
  // Phones check for internet via special URLs; send them to the setup page.
  if (s_captiveOn && isForeignHost()) { redirect("http://" + WiFi.softAPIP().toString() + "/"); return; }
  server.send(404, "text/plain", "Not found");
}

static void webTask(void*) {
  for (;;) {
    bool want = s_captiveWanted;
    if (want != s_captiveOn) {
      if (want) dns.start(53, "*", WiFi.softAPIP());
      else dns.stop();
      s_captiveOn = want;
    }
    if (s_captiveOn) dns.processNextRequest();
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void webSetCaptivePortal(bool on) { s_captiveWanted = on; }

void webBegin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/logs", HTTP_GET, handleLogs);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/art.jpg", HTTP_GET, handleArt);

  server.on("/api/wifi", HTTP_POST, [] {
    String ssid = server.arg("ssid");
    if (ssid.length() == 0 || ssid.length() > 32) { server.send(400, "text/plain", "Missing network name"); return; }
    deviceTryWifi(ssid, server.arg("pass"));
    sendOk("connecting");
  });
  server.on("/api/wifi/remove", HTTP_POST, [] { deviceRemoveWifi(server.arg("ssid")); sendOk(); });
  server.on("/api/setup/done", HTTP_POST, [] { deviceSetupDone(); sendOk(); });

  server.on("/api/spotify/client", HTTP_POST, [] {
    String id = server.arg("id");
    id.trim();
    if (id.length() < 20 || id.length() > 64) { server.send(400, "text/plain", "That doesn't look like a Client ID"); return; }
    deviceSetSpotifyClientId(id);
    sendOk();
  });
  server.on("/spotify/login", HTTP_GET, handleSpotifyLogin);
  server.on("/spotify/callback", HTTP_GET, handleSpotifyCallback);
  server.on("/api/spotify/logout", HTTP_POST, [] { deviceSpotifyLogout(); sendOk(); });

  server.on("/api/update/check", HTTP_POST, [] { deviceCheckUpdate(); sendOk(); });
  server.on("/api/update/install", HTTP_POST, [] { deviceInstallUpdate(); sendOk(); });

  server.on("/api/reset", HTTP_POST, [] {
    String what = server.arg("what");
    if (what != "wifi" && what != "spotify" && what != "all") { server.send(400, "text/plain", "Unknown reset"); return; }
    sendOk("resetting");
    delay(300);   // let the response go out first
    deviceReset(what);
  });
  server.on("/api/restart", HTTP_POST, [] { sendOk("restarting"); delay(300); deviceRestart(); });

  server.onNotFound(handleNotFound);
  const char* headers[] = { "Host" };
  server.collectHeaders(headers, 1);
  server.begin();

  if (MDNS.begin(DEVICE_HOSTNAME)) MDNS.addService("http", "tcp", 80);
  xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 0);
  LOG.printf("Web interface ready: http://%s.local\n", DEVICE_HOSTNAME);
}
