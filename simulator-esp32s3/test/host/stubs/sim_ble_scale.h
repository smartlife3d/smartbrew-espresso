#pragma once
#include <Arduino.h>
class SimBleScale { public:
    void setWeight(float g){ _w=g; }
    void notifyNow(){}
    float _w=0;
};
extern SimBleScale g_simBleScale;
