// What the web server (web.cpp) can ask of the device (app.cpp).
// Anything that needs the internet is only *requested* here and carried out by
// the network task, so two TLS connections never compete for the little RAM.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Fill `doc` with everything the web page shows.
void deviceStatus(JsonDocument& doc);

// Wi-Fi
void deviceTryWifi(const String& ssid, const String& pass);   // join; saved only if it works
void deviceRemoveWifi(const String& ssid);
void deviceSetupDone();            // user finished the hotspot setup: turn the hotspot off soon
bool deviceHotspotActive();

// Spotify
void deviceSetSpotifyClientId(const String& id);
String deviceSpotifyLoginUrl(const String& deviceBaseUrl);   // empty if no Client ID yet
bool deviceSpotifyCallback(const String& code, const String& state, const String& error, String& msg);
void deviceSpotifyLogout();

// Updates
void deviceCheckUpdate();
void deviceInstallUpdate();

// Reset / restart. what = "wifi", "spotify" or "all"
void deviceReset(const String& what);
void deviceRestart();

// web.cpp
void webBegin();
void webSetCaptivePortal(bool on);
