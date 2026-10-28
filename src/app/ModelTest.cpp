// Geometry self test: Kestral.exe --model-test drives the assistant tools on fresh designs and checks
// volumes, positions and body counts. Writes PASS / FAIL lines to kestral.log and to
// %TEMP%\kestral-model-test.txt (Linux: /tmp), exits with 1 when anything failed. It cannot click
// the UI, so dialogs and picking still need a person.
#include "app/App.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/SaveLoad.h"
#include "platform/Platform.h"
#include "kernel/Features.h"
#include "sketch/Profiles.h"
#include "sketch/Text.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>

namespace kst {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::string low(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string numStr(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}

std::string readMeasure(const std::string& text, const char* key, double want) {
    auto p = text.find(key);
    if (p == std::string::npos) return text;
    double v = std::atof(text.c_str() + p + std::strlen(key));
    if (std::abs(v - want) > 0.05) return text;
    return {};
}

} // namespace

void App::modelTestStep() {
    struct Step {
        const char* name;
        double timeout;
        std::function<void()> run;
        std::function<std::string()> check;
    };
    std::vector<Step> steps;
    static int step = 0, phase = 0;
    static double t0 = 0;
    static std::string log, last;
    static int sidA = 0, sidB = 0, sidC = 0, pathEdge = -1, textId = 0;
    static double textVol = 0, textSy = 0;
    static double expectX = 0;
    activity();
    static const std::string rtPath = (std::filesystem::temp_directory_path() / "kestral-step-roundtrip.step").string();
    auto bodyVol = [&](uint32_t id) {
        auto bit = doc_.bodies.find(id);
        if (bit == doc_.bodies.end() || !doc_.isBodyAlive(bit->second)) return -1.0;
        auto pit = doc_.parts.find(bit->second.partId);
        if (pit == doc_.parts.end() || !pit->second.shape) return -1.0;
        ShapePtr sh = pit->second.shape;
        glm::mat3 L(bit->second.xform);
        if (std::abs(std::abs(glm::determinant(L)) - 1.0) > 1e-3) sh = kernel::placeShape(sh, bit->second.xform);
        return std::abs(kernel::solidVolume(sh));
    };
    auto grab = [&](const char* key) {
        auto p = last.find(key);
        return p == std::string::npos ? 0 : std::atoi(last.c_str() + p + std::strlen(key));
    };
    auto call = [&](const std::string& n, const std::string& j) { last = aiRunTool(n, j); };
    auto volName = [&](const std::string& nm) {
        for (auto& [id, b] : doc_.bodies)
            if (doc_.isBodyAlive(b) && (b.name == nm || aiBodyPath(id) == nm)) return bodyVol(id);
        return -1.0;
    };
    auto sizeOf = [&](const std::string& nm) {
        for (auto& [id, b] : doc_.bodies) {
            if (!doc_.isBodyAlive(b) || b.name != nm) continue;
            auto pit = doc_.parts.find(b.partId);
            if (pit != doc_.parts.end() && pit->second.mesh && pit->second.mesh->bounds.valid())
                return pit->second.mesh->bounds.transformed(b.xform).max - pit->second.mesh->bounds.transformed(b.xform).min;
        }
        return glm::vec3(0);
    };
    auto nAlive = [&] {
        int n = 0;
        for (auto& [id, b] : doc_.bodies)
            if (doc_.isBodyAlive(b)) ++n;
        return n;
    };
    auto fails = [&] {
        std::string s;
        for (auto& f : doc_.timeline)
            if (f.failed && !f.error.empty()) s += f.name + ": " + f.error + " | ";
        return s;
    };
    auto near = [](double a, double b) { return std::abs(a - b) <= std::max(3.0, 0.04 * std::abs(b)); };
    auto fresh = [&] {
        if (dlg_.open) closeFeatureDialog(false);
        if (tdlg_.open) closeToolDialog(false);
        if (sk_.active) leaveSketchMode();
        newDocument();
        clearSelection();
        selFaces_.clear();
        selEdges_.clear();
        selProfiles_.clear();
    };
    {
        steps.push_back({"box", 30, [&] { fresh(); call("create_box", R"({"x":0,"y":0,"z":0,"width":10,"depth":20,"height":30,"name":"Block"})"); },
            [&] { if (!fails().empty()) return fails(); if (!near(volName("Block"), 6000)) return "vol " + numStr(volName("Block")); return std::string(); }});
        steps.push_back({"get_design", 15, [&] { call("get_design", "{}"); },
            [&] { if (last.find("volume_mm3") == std::string::npos || last.find("\"id\"") == std::string::npos) return std::string("missing volume or id"); return std::string(); }});
        steps.push_back({"get_body", 15, [&] { call("get_body", R"({"body":"Block"})"); },
            [&] { if (last.find("\"face_count\":6") == std::string::npos && last.find("\"face_count\": 6") == std::string::npos) return last.substr(0, 180); int planes = 0; for (size_t p = 0; (p = last.find("\"plane\"", p)) != std::string::npos; p += 7) ++planes; if (planes < 6) return "planes " + std::to_string(planes); return std::string(); }});
        steps.push_back({"param box", 30, [&] { fresh(); call("set_parameter", R"({"name":"Side","expression":"10"})"); },
            [&] { return last.find("Side") == std::string::npos ? last : std::string(); }});
        steps.push_back({"param box 2", 30, [&] { call("create_box", R"({"width":"Side","depth":"Side","height":"Side*2","name":"Expr"})"); },
            [&] { if (!fails().empty()) return fails(); if (!near(volName("Expr"), 2000)) return "vol " + numStr(volName("Expr")) + " " + last; return std::string(); }});
        steps.push_back({"param grows", 30, [&] { call("set_parameter", R"({"name":"Side","expression":"20"})"); },
            [&] { if (!fails().empty()) return fails(); if (!near(volName("Expr"), 16000)) return "vol " + numStr(volName("Expr")); return std::string(); }});
        steps.push_back({"cylinder Y", 30, [&] { fresh(); call("create_cylinder", R"({"diameter":10,"height":20,"axis":"y","name":"Rod"})"); },
            [&] { if (!fails().empty()) return fails(); auto s = sizeOf("Rod"); if (!near(volName("Rod"), kPi * 25 * 20)) return "vol " + numStr(volName("Rod")); if (!(s.y > 18 && s.y < 22)) return "size y " + numStr(s.y); return std::string(); }});
        steps.push_back({"tube", 30, [&] { fresh(); call("create_tube", R"({"diameter":20,"inner_diameter":10,"height":10,"name":"Tube"})"); },
            [&] { if (!fails().empty()) return fails(); if (nAlive() != 1) return "bodies " + std::to_string(nAlive()); if (!near(volName("Tube"), kPi * 75 * 10)) return "vol " + numStr(volName("Tube")); return std::string(); }});
        steps.push_back({"cone", 40, [&] { fresh(); call("create_cone", R"({"bottom_diameter":20,"top_diameter":0,"height":30,"axis":"z","name":"Cone"})"); },
            [&] { if (!fails().empty()) return fails(); if (!near(volName("Cone"), kPi * 100 * 30 / 3.0)) return "vol " + numStr(volName("Cone")); return std::string(); }});
        steps.push_back({"sphere", 40, [&] { fresh(); call("create_sphere", R"({"diameter":20,"x":200,"z":10,"name":"Ball"})"); },
            [&] { if (!fails().empty()) return fails(); if (!near(volName("Ball"), 4.0 / 3.0 * kPi * 1000)) return "vol " + numStr(volName("Ball")) + " " + last; return std::string(); }});
        steps.push_back({"torus", 40, [&] { fresh(); call("create_torus", R"({"major_diameter":60,"tube_diameter":10,"name":"Ring"})"); },
            [&] { if (!fails().empty()) return fails(); if (!near(volName("Ring"), 2 * kPi * 30 * kPi * 25)) return "vol " + numStr(volName("Ring")); return std::string(); }});
        steps.push_back({"wedge", 30, [&] { fresh(); call("create_wedge", R"({"length":10,"width":8,"height":6,"name":"Wedge"})"); },
            [&] {
                if (!fails().empty()) return fails();
                if (!near(volName("Wedge"), 240)) return "vol " + numStr(volName("Wedge"));
                for (auto& [id, b] : doc_.bodies) {
                    if (!doc_.isBodyAlive(b) || b.name != "Wedge") continue;
                    auto pit = doc_.parts.find(b.partId);
                    auto bb = pit->second.mesh->bounds.transformed(b.xform);
                    if (bb.min.x < -1 || bb.max.x < 8) return "x " + numStr(bb.min.x) + " " + numStr(bb.max.x);
                }
                return std::string();
            }});
        steps.push_back({"sketch extrude", 30, [&] { fresh(); call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id="); call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":0,"y":0,"w":10,"h":20}]})"); call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":5,"name":"Slab"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Slab"), 1000)) return "vol " + numStr(volName("Slab")) + " " + last; return std::string(); }});
        steps.push_back({"get_sketch", 10, [&] { call("get_sketch", "{\"sketch_id\":" + std::to_string(sidA) + "}"); },
            [&] { if (last.find("profiles") == std::string::npos) return last.substr(0, 160); return std::string(); }});
        steps.push_back({"revolve", 40, [&] { fresh(); call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id="); call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":0,"y":0,"w":10,"h":20}]})"); call("revolve", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"axis":"Y","angle":360,"name":"Spun"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Spun"), kPi * 100 * 20)) return "vol " + numStr(volName("Spun")); return std::string(); }});
        steps.push_back({"hole target", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":10,"name":"Low"})");
        }, [&] { return fails().empty() ? std::string() : fails(); }});
        steps.push_back({"hole target 2", 30, [&] { call("create_box", R"({"z":20,"width":40,"depth":40,"height":10,"name":"High"})"); },
            [&] { return fails().empty() ? std::string() : fails(); }});
        steps.push_back({"hole target 3", 40, [&] { call("create_hole", R"({"positions":[[20,20,30]],"diameter":10,"target_bodies":["High"]})"); },
            [&] {
                if (!fails().empty()) return fails();
                if (!near(volName("Low"), 16000)) return "low " + numStr(volName("Low"));
                if (!near(volName("High"), 16000 - kPi * 25 * 10)) return "high " + numStr(volName("High"));
                return std::string();
            }});
        steps.push_back({"tapped hole", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":20,"name":"Plate"})");
        }, [&] { return fails().empty() ? std::string() : fails(); }});
        steps.push_back({"tapped hole 2", 120, [&] {
            call("create_hole", R"({"positions":[[20,20,20]],"diameter":10,"depth":10,"drill_angle":118,"spotface_diameter":16,"spotface_depth":2,"tapped":true,"pitch":1.5,"target_bodies":["Plate"]})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double coneLen = 5.0 / std::tan(59.0 * kPi / 180.0);
            double bare = 32000.0 - kPi * (25.0 * 10.0 + 78.0 + 25.0 * coneLen / 3.0);
            double v = volName("Plate");
            if (!(v < bare - 40.0)) return "untapped " + numStr(v) + " bare " + numStr(bare);
            if (!(v > bare - 2000.0)) return "gone " + numStr(v);
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Plate") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                if (kernel::pointInside(pit->second.shape, b.xform, {20, 20, 8})) return std::string("point stayed");
                if (!kernel::pointInside(pit->second.shape, b.xform, {20, 20, 4})) return std::string("below tip gone");
                if (kernel::pointInside(pit->second.shape, b.xform, {26.5, 20, 19})) return std::string("spotface stayed");
                if (!kernel::pointInside(pit->second.shape, b.xform, {26.5, 20, 16})) return std::string("wall gone");
            }
            return std::string();
        }});
        steps.push_back({"extrude cut", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":10,"name":"Base"})");
        }, [&] { return std::string(); }});
        steps.push_back({"extrude cut 2", 30, [&] { call("create_box", R"({"z":20,"width":40,"depth":40,"height":10,"name":"Top"})"); },
            [&] { return std::string(); }});
        steps.push_back({"extrude cut 3", 40, [&] {
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":15,"y":15,"w":10,"h":10}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":40,"operation":"cut","target_bodies":["Top"]})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (!near(volName("Base"), 16000)) return "base " + numStr(volName("Base"));
            if (!near(volName("Top"), 15000)) return "top " + numStr(volName("Top"));
            return std::string();
        }});
        steps.push_back({"rotate", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":20,"height":30,"name":"Slab"})"); },
            [&] { return std::string(); }});
        steps.push_back({"rotate 2", 30, [&] { call("rotate", R"({"names":["Slab"],"axis":"x","angle":90})"); },
            [&] {
                if (!fails().empty()) return fails();
                auto s = sizeOf("Slab");
                if (!near(s.x, 10) || !near(s.y, 30) || !near(s.z, 20)) return "size " + numStr(s.x) + " " + numStr(s.y) + " " + numStr(s.z);
                return std::string();
            }});
        steps.push_back({"scale", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Cube"})"); },
            [&] { return std::string(); }});
        steps.push_back({"scale 2", 30, [&] { call("scale", R"({"names":["Cube"],"factor":2})"); },
            [&] { if (!fails().empty()) return fails(); if (!near(volName("Cube"), 8000)) return "vol " + numStr(volName("Cube")); return std::string(); }});
        steps.push_back({"transform", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Cube"})"); },
            [&] { return std::string(); }});
        steps.push_back({"transform 2", 30, [&] { call("set_transform", R"({"names":["Cube"],"matrix":[1,0,0,10, 0,1,0,0, 0,0,1,0, 0,0,0,1]})"); },
            [&] {
                if (!fails().empty()) return fails();
                for (auto& [id, b] : doc_.bodies) {
                    if (!doc_.isBodyAlive(b) || b.name != "Cube") continue;
                    auto pit = doc_.parts.find(b.partId);
                    glm::vec3 c = pit->second.mesh->bounds.transformed(b.xform).center();
                    if (!near(c.x, 15)) return "center " + numStr(c.x);
                }
                return std::string();
            }});
        steps.push_back({"combine", 40, [&] {
            fresh();
            call("create_box", R"({"width":10,"depth":10,"height":10,"name":"J1"})");
        }, [&] { return std::string(); }});
        steps.push_back({"combine 2", 30, [&] { call("create_box", R"({"x":9,"width":10,"depth":10,"height":10,"name":"J2"})"); },
            [&] { return std::string(); }});
        steps.push_back({"combine 3", 40, [&] { call("combine", R"({"target":"J1","tools":["J2"],"operation":"join"})"); },
            [&] { if (!fails().empty()) return fails(); if (nAlive() != 1) return "bodies " + std::to_string(nAlive()); double v = 0; for (auto& [id, b] : doc_.bodies) if (doc_.isBodyAlive(b)) v += bodyVol(id); if (!near(v, 1900)) return "vol " + numStr(v); return std::string(); }});
        steps.push_back({"circular", 30, [&] { fresh(); call("create_box", R"({"x":20,"width":4,"depth":4,"height":4,"name":"Peg"})"); },
            [&] { return std::string(); }});
        steps.push_back({"circular 2", 30, [&] { call("circular_pattern", R"({"names":["Peg"],"count":4,"angle":360,"axis":"z"})"); },
            [&] { if (!fails().empty()) return fails(); if (nAlive() != 4) return "bodies " + std::to_string(nAlive()); return std::string(); }});
        steps.push_back({"rect pat", 30, [&] { fresh(); call("create_box", R"({"width":5,"depth":5,"height":5,"name":"Cell"})"); },
            [&] { return std::string(); }});
        steps.push_back({"rect pat 2", 30, [&] { call("rect_pattern", R"({"names":["Cell"],"count_x":3,"spacing_x":20,"count_y":2,"spacing_y":15})"); },
            [&] { if (!fails().empty()) return fails(); if (nAlive() != 6) return "bodies " + std::to_string(nAlive()); return std::string(); }});
        steps.push_back({"mirror", 30, [&] { fresh(); call("create_box", R"({"x":10,"width":10,"depth":10,"height":10,"name":"M"})"); },
            [&] { return std::string(); }});
        steps.push_back({"mirror 2", 30, [&] { call("mirror", R"({"names":["M"],"plane":"YZ"})"); },
            [&] { if (!fails().empty()) return fails(); if (nAlive() != 2) return "bodies " + std::to_string(nAlive()); return std::string(); }});
        steps.push_back({"shell", 40, [&] { fresh(); call("create_box", R"({"width":20,"depth":20,"height":20,"name":"Sh"})"); },
            [&] { return std::string(); }});
        steps.push_back({"shell 2", 40, [&] { call("shell", R"({"names":["Sh"],"thickness":2})"); },
            [&] { if (!fails().empty()) return fails(); if (!near(volName("Sh"), 8000 - 4096)) return "vol " + numStr(volName("Sh")); return std::string(); }});
        steps.push_back({"shell out", 40, [&] { fresh(); call("create_box", R"({"width":20,"depth":20,"height":20,"name":"Out"})"); },
            [&] { return std::string(); }});
        steps.push_back({"shell out 2", 40, [&] { call("shell", R"({"names":["Out"],"thickness":2,"side":"outside"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Out"), 13824 - 8000)) return "vol " + numStr(volName("Out")); return std::string(); }});
        steps.push_back({"shell both", 40, [&] { fresh(); call("create_box", R"({"width":20,"depth":20,"height":20,"name":"Mid"})"); },
            [&] { return std::string(); }});
        steps.push_back({"shell both 2", 40, [&] { call("shell", R"({"names":["Mid"],"thickness":2,"side":"both"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Mid"), 10648 - 5832)) return "vol " + numStr(volName("Mid")); return std::string(); }});
        steps.push_back({"shell open", 40, [&] { fresh(); call("create_box", R"({"width":20,"depth":20,"height":20,"name":"Open"})"); },
            [&] { return std::string(); }});
        steps.push_back({"shell open 2", 40, [&] {
            int face = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Open") continue;
                auto pit = doc_.parts.find(b.partId);
                uint32_t n = kernel::faceCount(pit->second.shape);
                for (uint32_t i = 0; i < n; ++i) {
                    glm::dvec3 c, normal; bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, i, c, normal, planar) || !planar) continue;
                    if (normal.z > 0.9) face = (int)i;
                }
            }
            call("shell", "{\"names\":[\"Open\"],\"thickness\":2,\"remove_faces\":[" + std::to_string(face) + "]}");
        }, [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Open"), 8000.0 - 16.0 * 16.0 * 18.0)) return "vol " + numStr(volName("Open")); return std::string(); }});
        steps.push_back({"shell cyl", 40, [&] { fresh(); call("create_cylinder", R"({"diameter":20,"height":30,"name":"Can"})"); },
            [&] { return std::string(); }});
        steps.push_back({"shell cyl 2", 40, [&] { call("shell", R"({"names":["Can"],"thickness":2})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Can"), kPi * (3000.0 - 1664.0))) return "vol " + numStr(volName("Can")); return std::string(); }});
        steps.push_back({"shell chamfer", 40, [&] { fresh(); call("create_cylinder", R"({"diameter":20,"height":30,"name":"Cham"})"); },
            [&] { return std::string(); }});
        steps.push_back({"shell chamfer 2", 40, [&] { call("chamfer", R"({"names":["Cham"],"edges":"top","distance":2})"); },
            [&] { if (!fails().empty()) return fails() + last; return std::string(); }});
        steps.push_back({"shell chamfer 3", 40, [&] { call("shell", R"({"names":["Cham"],"thickness":2})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                double v = volName("Cham");
                if (!(v > 2000.0 && v < 8000.0)) return "vol " + numStr(v);
                return std::string();
            }});
        steps.push_back({"mirror folder", 30, [&] {
            fresh();
            call("create_component", R"({"name":"Test"})");
            call("create_box", R"({"x":10,"width":10,"depth":10,"height":10,"name":"M","component":"Test"})");
        }, [&] { return std::string(); }});
        steps.push_back({"mirror folder 2", 30, [&] { call("mirror", R"({"names":["Test/M"],"plane":"XZ"})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                int inFolder = 0;
                std::string paths;
                for (auto& [id, b] : doc_.bodies) {
                    if (!doc_.isBodyAlive(b)) continue;
                    std::string p = aiBodyPath(id);
                    paths += p + " ";
                    if (p.rfind("Test/", 0) == 0) ++inFolder;
                }
                if (nAlive() != 2 || inFolder != 2) return paths.empty() ? last : paths;
                return std::string();
            }});
        steps.push_back({"split", 40, [&] { fresh(); call("create_box", R"({"z":-10,"width":20,"depth":20,"height":20,"name":"Cut"})"); },
            [&] { return std::string(); }});
        steps.push_back({"split 2", 40, [&] { call("split_body", R"({"names":["Cut"],"plane":"XY"})"); },
            [&] {
                if (!fails().empty()) return fails();
                if (nAlive() != 2) return "bodies " + std::to_string(nAlive());
                for (auto& [id, b] : doc_.bodies)
                    if (doc_.isBodyAlive(b) && !near(bodyVol(id), 4000)) return "piece " + numStr(bodyVol(id));
                return std::string();
            }});
        steps.push_back({"split yz", 40, [&] { fresh(); call("create_box", R"({"x":-10,"width":20,"depth":10,"height":10,"name":"CutX"})"); },
            [&] { return std::string(); }});
        steps.push_back({"split yz 2", 40, [&] { call("split_body", R"({"names":["CutX"],"plane":"YZ"})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
                for (auto& [id, b] : doc_.bodies)
                    if (doc_.isBodyAlive(b) && !near(bodyVol(id), 1000)) return "piece " + numStr(bodyVol(id));
                return std::string();
            }});
        steps.push_back({"fillet edge", 40, [&] { fresh(); call("create_box", R"({"width":20,"depth":20,"height":20,"name":"F"})"); },
            [&] { return std::string(); }});
        steps.push_back({"fillet edge 2", 40, [&] { call("fillet", R"({"radius":2,"edge_refs":[{"body":"F","edges":[0]}]})"); },
            [&] { if (!fails().empty()) return fails() + last; double v = volName("F"); if (!(v < 8000 && v > 7000)) return "vol " + numStr(v); return std::string(); }});
        steps.push_back({"chamfer 2d", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Bevel"})"); },
            [&] { return std::string(); }});
        steps.push_back({"chamfer 2d 2", 40, [&] {
            pathEdge = -1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Bevel") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::edgeCount(pit->second.shape);
                for (uint32_t e = 0; e < n && pathEdge < 0; ++e) {
                    kernel::EdgeGeom g;
                    if (!kernel::edgeGeom(pit->second.shape, b.xform, e, g) || g.type != 0) continue;
                    if (std::abs(glm::length(g.b - g.a) - 10.0) < 0.05) pathEdge = (int)e;
                }
            }
            if (pathEdge < 0) return;
            call("chamfer", std::string("{\"names\":[\"Bevel\"],\"distance\":2,\"distance2\":4,\"edge_refs\":[{\"body\":\"Bevel\",\"edges\":[") + std::to_string(pathEdge) + "]}]}");
        }, [&] {
            if (pathEdge < 0) return std::string("no edge");
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive());
            double v = volName("Bevel");
            if (std::abs(v - 960.0) > 1.0) return "vol " + numStr(v);
            return std::string();
        }});
        steps.push_back({"full round", 30, [&] { fresh(); call("create_box", R"({"width":40,"depth":20,"height":4,"name":"Plate"})"); },
            [&] { return std::string(); }});
        steps.push_back({"full round 2", 40, [&] {
            pathEdge = -1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Plate") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::faceCount(pit->second.shape);
                for (uint32_t f = 0; f < n && pathEdge < 0; ++f) {
                    glm::dvec3 c, nm;
                    bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, f, c, nm, planar) || !planar) continue;
                    glm::dvec3 w = glm::dvec3(glm::dmat4(b.xform) * glm::dvec4(c, 1.0));
                    glm::dvec3 n = glm::normalize(glm::dmat3(b.xform) * nm);
                    if (std::abs(w.x - 40) < 0.05 && std::abs(w.y - 10) < 0.05 && std::abs(w.z - 2) < 0.05 && n.x > 0.9)
                        pathEdge = (int)f;
                }
            }
            if (pathEdge < 0) return;
            call("full_round", std::string("{\"body\":\"Plate\",\"face\":") + std::to_string(pathEdge) + "}");
        }, [&] {
            if (pathEdge < 0) return std::string("no face");
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive());
            double v = volName("Plate");
            double want = 3040.0 + 40.0 * kPi;
            if (std::abs(v - want) > 1.0) return "vol " + numStr(v) + " " + last;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Plate") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                if (kernel::pointInside(pit->second.shape, b.xform, {40, 10, 0})) return std::string("corner stayed");
                if (kernel::pointInside(pit->second.shape, b.xform, {39, 10, 4})) return std::string("top reached");
                if (!kernel::pointInside(pit->second.shape, b.xform, {20, 10, 2})) return std::string("core gone");
                if (pit->second.mesh && pit->second.mesh->bounds.valid()) {
                    float mx = pit->second.mesh->bounds.transformed(b.xform).max.x;
                    if (std::abs(mx - 40.f) > 0.15f) return "max x " + numStr(mx);
                }
            }
            return std::string();
        }});
        steps.push_back({"var fillet", 40, [&] {
            fresh();
            call("create_box", R"({"z":-10,"width":20,"depth":20,"height":20,"name":"VF"})");
        }, [&] { return std::string(); }});
        steps.push_back({"var fillet 2", 50, [&] {
            int edge = -1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "VF") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                std::vector<std::vector<glm::dvec3>> polys;
                kernel::edgePolylines(pit->second.shape, 100000, polys);
                glm::dmat4 xf(b.xform);
                for (size_t e = 0; e < polys.size(); ++e) {
                    if (polys[e].size() < 2) continue;
                    glm::dvec3 a = glm::dvec3(xf * glm::dvec4(polys[e].front(), 1.0));
                    glm::dvec3 c = glm::dvec3(xf * glm::dvec4(polys[e].back(), 1.0));
                    glm::dvec3 d = c - a;
                    double len = glm::length(d);
                    if (len > 1.0 && std::abs(d.z / len) > 0.999) { edge = (int)e; break; }
                }
            }
            if (edge < 0) { last = "no vertical edge"; return; }
            call("fillet", "{\"radius\":1,\"radius_end\":8,\"edge_refs\":[{\"body\":\"VF\",\"edges\":[" + std::to_string(edge) + "]}]}");
        }, [&] {
            if (last.find("no vertical") != std::string::npos) return last;
            if (!fails().empty()) return fails() + last;
            double v = volName("VF");
            if (!(v > 7800 && v < 7970)) return "vol " + numStr(v) + " " + last;
            return std::string();
        }});
        steps.push_back({"var fillet 3", 40, [&] { call("split_body", R"({"names":["VF"],"plane":"XY"})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
                double a = 0, b = 0;
                int n = 0;
                for (auto& [id, body] : doc_.bodies) {
                    if (!doc_.isBodyAlive(body)) continue;
                    if (n == 0) a = bodyVol(id);
                    else b = bodyVol(id);
                    ++n;
                }
                if (std::abs(a - b) < 40) return "halves " + numStr(a) + " " + numStr(b);
                return std::string();
            }});
        steps.push_back({"rib", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":20,"name":"Base"})");
        }, [&] { return std::string(); }});
        steps.push_back({"rib 2", 50, [&] {
            call("create_sketch", R"({"plane":"XY","offset":30})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,20],"p2":[40,20]}]})");
            call("rib", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"thickness":4,"flip":true,"names":["Base"],"name":"Base"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Base");
            if (!(v > 33400 && v < 33800)) return "vol " + numStr(v) + " " + last;
            float z0 = 0, z1 = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
                z0 = bb.min.z;
                z1 = bb.max.z;
            }
            if (!near(z1, 30)) return "top " + numStr(z1);
            if (!near(z0, 0)) return "bottom " + numStr(z0);
            return std::string();
        }});
        steps.push_back({"web", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":20,"name":"Base"})");
        }, [&] { return std::string(); }});
        steps.push_back({"web 2", 50, [&] {
            call("create_sketch", R"({"plane":"XZ","offset":-20})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[5,30],"p2":[35,30]}]})");
            call("web", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"thickness":4,"flip":true,"names":["Base"],"name":"Base"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Base");
            if (!(v > 33000 && v < 33400)) return "vol " + numStr(v) + " " + last;
            float x = 0, y = 0, z0 = 0, z1 = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
                x = bb.max.x - bb.min.x;
                y = bb.max.y - bb.min.y;
                z0 = bb.min.z;
                z1 = bb.max.z;
            }
            if (!near(x, 40)) return "width " + numStr(x);
            if (!near(y, 40)) return "depth " + numStr(y);
            if (!near(z1, 30)) return "top " + numStr(z1);
            if (!near(z0, 0)) return "bottom " + numStr(z0);
            return std::string();
        }});
        steps.push_back({"emboss", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":20,"name":"Base"})");
        }, [&] { return std::string(); }});
        steps.push_back({"emboss 2", 50, [&] {
            call("create_sketch", R"({"plane":"XY","offset":30})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":15,"y":15,"w":10,"h":10}]})");
            call("emboss", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"depth":4,"flip":true,"names":["Base"],"name":"Base"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Base");
            if (!(v > 32300 && v < 32500)) return "vol " + numStr(v) + " " + last;
            float z1 = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
                z1 = bb.max.z;
            }
            if (!near(z1, 24)) return "top " + numStr(z1);
            return std::string();
        }});
        steps.push_back({"boss", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":20,"name":"Base"})");
        }, [&] { return std::string(); }});
        steps.push_back({"boss 2", 50, [&] {
            call("boss", R"({"x":20,"y":20,"z":20,"diameter":20,"height":10,"hole":8,"axis":"z","names":["Base"],"name":"Base"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Base");
            double want = 32000.0 + kPi * 84.0 * 10.0;
            if (!(v > want - 200 && v < want + 200)) return "vol " + numStr(v) + " " + last;
            float z1 = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                z1 = pit->second.mesh->bounds.transformed(b.xform).max.z;
            }
            if (!near(z1, 30)) return "top " + numStr(z1);
            return std::string();
        }});
        steps.push_back({"snap", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":20,"name":"Base"})");
        }, [&] { return std::string(); }});
        steps.push_back({"snap 2", 50, [&] {
            call("snap", R"({"x":38,"y":20,"z":20,"length":12,"width":8,"thickness":2,"hook":2,"hook_length":3,"axis":"z","names":["Base"],"name":"Base"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Base");
            if (!(v > 32210 && v < 32270)) return "vol " + numStr(v) + " " + last;
            float x1 = 0, z0 = 0, z1 = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
                x1 = bb.max.x;
                z0 = bb.min.z;
                z1 = bb.max.z;
            }
            if (!(x1 > 40.5f && x1 < 41.5f)) return "side " + numStr(x1);
            if (!(z1 > 31.5f && z1 < 32.5f)) return "top " + numStr(z1);
            if (!near(z0, 0)) return "bottom " + numStr(z0);
            return std::string();
        }});
        steps.push_back({"rest", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":20,"name":"Base"})");
        }, [&] { return std::string(); }});
        steps.push_back({"rest 2", 50, [&] {
            call("create_sketch", R"({"plane":"XY","offset":20})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":15,"y":15,"w":10,"h":10}]})");
            call("rest", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"thickness":4,"names":["Base"],"name":"Base"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Base");
            if (!(v > 32350 && v < 32450)) return "vol " + numStr(v) + " " + last;
            float x = 0, y = 0, z0 = 0, z1 = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
                x = bb.max.x - bb.min.x;
                y = bb.max.y - bb.min.y;
                z0 = bb.min.z;
                z1 = bb.max.z;
            }
            if (!(z1 > 23.5f && z1 < 24.5f)) return "top " + numStr(z1);
            if (!near(z0, 0)) return "bottom " + numStr(z0);
            if (!near(x, 40)) return "width " + numStr(x);
            if (!near(y, 40)) return "depth " + numStr(y);
            return std::string();
        }});
        steps.push_back({"lip", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":20,"name":"Base"})");
        }, [&] { return std::string(); }});
        steps.push_back({"lip 2", 50, [&] {
            call("create_sketch", R"({"plane":"XY","offset":20})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":0,"y":0,"w":40,"h":40}]})");
            call("lip", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"width":4,"height":2,"names":["Base"],"name":"Base"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Base");
            if (!(v > 33080 && v < 33220)) return "vol " + numStr(v) + " " + last;
            float x = 0, y = 0, z0 = 0, z1 = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
                x = bb.max.x - bb.min.x;
                y = bb.max.y - bb.min.y;
                z0 = bb.min.z;
                z1 = bb.max.z;
            }
            if (!(z1 > 21.6f && z1 < 22.4f)) return "top " + numStr(z1);
            if (!near(z0, 0)) return "bottom " + numStr(z0);
            if (!near(x, 40)) return "width " + numStr(x);
            if (!near(y, 40)) return "depth " + numStr(y);
            return std::string();
        }});
        steps.push_back({"silhouette", 40, [&] {
            fresh();
            call("create_sphere", R"({"diameter":20,"name":"Ball"})");
        }, [&] { return std::string(); }});
        steps.push_back({"silhouette 2", 80, [&] {
            call("silhouette", R"({"axis":"z","names":["Ball"]})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
            double a = volName("Ball 1"), b = volName("Ball 2");
            if (!(a > 2000 && a < 2200)) return "a " + numStr(a) + " " + last;
            if (!(b > 2000 && b < 2200)) return "b " + numStr(b) + " " + last;
            bool upper = false, lower = false;
            for (auto& [id, body] : doc_.bodies) {
                if (!doc_.isBodyAlive(body)) continue;
                auto pit = doc_.parts.find(body.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                Aabb bb = pit->second.mesh->bounds.transformed(body.xform);
                if (bb.min.z > -0.6f && bb.max.z > 9.f) upper = true;
                if (bb.max.z < 0.6f && bb.min.z < -9.f) lower = true;
            }
            if (!upper || !lower) return "halves " + last;
            return std::string();
        }});
        steps.push_back({"joint", 40, [&] {
            fresh();
            call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Base"})");
            call("create_box", R"({"x":40,"width":10,"depth":10,"height":10,"name":"Cap"})");
        }, [&] { return std::string(); }});
        steps.push_back({"joint 2", 40, [&] {
            call("joint", R"({"stay":"Base","move":"Cap"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (!near(volName("Base"), 1000) || !near(volName("Cap"), 1000)) return "vol " + numStr(volName("Cap")) + " " + last;
            glm::vec3 base(0), cap(0);
            bool gotB = false, gotC = false;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                glm::vec3 c = pit->second.mesh->bounds.transformed(b.xform).center();
                if (b.name == "Base") { base = c; gotB = true; }
                if (b.name == "Cap") { cap = c; gotC = true; }
            }
            if (!gotB || !gotC) return "missing " + last;
            auto off = [](glm::vec3 c) { return std::abs(c.x - 5.f) > 0.5f || std::abs(c.y - 5.f) > 0.5f || std::abs(c.z - 5.f) > 0.5f; };
            if (off(base)) return "base " + numStr(base.x) + " " + numStr(base.y) + " " + numStr(base.z);
            if (off(cap)) return "cap " + numStr(cap.x) + " " + numStr(cap.y) + " " + numStr(cap.z);
            return std::string();
        }});
        steps.push_back({"revolute", 40, [&] {
            fresh();
            call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Base"})");
            call("create_box", R"({"x":40,"width":10,"depth":10,"height":10,"name":"Arm"})");
        }, [&] { return std::string(); }});
        steps.push_back({"revolute 2", 40, [&] {
            call("joint", R"({"stay":"Base","move":"Arm","type":"revolute","axis":"z","angle":90,"stay_x":5,"stay_y":5,"stay_z":5,"move_x":40,"move_y":5,"move_z":5})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (!near(volName("Base"), 1000) || !near(volName("Arm"), 1000)) return "vol " + numStr(volName("Arm")) + " " + last;
            glm::vec3 base(0), arm(0);
            bool gotB = false, gotA = false;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                glm::vec3 c = pit->second.mesh->bounds.transformed(b.xform).center();
                if (b.name == "Base") { base = c; gotB = true; }
                if (b.name == "Arm") { arm = c; gotA = true; }
            }
            if (!gotB || !gotA) return "missing " + last;
            if (std::abs(base.x - 5.f) > 0.5f || std::abs(base.y - 5.f) > 0.5f || std::abs(base.z - 5.f) > 0.5f)
                return "base " + numStr(base.x) + " " + numStr(base.y) + " " + numStr(base.z);
            if (std::abs(arm.x - 5.f) > 0.5f || std::abs(arm.y - 10.f) > 0.5f || std::abs(arm.z - 5.f) > 0.5f)
                return "arm " + numStr(arm.x) + " " + numStr(arm.y) + " " + numStr(arm.z);
            return std::string();
        }});
        steps.push_back({"slider", 40, [&] {
            fresh();
            call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Base"})");
            call("create_box", R"({"x":40,"width":10,"depth":10,"height":10,"name":"Shoe"})");
        }, [&] { return std::string(); }});
        steps.push_back({"slider 2", 40, [&] {
            call("joint", R"({"stay":"Base","move":"Shoe","type":"slider","axis":"z","distance":20})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (!near(volName("Base"), 1000) || !near(volName("Shoe"), 1000)) return "vol " + numStr(volName("Shoe")) + " " + last;
            glm::vec3 base(0), shoe(0);
            bool gotB = false, gotS = false;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                glm::vec3 c = pit->second.mesh->bounds.transformed(b.xform).center();
                if (b.name == "Base") { base = c; gotB = true; }
                if (b.name == "Shoe") { shoe = c; gotS = true; }
            }
            if (!gotB || !gotS) return "missing " + last;
            if (std::abs(base.x - 5.f) > 0.5f || std::abs(base.y - 5.f) > 0.5f || std::abs(base.z - 5.f) > 0.5f)
                return "base " + numStr(base.x) + " " + numStr(base.y) + " " + numStr(base.z);
            if (std::abs(shoe.x - 5.f) > 0.5f || std::abs(shoe.y - 5.f) > 0.5f || std::abs(shoe.z - 25.f) > 0.5f)
                return "shoe " + numStr(shoe.x) + " " + numStr(shoe.y) + " " + numStr(shoe.z);
            return std::string();
        }});
        steps.push_back({"component", 40, [&] {
            fresh();
            call("run_command", R"({"command_id":"new_component"})");
            call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Peg"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (!near(volName("Peg"), 1000)) return "vol " + numStr(volName("Peg")) + " " + last;
            std::string path;
            for (auto& [id, b] : doc_.bodies)
                if (doc_.isBodyAlive(b) && b.name == "Peg") path = aiBodyPath(id);
            if (path != "Component1/Peg") return path.empty() ? last : path;
            return std::string();
        }});
        steps.push_back({"component ground", 40, [&] {
            call("ground_component", R"({"name":"Component1"})");
            call("move", R"({"names":["Peg"],"x":10})");
        }, [&] {
            if (fails().find("grounded") == std::string::npos) return "no refuse " + fails() + last;
            if (!near(volName("Peg"), 1000)) return "vol " + numStr(volName("Peg")) + " " + last;
            glm::vec3 c(0);
            bool got = false;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Peg") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                c = pit->second.mesh->bounds.transformed(b.xform).center();
                got = true;
            }
            if (!got) return "missing " + last;
            if (std::abs(c.x - 5.f) > 0.5f || std::abs(c.y - 5.f) > 0.5f || std::abs(c.z - 5.f) > 0.5f)
                return "moved " + numStr(c.x) + " " + numStr(c.y) + " " + numStr(c.z);
            return std::string();
        }});
        steps.push_back({"asbuilt", 40, [&] {
            fresh();
            call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Base"})");
            call("create_box", R"({"x":40,"width":10,"depth":10,"height":10,"name":"Cap"})");
        }, [&] { return std::string(); }});
        steps.push_back({"asbuilt 2", 40, [&] {
            call("asbuilt_joint", R"({"stay":"Base","move":"Cap"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (!near(volName("Base"), 1000) || !near(volName("Cap"), 1000)) return "vol " + numStr(volName("Cap")) + " " + last;
            glm::vec3 base(0), cap(0);
            uint32_t baseFeat = 0, capFeat = 0;
            bool gotB = false, gotC = false;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                glm::vec3 c = pit->second.mesh->bounds.transformed(b.xform).center();
                if (b.name == "Base") { base = c; gotB = true; baseFeat = b.featureId; }
                if (b.name == "Cap") { cap = c; gotC = true; capFeat = b.featureId; }
            }
            if (!gotB || !gotC) return "missing " + last;
            bool owns = false;
            for (auto& ft : doc_.timeline)
                if (ft.type == FeatureType::AsBuilt && ft.id == capFeat) owns = true;
            if (!owns) return "owner " + last;
            if (baseFeat == capFeat) return "base locked " + last;
            auto offB = [](glm::vec3 c) { return std::abs(c.x - 5.f) > 0.5f || std::abs(c.y - 5.f) > 0.5f || std::abs(c.z - 5.f) > 0.5f; };
            auto offC = [](glm::vec3 c) { return std::abs(c.x - 45.f) > 0.5f || std::abs(c.y - 5.f) > 0.5f || std::abs(c.z - 5.f) > 0.5f; };
            if (offB(base)) return "base " + numStr(base.x) + " " + numStr(base.y) + " " + numStr(base.z);
            if (offC(cap)) return "cap " + numStr(cap.x) + " " + numStr(cap.y) + " " + numStr(cap.z);
            return std::string();
        }});
        steps.push_back({"rigid", 40, [&] {
            fresh();
            call("create_box", R"({"width":10,"depth":10,"height":10,"name":"A"})");
            call("create_box", R"({"x":20,"width":10,"depth":10,"height":10,"name":"B"})");
            call("create_box", R"({"x":40,"width":10,"depth":10,"height":10,"name":"C"})");
        }, [&] { return std::string(); }});
        steps.push_back({"rigid 2", 40, [&] {
            call("rigid_group", R"({"names":["A","B","C"]})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 3) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (!near(volName("A"), 1000) || !near(volName("B"), 1000) || !near(volName("C"), 1000)) return "vol " + numStr(volName("B")) + " " + last;
            glm::vec3 ca(0), cb(0), cc(0);
            uint32_t fa = 0, fb = 0, fc = 0;
            int got = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                glm::vec3 c = pit->second.mesh->bounds.transformed(b.xform).center();
                if (b.name == "A") { ca = c; fa = b.featureId; ++got; }
                if (b.name == "B") { cb = c; fb = b.featureId; ++got; }
                if (b.name == "C") { cc = c; fc = b.featureId; ++got; }
            }
            if (got != 3) return "missing " + last;
            bool owns = false;
            for (auto& ft : doc_.timeline)
                if (ft.type == FeatureType::Rigid && ft.id == fa && ft.id == fb && ft.id == fc) owns = true;
            if (!owns) return "owner " + last;
            auto off = [](glm::vec3 c, float x) { return std::abs(c.x - x) > 0.5f || std::abs(c.y - 5.f) > 0.5f || std::abs(c.z - 5.f) > 0.5f; };
            if (off(ca, 5.f)) return "A " + numStr(ca.x);
            if (off(cb, 25.f)) return "B " + numStr(cb.x);
            if (off(cc, 45.f)) return "C " + numStr(cc.x);
            return std::string();
        }});
        steps.push_back({"rigid 3", 40, [&] {
            call("move", R"({"names":["B"],"x":10})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 3) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (!near(volName("A"), 1000) || !near(volName("B"), 1000) || !near(volName("C"), 1000)) return "vol " + numStr(volName("A")) + " " + last;
            glm::vec3 ca(0), cb(0), cc(0);
            int got = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                glm::vec3 c = pit->second.mesh->bounds.transformed(b.xform).center();
                if (b.name == "A") { ca = c; ++got; }
                if (b.name == "B") { cb = c; ++got; }
                if (b.name == "C") { cc = c; ++got; }
            }
            if (got != 3) return "missing " + last;
            auto off = [](glm::vec3 c, float x) { return std::abs(c.x - x) > 0.5f || std::abs(c.y - 5.f) > 0.5f || std::abs(c.z - 5.f) > 0.5f; };
            if (off(ca, 15.f)) return "A " + numStr(ca.x);
            if (off(cb, 35.f)) return "B " + numStr(cb.x);
            if (off(cc, 55.f)) return "C " + numStr(cc.x);
            return std::string();
        }});
        steps.push_back({"section", 30, [&] {
            fresh();
            call("create_box", R"({"width":20,"depth":20,"height":20,"name":"Block"})");
        }, [&] { return std::string(); }});
        steps.push_back({"section 2", 20, [&] {
            call("section_analysis", R"({"plane":"XY","offset":10})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (!near(volName("Block"), 8000)) return "vol " + numStr(volName("Block")) + " " + last;
            if (!doc_.section.on || doc_.section.axis != 2 || doc_.section.flip) return "plane " + last;
            if (std::abs(doc_.section.offset - 10) > 0.01) return "offset " + numStr(doc_.section.offset);
            if (!doc_.section.keeps(glm::dvec3(0, 0, 5))) return std::string("low");
            if (doc_.section.keeps(glm::dvec3(0, 0, 15))) return std::string("high");
            return std::string();
        }});
        steps.push_back({"section 3", 20, [&] {
            call("section_analysis", R"({"plane":"XY","offset":10,"flip":true})");
        }, [&] {
            if (!near(volName("Block"), 8000)) return "vol " + numStr(volName("Block"));
            if (!doc_.section.on || !doc_.section.flip) return "flip " + last;
            if (!doc_.section.keeps(glm::dvec3(0, 0, 15))) return std::string("high");
            if (doc_.section.keeps(glm::dvec3(0, 0, 5))) return std::string("low");
            return std::string();
        }});
        steps.push_back({"section 4", 20, [&] {
            call("section_analysis", R"({"on":false})");
        }, [&] {
            if (doc_.section.on) return "still on " + last;
            if (!doc_.section.keeps(glm::dvec3(0, 0, 15))) return std::string("cut");
            if (!near(volName("Block"), 8000)) return "vol " + numStr(volName("Block"));
            return std::string();
        }});
        steps.push_back({"folder", 30, [&] { fresh(); call("create_component", R"({"name":"Wheel"})"); call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Hub","component":"Wheel"})"); },
            [&] { if (aiBodyPath([&]{ for (auto& [id, b] : doc_.bodies) if (doc_.isBodyAlive(b) && b.name=="Hub") return id; return 0u; }()) != "Wheel/Hub") {
                std::string p; for (auto& [id, b] : doc_.bodies) if (doc_.isBodyAlive(b)) p = aiBodyPath(id); return p.empty() ? last : p;
            } return std::string(); }});
        steps.push_back({"sweep", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"circle","x":0,"y":0,"r":5}]})");
            call("create_sketch", R"({"plane":"XZ"})"); sidB = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidB) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[0,30]}]})");
            call("sweep", "{\"profile_sketch\":" + std::to_string(sidA) + ",\"path_sketch\":" + std::to_string(sidB) + ",\"name\":\"Pipe\"}");
        }, [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Pipe"), kPi * 25 * 30)) return "vol " + numStr(volName("Pipe")); return std::string(); }});
        steps.push_back({"sweep twist", 50, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":-5,"y":-2,"w":10,"h":4}]})");
            call("create_sketch", R"({"plane":"XZ"})"); sidB = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidB) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[0,30]}]})");
            call("sweep", "{\"profile_sketch\":" + std::to_string(sidA) + ",\"path_sketch\":" + std::to_string(sidB) + ",\"twist\":90,\"name\":\"Twist\"}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (!near(volName("Twist"), 10.0 * 4.0 * 30.0)) return "vol " + numStr(volName("Twist"));
            float sx = 0, sy = 0, sz = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Twist") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
                sx = bb.max.x - bb.min.x;
                sy = bb.max.y - bb.min.y;
                sz = bb.max.z - bb.min.z;
            }
            if (!(sy > 8 && sx > 8)) return "size " + numStr(sx) + "," + numStr(sy) + "," + numStr(sz);
            if (!near(sz, 30)) return "height " + numStr(sz);
            return std::string();
        }});
        steps.push_back({"sweep rail", 50, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"circle","x":0,"y":0,"r":5}]})");
            call("create_sketch", R"({"plane":"XZ"})"); sidB = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidB) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[0,30]}]})");
            call("create_sketch", R"({"plane":"XZ"})"); sidC = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidC) + R"(,"entities":[{"type":"line","p1":[5,0],"p2":[10,30]}]})");
            call("sweep", "{\"profile_sketch\":" + std::to_string(sidA) + ",\"path_sketch\":" + std::to_string(sidB) + ",\"guide_sketch\":" + std::to_string(sidC) + ",\"name\":\"Horn\"}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Horn");
            double want = kPi * 30.0 / 3.0 * (100.0 + 50.0 + 25.0);
            if (std::abs(v - want) > 40.0) return "vol " + numStr(v) + " " + last;
            float sx = 0, sz = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Horn") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
                sx = bb.max.x - bb.min.x;
                sz = bb.max.z - bb.min.z;
            }
            if (!(sx > 16.0f && sx < 24.0f)) return "width " + numStr(sx);
            if (std::abs(sz - 30.f) > 0.5f) return "height " + numStr(sz);
            return std::string();
        }});
        steps.push_back({"loft", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"circle","x":0,"y":0,"r":10}]})");
            call("create_sketch", R"({"plane":"XY","offset":20})"); sidB = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidB) + R"(,"entities":[{"type":"circle","x":0,"y":0,"r":5}]})");
            call("loft", "{\"sketch_ids\":[" + std::to_string(sidA) + "," + std::to_string(sidB) + "],\"name\":\"Frust\"}");
        }, [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Frust"), 20.0 / 3.0 * kPi * 175)) return "vol " + numStr(volName("Frust")); return std::string(); }});
        steps.push_back({"loft rail", 50, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"circle","x":0,"y":0,"r":10}]})");
            call("create_sketch", R"({"plane":"XY","offset":40})"); sidB = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidB) + R"(,"entities":[{"type":"circle","x":0,"y":0,"r":10}]})");
            call("create_sketch", R"({"plane":"XZ"})"); sidC = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidC) + R"(,"entities":[{"type":"arc","p1":[10,0],"p2":[30,20],"p3":[10,40]}]})");
            call("loft", "{\"sketch_ids\":[" + std::to_string(sidA) + "," + std::to_string(sidB) + "],\"rails\":[" + std::to_string(sidC) + "],\"name\":\"RailLoft\"}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("RailLoft");
            if (!(v > 40000 && v < 110000)) return "vol " + numStr(v);
            float maxX = -1e9f, sz = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "RailLoft") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
                maxX = bb.max.x;
                sz = bb.max.z - bb.min.z;
            }
            if (!near(maxX, 30)) return "x " + numStr(maxX);
            if (!near(sz, 40)) return "height " + numStr(sz);
            return std::string();
        }});
        steps.push_back({"constrain", 15, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[10,0]}]})");
            int p0 = 0, p1 = 0;
            auto q = last.find("entities=");
            if (q != std::string::npos) std::sscanf(last.c_str() + q, "entities=%d,%d", &p0, &p1);
            call("sketch_constrain", "{\"sketch_id\":" + std::to_string(sidA) + ",\"constraints\":[{\"type\":\"distance\",\"refs\":[" + std::to_string(p0) + "," + std::to_string(p1) + "],\"value\":25}]}");
        }, [&] {
            auto it = doc_.sketches.find((uint32_t)sidA);
            if (it == doc_.sketches.end()) return last;
            for (auto& [id, e] : it->second.ents)
                if (e.type == sk::EntType::Line && !e.locked) {
                    double len = glm::length(it->second.pt(e.b) - it->second.pt(e.a));
                    if (!near(len, 25)) return "len " + numStr(len) + " " + last;
                }
            return std::string();
        }});
        steps.push_back({"sketch fillet", 15, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":0,"y":0,"w":20,"h":20}]})");
            auto it = doc_.sketches.find((uint32_t)sidA);
            uint32_t pt = 0;
            if (it != doc_.sketches.end())
                for (auto& [id, e] : it->second.ents)
                    if (e.type == sk::EntType::Point && !e.locked && std::abs(e.x) < 1e-6 && std::abs(e.y) < 1e-6) pt = id;
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + ",\"op\":\"fillet\",\"corner\":" + std::to_string(pt) + "}");
        }, [&] {
            auto it = doc_.sketches.find((uint32_t)sidA);
            if (it == doc_.sketches.end()) return last;
            int arcs = 0;
            for (auto& [id, e] : it->second.ents) if (e.type == sk::EntType::Arc) ++arcs;
            if (!arcs) return "no arc " + last;
            return std::string();
        }});
        steps.push_back({"fillet meet", 15, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[20,0]},{"type":"line","p1":[20,0],"p2":[20,20]}]})");
            auto it = doc_.sketches.find((uint32_t)sidA);
            uint32_t pt = 0;
            if (it != doc_.sketches.end())
                for (auto& [id, e] : it->second.ents)
                    if (e.type == sk::EntType::Point && std::abs(e.x - 20) < 1e-6 && std::abs(e.y) < 1e-6) pt = id;
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + ",\"op\":\"fillet\",\"corner\":" + std::to_string(pt) + ",\"radius\":3}");
        }, [&] {
            if (last.find("Filleted") == std::string::npos) return last;
            auto it = doc_.sketches.find((uint32_t)sidA);
            if (it == doc_.sketches.end()) return last;
            int arcs = 0;
            for (auto& [id, e] : it->second.ents) if (e.type == sk::EntType::Arc) ++arcs;
            if (!arcs) return "no arc " + last;
            if (sk_.active) return std::string("sketch window stayed open");
            return std::string();
        }});
        steps.push_back({"offset plane", 15, [&] { fresh(); call("offset_plane", R"({"plane":"XY","offset":40})"); },
            [&] {
                if (doc_.constructPlanes.empty()) return last;
                if (!near(doc_.constructPlanes.begin()->second.o.z, 40)) return "z " + numStr(doc_.constructPlanes.begin()->second.o.z);
                return std::string();
            }});
        steps.push_back({"angle plane", 15, [&] { fresh(); call("plane_at_angle", R"({"plane":"XY","axis":"x","angle":90})"); },
            [&] {
                if (doc_.constructPlanes.size() != 1) return last;
                auto& pl = doc_.constructPlanes.begin()->second;
                glm::dvec3 n = glm::normalize(glm::cross(glm::dvec3(pl.x), glm::dvec3(pl.y)));
                if (!near(n.y, -1)) return "ny " + numStr(n.y);
                if (glm::length(glm::dvec3(pl.o)) > 0.05) return "origin " + numStr(pl.o.z);
                return std::string();
            }});
        steps.push_back({"midplane", 15, [&] {
            fresh();
            call("offset_plane", R"({"plane":"XY","offset":40})");
            int pid = grab("plane_id=");
            call("midplane", "{\"plane\":\"XY\",\"other_plane\":" + std::to_string(pid) + "}");
        }, [&] {
            bool ok = false;
            for (auto& [id, pl] : doc_.constructPlanes)
                if (near(pl.o.z, 20) && std::abs(pl.o.x) < 0.05 && std::abs(pl.o.y) < 0.05) ok = true;
            if (!ok) return last;
            return std::string();
        }});
        steps.push_back({"3pt plane", 15, [&] {
            fresh();
            call("plane_3pt", R"({"x1":0,"y1":0,"z1":0,"x2":10,"y2":0,"z2":0,"x3":0,"y3":0,"z3":10})");
        }, [&] {
            if (doc_.constructPlanes.size() != 1) return last;
            auto& pl = doc_.constructPlanes.begin()->second;
            if (std::abs(pl.o.x) > 0.05 || std::abs(pl.o.y) > 0.05 || std::abs(pl.o.z) > 0.05) return "origin " + last;
            glm::dvec3 n = glm::normalize(glm::cross(glm::dvec3(pl.x), glm::dvec3(pl.y)));
            if (std::abs(n.x) > 0.05 || std::abs(n.y + 1.0) > 0.05 || std::abs(n.z) > 0.05)
                return "n " + numStr(n.x) + " " + numStr(n.y) + " " + numStr(n.z);
            return std::string();
        }});
        steps.push_back({"tan plane", 30, [&] { fresh(); call("create_cylinder", R"({"diameter":10,"height":20,"name":"Shaft"})"); },
            [&] { return std::string(); }});
        steps.push_back({"tan plane 2", 20, [&] {
            call("plane_tangent", R"({"body":"Shaft","angle":0})");
        }, [&] {
            if (doc_.constructPlanes.size() != 1) return last;
            auto& pl = doc_.constructPlanes.begin()->second;
            double rxy = std::hypot(pl.o.x, pl.o.y);
            if (std::abs(rxy - 5.0) > 0.05) return "r " + numStr(rxy);
            if (pl.o.z < -0.05 || pl.o.z > 20.05) return "z " + numStr(pl.o.z);
            glm::dvec3 n = glm::normalize(glm::cross(glm::dvec3(pl.x), glm::dvec3(pl.y)));
            if (std::abs(n.z) > 0.05) return "nz " + numStr(n.z);
            double dist = std::abs(glm::dot(n, glm::dvec3(0, 0, pl.o.z) - pl.o));
            if (std::abs(dist - 5.0) > 0.05) return "d " + numStr(dist);
            return std::string();
        }});
        steps.push_back({"axis", 15, [&] { fresh(); call("axis", R"({"x1":0,"y1":0,"z1":0,"x2":0,"y2":0,"z2":40})"); },
            [&] {
                if (doc_.constructAxes.size() != 1) return last;
                auto& ax = doc_.constructAxes.begin()->second;
                if (std::abs(ax.direction.x) > 0.05 || std::abs(ax.direction.y) > 0.05 || !near(ax.direction.z, 1))
                    return "dir " + numStr(ax.direction.z) + " " + last;
                if (glm::length(ax.origin) > 0.05) return "origin " + last;
                return std::string();
            }});
        steps.push_back({"axis edge", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":20,"height":30,"name":"Slab"})"); },
            [&] { return std::string(); }});
        steps.push_back({"axis edge 2", 20, [&] {
            pathEdge = -1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Slab") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::edgeCount(pit->second.shape);
                for (uint32_t e = 0; e < n && pathEdge < 0; ++e) {
                    kernel::EdgeGeom g;
                    if (!kernel::edgeGeom(pit->second.shape, b.xform, e, g) || g.type != 0) continue;
                    glm::dvec3 d = g.b - g.a;
                    if (std::abs(d.z) > 25 && std::abs(d.x) < 0.1 && std::abs(d.y) < 0.1) pathEdge = (int)e;
                }
            }
            if (pathEdge < 0) return;
            call("axis", std::string("{\"body\":\"Slab\",\"edge\":") + std::to_string(pathEdge) + "}");
        }, [&] {
            if (pathEdge < 0) return std::string("no edge");
            if (doc_.constructAxes.size() != 1) return last;
            auto& ax = doc_.constructAxes.begin()->second;
            if (std::abs(ax.direction.x) > 0.05 || std::abs(ax.direction.y) > 0.05 || std::abs(std::abs(ax.direction.z) - 1) > 0.05)
                return "dir " + last;
            bool xok = std::abs(ax.origin.x) < 0.05 || std::abs(ax.origin.x - 10) < 0.05;
            bool yok = std::abs(ax.origin.y) < 0.05 || std::abs(ax.origin.y - 20) < 0.05;
            bool zok = std::abs(ax.origin.z) < 0.05 || std::abs(ax.origin.z - 30) < 0.05;
            if (!xok || !yok || !zok) return "origin " + numStr(ax.origin.x) + "," + numStr(ax.origin.y) + "," + numStr(ax.origin.z);
            return std::string();
        }});
        steps.push_back({"axis cyl", 40, [&] { fresh(); call("create_cylinder", R"({"x":15,"y":4,"diameter":10,"height":20,"name":"Shaft"})"); },
            [&] { return std::string(); }});
        steps.push_back({"axis cyl 2", 20, [&] { call("axis", R"({"body":"Shaft"})"); },
            [&] {
                if (doc_.constructAxes.size() != 1) return last;
                auto& ax = doc_.constructAxes.begin()->second;
                if (std::abs(ax.direction.x) > 0.05 || std::abs(ax.direction.y) > 0.05 || std::abs(std::abs(ax.direction.z) - 1) > 0.05)
                    return "dir " + last;
                if (std::abs(ax.origin.x - 15) > 0.05 || std::abs(ax.origin.y - 4) > 0.05)
                    return "origin " + numStr(ax.origin.x) + "," + numStr(ax.origin.y) + "," + numStr(ax.origin.z);
                if (!(std::abs(ax.origin.z) < 0.05 || std::abs(ax.origin.z - 20) < 0.05))
                    return "z " + numStr(ax.origin.z);
                return std::string();
            }});
        steps.push_back({"point", 15, [&] { fresh(); call("point", R"({"x":12,"y":8,"z":3})"); },
            [&] {
                if (doc_.constructPoints.size() != 1) return last;
                auto p = doc_.constructPoints.begin()->second;
                if (!near(p.x, 12) || !near(p.y, 8) || !near(p.z, 3)) return "pt " + last;
                return std::string();
            }});
        steps.push_back({"point edge", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":20,"height":30,"name":"Slab"})"); },
            [&] { return std::string(); }});
        steps.push_back({"point edge 2", 20, [&] {
            pathEdge = -1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Slab") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::edgeCount(pit->second.shape);
                for (uint32_t e = 0; e < n && pathEdge < 0; ++e) {
                    kernel::EdgeGeom g;
                    if (!kernel::edgeGeom(pit->second.shape, b.xform, e, g) || g.type != 0) continue;
                    glm::dvec3 d = g.b - g.a;
                    if (std::abs(d.z) > 25 && std::abs(d.x) < 0.1 && std::abs(d.y) < 0.1) pathEdge = (int)e;
                }
            }
            if (pathEdge < 0) return;
            call("point", std::string("{\"body\":\"Slab\",\"edge\":") + std::to_string(pathEdge) + "}");
        }, [&] {
            if (pathEdge < 0) return std::string("no edge");
            if (doc_.constructPoints.size() != 1) return last;
            auto p = doc_.constructPoints.begin()->second;
            bool xok = std::abs(p.x) < 0.05 || std::abs(p.x - 10) < 0.05;
            bool yok = std::abs(p.y) < 0.05 || std::abs(p.y - 20) < 0.05;
            if (!xok || !yok || std::abs(p.z - 15) > 0.05)
                return "pt " + numStr(p.x) + "," + numStr(p.y) + "," + numStr(p.z);
            return std::string();
        }});
        steps.push_back({"point face", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":20,"height":30,"name":"Slab"})"); },
            [&] { return std::string(); }});
        steps.push_back({"point face 2", 20, [&] {
            pathEdge = -1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Slab") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::faceCount(pit->second.shape);
                for (uint32_t f = 0; f < n && pathEdge < 0; ++f) {
                    glm::dvec3 c, nm;
                    bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, f, c, nm, planar) || !planar) continue;
                    glm::dvec3 w = glm::dvec3(glm::dmat4(b.xform) * glm::dvec4(c, 1.0));
                    if (std::abs(w.x - 5) < 0.05 && std::abs(w.y - 10) < 0.05 && std::abs(w.z - 30) < 0.05) pathEdge = (int)f;
                }
            }
            if (pathEdge < 0) return;
            call("point", std::string("{\"body\":\"Slab\",\"face\":") + std::to_string(pathEdge) + "}");
        }, [&] {
            if (pathEdge < 0) return std::string("no face");
            if (doc_.constructPoints.size() != 1) return last;
            auto p = doc_.constructPoints.begin()->second;
            if (std::abs(p.x - 5) > 0.05 || std::abs(p.y - 10) > 0.05 || std::abs(p.z - 30) > 0.05)
                return "pt " + numStr(p.x) + "," + numStr(p.y) + "," + numStr(p.z);
            return std::string();
        }});
        steps.push_back({"point corner", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":20,"height":30,"name":"Slab"})"); },
            [&] { return std::string(); }});
        steps.push_back({"point corner 2", 20, [&] {
            pathEdge = -1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Slab") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                std::vector<glm::dvec3> pts;
                kernel::vertexPoints(pit->second.shape, b.xform, pts);
                for (uint32_t i = 0; i < pts.size() && pathEdge < 0; ++i)
                    if (std::abs(pts[i].x - 10) < 0.05 && std::abs(pts[i].y - 20) < 0.05 && std::abs(pts[i].z - 30) < 0.05)
                        pathEdge = (int)i;
            }
            if (pathEdge < 0) return;
            call("point", std::string("{\"body\":\"Slab\",\"vertex\":") + std::to_string(pathEdge) + "}");
        }, [&] {
            if (pathEdge < 0) return std::string("no corner");
            if (doc_.constructPoints.size() != 1) return last;
            auto p = doc_.constructPoints.begin()->second;
            if (std::abs(p.x - 10) > 0.05 || std::abs(p.y - 20) > 0.05 || std::abs(p.z - 30) > 0.05)
                return "pt " + numStr(p.x) + "," + numStr(p.y) + "," + numStr(p.z);
            return std::string();
        }});
        steps.push_back({"split face", 40, [&] { fresh(); call("create_box", R"({"width":20,"depth":20,"height":20,"name":"Slab"})"); },
            [&] { return std::string(); }});
        steps.push_back({"split face 2", 40, [&] { call("split_face", R"({"names":["Slab"],"plane":"XY","offset":10})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
                if (!near(volName("Slab"), 8000)) return "vol " + numStr(volName("Slab")) + " " + last;
                int faces = 0;
                for (auto& [id, b] : doc_.bodies) {
                    if (!doc_.isBodyAlive(b) || b.name != "Slab") continue;
                    auto pit = doc_.parts.find(b.partId);
                    if (pit != doc_.parts.end() && pit->second.shape) faces = (int)kernel::faceCount(pit->second.shape);
                }
                if (faces <= 6) return "faces " + std::to_string(faces) + " " + last;
                return std::string();
            }});
        steps.push_back({"split curve", 40, [&] { fresh(); call("create_box", R"({"width":20,"depth":20,"height":20,"name":"Slab"})"); },
            [&] { return std::string(); }});
        steps.push_back({"split curve 2", 40, [&] {
            call("create_sketch", R"({"plane":"XY"})");
            sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"circle","x":10,"y":10,"r":5}]})");
            call("split_face", "{\"names\":[\"Slab\"],\"sketch_id\":" + std::to_string(sidA) + "}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (!near(volName("Slab"), 8000)) return "vol " + numStr(volName("Slab")) + " " + last;
            int faces = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Slab") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit != doc_.parts.end() && pit->second.shape) faces = (int)kernel::faceCount(pit->second.shape);
            }
            if (faces <= 6) return "faces " + std::to_string(faces) + " " + last;
            return std::string();
        }});
        steps.push_back({"spline", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"spline","closed":true,"points":[[0,0],[20,0],[20,20],[0,20]]}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":10,"name":"Blob"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            int splines = 0;
            auto it = doc_.sketches.find((uint32_t)sidA);
            if (it != doc_.sketches.end())
                for (auto& [id, e] : it->second.ents) if (e.type == sk::EntType::Spline) ++splines;
            if (!splines) return "no spline " + last;
            std::vector<sk::Vec2> fit = {{0, 0}, {20, 0}, {20, 20}, {0, 20}};
            auto poly = sk::Sketch::sampleFitSpline(fit, true, 8);
            double area = 0;
            for (size_t i = 0; i + 1 < poly.size(); ++i)
                area += poly[i].x * poly[i + 1].y - poly[i].y * poly[i + 1].x;
            area = std::abs(area) * 0.5;
            double v = volName("Blob");
            if (!near(v, area * 10)) return "vol " + numStr(v) + " want " + numStr(area * 10);
            return std::string();
        }});
        steps.push_back({"text", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"text","text":"I","x":0,"y":0,"height":10}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":5,"name":"Letter"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (!near(volName("Letter"), 100)) return "vol " + numStr(volName("Letter")) + " " + last;
            return std::string();
        }});
        steps.push_back({"text font", 60, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"text","text":"I","x":0,"y":0,"height":10,"font":"Arial"}]})");
            textId = grab("text_id=");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (!textId) return std::string("no text_id ") + last;
            return std::string();
        }});
        steps.push_back({"text font 2", 60, [&] {
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":5,"name":"Glyph"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Glyph");
            auto s = sizeOf("Glyph");
            if (!(v > 20)) return "vol " + numStr(v) + " " + last;
            if (!(s.y > 4 && s.y < 11)) return "height " + numStr(s.y) + " " + last;
            if (!(s.x > 0 && s.x < s.y)) return "width " + numStr(s.x) + " " + last;
            if (!(s.z > 4.5 && s.z < 5.5)) return "depth " + numStr(s.z);
            textVol = v;
            textSy = s.y;
            return std::string();
        }});
        steps.push_back({"text font 3", 60, [&] {
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + ",\"entities\":[{\"type\":\"text\",\"text\":\"I\",\"text_id\":"
                + std::to_string(textId) + ",\"height\":20,\"font\":\"Arial\"}]}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Glyph");
            auto s = sizeOf("Glyph");
            double want = textVol * 4.0;
            if (!(want > 0) || std::abs(v - want) > 0.03 * want) return "vol " + numStr(v) + " want " + numStr(want);
            if (!(textSy > 0) || std::abs(s.y - textSy * 2.0) > 0.05 * textSy * 2.0) return "height " + numStr(s.y);
            return std::string();
        }});
        auto centersNamed = [&](const std::string& prefix, std::vector<glm::vec3>& out) {
            out.clear();
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                if (b.name != prefix && b.name.rfind(prefix + ":", 0) != 0) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                out.push_back(pit->second.mesh->bounds.transformed(b.xform).center());
            }
        };
        steps.push_back({"path", 30, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[40,0]},{"type":"line","p1":[40,0],"p2":[80,0]}]})");
            call("create_box", R"({"width":4,"depth":4,"height":4,"name":"Peg"})");
        }, [&] { return std::string(); }});
        steps.push_back({"path 2", 30, [&] {
            call("pattern_on_path", "{\"names\":[\"Peg\"],\"sketch_id\":" + std::to_string(sidA) + R"(,"count":4,"distance":20})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 4) return "bodies " + std::to_string(nAlive()) + " " + last;
            std::vector<glm::vec3> cs;
            centersNamed("Peg", cs);
            if (cs.size() != 4) return "centers " + std::to_string(cs.size());
            std::sort(cs.begin(), cs.end(), [](const glm::vec3& a, const glm::vec3& b) { return a.x < b.x; });
            float wantX[4] = {2, 22, 42, 62};
            for (int i = 0; i < 4; ++i) {
                if (!near(cs[i].x, wantX[i]) || !near(cs[i].y, 2) || !near(cs[i].z, 2))
                    return "at " + numStr(cs[i].x) + "," + numStr(cs[i].y) + "," + numStr(cs[i].z);
            }
            return std::string();
        }});
        steps.push_back({"path flip", 30, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[60,0]}]})");
            call("create_box", R"({"width":4,"depth":4,"height":4,"name":"Rev"})");
        }, [&] { return std::string(); }});
        steps.push_back({"path flip 2", 30, [&] {
            call("pattern_on_path", "{\"names\":[\"Rev\"],\"sketch_id\":" + std::to_string(sidA) + R"(,"count":3,"distance":20,"flip":true})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 3) return "bodies " + std::to_string(nAlive()) + " " + last;
            std::vector<glm::vec3> cs;
            centersNamed("Rev", cs);
            if (cs.size() != 3) return "centers " + std::to_string(cs.size());
            std::sort(cs.begin(), cs.end(), [](const glm::vec3& a, const glm::vec3& b) { return a.x < b.x; });
            float wantX[3] = {-38, -18, 2};
            for (int i = 0; i < 3; ++i)
                if (!near(cs[i].x, wantX[i]) || !near(cs[i].y, 2))
                    return "at " + numStr(cs[i].x) + "," + numStr(cs[i].y);
            return std::string();
        }});
        steps.push_back({"path arc", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"arc","center":[0,0],"start":[30,0],"end":[0,30]}]})");
            call("create_box", R"({"x":30,"width":20,"depth":4,"height":4,"name":"ArcPeg"})");
        }, [&] { return std::string(); }});
        steps.push_back({"path arc 2", 40, [&] {
            double arcLen = 30.0 * kPi * 0.5;
            call("pattern_on_path", "{\"names\":[\"ArcPeg\"],\"sketch_id\":" + std::to_string(sidA) +
                 ",\"count\":2,\"distance\":" + numStr(arcLen) + R"(,"distance_type":"extent","orientation":"path"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
            std::vector<glm::vec3> cs;
            centersNamed("ArcPeg", cs);
            if (cs.size() != 2) return "centers " + std::to_string(cs.size());
            glm::vec3 moved = glm::length(cs[0] - glm::vec3(40, 2, 2)) > glm::length(cs[1] - glm::vec3(40, 2, 2)) ? cs[0] : cs[1];
            if (!near(moved.x, -2) || !near(moved.y, 40) || !near(moved.z, 2))
                return "moved " + numStr(moved.x) + "," + numStr(moved.y) + "," + numStr(moved.z);
            return std::string();
        }});
        steps.push_back({"path edge", 30, [&] { fresh(); call("create_box", R"({"width":40,"depth":4,"height":4,"name":"Rail"})"); },
            [&] { return std::string(); }});
        steps.push_back({"path edge 2", 30, [&] {
            pathEdge = -1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Rail") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::edgeCount(pit->second.shape);
                for (uint32_t e = 0; e < n && pathEdge < 0; ++e) {
                    kernel::EdgeGeom g;
                    if (!kernel::edgeGeom(pit->second.shape, b.xform, e, g) || g.type != 0) continue;
                    glm::dvec3 d = g.b - g.a;
                    if (std::abs(d.x) > 30 && std::abs(d.y) < 0.1 && std::abs(d.z) < 0.1 && std::abs(g.a.z) < 0.1)
                        pathEdge = (int)e;
                }
            }
            if (pathEdge < 0) return;
            call("pattern_on_path", std::string("{\"names\":[\"Rail\"],\"edge_body\":\"Rail\",\"edge\":") + std::to_string(pathEdge) +
                 R"(,"count":3,"distance":15})");
        }, [&] {
            if (pathEdge < 0) return std::string("no edge");
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 3) return "bodies " + std::to_string(nAlive()) + " " + last;
            std::vector<glm::vec3> cs;
            centersNamed("Rail", cs);
            if (cs.size() != 3) return "centers " + std::to_string(cs.size());
            std::vector<float> xs;
            for (auto& c : cs) {
                xs.push_back(c.x);
                if (!near(c.y, 2) || !near(c.z, 2)) return "yz " + numStr(c.y) + "," + numStr(c.z);
            }
            std::sort(xs.begin(), xs.end());
            bool neg = near(xs[0], -10) && near(xs[1], 5) && near(xs[2], 20);
            bool pos = near(xs[0], 20) && near(xs[1], 35) && near(xs[2], 50);
            if (!neg && !pos) return "xs " + numStr(xs[0]) + "," + numStr(xs[1]) + "," + numStr(xs[2]);
            return std::string();
        }});
        steps.push_back({"measure", 10, [&] { call("measure", R"({"points":[[0,0,0],[3,4,0]]})"); },
            [&] { if (last.find("distance_mm=5") == std::string::npos) return last; return std::string(); }});
        steps.push_back({"measure angle", 10, [&] { call("measure", R"({"kind":"angle","points":[[1,0,0],[0,0,0],[0,1,0]]})"); },
            [&] { return readMeasure(last, "angle_deg=", 90); }});
        steps.push_back({"measure area", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Cube"})"); },
            [&] { return fails().empty() ? std::string() : fails(); }});
        steps.push_back({"measure area 2", 15, [&] { call("measure", R"({"kind":"area","body":"Cube"})"); },
            [&] { return readMeasure(last, "area_mm2=", 100); }});
        steps.push_back({"measure loop", 15, [&] { call("measure", R"({"kind":"loop","body":"Cube"})"); },
            [&] { return readMeasure(last, "loop_mm=", 40); }});
        steps.push_back({"measure corner", 15, [&] {
            int f0 = -1, f1 = -1;
            glm::dvec3 n0(0);
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Cube") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::faceCount(pit->second.shape);
                for (uint32_t i = 0; i < n; ++i) {
                    glm::dvec3 c, normal; bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, i, c, normal, planar) || !planar) continue;
                    if (f0 < 0) { f0 = (int)i; n0 = glm::normalize(normal); }
                    else if (std::abs(glm::dot(glm::normalize(normal), n0)) < 0.1) { f1 = (int)i; break; }
                }
            }
            if (f0 < 0 || f1 < 0) { last = "no faces"; return; }
            call("measure", "{\"kind\":\"angle\",\"body\":\"Cube\",\"face\":" + std::to_string(f0) + ",\"face2\":" + std::to_string(f1) + "}");
        }, [&] { return readMeasure(last, "angle_deg=", 90); }});
        steps.push_back({"measure radius", 30, [&] { fresh(); call("create_cylinder", R"({"diameter":10,"height":10,"name":"Rod"})"); },
            [&] { return fails().empty() ? std::string() : fails(); }});
        steps.push_back({"measure radius 2", 15, [&] { call("measure", R"({"kind":"radius","body":"Rod"})"); },
            [&] { return readMeasure(last, "radius_mm=", 5); }});
        steps.push_back({"interfere", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":10,"height":10,"name":"P"})"); },
            [&] { return std::string(); }});
        steps.push_back({"interfere 2", 30, [&] { call("create_box", R"({"x":5,"width":10,"depth":10,"height":10,"name":"Q"})"); },
            [&] { return std::string(); }});
        steps.push_back({"interfere 3", 30, [&] { call("interference", R"({"names":["P","Q"]})"); },
            [&] { if (last.find("P") == std::string::npos || last.find("Q") == std::string::npos) return last; return std::string(); }});
        steps.push_back({"align", 40, [&] {
            fresh();
            call("create_box", R"({"width":10,"depth":10,"height":10,"name":"A"})");
        }, [&] { return std::string(); }});
        steps.push_back({"align 2", 30, [&] { call("create_box", R"({"x":50,"width":10,"depth":10,"height":10,"name":"B"})"); },
            [&] { return std::string(); }});
        steps.push_back({"align 3", 40, [&] {
            int fa = 0, fb = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                uint32_t n = kernel::faceCount(pit->second.shape);
                for (uint32_t i = 0; i < n; ++i) {
                    glm::dvec3 c, normal; bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, i, c, normal, planar) || !planar) continue;
                    glm::dvec3 nw = glm::normalize(glm::dmat3(b.xform) * normal);
                    if (b.name == "A" && nw.x > 0.9) fa = (int)i;
                    if (b.name == "B" && nw.x < -0.9) fb = (int)i;
                }
            }
            call("align", "{\"source_body\":\"A\",\"source_kind\":\"face\",\"source_index\":" + std::to_string(fa) +
                 ",\"target_body\":\"B\",\"target_kind\":\"face\",\"target_index\":" + std::to_string(fb) + "}");
            expectX = 45;
        }, [&] {
            if (!fails().empty()) return fails() + last;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "A") continue;
                auto pit = doc_.parts.find(b.partId);
                float x = pit->second.mesh->bounds.transformed(b.xform).center().x;
                if (!near(x, expectX)) return "center " + numStr(x);
            }
            return std::string();
        }});
        steps.push_back({"undo", 20, [&] { fresh(); call("create_box", R"({"width":8,"depth":8,"height":8,"name":"U"})"); },
            [&] { return std::string(); }});
        steps.push_back({"undo 2", 15, [&] { call("run_command", R"({"command_id":"undo"})"); },
            [&] { if (last.find("Undid") == std::string::npos) return last; if (nAlive() != 0) return "still " + std::to_string(nAlive()); return std::string(); }});
        steps.push_back({"coil", 40, [&] { fresh(); call("create_coil", R"({"diameter":20,"height":30,"turns":3,"section":2,"name":"Spring"})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                double len = std::sqrt(std::pow(2 * kPi * 10 * 3, 2) + 30 * 30);
                if (!near(volName("Spring"), kPi * len)) return "vol " + numStr(volName("Spring"));
                return std::string();
            }});
        steps.push_back({"pipe", 40, [&] { fresh(); call("pipe", R"({"diameter":10,"length":40,"name":"Hose"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Hose"), kPi * 25 * 40)) return "vol " + numStr(volName("Hose")); return std::string(); }});
        steps.push_back({"pipe hole", 40, [&] { fresh(); call("pipe", R"({"diameter":10,"inner_diameter":6,"length":40,"name":"Hose2"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (nAlive() != 1) return "bodies " + std::to_string(nAlive()); if (!near(volName("Hose2"), kPi * 16 * 40)) return "vol " + numStr(volName("Hose2")); return std::string(); }});
        steps.push_back({"pipe path", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XZ"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[0,30]}]})");
            call("pipe", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"diameter":8,"name":"Hose3"})");
        }, [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Hose3"), kPi * 16 * 30)) return "vol " + numStr(volName("Hose3")); return std::string(); }});
        steps.push_back({"press", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Pull"})"); },
            [&] { return std::string(); }});
        steps.push_back({"press 2", 30, [&] { call("press_pull", R"({"names":["Pull"],"distance":5,"direction":"z"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Pull"), 1500)) return "vol " + numStr(volName("Pull")); return std::string(); }});
        steps.push_back({"replace", 30, [&] { fresh(); call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Cap"})"); },
            [&] { return std::string(); }});
        steps.push_back({"replace 2", 40, [&] { call("replace_face", R"({"names":["Cap"],"plane":"XY","offset":15})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Cap"), 1500)) return "vol " + numStr(volName("Cap")); return std::string(); }});
        steps.push_back({"box btn", 30, [&] { fresh(); call("run_command", R"({"command_id":"box"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Box1"), 1000)) return "vol " + numStr(volName("Box1")) + " " + last; return std::string(); }});
        steps.push_back({"cyl btn", 30, [&] { fresh(); call("run_command", R"({"command_id":"cylinder"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!near(volName("Cyl1"), kPi * 25 * 10)) return "vol " + numStr(volName("Cyl1")) + " " + last; return std::string(); }});
        steps.push_back({"sphere btn", 40, [&] { fresh(); call("run_command", R"({"command_id":"sphere"})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
                double want = 4.0 / 3.0 * kPi * 125.0;
                if (!near(volName("Sphere1"), want)) return "vol " + numStr(volName("Sphere1")) + " want " + numStr(want) + " " + last;
                return std::string();
            }});
        steps.push_back({"cone btn", 40, [&] { fresh(); call("run_command", R"({"command_id":"cone"})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
                double want = kPi * 25.0 * 10.0 / 3.0;
                if (!near(volName("Cone1"), want)) return "vol " + numStr(volName("Cone1")) + " want " + numStr(want) + " " + last;
                return std::string();
            }});
        steps.push_back({"torus btn", 40, [&] { fresh(); call("run_command", R"({"command_id":"torus"})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
                double want = 2.0 * kPi * 10.0 * kPi * 4.0;
                if (!near(volName("Torus1"), want)) return "vol " + numStr(volName("Torus1")) + " want " + numStr(want) + " " + last;
                return std::string();
            }});
        steps.push_back({"wedge btn", 30, [&] { fresh(); call("run_command", R"({"command_id":"wedge"})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
                if (!near(volName("Wedge1"), 500)) return "vol " + numStr(volName("Wedge1")) + " " + last;
                return std::string();
            }});
        steps.push_back({"tube btn", 40, [&] { fresh(); call("run_command", R"({"command_id":"tube"})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
                double want = kPi * 16.0 * 10.0;
                if (!near(volName("Tube1"), want)) return "vol " + numStr(volName("Tube1")) + " want " + numStr(want) + " " + last;
                return std::string();
            }});
        steps.push_back({"to face", 30, [&] { fresh(); call("create_box", R"({"width":40,"depth":40,"height":20,"name":"Block"})"); },
            [&] { return std::string(); }});
        steps.push_back({"to face 2", 40, [&] {
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":0,"y":0,"w":10,"h":10}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"to_body":"Block","operation":"new","name":"Peg"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (!near(volName("Peg"), 2000)) return "vol " + numStr(volName("Peg")) + " " + last;
            if (!near(volName("Block"), 32000)) return "block " + numStr(volName("Block"));
            return std::string();
        }});
        steps.push_back({"draft", 40, [&] { fresh(); call("create_box", R"({"width":20,"depth":20,"height":20,"name":"Mold"})"); },
            [&] { return std::string(); }});
        steps.push_back({"draft 2", 40, [&] { call("draft", R"({"names":["Mold"],"angle":10,"direction":"z"})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                double inset = 20.0 * std::tan(10.0 * kPi / 180.0);
                double top = 20.0 - 2.0 * inset;
                double expect = 20.0 / 3.0 * (400.0 + top * top + std::sqrt(400.0 * top * top));
                if (!near(volName("Mold"), expect)) return "vol " + numStr(volName("Mold")) + " want " + numStr(expect);
                return std::string();
            }});
        steps.push_back({"gear", 60, [&] { fresh(); call("gear", R"({"type":"spur","module":1,"teeth":12,"width":5,"bore":0,"name":"Gear"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!(volName("Gear") > 1)) return "vol " + numStr(volName("Gear")); return std::string(); }});
        steps.push_back({"rack", 60, [&] { fresh(); call("gear", R"({"type":"rack","module":1,"teeth":12,"width":5,"name":"Rack"})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                if (!(volName("Rack") > 1)) return "vol " + numStr(volName("Rack"));
                glm::vec3 sz = sizeOf("Rack");
                float len = std::max(sz.x, sz.y);
                if (std::abs(len - 12 * kPi) > 3.5) return "length " + numStr(len) + " (want about " + numStr(12 * kPi) + ")";
                return std::string();
            }});
        steps.push_back({"blades", 90, [&] { fresh(); call("blades", R"({"type":"propeller","diameter":40,"blade_count":2,"pitch":20,"hub_diameter":8,"chord":8,"bore":2,"name":"Prop"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!(volName("Prop") > 1)) return "vol " + numStr(volName("Prop")); return std::string(); }});
        steps.push_back({"wing", 90, [&] { fresh(); call("wing", R"({"root_chord":40,"span":60,"tip_chord":30,"airfoil":"NACA 2412","name":"Wing"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (!(volName("Wing") > 1)) return "vol " + numStr(volName("Wing")); return std::string(); }});
        steps.push_back({"dovetail", 40, [&] { fresh(); call("dovetail", R"({"type":"key","length":20,"width":8,"depth":5,"name":"Key"})"); },
            [&] { if (!fails().empty()) return fails() + last; if (nAlive() < 1) return "no body " + last; return std::string(); }});
        steps.push_back({"thread", 40, [&] { fresh(); call("create_cylinder", R"({"diameter":10,"height":20,"name":"Shaft"})"); },
            [&] { return std::string(); }});
        steps.push_back({"thread 2", 90, [&] { call("thread", R"({"body":"Shaft","cosmetic":true,"diameter":10,"pitch":1.5})"); },
            [&] {
                if (last.find("cancelled") != std::string::npos || last.find("not round") != std::string::npos) return last;
                for (auto& f : doc_.timeline)
                    if (f.type == FeatureType::Thread && f.failed) return f.error;
                if (!near(volName("Shaft"), kPi * 25 * 20)) return "vol " + numStr(volName("Shaft"));
                return std::string();
            }});
        steps.push_back({"thread modeled", 40, [&] { fresh(); call("create_cylinder", R"({"diameter":10,"height":20,"name":"Shaft"})"); },
            [&] { return std::string(); }});
        steps.push_back({"thread modeled 2", 120, [&] { call("thread", R"({"body":"Shaft","diameter":10,"pitch":1.5})"); },
            [&] {
                if (!fails().empty()) return fails() + last;
                if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
                double full = kPi * 25.0 * 20.0;
                double v = volName("Shaft");
                if (!(v < full - 40.0)) return "uncut " + numStr(v);
                if (!(v > full * 0.55)) return "gone " + numStr(v);
                int smooth = 0;
                for (auto& [id, b] : doc_.bodies) {
                    if (!doc_.isBodyAlive(b) || b.name != "Shaft") continue;
                    auto pit = doc_.parts.find(b.partId);
                    if (pit == doc_.parts.end() || !pit->second.shape) continue;
                    uint32_t n = kernel::faceCount(pit->second.shape);
                    for (uint32_t i = 0; i < n; ++i) {
                        double r = 0;
                        if (std::strcmp(kernel::faceKind(pit->second.shape, i, r), "other") == 0) ++smooth;
                    }
                }
                if (smooth < 4) return "faceted " + std::to_string(smooth);
                return std::string();
            }});
        steps.push_back({"step export", 40, [&] {
            fresh();
            call("run_command", R"({"command_id":"new_component"})");
            call("create_box", R"({"width":10,"depth":10,"height":10,"name":"Peg"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            return std::string();
        }});
        steps.push_back({"step export 2", 40, [&] {
            call("export_step", "{\"path\":" + json::Value(rtPath).dump() + "}");
        }, [&] {
            if (last.find("saved=") == std::string::npos) return last;
            return std::string();
        }});
        steps.push_back({"step export 3", 60, [&] {
            fresh();
            call("import_file", "{\"path\":" + json::Value(rtPath).dump() + "}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            std::string path;
            glm::vec3 c(0);
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Peg") continue;
                path = aiBodyPath(id);
                auto pit = doc_.parts.find(b.partId);
                if (pit != doc_.parts.end() && pit->second.mesh && pit->second.mesh->bounds.valid())
                    c = pit->second.mesh->bounds.transformed(b.xform).center();
            }
            const std::string tail = "Component1/Peg";
            if (path.size() < tail.size() || path.compare(path.size() - tail.size(), tail.size(), tail) != 0)
                return path.empty() ? std::string("no Peg ") + last : path;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + path;
            if (!near(volName("Peg"), 1000)) return "vol " + numStr(volName("Peg"));
            if (std::abs(c.x - 5.f) > 0.5f || std::abs(c.y - 5.f) > 0.5f || std::abs(c.z - 5.f) > 0.5f)
                return "center " + numStr(c.x) + "," + numStr(c.y) + "," + numStr(c.z);
            return std::string();
        }});
        steps.push_back({"ellipse", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"ellipse","center":[0,0],"major":10,"minor":5,"rotation":0}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":4,"name":"Disk"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double want = kPi * 10.0 * 5.0 * 4.0;
            double v = volName("Disk");
            if (std::abs(v - want) > 1.0) return "vol " + numStr(v) + " want " + numStr(want);
            auto s = sizeOf("Disk");
            if (std::abs(s.x - 20.f) > 0.5f || std::abs(s.y - 10.f) > 0.5f || std::abs(s.z - 4.f) > 0.5f)
                return "size " + numStr(s.x) + "," + numStr(s.y) + "," + numStr(s.z);
            return std::string();
        }});
        steps.push_back({"ellipse arc", 30, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"ellipse_arc","center":[0,0],"major":10,"minor":5,"start":0,"end":180}]})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            auto it = doc_.sketches.find((uint32_t)sidA);
            if (it == doc_.sketches.end()) return std::string("no sketch ") + last;
            for (auto& [id, e] : it->second.ents) {
                if (e.type != sk::EntType::EllipArc) continue;
                sk::Sketch::EllipseGeom g;
                if (!it->second.ellipseGeom(e, g)) return std::string("geom");
                if (std::abs(g.sweep - kPi) > 0.05) return "sweep " + numStr(g.sweep);
                auto mid = it->second.ellipsePoint(g, 0.5);
                if (std::abs(mid.x) > 0.05 || std::abs(mid.y - 5.0) > 0.05)
                    return "mid " + numStr(mid.x) + "," + numStr(mid.y);
                return std::string();
            }
            return std::string("no arc ") + last;
        }});
        steps.push_back({"control spline", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"control_spline","closed":true,"points":[[0,0],[20,0],[20,20],[0,20]]}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":10,"name":"Pull"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            std::vector<sk::Vec2> pts = {{0, 0}, {20, 0}, {20, 20}, {0, 20}};
            auto poly = sk::Sketch::sampleCtrlSpline(pts, true, 8);
            auto fit = sk::Sketch::sampleFitSpline(pts, true, 8);
            auto areaOf = [](const std::vector<sk::Vec2>& q) {
                double a = 0;
                for (size_t i = 0; i + 1 < q.size(); ++i) a += q[i].x * q[i + 1].y - q[i].y * q[i + 1].x;
                return std::abs(a) * 0.5;
            };
            double want = areaOf(poly) * 10.0;
            double fitV = areaOf(fit) * 10.0;
            double v = volName("Pull");
            if (near(v, fitV)) return "still a fit spline " + numStr(v);
            if (!near(v, want)) return "vol " + numStr(v) + " want " + numStr(want);
            double corner = 1e9;
            for (auto& p : poly) corner = std::min(corner, glm::length(p - sk::Vec2(20, 0)));
            if (corner < 2.0) return "passes a control point " + numStr(corner);
            return std::string();
        }});
        steps.push_back({"control spline open", 30, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"control_spline","points":[[0,0],[0,20],[20,20],[20,0]]}]})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            auto it = doc_.sketches.find((uint32_t)sidA);
            if (it == doc_.sketches.end()) return std::string("no sketch ") + last;
            for (auto& [id, e] : it->second.ents) {
                if (e.type != sk::EntType::CtrlSpline) continue;
                auto poly = it->second.ctrlSplinePoly(e, 8);
                if (poly.size() < 2) return std::string("short");
                if (glm::length(poly.front() - sk::Vec2(0, 0)) > 0.05) return "start " + numStr(poly.front().x) + "," + numStr(poly.front().y);
                if (glm::length(poly.back() - sk::Vec2(20, 0)) > 0.05) return "end " + numStr(poly.back().x) + "," + numStr(poly.back().y);
                double best = 1e9;
                for (auto& p : poly) best = std::min(best, glm::length(p - sk::Vec2(10, 15)));
                if (best > 0.05) return "mid " + numStr(best);
                double pull = 1e9;
                for (auto& p : poly) pull = std::min(pull, glm::length(p - sk::Vec2(0, 20)));
                if (pull < 1.0) return "passes a control point " + numStr(pull);
                return std::string();
            }
            return std::string("no spline ") + last;
        }});
        steps.push_back({"rect 3pt", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect_3pt","p1":[0,0],"p2":[6,8],"p3":[-3.2,2.4]}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":5,"name":"Slab"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Slab");
            if (std::abs(v - 200.0) > 1.0) return "vol " + numStr(v);
            auto it = doc_.sketches.find((uint32_t)sidA);
            if (it == doc_.sketches.end()) return std::string("no sketch");
            int n10 = 0, n4 = 0;
            bool angled = false;
            for (auto& [id, e] : it->second.ents) {
                if (e.type != sk::EntType::Line || e.construction) continue;
                sk::Vec2 d = it->second.pt(e.b) - it->second.pt(e.a);
                double L = glm::length(d);
                if (std::abs(L - 10.0) < 0.05) {
                    ++n10;
                    if (std::abs(std::abs(d.x) - 6.0) < 0.05 && std::abs(std::abs(d.y) - 8.0) < 0.05) angled = true;
                } else if (std::abs(L - 4.0) < 0.05) ++n4;
            }
            if (n10 != 2 || n4 != 2 || !angled) return "sides " + std::to_string(n10) + "," + std::to_string(n4);
            return std::string();
        }});
        steps.push_back({"polygon in", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"polygon_in","center":[0,0],"sides":4,"r":10}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":5,"name":"Tile"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Tile");
            if (std::abs(v - 1000.0) > 1.0) return "vol " + numStr(v);
            auto it = doc_.sketches.find((uint32_t)sidA);
            if (it == doc_.sketches.end()) return std::string("no sketch");
            int on = 0, circ = 0;
            bool corner = false;
            for (auto& [id, e] : it->second.ents) {
                if (e.type == sk::EntType::Circle && e.construction) {
                    if (std::abs(e.r - 10.0) < 0.05) ++circ;
                }
                if (e.type != sk::EntType::Point || e.locked) continue;
                sk::Vec2 p = it->second.pt(id);
                double d = glm::length(p);
                if (d < 0.05) continue;
                if (std::abs(d - 10.0) < 0.05) {
                    ++on;
                    if (std::abs(p.x - 10.0) < 0.05 && std::abs(p.y) < 0.05) corner = true;
                }
            }
            if (circ != 1 || on != 4 || !corner) return "circle " + std::to_string(circ) + " corners " + std::to_string(on);
            return std::string();
        }});
        steps.push_back({"slot overall", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"slot_overall","p1":[0,0],"p2":[30,0],"width":10}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":4,"name":"Slot"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double want = 800.0 + 100.0 * kPi;
            double v = volName("Slot");
            if (std::abs(v - want) > 1.0) return "vol " + numStr(v) + " want " + numStr(want);
            auto s = sizeOf("Slot");
            if (std::abs(s.x - 30.f) > 0.5f || std::abs(s.y - 10.f) > 0.5f || std::abs(s.z - 4.f) > 0.5f)
                return "size " + numStr(s.x) + "," + numStr(s.y) + "," + numStr(s.z);
            return std::string();
        }});
        steps.push_back({"slot center", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"slot_center","center":[0,0],"end":[10,0],"width":10}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":4,"name":"Slot"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double want = 800.0 + 100.0 * kPi;
            double v = volName("Slot");
            if (std::abs(v - want) > 1.0) return "vol " + numStr(v) + " want " + numStr(want);
            glm::vec3 sz(0), ctr(0);
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Slot") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh || !pit->second.mesh->bounds.valid()) continue;
                auto bb = pit->second.mesh->bounds.transformed(b.xform);
                sz = bb.max - bb.min;
                ctr = bb.center();
            }
            if (std::abs(sz.x - 30.f) > 0.5f || std::abs(sz.y - 10.f) > 0.5f || std::abs(sz.z - 4.f) > 0.5f)
                return "size " + numStr(sz.x) + "," + numStr(sz.y) + "," + numStr(sz.z);
            if (std::abs(ctr.x) > 0.5f || std::abs(ctr.y) > 0.5f)
                return "center " + numStr(ctr.x) + "," + numStr(ctr.y);
            return std::string();
        }});
        steps.push_back({"slot arc", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"slot_arc","p1":[20,0],"p2":[0,20],"p3":[-20,0],"width":4}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":2,"name":"Bend"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double want = 168.0 * kPi;
            double v = volName("Bend");
            if (std::abs(v - want) > 1.0) return "vol " + numStr(v) + " want " + numStr(want);
            glm::vec3 sz(0), ctr(0);
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Bend") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh || !pit->second.mesh->bounds.valid()) continue;
                auto bb = pit->second.mesh->bounds.transformed(b.xform);
                sz = bb.max - bb.min;
                ctr = bb.center();
            }
            if (std::abs(sz.x - 44.f) > 0.5f || std::abs(sz.y - 24.f) > 0.5f || std::abs(sz.z - 2.f) > 0.5f)
                return "size " + numStr(sz.x) + "," + numStr(sz.y) + "," + numStr(sz.z);
            if (std::abs(ctr.x) > 0.5f || std::abs(ctr.y - 10.f) > 0.5f)
                return "center " + numStr(ctr.x) + "," + numStr(ctr.y);
            return std::string();
        }});
        steps.push_back({"sketch chamfer", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":0,"y":0,"w":10,"h":10}]})");
            int corner = 0;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit != doc_.sketches.end()) {
                for (auto& [id, e] : sit->second.ents)
                    if (!e.locked && e.type == sk::EntType::Point && std::abs(e.x) < 1e-4 && std::abs(e.y) < 1e-4)
                        corner = (int)id;
            }
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + ",\"op\":\"chamfer\",\"corner\":" + std::to_string(corner) + ",\"distance\":2}");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":5,"name":"Plate"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Plate");
            if (std::abs(v - 490.0) > 1.0) return "vol " + numStr(v) + " want 490";
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit == doc_.sketches.end()) return std::string("sketch gone");
            bool bevel = false;
            for (auto& [id, e] : sit->second.ents) {
                if (e.locked || e.type != sk::EntType::Line) continue;
                sk::Vec2 a = sit->second.pt(e.a), b = sit->second.pt(e.b);
                auto near2 = [](sk::Vec2 p, double x, double y) {
                    return std::abs(p.x - x) < 0.05 && std::abs(p.y - y) < 0.05;
                };
                if ((near2(a, 2, 0) && near2(b, 0, 2)) || (near2(a, 0, 2) && near2(b, 2, 0))) bevel = true;
            }
            if (!bevel) return std::string("no bevel from (2,0) to (0,2)");
            return std::string();
        }});
        steps.push_back({"sketch break", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[20,0]},{"type":"line","p1":[10,-10],"p2":[10,10]}]})");
            int lineId = 0;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit != doc_.sketches.end()) {
                for (auto& [id, e] : sit->second.ents) {
                    if (e.locked || e.type != sk::EntType::Line) continue;
                    sk::Vec2 a = sit->second.pt(e.a), b = sit->second.pt(e.b);
                    if (std::abs(a.y) < 1e-4 && std::abs(b.y) < 1e-4 && std::abs(glm::length(b - a) - 20.0) < 1e-3)
                        lineId = (int)id;
                }
            }
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + ",\"op\":\"break\",\"entity\":" + std::to_string(lineId) + "}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit == doc_.sketches.end()) return std::string("sketch gone");
            auto near2 = [](sk::Vec2 p, double x, double y) {
                return std::abs(p.x - x) < 0.05 && std::abs(p.y - y) < 0.05;
            };
            auto isSeg = [&](sk::Vec2 a, sk::Vec2 b, double x0, double y0, double x1, double y1) {
                return (near2(a, x0, y0) && near2(b, x1, y1)) || (near2(a, x1, y1) && near2(b, x0, y0));
            };
            bool left = false, right = false, vert = false, whole = false;
            int lines = 0;
            for (auto& [id, e] : sit->second.ents) {
                if (e.locked || e.type != sk::EntType::Line) continue;
                ++lines;
                sk::Vec2 a = sit->second.pt(e.a), b = sit->second.pt(e.b);
                if (isSeg(a, b, 0, 0, 10, 0)) left = true;
                if (isSeg(a, b, 10, 0, 20, 0)) right = true;
                if (isSeg(a, b, 10, -10, 10, 10)) vert = true;
                if (isSeg(a, b, 0, 0, 20, 0)) whole = true;
            }
            if (whole || !left || !right || !vert || lines != 3)
                return "lines " + std::to_string(lines) + " left " + std::to_string(left) + " right " + std::to_string(right) + " vert " + std::to_string(vert) + " whole " + std::to_string(whole);
            return std::string();
        }});
        auto lineBetween = [&](uint32_t sid, double x0, double y0, double x1, double y1) {
            auto sit = doc_.sketches.find(sid);
            if (sit == doc_.sketches.end()) return false;
            auto near2 = [](sk::Vec2 p, double x, double y) { return std::abs(p.x - x) < 0.05 && std::abs(p.y - y) < 0.05; };
            for (auto& [id, e] : sit->second.ents) {
                if (e.locked || e.type != sk::EntType::Line) continue;
                sk::Vec2 a = sit->second.pt(e.a), b = sit->second.pt(e.b);
                if ((near2(a, x0, y0) && near2(b, x1, y1)) || (near2(a, x1, y1) && near2(b, x0, y0))) return true;
            }
            return false;
        };
        steps.push_back({"sketch move", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[10,0]}]})");
            int lineId = 0;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit != doc_.sketches.end())
                for (auto& [id, e] : sit->second.ents)
                    if (!e.locked && e.type == sk::EntType::Line) lineId = (int)id;
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + ",\"op\":\"move\",\"entities\":[" + std::to_string(lineId) + "],\"dx\":5,\"dy\":3}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (!lineBetween((uint32_t)sidA, 5, 3, 15, 3)) return std::string("line did not move to (5,3)-(15,3)");
            if (lineBetween((uint32_t)sidA, 0, 0, 10, 0)) return std::string("original line still there");
            return std::string();
        }});
        steps.push_back({"sketch rotate", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[10,0]}]})");
            int lineId = 0;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit != doc_.sketches.end())
                for (auto& [id, e] : sit->second.ents)
                    if (!e.locked && e.type == sk::EntType::Line) lineId = (int)id;
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + ",\"op\":\"rotate\",\"entities\":[" + std::to_string(lineId) + "],\"angle\":90,\"pivot\":[0,0]}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (!lineBetween((uint32_t)sidA, 0, 0, 0, 10)) return std::string("line did not turn to (0,0)-(0,10)");
            if (lineBetween((uint32_t)sidA, 0, 0, 10, 0)) return std::string("original line still there");
            return std::string();
        }});
        steps.push_back({"sketch scale", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"circle","center":[10,0],"r":5}]})");
            int circ = 0;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit != doc_.sketches.end())
                for (auto& [id, e] : sit->second.ents)
                    if (!e.locked && e.type == sk::EntType::Circle) circ = (int)id;
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + ",\"op\":\"scale\",\"entities\":[" + std::to_string(circ) + "],\"factor\":2,\"pivot\":[0,0]}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit == doc_.sketches.end()) return std::string("sketch gone");
            for (auto& [id, e] : sit->second.ents) {
                if (e.locked || e.type != sk::EntType::Circle) continue;
                sk::Vec2 c = sit->second.pt(e.c);
                if (std::abs(c.x - 20) < 0.05 && std::abs(c.y) < 0.05 && std::abs(e.r - 10) < 0.05) return std::string();
                return "circle " + numStr(c.x) + "," + numStr(c.y) + " r " + numStr(e.r);
            }
            return std::string("no circle");
        }});
        steps.push_back({"sketch copy", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[10,0]}]})");
            int lineId = 0;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit != doc_.sketches.end())
                for (auto& [id, e] : sit->second.ents)
                    if (!e.locked && e.type == sk::EntType::Line) lineId = (int)id;
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + ",\"op\":\"copy\",\"entities\":[" + std::to_string(lineId) + "],\"dx\":0,\"dy\":10}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (!lineBetween((uint32_t)sidA, 0, 0, 10, 0)) return std::string("original line is gone");
            if (!lineBetween((uint32_t)sidA, 0, 10, 10, 10)) return std::string("copy did not land at y=10");
            return std::string();
        }});
        auto nLines = [&](uint32_t sid) {
            int n = 0;
            auto sit = doc_.sketches.find(sid);
            if (sit == doc_.sketches.end()) return 0;
            for (auto& [id, e] : sit->second.ents)
                if (!e.locked && e.type == sk::EntType::Line) ++n;
            return n;
        };
        steps.push_back({"sketch pattern rect", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[10,0]}]})");
            int lineId = 0;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit != doc_.sketches.end())
                for (auto& [id, e] : sit->second.ents)
                    if (!e.locked && e.type == sk::EntType::Line) lineId = (int)id;
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + ",\"op\":\"pattern_rect\",\"entities\":[" + std::to_string(lineId) + "],\"count_x\":3,\"spacing_x\":20,\"count_y\":1,\"spacing_y\":0}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nLines((uint32_t)sidA) != 3) return "lines " + std::to_string(nLines((uint32_t)sidA));
            if (!lineBetween((uint32_t)sidA, 0, 0, 10, 0)) return std::string("original missing");
            if (!lineBetween((uint32_t)sidA, 20, 0, 30, 0)) return std::string("missing copy at x=20");
            if (!lineBetween((uint32_t)sidA, 40, 0, 50, 0)) return std::string("missing copy at x=40");
            return std::string();
        }});
        steps.push_back({"sketch pattern circ", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[10,0],"p2":[12,0]}]})");
            int lineId = 0;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit != doc_.sketches.end())
                for (auto& [id, e] : sit->second.ents)
                    if (!e.locked && e.type == sk::EntType::Line) lineId = (int)id;
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + ",\"op\":\"pattern_circ\",\"entities\":[" + std::to_string(lineId) + "],\"count\":4,\"angle\":360,\"pivot\":[0,0]}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nLines((uint32_t)sidA) != 4) return "lines " + std::to_string(nLines((uint32_t)sidA));
            if (!lineBetween((uint32_t)sidA, 10, 0, 12, 0)) return std::string("original missing");
            if (!lineBetween((uint32_t)sidA, 0, 10, 0, 12)) return std::string("missing 90 degree copy");
            if (!lineBetween((uint32_t)sidA, -10, 0, -12, 0)) return std::string("missing 180 degree copy");
            if (!lineBetween((uint32_t)sidA, 0, -10, 0, -12)) return std::string("missing 270 degree copy");
            return std::string();
        }});
        auto lineLen = [&](uint32_t sid) {
            auto sit = doc_.sketches.find(sid);
            if (sit == doc_.sketches.end()) return -1.0;
            for (auto& [id, e] : sit->second.ents) {
                if (e.locked || e.type != sk::EntType::Line) continue;
                return glm::length(sit->second.pt(e.b) - sit->second.pt(e.a));
            }
            return -1.0;
        };
        steps.push_back({"driven dim", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[10,0]}]})");
            int pa = 0, pb = 0;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit != doc_.sketches.end()) {
                for (auto& [id, e] : sit->second.ents) {
                    if (e.locked || e.type != sk::EntType::Point) continue;
                    if (std::abs(e.x) < 1e-4 && std::abs(e.y) < 1e-4) pa = (int)id;
                    if (std::abs(e.x - 10) < 1e-4 && std::abs(e.y) < 1e-4) pb = (int)id;
                }
            }
            call("sketch_constrain", "{\"sketch_id\":" + std::to_string(sidA) + ",\"constraints\":[{\"type\":\"distance\",\"refs\":[" + std::to_string(pa) + "," + std::to_string(pb) + "],\"value\":99,\"driven\":true}]}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            double len = lineLen((uint32_t)sidA);
            if (std::abs(len - 10) > 0.05) return "len " + numStr(len);
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit == doc_.sketches.end()) return std::string("sketch gone");
            bool saw = false;
            for (auto& [id, c] : sit->second.cons) {
                if (!c.driven || c.type != sk::CType::Distance) continue;
                saw = true;
                if (std::abs(c.value - 10) > 0.05) return "driven " + numStr(c.value);
            }
            if (!saw) return std::string("no driven dimension");
            sk::computeDof(sit->second);
            if (sit->second.dof != 4) return "dof " + std::to_string(sit->second.dof);
            return std::string();
        }});
        steps.push_back({"driven dim 2", 40, [&] {
            int pa = 0, pb = 0;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit != doc_.sketches.end()) {
                for (auto& [id, e] : sit->second.ents) {
                    if (e.locked || e.type != sk::EntType::Point) continue;
                    if (std::abs(e.x) < 1e-4 && std::abs(e.y) < 1e-4) pa = (int)id;
                    if (std::abs(e.x - 10) < 1e-4 && std::abs(e.y) < 1e-4) pb = (int)id;
                }
            }
            call("sketch_constrain", "{\"sketch_id\":" + std::to_string(sidA) + ",\"constraints\":[{\"type\":\"distance\",\"refs\":[" + std::to_string(pa) + "," + std::to_string(pb) + "],\"value\":25}]}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            double len = lineLen((uint32_t)sidA);
            if (std::abs(len - 25) > 0.05) return "len " + numStr(len);
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit == doc_.sketches.end()) return std::string("sketch gone");
            bool saw = false;
            for (auto& [id, c] : sit->second.cons) {
                if (!c.driven || c.type != sk::CType::Distance) continue;
                saw = true;
                if (std::abs(std::abs(c.value) - 25) > 0.05) return "driven " + numStr(c.value);
            }
            if (!saw) return std::string("driven dimension lost");
            sk::computeDof(sit->second);
            if (sit->second.dof != 3) return "dof " + std::to_string(sit->second.dof);
            return std::string();
        }});
        steps.push_back({"sketch intersect", 40, [&] {
            fresh();
            call("create_cylinder", R"({"diameter":20,"height":30,"name":"Post"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            return std::string();
        }});
        steps.push_back({"sketch intersect 2", 40, [&] {
            call("create_sketch", R"({"plane":"XY","offset":15})"); sidA = grab("sketch_id=");
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"op":"intersect","body":"Post"})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":4,"name":"Slice"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
            double want = 400.0 * kPi;
            double v = volName("Slice");
            if (std::abs(v - want) > 1.0) return "vol " + numStr(v) + " want " + numStr(want);
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit == doc_.sketches.end()) return std::string("sketch gone");
            int circles = 0;
            double r = 0, cx = 0, cy = 0;
            for (auto& [id, e] : sit->second.ents) {
                if (e.locked || e.type != sk::EntType::Circle) continue;
                ++circles;
                r = e.r;
                sk::Vec2 c = sit->second.pt(e.c);
                cx = c.x; cy = c.y;
            }
            if (circles != 1 || std::abs(r - 10) > 0.05 || std::abs(cx) > 0.05 || std::abs(cy) > 0.05)
                return "circles " + std::to_string(circles) + " r " + numStr(r) + " at " + numStr(cx) + "," + numStr(cy);
            return std::string();
        }});
        steps.push_back({"sketch onto face", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":20,"name":"Block"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            return std::string();
        }});
        steps.push_back({"sketch onto face 2", 40, [&] {
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[-10,10],"p2":[50,10]}]})");
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"op":"project_face","body":"Block","at":[20,20,20],"direction":[0,1,1]})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit == doc_.sketches.end()) return std::string("sketch gone");
            auto seg = [&](const sk::Entity& e, double ax, double ay, double bx, double by) {
                sk::Vec2 p = sit->second.pt(e.a), q = sit->second.pt(e.b);
                auto near = [](double a, double b) { return std::abs(a - b) <= 0.05; };
                return (near(p.x, ax) && near(p.y, ay) && near(q.x, bx) && near(q.y, by))
                    || (near(p.x, bx) && near(p.y, by) && near(q.x, ax) && near(q.y, ay));
            };
            bool orig = false;
            double minX = 1e9, maxX = -1e9;
            int lines = 0, drops = 0;
            std::string dump;
            for (auto& [id, e] : sit->second.ents) {
                if (e.locked || e.type != sk::EntType::Line) continue;
                ++lines;
                sk::Vec2 p = sit->second.pt(e.a), q = sit->second.pt(e.b);
                if (seg(e, -10, 10, 50, 10)) { orig = true; continue; }
                if (std::abs(p.y - 30) <= 0.05 && std::abs(q.y - 30) <= 0.05) {
                    ++drops;
                    minX = std::min(minX, std::min(p.x, q.x));
                    maxX = std::max(maxX, std::max(p.x, q.x));
                } else {
                    dump += " (" + numStr(p.x) + "," + numStr(p.y) + ")-(" + numStr(q.x) + "," + numStr(q.y) + ")";
                }
            }
            if (!orig || drops < 1 || !dump.empty() || std::abs(minX) > 0.05 || std::abs(maxX - 40) > 0.05)
                return "lines " + std::to_string(lines) + " drops " + std::to_string(drops)
                    + " x " + numStr(minX) + ".." + numStr(maxX) + (orig ? "" : " no original") + dump;
            return std::string();
        }});
        steps.push_back({"sketch crossing", 30, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[20,0]}]})");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[10,-10],"p2":[10,10]}]})");
            call("sketch_ops", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"op":"point_cross","at":[10,0]})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit == doc_.sketches.end()) return std::string("sketch gone");
            uint32_t pid = 0;
            for (auto& [id, e] : sit->second.ents) {
                if (e.locked || e.type != sk::EntType::Point) continue;
                if (std::abs(e.x - 10) <= 0.05 && std::abs(e.y) <= 0.05) pid = id;
            }
            if (!pid) return std::string("no point at the crossing ") + last;
            int on = 0;
            for (auto& [id, c] : sit->second.cons) {
                if (c.type != sk::CType::PointOnObject || c.e[0] != pid) continue;
                const sk::Entity* o = sit->second.find(c.e[1]);
                if (!o || o->locked || o->type != sk::EntType::Line) return std::string("point is not on both lines");
                ++on;
            }
            if (on != 2) return "on " + std::to_string(on);
            return std::string();
        }});
        steps.push_back({"sketch crossing 2", 30, [&] {
            auto sit = doc_.sketches.find((uint32_t)sidA);
            uint32_t hline = 0, vline = 0, cross = 0;
            if (sit != doc_.sketches.end()) {
                for (auto& [id, e] : sit->second.ents) {
                    if (e.locked || e.type != sk::EntType::Line) continue;
                    sk::Vec2 a = sit->second.pt(e.a), b = sit->second.pt(e.b);
                    if (std::abs(a.y - b.y) <= 0.05) hline = id;
                    if (std::abs(a.x - b.x) <= 0.05) vline = id;
                }
                for (auto& [id, e] : sit->second.ents) {
                    if (e.locked || e.type != sk::EntType::Point) continue;
                    if (std::abs(e.x - 10) <= 0.05 && std::abs(e.y) <= 0.05) cross = id;
                }
            }
            call("sketch_constrain", "{\"sketch_id\":" + std::to_string(sidA)
                + ",\"constraints\":[{\"type\":\"horizontal\",\"refs\":[" + std::to_string(hline)
                + "]},{\"type\":\"vertical\",\"refs\":[" + std::to_string(vline)
                + "]},{\"type\":\"hdistance\",\"refs\":[1," + std::to_string(cross) + "],\"value\":15}]}");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            auto sit = doc_.sketches.find((uint32_t)sidA);
            if (sit == doc_.sketches.end()) return std::string("sketch gone");
            uint32_t pid = 0;
            int on = 0;
            for (auto& [id, c] : sit->second.cons)
                if (c.type == sk::CType::PointOnObject) {
                    const sk::Entity* p = sit->second.find(c.e[0]);
                    if (p && p->type == sk::EntType::Point && !p->locked) { pid = c.e[0]; ++on; }
                }
            if (on != 2 || !pid) return "on " + std::to_string(on) + " " + last;
            const sk::Entity* p = sit->second.find(pid);
            if (std::abs(p->x - 15) > 0.05 || std::abs(p->y) > 0.05)
                return "at " + numStr(p->x) + "," + numStr(p->y) + " " + last;
            bool vert = false, horiz = false;
            for (auto& [id, e] : sit->second.ents) {
                if (e.locked || e.type != sk::EntType::Line) continue;
                sk::Vec2 a = sit->second.pt(e.a), b = sit->second.pt(e.b);
                if (std::abs(a.x - 15) <= 0.05 && std::abs(b.x - 15) <= 0.05) vert = true;
                if (std::abs(a.y) <= 0.05 && std::abs(b.y) <= 0.05
                    && std::abs(std::min(a.x, b.x)) <= 0.05 && std::abs(std::max(a.x, b.x) - 20) <= 0.05)
                    horiz = true;
            }
            if (!vert || !horiz) return std::string(vert ? "horizontal line moved" : "vertical line stayed");
            return std::string();
        }});
        steps.push_back({"thin extrude", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[20,0]}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":10,"thickness":2,"name":"Wall"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Wall");
            if (std::abs(v - 400) > 1) return "vol " + numStr(v) + " " + last;
            glm::vec3 sz = sizeOf("Wall");
            if (std::abs(sz.x - 20) > 0.5 || std::abs(sz.y - 2) > 0.5 || std::abs(sz.z - 10) > 0.5)
                return "size " + numStr(sz.x) + "," + numStr(sz.y) + "," + numStr(sz.z);
            glm::vec3 c(0);
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Wall") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit != doc_.parts.end() && pit->second.mesh) c = pit->second.mesh->bounds.transformed(b.xform).center();
            }
            if (std::abs(c.y) > 0.5) return "center y " + numStr(c.y);
            return std::string();
        }});
        steps.push_back({"thin extrude side", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[20,0]}]})");
            call("extrude", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"distance":10,"thickness":2,"side":"one","name":"Wall"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            double v = volName("Wall");
            if (std::abs(v - 400) > 1) return "vol " + numStr(v) + " " + last;
            glm::vec3 c(0);
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Wall") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit != doc_.parts.end() && pit->second.mesh) c = pit->second.mesh->bounds.transformed(b.xform).center();
            }
            if (std::abs(c.y - 1) > 0.5) return "center y " + numStr(c.y);
            return std::string();
        }});
        steps.push_back({"thin revolve", 50, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[10,0],"p2":[10,20]}]})");
            call("revolve", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"angle":360,"axis":"y","thickness":2,"name":"Tube"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Tube");
            if (std::abs(v - 800 * kPi) > 1) return "vol " + numStr(v) + " " + last;
            return std::string();
        }});
        steps.push_back({"thin sweep", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"line","p1":[-2,0],"p2":[2,0]}]})");
            call("create_sketch", R"({"plane":"XZ"})"); sidB = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidB) + R"(,"entities":[{"type":"line","p1":[0,0],"p2":[0,30]}]})");
            call("sweep", "{\"profile_sketch\":" + std::to_string(sidA) + ",\"path_sketch\":" + std::to_string(sidB) + R"(,"thickness":2,"name":"SweepWall"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("SweepWall");
            if (std::abs(v - 240) > 1) return "vol " + numStr(v) + " " + last;
            glm::vec3 sz = sizeOf("SweepWall");
            if (std::abs(sz.x - 4) > 0.5 || std::abs(sz.y - 2) > 0.5 || std::abs(sz.z - 30) > 0.5)
                return "size " + numStr(sz.x) + "," + numStr(sz.y) + "," + numStr(sz.z);
            return std::string();
        }});
        steps.push_back({"loft point", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":0,"y":0,"w":10,"h":10}]})");
            call("loft", "{\"sketch_ids\":[" + std::to_string(sidA) + R"(],"point":[5,5,20],"name":"Pyramid"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Pyramid");
            if (std::abs(v - 2000.0 / 3.0) > 1) return "vol " + numStr(v) + " " + last;
            glm::vec3 sz = sizeOf("Pyramid");
            if (std::abs(sz.x - 10) > 0.5 || std::abs(sz.y - 10) > 0.5 || std::abs(sz.z - 20) > 0.5)
                return "size " + numStr(sz.x) + "," + numStr(sz.y) + "," + numStr(sz.z);
            return std::string();
        }});
        steps.push_back({"loft point start", 40, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"rect","x":0,"y":0,"w":10,"h":10}]})");
            call("loft", "{\"sketch_ids\":[" + std::to_string(sidA) + R"(],"point":[5,5,-20],"point_end":"start","name":"Pyramid"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Pyramid");
            if (std::abs(v - 2000.0 / 3.0) > 1) return "vol " + numStr(v) + " " + last;
            float z0 = 1e9f, z1 = -1e9f;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Pyramid") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
                z0 = bb.min.z; z1 = bb.max.z;
            }
            if (std::abs(z0 - (-20.f)) > 0.5f || std::abs(z1) > 0.5f)
                return "z " + numStr(z0) + " " + numStr(z1);
            return std::string();
        }});
        steps.push_back({"loft centerline", 50, [&] {
            fresh();
            call("create_sketch", R"({"plane":"XY"})"); sidA = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidA) + R"(,"entities":[{"type":"circle","x":0,"y":0,"r":10}]})");
            call("create_sketch", R"({"plane":"XY","offset":40})"); sidB = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidB) + R"(,"entities":[{"type":"circle","x":0,"y":0,"r":10}]})");
            call("create_sketch", R"({"plane":"XZ"})"); sidC = grab("sketch_id=");
            call("sketch_add", "{\"sketch_id\":" + std::to_string(sidC) + R"(,"entities":[{"type":"arc","p1":[0,0],"p2":[15,20],"p3":[0,40]}]})");
            call("loft", "{\"sketch_ids\":[" + std::to_string(sidA) + "," + std::to_string(sidB) + "],\"centerline\":" + std::to_string(sidC) + R"(,"name":"Bow"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Bow");
            if (!near(v, 4000.0 * kPi)) return "vol " + numStr(v) + " " + last;
            bool mid = false, axis = false;
            float maxX = 0;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Bow") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                mid = kernel::pointInside(pit->second.shape, b.xform, {15, 0, 20});
                axis = kernel::pointInside(pit->second.shape, b.xform, {0, 0, 20});
                if (pit->second.mesh) maxX = pit->second.mesh->bounds.transformed(b.xform).max.x;
            }
            if (!mid) return std::string("middle is empty");
            if (axis) return std::string("still on the axis");
            if (maxX < 20.f || maxX > 30.f) return "max x " + numStr(maxX);
            return std::string();
        }});
        steps.push_back({"thicken", 40, [&] { fresh(); call("create_box", R"({"width":20,"depth":20,"height":10,"name":"Slab"})"); },
            [&] { return std::string(); }});
        steps.push_back({"thicken 2", 40, [&] {
            int face = -1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Slab") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::faceCount(pit->second.shape);
                for (uint32_t i = 0; i < n; ++i) {
                    glm::dvec3 c, normal; bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, i, c, normal, planar) || !planar) continue;
                    if (normal.z > 0.9) face = (int)i;
                }
            }
            call("thicken", "{\"names\":[\"Slab\"],\"thickness\":4,\"side\":\"one\",\"faces\":[" + std::to_string(face) + R"(],"name":"Lid"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (std::abs(volName("Lid") - 1600.0) > 1) return "lid " + numStr(volName("Lid")) + " " + last;
            if (std::abs(volName("Slab") - 4000.0) > 1) return "slab " + numStr(volName("Slab"));
            bool up = false, down = false;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Lid") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                up = kernel::pointInside(pit->second.shape, b.xform, {10, 10, 12});
                down = kernel::pointInside(pit->second.shape, b.xform, {10, 10, 8});
            }
            if (!up) return std::string("above the face is empty");
            if (down) return std::string("grew into the body");
            return std::string();
        }});
        steps.push_back({"thicken both", 40, [&] { fresh(); call("create_box", R"({"width":20,"depth":20,"height":10,"name":"Slab"})"); },
            [&] { return std::string(); }});
        steps.push_back({"thicken both 2", 40, [&] {
            int face = -1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Slab") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::faceCount(pit->second.shape);
                for (uint32_t i = 0; i < n; ++i) {
                    glm::dvec3 c, normal; bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, i, c, normal, planar) || !planar) continue;
                    if (normal.z > 0.9) face = (int)i;
                }
            }
            call("thicken", "{\"names\":[\"Slab\"],\"thickness\":4,\"side\":\"both\",\"faces\":[" + std::to_string(face) + R"(],"name":"Band"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
            if (std::abs(volName("Band") - 1600.0) > 1) return "band " + numStr(volName("Band")) + " " + last;
            if (std::abs(volName("Slab") - 4000.0) > 1) return "slab " + numStr(volName("Slab"));
            bool hi = false, lo = false, outHi = false, outLo = false;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Band") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                hi = kernel::pointInside(pit->second.shape, b.xform, {10, 10, 11});
                lo = kernel::pointInside(pit->second.shape, b.xform, {10, 10, 9});
                outHi = kernel::pointInside(pit->second.shape, b.xform, {10, 10, 13});
                outLo = kernel::pointInside(pit->second.shape, b.xform, {10, 10, 7});
            }
            if (!hi || !lo) return std::string("not centered on the face");
            if (outHi || outLo) return std::string("thicker than asked");
            return std::string();
        }});
        steps.push_back({"thicken cyl", 40, [&] { fresh(); call("create_cylinder", R"({"diameter":20,"height":20,"name":"Shaft"})"); },
            [&] { return std::string(); }});
        steps.push_back({"thicken cyl 2", 50, [&] {
            int face = -1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Shaft") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::faceCount(pit->second.shape);
                for (uint32_t i = 0; i < n; ++i) {
                    glm::dvec3 c, normal; bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, i, c, normal, planar)) continue;
                    if (!planar) face = (int)i;
                }
            }
            call("thicken", "{\"names\":[\"Shaft\"],\"thickness\":2,\"side\":\"one\",\"faces\":[" + std::to_string(face) + R"(],"name":"Sleeve"})");
        }, [&] {
            if (!fails().empty()) return fails() + last;
            if (nAlive() != 2) return "bodies " + std::to_string(nAlive()) + " " + last;
            double v = volName("Sleeve");
            if (std::abs(v - 880.0 * kPi) > 1) return "sleeve " + numStr(v) + " " + last;
            if (!near(volName("Shaft"), 2000.0 * kPi)) return "shaft " + numStr(volName("Shaft"));
            bool outR = false, inR = false;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || b.name != "Sleeve") continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                outR = kernel::pointInside(pit->second.shape, b.xform, {11, 0, 10});
                inR = kernel::pointInside(pit->second.shape, b.xform, {9, 0, 10});
            }
            if (!outR) return std::string("outside the shaft is empty");
            if (inR) return std::string("grew into the shaft");
            return std::string();
        }});
    }

    {
        // Save and reopen: every feature must rebuild from the file alone (bodies of the same pass,
        // auto targets, renamed bodies).
        static const std::string kst = (std::filesystem::temp_directory_path() / "kestral-reopen-test.kst").string();
        static const double want = 32000.0 - kPi * 25.0 * 20.0;
        steps.push_back({"reopen", 40, [&] {
            fresh();
            call("create_box", R"({"width":40,"depth":40,"height":20,"name":"Block"})");
        }, [&] { return std::string(); }});
        steps.push_back({"reopen rename", 20, [&] { call("rename", R"({"name":"Block","new_name":"Plate"})"); },
            [&] { return volName("Plate") > 0 ? std::string() : "rename: " + last; }});
        steps.push_back({"reopen hole", 40, [&] { call("create_hole", R"({"positions":[[20,20,20]],"diameter":10})"); }, [&] {
            if (!fails().empty()) return fails() + last;
            if (!near(volName("Plate"), want)) return "vol " + numStr(volName("Plate")) + " " + last;
            return std::string();
        }});
        steps.push_back({"reopen 2", 60, [&] {
            if (!saveDocument(doc_, kst)) { last = "save failed"; return; }
            dirty_ = false;
            openDocumentPath(kst);
        }, [&] {
            if (last == "save failed") return last;
            if (!fails().empty()) return "after reopen: " + fails();
            if (nAlive() != 1) return "bodies " + std::to_string(nAlive());
            if (!near(volName("Plate"), want)) return "vol " + numStr(volName("Plate")) + " (name kept?)";
            return std::string();
        }});
    }

    auto finish = [&] {
        auto outPath = std::filesystem::temp_directory_path() / "kestral-model-test.txt";
        std::ofstream f(outPath, std::ios::binary);
        f << log;
        log::info(std::string(modelTestFail_ ? "model test FAILED" : "model test passed") + " (" + outPath.string() + ")");
        dirty_ = false;
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    };
    if (step == 0 && phase == 0) {
        // KESTRAL_TEST_FROM=<step name> starts the suite at that step (quick re-checks).
        if (const char* from = std::getenv("KESTRAL_TEST_FROM"))
            for (size_t i = 0; i < steps.size(); ++i)
                if (from == std::string(steps[i].name)) { step = (int)i; break; }
    }
    if (step >= (int)steps.size()) { finish(); return; }
    auto busy = [&] { return recomputeRunning_ || recomputeQueuedFrom_ >= 0 || (jobs_ && jobs_->active() > 0); };
    if (phase == 0) {
        steps[step].run();
        phase = 1;
        t0 = time_;
        return;
    }
    if (busy()) {
        if (time_ - t0 > steps[step].timeout) {
            log += std::string("FAIL ") + steps[step].name + " timeout\n";
            modelTestFail_ = true;
            log::info(std::string("FAIL ") + steps[step].name + " timeout");
            ++step; phase = 0;
        }
        return;
    }
    std::string why = steps[step].check();
    if (why.empty()) {
        log += std::string("PASS ") + steps[step].name + "\n";
        log::info(std::string("PASS ") + steps[step].name);
    } else {
        log += std::string("FAIL ") + steps[step].name + " " + why + "\n";
        modelTestFail_ = true;
        log::info(std::string("FAIL ") + steps[step].name + " " + why);
    }
    ++step;
    phase = 0;
    if (step >= (int)steps.size()) finish();
}

