#pragma once

#include <lvgl.h>
#include "network_client.h"

void createEspressoUi();
void updateEspressoUiTelemetry(const TelemetryData& data);
void updateEspressoUiHistory(const HistoryData& data);
bool historyLedgerVisible();
void resetShotChart();
