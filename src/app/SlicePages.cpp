// The Slice tab's other pages, like OrcaSlicer's: Home (new, open, recent), Project (name, author,
// notes, the plates and filaments), Calibration (tests that print as their own slice tab). Also the
// Prepare tool windows: cut, painting, measure and variable layer height.
#include "app/App.h"
#include "app/SliceStyle.h"
#include "core/Json.h"
#include "platform/Platform.h"
#include "print/OrcaOptions.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace kst {

using namespace ui;
using sl::OIcon;
namespace fs = std::filesystem;

namespace {

void fullPage(App* /*app*/, float S, float top, const char* id) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - top));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::Panel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    (void)S;
}
void endPage() {
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// A big card button (Orca's New Project / Open Project).
bool card(ImDrawList* dl, float S, const char* id, OIcon ic, const char* title, const char* sub, float w) {
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + 100 * S);
    bool c = ImGui::InvisibleButton(id, ImVec2(w, 100 * S));
    bool hv = ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, hv ? col::Hover : col::Card, 8 * S);
    dl->AddRect(a, b, hv ? sl::orangeU() : col::PanelBorder, 8 * S);
    ImVec2 ic0(a.x + 24 * S, a.y + 26 * S);
    dl->AddRect(ic0, ImVec2(ic0.x + 48 * S, ic0.y + 48 * S), sl::orangeU(), 8 * S, 0, 1.5f * S);
    sl::drawOIcon(dl, ImVec2(ic0.x + 24 * S, ic0.y + 24 * S), 26 * S, ic, sl::orangeU());
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.15f, ImVec2(a.x + 90 * S, a.y + 30 * S), col::Text, title);
    dl->AddText(ImVec2(a.x + 90 * S, a.y + 56 * S), col::TextDim, sub);
    return c;
}

std::string ago(fs::file_time_type t) {
    auto now = fs::file_time_type::clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - t).count();
    char b[64];
    if (secs < 3600) std::snprintf(b, sizeof b, "%lld min ago", (long long)std::max<long long>(1, secs / 60));
    else if (secs < 86400 * 2) std::snprintf(b, sizeof b, "%lld h ago", (long long)(secs / 3600));
    else std::snprintf(b, sizeof b, "%lld days ago", (long long)(secs / 86400));
    return b;
}

} // namespace

// ===================================================================== Home

