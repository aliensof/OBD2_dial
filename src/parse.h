#pragma once
// Pure ELM327 response parsing — no Arduino deps, host-testable.
#include <string>
#include <vector>
#include <cstdint>

// Split raw ELM output into lines, each stripped of spaces, CR/LF and '>'.
std::vector<std::string> elmLines(const std::string& raw);

// Find a line containing `echo` (e.g. "410C") and read `n` data bytes after it.
bool pidBytes(const std::string& raw, const std::string& echo, uint8_t* out, int n);

// Decode a mode-03 response into codes like "P0301". Handles CAN framing
// (count byte after 43) and K-line framing (codes directly after 43).
// ponytail: single-frame per ECU only; multi-frame (>2 codes on CAN) is phase 2.
std::vector<std::string> parseDtcs(const std::string& raw);

// Short human description for a DTC, generic fallback by prefix.
const char* dtcDescription(const std::string& code);
