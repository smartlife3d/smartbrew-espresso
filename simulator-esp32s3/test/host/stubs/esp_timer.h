#pragma once
#include <cstdint>
typedef void* esp_timer_handle_t;
extern uint64_t g_fakeUs;
inline uint64_t esp_timer_get_time() { return g_fakeUs; }
