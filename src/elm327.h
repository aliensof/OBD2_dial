#pragma once
#include <string>
#include <vector>
#include <climits>
#include <cmath>
#include <cstdint>

#define VAL_NONE INT_MIN

// BLE central talking ELM327 AT/PID text protocol to an OBD2 dongle.
class Elm327 {
public:
    void begin();
    // Scan + connect + AT init. Blocking (several seconds). False if no dongle found.
    bool connect();
    bool isConnected() const;
    bool protoReady() const { return m_proto; }
    // Probe the car bus ("0100"); call when connected but !protoReady (ignition off).
    bool ensureProto();
    const char* status() const { return m_status; }

    int rpm();
    int speedKmh();
    int coolantC();
    int intakeC();
    int loadPct();
    float battVolts(); // NAN on failure
    int dtcCount(bool& milOn);
    std::vector<std::string> readDtcs();
    bool clearDtcs();

    // Send raw command, wait for '>' prompt or timeout, return full response.
    std::string cmd(const std::string& c, uint32_t timeoutMs = 3000);

private:
    bool readPid(const char* pid, const char* echo, uint8_t* out, int n);
    bool m_proto = false;
    const char* m_status = "Starting...";
};
