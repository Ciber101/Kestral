// Recovery: unsaved work is never only in memory.
//
// Every open design with unsaved work has its own recovery copy in <app data>/autosave:
//   <key>.kst   the design as it is now (structure only: the same text as a saved design)
//   <key>.info  line 1: the design file it belongs to ("" when never saved), line 2: its name
// The key starts with this Kestral's instance tag, and <tag>.lock is held while this Kestral runs, so a
// second Kestral never offers (or discards) the work of one that is still open.
//
// A copy is written a few seconds after each change, when another tab comes in front, before every
// close prompt and from the crash, hang and device lost handlers. It goes away only when that work is
// saved into its design file. Don't Save and Discard move it to autosave/discarded (the newest 40 are
// kept), never delete it. Whatever is left when Kestral starts is offered back, with the design file it
// belongs to, so Ctrl+S saves it as that design's next version.
#include "app/App.h"
#include "core/Log.h"
#include "core/SaveLoad.h"
#include "platform/Platform.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace kst {

namespace fs = std::filesystem;

namespace {

std::string readFileText(const std::string& path) {
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string stampNow(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[64];
    std::strftime(b, sizeof b, fmt, &tmv);
    return b;
}

std::time_t fileTime(const std::string& path) {
    std::error_code ec;
    auto t = fs::last_write_time(u8path(path), ec);
    if (ec) return 0;
    auto sys = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now());
    return std::chrono::system_clock::to_time_t(sys);
}

// The name a design file stores ("doc|<name>" near the top), without loading the whole design.
std::string storedName(const std::string& path) {
    std::ifstream f(u8path(path), std::ios::binary);
    std::string line;
    for (int i = 0; i < 8 && std::getline(f, line); ++i) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("doc|", 0) == 0) return line.substr(4);
    }
    return {};
}

} // namespace

bool App::hasWork(const Document& d) {
    return !d.timeline.empty() || !d.bodies.empty() || !d.sketches.empty() || !d.params.empty();
}

bool App::writeRecoveryFiles(const std::string& dir, const std::string& key, const std::string& data,
                             const std::string& original, const std::string& name) {
    if (key.empty()) return false;
    std::error_code ec;
    fs::create_directories(u8path(dir), ec);
    if (!writeFileAtomic(dir + "/" + key + ".kst", data)) return false;
    writeFileAtomic(dir + "/" + key + ".info", original + "\n" + name + "\n");
    return true;
}

std::string App::recoveryDir() const { return platform::appDataDir() + "/autosave"; }

bool App::inRecoveryDir(const std::string& path) const {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(u8path(path), ec);
    if (ec) p = u8path(path);
    fs::path d = fs::weakly_canonical(u8path(recoveryDir()), ec);
    if (ec) d = u8path(recoveryDir());
    // Anything in the recovery folder or below it (the discarded folder too).
    std::string ps = pathToU8(p.parent_path()), ds = pathToU8(d);
    for (auto* s : {&ps, &ds}) {
        for (auto& c : *s) {
            if (c == '\\') c = '/';
#ifdef _WIN32
            c = (char)std::tolower((unsigned char)c);
#endif
        }
        while (!s->empty() && s->back() == '/') s->pop_back();
    }
    return !ds.empty() && (ps == ds || (ps.size() > ds.size() && ps.compare(0, ds.size(), ds) == 0 && ps[ds.size()] == '/'));
}

void App::syncDirty() {
    if (doc_.revision != cleanRev_) dirty_ = true;
}

void App::markClean() {
    dirty_ = false;
    cleanRev_ = doc_.revision;
}

bool App::tabUnsaved(int i) {
    if (i == designActive_) {
        syncDirty();
        return dirty_ && hasWork(doc_);
    }
    if (i < 0 || i >= (int)designTabs_.size()) return false;
    DesignTab& t = designTabs_[(size_t)i];
    if (t.doc.revision != t.cleanRev) t.dirty = true;
    return t.dirty && hasWork(t.doc);
}

