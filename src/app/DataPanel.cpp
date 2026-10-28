// The data panel (Fusion's left side "Data Panel"): every design in Kestral Designs, by folder.
// Double click a design to open it. Opens by itself after a Fusion library import.
#include "app/App.h"
#include "app/Thumbnails.h"
#include "core/Log.h"
#include "platform/Platform.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <ctime>
#include <filesystem>

namespace kst {

using namespace ui;
namespace fs = std::filesystem;

namespace {
long long fileStamp(const std::string& p) {
    std::error_code ec;
    auto t = fs::last_write_time(u8path(p), ec);
    return ec ? 0 : (long long)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}
} // namespace

// ===================================================================== previews

void App::thumbFromItems(const std::string& designPath, std::vector<kernel::ExportItem> items) {
    if (designPath.empty() || items.empty()) return;
    std::string png = thumb::pathFor(designPath);
    JobSystem* jobs = jobs_.get();
    jobs_->submit("preview", [this, jobs, items = std::move(items), png, designPath] {
        std::vector<thumb::Item> ti;
        for (auto& it : items)
            if (it.mesh) ti.push_back({it.mesh, it.xform, it.color});
        auto px = thumb::render(ti);
        bool ok = !px.empty() && thumb::writePng(png, px, thumb::kSize, thumb::kSize);
        jobs->post([this, designPath, ok] {
            if (ok) thumbs_[designPath].stamp = -1;   // load the new one
        });
    });
}

void App::thumbFromDoc(const std::string& designPath) {
    std::vector<kernel::ExportItem> items;
    for (auto& [id, b] : doc_.bodies) {
        if (!bodyShown(b)) continue;
        auto pit = doc_.parts.find(b.partId);
        if (pit != doc_.parts.end() && pit->second.mesh) items.push_back({nullptr, pit->second.mesh, b.xform, b.name, b.color});
    }
    thumbFromItems(designPath, std::move(items));
}

ImTextureData* App::thumbFor(const std::string& designPath) {
    ThumbTex& e = thumbs_[designPath];
    if (e.stamp != -1 && (e.tex || e.missing)) return e.tex;
    if (thumbLoadsThisFrame_ >= 6) return e.tex;   // a few per frame keeps scrolling smooth
    ++thumbLoadsThisFrame_;
    std::string png = thumb::pathFor(designPath);
    long long pst = fileStamp(png);
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (pst == 0 || !thumb::readPng(png, px, w, h)) {
        e.missing = true;
        e.stamp = 0;
        return e.tex;
    }
    // An older preview still shows; a fresh one is made in the background.
    e.missing = pst < fileStamp(designPath);
    e.stamp = pst;
    if (e.tex) {
        e.tex->SetStatus(ImTextureStatus_WantDestroy);
        thumbGraveyard_.push_back(e.tex);
    }
    ImTextureData* t = IM_NEW(ImTextureData)();
    t->Create(ImTextureFormat_RGBA32, w, h);
    std::memcpy(t->GetPixels(), px.data(), px.size());
    t->UseColors = true;
    ImGui::RegisterUserTexture(t);
    e.tex = t;
    return t;
}

void App::flushTextureGraveyard() {
    // Textures the graphics backend has freed by now are let go for good (every page calls this).
    for (size_t i = 0; i < thumbGraveyard_.size();) {
        ImTextureData* t = thumbGraveyard_[i];
        if (t->Status == ImTextureStatus_Destroyed) {
            ImGui::UnregisterUserTexture(t);
            IM_DELETE(t);
            thumbGraveyard_.erase(thumbGraveyard_.begin() + (long)i);
        } else {
            ++i;
        }
    }
}

void App::updateThumbnails() {
    thumbLoadsThisFrame_ = 0;
    flushTextureGraveyard();
    bool rebuildIdle = recomputeQueuedFrom_ < 0 && !recomputeRunning_ && !recomputeResults_;
    // A design just opened: once its geometry is rebuilt, its preview is refreshed.
    if (!thumbAfterRebuild_.empty()) {
        if (thumbAfterRebuild_ != doc_.path) thumbAfterRebuild_.clear();
        else if (rebuildIdle && !doc_.bodies.empty()) {
            thumbFromDoc(doc_.path);
            thumbAfterRebuild_.clear();
        }
    }
    // Designs without a preview that came from Fusion: made from their STEP in the background,
    // one at a time, only while the data panel shows them and no import or rebuild is running.
    if (!dataOpen_ || thumbBusy_ || libRunning_ || !rebuildIdle) return;
    for (auto& it : dataItems_) {
        if (it.dir) continue;
        auto f = thumbs_.find(it.path);
        if (f == thumbs_.end() || !f->second.missing || thumbTried_.count(it.path)) continue;
        thumbTried_.insert(it.path);
        std::string step;
        std::error_code ec;
        for (const char* ext : {".step", ".stp", ".STEP", ".STP"}) {
            fs::path c = u8path(it.path);
            c.replace_extension(ext);
            if (fs::exists(c, ec)) { step = pathToU8(c); break; }
        }
        // Huge files wait until the design is opened (reading them takes minutes).
        if (step.empty() || fs::file_size(u8path(step), ec) > 60ull * 1024 * 1024) continue;
        thumbBusy_ = true;
        std::string design = it.path, png = thumb::pathFor(it.path);
        JobSystem* jobs = jobs_.get();
        auto cancel = std::make_shared<CancelToken>();
        jobs_->submit("preview", [this, jobs, design, png, step, cancel] {
            bool ok = false;
            try {
                kernel::ImportResult r = kernel::importFile(step, *cancel, [](const std::string&) {});
                std::vector<thumb::Item> ti;
                for (auto& b : r.bodies)
                    if (b.part >= 0 && b.part < (int)r.parts.size() && r.parts[b.part].mesh)
                        ti.push_back({r.parts[b.part].mesh, b.xform, b.hasColor ? b.color : glm::vec3(0.72f, 0.74f, 0.77f)});
                auto px = thumb::render(ti);
                ok = !px.empty() && thumb::writePng(png, px, thumb::kSize, thumb::kSize);
                if (!ok) log::warn("No preview for " + design + " (" + std::to_string(ti.size()) + " bodies)");
            } catch (const std::exception& e) {
                log::warn("No preview for " + design + ": " + e.what());
            }
            jobs->post([this, design, ok] {
                thumbBusy_ = false;
                if (ok) thumbs_[design].stamp = -1;
            });
        }, [this](const std::string&) { thumbBusy_ = false; });
        return;
    }
}

namespace {
bool iconButton(const char* id, ui::Icon icon, float size, bool enabled, const char* tip, bool active = false) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ui::col::Active, 4.f);
    else if (hov && enabled) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ui::col::Hover, 4.f);
    ui::drawIcon(dl, ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.68f, icon, enabled);
    if (hov && tip && *tip) ImGui::SetTooltip("%s", tip);
    return ImGui::IsItemClicked(ImGuiMouseButton_Left) && enabled;
}
} // namespace

