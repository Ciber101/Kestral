// Slice and print from the assistant: "make the walls 3 thick, infill 25% and print it".
// The assistant can read your OrcaSlicer setup, pick or change profiles, slice in the background,
// and start the print. Nothing is ever sent to a printer without you pressing Print in Kestral.
#include "app/App.h"
#include "print/OrcaOptions.h"
#include <glm/gtc/matrix_transform.hpp>
#include "core/Json.h"
#include "core/Log.h"
#include "platform/Embed.h"
#include "platform/Platform.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <thread>

namespace kst {

using namespace ui;
using json::Value;

namespace {

double nowSecs() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Runs fn on the main thread and waits for its answer (gives up if the chat is stopped).
using PostFn = std::function<void(std::function<void()>)>;

template <class T>
bool onMain(const PostFn& post, const std::shared_ptr<std::atomic<bool>>& cancel, std::function<T()> fn, T& out) {
    auto p = std::make_shared<std::promise<T>>();
    auto f = p->get_future();
    post([p, fn] { p->set_value(fn()); });
    while (!cancel->load())
        if (f.wait_for(std::chrono::milliseconds(150)) == std::future_status::ready) { out = f.get(); return true; }
    return false;
}

std::string valueText(const Value& v) {
    if (v.isString()) return v.asString();
    if (v.isBool()) return v.asBool() ? "1" : "0";
    if (v.isNumber()) {
        double d = v.asNumber();
        char b[32];
        if (std::abs(d - std::round(d)) < 1e-9) std::snprintf(b, sizeof b, "%lld", (long long)std::llround(d));
        else std::snprintf(b, sizeof b, "%g", d);
        return b;
    }
    return v.dump();
}

std::string statusLine(const print::PrintStatus& s) {
    if (!s.ok) return s.error;
    std::ostringstream o;
    o << "The printer is " << s.state;
    if (s.percent >= 0 && (s.state == "printing" || s.state == "paused")) o << ", " << s.percent << "% done";
    if (s.minutesLeft > 0 && s.state == "printing") o << ", about " << print::formatDuration(s.minutesLeft * 60.0) << " left";
    if (!s.job.empty()) o << " (job: " << s.job << ")";
    if (s.nozzleC >= 0) o << ". Nozzle " << (int)std::lround(s.nozzleC) << " C";
    if (s.bedC >= 0) o << ", bed " << (int)std::lround(s.bedC) << " C";
    return o.str() + ".";
}

} // namespace

bool App::aiIsPrintTool(const std::string& n) {
    return n == "print_setup" || n == "list_print_profiles" || n == "slice" || n == "start_print" || n == "printer_status" ||
           n == "set_printer" || n == "slice_view" || n == "slicer_engine" || n == "bambu_printers" || n == "printer_control" ||
           n == "slice_tab" || n == "find_slice_settings" || n == "printer_device";
}

print::PrinterConn App::printerConn() const {
    print::PrinterConn c;
    c.kind = print::kindFromString(settings_.printerKind);
    c.name = settings_.printerName;
    c.host = settings_.printerHost;
    c.serial = settings_.printerSerial;
    c.secret = platform::unprotectSecret(settings_.printerSecret);
    c.useAms = settings_.printerUseAms;
    c.amsSlot = settings_.printerAmsSlot;
    if (std::getenv("KESTRAL_FAKE_DEVICE")) { c.kind = print::PrinterKind::BambuLan; c.name = "Test A1 mini"; }   // developer check of the Device tab
    return c;
}

// Reading every OrcaSlicer profile takes a moment: kept for a minute (worker threads only).
print::OrcaSetup App::orcaSetupCached(bool refresh) {
    {
        std::lock_guard lock(print_.m);
        if (!refresh && nowSecs() - print_.setupAt < 60.0) return print_.setup;
    }
    std::string exe;
    {
        std::lock_guard lock(print_.m);
        exe = print_.exeHint;
    }
    print::OrcaSetup s = print::findOrca(exe);
    std::lock_guard lock(print_.m);
    print_.setup = s;
    print_.setupAt = nowSecs();
    return s;
}

void App::openInOrca(const std::string& file) {
    // Bambu Lab printers in normal (cloud) mode, until Kestral talks to Bambu's network itself: the
    // print file opens in OrcaSlicer's own window (the installed OrcaSlicer, signed in to Bambu).
    std::string exe = platform::findOrcaSlicer();
    if (exe.empty()) exe = orcaPath();
    if (exe.empty()) { toast("OrcaSlicer was not found", ToastKind::Warning); return; }
    std::string err;
    if (!platform::launch(exe, file, err)) toast(err, ToastKind::Error);
    activity();
}

// ===================================================================== tools

std::string App::aiPrintTool(const std::string& name, const std::string& argsJson, const PostFn& post,
                             const std::shared_ptr<std::atomic<bool>>& cancel) {
    Value a = Value::parse(argsJson);
    {
        std::string exe;   // finds OrcaSlicer (main thread) so the worker side knows where it is
        if (!onMain<std::string>(post, cancel, [this] { return orcaPath(); }, exe)) return "Stopped.";
    }

    if (name == "print_setup") {
        print::OrcaSetup s = orcaSetupCached(true);
        print::PrinterConn c;
        if (!onMain<print::PrinterConn>(post, cancel, [this] { return printerConn(); }, c)) return "Stopped.";
        Value o = Value::object();
        Value orca = Value::object();
        orca.set("installed", Value(!s.exe.empty()));
        orca.set("problem", Value(s.problem));
        orca.set("current_printer_profile", Value(s.machine));
        orca.set("current_process_profile", Value(s.process));
        orca.set("current_filament_profile", Value(s.filament));
        orca.set("current_bed_type", Value(s.bedType));
        int userCount = 0;
        for (auto& p : s.presets) userCount += p.user ? 1 : 0;
        orca.set("profiles", Value((int)s.presets.size()));
        orca.set("your_own_profiles", Value(userCount));
        o.set("orcaslicer", orca);
        Value known = Value::array();
        for (auto& k : s.knownPrinters) {
            Value kp = Value::object();
            kp.set("name", Value(k.name));
            kp.set("model", Value(k.model));
            kp.set("ip", Value(k.ip));
            kp.set("serial", Value(k.serial));
            kp.set("orcaslicer_has_access_code", Value(!print::orcaSavedAccessCode(s, k.serial).empty()));
            known.push(kp);
        }
        o.set("printers_orcaslicer_knows", known);
        Value kp = Value::object();
        kp.set("connection", Value(print::kindId(c.kind)));
        kp.set("description", Value(print::kindName(c.kind)));
        kp.set("name", Value(c.name));
        kp.set("host", Value(c.host));
        kp.set("serial", Value(c.serial));
        kp.set("has_access_code_or_key", Value(!c.secret.empty()));
        kp.set("use_ams", Value(c.useAms));
        if (c.useAms) kp.set("ams_slot", Value(c.amsSlot + 1));
        o.set("kestral_printer", kp);
        o.set("bambu_connect_installed", Value(platform::urlSchemeRegistered("bambu-connect")));
        {
            std::lock_guard lock(print_.m);
            if (print_.last.ok) {
                Value ls = Value::object();
                ls.set("design", Value(print_.lastName));
                ls.set("print_time", Value(print::formatDuration(print_.last.seconds)));
                ls.set("filament_grams", Value(std::round(print_.last.grams * 10) / 10));
                o.set("last_slice", ls);
            }
        }
        return o.dump();
    }

    if (name == "list_print_profiles") {
        print::OrcaSetup s = orcaSetupCached(false);
        std::string type = toLower(a["type"].asString("process"));
        if (type == "printer") type = "machine";
        if (type == "print") type = "process";
        auto hits = print::searchPresets(s, type, a["search"].asString(), 30);
        if (hits.empty()) return "No " + type + " profiles match \"" + a["search"].asString() + "\".";
        std::ostringstream o;
        o << type << " profiles (yours first):\n";
        for (auto* p : hits) {
            o << "- " << p->name << (p->user ? "  [yours]" : "  [built in]");
            if (!p->printerModel.empty()) o << "  printer: " << p->printerModel;
            if (!p->compatible.empty() && p->compatible[0][0] != '\x01') o << "  for: " << p->compatible[0] << (p->compatible.size() > 1 ? " ..." : "");
            if (p->user && !p->inherits.empty()) o << "  based on: " << p->inherits;
            o << "\n";
        }
        return o.str();
    }

    if (name == "slice") {
        // 1. What to print, from the open design.
        struct Pick { std::vector<kernel::ExportItem> items; std::string name; };
        Pick pick;
        bool onlySelected = toLower(a["what"].asString()) == "selected";
        if (!onMain<Pick>(post, cancel, [this, onlySelected] {
                Pick p;
                p.name = doc_.name.empty() ? "Kestral design" : doc_.name;
                // What is on the Slice tab's plate, where it sits there (the user sees the same).
                plateSync();
                for (auto& [id, o] : slice_.plate) {
                    if (!o.onPlate) continue;
                    if (onlySelected && !(o.bodyId && selection_.count(o.bodyId)) && !plateIsSel(id, 0)) continue;
                    if (!o.mesh) continue;
                    for (int k = 0; k < (int)o.pos.size(); ++k) {
                        if (plateOfCopy(o, k) != slice_.activePlate) continue;
                        glm::vec2 off = plateOffset(plateOfCopy(o, k));
                        glm::mat4 x = glm::translate(glm::mat4(1.f), glm::vec3(-off.x, -off.y, 0.f)) * plateXform(id, k);
                        p.items.push_back({o.shape, o.mesh, x, o.pos.size() > 1 ? o.name + " " + std::to_string(k + 1) : o.name, o.color});
                    }
                }
                return p;
            }, pick))
            return "Stopped.";
        if (pick.items.empty()) return onlySelected ? "Nothing is selected." : "There is nothing visible to print.";
        // 2. Write it out for OrcaSlicer.
        std::string dir = platform::tempDir() + "/slice";
        std::error_code ec;
        std::filesystem::create_directories(u8path(dir), ec);
        std::string safe = pick.name;
        for (auto& ch : safe) if (std::strchr("<>:\"/\\|?*", ch)) ch = '_';
        std::string model = dir + "/" + safe + ".3mf";
        CancelToken ct;
        try {
            kernel::export3mf(pick.items, model, ct);
        } catch (const std::exception& e) {
            return std::string("Could not prepare the design: ") + e.what();
        }
        // 3. Slice.
        print::OrcaSetup s = orcaSetupCached(false);
        if (s.exe.empty()) return "OrcaSlicer is not installed. " + s.problem;
        print::SliceRequest r;
        r.model3mf = model;
        r.outDir = dir;
        r.name = pick.name;
        r.machine = a["printer_profile"].asString();
        r.process = a["process_profile"].asString();
        r.filament = a["filament_profile"].asString();
        r.bedType = a["bed_type"].asString();
        r.arrange = a["arrange"].asBool(false);   // the plate is already arranged (slice_view arrange_now)
        {
            // Not named: what the Slice tab has picked (the same thing the user would slice with).
            struct Picks { std::string m, p, f, b; };
            Picks tab;
            if (!onMain<Picks>(post, cancel, [this] { return Picks{slice_.machine, slice_.process, slice_.filament, slice_.bedType}; }, tab))
                return "Stopped.";
            if (r.machine.empty()) {
                r.machine = tab.m;
                if (r.process.empty()) r.process = tab.p;
                if (r.filament.empty()) r.filament = tab.f;
            }
            if (r.bedType.empty()) r.bedType = tab.b;
        }
        std::string changes;
        for (auto& [k, v] : a["settings"].members()) {
            r.overrides[k] = valueText(v);
            changes += (changes.empty() ? "" : ", ") + print::normalizeSettingKey(k) + " = " + valueText(v);
        }
        post([this, n = pick.name] {
            jobs_->setStatus("Slicing " + n + " with OrcaSlicer");
            activity();
        });
        print::SliceResult res = print::slice(s, r, cancel.get());
        post([this] { jobs_->setStatus(""); });
        if (!res.ok) return "Slicing failed: " + res.error;
        {
            std::lock_guard lock(print_.m);
            print_.last = res;
            print_.lastName = pick.name;
            print_.lastChanges = changes;
        }
        post([this, res] { slice_.slicedRev = doc_.revision; showSliceResult(res); });   // the Slice tab shows it too
        std::ostringstream o;
        o << "Sliced \"" << pick.name << "\". Printer: " << res.machine << ". Process: " << res.process << ". Filament: " << res.filament;
        if (!res.bedType.empty()) o << ". Plate: " << res.bedType;
        o << ". " << (changes.empty() ? "No settings changed." : "Changed: " + changes + ".");
        o << " Print time about " << print::formatDuration(res.seconds) << ", " << std::round(res.grams * 10) / 10 << " g of filament";
        if (res.meters > 0) o << " (" << std::round(res.meters * 100) / 100 << " m)";
        o << ".";
        if (!res.warnings.empty()) {
            o << " OrcaSlicer warnings: ";
            for (size_t i = 0; i < res.warnings.size() && i < 4; ++i) o << (i ? "; " : "") << res.warnings[i];
            o << ".";
        }
        o << " To print it, call start_print (the user confirms with a Print button).";
        return o.str();
    }

    if (name == "start_print") {
        print::SliceResult last;
        std::string jobName, changes;
        {
            std::lock_guard lock(print_.m);
            last = print_.last;
            jobName = print_.lastName;
            changes = print_.lastChanges;
        }
        if (!last.ok) return "Nothing is sliced yet: call slice first.";
        print::PrinterConn c;
        if (!onMain<print::PrinterConn>(post, cancel, [this] { return printerConn(); }, c)) return "Stopped.";
        if (c.kind == print::PrinterKind::None) {
            // Nothing set up in Kestral, but OrcaSlicer is linked to exactly one Bambu Lab printer: use
            // it through OrcaSlicer, the way its own Print button would (no access code needed for that).
            print::OrcaSetup s = orcaSetupCached(false);
            if (s.knownPrinters.size() == 1) {
                auto k = s.knownPrinters[0];
                std::string code = print::orcaSavedAccessCode(s, k.serial);
                if (!onMain<print::PrinterConn>(post, cancel, [this, k, code] {
                        // LAN mode when the access code is known (no Bambu software needed), else through OrcaSlicer's plugin.
                        settings_.printerKind = print::kindId(code.empty() ? print::PrinterKind::BambuOrca : print::PrinterKind::BambuLan);
                        settings_.printerName = k.model.empty() ? k.name : k.model;
                        settings_.printerHost = k.ip;
                        settings_.printerSerial = k.serial;
                        if (!code.empty()) settings_.printerSecret = platform::protectSecret(code);
                        settings_.save(settingsPath_);
                        return printerConn();
                    }, c)) return "Stopped.";
            }
        }
        if (c.kind == print::PrinterKind::None)
            return "No printer is set up in Kestral. Call print_setup and help the user pick a connection with set_printer.";
        bool autoPrint = false;
        onMain<bool>(post, cancel, [this] { return settings_.aiAutoPrint; }, autoPrint);
        if (!autoPrint) {
        // Ask the person. Nothing goes to the printer without their click (unless they allowed it in Preferences).
        std::ostringstream t;
        t << jobName << "\n" << (c.name.empty() ? last.machine : c.name) << "  |  " << last.filament << "\n"
          << "About " << print::formatDuration(last.seconds) << ", " << std::round(last.grams * 10) / 10 << " g";
        if (!changes.empty()) t << "\nChanged: " << changes;
        auto answer = std::make_shared<std::promise<bool>>();
        auto fut = answer->get_future();
        post([this, answer, text = t.str()] {
            print_.askOpen = true;
            print_.askTitle = "Start this print?";
            print_.askYes = "Print";
            print_.askText = text;
            print_.askAnswer = answer;
            activity();
        });
        bool yes = false, got = false;
        auto t0 = std::chrono::steady_clock::now();
        while (!cancel->load() && std::chrono::steady_clock::now() - t0 < std::chrono::minutes(15)) {
            if (fut.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) { yes = fut.get(); got = true; break; }
        }
        if (!got) {
            post([this] { print_.askOpen = false; print_.askAnswer.reset(); });
            return "No answer from the user, nothing was sent.";
        }
        if (!yes) return "The user pressed Cancel. Nothing was sent to the printer.";
        }
        post([this] { jobs_->setStatus("Sending the print"); activity(); });
        print::SendResult sr = print::sendPrint(c, last.gcode3mf, jobName, cancel.get());   // (Bambu via OrcaSlicer: below)
        post([this] { jobs_->setStatus(""); });
        if (!sr.ok) return "Could not start the print: " + sr.message;
        std::string out = sr.message;
        if (c.kind == print::PrinterKind::BambuOrca && sr.needsClick) {
            post([this, f = last.gcode3mf] { openInOrca(f); });
            out = "Bambu's network plugin is not on this computer, so the print opened in OrcaSlicer's own window: the user "
                  "presses Print plate there.";
        }
        // Watch it start when the printer can be asked.
        bool canAsk = !c.host.empty() && !c.secret.empty() && (c.kind != print::PrinterKind::BambuLan || !c.serial.empty());
        if (canAsk && (sr.needsClick || c.kind == print::PrinterKind::BambuLan || c.kind == print::PrinterKind::BambuOrca)) {
            int waitSec = sr.needsClick ? 240 : 90;
            auto w0 = std::chrono::steady_clock::now();
            while (!cancel->load() && std::chrono::steady_clock::now() - w0 < std::chrono::seconds(waitSec)) {
                print::PrintStatus st = print::printerStatus(c, 6000);
                if (st.ok && (st.state == "printing" || st.state == "preparing")) {
                    out += " Confirmed by the printer: " + statusLine(st);
                    break;
                }
                for (int i = 0; i < 25 && !cancel->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        return out;
    }

    if (name == "slice_view") {
        std::string out;
        if (!onMain<std::string>(post, cancel, [this, a] {
                if (a["tab"].asString() == "slice") enterSliceWorkspace();
                if (a["tab"].asString() == "design") leaveSliceWorkspace();
                bool reload = false;
                auto pick = [&](const char* key, std::string& field) {
                    std::string v = a[key].asString();
                    if (v.empty()) return;
                    field = v;
                    reload = true;
                };
                pick("printer_profile", slice_.machine);
                pick("process_profile", slice_.process);
                pick("filament_profile", slice_.filament);
                if (!a["bed_type"].asString().empty()) slice_.bedType = a["bed_type"].asString();
                for (auto& [k, v] : a["settings"].members()) {
                    std::string key = print::normalizeSettingKey(k), val = valueText(v);
                    if (val.empty()) slice_.overrides.erase(key);
                    else sliceSetValue(key, val);
                }
                if (a["auto_orient"].isBool()) slice_.orient = a["auto_orient"].asBool();
                plateSync();
                if (a["build_plates"].isNumber()) {
                    slice_.plates = std::clamp((int)a["build_plates"].asNumber(), 1, 36);
                    for (auto& [id, o] : slice_.plate)
                        for (auto& pl : o.plateOf) pl = std::min(pl, slice_.plates - 1);
                    slice_.framed = false;
                }
                if (a["active_plate"].isNumber()) slice_.activePlate = std::clamp((int)a["active_plate"].asNumber() - 1, 0, slice_.plates - 1);
                for (auto& item : a["plate"].items()) {
                    std::string nm = toLower(item["body"].asString());
                    uint32_t bid = 0;
                    for (auto& [id, o] : slice_.plate)
                        if (toLower(o.name) == nm || std::to_string(id) == nm) bid = id;
                    if (!bid) continue;
                    auto& o = slice_.plate[bid];
                    if (item["on_plate"].isBool()) o.onPlate = item["on_plate"].asBool();
                    if (item["copies"].isNumber()) {
                        int n = std::clamp((int)item["copies"].asNumber(), 1, 64);
                        while ((int)o.pos.size() < n) o.pos.push_back(o.pos.empty() ? glm::vec2(plateBedRect().center()) : o.pos.back() + glm::vec2(10.f));
                        while ((int)o.pos.size() > n) o.pos.pop_back();
                        slice_.plateSel = bid;
                    }
                    if (item["rotate"].isNumber()) { o.rotZ = (float)item["rotate"].asNumber(); o.cacheValid = false; }
                    if (item["scale"].isNumber() && item["scale"].asNumber() > 1) { o.scale = glm::vec3((float)item["scale"].asNumber() / 100.f); o.cacheValid = false; }
                    if (item["lay_flat"].asBool()) plateLayFlat(bid, -1);
                    if (item["x"].isNumber() && !o.pos.empty()) o.pos[0].x = (float)item["x"].asNumber();
                    if (item["y"].isNumber() && !o.pos.empty()) o.pos[0].y = (float)item["y"].asNumber();
                    if (item["build_plate"].isNumber()) {
                        int pl = std::clamp((int)item["build_plate"].asNumber() - 1, 0, slice_.plates - 1);
                        o.plateOf.assign(o.pos.size(), pl);
                        if (!item["x"].isNumber()) plateArrangeOne(pl);
                    }
                    if (item["copies"].isNumber() && !item["x"].isNumber()) plateArrange();
                }
                if (a["arrange_now"].asBool()) plateArrange();
                if (reload) sliceLoadSettings();
                if (a["view"].asString() == "preview") setSliceTop(2);
                if (a["view"].asString() == "design") setSliceTop(1);
                int n = (int)slice_.meshes.size();
                if (a["top_layer"].isNumber() && n) slice_.layerHi = std::clamp((int)a["top_layer"].asNumber() - 1, 0, n - 1);
                if (a["bottom_layer"].isNumber() && n) slice_.layerLo = std::clamp((int)a["bottom_layer"].asNumber() - 1, 0, n - 1);
                if (slice_.layerLo > slice_.layerHi) std::swap(slice_.layerLo, slice_.layerHi);
                for (auto& [k, v] : a["features"].members())
                    for (int f = 0; f < print::kFeatureCount; ++f)
                        if (toLower(print::featureName(f)) == toLower(k)) slice_.show[f] = v.asBool(true);
                sceneDirty_ = true;
                activity();
                std::ostringstream o;
                o << "Slice tab " << (sliceWs_ ? "open" : "closed") << ". Printer: " << (slice_.machine.empty() ? "none" : slice_.machine)
                  << ". Process: " << slice_.process << ". Filament: " << slice_.filament << ". Plate: " << slice_.bedType << ".";
                {
                    Aabb bed = plateBedRect();
                    o << " Plate " << std::lround(bed.max.x - bed.min.x) << " x " << std::lround(bed.max.y - bed.min.y) << " mm:";
                    for (auto& [id, po] : slice_.plate) {
                        const Aabb& cb = plateCore(id);
                        o << " " << po.name << " [plate " << (plateOfCopy(po, 0) + 1) << "]" << (po.onPlate ? "" : " (not on the plate)") << " size " << std::lround(cb.max.x - cb.min.x) << "x"
                          << std::lround(cb.max.y - cb.min.y) << "x" << std::lround(cb.max.z - cb.min.z) << " mm, rotate " << po.rotZ << ", scale "
                          << std::lround(po.scale.x * 100) << "%, at";
                        for (int k = 0; k < (int)po.pos.size(); ++k)
                            o << " (" << std::lround(po.pos[(size_t)k].x) << ", " << std::lround(po.pos[(size_t)k].y) << ")" << (plateInstanceOnBed(id, k) ? "" : " OFF THE BED");
                        o << ";";
                    }
                }
                if (!slice_.overrides.empty()) {
                    o << " Settings changed in the tab:";
                    for (auto& [k, v] : slice_.overrides) o << " " << k << "=" << v;
                    o << ".";
                }
                if (slice_.paths) {
                    auto& L = slice_.paths->layers;
                    o << " Sliced: " << L.size() << " layers. Showing " << (slice_.preview ? "the preview" : "the design");
                    if (slice_.preview && !L.empty())
                        o << ", layers " << slice_.layerLo + 1 << " to " << slice_.layerHi + 1 << " (z " << L[(size_t)slice_.layerLo].z << " to "
                          << L[(size_t)slice_.layerHi].z << " mm)";
                    double total = 0;
                    for (int f = 0; f < print::kFeatureCount; ++f) total += slice_.paths->length[f];
                    o << ". Features:";
                    for (int f = 0; f < print::kFeatureCount; ++f)
                        if (slice_.paths->used[f])
                            o << " " << print::featureName(f) << " " << std::lround(total > 0 ? slice_.paths->length[f] * 100 / total : 0) << "%"
                              << (slice_.show[f] ? "" : " (hidden)") << ";";
                    if (slice_.slicedRev != doc_.revision) o << " The design changed since this slice.";
                } else {
                    o << " Nothing sliced yet.";
                }
                return o.str();
            }, out))
            return "Stopped.";
        return out;
    }

    if (name == "find_slice_settings") {
        // Any OrcaSlicer setting by words: its key, name, value now, unit and choices.
        std::string out;
        if (!onMain<std::string>(post, cancel, [this, a] {
                std::string q = toLower(a["search"].asString());
                std::vector<std::string> words;
                for (size_t st = 0; st < q.size();) {
                    size_t e = q.find(' ', st);
                    std::string w = q.substr(st, e == std::string::npos ? std::string::npos : e - st);
                    if (!w.empty()) words.push_back(w);
                    if (e == std::string::npos) break;
                    st = e + 1;
                }
                std::ostringstream o;
                int n = 0;
                for (const char* tab : {"process", "filament", "printer"}) {
                    for (const print::OrcaLayoutRow* r : print::orcaLayout(tab)) {
                        const print::OrcaOptionDef* d = print::orcaOption(r->key);
                        if (!d) continue;
                        std::string hay = toLower(std::string(d->label) + " " + r->key + " " + r->group + " " + r->page + " " + d->tooltip);
                        bool all = true;
                        for (auto& w : words) all &= hay.find(w) != std::string::npos;
                        if (!all) continue;
                        if (++n > 40) break;
                        std::string v = sliceValue(r->key);
                        if (v.size() > 160) v = v.substr(0, 160) + "...";
                        o << r->key << " (" << tab << " > " << r->page << " > " << r->group << "): \"" << d->label << "\" = " << v << (*d->unit ? std::string(" ") + d->unit : "")
                          << (slice_.overrides.count(r->key) ? " (changed in the tab)" : "") << ", type " << d->type;
                        if (*d->values) {
                            std::string vals = d->values;
                            for (auto& c : vals) if (c == '\n') c = '|';
                            o << ", choices " << vals;
                        }
                        o << "\n";
                    }
                }
                if (!n) return std::string("No setting matches. Try fewer or other words.");
                return o.str() + "Change one with slice_view settings {key: value} (G-code settings take the whole text).";
            }, out))
            return "Stopped.";
        return out;
    }

    if (name == "slice_tab") {
        std::string out;
        if (!onMain<std::string>(post, cancel, [this, a] {
                std::ostringstream note;
                if (!sliceWs_) enterSliceWorkspace();
                auto partBy = [&](const std::string& nm) -> uint32_t {
                    std::string l = toLower(nm);
                    for (auto& [id, o] : slice_.plate) if (toLower(o.name) == l || std::to_string(id) == l) return id;
                    for (auto& [id, o] : slice_.plate) if (toLower(o.name).find(l) != std::string::npos) return id;
                    return 0;
                };
                auto parseHex = [](const std::string& h, glm::vec3& c) {
                    std::string x = h.size() && h[0] == '#' ? h.substr(1) : h;
                    if (x.size() < 6) return false;
                    unsigned v = (unsigned)std::strtoul(x.substr(0, 6).c_str(), nullptr, 16);
                    c = glm::vec3(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f);
                    return true;
                };
                std::string page = toLower(a["page"].asString());
                const char* pages[] = {"home", "prepare", "preview", "device", "project", "calibration"};
                for (int i = 0; i < 6; ++i) if (page == pages[i]) setSliceTop(i);
                sliceFilsEnsure();
                // Filaments
                if (a["sync_from_ams"].asBool()) { slice_.syncAmsFils = true; devicePoll(true); note << "Reading the AMS. "; }
                for (auto& f : a["filaments"].items()) {
                    int num = f["number"].isNumber() ? (int)f["number"].asNumber() : 0;
                    int i = num - 1;
                    if (i < 0 || i >= (int)slice_.ex.fils.size()) i = sliceAddFil(slice_.filament, slicePaletteColor(sliceNewFilColor()), false);
                    if (i < 0) { note << "16 filaments at most. "; continue; }
                    std::string prof = f["profile"].asString();
                    if (!prof.empty() && slice_.setup) {
                        const print::Preset* p = print::findPreset(*slice_.setup, "filament", prof);
                        if (!p) { auto found = print::searchPresets(*slice_.setup, "filament", prof + " " + slice_.machine, 1); if (!found.empty()) p = found[0]; }
                        if (!p) note << "No filament profile like \"" << prof << "\". ";
                        else if (i == 0) { slice_.filament = p->name; sliceLoadSettings(); }
                        else slice_.ex.fils[(size_t)i].preset = p->name;
                    }
                    glm::vec3 c;
                    if (parseHex(f["color"].asString(), c)) {
                        slice_.ex.fils[(size_t)i].color = c;
                        slice_.ex.fils[(size_t)i].autoMade = false;
                        if (i == 0) slice_.fil1ColorSet = true;
                    }
                }
                {
                    std::vector<int> rm;
                    for (auto& v : a["remove_filaments"].items()) rm.push_back((int)v.asNumber() - 1);
                    std::sort(rm.rbegin(), rm.rend());
                    for (int i : rm) sliceRemoveFil(i);
                }
                if (a["flushing"].isObject()) {
                    const Value& fl = a["flushing"];
                    if (fl["multiplier"].isNumber()) slice_.ex.flushMul = (float)fl["multiplier"].asNumber();
                    size_t n = slice_.ex.fils.size();
                    if (fl["matrix"].isArray() && fl["matrix"].size() == n) {
                        for (size_t r = 0; r < n; ++r)
                            for (size_t c2 = 0; c2 < n && c2 < fl["matrix"][r].size(); ++c2) slice_.ex.flush[r * n + c2] = (float)fl["matrix"][r][c2].asNumber();
                        slice_.ex.flushAuto = false;
                    }
                    if (fl["auto"].asBool()) { slice_.ex.flushAuto = true; sliceFlushDefaults(); }
                }
                // Plates
                if (a["add_plate"].asBool()) plateNewPlate();
                for (auto& pl : a["plates"].items()) {
                    int p = (int)pl["number"].asNumber(0) - 1;
                    if (p < 0 || p >= slice_.plates) continue;
                    slice_.ex.plateNames.resize((size_t)slice_.plates);
                    slice_.ex.plateLocked.resize((size_t)slice_.plates, 0);
                    slice_.ex.plateBeds.resize((size_t)slice_.plates);
                    if (pl["name"].isString()) slice_.ex.plateNames[(size_t)p] = pl["name"].asString();
                    if (pl["plate_type"].isString()) slice_.ex.plateBeds[(size_t)p] = pl["plate_type"].asString();
                    if (pl["locked"].isBool()) slice_.ex.plateLocked[(size_t)p] = pl["locked"].asBool() ? 1 : 0;
                    if (pl["auto_orient"].asBool()) plateOrientPlate(p);
                    if (pl["arrange"].asBool()) plateArrangeOne(p);
                    if (pl["to_front"].asBool()) plateToFront(p);
                    if (pl["delete"].asBool()) plateDeletePlate(p);
                }
                if (!a["add_model_file"].asString().empty()) note << "Adding a model from a file needs the user to pick it: ask them to press Add on the plate. ";
                // Parts
                for (auto& pt : a["parts"].items()) {
                    uint32_t id = partBy(pt["name"].asString());
                    if (!id) { note << "No part \"" << pt["name"].asString() << "\". "; continue; }
                    auto& o = slice_.plate[id];
                    const Value& fv = pt["filament"];
                    if (fv.isString() && toLower(fv.asString()) == "auto") { o.autoFil = true; o.useColor = true; }
                    else if (fv.isNumber()) { o.autoFil = false; o.fil = std::clamp((int)fv.asNumber() - 1, 0, (int)slice_.ex.fils.size() - 1); }
                    for (auto& [k, v] : pt["settings"].members()) {
                        std::string key = print::normalizeSettingKey(k), val = valueText(v);
                        if (val.empty()) o.settings.erase(key);
                        else o.settings[key] = val;
                    }
                    if (pt["auto_orient"].asBool()) plateAutoOrient(id);
                    if (pt["select"].asBool()) plateSelect(id, 0, true);
                    std::string sp = toLower(pt["split"].asString());
                    if (sp == "objects" || sp == "parts") plateSplit(id, sp == "parts");
                    if (pt["cut_height"].isNumber() && slice_.plate.count(id)) {
                        plateSelect(id, 0, false);
                        slice_.cutZ = (float)pt["cut_height"].asNumber();
                        std::string keep = toLower(pt["cut_keep"].asString());
                        slice_.cutKeep = keep == "upper" ? 1 : keep == "lower" ? 2 : 0;
                        plateCutSelected();
                    }
                    // Painting by face number (get_body lists faces): {"paint":{"type":"color|support|seam","faces":[...],"value":2}}
                    if (pt["paint"].isObject() && slice_.plate.count(id)) {
                        auto& po = slice_.plate[id];
                        std::string ty = toLower(pt["paint"]["type"].asString());
                        auto& m = ty == "support" ? po.paintSupport : ty == "seam" ? po.paintSeam : po.paintColor;
                        int v = (int)pt["paint"]["value"].asNumber(1);
                        if (pt["paint"]["clear"].asBool()) m.clear();
                        for (auto& f : pt["paint"]["faces"].items()) {
                            uint32_t fi = (uint32_t)f.asNumber();
                            if (v <= 0) m.erase(fi);
                            else m[fi] = v;
                        }
                    }
                }
                if (a["auto_orient_all"].asBool()) for (int p = 0; p < slice_.plates; ++p) plateOrientPlate(p);
                sliceAutoFils();
                // Preview colors
                std::string vt = toLower(a["view_type"].asString());
                const char* vts[] = {"line_type", "filament", "speed", "layer_height", "line_width", "fan", "temperature", "flow"};
                for (int i = 0; i < 8; ++i) if (vt == vts[i]) slice_.viewType = i;
                // Project
                if (a["project"].isObject()) {
                    const Value& pj = a["project"];
                    if (pj["name"].isString()) slice_.ex.projName = pj["name"].asString();
                    if (pj["author"].isString()) slice_.ex.projAuthor = pj["author"].asString();
                    if (pj["license"].isString()) slice_.ex.projLicense = pj["license"].asString();
                    if (pj["description"].isString()) slice_.ex.projDesc = pj["description"].asString();
                }
                // AMS slots for the filaments of the last slice ("A1-2" style, "Ext", or tray numbers).
                if (a["ams_slots"].isArray()) {
                    sliceAutoAmsMap();
                    size_t i = 0;
                    for (auto& v : a["ams_slots"].items()) {
                        if (i >= slice_.ex.amsMap.size()) break;
                        int tid = -1;
                        if (v.isNumber()) tid = (int)v.asNumber();
                        else {
                            std::string t = toLower(v.asString());
                            int u = 0, sl2 = 0;
                            if (t == "ext" || t == "external") tid = 254;
                            else if (std::sscanf(t.c_str(), "a%d-%d", &u, &sl2) == 2) tid = (u - 1) * 4 + (sl2 - 1);
                            else if (std::sscanf(t.c_str(), "%d", &sl2) == 1) tid = sl2 - 1;
                        }
                        slice_.ex.amsMap[i++] = tid;
                    }
                }
                if (!a["save_process_preset"].asString().empty()) {
                    std::string e = sliceSaveProcessPreset(a["save_process_preset"].asString());
                    note << (e.empty() ? "Saved the process preset. " : e + ". ");
                }
                std::string cal = toLower(a["calibration"].asString());
                if (cal == "flow_coarse") sliceStartCalibration(1, 1);
                else if (cal == "flow_fine") sliceStartCalibration(1, 2);
                else if (cal == "temperature") sliceStartCalibration(2, 1);
                else if (cal == "pressure_advance") sliceStartCalibration(3, 1);
                std::string sl3 = toLower(a["slice"].asString());
                if (sl3 == "plate") startSliceJob();
                else if (sl3 == "all") sliceAllPlates();
                sceneDirty_ = true;
                activity();
                // What the tab has now.
                std::ostringstream o;
                o << note.str();
                const char* tops[] = {"Home", "Prepare", "Preview", "Device", "Project", "Calibration"};
                o << "Slice tab page: " << tops[sliceCurTop()] << ". Filaments:";
                for (int i = 0; i < (int)slice_.ex.fils.size(); ++i) {
                    glm::vec3 c = sliceFilColor(i);
                    char hx[16];
                    std::snprintf(hx, sizeof hx, "#%02X%02X%02X", (int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255));
                    o << " " << i + 1 << " " << sliceFilPreset(i) << " " << hx << ";";
                }
                o << " Plates: " << slice_.plates << " (Slice plate slices " << slice_.activePlate + 1 << ").";
                for (int p = 0; p < slice_.plates; ++p) {
                    o << " Plate " << p + 1 << " \"" << plateName(p) << "\"" << (p < (int)slice_.ex.plateLocked.size() && slice_.ex.plateLocked[(size_t)p] ? " locked" : "");
                    if (p < (int)slice_.ex.plateResults.size() && slice_.ex.plateResults[(size_t)p].ok)
                        o << " sliced " << print::formatDuration(slice_.ex.plateResults[(size_t)p].seconds) << " " << std::lround(slice_.ex.plateResults[(size_t)p].grams) << " g";
                    o << ";";
                }
                o << " Parts:";
                for (auto& [id, po] : slice_.plate) {
                    o << " " << po.name << " (filament " << po.fil + 1 << (po.autoFil ? " auto" : "") << ", plate " << plateOfCopy(po, 0) + 1 << (po.onPlate ? "" : ", off the plate");
                    for (auto& [k, v] : po.settings) o << ", " << k << "=" << v;
                    if (!po.paintColor.empty()) o << ", " << po.paintColor.size() << " faces color painted";
                    if (!po.paintSupport.empty()) o << ", " << po.paintSupport.size() << " faces support painted";
                    if (!po.paintSeam.empty()) o << ", " << po.paintSeam.size() << " faces seam painted";
                    o << ");";
                }
                if (!slice_.ex.amsMap.empty()) {
                    o << " AMS slots for the sliced filaments:";
                    for (size_t i = 0; i < slice_.ex.amsMap.size(); ++i) {
                        int m = slice_.ex.amsMap[i];
                        o << " " << i + 1 << "->" << (m == 254 ? std::string("Ext") : m < 0 ? std::string("none") : "A" + std::to_string(m / 4 + 1) + "-" + std::to_string(m % 4 + 1));
                    }
                    o << ".";
                }
                const auto& st = slice_.devStatus;
                if (st.ok && (!st.trays.empty() || st.hasExt)) {
                    o << " AMS now:";
                    for (auto& t : st.trays)
                        o << " A" << t.ams + 1 << "-" << t.slot + 1 << " " << (t.loaded ? (t.name.empty() ? t.type : t.name) + " " + t.color : std::string("empty")) << ";";
                    if (st.hasExt) o << " Ext " << (st.ext.loaded ? st.ext.type + " " + st.ext.color : std::string("empty")) << ";";
                }
                return o.str();
            }, out))
            return "Stopped.";
        return out;
    }

    if (name == "printer_device") {
        // The Device tab's controls. Only when the user asks for it (it moves a real machine).
        std::string out;
        if (!onMain<std::string>(post, cancel, [this, a] {
                std::ostringstream o;
                auto num = [&](const char* k) { return a[k].isNumber(); };
                if (num("nozzle_temp")) { deviceSend("M104 S" + std::to_string((int)std::clamp(a["nozzle_temp"].asNumber(), 0.0, 320.0)), false, "Nozzle temperature set"); o << "Nozzle target set. "; }
                if (num("bed_temp")) { deviceSend("M140 S" + std::to_string((int)std::clamp(a["bed_temp"].asNumber(), 0.0, 120.0)), false, "Bed temperature set"); o << "Bed target set. "; }
                auto fan = [&](const char* k, int P) {
                    if (!num(k)) return;
                    int v = (int)std::lround(std::clamp(a[k].asNumber(), 0.0, 100.0) * 2.55);
                    deviceSend("M106 P" + std::to_string(P) + " S" + std::to_string(v), false, std::string(k) + " set");
                    o << k << " set. ";
                };
                fan("fan_part", 1);
                fan("fan_aux", 2);
                fan("fan_chamber", 3);
                if (a["light"].isBool()) { deviceLight(a["light"].asBool()); o << "Lamp " << (a["light"].asBool() ? "on" : "off") << ". "; }
                std::string sp = toLower(a["speed"].asString());
                const char* lv[] = {"", "silent", "standard", "sport", "ludicrous"};
                for (int i = 1; i <= 4; ++i) if (sp == lv[i]) { deviceSpeed(i); o << "Speed " << lv[i] << ". "; }
                if (a["home"].asBool()) { deviceSend("G28", false, "Homing"); o << "Homing. "; }
                if (a["move"].isObject()) {
                    std::string mv;
                    for (const char* ax : {"x", "y", "z"})
                        if (a["move"][ax].isNumber()) {
                            char b[32];
                            std::snprintf(b, sizeof b, " %c%.2f", (char)std::toupper((unsigned char)ax[0]), a["move"][ax].asNumber());
                            mv += b;
                        }
                    if (!mv.empty()) { deviceSend("G91\nG0" + mv + " F3000\nG90", false, "Moved"); o << "Moved" << mv << ". "; }
                }
                if (num("extrude_mm")) {
                    if (slice_.devStatus.nozzleC < 170) o << "Not extruding: the nozzle is cold (heat it first). ";
                    else { deviceSend("M83\nG0 E" + std::to_string(a["extrude_mm"].asNumber()) + " F300", false, "Extruded"); o << "Extruding. "; }
                }
                if (a["unload"].asBool()) {
                    deviceSend(R"({"print":{"sequence_id":"0","command":"ams_change_filament","target":255,"curr_temp":220,"tar_temp":220}})", true, "Unloading");
                    o << "Unloading the filament. ";
                }
                if (!a["gcode"].asString().empty()) { deviceSend(a["gcode"].asString(), false, "G-code sent"); o << "G-code sent. "; }
                if (a["camera"].isBool()) { camOn_ = a["camera"].asBool(); if (!camOn_) camera_.stop(); o << "Camera " << (camOn_ ? "on" : "off") << ". "; }
                if (a["show"].asBool()) setSliceTop(3);
                devicePoll(true);
                const auto& st = slice_.devStatus;
                if (st.ok) {
                    o << "Last status: " << st.state << ", nozzle " << std::lround(st.nozzleC) << "/" << std::lround(st.nozzleTarget) << " C, bed " << std::lround(st.bedC) << "/"
                      << std::lround(st.bedTarget) << " C";
                    if (st.partFan >= 0) o << ", part fan " << st.partFan << "%";
                    if (st.lightKnown) o << ", lamp " << (st.light ? "on" : "off");
                    if (st.layer >= 0) o << ", layer " << st.layer << "/" << st.totalLayers;
                    o << ".";
                } else {
                    o << (st.error.empty() ? "No status from the printer yet." : st.error);
                }
                return o.str();
            }, out))
            return "Stopped.";
        return out;
    }

    if (name == "bambu_printers") {
        print::BambuCloudStatus st = print::bambuCloudStatus(cancel.get());
        post([this, st] { bambu_.status = st; bambu_.checked = true; activity(); });   // Preferences shows the same
        if (!st.pluginFound) return st.error;
        std::ostringstream o;
        if (!st.error.empty()) o << st.error << " ";
        {
            print::PrinterConn pc;
            onMain<print::PrinterConn>(post, cancel, [this] { return printerConn(); }, pc);
            if (!st.loggedIn) o << (pc.host.empty() || pc.secret.empty() ? "Kestral does NOT have the printer's IP and access code yet: use set_printer "
                                                                           "with from_orcaslicer_printer to fill them in. "
                                                                         : "Kestral has the printer's IP and access code, so start_print will send over the home network. ");
        }
        if (st.loggedIn) o << "Signed in to Bambu as " << st.user << ". ";
        o << "Plugin " << st.pluginVersion << ". Printers:";
        for (auto& d : st.devices) o << " " << d.name << " (id " << d.id << ", model " << d.model << ", " << (d.online ? "online" : "offline") << ");";
        if (st.devices.empty()) o << " none.";
        if (!st.detail.empty()) o << " Details: " << st.detail;
        return o.str();
    }

    if (name == "slicer_engine") {
        std::string action = toLower(a["action"].asString("status"));
        auto status = [&] {
            std::string out;
            onMain<std::string>(post, cancel, [this] {
                std::string o = engineStatusText() + ".";
                if (!slice_.pending.empty()) o += " OrcaSlicer " + slice_.pending + " is installed and waits for a restart of Kestral.";
                if (slice_.installing) o += " Installing now.";
                if (slice_.checked) {
                    if (!slice_.latest.error.empty()) o += " Last check failed: " + slice_.latest.error;
                    else o += " Newest release: OrcaSlicer " + slice_.latest.version + ".";
                }
                return o;
            }, out);
            return out;
        };
        if (action == "status") return status();
        if (action == "check" || action == "install") {
            print::EngineRelease r = print::latestEngineRelease(cancel.get());
            if (!r.error.empty()) return "Could not check: " + r.error;
            std::string cur;
            onMain<std::string>(post, cancel, [this, r] {
                slice_.latest = r;
                slice_.checked = true;
                activity();
                return slice_.engineVersion;
            }, cur);
            bool newer = cur.empty() || print::compareVersions(r.version, cur) > 0;
            if (action == "check")
                return "Newest OrcaSlicer: " + r.version + (cur.empty() ? "" : ", in use: " + cur) + (newer ? ". An update is available." : ". Up to date.");
            if (!newer) return "Already up to date (OrcaSlicer " + cur + ").";
            // Downloading software: the user says yes first.
            auto answer = std::make_shared<std::promise<bool>>();
            auto fut = answer->get_future();
            post([this, answer, r] {
                print_.askOpen = true;
                print_.askTitle = "Install the slicer engine?";
                print_.askYes = "Install";
                print_.askText = "OrcaSlicer " + r.version + " (free, open source) from OrcaSlicer's releases on GitHub, about " +
                                 std::to_string(r.size / 1000000) + " MB. Kestral runs it hidden to slice.";
                print_.askAnswer = answer;
                activity();
            });
            bool yes = false, got = false;
            auto t0 = std::chrono::steady_clock::now();
            while (!cancel->load() && std::chrono::steady_clock::now() - t0 < std::chrono::minutes(15))
                if (fut.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) { yes = fut.get(); got = true; break; }
            if (!got) { post([this] { print_.askOpen = false; print_.askAnswer.reset(); }); return "No answer from the user, nothing installed."; }
            if (!yes) return "The user said no. Nothing installed.";
            post([this] { engineCheck(true); });
            return "Installing OrcaSlicer " + r.version + " in the background (the Slice tab shows progress). When it is done the user "
                   "restarts Kestral (slicer_engine restart) unless no engine was installed before, then it is used right away.";
        }
        if (action == "restart") {
            std::string pend;
            onMain<std::string>(post, cancel, [this] { return slice_.pending; }, pend);
            if (pend.empty()) return "No installed update is waiting.";
            post([this] { restartKestral(); });
            return "Restarting Kestral (it asks to save first when there are unsaved changes).";
        }
        return "Unknown action " + action + ".";
    }

    if (name == "printer_control") {
        std::string action = toLower(a["action"].asString());
        if (action == "cancel") action = "stop";
        print::PrinterConn c;
        if (!onMain<print::PrinterConn>(post, cancel, [this] { return printerConn(); }, c)) return "Stopped.";
        if (c.kind == print::PrinterKind::None) return "No printer is set up in Kestral yet.";
        if (action == "stop") {
            bool autoOk = false;
            onMain<bool>(post, cancel, [this] { return settings_.aiAutoPrint; }, autoOk);
            if (!autoOk) {
                auto answer = std::make_shared<std::promise<bool>>();
                auto fut = answer->get_future();
                post([this, answer] {
                    print_.askOpen = true;
                    print_.askTitle = "Stop the print?";
                    print_.askText = "The assistant wants to cancel the print on the printer. It cannot be continued afterwards.";
                    print_.askYes = "Stop the print";
                    print_.askAnswer = answer;
                    activity();
                });
                bool yes = false, got = false;
                auto t0 = std::chrono::steady_clock::now();
                while (!cancel->load() && std::chrono::steady_clock::now() - t0 < std::chrono::minutes(10))
                    if (fut.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) { yes = fut.get(); got = true; break; }
                if (!got) { post([this] { print_.askOpen = false; print_.askAnswer.reset(); }); return "No answer from the user, the print keeps going."; }
                if (!yes) return "The user said no. The print keeps going.";
            }
        }
        print::SendResult r = print::printerControl(c, action, cancel.get());
        if (!r.ok) return "Could not " + action + " the print: " + r.message;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        print::PrintStatus st = print::printerStatus(c, 8000);
        post([this, st] { slice_.devStatus = st; activity(); });
        return r.message + (st.ok ? " The printer now says: " + st.state + "." : "");
    }

    if (name == "printer_status") {
        print::PrinterConn c;
        if (!onMain<print::PrinterConn>(post, cancel, [this] { return printerConn(); }, c)) return "Stopped.";
        if (c.kind == print::PrinterKind::None) return "No printer is set up in Kestral yet.";
        return statusLine(print::printerStatus(c, 8000));
    }

    if (name == "set_printer") {
        print::PrinterKind kind = print::kindFromString(a["connection"].asString());
        if (kind == print::PrinterKind::None)
            return "Unknown connection. Use one of: bambu_orca, bambu_connect, bambu_lan, octoprint, moonraker, prusalink.";
        print::OrcaSetup s = orcaSetupCached(false);
        std::string fromOrca = a["from_orcaslicer_printer"].asString();
        std::string host = a["host"].asString(), serial = a["serial"].asString(), secret = a["access_code"].asString(a["api_key"].asString());
        std::string pname = a["name"].asString();
        if (!fromOrca.empty()) {
            const print::OrcaSetup::KnownPrinter* k = nullptr;
            for (auto& kp : s.knownPrinters)
                if (kp.serial == fromOrca || toLower(kp.name) == toLower(fromOrca) || toLower(kp.model) == toLower(fromOrca)) k = &kp;
            if (!k && s.knownPrinters.size() == 1) k = &s.knownPrinters[0];
            if (!k) return "OrcaSlicer does not know a printer called \"" + fromOrca + "\".";
            if (host.empty()) host = k->ip;
            if (serial.empty()) serial = k->serial;
            if (secret.empty()) secret = print::orcaSavedAccessCode(s, k->serial);
            if (pname.empty()) pname = k->model;
        }
        bool useAms = a["use_ams"].asBool(false);
        int slot = std::max(0, (int)a["ams_slot"].asNumber(1) - 1);
        print::PrinterConn c;
        bool hasHost = a.has("host"), hasSerial = a.has("serial");
        bool okMain = onMain<print::PrinterConn>(post, cancel, [this, kind, pname, host, serial, secret, useAms, slot, hasHost, hasSerial] {
            settings_.printerKind = print::kindId(kind);
            if (!pname.empty()) settings_.printerName = pname;
            if (!host.empty() || hasHost) settings_.printerHost = host;
            if (!serial.empty() || hasSerial) settings_.printerSerial = serial;
            if (!secret.empty()) settings_.printerSecret = platform::protectSecret(secret);
            settings_.printerUseAms = useAms;
            settings_.printerAmsSlot = slot;
            settings_.save(settingsPath_);
            return printerConn();
        }, c);
        if (!okMain) return "Stopped.";
        return "Saved: " + std::string(print::kindName(kind)) + (c.name.empty() ? "" : " (" + c.name + ")") + ". " + print::testConnection(c);
    }
    return "Unknown tool " + name;
}

// ===================================================================== confirmation card

void App::drawPrintConfirm() {
    if (!print_.askOpen) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420 * S, 0));
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##printask", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted(print_.askTitle.c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", print_.askText.c_str());
    ImGui::Spacing();
    auto answer = [&](bool yes) {
        if (print_.askAnswer) print_.askAnswer->set_value(yes);
        print_.askAnswer.reset();
        print_.askOpen = false;
    };
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 0.42f, 0.21f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 0.52f, 0.3f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (print_.askYes == "Print") {
        if (ImGui::Checkbox("Let the assistant start prints without asking", &settings_.aiAutoPrint)) settings_.save(settingsPath_);
        ImGui::Spacing();
    }
    bool go = ImGui::Button(print_.askYes.c_str(), ImVec2(130 * S, 0));
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    bool no = ImGui::Button("Cancel", ImVec2(110 * S, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (go) answer(true);
    else if (no) answer(false);
    ImGui::End();
}

// ===================================================================== Preferences > 3D printer

void App::bambuCloudRefresh() {
    if (bambu_.loading) return;
    bambu_.loading = true;
    JobSystem* jobs = jobs_.get();
    jobs_->submit("bambu printers", [this, jobs] {
        print::BambuCloudStatus st = print::bambuCloudStatus(nullptr);
        jobs->post([this, st] {
            bambu_.loading = false;
            bambu_.checked = true;
            bambu_.status = st;
            activity();
        });
    }, [this](const std::string& e) {
        bambu_.loading = false;
        bambu_.status.error = e;
    });
}

void App::drawBambuCloudPicker() {
    if (bambu_.loading) {
        ImGui::TextDisabled("Asking Bambu's cloud...");
        activity();
        return;
    }
    if (ImGui::Button("Find my Bambu printers")) bambuCloudRefresh();
    if (!bambu_.checked) return;
    const auto& st = bambu_.status;
    if (!st.error.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::col::Error), "%s", st.error.c_str());
    if (st.loggedIn) ImGui::TextDisabled("Signed in to Bambu as %s (plugin %s)", st.user.c_str(), st.pluginVersion.c_str());
    for (auto& d : st.devices) {
        std::string label = d.name + (d.online ? "  (online)" : "  (offline)") + "##" + d.id;
        if (ImGui::Selectable(label.c_str(), settings_.printerSerial == d.id)) {
            settings_.printerSerial = d.id;
            settings_.printerName = d.name;
            settings_.save(settingsPath_);
        }
    }
    if (st.loggedIn && st.devices.empty()) ImGui::TextDisabled("No printers on this Bambu account.");
}

void App::drawPrinterPrefs() {
    const float S = uiScale_;
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted("3D printer (for the assistant's \"slice and print\")");
    ImGui::PopFont();
    flashItem("pref:Printers");
    if (ImGui::Checkbox("Let the assistant start prints without asking", &settings_.aiAutoPrint)) settings_.save(settingsPath_);
    ImGui::SetItemTooltip("For \"model it and print it\" while you are away. Off: the assistant shows a Print / Cancel card first.");
    // LAN mode first: it talks to the printer directly. The two Bambu software routes come last.
    static const print::PrinterKind kinds[] = {print::PrinterKind::None, print::PrinterKind::BambuLan, print::PrinterKind::OctoPrint,
                                               print::PrinterKind::Moonraker, print::PrinterKind::PrusaLink, print::PrinterKind::BambuConnect,
                                               print::PrinterKind::BambuOrca};
    print::PrinterKind cur = print::kindFromString(settings_.printerKind);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##pkind", print::kindName(cur))) {
        for (auto k : kinds)
            if (ImGui::Selectable(print::kindName(k), k == cur)) {
                settings_.printerKind = print::kindId(k);
                settings_.save(settingsPath_);
                print_.testMsg.clear();
            }
        ImGui::EndCombo();
    }
    cur = print::kindFromString(settings_.printerKind);
    auto dim = [&](const char* t) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", t);
        ImGui::PopStyleColor();
    };
    switch (cur) {
    case print::PrinterKind::BambuOrca:
        dim("Optional. Kestral sends the print with Bambu's own network plugin, the way OrcaSlicer does. The printer stays in its "
            "normal mode (Bambu Handy keeps working). Kestral does not include the plugin: it uses the copy and the Bambu login "
            "OrcaSlicer already has on this computer, under Bambu's own terms. LAN mode (the first choice) needs no plugin. "
            "Pick the printer with Find my Bambu printers. Add the IP and access code too (or use the OrcaSlicer button) so "
            "Kestral can also see the print's progress.");
        drawBambuCloudPicker();
        ImGui::Spacing();
        break;
    case print::PrinterKind::BambuConnect:
        dim("Kestral slices and hands the file to Bambu Connect (free app from Bambu Lab), where you press Print. Normal "
            "printer mode, Bambu Handy keeps working.");
        break;
    case print::PrinterKind::BambuLan:
        dim("Recommended. Kestral talks to the printer directly over your home network (no Bambu software involved) and starts "
            "the print itself after you press Print. Put the printer in LAN only mode with Developer mode on (printer screen: "
            "Settings > WLAN / Network); the access code is on the same screen. In LAN only mode Bambu Handy and the cloud do not work.");
        break;
    case print::PrinterKind::OctoPrint: dim("Address of OctoPrint (e.g. 192.168.1.50) and an API key (OctoPrint: Settings > Application Keys)."); break;
    case print::PrinterKind::Moonraker: dim("Address of the Klipper machine (Mainsail / Fluidd), e.g. 192.168.1.60. API key only if you set one."); break;
    case print::PrinterKind::PrusaLink: dim("Address of the printer and its PrusaLink API key (printer menu: Settings > Network > PrusaLink)."); break;
    default: dim("Pick how Kestral reaches your printer. The assistant can also set this up for you: just tell it your printer."); break;
    }
    if (cur == print::PrinterKind::None) return;
    bool changed = false;
    ImGui::SetNextItemWidth(200 * S);
    changed |= ImGui::InputTextWithHint("Name##pn", "e.g. A1 mini", &settings_.printerName);
    bool bambu = cur == print::PrinterKind::BambuOrca || cur == print::PrinterKind::BambuConnect || cur == print::PrinterKind::BambuLan;
    ImGui::SetNextItemWidth(200 * S);
    changed |= ImGui::InputTextWithHint("IP address##ph", "192.168.1.x", &settings_.printerHost);
    if (bambu) {
        ImGui::SetNextItemWidth(200 * S);
        changed |= ImGui::InputTextWithHint("Serial number##ps", "printer screen: Settings > Device", &settings_.printerSerial);
    }
    {
        static std::string secretEdit;
        ImGui::SetNextItemWidth(200 * S);
        std::string hint = settings_.printerSecret.empty() ? (bambu ? "access code" : "API key") : "saved (type to replace)";
        if (ImGui::InputTextWithHint(bambu ? "Access code##pc" : "API key##pc", hint.c_str(), &secretEdit, ImGuiInputTextFlags_Password)) {}
        if (!secretEdit.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Save##psec")) {
                settings_.printerSecret = platform::protectSecret(secretEdit);
                secretEdit.clear();
                changed = true;
            }
        }
    }
    if (bambu) {
        changed |= ImGui::Checkbox("Feed from the AMS", &settings_.printerUseAms);
        if (settings_.printerUseAms) {
            ImGui::SameLine();
            int slot = settings_.printerAmsSlot + 1;
            ImGui::SetNextItemWidth(80 * S);
            if (ImGui::InputInt("slot##pslot", &slot)) { settings_.printerAmsSlot = std::clamp(slot, 1, 16) - 1; changed = true; }
        }
        // Printers OrcaSlicer already knows: one click fills everything in.
        print::OrcaSetup s;
        {
            std::lock_guard lock(print_.m);
            s = print_.setup;
        }
        if (s.configDir.empty() && !print_.setupLoading) {
            print_.setupLoading = true;
            orcaPath();   // refreshes the program hint for the worker
            jobs_->submit("orca setup", [this] { orcaSetupCached(true); jobs_->post([this] { print_.setupLoading = false; }); });
        }
        for (auto& k : s.knownPrinters) {
            std::string label = "Use " + k.model + " (" + k.ip + ") from OrcaSlicer##" + k.serial;
            if (ImGui::Button(label.c_str())) {
                settings_.printerHost = k.ip;
                settings_.printerSerial = k.serial;
                if (settings_.printerName.empty()) settings_.printerName = k.model;
                std::string code = print::orcaSavedAccessCode(s, k.serial);
                if (!code.empty()) settings_.printerSecret = platform::protectSecret(code);
                changed = true;
            }
        }
    }
    if (changed) settings_.save(settingsPath_);
    if (ImGui::Button(print_.testing ? "Testing..." : "Test the connection") && !print_.testing) {
        print_.testing = true;
        print_.testMsg = "Asking the printer...";
        print::PrinterConn c = printerConn();
        jobs_->submit("printer test", [this, c] {
            std::string m = print::testConnection(c);
            jobs_->post([this, m] { print_.testing = false; print_.testMsg = m; });
        });
    }
    if (!print_.testMsg.empty()) {
        ImGui::SameLine();
        ImGui::TextWrapped("%s", print_.testMsg.c_str());
    }
}

} // namespace kst
