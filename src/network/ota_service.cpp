// =================================================================================================
//  ota_service.cpp - see ota_service.h
// =================================================================================================

#include "ota_service.h"
#include "logger.h"
#include "hal/hal_psm.h"
#include "hal/hal_button.h"
#include "brew_controller.h"

#include <esp_task_wdt.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>

// -------------------------------------------------------------------------------------------------
void OtaService::begin() {
    if (!_flashMutex) {
        _flashMutex = xSemaphoreCreateMutex();
    }
    SB_LOGI("OTA service ready. Hardware interlocks and mutex protection armed.");
}

void OtaService::disarmForFlash() {
    SB_BANNER("");
    SB_LOGE("*** DISARMING POWER STAGE FOR FIRMWARE FLASH ***");
    g_brew.requestStop("OTA disarm");
    g_psm.disarm();
    g_button.abort();
    g_psm.setDutyPct(0.0f);
    digitalWrite(PIN_PSM_GATE, LOW);
    digitalWrite(PIN_BTN_TRIG, LOW);
    SB_LOGE("*** PSM DISARMED - the pump cannot be driven during the flash. ***");
    SB_BANNER("");
    delay(50);
}

void OtaService::rearmAfterFailure() {
    g_psm.arm();
    g_button.begin();
    g_ota._lastChunkMs.store(0, std::memory_order_relaxed);
    if (g_ota._result.load(std::memory_order_relaxed) == OtaResult::RUNNING) {
        g_ota._result.store(OtaResult::ERROR, std::memory_order_relaxed);
    }
    SB_LOGI("Power stage and virtual button re-armed after OTA failure or abort.");
}

void OtaService::abort() {
    _aborted.store(true, std::memory_order_relaxed);
    if (_flashMutex) xSemaphoreTake(_flashMutex, portMAX_DELAY);
    if (_result.load(std::memory_order_relaxed) == OtaResult::RUNNING) {
        Update.abort();
        _result.store(OtaResult::ERROR, std::memory_order_relaxed);
        strlcpy(_lastError, "OTA aborted by watchdog or user", sizeof(_lastError));
        rearmAfterFailure();
    }
    if (_flashMutex) xSemaphoreGive(_flashMutex);
    _downloadInProgress.store(false, std::memory_order_relaxed);
}

void OtaService::checkWatchdog() {
    const uint32_t last = _lastChunkMs.load(std::memory_order_relaxed);
    if (_result.load(std::memory_order_relaxed) == OtaResult::RUNNING && last != 0 && (millis() - last > 30000UL)) {
        SB_LOGE("OTA upload timeout (>30s) - aborting and re-arming");
        abort();
    }
}

void OtaService::reportAfterUpdate() {
    if (g_ota.result() == OtaResult::OK) {
        SB_LOGI("OTA SUCCESS: %u bytes written. Rebooting into the new image.",
                (unsigned)g_ota.written());
    }
}

// -------------------------------------------------------------------------------------------------
void OtaService::onUploadStart() {
    disarmForFlash();
    _result.store(OtaResult::RUNNING, std::memory_order_relaxed);
    _aborted.store(false, std::memory_order_relaxed);
    _written.store(0, std::memory_order_relaxed);
    _total.store(0, std::memory_order_relaxed);
    _lastChunkMs.store(millis(), std::memory_order_relaxed);
    _msg[0] = '\0';
    _lastError[0] = '\0';
    SB_LOGI("OTA upload started.");
}

