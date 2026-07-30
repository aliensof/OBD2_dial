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

enum Mode { GAUGES, CODE_MENU, DTC_LIST, DTC_CONFIRM, SCAN_LIST, INFO_LIST };

static const std::vector<std::string> kCodeMenu = {"Fault codes", "Pending codes",
                                                   "Freeze frame", "Clear faults",
                                                   "Service reset"};
static int menuSel = 0;

// Session peaks, one slot per kPids row. VAL_NONE = nothing seen yet.
static std::vector<int> pkMin, pkMax;

static void resetPeaks()
{
    pkMin.assign(kPidCount, VAL_NONE);
    pkMax.assign(kPidCount, VAL_NONE);
}

// Values worth shouting about even when you're looking at another page. These
// are polled in the background, so keep the list short — each row costs one
// command every WATCH_MS.
// ponytail: thresholds are guesses for a warm VW-group engine; tune on the car.
struct Watch {
    uint8_t pid; // 0 = battery voltage via ATRV
    int lo, hi;  // alarm outside this band
    const char* msg;
};
static const Watch kWatch[] = {
    {0x05, INT_MIN, 110, "COOLANT HOT"},  // fans run ~105, so 110 means trouble
    {0x00, 115, INT_MAX, "VOLTAGE LOW"},  // tenths of a volt; alternator/belt
};
#define WATCH_MS 5000
#define ALARM_REPEAT_MS 60000
static uint32_t lastWatch = 0;
static uint32_t lastAlarm = 0;

// Gauge pages come from kPids, filtered to what this car answers; the battery,
// codes and scan pages always follow them.
static std::vector<int> gauges;
static int battPage() { return (int)gauges.size(); }
static int dtcPage() { return battPage() + 1; }
static int scanPage() { return battPage() + 2; }
static int pageCount() { return battPage() + 3; }

static void buildPages()
{
    gauges.clear();
    for (int i = 0; i < kPidCount; i++)
        // Before the car answers (demo mode, ignition off) show everything.
        if (!elm.protoReady() || elm.supports(kPids[i].pid)) gauges.push_back(i);
    Serial.printf("[ui] %d gauge pages\n", (int)gauges.size());
}

static Mode mode = GAUGES;
static int page = 0;
static long encAnchor = 0;
static uint32_t lastPoll = 0;
static std::vector<std::string> dtcs;
static int dtcIdx = 0;
static uint32_t confirmSince = 0;
static bool demoMode = false; // browse the UI without a dongle
static std::vector<std::string> infoLines; // freeze-frame text, scrolled in INFO_LIST
static int infoIdx = 0;
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

    if (page < battPage()) {
        int gi = gauges[page];
        const PidDef& p = kPids[gi];
        int v = elm.readPid(p);
        fmtVal(buf, sizeof(buf), v);
        char sub[24] = "";
        if (v != VAL_NONE) {
            if (pkMax[gi] == VAL_NONE || v > pkMax[gi]) pkMax[gi] = v;
            if (pkMin[gi] == VAL_NONE || v < pkMin[gi]) pkMin[gi] = v;
            frac = (float)(v - p.lo) / (p.hi - p.lo);
            if (frac > 0.85f) color = TFT_RED;
            // cool-looking things stay cool-coloured; cold engine reads blue
            if (p.pid == 0x0D || p.pid == 0x0F || p.pid == 0x46) color = TFT_CYAN;
            if (p.pid == 0x05 && v < 70) color = TFT_CYAN;
            // signed gauges (trims, timing, ambient) need both ends
            if (p.lo < 0) snprintf(sub, sizeof(sub), "%d / %d", pkMin[gi], pkMax[gi]);
            else snprintf(sub, sizeof(sub), "max %d", pkMax[gi]);
        }
        ui.gauge(p.label, buf, p.unit, frac, color, sub[0] ? sub : nullptr);
        return;
    }
    if (page == battPage()) {
        float bv = elm.battVolts();
        if (isnan(bv)) {
            snprintf(buf, sizeof(buf), "--");
        } else {
            snprintf(buf, sizeof(buf), "%.1f", bv);
            frac = (bv - 10.0f) / 5.0f;
            color = bv < 11.8f ? TFT_RED : TFT_GREEN;
        }
        ui.gauge("BATTERY", buf, "V", frac, color);
    } else if (page == dtcPage()) {
        bool mil = false;
        int n = elm.dtcCount(mil);
        ui.dtcSummary(n == VAL_NONE ? -1 : n, mil);
    } else {
        ui.message("BLE SCAN", elm.hasSaved() ? "tap to scan" : "tap to scan & pick dongle");
    }
}

