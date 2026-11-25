// =================================================================================================
//  logger.h - Asynchronous ring-buffer logger drained by a Core 0 background task.
//             Dual-transport output to USB CDC and UART0.
//
//  Tasks on Core 1 (such as the real-time control task) send formatted log messages to an 8 KB
//  FreeRTOS ring buffer non-blocking (0 timeout). If the buffer is full, messages are dropped
//  and counted without stalling the control task.
// =================================================================================================
#pragma once

#include <Arduino.h>

#ifndef SB_LOG_LEVEL
#define SB_LOG_LEVEL 3
#endif

namespace sb_log {

enum Level : uint8_t { OFF = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4, VERBOSE = 5 };

void initAsync();
void writeLog(Level lvl, const char* msg);
void putRaw(const char* s);
uint32_t logDrops();
String getRecentLogs();
void clearRecentLogs();

}  // namespace sb_log

// -------------------------------------------------------------------------------------------------
// Note: the level argument is passed fully qualified (e.g. sb_log::ERROR) and used verbatim in the
// body, so it must NOT be namespaced again inside the macro.
// -------------------------------------------------------------------------------------------------
#define SB_LOG_AT(lvl, fmt, ...)                                                    \
    do {                                                                             \
        if ((lvl) <= (sb_log::Level)(SB_LOG_LEVEL)) {                               \
            char _sbbuf[200];                                                        \
            snprintf(_sbbuf, sizeof(_sbbuf), fmt, ##__VA_ARGS__);                   \
            sb_log::writeLog(lvl, _sbbuf);                                          \
        }                                                                            \
    } while (0)

#define SB_LOGE(fmt, ...) SB_LOG_AT(sb_log::ERROR, fmt, ##__VA_ARGS__)
#define SB_LOGW(fmt, ...) SB_LOG_AT(sb_log::WARN,  fmt, ##__VA_ARGS__)
#define SB_LOGI(fmt, ...) SB_LOG_AT(sb_log::INFO,  fmt, ##__VA_ARGS__)
#define SB_LOGD(fmt, ...) SB_LOG_AT(sb_log::DEBUG, fmt, ##__VA_ARGS__)
#define SB_LOGV(fmt, ...) SB_LOG_AT(sb_log::VERBOSE, fmt, ##__VA_ARGS__)

#define SB_BANNER(text) SB_LOGI("============================================================")