void App::openDataPanel(const std::string& folder) {
    dataOpen_ = true;
    std::error_code ec;
    dataCur_ = !folder.empty() && fs::is_directory(u8path(folder), ec) ? folder : platform::designsDir();
    dataScanT_ = -10;
    dataSearch_[0] = 0;
}

void App::drawDataPanel() {
    if (!dataOpen_) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    std::string root = platform::designsDir();
    if (dataCur_.empty()) dataCur_ = root;
    std::error_code ec;

    // Refresh the listing now and then (designs appear while an import runs).
    std::string q = toLower(dataSearch_);
    if (time_ - dataScanT_ > 2.0) {
        dataScanT_ = time_;
        dataItems_.clear();
        auto add = [&](const fs::directory_entry& e) {
            std::string name = pathToU8(e.path().filename());
            if (name.empty() || name[0] == '.' || name.rfind("_Fusion backups", 0) == 0) return;
            bool dir = e.is_directory(ec);
            if (dir && name.size() > 7 && name.substr(name.size() - 7) == ".meshes") return;
            if (!dir && toLower(pathToU8(e.path().extension())) != ".kst") return;
            long long mt = 0;
            auto t = e.last_write_time(ec);
            // File times count from 1601 on Windows: convert to the system clock (1970) before showing a date.
            if (!ec) mt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch()).count();
            DataItem it;
            it.name = dir ? name : pathToU8(e.path().stem());
            it.path = pathToU8(e.path());
            it.dir = dir;
            it.mtime = mt;
            if (dir) {
                int n = 0;
                for (auto& c : fs::directory_iterator(e.path(), fs::directory_options::skip_permission_denied, ec)) {
                    if (++n > 999) break;
                    std::string cn = pathToU8(c.path().filename());
                    if (!cn.empty() && cn[0] == '.') { --n; continue; }
                    if (!c.is_directory(ec) && toLower(pathToU8(c.path().extension())) != ".kst") --n;
                }
                it.count = n;
            } else {
                it.version = latestVersion(it.path);
            }
            dataItems_.push_back(it);
        };
        if (q.empty()) {
            for (auto& e : fs::directory_iterator(u8path(dataCur_), fs::directory_options::skip_permission_denied, ec)) add(e);
        } else {
            // Searching looks through every folder below.
            int n = 0;
            for (auto it = fs::recursive_directory_iterator(u8path(dataCur_), fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator() && n < 4000; it.increment(ec), ++n) {
                if (ec) break;
                if (it->is_regular_file(ec) && toLower(pathToU8(it->path().filename())).find(q) != std::string::npos) add(*it);
            }
        }
        std::sort(dataItems_.begin(), dataItems_.end(), [](const DataItem& a, const DataItem& b) {
            if (a.dir != b.dir) return a.dir;
            return toLower(a.name) < toLower(b.name);
        });
    }

    float top = (34.f + 76.f + 8.f) * S;
    float h = vp->Size.y - top - (38.f + 12.f) * S;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 8 * S, vp->Pos.y + top));
    ImGui::SetNextWindowSize(ImVec2(std::min(400 * S, vp->Size.x * 0.45f), h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * S, 12 * S));
    ImGui::Begin("##datapanel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* wdl = ImGui::GetWindowDrawList();
    float W = ImGui::GetContentRegionAvail().x;
    // Header: title, list / grid switch, close.
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushFont(fonts_.bold, 19.f * S);
        wdl->AddText(p, col::Text, "Data");
        ImGui::PopFont();
        float bs = 24 * S;
        ImGui::SetCursorScreenPos(ImVec2(p.x + W - bs * 3 - 8 * S, p.y - 2 * S));
        if (iconButton("##dgrid", Icon::Grid, bs, true, "Preview cards", dataGrid_)) dataGrid_ = true;
        ImGui::SameLine(0, 2 * S);
        if (iconButton("##dlist", Icon::Display, bs, true, "Compact list", !dataGrid_)) dataGrid_ = false;
        ImGui::SameLine(0, 6 * S);
        if (iconButton("##dclose", Icon::Close, bs, true, "Close the data panel")) dataOpen_ = false;
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 30 * S));
    }

    // Breadcrumb with a back button: Kestral Designs > From Fusion > Project
    {
        fs::path rel = fs::relative(u8path(dataCur_), u8path(root), ec);
        std::vector<std::pair<std::string, std::string>> crumbs = {{"Kestral Designs", root}};
        if (ec || pathToU8(rel).rfind("..", 0) == 0) {
            crumbs = {{pathToU8(u8path(dataCur_).filename()), dataCur_}};   // outside Kestral Designs
        } else if (!rel.empty() && pathToU8(rel) != ".") {
            fs::path acc = u8path(root);
            for (auto& part : rel) {
                acc /= part;
                crumbs.push_back({pathToU8(part), pathToU8(acc)});
            }
        }
        if (crumbs.size() > 1) {
            if (ImGui::ArrowButton("##dback", ImGuiDir_Left)) { dataCur_ = crumbs[crumbs.size() - 2].second; dataScanT_ = -10; }
            ImGui::SetItemTooltip("Up one folder");
            ImGui::SameLine(0, 6 * S);
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        for (size_t i = 0; i < crumbs.size(); ++i) {
            if (i) { ImGui::SameLine(0, 0); ImGui::TextDisabled("/"); ImGui::SameLine(0, 0); }
            ImGui::PushID((int)i);
            bool last = i + 1 == crumbs.size();
            if (last) {
                ImGui::AlignTextToFramePadding();
                ImGui::PushFont(fonts_.bold, 0.f);
                ImGui::TextUnformatted(crumbs[i].first.c_str());
                ImGui::PopFont();
            } else if (ImGui::Button(crumbs[i].first.c_str())) { dataCur_ = crumbs[i].second; dataScanT_ = -10; }
            ImGui::PopID();
        }
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * S, 6 * S));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##datasearch", "Search all designs", dataSearch_, sizeof dataSearch_)) dataScanT_ = -10;
    ImGui::PopStyleVar(2);
    ImGui::Spacing();
    {   // Actions as soft pills.
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12 * S);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * S, 4 * S));
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(col::Accent));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (ImGui::Button("Open...")) openDocumentDialog();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Show in folder")) platform::revealInExplorer(dataCur_);
        ImGui::SameLine();
        if (ImGui::Button("Move from Fusion")) fusionMoveOpen_ = true;
        ImGui::PopStyleVar(2);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginChild("##datalist", ImVec2(0, 0));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (dataItems_.empty())
        ImGui::TextDisabled(q.empty() ? "No designs here yet." : "Nothing matches.");
    auto whenOf = [](long long mt, char* buf, size_t n) {
        buf[0] = 0;
        if (mt <= 0) return;
        std::time_t t = (std::time_t)mt;
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        std::strftime(buf, n, "%b %d %Y", &tmv);
    };
    auto itemActions = [&](const DataItem& it, bool hov) {
        if (ImGui::IsItemClicked()) {
            dataSel_ = it.path;
            if (it.dir) { dataCur_ = it.path; dataScanT_ = -10; dataSearch_[0] = 0; }   // folders open on one click
        }
        if (!it.dir && hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openDocumentPath(it.path);
        if (hov && !it.dir) ImGui::SetTooltip("%s\nDouble click to open", it.path.c_str());
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (!it.dir && ImGui::MenuItem("Open")) openDocumentPath(it.path);
            if (!it.dir) {
                auto vers = designVersions(it.path);
                if (ImGui::BeginMenu("Open an older version", vers.size() > 1)) {
                    for (int v : vers) {
                        std::string lbl = "v" + std::to_string(v) + (v == vers.front() ? "  (newest)" : "");
                        std::error_code vec;
                        auto vt = std::filesystem::last_write_time(u8path(versionsDir(it.path) + "/v" + std::to_string(v) + ".kst"), vec);
                        char when2[64] = "";
                        if (!vec) {
                            std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(vt));
                            std::tm tmv{};
#ifdef _WIN32
                            localtime_s(&tmv, &t);
#else
                            localtime_r(&t, &tmv);
#endif
                            std::strftime(when2, sizeof when2, "%b %d  %H:%M", &tmv);
                        }
                        if (ImGui::MenuItem(lbl.c_str(), when2)) openDesignVersion(it.path, v);
                    }
                    ImGui::EndMenu();
                }
            }
            if (it.dir && ImGui::MenuItem("Open folder")) { dataCur_ = it.path; dataScanT_ = -10; }
            if (ImGui::MenuItem("Show in folder")) platform::revealInExplorer(it.path);
            ImGui::EndPopup();
        }
    };
    // Folders first: one roomy row each, with how many designs are inside.
    bool anyFolder = false, anyFile = false;
    for (auto& it : dataItems_) (it.dir ? anyFolder : anyFile) = true;
    if (anyFolder) { ImGui::TextDisabled("FOLDERS"); ImGui::Spacing(); }
    for (size_t i = 0; i < dataItems_.size(); ++i) {
        const DataItem& it = dataItems_[i];
        if (!it.dir) continue;
        ImGui::PushID((int)i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, rh = 40 * S;
        ImGui::InvisibleButton("##item", ImVec2(w, rh));
        bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), hov ? col::Selected : col::Card, 8 * S);
        drawIcon(dl, ImVec2(p.x + 22 * S, p.y + rh * 0.5f), 22 * S, Icon::Folder, true);
        float lh = ImGui::GetTextLineHeight();
        dl->PushClipRect(ImVec2(p.x + 40 * S, p.y), ImVec2(p.x + w - 60 * S, p.y + rh), true);
        dl->AddText(ImVec2(p.x + 42 * S, p.y + (rh - lh) * 0.5f), col::Text, it.name.c_str());
        dl->PopClipRect();
        std::string cnt = std::to_string(it.count);
        ImVec2 cs = ImGui::CalcTextSize(cnt.c_str());
        ImVec2 c0(p.x + w - cs.x - 34 * S, p.y + (rh - cs.y) * 0.5f - 2 * S);
        dl->AddRectFilled(c0, ImVec2(c0.x + cs.x + 12 * S, c0.y + cs.y + 4 * S), col::Chip, 9 * S);
        dl->AddText(ImVec2(c0.x + 6 * S, c0.y + 2 * S), col::TextDim, cnt.c_str());
        drawIcon(dl, ImVec2(p.x + w - 14 * S, p.y + rh * 0.5f), 10 * S, Icon::Chevron, true);
        itemActions(it, hov);
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, 1 * S));
    }
    if (anyFile) { ImGui::Spacing(); ImGui::TextDisabled("DESIGNS"); ImGui::Spacing(); }
    // Designs: preview cards in a grid, or compact rows.
    float avail = ImGui::GetContentRegionAvail().x, gap = 8 * S;
    int cols = dataGrid_ ? std::max(1, (int)((avail + gap) / (150 * S + gap))) : 1;
    float cw = dataGrid_ ? (avail - gap * (cols - 1)) / cols : avail;
    float ch = dataGrid_ ? cw + 40 * S : 52 * S;
    int k = 0;
    for (size_t i = 0; i < dataItems_.size(); ++i) {
        const DataItem& it = dataItems_[i];
        if (it.dir) continue;
        ImGui::PushID((int)i);
        if (k % cols) ImGui::SameLine(0, gap);
        ++k;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImTextureData* tex = nullptr;
        if (ImGui::IsRectVisible(p, ImVec2(p.x + cw, p.y + ch))) tex = thumbFor(it.path);
        ImGui::InvisibleButton("##item", ImVec2(cw, ch));
        bool hov = ImGui::IsItemHovered();
        bool sel = dataSel_ == it.path;
        bool current = !doc_.path.empty() && doc_.path == it.path;
        dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), sel ? col::Selected : col::Card, 8 * S);
        if (hov || sel) dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), col::Accent, 8 * S, 0, 1.5f * S);
        char when[64];
        whenOf(it.mtime, when, sizeof when);
        if (dataGrid_) {
            ImVec2 t0(p.x + 6 * S, p.y + 6 * S), t1(p.x + cw - 6 * S, p.y + cw - 6 * S);
            dl->AddRectFilled(t0, t1, col::Chip, 6 * S);
            if (tex) dl->AddImage(tex->GetTexRef(), t0, t1);
            else drawIcon(dl, ImVec2((t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f), 34 * S, Icon::Component, false);
            dl->PushClipRect(ImVec2(p.x + 6 * S, t1.y), ImVec2(p.x + cw - 6 * S, p.y + ch), true);
            ImGui::PushFont(fonts_.bold, 0.f);
            dl->AddText(ImVec2(p.x + 8 * S, t1.y + 5 * S), col::Text, it.name.c_str());
            ImGui::PopFont();
            dl->AddText(ImVec2(p.x + 8 * S, t1.y + 21 * S), current ? col::Accent : col::TextDim, current ? "Open now" : when);
            dl->PopClipRect();
            if (it.version > 0) {   // version badge on the preview, like Fusion's
                std::string vs = "v" + std::to_string(it.version);
                ImVec2 vz = ImGui::CalcTextSize(vs.c_str());
                ImVec2 b0(t1.x - vz.x - 14 * S, t0.y + 4 * S);
                dl->AddRectFilled(b0, ImVec2(t1.x - 4 * S, b0.y + vz.y + 4 * S), col::Accent, 8 * S);
                dl->AddText(ImVec2(b0.x + 5 * S, b0.y + 2 * S), IM_COL32_WHITE, vs.c_str());
            }
        } else {
            float ts = ch - 10 * S;
            ImVec2 t0(p.x + 5 * S, p.y + 5 * S), t1(t0.x + ts, t0.y + ts);
            dl->AddRectFilled(t0, t1, col::Chip, 5 * S);
            if (tex) dl->AddImage(tex->GetTexRef(), t0, t1);
            else drawIcon(dl, ImVec2((t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f), 20 * S, Icon::Component, false);
            float lh = ImGui::GetTextLineHeight();
            dl->PushClipRect(ImVec2(t1.x + 6 * S, p.y), ImVec2(p.x + cw - 4 * S, p.y + ch), true);
            dl->AddText(ImVec2(t1.x + 10 * S, p.y + ch * 0.5f - lh - 1 * S), col::Text, it.name.c_str());
            std::string sub = (it.version > 0 ? "v" + std::to_string(it.version) + "   " : std::string()) + (current ? "Open now" : when);
            dl->AddText(ImVec2(t1.x + 10 * S, p.y + ch * 0.5f + 1 * S), current ? col::Accent : col::TextDim, sub.c_str());
            dl->PopClipRect();
        }
        itemActions(it, hov);
        ImGui::PopID();
        if (!dataGrid_) ImGui::Dummy(ImVec2(0, 1 * S));
    }
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace kst
