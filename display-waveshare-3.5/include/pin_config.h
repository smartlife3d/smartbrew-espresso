#pragma once

// =============================================================================
// Waveshare ESP32-S3-Touch-LCD-3.5B Pin Configurations
// Board: ESP32-S3R8 + 3.5-inch AXS15231B (480x320 QSPI) + AXS15231B Touch
// =============================================================================

// Display (Hardware QSPI Mode for Waveshare ESP32-S3-Touch-LCD-3.5B)
#define LCD_QSPI_CS_PIN   12
#define LCD_QSPI_CLK_PIN  5
#define LCD_QSPI_D0_PIN   1
#define LCD_QSPI_D1_PIN   2
#define LCD_QSPI_D2_PIN   3
#define LCD_QSPI_D3_PIN   4
#define LCD_BL_PIN        6

// I2C Bus (Shared: TCA9554 IO Expander, AXS15231B Touch, PCF85063 RTC, QMI8658 IMU, AXP2101 PMIC)
#define I2C_SDA_PIN       8
#define I2C_SCL_PIN       7
#define TCA9554_I2C_ADDR  0x20
#define TCA9554_LCD_RST_PIN 1  // Pin 1 of TCA9554 controls LCD_RST

// Touch (I2C)
#define TOUCH_I2C_ADDR    0x3B // AXS15231B Touch controller address

// Display dimensions in Landscape
#define SCREEN_WIDTH      480
#define SCREEN_HEIGHT     320
