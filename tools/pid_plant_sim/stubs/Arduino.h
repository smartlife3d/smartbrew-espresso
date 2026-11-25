#pragma once
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
extern uint32_t g_ms;
inline uint32_t millis() { return g_ms; }
inline size_t strlcpy(char* d, const char* s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l < n-1 ? l : n-1; memcpy(d,s,c); d[c]=0; } return l; }
template<class T> T constrain(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
