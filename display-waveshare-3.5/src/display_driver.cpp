#include "display_driver.h"
#include "TCA9554.h"
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;
static XPowersPMU power;

static bool touch_online = false;

// Timestamp of the last touch the AXS15231B panel reported.  The screen saver
// uses it to decide when the display has been idle for long enough.
volatile uint32_t g_last_touch_ms = 0;
volatile bool g_touch_down = false;

#define BUF_PIXELS (SCREEN_WIDTH * SCREEN_HEIGHT) // 480 * 320 = 153,600

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_draw_buf1 = nullptr;
static lv_color_t *disp_draw_buf2 = nullptr;
static lv_color_t *rotated_fb = nullptr;

static uint32_t flush_count = 0;

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    if (gfx != nullptr && rotated_fb != nullptr && area != nullptr) {
        // Cache-friendly tiled 90-degree clockwise rotation of the dirty sub-area:
        // Only the invalidated region [area->x1..x2, area->y1..y2] is rotated into
        // the persistent PSRAM framebuffer in 16x16 tiles so both source reads and
        // destination writes stay within ESP32-S3 DCache lines.
        const int32_t x1 = area->x1;
        const int32_t y1 = area->y1;
        const int32_t x2 = area->x2;
        const int32_t y2 = area->y2;
        const int32_t src_stride = x2 - x1 + 1;
        constexpr int32_t TILE = 16;

        for (int32_t ty = y1; ty <= y2; ty += TILE) {
            const int32_t ly_end = (ty + TILE - 1 < y2) ? (ty + TILE - 1) : y2;
            for (int32_t tx = x1; tx <= x2; tx += TILE) {
                const int32_t lx_end = (tx + TILE - 1 < x2) ? (tx + TILE - 1) : x2;
                for (int32_t lx = tx; lx <= lx_end; ++lx) {
                    lv_color_t *dst_row = &rotated_fb[lx * 320];
                    for (int32_t ly = ty; ly <= ly_end; ++ly) {
                        dst_row[319 - ly] = color_p[(ly - y1) * src_stride + (lx - x1)];
                    }
                }
            }
        }

        if (lv_disp_flush_is_last(disp)) {
#if (LV_COLOR_16_SWAP != 0)
            gfx->draw16bitBeRGBBitmap(0, 0, (uint16_t *)rotated_fb, 320, 480);
#else
            gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)rotated_fb, 320, 480);
#endif
        }
    }

    flush_count++;
    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    if (!touch_online) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    bsp_touch_read();
    touch_data_t touch_data;
    if (bsp_touch_get_coordinates(&touch_data)) {
        data->state = LV_INDEV_STATE_PR;
        g_last_touch_ms = millis();
        g_touch_down = true;
        // Native touch: coords[0].x in 0..319, coords[0].y in 0..479
        int16_t lx = touch_data.coords[0].y;
        int16_t ly = 319 - touch_data.coords[0].x;
        if (lx < 0) lx = 0;
        if (lx >= SCREEN_WIDTH) lx = SCREEN_WIDTH - 1;
        if (ly < 0) ly = 0;
        if (ly >= SCREEN_HEIGHT) ly = SCREEN_HEIGHT - 1;
        data->point.x = lx;
        data->point.y = ly;
    } else {
        data->state = LV_INDEV_STATE_REL;
        g_touch_down = false;
    }
}