std::string App::newRecoveryKey() {
    if (instanceTag_.empty()) {
        std::random_device rd;
        char b[16];
        std::snprintf(b, sizeof b, "%04x", (unsigned)(rd() & 0xffff));
        instanceTag_ = "k" + stampNow("%Y%m%d%H%M%S") + b;
    }
    static int n = 0;
    return instanceTag_ + "_" + std::to_string(++n);
}

bool App::writeRecoverySync(const Document& d, std::string& key, uint64_t& recRev) {
    if (!hasWork(d)) return true;
    if (key.empty()) key = newRecoveryKey();
    std::string data;
    if (!serializeDocument(d, data)) return false;
    if (!writeRecoveryFiles(recoveryDir(), key, data, d.path, d.name)) {
        log::warn("Could not write the recovery copy of " + d.name);
        return false;
    }
    recRev = d.revision;
    return true;
}

void App::writeAllRecoverySync() {
    if (modelTest_ || uiTest_) return;
    syncDirty();
    if (dirty_ && recRev_ != doc_.revision) writeRecoverySync(doc_, asKey_, recRev_);
    for (int i = 0; i < (int)designTabs_.size(); ++i) {
        if (i == designActive_) continue;
        DesignTab& t = designTabs_[(size_t)i];
        if (t.doc.revision != t.cleanRev) t.dirty = true;
        if (t.dirty && t.recRev != t.doc.revision) writeRecoverySync(t.doc, t.asKey, t.recRev);
    }
}

void App::dropRecovery(const std::string& key) {
    if (key.empty()) return;
    std::error_code ec;
    for (const char* ext : {".kst", ".info", ".kst.saving", ".info.saving"}) fs::remove(u8path(recoveryDir() + "/" + key + ext), ec);
}

void App::discardRecovery(const std::string& key) {
    if (key.empty()) return;
    std::error_code ec;
    std::string dir = recoveryDir(), bin = dir + "/discarded";
    fs::create_directories(u8path(bin), ec);
    std::string base = stampNow("%Y%m%d-%H%M%S") + "-" + key;
    for (const char* ext : {".kst", ".info"}) {
        std::string from = dir + "/" + key + ext;
        if (!fs::exists(u8path(from), ec)) continue;
        fs::rename(u8path(from), u8path(bin + "/" + base + ext), ec);
        if (ec) {   // keep it where it is rather than lose it
            log::warn("Could not move " + from + " to the discarded folder: " + ec.message());
            ec.clear();
        }
    }
    // The discarded folder keeps the newest 40 designs.
    std::vector<std::pair<fs::file_time_type, fs::path>> all;
    for (auto& e : fs::directory_iterator(u8path(bin), ec))
        if (e.path().extension() == ".kst") all.push_back({fs::last_write_time(e.path(), ec), e.path()});
    std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 40; i < all.size(); ++i) {
        fs::remove(all[i].second, ec);
        fs::path info = all[i].second;
        info.replace_extension(".info");
        fs::remove(info, ec);
    }
}

void App::discardDesignChanges(int tab) {
    bool act = tab == designActive_;
    if (!act && (tab < 0 || tab >= (int)designTabs_.size())) return;
    DesignTab* t = act ? nullptr : &designTabs_[(size_t)tab];
    Document& d = act ? doc_ : t->doc;
    std::string& key = act ? asKey_ : t->asKey;
    uint64_t& recRev = act ? recRev_ : t->recRev;
    bool& ownAuto = act ? ownAutosaved_ : t->ownAutosaved;
    // What is thrown away is kept in the discarded folder: Don't Save is sometimes a misclick.
    if (hasWork(d) && writeRecoverySync(d, key, recRev)) discardRecovery(key);
    else dropRecovery(key);
    key.clear();
    recRev = 0;
    // Autosave wrote these changes into the design file itself: put the last saved version back.
    if (ownAuto && !d.path.empty()) {
        int v = latestVersion(d.path);
        std::string vf = versionsDir(d.path) + "/v" + std::to_string(v) + ".kst";
        std::string data = v > 0 ? readFileText(vf) : std::string();
        if (!data.empty() && writeFileAtomic(d.path, data)) log::info("Put v" + std::to_string(v) + " back into " + d.path);
        else log::warn("Could not put the saved version back into " + d.path);
    }
    ownAuto = false;
    if (act) {
        ownRev_ = 0;
        markClean();
    } else {
        t->ownRev = 0;
        t->dirty = false;
        t->cleanRev = t->doc.revision;
    }
}

