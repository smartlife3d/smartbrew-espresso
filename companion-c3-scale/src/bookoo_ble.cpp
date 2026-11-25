#include "bookoo_ble.h"

#include <NimBLEDevice.h>

#include <string.h>

#include <atomic>

#include "scale_identity.h"
// Arduino-ESP32 3.3 frees the BLE controller's RAM before setup() unless a BLE library says it is
// in use. Without that, the heap takes over the controller's memory and esp_bt_controller_init()
// later crashes in malloc (LoadProhibited). Including the core's header registers a constructor
// that keeps the memory reserved; it costs nothing if the library already does the same.
#if __has_include(<esp32-hal-alloc-ble-mem.h>)
#include <esp32-hal-alloc-ble-mem.h>
#endif

// ---------------------------------------------------------------------------
// Tare request plumbing
// ---------------------------------------------------------------------------
static volatile bool s_tareRequested = false;
static volatile bool s_connected      = false;
static volatile uint32_t s_notifyCount = 0;

static portMUX_TYPE s_tareMux = portMUX_INITIALIZER_UNLOCKED;
static NimBLECharacteristic* s_weightChar = nullptr;
// NimBLE 2.x no longer counts a characteristic's subscribers, so track them here: one bit per
// connection handle. A disconnect clears its bit, because the stack's unsubscribe event for a
// dropped link arrives after the connection is gone and never reaches onSubscribe().
static std::atomic<uint32_t> s_subscriberMask{0};
static inline uint32_t connBit(uint16_t connHandle) { return 1UL << (connHandle & 31U); }

// ---------------------------------------------------------------------------
// GATT callbacks
// ---------------------------------------------------------------------------
class BookooServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* /*server*/, NimBLEConnInfo& /*connInfo*/) override {
        s_connected = true;
        Serial.println(F("[ble] client connected"));
    }

    void onDisconnect(NimBLEServer* /*server*/, NimBLEConnInfo& connInfo, int /*reason*/) override {
        s_subscriberMask.fetch_and(~connBit(connInfo.getConnHandle()), std::memory_order_relaxed);
        s_connected = false;
        Serial.println(F("[ble] client disconnected"));
    }

    void onMTUChange(uint16_t /*mtu*/, NimBLEConnInfo& /*connInfo*/) override {
        // Nothing to do: every Bookoo frame is 20 bytes, well under any MTU.
    }
};

// Accepts the documented Bookoo tare command
//   {0x03, 0x0A, 0x01, 0x00, 0x00, 0x08}
// and, per the spec, any write whose leading byte is 0x03.
class BookooCharCallbacks : public NimBLECharacteristicCallbacks {
    void handleWrite(NimBLECharacteristic* chr) {
        std::string payload = chr->getValue();
        if (payload.empty()) return;

        const uint8_t* data = (const uint8_t*)payload.data();
        const size_t len = payload.size();

        const bool exactTare = (len >= BOOKOO_TARE_CMD_LEN) &&
                               (data[0] == BOOKOO_TARE_CMD_0) && (data[1] == 0x0A) &&
                               (data[2] == 0x01) && (data[3] == 0x00) &&
                               (data[4] == 0x00) && (data[5] == 0x08);
        const bool leadingTare = (data[0] == BOOKOO_TARE_CMD_0);

        if (exactTare || leadingTare) {
            s_tareRequested = true;
            Serial.println(F("[ble] tare requested by client"));
        } else {
            Serial.printf("[ble] ignored %u-byte write 0x%02X\n",
                          (unsigned)len, (unsigned)data[0]);
        }
    }

    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& /*connInfo*/) override {
        handleWrite(chr);
    }
};

