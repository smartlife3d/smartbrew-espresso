#pragma once
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "esp_timer.h"
extern uint32_t g_fakeMicros;
inline uint32_t micros() { return g_fakeMicros; }
inline uint32_t millis() { return g_fakeMicros / 1000; }
#define IRAM_ATTR
#define HIGH 1
#define LOW 0
#define INPUT_PULLDOWN 2
#define INPUT_PULLUP 3
#define OUTPUT 1
inline void pinMode(uint8_t, uint8_t) {}
inline void digitalWrite(uint8_t, uint8_t) {}
extern int g_fakePinLevel;
extern int g_fakeBaristaLevel;
inline int  digitalRead(uint8_t p) { return (p == 0) ? g_fakeBaristaLevel : g_fakePinLevel; }
inline uint8_t digitalPinToInterrupt(uint8_t p) { return p; }
struct SerialStub {
    template<typename... A> void printf(const char* f, A...) { (void)f; }
    void println(const char*) {}
    void print(const char*) {}
};
extern SerialStub Serial;
using isr_handler_t = void(*)();
extern isr_handler_t g_fakeIsr;
inline void attachInterrupt(uint8_t pin, isr_handler_t h, int) { g_fakeIsr = h; }
#define CHANGE 3
#define RISING 1
#define FALLING 2
#define F(x) (x)