// ===================================================================== autosave

void App::updateAutosave() {
    if (modelTest_ || uiTest_) return;   // a self test must not touch the recovery files
    syncDirty();
    if (!dirty_) {
        lastSaveActivity_ = time_;
        return;
    }
    if (autosaveBusy_) return;
    // 1) The recovery copy: a few seconds after a change. Always on (it is invisible and costs a
    //    fraction of a millisecond to prepare); the preference below is about the design file.
    if (doc_.revision != recRev_ && hasWork(doc_) && time_ - lastRecoveryWrite_ >= 3.0) {
        lastRecoveryWrite_ = time_;
        std::string data;
        if (!serializeDocument(doc_, data)) return;
        if (asKey_.empty()) asKey_ = newRecoveryKey();
        std::string key = asKey_, dir = recoveryDir(), orig = doc_.path, name = doc_.name;
        uint64_t rev = doc_.revision;
        autosaveBusy_ = true;
        JobSystem* jobs = jobs_.get();
        jobs_->submit("recovery copy", [this, jobs, data, key, dir, orig, name, rev] {
            bool ok = writeRecoveryFiles(dir, key, data, orig, name);
            jobs->post([this, ok, key, rev] {
                autosaveBusy_ = false;
                if (!ok) {
                    static double warned = -1e9;
                    if (time_ - warned > 300) { toast("Could not write the recovery copy (is the disk full?)", ToastKind::Warning); warned = time_; }
                    return;
                }
                // Note it on the design it belongs to (it may be in another tab by now). Saved in the
                // meantime: the copy is not needed.
                if (key == asKey_) {
                    recRev_ = rev;
                    if (!dirty_ && doc_.revision == cleanRev_) dropRecovery(key);
                    return;
                }
                for (auto& t : designTabs_) {
                    if (t.asKey != key) continue;
                    t.recRev = rev;
                    if (!t.dirty && t.doc.revision == t.cleanRev) dropRecovery(key);
                    return;
                }
                dropRecovery(key);   // its design is closed: it was saved or discarded
            });
        }, [this](const std::string& err) {
            autosaveBusy_ = false;
            log::warn("Recovery copy failed: " + err);
        });
        return;
    }
    // 2) The design file itself (Preferences: "Autosave saved designs straight into their file").
    //    Not a version: Ctrl+S still makes one, and Don't Save puts the last version back.
    if (settings_.autosaveSec <= 0 || !settings_.autosaveToFile || doc_.path.empty() || dlg_.open || tdlg_.open) return;
    if (doc_.revision == ownRev_ || inRecoveryDir(doc_.path)) return;
    if (time_ - lastSaveActivity_ < std::max(5.f, settings_.autosaveSec)) return;
    lastSaveActivity_ = time_;
    std::string data;
    if (!serializeDocument(doc_, data)) return;
    std::string own = doc_.path;
    uint64_t rev = doc_.revision;
    autosaveBusy_ = true;
    JobSystem* jobs = jobs_.get();
    jobs_->submit("autosave", [this, jobs, data, own, rev] {
        bool ok = writeFileAtomic(own, data);
        jobs->post([this, ok, own, rev] {
            autosaveBusy_ = false;
            if (!ok) {
                toast("Autosave could not write " + own + ". Your work is still in the recovery copy.", ToastKind::Warning);
                return;
            }
            if (own == doc_.path) {
                ownRev_ = rev;
                ownAutosaved_ = true;
                lastAutosaveTime_ = time_;
                return;
            }
            for (auto& t : designTabs_)
                if (t.doc.path == own) { t.ownRev = rev; t.ownAutosaved = true; }
        });
    }, [this](const std::string& err) {
        autosaveBusy_ = false;
        log::warn("Autosave failed: " + err);
    });
}

