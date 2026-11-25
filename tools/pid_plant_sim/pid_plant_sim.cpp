// =================================================================================================
//  pid_plant_sim.cpp - closed-loop check of the pressure loop (src/control/pid_controller.cpp)
//
//  Compiles the REAL pid_controller.cpp on the host and runs Classic 9-Bar against a lumped
//  hydraulic plant, with the controller's default settings (sbConfigDefaults(): Ulka EX5 preset
//  gains Kp 14 / Ki 3.5 / Kd 1.2, headspace 1.5 bar, pressureCtrlMode 1):
//
//      C dP/dt = Q_pump(duty, P) - Q_out(P)
//      Q_pump : the firmware's Ulka EX5 model, delivered in whole 10 ms half-cycle strokes (PSM)
//      Q_out  : puck k*sqrt(P) sized for Q ml/s at 9 bar, or a blind basket with an OPV at 10 bar
//      4 ml of headspace is filled before pressure builds.
//
//  Also runs Gentle Lever (soak, ramp, 25 s decline) and prints the soak and decline error.
//  Prints the overshoot in pre-infusion and at 9 bar, the deepest dip under 9 bar after the ramp
//  and the mean tracking error, for both loops (classic = mode 0, adaptive = mode 1) across
//  compliance C = 0.3..3 ml/bar and puck flow Q = 1..3.5 ml/s.
//
//  Run:  tools/pid_plant_sim/run.sh
// =================================================================================================
#include "config_schema.h"
#include "pump_presets.h"
#include "pump_physics.h"
#include "control/pid_controller.h"
uint32_t g_ms = 0; bool g_verbose = false; ConfigStore g_config;
struct Phase { float t0, t1, p0, p1; };   // linear target segments
static float targetAt(const Phase* ph, int n, float t) {
    for (int i = 0; i < n; ++i) if (t < ph[i].t1) {
        float a = (t - ph[i].t0) / (ph[i].t1 - ph[i].t0); return ph[i].p0 + a * (ph[i].p1 - ph[i].p0); }
    return ph[n-1].p1;
}
static void run(uint8_t mode, bool blind, const char* label, float C = 1.5f, float puckFlow = 2.0f) {
    g_config.cfg() = sbConfigDefaults(); g_config.cfg().pressureCtrlMode = mode;
    g_pid.applyConfig(g_config.cfg()); g_pid.reset(0);
    // Classic 9-Bar: pre-infusion 3 bar 4 s, ramp 3->9 in 3 s, hold 9 bar 20 s, decline 9->6 in 10 s
    const Phase ph[] = {{0,4,3,3},{4,7,3,9},{7,27,9,9},{27,37,9,6}};
    const float k = blind ? 0.0f : puckFlow / sqrtf(9.0f);   // 2 ml/s through the puck at 9 bar
    float tPre90 = -1, minUnder = 0, P = 0, maxOverPre = 0, maxOverMain = 0, sumAbs = 0; int nAbs = 0, acc = 0;
    float headspace = 4.0f;                // ml of empty headspace to fill before pressure builds
    for (int i = 0; i < 37000; ++i) {      // 1 ms steps, control at 100 Hz
        g_ms = i;
        const float t = i / 1000.0f;
        static float duty = 0;
        if (i % 10 == 0) duty = g_pid.update(P, targetAt(ph, 4, t), t < 4 ? 60 : 100, 0.01f);
        // PSM: whole half-cycles (10 ms at 50 Hz); Bresenham on duty
        float q = 0;
        if (i % 10 == 0) { acc += (int)duty; if (acc >= 100) { acc -= 100; q = 1; } }
        const float Qp = calculateVirtualFlow(P, 100.0f, 15.0f, 10.8f, 1.35f);  // ml/s at full stroke
        float vin = q * Qp * 0.010f;       // ml this half-cycle
        if (headspace > 0) { float f = std::min(headspace, vin); headspace -= f; vin -= f; }
        float qout = k * sqrtf(std::max(P, 0.0f)) * 0.001f;
        if (blind && P > 10.0f) qout += (P - 10.0f) * 2.0f * 0.001f;       // OPV
        P += (vin - qout) / C; if (P < 0) P = 0;
        const float tgt = targetAt(ph, 4, t);
        if (t > 0.5f && t < 4.0f)  maxOverPre  = std::max(maxOverPre,  P - 3.0f);
        if (tPre90 < 0 && P >= 2.7f) tPre90 = t;
        if (t > 7.0f && t < 27.0f) maxOverMain = std::max(maxOverMain, P - 9.0f);
        if (t > 10.0f && t < 27.0f) minUnder = std::min(minUnder, P - 9.0f);
        if (t > 9.0f) { sumAbs += fabsf(P - tgt); nAbs++; }
    }
    printf("%-34s C=%.1f Q=%.1f  pre 90%% at %4.2f s, over %+.2f | over main %+.2f | dip at 9 bar %+.2f | mean|err| %.2f\n",
           label, C, puckFlow, tPre90, maxOverPre, maxOverMain, minUnder, sumAbs / nAbs);
}
// Gentle Lever: 1.5 bar soak (50 % cap), ramp 2->8.5 bar in 3 s, decline 8.5->5 bar over 25 s.
static void runLever(uint8_t mode, float C, float Qp9, float headspaceMl) {
    g_config.cfg() = sbConfigDefaults(); g_config.cfg().pressureCtrlMode = mode;
    g_pid.applyConfig(g_config.cfg()); g_pid.reset(0);
    const Phase ph[] = {{0,8,1.5f,1.5f},{8,11,2,8.5f},{11,36,8.5f,5.0f}};
    const uint8_t cap[] = {50,100,100};
    const float k = Qp9 / sqrtf(9.0f);
    float P = 0, hs = headspaceMl, soakSum = 0, decSum = 0, decBias = 0; int nS = 0, nD = 0, acc = 0, flips = 0;
    float tSoak90 = -1, duty = 0, lastDuty = 0, tTgt6 = -1, tP6 = -1, rampOver = 0;
    for (int i = 0; i < 36000; ++i) {
        g_ms = i; const float t = i / 1000.0f;
        int pi = t < 8 ? 0 : (t < 11 ? 1 : 2);
        if (i % 10 == 0) { duty = g_pid.update(P, targetAt(ph, 3, t), cap[pi], 0.01f);
            if ((duty == 0) != (lastDuty == 0) && t > 15) flips++; lastDuty = duty; }
        float q = 0; if (i % 10 == 0) { acc += (int)duty; if (acc >= 100) { acc -= 100; q = 1; } }
        float vin = q * calculateVirtualFlow(P, 100.0f, 15.0f, 10.8f, 1.35f) * 0.010f;
        if (hs > 0) { float f = std::min(hs, vin); hs -= f; vin -= f; }
        P += (vin - k * sqrtf(std::max(P, 0.0f)) * 0.001f) / C; if (P < 0) P = 0;
        const float tgt = targetAt(ph, 3, t);
        if (t > 4 && t < 8) { soakSum += P - tgt; nS++; }
        if (tSoak90 < 0 && P >= 1.35f) tSoak90 = t;
        if (t > 8 && tTgt6 < 0 && tgt >= 6.0f) tTgt6 = t;
        if (t > 8 && tP6 < 0 && P >= 6.0f) tP6 = t;
        if (t > 11 && t < 18) rampOver = std::max(rampOver, P - tgt);
        if (t > 16) { decSum += fabsf(P - tgt); decBias += P - tgt; nD++; }
    }
    printf("lever %s C=%.1f Q=%.1f  soak 90%% at %4.2f s, bias %+.2f | ramp lag at 6 bar %.2f s, over %+.2f | decline mean|err| %.2f bias %+.2f | flips %d\n",
           mode ? "adaptive" : "classic ", C, Qp9, tSoak90, soakSum/nS, tP6 - tTgt6, rampOver, decSum/nD, decBias/nD, flips);
}
int main() {
    for (float C : {0.3f, 0.6f, 1.5f, 3.0f}) for (float Q : {1.0f, 2.0f, 3.5f}) {
        run(0, false, "classic,  puck", C, Q);
        run(1, false, "adaptive, puck", C, Q);
    }
    for (float C : {0.3f, 1.5f}) { run(0, true, "classic,  blind", C); run(1, true, "adaptive, blind", C); }
    for (float C : {0.6f, 1.5f, 3.0f}) for (float Q : {1.5f, 2.5f}) { runLever(0, C, Q, 12); runLever(1, C, Q, 12); }
}
