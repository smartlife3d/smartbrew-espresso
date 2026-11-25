// ssd1306.h - minimal SSD1306 128x32 OLED driver over the ESP32 core Wire
// library (no external dependency: Wire ships with framework-arduinoespressif32).
//
// Page-addressed 128x32 framebuffer (4 pages x 8 rows) held in RAM; flush()
// streams all 512 bytes over I2C in 32-byte chunks.

#pragma once

#include <Arduino.h>
#include <stdint.h>

#define SSD1306_WIDTH  128
#define SSD1306_HEIGHT 32
#define SSD1306_PAGES  (SSD1306_HEIGHT / 8)

// 5x7 glyph cell, 6 px advance.
#define SSD1306_CHAR_W  5
#define SSD1306_CHAR_H  7

class Ssd1306 {
public:
    Ssd1306();

    // Scans the bus for addr (0x3C or 0x3D) and runs the init sequence.
    bool begin(uint8_t sdaPin, uint8_t sclPin, uint8_t contrast = 0xCF);
    bool present() const { return _present; }

    void clear();
    void setContrast(uint8_t contrast);
    void displayOn(bool on);          // false = panel sleep (saves power)

    void setPixel(int x, int y, bool on);
    void fillRect(int x, int y, int w, int h, bool on);
    void drawRect(int x, int y, int w, int h, bool on);
    void drawHLine(int x, int y, int w, bool on);

    // scale 1 => 5x7 glyph, scale 2 => 10x14 glyph (6*scale px advance).
    void drawChar(int x, int y, char c, uint8_t scale, bool inverted);
    void drawText(int x, int y, const char* text, uint8_t scale = 1, bool inverted = false);
    // Draws so the string ENDS at xRight (right-aligned).
    void drawTextRight(int xRight, int y, const char* text, uint8_t scale = 1, bool inverted = false);
    // Right-aligned fixed-point number: 2 decimals, e.g. 12.34 (scale 2).
    void drawNumberRight(int xRight, int y, float value, uint8_t decimals,
                         uint8_t scale = 1, bool showSign = false);

    void drawBattery(int x, int y, uint8_t pct);   // outline + tip + fill bar

    void flush();                     // push the framebuffer to the panel

private:
    void command(uint8_t cmd);
    void commands(const uint8_t* cmds, size_t count);
    void pageAddress(int page);
    void columnAddress(int col);

    bool     _present;
    bool     _panelOn;
    uint8_t  _addr;
    uint8_t  _contrast;
    uint8_t  _buffer[SSD1306_WIDTH * SSD1306_PAGES];
};
