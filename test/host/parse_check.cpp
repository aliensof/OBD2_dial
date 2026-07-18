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

    // Descriptions
    assert(strcmp(dtcDescription("P0301"), "Cyl 1 misfire") == 0);
    assert(strcmp(dtcDescription("P1234"), "Manufacturer code") == 0);
    assert(strcmp(dtcDescription("U1016"), "Network/comm code") == 0);

    printf("parse_check: all OK\n");
    return 0;
}
