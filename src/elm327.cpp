#include "elm327.h"
#include "parse.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

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

void Elm327::begin()
{
    s_rxMutex = xSemaphoreCreateMutex();
    NimBLEDevice::init("");
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
    for (NimBLERemoteService* svc : *svcs) {
        std::string su = svc->getUUID().toString();
        if (su == "0x1800" || su == "0x1801" || su == "0x180a") continue;
        NimBLERemoteCharacteristic* notif = nullptr;
        NimBLERemoteCharacteristic* wr = nullptr;
        std::vector<NimBLERemoteCharacteristic*>* chrs = svc->getCharacteristics(true);
        for (NimBLERemoteCharacteristic* c : *chrs) {
            if (!notif && c->canNotify()) notif = c;
            if (!wr && (c->canWrite() || c->canWriteNoResponse())) wr = c;
        }
        if (notif && wr) {
            if (!notif->subscribe(true, notifyCb)) continue;
            *outWrite = wr;
            return true;
        }
    }
    return false;
}

std::vector<std::string> Elm327::scanBle()
{
    std::vector<std::string> out;
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    NimBLEScanResults results = scan->start(5, false);
    for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice d = results.getDevice(i);
        std::string name = d.haveName() ? d.getName() : d.getAddress().toString();
        if (name.size() > 16) name.resize(16);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%s %ddB", looksLikeObd(d) ? "*" : "", name.c_str(),
                 d.getRSSI());
        out.push_back(buf);
    }
    return out;
}

bool Elm327::connect()
{
    m_proto = false;
    m_status = "Scanning for OBD2...";

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    NimBLEScanResults results = scan->start(5, false);

    int found = -1;
    for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice d = results.getDevice(i);
        if (looksLikeObd(d)) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        m_status = "Dongle not found";
        return false;
    }

    m_status = "Connecting...";
    NimBLEAdvertisedDevice dev = results.getDevice(found);
    if (!s_client) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(&s_clientCb, false);
    }
    if (!s_client->connect(&dev)) {
        m_status = "Connect failed";
        return false;
    }
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
    if (!s_connected || !s_writeChr) return "";
    xSemaphoreTake(s_rxMutex, portMAX_DELAY);
    s_rx.clear();
    xSemaphoreGive(s_rxMutex);

    if (!s_writeChr->writeValue(c + "\r", false)) return "";

    uint32_t start = millis();
    std::string out;
    while (millis() - start < timeoutMs) {
        xSemaphoreTake(s_rxMutex, portMAX_DELAY);
        out = s_rx;
        xSemaphoreGive(s_rxMutex);
        if (out.find('>') != std::string::npos) return out;
        if (!s_connected) break;
        delay(20);
    }
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
