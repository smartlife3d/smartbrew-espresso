// =================================================================================================
//  test_settle_chain.cpp - Host integration test: simulator tail -> controller settling window
//
//  The simulator models the last of the coffee still draining into the cup after the pump stops.
//  The controller's WeightSettle follows exactly that tail so the clients can show the real yield
//  in the cup instead of the value that merely tripped the cutoff.
//
//  This test drives the REAL simulator physics and feeds the result into the REAL controller
//  settling rules, so the whole chain is checked end to end - the part that a unit test of either
//  side on its own cannot cover.
//
//  Usage:  ./test/host/run.sh
// =================================================================================================
#include <Arduino.h>
#include "sim_physics.h"
#include "sim_dimmer.h"
#include "sim_button.h"
#include "sim_pressure.h"
#include "sim_ble_scale.h"
#include "weight_settle.h"
#include "profile_types.h"

#include <cstdio>
#include <string>

uint32_t g_fakeMicros = 1000000;
SerialStub Serial;
SimDimmer g_simDimmer;
SimButtonManager g_simButton;
SimPressure g_simPressure;
SimBleScale g_simBleScale;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what, const std::string& detail = "") {
    ++g_checks;
    if (ok) {
        printf("  [PASS] %s%s%s\n", what, detail.empty() ? "" : "  ", detail.c_str());
    } else {
        ++g_failures;
        printf("  [FAIL] %s%s%s\n", what, detail.empty() ? "" : "  ", detail.c_str());
    }
}

static std::string fmt(const char* f, float v) {
    char buf[64];
    snprintf(buf, sizeof(buf), f, v);
    return std::string(buf);
}

static std::string fmt2(const char* f, float a, float b) {
    char buf[96];
    snprintf(buf, sizeof(buf), f, a, b);
    return std::string(buf);
}

int main() {
    g_simPhysics.begin();
    g_simPhysics.setTailMs(1000);
    g_simButton._b = true;             // the machine is brewing
    g_simDimmer._duty = 36.0f;         // the controller's feed-forward duty for 9 bar

    printf("\nIntegration: the simulator's post-shot tail feeds the controller's settling window\n");

    // ---- 1. Brew for 8 s -----------------------------------------------------------------------
    for (int k = 0; k < 80000; ++k) { g_fakeMicros += 100; g_simPhysics.update(); }
    const float atStop = g_simPhysics.weightGrams();
    check(atStop > 5.0f, "the cup has coffee in it when the machine stops",
          fmt("W=%.2f g", atStop));

    // ---- 2. The machine stops. The controller opens its settling window at this point -----------
    // The controller captures ONE clock per control tick and uses it for everything in that tick -
    // including opening the window and feeding it the update. The test does the same, because the
    // bug that killed the tail was exactly a mismatch between two clocks.
    g_simButton._b = false;
    WeightSettle settle;
    {
        const uint32_t tickNow = millis();
        settle.open(tickNow, atStop);
        check(settle.active(), "the controller's settling window is open at the stop");

        // The very first update of that same tick, with the very same timestamp.
        const WeightSettleResult first = settle.update(tickNow, atStop, true);
        check(first == WeightSettleResult::OPEN,
              "the first update of the tick that opened the window does not expire it");
    }

    // ---- 3. Feed the simulator's weight into the settling rules, as the 100 Hz tick does -------
    bool   sawSettling = false;
    float  maxSeen = atStop;
    float  firstTailWeight = -1.0f;
    WeightSettleResult last = WeightSettleResult::OPEN;

    for (int k = 0; k < 30000; ++k) {          // up to 3 s
        g_fakeMicros += 100;
        g_simPhysics.update();

        const float w = g_simPhysics.weightGrams();
        if (w > maxSeen) maxSeen = w;
        if (firstTailWeight < 0.0f && w > atStop + 0.05f) firstTailWeight = w;

        last = settle.update(millis(), w, true);
        if (settle.active()) sawSettling = true;
        if (last != WeightSettleResult::OPEN) break;
    }

    check(sawSettling, "the settling window stayed open through the tail");
    check(firstTailWeight > 0.0f, "the simulator actually pushed the tail weight up",
          firstTailWeight > 0.0f ? fmt("W=%.2f g", firstTailWeight) : "");
    check(settle.finalWeightG() > atStop,
          "the settled yield is higher than the reading that stopped the shot",
          fmt2("cutoff=%.2f g -> yield=%.2f g", atStop, settle.finalWeightG()));
    check(settle.finalWeightG() >= maxSeen - 0.01f,
          "the settled yield is the highest weight the scale showed",
          fmt2("yield=%.2f g, max seen=%.2f g", settle.finalWeightG(), maxSeen));
    check(last == WeightSettleResult::LIFTED,
          "the window closed because the cup came off the scale",
          last == WeightSettleResult::LIFTED ? "LIFTED" : "a different reason");

    // ---- 4. The cup is gone, so the scale reads 0 g for the next shot --------------------------
    const float afterTail = g_simPhysics.weightGrams();
    check(afterTail < 0.01f, "the scale is back to 0 g for the next shot",
          fmt("W=%.2f g", afterTail));

    // ---- 5. The number the brew ledger records is the yield in the cup -------------------------
    // The ledger writes one dose for the shot. It must be the weight that ended up in the cup,
    // not the reading that merely tripped the cutoff.
    const float ledgerDose = ledgerDoseG(atStop, settle.finalWeightG());
    check(ledgerDose > atStop,
          "the ledger dose is higher than the cutoff reading",
          fmt2("cutoff=%.2f g -> ledger=%.2f g", atStop, ledgerDose));
    check(ledgerDose == settle.finalWeightG(),
          "the ledger dose is the settled yield",
          fmt("ledger=%.2f g", ledgerDose));

    printf("\nALL TESTS %s (%d failure(s), %d check(s))\n",
           g_failures == 0 ? "PASSED" : "FAILED", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