// ===================================================================== start up

void App::checkRecovery() {
    recovered_.clear();
    std::error_code ec;
    std::string dir = recoveryDir();
    fs::create_directories(u8path(dir), ec);
    if (instanceTag_.empty()) newRecoveryKey();   // makes the tag
    std::string myLock = dir + "/" + instanceTag_ + ".lock";
    if (!platform::holdLock(myLock)) log::warn("Could not hold " + myLock);
    // Older Kestral kept one shared recovery file (and crash.kst): offered when newer than the last
    // clean exit, as before.
    std::time_t cleanT = fileTime(platform::appDataDir() + "/clean_exit");
    for (const char* name : {"recovery.kst", "crash.kst"}) {
        std::string p = dir + "/" + name;
        if (!fs::exists(u8path(p), ec)) continue;
        std::time_t t = fileTime(p);
        if (cleanT && t <= cleanT) continue;
        RecoveredWork w;
        w.file = p;
        w.when = t;
        w.legacy = true;
        w.name = storedName(p);
        recovered_.push_back(w);
    }
    // Per design copies whose Kestral is not running.
    for (auto& e : fs::directory_iterator(u8path(dir), ec)) {
        if (!e.is_regular_file(ec)) continue;
        fs::path p = e.path();
        std::string stem = pathToU8(p.stem()), ext = pathToU8(p.extension());
        if (ext == ".lock") {
            std::string tag = stem;
            if (tag != instanceTag_ && !platform::lockHeld(pathToU8(p))) fs::remove(p, ec);   // that Kestral is gone
            continue;
        }
        if (ext != ".kst" || stem == "recovery" || stem == "crash") continue;
        size_t us = stem.find('_');
        std::string tag = us == std::string::npos ? stem : stem.substr(0, us);
        if (tag == instanceTag_ || platform::lockHeld(dir + "/" + tag + ".lock")) continue;   // a running Kestral's
        RecoveredWork w;
        w.file = pathToU8(p);
        w.key = stem;
        w.when = fileTime(w.file);
        std::istringstream info(readFileText(dir + "/" + stem + ".info"));
        std::getline(info, w.original);
        std::getline(info, w.name);
        if (w.name.empty()) w.name = storedName(w.file);
        recovered_.push_back(w);
    }
    std::sort(recovered_.begin(), recovered_.end(), [](auto& a, auto& b) { return a.when > b.when; });
    if (!recovered_.empty()) log::info("Recovered work waiting: " + std::to_string(recovered_.size()) + " design(s)");
}

void App::restoreWork(const RecoveredWork& w) {
    auto r = loadDocument(w.file);
    if (!r.ok) {
        toast("Could not open the recovered " + (w.name.empty() ? std::string("design") : w.name) + ": " + r.error, ToastKind::Error);
        return;
    }
    if (docInUse()) newDesignTab();
    leaveSketchMode();
    sk_.active = false;
    doc_ = std::move(r.doc);
    std::error_code ec;
    bool origOk = !w.original.empty() && !inRecoveryDir(w.original) && fs::is_directory(u8path(w.original).parent_path(), ec);
    doc_.path = origOk ? w.original : std::string();
    std::string nm = w.name;
    if (nm.empty() || nm == "recovery" || nm == "crash") nm = origOk ? pathToU8(u8path(w.original).stem()) : std::string("Recovered design");
    doc_.name = nm;
    doc_.components[doc_.rootId].name = doc_.name;
    doc_.version = doc_.path.empty() ? 0 : latestVersion(doc_.path);
    undo_.clear();
    selection_.clear();
    isolated_.clear();
    browserHover_.clear();
    ownRev_ = 0;
    ownAutosaved_ = false;
    recRev_ = 0;
    if (w.legacy) {
        // Under its own key from now on; the old shared file goes to the discarded folder.
        asKey_.clear();
        std::string bin = recoveryDir() + "/discarded";
        fs::create_directories(u8path(bin), ec);
        fs::rename(u8path(w.file), u8path(bin + "/" + stampNow("%Y%m%d-%H%M%S") + "-" + pathToU8(u8path(w.file).filename())), ec);
    } else {
        asKey_ = w.key;   // saving it removes this copy; until then it stays
    }
    doc_.touch();
    cleanRev_ = 0;
    dirty_ = true;   // recovered work is unsaved work
    writeRecoverySync(doc_, asKey_, recRev_);
    startOpen_ = false;
    markSceneDirty();
    rebuildScene();
    float a = vk_.extent.height ? (float)vk_.extent.width / (float)vk_.extent.height : 1.f;
    cam_.home(sceneBounds_, a);
    scheduleRecompute(0);
    toast(doc_.path.empty() ? "Recovered " + doc_.name + ". It was never saved: Ctrl+S saves it as a design."
                            : "Recovered " + doc_.name + ". Ctrl+S saves it as the next version of " + pathToU8(u8path(doc_.path).filename()) + ".",
          ToastKind::Warning);
}

