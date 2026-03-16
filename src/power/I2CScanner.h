#pragma once
// ============================================================
// I2CScanner.h — Boot-time I2C bus scanner
//
// PURPOSE
// -------
// The SDA/SCL pin order for the T-Embed SI4732 cannot be
// confirmed from datasheets alone — the product pinmap labels
// pins from the SI4732 module's perspective while the working
// code from the sister board (T-Embed CC1101) uses the
// opposite order. This scanner runs at boot and tries both
// configurations, reporting which one finds devices.
//
// Expected devices on the T-Embed SI4732 I2C bus:
//   0x40  ES7210  microphone ADC
//   0x55  BQ27220 battery fuel gauge
//   0x63  SI4732  radio tuner (SEN=VCC) — or 0x11 (SEN=GND)
//   0x6B  BQ25896 battery charger
//
// USAGE
// -----
//   In setup(), before Wire.begin():
//     I2CScanner::scanAndReport();
//
//   The function tries SDA=18/SCL=8 and SDA=8/SCL=18,
//   prints found addresses to Serial, and returns the
//   configuration that found the most devices.
//
// Once pins are confirmed, set I2C_SDA and I2C_SCL in
// PinConfig.h to the confirmed values and remove scanner.
// ============================================================
#include <Arduino.h>
#include <Wire.h>

namespace I2CScanner {

struct ScanResult {
    int  sda;
    int  scl;
    int  devicesFound;
    bool foundSI4732;     // 0x63 or 0x11
    bool foundES7210;     // 0x40
    bool foundBQ27220;    // 0x55
    bool foundBQ25896;    // 0x6B
};

// Scan one bus configuration
static ScanResult scan(int sda, int scl) {
    ScanResult r = { sda, scl, 0, false, false, false, false };

    Wire.end();
    delay(10);
    Wire.begin(sda, scl, 400000);
    Wire.setTimeOut(20);
    delay(20);

    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            r.devicesFound++;
            if (addr == 0x63 || addr == 0x11) r.foundSI4732  = true;
            if (addr == 0x40)                  r.foundES7210  = true;
            if (addr == 0x55)                  r.foundBQ27220 = true;
            if (addr == 0x6B)                  r.foundBQ25896 = true;
            Serial.printf("  [I2C SDA=%d SCL=%d]  found 0x%02X", sda, scl, addr);
            // Annotate known devices
            if (addr == 0x63) Serial.print("  ← SI4732 (SEN=VCC)");
            if (addr == 0x11) Serial.print("  ← SI4732 (SEN=GND)");
            if (addr == 0x40) Serial.print("  ← ES7210 microphone");
            if (addr == 0x55) Serial.print("  ← BQ27220 fuel gauge");
            if (addr == 0x6B) Serial.print("  ← BQ25896 charger");
            Serial.println();
        }
    }
    return r;
}

// Try both pin orders, return the better one.
// After returning, Wire is left initialised on the winner.
static ScanResult scanAndReport() {
    Serial.println("\n[I2CScanner] Scanning both SDA/SCL configurations...");

    ScanResult a = scan(18, 8);   // From product pinmap image
    Serial.printf("  Config A (SDA=18 SCL=8):  %d device(s)\n", a.devicesFound);

    ScanResult b = scan(8, 18);   // From T-Embed CC1101 working code
    Serial.printf("  Config B (SDA=8  SCL=18): %d device(s)\n", b.devicesFound);

    ScanResult& winner = (b.devicesFound >= a.devicesFound) ? b : a;

    Serial.printf("[I2CScanner] Using SDA=%d SCL=%d (%d device(s) found)\n",
                  winner.sda, winner.scl, winner.devicesFound);

    if (!winner.foundSI4732)  Serial.println("[I2CScanner] WARNING: SI4732 not found (0x63/0x11)");
    if (!winner.foundES7210)  Serial.println("[I2CScanner] WARNING: ES7210 not found (0x40)");
    if (!winner.foundBQ27220) Serial.println("[I2CScanner] WARNING: BQ27220 not found (0x55)");
    if (!winner.foundBQ25896) Serial.println("[I2CScanner] WARNING: BQ25896 not found (0x6B)");

    if (winner.devicesFound == 0) {
        Serial.println("[I2CScanner] ERROR: No I2C devices found on either config.");
        Serial.println("[I2CScanner] Check hardware — nothing will work correctly.");
    }

    // Leave Wire configured on the winning pins
    Wire.end();
    delay(10);
    Wire.begin(winner.sda, winner.scl, 400000);
    Wire.setTimeOut(100);

    return winner;
}

} // namespace I2CScanner
