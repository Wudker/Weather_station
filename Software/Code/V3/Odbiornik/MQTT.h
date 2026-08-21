#pragma once

#include "PogodynkaRxTypes.h"

void mqttBegin();
void mqttLoop();
bool mqttConnected();
bool mqttPublishMeasurement(const Dane_ESP& data);
