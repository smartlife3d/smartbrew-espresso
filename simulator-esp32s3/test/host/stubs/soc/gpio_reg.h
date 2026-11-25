#pragma once
#include <cstdint>
extern volatile uint32_t g_fakeGpioIn;
#define GPIO_IN_REG ((uintptr_t)&g_fakeGpioIn)
