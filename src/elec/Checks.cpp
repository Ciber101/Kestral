// Wiring checks: nets from the wires, the voltage on every rail, logic levels, current budgets,
// motor channels, I2C addresses, shorts and the pins that must be tied. Then the wiring guide.
#include "elec/Elec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace kst::elec {

namespace {

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}
std::string volts(double v) { return fmt("%.1f V", v); }
std::string amps(double a) { return a < 0.1 ? fmt("%.0f mA", a * 1000) : fmt("%.2f A", a); }

bool isOutput(PinKind k) {
    return k == PinKind::DigitalOut || k == PinKind::Pwm || k == PinKind::UartTx || k == PinKind::SpiMosi || k == PinKind::SpiSck ||
           k == PinKind::SpiCs;
}
bool isSignal(PinKind k) {
    return k == PinKind::DigitalIO || k == PinKind::DigitalIn || k == PinKind::DigitalOut || k == PinKind::Analog || k == PinKind::Pwm ||
           k == PinKind::I2cSda || k == PinKind::I2cScl || k == PinKind::UartTx || k == PinKind::UartRx || k == PinKind::SpiMosi ||
           k == PinKind::SpiMiso || k == PinKind::SpiSck || k == PinKind::SpiCs;
}
// Safe hobby ratings for short chassis runs, amps.
double awgAmps(int awg) {
    switch (awg) {
    case 30: return 0.5;
    case 28: return 0.8;
    case 26: return 1.3;
    case 24: return 2.1;
    case 22: return 3.0;
    case 20: return 5.0;
    case 18: return 7.0;
    case 16: return 10.0;
    case 14: return 15.0;
    case 12: return 20.0;
    case 10: return 30.0;
    default: return awg > 30 ? 0.3 : 40.0;
    }
}

struct Ctx {
    const ElecDoc& d;
    Report& r;
    std::map<std::string, std::string> parent;   // union find over "part:pin"
    std::map<std::string, int> netOf;
    std::map<std::string, double> drawTyp, drawPeak;   // per "part:pin" of PowerIn, filled lazily
    std::set<std::string> busy;

    explicit Ctx(const ElecDoc& dd, Report& rr) : d(dd), r(rr) {}

    static std::string key(uint32_t p, const std::string& pin) { return std::to_string(p) + ":" + pin; }
    std::string find(const std::string& k) {
        auto it = parent.find(k);
        if (it == parent.end()) { parent[k] = k; return k; }
        if (it->second == k) return k;
        std::string root = find(it->second);
        parent[k] = root;
        return root;
    }
    void join(const std::string& a, const std::string& b) {
        std::string ra = find(a), rb = find(b);
        if (ra != rb) parent[ra] = rb;
    }
    const PartDef* defOf(uint32_t id) const {
        const ElecPart* p = d.part(id);
        return p ? findDef(p->def) : nullptr;
    }
    std::string name(uint32_t id) const { return partLabel(d, id); }
    std::string pinName(uint32_t id, const std::string& pin) const {
        const PartDef* df = defOf(id);
        const PinDef* p = df ? df->pin(pin) : nullptr;
        return name(id) + " " + (p ? p->label : pin);
    }
    void issue(Severity s, const std::string& t, std::vector<uint32_t> wires = {}, std::vector<uint32_t> parts = {}) {
        for (auto& i : r.issues)
            if (i.text == t) return;
        r.issues.push_back({s, t, std::move(wires), std::move(parts)});
    }
    const Net* net(uint32_t part, const std::string& pin) {
        auto it = netOf.find(find(key(part, pin)));
        return it == netOf.end() ? nullptr : &r.nets[(size_t)it->second];
    }
    // The supply on a part's power input (the highest when there are several).
    double supplyOf(uint32_t part, double* vmin = nullptr) {
        const PartDef* df = defOf(part);
        double v = 0, lo = 0;
        if (!df) return 0;
        for (auto& p : df->pins)
            if (p.kind == PinKind::PowerIn)
                if (const Net* n = net(part, p.id))
                    if (n->volts > v) { v = n->volts; lo = n->voltsMin; }
        if (vmin) *vmin = lo;
        return v;
    }
    // Amps a part takes on one power input, its own draw plus what its outputs fed by that input give.
    void draw(uint32_t part, const PinDef& in, double& typ, double& peak);
    void loadOn(const Net& n, uint32_t srcPart, double& typ, double& peak);
    double motorStall(uint32_t motorPart, double v) {
        const PartDef* md = defOf(motorPart);
        if (!md || !md->hasMotor || md->motor.ratedV <= 0) return 0;
        return md->motor.stallA * v / md->motor.ratedV;
    }
    double motorRun(uint32_t motorPart, double v) {
        const PartDef* md = defOf(motorPart);
        if (!md || !md->hasMotor || md->motor.ratedV <= 0) return 0;
        // A tank track under load: about a third of stall, never below the no load current.
        return std::max(md->motor.noLoadA, md->motor.stallA / 3.0) * v / md->motor.ratedV;
    }
};

