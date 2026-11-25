// =================================================================================================
//  sim_ble_scale.cpp - Bookoo Themis BLE Scale Emulator
// =================================================================================================
#include "sim_ble_scale.h"
#include "sim_physics.h"
#include <NimBLEDevice.h>
// Arduino-ESP32 3.3 frees the BLE controller's RAM before setup() unless a BLE library says it is
// in use. Without that, the heap takes over the controller's memory and esp_bt_controller_init()
// later crashes in malloc (LoadProhibited). Including the core's header registers a constructor
// that keeps the memory reserved; it costs nothing if the library already does the same.
#if __has_include(<esp32-hal-alloc-ble-mem.h>)
#include <esp32-hal-alloc-ble-mem.h>
#endif

SimBleScale g_simBleScale;

namespace {
// 128-bit canonical Bookoo Themis UUIDs matching scale_bookoo.h exactly
static constexpr const char* BOOKOO_SERVICE_UUID = "00000ffe-0000-1000-8000-00805f9b34fb";
static constexpr const char* BOOKOO_NOTIFY_UUID  = "0000ff11-0000-1000-8000-00805f9b34fb";
static constexpr const char* BOOKOO_WRITE_UUID   = "0000ff12-0000-1000-8000-00805f9b34fb";
static constexpr uint16_t   BOOKOO_SHORT_SVC     = 0x0FFE;
static constexpr size_t     BOOKOO_FRAME_LEN     = 20;

static NimBLECharacteristic* s_weightChar = nullptr;
static std::atomic<bool>     s_connected{false};
static std::atomic<bool>     s_subscribed{false};
static std::atomic<uint32_t> s_notifyCount{0};
static std::atomic<uint32_t> s_tareCount{0};
// NimBLE 2.x no longer counts a characteristic's subscribers, so track them here: one bit per
// connection handle. A disconnect clears its bit, because the stack's unsubscribe event for a
// dropped link arrives after the connection is gone and never reaches onSubscribe().
static std::atomic<uint32_t> s_subscriberMask{0};
static inline uint32_t connBit(uint16_t connHandle) { return 1UL << (connHandle & 31U); }

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* /*server*/, NimBLEConnInfo& connInfo) override {
        s_connected.store(true, std::memory_order_relaxed);
        Serial.printf("[BLE_SCALE] Client connected! Peer: %s\n",
                      connInfo.getAddress().toString().c_str());
    }

    // advertiseOnDisconnect(true) in begin() restarts advertising after the link drops.
    void onDisconnect(NimBLEServer* /*server*/, NimBLEConnInfo& connInfo, int reason) override {
        s_subscriberMask.fetch_and(~connBit(connInfo.getConnHandle()), std::memory_order_relaxed);
        s_connected.store(false, std::memory_order_relaxed);
        s_subscribed.store(false, std::memory_order_relaxed);
        Serial.printf("[BLE_SCALE] Client disconnected (reason 0x%02X). Restarting advertising...\n",
                      reason);
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo& /*connInfo*/) override {
        Serial.printf("[BLE_SCALE] MTU updated to %u\n", mtu);
    }
};

class CharCallbacks : public NimBLECharacteristicCallbacks {
    void handleWrite(NimBLECharacteristic* chr) {
        std::string payload = chr->getValue();
        if (payload.empty()) return;

        const uint8_t* data = reinterpret_cast<const uint8_t*>(payload.data());
        const size_t len = payload.size();

        // Standard Bookoo tare command: {0x03,0x0A,0x01,0x00,0x00,0x08}.
        // The controller also sends a software tare; both must really zero the cup, otherwise the
        // weight the controller subtracts its tare offset from would keep growing.
        if (data[0] == 0x03) {
            s_tareCount.fetch_add(1, std::memory_order_relaxed);
            g_simBleScale.tare();
            Serial.println(F("[BLE_SCALE] TARE command -> cup weight reset to 0.00 g"));
        } else {
            Serial.printf("[BLE_SCALE] Unknown command received (0x%02X, len=%u)\n", data[0], (unsigned)len);
        }
    }

    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& /*connInfo*/) override { handleWrite(chr); }

    void onSubscribe(NimBLECharacteristic* /*chr*/, NimBLEConnInfo& connInfo, uint16_t subVal) override {
        const bool sub = (subVal > 0);
        const uint32_t bit = connBit(connInfo.getConnHandle());
        if (sub) s_subscriberMask.fetch_or(bit, std::memory_order_relaxed);
        else     s_subscriberMask.fetch_and(~bit, std::memory_order_relaxed);
        s_subscribed.store(sub, std::memory_order_relaxed);
        Serial.printf("[BLE_SCALE] Subscription changed: %s (val=0x%04X)\n", sub ? "ACTIVE (33 Hz streaming)" : "INACTIVE", subVal);
    }
};

