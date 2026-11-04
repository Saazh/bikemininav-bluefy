// BikeMiniNav Linux/Bluefy - Waveshare ESP32-S3 AMOLED 1.43, LVGL 9, NimBLE.
// Protocol is shared with web/protocol.mjs: BLE opcode 0x05 fits a 20-byte write.
// Driver source files are fetched from the upstream Waveshare LVGL9 demo
// by `python tools/bootstrap_drivers.py` before the first PlatformIO build.
// The GATT characteristic is deliberately write-only: nearby devices can
// spoof navigation in this prototype. Add pairing/bonding for field use.

#include <Arduino.h>
#define LV_CONF_INCLUDE_SIMPLE
#include <lvgl.h>
#include <JPEGDEC.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include "amoled.h"
#include "FT3168.h"

static constexpr int SCREEN = 466;
static constexpr int MAP = 288;
static constexpr int MAP_X = (SCREEN - MAP) / 2;
static constexpr int MAP_Y = 90;
static constexpr size_t MAX_JPG = 90000;
static constexpr char SERVICE_UUID[] = "7E9D1000-2A39-4B25-B3E4-CB7D1FC2AC10";
static constexpr char WRITE_UUID[] = "7E9D1001-2A39-4B25-B3E4-CB7D1FC2AC10";

Amoled amoled;
static lv_display_t *lvDisp = nullptr;
static lv_obj_t *mapCanvas, *topLabel, *turnLabel, *speedLabel, *creditLabel;
static uint8_t *mapRGB = nullptr;
static uint8_t *rxJPEG = nullptr;
static uint8_t *pendingJPEG = nullptr;
static size_t rxCount = 0, expectedBytes = 0, completedLen = 0;
static uint32_t expectedCRC = 0;
static bool receiving = false, imageReady = false, decoding = false, navDirty = false;
static SemaphoreHandle_t lock;
static char navInstruction[65] = "Connect iPhone";
static int turnMeters = 0, remainMeters = 0;
static float speedKmh = 0;
static uint32_t lastNavMs = 0;
static bool navEverReceived = false;
static bool signalLostShown = false;
static constexpr const char* TURN_NAMES[] = {
    "Left", "Right", "Sharp left", "Sharp right", "Slight left",
    "Slight right", "Straight", "Roundabout", "Exit roundabout",
    "U-turn", "Destination", "Start", "Keep left", "Keep right"
};

static uint32_t clockMs() { return millis(); }