void Ctx::loadOn(const Net& n, uint32_t srcPart, double& typ, double& peak) {
    std::set<std::string> seenRail;
    for (auto& np : n.pins) {
        const PartDef* df = defOf(np.part);
        const PinDef* p = df ? df->pin(np.pin) : nullptr;
        if (!p) continue;
        if (p->kind == PinKind::PowerIn) {
            if (np.part == srcPart) continue;
            double t = 0, k = 0;
            draw(np.part, *p, t, k);
            typ += t;
            peak += k;
        }
    }
}

void Ctx::draw(uint32_t part, const PinDef& in, double& typ, double& peak) {
    std::string k = key(part, in.id);
    auto it = drawTyp.find(k);
    if (it != drawTyp.end()) { typ = it->second; peak = drawPeak[k]; return; }
    if (busy.count(k)) { typ = peak = 0; return; }
    busy.insert(k);
    typ = in.iDraw;
    peak = std::max(in.iDrawPeak, in.iDraw);
    const PartDef* df = defOf(part);
    const Net* inNet = net(part, in.id);
    double vin = inNet && inNet->volts > 0 ? inNet->volts : (in.vMax > 0 ? in.vMax : 5.0);
    if (df) {
        std::set<int> netsDone;
        for (auto& o : df->pins) {
            if (o.regFrom != in.id) continue;
            const Net* on = net(part, o.id);
            if (!on) continue;
            int ni = netOf[find(key(part, o.id))];
            if (o.kind == PinKind::PowerOut) {
                if (netsDone.count(ni)) continue;
                netsDone.insert(ni);
                double t = 0, pk = 0;
                loadOn(*on, part, t, pk);
                double vout = o.vOut > 0 ? o.vOut : vin;
                double eff = vout < vin - 0.3 ? 0.9 : 1.0;   // a switching regulator, or straight through
                typ += t * vout / vin / eff;
                peak += pk * vout / vin / eff;
            } else if (o.kind == PinKind::MotorOut) {
                for (auto& np : on->pins) {
                    const PartDef* md = defOf(np.part);
                    const PinDef* mp = md ? md->pin(np.pin) : nullptr;
                    if (!mp || mp->kind != PinKind::MotorIn || mp->id != "M+") continue;
                    typ += motorRun(np.part, vin);
                    peak += motorStall(np.part, vin);
                }
            }
        }
    }
    busy.erase(k);
    drawTyp[k] = typ;
    drawPeak[k] = peak;
}

} // namespace

const char* kindName(PinKind k) {
    switch (k) {
    case PinKind::PowerOut: return "power out";
    case PinKind::PowerIn: return "power in";
    case PinKind::Ground: return "ground";
    case PinKind::DigitalIO: return "GPIO";
    case PinKind::DigitalIn: return "input";
    case PinKind::DigitalOut: return "output";
    case PinKind::Analog: return "analog in";
    case PinKind::Pwm: return "PWM out";
    case PinKind::I2cSda: return "I2C SDA";
    case PinKind::I2cScl: return "I2C SCL";
    case PinKind::UartTx: return "UART TX";
    case PinKind::UartRx: return "UART RX";
    case PinKind::SpiMosi: return "SPI MOSI";
    case PinKind::SpiMiso: return "SPI MISO";
    case PinKind::SpiSck: return "SPI SCK";
    case PinKind::SpiCs: return "SPI CS";
    case PinKind::MotorOut: return "motor out";
    case PinKind::MotorIn: return "motor";
    case PinKind::Camera: return "camera ribbon";
    default: return "other";
    }
}

