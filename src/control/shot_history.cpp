// =================================================================================================
//  shot_history.cpp - see shot_history.h
// =================================================================================================

#include "shot_history.h"
#include "shot_tail.h"
#include "logger.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <FS.h>
#include <time.h>
#include <esp_heap_caps.h>

#define HISTORY_DIR   "/history"
#define HISTORY_SLOTS 15

namespace {
struct ScopedFsLock {
    SemaphoreHandle_t m;
    explicit ScopedFsLock(SemaphoreHandle_t mtx) : m(mtx) {
        if (m) xSemaphoreTake(m, portMAX_DELAY);
    }
    ~ScopedFsLock() {
        if (m) xSemaphoreGive(m);
    }
};
} // namespace

void ShotHistory::scanSlots() {
    _presentSlots = 0;
    int maxSlot = -1;
    File dir = LittleFS.open(HISTORY_DIR);
    if (!dir || !dir.isDirectory()) return;

    File file = dir.openNextFile();
    while (file) {
        const char* name = file.name();
        const char* p = strrchr(name, '/');
        const char* base = p ? p + 1 : name;
        int slot = -1;
        if (sscanf(base, "%d.json", &slot) == 1 && slot >= 0 && slot < HISTORY_SLOTS) {
            _presentSlots |= (1 << slot);
            if (slot > maxSlot) maxSlot = slot;
        }
        file = dir.openNextFile();
    }

    if (_presentSlots == 0) {
        _nextSlot = 0;
    } else {
        int nextEmpty = -1;
        for (int i = 0; i < HISTORY_SLOTS; ++i) {
            if (!(_presentSlots & (1 << i))) {
                nextEmpty = i;
                break;
            }
        }
        if (nextEmpty >= 0) {
            _nextSlot = (uint8_t)nextEmpty;
        } else {
            _nextSlot = (uint8_t)((maxSlot + 1) % HISTORY_SLOTS);
        }
    }
}

void ShotHistory::begin() {
    if (!_fsMutex) {
        _fsMutex = xSemaphoreCreateMutex();
    }
    const size_t bufBytes = sizeof(ShotSample) * SB_HISTORY_ON_SHOT;
    if (!_samples) {
        _samples = static_cast<ShotSample*>(heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!_samples) _samples = static_cast<ShotSample*>(malloc(bufBytes));
        if (_samples) memset(_samples, 0, bufBytes);
    }
    if (!_frozen.samples) {
        _frozen.samples = static_cast<ShotSample*>(heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!_frozen.samples) _frozen.samples = static_cast<ShotSample*>(malloc(bufBytes));
        if (_frozen.samples) memset(_frozen.samples, 0, bufBytes);
    }
    if (!_writeScratch) {
        _writeScratch = static_cast<ShotSample*>(heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!_writeScratch) _writeScratch = static_cast<ShotSample*>(malloc(bufBytes));
        if (_writeScratch) memset(_writeScratch, 0, bufBytes);
    }
    ensureDir();
    scanSlots();
    _ready = (_samples != nullptr && _frozen.samples != nullptr && _writeScratch != nullptr);
}

void ShotHistory::ensureDir() {
    if (!LittleFS.exists(HISTORY_DIR)) {
        if (!LittleFS.begin(false, "/littlefs", 10, "spiffs") &&
            !LittleFS.begin(false, "/littlefs", 10, "littlefs") &&
            !LittleFS.begin(true,  "/littlefs", 10, "spiffs") &&
            !LittleFS.begin(true,  "/littlefs", 10, "littlefs")) {
            SB_LOGE("LittleFS mount failed - history disabled");
            return;
        }
        LittleFS.mkdir(HISTORY_DIR);
    }
}

void ShotHistory::resetCapture(const char* profId, const char* profName) {
    _count     = 0;
    _peakBar   = 0.0f;
    _doseG     = 0.0f;
    _settledDoseG = 0.0f;
    _capturing = true;
    if (profId && profName) {
        strlcpy(_profileId,   profId,   sizeof(_profileId));
        strlcpy(_profileName, profName, sizeof(_profileName));
    } else {
        const Profile& p = g_profile.profile();
        strlcpy(_profileId,   p.id,   sizeof(_profileId));
        strlcpy(_profileName, p.name, sizeof(_profileName));
    }
}

