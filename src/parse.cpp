#include "parse.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::vector<std::string> elmLines(const std::string& raw)
{
    std::vector<std::string> out;
    std::string line;
    for (char c : raw) {
        if (c == '\r' || c == '\n') {
            if (!line.empty()) out.push_back(line);
            line.clear();
        } else if (c != ' ' && c != '>') {
            line += (char)toupper((unsigned char)c);
        }
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

static bool isHexStr(const std::string& s)
{
    for (char c : s)
        if (hexVal(c) < 0) return false;
    return !s.empty();
}

bool pidBytes(const std::string& raw, const std::string& echo, uint8_t* out, int n)
{
    for (const std::string& line : elmLines(raw)) {
        size_t pos = line.find(echo);
        if (pos == std::string::npos) continue;
        size_t start = pos + echo.size();
        if (line.size() < start + (size_t)n * 2) continue;
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            int hi = hexVal(line[start + i * 2]);
            int lo = hexVal(line[start + i * 2 + 1]);
            if (hi < 0 || lo < 0) ok = false;
            else out[i] = (uint8_t)(hi * 16 + lo);
        }
        if (ok) return true;
    }
    return false;
}

std::string dtcFromBytes(uint8_t hi, uint8_t lo)
{
    static const char letters[] = "PCBU";
    char buf[6];
    snprintf(buf, sizeof(buf), "%c%d%X%02X", letters[hi >> 6], (hi >> 4) & 3, hi & 0x0F, lo);
    return buf;
}

static std::string decodeDtc(const std::string& four)
{
    static const char letters[] = "PCBU";
    int d = hexVal(four[0]);
    std::string code;
    code += letters[d >> 2];
    code += (char)('0' + (d & 3));
    code += four.substr(1);
    return code;
}

std::vector<std::string> parseDtcs(const std::string& raw, const char* mode)
{
    std::vector<std::string> codes;
    for (const std::string& line : elmLines(raw)) {
        if (line.size() < 6 || line.compare(0, 2, mode) != 0) continue;
        std::string rest = line.substr(2);
        if (!isHexStr(rest)) continue;
        // CAN: "43" + count byte + N*2 bytes -> rest length % 4 == 2
        if (rest.size() % 4 == 2) rest = rest.substr(2);
        for (size_t i = 0; i + 4 <= rest.size(); i += 4) {
            std::string four = rest.substr(i, 4);
            if (four == "0000") continue;
            std::string code = decodeDtc(four);
            if (std::find(codes.begin(), codes.end(), code) == codes.end())
                codes.push_back(code);
        }
    }
    return codes;
}

struct DtcDesc {
    const char* code;
    const char* desc;
};

// ponytail: small table of common codes; everything else gets a generic label.
static const DtcDesc kDescs[] = {
    {"P0011", "Intake cam timing"},
    {"P0016", "Crank/cam correlation"},
    {"P0087", "Fuel rail pressure low"},
    {"P0101", "MAF sensor range"},
    {"P0106", "MAP sensor range"},
    {"P0113", "Intake air temp high"},
    {"P0118", "Coolant sensor high"},
    {"P0128", "Thermostat / warm-up"},
    {"P0171", "System too lean"},
    {"P0234", "Turbo overboost"},
    {"P0299", "Turbo underboost"},
    {"P0300", "Random misfire"},
    {"P0301", "Cyl 1 misfire"},
    {"P0302", "Cyl 2 misfire"},
    {"P0303", "Cyl 3 misfire"},
    {"P0304", "Cyl 4 misfire"},
    {"P0325", "Knock sensor"},
    {"P0341", "Cam position sensor"},
    {"P0401", "EGR flow insufficient"},
    {"P0420", "Catalyst efficiency"},
    {"P0441", "EVAP purge flow"},
    {"P0455", "EVAP leak (large)"},
    {"P0501", "Vehicle speed sensor"},
    {"P0562", "System voltage low"},
    {"P0605", "ECU internal error"},
};

const char* dtcDescription(const std::string& code)
{
    for (const DtcDesc& d : kDescs)
        if (code == d.code) return d.desc;
    if (code.size() < 2) return "";
    if (code[0] == 'P' && (code[1] == '1' || code[1] == '3')) return "Manufacturer code";
    switch (code[0]) {
        case 'U': return "Network/comm code";
        case 'C': return "Chassis code";
        case 'B': return "Body code";
    }
    return "Powertrain code";
}