std::string partLabel(const ElecDoc& d, uint32_t id) {
    const ElecPart* p = d.part(id);
    if (!p) return "(missing part)";
    if (!p->label.empty()) return p->label;
    const PartDef* df = findDef(p->def);
    return df ? df->name : p->def;
}

Report check(const ElecDoc& d) {
    Report r;
    Ctx c(d, r);

    // ---- every pin, joined inside parts by rail
    for (auto& p : d.parts) {
        const PartDef* df = findDef(p.def);
        if (!df) {
            c.issue(Severity::Error, "\"" + c.name(p.id) + "\" uses a part Kestral does not know (" + p.def + ").", {}, {p.id});
            continue;
        }
        r.massG += df->massG;
        std::map<std::string, std::string> firstOfRail;
        for (auto& pin : df->pins) {
            std::string k = Ctx::key(p.id, pin.id);
            c.find(k);
            if (!pin.rail.empty()) {
                auto it = firstOfRail.find(pin.rail);
                if (it == firstOfRail.end()) firstOfRail[pin.rail] = k;
                else c.join(it->second, k);
            }
        }
    }
    // ---- wires
    std::set<uint32_t> wired;
    for (auto& w : d.wires) {
        const PartDef* da = c.defOf(w.a);
        const PartDef* db = c.defOf(w.b);
        if (!da || !db) {
            c.issue(Severity::Error, "A wire goes to a part that is gone.", {w.id});
            continue;
        }
        wired.insert(w.a);
        wired.insert(w.b);
        if (w.aPin == "HEADER" || w.bPin == "HEADER") {
            uint32_t hat = da->stacksOnPi ? w.a : w.b, pi = da->stacksOnPi ? w.b : w.a;
            const PartDef* dh = c.defOf(hat);
            const PartDef* dp = c.defOf(pi);
            if (!dh->stacksOnPi || !dp->pin("GPIO17")) {
                c.issue(Severity::Error, "Only a HAT presses onto a Raspberry Pi header (" + c.name(w.a) + " and " + c.name(w.b) + ").", {w.id});
                continue;
            }
            for (auto& hp : dh->pins)
                if (!hp.header.empty() && dp->pin(hp.header)) c.join(Ctx::key(hat, hp.id), Ctx::key(pi, hp.header));
            continue;
        }
        if (!da->pin(w.aPin) || !db->pin(w.bPin)) {
            c.issue(Severity::Error, "A wire uses a pin that does not exist (" + c.name(w.a) + " " + w.aPin + " to " + c.name(w.b) + " " + w.bPin + ").",
                    {w.id});
            continue;
        }
        c.join(Ctx::key(w.a, w.aPin), Ctx::key(w.b, w.bPin));
    }

    // ---- nets and their voltage
    std::map<std::string, int> rootNet;
    for (auto& [k, v] : c.parent) {
        std::string root = c.find(k);
        auto it = rootNet.find(root);
        int ni;
        if (it == rootNet.end()) {
            ni = (int)r.nets.size();
            rootNet[root] = ni;
            r.nets.push_back({});
        } else {
            ni = it->second;
        }
        size_t colon = k.find(':');
        r.nets[(size_t)ni].pins.push_back({(uint32_t)std::stoul(k.substr(0, colon)), k.substr(colon + 1)});
        c.netOf[root] = ni;
    }
    for (size_t ni = 0; ni < r.nets.size(); ++ni) {
        Net& n = r.nets[ni];
        if (n.pins.size() < 2) continue;
        std::vector<std::pair<uint32_t, const PinDef*>> srcs;
        for (auto& np : n.pins) {
            const PartDef* df = c.defOf(np.part);
            const PinDef* p = df ? df->pin(np.pin) : nullptr;
            if (!p) continue;
            if (p->kind == PinKind::Ground) n.ground = true;
            if (p->kind == PinKind::PowerOut) srcs.push_back({np.part, p});
        }
        for (auto& [part, p] : srcs) {
            if (p->vOut > n.volts) {
                n.volts = p->vOut;
                n.voltsMin = p->vMin > 0 ? p->vMin : p->vOut;
            }
        }
        if (!srcs.empty() && n.ground)
            c.issue(Severity::Error, "SHORT: " + c.pinName(srcs[0].first, srcs[0].second->id) + " is wired to ground. Fix it before power goes on.");
        for (size_t i = 1; i < srcs.size(); ++i) {
            if (srcs[i].first == srcs[0].first) continue;
            double dv = std::abs(srcs[i].second->vOut - srcs[0].second->vOut);
            c.issue(dv > 0.3 ? Severity::Error : Severity::Warning,
                    std::string(dv > 0.3 ? "Two different supplies are joined: " : "Two supplies feed the same rail (back feeding): ") +
                        c.pinName(srcs[0].first, srcs[0].second->id) + " (" + volts(srcs[0].second->vOut) + ") and " +
                        c.pinName(srcs[i].first, srcs[i].second->id) + " (" + volts(srcs[i].second->vOut) + "). Use one of them at a time.");
        }
    }

    // ---- per part: power, ground, levels, required pins
    for (auto& p : d.parts) {
        const PartDef* df = findDef(p.def);
        if (!df) continue;
        bool any = wired.count(p.id) > 0;
        if (!any) {
            c.issue(Severity::Info, c.name(p.id) + " is not wired yet.", {}, {p.id});
            continue;
        }
        bool powered = false, hasPowerIn = false;
        std::set<std::string> railsDone;
        for (auto& pin : df->pins) {
            if (pin.kind != PinKind::PowerIn) continue;
            if (!pin.rail.empty() && railsDone.count(pin.rail)) continue;
            if (!pin.rail.empty()) railsDone.insert(pin.rail);
            if (pin.id == "USBC" && df->stacksOnPi) continue;   // a HAT's charge port is optional
            hasPowerIn = true;
            const Net* n = c.net(p.id, pin.id);
            if (!n || n->volts <= 0) continue;
            powered = true;
            if (n->volts > pin.vMax + 0.05)
                c.issue(Severity::Error, c.pinName(p.id, pin.id) + " gets " + volts(n->volts) + " but takes at most " + volts(pin.vMax) +
                                             ". It would be damaged.", {}, {p.id});
            else if (n->voltsMin < pin.vMin - 0.05)
                c.issue(n->volts < pin.vMin - 0.05 ? Severity::Error : Severity::Warning,
                        c.pinName(p.id, pin.id) + " needs at least " + volts(pin.vMin) + " but gets " +
                            (n->volts < pin.vMin - 0.05 ? volts(n->volts) : "down to " + volts(n->voltsMin) + " as the battery empties") + ".",
                        {}, {p.id});
        }
        if (hasPowerIn && !powered && !df->hasBattery)
            c.issue(Severity::Warning, c.name(p.id) + " has no power: wire its " +
                                           [&] { for (auto& pin : df->pins) if (pin.kind == PinKind::PowerIn && !(pin.id == "USBC" && df->stacksOnPi)) return pin.label; return std::string("power"); }() +
                                           " to a supply.", {}, {p.id});
        // Ground back to whatever powers it.
        if (powered && !df->hasBattery) {
            bool gndOk = false;
            for (auto& pin : df->pins)
                if (pin.kind == PinKind::Ground)
                    if (const Net* n = c.net(p.id, pin.id))
                        for (auto& np : n->pins)
                            if (np.part != p.id) gndOk = true;
            if (!gndOk) c.issue(Severity::Error, c.name(p.id) + " has power but no ground wire back to its supply.", {}, {p.id});
        }
        for (auto& pin : df->pins) {
            if (pin.must.empty()) continue;
            const Net* n = c.net(p.id, pin.id);
            if (!n || n->pins.size() < 2) c.issue(Severity::Error, c.pinName(p.id, pin.id) + " is not connected: " + pin.must + ".", {}, {p.id});
            else if (n->ground) c.issue(Severity::Error, c.pinName(p.id, pin.id) + " is tied to ground: " + pin.must + ".", {}, {p.id});
        }
    }

    // ---- signal nets: levels, directions, swaps, pin current
    for (auto& n : r.nets) {
        if (n.pins.size() < 2) continue;
        double level = n.volts;   // a signal joined to a rail sits at that rail
        std::string levelFrom;
        std::vector<std::pair<uint32_t, const PinDef*>> pins;
        for (auto& np : n.pins) {
            const PartDef* df = c.defOf(np.part);
            const PinDef* p = df ? df->pin(np.pin) : nullptr;
            if (!p) continue;
            // A HAT pin sitting on a Pi header pin is that same pin: count it once.
            if (!df->stacksOnPi) {
                bool mirrored = false;
                for (auto& q : n.pins) {
                    const PartDef* dq = c.defOf(q.part);
                    const PinDef* pq = dq ? dq->pin(q.pin) : nullptr;
                    if (q.part != np.part && dq && dq->stacksOnPi && pq && pq->header == np.pin) mirrored = true;
                }
                if (mirrored) continue;
            }
            pins.push_back({np.part, p});
        }
        bool levelReported = false;
        for (auto& [part, p] : pins) {
            if (!isSignal(p->kind) || p->vOut == 0) continue;
            double v = p->vOut < 0 ? c.supplyOf(part) : p->vOut;
            if (v > level) { level = v; levelFrom = c.pinName(part, p->id); }
        }
        int tx = 0, rx = 0, sda = 0, scl = 0, motorOut = 0, motorIn = 0, cams = 0, outs = 0;
        std::set<uint32_t> outParts;
        double ledLoad = 0, srcMax = 0;
        std::string srcPin;
        for (auto& [part, p] : pins) {
            switch (p->kind) {
            case PinKind::UartTx: ++tx; break;
            case PinKind::UartRx: ++rx; break;
            case PinKind::I2cSda: ++sda; break;
            case PinKind::I2cScl: ++scl; break;
            case PinKind::MotorOut: ++motorOut; break;
            case PinKind::MotorIn: ++motorIn; break;
            case PinKind::Camera: ++cams; break;
            default: break;
            }
            if (isOutput(p->kind) && !outParts.count(part)) { ++outs; outParts.insert(part); }
            if (p->kind == PinKind::DigitalIn) ledLoad += p->iDraw;
            if ((isOutput(p->kind) || p->kind == PinKind::DigitalIO) && p->iMax > 0 && srcMax == 0) { srcMax = p->iMax; srcPin = c.pinName(part, p->id); }
            if (isSignal(p->kind) && p->vMax > 0 && level > p->vMax + 0.25 && !levelReported && (levelReported = true))
                c.issue(Severity::Error, c.pinName(part, p->id) + " takes at most " + volts(p->vMax) + " but gets " + volts(level) +
                                             (levelFrom.empty() ? "" : " from " + levelFrom) +
                                             ". Use a level shifter or a divider, or run the other side at 3.3 V.");
        }
        if (tx > 1 && rx == 0) c.issue(Severity::Error, "Two UART TX pins are joined. Cross them: TX goes to RX.");
        if (rx > 1 && tx == 0) c.issue(Severity::Error, "Two UART RX pins are joined. Cross them: RX goes to TX.");
        if (sda && scl) c.issue(Severity::Error, "I2C SDA and SCL are joined on one wire: SDA goes to SDA and SCL to SCL.");
        if (outs > 1 && tx == 0) c.issue(Severity::Warning, "Two outputs drive the same wire (" + c.pinName(pins[0].first, pins[0].second->id) + "): only one side should drive it.");
        if (motorOut && !motorIn && pins.size() > 1)
            for (auto& [part, p] : pins)
                if (p->kind != PinKind::MotorOut) c.issue(Severity::Error, c.pinName(part, p->id) + " is on a motor output: only a motor goes there.");
        if (motorIn && !motorOut && n.volts > 0)
            c.issue(Severity::Warning, "A motor is wired straight to a supply: it runs flat out with no speed or direction control.");
        if (cams == 1 && pins.size() > 1) c.issue(Severity::Error, "A camera ribbon goes to something that is not a camera port.");
        if (cams == 2) {
            std::set<std::string> kinds;
            for (auto& [part, p] : pins) if (p->kind == PinKind::Camera) kinds.insert(p->note);
            if (kinds.size() == 2)
                c.issue(Severity::Info, "The camera needs a 22 to 15 pin ribbon (\"Standard to Mini\", sold for the Pi 5): the Pi 5 port is the small 22 pin one.");
        }
        if (srcMax > 0 && ledLoad > srcMax)
            c.issue(Severity::Error, srcPin + " can give " + amps(srcMax) + " but its loads take " + amps(ledLoad) + ".");
    }

    // ---- motors on their channels
    for (auto& p : d.parts) {
        const PartDef* df = findDef(p.def);
        if (!df || !df->hasMotor) continue;
        const Net* np = c.net(p.id, "M+");
        const Net* nm = c.net(p.id, "M-");
        auto driverOf = [&](const Net* n) -> std::pair<uint32_t, const PinDef*> {
            if (!n) return {0, nullptr};
            for (auto& x : n->pins) {
                const PartDef* dd = c.defOf(x.part);
                const PinDef* pp = dd ? dd->pin(x.pin) : nullptr;
                if (pp && pp->kind == PinKind::MotorOut) return {x.part, pp};
            }
            return {0, nullptr};
        };
        auto a = driverOf(np), b = driverOf(nm);
        if (!a.second && !b.second) continue;
        if (!a.second || !b.second || a.first != b.first) {
            c.issue(Severity::Warning, c.name(p.id) + ": both motor leads should go to one driver channel (A and B of the same port).", {}, {p.id});
            continue;
        }
        const PartDef* dd = c.defOf(a.first);
        const PinDef* in = dd ? dd->pin(a.second->regFrom) : nullptr;
        double v = 0, vlo = 0;
        if (in)
            if (const Net* n = c.net(a.first, in->id)) { v = n->volts; vlo = n->voltsMin; }
        if (v <= 0) continue;
        double stall = c.motorStall(p.id, v);
        double peakCap = a.second->iPeak > 0 ? a.second->iPeak : a.second->iMax;
        if (stall > peakCap + 1e-6)
            c.issue(Severity::Error, c.name(p.id) + " can stall at about " + amps(stall) + " on " + volts(v) + ", more than the " + amps(peakCap) +
                                         " " + c.name(a.first) + " gives. A jammed track can burn the driver: use a bigger driver or a current limit.",
                    {}, {p.id, a.first});
        else if (stall > a.second->iMax + 1e-6)
            c.issue(Severity::Warning, c.name(p.id) + " stalls at about " + amps(stall) + ": fine for starts, but " + c.name(a.first) +
                                           " only holds " + amps(a.second->iMax) + " for long. Do not let it sit stalled.",
                    {}, {p.id, a.first});
        if (df->motor.ratedV > 0 && std::abs(v - df->motor.ratedV) / df->motor.ratedV > 0.15) {
            double lo = df->motor.noLoadRpm * vlo / df->motor.ratedV, hi = df->motor.noLoadRpm * v / df->motor.ratedV;
            c.issue(v > df->motor.ratedV * 1.25 ? Severity::Warning : Severity::Info,
                    c.name(p.id) + " is rated " + volts(df->motor.ratedV) + " and runs on " + volts(vlo) + " to " + volts(v) + ": about " +
                        fmt("%.0f", lo) + " to " + fmt("%.0f", hi) + " RPM with no load" +
                        (v > df->motor.ratedV * 1.25 ? ", hotter and faster wear than rated." : ", and less torque than rated."),
                    {}, {p.id});
        }
    }

    // ---- I2C addresses on each bus
    for (auto& n : r.nets) {
        std::map<int, std::vector<uint32_t>> addr;
        for (auto& np : n.pins) {
            const PartDef* df = c.defOf(np.part);
            const PinDef* p = df ? df->pin(np.pin) : nullptr;
            if (!p || p->kind != PinKind::I2cSda) continue;
            for (int a : df->i2cAddr) addr[a].push_back(np.part);
        }
        for (auto& [a, parts] : addr)
            if (parts.size() > 1)
                c.issue(Severity::Error, c.name(parts[0]) + " and " + c.name(parts[1]) + " share I2C address " + fmt("0x%02X", a) +
                                             " on one bus: change one with its address pin.");
    }

    // ---- power budget per supply rail
    std::set<int> railDone;
    for (auto& p : d.parts) {
        const PartDef* df = findDef(p.def);
        if (!df) continue;
        for (auto& pin : df->pins) {
            if (pin.kind != PinKind::PowerOut) continue;
            std::string root = c.find(Ctx::key(p.id, pin.id));
            int ni = c.netOf.count(root) ? c.netOf[root] : -1;
            if (ni < 0 || railDone.count(ni)) continue;
            const Net& n = r.nets[(size_t)ni];
            if (n.pins.size() < 2) continue;
            railDone.insert(ni);
            PowerLine pl;
            pl.part = p.id;
            pl.pin = pin.id;
            pl.volts = pin.vOut;
            pl.maxA = pin.iMax;
            c.loadOn(n, p.id, pl.typA, pl.peakA);
            if (pl.typA <= 0 && pl.peakA <= 0) continue;
            r.power.push_back(pl);
            if (pl.typA > pl.maxA + 1e-6)
                c.issue(Severity::Error, c.pinName(p.id, pin.id) + " gives " + amps(pl.maxA) + " but the parts on it take about " + amps(pl.typA) + " running.");
            else if (pl.peakA > pl.maxA * 1.05)
                c.issue(Severity::Warning, c.pinName(p.id, pin.id) + " gives " + amps(pl.maxA) + "; the parts on it can peak at " + amps(pl.peakA) +
                                               " (servos stalling, the Pi at full load). It may reset or brown out at those moments.");
            if (df->hasBattery) {
                r.batteryTypA += pl.typA;
                r.batteryPeakA += pl.peakA;
            }
        }
    }
    for (auto& p : d.parts) {
        const PartDef* df = findDef(p.def);
        if (!df || !df->hasBattery) continue;
        double ah = df->battery.capacityAh;
        auto it = p.vals.find("capacity_mAh");
        if (it != p.vals.end() && it->second > 0) ah = it->second / 1000.0;
        if (r.batteryTypA > 0) r.runtimeH = ah * 0.8 / r.batteryTypA;   // 80%: do not run lithium flat
        break;
    }

    // ---- wire gauges
    for (auto& w : d.wires) {
        if (w.aPin == "HEADER") continue;
        double amp = 0;
        for (int side = 0; side < 2; ++side) {
            uint32_t pid = side ? w.b : w.a;
            const std::string& pn = side ? w.bPin : w.aPin;
            const PartDef* df = c.defOf(pid);
            const PinDef* p = df ? df->pin(pn) : nullptr;
            if (!p) continue;
            if (p->kind == PinKind::PowerIn) {
                double t = 0, k = 0;
                c.draw(pid, *p, t, k);
                amp = std::max(amp, k);
            } else if (p->kind == PinKind::MotorIn) {
                double v = 0;
                if (const Net* n = c.net(pid, pn))
                    for (auto& x : n->pins) {
                        const PartDef* dd = c.defOf(x.part);
                        const PinDef* pp = dd ? dd->pin(x.pin) : nullptr;
                        if (pp && pp->kind == PinKind::MotorOut) v = c.supplyOf(x.part);
                    }
                amp = std::max(amp, c.motorStall(pid, v));
            }
        }
        if (amp <= 0) continue;
        double cap = awgAmps(w.awg);
        if (amp > cap * 1.5)
            c.issue(Severity::Error, "The " + std::to_string(w.awg) + " AWG wire " + c.pinName(w.a, w.aPin) + " to " + c.pinName(w.b, w.bPin) +
                                         " can carry about " + amps(cap) + " but may see " + amps(amp) + ": use thicker wire.", {w.id});
        else if (amp > cap)
            c.issue(Severity::Warning, "The " + std::to_string(w.awg) + " AWG wire " + c.pinName(w.a, w.aPin) + " to " + c.pinName(w.b, w.bPin) +
                                           " is thin for peaks of " + amps(amp) + ".", {w.id});
    }

    std::stable_sort(r.issues.begin(), r.issues.end(), [](const Issue& a, const Issue& b) { return (int)a.sev < (int)b.sev; });
    for (auto& i : r.issues) {
        if (i.sev == Severity::Error) ++r.errors;
        if (i.sev == Severity::Warning) ++r.warnings;
    }
    return r;
}

