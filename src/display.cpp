#include "display.h"

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#include "config.h"
#include "power.h"

namespace display {
namespace {

constexpr int PIN_MOSI = 1;
constexpr int PIN_MISO = 2;
constexpr int PIN_SCLK = 5;
constexpr int PIN_DC = 3;

constexpr uint8_t TCA_ADDR = 0x20;
constexpr uint8_t TCA_REG_OUTPUT = 0x01;
constexpr uint8_t TCA_REG_CONFIG = 0x03;

constexpr uint8_t FT6336_REG_TD_STATUS = 0x02;

struct St7796InitCmd {
    uint8_t cmd;
    const uint8_t* data;
    uint8_t dataBytes;
    uint16_t delayMs;
};

#define ST7796_CMD(_cmd, _data, _delay) \
    {(_cmd), (const uint8_t[]){_data}, sizeof((uint8_t[]){_data}), (_delay)}
#define ST7796_CMD0(_cmd, _delay) \
    {(_cmd), nullptr, 0, (_delay)}

static const St7796InitCmd kSt7796Init[] = {
    ST7796_CMD0(0x11, 120),
    ST7796_CMD(0x3A, 0x05, 0),
    ST7796_CMD(0xF0, 0xC3, 0),
    ST7796_CMD(0xF0, 0x96, 0),
    ST7796_CMD(0xB4, 0x01, 0),
    ST7796_CMD(0xB7, 0xC6, 0),
    {0xC0, (const uint8_t[]){0x80, 0x45}, 2, 0},
    ST7796_CMD(0xC1, 0x13, 0),
    ST7796_CMD(0xC2, 0xA7, 0),
    ST7796_CMD(0xC5, 0x0A, 0),
    {0xE8, (const uint8_t[]){0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8, 0},
    {0xE0, (const uint8_t[]){0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33,
                              0x47, 0x17, 0x13, 0x13, 0x2B, 0x31},
     14, 0},
    {0xE1, (const uint8_t[]){0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33,
                              0x47, 0x38, 0x15, 0x16, 0x2C, 0x32},
     14, 0},
    ST7796_CMD(0xF0, 0x3C, 0),
    ST7796_CMD(0xF0, 0x69, 120),
    ST7796_CMD0(0x29, 0),
};

esp_lcd_panel_io_handle_t g_panelIo = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;

lv_disp_draw_buf_t drawBuffer;
lv_color_t* lvBuffer1 = nullptr;
lv_color_t* lvBuffer2 = nullptr;
lv_disp_drv_t dispDrv;
lv_indev_drv_t indevDrv;

bool tcaWriteReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(TCA_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

void tca9554Reset() {
    const uint8_t outMask =
        static_cast<uint8_t>((1u << cfg::EXIO_CAM_PWDN) | (1u << cfg::EXIO_LCD_RST) | (1u << cfg::EXIO_LCD_CS));
    tcaWriteReg(TCA_REG_CONFIG, static_cast<uint8_t>(~outMask));
    delay(100);
    tcaWriteReg(TCA_REG_OUTPUT, 0x00);
    delay(100);
    tcaWriteReg(TCA_REG_OUTPUT, static_cast<uint8_t>(1u << cfg::EXIO_LCD_RST));
    delay(10);
}

void sendSt7796Init(esp_lcd_panel_io_handle_t io) {
    for (const auto& entry : kSt7796Init) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, entry.cmd, entry.data, entry.dataBytes));
        if (entry.delayMs > 0) {
            delay(entry.delayMs);
        }
    }
}

void enableBacklight() {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = LEDC_TIMER_0;
    timer.duty_resolution = static_cast<ledc_timer_bit_t>(cfg::BL_LEDC_RESOLUTION_BITS);
    timer.freq_hz = cfg::BL_LEDC_FREQ_HZ;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {};
    channel.gpio_num = cfg::PIN_BL_PRIMARY;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = static_cast<ledc_channel_t>(cfg::BL_LEDC_CHANNEL);
    channel.timer_sel = LEDC_TIMER_0;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.duty = cfg::BL_BRIGHTNESS;
    channel.hpoint = 0;
    ledc_channel_config(&channel);
}

void applyBacklight(uint8_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(cfg::BL_LEDC_CHANNEL), duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(cfg::BL_LEDC_CHANNEL));
}

