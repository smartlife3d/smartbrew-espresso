// =================================================================================================
//  test_weight_settle.cpp - Host regression tests for the post-shot weight tail
//
//  The cutoff reading is not the yield: when the pump stops, the last of the coffee is still
//  draining out of the grouphead, so the cup keeps getting heavier for a second or two. The
//  controller watches the scale through that tail and reports it, so every client can show the
//  real yield in the cup instead of the value that merely tripped the cutoff.
//
//  The rules that decide when the yield is final are timing rules, and a compile check cannot
//  catch a wrong one - so they are asserted here, driving the REAL header
//  (src/control/weight_settle.h) exactly the way the firmware drives it.
//
//  Usage:  ./test/host/run.sh
// =================================================================================================
#include "weight_settle.h"
#include "profile_types.h"

#include <cstdio>
#include <string>

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

static std::string fmtMs(uint32_t v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "closed at t=%u ms", (unsigned)v);
    return std::string(buf);
}

// -------------------------------------------------------------------------------------------------
int main() {
    printf("\nTest 1: a window that was never opened stays closed\n");
    {
        WeightSettle s;
        check(!s.active(), "a fresh window is not active");
        check(s.update(1000, 30.0f, true) == WeightSettleResult::SETTLED,
              "updating a closed window reports settled");
    }

    printf("\nTest 2: the cup still filling keeps the window open and tracks the maximum\n");
    {
        WeightSettle s;
        s.open(0, 36.2f);

        // The last of the coffee arrives: 36.2 -> 37.1 -> 37.6 g.
        check(s.update(200, 37.1f, true) == WeightSettleResult::OPEN, "a rising weight keeps it open");
        check(s.update(400, 37.6f, true) == WeightSettleResult::OPEN, "and keeps it open again");
        check(s.finalWeightG() == 37.6f, "the highest weight is kept",
              fmt("final=%.1f g", s.finalWeightG()));
    }

    printf("\nTest 3: a weight that stops moving settles after the stable window\n");
    {
        WeightSettle s;
        s.open(0, 36.2f);
        s.update(200, 37.1f, true);

        // The cup has stopped filling. It must NOT settle before the stable window is up.
        check(s.update(1000, 37.1f, true) == WeightSettleResult::OPEN,
              "still open before the stable window expires");
        check(s.update(2199, 37.1f, true) == WeightSettleResult::OPEN,
              "still open one millisecond before the stable window expires");
        check(s.update(2200, 37.1f, true) == WeightSettleResult::SETTLED,
              "settled once the stable window has passed");
        check(!s.active(), "the window is closed after settling");
        check(s.finalWeightG() == 37.1f, "the settled yield is the highest weight seen",
              fmt("final=%.1f g", s.finalWeightG()));
    }

    printf("\nTest 4: a rising weight restarts the stable window\n");
    {
        WeightSettle s;
        s.open(0, 36.2f);
        s.update(1500, 37.0f, true);     // the cup moved at t=1.5 s

        // t=2.0 s would have been past the original stable window, but the cup moved again.
        check(s.update(2000, 37.0f, true) == WeightSettleResult::OPEN,
              "the stable window restarted when the cup moved");
        check(s.update(3499, 37.0f, true) == WeightSettleResult::OPEN,
              "still open just before the restarted window expires");
        check(s.update(3500, 37.0f, true) == WeightSettleResult::SETTLED,
              "settled once the restarted window has passed");
    }

    printf("\nTest 5: lifting the cup settles the yield immediately and keeps the maximum\n");
    {
        WeightSettle s;
        s.open(0, 36.2f);
        s.update(200, 37.6f, true);

        // The operator picks the cup up: the scale drops to zero.
        check(s.update(400, 0.0f, true) == WeightSettleResult::LIFTED,
              "lifting the cup settles the window at once");
        check(!s.active(), "the window is closed");
        check(s.finalWeightG() == 37.6f, "the yield is the weight in the cup, not the empty scale",
              fmt("final=%.1f g", s.finalWeightG()));
    }

    printf("\nTest 6: a small dip is noise, not the cup being lifted\n");
    {
        WeightSettle s;
        s.open(0, 37.0f);

        // A droplet bouncing on the platform dips the reading a little. That must not end the
        // window - otherwise the yield would be frozen before the last of the coffee arrived.
        check(s.update(200, 36.8f, true) == WeightSettleResult::OPEN,
              "a dip below the lift threshold keeps the window open");
        check(s.active(), "the window is still open");
    }

    printf("\nTest 7: a disconnected scale settles the window at once\n");
    {
        WeightSettle s;
        s.open(0, 37.0f);
        check(s.update(200, 37.2f, false) == WeightSettleResult::DISCONNECTED,
              "a lost scale link settles the window");
        check(!s.active(), "the window is closed");
        check(s.finalWeightG() == 37.2f, "the last weight seen is kept",
              fmt("final=%.1f g", s.finalWeightG()));
    }

    printf("\nTest 8: the window never stays open forever\n");
    {
        WeightSettle s;
        s.open(0, 37.0f);

        // A scale that creeps upward forever must not hold the window open for ever.
        uint32_t t = 0;
        WeightSettleResult r = WeightSettleResult::OPEN;
        while (r == WeightSettleResult::OPEN && t < 30000) {
            t += 100;
            r = s.update(t, 37.0f + (t / 1000.0f) * 0.5f, true);
        }
        check(r == WeightSettleResult::TIMED_OUT, "a creeping scale times the window out");
        check(t >= WEIGHT_SETTLE_MAX_MS, "and not before the cap", fmtMs(t));
    }

    printf("\nTest 9: a settled window can be reopened for the next shot\n");
    {
        WeightSettle s;
        s.open(0, 36.2f);
        s.update(200, 37.6f, true);
        s.update(3000, 37.6f, true);
        check(!s.active(), "closed after settling");

        s.open(10000, 12.0f);
        check(s.active(), "reopened for the next shot");
        check(s.finalWeightG() == 12.0f, "the new shot starts from its own weight",
              fmt("final=%.1f g", s.finalWeightG()));
    }

    printf("\nTest 10: a timestamp a hair BEFORE the window opened is not 'expired'\n");
    {
        // This is the bug that killed the tail on the real controller. The window was opened with
        // the millisecond clock, while the first update arrived with the control tick's clock -
        // captured a few milliseconds EARLIER. The unsigned subtraction nowMs - _startMs wrapped
        // round to ~4.29e9 and read as "the window had already expired", so the settling window
        // closed on its very first tick and never saw the tail at all.
        WeightSettle s;
        s.open(10000, 34.9f);

        const WeightSettleResult r = s.update(9996, 34.9f, true);
        check(r == WeightSettleResult::OPEN,
              "a slightly stale timestamp does not expire the window");
        check(s.active(), "the window is still open");

        // And the window must still track the tail afterwards.
        check(s.update(10100, 35.6f, true) == WeightSettleResult::OPEN,
              "the window keeps following the cup");
        check(s.finalWeightG() == 35.6f, "and keeps the highest weight",
              fmt("final=%.1f g", s.finalWeightG()));
    }

    printf("\nTest 11: the same skew does not fake a 'stable' window either\n");
    {
        WeightSettle s;
        s.open(10000, 34.9f);
        // A stale timestamp must not be mistaken for "the weight has been still for 2 s".
        check(s.update(5000, 34.9f, true) == WeightSettleResult::OPEN,
              "a timestamp from long ago does not settle the window");
        check(s.active(), "the window is still open");
    }

    printf("\nTest 12: the ledger records the settled yield, not the cutoff reading\n");
    {
        // The brew ledger writes one number for the shot: the weight that ended up in the cup.
        // Until the tail has run out that number is the reading that tripped the cutoff.
        check(ledgerDoseG(34.9f, 0.0f) == 34.9f,
              "before the tail, the ledger holds the cutoff reading");
        check(ledgerDoseG(34.9f, 36.4f) == 36.4f,
              "after the tail, the ledger holds the settled yield");
        check(ledgerDoseG(34.9f, 34.9f) == 34.9f,
              "a tail that added nothing changes nothing");
        check(ledgerDoseG(34.9f, 30.0f) == 34.9f,
              "the ledger never goes backwards");
    }

    printf("\nTest 13: the ledger dose follows the tail the simulator produces\n");
    {
        // The number the ledger ends up with has to be the one the scale actually showed, not a
        // value derived from the cutoff.
        WeightSettle s;
        s.open(0, 34.9f);
        s.update(200, 35.6f, true);
        s.update(400, 36.1f, true);
        s.update(600, 36.4f, true);
        check(ledgerDoseG(34.9f, s.finalWeightG()) == 36.4f,
              "the ledger dose is the highest weight the scale showed",
              fmt("dose=%.1f g", ledgerDoseG(34.9f, s.finalWeightG())));
    }

    printf("\nALL TESTS %s (%d failure(s), %d check(s))\n",
           g_failures == 0 ? "PASSED" : "FAILED", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
