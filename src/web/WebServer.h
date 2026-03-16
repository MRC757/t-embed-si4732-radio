#pragma once
#include <Arduino.h>

// Call once from setup() after powerManager and radioController are ready.
void webServerBegin();

// Call from loop() — processes captive portal DNS when active.
void webLoop();

// Returns true when the ESP32 system clock is NTP-synchronised.
bool isNtpSynced();