void App::drawSliceHome() {
    const float S = uiScale_;
    fullPage(this, S, sliceTop(), "##slicehome");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    // Left: Kestral and the printer (Orca's account column).
    float leftW = 260 * S;
    dl->AddRectFilled(wp, ImVec2(wp.x + leftW, wp.y + ws.y), col::Card);
    ui::drawLogo(dl, ImVec2(wp.x + leftW * 0.5f - 34 * S, wp.y + 26 * S), 68 * S);
    {
        const char* t = "Kestral";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(fonts_.bold, ImGui::GetFontSize() * 1.2f, ImVec2(wp.x + (leftW - ts.x * 1.2f) * 0.5f, wp.y + 104 * S), col::Text, t);
        print::PrinterConn c = printerConn();
        std::string pn = c.kind == print::PrinterKind::None ? std::string("No printer set up") : (c.name.empty() ? std::string("Printer") : c.name);
        ImVec2 ps = ImGui::CalcTextSize(pn.c_str());
        dl->AddText(ImVec2(wp.x + (leftW - ps.x) * 0.5f, wp.y + 132 * S), col::TextDim, pn.c_str());
        ImGui::SetCursorScreenPos(ImVec2(wp.x + leftW * 0.5f - 60 * S, wp.y + 158 * S));
        if (ImGui::Button(c.kind == print::PrinterKind::None ? "Set up a printer" : "Printer setup", ImVec2(120 * S, 0))) settingsOpen_ = true;
        dl->AddLine(ImVec2(wp.x, wp.y + 200 * S), ImVec2(wp.x + leftW, wp.y + 200 * S), col::Divider);
        ImVec2 r0(wp.x, wp.y + 206 * S);
        dl->AddRectFilled(r0, ImVec2(r0.x + leftW, r0.y + 40 * S), sl::orangeU(60));
        sl::drawOIcon(dl, ImVec2(r0.x + 24 * S, r0.y + 20 * S), 16 * S, OIcon::Folder, col::Text);
        dl->AddText(ImVec2(r0.x + 44 * S, r0.y + 20 * S - ImGui::GetFontSize() * 0.5f), col::Text, "Recent");
    }
    float x0 = wp.x + leftW + 40 * S, y0 = wp.y + 30 * S;
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    if (card(dl, S, "##new", OIcon::Plus, "New Project", "An empty plate in a new tab", 280 * S)) {
        // A fresh slice tab with nothing on it (add models with Add, or open a design).
        SliceDocState d;
        static int n = 1;
        d.key = "project:" + std::to_string(n);
        d.name = "Project " + std::to_string(n++);
        sliceDocs_.push_back(std::move(d));
        sliceActivate((int)sliceDocs_.size() - 1);
        setSliceTop(1);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0 + 300 * S, y0));
    if (card(dl, S, "##open", OIcon::Folder, "Open Project", "A model onto the plate (3MF, STL, STEP, OBJ)", 300 * S)) {
        setSliceTop(1);
        plateAddFromFile();
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 130 * S));
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted("Recently opened");
    ImGui::PopFont();
    // Designs (their slice tab) and sliced prints (their preview).
    float cw = 170 * S, ch = 200 * S, gap = 14 * S;
    int cols = std::max(1, (int)((ws.x - leftW - 80 * S + gap) / (cw + gap)));
    std::vector<std::string> recent;
    std::error_code ec;
    for (auto& r : settings_.recentFiles)
        if (fs::exists(u8path(r), ec)) recent.push_back(r);
    std::vector<std::pair<fs::file_time_type, std::string>> prints;
    for (auto it = fs::directory_iterator(u8path(platform::tempDir() + "/slice"), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string nm = pathToU8(it->path().filename());
        if (nm.size() > 10 && nm.rfind(".gcode.3mf") == nm.size() - 10) prints.push_back({it->last_write_time(ec), pathToU8(it->path())});
    }
    std::sort(prints.begin(), prints.end(), [](auto& a, auto& b) { return a.first > b.first; });
    int i = 0;
    float gy = y0 + 160 * S;
    auto cell = [&](const std::string& title, const std::string& sub, ImTextureData* tex, bool& clicked) {
        int r = i / cols, c = i % cols;
        ImVec2 a(x0 + c * (cw + gap), gy + r * (ch + gap));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i + 7000);
        clicked = ImGui::InvisibleButton("##rc", ImVec2(cw, ch));
        bool hv = ImGui::IsItemHovered();
        ImGui::PopID();
        dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + cw), hv ? col::Hover : col::Card, 6 * S);
        if (tex) dl->AddImage(tex->GetTexRef(), ImVec2(a.x + 8 * S, a.y + 8 * S), ImVec2(a.x + cw - 8 * S, a.y + cw - 8 * S));
        else sl::drawOIcon(dl, ImVec2(a.x + cw * 0.5f, a.y + cw * 0.5f), cw * 0.35f, OIcon::Prepare, col::TextDim);
        if (hv) dl->AddRect(a, ImVec2(a.x + cw, a.y + cw), sl::orangeU(), 6 * S, 0, 1.5f * S);
        std::string t = title.size() > 22 ? title.substr(0, 21) + "..." : title;
        dl->AddText(ImVec2(a.x, a.y + cw + 6 * S), col::Text, t.c_str());
        dl->AddText(ImVec2(a.x, a.y + cw + 24 * S), col::TextDim, sub.c_str());
        if (hv) ImGui::SetTooltip("%s", title.c_str());
        ++i;
    };
    for (auto& p : recent) {
        if (i >= cols * 2) break;
        bool cl = false;
        cell(pathToU8(u8path(p).stem()), "Design: open and slice", thumbFor(p), cl);
        if (cl) {
            leaveSliceWorkspace();
            openDocumentPath(p);
            enterSliceWorkspace();
            setSliceTop(1);
        }
    }
    for (auto& [t, p] : prints) {
        if (i >= cols * 3) break;
        bool cl = false;
        std::string nm = pathToU8(u8path(p).filename());
        nm = nm.substr(0, nm.size() - 10);
        cell(nm, "Sliced " + ago(t), sliceThumbFor(p), cl);
        if (cl) {
            print::SliceResult r;
            r.ok = true;
            r.gcode3mf = p;
            std::string info = print::readZipEntry(p, "Metadata/slice_info.config");
            auto meta = [&](const char* key) {
                size_t k = info.find(std::string("key=\"") + key + "\"");
                if (k == std::string::npos) return 0.0;
                size_t v = info.find("value=\"", k);
                return v == std::string::npos ? 0.0 : std::atof(info.c_str() + v + 7);
            };
            r.seconds = meta("prediction");
            r.grams = meta("weight");
            {
                std::lock_guard lock(print_.m);
                print_.last = r;
                print_.lastName = nm;
            }
            loadSlicePreview(p);
            setSliceTop(2);
        }
    }
    if (i == 0) {
        ImGui::SetCursorScreenPos(ImVec2(x0, gy));
        ImGui::TextDisabled("Designs you open and prints you slice show up here.");
    }
    endPage();
}

// ===================================================================== Project

