// =================================================================================================
//  shot_history.h - In-RAM ring buffer + LittleFS persistence of completed extraction graphs
//                   docs/API.md `GET /api/v1/history`
//
//  Layout on flash: /history/<index>.json  (index 0..14, round robin)
//  Each file holds the shot header (profile, epochMs as a 64-bit millisecond timestamp, duration,
//  peak, dose) plus its ShotSample array as a compact nested JSON array.
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "profile_types.h"
#include "control/profile_engine.h"

class ShotHistory {
public:
    void begin();

    void resetCapture(const char* profId = nullptr, const char* profName = nullptr);
    void addSample(const ShotSample& s);

    // Appends a sample to the shot that is waiting to be written - the post-shot tail.
    //
    // The tail belongs to the shot that just ended, not to whatever is being captured now, so it is
    // routed explicitly instead of being guessed from the capture state. Without this the stored
    // curve would stop at the reading that tripped the cutoff, and the ledger's graph would show a
    // shot that ends short of the coffee that actually ended up in the cup.
    void addTailSample(const ShotSample& s);

    // Freezes the finished shot so that the next one can start capturing straight away while this
    // one is still waiting to be written to flash.
    //
    // A shot is not really over when the pump stops: the last of the coffee is still arriving in the
    // cup, so the ledger entry is written a moment later. The curve is copied here at the moment the
    // pump stops, so the frozen copy - and not whatever the next shot captures - is what ends up in
    // the ledger.
    //
    // durationMs is the shot's own brewing time. It is passed in rather than derived from the last
    // sample, because the tail samples that follow sit beyond the end of the shot.
    void freezeShot(uint32_t durationMs = 0);

    // The yield that ended up in the cup. It is only known once the post-shot weight tail has run
    // out, so it is recorded separately from the reading that merely tripped the cutoff.
    void setSettledDose(float g);

    int  saveShot();                       // persists the current buffer, returns the slot or -1
    int  saveExternalShot(const char* profileId, const char* profileName,
                          float peakBar, float doseG, uint32_t durationMs,
                          uint64_t epochMs, const char* samplesJsonArray);
    int  count() const;                    // 0..15 saved shots
    bool readShot(int slot, String& jsonOut) const;
    bool readRaw(int slot, uint8_t** data, size_t* len) const;   // caller frees
    int  deleteShot(int slot);
    int  nextSlot() const { return _nextSlot; }
    bool captureActive() const { return _capturing; }

    // Wall-clock synchronization from client / NTP
    void     setWallTime(uint64_t epochMs);
    uint64_t wallTimeNow() const;

    uint16_t sampleCount() const { return _count; }
    float    peakBar()  const { return _peakBar; }
    // The dose the ledger records: the yield that ended up in the cup once the post-shot tail has
    // run out, and the reading that tripped the cutoff until then.
    float    doseG()    const;
    uint32_t durationMs() const { return (_count && _samples) ? _samples[_count - 1].tMs : 0; }

    // Lightweight summary JSON for GET /api/v1/history
    String summaryJson() const;

    // Live capture buffer (used by /api/v1/status during a shot)
    const ShotSample* samples() const { return _samples; }

private:
    void ensureDir();

    // A finished shot that is waiting to be written to flash. It holds its own copy of the curve,
    // so the live capture can be reused by the next shot straight away.
    struct FrozenShot {
        ShotSample* samples   = nullptr;
        uint16_t   count      = 0;
        float      peakBar    = 0.0f;
        float      cutoffG    = 0.0f;   // the highest weight this shot recorded
        float      settledG   = 0.0f;   // the yield in the cup, once the tail has run out
        uint32_t   durationMs = 0;
        char       profileId[40] = {0};
        char       profileName[SB_PROFILE_NAME_LEN] = {0};
        bool       valid      = false;
    };

    mutable SemaphoreHandle_t _fsMutex = nullptr;
    mutable portMUX_TYPE      _captureMux = portMUX_INITIALIZER_UNLOCKED;
    uint16_t   _presentSlots = 0;
    void       scanSlots();
    ShotSample* _samples = nullptr;
    ShotSample* _writeScratch = nullptr;
    uint16_t   _count = 0;
    bool       _capturing = false;
    float      _peakBar = 0.0f;
    float      _doseG = 0.0f;
    float      _settledDoseG = 0.0f;    // the settled yield of the live capture
    char       _profileId[40] = {0};
    char       _profileName[SB_PROFILE_NAME_LEN] = {0};
    uint8_t    _nextSlot = 0;
    bool       _ready = false;
    std::atomic<uint64_t> _wallBaseMs{0};
    std::atomic<uint32_t> _wallSyncedAtMs{0};
    FrozenShot _frozen;
};

extern ShotHistory g_history;