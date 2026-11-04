# BikeMiniNav v0.5 · Round AMOLED UI

A replacement firmware screen for **Waveshare ESP32-S3 1.43-inch AMOLED (466 × 466)**. It uses the same NimBLE service, characteristic, 12-byte navigation packets, and 288 × 288 JPEG transfer as the working **Bluefy BikeMiniNav v0.4** website.

## What changed

- Edge-to-edge, 466 × 466 street map, scaled from the browser's 288 × 288 JPEG. This makes the map larger, **not higher resolution**; use **Close · 18** in the website for closer streets.
- Dark translucent cards for maneuver and ride metrics, large typographic values, and clean vector turn arrows. These use LVGL alpha blending, **not true background blur**.
- **Three screens**: live map, next turn, and ride dashboard.
- Swipe left/right on the round touch display to move between screens. Tap the upper half to toggle the next-turn view; tap the lower half to toggle ride data. Actual map panning and previewing future turns still require the iPhone app because the firmware receives only the current 288 × 288 JPEG and the next maneuver.
- BLE starts before AMOLED initialization so the device remains discoverable when the display fails; uses name-only advertising proven to work with Bluefy.
- Repeating USB-serial diagnostics, CRC-checked double-buffered JPEG reception and a stale-navigation warning after 25 seconds.
- The mini-map retains OpenStreetMap contributor attribution on the circular screen.

## Replace your firmware on Linux

In your local checkout of `https://github.com/Saazh/bikemininav-bluefy`, activate your existing venv:

```bash
cd ~/YOUR_PATH/bikemininav-bluefy
source .venv/bin/activate
cp firmware/src/main.cpp firmware/src/main.cpp.backup
```

Extract this ZIP (or copy its files into the repository). Copy the included files:

```bash
cp /PATH_TO_EXTRACTED/BikeMiniNav_Apple_Firmware/firmware/src/main.cpp firmware/src/main.cpp
cp /PATH_TO_EXTRACTED/BikeMiniNav_Apple_Firmware/firmware/tools/enable_apple_fonts.py firmware/tools/
```

If your project already uses the working `platformio.ini` with USB CDC flags and NimBLE-Arduino 2.3.0, **keep it**. The provided `platformio.ini` is supplied as a known configuration for convenience.

**Important:** fonts need to be enabled in `lv_conf.h`. If this is a fresh checkout, bootstrap the Waveshare files first:

```bash
cd firmware
python tools/bootstrap_drivers.py
python tools/enable_apple_fonts.py
pio run -t clean
pio run -t upload
pio device monitor -b 115200
```

If `bootstrap_drivers.py` was already run, you need only run `enable_apple_fonts.py` before building. Always run the font script **after** bootstrap, since bootstrap replaces `include/lv_conf.h`.

### Confirm it works

1. Boot the Waveshare. The AMOLED should display a dark map-first layout and `WAITING FOR IPHONE`.
2. Open `https://saazh.github.io/bikemininav-bluefy/web/` in **Bluefy** and connect to `BikeMiniNav`.
3. Tap **Send test mini-map**. A larger map should fill the circular display and the example navigation data should update the cards.
4. Swipe left to the giant direction arrow, swipe left again to the speed/ride dashboard, and swipe left once more to return to the map.
5. Plan a real route on the iPhone and keep Bluefy foregrounded and the phone unlocked while riding.

### Diagnostics

On USB serial at 115200 you should see repeating lines similar to:

```
ESP32 ALIVE | BLE=ON | display=READY | map=1 | rejected=0
MAP: displayed 23543 bytes
UI: page = TURN
```

If the AMOLED is black but BLE appears, check for `DISPLAY: AMOLED begin() failed`. If the build reports undefined `lv_font_montserrat_48`, rerun the font script after the driver bootstrap. If the watch refuses a JPEG, check the `rejected` counter and whether the website sent a 288 × 288 JPEG under 90 KB.

## Limitations

This is a **prototype**, not a safety-certified turn-by-turn system. A locked iPhone can suspend Bluefy and stop updates. The screen does not have offline maps or future turn data, and the BLE write characteristic is not paired/authenticated. Full-screen scaling has a cost in PSRAM bandwidth and image sharpness. Test stationary before cycling. The updated source was statically checked, but **could not be compiled or tested on the Waveshare in this environment**, so please report PlatformIO errors if they appear.
