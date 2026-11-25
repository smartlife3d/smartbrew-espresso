// =================================================================================================
//  test_pump_flow.cpp - Host-side regression test for the pump flow model and the scale flow rules
//
//  Covers docs/PumpFlow.md, driving the controller's own headers:
//    include/pump_physics.h       virtual flow, its inverse (FLOW feed-forward), compliance
//    src/control/scale_flow.h     time-windowed weight slope, learned drip lag
//
//  Build and run:  ./test/host/run.sh
// =================================================================================================
#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <stdint.h>

#include "pump_physics.h"
#include "scale_flow.h"
#include "flow_calibration.h"

static int failures = 0;
static void check(const char* what, bool ok, const char* detail) {
    printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) failures++;
}

// Ulka EX5 preset (include/pump_presets.h)
constexpr float kMaxFlow = 10.8f;
constexpr float kStall   = 15.0f;

int main() {
    char d[200];

    // ---- F-6: the exponent is a parameter, and its default reproduces the old model ----------
    printf("\nPump model: P-Q exponent\n");
    {
        const float oldModel = 0.5f * kMaxFlow * (1.0f - powf(6.0f / kStall, 1.35f));
        const float dflt     = calculateVirtualFlow(6.0f, 50.0f, kStall, kMaxFlow);
        snprintf(d, sizeof(d), "%.4f vs %.4f ml/s", dflt, oldModel);
        check("default exponent equals the old 1.35 model", fabsf(dflt - oldModel) < 1e-5f, d);

        const float soft = calculateVirtualFlow(6.0f, 50.0f, kStall, kMaxFlow, 1.0f);
        const float hard = calculateVirtualFlow(6.0f, 50.0f, kStall, kMaxFlow, 2.0f);
        snprintf(d, sizeof(d), "exp 1.0 -> %.2f, exp 2.0 -> %.2f ml/s", soft, hard);
        check("a larger exponent keeps more flow at 6 bar", hard > dflt && dflt > soft, d);

        const float atStall = calculateVirtualFlow(kStall, 100.0f, kStall, kMaxFlow, 1.8f);
        snprintf(d, sizeof(d), "%.3f ml/s", atStall);
        check("no flow at the stall pressure", atStall == 0.0f, d);
    }

    // ---- F-1: the FLOW feed-forward is the inverse of the model -------------------------------
    printf("\nFLOW feed-forward\n");
    {
        const float duty = calculateDutyForFlow(2.0f, 6.0f, kStall, kMaxFlow);
        snprintf(d, sizeof(d), "%.1f %% (old fixed line gave %.1f %%)", duty, 2.0f * 18.0f + 15.0f);
        // 6/15 = 0.4, 0.4^1.35 = 0.290, so the pump can deliver 10.8 * 0.710 = 7.67 ml/s at full
        // duty and 2 ml/s needs 26.1 %.
        check("2 ml/s at 6 bar needs ~26 % duty", duty > 25.5f && duty < 26.7f, d);

        bool roundTrip = true;
        for (float p = 0.0f; p <= 12.0f; p += 1.5f) {
            for (float q = 0.5f; q <= 4.0f; q += 0.5f) {
                const float u = calculateDutyForFlow(q, p, kStall, kMaxFlow, 1.35f);
                if (u >= 100.0f) continue;   // past what the pump can deliver
                const float back = calculateVirtualFlow(p, u, kStall, kMaxFlow, 1.35f);
                if (fabsf(back - q) > 1e-3f) roundTrip = false;
            }
        }
        check("duty -> flow -> duty round-trips over 0-12 bar", roundTrip, "");

        const float low  = calculateDutyForFlow(2.0f, 2.0f, kStall, kMaxFlow);
        const float high = calculateDutyForFlow(2.0f, 10.0f, kStall, kMaxFlow);
        snprintf(d, sizeof(d), "%.1f %% at 2 bar, %.1f %% at 10 bar", low, high);
        check("more back-pressure needs more duty", high > low, d);

        const float stalled = calculateDutyForFlow(2.0f, kStall, kStall, kMaxFlow);
        snprintf(d, sizeof(d), "%.1f %%", stalled);
        check("at stall the full duty is asked for", stalled == 100.0f, d);
        check("zero target needs zero duty", calculateDutyForFlow(0.0f, 6.0f, kStall, kMaxFlow) == 0.0f, "");
    }

    // ---- F-5: hydraulic compliance ------------------------------------------------------------
    printf("\nHydraulic compliance\n");
    {
        // A 0 -> 9 bar ramp in 4 s with C = 1.5 ml/bar stores 3.4 ml/s of what the pump delivers.
        const float rising = complianceCorrectedFlow(6.0f, 2.25f, 1.5f);
        snprintf(d, sizeof(d), "%.2f ml/s", rising);
        check("rising pressure: puck flow = pump - C*dP/dt", fabsf(rising - 2.625f) < 1e-4f, d);

        const float falling = complianceCorrectedFlow(1.0f, -2.0f, 1.5f);
        snprintf(d, sizeof(d), "%.2f ml/s", falling);
        check("falling pressure releases stored water", fabsf(falling - 4.0f) < 1e-4f, d);

        check("never negative", complianceCorrectedFlow(1.0f, 5.0f, 1.5f) == 0.0f, "");
        check("C = 0 turns the correction off", complianceCorrectedFlow(3.2f, 4.0f, 0.0f) == 3.2f, "");
        check("steady pressure: no correction", complianceCorrectedFlow(2.0f, 0.0f, 1.5f) == 2.0f, "");
    }

    // ---- F-5 regression: pressure ripple must not swing the flow estimate ---------------------
    // The first F-5 build differentiated the pressure with a fast filter. A steady 9 bar with
    // +/-0.3 bar ripple then drew the flow estimate as a 0.4 - 3.7 ml/s zig-zag while the cup was
    // filling at a steady 2 g/s. This drives the same pipeline as BrewController::tick().
    printf("\nPuck flow estimate under pressure ripple\n");
    {
        const float duty = 36.0f, C = 1.5f, dt = 0.01f;
        const float steady = calculateVirtualFlow(9.0f, duty, kStall, kMaxFlow);
        float worstDev = 0.0f;
        for (float hz : {0.5f, 1.0f, 2.5f, 5.0f}) {
            PuckFlowRate rate;
            rate.reset(9.0f);
            float flow = steady;
            for (int i = 0; i < 2000; ++i) {
                const float p = 9.0f + 0.3f * sinf(2.0f * (float)M_PI * hz * i * dt);
                const float q = complianceCorrectedFlow(
                    calculateVirtualFlow(p, duty, kStall, kMaxFlow), rate.update(p, dt), C);
                flow = 0.06f * q + 0.94f * flow;            // the display filter in tick()
                if (i > 800 && fabsf(flow - steady) > worstDev) worstDev = fabsf(flow - steady);
            }
        }
        snprintf(d, sizeof(d), "worst %.2f ml/s off a steady %.2f ml/s", worstDev, steady);
        check("+/-0.3 bar ripple moves the flow < 0.2 ml/s", worstDev < 0.2f, d);

        // A real profile ramp, 0 -> 9 bar in 3 s (3 bar/s), must still be corrected: the ideal
        // correction is C * 3 = 4.5 ml/s.
        PuckFlowRate rate;
        rate.reset(0.0f);
        float corr = 0.0f;
        int n = 0;
        for (int i = 0; i < 300; ++i) {
            const float r = rate.update(9.0f * i / 300.0f, dt);
            if (i >= 150) { corr += C * r; ++n; }
        }
        corr /= (float)n;
        snprintf(d, sizeof(d), "%.2f of 4.50 ml/s in the second half of the ramp", corr);
        check("a 3 bar/s ramp keeps most of its correction", corr > 2.5f, d);
    }

    // ---- F-2: the weight slope is windowed in time, not in samples ----------------------------
    printf("\nWeight slope: time window\n");
    {
        const uint32_t windowMs = 1500;
        const int rates[] = {5, 8, 20};
        float spans[3] = {0, 0, 0};
        bool allTwo = true;
        for (int r = 0; r < 3; ++r) {
            WeightSlope ws;
            const uint32_t period = 1000 / rates[r];
            for (uint32_t t = 0; t <= 6000; t += period) {
                ws.push(10000 + t, 2.0f * t * 0.001f, windowMs);   // a steady 2.0 g/s pour
            }
            spans[r] = (float)ws.spanMs();
            if (fabsf(ws.slope() - 2.0f) > 0.01f) allTwo = false;
        }
        snprintf(d, sizeof(d), "span %.0f / %.0f / %.0f ms at 5 / 8 / 20 Hz", spans[0], spans[1], spans[2]);
        check("every report rate spans the same ~1.5 s", spans[0] > 1300 && spans[0] <= 1500 &&
              spans[1] > 1300 && spans[1] <= 1500 && spans[2] > 1300 && spans[2] <= 1500, d);
        check("a steady 2 g/s pour reads 2 g/s on every scale", allTwo, "");

        WeightSlope few;
        few.push(1000, 0.0f, windowMs);
        few.push(1100, 1.0f, windowMs);
        snprintf(d, sizeof(d), "%.2f g/s from 2 readings", few.slope());
        check("too few readings give no slope", few.slope() == 0.0f, d);

        WeightSlope noisy;
        for (uint32_t t = 0; t <= 3000; t += 100) {
            const float jitter = ((t / 100) % 2) ? 0.15f : -0.15f;
            noisy.push(t, 1.5f * t * 0.001f + jitter, windowMs);
        }
        snprintf(d, sizeof(d), "%.2f g/s", noisy.slope());
        check("+/-0.15 g jitter on a 1.5 g/s pour", fabsf(noisy.slope() - 1.5f) < 0.1f, d);

        WeightSlope lifted;
        for (uint32_t t = 0; t <= 1500; t += 100) lifted.push(t, 40.0f - 0.01f * t, windowMs);
        check("a falling weight never reads as negative flow", lifted.slope() == 0.0f, "");

        // F-3 relies on this: readings taken during the lockout are already in the window when
        // the lockout lifts, so the first cutoff evaluation has a full second and a half of data.
        WeightSlope primed;
        for (uint32_t t = 0; t <= 3000; t += 200) primed.push(t, 1.8f * t * 0.001f, windowMs);
        snprintf(d, sizeof(d), "%u readings, %.2f g/s at the end of a 3 s lockout",
                 (unsigned)primed.count(), primed.slope());
        check("a window filled through the lockout is ready", primed.count() >= 7 &&
              fabsf(primed.slope() - 1.8f) < 0.01f, d);
    }

    // ---- F-4: the drip lag is learned from the measured overshoot -----------------------------
    printf("\nLearned drip lag\n");
    {
        float lag = 0.0f;
        check("measures drip / slope", scaleLagMeasure(34.0f, 35.8f, 2.0f, lag) && fabsf(lag - 0.9f) < 1e-4f, "");
        check("a slope below 0.3 g/s is not trusted", !scaleLagMeasure(34.0f, 35.0f, 0.2f, lag), "");
        check("a cup lighter than the cutoff is rejected", !scaleLagMeasure(34.0f, 33.0f, 2.0f, lag), "");
        check("a 7 g jump is a bumped scale, not a drip", !scaleLagMeasure(34.0f, 41.0f, 2.0f, lag), "");

        // Simulate shots on a machine whose real drip lasts 0.9 s, starting from the 0.6 s default.
        const float trueLag = 0.9f;
        const float target  = 36.0f;
        float cfgLag = 0.6f;
        float lag10 = 0.0f;
        float lastYield = 0.0f;
        for (int shot = 1; shot <= 20; ++shot) {
            const float s       = 2.0f;
            const float cutoffG = target - sbCutoffSlopeClamp(s) * cfgLag;
            const float settled = cutoffG + s * trueLag;
            float m = 0.0f;
            if (scaleLagMeasure(cutoffG, settled, s, m)) cfgLag = scaleLagLearn(cfgLag, m);
            if (shot == 10) lag10 = cfgLag;
            lastYield = settled;
        }
        snprintf(d, sizeof(d), "%.3f s after 10 shots, %.3f s after 20", lag10, cfgLag);
        check("converges towards the real 0.9 s lag", fabsf(lag10 - trueLag) < 0.12f &&
              fabsf(cfgLag - trueLag) < 0.05f, d);
        snprintf(d, sizeof(d), "yield %.2f g for a 36.0 g target", lastYield);
        check("the yield lands on target once learned", fabsf(lastYield - target) < 0.1f, d);

        // One odd shot moves the lag by a tenth of the error at most.
        const float before = 0.9f;
        const float after  = scaleLagLearn(before, 3.0f);
        snprintf(d, sizeof(d), "%.2f s -> %.2f s", before, after);
        check("a single outlier shot cannot throw the lag off", after - before < 0.25f, d);
    }

    // ---- Pump flow calibration against the scale ----------------------------------------------
    // The bench shot that prompted this read ~1.9 ml/s on the model while the cup filled at
    // ~2.3 ml/s: a pump ~20 % stronger than its preset. The calibration must find that, ignore
    // the parts of the shot that are not steady, and converge without overshooting.
    printf("\nPump flow calibration against the scale\n");
    {
        const float truePumpStrength = 1.20f;   // the real pump vs its preset
        const float dt = 0.01f;
        float flowScale = 1.0f;
        float after5 = 0.0f;
        for (int shot = 1; shot <= 10; ++shot) {
            FlowCalibration cal;
            for (int i = 0; i < 2500; ++i) {           // a 25 s shot
                const float t = i * dt;
                // 0-4 s pre-infusion at 3 bar, 4-7 s ramp to 9 bar, then steady 9 bar
                const float p = (t < 4.0f) ? 3.0f : (t < 7.0f ? 3.0f + 2.0f * (t - 4.0f) : 9.0f);
                const float dpdt = (t >= 4.0f && t < 7.0f) ? 2.0f : 0.0f;
                const float duty = 36.0f;
                const float model = calculateVirtualFlow(p, duty, kStall, kMaxFlow * flowScale);
                const float truth = calculateVirtualFlow(p, duty, kStall, kMaxFlow * truePumpStrength);
                const float cup   = (t < 6.0f) ? 0.0f : truth;   // nothing reaches the cup at first
                cal.addTick(t > 3.0f /* lockout over */, p, dpdt, model, cup);
            }
            float r = 0.0f;
            if (cal.ratio(r)) flowScale = flowScaleLearn(flowScale, r);
            if (shot == 5) after5 = flowScale;
        }
        snprintf(d, sizeof(d), "%.3f after 5 shots, %.3f after 10 (true 1.200)", after5, flowScale);
        check("converges to the real pump strength", fabsf(after5 - 1.2f) < 0.06f &&
              fabsf(flowScale - 1.2f) < 0.015f, d);
        check("never overshoots", flowScale <= 1.2001f, d);

        FlowCalibration ramp;
        for (int i = 0; i < 1000; ++i) ramp.addTick(true, 6.0f, 2.0f, 3.0f, 3.6f);
        check("a pressure ramp is not used", ramp.ticks() == 0, "");

        FlowCalibration preinf;
        for (int i = 0; i < 1000; ++i) preinf.addTick(true, 2.5f, 0.0f, 3.0f, 3.6f);
        check("pre-infusion pressure is not used", preinf.ticks() == 0, "");

        FlowCalibration shortShot;
        for (int i = 0; i < 200; ++i) shortShot.addTick(true, 9.0f, 0.0f, 2.0f, 2.4f);
        float r = 0.0f;
        check("2 s of steady data is not enough", !shortShot.ratio(r), "");

        FlowCalibration bumped;
        for (int i = 0; i < 600; ++i) bumped.addTick(true, 9.0f, 0.0f, 1.0f, 3.0f);
        check("a 3x disagreement is rejected", !bumped.ratio(r), "");

        check("the scale stays within 0.5 - 1.5", flowScaleLearn(1.45f, 2.0f) == 1.5f &&
              flowScaleLearn(0.52f, 0.5f) == 0.5f, "");
        check("NaN falls back to no correction", flowScaleClamp(NAN) == 1.0f, "");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