void App::restoreRecovered(size_t i) {
    if (i >= recovered_.size()) return;
    RecoveredWork w = recovered_[i];
    recovered_.erase(recovered_.begin() + (long)i);
    restoreWork(w);
}

void App::dismissRecovered(size_t i) {
    if (i >= recovered_.size()) return;
    RecoveredWork w = recovered_[i];
    recovered_.erase(recovered_.begin() + (long)i);
    if (!w.legacy) {
        discardRecovery(w.key);
        return;
    }
    std::error_code ec;
    std::string bin = recoveryDir() + "/discarded";
    fs::create_directories(u8path(bin), ec);
    fs::rename(u8path(w.file), u8path(bin + "/" + stampNow("%Y%m%d-%H%M%S") + "-" + pathToU8(u8path(w.file).filename())), ec);
}

void App::drawRecoveryPrompt() {
    if (recovered_.empty() || recoveryHidden_) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.35f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Recovered work", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(recovered_.size() == 1 ? "Kestral found unsaved work from last time." : "Kestral found unsaved work from last time:");
    ImGui::Spacing();
    int act = -1, kind = 0;   // kind 1 restore, 2 discard
    for (size_t i = 0; i < recovered_.size(); ++i) {
        const RecoveredWork& w = recovered_[i];
        ImGui::PushID((int)i);
        ImGui::Separator();
        char when[64] = "unknown time";
        if (w.when) {
            std::tm tmv{};
#ifdef _WIN32
            localtime_s(&tmv, &w.when);
#else
            localtime_r(&w.when, &tmv);
#endif
            std::strftime(when, sizeof when, "%b %d, %H:%M", &tmv);
        }
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted(w.name.empty() ? "Untitled design" : w.name.c_str());
        ImGui::PopFont();
        std::string where = w.original.empty() || inRecoveryDir(w.original) ? std::string("Never saved") : w.original;
        ImGui::TextDisabled("%s  |  %s", when, where.c_str());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.29f, 0.63f, 0.36f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (ImGui::Button("Restore", ImVec2(100 * S, 0))) { act = (int)i; kind = 1; }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(100 * S, 0))) { act = (int)i; kind = 2; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves it to the discarded folder (autosave\\discarded), it is not deleted");
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::Spacing();
    if (recovered_.size() > 1 && ImGui::Button("Restore all", ImVec2(110 * S, 0))) { act = 0; kind = 3; }
    if (recovered_.size() > 1) ImGui::SameLine();
    if (ImGui::Button("Later", ImVec2(110 * S, 0))) recoveryHidden_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep it for now: offered again next time (and under Recover on the start page)");
    ImGui::End();
    if (kind == 1) restoreRecovered((size_t)act);
    else if (kind == 2) dismissRecovered((size_t)act);
    else if (kind == 3) while (!recovered_.empty()) restoreRecovered(0);
}

} // namespace kst
