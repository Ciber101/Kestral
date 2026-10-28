// Move from Fusion: a Fusion script exports every design in every project in one run (STEP for
// the shapes, a list of the parts that were hidden, mesh bodies as STL, a .f3d backup). Kestral
// then scans that folder, shows what is new, updated or already imported, and turns the picked
// designs into Kestral designs next to their STEP files, hidden parts hidden again.
#include "app/App.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/SaveLoad.h"
#include "generated/fusion_script.h"
#include "platform/Embed.h"
#include "platform/Platform.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>

namespace kst {

using namespace ui;
namespace fs = std::filesystem;

namespace {

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::file_time_type mtimeOr(const fs::path& p, fs::file_time_type def) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? def : t;
}

fs::path sidecarOf(const fs::path& step) {
    fs::path p = step;
    p.replace_extension(".kestral.json");
    return p;
}

// Strips Fusion's occurrence number: "Wheel:2" -> "Wheel".
std::string stripOcc(const std::string& s) {
    auto c = s.rfind(':');
    if (c == std::string::npos) return s;
    for (size_t i = c + 1; i < s.size(); ++i)
        if (!std::isdigit((unsigned char)s[i])) return s;
    return s.substr(0, c);
}

} // namespace

// ===================================================================== converting one design

bool App::saveImportAsDesign(std::vector<LibPiece>& pieces, const std::string& sidecar, const std::string& out,
                             std::string& err, int& hiddenApplied) {
    hiddenApplied = 0;
    // Build in a scratch document so whatever is open stays untouched.
    Document scratch;
    std::swap(doc_, scratch);
    bool ok = false;
    try {
        std::vector<uint32_t> fids;
        for (auto& pc : pieces) {
            Feature f;
            f.id = doc_.newId();
            f.type = FeatureType::Import;
            f.name = "Import " + pathToU8(u8path(pc.src).filename());
            f.importPath = pc.src;
            doc_.timeline.push_back(f);
            doc_.markerPos = (int)doc_.timeline.size();
            addImportToDoc(*pc.r, f.id, pc.src);
            fids.push_back(f.id);
        }
        // Parts that were hidden in Fusion come back hidden (the export turned them on so the
        // STEP has them).
        json::Value side = json::Value::parse(sidecar);
        const json::Value& hid = side["hidden"];
        if (!pieces.empty() && hid.isObject()) {
            const kernel::ImportResult& r = *pieces[0].r;
            std::vector<uint32_t> ids = doc_.featureBodies(fids[0]);
            std::vector<std::string> occs, folders;
            for (auto& v : hid["occurrences"].items()) occs.push_back(v.asString());
            for (auto& v : hid["bodyFolders"].items()) folders.push_back(v.asString());
            size_t listed = occs.size() + folders.size() + hid["bodies"].size();
            std::string samples;
            size_t k = 0;
            // Hidden bodies recorded with where they sit (script v3): found by box, because STEP
            // often drops body names (all bodies in the top component come in unnamed).
            std::vector<std::array<double, 6>> boxes;
            for (auto& hb : hid["bodies"].items())
                for (auto& bx : hb["boxes"].items())
                    if (bx.size() == 6) {
                        std::array<double, 6> b6{};
                        for (size_t q = 0; q < 6; ++q) b6[q] = bx[q].asNumber();
                        boxes.push_back(b6);
                    }
            auto boxMatches = [&](const kernel::ImportResult::BodyOut& bo) {
                const MeshPtr& m = r.parts[bo.part].mesh;
                if (boxes.empty() || !m || m->verts.empty()) return false;
                glm::vec3 lo(1e30f), hi(-1e30f);
                for (size_t v = 0; v + 5 < m->verts.size(); v += 6) {
                    glm::vec3 p = glm::vec3(bo.xform * glm::vec4(m->verts[v], m->verts[v + 1], m->verts[v + 2], 1.f));
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                }
                for (auto& b : boxes) {
                    double diag = std::sqrt((b[3] - b[0]) * (b[3] - b[0]) + (b[4] - b[1]) * (b[4] - b[1]) + (b[5] - b[2]) * (b[5] - b[2]));
                    double tol = 0.15 + 0.01 * diag;
                    if (std::abs(lo.x - b[0]) < tol && std::abs(lo.y - b[1]) < tol && std::abs(lo.z - b[2]) < tol &&
                        std::abs(hi.x - b[3]) < tol && std::abs(hi.y - b[4]) < tol && std::abs(hi.z - b[5]) < tol)
                        return true;
                }
                return false;
            };
            for (const auto& bo : r.bodies) {
                if (samples.size() < 300) samples += " [" + bo.occPath + " | " + bo.compDef + " | " + bo.bodyName + "]";
                if (bo.part < 0 || bo.part >= (int)r.parts.size()) continue;
                if (k >= ids.size()) break;
                uint32_t id = ids[k++];
                bool hide = false;
                for (auto& o : occs)
                    if (bo.occPath == o || bo.occPath.rfind(o + "+", 0) == 0) hide = true;
                for (auto& fo : folders)
                    if (bo.compDef == fo || stripOcc(bo.compDef) == fo) hide = true;
                for (auto& hb : hid["bodies"].items()) {
                    std::string comp = hb["component"].asString(), body = hb["body"].asString();
                    bool compMatch = bo.compDef == comp || stripOcc(bo.compDef) == comp ||
                                     (hb["root"].asBool(false) && bo.occPath.empty());
                    if (compMatch && (bo.bodyName == body || (bo.bodyName.empty() && hb["only"].asBool(false)))) hide = true;
                }
                if (!hide && boxMatches(bo)) hide = true;
                if (hide) {
                    auto bit = doc_.bodies.find(id);
                    if (bit != doc_.bodies.end()) {
                        bit->second.visible = false;
                        syncBodyOverride(id);
                        ++hiddenApplied;
                    }
                }
            }
            if (listed > 0)
                log::info("Fusion library: " + pathToU8(u8path(out).stem()) + ": " + std::to_string(listed) + " hidden in Fusion, " +
                          std::to_string(hiddenApplied) + " bodies hidden" + (hiddenApplied ? std::string() : ". Bodies look like:" + samples));
        }
        refreshSignatures();
        doc_.name = pathToU8(u8path(out).stem());
        doc_.path = out;
        ok = saveDocument(doc_, out);
        if (!ok) err = "could not write " + out;
        else thumbFromDoc(out);   // preview for the data panel (rendered on a worker)
    } catch (const std::exception& e) {
        err = e.what();
    }
    std::swap(doc_, scratch);
    return ok;
}

