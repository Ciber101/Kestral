// The Illustration dialog (UTILITIES > Illustration, File > Export Illustration): a live preview of
// the picture and its options, then Save as PNG or SVG.
#include "app/App.h"
#include "app/SliceStyle.h"
#include "core/Log.h"
#include "platform/Platform.h"

#include <imgui.h>

#include <algorithm>
#include <fstream>

namespace kst {

using namespace ui;

namespace {
struct SizeChoice { const char* label; int w, h; };
const SizeChoice kSizes[] = {{"1920 x 1080 (HD)", 1920, 1080}, {"2560 x 1440", 2560, 1440}, {"3840 x 2160 (4K)", 3840, 2160},
                             {"2048 x 2048 (square)", 2048, 2048}, {"3508 x 2480 (A4 at 300 dpi)", 3508, 2480}, {"1280 x 720", 1280, 720}};
} // namespace

std::vector<illus::Part> App::illusParts(bool selectedOnly, const std::vector<std::string>& names) const {
    std::vector<illus::Part> out;
    std::set<uint32_t> only;
    if (!names.empty()) {
        std::string missing;
        auto ids = aiFindBodies(names, missing);
        only.insert(ids.begin(), ids.end());
    } else if (selectedOnly) {
        only.insert(selection_.begin(), selection_.end());
    }
    for (auto& [id, b] : doc_.bodies) {
        if (only.empty() ? !bodyShown(b) : !only.count(id)) continue;
        auto pit = doc_.parts.find(b.partId);
        if (pit == doc_.parts.end() || !pit->second.mesh) continue;
        out.push_back({pit->second.mesh, b.xform, b.color, b.name});
    }
    return out;
}

illus::Options App::illusOptions(int w, int h) const {
    illus::Options o = illus_.opt;
    o.width = w;
    o.height = h;
    o.camFwd = cam_.forward();
    o.camUp = cam_.rot * glm::vec3(0, 1, 0);
    return o;
}

void App::illusPreview() {
    if (illus_.busy) { illus_.dirty = true; return; }
    illus_.dirty = false;
    illus_.busy = true;
    const SizeChoice& sc = kSizes[std::clamp(illus_.size, 0, (int)(sizeof kSizes / sizeof kSizes[0]) - 1)];
    // The preview keeps the final picture's shape (so balloons and the list sit where they will).
    int pw = 960, ph = std::max(64, (int)std::lround(960.0 * sc.h / sc.w));
    if (ph > 720) { ph = 720; pw = std::max(64, (int)std::lround(720.0 * sc.w / sc.h)); }
    auto parts = illusParts(illus_.selectedOnly, {});
    illus::Options o = illusOptions(pw, ph);
    JobSystem* jobs = jobs_.get();
    jobs_->submit("illustration preview", [this, jobs, parts, o] {
        illus::Result r = illus::render(parts, o, false);
        jobs->post([this, r] {
            illus_.busy = false;
            if (illus_.tex) dropTexture(illus_.tex);
            illus_.tex = r.rgba.empty() ? nullptr : makeTexture(r.rgba.data(), r.width, r.height);
            illus_.texW = r.width;
            illus_.texH = r.height;
            illus_.numbered = r.partsNumbered;
            if (illus_.dirty) illusPreview();
            activity();
        });
    }, [this](const std::string& e) { illus_.busy = false; illus_.status = "Preview failed: " + e; });
}

void App::illusSave(bool svg) {
    std::string def = (doc_.name.empty() ? std::string("Kestral") : doc_.name) + (svg ? ".svg" : ".png");
    std::string path = svg ? platform::saveFileDialog("Save the illustration", "SVG drawing", "*.svg", "svg", def, platform::designsDir())
                           : platform::saveFileDialog("Save the illustration", "PNG picture", "*.png", "png", def, platform::designsDir());
    if (path.empty()) return;
    const SizeChoice& sc = kSizes[std::clamp(illus_.size, 0, (int)(sizeof kSizes / sizeof kSizes[0]) - 1)];
    auto parts = illusParts(illus_.selectedOnly, {});
    illus::Options o = illusOptions(sc.w, sc.h);
    illus_.saving = true;
    illus_.status = "Saving " + path + "...";
    JobSystem* jobs = jobs_.get();
    jobs_->submit("illustration", [this, jobs, parts, o, path, svg] {
        illus::Result r = illus::render(parts, o, svg);
        bool ok;
        if (svg) {
            std::ofstream f(u8path(path), std::ios::binary | std::ios::trunc);
            f << r.svg;
            ok = (bool)f;
        } else {
            ok = illus::writePng(path, r.rgba, r.width, r.height);
        }
        jobs->post([this, ok, path] {
            illus_.saving = false;
            illus_.status = ok ? "Saved " + path : "Could not write " + path;
            if (ok) {
                toast("Illustration saved", ToastKind::Success);
                platform::revealInExplorer(path);
            } else {
                toast("Could not save the illustration", ToastKind::Error);
            }
            log::info("Illustration: " + illus_.status);
        });
    }, [this](const std::string& e) { illus_.saving = false; illus_.status = "Failed: " + e; });
}

void App::drawIllustration() {
    if (!illusTest_.empty() && recomputeQueuedFrom_ < 0 && !recomputeRunning_ && !recomputeResults_ && ImGui::GetFrameCount() > 30) {
        std::string base = illusTest_;
        illusTest_.clear();
        auto parts = illusParts(false, {});
        for (int st = 0; st < 4; ++st) {
            illus::Options o = illusOptions(1600, 900);
            o.style = (illus::Style)st;
            illus::Result r = illus::render(parts, o, st == 2 || st == 1);
            illus::writePng(base + std::to_string(st) + ".png", r.rgba, r.width, r.height);
            if (!r.svg.empty()) { std::ofstream f(base + std::to_string(st) + ".svg"); f << r.svg; }
            log::info("illustration test " + std::to_string(st) + ": " + std::to_string(parts.size()) + " parts, " + std::to_string(r.partsNumbered) + " numbered");
        }
    }
    if (!illus_.open) {
        if (illus_.tex) { dropTexture(illus_.tex); illus_.tex = nullptr; }
        return;
    }
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(1180 * S, 680 * S), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Illustration", &illus_.open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowAppearing()) illus_.dirty = true;
    // A design that changed (or the view, for Current view) makes a new preview.
    uint64_t sig = doc_.revision * 31 + selection_.size() * 7;
    if (illus_.opt.view == illus::View::Current) {
        glm::vec3 f = cam_.forward();
        sig += (uint64_t)std::lround((f.x * 1000 + f.y * 100000 + f.z * 10000000));
    }
    if (sig != illus_.sig) { illus_.sig = sig; illus_.dirty = true; }
    if (illus_.dirty && !illus_.busy) illusPreview();

    float optW = 320 * S;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    // ---- preview
    ImGui::BeginChild("##ilprev", ImVec2(avail.x - optW - 8 * S, avail.y), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos(), sz = ImGui::GetContentRegionAvail();
        // A checker under transparent pictures, like image editors.
        if (illus_.opt.background == illus::Background::Transparent) {
            float c = 12 * S;
            for (float y = 0; y < sz.y; y += c)
                for (float x = 0; x < sz.x; x += c)
                    dl->AddRectFilled(ImVec2(p0.x + x, p0.y + y), ImVec2(p0.x + std::min(x + c, sz.x), p0.y + std::min(y + c, sz.y)),
                                      (((int)(x / c) + (int)(y / c)) & 1) ? IM_COL32(200, 200, 205, 255) : IM_COL32(235, 235, 240, 255));
        }
        if (illus_.tex && illus_.texW > 0) {
            float s = std::min(sz.x / illus_.texW, sz.y / illus_.texH);
            ImVec2 isz(illus_.texW * s, illus_.texH * s);
            ImVec2 a(p0.x + (sz.x - isz.x) * 0.5f, p0.y + (sz.y - isz.y) * 0.5f);
            dl->AddImage(illus_.tex->GetTexRef(), a, ImVec2(a.x + isz.x, a.y + isz.y));
            dl->AddRect(a, ImVec2(a.x + isz.x, a.y + isz.y), col::PanelBorder);
        }
        if (illus_.busy) {
            ImVec2 c(p0.x + sz.x - 22 * S, p0.y + 22 * S);
            float ang = (float)ImGui::GetTime() * 5.f;
            dl->PathArcTo(c, 9 * S, ang, ang + 4.2f, 18);
            dl->PathStroke(sl::orangeU(), 0, 2.5f * S);
            activity();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // ---- options
    ImGui::BeginChild("##ilopts", ImVec2(optW, avail.y), ImGuiChildFlags_None);
    bool ch = false;
    auto head = [&](const char* t) {
        ImGui::Spacing();
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
    };
    auto pick = [&](const char* id, int& v, const char* const* items, int n) {
        bool c = false;
        ImGui::SetNextItemWidth(-1);
        if (sl::beginMenuCombo(id, items[std::clamp(v, 0, n - 1)], optW)) {
            for (int i = 0; i < n; ++i)
                if (sl::menuRow(items[i], v == i)) { v = i; c = true; }
            sl::endMenuCombo();
        }
        return c;
    };
    head("View");
    {
        static const char* views[] = {"Isometric", "Front", "Top", "Right", "Left", "Back", "Bottom", "Current view"};
        int v = (int)illus_.opt.view;
        if (pick("##ilview", v, views, 8)) { illus_.opt.view = (illus::View)v; ch = true; }
    }
    head("Style");
    {
        static const char* styles[] = {"Shaded", "Illustration (flat tones)", "Line art", "Shaded, no lines"};
        int v = (int)illus_.opt.style;
        if (pick("##ilstyle", v, styles, 4)) { illus_.opt.style = (illus::Style)v; ch = true; }
        ImGui::TextDisabled("Line weight");
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(illus_.opt.style == illus::Style::ShadedNoLines);
        ImGui::SliderFloat("##ilweight", &illus_.opt.lineWeight, 0.4f, 3.f, "%.1fx");
        if (ImGui::IsItemDeactivatedAfterEdit()) ch = true;
        ImGui::EndDisabled();
    }
    head("Exploded");
    {
        float e = illus_.opt.explode * 100.f;
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##ilexplode", &e, 0.f, 150.f, e < 0.5f ? "assembled" : "%.0f%% apart");
        illus_.opt.explode = e / 100.f;
        if (ImGui::IsItemDeactivatedAfterEdit()) ch = true;
        ImGui::TextDisabled("Parts move away from the middle of the assembly.");
    }
    head("Parts");
    ch |= ImGui::Checkbox("Number the parts (balloons)", &illus_.opt.numbers);
    ImGui::BeginDisabled(!illus_.opt.numbers);
    ch |= ImGui::Checkbox("Parts list", &illus_.opt.partsList);
    ImGui::EndDisabled();
    ch |= ImGui::Checkbox("Only the selected bodies", &illus_.selectedOnly);
    if (illus_.selectedOnly && selection_.empty()) ImGui::TextColored(sl::changed(), "Nothing is selected: select bodies in the browser.");
    if (illus_.opt.numbers && illus_.numbered > 0) ImGui::TextDisabled("%d parts numbered (copies share a number)", illus_.numbered);
    head("Background");
    {
        static const char* bgs[] = {"Transparent", "White", "Dark"};
        int v = (int)illus_.opt.background;
        if (pick("##ilbg", v, bgs, 3)) { illus_.opt.background = (illus::Background)v; ch = true; }
    }
    head("Size");
    {
        const int n = (int)(sizeof kSizes / sizeof kSizes[0]);
        static const char* sizes[n];
        for (int i = 0; i < n; ++i) sizes[i] = kSizes[i].label;
        ch |= pick("##ilsize", illus_.size, sizes, n);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::BeginDisabled(illus_.saving);
    float bw = (optW - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (sl::orangeButton("Save PNG...", ImVec2(bw, 34 * S))) illusSave(false);
    ImGui::SameLine();
    if (ImGui::Button("Save SVG...", ImVec2(bw, 34 * S))) illusSave(true);
    ImGui::EndDisabled();
    ImGui::TextDisabled("SVG keeps the lines and numbers as vectors: they scale and can be edited (Inkscape, Illustrator).");
    if (!illus_.status.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + optW - 4 * S);
        ImGui::TextDisabled("%s", illus_.status.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    if (ch) illus_.dirty = true;
    ImGui::End();
}

} // namespace kst
