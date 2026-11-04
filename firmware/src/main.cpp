// BikeMiniNav | circular / glass UI for Waveshare ESP32-S3 1.43 AMOLED (466px)
// Compatible with BikeMiniNav web v0.4, BLE opcodes 0x01..0x05 and 288/384px JPEG.
// Keep display updates on the main loop; NimBLE callbacks run on another task.
// Requires LVGL 9.2.2, NimBLE-Arduino 2.x and the original Waveshare drivers.
// IMPORTANT: Navigation packets are not authenticated. Prototype, not safety-critical.

#include <Arduino.h>
#define LV_CONF_INCLUDE_SIMPLE
#include <lvgl.h>
#include <JPEGDEC.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <cstring>
#include <cmath>
#include "amoled.h"
#include "FT3168.h"

namespace {
constexpr int SCREEN = 466;
constexpr int MIN_MAP_SIZE = 288;
constexpr int MAX_MAP_SIZE = 384;
constexpr size_t MAX_JPG = 90000;
constexpr uint32_t LOST_AFTER_MS = 25000;
constexpr char SERVICE_UUID[] = "7E9D1000-2A39-4B25-B3E4-CB7D1FC2AC10";
constexpr char WRITE_UUID[]   = "7E9D1001-2A39-4B25-B3E4-CB7D1FC2AC10";
constexpr uint32_t BG = 0x09111B;
constexpr uint32_t WHITE = 0xFFFFFF;
constexpr uint32_t MUTED = 0xA5B7C7;
constexpr uint32_t CYAN = 0x55E3DD;
constexpr uint32_t CARD_BG = 0x101C2A;

const char *TURN_NAMES[] = {
    "Turn left", "Turn right", "Sharp left", "Sharp right", "Slight left",
    "Slight right", "Continue", "Roundabout", "Exit roundabout",
    "Make a U-turn", "Destination", "Start", "Keep left", "Keep right"
};

Amoled amoled;
SemaphoreHandle_t rxMutex = nullptr;
lv_display_t *display = nullptr;
NimBLEServer *bleServer = nullptr;
static bool displayReady = false;
static bool bleStarted = false;

// Separate BLE receive and decode buffers. Both are guarded by rxMutex.
static uint8_t *rxJPEG = nullptr;
static uint8_t *pendingJPEG = nullptr;
static size_t rxCount = 0, expectedBytes = 0, pendingLen = 0;
static uint32_t expectedCRC = 0;
static bool receiving = false, imageReady = false, decoding = false;
static uint32_t framesOK = 0, framesRejected = 0;

// Decode map frames into RGB565 before scaling to the 466px display.
static uint16_t *decodedRGB = nullptr;
static uint16_t *screenRGB = nullptr;
static int decodedMapSize = 0;

struct NavData {
    char instruction[80] = "Connect iPhone";
    uint8_t kind = 6;
    int32_t turnM = 0;
    uint32_t remainingM = 0;
    float speedKmh = 0;
    uint16_t heading = 0;
};
static NavData incomingNav;
static NavData currentNav;
static bool navDirty = false;
static bool everReceivedNav = false;
static uint32_t lastNavAt = 0;
static bool navLost = false;

enum class Page : uint8_t { MAP = 0, TURN = 1, RIDE = 2 };
static Page page = Page::MAP;
static int pendingGesture = 0;  // -1 = right, 1 = left, 2 = tap top, 3 = tap bottom.
static bool lastTouchDown = false;
static int16_t touchBeginX = 0, touchBeginY = 0;
static uint32_t touchBeganAt = 0;

// Widget handles (all LVGL calls must remain in setup()/loop()).
static lv_obj_t *mapCanvas = nullptr;
static lv_obj_t *statusPill = nullptr, *statusDot = nullptr, *statusLabel = nullptr;
static lv_obj_t *navCard = nullptr, *navAction = nullptr, *navDistance = nullptr;
static lv_obj_t *bottomCard = nullptr, *mapSpeed = nullptr, *mapRemaining = nullptr;
static lv_obj_t *mapHint = nullptr, *credit = nullptr;
static lv_obj_t *turnScreen = nullptr, *turnTitle = nullptr, *turnDistance = nullptr;
static lv_obj_t *turnAction = nullptr, *turnHint = nullptr;
static lv_obj_t *rideScreen = nullptr, *rideSpeed = nullptr, *rideRemaining = nullptr;
static lv_obj_t *rideHeading = nullptr;

struct ArrowGlyph {
    lv_obj_t *shaft = nullptr;
    lv_obj_t *head = nullptr;
    lv_point_precise_t shaftPoints[4] = {};
    lv_point_precise_t headPoints[3] = {};
    uint16_t size = 0;
};
static ArrowGlyph smallArrow, bigArrow;

static uint32_t readU32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static uint16_t readU16(const uint8_t *p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
static uint32_t crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
    }
    return ~crc;
}