void ShotHistory::freezeShot(uint32_t shotDurationMs) {
    if (_count < 1 || !_samples || !_frozen.samples) {
        portENTER_CRITICAL(&_captureMux);
        _frozen.valid = false;
        portEXIT_CRITICAL(&_captureMux);
        return;
    }
    bool droppedOlder = false;
    portENTER_CRITICAL(&_captureMux);
    droppedOlder = _frozen.valid;
    std::swap(_frozen.samples, _samples);
    _frozen.count      = _count;
    _frozen.peakBar    = _peakBar;
    _frozen.cutoffG    = _doseG;
    _frozen.settledG   = _settledDoseG;
    const uint32_t lastSampleMs = (_frozen.count && _frozen.samples)
                                ? _frozen.samples[_frozen.count - 1].tMs : 0;
    // The shot's own brewing time. The tail samples that follow sit beyond it and must not stretch
    // it - the duration is how long the pump ran, not how long the cup kept filling.
    _frozen.durationMs = shotFrozenDurationMs(shotDurationMs, lastSampleMs);
    strlcpy(_frozen.profileId,   _profileId,   sizeof(_frozen.profileId));
    strlcpy(_frozen.profileName, _profileName, sizeof(_frozen.profileName));
    _frozen.valid = true;
    // This shot's capture ends here. The next shot starts its own with resetCapture().
    _capturing = false;
    portEXIT_CRITICAL(&_captureMux);

    if (droppedOlder) {
        // Two shots finished before the first one reached the flash. The older curve is lost, which
        // cannot happen in practice: the write happens within a telemetry cycle of the shot ending.
        SB_LOGW("A shot is still waiting to be written - the older curve is dropped");
    }
}

void ShotHistory::addTailSample(const ShotSample& s) {
    portENTER_CRITICAL(&_captureMux);
    if (shotTailRoute(_frozen.valid) == ShotTailRoute::LIVE_CAPTURE) {
        portEXIT_CRITICAL(&_captureMux);
        // The tail started before the machine released the AC, so the freeze has not happened yet.
        // The samples belong to the live capture, which the freeze will pick up.
        addSample(s);
        return;
    }
    if (_frozen.samples && _frozen.count < SB_HISTORY_ON_SHOT) {
        _frozen.samples[_frozen.count++] = s;
        // The tail can only raise what the shot recorded, so the stored curve ends at the highest
        // weight the scale showed - the same figure the ledger records as the yield.
        shotTailAccumulate(s.weightG, s.pBar, _frozen.cutoffG, _frozen.peakBar);
    }
    portEXIT_CRITICAL(&_captureMux);
}

void ShotHistory::setSettledDose(float g) {
    portENTER_CRITICAL(&_captureMux);
    _settledDoseG = g;
    if (_frozen.valid) _frozen.settledG = g;
    portEXIT_CRITICAL(&_captureMux);
}

float ShotHistory::doseG() const {
    portENTER_CRITICAL(&_captureMux);
    const float d = ledgerDoseG(_doseG, _settledDoseG);
    portEXIT_CRITICAL(&_captureMux);
    return d;
}

void ShotHistory::addSample(const ShotSample& s) {
    if (!_capturing || !_samples) return;
    if (_count >= SB_HISTORY_ON_SHOT) return;

    _samples[_count++] = s;
    if (s.pBar > _peakBar) _peakBar = s.pBar;
    if (s.weightG > _doseG)  _doseG   = s.weightG;
}

void ShotHistory::setWallTime(uint64_t epochMs) {
    _wallSyncedAtMs.store(millis(), std::memory_order_relaxed);
    _wallBaseMs.store(epochMs, std::memory_order_release);
    SB_LOGD("Wall time synced to epoch %llu ms", (unsigned long long)epochMs);
}

uint64_t ShotHistory::wallTimeNow() const {
    const uint64_t base = _wallBaseMs.load(std::memory_order_acquire);
    if (base == 0) {
        // No client has told the controller the time. It keeps its own clock synced from the
        // network, so that is used rather than the uptime - a shot stamped with the uptime would
        // show up in the ledger as a time in 1970.
        time_t now = time(nullptr);
        if (now > 1700000000) return (uint64_t)now * 1000ULL;
        return (uint64_t)millis();
    }
    const uint32_t syncedAt = _wallSyncedAtMs.load(std::memory_order_relaxed);
    return base + (uint64_t)(millis() - syncedAt);
}