// Poll the watched values regardless of which page is showing, so an overheat
// still gets your attention while you're staring at the trim gauge.
static void checkAlarms()
{
    if (!elm.protoReady() || millis() - lastWatch < WATCH_MS) return;
    lastWatch = millis();
    for (const Watch& w : kWatch) {
        int v;
        char detail[24];
        if (w.pid == 0) {
            float bv = elm.battVolts();
            if (isnan(bv)) continue;
            v = (int)(bv * 10);
            snprintf(detail, sizeof(detail), "%.1f V", bv);
        } else {
            const PidDef* p = pidByNumber(w.pid);
            if (!p || !elm.supports(w.pid)) continue;
            v = elm.readPid(*p);
            if (v == VAL_NONE) continue;
            snprintf(detail, sizeof(detail), "%d %s", v, p->unit);
        }
        if (v >= w.lo && v <= w.hi) continue;
        if (millis() - lastAlarm < ALARM_REPEAT_MS) continue;
        lastAlarm = millis();
        Serial.printf("[alarm] %s: %s\n", w.msg, detail);
        for (int i = 0; i < 3; i++) {
            beep();
            delay(150);
        }
        ui.message(w.msg, detail);
        delay(2000);
        lastPoll = 0; // force the gauge back on screen
        return;
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
    Serial.begin(115200);
    delay(300); // let USB CDC enumerate so the boot lines aren't lost
    Serial.println("\n[boot] obd2-dial");
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

    resetPeaks();
    buildPages();
    if (!elm.hasSaved()) page = scanPage(); // first run: pick a dongle
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
            buildPages(); // the car just told us which PIDs it answers
            resetPeaks(); // new session, new peaks
            if (page >= pageCount()) page = 0;
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
                page = ((page + d) % pageCount() + pageCount()) % pageCount();
                lastPoll = 0;
                beep();
            }
            if (page == scanPage() && tap) {
                ui.message("Scanning...", "5 sec");
                scanResults = elm.scanBle();
                scanLabels.clear();
                for (const BleDev& dev : scanResults) scanLabels.push_back(dev.label);
                scanSel = 0;
                mode = SCAN_LIST;
                ui.list("BLE DEVICES", scanLabels, scanSel);
                break;
            }
            if (page == dtcPage() && tap) {
                menuSel = 0;
                mode = CODE_MENU;
                ui.list("CODES", kCodeMenu, menuSel);
            }
            if (hold && page < battPage()) {
                resetPeaks();
                beep();
                ui.message("Peaks reset", "");
                delay(800);
                lastPoll = 0;
                break;
            }
            if (lastPoll == 0 || millis() - lastPoll >= POLL_MS) {
                drawGauge();
                lastPoll = millis();
            }
            checkAlarms();
            break;

        case CODE_MENU:
            if (hold) {
                mode = GAUGES;
                lastPoll = 0;
                break;
            }
            if (tap) {
                if (menuSel == 0 || menuSel == 1) {
                    ui.message("Reading...", "");
                    dtcs = menuSel == 0 ? elm.readDtcs() : elm.readPending();
                    dtcIdx = 0;
                    mode = DTC_LIST;
                    showDtcList();
                    lastPoll = millis();
                } else if (menuSel == 2) {
                    ui.message("Reading...", "freeze frame");
                    infoLines = elm.freezeFrame();
                    infoIdx = 0;
                    if (infoLines.empty()) {
                        ui.message("No freeze frame", "no stored fault");
                        delay(2000);
                        ui.list("CODES", kCodeMenu, menuSel);
                    } else {
                        mode = INFO_LIST;
                        ui.list("FREEZE FRAME", infoLines, infoIdx);
                    }
                } else if (menuSel == 3) {
                    mode = DTC_CONFIRM;
                    confirmSince = millis();
                    ui.confirmClear();
                } else {
                    ui.message("Service reset", "VW-only - not yet");
                    delay(2000);
                    ui.list("CODES", kCodeMenu, menuSel);
                }
                break;
            }
            if (d) {
                menuSel += d;
                if (menuSel >= (int)kCodeMenu.size()) menuSel = kCodeMenu.size() - 1;
                if (menuSel < 0) menuSel = 0;
                ui.list("CODES", kCodeMenu, menuSel);
            }
            break;

        case DTC_LIST:
            if (tap || (dtcs.empty() && millis() - lastPoll > 2000)) {
                mode = CODE_MENU;
                ui.list("CODES", kCodeMenu, menuSel);
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
                page = dtcPage();
                lastPoll = 0;
            } else if (d || millis() - confirmSince > 8000) {
                mode = CODE_MENU;
                ui.list("CODES", kCodeMenu, menuSel);
            }
            break;

        case INFO_LIST:
            if (tap || hold) {
                mode = CODE_MENU;
                ui.list("CODES", kCodeMenu, menuSel);
                break;
            }
            if (d) {
                infoIdx += d;
                if (infoIdx >= (int)infoLines.size()) infoIdx = infoLines.size() - 1;
                if (infoIdx < 0) infoIdx = 0;
                ui.list("FREEZE FRAME", infoLines, infoIdx);
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
                page = 0;
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