void OtaService::onUploadChunk(uint8_t* data, size_t len, size_t index, size_t total, bool final) {
    // If a background URL download is active, refuse immediately without blocking!
    if (_downloadInProgress.load(std::memory_order_relaxed)) {
        SB_LOGW("Browser upload rejected: background OTA download in progress");
        return;
    }

    // R3-06: Sticky abort. If aborted, reject chunks until a fresh upload starts at index 0.
    if (index != 0 && (_aborted.load(std::memory_order_relaxed) || _result.load(std::memory_order_relaxed) != OtaResult::RUNNING)) {
        return;
    }

    _lastChunkMs.store(millis(), std::memory_order_relaxed);

    if (index == 0) {
        if (_flashMutex) xSemaphoreTake(_flashMutex, portMAX_DELAY);
        onUploadStart();
        _total.store(total, std::memory_order_relaxed);
        // `total` is the request's Content-Length: the whole multipart body, i.e. the file plus
        // the form boundaries and part headers. Sizing the update with it left every upload a
        // few hundred bytes short, and Update.end() then aborted it (UPDATE_ERROR_ABORT, 12).
        // The file size is not known up front, so open the update unsized and close it with
        // end(true), which finalises exactly the bytes written. esp_ota_end() still verifies
        // the image header and SHA-256, so a truncated upload is rejected there.
        const size_t maxSpace = (size_t)ESP.getFreeSketchSpace();
        const size_t targetSize = UPDATE_SIZE_UNKNOWN;
        if (!Update.begin(targetSize, U_FLASH)) {
            _result.store(OtaResult::ERROR, std::memory_order_relaxed);
            _aborted.store(true, std::memory_order_relaxed);
            snprintf(_lastError, sizeof(_lastError), "Update.begin failed (target %u, free %u)",
                     (unsigned)targetSize, (unsigned)maxSpace);
            SB_LOGE("OTA ERROR: %s", _lastError);
            rearmAfterFailure();
            if (_flashMutex) xSemaphoreGive(_flashMutex);
            return;
        }
        if (_flashMutex) xSemaphoreGive(_flashMutex);
    }

    if (!Update.isRunning() || _aborted.load(std::memory_order_relaxed)) {
        return;
    }

    if (_flashMutex) xSemaphoreTake(_flashMutex, portMAX_DELAY);
    size_t w = Update.write(data, len);
    _written.fetch_add(w, std::memory_order_relaxed);
    if (w != len) {
        _result.store(OtaResult::ERROR, std::memory_order_relaxed);
        _aborted.store(true, std::memory_order_relaxed);
        snprintf(_lastError, sizeof(_lastError), "short write %u/%u", (unsigned)w, (unsigned)len);
        Update.abort();
        SB_LOGE("OTA ERROR: %s", _lastError);
        rearmAfterFailure();
        if (_flashMutex) xSemaphoreGive(_flashMutex);
        return;
    }

    if (final) {
        // end(true): finalise the bytes written (the update was opened unsized). The rollback
        // does not depend on this flag: the new image boots in PENDING_VERIFY and the control
        // task calls esp_ota_mark_app_valid_cancel_rollback() once it has run cleanly.
        if (Update.end(true)) {
            _result.store(OtaResult::OK, std::memory_order_relaxed);
            snprintf(_msg, sizeof(_msg), "%u bytes", (unsigned)_written.load(std::memory_order_relaxed));
            SB_LOGI("OTA write complete (%u bytes).", (unsigned)_written.load(std::memory_order_relaxed));
        } else {
            _result.store(OtaResult::ERROR, std::memory_order_relaxed);
            _aborted.store(true, std::memory_order_relaxed);
            snprintf(_lastError, sizeof(_lastError), "Update.end failed (%d)", Update.getError());
            Update.abort();
            SB_LOGE("OTA ERROR: %s", _lastError);
            rearmAfterFailure();
        }
    }

    if (_flashMutex) xSemaphoreGive(_flashMutex);
}

