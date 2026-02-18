/*
 * Fossibot F2400 ESP32 REST API Gateway
 * 
 * REST API for monitoring and controlling Fossibot F2400 via BLE + MODBUS.
 * ESP32 connects to the power station over Bluetooth LE, reads MODBUS registers,
 * and exposes the data through a simple HTTP API.
 *
 * Features:
 * - REST API for reading parameters and control
 * - MODBUS protocol for data acquisition (Input Registers)
 * - MODBUS Write Single Register for output control (AC/USB/DC/Light)
 * - WiFi with hardcoded credentials (edit WIFI_SSID / WIFI_PASSWORD below)
 * - Optional logging (Serial + UDP broadcast)
 * - Safe values for HomeKit (all values >= 0.01, time clamped)
 * - NTP time sync with scheduled daily restart
 * - Fallback restart via millis() if NTP unavailable
 *
 * Based on protocol research from:
 * - https://github.com/Ylianst/ESP-FBot (Apache-2.0)
 * - https://github.com/schauveau/sydpower-mqtt (MIT)
 * - https://github.com/iamslan/fossibot-reverse-engineering
 *
 * License: Apache 2.0
 * ============================================================================
 * REST API ENDPOINTS:
 * ============================================================================
 *
 * READING DATA:
 * GET /api/battery       → Battery level (%, min 0.01)
 * GET /api/timeRemain    → Time remaining (minutes, 1-9999):
 *                          - Status 1 (grid) → time to full charge
 *                          - Status 2 (battery) → time to discharge
 * GET /api/timeRemainHr  → Time remaining (hours.minutes, 0.01-99.59)
 * GET /api/power         → Universal power (W):
 *                          - Status 1 (grid) → input power (min 0.01)
 *                          - Status 2 (battery) → output power (min 0.01)
 *                          - Status 0.01 (off) → 0.01
 * GET /api/status        → Status: 1=grid, 2=battery, 0.01=off/unreachable
 * GET /api/socket220     → AC outlet status (1=on, 0=off)
 * GET /api/socketUSB     → USB ports status (1=on, 0=off)
 * GET /api/all           → All data as JSON
 *
 * CONTROL:
 * GET /api/set?socket220=1   → Turn on AC outlet (220V)
 * GET /api/set?socket220=0   → Turn off AC outlet
 * GET /api/set?socketUSB=1   → Turn on USB ports
 * GET /api/set?socketUSB=0   → Turn off USB ports
 * GET /api/set?socketDC=1    → Turn on DC outputs
 * GET /api/set?socketDC=0    → Turn off DC outputs
 * GET /api/set?socketLight=1 → Turn on LED light
 * GET /api/set?socketLight=0 → Turn off LED light
 * GET /api/restart           → Manual ESP32 restart (3 sec delay)
 *
 * EXAMPLES:
 * curl http://espfossi.local/api/battery
 * curl http://espfossi.local/api/status
 * curl http://espfossi.local/api/set?socket220=1
 *
 * FLASH:
 * esptool -b 921600 write_flash 0x0 fossibot.ino.merged.bin
 */

#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <time.h>

// ============================================================================
// CONFIGURATION — EDIT THESE VALUES
// ============================================================================

#define FIRMWARE_VERSION "5.7"

// Logging (true = Serial + UDP broadcast, false = disabled)
#define ENABLE_LOGGING true

// WiFi credentials
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Fossibot BLE MAC address (find via BLE scanner, device name starts with "FOSSIBOT" or "POWER")
#define FOSSIBOT_MAC "00:00:00:00:00:00"

// Hardware pins (ESP32-WROOM defaults)
#define LED_PIN 16
#define BOOT_BUTTON_PIN 0

// UDP logging port (listen: nc -kulnw0 5678)
#define UDP_LOG_PORT 5678

// ============================================================================
// MODBUS PROTOCOL
// ============================================================================

#define MODBUS_FUNCTION_READ 0x04      // Read Input Registers
#define MODBUS_FUNCTION_WRITE 0x06     // Write Single Register
#define MODBUS_START_ADDR 0x0000
#define MODBUS_REG_COUNT 0x50          // 80 registers

// Control registers (from ESP-FBot)
#define REG_USB_CONTROL 24
#define REG_DC_CONTROL 25
#define REG_AC_CONTROL 26
#define REG_LIGHT_CONTROL 27

// Status flag bits — register 41 (from sydpower-mqtt/MQTT-MODBUS.md)
#define FLAG_AC_INPUT  (1 << 1)   // bit 1  — AC input connected (cable plugged in)
#define FLAG_AC_INPUT2 (1 << 3)   // bit 3  — AC input connected (duplicate of bit 1)
#define FLAG_AC_CHARGE (1 << 4)   // bit 4  — Actively charging from AC
#define FLAG_DC_INPUT  (1 << 5)   // bit 5  — DC input connected (solar/car)
#define FLAG_DC_INPUT2 (1 << 6)   // bit 6  — DC input connected (duplicate of bit 5)
#define FLAG_USB       (1 << 9)   // bit 9  — USB ports enabled
#define FLAG_DC        (1 << 10)  // bit 10 — DC outputs enabled
#define FLAG_AC        (1 << 11)  // bit 11 — AC inverter enabled
#define FLAG_LIGHT     (1 << 12)  // bit 12 — LED light enabled

// Timing
#define POLL_INTERVAL_MS 25000
#define BLE_SCAN_DURATION 1
#define BLE_RECONNECT_INTERVAL 30000

// ============================================================================
// NTP & SCHEDULED RESTART
// ============================================================================

