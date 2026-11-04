# BikeMiniNav v0.4 – Refined interactive map (Linux + iPhone Bluefy)

This package replaces the **`web/` folder only** in the public repository:
https://github.com/Saazh/bikemininav-bluefy

**Keep the working Waveshare firmware as it is.** This version retains the BLE UUIDs, 12-byte navigation packet, and JPEG transfer protocol. It also retains the v0.3 HeiGIT routing endpoints.

## Changes

- Map-first, iPhone-friendly interface with translucent/frosted controls inspired by Apple's clean design language.
- iPhone map starts close at zoom **17.5**, route planning returns to street-level follow mode rather than fitting the entire route.
- Drag or pinch to explore the map; the map stays in explore mode instead of snapping back to GPS.
- Right-side **+ / −** controls, **⌾ Follow** current GPS, **◇ Show route** overview.
- Small translucent maneuver pins along the route. Tap a pin or use **‹ / ›** to preview individual turns. The map zooms to the selected turn.
- **Choose destination** toggles tap-to-place mode so exploring the route never accidentally changes your destination.
- Compact floating turn, speed, time, and distance chips on top of the map.
- Waveshare mini-map default zoom increased from 16 to **17**; the device setting allows 16/17/18. Its JPEG now contains small translucent maneuver symbols when they are in view.
- The Waveshare's mini-map remains GPS-centered while the iPhone explores a route. This revision does **not** add direct touch panning to the Waveshare firmware.
- Search and route HTTP requests continue using HeiGIT/openrouteservice from v0.3.

## Install on Linux

From your local repository checkout (`bikemininav-bluefy/`), after downloading the ZIP:

```bash
unzip ~/Downloads/BikeMiniNav_Apple_Map_Update.zip -d /tmp
cp -a /tmp/BikeMiniNav_Apple_Map_Update/web/. ./web/
node --check web/app.mjs
node --test web/tests/*.test.mjs
git add web
git commit -m "Redesign cycling map and add route step explorer"
git push origin main
```

Adjust the ZIP path if your browser saved it elsewhere. GitHub Pages should publish the new web version automatically. Open:
https://saazh.github.io/bikemininav-bluefy/web/

Reload the page in Bluefy until **v0.4** appears in the header. If the old design persists, close/reopen the Bluefy tab or clear the site's cache.

## How to use

1. Open the HTTPS page in **Bluefy** on your iPhone.
2. In **Where to?**, tap **Enable iPhone location** and grant precise location access. If you have no GPS fix, the map initially shows Bremen.
3. Open **Routing API settings** and enter your HeiGIT/openrouteservice API key (if it is not saved).
4. Search a destination, or tap **Choose destination** and tap a point on the map.
5. Tap **Plan cycling route**. It will show nearby streets, route highlight, tiny turn icons, and the next maneuver at the top.
6. Drag or pinch to explore. Tap a maneuver badge on the map or **‹ / ›** on the bottom floating card to inspect turns. **◇** shows the whole route, **⌾** returns to following the rider.
7. In **Your Waveshare**, tap **Connect BikeMiniNav**, then **Send test mini-map**. Switch **Display map zoom** to **Close · 18** for an even closer watch view. This setting affects the next transmitted JPEG; without GPS, tap **Send test mini-map** again to transmit it.

## Notes and limitations

- The round watch screen still gets a **288 × 288 JPEG** centered on the phone's GPS. Its preview zoom is independent of the iPhone's interactive Leaflet map zoom.
- BLE JPEG transfer speed depends on negotiated ATT packet limits and phone/device behavior; fallbacks remain available.
- You must keep Bluefy active and the iPhone unlocked for reliable continuous position and Bluetooth updates. iOS backgrounding can suspend the browser.
- Rerouting remains manual. This is a prototype, not yet a verified safety-critical navigation device.
- OpenStreetMap standard tiles are fetched only as needed for the visible mini-map. Preserve map attribution and comply with OSM tile usage policy.
- Do not commit your routing API key to GitHub; enter it in the browser's private settings.

## Validation

- `node --check` completed on all changed modules.
- Eight Node unit tests pass, including new route-step mapping tests.
- Chromium browser smoke test was run with mocked GPS, BLE-free rendering, and simulated routing response, confirming route creation and next-step navigation.
- Live iPhone/Bluefy BLE and actual Waveshare image transfer remain unverified in this environment.