class PacketReceiver final : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic,
                 NimBLEConnInfo &connection) override {
        (void)connection;
        NimBLEAttValue value = characteristic->getValue();
        const uint8_t *p = value.data();
        const size_t n = value.size();
        if (!p || n == 0 || !rxMutex) return;

        xSemaphoreTake(rxMutex, portMAX_DELAY);
        switch (p[0]) {
            case 0x01: {  // Legacy JSON navigation, still supported.
                JsonDocument json;
                if (!deserializeJson(json, p + 1, n - 1)) {
                    incomingNav.turnM = json["turn_m"] | 0;
                    incomingNav.remainingM = json["remain_m"] | 0;
                    incomingNav.speedKmh = json["speed"] | 0.0f;
                    incomingNav.kind = json["turn_type"] | 6;
                    const char *s = json["instruction"] | "Continue";
                    snprintf(incomingNav.instruction, sizeof(incomingNav.instruction), "%s", s);
                    navDirty = true;
                    everReceivedNav = true;
                    lastNavAt = millis();
                }
                break;
            }
            case 0x05: {  // Current compact 12-byte web v0.4 navigation.
                if (n != 12) break;
                incomingNav.kind = p[1];
                incomingNav.turnM = readU16(p + 2);
                incomingNav.remainingM = readU32(p + 4);
                incomingNav.speedKmh = readU16(p + 8) / 10.0f;
                incomingNav.heading = readU16(p + 10);
                const uint8_t kind = incomingNav.kind;
                snprintf(incomingNav.instruction, sizeof(incomingNav.instruction), "%s",
                         kind < sizeof(TURN_NAMES)/sizeof(TURN_NAMES[0])
                         ? TURN_NAMES[kind] : "Continue");
                navDirty = true;
                everReceivedNav = true;
                lastNavAt = millis();
                break;
            }
            case 0x02: {  // JPEG start: opcode, length LE32, CRC32 LE32.
                receiving = false;
                rxCount = 0;
                if (n != 9 || decoding || imageReady) break;
                expectedBytes = readU32(p + 1);
                expectedCRC = readU32(p + 5);
                receiving = rxJPEG && pendingJPEG && expectedBytes > 0 && expectedBytes <= MAX_JPG;
                if (!receiving) ++framesRejected;
                break;
            }
            case 0x03: {  // JPEG data, 1-byte opcode + up to MTU-3-1 payload.
                if (!receiving) break;
                if (n < 2 || rxCount > expectedBytes || n - 1 > expectedBytes - rxCount) {
                    receiving = false;
                    ++framesRejected;
                    break;
                }
                memcpy(rxJPEG + rxCount, p + 1, n - 1);
                rxCount += n - 1;
                break;
            }
            case 0x04: {  // Commit only if complete and CRC matches.
                if (receiving && rxCount == expectedBytes &&
                    !decoding && !imageReady && crc32(rxJPEG, rxCount) == expectedCRC) {
                    uint8_t *tmp = rxJPEG;
                    rxJPEG = pendingJPEG;
                    pendingJPEG = tmp;
                    pendingLen = rxCount;
                    imageReady = true;
                    ++framesOK;
                } else if (receiving) {
                    ++framesRejected;
                }
                receiving = false;
                break;
            }
            default: break;
        }
        xSemaphoreGive(rxMutex);
    }
};
static PacketReceiver receiver;