// ===================================================================== the script

void App::writeFusionScript() {
    std::string dir = platform::designsDir() + "/Fusion export script/KestralExportAll";
    std::string dest = platform::designsDir() + "/From Fusion";
#ifdef _WIN32
    for (auto& c : dir) if (c == '/') c = '\\';
    for (auto& c : dest) if (c == '/') c = '\\';
#endif
    std::error_code ec;
    fs::create_directories(u8path(dir), ec);
    std::string py = gen::kFusionScriptPy;
    // The script exports straight into this Kestral's designs folder (whatever Windows calls
    // Documents on this PC, OneDrive included).
    auto at = py.find("__KESTRAL_DEST__");
    if (at != std::string::npos) py.replace(at, 16, dest);
    bool ok = true;
    for (auto [name, text] : {std::pair<std::string, std::string>{"KestralExportAll.py", py},
                              std::pair<std::string, std::string>{"KestralExportAll.manifest", gen::kFusionScriptManifest}}) {
        std::ofstream f(u8path(dir + "/" + name), std::ios::binary | std::ios::trunc);
        f << text;
        ok = ok && (bool)f;
    }
    if (!ok) { toast("Could not write the script to " + dir, ToastKind::Error); return; }
    fusionScriptDir_ = dir;
    platform::revealInExplorer(dir + (dir.find('\\') != std::string::npos ? "\\" : "/") + "KestralExportAll.py");
    toast("Script saved. Add that folder in Fusion's Scripts and Add-Ins", ToastKind::Success);
}