bool readTouch(int16_t* outX, int16_t* outY) {
    Wire.beginTransmission(cfg::FT6336_I2C_ADDR);
    Wire.write(FT6336_REG_TD_STATUS);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(static_cast<int>(cfg::FT6336_I2C_ADDR), 5) != 5) return false;

    const uint8_t touches = Wire.read() & 0x0F;
    if (touches == 0) return false;

    const uint8_t xh = Wire.read();
    const uint8_t xl = Wire.read();
    const uint8_t yh = Wire.read();
    const uint8_t yl = Wire.read();

    int16_t x = static_cast<int16_t>(((xh & 0x0F) << 8) | xl);
    int16_t y = static_cast<int16_t>(((yh & 0x0F) << 8) | yl);

    if (cfg::TOUCH_SWAP_XY) {
        const int16_t tmp = x;
        x = y;
        y = tmp;
    }
    if (cfg::TOUCH_MIRROR_X) {
        x = static_cast<int16_t>(cfg::SCREEN_WIDTH - 1 - x);
    }
    if (cfg::TOUCH_MIRROR_Y) {
        y = static_cast<int16_t>(cfg::SCREEN_HEIGHT - 1 - y);
    }

    *outX = constrain(x, 0, static_cast<int16_t>(cfg::SCREEN_WIDTH - 1));
    *outY = constrain(y, 0, static_cast<int16_t>(cfg::SCREEN_HEIGHT - 1));
    return true;
}

bool onFlushDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* userCtx) {
    lv_disp_flush_ready(static_cast<lv_disp_drv_t*>(userCtx));
    return false;
}

void flushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* pixels) {
    esp_lcd_panel_draw_bitmap(g_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
    if (drv->full_refresh) {
        lv_disp_flush_ready(drv);
    }
}

void touchReadCb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    (void)drv;
    static int16_t lastX = 0;
    static int16_t lastY = 0;

    int16_t x = 0;
    int16_t y = 0;
    if (readTouch(&x, &y)) {
        lastX = x;
        lastY = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = lastX;
    data->point.y = lastY;
}

}  // namespace

void begin() {
    Serial.println("[display] esp_lcd init");
    Wire.begin(cfg::PIN_I2C_SDA, cfg::PIN_I2C_SCL);
    Wire.setClock(400000);
    delay(20);

    power::begin();
    tca9554Reset();

    spi_bus_config_t busCfg = {};
    busCfg.mosi_io_num = PIN_MOSI;
    busCfg.miso_io_num = PIN_MISO;
    busCfg.sclk_io_num = PIN_SCLK;
    busCfg.quadwp_io_num = -1;
    busCfg.quadhd_io_num = -1;
    busCfg.max_transfer_sz = cfg::SCREEN_WIDTH * cfg::SCREEN_HEIGHT * static_cast<int>(sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &busCfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t ioCfg = {};
    ioCfg.cs_gpio_num = -1;
    ioCfg.dc_gpio_num = PIN_DC;
    ioCfg.spi_mode = cfg::DISPLAY_SPI_MODE;
    ioCfg.pclk_hz = 40 * 1000 * 1000;
    ioCfg.trans_queue_depth = 10;
    ioCfg.lcd_cmd_bits = 8;
    ioCfg.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(reinterpret_cast<esp_lcd_spi_bus_handle_t>(SPI3_HOST), &ioCfg, &g_panelIo));

    esp_lcd_panel_dev_config_t panelCfg = {};
    panelCfg.reset_gpio_num = -1;
    panelCfg.color_space = ESP_LCD_COLOR_SPACE_BGR;
    panelCfg.bits_per_pixel = 16;
    panelCfg.vendor_config = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(g_panelIo, &panelCfg, &g_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel));
    sendSt7796Init(g_panelIo);
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(g_panel, cfg::DISPLAY_COLOR_INVERT));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(g_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(g_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(g_panel, true));

    enableBacklight();

    const size_t bufPixels = static_cast<size_t>(cfg::SCREEN_WIDTH) * cfg::LVGL_BUFFER_LINES;
    const size_t bufBytes = bufPixels * sizeof(lv_color_t);
    lvBuffer1 = static_cast<lv_color_t*>(heap_caps_malloc(bufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    lvBuffer2 = static_cast<lv_color_t*>(heap_caps_malloc(bufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (lvBuffer1 == nullptr || lvBuffer2 == nullptr) {
        Serial.println("[display] DMA buffer alloc failed");
    }

    lv_init();
    lv_disp_draw_buf_init(&drawBuffer, lvBuffer1, lvBuffer2, bufPixels);

    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res = cfg::SCREEN_WIDTH;
    dispDrv.ver_res = cfg::SCREEN_HEIGHT;
    dispDrv.flush_cb = flushCb;
    dispDrv.draw_buf = &drawBuffer;

    esp_lcd_panel_io_callbacks_t ioCbs = {};
    ioCbs.on_color_trans_done = onFlushDone;
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(g_panelIo, &ioCbs, &dispDrv));

    lv_disp_drv_register(&dispDrv);

    lv_indev_drv_init(&indevDrv);
    indevDrv.type = LV_INDEV_TYPE_POINTER;
    indevDrv.read_cb = touchReadCb;
    lv_indev_drv_register(&indevDrv);

    Serial.println("[display] ready");
}

void tick() {
    lv_timer_handler();
}

void setBacklight(uint8_t duty) {
    applyBacklight(duty);
}

}  // namespace display