static void startBLE() {
    Serial.println("BLE: starting name-only advertising");
    NimBLEDevice::init("BikeMiniNav");
    NimBLEDevice::setMTU(247);
    bleServer = NimBLEDevice::createServer();
    bleServer->advertiseOnDisconnect(true);
    NimBLEService *service = bleServer->createService(SERVICE_UUID);
    NimBLECharacteristic *write = service->createCharacteristic(
        WRITE_UUID, NIMBLE_PROPERTY::WRITE, 260);
    write->setCallbacks(&receiver);
    service->start();
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    // Tested on Bluefy: do NOT pack the full 128-bit UUID in the ad payload.
    advertising->setName("BikeMiniNav");
    bleStarted = advertising->start();
    Serial.printf("BLE: advertising BikeMiniNav = %s\n", bleStarted ? "YES" : "NO");
}

static int jpegDraw(JPEGDRAW *block) {
    if (!decodedRGB || block->x < 0 || block->y < 0 ||
        block->x + block->iWidth > decodedMapSize ||
        block->y + block->iHeight > decodedMapSize) return 0;
    for (int y = 0; y < block->iHeight; ++y) {
        memcpy(decodedRGB + (block->y + y) * decodedMapSize + block->x,
               block->pPixels + y * block->iWidth,
               block->iWidth * sizeof(uint16_t));
    }
    return 1;
}

// Interpolate RGB565 channels to keep street labels legible on the larger round map.
static inline uint16_t blend565(uint16_t a, uint16_t b, uint32_t t) {
    const uint32_t inv = 256 - t;
    const uint32_t r = ((((a >> 11) & 31U) * inv + ((b >> 11) & 31U) * t) >> 8);
    const uint32_t g = ((((a >> 5) & 63U) * inv + ((b >> 5) & 63U) * t) >> 8);
    const uint32_t bl = (((a & 31U) * inv + (b & 31U) * t) >> 8);
    return (r << 11) | (g << 5) | bl;
}

static void expandMapToRoundScreen(int sourceSize) {
    static uint16_t x0[SCREEN], x1[SCREEN];
    static uint8_t fx[SCREEN];
    for (int x = 0; x < SCREEN; ++x) {
        const uint32_t sx = ((uint32_t)x * (sourceSize - 1) * 256U) / (SCREEN - 1);
        x0[x] = sx >> 8;
        x1[x] = x0[x] < sourceSize - 1 ? x0[x] + 1 : x0[x];
        fx[x] = sx & 255U;
    }

    // The display's own circular glass clips the unused corner pixels.
    for (int y = 0; y < SCREEN; ++y) {
        const uint32_t sy = ((uint32_t)y * (sourceSize - 1) * 256U) / (SCREEN - 1);
        const int y0 = sy >> 8;
        const int y1 = y0 < sourceSize - 1 ? y0 + 1 : y0;
        const uint32_t fy = sy & 255U;
        const uint16_t *row0 = decodedRGB + y0 * sourceSize;
        const uint16_t *row1 = decodedRGB + y1 * sourceSize;
        for (int x = 0; x < SCREEN; ++x) {
            const uint16_t a = blend565(row0[x0[x]], row0[x1[x]], fx[x]);
            const uint16_t b = blend565(row1[x0[x]], row1[x1[x]], fx[x]);
            screenRGB[y * SCREEN + x] = blend565(a, b, fy);
        }
        if ((y & 31) == 0) delay(0);  // Yield during JPEG/map processing.
    }
}

