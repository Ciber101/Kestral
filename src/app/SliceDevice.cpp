// The Device tab, laid out like OrcaSlicer's: the printer at the left (Status, Storage, Update,
// Assistant), its camera, the controls (temperatures, fans, lamp, moving the head and bed, the
// extruder), the AMS and the print in progress. Also the Send print job window, where each filament
// of the print gets its AMS slot.
#include "app/App.h"
#include "app/SliceStyle.h"
#include "platform/Platform.h"
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>

namespace kst {

using namespace ui;
using sl::OIcon;

namespace {

glm::vec3 hexCol(const std::string& s, glm::vec3 d = glm::vec3(0.5f)) {
    if (s.size() >= 7 && s[0] == '#') {
        unsigned v = (unsigned)std::strtoul(s.substr(1, 6).c_str(), nullptr, 16);
        return glm::vec3(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f);
    }
    return d;
}

std::string trayName(int tid) {
    if (tid == 254) return "Ext";
    if (tid < 0) return "-";
    return "A" + std::to_string(tid / 4 + 1) + "-" + std::to_string(tid % 4 + 1);
}

// The filament type a profile prints ("PLA", "PETG"...), from its name.
std::string typeOfPreset(const std::string& name) {
    static const char* kinds[] = {"PLA-CF", "PETG-CF", "PA-CF", "PET-CF", "PETG", "PLA", "ABS", "ASA", "TPU", "PA", "PC", "PVA", "HIPS", "PET", "PPS", "PP", "PE"};
    std::string u = name;
    for (auto& c : u) c = (char)std::toupper((unsigned char)c);
    for (auto* k : kinds)
        if (u.find(k) != std::string::npos) return k;
    return {};
}

} // namespace

// ===================================================================== status

void App::devicePoll(bool now) {
    if (slice_.devPolling) return;
    double t = ImGui::GetTime();
    if (!now && t - slice_.devLastPoll < 5.0) return;
    slice_.devLastPoll = t;
    print::PrinterConn c = printerConn();
    if (c.kind == print::PrinterKind::None) return;
    if (std::getenv("KESTRAL_FAKE_DEVICE")) {   // developer check: a printer that is printing, with an AMS
        auto& st = slice_.devStatus;
        st = print::PrintStatus{};
        st.ok = true; st.state = "printing"; st.percent = 42; st.minutesLeft = 37; st.job = "Tank body"; st.nozzleC = 219; st.nozzleTarget = 220;
        st.bedC = 64; st.bedTarget = 65; st.partFan = 80; st.lightKnown = true; st.light = true; st.layer = 51; st.totalLayers = 120; st.speedLevel = 2;
        st.nozzleDia = 0.4; st.nozzleType = "stainless_steel"; st.amsUnits = 1; st.trayNow = 1;
        const char* cols[4] = {"#FFFFFF", "#1E5BFF", "#F2D21A", "#000000"};
        const char* types[4] = {"PLA", "PLA", "PETG", "TPU"};
        for (int i = 0; i < 4; ++i) { print::PrintStatus::AmsTray t; t.ams = 0; t.slot = i; t.loaded = true; t.type = types[i]; t.color = cols[i]; t.name = std::string(types[i]) + " Basic"; t.remain = 80 - i * 15; t.humidity = 3; st.trays.push_back(t); }
        ++slice_.devSeq;
        return;
    }
    slice_.devPolling = true;
    JobSystem* jobs = jobs_.get();
    jobs_->submit("printer status", [this, jobs, c] {
        print::PrintStatus st = print::printerStatus(c, 6000);
        jobs->post([this, st] {
            slice_.devPolling = false;
            slice_.devStatus = st;
            slice_.devLastPoll = ImGui::GetTime();
            ++slice_.devSeq;
            activity();
        });
    }, [this](const std::string& e) {
        slice_.devPolling = false;
        slice_.devStatus = print::PrintStatus{};
        slice_.devStatus.error = e;
    });
}

void App::deviceCommand(const std::string& action) {
    if (slice_.devBusy) return;
    print::PrinterConn c = printerConn();
    slice_.devBusy = true;
    slice_.devMsg = action == "stop" ? "Stopping..." : action == "pause" ? "Pausing..." : "Resuming...";
    JobSystem* jobs = jobs_.get();
    jobs_->submit("printer command", [this, jobs, c, action] {
        print::SendResult r = print::printerControl(c, action, nullptr);
        jobs->post([this, r] {
            slice_.devBusy = false;
            slice_.devMsg = r.message;
            toast(r.message, r.ok ? ToastKind::Success : ToastKind::Error);
            devicePoll(true);
        });
    }, [this](const std::string& e) {
        slice_.devBusy = false;
        slice_.devMsg = e;
    });
}

void App::deviceSend(const std::string& gcodeOrJson, bool json, const std::string& what) {
    print::PrinterConn c = printerConn();
    if (c.kind == print::PrinterKind::None) { toast("Set up the printer first (Preferences > Printer)", ToastKind::Warning); return; }
    JobSystem* jobs = jobs_.get();
    jobs_->submit("printer control", [this, jobs, c, gcodeOrJson, json, what] {
        print::SendResult r = json ? print::printerMqtt(c, gcodeOrJson) : print::printerGcode(c, gcodeOrJson);
        jobs->post([this, r, what] {
            if (!r.ok) toast(what + ": " + r.message, ToastKind::Error);
            slice_.devMsg = r.ok ? what : r.message;
            slice_.devLastPoll = ImGui::GetTime() - 4.0;   // fresh status soon
        });
    });
}

void App::deviceLight(bool on) {
    std::string j = std::string(R"({"system":{"sequence_id":"0","command":"ledctrl","led_node":"chamber_light","led_mode":")") + (on ? "on" : "off") +
                    R"(","led_on_time":500,"led_off_time":500,"loop_times":0,"interval_time":0}})";
    deviceSend(j, true, on ? "Lamp on" : "Lamp off");
    slice_.devStatus.light = on;
    slice_.devStatus.lightKnown = true;
}

void App::deviceSpeed(int level) {
    deviceSend(R"({"print":{"sequence_id":"0","command":"print_speed","param":")" + std::to_string(std::clamp(level, 1, 4)) + "\"}}", true, "Speed changed");
    slice_.devStatus.speedLevel = level;
}

