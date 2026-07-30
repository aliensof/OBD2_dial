#include "elm327.h"
#include "parse.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

// Connection-time logging is always on (once per connect, and it's what you
// need when a dongle misbehaves). Build with -DOBD2_TRACE for per-command
// traffic, which is far too chatty to leave on at 3 polls/sec.
#define LOG(...) Serial.printf(__VA_ARGS__)
#ifdef OBD2_TRACE
#define TRACE(...) Serial.printf(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

// Print a response with control chars visible.
static void logRaw(const char* tag, const std::string& s)
{
    LOG("%s [%d] \"", tag, (int)s.size());
    for (char c : s) {
        if (c == '\r') LOG("\\r");
        else if (c == '\n') LOG("\\n");
        else if (isprint((unsigned char)c)) LOG("%c", c);
        else LOG("\\x%02X", (unsigned char)c);
    }
    LOG("\"\n");
}

// NimBLE callbacks are C-style; single dongle, so plain statics are fine.
static NimBLEClient* s_client = nullptr;
static NimBLERemoteCharacteristic* s_writeChr = nullptr;
static volatile bool s_connected = false;
static std::string s_rx;
static SemaphoreHandle_t s_rxMutex;

static void notifyCb(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool)
{
    xSemaphoreTake(s_rxMutex, portMAX_DELAY);
    s_rx.append((const char*)data, len);
    xSemaphoreGive(s_rxMutex);
}

class ClientCb : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient*) override { s_connected = false; }
};
static ClientCb s_clientCb;

static Preferences s_prefs;

void Elm327::begin()
{
    s_rxMutex = xSemaphoreCreateMutex();
    NimBLEDevice::init("");
    s_prefs.begin("obd2");
    m_addr = s_prefs.getString("addr", "").c_str();
    m_addrType = s_prefs.getUChar("type", 0);
    LOG("[elm] saved dongle addr=\"%s\" type=%u\n", m_addr.c_str(), m_addrType);
}

void Elm327::saveDongle(const BleDev& d)
{
    m_addr = d.addr;
    m_addrType = d.addrType;
    s_prefs.putString("addr", d.addr.c_str());
    s_prefs.putUChar("type", d.addrType);
    disconnect();
}

void Elm327::disconnect()
{
    if (s_client && s_client->isConnected()) s_client->disconnect();
    s_connected = false;
    m_proto = false;
}

bool Elm327::isConnected() const
{
    return s_connected;
}

static bool looksLikeObd(NimBLEAdvertisedDevice& d)
{
    static const char* uuids[] = {"fff0", "ffe0", "18f0"};
    for (const char* u : uuids)
        if (d.isAdvertisingService(NimBLEUUID(u))) return true;
    std::string name = d.getName();
    for (char& c : name) c = tolower(c);
    static const char* names[] = {"obd", "elm", "vlink", "ios-", "v-link"};
    for (const char* n : names)
        if (name.find(n) != std::string::npos) return true;
    return false;
}