// Tracks who listens to the weight characteristic (see s_subscriberMask).
class BookooWeightCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* /*chr*/, NimBLEConnInfo& connInfo,
                     uint16_t subValue) override {
        const uint32_t bit = connBit(connInfo.getConnHandle());
        if (subValue) s_subscriberMask.fetch_or(bit, std::memory_order_relaxed);
        else          s_subscriberMask.fetch_and(~bit, std::memory_order_relaxed);
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool bookooInit(void) {
    NimBLEDevice::init(scaleDeviceName());
    NimBLEDevice::setPower(9);   // +9 dBm (NimBLE 2.x takes dBm, not an esp_power_level_t)

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new BookooServerCallbacks());
    server->advertiseOnDisconnect(true);

    NimBLEService* service = server->createService(NimBLEUUID((uint16_t)BOOKOO_SERVICE_UUID));

    // 0xFF11 - weight notifications.
    NimBLECharacteristic* weight =
        service->createCharacteristic(NimBLEUUID((uint16_t)BOOKOO_NOTIFY_UUID),
                                      NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
    static uint8_t initial[BOOKOO_FRAME_LEN];
    memset(initial, 0, sizeof(initial));
    initial[0] = 0x02;   // Bookoo frame type, zero weight, positive sign
    weight->setValue((uint8_t*)initial, BOOKOO_FRAME_LEN);
    weight->setCallbacks(new BookooWeightCallbacks());
    s_weightChar = weight;

    // 0xFF12 - tare / command writes.
    NimBLECharacteristic* cmd =
        service->createCharacteristic(NimBLEUUID((uint16_t)BOOKOO_WRITE_UUID),
                                      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
                                          NIMBLE_PROPERTY::READ);
    cmd->setCallbacks(new BookooCharCallbacks());

    // No service->start(): NimBLE 2.x starts the services with the server, when advertising starts.

    NimBLEAdvertisementData advData;
    advData.setFlags(0x06);                       // LE General Discoverable, BR/EDR not supported
    advData.setName(scaleDeviceName());
    advData.setCompleteServices(NimBLEUUID((uint16_t)BOOKOO_SERVICE_UUID));
    NimBLEDevice::getAdvertising()->setAdvertisementData(advData);

    NimBLEDevice::startAdvertising();
    Serial.printf("[ble] advertising as \"%s\" (svc 0x%04X, notify 0x%04X, write 0x%04X)\n",
                  scaleDeviceName(), BOOKOO_SERVICE_UUID, BOOKOO_NOTIFY_UUID,
                  BOOKOO_WRITE_UUID);
    return true;
}

uint8_t bookooBuildFrame(const ScaleSample& s, uint8_t* out) {
    memset(out, 0, BOOKOO_FRAME_LEN);

    // Bookoo preamble: frame type 0x02 followed by four zero bytes.
    out[0] = 0x02;
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x00;
    out[4] = 0x00;
    out[5] = 0x00;

    float grams = s.grams;
    const bool negative = (grams < 0.0f);
    if (negative) grams = -grams;

    // Hundredths of a gram in a 24-bit big-endian field (max 167772.15 g).
    uint32_t magnitude = (uint32_t)(grams * 100.0f + 0.5f);
    if (magnitude > 0x00FFFFFFUL) magnitude = 0x00FFFFFFUL;

    out[6] = negative ? BOOKOO_SIGN_NEG : BOOKOO_SIGN_POS;
    out[7] = (uint8_t)((magnitude >> 16) & 0xFF);
    out[8] = (uint8_t)((magnitude >> 8) & 0xFF);
    out[9] = (uint8_t)(magnitude & 0xFF);

    // out[10..19] stay zero-padded: Bookoo apps ignore them.
    return BOOKOO_FRAME_LEN;
}

void bookooTick(const ScaleSample& s) {
    if (s_weightChar == nullptr) return;
    if (s_subscriberMask.load(std::memory_order_relaxed) == 0) return;   // nobody is listening

    uint8_t frame[BOOKOO_FRAME_LEN];
    bookooBuildFrame(s, frame);
    s_weightChar->setValue((uint8_t*)frame, BOOKOO_FRAME_LEN);
    s_weightChar->notify();
    s_notifyCount++;
}

bool bookooTakeTareRequest(void) {
    bool taken = false;
    portENTER_CRITICAL(&s_tareMux);
    if (s_tareRequested) {
        s_tareRequested = false;
        taken = true;
    }
    portEXIT_CRITICAL(&s_tareMux);
    return taken;
}

bool bookooIsConnected(void) { return s_connected; }
uint32_t bookooNotifyCount(void) { return s_notifyCount; }