// ===================================================================== scanning

void App::scanFusionLibrary(const std::string& folder) {
    if (libScanning_ || libRunning_) return;
    libScanning_ = true;
    libFolder_ = folder;
    libEntries_.clear();
    libShowDone_ = false;
    JobSystem* jobs = jobs_.get();
    jobs_->setStatus("Looking through the exported designs");
    jobs_->submit("scan fusion library", [this, jobs, folder] {
        std::vector<LibEntry> out;
        std::error_code ec;
        fs::path root = u8path(folder);
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const fs::path& p = it->path();
            std::string fname = pathToU8(p.filename());
            if (it->is_directory(ec) && (fname.rfind("_Fusion backups", 0) == 0 || (fname.size() > 7 && fname.substr(fname.size() - 7) == ".meshes"))) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::string ext = toLower(pathToU8(p.extension()));
            if (ext != ".step" && ext != ".stp") continue;
            LibEntry e;
            e.step = pathToU8(p);
            fs::path kst = p;
            kst.replace_extension(".kst");
            e.kst = pathToU8(kst);
            e.rel = pathToU8(fs::relative(p, root, ec));
            if (ec) e.rel = fname;
            if (e.rel.size() > 5) e.rel = e.rel.substr(0, e.rel.size() - pathToU8(p.extension()).size());
            auto never = fs::file_time_type::min();
            if (!fs::exists(kst, ec)) {
                e.status = 0;
                e.pick = true;
            } else {
                auto src = std::max(mtimeOr(p, never), mtimeOr(sidecarOf(p), never));
                e.status = mtimeOr(kst, never) >= src ? 2 : 1;
                e.pick = false;   // updated designs only come in when you tick them
            }
            out.push_back(std::move(e));
        }
        std::sort(out.begin(), out.end(), [](const LibEntry& a, const LibEntry& b) { return toLower(a.rel) < toLower(b.rel); });
        jobs->post([this, jobs, out] {
            jobs->setStatus("");
            libEntries_ = out;
            libScanning_ = false;
            if (out.empty()) toast("No exported designs in " + libFolder_ + ". Run the script in Fusion first.", ToastKind::Warning);
        });
    }, [this, jobs](const std::string& e) {
        jobs->setStatus("");
        libScanning_ = false;
        toast("Could not read that folder: " + e, ToastKind::Error);
    });
}

// ===================================================================== importing

