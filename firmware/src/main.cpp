// BikeMiniNav Linux/Bluefy - Waveshare ESP32-S3 AMOLED 1.43, LVGL 9, NimBLE.
#include <Arduino.h>
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
static bool displayReady = false, bleStarted = false;
static SemaphoreHandle_t lock;
static char navInstruction[65] = "Connect iPhone";
static int turnMeters = 0, remainMeters = 0;
static float speedKmh = 0;
static uint32_t lastNavMs = 0;
static bool navEverReceived = false;
static bool signalLostShown = false;
static constexpr const char *TURN_NAMES[] = {
    "Left", "Right", "Sharp left", "Sharp right", "Slight left",
    "Slight right", "Straight", "Roundabout", "Exit roundabout",
    "U-turn", "Destination", "Start", "Keep left", "Keep right"
};

static uint32_t clockMs() { return millis(); }

static uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

static uint32_t u32le(const uint8_t *data) {
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
           (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

class PacketReceiver : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
        (void)connInfo;
        const NimBLEAttValue value = characteristic->getValue();
        const uint8_t *data = value.data();
        const size_t length = value.size();
        if (!data || !length) return;

        xSemaphoreTake(lock, portMAX_DELAY);
        switch (data[0]) {
            case 0x01: {
                JsonDocument json;
                DeserializationError error = deserializeJson(json, data + 1, length - 1);
                if (!error) {
                    turnMeters = json["turn_m"] | 0;
                    remainMeters = json["remain_m"] | 0;
                    speedKmh = json["speed"] | 0.0f;
                    const char *instruction = json["instruction"] | "Follow route";
                    snprintf(navInstruction, sizeof(navInstruction), "%s", instruction);
                    navDirty = true;
                    navEverReceived = true;
                    lastNavMs = millis();
                    signalLostShown = false;
                }
                break;
            }
            case 0x05: {
                if (length != 12) break;
                const uint8_t kind = data[1];
                turnMeters = uint16_t(data[2]) | (uint16_t(data[3]) << 8);
                remainMeters = static_cast<int>(u32le(data + 4));
                speedKmh = (uint16_t(data[8]) | (uint16_t(data[9]) << 8)) / 10.0f;
                const uint16_t heading = uint16_t(data[10]) | (uint16_t(data[11]) << 8);
                (void)heading;
                snprintf(navInstruction, sizeof(navInstruction), "%s",
                         kind < sizeof(TURN_NAMES) / sizeof(TURN_NAMES[0]) ? TURN_NAMES[kind] : "Follow route");
                navDirty = true;
                navEverReceived = true;
                lastNavMs = millis();
                signalLostShown = false;
                break;
            }
            case 0x02: {
                receiving = false;
                rxCount = 0;
                if (length != 9) break;
                expectedBytes = u32le(data + 1);
                expectedCRC = u32le(data + 5);
                if (expectedBytes > 0 && expectedBytes <= MAX_JPG) receiving = true;
                break;
            }
            case 0x03:
                if (!receiving || (rxCount + length - 1) > expectedBytes) {
                    receiving = false;
                    break;
                }
                memcpy(rxJPEG + rxCount, data + 1, length - 1);
                rxCount += length - 1;
                break;
            case 0x04:
                if (receiving && rxCount == expectedBytes &&
                    crc32(rxJPEG, rxCount) == expectedCRC && !decoding) {
                    uint8_t *temporary = rxJPEG;
                    rxJPEG = pendingJPEG;
                    pendingJPEG = temporary;
                    completedLen = rxCount;
                    imageReady = true;
                }
                receiving = false;
                break;
            default:
                break;
        }
        xSemaphoreGive(lock);
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
    auto advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setName("BikeMiniNav");
    advertising->start();
    bleStarted = true;
    Serial.println("BLE: advertising BikeMiniNav");
}

static int jpegDraw(JPEGDRAW *block) {
    if (block->x < 0 || block->y < 0 || block->x + block->iWidth > MAP ||
        block->y + block->iHeight > MAP) return 0;
    for (int row = 0; row < block->iHeight; ++row) {
        memcpy(mapRGB + ((block->y + row) * MAP + block->x) * 2,
               block->pPixels + row * block->iWidth, block->iWidth * 2);
    }
    return 1;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    amoled.drawArea(area->x1, area->y1, area->x2, area->y2, (uint16_t *)pixels);
    lv_display_flush_ready(display);
}

static void roundArea(lv_event_t *e) {
    auto *area = static_cast<lv_area_t *>(lv_event_get_param(e));
    if (!area) return;
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
    if (area->x2 >= SCREEN) area->x2 = SCREEN - 1;
    if (area->y2 >= SCREEN) area->y2 = SCREEN - 1;
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
    delay(1500);  // Let USB CDC connect after reset.
    Serial.println("\n========== BIKEMININAV BOOT ==========");

    lock = xSemaphoreCreateMutex();
    rxJPEG = static_cast<uint8_t *>(heap_caps_malloc(MAX_JPG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    pendingJPEG = static_cast<uint8_t *>(heap_caps_malloc(MAX_JPG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    mapRGB = static_cast<uint8_t *>(heap_caps_malloc(MAP * MAP * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    Serial.printf("PSRAM free: %u bytes\n", (unsigned)ESP.getFreePsram());
    if (!lock || !rxJPEG || !pendingJPEG || !mapRGB) {
        Serial.println("FATAL: PSRAM / mutex allocation failed. Check OPI PSRAM settings.");
        while (true) { Serial.println("FATAL: no map buffers"); delay(2000); }
    }

    // Start BLE BEFORE touch/display initialization. A display fault should
    // not hide the device from Bluefy or prevent USB serial diagnostics.
    initBLE();

    Serial.println("DISPLAY: starting touch...");
    Touch_Init();
    Serial.println("DISPLAY: starting AMOLED...");
    if (!amoled.begin()) {
        Serial.println("DISPLAY ERROR: AMOLED init failed; BLE remains available.");
        // loop() will remain alive and print diagnostics, without calling LVGL.
        return;
    }

    Serial.println("LVGL: initializing...");
    lv_init();
    lv_tick_set_cb(clockMs);
    lvDisp = lv_display_create(SCREEN, SCREEN);
    if (!lvDisp) {
        Serial.println("DISPLAY ERROR: LVGL display allocation failed");
        return;
    }

    constexpr size_t bufferBytes = SCREEN * 48 * 2;
    auto *bufferA = static_cast<uint8_t *>(heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *bufferB = static_cast<uint8_t *>(heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!bufferA || !bufferB) {
        Serial.println("DISPLAY ERROR: LVGL buffers allocation failed");
        return;
    }
    lv_display_set_flush_cb(lvDisp, flush);
    lv_display_set_buffers(lvDisp, bufferA, bufferB, bufferBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(lvDisp, roundArea, LV_EVENT_INVALIDATE_AREA, nullptr);
    auto *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, readTouch);
    initUI();
    displayReady = true;
    Serial.println("DISPLAY: ready");
    Serial.println("BIKEMININAV: Ready for iPhone");
}

void loop() {
    // Repeated heartbeat even when early startup messages were missed.
    static uint32_t lastLog = 0;
    if ((uint32_t)(millis() - lastLog) >= 2000) {
        lastLog = millis();
        Serial.printf("ESP32 ALIVE | display=%s | BLE=%s\n",
                      displayReady ? "READY" : "NOT READY",
                      bleStarted ? "STARTED" : "FAILED");
    }
    if (!displayReady) { delay(20); return; }

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
