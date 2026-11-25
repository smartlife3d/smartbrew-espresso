// =================================================================================================
//  test_transitions.cpp - Host regression tests for the phase transition reason
//
//  A phase change is reported in the controller log with the reason it fired. A silent or wrong
//  reason defeats the whole point of the log: the operator cannot tell whether a phase ended
//  because the weight target was reached, because the pressure crossed a threshold, or because
//  the phase timed out.
//
//  These tests compile the REAL shared header (include/profile_types.h) natively, so they cover
//  the same code the firmware runs.
//
//  Usage:  ./test/host/run.sh
// =================================================================================================
#include "profile_types.h"

#include <cstdio>
#include <cstring>
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

static std::string reason(const Transition& tr, float p, float w, float flow, float t) {
    char buf[96];
    describeTransitionReason(tr, p, w, flow, t, buf, sizeof(buf));
    return std::string(buf);
}

// -------------------------------------------------------------------------------------------------
int main() {
    printf("\nTest 1: a weight trigger names the measured value and the threshold\n");
    {
        Transition tr;
        tr.durationSec = 20.0f;
        tr.triggerCount = 1;
        tr.triggers[0].type = TriggerType::WEIGHT_GE;
        tr.triggers[0].value = 12.0f;

        const std::string r = reason(tr, 0.0f, 36.2f, 0.0f, 10.0f);
        check(r == "weight 36.2 g >= 12.0 g", "weight reason names both numbers", r);
    }

    printf("\nTest 2: a pressure trigger is named too\n");
    {
        Transition tr;
        tr.durationSec = 20.0f;
        tr.triggerCount = 1;
        tr.triggers[0].type = TriggerType::PRESSURE_GE;
        tr.triggers[0].value = 9.0f;

        const std::string r = reason(tr, 9.1f, 0.0f, 0.0f, 5.0f);
        check(r == "pressure 9.10 bar >= 9.00 bar", "pressure reason names both numbers", r);
    }

    printf("\nTest 3: a trigger that has NOT fired produces no reason\n");
    {
        Transition tr;
        tr.durationSec = 20.0f;
        tr.triggerCount = 1;
        tr.triggers[0].type = TriggerType::WEIGHT_GE;
        tr.triggers[0].value = 12.0f;

        const std::string r = reason(tr, 0.0f, 3.0f, 0.0f, 5.0f);
        check(r == "the transition condition was met", "unsatisfied trigger falls back", r);
    }

    printf("\nTest 4: OR logic joins the satisfied triggers with ' OR '\n");
    {
        Transition tr;
        tr.durationSec = 30.0f;
        tr.logic = TriggerLogic::OR;
        tr.triggerCount = 2;
        tr.triggers[0].type = TriggerType::WEIGHT_GE;
        tr.triggers[0].value = 12.0f;
        tr.triggers[1].type = TriggerType::PRESSURE_GE;
        tr.triggers[1].value = 9.0f;

        // Only the weight trigger is satisfied.
        const std::string r = reason(tr, 5.0f, 13.0f, 0.0f, 6.0f);
        check(r == "weight 13.0 g >= 12.0 g", "OR lists only the satisfied trigger", r);
    }

    printf("\nTest 5: AND logic joins the satisfied triggers with ' AND '\n");
    {
        Transition tr;
        tr.durationSec = 30.0f;
        tr.logic = TriggerLogic::AND;
        tr.triggerCount = 2;
        tr.triggers[0].type = TriggerType::WEIGHT_GE;
        tr.triggers[0].value = 12.0f;
        tr.triggers[1].type = TriggerType::PRESSURE_GE;
        tr.triggers[1].value = 9.0f;

        // Both are satisfied now.
        const std::string r = reason(tr, 9.4f, 13.0f, 0.0f, 6.0f);
        check(r.find(" AND ") != std::string::npos, "AND joins the two triggers", r);
        check(r.find("weight 13.0 g >= 12.0 g") != std::string::npos, "AND names the weight", r);
        check(r.find("pressure 9.40 bar >= 9.00 bar") != std::string::npos, "AND names the pressure", r);
    }

    printf("\nTest 6: the phase timeout wins over the triggers\n");
    {
        Transition tr;
        tr.durationSec = 8.0f;
        tr.triggerCount = 1;
        tr.triggers[0].type = TriggerType::WEIGHT_GE;
        tr.triggers[0].value = 12.0f;

        // The weight trigger is NOT satisfied, but the phase ran past its duration.
        const std::string r = reason(tr, 0.0f, 1.0f, 0.0f, 9.0f);
        check(r == "phase timeout after 8.0 s", "timeout is reported as the reason", r);
    }

    printf("\nTest 7: a phase with no triggers reports its duration\n");
    {
        Transition tr;
        tr.durationSec = 4.0f;
        tr.triggerCount = 0;

        const std::string r = reason(tr, 0.0f, 0.0f, 0.0f, 4.5f);
        check(r == "duration 4.0 s elapsed", "duration-only phase reports its duration", r);
    }

    printf("\nTest 8: the reason buffer is always terminated\n");
    {
        Transition tr;
        tr.durationSec = 20.0f;
        tr.logic = TriggerLogic::AND;
        tr.triggerCount = SB_MAX_TRIGGERS;
        for (uint8_t i = 0; i < SB_MAX_TRIGGERS; ++i) {
            tr.triggers[i].type = TriggerType::WEIGHT_GE;
            tr.triggers[i].value = 1.0f;
        }

        char tiny[8];
        memset(tiny, 'X', sizeof(tiny));
        describeTransitionReason(tr, 0.0f, 99.0f, 0.0f, 5.0f, tiny, sizeof(tiny));
        check(tiny[sizeof(tiny) - 1] == '\0', "a short buffer is still terminated");

        char big[96];
        memset(big, 'X', sizeof(big));
        describeTransitionReason(tr, 0.0f, 99.0f, 0.0f, 5.0f, big, sizeof(big));
        check(big[sizeof(big) - 1] == '\0', "a full buffer is still terminated");
    }

    printf("\nTest 9: a flow trigger and a time trigger are named\n");
    {
        Transition tr;
        tr.durationSec = 30.0f;
        tr.triggerCount = 1;
        tr.triggers[0].type = TriggerType::FLOW_GE;
        tr.triggers[0].value = 2.5f;

        const std::string r = reason(tr, 0.0f, 0.0f, 3.2f, 4.0f);
        check(r == "flow 3.20 ml/s >= 2.50 ml/s", "flow reason names both numbers", r);
    }
    {
        Transition tr;
        tr.durationSec = 30.0f;
        tr.triggerCount = 1;
        tr.triggers[0].type = TriggerType::TIME_SEC;
        tr.triggers[0].value = 5.0f;

        const std::string r = reason(tr, 0.0f, 0.0f, 0.0f, 5.2f);
        check(r == "phase time 5.2 s >= 5.0 s", "time reason names both numbers", r);
    }

    printf("\nALL TESTS %s (%d failure(s), %d check(s))\n",
           g_failures == 0 ? "PASSED" : "FAILED", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
