#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Full-screen redraws into a canvas, pushed once per frame — no flicker.
class Ui {
public:
    void begin();
    void connecting(const char* msg);
    // frac 0..1 draws a progress arc around the rim; pass -1 for none.
    // sub is a small dim line under the unit (session peaks); nullptr for none.
    void gauge(const char* label, const char* value, const char* unit, float frac,
               uint16_t arcColor, const char* sub = nullptr);
    void dtcSummary(int count, bool milOn);
    void dtcCode(int idx, int total, const char* code, const char* desc);
    void confirmClear();
    void message(const char* line1, const char* line2);
    // Scrollable list with highlighted selection (5 visible items).
    void list(const char* title, const std::vector<std::string>& items, int sel);
};