#define NTP_SERVER_1 "ua.pool.ntp.org"
#define NTP_SERVER_2 "pool.ntp.org"
#define NTP_SERVER_3 "time.google.com"
#define TIMEZONE "EET-2EEST,M3.5.0/3,M10.5.0/4"  // Kyiv: UTC+2 winter, UTC+3 summer
#define NTP_TIMEOUT_SEC 600

#define RESTART_HOUR 2
#define RESTART_MINUTE 1
#define FALLBACK_RESTART_MS 87000000UL  // 24h 10min in ms (if NTP unavailable)
#define TIME_CHECK_INTERVAL_MS 60000

// ============================================================================
// BLE UUIDs (from ESP-FBot)
// ============================================================================

static BLEUUID serviceUUID("0000a002-0000-1000-8000-00805f9b34fb");
static BLEUUID charNotifyUUID("0000c305-0000-1000-8000-00805f9b34fb");
static BLEUUID charWriteUUID("0000c304-0000-1000-8000-00805f9b34fb");

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

WebServer server(80);

// BLE
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pNotifyCharacteristic = nullptr;
BLERemoteCharacteristic* pWriteCharacteristic = nullptr;
bool isConnected = false;

// UDP logging
WiFiUDP udp;
IPAddress udpLogIP(255, 255, 255, 255);
bool udpLogActive = false;

// Battery data
struct FossibotData {
    int power;              // Output power (W)
    float status;           // 1=grid, 2=battery, 0.01=off/unreachable
    int charge;             // Input/charge power (W)
    int socket220;          // AC outlet: 1=on, 0=off
    int socketUSB;          // USB ports: 1=on, 0=off
    int time;               // Remaining time (minutes)
    float batteryPercent;   // Battery level (%)
    int inputPower;         // Input power (W)
    int outputPower;        // Output power (W)
    bool dcActive;          // DC outputs active
    bool lightActive;       // LED light active
    float acVoltage;        // AC voltage (V)
    float acFrequency;      // AC frequency (Hz)
    unsigned long lastUpdate;

    FossibotData() {
        power = 0;
        status = 0.01;  // Unknown/off (HomeKit-safe, never 0)
        charge = 0;
        socket220 = 0;
        socketUSB = 0;
        time = 0;
        batteryPercent = 0.0;
        inputPower = 0;
        outputPower = 0;
        dcActive = false;
        lightActive = false;
        acVoltage = 0;
        acFrequency = 0;
        lastUpdate = 0;
    }
} fossibotData;

// Timers
unsigned long lastPollTime = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long ledBlinkTime = 0;
bool ledState = false;
unsigned long bleConnectTime = 0;

// Failed poll counter — after MAX_FAILED_POLLS consecutive failures, status → 0.01
uint8_t failedPollCount = 0;
#define MAX_FAILED_POLLS 10

// Time registers for API
uint16_t g_remainingTimeMinutes = 0;  // Reg[59] — time to discharge (minutes)
uint16_t g_timeToFullMinutes = 0;     // Reg[58] — time to full charge (minutes)

// NTP state
bool ntpSynced = false;
time_t bootTime = 0;
unsigned long ntpSyncStartTime = 0;

// Restart state
bool restartScheduledToday = false;
unsigned long lastTimeCheck = 0;
unsigned long bootMillis = 0;

// ============================================================================
// UDP LOGGING
// ============================================================================

void udpLog(const char* format, ...) {
#if ENABLE_LOGGING
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Serial.print(buffer);
    if (udpLogActive && WiFi.isConnected()) {
        udp.beginPacket(udpLogIP, UDP_LOG_PORT);
        udp.write((const uint8_t*)buffer, strlen(buffer));
        udp.endPacket();
    }
#endif
}

void udpLogLn(const char* format, ...) {
#if ENABLE_LOGGING
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Serial.println(buffer);
    if (udpLogActive && WiFi.isConnected()) {
        udp.beginPacket(udpLogIP, UDP_LOG_PORT);
        udp.write((const uint8_t*)buffer, strlen(buffer));
        udp.write((const uint8_t*)"\n", 1);
        udp.endPacket();
    }
#endif
}

// ============================================================================
// LED INDICATION
// ============================================================================

void ledOn() { digitalWrite(LED_PIN, HIGH); ledState = true; }
void ledOff() { digitalWrite(LED_PIN, LOW); ledState = false; }

void ledBlink(int times, int delayMs = 100) {
    for (int i = 0; i < times; i++) {
        ledOn(); delay(delayMs);
        ledOff(); delay(delayMs);
    }
}

void updateLedStatus() {
    // Solid: connected to both WiFi and Fossibot
    // Slow blink (1s): WiFi ok, no Fossibot
    // Fast blink (0.2s): no WiFi
    if (!WiFi.isConnected()) {
        if (millis() - ledBlinkTime > 200) {
            ledState = !ledState;
            digitalWrite(LED_PIN, ledState);
            ledBlinkTime = millis();
        }
    } else if (!isConnected) {
        if (millis() - ledBlinkTime > 1000) {
            ledState = !ledState;
            digitalWrite(LED_PIN, ledState);
            ledBlinkTime = millis();
        }
    } else {
        if (!ledState) ledOn();
    }
}

// ============================================================================
// BLE CALLBACKS
// ============================================================================

class MyClientCallback : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) override {
        udpLogLn("✅ BLE: Connected to Fossibot");
        isConnected = true;
        fossibotData.status = 0.01;
        failedPollCount = 0;
        bleConnectTime = millis();
        ledBlink(2, 50);
    }

    void onDisconnect(BLEClient* pclient) override {
        udpLogLn("❌ BLE: Disconnected from Fossibot");
        isConnected = false;
        fossibotData.status = 0.01;
    }
};

static MyClientCallback clientCallback;

