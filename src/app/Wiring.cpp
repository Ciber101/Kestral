// Electronics: real parts from the catalog placed in the design, wired pin to pin, checked like the
// real thing and turned into a wiring guide. The wiring tool (assistant and MCP) and its window.
#include "app/App.h"
#include "core/Json.h"
#include "elec/Elec.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace kst {
using json::Value;

namespace {

std::string low(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// A sensible wire color for a pin, the way most kits do it.
std::string colorFor(const PinDef* p) {
    if (!p) return "";
    switch (p->kind) {
    case PinKind::Ground: return "black";
    case PinKind::PowerOut:
    case PinKind::PowerIn: return "red";
    case PinKind::I2cSda: return "blue";
    case PinKind::I2cScl: return "yellow";
    case PinKind::MotorOut:
    case PinKind::MotorIn: return "white";
    case PinKind::UartTx: return "green";
    case PinKind::UartRx: return "white";
    case PinKind::Pwm: return "orange";
    case PinKind::Camera: return "ribbon";
    default: return "yellow";
    }
}

ImU32 colorRGB(const std::string& c) {
    std::string s = low(c);
    if (s == "red") return IM_COL32(230, 50, 40, 255);
    if (s == "black") return IM_COL32(30, 30, 30, 255);
    if (s == "blue") return IM_COL32(40, 110, 240, 255);
    if (s == "yellow") return IM_COL32(240, 210, 40, 255);
    if (s == "green") return IM_COL32(40, 180, 70, 255);
    if (s == "white") return IM_COL32(235, 235, 235, 255);
    if (s == "orange") return IM_COL32(250, 140, 30, 255);
    if (s == "brown") return IM_COL32(130, 80, 40, 255);
    if (s == "purple") return IM_COL32(150, 70, 200, 255);
    if (s == "gray" || s == "grey") return IM_COL32(140, 140, 140, 255);
    return IM_COL32(200, 200, 200, 255);
}

} // namespace

ElecPart* App::elecFind(const std::string& nameOrId) {
    std::string l = low(nameOrId);
    for (auto& p : doc_.elec.parts)
        if (low(elec::partLabel(doc_.elec, p.id)) == l || std::to_string(p.id) == nameOrId) return &p;
    for (auto& p : doc_.elec.parts)
        if (low(p.label).find(l) != std::string::npos && !l.empty()) return &p;
    return nullptr;
}

// Where a part is drawn: the center of the body or component it is attached to.
bool App::elecWhere(const ElecPart& p, glm::dvec3& at) {
    if (p.attach.empty()) return false;
    Aabb bb;
    uint32_t cid = compByPath(p.attach);
    for (auto& [id, b] : doc_.bodies) {
        if (!doc_.isBodyAlive(b)) continue;
        bool hit = b.name == p.attach || aiBodyPath(id) == p.attach;
        if (!hit && cid) {
            for (uint32_t c = b.componentId, g = 0; c && g < 64; ++g) {
                if (c == cid) { hit = true; break; }
                auto it = doc_.components.find(c);
                if (it == doc_.components.end()) break;
                c = it->second.parentId;
            }
        }
        if (!hit) continue;
        auto pit = doc_.parts.find(b.partId);
        if (pit != doc_.parts.end() && pit->second.mesh) bb.add(pit->second.mesh->bounds.transformed(b.xform));
    }
    if (!bb.valid()) return false;
    at = glm::dvec3(bb.center());
    return true;
}

std::string App::wiringTool(const std::string& argsJson) {
    Value a = Value::parse(argsJson);
    std::string act = low(a["action"].asString("check"));
    ElecDoc& E = doc_.elec;
    auto changed = [&](const char* what) {
        pushUndo(what);
        doc_.touch();
        dirty_ = true;
    };
    auto partJson = [&](const PartDef& d, bool full) {
        Value o = Value::object();
        o.set("id", d.id);
        o.set("name", d.name);
        o.set("category", d.category);
        if (d.massG > 0) o.set("mass_g", d.massG);
        Value sp = Value::object();
        for (auto& [k, v] : d.specs) sp.set(k, v);
        o.set("specs", sp);
        if (!full) return o;
        Value pins = Value::array();
        for (auto& p : d.pins) {
            Value pj = Value::object();
            pj.set("id", p.id);
            pj.set("label", p.label);
            pj.set("kind", elec::kindName(p.kind));
            if (!p.connector.empty()) pj.set("where", p.connector);
            if (!p.note.empty()) pj.set("note", p.note);
            if (!p.must.empty()) pj.set("must", p.must);
            pins.push(pj);
        }
        o.set("pins", pins);
        Value notes = Value::array();
        for (auto& n : d.notes) notes.push(Value(n));
        o.set("notes", notes);
        Value src = Value::array();
        for (auto& n : d.sources) src.push(Value(n));
        o.set("sources", src);
        return o;
    };

    if (act == "catalog") {
        Value arr = Value::array();
        for (auto* d : elec::search(a["query"].asString())) arr.push(partJson(*d, false));
        return arr.size() ? arr.dump() : "No part matches. Try a shorter word (pi, servo, motor, driver, battery, imu).";
    }
    if (act == "part_info") {
        const PartDef* d = elec::findDef(a["part"].asString());
        if (!d) {
            if (ElecPart* ep = elecFind(a["part"].asString())) d = elec::findDef(ep->def);
        }
        return d ? partJson(*d, true).dump() : "No such part. Use action catalog.";
    }
    if (act == "list") {
        Value o = Value::object();
        Value parts = Value::array();
        for (auto& p : E.parts) {
            Value pj = Value::object();
            pj.set("label", elec::partLabel(E, p.id));
            pj.set("part", p.def);
            if (!p.attach.empty()) pj.set("attach", p.attach);
            for (auto& [k, v] : p.vals) pj.set(k, v);
            parts.push(pj);
        }
        Value wires = Value::array();
        for (auto& w : E.wires) {
            Value wj = Value::object();
            wj.set("id", (double)w.id);
            wj.set("from", elec::partLabel(E, w.a) + " " + w.aPin);
            wj.set("to", elec::partLabel(E, w.b) + " " + w.bPin);
            if (!w.color.empty()) wj.set("color", w.color);
            wj.set("awg", (double)w.awg);
            wires.push(wj);
        }
        o.set("parts", parts);
        o.set("wires", wires);
        return o.dump();
    }
    if (act == "add") {
        const PartDef* d = elec::findDef(a["part"].asString());
        if (!d) {
            auto hits = elec::search(a["part"].asString());
            if (hits.size() == 1) d = hits[0];
        }
        if (!d) return "Error: no part \"" + a["part"].asString() + "\". Use action catalog to find its id.";
        std::string label = a["label"].asString();
        if (label.empty()) label = d->name;
        if (elecFind(label) && low(elec::partLabel(E, elecFind(label)->id)) == low(label))
            return "Error: a part is already called \"" + label + "\". Give this one its own label.";
        changed("Add part");
        ElecPart p;
        p.id = E.nextId++;
        p.def = d->id;
        p.label = label;
        p.attach = a["attach"].asString();
        if (a["capacity_mAh"].isNumber()) p.vals["capacity_mAh"] = a["capacity_mAh"].asNumber();
        E.parts.push_back(p);
        return "Added " + label + " (" + d->name + "). Pins: use action part_info with part " + d->id + ".";
    }
    if (act == "set") {
        ElecPart* p = elecFind(a["label"].asString());
        if (!p) return "Error: no part \"" + a["label"].asString() + "\".";
        changed("Edit part");
        if (a.has("new_label")) p->label = a["new_label"].asString();
        if (a.has("attach")) p->attach = a["attach"].asString();
        if (a["capacity_mAh"].isNumber()) p->vals["capacity_mAh"] = a["capacity_mAh"].asNumber();
        return "Updated " + elec::partLabel(E, p->id) + ".";
    }
    if (act == "remove") {
        ElecPart* p = elecFind(a["label"].asString());
        if (!p) return "Error: no part \"" + a["label"].asString() + "\".";
        changed("Remove part");
        uint32_t id = p->id;
        E.wires.erase(std::remove_if(E.wires.begin(), E.wires.end(), [&](const Wire& w) { return w.a == id || w.b == id; }), E.wires.end());
        E.parts.erase(std::remove_if(E.parts.begin(), E.parts.end(), [&](const ElecPart& q) { return q.id == id; }), E.parts.end());
        return "Removed the part and its wires.";
    }
    if (act == "stack") {
        ElecPart* hat = elecFind(a["hat"].asString());
        ElecPart* pi = elecFind(a["pi"].asString());
        if (!hat || !pi) return "Error: name the HAT and the Pi by their labels.";
        const PartDef* dh = elec::findDef(hat->def);
        if (!dh || !dh->stacksOnPi) return "Error: " + elec::partLabel(E, hat->id) + " is not a HAT.";
        changed("Stack HAT");
        Wire w;
        w.id = E.nextId++;
        w.a = hat->id;
        w.b = pi->id;
        w.aPin = w.bPin = "HEADER";
        E.wires.push_back(w);
        return "Stacked " + elec::partLabel(E, hat->id) + " on " + elec::partLabel(E, pi->id) + ".";
    }
    if (act == "connect") {
        ElecPart* pa = elecFind(a["from_part"].asString());
        ElecPart* pb = elecFind(a["to_part"].asString());
        if (!pa || !pb) return "Error: name both parts by their labels (action list).";
        const PartDef* da = elec::findDef(pa->def);
        const PartDef* db = elec::findDef(pb->def);
        std::string ap = a["from_pin"].asString(), bp = a["to_pin"].asString();
        auto pinOf = [](const PartDef* d, std::string& id) -> const PinDef* {
            if (!d) return nullptr;
            if (const PinDef* p = d->pin(id)) return p;
            for (auto& p : d->pins)
                if (low(p.label) == low(id) || low(p.id) == low(id)) { id = p.id; return &p; }
            return nullptr;
        };
        const PinDef* pA = pinOf(da, ap);
        const PinDef* pB = pinOf(db, bp);
        if (!pA) return "Error: " + elec::partLabel(E, pa->id) + " has no pin \"" + a["from_pin"].asString() + "\" (action part_info).";
        if (!pB) return "Error: " + elec::partLabel(E, pb->id) + " has no pin \"" + a["to_pin"].asString() + "\" (action part_info).";
        changed("Add wire");
        Wire w;
        w.id = E.nextId++;
        w.a = pa->id;
        w.b = pb->id;
        w.aPin = ap;
        w.bPin = bp;
        w.color = a["color"].asString();
        if (w.color.empty()) w.color = colorFor(pA->kind == PinKind::Other ? pB : pA);
        w.awg = a["awg"].isNumber() ? (int)a["awg"].asNumber() : 0;
        w.note = a["note"].asString();
        if (w.awg <= 0) {
            // Thick enough for what flows: motors and battery leads get real wire.
            bool heavy = pA->kind == PinKind::MotorIn || pB->kind == PinKind::MotorIn || pA->kind == PinKind::MotorOut ||
                         pB->kind == PinKind::MotorOut || da->hasBattery || db->hasBattery;
            bool power = pA->kind == PinKind::PowerOut || pA->kind == PinKind::PowerIn || pB->kind == PinKind::PowerOut || pB->kind == PinKind::PowerIn;
            w.awg = heavy ? 20 : power ? 24 : 26;
        }
        E.wires.push_back(w);
        elec::Report r = elec::check(E);
        std::string about;
        for (auto& i : r.issues)
            if (std::find(i.wires.begin(), i.wires.end(), w.id) != i.wires.end() || i.text.find(elec::partLabel(E, pa->id)) != std::string::npos)
                if (i.sev != elec::Severity::Info) about += std::string(i.sev == elec::Severity::Error ? "\nstill to fix: " : "\nto check: ") + i.text;
        return "Wired " + elec::partLabel(E, pa->id) + " " + pA->label + " to " + elec::partLabel(E, pb->id) + " " + pB->label + " (" + w.color +
               ", " + std::to_string(w.awg) + " AWG)." + about;
    }
    if (act == "disconnect") {
        uint32_t wid = a["wire"].isNumber() ? (uint32_t)a["wire"].asNumber() : 0;
        ElecPart* pa = elecFind(a["from_part"].asString());
        ElecPart* pb = elecFind(a["to_part"].asString());
        size_t before = E.wires.size();
        auto match = [&](const Wire& w) {
            if (wid) return w.id == wid;
            if (!pa || !pb) return false;
            bool fw = w.a == pa->id && w.b == pb->id && (a["from_pin"].asString().empty() || w.aPin == a["from_pin"].asString());
            bool bw = w.b == pa->id && w.a == pb->id && (a["from_pin"].asString().empty() || w.bPin == a["from_pin"].asString());
            return fw || bw;
        };
        bool any = std::any_of(E.wires.begin(), E.wires.end(), match);
        if (!any) return "No such wire (action list shows wire ids).";
        changed("Remove wire");
        E.wires.erase(std::remove_if(E.wires.begin(), E.wires.end(), match), E.wires.end());
        return "Removed " + std::to_string(before - E.wires.size()) + " wire(s).";
    }
    if (act == "guide") return elec::guide(E, elec::check(E));
    return elec::summary(E, elec::check(E));
}

void App::drawWiringOverlay() {
    // Wires in the view between the parts they join (parts attached to bodies or components).
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float S = uiScale_;
    std::map<uint32_t, glm::dvec3> at;
    for (auto& p : doc_.elec.parts) {
        glm::dvec3 w;
        if (elecWhere(p, w)) at[p.id] = w;
    }
    double sag = std::max(5.0, sceneBounds_.valid() ? sceneBounds_.diag() * 0.03 : 10.0);
    for (auto& w : doc_.elec.wires) {
        auto ia = at.find(w.a), ib = at.find(w.b);
        if (ia == at.end() || ib == at.end()) continue;
        glm::dvec3 p0 = ia->second, p1 = ib->second, mid = (p0 + p1) * 0.5 - glm::dvec3(0, 0, sag);
        ImVec2 prev;
        bool havePrev = false;
        bool sel = (int)w.id == wiringSel_;
        for (int k = 0; k <= 16; ++k) {
            double t = k / 16.0;
            glm::dvec3 q = (1 - t) * (1 - t) * p0 + 2 * (1 - t) * t * mid + t * t * p1;
            ImVec2 s;
            if (!worldToScreen(q, s)) { havePrev = false; continue; }
            if (havePrev) {
                if (w.color == "black" || w.color.empty()) dl->AddLine(prev, s, IM_COL32(200, 200, 200, 120), (sel ? 6.f : 4.f) * S);
                dl->AddLine(prev, s, colorRGB(w.color.empty() ? "gray" : w.color), (sel ? 4.f : 2.f) * S);
            }
            prev = s;
            havePrev = true;
        }
    }
    for (auto& [id, w] : at) {
        ImVec2 s;
        if (!worldToScreen(w, s)) continue;
        dl->AddCircleFilled(s, 4 * S, IM_COL32(255, 107, 53, 230));
        if (wiringLabels_) dl->AddText(ImVec2(s.x + 6 * S, s.y - 7 * S), IM_COL32(255, 255, 255, 230), elec::partLabel(doc_.elec, id).c_str());
    }
}

void App::drawWiringWindow() {
    if (!wiringOpen_) return;
    drawWiringOverlay();
    const float S = uiScale_;
    ImGui::SetNextWindowSize(ImVec2(720 * S, 760 * S), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Electronics and wiring###wiringwin", &wiringOpen_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ElecDoc& E = doc_.elec;
    elec::Report rep = elec::check(E);
    auto act = [&](Value v) { std::string r = wiringTool(v.dump()); if (r.rfind("Error", 0) == 0) toast(r, ToastKind::Warning); return r; };

    ImGui::TextWrapped("Add the real parts, attach each one to the body or component it is, and wire pin to pin. Kestral checks "
                       "voltages, logic levels, current and shorts the way the real parts would see them, and writes the wiring guide.");
    ImGui::Checkbox("Show part names in the view", &wiringLabels_);
    ImGui::Separator();

    // ---- add a part
    if (ImGui::CollapsingHeader("Add a part", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(240 * S);
        ImGui::InputTextWithHint("##elq", "search: pi, servo, motor, driver...", &wiringQuery_);
        auto hits = elec::search(wiringQuery_);
        if (ImGui::BeginListBox("##elcat", ImVec2(-1, 130 * S))) {
            for (auto* d : hits) {
                std::string row = d->name + "   (" + d->category + ")";
                if (ImGui::Selectable(row.c_str(), wiringPick_ == d->id)) wiringPick_ = d->id;
                if (ImGui::IsItemHovered() && !d->specs.empty()) {
                    ImGui::BeginTooltip();
                    for (auto& [k, v] : d->specs) ImGui::Text("%s: %s", k.c_str(), v.c_str());
                    ImGui::EndTooltip();
                }
            }
            ImGui::EndListBox();
        }
        ImGui::SetNextItemWidth(240 * S);
        ImGui::InputTextWithHint("##ellab", "label, like Left drive motor", &wiringLabel_);
        ImGui::SameLine();
        ImGui::BeginDisabled(wiringPick_.empty());
        if (ImGui::Button("Add")) {
            std::vector<uint32_t> sel = selectedBodies();
            std::string attach = sel.empty() ? std::string() : aiBodyPath(sel[0]);
            Value v = Value::object();
            v.set("action", "add");
            v.set("part", wiringPick_);
            v.set("label", wiringLabel_);
            v.set("attach", attach);
            act(v);
            wiringLabel_.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(a selected body becomes where it sits)");
    }

    // ---- parts and their pins
    if (ImGui::CollapsingHeader("Parts and pins", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (E.parts.empty()) ImGui::TextDisabled("No parts yet.");
        uint32_t removeId = 0;
        for (auto& p : E.parts) {
            const PartDef* d = elec::findDef(p.def);
            ImGui::PushID((int)p.id);
            std::string head = elec::partLabel(E, p.id) + (d ? "  (" + d->name + ")" : "") + "###part";
            bool open = ImGui::TreeNode(head.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60 * S);
            if (ImGui::SmallButton("Remove")) removeId = p.id;
            if (open) {
                ImGui::SetNextItemWidth(260 * S);
                std::string at = p.attach;
                if (ImGui::InputTextWithHint("Sits on", "body or component path", &at, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    Value v = Value::object();
                    v.set("action", "set");
                    v.set("label", elec::partLabel(E, p.id));
                    v.set("attach", at);
                    act(v);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Use selection")) {
                    std::vector<uint32_t> sel = selectedBodies();
                    if (!sel.empty()) {
                        Value v = Value::object();
                        v.set("action", "set");
                        v.set("label", elec::partLabel(E, p.id));
                        v.set("attach", aiBodyPath(sel[0]));
                        act(v);
                    }
                }
                if (d && d->stacksOnPi) {
                    for (auto& q : E.parts) {
                        const PartDef* dq = elec::findDef(q.def);
                        if (!dq || !dq->pin("GPIO17") || dq->stacksOnPi) continue;
                        std::string b = "Press onto " + elec::partLabel(E, q.id);
                        if (ImGui::SmallButton(b.c_str())) {
                            Value v = Value::object();
                            v.set("action", "stack");
                            v.set("hat", elec::partLabel(E, p.id));
                            v.set("pi", elec::partLabel(E, q.id));
                            act(v);
                        }
                    }
                }
                if (d && ImGui::BeginTable("pins", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Pin");
                    ImGui::TableSetupColumn("Kind");
                    ImGui::TableSetupColumn("Where");
                    ImGui::TableSetupColumn("");
                    ImGui::TableHeadersRow();
                    for (auto& pin : d->pins) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        bool picked = wiringFromPart_ == p.id && wiringFromPin_ == pin.id;
                        ImGui::TextUnformatted(pin.label.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", elec::kindName(pin.kind));
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", pin.connector.c_str());
                        if (!pin.note.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pin.note.c_str());
                        ImGui::TableNextColumn();
                        ImGui::PushID(pin.id.c_str());
                        if (!wiringFromPart_) {
                            if (ImGui::SmallButton("Wire from")) { wiringFromPart_ = p.id; wiringFromPin_ = pin.id; }
                        } else if (picked) {
                            if (ImGui::SmallButton("Cancel")) { wiringFromPart_ = 0; wiringFromPin_.clear(); }
                        } else if (ImGui::SmallButton("Wire to here")) {
                            Value v = Value::object();
                            v.set("action", "connect");
                            v.set("from_part", elec::partLabel(E, wiringFromPart_));
                            v.set("from_pin", wiringFromPin_);
                            v.set("to_part", elec::partLabel(E, p.id));
                            v.set("to_pin", pin.id);
                            act(v);
                            wiringFromPart_ = 0;
                            wiringFromPin_.clear();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (removeId) {
            Value v = Value::object();
            v.set("action", "remove");
            v.set("label", std::to_string(removeId));
            act(v);
        }
        if (wiringFromPart_)
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "Wiring from %s %s: press Wire to here on the other pin.",
                               elec::partLabel(E, wiringFromPart_).c_str(), wiringFromPin_.c_str());
    }

    // ---- wires
    if (ImGui::CollapsingHeader("Wires", ImGuiTreeNodeFlags_DefaultOpen)) {
        uint32_t drop = 0;
        static const char* colors[] = {"red", "black", "yellow", "blue", "green", "white", "orange", "brown", "purple", "gray"};
        for (auto& w : E.wires) {
            ImGui::PushID((int)w.id);
            std::string t = w.aPin == "HEADER" ? elec::partLabel(E, w.a) + " pressed onto " + elec::partLabel(E, w.b)
                                               : elec::partLabel(E, w.a) + " " + w.aPin + "  ->  " + elec::partLabel(E, w.b) + " " + w.bPin;
            ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(colorRGB(w.color)), ImGuiColorEditFlags_NoTooltip, ImVec2(12 * S, 12 * S));
            ImGui::SameLine();
            if (ImGui::Selectable(t.c_str(), wiringSel_ == (int)w.id, 0, ImVec2(380 * S, 0))) wiringSel_ = (int)w.id;
            if (w.aPin != "HEADER") {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80 * S);
                if (ImGui::BeginCombo("##col", w.color.c_str())) {
                    for (auto* c : colors)
                        if (ImGui::Selectable(c, w.color == c)) { pushUndo("Wire color"); w.color = c; doc_.touch(); dirty_ = true; }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70 * S);
                int awg = w.awg;
                if (ImGui::InputInt("AWG", &awg, 2, 2)) {
                    pushUndo("Wire gauge");
                    w.awg = std::clamp(awg, 10, 32);
                    doc_.touch();
                    dirty_ = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) drop = w.id;
            ImGui::PopID();
        }
        if (drop) {
            Value v = Value::object();
            v.set("action", "disconnect");
            v.set("wire", (double)drop);
            act(v);
        }
        if (E.wires.empty()) ImGui::TextDisabled("No wires yet.");
    }

    // ---- check
    std::string head = "Check: " + std::to_string(rep.errors) + " to fix, " + std::to_string(rep.warnings) + " to check###elcheck";
    if (ImGui::CollapsingHeader(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (auto& i : rep.issues) {
            ImVec4 col = i.sev == elec::Severity::Error ? ImVec4(1.f, 0.35f, 0.3f, 1.f)
                         : i.sev == elec::Severity::Warning ? ImVec4(1.f, 0.75f, 0.25f, 1.f) : ImVec4(0.7f, 0.75f, 0.8f, 1.f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextWrapped("%s %s", i.sev == elec::Severity::Error ? "FIX" : i.sev == elec::Severity::Warning ? "CHECK" : "NOTE", i.text.c_str());
            ImGui::PopStyleColor();
        }
        for (auto& pl : rep.power)
            ImGui::TextDisabled("%s %s: %.2f A running, %.2f A peak of %.2f A", elec::partLabel(E, pl.part).c_str(), pl.pin.c_str(), pl.typA, pl.peakA, pl.maxA);
        if (rep.runtimeH > 0) ImGui::Text("Battery: about %.1f hours of driving", rep.runtimeH);
        if (rep.massG > 0) ImGui::TextDisabled("Electronics: %.0f g", rep.massG);
    }
    if (ImGui::Button("Copy the wiring guide")) {
        ImGui::SetClipboardText(elec::guide(E, rep).c_str());
        toast("Wiring guide copied");
    }
    ImGui::End();
}

} // namespace kst
