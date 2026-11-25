#pragma once
#include <cstdio>
#define SB_LOGI(f, ...) do { if (g_verbose) { printf("I| " f "\n", ##__VA_ARGS__); } } while (0)
#define SB_LOGW(f, ...) do { if (g_verbose) { printf("W| " f "\n", ##__VA_ARGS__); } } while (0)
#define SB_LOGE SB_LOGW
#define SB_LOGD(...) do {} while (0)
extern bool g_verbose;
