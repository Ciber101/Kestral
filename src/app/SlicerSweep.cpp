// Developer check of the slicer settings: in a --batch script, the line
//   {"t":"_slicer_sweep","a":{"report":"<path>", "only":"<key prefix>", "machine":"", "process":"", "filament":"", "bed":""}}
// slices the design once as it is, then once per OrcaSlicer setting with that setting changed (every
// choice of a list setting, the other state of an on/off one, a nearby value of a number), and writes
// which ones the engine refused and why. Profiles default to the ones OrcaSlicer has selected.
#include "app/App.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Types.h"
#include "kernel/Kernel.h"
#include "platform/Platform.h"
#include "print/Orca.h"
#include "print/OrcaOptions.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace kst {

namespace {

std::string fmtNum(double v, bool integer) {
    char b[32];
    if (integer) std::snprintf(b, sizeof b, "%lld", (long long)std::llround(v));
    else std::snprintf(b, sizeof b, "%g", std::round(v * 1000.0) / 1000.0);
    return b;
}

// The test values for one setting, from its kind and its value in the profiles.
std::vector<std::string> testValues(const print::OrcaOptionDef& d, const std::string& cur) {
    std::string t = d.type;
    std::vector<std::string> out;
    double lo = d.min && *d.min ? std::atof(d.min) : -1e18, hi = d.max && *d.max ? std::atof(d.max) : 1e18;
    if (t == "coBool" || t == "coBools") {
        out.push_back(cur == "1" || cur == "true" ? "0" : "1");
    } else if (t == "coEnum" || t == "coEnums") {
        for (auto& c : print::splitChoices(d.values))
            if (c != cur) out.push_back(c);
    } else if (t == "coInt" || t == "coInts") {
        long long n = std::atoll(cur.c_str());
        double v = n + 1 <= hi ? n + 1 : n - 1;
        if (v >= lo && v <= hi) out.push_back(fmtNum(v, true));
    } else if (t == "coFloat" || t == "coFloats" || t == "coPercent" || t == "coPercents" || t == "coFloatOrPercent") {
        bool pct = !cur.empty() && cur.back() == '%';
        double x = std::atof(cur.c_str());
        double v = x != 0 ? x * 1.2 : (lo > 0 && lo < 1e17 ? lo : 1.0);
        if (v > hi) v = x * 0.8;
        v = std::clamp(v, lo, hi);
        if (v != x) out.push_back(fmtNum(v, false) + (pct || t == "coPercent" || t == "coPercents" ? "%" : ""));
    }
    return out;   // text, points and G-code are not swept
}

std::string firstValue(const json::Value& v) {
    const json::Value& x = v.isArray() && v.size() ? v[(size_t)0] : v;
    if (x.isString()) return x.asString();
    if (x.isNumber()) return fmtNum(x.asNumber(), false);
    if (x.isBool()) return x.asBool() ? "1" : "0";
    return {};
}

} // namespace

