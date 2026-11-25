// =================================================================================================
//  ota_service.h - Disarm-protected firmware / filesystem update   (docs/Architecture.md)
//
//  SAFETY INVARIANT: the PSM must be disarmed before a single flash sector is erased, and must not
//  be re-armed until the new image has booted and re-initialised itself. A brown-out half way
//  through a flash write must never leave the TRIAC gate high.
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <Update.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum class OtaResult : uint8_t { IDLE, RUNNING, OK, ERROR };

class OtaService {
public:
    void begin();

    // Call from AsyncWebServer's upload handler.
    void onUploadStart();
    void onUploadChunk(uint8_t* data, size_t len, size_t index, size_t total, bool final);
    OtaResult result() const { return _result.load(std::memory_order_relaxed); }
    uint32_t  written() const { return _written.load(std::memory_order_relaxed); }
    size_t    contentLength() const { return _total.load(std::memory_order_relaxed); }
    const char* message() const { return _msg; }
    const char* lastError() const { return _lastError; }

    // GitHub release pull (config.otaGitHubUrl) - returns true if the update completed.
    bool updateFromUrl(const char* url);

    // Abort active OTA and recover hardware safely
    void abort();
    void checkWatchdog();

    // Plan / rebuild guard: everything that must happen before a flash write.
    static void disarmForFlash();
    static void rearmAfterFailure();
    static void reportAfterUpdate();

private:
    std::atomic<OtaResult> _result{OtaResult::IDLE};
    std::atomic<uint32_t>  _written{0};
    std::atomic<size_t>    _total{0};
    std::atomic<uint32_t>  _lastChunkMs{0};
    std::atomic<bool>      _aborted{false};
    std::atomic<bool>      _downloadInProgress{false};
    SemaphoreHandle_t      _flashMutex = nullptr;
    char                   _msg[96] = {0};
    char                   _lastError[160] = {0};
};

extern OtaService g_ota;