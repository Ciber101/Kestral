// The Slice tab: Kestral's own slicer screen. Pick printer, filament and process, change settings,
// slice, look through the layers and print. The slicing itself is OrcaSlicer's engine running hidden
// (Kestral keeps its own copy and updates it from OrcaSlicer's releases), so results match OrcaSlicer
// exactly and nothing of OrcaSlicer ever shows on screen.
#include "app/App.h"
#include "app/Thumbnails.h"
#include "core/Log.h"
#include "platform/Embed.h"
#include "platform/Platform.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <fstream>

namespace kst {

using namespace ui;
using json::Value;

namespace {

double secsNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A setting's value as text (arrays: their first entry, which is what one extruder uses).
std::string settingText(const Value& v) {
    if (v.isArray()) return v.size() ? settingText(v[(size_t)0]) : std::string();
    if (v.isString()) return v.asString();
    if (v.isNumber()) {
        std::ostringstream o;
        o << v.asNumber();
        return o.str();
    }
    if (v.isBool()) return v.asBool() ? "1" : "0";
    return {};
}

// "sparse_infill_density" -> "Sparse infill density"
std::string humanKey(const std::string& k) {
    std::string s = k;
    for (auto& c : s) if (c == '_') c = ' ';
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

bool metaKey(const std::string& k) {
    static const char* skip[] = {"name", "inherits", "from", "type", "version", "instantiation", "setting_id", "filament_id",
                                 "compatible_printers", "compatible_printers_condition", "compatible_prints", "compatible_prints_condition",
                                 "print_settings_id", "printer_settings_id", "filament_settings_id", "is_custom_defined", "printer_model",
                                 "printer_variant", "default_print_profile", "default_filament_profile", "upward_compatible_machine",
                                 "inherits_group", "filament_vendor"};
    for (auto* s : skip) if (k == s) return true;
    return k.find("gcode") != std::string::npos && k != "gcode_flavor";   // long scripts: not one line settings
}

struct Choice { const char* value; const char* label; };
const Choice kPatterns[] = {{"gyroid", "Gyroid"}, {"grid", "Grid"}, {"crosshatch", "Cross hatch"}, {"honeycomb", "Honeycomb"},
                            {"3dhoneycomb", "3D honeycomb"}, {"cubic", "Cubic"}, {"adaptivecubic", "Adaptive cubic"}, {"triangles", "Triangles"},
                            {"tri-hexagon", "Tri hexagon"}, {"line", "Line"}, {"rectilinear", "Rectilinear"}, {"alignedrectilinear", "Aligned rectilinear"},
                            {"zig-zag", "Zig zag"}, {"concentric", "Concentric"}, {"lightning", "Lightning"}, {"supportcubic", "Support cubic"},
                            {"hilbertcurve", "Hilbert curve"}, {"archimedeanchords", "Archimedean chords"}, {"octagramspiral", "Octagram spiral"}};
const Choice kSupport[] = {{"normal(auto)", "Normal (auto)"}, {"tree(auto)", "Tree (auto)"}, {"normal(manual)", "Normal (manual)"},
                           {"tree(manual)", "Tree (manual)"}};
const Choice kBrim[] = {{"auto_brim", "Auto"}, {"brim_ears", "Mouse ears"}, {"outer_only", "Outer only"}, {"inner_only", "Inner only"},
                        {"outer_and_inner", "Outer and inner"}, {"no_brim", "None"}};
const char* kBeds[] = {"Cool Plate", "Engineering Plate", "High Temp Plate", "Textured PEI Plate", "Textured Cool Plate", "Supertack Plate"};

} // namespace

// ===================================================================== engine

std::string App::orcaPath() {
    std::string found = orcaPathFind();
    std::lock_guard lock(print_.m);
    print_.exeHint = found;
    return found;
}

std::string App::orcaPathFind() {
    std::error_code ec;
    auto ok = [&](const std::string& p) { return !p.empty() && (p.rfind("flatpak:", 0) == 0 || std::filesystem::exists(u8path(p), ec)); };
    if (ok(orcaExe_)) return orcaExe_;
    // Kestral's own copy of the engine, then an OrcaSlicer installed on this computer.
    if (slice_.engine.exe.empty()) slice_.engine = print::managedEngine();
    if (ok(slice_.engine.exe)) return orcaExe_ = slice_.engine.exe;
    if (ok(settings_.slicerPath) && toLower(settings_.slicerPath).find("orca") != std::string::npos) return orcaExe_ = settings_.slicerPath;
    std::string p = platform::findOrcaSlicer();
    if (ok(p)) return orcaExe_ = p;
    for (auto& s : platform::detectSlicers())
        if (toLower(s.name).find("orca") != std::string::npos) return orcaExe_ = s.path;
    return {};
}

std::string App::engineStatusText() {
    std::string exe = orcaPathFind();
    if (exe.empty()) return "No slicer engine yet";
    std::string v = slice_.engineVersion.empty() ? std::string() : " " + slice_.engineVersion;
    return slice_.engine.managed && exe == slice_.engine.exe ? "Slicer engine: OrcaSlicer" + v
                                                             : "Slicer engine: your installed OrcaSlicer" + v;
}

void App::engineCheck(bool install) {
    if (slice_.checking || slice_.installing) return;
    if (install && slice_.latest.url.empty()) {
        // Nothing looked up yet: look, then install.
    } else if (install) {
        slice_.installing = true;
        slice_.installError.clear();
        slice_.engineCancel = std::make_shared<std::atomic<bool>>(false);
        auto rel = slice_.latest;
        auto cancel = slice_.engineCancel;
        JobSystem* jobs = jobs_.get();
        jobs_->submit("install slicer engine", [this, jobs, rel, cancel] {
            std::string err;
            bool ok = print::installEngine(rel, [this](const std::string& stage, double f) {
                std::lock_guard lk(slice_.m);
                slice_.installStage = stage;
                slice_.installFrac = f;
            }, cancel.get(), err);
            jobs->post([this, ok, err] {
                slice_.installing = false;
                if (!ok) {
                    slice_.installError = err;
                    if (err != "Stopped") toast("Slicer engine: " + err, ToastKind::Error);
                    activity();
                    return;
                }
                slice_.pending = print::pendingEngineVersion();
                if (orcaPathFind().empty()) {
                    // Nothing in use yet: start using it right away, no restart needed.
                    print::applyPendingEngine();
                    slice_.pending.clear();
                    slice_.engine = print::managedEngine();
                    orcaExe_.clear();
                    slice_.engineVersion = slice_.engine.version;
                    slice_.choicesLoaded = false;
                    { std::lock_guard lock(print_.m); print_.setupAt = -1e9; }
                    sliceLoadChoices();
                    toast("Slicer engine installed: OrcaSlicer " + slice_.engine.version, ToastKind::Success);
                } else {
                    toast("OrcaSlicer " + slice_.pending + " is ready. Restart Kestral to finish.", ToastKind::Success);
                }
                activity();
            });
        }, [this](const std::string& e) {
            slice_.installing = false;
            slice_.installError = e;
            activity();
        });
        activity();
        return;
    }
    slice_.checking = true;
    JobSystem* jobs = jobs_.get();
    jobs_->submit("check slicer engine", [this, jobs, install] {
        print::EngineRelease r = print::latestEngineRelease(nullptr);
        jobs->post([this, r, install] {
            slice_.checking = false;
            slice_.checked = true;
            slice_.latest = r;
            if (!r.error.empty()) toast("Could not check for slicer updates: " + r.error, ToastKind::Warning);
            else if (install) engineCheck(true);
            activity();
        });
    }, [this](const std::string& e) {
        slice_.checking = false;
        slice_.latest.error = e;
        activity();
    });
    activity();
}

void App::restartKestral() {
    restartAfterExit_ = true;
    requestQuit();
}

// ===================================================================== entering and leaving

void App::enterSliceWorkspace(int tab) {
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sk_.active) finishSketch();
    if (sliceWs_) { if (tab >= 0) sliceActivate(tab); return; }
    // The tab: the one asked for, else the open design's own (made the first time).
    if (tab < 0) {
        tab = sliceTabFor(sliceDocKey());
        if (tab < 0) {
            SliceDocState d;
            d.key = sliceDocKey();
            d.name = doc_.name;
            sliceDocs_.push_back(std::move(d));
            tab = (int)sliceDocs_.size() - 1;
        }
    }
    sliceActivate(tab);
    sliceWs_ = true;
    startOpen_ = false;
    slice_.designCam = cam_;
    slice_.haveDesignCam = true;
    slice_.framed = false;
    // Bodies selected in the design are the ones to print: the plate holds just those.
    // An empty selection shows every visible body again. (A tab of a design not open stays as it was.)
    if (slice_.live && sliceTabFor(sliceDocKey()) == sliceActive_) {
    slice_.plateOnly.clear();
    for (uint32_t id : selection_) slice_.plateOnly.insert(id);
    for (auto it = slice_.plateOnly.begin(); it != slice_.plateOnly.end();)
        it = doc_.bodies.count(*it) ? std::next(it) : slice_.plateOnly.erase(it);
    if (slice_.plateOnly.empty()) {
        // Nothing (valid) selected: fall back to whole bodies picked through faces and edges.
        std::set<uint32_t> picked;
        for (auto& f : selFaces_) picked.insert(f.first);
        for (auto& e : selEdges_) picked.insert(e.first);
        for (auto it = picked.begin(); it != picked.end();)
            it = doc_.bodies.count(*it) ? std::next(it) : picked.erase(it);
        slice_.plateOnly = std::move(picked);
    }
    // The design changed since the last slice: show the design, the old preview is out of date.
    if (slice_.slicedRev != doc_.revision) slice_.preview = false;
    }
    if (!slice_.choicesLoaded) sliceLoadChoices();
    plateSync();
    if (!slice_.engineVersionAsked) {
        slice_.engineVersionAsked = true;
        std::string exe = orcaPath();
        if (!exe.empty() && exe.rfind("flatpak:", 0) != 0) {
            if (slice_.engine.managed && exe == slice_.engine.exe && !slice_.engine.version.empty()) {
                slice_.engineVersion = slice_.engine.version;
            } else {
                JobSystem* jobs = jobs_.get();
                jobs_->submit("slicer engine version", [this, jobs, exe] {
                    std::string v = print::engineVersionOf(exe);
                    jobs->post([this, v] { slice_.engineVersion = v; activity(); });
                });
            }
        }
        slice_.pending = print::pendingEngineVersion();
    }
    sceneDirty_ = true;
    activity();
}

void App::leaveSliceWorkspace() {
    if (!sliceWs_) return;
    sliceSnapshot();   // the plate as it is, in case another design is opened next
    sliceWs_ = false;
    if (slice_.haveDesignCam) cam_ = slice_.designCam;
    sceneDirty_ = true;
    activity();
}

// ===================================================================== profiles and settings

void App::sliceLoadChoices() {
    if (slice_.setupLoading) return;
    slice_.setupLoading = true;
    orcaPath();   // the worker side needs to know where the engine is
    JobSystem* jobs = jobs_.get();
    jobs_->submit("read print profiles", [this, jobs] {
        auto s = std::make_shared<const print::OrcaSetup>(orcaSetupCached(true));
        jobs->post([this, s] {
            slice_.setupLoading = false;
            slice_.setup = s;
            slice_.choicesLoaded = true;
            // First time: what OrcaSlicer has chosen. Choices that no longer exist are dropped.
            if (slice_.machine.empty() || !print::findPreset(*s, "machine", slice_.machine)) slice_.machine = s->machine;
            if (slice_.process.empty() || !print::findPreset(*s, "process", slice_.process)) slice_.process = s->process;
            if (slice_.filament.empty() || !print::findPreset(*s, "filament", slice_.filament)) slice_.filament = s->filament;
            if (slice_.bedType.empty()) slice_.bedType = s->bedType;
            slice_.choicesFor = "\x01";   // lists are rebuilt
            sliceLoadSettings();
            activity();
        });
    }, [this](const std::string& e) {
        slice_.setupLoading = false;
        toast("Could not read the print profiles: " + e, ToastKind::Error);
    });
}

// The printer's bed as OrcaSlicer draws it (its model, grid and plate labels), read again when the
// printer or the plate type changes.
void App::sliceLoadBed() {
    if (!slice_.setup || slice_.bedLoading || slice_.machine.empty()) return;
    std::string key = slice_.machine + "|" + slice_.bedType;
    if (key == slice_.bedFor) return;
    slice_.bedLoading = true;
    auto s = slice_.setup;
    std::string mach = slice_.machine, bt = slice_.bedType;
    JobSystem* jobs = jobs_.get();
    jobs_->submit("read the printer's bed", [this, jobs, s, mach, bt, key] {
        print::BedScene bs = print::loadBedScene(*s, mach, bt);
        jobs->post([this, bs, key] {
            slice_.bedLoading = false;
            bool newPrinter = slice_.bedFor.substr(0, slice_.bedFor.find('|')) != key.substr(0, key.find('|'));
            slice_.bedFor = key;
            slice_.bedScene = bs;
            slice_.bed = bs.plate;
            if (newPrinter) slice_.framed = false;
            if (!bs.modelFile.empty()) log::info("Bed model: " + bs.modelFile);
            sceneDirty_ = true;
            activity();
        });
    }, [this](const std::string&) { slice_.bedLoading = false; });
}

void App::sliceLoadSettings() {
    if (!slice_.setup) return;
    auto s = slice_.setup;
    // Lists that fit the printer.
    if (slice_.choicesFor != slice_.machine) {
        slice_.choicesFor = slice_.machine;
        slice_.printers = print::presetChoices(*s, "machine", "");
        slice_.filaments = print::presetChoices(*s, "filament", slice_.machine);
        slice_.processes = print::presetChoices(*s, "process", slice_.machine);
        auto has = [](const std::vector<const print::Preset*>& v, const std::string& n) {
            for (auto* p : v) if (p->name == n) return true;
            return false;
        };
        if (!slice_.machine.empty()) {
            if (!has(slice_.processes, slice_.process)) slice_.process = slice_.processes.empty() ? "" : slice_.processes.front()->name;
            if (!has(slice_.filaments, slice_.filament)) slice_.filament = slice_.filaments.empty() ? "" : slice_.filaments.front()->name;
        }
    }
    std::string key = slice_.machine + "|" + slice_.process + "|" + slice_.filament;
    if (key == slice_.settingsFor || slice_.settingsLoading) return;
    slice_.settingsLoading = true;
    std::string mach = slice_.machine, proc = slice_.process, fil = slice_.filament;
    JobSystem* jobs = jobs_.get();
    jobs_->submit("read print settings", [this, jobs, s, mach, proc, fil, key] {
        auto get = [&](const std::string& type, const std::string& name) {
            const print::Preset* p = name.empty() ? nullptr : print::findPreset(*s, type, name);
            std::string err;
            return p ? Value::parse(print::resolvePreset(*s, *p, err)) : Value();
        };
        Value m = get("machine", mach), p = get("process", proc), f = get("filament", fil);

        // Every setting, the engine's own defaults included (profiles only list what they change).
        Value all;
        if (!mach.empty() && !proc.empty() && !fil.empty()) {
            std::string err;
            all = Value::parse(print::fullSettings(*s, mach, proc, fil, platform::tempDir() + "/slice/settings", nullptr, err));
            if (!err.empty()) log::warn("All settings: " + err);
        }
        jobs->post([this, m, p, f, all, key, mach] {
            slice_.settingsLoading = false;
            slice_.machineJson = m;
            slice_.processJson = p;
            slice_.filamentJson = f;
            slice_.allJson = all;
            slice_.settingsFor = key;
            (void)mach;
            // Choices changed while reading: read again.
            if (key != slice_.machine + "|" + slice_.process + "|" + slice_.filament) sliceLoadSettings();
            activity();
        });
    }, [this](const std::string&) { slice_.settingsLoading = false; });
}

// ===================================================================== slicing

void App::startSliceJob() {
    if (slice_.slicing) return;
    std::string exe = orcaPath();
    if (exe.empty()) { toast("Install the slicer engine first (top right of the Slice tab)", ToastKind::Warning); return; }
    if (slice_.machine.empty() || slice_.process.empty() || slice_.filament.empty()) {
        toast("Pick a printer, filament and process first", ToastKind::Warning);
        return;
    }
    // What is on the plate, exactly where it was placed (Prepare).
    plateSync();
    std::vector<kernel::ExportItem> items;
    std::vector<int> itemFil;   // tab filament of each item
    std::vector<std::vector<int>> itemPaint;   // painted tab filament (1 based) per triangle
    sliceFilsEnsure();
    for (auto& [id, o] : slice_.plate) {
        if (!o.onPlate) continue;
        if (!plateInFilter(id)) continue;
        if (slice_.onlySelected && !plateIsSel(id, 0) && !(o.bodyId && selection_.count(o.bodyId))) continue;
        if (!o.mesh) continue;
        for (int k = 0; k < (int)o.pos.size(); ++k) {
            if (plateOfCopy(o, k) != slice_.activePlate) continue;   // Slice plate: the plate in front
            glm::vec2 off = plateOffset(plateOfCopy(o, k));
            glm::mat4 x = glm::translate(glm::mat4(1.f), glm::vec3(-off.x, -off.y, 0.f)) * plateXform(id, k);
            kernel::ExportItem it{o.shape, o.mesh, x, o.pos.size() > 1 ? o.name + " " + std::to_string(k + 1) : o.name, o.color};
            for (auto& [sk, sv] : o.settings) it.settings.push_back({sk, sv});
            // Painted faces: per triangle of the part's mesh.
            std::vector<int> pc;
            if (!o.paintColor.empty() || !o.paintSupport.empty() || !o.paintSeam.empty()) {
                const auto& tf = o.mesh->triFace;
                size_t nt = o.mesh->indices.size() / 3;
                auto fill = [&](const std::map<uint32_t, int>& m, std::vector<uint8_t>& out) {
                    if (m.empty()) return;
                    out.assign(nt, 0);
                    for (size_t t = 0; t < nt && t < tf.size(); ++t) { auto f = m.find(tf[t]); if (f != m.end()) out[t] = (uint8_t)f->second; }
                };
                fill(o.paintSupport, it.triSupport);
                fill(o.paintSeam, it.triSeam);
                if (!o.paintColor.empty()) {
                    pc.assign(nt, 0);
                    for (size_t t = 0; t < nt && t < tf.size(); ++t) { auto f = o.paintColor.find(tf[t]); if (f != o.paintColor.end()) pc[t] = std::clamp(f->second, 1, (int)slice_.ex.fils.size()); }
                }
            }
            items.push_back(it);
            itemPaint.push_back(std::move(pc));
            itemFil.push_back(std::clamp(o.fil, 0, (int)slice_.ex.fils.size() - 1));
        }
    }
    if (items.empty()) {
        toast(slice_.onlySelected ? "Nothing is selected" : slice_.plates > 1 ? "Plate " + std::to_string(slice_.activePlate + 1) + " is empty" : "There is nothing visible to print",
              ToastKind::Warning);
        return;
    }
    std::string name = doc_.name.empty() ? "Kestral design" : doc_.name;
    if (sliceActive_ >= 0 && sliceActive_ < (int)sliceDocs_.size() && !sliceDocs_[(size_t)sliceActive_].name.empty()) name = sliceDocs_[(size_t)sliceActive_].name;
    if (!slice_.ex.projName.empty()) name = slice_.ex.projName;
    if (slice_.plates > 1) {
        std::string pn = slice_.activePlate < (int)slice_.ex.plateNames.size() ? slice_.ex.plateNames[(size_t)slice_.activePlate] : std::string();
        name += pn.empty() ? " plate " + std::to_string(slice_.activePlate + 1) : " " + pn;
    }
    print::SliceRequest r;
    r.machine = slice_.machine;
    r.process = slice_.process;
    r.filament = slice_.filament;
    r.bedType = slice_.bedType;
    if (slice_.activePlate < (int)slice_.ex.plateBeds.size() && !slice_.ex.plateBeds[(size_t)slice_.activePlate].empty())
        r.bedType = slice_.ex.plateBeds[(size_t)slice_.activePlate];
    r.overrides = slice_.overrides;
    for (auto& [k, v] : slice_.ex.tabOverrides) r.overrides[k] = v;   // this tab's own (calibration)
    r.arrange = false;   // Kestral placed everything already (Arrange on the plate)
    r.orient = slice_.orient;
    r.name = name;
    r.outDir = platform::tempDir() + "/slice";
    std::string safe = name;
    for (auto& ch : safe) if (std::strchr("<>:\"/\\|?*", ch)) ch = '_';
    std::string model = r.outDir + "/" + safe + ".3mf";
    r.model3mf = model;
    // The filaments this plate uses, in the tab's order. One: a plain single filament print (no prime
    // tower). Several: each part prints with its own, the prime tower goes where it fits.
    std::vector<glm::vec3> toolColors;
    std::vector<int> used;
    for (int f : itemFil) if (std::find(used.begin(), used.end(), f) == used.end()) used.push_back(f);
    for (auto& pv : itemPaint)
        for (int f1 : pv) if (f1 > 0 && std::find(used.begin(), used.end(), f1 - 1) == used.end()) used.push_back(f1 - 1);
    std::sort(used.begin(), used.end());
    {
        auto hexOf = [](glm::vec3 c) {
            char b[8];
            std::snprintf(b, sizeof b, "#%02X%02X%02X", (int)std::lround(std::clamp(c.r, 0.f, 1.f) * 255), (int)std::lround(std::clamp(c.g, 0.f, 1.f) * 255),
                          (int)std::lround(std::clamp(c.b, 0.f, 1.f) * 255));
            return std::string(b);
        };
        auto slotOf = [&](int tabFil) { return (int)(std::find(used.begin(), used.end(), tabFil) - used.begin()) + 1; };
        for (size_t i = 0; i < items.size(); ++i) {
            items[i].extruder = slotOf(itemFil[i]);
            if (!itemPaint[i].empty() && used.size() > 1) {
                items[i].triColor.assign(itemPaint[i].size(), 0);
                for (size_t t = 0; t < itemPaint[i].size(); ++t) if (itemPaint[i][t] > 0) items[i].triColor[t] = (uint8_t)slotOf(itemPaint[i][t] - 1);
            }
        }
        for (int f : used) toolColors.push_back(sliceFilColor(f));
        log::info("Slice: " + std::to_string(items.size()) + " parts, " + std::to_string(used.size()) + " filaments");
        if (used.size() > 1) {
            for (int f : used) {
                r.filamentColors.push_back(hexOf(sliceFilColor(f)));
                r.filamentPresets.push_back(sliceFilPreset(f));
            }
            r.filament = sliceFilPreset(used[0]);
            // Flushing volumes between them (Orca's Flushing volumes window).
            size_t n = slice_.ex.fils.size();
            if (slice_.ex.flush.size() == n * n && !r.overrides.count("flush_volumes_matrix")) {
                std::string mtx;
                for (int a : used)
                    for (int b : used) {
                        char t[32];
                        std::snprintf(t, sizeof t, "%.0f", a == b ? 0.f : slice_.ex.flush[(size_t)a * n + (size_t)b] * slice_.ex.flushMul);
                        mtx += (mtx.empty() ? "" : ",") + std::string(t);
                    }
                r.overrides["flush_volumes_matrix"] = mtx;
            }
            // Several filaments make a prime tower: put it in the plate corner farthest from the
            // parts (its default spot can be off small beds, and OrcaSlicer then refuses).
            Aabb bed = plateBedRect();
            std::vector<Aabb> boxes;
            for (auto& it : items) if (it.mesh) boxes.push_back(it.mesh->bounds.transformed(it.xform));
            const float tw = 62.f, td = 50.f, m = 6.f;   // Bambu profiles: a 60 mm wide tower
            glm::vec2 cands[4] = {{bed.max.x - tw - m, bed.max.y - td - m}, {bed.min.x + m, bed.max.y - td - m},
                                  {bed.max.x - tw - m, bed.min.y + m}, {bed.min.x + m, bed.min.y + m}};
            glm::vec2 best = cands[0];
            float bestScore = -1e30f;
            for (auto c : cands) {
                float score = 1e30f;   // distance to the nearest part (overlap scores below zero)
                for (auto& b : boxes) {
                    float dx = std::max({b.min.x - (c.x + tw), c.x - b.max.x, 0.f});
                    float dy = std::max({b.min.y - (c.y + td), c.y - b.max.y, 0.f});
                    bool over = c.x < b.max.x && c.x + tw > b.min.x && c.y < b.max.y && c.y + td > b.min.y;
                    score = std::min(score, over ? -1.f : std::hypot(dx, dy));
                }
                if (score > bestScore) { bestScore = score; best = c; }
            }
            char bx[32], by[32];
            std::snprintf(bx, sizeof bx, "%.1f", best.x);
            std::snprintf(by, sizeof by, "%.1f", best.y);
            if (!r.overrides.count("wipe_tower_x")) r.overrides["wipe_tower_x"] = bx;
            if (!r.overrides.count("wipe_tower_y")) r.overrides["wipe_tower_y"] = by;
        } else {
            for (auto& it : items) it.extruder = 0;   // one filament: a plain single filament print
            r.filament = sliceFilPreset(used.empty() ? 0 : used[0]);
            r.filamentColors = {hexOf(sliceFilColor(used.empty() ? 0 : used[0]))};
        }
    }
    std::vector<int> slicedFils = used;
    std::string changes;
    for (auto& [k, v] : slice_.overrides) changes += (changes.empty() ? "" : ", ") + k + " = " + v;
    slice_.slicing = true;
    slice_.error.clear();
    slice_.sliceStart = secsNow();
    slice_.cancel = std::make_shared<std::atomic<bool>>(false);
    uint64_t rev = doc_.revision;
    auto cancel = slice_.cancel;
    std::string tabKey = sliceActive_ >= 0 ? sliceDocs_[(size_t)sliceActive_].key : std::string();
    int plateIdx = slice_.activePlate;
    JobSystem* jobs = jobs_.get();
    jobs_->setStatus("Slicing " + name);
    jobs_->submit("slice", [this, jobs, items, r, model, name, changes, rev, cancel, tabKey, toolColors, slicedFils, plateIdx] {
        std::error_code ec;
        std::filesystem::create_directories(u8path(r.outDir), ec);
        CancelToken ct;
        kernel::export3mf(items, model, ct);
        print::OrcaSetup s = orcaSetupCached(false);
        print::SliceResult res = print::slice(s, r, cancel.get());
        if (res.ok) {
            // The plate's picture (the engine's own needs a screen): Kestral draws one next to the file.
            std::vector<thumb::Item> ti;
            for (auto& it : items) {
                thumb::Item t;
                t.mesh = it.mesh;
                t.xform = it.xform;
                int slot = std::max(1, it.extruder) - 1;
                t.color = slot < (int)toolColors.size() ? toolColors[(size_t)slot] : it.color;
                ti.push_back(t);
            }
            auto px = thumb::render(ti, 256);
            if (!px.empty()) thumb::writePng(res.gcode3mf + ".png", px, 256, 256);
        }
        jobs->post([this, jobs, res, rev, cancel, tabKey, name, changes, toolColors, slicedFils, plateIdx] {
            jobs->setStatus("");
            slice_.slicing = false;
            if (cancel->load()) { slice_.sliceQueue.clear(); return; }
            if (res.ok) {   // a new picture for this file
                auto f = plateThumbTex_.find(res.gcode3mf);
                if (f != plateThumbTex_.end()) { dropTexture(f->second); plateThumbTex_.erase(f); }
            }
            auto keep = [&](SliceTab::Extra& ex) {
                if (!res.ok) return;
                if ((int)ex.plateResults.size() <= plateIdx) ex.plateResults.resize((size_t)plateIdx + 1);
                ex.plateResults[(size_t)plateIdx] = res;
            };
            // The tab it was sliced for may be parked by now: the result waits there.
            int ti = sliceTabFor(tabKey);
            if (!tabKey.empty() && ti >= 0 && ti != sliceActive_) {
                auto& d = sliceDocs_[(size_t)ti];
                if (!res.ok) { d.error = res.error; return; }
                d.last = res;
                d.lastName = name;
                d.lastChanges = changes;
                d.slicedRev = rev;
                d.toolColors = toolColors;
                d.ex.slicedFils = slicedFils;
                keep(d.ex);
                d.pendingPreview = res.gcode3mf;
                toast(d.name + " is sliced: its tab has the preview", ToastKind::Success);
                return;
            }
            if (res.ok) { slice_.toolColors = toolColors; slice_.ex.slicedFils = slicedFils; keep(slice_.ex); }
            if (res.ok) {
                std::lock_guard lock(print_.m);
                print_.last = res;
                print_.lastName = name;
                print_.lastChanges = changes;
            }
            if (!res.ok) {
                slice_.error = res.error;
                slice_.sliceQueue.clear();
                activity();
                return;
            }
            slice_.slicedRev = rev;
            print::SliceResult shown = res;
            shown.filament.clear();   // filament 1 stays the tab's (the slice may have used another one)
            showSliceResult(shown);
            // Slice all: the next plate.
            if (!slice_.sliceQueue.empty()) {
                int next = slice_.sliceQueue.front();
                slice_.sliceQueue.erase(slice_.sliceQueue.begin());
                if (next >= 0 && next < slice_.plates) {
                    slice_.activePlate = next;
                    startSliceJob();
                }
            }
        });
    }, [this, jobs](const std::string& e) {
        jobs->setStatus("");
        slice_.slicing = false;
        slice_.error = e;
        activity();
    });
    activity();
}

void App::showSliceResult(const print::SliceResult& r) {
    // The assistant slices too: the tab follows whatever was sliced last.
    if (!r.machine.empty()) slice_.machine = r.machine;
    if (!r.process.empty()) slice_.process = r.process;
    if (!r.filament.empty()) slice_.filament = r.filament;
    if (!r.bedType.empty()) slice_.bedType = r.bedType;
    if (slice_.slicedRev == 0) slice_.slicedRev = doc_.revision;
    slice_.error.clear();
    sliceLoadSettings();
    loadSlicePreview(r.gcode3mf);
}

glm::vec3 App::sliceFilamentColor(int) const {
    // The chosen filament profile's color (#RRGGBB); Kestral orange when it has none.
    const json::Value& fc = slice_.filamentJson["filament_colour"];
    std::string s = fc.isArray() && fc.size() ? fc[(size_t)0].asString() : fc.asString();
    if (s.size() == 7 && s[0] == '#') {
        unsigned v = (unsigned)std::strtoul(s.c_str() + 1, nullptr, 16);
        return glm::vec3(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f);
    }
    return glm::vec3(1.f, 0.42f, 0.21f);
}

void App::sliceBuildScalarMeshes(int mode) {
    // The preview colored by a value: the lines again, grouped by gradient step (on a worker).
    if (slice_.scalarBuilding == mode || !slice_.paths) return;
    slice_.scalarBuilding = mode;
    auto tp = slice_.paths;
    JobSystem* jobs = jobs_.get();
    jobs_->submit("preview colors", [this, jobs, tp, mode] {
        std::vector<std::array<MeshPtr, 16>> out(tp->layers.size());
        for (size_t i = 0; i < tp->layers.size(); ++i)
            for (int b = 0; b < print::kBins; ++b) out[i][b] = print::toolpathMesh(tp->layers[i], b, (size_t)-1, mode, tp->lo[mode], tp->hi[mode]);
        jobs->post([this, tp, mode, out = std::move(out)]() mutable {
            if (slice_.scalarBuilding == mode) slice_.scalarBuilding = -1;
            if (slice_.paths != tp) return;
            slice_.meshesScalar = std::move(out);
            slice_.scalarMode = mode;
            sceneDirty_ = true;
            activity();
        });
    });
}

void App::loadSlicePreview(const std::string& file) {
    if (file.empty()) return;
    slice_.loadingPreview = true;
    JobSystem* jobs = jobs_.get();
    jobs_->submit("print preview", [this, jobs, file] {
        auto tp = std::make_shared<print::Toolpaths>(print::loadToolpaths(file));
        std::vector<std::array<MeshPtr, print::kFeatureCount>> meshes(tp->layers.size());
        for (size_t i = 0; i < tp->layers.size(); ++i)
            for (int f = 0; f < print::kFeatureCount; ++f)
                if (tp->used[f]) meshes[i][f] = print::toolpathMesh(tp->layers[i], f);
        // Printed with several filaments: the same lines again, grouped by filament.
        std::vector<std::array<MeshPtr, 16>> byTool;
        int tools = 0;
        for (bool u : tp->toolUsed) tools += u;
        if (tools >= 1) {
            byTool.resize(tp->layers.size());
            for (size_t i = 0; i < tp->layers.size(); ++i)
                for (int t = 0; t < 16; ++t)
                    if (tp->toolUsed[t]) byTool[i][t] = print::toolpathMesh(tp->layers[i], t, (size_t)-1, true);
        }
        jobs->post([this, tp, meshes = std::move(meshes), byTool = std::move(byTool), file]() mutable {
            slice_.loadingPreview = false;
            if (!tp->ok()) {
                toast("Could not show the preview: " + tp->error, ToastKind::Warning);
                activity();
                return;
            }
            slice_.paths = tp;
            slice_.meshes = std::move(meshes);
            slice_.meshesTool = std::move(byTool);
            slice_.pathsFile = file;
            slice_.layerLo = 0;
            slice_.layerHi = (int)tp->layers.size() - 1;
            for (int f = 0; f < print::kFeatureCount; ++f) slice_.show[f] = tp->used[f];
            // Orca's defaults: travel, retracts and filament changes hidden; seams shown.
            for (auto f : {print::Feature::Travel, print::Feature::Retract, print::Feature::Unretract, print::Feature::ToolChange}) slice_.show[(int)f] = false;
            {
                int tools = 0;
                for (bool u : tp->toolUsed) tools += u;
                if (tools > 1 && slice_.viewType == 0) slice_.viewType = 1;   // several filaments: show their colors
            }
            slice_.meshesScalar.clear();
            slice_.scalarMode = -1;
            slice_.preview = true;
            sceneDirty_ = true;
            activity();
        });
    }, [this](const std::string& e) {
        slice_.loadingPreview = false;
        toast("Could not show the preview: " + e, ToastKind::Warning);
    });
}

void App::rebuildSliceScene() {
    std::vector<SceneBatch> batches;
    std::vector<MaterialGpu> mats;
    auto addMat = [&](uint32_t rgba, float rough) {
        MaterialGpu g;
        g.c0 = glm::vec4((rgba & 255) / 255.f, ((rgba >> 8) & 255) / 255.f, ((rgba >> 16) & 255) / 255.f, 1.f);
        g.c2 = glm::vec4(0.f, rough, 10.f, 0.f);
        mats.push_back(g);
        return (uint32_t)mats.size() - 1;
    };
    // OrcaSlicer's colors: the bed model, the plate, its grid lines (RGBA8, R in the low byte).
    const bool darkUi = ui::darkness() > 0.f;
    auto rgb = [](float r, float g, float b) {
        return 0xFF000000u | (uint32_t)std::lround(r * 255) | ((uint32_t)std::lround(g * 255) << 8) | ((uint32_t)std::lround(b * 255) << 16);
    };
    uint32_t bedMat = addMat(rgb(0.2666f, 0.2784f, 0.2784f), 0.8f);
    uint32_t baseMat = addMat(darkUi ? rgb(0.255f, 0.255f, 0.283f) : rgb(0.3255f, 0.337f, 0.337f), 0.6f);
    uint32_t gridMinMat = addMat(darkUi ? rgb(0.431f, 0.431f, 0.463f) : rgb(0.52f, 0.53f, 0.53f), 0.8f);
    uint32_t gridMajMat = addMat(darkUi ? rgb(0.52f, 0.52f, 0.55f) : rgb(0.62f, 0.63f, 0.63f), 0.8f);
    uint32_t featMat[print::kFeatureCount];
    for (int f = 0; f < print::kFeatureCount; ++f) featMat[f] = addMat(print::featureColor(f), 0.5f);
    sceneBounds_ = Aabb();
    uint32_t id = 1;
    bool preview0 = slice_.preview && slice_.paths;
    if (slice_.bed && !slice_.bed->indices.empty()) {
        // One bed per build plate (the preview shows the sliced plate only).
        int n = preview0 ? 1 : std::max(1, slice_.plates);
        for (int pl = 0; pl < n; ++pl) {
            int which = preview0 ? slice_.activePlate : pl;
            glm::vec2 off = preview0 ? glm::vec2(0) : plateOffset(which);
            glm::mat4 X = glm::translate(glm::mat4(1.f), glm::vec3(off.x, off.y, 0.f));
            auto addLook = [&](const MeshPtr& mp, uint32_t mat) {
                if (!mp || mp->indices.empty()) return;
                RenderInstance li;
                li.material = mat;
                li.color = mats[mat].c0;
                li.bodyId = id++;
                li.xform = X;
                batches.push_back({mp, {li}});
            };
            addLook(slice_.bed, bedMat);
            addLook(slice_.bedScene.model, baseMat);
            addLook(slice_.bedScene.gridThin, gridMinMat);
            addLook(slice_.bedScene.gridBold, gridMajMat);
            for (auto& d : slice_.bedScene.decals) addLook(d.mesh, addMat(d.rgba, 0.7f));
            if (which == slice_.activePlate || slice_.plates == 1) sceneBounds_.add(slice_.bed->bounds.transformed(X));
        }
    }
    bool preview = preview0;
    if (!preview) {
        // Prepare: every object on the plate where it was placed. Selected glows, off the plate shows red.
        plateSync();
        slice_.pickMap.clear();
        std::map<const MeshData*, size_t> batchOf;
        for (auto& [bid, o] : slice_.plate) {
            if (!o.onPlate) continue;
            if (!plateInFilter(bid)) continue;
            if (!o.mesh || o.mesh->indices.empty()) continue;
            // The color it will print in: its own, or filament 1's when that is switched off.
            glm::vec3 pc = sliceFilColor(o.fil);
            uint32_t rgba = 0xFF000000u | (uint32_t)(std::clamp(pc.r, 0.f, 1.f) * 255) | ((uint32_t)(std::clamp(pc.g, 0.f, 1.f) * 255) << 8) |
                            ((uint32_t)(std::clamp(pc.b, 0.f, 1.f) * 255) << 16);
            uint32_t bodyMat = addMat(rgba, 0.45f);
            for (int k = 0; k < (int)o.pos.size(); ++k) {
                RenderInstance ri;
                bool onBed = plateInstanceOnBed(bid, k);
                ri.material = onBed ? bodyMat : addMat(0xFF3C3CDCu, 0.5f);   // red (RGBA8, R low)
                ri.color = mats[ri.material].c0;
                ri.bodyId = 1000 + (uint32_t)slice_.pickMap.size();
                slice_.pickMap.push_back({bid, k});
                ri.xform = plateXform(bid, k);
                if (plateIsSel(bid, k)) ri.flags |= kInstSelected;
                MeshPtr shown = platePaintMesh(o);
                auto it = batchOf.find(shown.get());
                if (it == batchOf.end()) {
                    it = batchOf.emplace(shown.get(), batches.size()).first;
                    batches.push_back({shown, {}});
                }
                batches[it->second].instances.push_back(ri);
                sceneBounds_.add(o.mesh->bounds.transformed(ri.xform));
            }
        }
    } else {
        int n = (int)slice_.meshes.size();
        int lo = std::clamp(slice_.layerLo, 0, std::max(n - 1, 0)), hi = std::clamp(slice_.layerHi, 0, std::max(n - 1, 0));
        // Orca's view types: line types, filaments, or a value (speed, height...) as a gradient.
        int mode = std::clamp(slice_.viewType, 0, print::ViewModes - 1);
        const std::vector<std::array<MeshPtr, 16>>* keyed = nullptr;
        if (mode == print::ViewTool && (int)slice_.meshesTool.size() == n) keyed = &slice_.meshesTool;
        if (mode >= print::ViewSpeed) {
            if (slice_.scalarMode == mode && (int)slice_.meshesScalar.size() == n) keyed = &slice_.meshesScalar;
            else sliceBuildScalarMeshes(mode);   // on a worker; line types until it is done
        }
        uint32_t kmat[16];
        if (keyed) {
            for (int t = 0; t < 16; ++t) {
                uint32_t rgba;
                if (mode == print::ViewTool) {
                    glm::vec3 c = t < (int)slice_.toolColors.size() ? slice_.toolColors[(size_t)t] : sliceFilColor(0);
                    rgba = 0xFF000000u | (uint32_t)(std::clamp(c.r, 0.f, 1.f) * 255) | ((uint32_t)(std::clamp(c.g, 0.f, 1.f) * 255) << 8) |
                           ((uint32_t)(std::clamp(c.b, 0.f, 1.f) * 255) << 16);
                } else {
                    rgba = print::binColor(t);
                }
                kmat[t] = addMat(rgba, 0.5f);
            }
        }
        bool cut = slice_.moveHi >= 0 && hi < (int)slice_.paths->layers.size() && (size_t)slice_.moveHi < slice_.paths->layers[(size_t)hi].segs.size();
        auto inst = [&](uint32_t mat) {
            RenderInstance ri;
            ri.material = mat;
            ri.color = mats[mat].c0;
            ri.bodyId = id;
            return ri;
        };
        // Every layer is passed (hidden ones without instances) so the geometry uploads once.
        for (int i = 0; i < n; ++i) {
            bool vis = i >= lo && i <= hi && !(cut && i == hi);
            if (keyed) {
                for (int t = 0; t < 16; ++t) {
                    const MeshPtr& m = (*keyed)[(size_t)i][t];
                    if (!m) continue;
                    SceneBatch b{m, {}};
                    if (vis) b.instances.push_back(inst(kmat[t]));
                    batches.push_back(std::move(b));
                }
            }
            for (int f = 0; f < print::kFeatureCount; ++f) {
                const MeshPtr& m = slice_.meshes[(size_t)i][f];
                if (!m) continue;
                if (keyed && !print::featureIsMove(f)) continue;   // drawn by the keyed meshes
                SceneBatch b{m, {}};
                if (vis && slice_.show[f]) b.instances.push_back(inst(featMat[f]));
                batches.push_back(std::move(b));
            }
        }
        // The move slider shows the top layer only up to a point: that layer gets its own meshes.
        if (cut) {
            const auto& L = slice_.paths->layers[(size_t)hi];
            size_t upTo = (size_t)slice_.moveHi;
            if (keyed)
                for (int t = 0; t < 16; ++t) {
                    if (!(*keyed)[(size_t)hi][t]) continue;
                    MeshPtr pm = print::toolpathMesh(L, t, upTo, mode, slice_.paths->lo[mode], slice_.paths->hi[mode]);
                    if (pm) batches.push_back({pm, {inst(kmat[t])}});
                }
            for (int f = 0; f < print::kFeatureCount; ++f) {
                if (!slice_.meshes[(size_t)hi][f] || !slice_.show[f]) continue;
                if (keyed && !print::featureIsMove(f)) continue;
                MeshPtr pm = print::toolpathMesh(L, f, upTo);
                if (pm) batches.push_back({pm, {inst(featMat[f])}});
            }
            // The nozzle: a small cone where it is now.
            if (upTo > 0 && upTo <= L.segs.size()) {
                const auto& sg = L.segs[upTo - 1];
                slice_.nozzleAt = glm::vec3(sg.x1, sg.y1, L.z);
                slice_.nozzleShown = true;
            }
        } else {
            slice_.nozzleShown = false;
        }
        sceneBounds_.add(slice_.paths->bounds);
    }
    renderer_.setScene(batches, {}, mats, {});
    sceneDirty_ = false;
    sceneRev_ = doc_.revision;
    if (!slice_.framed && sceneBounds_.valid()) {
        slice_.framed = true;
        ImGuiIO& io = ImGui::GetIO();
        cam_.home(sceneBounds_, io.DisplaySize.y > 0 ? io.DisplaySize.x / io.DisplaySize.y : 1.f);
    }
    activity();
}

// ===================================================================== printing

void App::slicePrint() {
    print::SliceResult last;
    std::string name;
    {
        std::lock_guard lock(print_.m);
        last = print_.last;
        name = print_.lastName;
    }
    if (!last.ok) { toast("Slice first", ToastKind::Warning); return; }
    print::PrinterConn c = printerConn();
    if (c.kind == print::PrinterKind::None) {
        toast("Set up your printer first (Preferences > Printer)", ToastKind::Warning);
        settingsOpen_ = true;
        return;
    }
    // Each filament of the print takes the AMS slot picked in the Send print window.
    // (No status from the printer: the AMS setting from Preferences, as before.)
    if (slice_.devStatus.ok) {
        sliceAutoAmsMap();
        c.amsMapping = slice_.ex.amsMap;
        bool anyAms = false;
        for (int m : c.amsMapping) anyAms |= m >= 0 && m < 254;
        c.useAms = anyAms;
        if (!anyAms) c.amsMapping.clear();
    }
    c.bedLeveling = slice_.sendLeveling;
    c.flowCali = slice_.sendFlowCali;
    c.timelapse = slice_.sendTimelapse;
    JobSystem* jobs = jobs_.get();
    jobs_->setStatus("Sending the print");
    jobs_->submit("send print", [this, jobs, c, last, name] {
        print::SendResult sr = print::sendPrint(c, last.gcode3mf, name, nullptr);
        jobs->post([this, jobs, sr, c, last] {
            jobs->setStatus("");
            // No Bambu plugin on this computer: OrcaSlicer's own window sends it.
            if (sr.needsClick && c.kind == print::PrinterKind::BambuOrca) openInOrca(last.gcode3mf);
            toast(sr.ok ? sr.message : "Could not start the print: " + sr.message, sr.ok ? ToastKind::Success : ToastKind::Error);
        });
    }, [this, jobs](const std::string& e) {
        jobs->setStatus("");
        toast("Could not start the print: " + e, ToastKind::Error);
    });
}

void App::sliceAllPlates() {
    slice_.sliceQueue.clear();
    for (int p = 0; p < slice_.plates; ++p) if (p != slice_.activePlate) slice_.sliceQueue.push_back(p);
    startSliceJob();
}

void App::sliceSaveGcode() {
    std::string file;
    {
        std::lock_guard lock(print_.m);
        file = print_.last.ok ? print_.last.gcode3mf : std::string();
    }
    if (file.empty()) return;
    std::string g = print::readZipEntry(file, "Metadata/plate_1.gcode");
    if (g.empty()) { toast("No G-code in the sliced file", ToastKind::Error); return; }
    std::string base = doc_.name.empty() ? "print" : doc_.name;
    std::string to = platform::saveFileDialog("Save the G-code", "G-code (*.gcode)", "*.gcode", "gcode", base + ".gcode");
    if (to.empty()) return;
    std::ofstream o(u8path(to), std::ios::binary | std::ios::trunc);
    o << g;
    if (!o) toast("Could not save " + to, ToastKind::Error);
    else toast("Saved " + pathToU8(u8path(to).filename()), ToastKind::Success);
}

void App::sliceSaveFile() {
    std::string file;
    {
        std::lock_guard lock(print_.m);
        file = print_.last.ok ? print_.last.gcode3mf : std::string();
    }
    if (file.empty()) return;
    std::string base = doc_.name.empty() ? "print" : doc_.name;
    std::string to = platform::saveFileDialog("Save the print file", "Print file (*.gcode.3mf)", "*.gcode.3mf", "3mf", base + ".gcode.3mf");
    if (to.empty()) return;
    std::error_code ec;
    std::filesystem::copy_file(u8path(file), u8path(to), std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) toast("Could not save: " + ec.message(), ToastKind::Error);
    else toast("Saved " + pathToU8(u8path(to).filename()), ToastKind::Success);
}

void App::handleSliceViewInput() {
    // Orbit, pan and zoom in the Slice tab's view. Left drag orbits (like OrcaSlicer), the other
    // buttons follow the navigation style chosen in Preferences.
    ImGuiIO& io = ImGui::GetIO();
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 m = io.MousePos;
    int curTop = sliceCurTop();
    if (curTop != 1 && curTop != 2) { viewportHovered_ = false; return; }   // Home, Device, Project, Calibration cover the view
    float left = vp->Pos.x + sliceSideW(), top = vp->Pos.y + sliceTop();
    (void)S;
    bool inRect = m.x >= left && m.x < vp->Pos.x + vp->Size.x && m.y >= top && m.y < vp->Pos.y + vp->Size.y;
    bool hovered = inRect && !io.WantCaptureMouse && glfwGetWindowAttrib(window_, GLFW_HOVERED);
    bool prepare = curTop == 1;
    viewportHovered_ = prepare && hovered;   // parts on the plate can be picked
    float W = std::max(io.DisplaySize.x, 1.f), H = std::max(io.DisplaySize.y, 1.f);
    if (prepare && drag_ == Drag::None && handlePlateInput(hovered)) return;
    if (drag_ == Drag::None && hovered) {
        for (int btn = 0; btn < 3; ++btn) {
            if (!ImGui::IsMouseClicked((ImGuiMouseButton)btn)) continue;
            Drag d = btn == 0 ? Drag::Orbit : (Drag)navFor(btn);
            if (d == Drag::None) d = Drag::Pan;
            drag_ = d;
            dragBtn_ = btn;
            dragStart_ = m;
            break;
        }
        if (io.MouseWheel != 0.f) {
            float steps = io.MouseWheel * (settings_.reverseZoom ? -1.f : 1.f);
            cam_.zoomAt(steps, glm::vec2(m.x / W * 2.f - 1.f, 1.f - m.y / H * 2.f), W / H);
            activity();
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) { slice_.framed = false; sceneDirty_ = true; }
    }
    if (drag_ != Drag::None) {
        ImVec2 d = io.MouseDelta;
        if (drag_ == Drag::Orbit && (d.x != 0 || d.y != 0)) cam_.orbit(d.x, d.y, settings_.constrainedOrbit);
        if (drag_ == Drag::Pan && (d.x != 0 || d.y != 0)) cam_.pan(d.x, d.y, H);
        if (drag_ == Drag::Zoom && d.y != 0)
            cam_.zoomAt(-d.y * 0.03f, glm::vec2(dragStart_.x / W * 2.f - 1.f, 1.f - dragStart_.y / H * 2.f), W / H);
        if (!ImGui::IsMouseDown((ImGuiMouseButton)std::max(dragBtn_, 0))) { drag_ = Drag::None; dragBtn_ = -1; }
        activity();
    }
}

} // namespace kst