static void notifyCallback(
    BLERemoteCharacteristic* pBLERemoteCharacteristic,
    uint8_t* pData, size_t length, bool isNotify) {
    parseFossibotData(pData, length);
}

// ============================================================================
// MODBUS RTU PARSING
// ============================================================================

// Extract register value from MODBUS response.
// Fossibot echoes the 6-byte request before raw register data.
// Layout: [echo(6 bytes)][reg0_hi][reg0_lo][reg1_hi][reg1_lo]...
uint16_t getModbusRegister(const uint8_t* data, uint16_t length, uint16_t regIndex) {
    uint16_t offset = 6 + (regIndex * 2);
    if (offset + 1 >= length) return 0;
    return (data[offset] << 8) | data[offset + 1];
}

void parseFossibotData(uint8_t* data, size_t length) {
    // Debug: raw data
    udpLog("📥 RAW (%d bytes): ", length);
    for (size_t i = 0; i < min(length, (size_t)32); i++) {
        udpLog("%02X ", data[i]);
    }
    if (length > 32) udpLog("...");
    udpLogLn("");

    // Debug: check offsets
    if (length >= 8) {
        udpLogLn("🔍 DEBUG offsets:");
        udpLogLn("   Offset 0: %02X %02X %02X", data[0], data[1], data[2]);
        if (length >= 10)
            udpLogLn("   Offset 6: %02X %02X %02X", data[6], data[7], data[8]);
        if (length >= 12)
            udpLogLn("   Offset 8: %02X %02X %02X", data[8], data[9], data[10]);
    }

    if (length < 7) {
        udpLogLn("⚠️  MODBUS: Response too short (%d bytes)", length);
        return;
    }

    uint8_t deviceAddr = data[0];
    uint8_t functionCode = data[1];
    uint8_t byteCount = data[2];

    // Verify full packet received
    size_t expectedLength = 3 + byteCount + 2;
    if (length < expectedLength) {
        udpLogLn("⚠️  MODBUS: Incomplete packet! Got %d, expected %d bytes", length, expectedLength);
        udpLogLn("   MTU might be too small (%d bytes)", pClient->getMTU());
        return;
    }
    udpLogLn("✅ MODBUS: Full packet (%d bytes, expected %d)", length, expectedLength);

    // NOTE: CRC is NOT validated. Fossibot uses non-standard format:
    // echo of request (6 bytes) + raw register data. CRC position unknown.

    if (deviceAddr != 0x11) {
        udpLogLn("⚠️  MODBUS: Wrong device address: 0x%02X", deviceAddr);
        return;
    }

    // 0x06 = Write Single Register response (just an echo)
    if (functionCode == 0x06) {
        udpLogLn("✅ MODBUS: Write register confirmed (0x06)");
        return;
    }

    if (functionCode != 0x03 && functionCode != 0x04) {
        udpLogLn("⚠️  MODBUS: Unknown function code: 0x%02X", functionCode);
        return;
    }

    const char* funcName = (functionCode == 0x03) ? "Holding" : "Input";
    udpLogLn("✅ MODBUS: %s Registers (%d bytes, %d regs)", funcName, length, byteCount / 2);

    // Debug: dump first 10 registers
    if (length >= 20) {
        udpLog("🔍 DATA (offset 6): ");
        for (int i = 6; i < min((int)length, 20); i++) {
            udpLog("%02X ", data[i]);
        }
        udpLogLn("...");

        udpLogLn("🔢 REGISTERS:");
        for (int i = 0; i < 10 && (6 + i*2 + 1) < length; i++) {
            uint16_t val = (data[6 + i*2] << 8) | data[6 + i*2 + 1];
            udpLogLn("   Reg[%2d] = 0x%04X = %5d", i, val, val);
        }

        // Critical registers 56-60
        udpLogLn("🔢 KEY REGISTERS:");
        for (int i = 56; i <= 60 && (6 + i*2 + 1) < length; i++) {
            uint16_t val = (data[6 + i*2] << 8) | data[6 + i*2 + 1];
            const char* name = "";
            if (i == 56) name = " (Battery %)";
            else if (i == 58) name = " (Time to Full min)";
            else if (i == 59) name = " (Remaining Time min)";
            udpLogLn("   Reg[%2d] = 0x%04X = %5d%s", i, val, val, name);
        }
    }

    // Parse key registers (indices from ESP-FBot)
    float batteryPercent = getModbusRegister(data, length, 56) / 10.0f;
    uint16_t acInputWatts = getModbusRegister(data, length, 3);
    uint16_t dcInputWatts = getModbusRegister(data, length, 4);
    uint16_t inputWatts = getModbusRegister(data, length, 6);
    uint16_t outputWatts = getModbusRegister(data, length, 39);
    uint16_t totalWatts = getModbusRegister(data, length, 20);
    uint16_t systemWatts = getModbusRegister(data, length, 21);

    uint16_t timeToFullMinutes = getModbusRegister(data, length, 58);
    uint16_t remainingTimeMinutes = getModbusRegister(data, length, 59);

    // Register 41 — output status flags
    uint16_t stateFlags = getModbusRegister(data, length, 41);
    bool usbActive = (stateFlags & FLAG_USB) != 0;
    bool dcActive = (stateFlags & FLAG_DC) != 0;
    bool acActive = (stateFlags & FLAG_AC) != 0;
    bool lightActive = (stateFlags & FLAG_LIGHT) != 0;

    fossibotData.batteryPercent = batteryPercent;
    fossibotData.inputPower = inputWatts;
    fossibotData.outputPower = outputWatts;
    fossibotData.power = outputWatts;
    fossibotData.charge = inputWatts;

    // Determine power source from register 41 bit flags:
    // bit 1/3 = AC input physically connected (even when battery full, charger idle)
    // bit 5/6 = DC input connected (solar panel / car charger)
    bool acInputConnected = (stateFlags & FLAG_AC_INPUT) || (stateFlags & FLAG_AC_INPUT2);
    bool dcInputConnected = (stateFlags & FLAG_DC_INPUT) || (stateFlags & FLAG_DC_INPUT2);

    if (acInputConnected || dcInputConnected) {
        fossibotData.status = 1; // Grid power (AC or DC input connected)
    } else {
        fossibotData.status = 2; // Battery power
    }

    // Successful parse — reset failure counter
    failedPollCount = 0;

    udpLogLn("🔌 STATUS: %s (stateFlags=0x%04X, AC_IN=%d, DC_IN=%d)",
             acInputConnected || dcInputConnected ? "GRID" : "BATTERY",
             stateFlags, acInputConnected, dcInputConnected);

    fossibotData.socketUSB = usbActive ? 1 : 0;
    fossibotData.dcActive = dcActive;
    fossibotData.socket220 = acActive ? 1 : 0;
    fossibotData.lightActive = lightActive;

    // Clamp time values to sane range
    if (remainingTimeMinutes > 9999) remainingTimeMinutes = 9999;
    if (timeToFullMinutes > 9999) timeToFullMinutes = 9999;

    g_remainingTimeMinutes = remainingTimeMinutes;
    g_timeToFullMinutes = timeToFullMinutes;
    fossibotData.lastUpdate = millis();

    int remainHours = remainingTimeMinutes / 60;
    int remainMins = remainingTimeMinutes % 60;

    udpLogLn("📊 DATA: Battery=%.1f%%, In=%dW (AC:%dW DC:%dW), Out=%dW, Total=%dW",
             fossibotData.batteryPercent,
             inputWatts, acInputWatts, dcInputWatts,
             outputWatts, totalWatts);
    udpLogLn("⏱️  TIME: ToFull=%d min, Remaining=%d min (%dh %dm)",
             timeToFullMinutes, remainingTimeMinutes, remainHours, remainMins);
    udpLogLn("🔌 OUTPUTS: AC=%d, USB=%d, DC=%d, Light=%d, Status=%.2f",
             acActive, usbActive, dcActive, lightActive, fossibotData.status);
}