int ShotHistory::saveShot() {
    ScopedFsLock lock(_fsMutex);
    if (!_ready || !_writeScratch) {
        return -1;
    }

    // Snapshot and swap the sample buffer out under _captureMux so the 100 Hz control task on Core 1
    // never blocks on LittleFS flash writes.
    bool     frozen = false;
    uint16_t count = 0;
    float    writtenDose = 0.0f;
    uint32_t durMs = 0;
    float    peak = 0.0f;
    char     profId[40] = {0};
    char     profName[SB_PROFILE_NAME_LEN] = {0};

    portENTER_CRITICAL(&_captureMux);
    frozen = _frozen.valid;
    if (!frozen) _capturing = false;
    count = frozen ? _frozen.count : _count;
    if (count >= 2) {
        writtenDose = frozen ? ledgerDoseG(_frozen.cutoffG, _frozen.settledG)
                             : ledgerDoseG(_doseG, _settledDoseG);
        durMs = frozen ? _frozen.durationMs
                       : ((_count && _samples) ? _samples[_count - 1].tMs : 0);
        peak  = frozen ? _frozen.peakBar : _peakBar;
        strlcpy(profId,   frozen ? _frozen.profileId   : _profileId,   sizeof(profId));
        strlcpy(profName, frozen ? _frozen.profileName : _profileName, sizeof(profName));
        if (frozen) {
            std::swap(_writeScratch, _frozen.samples);
            _frozen.valid = false;
        } else {
            std::swap(_writeScratch, _samples);
        }
    }
    portEXIT_CRITICAL(&_captureMux);

    if (count < 2) {
        SB_LOGW("Shot not saved: only %u samples captured.", count);
        return -1;
    }
    ensureDir();

    // Building the document chunk by chunk keeps peak RAM well under the 900-sample worst case.
    const uint64_t nowMs = wallTimeNow();
    char           slotName[32];
    snprintf(slotName, sizeof(slotName), "%s/%u.json", HISTORY_DIR, (unsigned)_nextSlot);
    char           tmpPath[32];
    snprintf(tmpPath, sizeof(tmpPath), "%s/tmp.json", HISTORY_DIR);

    File f = LittleFS.open(tmpPath, "w");
    if (!f) {
        SB_LOGE("Cannot write %s", tmpPath);
        return -1;
    }

    char head[512];
    snprintf(head, sizeof(head),
             "{\"slot\":%u,\"id\":\"%u-%llu\",\"profileId\":\"%s\",\"profileName\":\"%s\","
             "\"epochMs\":%llu,\"durationMs\":%lu,\"peakBar\":%.2f,\"doseG\":%.2f,\"dose_g\":%.2f,\"count\":%u,\"s\":[",
             _nextSlot, _nextSlot, (unsigned long long)nowMs,
             profId, profName,
             (unsigned long long)nowMs, (unsigned long)durMs, peak, writtenDose, writtenDose, count);
    f.print(head);

    const ShotSample* src = _writeScratch;
    char row[96];
    for (uint16_t i = 0; i < count; ++i) {
        const ShotSample& s = src[i];
        const int n = snprintf(row, sizeof(row),
                               "%s[%lu,%.2f,%.2f,%.2f,%u,%.1f,%.2f,%u]",
                               i ? "," : "", (unsigned long)s.tMs, s.pBar, s.pTarget, s.flowVirt,
                               s.psmPct, s.weightG, s.flowWeight, s.phaseIdx);
        f.write((const uint8_t*)row, n);
    }
    f.print("]}");
    f.close();

    if (!LittleFS.rename(tmpPath, slotName)) {
        LittleFS.remove(slotName);
        if (!LittleFS.rename(tmpPath, slotName)) {
            SB_LOGE("Failed to rename %s to %s", tmpPath, slotName);
            return -1;
        }
    }

    _presentSlots |= (1 << _nextSlot);

    SB_LOGI("Shot %u saved to %s (%u samples, %.1fs, peak %.2f bar, %.1f g)",
            _nextSlot, slotName, count, durMs / 1000.0f, peak, writtenDose);

    const int savedSlot = (int)_nextSlot;
    _nextSlot = (uint8_t)((_nextSlot + 1) % HISTORY_SLOTS);
    return savedSlot;
}