void App::drawSliceProject() {
    const float S = uiScale_;
    fullPage(this, S, sliceTop(), "##sliceproject");
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    float w = std::min(820 * S, ws.x - 80 * S);
    ImGui::SetCursorScreenPos(ImVec2(wp.x + (ws.x - w) * 0.5f, wp.y + 30 * S));
    ImGui::BeginChild("##proj", ImVec2(w, ws.y - 60 * S));
    auto& ex = slice_.ex;
    std::string tabName = sliceActive_ >= 0 && sliceActive_ < (int)sliceDocs_.size() ? sliceDocs_[(size_t)sliceActive_].name : doc_.name;
    ImGui::PushFont(fonts_.bold, ImGui::GetFontSize() * 1.3f);
    ImGui::TextUnformatted(ex.projName.empty() ? tabName.c_str() : ex.projName.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("Model information of this print project (saved with the tab; the name is the print's name on the printer).");
    ImGui::Spacing();
    float lw = 120 * S;
    auto field = [&](const char* label, std::string& v, const char* hint) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint((std::string("##") + label).c_str(), hint, &v);
    };
    field("Project name", ex.projName, tabName.c_str());
    field("Author", ex.projAuthor, "Your name");
    {
        const char* lic[] = {"", "CC BY", "CC BY-SA", "CC BY-NC", "CC BY-NC-SA", "CC BY-ND", "CC BY-NC-ND", "CC0", "GPL", "MIT", "All rights reserved"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("License");
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##lic", ex.projLicense.empty() ? "None" : ex.projLicense.c_str())) {
            for (auto* l : lic)
                if (ImGui::Selectable(*l ? l : "None", ex.projLicense == l)) ex.projLicense = l;
            ImGui::EndCombo();
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Description");
    ImGui::SameLine(lw);
    ImGui::InputTextMultiline("##desc", &ex.projDesc, ImVec2(-1, 120 * S));
    ImGui::Spacing();
    ImGui::SeparatorText("Plates");
    for (int p = 0; p < slice_.plates; ++p) {
        int parts = 0;
        for (auto& [id, o] : slice_.plate)
            if (o.onPlate)
                for (int k = 0; k < (int)o.pos.size(); ++k) parts += plateOfCopy(o, k) == p;
        const print::SliceResult* r = p < (int)ex.plateResults.size() && ex.plateResults[(size_t)p].ok ? &ex.plateResults[(size_t)p] : nullptr;
        ImGui::PushID(p);
        ImVec2 a = ImGui::GetCursorScreenPos();
        float th = 64 * S;
        ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + th, a.y + th), col::Card, 4 * S);
        if (r) if (ImTextureData* t = sliceThumbFor(r->gcode3mf)) ImGui::GetWindowDrawList()->AddImage(t->GetTexRef(), a, ImVec2(a.x + th, a.y + th));
        ImGui::Dummy(ImVec2(th, th));
        ImGui::SameLine(th + 14 * S);
        ImGui::BeginGroup();
        ImGui::Text("Plate %d: %s", p + 1, plateName(p).c_str());
        ImGui::TextDisabled("%d part%s%s", parts, parts == 1 ? "" : "s", r ? ("   " + print::formatDuration(r->seconds) + "   " + std::to_string((int)std::lround(r->grams)) + " g").c_str() : "   not sliced");
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::SeparatorText("Filaments");
    for (int f = 0; f < (int)ex.fils.size(); ++f) {
        ImVec2 a = ImGui::GetCursorScreenPos();
        float fh = ImGui::GetFrameHeight();
        glm::vec3 c = sliceFilColor(f);
        sl::filChip(ImGui::GetWindowDrawList(), a, fh, c.r, c.g, c.b, f + 1);
        ImGui::Dummy(ImVec2(fh, fh));
        ImGui::SameLine();
        int users = 0;
        for (auto& [id, o] : slice_.plate) users += o.onPlate && o.fil == f;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s   (%d part%s)", sliceFilPreset(f).c_str(), users, users == 1 ? "" : "s");
    }
    ImGui::EndChild();
    endPage();
}

// ===================================================================== Calibration

MeshPtr App::plateBoxMesh(float w, float d, float h) {
    auto m = std::make_shared<MeshData>();
    glm::vec3 lo(-w * 0.5f, -d * 0.5f, 0.f), hi(w * 0.5f, d * 0.5f, h);
    const glm::vec3 n[6] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
    auto corner = [&](int i) { return glm::vec3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z); };
    // Each face: 4 corners in counter clockwise order seen from outside.
    const int q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; ++f) {
        uint32_t base = (uint32_t)m->vertexCount();
        for (int k = 0; k < 4; ++k) {
            glm::vec3 p = corner(q[f][k]);
            m->verts.insert(m->verts.end(), {p.x, p.y, p.z, n[f].x, n[f].y, n[f].z});
        }
        m->indices.insert(m->indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        m->triFace.push_back((uint32_t)f);
        m->triFace.push_back((uint32_t)f);
    }
    m->faceCount = 6;
    m->bounds.add(lo);
    m->bounds.add(hi);
    // Its outline, so the part reads clearly on the plate.
    for (int e = 0; e < 12; ++e) {
        static const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        glm::vec3 a = corner(E[e][0]), b = corner(E[e][1]);
        m->edgeVerts.insert(m->edgeVerts.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
        m->edgeCount++;
    }
    return m;
}

void App::sliceStartCalibration(int kind, int pass) {
    // A test prints in its own slice tab: its pieces, and what each piece tests.
    SliceDocState d;
    static int n = 1;
    const char* names[] = {"", "Flow ratio", "Temperature tower", "Pressure advance tower"};
    d.key = "calibration:" + std::to_string(n++);
    d.name = std::string("Calibration: ") + names[std::clamp(kind, 1, 3)] + (kind == 1 ? (pass == 2 ? " (fine)" : " (coarse)") : "");
    sliceDocs_.push_back(std::move(d));
    if (!sliceWs_) enterSliceWorkspace((int)sliceDocs_.size() - 1);
    else sliceActivate((int)sliceDocs_.size() - 1);
    slice_.ex.calKind = kind;
    slice_.ex.calValues.clear();
    std::string key = sliceDocs_[(size_t)sliceActive_].key;
    double lh = std::atof(sliceValue("layer_height").c_str());
    double flh = std::atof(sliceValue("initial_layer_print_height").c_str());
    if (lh <= 0.01) lh = 0.2;
    if (flh <= 0.01) flh = lh;
    // layer_num in OrcaSlicer's G-code templates counts from 0 (the first layer).
    auto layerAt = [&](double z) { return std::max(0, (int)std::lround((z - flh) / lh) + 1); };
    if (kind == 1) {
        // Orca's flow ratio test: pads printed with slightly less and more flow; the smoothest top wins.
        std::vector<float> mods;
        if (pass == 2) for (int i = -9; i <= 0; ++i) mods.push_back(i / 100.f);
        else for (int i = -20; i <= 20; i += 5) mods.push_back(i / 100.f);
        for (float m : mods) {
            char nm[32];
            std::snprintf(nm, sizeof nm, "%+d%%", (int)std::lround(m * 100));
            uint32_t oid = plateAddPart(key, plateBoxMesh(22.f, 22.f, 3.f), nullptr, glm::mat4(1.f), nm, glm::vec3(0.72f, 0.74f, 0.77f));
            auto& o = slice_.plate[oid];
            char v[32];
            std::snprintf(v, sizeof v, "%.3f", 1.0 + m);
            o.settings["print_flow_ratio"] = v;
            o.settings["top_surface_pattern"] = "monotonic";
            o.settings["top_shell_layers"] = "5";
            o.settings["bottom_shell_layers"] = "1";
            o.settings["sparse_infill_density"] = "35%";
            slice_.ex.calValues.push_back(m);
        }
    } else if (kind == 2) {
        // A tower of 10 mm blocks, hottest at the bottom, 5 C cooler each block.
        double t0 = std::atof(sliceValue("nozzle_temperature").c_str());
        if (t0 < 150) t0 = 220;
        int blocks = 7;
        double top = t0 + 15;
        uint32_t oid = plateAddPart(key, plateBoxMesh(20.f, 20.f, 10.f * blocks), nullptr, glm::mat4(1.f), "Temperature tower", glm::vec3(0.72f, 0.74f, 0.77f));
        slice_.plate[oid].settings["sparse_infill_density"] = "15%";
        std::string g = sliceValue("layer_change_gcode");
        for (int b = 0; b < blocks; ++b) {
            double t = top - 5.0 * b;
            slice_.ex.calValues.push_back((float)t);
            if (b == 0) continue;
            g += "\n{if layer_num == " + std::to_string(layerAt(10.0 * b)) + "}M104 S" + std::to_string((int)t) + " ; Kestral temperature tower{endif}";
        }
        slice_.ex.tabOverrides["layer_change_gcode"] = g;
        slice_.ex.tabOverrides["nozzle_temperature"] = std::to_string((int)top);
        slice_.ex.tabOverrides["nozzle_temperature_initial_layer"] = std::to_string((int)top);
    } else {
        // Pressure advance: a hollow tower, the value going up every millimetre.
        uint32_t oid = plateAddPart(key, plateBoxMesh(40.f, 40.f, 40.f), nullptr, glm::mat4(1.f), "Pressure advance tower", glm::vec3(0.72f, 0.74f, 0.77f));
        auto& o = slice_.plate[oid];
        o.settings["sparse_infill_density"] = "0%";
        o.settings["top_shell_layers"] = "0";
        o.settings["bottom_shell_layers"] = "2";
        o.settings["wall_loops"] = "2";
        // Bambu printers take M900 (their profiles say "klipper" but the firmware is Bambu's own).
        bool klipper = sliceValue("gcode_flavor") == "klipper" && slice_.machine.find("Bambu") == std::string::npos;
        double step = 0.002;
        std::string g = sliceValue("layer_change_gcode");
        for (int mm = 0; mm < 40; ++mm) {
            double k = mm * step;
            slice_.ex.calValues.push_back((float)k);
            char line[128];
            if (klipper) std::snprintf(line, sizeof line, "SET_PRESSURE_ADVANCE ADVANCE=%.4f", k);
            else std::snprintf(line, sizeof line, "M900 K%.4f L1000 M10", k);
            g += "\n{if layer_num == " + std::to_string(layerAt(std::max(flh, (double)mm))) + "}" + line + " ; Kestral PA tower{endif}";
        }
        slice_.ex.tabOverrides["layer_change_gcode"] = g;
    }
    sliceAutoFils();
    plateArrangeOne(slice_.activePlate);
    setSliceTop(1);
    toast("The test is on the plate: Slice plate, then Print plate. Come back to Calibration to save the result.", ToastKind::Success);
}

void App::drawSliceCalibration() {
    const float S = uiScale_;
    fullPage(this, S, sliceTop(), "##slicecal");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    float leftW = 230 * S;
    dl->AddRectFilled(wp, ImVec2(wp.x + leftW, wp.y + ws.y), col::Card);
    const char* pages[] = {"Flow Dynamics", "Flow ratio", "Temperature", "Pressure advance"};
    for (int i = 0; i < 4; ++i) {
        ImVec2 a(wp.x, wp.y + 12 * S + i * 40 * S);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i + 8000);
        if (ImGui::InvisibleButton("##cp", ImVec2(leftW, 40 * S))) slice_.calPage = i;
        bool hv = ImGui::IsItemHovered();
        ImGui::PopID();
        bool on = slice_.calPage == i;
        if (on) dl->AddRectFilled(a, ImVec2(a.x + leftW, a.y + 40 * S), sl::orangeU(200));
        else if (hv) dl->AddRectFilled(a, ImVec2(a.x + leftW, a.y + 40 * S), col::Hover);
        dl->AddText(on ? fonts_.bold : ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + 18 * S, a.y + (40 * S - ImGui::GetFontSize()) * 0.5f), on ? IM_COL32_WHITE : col::Text, pages[i]);
        dl->AddText(ImVec2(a.x + leftW - 20 * S, a.y + (40 * S - ImGui::GetFontSize()) * 0.5f), on ? IM_COL32_WHITE : col::TextDim, ">");
    }
    float cw = std::min(900 * S, ws.x - leftW - 80 * S);
    ImGui::SetCursorScreenPos(ImVec2(wp.x + leftW + 40 * S, wp.y + 30 * S));
    ImGui::BeginChild("##calpage", ImVec2(cw, ws.y - 60 * S));
    ImGui::PushTextWrapPos(cw - 10 * S);
    auto title = [&](const char* t) {
        ImGui::PushFont(fonts_.bold, ImGui::GetFontSize() * 1.3f);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
        ImGui::Spacing();
    };
    auto bold = [&](const char* t) {
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
    };
    // The result of a test tab that is in front: pick the best piece, save it to the filament.
    auto resultBox = [&](int kind, const char* what) {
        ImGui::Spacing();
        bold("Save the result");
        if (slice_.ex.calKind != kind || slice_.ex.calValues.empty()) {
            ImGui::TextDisabled("Print the test first; its tab then shows here to pick the best %s.", what);
            return;
        }
        int& pick = slice_.calPick;
        pick = std::clamp(pick, 0, (int)slice_.ex.calValues.size() - 1);
        std::vector<std::string> labels;
        for (size_t vi = 0; vi < slice_.ex.calValues.size(); ++vi) {
            float v = slice_.ex.calValues[vi];
            char b[48];
            if (kind == 1) std::snprintf(b, sizeof b, "%+d%%", (int)std::lround(v * 100));
            else if (kind == 2) std::snprintf(b, sizeof b, "%d C", (int)v);
            else std::snprintf(b, sizeof b, "%.3f  (at %d mm)", v, (int)vi);
            labels.push_back(b);
        }
        ImGui::SetNextItemWidth(220 * S);
        if (ImGui::BeginCombo("Best one", labels[(size_t)pick].c_str())) {
            for (int i = 0; i < (int)labels.size(); ++i)
                if (ImGui::Selectable(labels[(size_t)i].c_str(), i == pick)) pick = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (sl::orangeButton("Use it")) {
            float v = slice_.ex.calValues[(size_t)pick];
            char b[32];
            if (kind == 1) {
                double base = std::atof(sliceValue("filament_flow_ratio").c_str());
                if (base <= 0.1) base = 0.98;
                std::snprintf(b, sizeof b, "%.3f", base * (1.0 + v));
                sliceSetValue("filament_flow_ratio", b);
            } else if (kind == 2) {
                std::snprintf(b, sizeof b, "%d", (int)v);
                sliceSetValue("nozzle_temperature", b);
                sliceSetValue("nozzle_temperature_initial_layer", b);
            } else {
                std::snprintf(b, sizeof b, "%.4f", v);
                sliceSetValue("pressure_advance", b);
                sliceSetValue("enable_pressure_advance", "1");
            }
            toast(std::string("Saved: ") + b + " (Filament settings; save it into a profile there to keep it)", ToastKind::Success);
        }
    };
    if (slice_.calPage == 0) {
        title("Flow Dynamics Calibration");
        bold("When do you need Flow Dynamics Calibration");
        ImGui::TextWrapped("Bambu printers calibrate flow dynamics (pressure advance) themselves before a print when this is on. "
                           "You only need it when you use a new filament (another brand or type, or damp filament), after changing the nozzle, "
                           "or when the filament's max volumetric speed or temperature changed.");
        ImGui::Spacing();
        ImGui::Checkbox("Calibrate before each print (sent with Print plate)", &slice_.sendFlowCali);
        ImGui::Spacing();
        bold("Manual calibration");
        ImGui::TextWrapped("Other printers, or to set it by hand: the pressure advance tower (next pages) prints a value per millimetre; "
                           "the height where the corners look sharpest gives the value.");
        if (sl::orangeButton("Manual calibration (pressure advance tower)")) sliceStartCalibration(3, 1);
        ImGui::SameLine();
        if (ImGui::Button("Wiki guide")) platform::openUrl("https://wiki.bambulab.com/en/software/bambu-studio/calibration_pa");
    } else if (slice_.calPage == 1) {
        title("Flow Rate Calibration");
        ImGui::TextWrapped("Pads printed with a little less and a little more flow than now. The one with the smoothest top (no gaps, no ridges) "
                           "tells the filament's right flow ratio. Do the coarse pass first, then the fine one around the best pad.");
        ImGui::Spacing();
        if (sl::orangeButton("Pass 1 (coarse: -20% to +20%)")) sliceStartCalibration(1, 1);
        ImGui::SameLine();
        if (sl::orangeButton("Pass 2 (fine: -9% to 0%)")) sliceStartCalibration(1, 2);
        ImGui::TextDisabled("Now: filament flow ratio %s", sliceValue("filament_flow_ratio").c_str());
        resultBox(1, "pad");
    } else if (slice_.calPage == 2) {
        title("Temperature Tower");
        ImGui::TextWrapped("A tower of 10 mm blocks: the bottom block prints 15 C hotter than the filament profile, each block above 5 C cooler. "
                           "Pick the block with the best surface, bridges and strength.");
        ImGui::Spacing();
        if (sl::orangeButton("Make the temperature tower")) sliceStartCalibration(2, 1);
        ImGui::TextDisabled("Now: nozzle %s C", sliceValue("nozzle_temperature").c_str());
        resultBox(2, "block");
    } else {
        title("Pressure Advance Tower");
        ImGui::TextWrapped("A hollow tower whose pressure advance goes up by 0.002 every millimetre. Measure the height where the corners look best "
                           "(sharp, no bulge, no gap): that height times 0.002 is the value.");
        ImGui::Spacing();
        if (sl::orangeButton("Make the pressure advance tower")) sliceStartCalibration(3, 1);
        ImGui::TextDisabled("Now: pressure advance %s (%s)", sliceValue("pressure_advance").c_str(), sliceValue("enable_pressure_advance") == "1" ? "on" : "off");
        resultBox(3, "height");
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    endPage();
}

// ===================================================================== Prepare tool windows

bool App::plateRayHit(uint32_t oid, int copy, ImVec2 px, glm::vec3& out) {
    auto it = slice_.plate.find(oid);
    if (it == slice_.plate.end() || !it->second.mesh) return false;
    glm::dvec3 o, d;
    if (!rayAt(px, o, d)) return false;
    glm::mat4 X = plateXform(oid, copy);
    const MeshData& m = *it->second.mesh;
    double best = 1e30;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        auto v = [&](uint32_t i) { return glm::dvec3(X * glm::vec4(m.verts[i * 6], m.verts[i * 6 + 1], m.verts[i * 6 + 2], 1.f)); };
        glm::dvec3 a = v(m.indices[t]), b = v(m.indices[t + 1]), c = v(m.indices[t + 2]);
        glm::dvec3 e1 = b - a, e2 = c - a, p = glm::cross(d, e2);
        double det = glm::dot(e1, p);
        if (std::abs(det) < 1e-12) continue;
        double inv = 1.0 / det;
        glm::dvec3 s = o - a;
        double u = glm::dot(s, p) * inv;
        if (u < 0 || u > 1) continue;
        glm::dvec3 q = glm::cross(s, e1);
        double vv = glm::dot(d, q) * inv;
        if (vv < 0 || u + vv > 1) continue;
        double tt = glm::dot(e2, q) * inv;
        if (tt > 0 && tt < best) best = tt;
    }
    if (best >= 1e30) return false;
    out = glm::vec3(o + d * best);
    return true;
}