std::string summary(const ElecDoc& d, const Report& r) {
    std::ostringstream o;
    o << d.parts.size() << " parts, " << d.wires.size() << " wires: " << r.errors << " errors, " << r.warnings << " warnings.\n";
    for (auto& i : r.issues)
        o << (i.sev == Severity::Error ? "ERROR: " : i.sev == Severity::Warning ? "WARNING: " : "note: ") << i.text << "\n";
    for (auto& pl : r.power)
        o << "power: " << partLabel(d, pl.part) << " " << pl.pin << " (" << volts(pl.volts) << ", " << amps(pl.maxA) << " max): about "
          << amps(pl.typA) << " running, " << amps(pl.peakA) << " peak\n";
    if (r.runtimeH > 0) o << fmt("Battery: about %.1f hours", r.runtimeH) << " at the running draw (" << amps(r.batteryTypA) << ").\n";
    if (r.massG > 0) o << fmt("Electronics mass: %.0f g\n", r.massG);
    return o.str();
}

std::string guide(const ElecDoc& d, const Report& r) {
    std::ostringstream o;
    o << "WIRING GUIDE\n\n";
    o << "Parts (" << d.parts.size() << ")\n";
    for (auto& p : d.parts) {
        const PartDef* df = findDef(p.def);
        o << "  - " << partLabel(d, p.id);
        if (df && partLabel(d, p.id) != df->name) o << " (" << df->name << ")";
        if (!p.attach.empty()) o << ", sits on " << p.attach;
        o << "\n";
    }
    // Order: stack HATs, then grounds, power, motors, then signals.
    auto rank = [&](const Wire& w) {
        if (w.aPin == "HEADER") return 0;
        const PartDef* da = findDef(d.part(w.a) ? d.part(w.a)->def : "");
        const PinDef* p = da ? da->pin(w.aPin) : nullptr;
        if (!p) return 5;
        if (p->kind == PinKind::Ground) return 1;
        if (p->kind == PinKind::PowerOut || p->kind == PinKind::PowerIn) return 2;
        if (p->kind == PinKind::MotorOut || p->kind == PinKind::MotorIn) return 3;
        return 4;
    };
    std::vector<const Wire*> ws;
    for (auto& w : d.wires) ws.push_back(&w);
    std::stable_sort(ws.begin(), ws.end(), [&](const Wire* a, const Wire* b) { return rank(*a) < rank(*b); });
    o << "\nSteps (power off and battery unplugged while you wire)\n";
    int n = 0;
    const char* heads[] = {"Stack", "Grounds first", "Power", "Motors", "Signals", "Other"};
    int lastRank = -1;
    for (auto* w : ws) {
        int rk = rank(*w);
        if (rk != lastRank) { o << "\n" << heads[rk] << "\n"; lastRank = rk; }
        ++n;
        if (w->aPin == "HEADER") {
            o << "  " << n << ". Press " << partLabel(d, w->a) << " onto the 40 pin header of " << partLabel(d, w->b)
              << ", every pin seated, with standoffs.\n";
            continue;
        }
        auto end = [&](uint32_t pid, const std::string& pin) {
            const PartDef* df = findDef(d.part(pid) ? d.part(pid)->def : "");
            const PinDef* p = df ? df->pin(pin) : nullptr;
            std::string s = partLabel(d, pid) + " " + (p ? p->label : pin);
            if (p && !p->connector.empty()) s += " [" + p->connector + "]";
            return s;
        };
        o << "  " << n << ". " << end(w->a, w->aPin) << "  ->  " << end(w->b, w->bPin);
        std::string meta;
        if (w->color == "ribbon") meta = "ribbon cable";
        else {
            if (!w->color.empty()) meta += w->color;
            meta += (meta.empty() ? "" : ", ") + std::to_string(w->awg) + " AWG";
        }
        o << "  (" << meta << ")";
        if (!w->note.empty()) o << "  " << w->note;
        o << "\n";
    }
    if (!r.issues.empty()) {
        o << "\nBefore you power it on\n";
        for (auto& i : r.issues)
            o << "  " << (i.sev == Severity::Error ? "[FIX] " : i.sev == Severity::Warning ? "[CHECK] " : "[NOTE] ") << i.text << "\n";
    }
    std::set<std::string> seenNotes;
    std::ostringstream notes;
    for (auto& p : d.parts) {
        const PartDef* df = findDef(p.def);
        if (!df) continue;
        for (auto& t : df->notes)
            if (seenNotes.insert(t).second) notes << "  - " << df->name << ": " << t << "\n";
    }
    if (!seenNotes.empty()) o << "\nThings that bite people\n" << notes.str();
    if (!r.power.empty()) {
        o << "\nPower budget\n";
        for (auto& pl : r.power)
            o << "  - " << partLabel(d, pl.part) << " " << pl.pin << " (" << volts(pl.volts) << ", up to " << amps(pl.maxA) << "): about " << amps(pl.typA)
              << " running, " << amps(pl.peakA) << " peak\n";
        if (r.runtimeH > 0) o << "  Battery life: about " << fmt("%.1f", r.runtimeH) << " hours of driving (80% of the pack).\n";
    }
    return o.str();
}

} // namespace kst::elec
