#include <M5Dial.h>
#include <string>
#include <vector>
#include "elm327.h"
#include "parse.h"
#include "ui.h"

// ponytail: tune if paging feels off — encoder counts per physical detent.
#define ENC_PER_DETENT 4
#define POLL_MS 300

static Elm327 elm;
static Ui ui;

enum Page { P_RPM, P_SPEED, P_COOLANT, P_BATT, P_INTAKE, P_LOAD, P_DTC, PAGE_COUNT };
enum Mode { GAUGES, DTC_LIST, DTC_CONFIRM };

static Mode mode = GAUGES;
static int page = P_RPM;
static long encAnchor = 0;
static uint32_t lastPoll = 0;
static std::vector<std::string> dtcs;
static int dtcIdx = 0;
static uint32_t confirmSince = 0;

static void beep()
{
    M5Dial.Speaker.tone(4000, 60);
}

// Whole detents since last call; keeps partial counts.
static int encDelta()
{
    long p = M5Dial.Encoder.read();
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
    M5Dial.begin(cfg, true, false); // encoder on, RFID off
    ui.begin();
    ui.connecting("Starting...");
    elm.begin();
    encAnchor = M5Dial.Encoder.read();
}

void loop()
{
    M5Dial.update();

    if (!elm.isConnected()) {
        mode = GAUGES;
        ui.connecting(elm.status());
        if (elm.connect()) {
            beep();
            lastPoll = 0;
        } else {
            ui.connecting(elm.status());
            delay(2000);
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
    }
    delay(10);
}
