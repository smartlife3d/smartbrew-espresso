#pragma once
#include <Arduino.h>
class SimButtonManager { public:
    bool isBrewing() const { return _b; }
    void update() {}
    bool _b = false;
};
extern SimButtonManager g_simButton;
