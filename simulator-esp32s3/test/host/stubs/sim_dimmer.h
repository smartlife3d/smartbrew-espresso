#pragma once
#include <Arduino.h>
class SimDimmer { public:
    float measuredDutyPct() const { return _duty; }
    float pumpDrivePct() const { return _duty; }   // a steady stroke pattern at _duty %
    void setMainsActive(bool a) { _active = a; }
    bool isMainsActive() const { return _active; }
    float _duty = 0.0f;
    bool  _active = false;
};
extern SimDimmer g_simDimmer;
