#pragma once

#include <Arduino.h>

#define MAX_DISPLAY_PROFILES 16
#define MAX_PROFILE_STEPS    8

struct ProfileStepData {
    char name[24];   // e.g. "Pre-Infusion", "Ramp to 9 Bar", "Peak"
    char type[16];   // "pressure", "flow", "power", "pause"
    char curve[16];  // "linear", "ease_in_out", "ease_out", "ease_in", "constant", "instant"
    float start_val; // bar or ml/s
    float end_val;   // bar or ml/s
    float duration;  // seconds
};

struct TelemetryData {
    uint32_t t_ms = 0;
    float shot_time = 0.0f;
    char state[24] = "IDLE";
    char mode[16] = "PROFILE";
    char pump_owner[16] = "CONTROLLER";
    uint8_t phase_idx = 0;
    char phase_name[32] = "";
    float pressure = 0.0f;
    float target_pressure = 0.0f;
    float flow_rate = 0.0f;
    uint8_t psm_pct = 0;
    float weight = 0.0f;
    float weight_flow = 0.0f;
    bool scale_conn = false;
    char scale_name[32] = "";
    int8_t scale_bat = -1;
    uint8_t ac_hz = 0;
    uint8_t phase_pct = 0;
    uint32_t faults = 0;
    bool brew_active = false;
    bool mcu_connected = false;
    // The post-shot weight is still settling: the last of the coffee is still draining into the
    // cup, so the live weight and drip rate are still worth showing.
    bool weight_settling = false;

    // Simulation mode
    bool simulation_mode = false;     // True if simulation mode is active on the Controller or the Waveshare display
    bool local_sim_running = false;   // True if Waveshare is executing local simulation

    // Profiles & targets
    char active_profile[48] = "Classic 9-Bar";
    char active_profile_id[40] = "classic_9bar";
    float target_weight = 36.0f;
    int total_profiles = 0;
    char profile_names[MAX_DISPLAY_PROFILES][48];
    char profile_ids[MAX_DISPLAY_PROFILES][32];
    int active_profile_idx = 0;
    bool profiles_updated = false;

    // Profile curve steps for chart guideline & timeline cards
    int step_count = 0;
    ProfileStepData steps[MAX_PROFILE_STEPS];
    float total_profile_duration = 37.0f;

    // Setup Portal state
    bool in_setup_portal = false;
    char portal_ssid[33] = "";

    // Reception timestamp for clock decoupling & dead reckoning
    uint32_t rx_millis = 0;
};

// =================================================================================================
// BREW LEDGER
//
// The Controller keeps every recorded shot in its LittleFS ledger and serves the list at
// GET /api/v1/history. The display shows that list, so the shots on the screen are the real
// recorded ones - not a built-in example that never changes.
// =================================================================================================
#define MAX_DISPLAY_SHOTS 16

struct HistoryShotData {
    char    profile_name[48] = "";
    float   dose_g = 0.0f;
    float   peak_bar = 0.0f;
    uint32_t duration_ms = 0;
    uint64_t epoch_ms = 0;
};

struct HistoryData {
    int      count = 0;
    bool     valid = false;     // at least one fetch reached the Controller
    bool     failed = false;    // the last fetch did not
    uint32_t updated_ms = 0;
    HistoryShotData shots[MAX_DISPLAY_SHOTS];
};

// Global telemetry copy & synchronization
extern TelemetryData g_telemetry;
extern SemaphoreHandle_t g_telemetryMutex;

extern HistoryData g_history;
extern SemaphoreHandle_t g_historyMutex;

void initNetworkClient();
void updateNetworkLoop();
bool getTelemetrySnapshot(TelemetryData& out);

// Brew ledger
void requestHistoryRefresh();          // the UI asks for a fresh list; the network task fetches it
bool getHistorySnapshot(HistoryData& out);  // the UI takes a copy to draw from

// Captive Portal (Phone Wi-Fi Setup) & Network Reset
void startSetupPortal();
void stopSetupPortal();
void requestStartSetupPortal();
void requestStopSetupPortal();
void resetWifiConfig();
bool isInSetupPortal();
const char* getPortalSsid();

// Machine control dispatchers
bool sendStartBrew();
bool sendStopBrew();
bool sendFlush(float seconds = 5.0f);
bool sendTare();
bool sendSelectProfile(const char* id);
void applySelectedProfile(int index);

// Simulation mode
void setSimulationMode(bool enable);
bool isSimulationMode();

// Planned profile curve evaluation at second t
float getPlannedProfilePressure(float t);

// Config getters/setters
const char* getWifiSsid();
const char* getControllerIp();
bool isWifiConnected();
void saveNetworkConfig(const char* ssid, const char* pass, const char* mcuHost);