MeshPtr App::platePaintMesh(SliceTab::PlateObj& o) {
    // The part's mesh with its painted faces in their colors (color painting always; support and
    // seam painting while their tool is open, like Orca).
    bool sup = slice_.paintMode == 2 || slice_.paintMode == 3, seam = slice_.paintMode == 4;
    if (!o.mesh || (o.paintColor.empty() && !(sup && !o.paintSupport.empty()) && !(seam && !o.paintSeam.empty()))) return o.mesh;
    uint64_t key = (uint64_t)o.paintColor.size() * 1315423911ull ^ (uint64_t)o.paintSupport.size() * 2654435761ull ^ (uint64_t)o.paintSeam.size() * 40503ull ^
                   (uint64_t)(sup ? 1 : 0) << 60 ^ (uint64_t)(seam ? 1 : 0) << 61;
    for (auto& [f, v] : o.paintColor) key = key * 31 + f * 7 + (uint64_t)v;
    for (auto& [f, v] : o.paintSupport) key = key * 37 + f * 5 + (uint64_t)v;
    for (auto& [f, v] : o.paintSeam) key = key * 41 + f * 3 + (uint64_t)v;
    for (auto& fl : slice_.ex.fils) key = key * 43 + (uint64_t)(fl.color.r * 255) + ((uint64_t)(fl.color.g * 255) << 8) + ((uint64_t)(fl.color.b * 255) << 16);
    if (o.paintMesh && o.paintKey == key) return o.paintMesh;
    auto m = std::make_shared<MeshData>(*o.mesh);
    m->faceColors.assign(std::max<uint32_t>(m->faceCount, 1), 0u);
    auto rgba = [](glm::vec3 c) {
        return 0xFF000000u | (uint32_t)(std::clamp(c.r, 0.f, 1.f) * 255) | ((uint32_t)(std::clamp(c.g, 0.f, 1.f) * 255) << 8) | ((uint32_t)(std::clamp(c.b, 0.f, 1.f) * 255) << 16);
    };
    for (auto& [f, v] : o.paintColor)
        if (f < m->faceColors.size()) m->faceColors[f] = rgba(sliceFilColor(std::clamp(v - 1, 0, (int)slice_.ex.fils.size() - 1)));
    if (sup)
        for (auto& [f, v] : o.paintSupport)
            if (f < m->faceColors.size()) m->faceColors[f] = v == 1 ? rgba(glm::vec3(0.2f, 0.85f, 0.3f)) : rgba(glm::vec3(0.9f, 0.2f, 0.2f));
    if (seam)
        for (auto& [f, v] : o.paintSeam)
            if (f < m->faceColors.size()) m->faceColors[f] = v == 1 ? rgba(glm::vec3(0.25f, 0.55f, 1.f)) : rgba(glm::vec3(0.9f, 0.2f, 0.2f));
    o.paintMesh = m;
    o.paintKey = key;
    return m;
}