void initDisplayAndTouch() {
    Serial.println("[Display] Initializing I2C bus (400kHz Fast-mode)...");
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(400000);
    delay(30);

    // Initialize AXP2101 PMIC at 0x34 to energize peripheral power rails
    if (power.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA_PIN, I2C_SCL_PIN)) {
        Serial.printf("[PMU] AXP2101 detected (Chip ID: 0x%02X). Enabling power rails...\n", power.getChipID());
        power.setALDO1Voltage(3300);
        power.setALDO2Voltage(3300);
        power.setALDO3Voltage(3300);
        power.setALDO4Voltage(3300);
        power.setBLDO1Voltage(1500);
        power.setBLDO2Voltage(2800);
        power.setCPUSLDOVoltage(1000);
        power.setDLDO1Voltage(3300);
        power.setDLDO2Voltage(3300);

        power.setDC1Voltage(3300);
        power.setDC2Voltage(1000);
        power.setDC3Voltage(3300);
        power.setDC4Voltage(1000);
        power.setDC5Voltage(3300);

        power.enableDC2();
        power.enableDC3();
        power.enableDC4();
        power.enableDC5();

        power.enableALDO1();
        power.enableALDO2();
        power.enableALDO3();
        power.enableALDO4();
        power.enableBLDO1();
        power.enableBLDO2();
        power.enableCPUSLDO();
        power.enableDLDO1();
        power.enableDLDO2();
        Serial.println("[PMU] Power rails successfully configured.");
        delay(50);
    } else {
        Serial.println("[PMU] AXP2101 PMIC not detected at 0x34.");
    }

    // Initialize TCA9554 IO Expander at 0x20
    TCA9554 tca(TCA9554_I2C_ADDR, &Wire);
    if (tca.begin()) {
        tca.pinMode1(0, OUTPUT); // Camera PWDN
        tca.pinMode1(1, OUTPUT); // LCD Reset

        // Hardware Reset sequence: High -> Low -> High (Waveshare official sequence)
        tca.write1(1, 1);
        delay(10);
        tca.write1(1, 0); // Assert reset LOW
        delay(50);
        tca.write1(1, 1); // Release reset HIGH
        delay(200);       // Wait 200ms for AXS15231B to fully wake
        Serial.println("[TCA9554] LCD hardware reset released.");
    } else {
        Serial.println("[Display] Error: TCA9554 at 0x20 NOT communicating!");
    }

    // Keep backlight OFF while initializing registers
    pinMode(LCD_BL_PIN, OUTPUT);
    digitalWrite(LCD_BL_PIN, LOW);

    // Initialize Arduino_ESP32QSPI data bus
    Serial.println("[Display] Initializing Arduino_ESP32QSPI data bus...");
    bus = new Arduino_ESP32QSPI(
        LCD_QSPI_CS_PIN,
        LCD_QSPI_CLK_PIN,
        LCD_QSPI_D0_PIN,
        LCD_QSPI_D1_PIN,
        LCD_QSPI_D2_PIN,
        LCD_QSPI_D3_PIN
    );

    // Native portrait configuration: 320 width x 480 height, rotation 0
    // KEEP ROTATION 0 NATIVELY TO PREVENT AXS15231B CONTROLLER MEMORY CORRUPTION
    Serial.println("[Display] Initializing Arduino_AXS15231B driver (Native 320x480, Rotation 0)...");
    gfx = new Arduino_AXS15231B(
        bus,
        -1,           // RST handled via TCA9554
        0,            // Native portrait rotation (0 = 320x480)
        false,        // IPS flag
        320,          // Width in portrait
        480           // Height in portrait
    );

    if (!gfx->begin()) {
        Serial.println("[Display] Error: gfx->begin() failed completely!");
    } else {
        Serial.println("[Display] AXS15231B QSPI display initialized successfully.");
    }

    // Clear display to pure black in native rotation 0
    gfx->fillScreen(RGB565_BLACK);

    // Turn ON backlight now that screen is cleanly initialized
    digitalWrite(LCD_BL_PIN, HIGH);
    Serial.println("[Display] Backlight turned ON.");

    // Initialize AXS15231B touch controller (native 320x480)
    bsp_touch_init(&Wire, -1, 0, 320, 480);
    touch_online = true;
    Serial.println("[Touch] AXS15231B touch controller initialized.");

    // Initialize LVGL
    lv_init();

    // Allocate framebuffers in PSRAM:
    // 2x 480x320 for LVGL draw buffer (300 KB each)
    // 1x 320x480 for rotated output buffer (300 KB)
    disp_draw_buf1 = (lv_color_t *)heap_caps_malloc(BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rotated_fb     = (lv_color_t *)heap_caps_malloc(BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!disp_draw_buf1 || !disp_draw_buf2 || !rotated_fb) {
        Serial.println("[Display] Error: Failed to allocate framebuffers in PSRAM!");
    } else {
        Serial.println("[Display] Successfully allocated 3x 300 KB buffers in PSRAM.");
    }

    lv_disp_draw_buf_init(&draw_buf, disp_draw_buf1, disp_draw_buf2, BUF_PIXELS);

    // Register Display Driver with 480x320 landscape logical resolution
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;  // 480
    disp_drv.ver_res = SCREEN_HEIGHT; // 320
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = false;
    lv_disp_drv_register(&disp_drv);

    // Register Touch Input Driver
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    Serial.println("[Display] Waveshare 3.5B LCD & Touch ready with LVGL (480x320).");
}