// ============================================================================
// BLE CONNECTION
// ============================================================================

bool connectToFossibot() {
    lastReconnectAttempt = millis();

    if (strlen(FOSSIBOT_MAC) < 17) {
        udpLogLn("❌ BLE: Invalid MAC address");
        return false;
    }

    udpLogLn("🔄 BLE: Connecting to %s...", FOSSIBOT_MAC);

    if (pClient != nullptr) {
        delete pClient;
        pClient = nullptr;
    }

    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallback);

    BLEAddress address(FOSSIBOT_MAC);
    if (!pClient->connect(address, BLE_ADDR_TYPE_PUBLIC)) {
        udpLogLn("❌ BLE: Connection failed");
        delete pClient;
        pClient = nullptr;
        return false;
    }
    udpLogLn("✅ BLE: Connected");

    // Increase MTU for large packets (Fossibot sends ~165 bytes for 80 registers)
    int currentMTU = pClient->getMTU();
    udpLogLn("🔧 BLE: Current MTU = %d", currentMTU);
    bool mtuSet = pClient->setMTU(512);
    if (mtuSet) {
        delay(100);
        udpLogLn("✅ BLE: MTU set to %d", pClient->getMTU());
    } else {
        udpLogLn("⚠️  BLE: MTU negotiation failed, staying at %d", currentMTU);
    }

    // Get service
    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == nullptr) {
        udpLogLn("❌ BLE: Service not found");
        pClient->disconnect();
        delete pClient;
        pClient = nullptr;
        return false;
    }
    udpLogLn("✅ BLE: Service A002 found");

    // Debug: list all characteristics
    udpLogLn("🔍 BLE: Characteristics in service A002:");
    std::map<std::string, BLERemoteCharacteristic*>* charMap = pRemoteService->getCharacteristics();
    if (charMap != nullptr) {
        int charCount = 0;
        for (auto &pair : *charMap) {
            BLERemoteCharacteristic* chr = pair.second;
            udpLogLn("   [%d] UUID: %s, Handle: 0x%04X",
                     ++charCount,
                     chr->getUUID().toString().c_str(),
                     chr->getHandle());
        }
        udpLogLn("   Total: %d characteristics", charCount);
    }

    // Get characteristics
    pNotifyCharacteristic = pRemoteService->getCharacteristic(charNotifyUUID);
    pWriteCharacteristic = pRemoteService->getCharacteristic(charWriteUUID);

    if (pNotifyCharacteristic == nullptr || pWriteCharacteristic == nullptr) {
        udpLogLn("❌ BLE: Characteristics not found");
        if (pNotifyCharacteristic == nullptr) udpLogLn("   - C305 (NOTIFY) missing");
        if (pWriteCharacteristic == nullptr) udpLogLn("   - C304 (WRITE) missing");
        pClient->disconnect();
        delete pClient;
        pClient = nullptr;
        return false;
    }

    udpLogLn("✅ BLE: Characteristics found");
    udpLogLn("   - C305 (NOTIFY) handle: 0x%04X", pNotifyCharacteristic->getHandle());
    udpLogLn("   - C304 (WRITE) handle: 0x%04X", pWriteCharacteristic->getHandle());

    // Subscribe to notifications on C305
    if (pNotifyCharacteristic->canNotify()) {
        pNotifyCharacteristic->registerForNotify(notifyCallback);

        // Manually enable CCCD (some Arduino BLE stacks don't do this automatically)
        BLERemoteDescriptor* pDescriptor = pNotifyCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902));
        if (pDescriptor != nullptr) {
            uint8_t notificationOn[] = {0x01, 0x00};
            pDescriptor->writeValue(notificationOn, 2, true);
            udpLogLn("✅ BLE: CCCD descriptor written (notifications enabled)");
        } else {
            udpLogLn("⚠️  BLE: CCCD descriptor not found!");
        }
        udpLogLn("✅ BLE: Subscribed to C305 notifications");
    } else {
        udpLogLn("⚠️  BLE: C305 does not support NOTIFY!");
    }

    isConnected = true;
    bleConnectTime = millis();
    ledBlink(3, 100);

    // First MODBUS request will be sent from loop() after 2 sec delay
    // (gives time for notification subscription to register)
    return true;
}

