#include "ui.h"
#include <M5Dial.h>

static M5Canvas canvas(&M5Dial.Display);

void Ui::begin()
{
    canvas.setColorDepth(16);
    canvas.createSprite(240, 240);
    canvas.setTextDatum(middle_center);
}

static void push()
{
    canvas.pushSprite(0, 0);
}

static void clear()
{
    canvas.fillSprite(TFT_BLACK);
    canvas.setTextDatum(middle_center);
}

void Ui::connecting(const char* msg)
{
    clear();
    canvas.setFont(&fonts::Font4);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_CYAN);
    canvas.drawString("OBD2", 120, 80);
    canvas.setTextColor(TFT_WHITE);
    canvas.setFont(&fonts::Font2);
    canvas.drawString(msg, 120, 130);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString("tap = demo mode", 120, 170);
    canvas.drawArc(120, 120, 112, 116, 0, 360, TFT_DARKGREY);
    push();
}

void Ui::gauge(const char* label, const char* value, const char* unit, float frac,
               uint16_t arcColor, const char* sub)
{
    clear();
    canvas.drawArc(120, 120, 110, 118, 135, 45, TFT_DARKGREY);
    if (frac >= 0) {
        if (frac > 1) frac = 1;
        if (frac > 0.01f) canvas.fillArc(120, 120, 110, 118, 135, 135 + 270 * frac, arcColor);
    }
    canvas.setFont(&fonts::Font4);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_SILVER);
    canvas.drawString(label, 120, 52);
    canvas.drawString(unit, 120, 182);
    canvas.setFont(&fonts::Font7);
    canvas.setTextSize(strlen(value) > 4 ? 1.0f : 1.4f);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString(value, 120, 118);
    if (sub) {
        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextColor(TFT_DARKGREY);
        canvas.drawString(sub, 120, 207);
    }
    push();
}

void Ui::dtcSummary(int count, bool milOn)
{
    clear();
    canvas.drawArc(120, 120, 110, 118, 0, 360, count > 0 ? TFT_RED : TFT_DARKGREEN);
    canvas.setFont(&fonts::Font4);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_SILVER);
    canvas.drawString("FAULT CODES", 120, 52);
    canvas.setFont(&fonts::Font7);
    canvas.setTextSize(1.4f);
    canvas.setTextColor(count > 0 ? TFT_ORANGE : TFT_WHITE);
    char buf[8];
    snprintf(buf, sizeof(buf), count < 0 ? "--" : "%d", count);
    canvas.drawString(buf, 120, 118);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    if (milOn) {
        canvas.setTextColor(TFT_RED);
        canvas.drawString("CHECK ENGINE", 120, 170);
    }
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString("tap = menu", 120, 195);
    push();
}

void Ui::dtcCode(int idx, int total, const char* code, const char* desc)
{
    clear();
    canvas.drawArc(120, 120, 110, 118, 0, 360, TFT_ORANGE);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_SILVER);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", idx + 1, total);
    canvas.drawString(buf, 120, 50);
    canvas.setFont(&fonts::Font4);
    canvas.setTextSize(1.8f);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString(code, 120, 110);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_ORANGE);
    canvas.drawString(desc, 120, 155);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString("hold to clear all", 120, 190);
    push();
}

void Ui::confirmClear()
{
    clear();
    canvas.drawArc(120, 120, 110, 118, 0, 360, TFT_RED);
    canvas.setFont(&fonts::Font4);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_RED);
    canvas.drawString("CLEAR CODES?", 120, 95);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("tap = yes", 120, 140);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString("rotate = cancel", 120, 165);
    push();
}

void Ui::list(const char* title, const std::vector<std::string>& items, int sel)
{
    clear();
    canvas.drawArc(120, 120, 110, 118, 0, 360, TFT_DARKGREY);
    canvas.setFont(&fonts::Font4);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_CYAN);
    canvas.drawString(title, 120, 45);
    canvas.setFont(&fonts::Font2);
    if (items.empty()) {
        canvas.setTextColor(TFT_SILVER);
        canvas.drawString("nothing found", 120, 120);
        canvas.setTextColor(TFT_DARKGREY);
        canvas.drawString("tap = back", 120, 205);
        push();
        return;
    }
    int off = sel - 2;
    if (off > (int)items.size() - 5) off = (int)items.size() - 5;
    if (off < 0) off = 0;
    for (int i = 0; i < 5 && off + i < (int)items.size(); i++) {
        bool isSel = (off + i == sel);
        if (isSel) canvas.fillRoundRect(20, 78 + i * 24 - 11, 200, 22, 6, TFT_NAVY);
        canvas.setTextColor(isSel ? TFT_ORANGE : TFT_WHITE);
        canvas.drawString(items[off + i].c_str(), 120, 78 + i * 24);
    }
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString("tap = choose   hold = back", 120, 205);
    push();
}

void Ui::message(const char* line1, const char* line2)
{
    clear();
    canvas.setFont(&fonts::Font4);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString(line1, 120, 100);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(TFT_SILVER);
    canvas.drawString(line2, 120, 145);
    push();
}