// -------------------------------------------------------------------------------------------------
bool OtaService::updateFromUrl(const char* url) {
    if (!url || !*url) {
        SB_LOGW("OTA from URL: no URL configured");
        return false;
    }

    bool expected = false;
    if (!_downloadInProgress.compare_exchange_strong(expected, true)) {
        SB_LOGW("OTA from URL rejected: download already in progress");
        return false;
    }

    if (_result.load(std::memory_order_relaxed) == OtaResult::RUNNING) {
        SB_LOGW("OTA already running");
        _downloadInProgress.store(false, std::memory_order_relaxed);
        return false;
    }

    disarmForFlash();

    _result.store(OtaResult::RUNNING, std::memory_order_relaxed);
    _aborted.store(false, std::memory_order_relaxed);
    _written.store(0, std::memory_order_relaxed);
    _lastChunkMs.store(millis(), std::memory_order_relaxed);

    HTTPClient http;
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    const bool isHttps = (strncasecmp(url, "https://", 8) == 0);
    if (isHttps) {
        secureClient.setInsecure();
    }
    const bool httpBegun = isHttps ? http.begin(secureClient, url) : http.begin(plainClient, url);
    if (!httpBegun) {
        _result.store(OtaResult::ERROR, std::memory_order_relaxed);
        strlcpy(_lastError, "bad URL", sizeof(_lastError));
        rearmAfterFailure();
        _downloadInProgress.store(false, std::memory_order_relaxed);
        return false;
    }
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(20000);

    const int code = http.GET();
    if (code != 200) {
        _result.store(OtaResult::ERROR, std::memory_order_relaxed);
        snprintf(_lastError, sizeof(_lastError), "HTTP %d", code);
        SB_LOGE("OTA pull failed: HTTP %d", code);
        http.end();
        rearmAfterFailure();
        _downloadInProgress.store(false, std::memory_order_relaxed);
        return false;
    }

    const int len = http.getSize();
    WiFiClient* stream = http.getStreamPtr();

    if (_flashMutex) xSemaphoreTake(_flashMutex, portMAX_DELAY);
    // Size the update with the download's own length when the server sends one; otherwise open
    // it unsized. Either way end(true) below finalises exactly the bytes received.
    const bool beginOk = Update.begin(len > 0 ? (size_t)len : UPDATE_SIZE_UNKNOWN, U_FLASH);
    if (_flashMutex) xSemaphoreGive(_flashMutex);

    if (!beginOk) {
        _result.store(OtaResult::ERROR, std::memory_order_relaxed);
        strlcpy(_lastError, "Update.begin failed", sizeof(_lastError));
        http.end();
        rearmAfterFailure();
        _downloadInProgress.store(false, std::memory_order_relaxed);
        return false;
    }

    uint8_t buf[1024];
    size_t got = 0;
    int    seen = 0;
    const uint32_t startMs = millis();
    _lastChunkMs.store(millis(), std::memory_order_relaxed);

    while (http.connected() && ((len < 0) || (seen < len))) {
        if (_aborted.load(std::memory_order_relaxed)) {
            SB_LOGW("OTA download aborted by user/watchdog");
            http.end();
            _downloadInProgress.store(false, std::memory_order_relaxed);
            return false;
        }

        // Overall 5-minute timeout
        if (millis() - startMs > 300000UL) {
            _result.store(OtaResult::ERROR, std::memory_order_relaxed);
            strlcpy(_lastError, "OTA download timeout (5 min exceeded)", sizeof(_lastError));
            if (_flashMutex) xSemaphoreTake(_flashMutex, portMAX_DELAY);
            Update.abort();
            if (_flashMutex) xSemaphoreGive(_flashMutex);
            http.end();
            rearmAfterFailure();
            _downloadInProgress.store(false, std::memory_order_relaxed);
            return false;
        }

        const size_t avail = stream->available();
        if (!avail) {
            // 30 s stall timeout
            if (millis() - _lastChunkMs.load(std::memory_order_relaxed) > 30000UL) {
                _result.store(OtaResult::ERROR, std::memory_order_relaxed);
                strlcpy(_lastError, "OTA download stalled (>30s)", sizeof(_lastError));
                if (_flashMutex) xSemaphoreTake(_flashMutex, portMAX_DELAY);
                Update.abort();
                if (_flashMutex) xSemaphoreGive(_flashMutex);
                http.end();
                rearmAfterFailure();
                _downloadInProgress.store(false, std::memory_order_relaxed);
                return false;
            }
            delay(1);
            continue;
        }

        // Read only what is already here. Stream::readBytes() busy-waits, without yielding, until
        // the whole buffer has arrived, which kept the core 0 idle task off the CPU and let the
        // task watchdog reset the board mid-update.
        size_t want = sizeof(buf);
        if (avail < want) want = avail;
        if (len > 0 && (size_t)(len - seen) < want) want = (size_t)(len - seen);
        const int n = stream->read(buf, want);
        if (n <= 0) { delay(1); continue; }
        _lastChunkMs.store(millis(), std::memory_order_relaxed);

        if (_flashMutex) xSemaphoreTake(_flashMutex, portMAX_DELAY);
        size_t writtenBytes = Update.write(buf, (size_t)n);
        if (_flashMutex) xSemaphoreGive(_flashMutex);

        if (writtenBytes != (size_t)n) {
            _result.store(OtaResult::ERROR, std::memory_order_relaxed);
            snprintf(_lastError, sizeof(_lastError), "short write %u/%d", (unsigned)writtenBytes, n);
            if (_flashMutex) xSemaphoreTake(_flashMutex, portMAX_DELAY);
            Update.abort();
            if (_flashMutex) xSemaphoreGive(_flashMutex);
            http.end();
            rearmAfterFailure();
            _downloadInProgress.store(false, std::memory_order_relaxed);
            return false;
        }

        got += writtenBytes;
        seen += n;
        _written.store(got, std::memory_order_relaxed);
        // On a fast link this loop never waits for data, and Update.write() keeps the flash busy.
        // Sleep one tick every 8 KB so the core 0 idle task (watched by the task watchdog) runs.
        if ((got & 0x1FFF) < (size_t)n) vTaskDelay(1);
        if (got % 32768 < sizeof(buf)) SB_LOGD("OTA %u / %d bytes", (unsigned)got, len);
    }

    if (_flashMutex) xSemaphoreTake(_flashMutex, portMAX_DELAY);
    // A download that ended early is caught here: with a known length, fewer bytes than `len`
    // means the stream broke off. end(true) would otherwise finalise a short image.
    const bool complete = (len < 0) || (seen == len);
    const bool endOk = complete && Update.end(true);
    if (_flashMutex) xSemaphoreGive(_flashMutex);

    _downloadInProgress.store(false, std::memory_order_relaxed);

    if (endOk) {
        _result.store(OtaResult::OK, std::memory_order_relaxed);
        _written.store(got, std::memory_order_relaxed);
        SB_LOGI("OTA pull complete: %u bytes. Rebooting into new firmware...", (unsigned)got);
        http.end();
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(500));
            ESP.restart();
        }, "reboot", 2048, nullptr, 1, nullptr);
        return true;
    } else {
        _result.store(OtaResult::ERROR, std::memory_order_relaxed);
        snprintf(_lastError, sizeof(_lastError), "Update.end failed after %u bytes", (unsigned)got);
        if (_flashMutex) xSemaphoreTake(_flashMutex, portMAX_DELAY);
        Update.abort();
        if (_flashMutex) xSemaphoreGive(_flashMutex);
        rearmAfterFailure();
    }
    http.end();
    return false;
}

OtaService g_ota;