void App::drawSliceToolPanels() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float left = sliceSideW();
    ImVec2 at(vp->Pos.x + left + (vp->Size.x - left) * 0.5f, vp->Pos.y + sliceTop() + 58 * S);
    auto begin = [&](const char* id) {
        ImGui::SetNextWindowPos(at, ImGuiCond_Always, ImVec2(0.5f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7 * S);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * S, 10 * S));
        ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    };
    auto end = [&] {
        ImGui::End();
        ImGui::PopStyleVar(2);
    };
    // ---- cut
    if (slice_.cutMode) {
        auto it = slice_.plate.find(slice_.plateSel);
        if (it == slice_.plate.end()) { slice_.cutMode = false; }
        else {
            const Aabb& cb = plateCore(slice_.plateSel);
            float hgt = cb.valid() ? cb.max.z - cb.min.z : 10.f;
            slice_.cutZ = std::clamp(slice_.cutZ, 0.1f, std::max(0.2f, hgt - 0.1f));
            begin("##cutpanel");
            ImGui::PushFont(fonts_.bold, 0.f);
            ImGui::Text("Cut  %s", it->second.name.c_str());
            ImGui::PopFont();
            ImGui::SetNextItemWidth(240 * S);
            ImGui::SliderFloat("Height", &slice_.cutZ, 0.1f, std::max(0.2f, hgt - 0.1f), "%.2f mm");
            ImGui::RadioButton("Keep both", &slice_.cutKeep, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Upper", &slice_.cutKeep, 1);
            ImGui::SameLine();
            ImGui::RadioButton("Lower", &slice_.cutKeep, 2);
            if (sl::orangeButton("Perform cut", ImVec2(130 * S, 0))) plateCutSelected();
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(80 * S, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) slice_.cutMode = false;
            end();
            // The cut plane over the part.
            if (slice_.cutMode && cb.valid()) {
                int k = slice_.plateSelCopy;
                glm::vec2 c = it->second.pos.empty() ? glm::vec2(0) : it->second.pos[(size_t)std::clamp(k, 0, (int)it->second.pos.size() - 1)] + plateOffset(plateOfCopy(it->second, k));
                glm::vec2 hs = glm::vec2(cb.max - cb.min) * 0.5f + glm::vec2(6.f);
                glm::dvec3 q[4] = {{c.x - hs.x, c.y - hs.y, slice_.cutZ}, {c.x + hs.x, c.y - hs.y, slice_.cutZ}, {c.x + hs.x, c.y + hs.y, slice_.cutZ}, {c.x - hs.x, c.y + hs.y, slice_.cutZ}};
                ImVec2 s[4];
                bool ok = true;
                for (int i = 0; i < 4; ++i) ok &= worldToScreen(q[i], s[i]);
                if (ok) {
                    ImDrawList* fg = ImGui::GetForegroundDrawList();
                    fg->AddConvexPolyFilled(s, 4, sl::orangeU(60));
                    fg->AddPolyline(s, 4, sl::orangeU(), ImDrawFlags_Closed, 2.f * S);
                }
            }
        }
    }
    // ---- painting
    if (slice_.paintMode) {
        const char* names[] = {"", "Color painting", "Support painting", "Support painting", "Seam painting"};
        begin("##paintpanel");
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted(names[std::clamp(slice_.paintMode, 0, 4)]);
        ImGui::PopFont();
        if (slice_.paintMode == 1) {
            ImGui::TextDisabled("Left click a face: paint it with the filament below. Right click: back to the part's filament.");
            sliceFilsEnsure();
            float fh = ImGui::GetFrameHeight();
            for (int f = 0; f < (int)slice_.ex.fils.size(); ++f) {
                if (f) ImGui::SameLine(0, 4 * S);
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushID(f + 9100);
                if (ImGui::InvisibleButton("##pf", ImVec2(fh, fh))) slice_.paintFil = f + 1;
                ImGui::PopID();
                glm::vec3 c = sliceFilColor(f);
                sl::filChip(ImGui::GetWindowDrawList(), p, fh, c.r, c.g, c.b, f + 1);
                if (slice_.paintFil == f + 1) ImGui::GetWindowDrawList()->AddRect(ImVec2(p.x - 2 * S, p.y - 2 * S), ImVec2(p.x + fh + 2 * S, p.y + fh + 2 * S), sl::orangeU(), 3 * S, 0, 2 * S);
            }
            ImGui::SameLine(0, 8 * S);
            if (ImGui::SmallButton("+ filament")) sliceAddFil(slice_.filament, slicePaletteColor(sliceNewFilColor()), false);
        } else if (slice_.paintMode == 4) {
            ImGui::TextDisabled("Left click a face: the seam goes there. Right click: keep the seam off it.");
        } else {
            ImGui::TextDisabled("Left click a face: always support it. Right click: never support it.");
            ImGui::TextDisabled("Supports are %s in the process settings.", sliceValue("enable_support") == "1" ? "on" : "off (painted faces still get support)");
        }
        if (ImGui::Button("Clear all")) {
            for (auto& [id, o] : slice_.plate) {
                if (slice_.paintMode == 1) o.paintColor.clear();
                else if (slice_.paintMode == 4) o.paintSeam.clear();
                else o.paintSupport.clear();
            }
            sceneDirty_ = true;
        }
        ImGui::SameLine();
        if (sl::orangeButton("Done") || ImGui::IsKeyPressed(ImGuiKey_Escape)) { slice_.paintMode = 0; sceneDirty_ = true; }
        end();
        at.y += 120 * S;
    }
    // ---- measure
    if (slice_.measure) {
        begin("##measurepanel");
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted("Measure");
        ImGui::PopFont();
        if (slice_.measurePts.size() < 2) ImGui::TextDisabled("Click two points on the parts.");
        else {
            glm::vec3 a = slice_.measurePts[0], b = slice_.measurePts[1], d = b - a;
            ImGui::Text("Distance  %.2f mm", glm::length(d));
            ImGui::TextDisabled("X %.2f   Y %.2f   Z %.2f", std::abs(d.x), std::abs(d.y), std::abs(d.z));
        }
        if (ImGui::Button("Clear")) slice_.measurePts.clear();
        ImGui::SameLine();
        if (sl::orangeButton("Done") || ImGui::IsKeyPressed(ImGuiKey_Escape)) { slice_.measure = false; slice_.measurePts.clear(); }
        end();
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        ImVec2 q[2];
        for (size_t i = 0; i < slice_.measurePts.size() && i < 2; ++i)
            if (worldToScreen(glm::dvec3(slice_.measurePts[i]), q[i])) fg->AddCircleFilled(q[i], 5 * S, sl::orangeU());
        if (slice_.measurePts.size() >= 2 && worldToScreen(glm::dvec3(slice_.measurePts[0]), q[0]) && worldToScreen(glm::dvec3(slice_.measurePts[1]), q[1]))
            fg->AddLine(q[0], q[1], sl::orangeU(), 2.f * S);
        at.y += 100 * S;
    }
    // ---- variable layer height: height ranges with their own layer height (Orca's height range modifier).
    if (slice_.varLayer) {
        auto it = slice_.plate.find(slice_.plateSel);
        if (it == slice_.plate.end()) slice_.varLayer = false;
        else {
            begin("##varlayer");
            ImGui::PushFont(fonts_.bold, 0.f);
            ImGui::Text("Layer height  %s", it->second.name.c_str());
            ImGui::PopFont();
            ImGui::TextDisabled("This object prints with its own layer height (the process has %s mm).", sliceValue("layer_height").c_str());
            auto& st = it->second.settings;
            float lh = st.count("layer_height") ? (float)std::atof(st["layer_height"].c_str()) : (float)std::atof(sliceValue("layer_height").c_str());
            ImGui::SetNextItemWidth(200 * S);
            if (ImGui::SliderFloat("Layer height", &lh, 0.04f, 0.32f, "%.2f mm")) {
                char b[16];
                std::snprintf(b, sizeof b, "%.2f", lh);
                st["layer_height"] = b;
            }
            const float presets[] = {0.08f, 0.12f, 0.16f, 0.20f, 0.24f, 0.28f};
            for (int i = 0; i < 6; ++i) {
                if (i) ImGui::SameLine();
                char b[16];
                std::snprintf(b, sizeof b, "%.2f", presets[i]);
                if (ImGui::SmallButton(b)) st["layer_height"] = b;
            }
            if (ImGui::Button("Reset")) st.erase("layer_height");
            ImGui::SameLine();
            if (sl::orangeButton("Done") || ImGui::IsKeyPressed(ImGuiKey_Escape)) slice_.varLayer = false;
            end();
        }
    }
}