void App::importFusionLibrary(std::vector<LibEntry> picks) {
    if (libRunning_ || picks.empty()) return;
    libRunning_ = true;
    libFinished_ = false;
    libCancel_ = std::make_shared<CancelToken>();
    libMadeLive_ = 0;
    libDone_ = 0;
    libTotal_ = (int)picks.size();
    libMade_ = libFailed_ = libHidden_ = 0;
    libFailMsg_.clear();
    auto cancel = libCancel_;
    JobSystem* jobs = jobs_.get();
    // Small designs first: one huge file (a mesh turned into a solid can be hundreds of MB) then
    // only holds up itself, at the end.
    std::stable_sort(picks.begin(), picks.end(), [](const LibEntry& x, const LibEntry& y) {
        std::error_code e1, e2;
        return fs::file_size(u8path(x.step), e1) < fs::file_size(u8path(y.step), e2);
    });
    jobs_->submit("fusion library", [this, jobs, picks, cancel] {
        int made = 0, failed = 0, hidden = 0;
        std::string firstFail;
        for (size_t i = 0; i < picks.size() && !cancel->isCancelled(); ++i) {
            const LibEntry& e = picks[i];
            fs::path step = u8path(e.step);
            std::string name = pathToU8(step.filename());
            jobs->setStatus("Fusion library " + std::to_string(i + 1) + " of " + std::to_string(picks.size()) + ": " + name);
            std::string sidecar = readText(sidecarOf(step));
            std::vector<LibPiece> pieces;
            try {
                const CancelToken& ct = *cancel;
                LibPiece main;
                main.src = e.step;
                std::string head = "Fusion library " + std::to_string(i + 1) + " of " + std::to_string(picks.size()) + ": " + name;
                std::error_code sec;
                auto bytes = fs::file_size(step, sec);
                if (!sec && bytes > 50ull * 1024 * 1024) head += " (" + std::to_string(bytes / (1024 * 1024)) + " MB, this one takes a while)";
                main.r = std::make_shared<kernel::ImportResult>(kernel::importFile(e.step, ct, [jobs, head, cancel](const std::string& phase) {
                    if (!cancel->isCancelled()) jobs->setStatus(head + ": " + phase);
                }));
                pieces.push_back(main);
                // Mesh bodies Fusion could not put in the STEP.
                json::Value side = json::Value::parse(sidecar);
                fs::path mdir = step.parent_path() / u8path(pathToU8(step.stem()) + ".meshes");
                for (auto& m : side["meshes"].items()) {
                    fs::path mp = mdir / u8path(m.asString());
                    std::error_code ec;
                    if (!fs::exists(mp, ec)) continue;
                    try {
                        LibPiece pc;
                        pc.src = pathToU8(mp);
                        pc.r = std::make_shared<kernel::ImportResult>(kernel::importFile(pc.src, ct, [](const std::string&) {}));
                        pieces.push_back(pc);
                    } catch (const std::exception& ex) {
                        log::warn("Fusion library: mesh " + pathToU8(mp) + ": " + ex.what());
                    }
                }
            } catch (const std::exception& ex) {
                ++failed;
                if (firstFail.empty()) firstFail = name + ": " + ex.what();
                log::warn("Fusion library: " + name + ": " + ex.what());
                jobs->post([this, cancel, d = (int)i + 1] { if (!cancel->isCancelled()) libDone_ = d; });
                continue;
            }
            auto prom = std::make_shared<std::promise<std::pair<std::string, int>>>();
            auto fut = prom->get_future();
            auto shared = std::make_shared<std::vector<LibPiece>>(std::move(pieces));
            std::string out = e.kst;
            jobs->post([this, shared, sidecar, out, prom, cancel, d = (int)i + 1] {
                std::string err;
                int h = 0;
                if (cancel->isCancelled()) { prom->set_value({"stopped", 0}); return; }
                bool ok = saveImportAsDesign(*shared, sidecar, out, err, h);
                libDone_ = d;
                if (ok) ++libMadeLive_;
                prom->set_value({ok ? std::string() : (err.empty() ? std::string("failed") : err), h});
            });
            while (fut.wait_for(std::chrono::milliseconds(200)) != std::future_status::ready)
                if (cancel->isCancelled()) break;
            if (cancel->isCancelled()) break;
            auto [err, h] = fut.get();
            if (err.empty()) { ++made; hidden += h; }
            else { ++failed; if (firstFail.empty()) firstFail = name + ": " + err; }
        }
        jobs->post([this, jobs, made, failed, hidden, firstFail, cancel] {
            if (cancel->isCancelled()) return;   // Stop already wrapped everything up
            jobs->setStatus("");
            libRunning_ = false;
            libFinished_ = true;
            libShowDone_ = true;
            libMade_ = made;
            libFailed_ = failed;
            libHidden_ = hidden;
            libFailMsg_ = firstFail;
            fusionMoveOpen_ = true;
            std::string msg = std::string("Finished! ") + std::to_string(made) +
                              (made == 1 ? " design" : " designs") + " imported" +
                              (failed ? ", " + std::to_string(failed) + " could not be read" : "");
            toast(msg, failed ? ToastKind::Warning : ToastKind::Success);
            settings_.lastDesignDir = libFolder_;
            settings_.save(settingsPath_);
            openDataPanel(libFolder_);
            scanFusionLibrary(libFolder_);   // refresh the statuses
            libShowDone_ = true;
        });
    }, [this, jobs](const std::string& e) {
        jobs->setStatus("");
        libRunning_ = false;
        toast("Fusion library import failed: " + e, ToastKind::Error);
    });
}