static void fillWelcomeBackground() {
    // Soft midnight gradient, until the first real OSM map arrives.
    for (int y = 0; y < SCREEN; ++y) {
        for (int x = 0; x < SCREEN; ++x) {
            const int dx = x - SCREEN / 2, dy = y - SCREEN / 2;
            const uint32_t radius2 = dx * dx + dy * dy;
            const uint8_t blue = radius2 < 29000 ? 8 : 3;
            const uint8_t green = radius2 < 29000 ? 9 : 5;
            screenRGB[y * SCREEN + x] = (uint16_t(blue) << 11) |
                    (uint16_t(green) << 5) | uint16_t(10);
        }
    }
}

static uint32_t lvTick() { return millis(); }
static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels) {
    amoled.drawArea(area->x1, area->y1, area->x2, area->y2, (uint16_t *)pixels);
    lv_display_flush_ready(disp);
}
static void alignInvalidArea(lv_event_t *e) {
    lv_area_t *area = static_cast<lv_area_t *>(lv_event_get_param(e));
    if (!area) return;
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
    if (area->x2 >= SCREEN) area->x2 = SCREEN - 1;
    if (area->y2 >= SCREEN) area->y2 = SCREEN - 1;
}

static void touchRead(lv_indev_t *dev, lv_indev_data_t *data) {
    static lv_point_t lastPoint = {SCREEN/2, SCREEN/2};
    uint16_t x = 0, y = 0;
    const bool pressed = getTouch(&x, &y);
    if (pressed) {
        lastPoint.x = x;
        lastPoint.y = y;
        data->point = lastPoint;
        data->state = LV_INDEV_STATE_PRESSED;
        if (!lastTouchDown) {
            touchBeginX = x;
            touchBeginY = y;
            touchBeganAt = millis();
        }
    } else {
        data->point = lastPoint;
        data->state = LV_INDEV_STATE_RELEASED;
        if (lastTouchDown) {
            const int dx = (int)lastPoint.x - touchBeginX;
            const int dy = (int)lastPoint.y - touchBeginY;
            const uint32_t held = millis() - touchBeganAt;
            if (abs(dx) > 65 && abs(dx) > abs(dy) * 3 / 2)
                pendingGesture = dx < 0 ? +1 : -1;
            else if (abs(dx) < 30 && abs(dy) < 30 && held < 450)
                pendingGesture = touchBeginY < 240 ? 2 : 3;
        }
    }
    lastTouchDown = pressed;
    (void)dev;
}

