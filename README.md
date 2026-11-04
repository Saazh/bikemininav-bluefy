# BikeMiniNav · Linux + iPhone Bluefy + Waveshare 1.43" AMOLED

**Source-code prototype, not hardware verified.** Linux-only development: ESP32-S3 firmware in PlatformIO and an HTTPS web navigation app used from Bluefy on iPhone. **No Xcode or Mac needed.**

The iPhone gets GPS; **openrouteservice** (using your personal API key) returns cycling directions; the browser loads only currently visible **OpenStreetMap** tiles; a 288×288 north-up map is drawn to canvas with a route and heading indicator and JPEG-compressed; Bluefy sends BLE frames; the Waveshare decodes JPEG into its LVGL9 canvas on a 466×466 round AMOLED. Turn instructions are sent as a separate 12-byte binary BLE packet.

## Requirements

- Waveshare **ESP32-S3-Touch-AMOLED-1.43** (466×466, SH8601/CO5300, FT3168; 16 MB flash, 8 MB PSRAM). **Not** the 1.43-B or 1.8-inch board.
- Linux computer with Python 3, Git, PlatformIO (`pio`) and a USB cable that supports data.
- iPhone with [Bluefy Web BLE Browser](https://bluefy.app/), Bluetooth on, location permission, internet and an active unlocked screen while testing.
- An account + API key at [openrouteservice](https://openrouteservice.org/dev/#/signup) for destination search and real **cycling-regular** routes. **A map-only/test transfer does not need a key.**
- HTTPS hosting (GitHub Pages, Cloudflare Pages, Netlify or your own server) for the website; Bluefy and phone geolocation cannot be expected to work from `file://` or plain HTTP.

## A. Flash ESP32-S3 from Linux

Open a terminal in `firmware/`:

```bash
pipx install platformio
# On Ubuntu/Debian, if needed first: sudo apt install pipx
# Alternatively install with a Python virtual environment or use VS Code PlatformIO.
python3 tools/bootstrap_drivers.py
pio run
pio device list
pio run -t upload
pio device monitor -b 115200
```

The bootstrap script clones the community LVGL9 Waveshare demonstration and copies its device-driver files into your local `firmware/src/`. The driver's code is **not** bundled in this ZIP. If you have previously cloned it, use `python3 tools/bootstrap_drivers.py --source /path/to/clone`. The demo is maintained at <https://github.com/thelastoutpostworkshop/waveshare_esp32s3_1.43_amoled_lvgl9>.

If `pio` is not found after pip installation, use `~/.local/bin/pio`, activate your venv, or add `~/.local/bin` to PATH. Flashing requires working Linux serial permissions (often `dialout` group; reconnect/login after changes). For a busy USB port, close the serial monitor before uploading.

Expected: the board shows "BIKE MINI NAV / Waiting for iPhone" and advertises BLE name `BikeMiniNav` with service UUID `7e9d1000-2a39-4b25-b3e4-cb7d1fc2ac10`.

> Firmware driver compatibility is inherited from a third-party BSP, and cannot be guaranteed without a board-specific build and hardware run. Arduino core/esp_lcd versions may need adjustment. Start from a known working build of that demo if you hit a compilation error.

## B. Publish the web app using GitHub Pages (all from Linux)

The `web/` folder is entirely static and **contains no pre-filled API keys**.

1. Create a new GitHub repository, for example `BikeMiniNav`.
2. From the extracted project root, run:

```bash
git init
git add README.md .gitignore web firmware
git commit -m "BikeMiniNav Bluefy browser and Waveshare firmware"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/BikeMiniNav.git
git push -u origin main
```

Use your own GitHub username and repository name in the remote URL.

3. On GitHub: **Settings → Pages → Deploy from a branch → main / (root)**, then save.
4. Open **`https://YOUR_USERNAME.github.io/BikeMiniNav/web/` in Bluefy** on the iPhone. Be sure to use the repository name and username you actually chose.

> You can preview the UI on Linux using `cd web && python3 -m http.server 8000`, then open `http://localhost:8000` in your Linux browser. That is **only** for UI testing, not iPhone Web Bluetooth. Don't use an unencrypted HTTP page on your phone.

## C. First test without routing key

1. Turn on the Waveshare and open your deployed `.../web/` URL in **Bluefy**, not Safari.
2. Enable iPhone Bluetooth and grant Bluefy Bluetooth permission when requested.
3. Tap **Connect BikeMiniNav**, select the BLE peripheral, wait for "Connected".
4. Tap **Send test mini-map**. The phone downloads the current map tiles (defaults to Bremen until GPS is enabled), paints a *simulated* route over them, encodes a 288×288 JPEG and sends it over BLE.
5. Check the Waveshare: actual streets plus a blue direction marker, turquoise example route, "Right 120m" and speed ~18.2 km/h. The status log shows map bytes, packet progress and any BLE errors.
6. If 180-byte packets fail, the browser retries 120, 64, then **20 bytes** (the slower but safest BLE write size). The 20-byte mode may take a long time for a full map. Don't start a new transfer while one is active.

If the map is blank, look in the web app's log for tile errors and check internet, HTTPS and CORS. If BLE is stuck, reconnect and send the test again.

## D. Use real GPS and cycling navigation

1. Tap **Enable iPhone GPS**, grant location permission and wait for the blue position dot.
2. Paste your ORS API key into the password field. The app uses it only for geocoding and route requests; **do not commit your key to GitHub**. By default, it's not persisted across visits; the optional “Remember” checkbox uses `localStorage` on your phone.
3. Search a location, pick a result (or tap a destination directly on the map). You may also paste `latitude, longitude` coordinates in the search field without geocoding.
4. Tap **Plan bicycle route**. ORS calculates cycling-regular directions and returns a GeoJSON route with maneuvers. The phone shows the line and updates the next turn as GPS moves.
5. When BLE is connected and GPS enabled, the app sends a compact turn/navigation packet about every 3 seconds. It sends an updated JPEG mini-map roughly every 12 seconds while moving; when stationary it throttles to about 60 seconds. The transfer itself can take longer on a slow BLE connection.
6. Keep Bluefy in the foreground with the screen unlocked. Browser GPS/BLE activity may stop if iOS locks the screen or suspends Bluefy.

**Off route:** the web app displays an alert if you're more than about 60 m from the planned route, but it does **not automatically reroute**. Stop safely and tap Plan bicycle route again.

## E. BLE protocol (one write-with-response characteristic)

GATT service `7e9d1000-2a39-4b25-b3e4-cb7d1fc2ac10`, characteristic `7e9d1001-2a39-4b25-b3e4-cb7d1fc2ac10`.

| Opcode | Bytes | Definition |
|---|---:|---|
| `0x05` | 12 | Navigation: u8 turn code, u16 LE distance to turn (m), u32 LE remaining (m), u16 LE speed ×10 km/h, u16 LE heading (degrees) |
| `0x02` | 9 | Begin JPEG: u32 LE JPEG byte length, u32 LE CRC32 |
| `0x03` | variable | JPEG fragment: opcode + JPEG data bytes (20/64/120/180 total bytes per packet) |
| `0x04` | 1 | End JPEG: check byte count and CRC32 then decode |
| `0x01` | variable | Legacy navigation JSON accepted by firmware for backward compatibility |

Image resolution is **288×288**; firmware accepts max **90,000 JPEG bytes**. Image transfer is sequential and uses `writeValueWithResponse`. The binary turn packet fits 20-byte ATT characteristics even with default BLE MTU. The decoder updates LVGL from the main loop, never a BLE callback.

Shared logic: `web/protocol.mjs`, `web/geometry.mjs`, `web/map-render.mjs`, `web/app.mjs`, `firmware/src/main.cpp`.

## F. Run source-level tests from Linux

```bash
node --test web/tests/*.test.mjs
node --check web/app.mjs
python3 -m unittest discover -s tests -v
```

These check CRC32, byte framing, low-MTU navigation, coordinate projection and route progress. No board-specific compilation or actual iPhone BLE integration has been verified here.

## Limitations, security and map licenses

- **Prototype only**. No hardware verification, no weatherproofing, no independent GPS. Map update latency depends on mobile internet, CORS and BLE throughput. A map can be stale: the display warns after about 25 seconds with no navigation packet.
- Browser BLE has no foreground/background guarantees on iOS. You generally must keep the iPhone unlocked with Bluefy on-screen.
- GPS measurements have errors. Route matching is a nearest-segment estimate, *not* a full map-matching engine. Turn timing and the heading marker need road testing.
- The BLE GATT characteristic is **not authenticated** in this version. Nearby BLE clients could inject fake navigation. Add authenticated pairing if you want to rely on it beyond controlled tests.
- Web map tiles are loaded from `tile.openstreetmap.org`, only around the **currently viewed** position and at one zoom level; no offline bulk downloads or prefetching. The browser is allowed to obey normal HTTP caching headers. Attribution is displayed on the phone and in the image/on the device. Follow the [OSMF tile usage policy](https://operations.osmfoundation.org/policies/tiles/).
- ORS has its own usage limits and API-key policy. Keep the API key private; a client-side browser app cannot completely protect an API key against inspection on that device. Consider a restricted backend proxy for public deployment.
- If OSM tile CORS prevents reading pixels into Canvas in your iPhone/Bluefy version, the mini-map transfer will fail visibly instead of pretending to succeed. Switch to a licensed CORS-capable tile provider in `web/map-render.mjs` if necessary.
- Display brightness and power use must be tested outdoors; the board also needs a separate suitable power supply and protected enclosure.

## Sources

- Waveshare board: <https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43>
- Driver BSP: <https://github.com/thelastoutpostworkshop/waveshare_esp32s3_1.43_amoled_lvgl9>
- Bluefy setup: <https://bluefy.app/guide.html>
- ORS directions: <https://openrouteservice.org/dev/>
- ORS geocoding: <https://giscience.github.io/openrouteservice/api-reference/endpoints/geocoder/>
- OSM tile policy: <https://operations.osmfoundation.org/policies/tiles/>
# bikemininav-bluefy