void buildBookooFrame(float grams, uint8_t* out) {
    memset(out, 0, BOOKOO_FRAME_LEN);

    out[0] = 0x02; // Frame header
    out[2] = 100;  // 100% battery

    const bool negative = (grams < 0.0f);
    if (negative) grams = -grams;

    // Hundredths of a gram in a 24-bit big-endian magnitude
    uint32_t magnitude = (uint32_t)(grams * 100.0f + 0.5f);
    if (magnitude > 0x00FFFFFFUL) magnitude = 0x00FFFFFFUL;

    out[6] = negative ? 0x2D : 0x2B; // '-' (0x2D) or '+' (0x2B)
    out[7] = (uint8_t)((magnitude >> 16) & 0xFF);
    out[8] = (uint8_t)((magnitude >> 8) & 0xFF);
    out[9] = (uint8_t)(magnitude & 0xFF);
}
} // namespace

void SimBleScale::begin(const char* deviceName) {
    NimBLEDevice::init(deviceName);
    NimBLEDevice::setPower(9);   // +9 dBm (NimBLE 2.x takes dBm, not an esp_power_level_t)

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    server->advertiseOnDisconnect(true);

    // Primary Bookoo service
    NimBLEService* service = server->createService(NimBLEUUID(BOOKOO_SERVICE_UUID));

    // 0xFF11: Weight notifications (canonical 128-bit UUID)
    s_weightChar = service->createCharacteristic(
        NimBLEUUID(BOOKOO_NOTIFY_UUID),
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
    );
    s_weightChar->setCallbacks(new CharCallbacks());

    uint8_t initial[BOOKOO_FRAME_LEN] = {0};
    initial[0] = 0x02;
    initial[6] = 0x2B;
    s_weightChar->setValue(initial, BOOKOO_FRAME_LEN);

    // 0xFF12: Tare command write
    NimBLECharacteristic* cmdChar = service->createCharacteristic(
        NimBLEUUID(BOOKOO_WRITE_UUID),
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ
    );
    cmdChar->setCallbacks(new CharCallbacks());

    // No service->start(): NimBLE 2.x starts the services with the server, when advertising starts.

    // Set up BLE Advertising
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(NimBLEUUID(BOOKOO_SERVICE_UUID));
    pAdvertising->addServiceUUID(NimBLEUUID(BOOKOO_SHORT_SVC));
    pAdvertising->enableScanResponse(true);

    NimBLEAdvertisementData advData;
    advData.setFlags(0x06); // General Discoverable, BR/EDR not supported
    advData.setName(deviceName);
    advData.setCompleteServices(NimBLEUUID(BOOKOO_SHORT_SVC));
    pAdvertising->setAdvertisementData(advData);

    NimBLEAdvertisementData scanData;
    scanData.setName(deviceName);
    pAdvertising->setScanResponseData(scanData);

    pAdvertising->start();
    Serial.printf("[BLE_SCALE] Advertising as \"%s\" (Bookoo GATT %s, notify %s)\n",
                  deviceName, BOOKOO_SERVICE_UUID, BOOKOO_NOTIFY_UUID);
}

void SimBleScale::setWeight(float grams) {
    _weightGrams.store(grams, std::memory_order_relaxed);
}

void SimBleScale::tare() {
    _weightGrams.store(0.0f, std::memory_order_relaxed);
    // The hydraulic model owns the cup weight and republishes it every physics tick, so it has to
    // be zeroed as well - otherwise a controller tare is undone within 10 ms.
    g_simPhysics.requestTare();
}

bool SimBleScale::isConnected() const {
    return s_connected.load(std::memory_order_relaxed);
}

bool SimBleScale::isSubscribed() const {
    return s_subscribed.load(std::memory_order_relaxed) ||
           s_subscriberMask.load(std::memory_order_relaxed) != 0;
}

uint32_t SimBleScale::notifyCount() const {
    return s_notifyCount.load(std::memory_order_relaxed);
}

uint32_t SimBleScale::tareCount() const {
    return s_tareCount.load(std::memory_order_relaxed);
}

void SimBleScale::update() {
    const uint32_t now = millis();
    // 33 Hz notification rate (~30 ms intervals)
    if (now - _lastNotifyMs < 30) return;
    _lastNotifyMs = now;

    notifyNow();
}

void SimBleScale::notifyNow() {
    if (!isSubscribed()) return;

    uint8_t frame[BOOKOO_FRAME_LEN];
    buildBookooFrame(_weightGrams.load(std::memory_order_relaxed), frame);
    s_weightChar->setValue(frame, BOOKOO_FRAME_LEN);
    s_weightChar->notify();
    s_notifyCount.fetch_add(1, std::memory_order_relaxed);
}