// Pick the first service exposing both a notify and a write characteristic.
static bool findUartChars(NimBLEClient* client, NimBLERemoteCharacteristic** outWrite)
{
    std::vector<NimBLERemoteService*>* svcs = client->getServices(true);
    LOG("[ble] %d services\n", (int)svcs->size());
    for (NimBLERemoteService* svc : *svcs) {
        std::string su = svc->getUUID().toString();
        if (su == "0x1800" || su == "0x1801" || su == "0x180a") {
            LOG("[ble] svc %s (skipped, generic)\n", su.c_str());
            continue;
        }
        LOG("[ble] svc %s\n", su.c_str());
        NimBLERemoteCharacteristic* notif = nullptr;
        NimBLERemoteCharacteristic* wr = nullptr;
        std::vector<NimBLERemoteCharacteristic*>* chrs = svc->getCharacteristics(true);
        for (NimBLERemoteCharacteristic* c : *chrs) {
            LOG("[ble]   chr %s%s%s%s%s\n", c->getUUID().toString().c_str(),
                c->canRead() ? " read" : "", c->canWrite() ? " write" : "",
                c->canWriteNoResponse() ? " writeNR" : "", c->canNotify() ? " notify" : "");
            if (!notif && c->canNotify()) notif = c;
            if (!wr && (c->canWrite() || c->canWriteNoResponse())) wr = c;
        }
        if (notif && wr) {
            // subscribe() reports success even when it never enabled anything:
            // with no CCCD it just sets the local callback and returns true. Check
            // the descriptor ourselves so a bad match falls through to the next
            // service instead of hanging on silent read timeouts.
            if (!notif->getDescriptor(NimBLEUUID((uint16_t)0x2902))) {
                LOG("[ble]   %s has no CCCD, skipping service\n",
                    notif->getUUID().toString().c_str());
                continue;
            }
            // response=true is required: NimBLE defaults to a CCCD write *without*
            // response, which the iCar Pro 2S silently drops -> no notifications
            // ever arrive and every command times out with zero bytes.
            bool sub = notif->subscribe(true, notifyCb, true);
            LOG("[ble]   -> pair notify=%s write=%s subscribe=%s\n",
                notif->getUUID().toString().c_str(), wr->getUUID().toString().c_str(),
                sub ? "OK" : "FAILED");
            if (!sub) continue;
            *outWrite = wr;
            return true;
        }
    }
    LOG("[ble] no service with notify+write found\n");
    return false;
}

std::vector<BleDev> Elm327::scanBle()
{
    std::vector<BleDev> out;
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    NimBLEScanResults results = scan->start(5, false);
    LOG("[scan] %d devices\n", results.getCount());
    for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice d = results.getDevice(i);
        LOG("[scan] %-18s type=%u rssi=%d obd=%d name=\"%s\" adv=%s\n",
            d.getAddress().toString().c_str(), d.getAddress().getType(), d.getRSSI(),
            looksLikeObd(d), d.haveName() ? d.getName().c_str() : "",
            d.haveServiceUUID() ? d.getServiceUUID().toString().c_str() : "-");
        std::string name = d.haveName() ? d.getName() : d.getAddress().toString();
        if (name.size() > 16) name.resize(16);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%s %ddB", looksLikeObd(d) ? "*" : "", name.c_str(),
                 d.getRSSI());
        BleDev dev;
        dev.label = buf;
        dev.addr = d.getAddress().toString();
        dev.addrType = d.getAddress().getType();
        out.push_back(dev);
    }
    return out;
}

bool Elm327::connect()
{
    m_proto = false;
    if (m_addr.empty()) {
        m_status = "No dongle chosen";
        return false;
    }

    m_status = "Connecting...";
    if (!s_client) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(&s_clientCb, false);
        s_client->setConnectTimeout(5); // default 30s freezes the UI far too long
    }
    LOG("[elm] connecting to %s type=%u\n", m_addr.c_str(), m_addrType);
    if (!s_client->connect(NimBLEAddress(m_addr, m_addrType))) {
        LOG("[elm] connect FAILED (not in range / wrong addr type)\n");
        m_status = "Dongle not in range";
        return false;
    }
    LOG("[elm] link up, mtu=%d\n", s_client->getMTU());
    if (!findUartChars(s_client, &s_writeChr)) {
        m_status = "No UART service";
        s_client->disconnect();
        return false;
    }
    s_connected = true;

    m_status = "Init dongle...";
    cmd("ATZ", 3000);
    cmd("ATE0");
    cmd("ATL0");
    cmd("ATS0");
    cmd("ATH0");
    cmd("ATSP0"); // auto protocol
    m_status = "Connected";
    ensureProto();
    return true;
}

bool Elm327::ensureProto()
{
    if (m_proto) return true;
    loadSupport();
    if (m_supp[0]) {
        m_proto = true;
        m_status = "Car online";
    } else {
        m_status = "Ignition off?";
    }
    return m_proto;
}