int ShotHistory::saveExternalShot(const char* profileId, const char* profileName,
                                  float peakBar, float doseG, uint32_t durationMs,
                                  uint64_t epochMs, const char* samplesJsonArray) {
    ScopedFsLock lock(_fsMutex);
    ensureDir();
    const uint8_t slot = _nextSlot;
    char slotName[32];
    snprintf(slotName, sizeof(slotName), "%s/%u.json", HISTORY_DIR, (unsigned)slot);
    char tmpPath[32];
    snprintf(tmpPath, sizeof(tmpPath), "%s/tmp_ext.json", HISTORY_DIR);

    File f = LittleFS.open(tmpPath, "w");
    if (!f) {
        SB_LOGE("Cannot write %s", tmpPath);
        return -1;
    }

    if (epochMs == 0) {
        epochMs = wallTimeNow();
    } else if (_wallBaseMs.load(std::memory_order_acquire) == 0 && epochMs > 1000000000000ULL) {
        setWallTime(epochMs);
    }

    char head[384];
    snprintf(head, sizeof(head),
             "{\"slot\":%u,\"id\":\"%u-%llu\",\"profileId\":\"%s\",\"profileName\":\"%s\","
             "\"epochMs\":%llu,\"durationMs\":%lu,\"peakBar\":%.2f,\"doseG\":%.2f,\"dose_g\":%.2f,\"count\":%u,\"s\":",
             slot, slot, (unsigned long long)epochMs,
             profileId ? profileId : "custom",
             profileName ? profileName : "Custom",
             (unsigned long long)epochMs, (unsigned long)durationMs,
             peakBar, doseG, doseG, 0);
    f.print(head);

    if (samplesJsonArray && samplesJsonArray[0] == '[') {
        f.print(samplesJsonArray);
    } else {
        f.print("[]");
    }
    f.print("}");
    f.close();

    if (!LittleFS.rename(tmpPath, slotName)) {
        LittleFS.remove(slotName);
        if (!LittleFS.rename(tmpPath, slotName)) {
            SB_LOGE("Failed to rename %s to %s", tmpPath, slotName);
            return -1;
        }
    }

    _presentSlots |= (1 << slot);
    _nextSlot = (uint8_t)((slot + 1) % HISTORY_SLOTS);
    SB_LOGI("External shot %u saved to %s (%.1fs, peak %.2f bar, %.1f g)",
            slot, slotName, durationMs / 1000.0f, peakBar, doseG);
    return (int)slot;
}

String ShotHistory::summaryJson() const {
    ScopedFsLock lock(_fsMutex);
    String out = "{\"ok\":true,\"count\":";
    out += String(count());
    out += ",\"shots\":[";
    bool first = true;
    for (int i = 0; i < HISTORY_SLOTS; ++i) {
        if (!(_presentSlots & (1 << i))) continue;
        char p[48];
        snprintf(p, sizeof(p), "%s/%d.json", HISTORY_DIR, i);
        File f = LittleFS.open(p, "r");
        if (!f) continue;
        char buf[384];
        size_t n = f.readBytes(buf, sizeof(buf) - 1);
        f.close();
        buf[n] = '\0';
        char* end = strstr(buf, ",\"s\":[");
        if (end) {
            *end = '}';
            *(end + 1) = '\0';
            if (!first) out += ",";
            first = false;
            out += buf;
        }
    }
    out += "]}";
    return out;
}

int ShotHistory::count() const {
    return __builtin_popcount(_presentSlots);
}

bool ShotHistory::readRaw(int slot, uint8_t** data, size_t* len) const {
    ScopedFsLock lock(_fsMutex);
    if (slot < 0 || slot >= HISTORY_SLOTS) return false;
    if (!(_presentSlots & (1 << slot))) return false;
    char p[48];
    snprintf(p, sizeof(p), "%s/%d.json", HISTORY_DIR, slot);
    File f = LittleFS.open(p, "r");
    if (!f) return false;

    const size_t sz = f.size();
    uint8_t* buf = (uint8_t*)malloc(sz + 1);
    if (!buf) { f.close(); return false; }
    size_t got = f.read(buf, sz);
    f.close();
    buf[got] = '\0';
    *data = buf;
    *len  = got;
    return true;
}

bool ShotHistory::readShot(int slot, String& jsonOut) const {
    uint8_t* buf = nullptr;
    size_t   len = 0;
    if (!readRaw(slot, &buf, &len)) return false;
    jsonOut = String((const char*)buf, len);
    free(buf);
    return true;
}

int ShotHistory::deleteShot(int slot) {
    ScopedFsLock lock(_fsMutex);
    if (slot < 0 || slot >= HISTORY_SLOTS) return -1;
    if (!(_presentSlots & (1 << slot))) return 0;
    char p[48];
    snprintf(p, sizeof(p), "%s/%d.json", HISTORY_DIR, slot);
    if (LittleFS.remove(p)) {
        _presentSlots &= ~(1 << slot);
        return 0;
    }
    return -1;
}

ShotHistory g_history;