static lv_obj_t *makePanel(lv_obj_t *parent, int x, int y, int w, int h,
                          int radius, uint8_t opacity) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(box, lv_color_hex(CARD_BG), 0);
    lv_obj_set_style_bg_opa(box, opacity, 0);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xB6C9D8), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_opa(box, 46, 0);
    return box;
}
static lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, int x, int y,
                           int width, const lv_font_t *font, uint32_t color,
                           lv_text_align_t alignment = LV_TEXT_ALIGN_LEFT) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, alignment, 0);
    return label;
}
static void setShown(lv_obj_t *object, bool show) {
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
static void initArrow(ArrowGlyph &a, lv_obj_t *parent, int x, int y,
                      int diameter, int stroke) {
    a.size = diameter;
    a.shaft = lv_line_create(parent);
    a.head = lv_line_create(parent);
    lv_obj_set_pos(a.shaft, x, y);
    lv_obj_set_pos(a.head, x, y);
    lv_obj_t *lines[] = {a.shaft, a.head};
    for (lv_obj_t *line : lines) {
        lv_obj_set_style_line_color(line, lv_color_hex(CYAN), 0);
        lv_obj_set_style_line_width(line, stroke, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
    }
}
static void point(lv_point_precise_t &pt, int size, int x, int y) {
    pt.x = (size * x) / 100;
    pt.y = (size * y) / 100;
}
static void updateArrow(ArrowGlyph &a, uint8_t kind) {
    // All points are inside a normalized 100x100 bounding box.
    int shaft[8] = {50,78,50,42,50,22,50,22};
    int head[6] = {30,42,50,20,70,42};
    int count = 2;
    if (kind == 0 || kind == 2 || kind == 4 || kind == 12) {
        shaft[0]=74; shaft[1]=77; shaft[2]=74; shaft[3]=48;
        shaft[4]=25; shaft[5]=48; count=3;
        head[0]=44; head[1]=29; head[2]=22; head[3]=48;
        head[4]=44; head[5]=67;
    } else if (kind == 1 || kind == 3 || kind == 5 || kind == 13) {
        shaft[0]=26; shaft[1]=77; shaft[2]=26; shaft[3]=48;
        shaft[4]=75; shaft[5]=48; count=3;
        head[0]=56; head[1]=29; head[2]=78; head[3]=48;
        head[4]=56; head[5]=67;
    } else if (kind == 9) {
        shaft[0]=75; shaft[1]=80; shaft[2]=75; shaft[3]=36;
        shaft[4]=27; shaft[5]=36; shaft[6]=27; shaft[7]=65; count=4;
        head[0]=10; head[1]=49; head[2]=27; head[3]=67;
        head[4]=44; head[5]=49;
    }
    for (int i = 0; i < count; ++i)
        point(a.shaftPoints[i], a.size, shaft[i*2], shaft[i*2+1]);
    for (int i = 0; i < 3; ++i)
        point(a.headPoints[i], a.size, head[i*2], head[i*2+1]);
    lv_line_set_points(a.shaft, a.shaftPoints, count);
    lv_line_set_points(a.head, a.headPoints, 3);
}

static void makeUI() {
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(BG), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    mapCanvas = lv_canvas_create(root);
    lv_canvas_set_buffer(mapCanvas, screenRGB, SCREEN, SCREEN, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(mapCanvas, 0, 0);

    // Status capsule rests near the upper edge of the round viewport.
    statusPill = makePanel(root, 139, 42, 188, 32, 16, 228);
    statusDot = lv_obj_create(statusPill);
    lv_obj_set_size(statusDot, 7, 7);
    lv_obj_set_pos(statusDot, 15, 12);
    lv_obj_set_style_bg_color(statusDot, lv_color_hex(0xFAAE65), 0);
    lv_obj_set_style_bg_opa(statusDot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(statusDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(statusDot, 0, 0);
    statusLabel = makeLabel(statusPill, "WAITING FOR IPHONE", 29, 9, 149,
                            &lv_font_montserrat_12, WHITE);

    navCard = makePanel(root, 63, 87, 340, 94, 25, 223);
    initArrow(smallArrow, navCard, 13, 12, 67, 8);
    navAction = makeLabel(navCard, "NO ROUTE", 93, 14, 222,
                          &lv_font_montserrat_14, MUTED);
    navDistance = makeLabel(navCard, "Ready to ride", 91, 34, 236,
                            &lv_font_montserrat_28, WHITE);

    bottomCard = makePanel(root, 90, 345, 286, 65, 27, 225);
    makeLabel(bottomCard, "SPEED", 17, 10, 110, &lv_font_montserrat_10, MUTED);
    mapSpeed = makeLabel(bottomCard, "0.0", 17, 25, 122, &lv_font_montserrat_28, WHITE);
    lv_obj_t *separator = lv_obj_create(bottomCard);
    lv_obj_set_pos(separator, 143, 13);
    lv_obj_set_size(separator, 1, 38);
    lv_obj_set_style_bg_color(separator, lv_color_hex(0x647A8D), 0);
    lv_obj_set_style_bg_opa(separator, 130, 0);
    lv_obj_set_style_border_width(separator, 0, 0);
    makeLabel(bottomCard, "TO GO", 157, 10, 113, &lv_font_montserrat_10, MUTED);
    mapRemaining = makeLabel(bottomCard, "--", 157, 28, 122,
                              &lv_font_montserrat_20, WHITE);
    mapHint = makeLabel(root, "SWIPE FOR DETAILS", 129, 421, 208,
                        &lv_font_montserrat_10, 0xDAE3ED, LV_TEXT_ALIGN_CENTER);
    credit = makeLabel(root, "(c) OpenStreetMap contributors", 125, 435, 216,
                       &lv_font_montserrat_10, 0xD7E4EF, LV_TEXT_ALIGN_CENTER);

    // Turn page: almost-black glass over the map; large directional symbol.
    turnScreen = makePanel(root, 0, 0, SCREEN, SCREEN, SCREEN / 2, 241);
    lv_obj_set_style_border_width(turnScreen, 0, 0);
    turnTitle = makeLabel(turnScreen, "NEXT TURN", 112, 83, 242,
                         &lv_font_montserrat_20, MUTED, LV_TEXT_ALIGN_CENTER);
    initArrow(bigArrow, turnScreen, 166, 134, 134, 14);
    turnDistance = makeLabel(turnScreen, "--", 48, 281, 370,
                             &lv_font_montserrat_48, WHITE, LV_TEXT_ALIGN_CENTER);
    turnAction = makeLabel(turnScreen, "Connect your iPhone", 52, 345, 362,
                           &lv_font_montserrat_20, CYAN, LV_TEXT_ALIGN_CENTER);
    turnHint = makeLabel(turnScreen, "SWIPE FOR RIDE DATA", 101, 422, 264,
                         &lv_font_montserrat_12, MUTED, LV_TEXT_ALIGN_CENTER);

    // Ride page: typography first, fewer visual distractions.
    rideScreen = makePanel(root, 0, 0, SCREEN, SCREEN, SCREEN / 2, 249);
    lv_obj_set_style_border_width(rideScreen, 0, 0);
    makeLabel(rideScreen, "YOUR RIDE", 107, 87, 252,
              &lv_font_montserrat_20, MUTED, LV_TEXT_ALIGN_CENTER);
    rideSpeed = makeLabel(rideScreen, "0.0", 52, 144, 362,
                          &lv_font_montserrat_48, WHITE, LV_TEXT_ALIGN_CENTER);
    makeLabel(rideScreen, "KM/H", 137, 212, 192,
              &lv_font_montserrat_14, CYAN, LV_TEXT_ALIGN_CENTER);
    makeLabel(rideScreen, "REMAINING", 92, 284, 138,
              &lv_font_montserrat_12, MUTED, LV_TEXT_ALIGN_CENTER);
    rideRemaining = makeLabel(rideScreen, "--", 83, 308, 151,
                              &lv_font_montserrat_28, WHITE, LV_TEXT_ALIGN_CENTER);
    makeLabel(rideScreen, "HEADING", 235, 284, 138,
              &lv_font_montserrat_12, MUTED, LV_TEXT_ALIGN_CENTER);
    rideHeading = makeLabel(rideScreen, "--", 235, 308, 138,
                            &lv_font_montserrat_28, WHITE, LV_TEXT_ALIGN_CENTER);
    makeLabel(rideScreen, "SWIPE TO RETURN TO MAP", 95, 422, 276,
              &lv_font_montserrat_12, MUTED, LV_TEXT_ALIGN_CENTER);

    updateArrow(smallArrow, 6);
    updateArrow(bigArrow, 6);
    setShown(turnScreen, false);
    setShown(rideScreen, false);
}

static void displayPage(Page newPage) {
    page = newPage;
    const bool showMap = page == Page::MAP;
    setShown(statusPill, showMap);
    setShown(navCard, showMap);
    setShown(bottomCard, showMap);
    setShown(mapHint, showMap);
    setShown(credit, showMap);
    setShown(turnScreen, page == Page::TURN);
    setShown(rideScreen, page == Page::RIDE);
    Serial.printf("UI: page = %s\n", page == Page::MAP ? "MAP" : page == Page::TURN ? "TURN" : "RIDE");
}
static void handleGesture() {
    const int g = pendingGesture;
    pendingGesture = 0;
    if (!g) return;
    if (g == +1) displayPage(static_cast<Page>((static_cast<int>(page) + 1) % 3));
    else if (g == -1) displayPage(static_cast<Page>((static_cast<int>(page) + 2) % 3));
    else if (g == 2) displayPage(page == Page::MAP ? Page::TURN : Page::MAP);
    else if (g == 3) displayPage(page == Page::RIDE ? Page::MAP : Page::RIDE);
}
static void formatMeters(char *dst, size_t capacity, uint32_t meters) {
    if (meters < 1000) snprintf(dst, capacity, "%u m", (unsigned)meters);
    else snprintf(dst, capacity, "%.1f km", meters / 1000.0f);
}
static const char *headingText(uint16_t angle) {
    static const char* directions[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return directions[((angle + 22) % 360) / 45];
}
static void paintNav() {
    char dist[30], remaining[30], speed[24];
    formatMeters(dist, sizeof(dist), currentNav.turnM < 0 ? 0U : (uint32_t)currentNav.turnM);
    formatMeters(remaining, sizeof(remaining), currentNav.remainingM);
    snprintf(speed, sizeof(speed), "%.1f", currentNav.speedKmh);

    lv_label_set_text(navAction, currentNav.instruction);
    lv_label_set_text(navDistance, dist);
    lv_label_set_text(mapSpeed, speed);
    lv_label_set_text(mapRemaining, remaining);
    lv_label_set_text(turnDistance, dist);
    lv_label_set_text(turnAction, currentNav.instruction);
    lv_label_set_text(rideSpeed, speed);
    lv_label_set_text(rideRemaining, remaining);
    lv_label_set_text(rideHeading, headingText(currentNav.heading));
    updateArrow(smallArrow, currentNav.kind);
    updateArrow(bigArrow, currentNav.kind);
}
static void updateStatus() {
    static int lastStatus = -1;
    bool received;
    uint32_t lastUpdate;
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    received = everReceivedNav;
    lastUpdate = lastNavAt;
    xSemaphoreGive(rxMutex);
    const bool lost = received && (uint32_t)(millis() - lastUpdate) > LOST_AFTER_MS;
    navLost = lost;
    const int status = lost ? 2 : received ? 1 : 0;
    if (status == lastStatus) return;
    lastStatus = status;
    if (lost) {
        lv_label_set_text(statusLabel, "SIGNAL LOST");
        lv_obj_set_style_bg_color(statusDot, lv_color_hex(0xFF9B74), 0);
    } else if (received) {
        lv_label_set_text(statusLabel, "NAVIGATION LIVE");
        lv_obj_set_style_bg_color(statusDot, lv_color_hex(CYAN), 0);
    } else {
        lv_label_set_text(statusLabel, "WAITING FOR IPHONE");
        lv_obj_set_style_bg_color(statusDot, lv_color_hex(0xFAAE65), 0);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1600);
    Serial.println("\n==== BIKEMININAV / CIRCULAR UI BOOT ====");

    rxMutex = xSemaphoreCreateMutex();
    if (!rxMutex) {
        Serial.println("FATAL: mutex allocation");
        while (true) delay(1000);
    }
    // BLE first: the board remains discoverable even if AMOLED init fails.
    startBLE();

    rxJPEG = static_cast<uint8_t *>(heap_caps_malloc(MAX_JPG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    pendingJPEG = static_cast<uint8_t *>(heap_caps_malloc(MAX_JPG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    decodedRGB = static_cast<uint16_t *>(heap_caps_malloc(MAX_MAP_SIZE * MAX_MAP_SIZE * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    screenRGB = static_cast<uint16_t *>(heap_caps_malloc(SCREEN * SCREEN * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    Serial.printf("PSRAM free after map allocation: %u\n", (unsigned)ESP.getFreePsram());
    if (!rxJPEG || !pendingJPEG || !decodedRGB || !screenRGB) {
        Serial.println("ERROR: Map allocation failed. Verify OPI PSRAM configuration.");
        return;
    }
    fillWelcomeBackground();

    Serial.println("DISPLAY: initializing touch");
    Touch_Init();
    Serial.println("DISPLAY: initializing AMOLED");
    if (!amoled.begin()) {
        Serial.println("DISPLAY: AMOLED begin() failed (BLE remains active)");
        return;
    }

    lv_init();
    lv_tick_set_cb(lvTick);
    display = lv_display_create(SCREEN, SCREEN);
    if (!display) {
        Serial.println("DISPLAY: LVGL display failed");
        return;
    }
    constexpr size_t BUFFER_BYTES = SCREEN * 40 * 2;
    auto *bufA = static_cast<uint8_t *>(heap_caps_malloc(BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *bufB = static_cast<uint8_t *>(heap_caps_malloc(BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!bufA || !bufB) {
        Serial.println("DISPLAY: LVGL draw buffer allocation failed");
        return;
    }
    lv_display_set_flush_cb(display, flush);
    lv_display_set_buffers(display, bufA, bufB, BUFFER_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(display, alignInvalidArea, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touchRead);

    makeUI();
    displayPage(Page::MAP);
    displayReady = true;
    Serial.println("DISPLAY: READY | Swipe to switch screens");
    Serial.println("BLE: advertising BikeMiniNav / waiting for iPhone");
}

void loop() {
    static uint32_t logAt = 0;
    if ((uint32_t)(millis() - logAt) > 3000) {
        logAt = millis();
        uint32_t ok, rejected;
        xSemaphoreTake(rxMutex, portMAX_DELAY);
        ok = framesOK;
        rejected = framesRejected;
        xSemaphoreGive(rxMutex);
        Serial.printf("ESP32 ALIVE | BLE=%s | display=%s | map=%lu | rejected=%lu\n",
                      bleStarted ? "ON" : "OFF", displayReady ? "READY" : "FAILED",
                      (unsigned long)ok, (unsigned long)rejected);
    }
    if (!displayReady) { delay(25); return; }

    bool hasImage = false, hasNav = false;
    size_t jpegLength = 0;
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    if (imageReady) {
        hasImage = true;
        jpegLength = pendingLen;
        imageReady = false;
        decoding = true;
    }
    if (navDirty) {
        currentNav = incomingNav;
        hasNav = true;
        navDirty = false;
    }
    xSemaphoreGive(rxMutex);

    if (hasImage) {
        static JPEGDEC decoder;
        if (decoder.openRAM(pendingJPEG, static_cast<int>(jpegLength), jpegDraw)) {
            decoder.setPixelType(RGB565_LITTLE_ENDIAN);
            const int imageWidth = decoder.getWidth();
            if (imageWidth >= MIN_MAP_SIZE && imageWidth <= MAX_MAP_SIZE &&
                decoder.getHeight() == imageWidth) {
                decodedMapSize = imageWidth;
                if (decoder.decode(0, 0, 0)) {
                    expandMapToRoundScreen(decodedMapSize);
                    lv_obj_invalidate(mapCanvas);
                    Serial.printf("MAP: displayed %u bytes\n", (unsigned)jpegLength);
                } else Serial.println("MAP: JPEG decode failed");
            } else Serial.println("MAP: unexpected JPEG size (expected 288 or 384 square)");
            decoder.close();
        } else Serial.println("MAP: cannot open JPEG");
        xSemaphoreTake(rxMutex, portMAX_DELAY);
        decoding = false;
        xSemaphoreGive(rxMutex);
    }
    if (hasNav) {
        paintNav();
        updateStatus();
    }
    static uint32_t lastStatusAt = 0;
    if ((uint32_t)(millis() - lastStatusAt) > 1000) {
        lastStatusAt = millis();
        updateStatus();
    }
    lv_timer_handler();
    handleGesture();
    delay(5);
}
