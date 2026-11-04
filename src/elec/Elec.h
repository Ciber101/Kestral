#pragma once
// The parts catalog and the wiring checks. Pure data and math: no UI, no kernel.
#include "core/Electronics.h"

#include <string>
#include <vector>

namespace kst::elec {

// Every part Kestral knows, from datasheets (sources on each part).
const std::vector<PartDef>& catalog();
const PartDef* findDef(const std::string& id);
// Parts whose name, id, maker or category contain every word of the query.
std::vector<const PartDef*> search(const std::string& query);

enum class Severity { Error, Warning, Info };
struct Issue {
    Severity sev = Severity::Info;
    std::string text;            // plain words: what is wrong and what to do
    std::vector<uint32_t> wires; // wires it is about
    std::vector<uint32_t> parts;
};

// A connected group of pins: everything at one voltage.
struct NetPin { uint32_t part = 0; std::string pin; };
struct Net {
    std::vector<NetPin> pins;
    double volts = 0;            // the supply on it (0 = a signal or nothing)
    double voltsMin = 0;         // battery nets: the voltage when the pack is empty
    bool ground = false;
};

struct PowerLine {
    uint32_t part = 0;           // the source part
    std::string pin;             // its output pin ("5V", "P0.+", battery "+")
    double volts = 0;
    double typA = 0, peakA = 0;  // what the loads on it take
    double maxA = 0;             // what it can give
};

struct Report {
    std::vector<Net> nets;
    std::vector<Issue> issues;
    std::vector<PowerLine> power;
    double batteryTypA = 0, batteryPeakA = 0, runtimeH = 0;   // the pack (first battery found)
    double massG = 0;
    int errors = 0, warnings = 0;
};

Report check(const ElecDoc& d);

// Step by step wiring guide for the real build (markdown-ish plain text).
std::string guide(const ElecDoc& d, const Report& r);
// Short text of the check for tools: counts, then every issue.
std::string summary(const ElecDoc& d, const Report& r);

const char* kindName(PinKind k);
std::string partLabel(const ElecDoc& d, uint32_t id);

} // namespace kst::elec