void App::sliceAfterStatus() {
    if (slice_.devSeqSeen == slice_.devSeq) return;
    slice_.devSeqSeen = slice_.devSeq;
    const auto& st = slice_.devStatus;
    if (!st.ok) { slice_.syncFromPrinter = slice_.syncAmsFils = false; return; }
    if (slice_.syncFromPrinter && slice_.setup) {
        slice_.syncFromPrinter = false;
        // The nozzle the printer has: the printer profile for that size, and its material.
        if (st.nozzleDia > 0) {
            char d[16];
            std::snprintf(d, sizeof d, "%.1f nozzle", st.nozzleDia);
            const print::Preset* mp = print::findPreset(*slice_.setup, "machine", slice_.machine);
            for (auto* p : slice_.printers)
                if (mp && p->printerModel == mp->printerModel && p->name.find(d) != std::string::npos && !p->user) {
                    if (p->name != slice_.machine && slice_.machine.find(d) == std::string::npos) { slice_.machine = p->name; slice_.overrides.clear(); sliceLoadSettings(); }
                    break;
                }
        }
        if (!st.nozzleType.empty()) sliceSetValue("nozzle_type", st.nozzleType);
        slice_.syncAmsFils = true;
        toast("Read the nozzle and AMS from the printer", ToastKind::Success);
    }
    if (slice_.syncAmsFils && slice_.setup) {
        slice_.syncAmsFils = false;
        // Orca's "Synchronize filament list from AMS": one filament per loaded slot.
        std::vector<const print::PrintStatus::AmsTray*> loaded;
        for (auto& t : st.trays) if (t.loaded) loaded.push_back(&t);
        if (loaded.empty() && st.hasExt && st.ext.loaded) loaded.push_back(&st.ext);
        if (loaded.empty()) { toast("The printer reports no filament in its AMS", ToastKind::Warning); return; }
        std::vector<SliceTab::Fil> nf;
        for (auto* t : loaded) {
            SliceTab::Fil f;
            f.color = hexCol(t->color, glm::vec3(0.8f));
            auto found = print::searchPresets(*slice_.setup, "filament", (t->name.empty() ? t->type : t->name) + " " + slice_.machine, 1);
            if (!found.empty()) f.preset = found[0]->name;
            nf.push_back(f);
            if (nf.size() >= 16) break;
        }
        if (!nf[0].preset.empty() && nf[0].preset != slice_.filament) { slice_.filament = nf[0].preset; sliceLoadSettings(); }
        nf[0].preset.clear();
        slice_.fil1ColorSet = true;
        // Parts keep their filament number where it still exists.
        slice_.ex.fils = nf;
        for (auto& [id, o] : slice_.plate) if (o.fil >= (int)nf.size()) o.fil = 0;
        sliceFlushDefaults();
        sliceAutoFils();
        sceneDirty_ = true;
        toast("Filaments taken from the AMS: " + std::to_string(nf.size()), ToastKind::Success);
    }
}

// ===================================================================== AMS mapping

void App::sliceAutoAmsMap() {
    // Each filament of the print: the AMS slot with the same type and the nearest color.
    const auto& st = slice_.devStatus;
    size_t n = std::max<size_t>(1, slice_.ex.slicedFils.size());
    if (slice_.ex.amsMap.size() == n) return;
    slice_.ex.amsMap.assign(n, -1);
    std::vector<bool> taken(256, false);
    for (size_t i = 0; i < n; ++i) {
        int tabFil = i < slice_.ex.slicedFils.size() ? slice_.ex.slicedFils[i] : 0;
        glm::vec3 want = sliceFilColor(tabFil);
        std::string type = typeOfPreset(sliceFilPreset(tabFil));
        int best = -1;
        float bd = 1e9f;
        auto consider = [&](const print::PrintStatus::AmsTray& t, int tid) {
            if (!t.loaded || taken[(size_t)tid]) return;
            float d = glm::length(hexCol(t.color) - want) + (type.empty() || t.type.empty() || typeOfPreset(t.type) == type ? 0.f : 5.f);
            if (d < bd) { bd = d; best = tid; }
        };
        for (auto& t : st.trays) consider(t, t.ams * 4 + t.slot);
        if (st.hasExt) consider(st.ext, 254);
        if (best < 0 && st.trays.empty() && n == 1) best = 254;   // no AMS: the spool holder
        slice_.ex.amsMap[i] = best;
        if (best >= 0 && best != 254) taken[(size_t)best] = true;
    }
}

// ===================================================================== Device tab

