// =================================================================================================
//  test_shot_tail.cpp - Host-side regression test for the post-shot tail rules
//
//  Covers src/control/shot_tail.h, which holds the rules that decide where a post-shot tail
//  sample belongs and what it contributes to the shot:
//
//    1. the tail is routed to the finished shot, never to the next one;
//    2. the tail can only RAISE the recorded weight and peak, so the stored curve ends at the
//       highest weight the scale showed - the same figure the ledger records as the yield;
//    3. the tail holds the gauges: the pressure and the flow keep the reading they had when the
//       pump stopped, so the curve does not trail away into a collapse of zeros;
//    4. the tail must not stretch the recorded brewing time.
//
//  Without these rules the stored curve stops at the reading which tripped the cutoff, and the
//  ledger's graph contradicts the ledger's own weight.
//
//  Usage:  compiled and run by test/host/run.sh
// =================================================================================================
#include <cstdio>
#include <cstdint>

#include "shot_tail.h"

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what);
    } else {
        std::printf("  [PASS] %s\n", what);
    }
}

static void checkEqF(float got, float want, const char* what) {
    ++g_checks;
    const bool ok = (got == want);
    if (!ok) {
        ++g_failures;
        std::printf("  [FAIL] %s  got=%.2f want=%.2f\n", what, got, want);
    } else {
        std::printf("  [PASS] %s  (%.2f)\n", what, got);
    }
}

static void checkEqU(uint32_t got, uint32_t want, const char* what) {
    ++g_checks;
    const bool ok = (got == want);
    if (!ok) {
        ++g_failures;
        std::printf("  [FAIL] %s  got=%lu want=%lu\n", what, (unsigned long)got, (unsigned long)want);
    } else {
        std::printf("  [PASS] %s  (%lu ms)\n", what, (unsigned long)got);
    }
}

// ------------------------------------------------------------------------------------------------
int main() {
    std::printf("Post-shot tail: where the samples go\n");

    // The shot has not been frozen yet - the tail still belongs to the live capture.
    check(shotTailRoute(false) == ShotTailRoute::LIVE_CAPTURE,
          "a tail sample before the freeze goes to the live capture");

    // The shot is frozen and waiting to be written - the tail must go to that record, so it can
    // never leak into the next shot's capture.
    check(shotTailRoute(true) == ShotTailRoute::FROZEN_SHOT,
          "a tail sample after the freeze goes to the finished shot");

    std::printf("\nPost-shot tail: what it contributes to the shot\n");

    // The cut-off reading so far.
    float cutoffG = 34.9f;
    float peakBar = 9.51f;

    // The last of the coffee arrives: the scale reads higher than it did at the stop.
    shotTailAccumulate(36.49f, 1.2f, cutoffG, peakBar);
    checkEqF(cutoffG, 36.49f, "the tail raises the recorded weight to the yield");
    checkEqF(peakBar, 9.51f, "the tail does not lower the peak pressure");

    // A noisy reading must never walk the recorded weight back down.
    shotTailAccumulate(36.10f, 0.4f, cutoffG, peakBar);
    checkEqF(cutoffG, 36.49f, "a lower tail reading does not walk the yield back");
    checkEqF(peakBar, 9.51f, "a lower tail pressure does not walk the peak back");

    // A fresh high reading is still allowed to raise it.
    shotTailAccumulate(36.6f, 2.0f, cutoffG, peakBar);
    checkEqF(cutoffG, 36.6f, "a higher tail reading raises the yield again");

    std::printf("\nPost-shot tail: the gauges the tail shows\n");

    // The gauges the tail holds.
    float heldP = 0.0f;
    float heldF = 0.0f;

    // While the pump is running, the held values follow the live readings.
    shotTailHoldGauges(9.42f, 2.8f, true, heldP, heldF);
    checkEqF(heldP, 9.42f, "the held pressure follows the live reading while the pump runs");
    checkEqF(heldF, 2.8f, "the held flow follows the live reading while the pump runs");

    // The pump stops: the live readings collapse, but the tail keeps the shot as it ended.
    shotTailHoldGauges(0.3f, 0.0f, false, heldP, heldF);
    checkEqF(heldP, 9.42f, "the tail holds the pressure the gauges had when the pump stopped");
    checkEqF(heldF, 2.8f, "the tail holds the flow the gauges had when the pump stopped");

    // A collapsed reading after the stop must not move the held values either.
    shotTailHoldGauges(0.0f, 0.0f, false, heldP, heldF);
    checkEqF(heldP, 9.42f, "a collapsed reading after the stop does not move the held pressure");
    checkEqF(heldF, 2.8f, "a collapsed reading after the stop does not move the held flow");

    // The next shot moves the gauges again - the held values belong to one shot only.
    shotTailHoldGauges(8.10f, 2.1f, true, heldP, heldF);
    checkEqF(heldP, 8.10f, "the next shot moves the held pressure again");
    checkEqF(heldF, 2.1f, "the next shot moves the held flow again");

    std::printf("\nPost-shot tail: the recorded brewing time\n");

    // The tail samples sit beyond the end of the shot, so they must not stretch it.
    checkEqU(shotFrozenDurationMs(20550, 25550), 20550,
             "the tail does not stretch the recorded brewing time");

    // When the brewing time was not passed in, the last sample is the fallback.
    checkEqU(shotFrozenDurationMs(0, 25550), 25550,
             "the last sample is the fallback when no brewing time was given");

    // A shot with no samples at all still has a duration of zero rather than a garbage value.
    checkEqU(shotFrozenDurationMs(0, 0), 0, "an empty shot has no duration");

    std::printf("\n%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