// ===================================================================== save a process preset

std::string App::sliceSaveProcessPreset(const std::string& name) {
    if (name.empty()) return "Give it a name";
    if (!slice_.setup) return "The profiles are not read yet";
    const print::Preset* cur = print::findPreset(*slice_.setup, "process", slice_.process);
    if (!cur) return "The process profile is missing";
    const print::Preset* base = print::systemBase(*slice_.setup, *cur);
    std::string err;
    json::Value now = json::Value::parse(print::resolvePreset(*slice_.setup, *cur, err));
    json::Value sys = base ? json::Value::parse(print::resolvePreset(*slice_.setup, *base, err)) : json::Value::object();
    if (!err.empty()) return err;
    // Only what differs from the built in profile it is based on, plus the changes made here.
    json::Value out = json::Value::object();
    out.set("type", json::Value("process"));
    out.set("name", json::Value(name));
    out.set("inherits", json::Value(base ? base->name : std::string()));
    out.set("from", json::Value("User"));
    out.set("is_custom_defined", json::Value("0"));
    out.set("print_settings_id", json::Value(name));
    out.set("version", now["version"].isString() ? now["version"] : json::Value("2.0.0.0"));
    for (auto& [k, v] : now.members()) {
        if (k == "name" || k == "inherits" || k == "from" || k == "type" || k == "print_settings_id" || k == "version" || k == "compatible_printers" ||
            k == "compatible_printers_condition" || k == "instantiation" || k == "setting_id")
            continue;
        if (!sys.has(k) || sys[k].dump() != v.dump()) out.set(k, v);
    }
    std::vector<std::string> applied;
    for (auto& [k, v] : slice_.overrides) {
        if (!now.has(k)) continue;   // printer or filament settings stay where they are
        if (now[k].isArray()) {
            json::Value a = json::Value::array();
            a.push(json::Value(v));
            out.set(k, a);
        } else {
            out.set(k, json::Value(v));
        }
        applied.push_back(k);
    }
    // Where OrcaSlicer keeps your own process presets (next to the ones you have, else user/default).
    std::string dir;
    for (auto& p : slice_.setup->presets)
        if (p.user && p.type == "process") { dir = pathToU8(u8path(p.path).parent_path()); break; }
    if (dir.empty()) dir = slice_.setup->configDir + "/user/default/process";
    std::error_code ec;
    fs::create_directories(u8path(dir), ec);
    std::string safe = name;
    for (auto& ch : safe) if (std::strchr("<>:\"/\\|?*", ch)) ch = '_';
    std::string file = dir + "/" + safe + ".json";
    {
        std::ofstream o(u8path(file), std::ios::binary | std::ios::trunc);
        o << out.dump();
        if (!o) return "Could not write " + file;
    }
    {
        std::ofstream o(u8path(dir + "/" + safe + ".info"), std::ios::binary | std::ios::trunc);
        o << "sync_info = \nuser_id = \nsetting_id = \nbase_id = \nupdated_time = " << (long long)std::time(nullptr) << "\n";
    }
    for (auto& k : applied) slice_.overrides.erase(k);
    slice_.process = name;
    slice_.choicesLoaded = false;   // read the profiles again (the new one shows in the list)
    slice_.choicesFor.clear();
    orcaSetupCached(true);
    sliceLoadChoices();
    return {};
}

} // namespace kst
