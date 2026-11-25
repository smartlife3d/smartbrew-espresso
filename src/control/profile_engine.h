// =================================================================================================
//  profile_engine.h - Multi-phase profile interpreter with AND/OR exit conditions
//                      docs/Profiles.md
// =================================================================================================
#pragma once

#include <Arduino.h>
#include "profile_types.h"

class ProfileEngine {
public:
    void begin();

    // ---- profile storage -----------------------------------------------------------------------
    bool loadActive(const char* id);          // "" -> the profile flagged active in /profiles/index.json
    bool loadById(const char* id);
    bool parseJson(const char* json, Profile& out, char* err, size_t errLen);
    void setActive(const Profile& p)   { _p = p; _loaded = true; _dirty = true; }
    const Profile& profile() const    { return _p; }
    bool loaded() const               { return _loaded; }

    // ---- run control ---------------------------------------------------------------------------
    void start(uint32_t nowMs = 0);           // reset runtime state, enter phase 0
    void stop();                              // freeze; caller is responsible for cutting power
    void nextPhase(uint32_t nowMs = 0);       // `NEXT_PHASE` command; pass the control tick's clock
    void overrideTarget(float bar);           // manual pressure slider for the current phase

    // ---- 100 Hz tick ---------------------------------------------------------------------------
    // Returns the setpoint the PID loop should chase for this tick.
    float tick(float pressureBar, float weightG, float flowVirt, uint32_t nowMs, bool isSimulation = false);

    // A short human-readable description of why the last phase transition fired, e.g.
    // "weight 36.2 g >= 12.0 g". Empty until a transition has fired.
    const char* transitionReason() const { return _transReason; }

    // ---- observation ---------------------------------------------------------------------------
    uint8_t  phaseIndex() const   { return _phaseIdx; }
    const char* phaseName() const;
    uint8_t  phaseCount() const   { return _p.phaseCount; }
    float    phaseElapsed() const { return _phaseElapsedSec; }
    float    phaseProgress() const;           // 0..1 through the phase's duration
    float    currentTarget() const{ return _currentTarget; }
    bool     running() const      { return _running; }
    bool     finished() const     { return _finished; }
    uint32_t startTimeMs() const  { return _startMs; }
    uint16_t elapsedMs() const    { return _running || _finished ? (uint16_t)(millis() - _startMs) : 0; }
    const Transition* activeTransition() const {
        return (_phaseIdx < _p.phaseCount) ? &_p.phases[_phaseIdx].transition : nullptr;
    }
    const Phase& currentPhase() const {
        static const Phase dummy{};
        return (_phaseIdx < _p.phaseCount) ? _p.phases[_phaseIdx] : dummy;
    }

    // ---- boot profile --------------------------------------------------------------------------
    static const char* builtinProfiles();
    static const char* defaultProfileJson();
    static const char* getPresetJson(const char* id);

    // ---- built-in profile seeding ----------------------------------------------------------------
    // A profile file written from a built-in preset carries "seed_version". A file the operator
    // saved never does (the save endpoint strips it), so the seeding at boot can tell the two apart:
    // it refreshes an outdated seed and never touches a profile somebody edited.
    // Bump SB_SEED_PROFILE_VERSION whenever a built-in preset changes.
    static constexpr int SB_SEED_PROFILE_VERSION = 2;   // v2: power caps 100 %, community presets
    static String seedJson(const char* id);                // the preset with its seed_version, "" if none
    static int    seedVersionOf(const String& fileJson);   // -1 = no seed_version (operator-owned)

private:
    float targetForPhase(const Phase& ph, float tNorm) const;
    void  enterPhase(uint8_t idx, uint32_t nowMs = 0);
    bool  transitionSatisfied(const Transition& tr, float pressure, float weight,
                              float flowVirt, float elapsedSec, bool isSimulation = false);
    // Works out why the transition fired and stores a short description in _transReason.
    void  buildTransitionReason(const Transition& tr, float pressure, float weight,
                                 float flowVirt, float elapsedSec, bool isSimulation = false);

    Profile _p{};
    bool    _loaded = false;
    bool    _dirty  = false;

    uint8_t  _phaseIdx = 0;
    bool     _running = false;
    bool     _finished = false;
    bool     _override = false;
    float    _overrideBar = 0.0f;
    uint32_t _startMs = 0;
    uint32_t _phaseStartMs = 0;
    float    _phaseElapsedSec = 0.0f;
    float    _currentTarget = 0.0f;
    char     _transReason[96] = {0};
};

extern ProfileEngine g_profile;