// UI smoke test: Kestral.exe --ui-test runs every toolbar command the way a click does (model tools on a
// design with a block and a sketch, sketch tools inside a sketch), lets it draw a few frames, then
// cancels. A command fails when it throws, shows an error, or leaves a step behind after Cancel.
// Picking and typing values still need a person; this catches commands that are broken on open.
void App::uiTestStep() {
    static const std::set<std::string> skip = {
        "insert_mesh", "insert_step", "send_to_slicer", "export_stl", "slicer_setup",   // file dialogs, other apps
        "new", "open", "save", "save_as", "import", "export", "exit", "quit", "close"};
    static std::vector<int> order;
    static size_t idx = 0;
    static int phase = 0, wait = 0, fails = 0, ran = 0;
    static size_t steps0 = 0;
    static double t0 = 0;
    static std::string log;
    static uint32_t sketchId = 0;
    static bool inSketchPart = false;
    activity();
    auto finish = [&] {
        auto outPath = std::filesystem::temp_directory_path() / "kestral-ui-test.txt";
        std::ofstream f(outPath, std::ios::binary);
        f << log;
        log::info("ui test " + std::string(fails ? "FAILED" : "passed") + ": " + std::to_string(ran) + " commands, " +
                  std::to_string(fails) + " failed (" + outPath.string() + ")");
        if (fails) modelTestFail_ = true;
        dirty_ = false;
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    };
    auto closeAll = [&] {
        for (int i = 0; i < 4 && (dlg_.open || tdlg_.open || sk_.choosingPlane); ++i) {
            commandCancel();
            if (sk_.choosingPlane) sketchCancel();
        }
    };
    auto setup = [&](bool sketch) {
        closeAll();
        if (sk_.active) leaveSketchMode();
        newDocument();
        clearSelection();
        aiRunTool("create_box", R"({"x":0,"y":0,"z":0,"width":40,"depth":40,"height":20,"name":"Block"})");
        std::string r = aiRunTool("create_sketch", R"({"plane":"XY","offset":30})");
        auto p = r.find("sketch_id=");
        sketchId = p == std::string::npos ? 0 : (uint32_t)std::atoi(r.c_str() + p + 10);
        aiRunTool("sketch_add", "{\"sketch_id\":" + std::to_string(sketchId) +
                                    R"(,"entities":[{"type":"circle","x":20,"y":20,"r":5},{"type":"line","p1":[0,0],"p2":[30,10]}]})");
        if (sk_.active) leaveSketchMode();
        if (sketch && sketchId) editSketch(sketchId);
    };
    if (order.empty() && idx == 0 && phase == 0) {
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 0; i < tools_.size(); ++i) {
                const Tool& t = tools_[i];
                bool isSk = i < toolIsSketch_.size() && toolIsSketch_[i];
                if (!t.available() || t.global || skip.count(t.id) || isSk != (pass == 1)) continue;
                order.push_back((int)i);
            }
        setup(false);
        phase = 1;
        return;
    }
    if (idx >= order.size()) { finish(); return; }
    const Tool& t = tools_[order[idx]];
    bool isSk = (size_t)order[idx] < toolIsSketch_.size() && toolIsSketch_[order[idx]];
    if (phase == 1) {
        if (isSk && !inSketchPart) { inSketchPart = true; setup(true); return; }
        if (isSk && !sk_.active && sketchId) editSketch(sketchId);
        steps0 = doc_.timeline.size();
        t0 = time_;
        ++ran;
        std::string ferr;
        if (!platform::guarded("ui test", [&] { runTool(t.id); }, ferr)) {
            log += "FAIL " + t.id + " threw: " + ferr + "\n";
            ++fails;
        }
        phase = 2;
        wait = 4;
        return;
    }
    if (phase == 2) {
        if (--wait > 0) return;
        bool busy = recomputeRunning_ || recomputeQueuedFrom_ >= 0;
        if (busy && time_ - t0 < 20) return;
        std::string bad;
        for (auto& ts : toasts_)
            if (ts.t0 >= t0 && (ts.kind == ToastKind::Error || ts.msg.find("internal error") != std::string::npos))
                bad += (bad.empty() ? "" : " | ") + ts.msg;
        bool opened = dlg_.open || tdlg_.open || sk_.choosingPlane || sk_.active;
        closeAll();
        if (!isSk && sk_.active) leaveSketchMode();
        phase = 3;
        wait = 3;
        if (!bad.empty()) { log += "FAIL " + t.id + " (" + t.label + "): " + bad + "\n"; ++fails; return; }
        log += std::string(opened ? "ok   " : "ok*  ") + t.id + " (" + t.label + ")\n";   // * = no dialog (acts at once)
        return;
    }
    if (phase == 3) {
        if (--wait > 0) return;
        bool busy = recomputeRunning_ || recomputeQueuedFrom_ >= 0;
        if (busy && time_ - t0 < 30) return;
        // A cancelled command must not leave a step behind (commands that act at once may add one).
        bool actsAtOnce = log.size() >= 4 && log.rfind("ok*  " + t.id) != std::string::npos;
        if (!actsAtOnce && doc_.timeline.size() > steps0 && !isSk) {
            log += "FAIL " + t.id + ": Cancel left " + std::to_string(doc_.timeline.size() - steps0) + " step(s)\n";
            ++fails;
        }
        for (auto& f : doc_.timeline)
            if (f.failed && !f.error.empty()) {
                log += "FAIL " + t.id + ": timeline step " + f.name + " failed: " + f.error + "\n";
                ++fails;
                setup(isSk);
                break;
            }
        if (actsAtOnce || doc_.timeline.size() > steps0) setup(isSk);   // start the next one clean
        ++idx;
        phase = 1;
    }
}

} // namespace kst
