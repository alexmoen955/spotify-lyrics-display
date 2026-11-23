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

## 2. Install the firmware (once, over USB)

1. Install **Arduino IDE 2** from arduino.cc.
2. **Boards Manager**: search `esp32` and install **esp32 by Espressif Systems** (version 4.0.0).
3. **Library Manager**: install all of these:
   - `Adafruit ST7735 and ST7789 Library` (accept "Install all" so it adds Adafruit GFX + BusIO)
   - `U8g2_for_Adafruit_GFX` (by olikraus)
   - `ArduinoJson` (by Benoit Blanchon, version 7.x)
   - `TJpg_Decoder` (by Bodmer)
4. Open `SpotifyLyricsDisplay.ino` and set:
   - **Tools → Board**: ESP32 Dev Module
   - **Tools → Partition Scheme**: **Minimal SPIFFS (1.9MB APP with OTA)**. The sketch's own `partitions.csv` is used automatically; this setting only sets the size check.
   - **Tools → Port**: your ESP32
5. Click **Upload**. If it hangs at `Connecting....`, hold the BOOT button until the upload starts.

After this first upload, new versions install over Wi-Fi from the web page (see *Updates*).

If the picture looks wrong, change `config.h` and upload again:

| Symptom                                  | Fix in `config.h`                                 |
|------------------------------------------|---------------------------------------------------|
| White or black screen only               | Check the wiring, especially CS, DC and RST      |
| Garbage stripe along one or two edges    | `TFT_INIT_TAB INITR_GREENTAB` (or `INITR_BLACKTAB`)|
| Colours look like a negative             | `TFT_INVERT 1`                                    |
| Red and blue swapped                     | `TFT_SWAP_RB 1`                                   |
| Upside down / sideways                   | `TFT_ROTATION` 1, 2 or 3                          |

## 3. Connect to Wi-Fi

With no Wi-Fi saved, the display starts a setup hotspot and shows a QR code.

1. Scan the QR code with your phone (or join the Wi-Fi **Lyrics-Display-Setup**, no password).
2. The setup page opens by itself (otherwise browse to **192.168.4.1**). Tap **Get started**.
3. Pick your network, enter the password and tap **Connect** (2.4 GHz networks only).

You can save several networks later; the display joins the strongest one it knows. If none of them is found for about a minute, the setup hotspot comes back so you can fix it from your phone.

## 4. Connect Spotify

The screen now shows a QR code and the address of the display's web page: **http://lyrics.local** (or the IP address it shows). Open it from a phone or computer on the same Wi-Fi.

1. Go to https://developer.spotify.com/dashboard and create an app (any name, tick **Web API**).
2. In the app's **Settings → Edit**, add this **Redirect URI** and save:
   `https://alexmoen955.github.io/spotify-lyrics-display/`
3. Copy the app's **Client ID** into the web page and click **Save**. (No Client Secret is needed.)
4. Click **Connect Spotify** and then **Agree**. You come back to the display's page, logged in.

The redirect URI is a tiny page in this repository (`docs/index.html`, served by GitHub Pages). Spotify only sends logins to secure `https` pages, so this page forwards the login back to your display on your own network. It stores nothing.

## 5. Use it

Play something on Spotify on any device. Within a few seconds the screen shows:

- **Lyrics view** (when synced lyrics exist): a blurred background in the album's colours that drifts slowly, a thumbnail of the art, the title and artist (long ones scroll), a progress bar in the album's accent colour, and lyrics that glide upward with the current line lighting up word by word. Instrumental gaps show three dots that fill up.
- **Album-art view** (press BOOT, or automatic when a song has no synced lyrics): the full cover with title, artist and a progress bar.
- **Idle**: a clock and date. The backlight dims after 2 minutes.

Lyrics come from **LRCLIB** (lrclib.net), a free community database. Most popular songs have them; some don't.

## The web page (http://lyrics.local)

- **Now playing**: cover, title, progress and whether lyrics were found
- **Spotify**: connected account, disconnect
- **Wi-Fi**: current network and signal, saved networks, add or remove networks
- **Updates**: check for and install new versions
- **Live logs**: everything the display does, in real time (pause, download)
- **Device**: restart, forget Wi-Fi, reset Spotify, factory reset

**Factory reset without the web page:** hold the BOOT button for 10 seconds.

## Updates

Pushing a version tag (for example `v2.1.0`) runs the GitHub Actions workflow in `.github/workflows/release.yml`. It builds the firmware and attaches `firmware.bin` to the GitHub release. The display checks for new releases twice a day; click **Install update** on the web page to install one. Wi-Fi and Spotify settings are kept.

## Tweaks (`config.h`)

`config.h` holds no passwords: Wi-Fi and the Spotify login are stored on the ESP32 itself.

- `LYRICS_LEAD_MS`: raise it if lyrics appear late, lower it if they appear early.
- `WORD_HIGHLIGHT`, `WORD_MS_PER_CHAR`: word-by-word highlighting on/off and its speed.
- `TIMEZONE`: for the idle clock (examples in the file).
- `POLL_INTERVAL_MS`: how often to ask Spotify (2.5 s by default; don't go below ~1.5 s or you'll be rate-limited).

## Troubleshooting

The **Live logs** on the web page show every step (or use **Tools → Serial Monitor** at 115200 baud over USB).

- *Spotify shows "INVALID_CLIENT: Invalid redirect URI"*: the redirect URI from step 4 isn't saved in your Spotify app.
- *"Spotify login expired"*: click **Connect Spotify** again on the web page.
- *Can't open lyrics.local*: use the IP address shown on the screen (some Android phones don't support `.local` names).
- Non-Latin lyrics (Japanese, Korean, Cyrillic…) won't render, because the built-in fonts only cover Western European characters.
- Podcasts and ads show the idle screen.