static uint32_t crc32(const uint8_t *data, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= data[i];
        for (int j = 0; j < 8; ++j) {
            c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~c;
}

static uint32_t u32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Called by the NimBLE host task. NEVER invoke LVGL from this thread.
class PacketReceiver : public NimBLECharacteristicCallbacks {
    uint8_t onWrite(NimBLECharacteristic *characteristic, NimBLEAttValue &value,
                    NimBLEConnInfo &connInfo) override {
        (void)characteristic;
        (void)connInfo;
        const uint8_t *p = value.data();
        const size_t len = value.size();
        if (!p || !len) return 0;
        xSemaphoreTake(lock, portMAX_DELAY);
        switch (p[0]) {
            case 0x01: {  // Navigation JSON; no image interleaving
                JsonDocument json;
                DeserializationError error = deserializeJson(json, p + 1, len - 1);
                if (!error) {
                    turnMeters = json["turn_m"] | 0;
                    remainMeters = json["remain_m"] | 0;
                    speedKmh = json["speed"] | 0.0f;
                    const char *s = json["instruction"] | "Follow route";
                    snprintf(navInstruction, sizeof(navInstruction), "%s", s);
                    navDirty = true;
                    navEverReceived = true;
                    lastNavMs = millis();
                    signalLostShown = false;
                }
                break;
            }
            case 0x05: { // Compact binary navigation (12 bytes; <= 20-byte GATT write)
                if (len != 12) break;
                const uint8_t kind = p[1];
                turnMeters = uint16_t(p[2]) | (uint16_t(p[3]) << 8);
                remainMeters = static_cast<int>(u32le(p + 4));
                speedKmh = (uint16_t(p[8]) | (uint16_t(p[9]) << 8)) / 10.0f;
                const uint16_t heading = uint16_t(p[10]) | (uint16_t(p[11]) << 8);
                (void)heading; // The map is north-up, so the heading arrow is drawn on the phone.
                snprintf(navInstruction, sizeof(navInstruction), "%s",
                         kind < sizeof(TURN_NAMES)/sizeof(TURN_NAMES[0]) ? TURN_NAMES[kind] : "Follow route");
                navDirty = true;
                navEverReceived = true;
                lastNavMs = millis();
                signalLostShown = false;
                break;
            }
            case 0x02: {  // Start JPEG: length + CRC32
                receiving = false;
                rxCount = 0;
                if (len != 9) break;
                expectedBytes = u32le(p + 1);
                expectedCRC = u32le(p + 5);
                if (expectedBytes > 0 && expectedBytes <= MAX_JPG) receiving = true;
                break;
            }
            case 0x03: {  // JPEG fragment
                if (!receiving || (rxCount + len - 1) > expectedBytes) {
                    receiving = false;
                    break;
                }
                memcpy(rxJPEG + rxCount, p + 1, len - 1);
                rxCount += len - 1;
                break;
            }
            case 0x04: {  // End JPEG; validate before showing it
                if (receiving && rxCount == expectedBytes &&
                    crc32(rxJPEG, rxCount) == expectedCRC && !decoding) {
                    uint8_t *tmp = rxJPEG;
                    rxJPEG = pendingJPEG;
                    pendingJPEG = tmp;
                    completedLen = rxCount;
                    imageReady = true;
                }
                receiving = false;
                break;
            }
            default: break;
        }
        xSemaphoreGive(lock);
        return 0;  // NimBLEGattStatus::Success
    }
};
static PacketReceiver receiver;

static void initBLE() {
    NimBLEDevice::init("BikeMiniNav");
    NimBLEDevice::setMTU(247);
    auto server = NimBLEDevice::createServer();
    server->advertiseOnDisconnect(true);
    auto service = server->createService(SERVICE_UUID);
    auto characteristic = service->createCharacteristic(WRITE_UUID, NIMBLE_PROPERTY::WRITE, 260);
    characteristic->setCallbacks(&receiver);
    service->start();
    auto ad = NimBLEDevice::getAdvertising();
    ad->addServiceUUID(SERVICE_UUID);
    ad->setName("BikeMiniNav");
    ad->start();
    Serial.println("BLE: advertising BikeMiniNav");
}

static int jpegDraw(JPEGDRAW *block) {
    if (block->x < 0 || block->y < 0 || block->x + block->iWidth > MAP ||
        block->y + block->iHeight > MAP) return 0;
    for (int y = 0; y < block->iHeight; ++y) {
        memcpy(mapRGB + ((block->y + y)*MAP + block->x)*2,
               block->pPixels + y*block->iWidth, block->iWidth*2);
    }
    return 1;
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels) {
    amoled.drawArea(area->x1, area->y1, area->x2, area->y2, (uint16_t *)pixels);
    lv_display_flush_ready(disp);
}

static void roundArea(lv_event_t *e) {
    auto *area = static_cast<lv_area_t *>(lv_event_get_param(e));
    if (!area) return;
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
    area->x2 = min(area->x2, SCREEN - 1);
    area->y2 = min(area->y2, SCREEN - 1);
}

static void readTouch(lv_indev_t *, lv_indev_data_t *data) {
    uint16_t x = 0, y = 0;
    if (getTouch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else data->state = LV_INDEV_STATE_RELEASED;
}

static lv_obj_t *makeLabel(const char *message, int y, int width, uint32_t color) {
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, message);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void initUI() {
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(0x08121D), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    topLabel = makeLabel("BIKE MINI NAV", 53, 225, 0x55DCC7);
    mapCanvas = lv_canvas_create(root);
    lv_canvas_set_buffer(mapCanvas, mapRGB, MAP, MAP, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(mapCanvas, lv_color_hex(0x1F3041), LV_OPA_COVER);
    lv_obj_set_pos(mapCanvas, MAP_X, MAP_Y);
    lv_obj_set_style_radius(mapCanvas, 17, 0);
    lv_obj_set_style_clip_corner(mapCanvas, true, 0);
    turnLabel = makeLabel("Waiting for iPhone", 389, 325, 0xFFFFFF);
    speedLabel = makeLabel("Connect using Bluetooth", 410, 310, 0xA6BCCB);
    creditLabel = makeLabel("(c) OpenStreetMap contributors", 432, 235, 0x8EA6B2);
    lv_obj_set_style_text_font(creditLabel, &lv_font_montserrat_10, 0);
    // Physical round display clips outside edges naturally.
}

void setup() {
    Serial.begin(115200);
    delay(600);
    lock = xSemaphoreCreateMutex();
    rxJPEG = static_cast<uint8_t *>(heap_caps_malloc(MAX_JPG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    pendingJPEG = static_cast<uint8_t *>(heap_caps_malloc(MAX_JPG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    mapRGB = static_cast<uint8_t *>(heap_caps_malloc(MAP*MAP*2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!lock || !rxJPEG || !pendingJPEG || !mapRGB) {
        Serial.println("Out of PSRAM / mutex creation failed");
        while (true) delay(1000);
    }
    Touch_Init();
    if (!amoled.begin()) {
        Serial.println("AMOLED initialization failed. Check driver version.");
        while (true) delay(1000);
    }
    lv_init();
    lv_tick_set_cb(clockMs);
    lvDisp = lv_display_create(SCREEN, SCREEN);
    constexpr size_t bufferBytes = SCREEN * 48 * 2;
    auto *bufferA = static_cast<uint8_t *>(heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *bufferB = static_cast<uint8_t *>(heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!bufferA || !bufferB) {
        Serial.println("LVGL buffer allocation failed");
        while (true) delay(1000);
    }
    lv_display_set_flush_cb(lvDisp, flush);
    lv_display_set_buffers(lvDisp, bufferA, bufferB, bufferBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(lvDisp, roundArea, LV_EVENT_INVALIDATE_AREA, nullptr);
    auto *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, readTouch);
    initUI();
    initBLE();
    Serial.println("Ready for iPhone");
}

void loop() {
    static JPEGDEC decoder;
    bool show = false;
    size_t showLength = 0;
    bool updateNav = false;
    char instruction[65];
    int turn = 0, remaining = 0;
    float speed = 0;

    xSemaphoreTake(lock, portMAX_DELAY);
    if (imageReady) {
        show = true;
        showLength = completedLen;
        imageReady = false;
        decoding = true;
    }
    if (navDirty) {
        updateNav = true;
        navDirty = false;
        strcpy(instruction, navInstruction);
        turn = turnMeters;
        remaining = remainMeters;
        speed = speedKmh;
    }
    xSemaphoreGive(lock);

    if (show) {
        if (decoder.openRAM(pendingJPEG, static_cast<int>(showLength), jpegDraw)) {
            decoder.setPixelType(RGB565_LITTLE_ENDIAN);
            if (decoder.getWidth() == MAP && decoder.getHeight() == MAP) {
                decoder.decode(0, 0, 0);
                lv_obj_invalidate(mapCanvas);
            } else Serial.println("Unexpected JPEG dimensions");
            decoder.close();
        } else Serial.println("JPEG decode/open failed");
        xSemaphoreTake(lock, portMAX_DELAY);
        decoding = false;
        xSemaphoreGive(lock);
    }

    if (updateNav) {
        char line[100];
        // Keep the most important fields visible on the tiny round screen.
        snprintf(line, sizeof(line), "%s  %dm", instruction, turn);
        lv_label_set_text(turnLabel, line);
        snprintf(line, sizeof(line), "%.1f km/h   |   %.1f km left", speed, remaining / 1000.0f);
        lv_label_set_text(speedLabel, line);
    }
    // Don't silently show apparently fresh navigation after iPhone/Bluefy stalls.
    bool showStaleWarning = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (navEverReceived && !signalLostShown && (uint32_t)(millis() - lastNavMs) > 25000) {
        showStaleWarning = true;
        signalLostShown = true;
    }
    xSemaphoreGive(lock);
    if (showStaleWarning) {
        lv_label_set_text(turnLabel, "SIGNAL LOST - check iPhone");
        lv_label_set_text(speedLabel, "GPS / BLE updates stopped");
    }
    lv_timer_handler();
    delay(5);
}
