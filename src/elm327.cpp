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
    std::string r = cmd("0100", 10000); // protocol search can be slow
    if (r.find("4100") != std::string::npos || r.find("41 00") != std::string::npos) {
        m_proto = true;
        m_status = "Car online";
    } else {
        m_status = "Ignition off?";
    }
    return m_proto;
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

bool Elm327::readPid(const char* pid, const char* echo, uint8_t* out, int n)
{
    if (!m_proto && !ensureProto()) return false;
    std::string r = cmd(pid);
    if (errResp(r)) return false;
    return pidBytes(r, echo, out, n);
}

int Elm327::rpm()
{
    uint8_t b[2];
    return readPid("010C", "410C", b, 2) ? (b[0] * 256 + b[1]) / 4 : VAL_NONE;
}

int Elm327::speedKmh()
{
    uint8_t b[1];
    return readPid("010D", "410D", b, 1) ? b[0] : VAL_NONE;
}

int Elm327::coolantC()
{
    uint8_t b[1];
    return readPid("0105", "4105", b, 1) ? b[0] - 40 : VAL_NONE;
}

int Elm327::intakeC()
{
    uint8_t b[1];
    return readPid("010F", "410F", b, 1) ? b[0] - 40 : VAL_NONE;
}

int Elm327::loadPct()
{
    uint8_t b[1];
    return readPid("0104", "4104", b, 1) ? b[0] * 100 / 255 : VAL_NONE;
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
    if (!readPid("0101", "4101", b, 4)) return VAL_NONE;
    milOn = b[0] & 0x80;
    return b[0] & 0x7F;
}

std::vector<std::string> Elm327::readDtcs()
{
    if (!m_proto && !ensureProto()) return {};
    return parseDtcs(cmd("03", 8000));
}

bool Elm327::clearDtcs()
{
    if (!m_proto) return false;
    std::string r = cmd("04", 8000);
    return r.find("44") != std::string::npos;
}