// PIDs 00/20/40 return a 32-bit "which of the next 32 PIDs do I answer" mask,
// and each mask's low bit says whether the following block exists.
void Elm327::loadSupport()
{
    static const char* req[3] = {"0100", "0120", "0140"};
    static const char* echo[3] = {"4100", "4120", "4140"};
    for (int i = 0; i < 3; i++) {
        m_supp[i] = 0;
        uint8_t b[4];
        std::string r = cmd(req[i], i ? 3000 : 10000); // first one may search protocols
        if (!pidBytes(r, echo[i], b, 4)) break;
        m_supp[i] = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        LOG("[elm] supported %s = %08X\n", req[i], (unsigned)m_supp[i]);
        if (!(m_supp[i] & 1)) break; // low bit = "next block supported"
    }
}

bool Elm327::supports(uint8_t pid) const
{
    if (pid == 0 || pid > 0x60) return false;
    int blk = (pid - 1) / 0x20;
    return m_supp[blk] & (1u << (31 - ((pid - 1) % 0x20)));
}

std::string Elm327::cmd(const std::string& c, uint32_t timeoutMs)
{
    if (!s_connected || !s_writeChr) {
        TRACE("[cmd] %s -> skipped, not connected\n", c.c_str());
        return "";
    }
    xSemaphoreTake(s_rxMutex, portMAX_DELAY);
    s_rx.clear();
    xSemaphoreGive(s_rxMutex);

    TRACE("[cmd] > %s\n", c.c_str());
    if (!s_writeChr->writeValue(c + "\r", false)) {
        LOG("[cmd] %s: write FAILED\n", c.c_str());
        return "";
    }

    uint32_t start = millis();
    std::string out;
    while (millis() - start < timeoutMs) {
        xSemaphoreTake(s_rxMutex, portMAX_DELAY);
        out = s_rx;
        xSemaphoreGive(s_rxMutex);
        if (out.find('>') != std::string::npos) {
#ifdef OBD2_TRACE
            logRaw("[cmd] <", out);
#endif
            return out;
        }
        if (!s_connected) break;
        delay(20);
    }
    LOG("[cmd] %s: TIMEOUT after %ums, partial:\n", c.c_str(), timeoutMs);
    logRaw("[cmd] <", out);
    return out; // whatever arrived before timeout/disconnect
}

static bool errResp(const std::string& r)
{
    return r.empty() || r.find("NO DATA") != std::string::npos ||
           r.find("UNABLE") != std::string::npos || r.find("ERROR") != std::string::npos;
}

bool Elm327::readBytes(const std::string& req, const std::string& echo, uint8_t* out, int n)
{
    if (!m_proto && !ensureProto()) return false;
    std::string r = cmd(req);
    if (errResp(r)) return false;
    return pidBytes(r, echo, out, n);
}

static int dRpm(const uint8_t* b) { return (b[0] * 256 + b[1]) / 4; }
static int dRaw(const uint8_t* b) { return b[0]; }
static int dU16(const uint8_t* b) { return b[0] * 256 + b[1]; }
static int dTemp(const uint8_t* b) { return b[0] - 40; }
static int dPct(const uint8_t* b) { return b[0] * 100 / 255; }
static int dTrim(const uint8_t* b) { return (b[0] - 128) * 100 / 128; } // signed, -100..+99%
static int dMaf(const uint8_t* b) { return (b[0] * 256 + b[1]) / 100; } // g/s
static int dTiming(const uint8_t* b) { return b[0] / 2 - 64; }         // deg before TDC
static int dMinutes(const uint8_t* b) { return (b[0] * 256 + b[1]) / 60; }