void App::drawSliceDevice() {
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float top = sliceTop();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - top));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::Panel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::Begin("##device", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus);
    devicePoll(false);
    if (slice_.devPolling || slice_.devBusy) activity();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    print::PrinterConn c = printerConn();
    const auto& st = slice_.devStatus;
    // ---- left column: the printer and the pages
    float leftW = 230 * S;
    dl->AddRectFilled(wp, ImVec2(wp.x + leftW, wp.y + ws.y), col::Card);
    {
        ImVec2 p(wp.x, wp.y + 8 * S);
        sl::drawOIcon(dl, ImVec2(p.x + 22 * S, p.y + 18 * S), 18 * S, OIcon::Printer, sl::orangeU());
        std::string nm = c.kind == print::PrinterKind::None ? std::string("No printer") : c.name.empty() ? std::string("Printer") : c.name;
        dl->AddText(fonts_.bold, ImGui::GetFontSize(), ImVec2(p.x + 40 * S, p.y + 18 * S - ImGui::GetFontSize() * 0.5f), col::Text, nm.c_str());
        bool online = st.ok;
        sl::drawOIcon(dl, ImVec2(p.x + leftW - 22 * S, p.y + 18 * S), 16 * S, OIcon::Wifi, online ? sl::orangeU() : col::TextDim);
        ImGui::SetCursorScreenPos(ImVec2(p.x + leftW - 34 * S, p.y + 6 * S));
        ImGui::InvisibleButton("##wifi", ImVec2(24 * S, 24 * S));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", online ? "Connected" : slice_.devPolling ? "Connecting..." : "Not reachable", st.wifi.empty() ? "" : ("  (Wi-Fi " + st.wifi + ")").c_str());
        const char* pages[4] = {"Status", "Storage", "Update", "Assistant (HMS)"};
        float y = p.y + 44 * S;
        for (int i = 0; i < 4; ++i) {
            ImVec2 a(wp.x, y), b(wp.x + leftW, y + 40 * S);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i + 4000);
            if (ImGui::InvisibleButton("##dp", ImVec2(leftW, 40 * S))) slice_.devPage = i;
            bool hv = ImGui::IsItemHovered();
            ImGui::PopID();
            bool on = slice_.devPage == i;
            if (on) dl->AddRectFilled(a, b, sl::orangeU(200));
            else if (hv) dl->AddRectFilled(a, b, col::Hover);
            ImGui::PushFont(on ? fonts_.bold : nullptr, 0.f);
            dl->AddText(ImVec2(a.x + 18 * S, a.y + (40 * S - ImGui::GetFontSize()) * 0.5f), on ? IM_COL32_WHITE : col::Text, pages[i]);
            ImGui::PopFont();
            dl->AddText(ImVec2(b.x - 20 * S, a.y + (40 * S - ImGui::GetFontSize()) * 0.5f), on ? IM_COL32_WHITE : col::TextDim, ">");
            if (i == 3 && !st.hms.empty()) dl->AddCircleFilled(ImVec2(b.x - 36 * S, a.y + 20 * S), 4 * S, col::Error);
            y += 40 * S;
        }
        ImGui::SetCursorScreenPos(ImVec2(wp.x + 18 * S, y + 10 * S));
        ImGui::PushTextWrapPos(wp.x + leftW - 12 * S);
        ImGui::TextDisabled("%s", print::kindName(c.kind));
        ImGui::PopTextWrapPos();
        if (!slice_.devMsg.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(wp.x + 18 * S, ImGui::GetCursorScreenPos().y + 6 * S));
            ImGui::PushTextWrapPos(wp.x + leftW - 12 * S);
            ImGui::TextDisabled("%s", slice_.devMsg.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::SetCursorScreenPos(ImVec2(wp.x + 18 * S, wp.y + ws.y - 40 * S));
        if (ImGui::SmallButton("Printer setup...")) settingsOpen_ = true;
    }
    if (c.kind == print::PrinterKind::None) {
        ImGui::SetCursorScreenPos(ImVec2(wp.x + leftW + 40 * S, wp.y + 40 * S));
        ImGui::BeginGroup();
        ImGui::PushFont(fonts_.bold, 0.f);
        ImGui::TextUnformatted("No printer is set up yet.");
        ImGui::PopFont();
        ImGui::TextDisabled("Kestral talks to Bambu Lab printers (home network or Bambu cloud), OctoPrint, Klipper and PrusaLink.");
        if (sl::orangeButton("Set up the printer...")) settingsOpen_ = true;
        ImGui::EndGroup();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }
    float x0 = wp.x + leftW + 10 * S, y0 = wp.y + 10 * S;
    float avail = ws.x - leftW - 20 * S;
    if (slice_.devPage != 0) {
        ImGui::SetCursorScreenPos(ImVec2(x0 + 10 * S, y0 + 10 * S));
        ImGui::BeginChild("##devpage", ImVec2(avail - 20 * S, ws.y - 40 * S));
        if (slice_.devPage == 1) {
            ImGui::PushFont(fonts_.bold, 0.f);
            ImGui::TextUnformatted("Storage");
            ImGui::PopFont();
            ImGui::Separator();
            if (!st.ok) ImGui::TextDisabled("%s", st.error.empty() ? "Waiting for the printer..." : st.error.c_str());
            else {
                ImGui::Text("SD card: %s", st.sdcard ? "inserted" : "none");
                ImGui::TextDisabled("Prints sent from Kestral go to the SD card first, then start. Timelapses are saved there too.");
            }
        } else if (slice_.devPage == 2) {
            ImGui::PushFont(fonts_.bold, 0.f);
            ImGui::TextUnformatted("Update");
            ImGui::PopFont();
            ImGui::Separator();
            if (st.ok) {
                ImGui::Text("Nozzle: %.1f mm %s", st.nozzleDia, st.nozzleType.c_str());
                ImGui::Text("AMS units: %d", st.amsUnits);
            }
            ImGui::TextDisabled("Firmware updates run on the printer: its screen, Bambu Handy, or OrcaSlicer's Device tab.");
            ImGui::Spacing();
            ImGui::TextUnformatted("Slicer engine");
            ImGui::TextDisabled("%s", engineStatusText().c_str());
        } else {
            ImGui::PushFont(fonts_.bold, 0.f);
            ImGui::TextUnformatted("Assistant (HMS)");
            ImGui::PopFont();
            ImGui::Separator();
            if (st.hms.empty()) ImGui::TextDisabled("No messages from the printer. All good.");
            for (auto& h : st.hms) {
                ImGui::PushID(h.c_str());
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Warning), "%s", h.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Look it up")) {
                    std::string code = h.substr(4);
                    platform::openUrl("https://wiki.bambulab.com/en/search?q=" + platform::urlEncode(code));
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }
    // ---- Status: camera (left), control (right), AMS and progress below.
    float camW = std::max(360 * S, avail * 0.56f), ctlW = avail - camW - 10 * S;
    float camH = std::min(camW * 9.f / 16.f + 34 * S, ws.y * 0.62f);
    auto panel = [&](ImVec2 a, ImVec2 b, const char* title) {
        dl->AddRectFilled(a, b, col::Card, 6 * S);
        dl->AddRectFilled(a, ImVec2(b.x, a.y + 30 * S), col::AppBar, 6 * S, ImDrawFlags_RoundCornersTop);
        dl->AddText(ImVec2(a.x + 14 * S, a.y + (30 * S - ImGui::GetFontSize()) * 0.5f), col::TextDim, title);
    };
    // Camera
    ImVec2 ca(x0, y0), cb(x0 + camW, y0 + camH);
    panel(ca, cb, "Camera");
    {
        ImVec2 i0(ca.x + 6 * S, ca.y + 34 * S), i1(cb.x - 6 * S, cb.y - 30 * S);
        dl->AddRectFilled(i0, i1, IM_COL32(0, 0, 0, 255));
        bool bambu = c.kind == print::PrinterKind::BambuOrca || c.kind == print::PrinterKind::BambuLan || c.kind == print::PrinterKind::BambuConnect;
        if (camOn_ && bambu && !camera_.running()) camera_.start(c);
        std::string jpeg;
        if (camera_.latest(jpeg, camSeq_)) {
            int w = 0, h = 0, n = 0;
            unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(jpeg.data()), (int)jpeg.size(), &w, &h, &n, 4);
            if (px) {
                // One texture, its pixels replaced in place (a new one per picture used up the graphics
                // driver's slots after a while and the picture turned into garbage).
                if (camTex_ && camW_ == w && camH_ == h && camTex_->Status == ImTextureStatus_OK) {
                    std::memcpy(camTex_->GetPixels(), px, (size_t)w * h * 4);
                    ImTextureRect r{0, 0, (unsigned short)w, (unsigned short)h};
                    camTex_->Updates.clear();
                    camTex_->Updates.push_back(r);
                    camTex_->UpdateRect = r;
                    camTex_->SetStatus(ImTextureStatus_WantUpdates);
                } else if (!camTex_ || camTex_->Status == ImTextureStatus_OK) {
                    dropTexture(camTex_);
                    camTex_ = makeTexture(px, w, h);
                    camW_ = w;
                    camH_ = h;
                }
                stbi_image_free(px);
            }
        }
        if (camOn_ && camTex_ && camW_ > 0) {
            float bw = i1.x - i0.x, bh = i1.y - i0.y, ar = (float)camW_ / (float)camH_;
            float iw = std::min(bw, bh * ar), ih = iw / ar;
            ImVec2 m0(i0.x + (bw - iw) * 0.5f, i0.y + (bh - ih) * 0.5f);
            dl->AddImage(camTex_->GetTexRef(), m0, ImVec2(m0.x + iw, m0.y + ih));
            activity();
        } else {
            std::string msg = !bambu ? "This printer has no camera Kestral can show." : !camOn_ ? "Camera off" : camera_.status();
            if (msg.empty()) { msg = "Waiting for a picture..."; activity(); }
            ImGui::SetCursorScreenPos(ImVec2(i0.x + 16 * S, i0.y + 16 * S));
            ImGui::PushTextWrapPos(i1.x - 16 * S);
            ImGui::TextDisabled("%s", msg.c_str());
            ImGui::PopTextWrapPos();
        }
        // Play / stop at the bottom left.
        ImVec2 pb(ca.x + 10 * S, cb.y - 26 * S);
        ImGui::SetCursorScreenPos(pb);
        if (ImGui::InvisibleButton("##camplay", ImVec2(22 * S, 22 * S))) {
            camOn_ = !camOn_;
            if (!camOn_) camera_.stop();
        }
        ImGui::SetItemTooltip(camOn_ ? "Stop the camera" : "Start the camera");
        dl->AddCircleFilled(ImVec2(pb.x + 11 * S, pb.y + 11 * S), 10 * S, sl::orangeU(), 20);
        sl::drawOIcon(dl, ImVec2(pb.x + 11 * S, pb.y + 11 * S), 10 * S, camOn_ ? OIcon::Stop : OIcon::Play, IM_COL32_WHITE);
    }
    // Printing progress under the camera.
    ImVec2 pa(x0, cb.y + 10 * S), pbb(x0 + camW, std::min(wp.y + ws.y - 10 * S, cb.y + 10 * S + 170 * S));
    panel(pa, pbb, "Printing progress");
    {
        ImVec2 t0(pa.x + 12 * S, pa.y + 40 * S);
        float th = std::min(110 * S, pbb.y - t0.y - 10 * S);
        dl->AddRectFilled(t0, ImVec2(t0.x + th, t0.y + th), IM_COL32(235, 235, 235, 255), 4 * S);
        print::SliceResult last;
        {
            std::lock_guard lock(print_.m);
            last = print_.last;
        }
        if (last.ok) if (ImTextureData* tx = sliceThumbFor(last.gcode3mf)) dl->AddImage(tx->GetTexRef(), t0, ImVec2(t0.x + th, t0.y + th));
        float tx = t0.x + th + 16 * S, tw = pbb.x - tx - 110 * S;
        bool active = st.ok && (st.state == "printing" || st.state == "paused" || st.state == "preparing");
        std::string job = st.job.empty() ? "N/A" : st.job;
        std::string state = st.ok ? st.state : "N/A";
        if (!state.empty()) state[0] = (char)std::toupper((unsigned char)state[0]);
        dl->AddText(ImVec2(tx, t0.y + 4 * S), col::Text, job.c_str());
        dl->AddText(fonts_.bold, ImGui::GetFontSize() * 1.2f, ImVec2(tx, t0.y + 26 * S), sl::orangeU(), state.c_str());
        char lay[64];
        std::snprintf(lay, sizeof lay, "Layer: %s", st.layer >= 0 && st.totalLayers > 0 ? (std::to_string(st.layer) + "/" + std::to_string(st.totalLayers)).c_str() : "N/A");
        ImVec2 ls = ImGui::CalcTextSize(lay);
        dl->AddText(ImVec2(tx + tw - ls.x, t0.y + 30 * S), col::TextDim, lay);
        float bar = st.percent >= 0 ? st.percent / 100.f : 0.f;
        ImVec2 b0(tx, t0.y + 56 * S), b1(tx + tw, t0.y + 62 * S);
        dl->AddRectFilled(b0, b1, col::Chip, 3 * S);
        dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * bar, b1.y), sl::orangeU(), 3 * S);
        char pct[64];
        std::snprintf(pct, sizeof pct, "%s   %s", st.percent >= 0 ? (std::to_string(st.percent) + "%").c_str() : "N/A",
                      st.minutesLeft >= 0 ? (print::formatDuration(st.minutesLeft * 60.0) + " left").c_str() : "");
        ImVec2 ps = ImGui::CalcTextSize(pct);
        dl->AddText(ImVec2(tx + tw - ps.x, t0.y + 68 * S), col::TextDim, pct);
        // Pause / resume and stop.
        ImGui::SetCursorScreenPos(ImVec2(tx + tw + 20 * S, t0.y + 44 * S));
        ImGui::BeginDisabled(slice_.devBusy || !active);
        if (st.state == "paused") { if (ImGui::Button("Resume", ImVec2(76 * S, 0))) deviceCommand("resume"); }
        else if (ImGui::Button("Pause", ImVec2(76 * S, 0))) deviceCommand("pause");
        ImGui::SetCursorScreenPos(ImVec2(tx + tw + 20 * S, t0.y + 44 * S + ImGui::GetFrameHeightWithSpacing()));
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(col::Error));
        if (ImGui::Button("Stop", ImVec2(76 * S, 0))) ImGui::OpenPopup("Stop the print?");
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (ImGui::BeginPopupModal("Stop the print?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextUnformatted("The print is cancelled and cannot be continued.");
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(col::Error));
            if (ImGui::Button("Stop the print", ImVec2(150 * S, 0))) { deviceCommand("stop"); ImGui::CloseCurrentPopup(); }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Keep printing", ImVec2(130 * S, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    // ---- Control (right)
    float cx0 = x0 + camW + 10 * S;
    ImVec2 ka(cx0, y0), kb(cx0 + ctlW, y0 + 300 * S);
    panel(ka, kb, "Control");
    {
        // Orca's control tabs at the right of the header.
        const char* tabs[] = {"Printer Parts", "Print Options", "Calibration"};
        float tx = kb.x - 8 * S;
        for (int i = 2; i >= 0; --i) {
            ImVec2 ts = ImGui::CalcTextSize(tabs[i]);
            tx -= ts.x + 16 * S;
            ImVec2 a(tx, ka.y + 5 * S);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i + 4100);
            if (ImGui::InvisibleButton("##ct", ImVec2(ts.x + 12 * S, 20 * S))) ImGui::OpenPopup("##ctlpop");
            bool hv = ImGui::IsItemHovered();
            if (ImGui::BeginPopup("##ctlpop")) {
                if (i == 0) {
                    ImGui::TextUnformatted("Printer parts");
                    ImGui::Separator();
                    ImGui::Text("Nozzle: %.1f mm, %s", st.nozzleDia, st.nozzleType.empty() ? "?" : st.nozzleType.c_str());
                    if (ImGui::Button("Use this nozzle in Kestral")) { slice_.syncFromPrinter = true; devicePoll(true); }
                } else if (i == 1) {
                    ImGui::TextUnformatted("Print options");
                    ImGui::Separator();
                    ImGui::Checkbox("Bed leveling before prints", &slice_.sendLeveling);
                    ImGui::Checkbox("Flow dynamics calibration before prints", &slice_.sendFlowCali);
                    ImGui::Checkbox("Timelapse", &slice_.sendTimelapse);
                    ImGui::TextDisabled("Kestral sends these with each print.");
                } else {
                    ImGui::TextUnformatted("Calibration");
                    ImGui::Separator();
                    if (ImGui::Button("Open the Calibration tab")) { ImGui::CloseCurrentPopup(); setSliceTop(5); }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            dl->AddRectFilled(a, ImVec2(a.x + ts.x + 12 * S, a.y + 20 * S), hv ? col::Hover : col::Card, 10 * S);
            dl->AddText(ImVec2(a.x + 6 * S, a.y + (20 * S - ts.y) * 0.5f), col::TextDim, tabs[i]);
        }
        ImGui::SetCursorScreenPos(ImVec2(ka.x + 12 * S, ka.y + 42 * S));
        ImGui::BeginGroup();
        // Temperatures: click a value to set a target.
        auto temp = [&](const char* id, OIcon ic, double now, double target, const char* code, bool settable, const char* tip) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            sl::drawOIcon(dl, ImVec2(p.x + 10 * S, p.y + 11 * S), 16 * S, ic, col::Text);
            char t[48];
            std::snprintf(t, sizeof t, "%s / %s C", now >= 0 ? std::to_string((int)std::lround(now)).c_str() : "_", target >= 0 ? std::to_string((int)std::lround(target)).c_str() : "_");
            ImGui::SetCursorScreenPos(ImVec2(p.x + 26 * S, p.y));
            ImGui::PushID(id);
            if (ImGui::Selectable(t, false, 0, ImVec2(110 * S, 22 * S)) && settable) { slice_.tempEdit = id; slice_.tempBuf = target > 0 ? std::to_string((int)target) : ""; ImGui::OpenPopup("##tset"); }
            ImGui::SetItemTooltip("%s", tip);
            if (ImGui::BeginPopup("##tset")) {
                ImGui::SetNextItemWidth(90 * S);
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                bool go = ImGui::InputText("C##tv", &slice_.tempBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal);
                ImGui::SameLine();
                go |= ImGui::Button("Set");
                if (go) {
                    int v = std::clamp(std::atoi(slice_.tempBuf.c_str()), 0, 320);
                    deviceSend(std::string(code) + " S" + std::to_string(v), false, "Temperature set");
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 30 * S));
        };
        temp("noz", OIcon::Nozzle, st.nozzleC, st.nozzleTarget, "M104", true, "Nozzle temperature (click to set)");
        temp("bed", OIcon::BedTemp, st.bedC, st.bedTarget, "M140", true, "Bed temperature (click to set)");
        temp("cham", OIcon::Chamber, st.chamberC, -1, "M141", false, "Chamber temperature");
        // Fan, speed, lamp
        ImVec2 p = ImGui::GetCursorScreenPos();
        auto bigBtn = [&](const char* id, OIcon ic, const char* label, bool on) {
            ImVec2 q = ImGui::GetCursorScreenPos();
            bool cl = ImGui::InvisibleButton(id, ImVec2(52 * S, 52 * S));
            bool hv = ImGui::IsItemHovered();
            if (hv) dl->AddRectFilled(q, ImVec2(q.x + 52 * S, q.y + 52 * S), col::Hover, 6 * S);
            sl::drawOIcon(dl, ImVec2(q.x + 26 * S, q.y + 18 * S), 22 * S, ic, on ? sl::orangeU() : col::Text);
            ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(q.x + (52 * S - ts.x) * 0.5f, q.y + 34 * S), col::Text, label);
            return cl;
        };
        ImGui::SetCursorScreenPos(p);
        if (bigBtn("##fan", OIcon::Fan, "Fan", st.partFan > 0)) ImGui::OpenPopup("##fans");
        if (ImGui::BeginPopup("##fans")) {
            auto fanRow = [&](const char* name, int P, int cur) {
                ImGui::PushID(P);
                int v = std::max(0, cur);
                ImGui::SetNextItemWidth(180 * S);
                if (ImGui::SliderInt(name, &v, 0, 100, "%d%%") && ImGui::IsItemDeactivatedAfterEdit()) {}
                if (ImGui::IsItemDeactivatedAfterEdit()) deviceSend("M106 P" + std::to_string(P) + " S" + std::to_string((int)std::lround(v * 2.55)), false, std::string(name) + " fan set");
                ImGui::PopID();
            };
            fanRow("Part", 1, st.partFan);
            fanRow("Aux", 2, st.auxFan);
            fanRow("Chamber", 3, st.chamberFan);
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 6 * S);
        const char* lv[] = {"?", "Silent", "Standard", "Sport", "Ludicrous"};
        const char* pctL[] = {"?", "50%", "100%", "124%", "166%"};
        if (bigBtn("##speed", OIcon::Rotate, st.speedLevel >= 1 && st.speedLevel <= 4 ? pctL[st.speedLevel] : "100%", false)) ImGui::OpenPopup("##speeds");
        if (ImGui::BeginPopup("##speeds")) {
            for (int i = 1; i <= 4; ++i)
                if (ImGui::Selectable((std::string(lv[i]) + "  " + pctL[i]).c_str(), st.speedLevel == i)) deviceSpeed(i);
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 6 * S);
        if (bigBtn("##lamp", OIcon::Lamp, "Lamp", st.lightKnown && st.light)) deviceLight(!(st.lightKnown && st.light));
        ImGui::EndGroup();
        // The jog pad: X / Y in a ring, +-1 and +-10, home in the middle (Orca's).
        float padR = std::min(100 * S, (kb.y - ka.y - 120 * S) * 0.5f);
        ImVec2 pc(ka.x + ctlW * 0.58f, ka.y + 40 * S + padR + 4 * S);
        ImU32 ring = col::Chip, ring2 = col::Hover;
        dl->AddCircleFilled(pc, padR, ring, 48);
        dl->AddCircleFilled(pc, padR * 0.62f, ring2, 48);
        dl->AddCircleFilled(pc, padR * 0.25f, col::Card, 32);
        for (int k = 0; k < 4; ++k) {
            float a = 0.785398f + k * 1.570796f;
            dl->AddLine(ImVec2(pc.x + std::cos(a) * padR * 0.25f, pc.y + std::sin(a) * padR * 0.25f), ImVec2(pc.x + std::cos(a) * padR, pc.y + std::sin(a) * padR), col::Card, 2 * S);
        }
        sl::drawOIcon(dl, pc, padR * 0.3f, OIcon::Home, sl::orangeU());
        auto lab = [&](float dx, float dy, const char* t) { ImVec2 ts = ImGui::CalcTextSize(t); dl->AddText(ImVec2(pc.x + dx - ts.x * 0.5f, pc.y + dy - ts.y * 0.5f), col::Text, t); };
        lab(0, -padR * 0.82f, "Y");
        lab(0, padR * 0.82f, "-Y");
        lab(padR * 0.82f, 0, "X");
        lab(-padR * 0.82f, 0, "-X");
        ImGui::SetCursorScreenPos(ImVec2(pc.x - padR, pc.y - padR));
        if (ImGui::InvisibleButton("##jog", ImVec2(padR * 2, padR * 2))) {
            ImVec2 m = ImGui::GetIO().MousePos;
            float dx = m.x - pc.x, dy = m.y - pc.y, r = std::sqrt(dx * dx + dy * dy);
            if (r < padR * 0.25f) deviceSend("G28", false, "Homing");
            else {
                int step = r < padR * 0.62f ? 1 : 10;
                std::string axis;
                if (std::abs(dx) > std::abs(dy)) axis = dx > 0 ? "X" : "X-";
                else axis = dy < 0 ? "Y" : "Y-";
                std::string mv = axis[0] + std::string(axis.size() > 1 ? "-" : "") + std::to_string(step);
                deviceSend("G91\nG0 " + mv + " F3000\nG90", false, "Moved " + mv + " mm");
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Inner ring: 1 mm, outer ring: 10 mm, middle: home all axes");
        // Bed (Z) buttons and the extruder.
        ImGui::SetCursorScreenPos(ImVec2(pc.x - padR, pc.y + padR + 12 * S));
        auto zb = [&](const char* label, const char* g, const char* tip) {
            if (ImGui::Button(label, ImVec2(42 * S, 0))) deviceSend(g, false, tip);
            ImGui::SetItemTooltip("%s", tip);
            ImGui::SameLine(0, 3 * S);
        };
        zb("^10", "G91\nG0 Z-10 F900\nG90", "Bed up 10 mm");
        zb("^1", "G91\nG0 Z-1 F900\nG90", "Bed up 1 mm");
        ImGui::TextDisabled("Bed");
        ImGui::SameLine(0, 3 * S);
        zb("v1", "G91\nG0 Z1 F900\nG90", "Bed down 1 mm");
        zb("v10", "G91\nG0 Z10 F900\nG90", "Bed down 10 mm");
        ImGui::NewLine();
        ImVec2 ex(kb.x - 70 * S, ka.y + 60 * S);
        ImGui::SetCursorScreenPos(ex);
        ImGui::BeginGroup();
        bool hot = st.nozzleC >= 170;
        ImGui::BeginDisabled(!hot);
        if (ImGui::Button("Up##ext", ImVec2(56 * S, 0))) deviceSend("M83\nG0 E-10 F900", false, "Retracted 10 mm");
        ImGui::SetItemTooltip("Pull the filament back 10 mm");
        if (ImGui::Button("Down##ext", ImVec2(56 * S, 0))) deviceSend("M83\nG0 E10 F900", false, "Extruded 10 mm");
        ImGui::SetItemTooltip("Push 10 mm of filament through");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Extruder");
        if (!hot) ImGui::TextDisabled("(heat the\nnozzle first)");
        ImGui::EndGroup();
    }
    // ---- AMS under the controls
    ImVec2 aa(cx0, kb.y + 10 * S), ab(cx0 + ctlW, std::min(wp.y + ws.y - 10 * S, kb.y + 10 * S + 200 * S));
    panel(aa, ab, "AMS");
    {
        float sx = aa.x + 14 * S, sy = aa.y + 44 * S;
        float slotW = std::min(64 * S, (ctlW - 40 * S) / 5.f), slotH = 86 * S;
        auto slot = [&](const print::PrintStatus::AmsTray& t, int tid) {
            glm::vec3 c3 = hexCol(t.color, glm::vec3(0.3f));
            ImVec2 a(sx, sy), b(sx + slotW, sy + slotH);
            dl->AddRectFilled(a, b, t.loaded ? sl::u32(c3.r, c3.g, c3.b) : col::Chip, 8 * S);
            dl->AddRect(a, b, st.trayNow == tid ? sl::orangeU() : col::PanelBorder, 8 * S, 0, st.trayNow == tid ? 2.5f * S : 1.f * S);
            ImU32 tc = t.loaded && sl::isDark(c3.r, c3.g, c3.b) ? IM_COL32_WHITE : IM_COL32(20, 20, 20, 255);
            std::string ty = t.loaded ? t.type : "Empty";
            ImVec2 ts = ImGui::CalcTextSize(ty.c_str());
            dl->AddText(ImVec2(a.x + (slotW - ts.x) * 0.5f, a.y + slotH * 0.4f), t.loaded ? tc : col::TextDim, ty.c_str());
            std::string nm = trayName(tid);
            ImVec2 ns = ImGui::CalcTextSize(nm.c_str());
            dl->AddText(ImVec2(a.x + (slotW - ns.x) * 0.5f, b.y + 4 * S), col::TextDim, nm.c_str());
            if (t.remain >= 0) {
                char r[16];
                std::snprintf(r, sizeof r, "%d%%", t.remain);
                ImVec2 rs = ImGui::CalcTextSize(r);
                dl->AddText(ImVec2(a.x + (slotW - rs.x) * 0.5f, a.y + 6 * S), t.loaded ? tc : col::TextDim, r);
            }
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(tid + 5000);
            ImGui::InvisibleButton("##slot", ImVec2(slotW, slotH));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s %s%s", nm.c_str(), t.name.c_str(), t.type.c_str(), st.trayNow == tid ? "  (loaded now)" : "");
            ImGui::PopID();
            sx += slotW + 8 * S;
        };
        if (!st.ok) {
            ImGui::SetCursorScreenPos(ImVec2(sx, sy));
            ImGui::TextDisabled("%s", slice_.devPolling ? "Asking the printer..." : st.error.empty() ? "No status yet." : st.error.c_str());
        } else {
            for (auto& t : st.trays) slot(t, t.ams * 4 + t.slot);
            if (st.hasExt) slot(st.ext, 254);
            if (st.trays.empty() && !st.hasExt) { ImGui::SetCursorScreenPos(ImVec2(sx, sy)); ImGui::TextDisabled("No AMS on this printer."); }
            if (!st.trays.empty()) {
                for (auto& t : st.trays) if (t.humidity >= 0) { char h[32]; std::snprintf(h, sizeof h, "Humidity %d", t.humidity); dl->AddText(ImVec2(aa.x + 14 * S, ab.y - 24 * S), col::TextDim, h); break; }
                ImGui::SetCursorScreenPos(ImVec2(ab.x - 220 * S, ab.y - 30 * S));
                if (ImGui::Button("Unload", ImVec2(80 * S, 0)))
                    deviceSend(R"({"print":{"sequence_id":"0","command":"ams_change_filament","target":255,"curr_temp":220,"tar_temp":220}})", true, "Unloading");
                ImGui::SetItemTooltip("Pull the filament out of the nozzle back into the AMS");
                ImGui::SameLine();
                if (ImGui::Button("Sync to Kestral", ImVec2(124 * S, 0))) { slice_.syncAmsFils = true; ++slice_.devSeq; }
                ImGui::SetItemTooltip("Use these AMS filaments as the Slice tab's filaments");
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ===================================================================== Send print job

void App::drawSliceSendDialog() {
    const float S = uiScale_;
    if (slice_.sendOpen) {
        ImGui::OpenPopup("Send print job");
        slice_.sendOpen = false;
        devicePoll(true);
        slice_.ex.amsMap.clear();   // fresh match with what is in the AMS now
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.45f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640 * S, 0));
    if (!ImGui::BeginPopupModal("Send print job", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (slice_.devPolling) activity();
    print::PrinterConn c = printerConn();
    print::SliceResult last;
    std::string name;
    {
        std::lock_guard lock(print_.m);
        last = print_.last;
        name = print_.lastName;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // The plate picture, the name, time and weight.
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float th = 150 * S;
    dl->AddRectFilled(p0, ImVec2(p0.x + th, p0.y + th), col::Card, 6 * S);
    if (last.ok) if (ImTextureData* tx = sliceThumbFor(last.gcode3mf)) dl->AddImage(tx->GetTexRef(), p0, ImVec2(p0.x + th, p0.y + th));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + th + 20 * S, p0.y));
    ImGui::BeginGroup();
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s      %.2f g", print::formatDuration(last.seconds).c_str(), last.grams);
    ImGui::Spacing();
    ImGui::TextDisabled("Printer");
    if (c.kind == print::PrinterKind::None) {
        ImGui::TextWrapped("No printer is set up yet.");
        if (ImGui::Button("Set up the printer...")) { settingsOpen_ = true; ImGui::CloseCurrentPopup(); }
    } else {
        ImVec2 cp = ImGui::GetCursorScreenPos();
        float cw = std::max(330 * S, ImGui::GetContentRegionAvail().x), ch = 56 * S;
        dl->AddRectFilled(cp, ImVec2(cp.x + cw, cp.y + ch), col::Card, 6 * S);
        dl->AddRect(cp, ImVec2(cp.x + cw, cp.y + ch), col::PanelBorder, 6 * S);
        sl::drawOIcon(dl, ImVec2(cp.x + 26 * S, cp.y + ch * 0.5f), 28 * S, OIcon::Printer, col::Text);
        std::string pn = (c.name.empty() ? std::string("Printer") : c.name) + (c.kind == print::PrinterKind::BambuLan || (!c.host.empty() && c.kind == print::PrinterKind::BambuOrca) ? " (LAN)" : "");
        dl->AddText(fonts_.bold, ImGui::GetFontSize(), ImVec2(cp.x + 50 * S, cp.y + 9 * S), col::Text, pn.c_str());
        std::string sub = slice_.devStatus.ok ? "Bambu Lab, " + slice_.devStatus.state : std::string(print::kindName(c.kind));
        if (sub.size() > 46) sub = sub.substr(0, 44) + "..";
        dl->AddText(ImVec2(cp.x + 50 * S, cp.y + 30 * S), col::TextDim, sub.c_str());
        ImGui::SetCursorScreenPos(ImVec2(cp.x + cw - 30 * S, cp.y + (ch - 22 * S) * 0.5f));
        if (ImGui::InvisibleButton("##refresh", ImVec2(22 * S, 22 * S))) { devicePoll(true); slice_.ex.amsMap.clear(); }
        ImGui::SetItemTooltip("Ask the printer again (AMS, status)");
        sl::drawOIcon(dl, ImVec2(cp.x + cw - 19 * S, cp.y + ch * 0.5f), 16 * S, OIcon::Sync, slice_.devPolling ? sl::orangeU() : col::Text);
        ImGui::SetCursorScreenPos(ImVec2(cp.x, cp.y + ch + 6 * S));
        ImGui::TextDisabled("Plate: %s", slice_.bedType.empty() ? "printer default" : slice_.bedType.c_str());
    }
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, std::max(ImGui::GetCursorScreenPos().y, p0.y + th) + 10 * S));
    bool bambu = c.kind == print::PrinterKind::BambuOrca || c.kind == print::PrinterKind::BambuLan || c.kind == print::PrinterKind::BambuConnect;
    bool mapped = true;
    if (bambu) {
        // Filaments: each one of the print gets its AMS slot (Orca's mapping), picked by type and color.
        ImGui::SeparatorText("Filament");
        sliceAutoAmsMap();
        const auto& st = slice_.devStatus;
        size_t n = slice_.ex.amsMap.size();
        float boxW = 76 * S;
        for (size_t i = 0; i < n; ++i) {
            int tabFil = i < slice_.ex.slicedFils.size() ? slice_.ex.slicedFils[i] : 0;
            glm::vec3 fc = sliceFilColor(tabFil);
            std::string type = typeOfPreset(sliceFilPreset(tabFil));
            if (type.empty()) type = "Filament " + std::to_string(tabFil + 1);
            ImGui::PushID((int)i + 6000);
            if (i) ImGui::SameLine(0, 10 * S);
            ImVec2 b = ImGui::GetCursorScreenPos();
            float bh = 62 * S;
            dl->AddRectFilled(b, ImVec2(b.x + boxW, b.y + bh), col::Card, 6 * S);
            int m = slice_.ex.amsMap[i];
            bool ok = m >= 0;
            mapped &= ok;
            dl->AddRect(b, ImVec2(b.x + boxW, b.y + bh), ok ? col::PanelBorder : col::Error, 6 * S, 0, ok ? 1.f : 1.5f * S);
            dl->AddRectFilled(b, ImVec2(b.x + boxW, b.y + 22 * S), sl::u32(fc.r, fc.g, fc.b), 6 * S, ImDrawFlags_RoundCornersTop);
            ImVec2 ts = ImGui::CalcTextSize(type.c_str());
            dl->AddText(ImVec2(b.x + (boxW - ts.x) * 0.5f, b.y + (22 * S - ts.y) * 0.5f), sl::isDark(fc.r, fc.g, fc.b) ? IM_COL32_WHITE : IM_COL32_BLACK, type.c_str());
            // The slot it maps to: its color and name.
            glm::vec3 sc(0.3f);
            const print::PrintStatus::AmsTray* tr = nullptr;
            for (auto& t : st.trays) if (t.ams * 4 + t.slot == m) tr = &t;
            if (m == 254 && st.hasExt) tr = &st.ext;
            if (tr) sc = hexCol(tr->color, sc);
            ImVec2 s0(b.x + 8 * S, b.y + 28 * S), s1(b.x + boxW - 8 * S, b.y + bh - 6 * S);
            ImGui::SetCursorScreenPos(b);
            if (ImGui::InvisibleButton("##map", ImVec2(boxW, bh))) ImGui::OpenPopup("##slots");
            if (ok) {
                dl->AddRectFilled(s0, s1, sl::u32(sc.r, sc.g, sc.b), 4 * S);
                std::string tn = trayName(m);
                ImVec2 ns = ImGui::CalcTextSize(tn.c_str());
                dl->AddText(ImVec2(s0.x + (s1.x - s0.x - ns.x) * 0.5f, s0.y + (s1.y - s0.y - ns.y) * 0.5f), sl::isDark(sc.r, sc.g, sc.b) ? IM_COL32_WHITE : IM_COL32_BLACK, tn.c_str());
            } else {
                ImVec2 cc((s0.x + s1.x) * 0.5f, (s0.y + s1.y) * 0.5f);
                dl->AddCircleFilled(cc, 8 * S, col::Error, 16);
                dl->AddText(ImVec2(cc.x - 2 * S, cc.y - ImGui::GetFontSize() * 0.5f), IM_COL32_WHITE, "!");
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Filament %d (%s): click to pick its AMS slot", tabFil + 1, sliceFilPreset(tabFil).c_str());
            if (ImGui::BeginPopup("##slots")) {
                ImGui::TextDisabled("AMS slot for filament %d", tabFil + 1);
                auto opt = [&](const print::PrintStatus::AmsTray& t, int tid) {
                    if (!t.loaded) return;
                    glm::vec3 tc = hexCol(t.color, glm::vec3(0.5f));
                    ImVec2 q = ImGui::GetCursorScreenPos();
                    std::string lb = "        " + trayName(tid) + "   " + (t.name.empty() ? t.type : t.name) + "##s" + std::to_string(tid);
                    if (ImGui::Selectable(lb.c_str(), m == tid)) slice_.ex.amsMap[i] = tid;
                    float hgt = ImGui::GetTextLineHeight();
                    ImGui::GetWindowDrawList()->AddRectFilled(q, ImVec2(q.x + hgt * 1.8f, q.y + hgt), sl::u32(tc.r, tc.g, tc.b), 3 * S);
                };
                for (auto& t : st.trays) opt(t, t.ams * 4 + t.slot);
                if (st.hasExt) opt(st.ext, 254);
                if (st.trays.empty() && !st.hasExt) {
                    if (ImGui::Selectable("External spool", m == 254)) slice_.ex.amsMap[i] = 254;
                    ImGui::TextDisabled(st.ok ? "The printer reports no AMS." : "No status from the printer yet.");
                }
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(ImVec2(b.x + boxW, b.y));
            ImGui::Dummy(ImVec2(0, bh));
            ImGui::PopID();
        }
        if (!mapped) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Error), "Not all filaments used in slicing are mapped to the printer. Pick a slot for each one.");
        ImGui::Spacing();
        auto onOff = [&](const char* label, bool& v, const char* tip) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SetItemTooltip("%s", tip);
            ImGui::SameLine(200 * S);
            ImGui::PushID(label);
            for (int k = 0; k < 2; ++k) {
                bool on = (k == 0) == v;
                if (k) ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Button, on ? sl::orange() : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
                ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                if (ImGui::Button(k == 0 ? "On" : "Off", ImVec2(44 * S, 0))) v = k == 0;
                ImGui::PopStyleColor(2);
            }
            ImGui::PopID();
        };
        onOff("Timelapse", slice_.sendTimelapse, "Record a timelapse of the print");
        onOff("Auto Bed Leveling", slice_.sendLeveling, "Level the bed before printing");
        onOff("Flow Dynamics Calibration", slice_.sendFlowCali, "Calibrate the filament's flow before printing");
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::BeginDisabled(c.kind == print::PrinterKind::None || (bambu && !mapped && !slice_.devStatus.trays.empty()));
    if (sl::orangeButton("Send", ImVec2(130 * S, 0))) {
        ImGui::CloseCurrentPopup();
        slicePrint();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100 * S, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace kst
