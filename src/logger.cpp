// =================================================================================================
//  logger.cpp - Async ring-buffer logger drained by a Core 0 background task
// =================================================================================================

#include "logger.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <esp_task_wdt.h>

namespace sb_log {

static RingbufHandle_t s_logRingBuf = nullptr;
static std::atomic<uint32_t> s_logDrops{0};
static std::atomic<bool> s_initAttempted{false};

static char s_recentLogBuf[8192];
static size_t s_recentHead = 0;
static bool s_recentWrapped = false;
static portMUX_TYPE s_recentMux = portMUX_INITIALIZER_UNLOCKED;

static inline char levelTag(Level lvl) {
    switch (lvl) {
        case ERROR: return '!';
        case WARN:  return '*';
        case INFO:  return ' ';
        default:    return ' ';
    }
}

void initAsync() {
    bool expected = false;
    if (!s_initAttempted.compare_exchange_strong(expected, true)) return;
    s_logRingBuf = xRingbufferCreate(8192, RINGBUF_TYPE_NOSPLIT);
    if (!s_logRingBuf) {
        Serial.println("ERR: Failed to allocate log ring buffer. Logging disabled to prevent blocking RT tasks.");
        return;
    }

    // Priority 2: above Arduino loopTask (priority 1) so logger is promptly drained on Core 0
    xTaskCreatePinnedToCore([](void*) {
        esp_err_t werr = esp_task_wdt_add(NULL);
        if (werr != ESP_OK) {
            Serial.printf("WARN: log_drain failed to subscribe to TWDT: %d\n", (int)werr);
        }
        for (;;) {
            esp_task_wdt_reset();
            size_t size = 0;
            char* item = (char*)xRingbufferReceive(s_logRingBuf, &size, pdMS_TO_TICKS(50));
            if (item) {
                if (size > 0) {
                    Serial.write((const uint8_t*)item, size);
                    Serial0.write((const uint8_t*)item, size);
                    portENTER_CRITICAL(&s_recentMux);
                    for (size_t i = 0; i < size; ++i) {
                        s_recentLogBuf[s_recentHead++] = item[i];
                        if (s_recentHead >= sizeof(s_recentLogBuf)) {
                            s_recentHead = 0;
                            s_recentWrapped = true;
                        }
                    }
                    portEXIT_CRITICAL(&s_recentMux);
                }
                vRingbufferReturnItem(s_logRingBuf, item);
            }
        }
    }, "log_drain", 3072, nullptr, 2, nullptr, 0);
}

void writeLog(Level lvl, const char* msg) {
    const char tag = levelTag(lvl);
    char line[220];
    int len = snprintf(line, sizeof(line), "%c|%s\n", tag, msg ? msg : "");
    if (len <= 0) return;
    if ((size_t)len >= sizeof(line)) len = sizeof(line) - 1;

    // If ring buffer is ready, send non-blocking (0 timeout).
    // On buffer overflow, drop the line and count it. Never block the caller!
    if (s_logRingBuf) {
        BaseType_t ok = xRingbufferSend(s_logRingBuf, line, len, 0);
        if (ok != pdTRUE) {
            s_logDrops.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    // Fallback sync write ONLY during early boot before initAsync() runs.
    // If initAsync() already ran and buffer failed to allocate, drop and count to prevent blocking RT tasks.
    if (!s_initAttempted.load(std::memory_order_relaxed)) {
        Serial.write((const uint8_t*)line, len);
        Serial0.write((const uint8_t*)line, len);
        portENTER_CRITICAL(&s_recentMux);
        for (int i = 0; i < len; ++i) {
            s_recentLogBuf[s_recentHead++] = line[i];
            if (s_recentHead >= sizeof(s_recentLogBuf)) {
                s_recentHead = 0;
                s_recentWrapped = true;
            }
        }
        portEXIT_CRITICAL(&s_recentMux);
    } else {
        s_logDrops.fetch_add(1, std::memory_order_relaxed);
    }
}

void putRaw(const char* s) {
    if (!s) return;
    if (s_logRingBuf) {
        size_t len = strlen(s);
        if (len > 0) {
            BaseType_t ok = xRingbufferSend(s_logRingBuf, s, len, 0);
            if (ok != pdTRUE) {
                s_logDrops.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }

    if (!s_initAttempted.load(std::memory_order_relaxed)) {
        Serial.print(s);
        Serial0.print(s);
        size_t slen = strlen(s);
        portENTER_CRITICAL(&s_recentMux);
        for (size_t i = 0; i < slen; ++i) {
            s_recentLogBuf[s_recentHead++] = s[i];
            if (s_recentHead >= sizeof(s_recentLogBuf)) {
                s_recentHead = 0;
                s_recentWrapped = true;
            }
        }
        portEXIT_CRITICAL(&s_recentMux);
    } else {
        s_logDrops.fetch_add(1, std::memory_order_relaxed);
    }
}

uint32_t logDrops() {
    return s_logDrops.load(std::memory_order_relaxed);
}

String getRecentLogs() {
    String out;
    portENTER_CRITICAL(&s_recentMux);
    if (!s_recentWrapped) {
        if (s_recentHead > 0) {
            out.reserve(s_recentHead + 1);
            for (size_t i = 0; i < s_recentHead; ++i) {
                out += s_recentLogBuf[i];
            }
        }
    } else {
        out.reserve(sizeof(s_recentLogBuf) + 1);
        for (size_t i = s_recentHead; i < sizeof(s_recentLogBuf); ++i) {
            out += s_recentLogBuf[i];
        }
        for (size_t i = 0; i < s_recentHead; ++i) {
            out += s_recentLogBuf[i];
        }
    }
    portEXIT_CRITICAL(&s_recentMux);
    return out;
}

void clearRecentLogs() {
    portENTER_CRITICAL(&s_recentMux);
    s_recentHead = 0;
    s_recentWrapped = false;
    portEXIT_CRITICAL(&s_recentMux);
}

}  // namespace sb_log
