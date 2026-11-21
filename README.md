# Spotify Now Playing + Synced Lyrics: setup guide

Hardware: ESP32 WROOM dev board + 1.44" 128×128 SPI LCD (ST7735S driver; usually a red board marked "1.44 TFT SPI 128x128").

## 1. Wiring

Unplug the ESP32 from USB first.

| LCD pin (labels vary)        | ESP32 pin | Purpose                     |
|------------------------------|-----------|-----------------------------|
| VCC                          | 3V3       | power (use 3.3 V, not 5 V)  |
| GND                          | GND       | ground                      |
| SCL / SCK / CLK              | GPIO 18   | SPI clock                   |
| SDA / MOSI / DIN             | GPIO 23   | SPI data                    |
| RES / RST / RESET            | GPIO 17   | reset                       |
| DC / A0 / RS                 | GPIO 16   | data/command                |
| CS                           | GPIO 5    | chip select                 |
| BL / LED / BLK               | GPIO 4    | backlight (dimmable)        |

- "SDA" on these screens is SPI data, not I²C. It still goes to GPIO 23.
- If your board has no BL/LED pin, ignore that row. If you'd rather not dim the backlight, wire BL to 3V3 and set `TFT_BL` to `-1` in `config.h`.
- The **BOOT** button on the ESP32 board toggles between the lyrics view and the album-art view. You don't need to wire anything for it.
- Keep the jumper wires short (under 15 cm) for a clean picture.

## 2. Arduino IDE

1. Install **Arduino IDE 2** from arduino.cc.
2. **Boards Manager**: search `esp32` and install **esp32 by Espressif Systems**.
3. **Library Manager**: install all of these:
   - `Adafruit ST7735 and ST7789 Library` (accept "Install all" so it adds Adafruit GFX + BusIO)
   - `U8g2_for_Adafruit_GFX` (by olikraus)
   - `ArduinoJson` (by Benoit Blanchon, version 7.x)
   - `TJpg_Decoder` (by Bodmer)

## 3. Test the screen

Open `SpotifyLyricsDisplay.ino` and set your WiFi in `config.h`. Leave the Spotify fields empty for now. You can list several networks: the display joins the strongest one it knows and switches automatically if it drops (2.4 GHz only):

```c
struct WifiNet { const char* ssid; const char* pass; };
static const WifiNet WIFI_LIST[] = {
  { "home-wifi",     "password" },
  { "phone-hotspot", "password" },
};
```

Then set:

- **Tools → Board**: ESP32 Dev Module
- **Tools → Partition Scheme**: **Huge APP (3MB No OTA/1MB SPIFFS)**. The default scheme is too small for this sketch.
- **Tools → Port**: your ESP32

Click **Upload**. If it hangs at `Connecting....`, hold the BOOT button until the upload starts.

You should see an animated spinner, then the message *"Add your Spotify keys to config.h"*. That means the wiring works.

If the picture looks wrong:

| Symptom                                  | Fix in `config.h`                                 |
|------------------------------------------|---------------------------------------------------|
| White or black screen only               | Check the wiring, especially CS, DC and RST      |
| Garbage stripe along one or two edges    | `TFT_INIT_TAB INITR_GREENTAB` (or `INITR_BLACKTAB`)|
| Colours look like a negative             | `TFT_INVERT 1`                                    |
| Red and blue swapped                     | `TFT_SWAP_RB 1`                                   |
| Upside down / sideways                   | `TFT_ROTATION` 1, 2 or 3                          |

## 4. Spotify keys (one time)

1. Go to https://developer.spotify.com/dashboard, log in, and click **Create app**.
   - Name/description: anything
   - **Redirect URI**: `http://127.0.0.1:8888/callback` (exactly this, then click **Add**)
   - Under APIs, tick **Web API**, then **Save**.
2. Open the app's **Settings** and copy the **Client ID**. Click **View client secret** and copy that too.
3. On your Mac, run the helper:
   ```bash
   python3 tools/get_spotify_token.py
   ```
   Paste the Client ID and Secret. A browser opens: click **Agree**.
4. The script prints three `#define` lines. Paste them over the empty ones in `config.h`.
5. Set `TIMEZONE` in `config.h` (examples are listed there) so the idle clock is right.
6. Upload again.

## 5. Use it

Play something on Spotify on any device. Within a few seconds the screen shows:

- **Lyrics view** (when synced lyrics exist): a blurred background in the album's colours that drifts slowly, a thumbnail of the art, the title and artist (long ones scroll), a progress bar in the album's accent colour, and lyrics that glide upward. The current line shows in bright white. Instrumental gaps show three dots that fill up.
- **Album-art view** (press BOOT, or automatic when a song has no synced lyrics): the full cover with title, artist, a progress bar and times.
- **Idle**: a clock and date. The backlight dims after 2 minutes.

Lyrics come from **LRCLIB** (lrclib.net), a free community database. Most popular songs have them; some don't.

## Tweaks (`config.h`)

- `LYRICS_LEAD_MS`: raise it if lyrics appear late, lower it if they appear early.
- `POLL_INTERVAL_MS`: how often to ask Spotify (2.5 s by default; don't go below ~1.5 s or you'll be rate-limited).

## Troubleshooting

Open **Tools → Serial Monitor** at 115200 baud. Every step is logged there.

- *"Spotify login rejected"*: the refresh token is wrong or was revoked. Run the helper again.
- *"WiFi failed"*: the ESP32 only works on 2.4 GHz networks.
- Non-Latin lyrics (Japanese, Korean, Cyrillic…) won't render, because the built-in fonts only cover Western European characters.
- Podcasts and ads show the idle screen.
