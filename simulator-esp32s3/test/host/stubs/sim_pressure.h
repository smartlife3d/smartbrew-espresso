#pragma once
#include <Arduino.h>
class SimPressure { public: void setBar(float b){ _bar=b; } float _bar=0; };
extern SimPressure g_simPressure;
