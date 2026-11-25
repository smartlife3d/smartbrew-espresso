// =================================================================================================
//  test_physics.cpp - Host-side regression test for the hydraulic plant (SimPhysics)
//
//  Why this exists: two of the worst bugs in this simulator were invisible to the compiler and to
//  "does it build" - the plant silently never advanced, and the plant silently advanced only at
//  the wrong call rate. Both are timing bugs that only show up when the module is driven the way
//  the firmware actually drives it.
//
//  The critical detail: main.cpp calls SimPhysics::update() from the Arduino loop(), which spins
//  thousands of times per second. The module has to accumulate the elapsed time across calls
//  itself. If it advances its own timestamp on every call, the elapsed time never reaches the
//  10 ms step threshold and the plant never moves.
//
//  Build and run:  ./test/host/run.sh
// =================================================================================================
#include <Arduino.h>
#include "sim_physics.h"
#include "sim_dimmer.h"
#include "sim_button.h"
#include "sim_pressure.h"
#include "sim_ble_scale.h"

uint32_t g_fakeMicros = 1000000;
SerialStub Serial;
SimDimmer g_simDimmer;
SimButtonManager g_simButton;
SimPressure g_simPressure;
SimBleScale g_simBleScale;

static int failures = 0;

static void check(const char* what, bool ok, const char* detail) {
    printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) failures++;
}

int main() {
    g_simPhysics.begin();
    g_simButton._b = true;            // machine is BREWING
    g_simDimmer._duty = 36.0f;        // the controller's feed-forward duty for 9 bar

    // ---- Test 1: driven the way main.cpp's loop() drives it (tight loop) ----------------------
    printf("\nTest 1: 1 s of a tight loop (the firmware's real call pattern)\n");
    for (int k = 0; k < 10000; ++k) { g_fakeMicros += 100; g_simPhysics.update(); }
    char d[160];
    snprintf(d, sizeof(d), "P=%.3f bar (expect >0, headspace filling)", g_simPhysics.pressureBar());
    check("plant advanced with a tight loop", g_simPhysics.pressureBar() > 0.05f, d);
    snprintf(d, sizeof(d), "V=%.3f ml (expect >0)", g_simPhysics.volumePumpedMl());
    check("pump volume accumulated", g_simPhysics.volumePumpedMl() > 0.5f, d);

    // ---- Test 2: the pressure must reach the feed-forward target ------------------------------
    printf("\nTest 2: 20 s at a constant 36 %% duty (feed-forward duty for 9 bar)\n");
    for (int k = 0; k < 200000; ++k) { g_fakeMicros += 100; g_simPhysics.update(); }
    const float p = g_simPhysics.pressureBar();
    snprintf(d, sizeof(d), "P=%.2f bar (expect 8.0 - 11.0)", p);
    check("equilibrium pressure matches the FF line", p > 8.0f && p < 11.0f, d);

    const float q = g_simPhysics.flowMlPerSec();
    snprintf(d, sizeof(d), "Q=%.2f ml/s (expect 1.0 - 2.5)", q);
    check("flow through the puck is realistic", q > 1.0f && q < 2.5f, d);

    const float w = g_simPhysics.weightGrams();
    snprintf(d, sizeof(d), "W=%.1f g (expect > 20)", w);
    check("cup weight accumulated", w > 20.0f, d);

    // ---- Test 3: the plant must track a duty change -------------------------------------------
    printf("\nTest 3: duty change 36 %% -> 60 %% must raise the pressure\n");
    const float before = g_simPhysics.pressureBar();
    g_simDimmer._duty = 60.0f;
    for (int k = 0; k < 100000; ++k) { g_fakeMicros += 100; g_simPhysics.update(); }
    const float after = g_simPhysics.pressureBar();
    snprintf(d, sizeof(d), "%.2f -> %.2f bar", before, after);
    check("pressure rose with more duty", after > before + 0.3f, d);

    // ---- Test 4: the post-shot weight tail -----------------------------------------------------
    // The cutoff reading is not the yield: the last of the coffee is still draining out of the
    // grouphead after the pump stops, so the cup keeps getting heavier for a moment. The
    // controller's settling window follows exactly this tail.
    printf("\nTest 4: the cup keeps filling for ~1 s after the machine stops\n");
    g_simPhysics.setTailMs(1000);
    g_simButton._b = true;
    for (int k = 0; k < 20000; ++k) { g_fakeMicros += 100; g_simPhysics.update(); }
    const float atStop = g_simPhysics.weightGrams();
    snprintf(d, sizeof(d), "W at stop = %.2f g", atStop);
    check("the cup had coffee in it when the machine stopped", atStop > 1.0f, d);

    // The machine stops.
    g_simButton._b = false;
    for (int k = 0; k < 100; ++k) { g_fakeMicros += 100; g_simPhysics.update(); }
    const float justAfter = g_simPhysics.weightGrams();
    snprintf(d, sizeof(d), "W = %.2f g (was %.2f g at the stop)", justAfter, atStop);
    check("the cup is NOT taken off the scale at the stop", justAfter > atStop * 0.9f, d);
    check("the post-shot tail is running", g_simPhysics.tailActive(), "");

    // Half a second into the tail the cup has to be noticeably heavier.
    for (int k = 0; k < 5000; ++k) { g_fakeMicros += 100; g_simPhysics.update(); }
    const float midTail = g_simPhysics.weightGrams();
    snprintf(d, sizeof(d), "W = %.2f g (expect > %.2f g)", midTail, atStop);
    check("the cup got heavier during the tail", midTail > atStop + 0.2f, d);

    // Past the tail the cup comes off the scale, so the scale is back to 0 g for the next shot.
    for (int k = 0; k < 8000; ++k) { g_fakeMicros += 100; g_simPhysics.update(); }
    const float afterTail = g_simPhysics.weightGrams();
    snprintf(d, sizeof(d), "W = %.2f g (expect 0.00)", afterTail);
    check("the cup comes off the scale at the end of the tail", afterTail < 0.01f, d);
    check("the tail is over", !g_simPhysics.tailActive(), "");

    // ---- Test 5: a new shot cancels a tail that is still running -------------------------------
    printf("\nTest 5: starting a new shot cancels a tail that is still running\n");
    g_simButton._b = true;
    for (int k = 0; k < 20000; ++k) { g_fakeMicros += 100; g_simPhysics.update(); }
    g_simButton._b = false;
    for (int k = 0; k < 200; ++k) { g_fakeMicros += 100; g_simPhysics.update(); }
    check("the tail is running before the new shot", g_simPhysics.tailActive(), "");
    g_simButton._b = true;
    for (int k = 0; k < 200; ++k) { g_fakeMicros += 100; g_simPhysics.update(); }
    check("the new shot cancelled the tail", !g_simPhysics.tailActive(), "");
    snprintf(d, sizeof(d), "W = %.2f g (expect 0.00 - a new cup)", g_simPhysics.weightGrams());
    check("the new shot starts from a clean cup", g_simPhysics.weightGrams() < 0.01f, d);

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