std::string App::slicerSweep(const std::string& argsJson) {
    json::Value a = json::Value::parse(argsJson);
    std::string report = a["report"].asString();
    if (report.empty()) report = platform::tempDir() + "/slicer_sweep.txt";
    std::string only = a["only"].asString();
    std::map<std::string, std::string> fixed;   // "settings": changes used in every slice
    for (auto& [k, v] : a["settings"].members()) fixed[k] = v.isString() ? v.asString() : v.dump();

    // The design as it is now, for the engine.
    std::vector<kernel::ExportItem> items;
    for (auto& [id, b] : doc_.bodies) {
        if (!doc_.isBodyAlive(b) || !bodyShown(b)) continue;
        auto pit = doc_.parts.find(b.partId);
        if (pit == doc_.parts.end() || !pit->second.mesh) continue;
        items.push_back({pit->second.shape, pit->second.mesh, b.xform, b.name.empty() ? "part" : b.name, b.color});
    }
    if (items.empty()) return "Error: the design has no bodies to slice.";

    std::string machine = a["machine"].asString(), process = a["process"].asString(), filament = a["filament"].asString(),
                bed = a["bed"].asString();
    JobSystem* jobs = jobs_.get();
    jobs_->submit("slicer sweep", [this, items, report, only, fixed, machine, process, filament, bed] {
        std::ofstream rep(report, std::ios::binary | std::ios::trunc);
        print::OrcaSetup s = orcaSetupCached(true);
        std::string work = platform::tempDir() + "/sweep" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
        std::error_code ec;
        std::filesystem::create_directories(u8path(work), ec);
        std::string model = work + "/sweep_model.3mf";
        CancelToken ct;
        kernel::export3mf(items, model, ct);

        print::SliceRequest base;
        base.model3mf = model;
        base.machine = machine.empty() ? s.machine : machine;
        base.process = process.empty() ? s.process : process;
        base.filament = filament.empty() ? s.filament : filament;
        base.bedType = bed.empty() ? s.bedType : bed;
        base.arrange = true;
        base.outDir = work;
        base.name = "sweep";
        rep << "Slicer sweep\nengine: " << s.exe << "\nprinter: " << base.machine << "\nprocess: " << base.process
            << "\nfilament: " << base.filament << "\nplate: " << base.bedType << "\n";
        for (auto& [k, v] : fixed) rep << "always: " << k << " = " << v << "\n";
        rep << "\n";

        std::string err;
        json::Value full = json::Value::parse(print::fullSettings(s, base.machine, base.process, base.filament, work + "/full", nullptr, err));
        if (!full.isObject()) { rep << "Could not read the profiles' settings: " << err << "\n"; return; }

        auto run = [&](const std::map<std::string, std::string>& ov, std::string& why) {
            print::SliceRequest r = base;
            r.overrides = fixed;
            for (auto& [k, v] : ov) r.overrides[k] = v;
            auto t0 = std::chrono::steady_clock::now();
            print::SliceResult res = print::slice(s, r, nullptr);
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            char b[96];
            if (res.ok) {
                std::snprintf(b, sizeof b, "ok  %.1f s, print %s, %.1f g", secs, print::formatDuration(res.seconds).c_str(), res.grams);
                why = b;
                for (auto& w : res.warnings) why += " | warning: " + w;
                return true;
            }
            why = res.error;
            for (auto& c : why) if (c == '\n' || c == '\r') c = ' ';
            return false;
        };

        std::string why;
        bool baseOk = run({}, why);
        rep << "BASELINE (profiles as they are): " << why << "\n\n";
        rep.flush();
        if (!baseOk) { rep << "The plain slice already fails, so the sweep stops here.\n"; return; }

        int tested = 0, failed = 0, skipped = 0;
        std::string failures;
        std::set<std::string> seen;
        for (const auto* row : {"process", "filament", "printer"}) {
            for (const print::OrcaLayoutRow* lr : print::orcaLayout(row)) {
                std::string key = lr->key ? lr->key : "";
                if (key.empty() || !seen.insert(key).second) continue;
                if (!only.empty() && key.rfind(only, 0) != 0) continue;
                const print::OrcaOptionDef* d = print::orcaOption(key);
                if (!d || !full.has(key)) { ++skipped; continue; }
                std::string cur = firstValue(full[key]);
                auto vals = testValues(*d, cur);
                if (vals.empty()) { ++skipped; continue; }
                std::string cat = d->category ? d->category : "";
                for (auto& v : vals) {
                    std::map<std::string, std::string> ov{{key, v}};
                    // Support settings only matter with supports on.
                    if (cat == "Support" && key != "enable_support") ov["enable_support"] = "1";
                    bool ok = run(ov, why);
                    ++tested;
                    std::string line = std::string(row) + "  " + key + " = " + v + " (was " + cur + ")" +
                                       (ov.size() > 1 ? " +enable_support" : "") + "  ->  " + (ok ? "" : "FAILED: ") + why;
                    rep << line << "\n";
                    rep.flush();
                    if (!ok) { ++failed; failures += line + "\n"; }
                    log::info("sweep " + std::to_string(tested) + ": " + key + " = " + v + (ok ? " ok" : " FAILED"));
                }
            }
        }
        rep << "\nDONE. " << tested << " slices, " << failed << " failed, " << skipped << " settings not swept (text, points, G-code).\n";
        rep << "\nFAILURES:\n" << (failures.empty() ? "none\n" : failures);
    }, [](const std::string& e) { log::error("slicer sweep: " + e); });
    return "Slicer sweep started; the report goes to " + report;
}

} // namespace kst