// Gauge order = rotation order. Unsupported PIDs are dropped at connect time,
// so a car that has no MAF or no fuel-level sender simply won't show those pages.
const PidDef kPids[] = {
    {0x0C, 2, "RPM", "rpm", dRpm, 0, 7000},
    {0x0D, 1, "SPEED", "km/h", dRaw, 0, 240},
    {0x05, 1, "COOLANT", "\xF7""C", dTemp, -40, 150},
    {0x0F, 1, "INTAKE", "\xF7""C", dTemp, -40, 90},
    {0x04, 1, "LOAD", "%", dPct, 0, 100},
    {0x11, 1, "THROTTLE", "%", dPct, 0, 100},
    {0x0B, 1, "MAP", "kPa", dRaw, 0, 255},
    {0x10, 2, "MAF", "g/s", dMaf, 0, 200},
    {0x06, 1, "SHORT TRIM", "%", dTrim, -25, 25},
    {0x07, 1, "LONG TRIM", "%", dTrim, -25, 25},
    {0x0E, 1, "TIMING", "\xF7", dTiming, -20, 60},
    {0x2F, 1, "FUEL", "%", dPct, 0, 100},
    {0x46, 1, "AMBIENT", "\xF7""C", dTemp, -40, 60},
    {0x1F, 2, "RUN TIME", "min", dMinutes, 0, 120},
    {0x21, 2, "MIL DIST", "km", dU16, 0, 1000},
};
const int kPidCount = sizeof(kPids) / sizeof(kPids[0]);

const PidDef* pidByNumber(uint8_t pid)
{
    for (int i = 0; i < kPidCount; i++)
        if (kPids[i].pid == pid) return &kPids[i];
    return nullptr;
}

int Elm327::readPid(const PidDef& p)
{
    char req[8], echo[8];
    snprintf(req, sizeof(req), "01%02X", p.pid);
    snprintf(echo, sizeof(echo), "41%02X", p.pid);
    uint8_t b[4];
    return readBytes(req, echo, b, p.nbytes) ? p.dec(b) : VAL_NONE;
}

float Elm327::battVolts()
{
    std::string r = cmd("ATRV");
    for (size_t i = 0; i < r.size(); i++) {
        if (isdigit((unsigned char)r[i])) {
            float v = atof(r.c_str() + i);
            return v > 5 ? v : NAN; // ignore stray digits from echo/garbage
        }
    }
    return NAN;
}

int Elm327::dtcCount(bool& milOn)
{
    uint8_t b[4];
    if (!readBytes("0101", "4101", b, 4)) return VAL_NONE;
    milOn = b[0] & 0x80;
    return b[0] & 0x7F;
}

std::vector<std::string> Elm327::readDtcs()
{
    if (!m_proto && !ensureProto()) return {};
    return parseDtcs(cmd("03", 8000));
}

std::vector<std::string> Elm327::readPending()
{
    if (!m_proto && !ensureProto()) return {};
    return parseDtcs(cmd("07", 8000), "47");
}

// Mode 02 replays the sensor values recorded when the fault was stored. Frame 0
// is the only one every ECU keeps. Response is "42 <pid> <frame> <data...>", so
// we read one byte more than the PID needs and skip the frame number.
std::vector<std::string> Elm327::freezeFrame()
{
    std::vector<std::string> lines;
    if (!m_proto && !ensureProto()) return lines;

    // PID 02 is the fault that froze the frame. No fault, no frame — bail out
    // here rather than timing out once per PID below.
    uint8_t b[5];
    char req[8], echo[8], line[32];
    if (!readBytes("020200", "4202", b, 3) || (!b[1] && !b[2])) return lines;
    lines.push_back("DTC " + dtcFromBytes(b[1], b[2]));

    for (int i = 0; i < kPidCount; i++) {
        const PidDef& p = kPids[i];
        if (!supports(p.pid)) continue;
        snprintf(req, sizeof(req), "02%02X00", p.pid);
        snprintf(echo, sizeof(echo), "42%02X", p.pid);
        if (!readBytes(req, echo, b, p.nbytes + 1)) continue;
        snprintf(line, sizeof(line), "%s %d%s", p.label, p.dec(b + 1), p.unit);
        lines.push_back(line);
    }
    return lines;
}

bool Elm327::clearDtcs()
{
    if (!m_proto) return false;
    std::string r = cmd("04", 8000);
    return r.find("44") != std::string::npos;
}
