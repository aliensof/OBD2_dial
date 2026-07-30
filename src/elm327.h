#pragma once
#include <string>
#include <vector>
#include <climits>
#include <cmath>
#include <cstdint>

#define VAL_NONE INT_MIN

struct BleDev {
    std::string label; // "Name -60dB", '*' prefix if it looks like an OBD dongle
    std::string addr;
    uint8_t addrType;
};

// One live-data gauge: how to ask for it and how to turn its bytes into a
// number. ponytail: one table row per gauge beats one switch case per gauge.
struct PidDef {
    uint8_t pid;
    uint8_t nbytes;
    const char* label;
    const char* unit;
    int (*dec)(const uint8_t*); // raw data bytes -> displayed integer
    int lo, hi;                 // range the rim arc spans
};
extern const PidDef kPids[];
extern const int kPidCount;
const PidDef* pidByNumber(uint8_t pid); // nullptr if not in the table

// BLE central talking ELM327 AT/PID text protocol to an OBD2 dongle.
class Elm327 {
public:
    void begin();
    // Connect to the saved dongle + AT init. Blocking. False if none saved / not in range.
    bool connect();
    bool hasSaved() const { return !m_addr.empty(); }
    // Persist this device as "my dongle" and use it from now on.
    void saveDongle(const BleDev& d);
    void disconnect();
    bool isConnected() const;
    bool protoReady() const { return m_proto; }
    // Probe the car bus ("0100"); call when connected but !protoReady (ignition off).
    bool ensureProto();
    const char* status() const { return m_status; }

    // Does this car answer that PID? False before the mask is read (ignition off).
    bool supports(uint8_t pid) const;
    int readPid(const PidDef& p); // VAL_NONE if unsupported / no answer

    float battVolts(); // NAN on failure
    int dtcCount(bool& milOn);
    std::vector<std::string> readDtcs();    // mode 03, stored codes
    std::vector<std::string> readPending(); // mode 07, pending codes
    // Mode 02 snapshot taken when the MIL fired, as display lines. Empty if none.
    std::vector<std::string> freezeFrame();
    bool clearDtcs();

    // Send raw command, wait for '>' prompt or timeout, return full response.
    std::string cmd(const std::string& c, uint32_t timeoutMs = 3000);

    // 5s BLE scan of everything nearby.
    std::vector<BleDev> scanBle();

private:
    bool readBytes(const std::string& req, const std::string& echo, uint8_t* out, int n);
    void loadSupport();
    bool m_proto = false;
    const char* m_status = "Starting...";
    std::string m_addr;
    uint8_t m_addrType = 0;
    uint32_t m_supp[3] = {0, 0, 0}; // supported-PID bitmaps for 01-20, 21-40, 41-60
};