// ============================================================================
// MODBUS COMMANDS
// ============================================================================

uint16_t calculateModbusCRC(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

void sendModbusReadCommand() {
    if (!isConnected || pWriteCharacteristic == nullptr) {
        udpLogLn("❌ MODBUS: Not connected");
        return;
    }

    uint8_t command[8];
    command[0] = 0x11;
    command[1] = MODBUS_FUNCTION_READ;
    command[2] = (MODBUS_START_ADDR >> 8) & 0xFF;
    command[3] = MODBUS_START_ADDR & 0xFF;
    command[4] = (MODBUS_REG_COUNT >> 8) & 0xFF;
    command[5] = MODBUS_REG_COUNT & 0xFF;

    uint16_t crc = calculateModbusCRC(command, 6);
    command[6] = (crc >> 8) & 0xFF;
    command[7] = crc & 0xFF;

    udpLog("📤 MODBUS: Read Input Regs (0x04, addr 0x%04X, count %d) CRC=0x%04X → ",
           MODBUS_START_ADDR, MODBUS_REG_COUNT, crc);
    for (size_t i = 0; i < 8; i++) udpLog("%02X ", command[i]);
    udpLogLn("");

    pWriteCharacteristic->writeValue(command, 8, true);
}

bool sendCommand(uint8_t* data, size_t length) {
    if (!isConnected || pWriteCharacteristic == nullptr) {
        udpLogLn("❌ BLE: Not connected");
        return false;
    }
    udpLog("📤 MODBUS: Control command: ");
    for (size_t i = 0; i < length; i++) udpLog("%02X ", data[i]);
    udpLogLn("");
    pWriteCharacteristic->writeValue(data, length, false);
    return true;
}

void sendModbusWriteCommand(uint16_t reg, uint16_t value) {
    uint8_t command[8];
    command[0] = 0x11;
    command[1] = MODBUS_FUNCTION_WRITE;
    command[2] = (reg >> 8) & 0xFF;
    command[3] = reg & 0xFF;
    command[4] = (value >> 8) & 0xFF;
    command[5] = value & 0xFF;
    uint16_t crc = calculateModbusCRC(command, 6);
    command[6] = (crc >> 8) & 0xFF;
    command[7] = crc & 0xFF;
    sendCommand(command, 8);
}

// AC outlet uses TOGGLE mode — only send command if state differs
bool setACOutput(bool enable) {
    if (fossibotData.socket220 == (enable ? 1 : 0)) {
        udpLogLn("🔌 AC: already %s", enable ? "ON" : "OFF");
        return true;
    }
    udpLogLn("🔌 AC: toggling → %s", enable ? "ON" : "OFF");
    sendModbusWriteCommand(REG_AC_CONTROL, 1);
    lastPollTime = millis() - POLL_INTERVAL_MS + 3000; // Poll in 3 sec for confirmation
    return true;
}

bool setUSBOutput(bool enable) {
    udpLogLn("🔌 USB: %s", enable ? "ON" : "OFF");
    sendModbusWriteCommand(REG_USB_CONTROL, enable ? 1 : 0);
    lastPollTime = millis() - POLL_INTERVAL_MS + 3000;
    return true;
}

bool setDCOutput(bool enable) {
    udpLogLn("🔌 DC: %s", enable ? "ON" : "OFF");
    sendModbusWriteCommand(REG_DC_CONTROL, enable ? 1 : 0);
    lastPollTime = millis() - POLL_INTERVAL_MS + 3000;
    return true;
}

bool setLightOutput(bool enable) {
    udpLogLn("💡 LED: %s", enable ? "ON" : "OFF");
    sendModbusWriteCommand(REG_LIGHT_CONTROL, enable ? 1 : 0);
    lastPollTime = millis() - POLL_INTERVAL_MS + 3000;
    return true;
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

float calcTimeRemainHr() {
    if (g_timeToFullMinutes > 0) return 99999.0f;
    uint16_t safeTime = min(g_remainingTimeMinutes, (uint16_t)9999);
    int hours = safeTime / 60;
    int minutes = safeTime % 60;
    return min(hours + (minutes / 100.0f), 99.59f);
}

// Universal power: input when on grid, output when on battery
float getUniversalPower() {
    if (fossibotData.status == 1) {
        return (fossibotData.charge < 0.01) ? 0.01f : (float)fossibotData.charge;
    } else if (fossibotData.status == 2) {
        return (fossibotData.power < 0.01) ? 0.01f : (float)fossibotData.power;
    }
    return 0.01f;
}

// Universal time: time-to-full when charging, time-remaining when discharging
uint16_t getUniversalTimeMinutes() {
    uint16_t t = (g_timeToFullMinutes > 0) ? g_timeToFullMinutes : g_remainingTimeMinutes;
    if (t > 9999) t = 9999;
    if (t < 1) t = 1;
    return t;
}

float getUniversalTimeHr() {
    float timeHr;
    if (g_timeToFullMinutes > 0) {
        uint16_t safeTime = min(max(g_timeToFullMinutes, (uint16_t)1), (uint16_t)9999);
        timeHr = (safeTime / 60) + ((safeTime % 60) / 100.0f);
    } else {
        timeHr = calcTimeRemainHr();
    }
    return max(min(timeHr, 99.59f), 0.01f);
}

float getSafeBattery() {
    return (fossibotData.batteryPercent < 0.01f) ? 0.01f : fossibotData.batteryPercent;
}

// ============================================================================
// REST API HANDLERS
// ============================================================================

void handleRoot() {
    const char* statusStr = "off";
    if (fossibotData.status == 1) statusStr = "grid";
    else if (fossibotData.status == 2) statusStr = "battery";

    uint16_t displayTime = getUniversalTimeMinutes();
    float timeHr = getUniversalTimeHr();
    float displayPower = getUniversalPower();
    float batteryDisplay = getSafeBattery();

    char timeStr[16];
    itoa(displayTime, timeStr, 10);
    char timeHrStr[16];
    snprintf(timeHrStr, sizeof(timeHrStr), "%.2f", timeHr);

    // Chunked response — no large buffer allocation needed
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.send(200, "text/html", "");

    server.sendContent_P(PSTR(
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Fossibot API</title>"
        "<style>"
        "body{font-family:monospace;padding:20px;background:#1a1a1a;color:#0f0;font-size:15px}"
        "h1{color:#0f0;font-size:23px}"
        "a{color:#0ff;text-decoration:none}a:hover{text-decoration:underline}"
        "pre{background:#000;padding:15px;border:1px solid #0f0;margin:10px 0;font-size:13px;white-space:pre-wrap;word-break:break-all}"
        ".live{color:#0ff;font-weight:bold}"
        "p{font-size:15px}"
        "</style></head><body>"
        "<h1>Fossibot F2400 REST API</h1><pre>"
    ));

    char buf[256];

    snprintf(buf, sizeof(buf),
        "GET <a href='/api/battery'>/api/battery</a>       → Battery (<span class='live'>%.2f%%</span>)\n",
        batteryDisplay);
    server.sendContent(buf);

    snprintf(buf, sizeof(buf),
        "GET <a href='/api/timeRemain'>/api/timeRemain</a>    → Time (<span class='live'>%s min</span>)\n",
        timeStr);
    server.sendContent(buf);

    snprintf(buf, sizeof(buf),
        "GET <a href='/api/timeRemainHr'>/api/timeRemainHr</a>  → Time (<span class='live'>%s</span>)\n",
        timeHrStr);
    server.sendContent(buf);

    snprintf(buf, sizeof(buf),
        "GET <a href='/api/power'>/api/power</a>         → Power (<span class='live'>%.2fW</span>)\n",
        displayPower);
    server.sendContent(buf);

    snprintf(buf, sizeof(buf),
        "GET <a href='/api/status'>/api/status</a>        → Status (<span class='live'>%s</span>)\n",
        statusStr);
    server.sendContent(buf);

    snprintf(buf, sizeof(buf),
        "GET <a href='/api/socket220'>/api/socket220</a>     → AC (<span class='live'>%s</span>)\n",
        fossibotData.socket220 ? "ON" : "OFF");
    server.sendContent(buf);

    snprintf(buf, sizeof(buf),
        "GET <a href='/api/socketUSB'>/api/socketUSB</a>     → USB (<span class='live'>%s</span>)\n",
        fossibotData.socketUSB ? "ON" : "OFF");
    server.sendContent(buf);

    server.sendContent_P(PSTR(
        "GET <a href='/api/all'>/api/all</a>           → All (JSON)\n"
        "GET <a href='/api/restart'>/api/restart</a>       → Manual Restart\n\n"
        "GET <a href='/api/set?socket220=1'>/api/set?socket220=1</a>\n"
        "GET <a href='/api/set?socket220=0'>/api/set?socket220=0</a>\n"
        "GET <a href='/api/set?socketUSB=1'>/api/set?socketUSB=1</a>\n"
        "GET <a href='/api/set?socketUSB=0'>/api/set?socketUSB=0</a>\n"
        "GET <a href='/api/set?socketDC=1'>/api/set?socketDC=1</a>\n"
        "GET <a href='/api/set?socketDC=0'>/api/set?socketDC=0</a>\n"
        "GET <a href='/api/set?socketLight=1'>/api/set?socketLight=1</a>\n"
        "GET <a href='/api/set?socketLight=0'>/api/set?socketLight=0</a>\n"
        "</pre>"
    ));

    snprintf(buf, sizeof(buf),
        "<p>Device: <span class='live'>%s</span></p>"
        "<p>IP: <span class='live'>%s</span></p>",
        isConnected ? "Connected" : "Disconnected",
        WiFi.localIP().toString().c_str());
    server.sendContent(buf);

    if (ntpSynced) {
        time_t now = time(nullptr);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        char currentTime[32];
        strftime(currentTime, sizeof(currentTime), "%Y-%m-%d %H:%M:%S", &timeinfo);
        snprintf(buf, sizeof(buf),
            "<p>Time: <span class='live'>%s</span></p>"
            "<p>Next restart: <span class='live'>%02d:%02d</span></p>",
            currentTime, RESTART_HOUR, RESTART_MINUTE);
        server.sendContent(buf);
    } else {
        snprintf(buf, sizeof(buf),
            "<p>NTP: <span class='live'>Not synced (fallback in %.1fh)</span></p>",
            (FALLBACK_RESTART_MS - millis()) / 3600000.0f);
        server.sendContent(buf);
    }

    server.sendContent_P(PSTR("</body></html>"));
    server.sendContent("");
}

void handlePower() {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", getUniversalPower());
    server.send(200, "text/plain", buf);
}

void handleStatus() {
    char buf[8];
    snprintf(buf, sizeof(buf), "%.2f", fossibotData.status);
    server.send(200, "text/plain", buf);
}

void handleSocket220() {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", fossibotData.socket220);
    server.send(200, "text/plain", buf);
}

void handleSocketUSB() {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", fossibotData.socketUSB);
    server.send(200, "text/plain", buf);
}

void handleBattery() {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", getSafeBattery());
    server.send(200, "text/plain", buf);
}

void handleTimeRemain() {
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", getUniversalTimeMinutes());
    server.send(200, "text/plain", buf);
}

void handleTimeRemainHr() {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%.2f", getUniversalTimeHr());
    server.send(200, "text/plain", buffer);
}

void handleAll() {
    StaticJsonDocument<512> doc;
    doc["power"] = getUniversalPower();
    doc["status"] = fossibotData.status;
    doc["socket220"] = fossibotData.socket220;
    doc["socketUSB"] = fossibotData.socketUSB;
    doc["batteryPercent"] = getSafeBattery();
    doc["timeRemain"] = getUniversalTimeMinutes();
    doc["timeRemainHr"] = getUniversalTimeHr();
    doc["connected"] = isConnected;
    doc["lastUpdate"] = fossibotData.lastUpdate;
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleSet() {
    String response = "";
    bool hasChanges = false;

    if (!isConnected) {
        server.send(503, "text/plain", "ERROR: Not connected to Fossibot");
        return;
    }

    if (server.hasArg("socket220")) {
        int value = server.arg("socket220").toInt();
        if (setACOutput(value == 1)) {
            response += "AC: " + String(value == 1 ? "ON" : "OFF") + "\n";
            hasChanges = true;
        } else { response += "AC: ERROR\n"; }
    }

    if (server.hasArg("socketUSB")) {
        int value = server.arg("socketUSB").toInt();
        if (setUSBOutput(value == 1)) {
            response += "USB: " + String(value == 1 ? "ON" : "OFF") + "\n";
            hasChanges = true;
        } else { response += "USB: ERROR\n"; }
    }

    if (server.hasArg("socketDC")) {
        int value = server.arg("socketDC").toInt();
        if (setDCOutput(value == 1)) {
            response += "DC: " + String(value == 1 ? "ON" : "OFF") + "\n";
            hasChanges = true;
        } else { response += "DC: ERROR\n"; }
    }

    if (server.hasArg("socketLight")) {
        int value = server.arg("socketLight").toInt();
        if (setLightOutput(value == 1)) {
            response += "Light: " + String(value == 1 ? "ON" : "OFF") + "\n";
            hasChanges = true;
        } else { response += "Light: ERROR\n"; }
    }

    if (!hasChanges) {
        response = "No valid parameters. Use: ?socket220=1 ?socketUSB=0 ?socketDC=1 ?socketLight=0";
    }
    server.send(200, "text/plain", response);
}

// ============================================================================
// NTP & SCHEDULED RESTART
// ============================================================================

void initNTP() {
    udpLogLn("🕐 NTP: Starting sync...");
    udpLogLn("   Servers: %s, %s, %s", NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    udpLogLn("   Timezone: %s", TIMEZONE);

    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    setenv("TZ", TIMEZONE, 1);
    tzset();
    ntpSyncStartTime = millis();

    // Let WiFi/DNS settle before NTP
    delay(2000);

    int attempts = 0;
    while (attempts < 20) {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            char timeStr[64];
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
            ntpSynced = true;
            bootTime = now;
            udpLogLn("✅ NTP: Synced! Time: %s", timeStr);
            udpLogLn("   Next restart: %02d:%02d", RESTART_HOUR, RESTART_MINUTE);
            return;
        }
        delay(1000);
        attempts++;
        udpLog(".");
    }
    udpLogLn("");
    udpLogLn("⚠️  NTP: Sync failed (20s timeout)");
    udpLogLn("   Fallback restart in %.1f hours", FALLBACK_RESTART_MS / 3600000.0f);
    ntpSynced = false;
}

bool checkScheduledRestart() {
    if (!ntpSynced) return false;
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_hour == RESTART_HOUR && timeinfo.tm_min == RESTART_MINUTE) {
        if (!restartScheduledToday) {
            restartScheduledToday = true;
            return true;
        }
    } else {
        restartScheduledToday = false;
    }
    return false;
}

bool checkFallbackRestart() {
    if (ntpSynced) return false;
    unsigned long uptime = millis();
    if (uptime >= FALLBACK_RESTART_MS) {
        udpLogLn("⚠️  FALLBACK: NTP unavailable, uptime %.1f hours", uptime / 3600000.0f);
        return true;
    }
    // Retry NTP sync every 10 minutes
    if (millis() - ntpSyncStartTime > 600000) {
        udpLogLn("🔄 NTP: Retrying sync...");
        time_t now = time(nullptr);
        if (now > 1700000000) {
            ntpSynced = true;
            bootTime = now;
            udpLogLn("✅ NTP: Synced on retry!");
        } else {
            udpLogLn("⚠️  NTP: Retry failed");
        }
        ntpSyncStartTime = millis();
    }
    return false;
}

void performRestart(const char* reason) {
    udpLogLn("\n========================================");
    udpLogLn("🔄 SYSTEM: RESTARTING");
    udpLogLn("========================================");
    udpLogLn("Reason: %s", reason);
    if (ntpSynced) {
        time_t now = time(nullptr);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        udpLogLn("Time: %s", timeStr);
    }
    udpLogLn("Uptime: %.2f hours", millis() / 3600000.0f);
    udpLogLn("Free Heap: %d bytes", ESP.getFreeHeap());
    udpLogLn("========================================\n");
    delay(3000);
    ESP.restart();
}

void handleRestart() {
    udpLogLn("🔄 API: Manual restart requested via /api/restart");
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "OK: Restarting in 3 seconds...");
    delay(500);
    performRestart("Manual restart via /api/restart");
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n========================================");
    Serial.println("  Fossibot F2400 ESP32 Gateway v" FIRMWARE_VERSION);
    Serial.println("========================================\n");

    pinMode(LED_PIN, OUTPUT);
    ledOff();
    ledBlink(3, 200);

    // WiFi
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("espfossi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        ledBlink(1, 50);
        attempts++;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ WiFi connection failed! Restarting...");
        delay(3000);
        ESP.restart();
    }

    Serial.println("✅ WiFi Connected!");
    Serial.printf("   IP: %s\n", WiFi.localIP().toString().c_str());
    WiFi.setSleep(false);

    // NTP
    bootMillis = millis();
    initNTP();

    // UDP logging
#if ENABLE_LOGGING
    udp.begin(UDP_LOG_PORT);
    udpLogActive = true;
    udpLogLn("\n========================================");
    udpLogLn("  UDP LOG STARTED (port %d)", UDP_LOG_PORT);
    udpLogLn("  Listen: nc -kulnw0 %d", UDP_LOG_PORT);
    udpLogLn("========================================\n");
#endif

    // BLE
    udpLogLn("🔵 BLE: Initializing...");
    BLEDevice::init("ESP32-Fossibot");
    if (!connectToFossibot()) {
        udpLogLn("⚠️  BLE: Initial connection failed, will retry in 10 sec");
    }

    // REST API routes
    server.on("/", handleRoot);
    server.on("/api/battery", handleBattery);
    server.on("/api/timeRemain", handleTimeRemain);
    server.on("/api/timeRemainHr", handleTimeRemainHr);
    server.on("/api/power", handlePower);
    server.on("/api/status", handleStatus);
    server.on("/api/socket220", handleSocket220);
    server.on("/api/socketUSB", handleSocketUSB);
    server.on("/api/all", handleAll);
    server.on("/api/set", handleSet);
    server.on("/api/restart", handleRestart);

    server.begin();
    udpLogLn("🌐 HTTP: Server started on port 80");
    udpLogLn("   URL: http://%s", WiFi.localIP().toString().c_str());

    if (MDNS.begin("espfossi")) {
        udpLogLn("✅ mDNS: http://espfossi.local");
        MDNS.addService("http", "tcp", 80);
    }

    udpLogLn("\n✅ Initialization complete!\n");
    ledBlink(5, 100);
}

// ============================================================================
// LOOP
// ============================================================================

void loop() {
    server.handleClient();
    yield();

    unsigned long now = millis();

    // Scheduled restart check (once per minute)
    if (now - lastTimeCheck >= TIME_CHECK_INTERVAL_MS) {
        lastTimeCheck = now;
        if (checkScheduledRestart()) {
            char reason[64];
            snprintf(reason, sizeof(reason), "Scheduled restart at %02d:%02d (NTP)",
                     RESTART_HOUR, RESTART_MINUTE);
            performRestart(reason);
            return;
        }
        if (checkFallbackRestart()) {
            performRestart("Fallback restart (24h 10m, NTP unavailable)");
            return;
        }
    }

    updateLedStatus();

    // WiFi reconnect
    if (!WiFi.isConnected()) {
        static unsigned long lastWiFiReconnect = 0;
        if (millis() - lastWiFiReconnect > 30000) {
            udpLogLn("⚠️  WiFi: Reconnecting...");
            WiFi.reconnect();
            lastWiFiReconnect = millis();
        }
    }

    // MODBUS polling
    if (isConnected) {
        // First request 2 sec after BLE connect (let notification subscription register)
        if (bleConnectTime > 0 && (now - bleConnectTime) >= 2000) {
            udpLogLn("🚀 LOOP: First MODBUS request (2s after connect)");
            sendModbusReadCommand();
            bleConnectTime = 0;
            lastPollTime = now;
        }

        // Regular polling
        if (now - lastPollTime > POLL_INTERVAL_MS) {
            udpLogLn("⏰ LOOP: Poll interval (%dms)", POLL_INTERVAL_MS);
            sendModbusReadCommand();
            lastPollTime = now;

            // Increment failure counter; parseFossibotData() resets it on success
            failedPollCount++;
            if (failedPollCount >= MAX_FAILED_POLLS) {
                fossibotData.status = 0.01;
                udpLogLn("⚠️  LOOP: %d polls without response → status = 0.01", failedPollCount);
            }
        }
    }

    // Track failures when BLE is disconnected too
    if (!isConnected) {
        static unsigned long lastDisconnectedCheck = 0;
        if (now - lastDisconnectedCheck > POLL_INTERVAL_MS) {
            failedPollCount++;
            lastDisconnectedCheck = now;
            if (failedPollCount >= MAX_FAILED_POLLS && fossibotData.status > 0.02) {
                fossibotData.status = 0.01;
                udpLogLn("⚠️  LOOP: BLE disconnected %d cycles → status = 0.01", failedPollCount);
            }
        }
    }

    // BLE reconnect
    if (!isConnected && (millis() - lastReconnectAttempt > BLE_RECONNECT_INTERVAL)) {
        udpLogLn("🔄 BLE: Reconnecting...");
        connectToFossibot();
    }

    delay(2);
}