// Stop is instant: the window and the app are free right away. A design the geometry kernel is
// still chewing on is dropped (it notices Stop at its next check, or finishes unseen); nothing
// half done is ever saved.
void App::stopFusionImport() {
    if (!libRunning_ || !libCancel_) return;
    libCancel_->cancelled = true;
    jobs_->setStatus("");
    libRunning_ = false;
    libFinished_ = true;
    libShowDone_ = true;
    libMade_ = libMadeLive_;
    libFailed_ = 0;
    libHidden_ = 0;
    libFailMsg_.clear();
    toast("Stopped. " + std::to_string(libMade_) + (libMade_ == 1 ? " design" : " designs") + " imported",
          ToastKind::Info);
    settings_.lastDesignDir = libFolder_;
    settings_.save(settingsPath_);
    openDataPanel(libFolder_);
    scanFusionLibrary(libFolder_);
}

// ===================================================================== window

void App::drawFusionMove() {
    if (!fusionMoveOpen_) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620 * S, std::min(vp->Size.y * 0.85f, 760 * S)), ImGuiCond_Appearing);
    if (!ImGui::Begin("Move everything from Fusion", &fusionMoveOpen_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    std::string dest = platform::designsDir() + "/From Fusion";
#ifdef _WIN32
    for (auto& c : dest) if (c == '/') c = '\\';
#endif
    auto heading = [&](const char* t) {
        ImGui::Spacing();
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
    };
    auto dim = [&](const std::string& t) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", t.c_str());
        ImGui::PopStyleColor();
    };

    if (libShowDone_ && !libRunning_) {
        // Big, clear "it finished" box.
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = 74 * S;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), libFailed_ ? th(IM_COL32(255, 245, 225, 255)) : th(IM_COL32(228, 247, 234, 255)), 6 * S);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 12 * S, p.y + 10 * S));
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::Text("Import finished: %d %s ready", libMade_, libMade_ == 1 ? "design" : "designs");
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x + 12 * S, p.y + 32 * S));
        std::string sub = libHidden_ ? std::to_string(libHidden_) + " parts hidden again, like in Fusion. " : std::string();
        if (libFailed_) sub += std::to_string(libFailed_) + " could not be read (" + libFailMsg_ + ")";
        ImGui::TextUnformatted(sub.c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x + 12 * S, p.y + h + 6 * S));
        if (ImGui::Button("Show my designs")) openDataPanel(libFolder_);
        ImGui::SameLine();
        if (ImGui::Button("OK")) libShowDone_ = false;
        ImGui::Separator();
    }

    ImGui::TextWrapped("Every design in every Fusion project comes over in one run. Fusion does the export with a "
                       "small script (Fusion's own STEP export, so shapes, components, folders, names and colors come "
                       "out exactly). Hidden parts come too and stay hidden. Then Kestral converts the library.");

    heading("1.  Get the export script");
    if (ImGui::Button("Save the script and show it")) writeFusionScript();
    std::string sdir = platform::designsDir() + "/Fusion export script/KestralExportAll";
#ifdef _WIN32
    for (auto& c : sdir) if (c == '/') c = '\\';
