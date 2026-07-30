// Host check: g++ -std=c++11 -I../../src parse_check.cpp ../../src/parse.cpp && ./a.out
#include "parse.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    // PID extraction, spaced and unspaced, with ELM noise
    uint8_t b[2];
    assert(pidBytes("SEARCHING...\r41 0C 1A F8\r\r>", "410C", b, 2));
    assert(b[0] == 0x1A && b[1] == 0xF8);
    assert((b[0] * 256 + b[1]) / 4 == 1726);
    assert(pidBytes("410D3C>", "410D", b, 1) && b[0] == 60);
    assert(!pidBytes("NO DATA\r>", "410C", b, 2));
    assert(!pidBytes("410C1A\r>", "410C", b, 2)); // truncated

    // DTCs — CAN framing: 43 <count> then codes
    auto c = parseDtcs("43 02 03 01 01 71\r>");
    assert(c.size() == 2 && c[0] == "P0301" && c[1] == "P0171");
    // CAN, no codes
    assert(parseDtcs("4300\r>").empty());
    // K-line framing: 43 then 3 code slots, zero-padded
    c = parseDtcs("43 03 01 01 71 00 00\r>");
    assert(c.size() == 2 && c[0] == "P0301" && c[1] == "P0171");
    // Two ECUs answering, duplicate code deduped
    c = parseDtcs("430103 01\r43010301\r>");
    assert(c.size() == 1 && c[0] == "P0301");
    // Letter decoding: 0xD -> U1...
    c = parseDtcs("4301D016\r>");
    assert(c.size() == 1 && c[0] == "U1016");

    // Multi-frame CAN: 6 codes split across three "N:" lines, zero-padded.
    // Single-frame parsing dropped all of these.
    c = parseDtcs("014\r0: 43 06 01 71 01 72\r1: 03 01 03 02 03 03\r2: 03 04 00 00 00 00\r>");
    assert(c.size() == 6);
    assert(c[0] == "P0171" && c[1] == "P0172" && c[2] == "P0301");
    assert(c[3] == "P0302" && c[4] == "P0303" && c[5] == "P0304");
    // Two multi-frame replies back to back stay separate
    auto j = joinFrames(elmLines("0:4302\r1:0301\r0:4301\r1:0171\r>"));
    assert(j.size() == 2 && j[0] == "43020301" && j[1] == "43010171");
    // Single-frame replies are untouched
    j = joinFrames(elmLines("41 0C 1A F8\r>"));
    assert(j.size() == 1 && j[0] == "410C1AF8");

    // Mode 07 pending codes frame exactly like mode 03, different prefix
    c = parseDtcs("47 01 01 71\r>", "47");
    assert(c.size() == 1 && c[0] == "P0171");
    assert(parseDtcs("47 01 01 71\r>").empty()); // not mistaken for mode 03

    // Raw DTC byte pairs (mode 02 freeze-frame PID 02)
    assert(dtcFromBytes(0x03, 0x01) == "P0301");
    assert(dtcFromBytes(0xD0, 0x16) == "U1016");
    assert(dtcFromBytes(0x01, 0x71) == "P0171");

    // Descriptions
    assert(strcmp(dtcDescription("P0301"), "Cyl 1 misfire") == 0);
    assert(strcmp(dtcDescription("P1234"), "Manufacturer code") == 0);
    assert(strcmp(dtcDescription("U1016"), "Network/comm code") == 0);

    printf("parse_check: all OK\n");
    return 0;
}
