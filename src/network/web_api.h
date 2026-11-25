// =================================================================================================
//  web_api.h - ESPAsyncWebServer: gzipped SPA, REST API and the 10 Hz WebSocket
//              docs/API.md
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <stddef.h>
#include <stdint.h>

class WebApi {
public:
    void begin(AsyncWebServer* http, uint16_t wsPort = 81);
    void loop();

    // Push one binary telemetry frame to every connected client.
    void broadcast(const uint8_t* frame, size_t len);
    void broadcastShotSummary(uint32_t durMs = 0, float doseG = -1.0f);
    uint32_t clients() const;

    // Telemetry task instrumentation. The dashboard graph is only as smooth as this task, so the
    // body duration and the gap between iterations are reported by /api/v1/status: a large body
    // means something in the send path blocks, a large gap means Core 0 is starved.
    void noteTelemetryCycle(uint32_t bodyUs, uint32_t periodUs, bool active);
    uint32_t teleBodyUsMax() const { return _teleBodyUsMax; }
    uint32_t teleBodyUsAvg() const { return _teleBodyUsAvg; }
    uint32_t telePeriodUsMax() const { return _telePeriodUsMax; }

    // Captive-portal handlers so an Android/iOS "sign in to network" sheet opens the dashboard.
    void registerCaptivePortal(AsyncWebServer* http);

private:
    // The server keeps at most kMaxWsClients live sockets. The id table has two extra slots so a
    // client that is still being reaped after cleanupClients() can be tracked without being lost.
    static constexpr uint8_t kMaxWsClients  = 4;
    static constexpr uint8_t kMaxWsTracked  = 6;

    void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
                   AwsEventType type, uint8_t* data, size_t len);
    void sendTelemetryTo(uint32_t clientId);

    AsyncWebServer* _http = nullptr;
    AsyncWebSocket* _ws = nullptr;

    // Ids of the currently connected WebSocket clients. The library exposes no way to enumerate
    // them, and the broadcast path needs them to send per client: one congested client must not be
    // able to stall the telemetry for everybody else.
    mutable portMUX_TYPE _wsMux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t _wsIds[kMaxWsTracked] = {0, 0, 0, 0, 0, 0};
    uint8_t  _wsIdCount = 0;

    // Diagnostics: frames skipped because a client was already behind.
    uint32_t _wsSkipped = 0;

    // Telemetry task instrumentation.
    uint32_t _teleBodyUsMax  = 0;
    uint32_t _teleBodyUsAvg  = 0;
    uint32_t _telePeriodUsMax = 0;

public:
    // How many telemetry frames the controller deliberately did not send because a client was
    // already more than two frames behind. A non-zero value means a client is not keeping up with
    // the stream - the graph will show gaps there even though the controller generated every frame.
    uint32_t skippedFrames() const { return _wsSkipped; }
};

extern WebApi g_web;