#endif
    ImGui::SameLine();
    if (ImGui::Button("Copy folder path")) ImGui::SetClipboardText(sdir.c_str());
    dim("The script is saved in:  " + sdir);

    heading("2.  Run it in Fusion (once)");
    ImGui::TextWrapped("In Fusion: UTILITIES tab > ADD-INS > Scripts and Add-Ins (Shift+S). Click the + next to "
                       "\"My Scripts\" (\"Script or add-in from device\"), pick the KestralExportAll folder above, then "
                       "select it and click Run. Press Yes to export into:");
    dim(dest);
    dim("Already exported with the first version of the script? Run it again: it re-exports every design once so "
        "hidden parts and mesh bodies come along. If Fusion crashes on a design, run it again and it skips that one.");

    heading("3.  Import the exported library");
    if (libRunning_) {
        ImGui::ProgressBar(libTotal_ ? (float)libDone_ / libTotal_ : 0.f, ImVec2(-90 * S, 0),
                           (std::to_string(libDone_) + " / " + std::to_string(libTotal_)).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(-1, 0))) stopFusionImport();
        activity();
    } else if (libScanning_) {
        ImGui::TextDisabled("Looking through the folder...");
        activity();
    } else {
        if (ImGui::Button(libEntries_.empty() ? "Check the usual folder" : "Check again")) {
            std::error_code ec;
            std::string f = libFolder_.empty() ? dest : libFolder_;
            if (fs::exists(u8path(f), ec)) scanFusionLibrary(f);
            else toast("Nothing there yet: run the script in Fusion first (step 2)", ToastKind::Warning);
        }
        ImGui::SameLine();
        if (ImGui::Button("Choose another folder...")) {
            std::string f = platform::pickFolderDialog("Folder the Fusion export script wrote", dest);
            if (!f.empty()) scanFusionLibrary(f);
        }
    }

    if (!libEntries_.empty() && !libRunning_) {
        int nNew = 0, nUpd = 0, nDone = 0, nPick = 0;
        for (auto& e : libEntries_) {
            (e.status == 0 ? nNew : e.status == 1 ? nUpd : nDone)++;
            if (e.pick) ++nPick;
        }
        ImGui::Text("%d new,  %d changed since you imported them,  %d already in Kestral", nNew, nUpd, nDone);
        static bool showDone = false;
        if (ImGui::SmallButton("Pick all new")) for (auto& e : libEntries_) e.pick = e.status == 0 ? true : e.pick;
        ImGui::SameLine();
        if (ImGui::SmallButton("Pick all changed")) for (auto& e : libEntries_) e.pick = e.status == 1 ? true : e.pick;
        ImGui::SameLine();
        if (ImGui::SmallButton("Pick none")) for (auto& e : libEntries_) e.pick = false;
        ImGui::SameLine();
        ImGui::Checkbox("Show ones already imported", &showDone);
        float listH = std::max(120 * S, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.2f);
        if (ImGui::BeginTable("##lib", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter,
                              ImVec2(0, listH))) {
            ImGui::TableSetupColumn("Design", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 150 * S);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < libEntries_.size(); ++i) {
                LibEntry& e = libEntries_[i];
                if (e.status == 2 && !showDone) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID((int)i);
                ImGui::BeginDisabled(e.status == 2);   // the same version never comes in twice
                ImGui::Checkbox(e.rel.c_str(), &e.pick);
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImVec4 c = e.status == 0 ? ImVec4(0.2f, 0.62f, 0.3f, 1) : e.status == 1 ? ImVec4(0.9f, 0.55f, 0.1f, 1) : th(ImVec4(0.5f, 0.53f, 0.57f, 1));
                ImGui::TextColored(c, "%s", e.status == 0 ? "New" : e.status == 1 ? "Changed in Fusion" : "Already imported");
            }
            ImGui::EndTable();
        }
        ImGui::BeginDisabled(nPick == 0);
        std::string label = "Import " + std::to_string(nPick) + (nPick == 1 ? " design" : " designs");
        if (ImGui::Button(label.c_str(), ImVec2(220 * S, 0))) {
            std::vector<LibEntry> picks;
            for (auto& e : libEntries_) if (e.pick && e.status != 2) picks.push_back(e);
            libShowDone_ = false;
            importFusionLibrary(picks);
        }
        ImGui::EndDisabled();
        if (nUpd) {
            ImGui::SameLine();
            ImGui::TextDisabled("Changed ones replace their Kestral copy.");
        }
    }
    ImGui::Spacing();
    dim("Each design becomes a Kestral design (.kst) next to its STEP, in the same folders as in Fusion. Find them in the "
        "data panel (folder button, top left) or with Ctrl+O. Keep the STEP files: the designs read their shapes from them. "
        "Fusion's timeline cannot leave Fusion in any format; the .f3d backups keep it safe.");
    ImGui::End();
}

} // namespace kst
