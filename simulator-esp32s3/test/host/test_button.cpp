// =================================================================================================
//  test_button.cpp - Host-side regression test for the brew-button handling (SimButtonManager)
//
//  Why this exists: the controller pulses the button TWICE per shot - once to start the machine
//  and once to stop it. An earlier version latched an "already acted" flag that was only cleared
//  on an *invalid* pulse, so the very first controller pulse worked and every later pulse was
//  silently swallowed. The machine could therefore never be stopped by the controller: the
//  controller waited 3 s for the AC to disappear, retried, and ended up in FAULT.
//
//  This test drives the real SimButtonManager through a fake GPIO so the edge/debounce logic is
//  exercised exactly as the firmware exercises it.
//
//  Build and run:  ./test/host/run.sh
// =================================================================================================
#include <Arduino.h>
#include "../../src/sim_button.h"
#include "sim_dimmer.h"          // stub: the real dimmer is not needed for the button logic

uint32_t g_fakeMicros = 1000000;
uint64_t g_fakeUs = 1000000;
volatile uint32_t g_fakeGpioIn = 0;
int g_fakePinLevel = 0;
int g_fakeBaristaLevel = 1;          // barista button (GPIO 0) idle = HIGH
isr_handler_t g_fakeIsr = nullptr;
SerialStub Serial;
SimDimmer g_simDimmer;

static int failures = 0;

static void check(const char* what, bool ok, const char* detail) {
    printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) failures++;
}

// Drive the controller's button pin: HIGH for `ms`, then LOW.
static void pulse(uint32_t ms) {
    g_fakePinLevel = 1;
    g_fakeGpioIn = 1u << 6;          // bit 6 = GPIO6 (the ISR shifts by the pin number)
    if (g_fakeIsr) g_fakeIsr();
    for (uint32_t t = 0; t < ms; t += 10) {
        g_fakeUs += 10000; g_fakeMicros += 10000;
        g_simButton.update();
    }
    g_fakePinLevel = 0;
    g_fakeGpioIn = 0;
    if (g_fakeIsr) g_fakeIsr();
    g_fakeUs += 10000; g_fakeMicros += 10000; g_simButton.update();
    g_fakeUs += 20000; g_fakeMicros += 20000; g_simButton.update();
}

int main() {
    g_simButton.begin();

    char d[160];
    check("starts idle", !g_simButton.isBrewing(), "state = IDLE");

    // ---- Test 1: the START pulse turns the machine on ------------------------------------------
    pulse(180);
    snprintf(d, sizeof(d), "state = %s, pulses = %lu",
             g_simButton.isBrewing() ? "BREWING" : "IDLE",
             (unsigned long)g_simButton.controllerPulseCount());
    check("controller pulse 1 starts the machine", g_simButton.isBrewing(), d);

    // ---- Test 2: the STOP pulse turns it back off ---------------------------------------------
    pulse(180);
    snprintf(d, sizeof(d), "state = %s, pulses = %lu",
             g_simButton.isBrewing() ? "BREWING" : "IDLE",
             (unsigned long)g_simButton.controllerPulseCount());
    check("controller pulse 2 STOPS the machine", !g_simButton.isBrewing(), d);

    // ---- Test 3: every further pulse must keep working ----------------------------------------
    bool allHandled = true;
    for (int i = 3; i <= 6; ++i) {
        pulse(180);
        const bool expected = (i % 2 == 1);       // 3,5 -> on ; 4,6 -> off
        if (g_simButton.isBrewing() != expected) allHandled = false;
    }
    snprintf(d, sizeof(d), "pulses = %lu (expect 6)", (unsigned long)g_simButton.controllerPulseCount());
    check("pulses 3-6 are all handled", allHandled && g_simButton.controllerPulseCount() == 6, d);

    // ---- Test 4: a sub-20 ms glitch must not toggle the machine ------------------------------
    const bool stateBefore = g_simButton.isBrewing();
    pulse(8);
    snprintf(d, sizeof(d), "state unchanged = %s", g_simButton.isBrewing() == stateBefore ? "yes" : "NO");
    check("8 ms glitch is rejected", g_simButton.isBrewing() == stateBefore, d);

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
