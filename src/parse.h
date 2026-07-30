#pragma once
// Pure ELM327 response parsing — no Arduino deps, host-testable.
#include <string>
#include <vector>
#include <cstdint>

// Split raw ELM output into lines, each stripped of spaces, CR/LF and '>'.
std::vector<std::string> elmLines(const std::string& raw);

// Find a line containing `echo` (e.g. "410C") and read `n` data bytes after it.
bool pidBytes(const std::string& raw, const std::string& echo, uint8_t* out, int n);

// Splice ELM327 multi-frame lines ("0:", "1:", ...) back into single lines.
std::vector<std::string> joinFrames(const std::vector<std::string>& lines);

// Decode a mode-03 response into codes like "P0301". Handles CAN framing
// (count byte after 43) and K-line framing (codes directly after 43).
// Pass mode "47" for pending codes (mode 07), which frame identically.
std::vector<std::string> parseDtcs(const std::string& raw, const char* mode = "43");

// Decode two raw DTC bytes (0x03,0x01) into "P0301".
std::string dtcFromBytes(uint8_t hi, uint8_t lo);

// Short human description for a DTC, generic fallback by prefix.
const char* dtcDescription(const std::string& code);
