// Everything the user sets up through the web page, stored in the ESP32's
// own flash (NVS). Nothing secret lives in the source code any more.
#pragma once
#include <Arduino.h>
#include <vector>

struct WifiCred {
  String ssid, pass;
};

namespace Settings {
void begin();

std::vector<WifiCred> wifiList();
void addWifi(const String& ssid, const String& pass);   // replaces an existing entry with the same name
bool removeWifi(const String& ssid);
void clearWifi();

String spotifyClientId();
void setSpotifyClientId(const String& id);
String spotifyRefreshToken();
void setSpotifyRefreshToken(const String& token);
String spotifyUser();
void setSpotifyUser(const String& name);
void clearSpotifyLogin();   // keeps the Client ID

void factoryReset();
}  // namespace Settings
