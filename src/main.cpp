#include <M5Dial.h>
#include <string>
#include <vector>
#include "elm327.h"
#include "parse.h"
#include "ui.h"

// ponytail: tune if paging feels off — encoder counts per physical detent.
#define ENC_PER_DETENT 4
#define POLL_MS 300

// M5Dial lib's Encoder never gets interrupts (its ESP32 pin table stops at
// GPIO39; the dial is on 40/41), so we count quadrature edges ourselves.
static volatile long s_encCount = 0;
static volatile uint8_t s_encState = 0;

static void IRAM_ATTR encIsr()
{
    // index = old state << 2 | new state; ponytail: flip signs if direction feels reversed
    static const int8_t tbl[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    uint8_t s = ((uint8_t)digitalRead(DIAL_ENCODER_PIN_A) << 1) |
                (uint8_t)digitalRead(DIAL_ENCODER_PIN_B);
    s_encCount += tbl[(s_encState << 2) | s];
    s_encState = s;
}

static Elm327 elm;
static Ui ui;

enum Page { P_RPM, P_SPEED, P_COOLANT, P_BATT, P_INTAKE, P_LOAD, P_DTC, P_SCAN, PAGE_COUNT };
enum Mode { GAUGES, DTC_LIST, DTC_CONFIRM, SCAN_LIST };

static Mode mode = GAUGES;
static int page = P_RPM;
static long encAnchor = 0;
static uint32_t lastPoll = 0;
static std::vector<std::string> dtcs;
static int dtcIdx = 0;
static uint32_t confirmSince = 0;
static bool demoMode = false; // browse the UI without a dongle
static std::vector<BleDev> scanResults;
static std::vector<std::string> scanLabels;
static int scanSel = 0;

static void beep()
{
    M5Dial.Speaker.tone(4000, 60);
}

// Whole detents since last call; keeps partial counts.
static int encDelta()
{
    long p = s_encCount;
    int d = (int)((p - encAnchor) / ENC_PER_DETENT);
    if (d) encAnchor += (long)d * ENC_PER_DETENT;
    return d;
}

static bool tapped()
{
    return M5Dial.Touch.getDetail().wasClicked() || M5Dial.BtnA.wasClicked();
}

static bool held()
{
    return M5Dial.Touch.getDetail().wasHold() || M5Dial.BtnA.wasHold();
}

static void fmtVal(char* buf, size_t n, int v)
{
    if (v == VAL_NONE) snprintf(buf, n, "--");
    else snprintf(buf, n, "%d", v);
}

static void drawGauge()
{
    char buf[16];
    float frac = -1;
    uint16_t color = TFT_GREEN;
    int v;
    switch (page) {
        case P_RPM:
            v = elm.rpm();
            fmtVal(buf, sizeof(buf), v);
            if (v != VAL_NONE) {
                frac = v / 7000.0f;
                if (frac > 0.85f) color = TFT_RED;
            }
            ui.gauge("RPM", buf, "x1000: 7 max", frac, color);
            break;
        case P_SPEED:
            v = elm.speedKmh();
            fmtVal(buf, sizeof(buf), v);
            if (v != VAL_NONE) frac = v / 240.0f;
            ui.gauge("SPEED", buf, "km/h", frac, TFT_CYAN);
            break;
        case P_COOLANT:
            v = elm.coolantC();
            fmtVal(buf, sizeof(buf), v);
            if (v != VAL_NONE) {
                frac = (v + 40) / 190.0f;
                color = v > 105 ? TFT_RED : (v < 70 ? TFT_CYAN : TFT_GREEN);
            }
            ui.gauge("COOLANT", buf, "\xF7""C", frac, color);
            break;
        case P_BATT: {
            float bv = elm.battVolts();
            if (isnan(bv)) snprintf(buf, sizeof(buf), "--");
            else snprintf(buf, sizeof(buf), "%.1f", bv);
            if (!isnan(bv)) {
                frac = (bv - 10.0f) / 5.0f;
                color = bv < 11.8f ? TFT_RED : TFT_GREEN;
            }
            ui.gauge("BATTERY", buf, "V", frac, color);
            break;
        }
        case P_INTAKE:
            v = elm.intakeC();
            fmtVal(buf, sizeof(buf), v);
            if (v != VAL_NONE) frac = (v + 40) / 130.0f;
            ui.gauge("INTAKE", buf, "\xF7""C", frac, TFT_CYAN);
            break;
        case P_LOAD:
            v = elm.loadPct();
            fmtVal(buf, sizeof(buf), v);
            if (v != VAL_NONE) frac = v / 100.0f;
            ui.gauge("LOAD", buf, "%", frac, TFT_GREEN);
            break;
        case P_DTC: {
            bool mil = false;
            int n = elm.dtcCount(mil);
            ui.dtcSummary(n == VAL_NONE ? -1 : n, mil);
            break;
        }
        case P_SCAN:
            ui.message("BLE SCAN", elm.hasSaved() ? "tap to scan" : "tap to scan & pick dongle");
            break;
    }
}

static void showDtcList()
{
    if (dtcs.empty()) {
        ui.message("No codes", "all clear");
        return;
    }
    if (dtcIdx >= (int)dtcs.size()) dtcIdx = dtcs.size() - 1;
    if (dtcIdx < 0) dtcIdx = 0;
    const std::string& c = dtcs[dtcIdx];
    ui.dtcCode(dtcIdx, dtcs.size(), c.c_str(), dtcDescription(c));
}

void setup()
{
    auto cfg = M5.config();
    M5Dial.begin(cfg, false, false); // our own encoder ISR below, RFID off
    ui.begin();
    ui.connecting("Starting...");
    elm.begin();

    pinMode(DIAL_ENCODER_PIN_A, INPUT_PULLUP);
    pinMode(DIAL_ENCODER_PIN_B, INPUT_PULLUP);
    s_encState = ((uint8_t)digitalRead(DIAL_ENCODER_PIN_A) << 1) |
                 (uint8_t)digitalRead(DIAL_ENCODER_PIN_B);
    attachInterrupt(digitalPinToInterrupt(DIAL_ENCODER_PIN_A), encIsr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(DIAL_ENCODER_PIN_B), encIsr, CHANGE);
    encAnchor = s_encCount;

    if (!elm.hasSaved()) page = P_SCAN; // first run: pick a dongle
}

void loop()
{
    M5Dial.update();

    if (!elm.isConnected() && !demoMode && elm.hasSaved()) {
        mode = GAUGES;
        ui.connecting(elm.status());
        // 2s tap window before each (blocking) connect attempt
        uint32_t t0 = millis();
        while (millis() - t0 < 2000) {
            M5Dial.update();
            if (tapped()) {
                demoMode = true;
                beep();
                lastPoll = 0;
                return;
            }
            delay(20);
        }
        ui.connecting("Connecting...");
        if (elm.connect()) {
            beep();
            lastPoll = 0;
        }
        return;
    }

    int d = encDelta();
    bool tap = tapped();
    bool hold = held();

    switch (mode) {
        case GAUGES:
            if (d) {
                page = ((page + d) % PAGE_COUNT + PAGE_COUNT) % PAGE_COUNT;
                lastPoll = 0;
                beep();
            }
            if (page == P_SCAN && tap) {
                ui.message("Scanning...", "5 sec");
                scanResults = elm.scanBle();
                scanLabels.clear();
                for (const BleDev& dev : scanResults) scanLabels.push_back(dev.label);
                scanSel = 0;
                mode = SCAN_LIST;
                ui.list("BLE DEVICES", scanLabels, scanSel);
                break;
            }
            if (page == P_DTC && tap) {
                ui.message("Reading...", "");
                dtcs = elm.readDtcs();
                dtcIdx = 0;
                mode = DTC_LIST;
                showDtcList();
                lastPoll = millis();
            }
            if (lastPoll == 0 || millis() - lastPoll >= POLL_MS) {
                drawGauge();
                lastPoll = millis();
            }
            break;

        case DTC_LIST:
            if (tap || (dtcs.empty() && millis() - lastPoll > 2000)) {
                mode = GAUGES;
                lastPoll = 0;
                break;
            }
            if (hold && !dtcs.empty()) {
                mode = DTC_CONFIRM;
                confirmSince = millis();
                ui.confirmClear();
                break;
            }
            if (d) {
                dtcIdx += d;
                showDtcList();
            }
            break;

        case DTC_CONFIRM:
            if (tap) {
                bool ok = elm.clearDtcs();
                beep();
                ui.message(ok ? "Codes cleared" : "Clear failed", ok ? "" : "try again");
                delay(1500);
                dtcs.clear();
                mode = GAUGES;
                page = P_DTC;
                lastPoll = 0;
            } else if (d || millis() - confirmSince > 8000) {
                mode = DTC_LIST;
                showDtcList();
            }
            break;

        case SCAN_LIST:
            if (hold || (scanResults.empty() && tap)) {
                mode = GAUGES;
                lastPoll = 0;
                break;
            }
            if (tap) {
                elm.saveDongle(scanResults[scanSel]);
                beep();
                ui.message("Dongle saved", "connecting...");
                delay(1000);
                demoMode = false;
                mode = GAUGES;
                page = P_RPM;
                lastPoll = 0;
                break;
            }
            if (d) {
                scanSel += d;
                if (scanSel >= (int)scanResults.size()) scanSel = scanResults.size() - 1;
                if (scanSel < 0) scanSel = 0;
                ui.list("BLE DEVICES", scanLabels, scanSel);
            }
            break;
    }
    delay(10);
}
