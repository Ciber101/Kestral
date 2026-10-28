// Assistant tools that build real features: sketches, profiles, booleans, patterns and the generators.
#include "app/App.h"
#include "app/MechTools.h"
#include "core/Json.h"
#include "core/Log.h"
#include "kernel/Features.h"
#include "sketch/Profiles.h"
#include "sketch/Text.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>

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

bool putExpr(char* buf, size_t n, const json::Value& v, const char* fallback) {
    std::string e;
    if (v.isString()) e = v.asString();
    else if (v.isNumber()) e = numStr(v.asNumber());
    else e = fallback ? fallback : "0";
    if (e.size() >= n) return false;
    std::snprintf(buf, n, "%s", e.c_str());
    return true;
}

bool read2(const json::Value& v, sk::Vec2& o) {
    if (v.size() < 2) return false;
    o = {v[(size_t)0].asNumber(), v[(size_t)1].asNumber()};
    return true;
}

bool read3(const json::Value& v, glm::dvec3& o) {
    if (v.size() < 3) return false;
    o = {v[(size_t)0].asNumber(), v[(size_t)1].asNumber(), v[(size_t)2].asNumber()};
    return true;
}

int opOf(const std::string& s) {
    std::string t = low(s);
    if (t == "join") return 1;
    if (t == "cut") return 2;
    if (t == "intersect") return 3;
    return 0;
}

int planeOf(const std::string& s) {
    std::string t = low(s);
    if (t == "xz") return 1;
    if (t == "yz") return 2;
    return 0;
}

sk::Plane originPlane(const std::string& name) {
    sk::Plane pl;
    std::string t = low(name);
    if (t == "xz") {
        pl.x = {1, 0, 0};
        pl.y = {0, 0, 1};
        pl.name = "XZ";
    } else if (t == "yz") {
        pl.x = {0, 1, 0};
        pl.y = {0, 0, 1};
        pl.name = "YZ";
    } else {
        pl.x = {1, 0, 0};
        pl.y = {0, 1, 0};
        pl.name = "XY";
    }
    return pl;
}

// Right handed axes with the sketch normal equal to `axis`, and +Z / +X preferring world +X and +Y.
void axisFrame(glm::dvec3 axis, glm::dvec3& x, glm::dvec3& y) {
    if (glm::length(axis) < 1e-12) axis = {0, 0, 1};
    axis = glm::normalize(axis);
    if (axis.z > 0.9) { x = {1, 0, 0}; y = {0, 1, 0}; return; }
    if (axis.z < -0.9) { x = {1, 0, 0}; y = {0, -1, 0}; return; }
    if (axis.y > 0.9) { x = {1, 0, 0}; y = {0, 0, -1}; return; }
    if (axis.y < -0.9) { x = {1, 0, 0}; y = {0, 0, 1}; return; }
    if (axis.x > 0.9) { x = {0, 1, 0}; y = {0, 0, 1}; return; }
    if (axis.x < -0.9) { x = {0, -1, 0}; y = {0, 0, 1}; return; }
    glm::dvec3 h = std::abs(axis.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0);
    x = glm::normalize(glm::cross(h, axis));
    y = glm::cross(axis, x);
}

// Sketch Y lies along `axis`, so a revolve about the sketch Y axis spins around it.
// x = cross(axis, up) is the direction a semicircle revolve succeeds with. The other
// perpendicular builds a face whose arc comes out the long way and the solid is short.
sk::Plane planeYAlong(glm::dvec3 axis, glm::dvec3 origin) {
    if (glm::length(axis) < 1e-12) axis = {0, 0, 1};
    axis = glm::normalize(axis);
    glm::dvec3 up = std::abs(axis.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0);
    sk::Plane pl;
    pl.x = glm::normalize(glm::cross(axis, up));
    pl.y = axis;
    pl.o = origin;
    pl.name = "Axis";
    return pl;
}

// Sketch plane whose normal is `axis`, so an extrude grows along it from the given corner.
sk::Plane planeNormal(glm::dvec3 axis, glm::dvec3 origin) {
    sk::Plane pl;
    axisFrame(axis, pl.x, pl.y);
    pl.o = origin;
    pl.name = "Axis";
    return pl;
}

glm::dvec3 axisVec(const json::Value& v, glm::dvec3 def) {
    if (v.isString()) {
        std::string s = low(v.asString());
        if (s == "x") return {1, 0, 0};
        if (s == "y") return {0, 1, 0};
        if (s == "z") return {0, 0, 1};
    }
    glm::dvec3 o;
    if (read3(v, o) && glm::length(o) > 1e-12) return glm::normalize(o);
    return def;
}

sk::CType constraintType(const std::string& s, bool& ok) {
    ok = true;
    std::string t = low(s);
    if (t == "coincident") return sk::CType::Coincident;
    if (t == "point_on" || t == "pointonobject") return sk::CType::PointOnObject;
    if (t == "horizontal") return sk::CType::Horizontal;
    if (t == "vertical") return sk::CType::Vertical;
    if (t == "parallel") return sk::CType::Parallel;
    if (t == "perpendicular") return sk::CType::Perpendicular;
    if (t == "equal") return sk::CType::Equal;
    if (t == "tangent") return sk::CType::Tangent;
    if (t == "midpoint") return sk::CType::Midpoint;
    if (t == "symmetric") return sk::CType::Symmetric;
    if (t == "fix") return sk::CType::Fix;
    if (t == "collinear") return sk::CType::Collinear;
    if (t == "distance") return sk::CType::Distance;
    if (t == "hdistance") return sk::CType::HDistance;
    if (t == "vdistance") return sk::CType::VDistance;
    if (t == "radius") return sk::CType::Radius;
    if (t == "diameter") return sk::CType::Diameter;
    if (t == "angle") return sk::CType::Angle;
    ok = false;
    return sk::CType::Coincident;
}

} // namespace

uint32_t App::aiEnsureFolder(const std::string& name) {
    if (name.empty()) return 0;
    std::string n = low(name);
    for (auto& [id, c] : doc_.components)
        if (id != doc_.rootId && low(c.name) == n) return id;
    pushUndo("Folder");
    Component c;
    c.id = doc_.newId();
    c.name = name;
    c.parentId = doc_.rootId;
    doc_.components[c.id] = c;
    doc_.touch();
    return c.id;
}

void App::aiNameOutput(uint32_t featureId, const std::string& name, uint32_t folder) {
    if (!featureId) return;
    if (!name.empty()) {
        BodyOverride o;
        o.name = name;
        o.visible = true;
        doc_.setOverride(featureId, 0, o);
    }
    if (folder) doc_.featureFolder[featureId] = folder;
}

bool App::aiModelTool(const std::string& name, const std::string& argsJson, std::string& out) {
    using json::Value;
    static const char* kNames[] = {
        "get_body", "create_component", "activate_component", "ground_component", "create_sketch", "sketch_add", "sketch_constrain", "sketch_ops", "get_sketch",
        "extrude", "revolve", "sweep", "loft", "rotate", "scale", "set_transform", "align", "combine",
        "circular_pattern", "rect_pattern", "pattern_on_path", "mirror", "shell", "thicken", "split_body", "gear", "blades", "thread", "wing",
        "dovetail", "interference", "measure", "offset_plane", "create_cylinder", "create_cone", "create_sphere",
        "create_torus", "create_wedge", "create_tube", "create_coil", "pipe", "draft", "press_pull", "offset_face",
        "plane_at_angle", "midplane", "plane_3pt", "plane_tangent", "replace_face", "axis", "point", "split_face", "rib", "web", "emboss", "boss", "snap", "rest", "lip", "silhouette", "joint", "asbuilt_joint", "rigid_group"};
    bool mine = false;
    for (const char* n : kNames)
        if (name == n) mine = true;
    if (!mine) return false;

    Value a = Value::parse(argsJson);
    std::string evalErr;
    auto num = [&](const Value& v, double def) {
        try {
            if (v.isString()) return peval::eval(v.asString(), doc_.params);
            if (v.isNumber()) return v.asNumber();
        } catch (const std::runtime_error& ex) { evalErr = ex.what(); }
        return def;
    };
    auto idsFrom = [&](const Value& v, std::string& err) {
        std::vector<std::string> names;
        if (v.isString()) names.push_back(v.asString());
        for (auto& e : v.items())
            if (e.isString()) names.push_back(e.asString());
        std::string missing;
        auto ids = aiFindBodies(names, missing);
        if (!missing.empty()) err = "Not found: " + missing + ". Use paths from get_design.";
        return ids;
    };
    auto refsFrom = [&](const Value& v, std::string& err) {
        std::vector<TargetRef> refs;
        for (uint32_t id : idsFrom(v, err)) {
            TargetRef r = refOfBody(id);
            if (r.first) refs.push_back(r);
        }
        return refs;
    };
    auto selectIds = [&](const std::vector<uint32_t>& ids) {
        clearSelection();
        selectBodies(ids, false);
    };
    auto folder = aiEnsureFolder(a["component"].asString());
    auto stamp = [&](uint32_t fid) { aiNameOutput(fid, a["name"].asString(), folder); };
    auto closedFeature = [&](const char* what) -> std::string {
        if (!dlg_.open) return std::string(what) + " did not open.";
        uint32_t id = dlg_.featureId;
        stamp(id);
        closeFeatureDialog(true);
        if (doc_.featureIndex(id) < 0) return std::string(what) + " was cancelled (nothing to build).";
        return {};
    };
    auto closedTool = [&](const char* what) -> std::string {
        if (!tdlg_.open) return std::string(what) + " did not open.";
        uint32_t id = tdlg_.featureId;
        stamp(id);
        closeToolDialog(true);
        if (doc_.featureIndex(id) < 0) return std::string(what) + " was cancelled.";
        return {};
    };
    auto useSketch = [&](uint32_t sid, std::string& err) -> sk::Sketch* {
        if (!doc_.sketches.count(sid)) { err = "No sketch with that id."; return nullptr; }
        if (!sk_.active || sk_.sketchId != sid) editSketch(sid);
        sk::Sketch* s = curSketch();
        if (!s) err = "Could not open the sketch.";
        return s;
    };
    auto pickProfiles = [&](uint32_t sid, int which) {
        selProfiles_.clear();
        auto it = doc_.sketches.find(sid);
        if (it == doc_.sketches.end()) return;
        const auto& profs = sketchProfiles(it->second);
        if (which >= 0) {
            if ((size_t)which < profs.size()) selProfiles_.insert({sid, (uint32_t)which});
        } else {
            for (size_t i = 0; i < profs.size(); ++i) selProfiles_.insert({sid, (uint32_t)i});
        }
    };
    auto setTargetsOnOpen = [&](const std::vector<TargetRef>& refs, bool revolve) {
        int fi = doc_.featureIndex(dlg_.featureId);
        if (fi < 0 || refs.empty()) return;
        Feature& f = doc_.timeline[fi];
        if (revolve && f.revolve) {
            auto p = std::make_shared<RevolveParams>(*f.revolve);
            p->targets = refs;
            f.revolve = p;
        } else if (!revolve && f.extrude) {
            auto p = std::make_shared<ExtrudeParams>(*f.extrude);
            p->targets = refs;
            f.extrude = p;
        }
    };

    if (!evalErr.empty()) { out = evalErr; return true; }

    if (name == "get_body") {
        std::string err;
        auto ids = idsFrom(a["body"], err);
        if (!err.empty()) { out = err; return true; }
        if (ids.empty()) { out = "Name a body."; return true; }
        auto bit = doc_.bodies.find(ids[0]);
        auto pit = doc_.parts.find(bit->second.partId);
        if (pit == doc_.parts.end() || !pit->second.shape) { out = "That body has no solid."; return true; }
        Value d = Value::object();
        d.set("id", (int)ids[0]);
        d.set("path", aiBodyPath(ids[0]));
        Value faces = Value::array();
        uint32_t n = kernel::faceCount(pit->second.shape);
        uint32_t limit = std::min(n, 80u);
        for (uint32_t i = 0; i < limit; ++i) {
            Value fo = Value::object();
            fo.set("id", (int)i);
            double radius = 0;
            fo.set("type", kernel::faceKind(pit->second.shape, i, radius));
            if (radius > 0) fo.set("radius", radius);
            glm::dvec3 c, normal;
            bool planar = false;
            if (kernel::faceInfo(pit->second.shape, i, c, normal, planar)) {
                glm::dmat4 xf(bit->second.xform);
                glm::dvec3 cw = glm::dvec3(xf * glm::dvec4(c, 1.0));
                glm::dvec3 nw = glm::normalize(glm::dmat3(xf) * normal);
                char buf[96];
                std::snprintf(buf, sizeof buf, "%.3f, %.3f, %.3f", cw.x, cw.y, cw.z);
                fo.set("center", buf);
                std::snprintf(buf, sizeof buf, "%.4f, %.4f, %.4f", nw.x, nw.y, nw.z);
                fo.set("normal", buf);
            }
            faces.push(fo);
        }
        d.set("faces", faces);
        d.set("face_count", (int)n);
        d.set("edge_count", (int)kernel::edgeCount(pit->second.shape));
        if (n > limit) d.set("truncated", true);
        out = d.dump();
        return true;
    }

    if (name == "create_component") {
        std::string nm = a["name"].asString();
        if (nm.empty()) { out = "name is empty."; return true; }
        // A new component goes inside the active one (like Fusion); parent names another, "" for the top.
        uint32_t parent = doc_.activeComponent && doc_.components.count(doc_.activeComponent) ? doc_.activeComponent : doc_.rootId;
        if (a.has("parent")) {
            std::string pn = a["parent"].asString();
            parent = pn.empty() ? doc_.rootId : compByPath(pn);
            if (!parent) { out = "No component named " + pn + "."; return true; }
        }
        uint32_t id = 0;
        for (auto& [cid, c] : doc_.components)
            if (cid != doc_.rootId && low(c.name) == low(nm) && c.parentId == parent) id = cid;
        if (!id) {
            pushUndo("Folder");
            Component c;
            c.id = doc_.newId();
            c.name = nm;
            c.parentId = parent;
            doc_.components[c.id] = c;
            id = c.id;
        }
        doc_.activeComponent = id;
        doc_.touch();
        out = "component_id=" + std::to_string(id) + " name=" + nm;
        return true;
    }

    if (name == "activate_component") {
        std::string nm = a["name"].asString();
        if (nm.empty()) {
            doc_.activeComponent = doc_.rootId;
            doc_.touch();
            out = "Activated the root.";
            return true;
        }
        std::string n = low(nm);
        uint32_t id = 0;
        for (auto& [cid, c] : doc_.components)
            if (cid != doc_.rootId && low(c.name) == n) id = cid;
        if (!id) { out = "No component named " + nm + "."; return true; }
        doc_.activeComponent = id;
        doc_.touch();
        out = "Activated " + doc_.components[id].name + ".";
        return true;
    }

    if (name == "ground_component") {
        std::string nm = a["name"].asString();
        if (nm.empty()) { out = "Name the component."; return true; }
        std::string n = low(nm);
        uint32_t id = 0;
        for (auto& [cid, c] : doc_.components)
            if (low(c.name) == n) id = cid;
        if (!id) { out = "No component named " + nm + "."; return true; }
        if (id == doc_.rootId) { out = "The root design cannot be grounded."; return true; }
        bool on = a.has("grounded") ? a["grounded"].asBool(true) : true;
        doc_.components[id].grounded = on;
        doc_.touch();
        dirty_ = true;
        out = std::string(on ? "Grounded " : "Ungrounded ") + doc_.components[id].name + ".";
        return true;
    }

    if (name == "create_sketch") {
        // An empty body ("", null, []) means no body: models often send one next to plane.
        const auto& bj = a["body"];
        bool hasBody = a.has("body") && !bj.isNull() && !(bj.isString() && bj.asString().empty()) && !(bj.isArray() && bj.size() == 0);
        if (hasBody) {
            std::string err;
            auto ids = idsFrom(a["body"], err);
            if (!err.empty() || ids.empty()) { out = err.empty() ? "Name the body whose face gets the sketch." : err; return true; }
            int face = (int)a["face"].asNumber(0);
            if (!createSketchOnFace(ids[0], (uint32_t)face, std::string())) { out = "That face cannot hold a sketch. Pick a flat face."; return true; }
        } else {
            sk::Plane pl;
            if (a["plane"].isObject()) {
                glm::dvec3 o{0}, n{0, 0, 1}, x{1, 0, 0};
                read3(a["plane"]["origin"], o);
                read3(a["plane"]["normal"], n);
                read3(a["plane"]["x_dir"], x);
                if (glm::length(n) < 1e-12) n = {0, 0, 1};
                n = glm::normalize(n);
                if (glm::length(x) < 1e-12 || std::abs(glm::dot(glm::normalize(x), n)) > 0.99)
                    x = std::abs(n.z) < 0.9 ? glm::cross(n, glm::dvec3(0, 0, 1)) : glm::cross(n, glm::dvec3(0, 1, 0));
                x = glm::normalize(x - n * glm::dot(n, x));
                pl.o = o;
                pl.x = x;
                pl.y = glm::cross(n, x);
                pl.name = "Custom";
            } else {
                pl = originPlane(a["plane"].asString("XY"));
            }
            glm::dvec3 nn = glm::cross(pl.x, pl.y);
            if (glm::length(nn) > 1e-12) pl.o += glm::normalize(nn) * a["offset"].asNumber(0);
            createSketchOn(pl, std::string());
        }
        sk::Sketch* s = curSketch();
        if (!s) { out = "Could not start a sketch."; return true; }
        out = "sketch_id=" + std::to_string(s->id);
        return true;
    }

    if (name == "sketch_add" || name == "sketch_constrain" || name == "sketch_ops" || name == "get_sketch") {
        uint32_t sid = (uint32_t)a["sketch_id"].asNumber(0);
        auto sit = doc_.sketches.find(sid);
        if (sit == doc_.sketches.end()) { out = "No sketch with that id."; return true; }
        sk::Sketch* s = &sit->second;
        auto closeSketchUi = [&] {
            if (!sk_.active) return;
            sk::Sketch* cs = curSketch();
            if (cs && cs->geometryCount() == 0) return;
            finishSketch();
        };
        if (name == "sketch_ops") {
            std::string err;
            s = useSketch(sid, err);
            if (!s) { out = err; return true; }
        }

        if (name == "get_sketch") {
            sk::computeDof(*s);
            auto profs = sk::findProfiles(*s);
            Value d = Value::object();
            d.set("sketch_id", (int)s->id);
            d.set("dof", s->dof);
            Value ents = Value::array();
            for (auto& [id, e] : s->ents) {
                if (e.locked) continue;
                Value eo = Value::object();
                eo.set("id", (int)id);
                const char* ty = e.type == sk::EntType::Line ? "line" : e.type == sk::EntType::Circle ? "circle"
                                 : e.type == sk::EntType::Arc ? "arc" : e.type == sk::EntType::Spline ? "spline"
                                 : e.type == sk::EntType::Ellipse ? "ellipse" : e.type == sk::EntType::EllipArc ? "ellipse_arc"
                                 : e.type == sk::EntType::CtrlSpline ? "control_spline" : "point";
                eo.set("type", ty);
                eo.set("construction", e.construction);
                if (e.type == sk::EntType::Point) {
                    char b[64];
                    std::snprintf(b, sizeof b, "%.4f, %.4f", e.x, e.y);
                    eo.set("at", b);
                } else if (e.type == sk::EntType::Circle) {
                    eo.set("radius", e.r);
                }
                ents.push(eo);
            }
            d.set("entities", ents);
            Value ps = Value::array();
            for (size_t i = 0; i < profs.size(); ++i) {
                Value po = Value::object();
                po.set("id", (int)i);
                po.set("area", profs[i].area);
                po.set("closed", true);
                po.set("holes", (int)profs[i].holes.size());
                ps.push(po);
            }
            d.set("profiles", ps);
            out = d.dump();
            return true;
        }

        if (name == "sketch_constrain") {
            int added = 0;
            for (auto& c : a["constraints"].items()) {
                bool ok = false;
                sk::CType ty = constraintType(c["type"].asString(), ok);
                if (!ok) { out = "Unknown constraint type '" + c["type"].asString() + "'."; return true; }
                std::vector<uint32_t> refs;
                for (auto& r : c["refs"].items())
                    if (r.isNumber()) refs.push_back((uint32_t)r.asNumber());
                if (refs.empty()) { out = "A constraint needs refs."; return true; }
                double value = 0;
                std::string expr;
                if (c["value"].isString()) {
                    expr = c["value"].asString();
                    try { value = peval::eval(expr, doc_.params); }
                    catch (const std::runtime_error& ex) { out = ex.what(); return true; }
                } else if (c["value"].isNumber()) value = c["value"].asNumber();
                if (ty == sk::CType::Angle) value = value * kPi / 180.0;
                bool driven = c["driven"].asBool(false);
                if (driven && !sk::isDimension(ty)) { out = "Only a dimension can be driven."; return true; }
                uint32_t e1 = refs.size() > 1 ? refs[1] : 0, e2 = refs.size() > 2 ? refs[2] : 0;
                uint32_t cid = s->addConstraint(ty, refs[0], e1, e2, value);
                if (ty == sk::CType::Fix && c["value2"].isNumber()) s->cons[cid].value2 = c["value2"].asNumber();
                if (!expr.empty() && !driven) s->cons[cid].expr = expr;
                if (driven) {
                    s->cons[cid].driven = true;
                    s->cons[cid].expr.clear();
                    double m = s->measureDimension(s->cons[cid]);
                    if (std::isfinite(m)) s->cons[cid].value = m;
                }
                ++added;
            }
            sk::SolveResult sr = sk::solve(*s, {});
            sk::computeDof(*s);
            s->touch();
            doc_.touch();
            if (!sr.ok) { out = "The constraints conflict, so the sketch was left as it was."; return true; }
            out = "Added " + std::to_string(added) + " constraints. dof=" + std::to_string(s->dof);
            closeSketchUi();
            return true;
        }

        if (name == "sketch_ops") {
            std::string op = low(a["op"].asString());
            if (op == "offset") {
                sketchOffsetPick((uint32_t)a["entity"].asNumber(0));
                sketchOffsetCommit(num(a["distance"], 0));
                out = sk_.picks.empty() && a["entity"].asNumber(0) ? "Offset did not apply. Try a smaller distance." : "Offset added.";
                // sketchOffsetCommit clears the chain by finishing. Report geometry.
                out = "Offset committed on the sketch.";
                closeSketchUi();
                return true;
            }
            if (op == "trim") {
                sk::Vec2 at{0, 0};
                read2(a["at"], at);
                trimAt((uint32_t)a["entity"].asNumber(0), at);
                out = "Trimmed.";
                closeSketchUi();
                return true;
            }
            if (op == "break") {
                uint32_t ent = (uint32_t)a["entity"].asNumber(0);
                breakAt(ent);
                out = s->find(ent) ? "Break did not apply. Another curve has to cross that one." : "Broke the curve where it was crossed.";
                closeSketchUi();
                return true;
            }
            if (op == "fillet") {
                uint32_t corner = (uint32_t)a["corner"].asNumber(0);
                sk::Vec2 p = s->pt(corner);
                int before = 0;
                for (auto& [id, e] : s->ents) if (e.type == sk::EntType::Arc) ++before;
                double rad = a.has("radius") ? num(a["radius"], 0) : num(a["distance"], 0);
                sketchFilletAt(corner, p, rad);
                int after = 0;
                for (auto& [id, e] : s->ents) if (e.type == sk::EntType::Arc) ++after;
                out = after > before ? "Filleted the corner." : "Fillet did not apply. Two lines have to meet at that corner.";
                closeSketchUi();
                return true;
            }
            if (op == "chamfer") {
                uint32_t corner = (uint32_t)a["corner"].asNumber(0);
                if (!s->find(corner)) { out = "No point with that id."; return true; }
                sk::Vec2 p = s->pt(corner);
                int before = (int)s->ents.size();
                double dist = a.has("distance") ? num(a["distance"], 0) : 0;
                sketchChamferAt(corner, p, dist);
                out = (int)s->ents.size() > before ? "Chamfered the corner." : "Chamfer did not apply. Two lines have to meet at that corner.";
                closeSketchUi();
                return true;
            }
            if (op == "mirror") {
                sk_.selEnt.clear();
                for (auto& e : a["entities"].items())
                    if (e.isNumber()) sk_.selEnt.insert((uint32_t)e.asNumber());
                mirrorSelection((uint32_t)a["axis"].asNumber(0));
                out = "Mirrored in the sketch.";
                closeSketchUi();
                return true;
            }
            if (op == "move" || op == "rotate" || op == "scale" || op == "copy") {
                sk_.selEnt.clear();
                for (auto& e : a["entities"].items())
                    if (e.isNumber()) sk_.selEnt.insert((uint32_t)e.asNumber());
                if (sk_.selEnt.empty()) { out = "Name the curves to transform."; return true; }
                sk::Vec2 pivot{0, 0};
                read2(a["pivot"], pivot);
                double dx = num(a["dx"], 0), dy = num(a["dy"], 0);
                double ang = num(a["angle"], 0);
                double fac = a.has("factor") ? num(a["factor"], 1) : 1;
                bool translate = op == "move" || op == "copy";
                bool rot = op == "rotate" || (op == "copy" && a.has("angle"));
                bool scale = op == "scale" || (op == "copy" && a.has("factor"));
                bool ok = sketchTransform(translate, rot, scale, op == "copy", dx, dy, ang, fac, pivot);
                out = ok ? (op == "copy" ? "Copied the sketch curves." : op == "rotate" ? "Rotated the sketch curves." : op == "scale" ? "Scaled the sketch curves." : "Moved the sketch curves.")
                         : "Transform did not apply.";
                closeSketchUi();
                return true;
            }
            if (op == "pattern_rect" || op == "pattern_circ") {
                sk_.selEnt.clear();
                for (auto& e : a["entities"].items())
                    if (e.isNumber()) sk_.selEnt.insert((uint32_t)e.asNumber());
                if (sk_.selEnt.empty()) { out = "Name the curves to pattern."; return true; }
                sk::Vec2 pivot{0, 0};
                read2(a["pivot"], pivot);
                bool ok = false;
                if (op == "pattern_circ") {
                    int n = (int)std::lround(num(a["count"], 4));
                    double ang = num(a["angle"], 360);
                    ok = sketchPattern(true, n, 1, 0, 0, ang, pivot);
                } else {
                    int nx = (int)std::lround(num(a["count_x"], 2));
                    int ny = (int)std::lround(num(a["count_y"], 1));
                    double sx = num(a["spacing_x"], 0), sy = num(a["spacing_y"], 0);
                    ok = sketchPattern(false, nx, ny, sx, sy, 0, pivot);
                }
                out = ok ? "Patterned the sketch curves." : "Pattern did not apply.";
                closeSketchUi();
                return true;
            }
            if (op == "project") {
                std::string berr;
                auto ids = idsFrom(a["body"], berr);
                if (ids.empty()) { out = berr.empty() ? "Name the body to project." : berr; return true; }
                auto bit = doc_.bodies.find(ids[0]);
                auto pit = doc_.parts.find(bit->second.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) { out = "That body has no solid."; return true; }
                std::vector<uint32_t> edges(kernel::edgeCount(pit->second.shape));
                for (uint32_t i = 0; i < edges.size(); ++i) edges[i] = i;
                int made = projectEdgesInto(*s, ids[0], edges);
                if (made) { sk::solve(*s, {}); s->touch(); }
                out = "Projected " + std::to_string(made) + " edges.";
                closeSketchUi();
                return true;
            }
            if (op == "intersect") {
                std::string berr;
                auto ids = idsFrom(a["body"], berr);
                if (ids.empty()) { out = berr.empty() ? "Name the body to intersect." : berr; return true; }
                int made = intersectBodyInto(*s, ids[0]);
                if (made) { sk::solve(*s, {}); s->touch(); }
                out = made ? "Intersected " + std::to_string(made) + " curves." : "The body does not cross the sketch plane.";
                closeSketchUi();
                return true;
            }
            if (op == "project_face") {
                std::string berr;
                auto ids = idsFrom(a["body"], berr);
                if (ids.empty()) { out = berr.empty() ? "Name the body." : berr; return true; }
                auto bit = doc_.bodies.find(ids[0]);
                auto pit = doc_.parts.find(bit->second.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) { out = "That body has no solid."; return true; }
                int face = a.has("face") ? (int)a["face"].asNumber(-1) : -1;
                if (face < 0) {
                    glm::dvec3 at;
                    if (!read3(a["at"], at)) { out = "Name the face, or a point on it."; return true; }
                    face = kernel::faceNear(pit->second.shape, bit->second.xform, at, 1.0);
                }
                if (face < 0) { out = "That point is not on the body."; return true; }
                std::vector<uint32_t> ents;
                if (a.has("entities"))
                    for (auto& e : a["entities"].items())
                        if (e.isNumber()) ents.push_back((uint32_t)e.asNumber());
                glm::dvec3 dir = s->plane.normal();
                if (a.has("direction") && !read3(a["direction"], dir)) { out = "Direction is [x,y,z]."; return true; }
                if (glm::length(dir) < 1e-12) { out = "Direction is [x,y,z]."; return true; }
                int made = projectCurvesOnFace(*s, ids[0], face, ents, dir);
                if (made > 0) { sk::solve(*s, {}); s->touch(); }
                out = made > 0 ? "Dropped " + std::to_string(made) + " curves onto the face."
                               : made < 0 ? "Onto Face works on lines, arcs, and circles."
                                          : "The curves do not meet that face.";
                closeSketchUi();
                return true;
            }
            if (op == "point_cross") {
                auto curveOk = [&](uint32_t id) {
                    const sk::Entity* e = s->find(id);
                    return e && (e->type == sk::EntType::Line || e->type == sk::EntType::Arc || e->type == sk::EntType::Circle);
                };
                sk::Vec2 hint{0, 0};
                bool hasAt = read2(a["at"], hint);
                std::vector<uint32_t> ents;
                if (a.has("entities"))
                    for (auto& e : a["entities"].items())
                        if (e.isNumber()) ents.push_back((uint32_t)e.asNumber());
                uint32_t ca = 0, cb = 0;
                sk::Vec2 at;
                if (ents.size() >= 2) {
                    if (!curveOk(ents[0]) || !curveOk(ents[1])) { out = "Crossing works on lines, arcs, and circles."; return true; }
                    std::vector<sk::Vec2> pts;
                    sk::crossingsOf(*s, ents[0], ents[1], pts);
                    if (pts.empty()) { out = "Those curves do not cross."; return true; }
                    if (pts.size() > 1 && !hasAt) { out = "Those curves meet more than once. Pass at."; return true; }
                    at = pts[0];
                    if (hasAt) {
                        double best = 1e300;
                        for (auto& p : pts) {
                            double d = glm::length(p - hint);
                            if (d < best) { best = d; at = p; }
                        }
                    }
                    ca = ents[0]; cb = ents[1];
                } else {
                    if (!hasAt) { out = "Pass at, or two curves."; return true; }
                    if (!sk::nearestCrossing(*s, hint, 1.0, ca, cb, at)) { out = "No crossing there."; return true; }
                }
                uint32_t p = addCrossingPoint(*s, ca, cb, at);
                if (!p) { out = "That point fights the sketch's constraints."; return true; }
                sk::Vec2 pos = s->pt(p);
                out = "point_id=" + std::to_string(p) + " at " + numStr(pos.x) + "," + numStr(pos.y);
                closeSketchUi();
                return true;
            }
            out = "Unknown sketch op.";
            return true;
        }

        // sketch_add
        bool construction = a["construction"].asBool(false);
        std::vector<uint32_t> made;
        uint32_t textNote = 0;
        auto addMade = [&](uint32_t id) { if (id) made.push_back(id); };
        auto reusePt = [&](sk::Vec2 p) {
            for (auto& [id, e] : s->ents)
                if (!e.locked && e.type == sk::EntType::Point && glm::length(sk::Vec2(e.x, e.y) - p) < 1e-6)
                    return id;
            return s->addPoint(p);
        };
        for (auto& e : a["entities"].items()) {
            std::string ty = low(e["type"].asString());
            bool cons = construction || e["construction"].asBool(false);
            if (ty == "text") {
                std::string word = e["text"].asString();
                if (word.empty()) { out = "Text is empty."; return true; }
                double h = num(e["height"], 10);
                if (!(h > 0)) { out = "Text height must be above zero."; return true; }
                std::string font = e["font"].asString();
                uint32_t nid = (uint32_t)e["text_id"].asNumber(0);
                if (nid) {
                    if (font.empty()) font = "Arial";
                    if (!sk::editSketchText(*s, nid, word, h, font)) {
                        out = "No text with that id, or that font produced nothing.";
                        return true;
                    }
                    textNote = nid;
                    continue;
                }
                sk::Vec2 at{num(e["x"], 0), num(e["y"], 0)};
                read2(e["p"], at);
                uint32_t note = 0;
                auto ids = sk::addSketchText(*s, word, at, h, cons, font, &note);
                if (ids.empty()) { out = font.empty() ? "That text has no outlines." : "That font produced no outlines."; return true; }
                for (uint32_t id : ids) addMade(id);
                if (note) textNote = note;
                continue;
            }
            if (ty == "spline" || ty == "control_spline") {
                std::vector<sk::Vec2> fit;
                for (auto& p : e["points"].items()) {
                    sk::Vec2 v;
                    if (p.isArray() && p.size() >= 2) v = {p[(size_t)0].asNumber(), p[(size_t)1].asNumber()};
                    else if (!read2(p, v)) { out = "Each spline point is [x, y]."; return true; }
                    fit.push_back(v);
                }
                bool closed = e["closed"].asBool(false);
                if ((int)fit.size() < (closed ? 3 : 2)) {
                    out = closed ? "A closed spline needs 3 points." : "A spline needs 2 points.";
                    return true;
                }
                std::vector<uint32_t> ids;
                for (auto& p : fit) ids.push_back(reusePt(p));
                uint32_t id = ty == "control_spline" ? s->addCtrlSpline(ids, closed, cons) : s->addSpline(ids, closed, cons);
                if (!id) { out = "Could not add that spline."; return true; }
                addMade(id);
            } else if (ty == "point") {
                sk::Vec2 p{e["x"].asNumber(), e["y"].asNumber()};
                read2(e["p"], p);
                addMade(reusePt(p));
            } else if (ty == "line") {
                sk::Vec2 p1, p2;
                if (!read2(e["p1"], p1) || !read2(e["p2"], p2)) { out = "A line needs p1 and p2."; return true; }
                uint32_t a1 = reusePt(p1), b1 = reusePt(p2);
                addMade(a1); addMade(b1);
                addMade(s->addLine(a1, b1, cons));
            } else if (ty == "rect") {
                sk::Vec2 p1, p2;
                if (e.has("w")) {
                    p1 = {e["x"].asNumber(), e["y"].asNumber()};
                    p2 = p1 + sk::Vec2(num(e["w"], 0), num(e["h"], 0));
                } else if (!read2(e["p1"], p1) || !read2(e["p2"], p2)) {
                    out = "A rect needs p1 and p2, or x, y, w, h."; return true;
                }
                uint32_t p0 = reusePt(p1), pA = reusePt({p2.x, p1.y}), pB = reusePt(p2), pC = reusePt({p1.x, p2.y});
                addMade(s->addLine(p0, pA, cons));
                addMade(s->addLine(pA, pB, cons));
                addMade(s->addLine(pB, pC, cons));
                addMade(s->addLine(pC, p0, cons));
            } else if (ty == "rect_3pt") {
                sk::Vec2 p1, p2, p3;
                if (!read2(e["p1"], p1) || !read2(e["p2"], p2) || !read2(e["p3"], p3)) {
                    out = "A 3 point rectangle needs p1, p2 and p3."; return true;
                }
                sk::Vec2 d = p2 - p1;
                double len = glm::length(d);
                if (len < 1e-9) { out = "That side has no length."; return true; }
                sk::Vec2 n(-d.y / len, d.x / len);
                double w = glm::dot(p3 - p1, n);
                if (std::abs(w) < 1e-6) { out = "The third point is on the side, so the rectangle has no width."; return true; }
                uint32_t a = reusePt(p1), b = reusePt(p2);
                uint32_t c = reusePt(p2 + n * w), dpt = reusePt(p1 + n * w);
                uint32_t l0 = s->addLine(a, b, cons), l1 = s->addLine(b, c, cons);
                uint32_t l2 = s->addLine(c, dpt, cons), l3 = s->addLine(dpt, a, cons);
                s->addConstraint(sk::CType::Perpendicular, l0, l1);
                s->addConstraint(sk::CType::Perpendicular, l0, l3);
                s->addConstraint(sk::CType::Parallel, l0, l2);
                addMade(l0); addMade(l1); addMade(l2); addMade(l3);
            } else if (ty == "circle") {
                sk::Vec2 c{e["x"].asNumber(), e["y"].asNumber()};
                read2(e["center"], c);
                double r = e.has("r") ? num(e["r"], 0) : e.has("radius") ? num(e["radius"], 0) : num(e["d"].isNumber() || e["d"].isString() ? e["d"] : e["diameter"], 0) * 0.5;
                if (!(r > 0)) { out = "A circle needs a radius or a diameter."; return true; }
                addMade(s->addCircle(reusePt(c), r, cons));
            } else if (ty == "arc") {
                if (e.has("p3") || (e.has("p1") && e.has("p2") && e.has("p3"))) {
                    sk::Vec2 p1, p2, p3;
                    if (!read2(e["p1"], p1) || !read2(e["p2"], p2) || !read2(e["p3"], p3)) { out = "A 3 point arc needs p1, p2, p3."; return true; }
                    sk::Vec2 ab = p2 - p1, ac = p3 - p1;
                    double d = 2.0 * (ab.x * ac.y - ab.y * ac.x);
                    if (std::abs(d) < 1e-12) { out = "Those three points are in a line."; return true; }
                    double ab2 = glm::dot(ab, ab), ac2 = glm::dot(ac, ac);
                    sk::Vec2 cen = p1 + sk::Vec2(ac.y * ab2 - ab.y * ac2, ab.x * ac2 - ac.x * ab2) / d;
                    auto ang = [&](sk::Vec2 p) { return std::atan2(p.y - cen.y, p.x - cen.x); };
                    auto ccw = [](double from, double to) { double s = to - from; if (s < 0) s += 2 * kPi; return s; };
                    sk::Vec2 start = p1, end = p3, mid = p2;
                    if (!(ccw(ang(start), ang(mid)) <= ccw(ang(start), ang(end)) + 1e-6)) std::swap(start, end);
                    uint32_t cs = reusePt(cen), as = reusePt(start), es = reusePt(end);
                    addMade(s->addArc(cs, as, es, cons));
                } else {
                    sk::Vec2 c, s0, e0;
                    if (!read2(e["center"], c) || !read2(e["start"], s0) || !read2(e["end"], e0)) {
                        out = "An arc needs center, start and end, or p1, p2, p3."; return true;
                    }
                    addMade(s->addArc(reusePt(c), reusePt(s0), reusePt(e0), cons));
                }
            } else if (ty == "polygon") {
                sk::Vec2 c{0, 0};
                read2(e["center"], c);
                int sides = std::max(3, (int)num(e["sides"], 6));
                double r = num(e["r"], num(e["radius"], 0));
                if (!(r > 0)) { out = "A polygon needs a radius."; return true; }
                std::vector<uint32_t> pts;
                for (int i = 0; i < sides; ++i) {
                    double th = 2 * kPi * i / sides;
                    pts.push_back(reusePt(c + sk::Vec2(std::cos(th), std::sin(th)) * r));
                }
                for (int i = 0; i < sides; ++i) addMade(s->addLine(pts[i], pts[(i + 1) % sides], cons));
            } else if (ty == "polygon_in") {
                sk::Vec2 c{0, 0};
                read2(e["center"], c);
                int sides = std::max(3, (int)num(e["sides"], 6));
                double r = num(e["r"], num(e["radius"], 0));
                if (!(r > 0)) { out = "An inscribed polygon needs a radius."; return true; }
                uint32_t cp = reusePt(c);
                std::vector<uint32_t> pts, lines;
                for (int i = 0; i < sides; ++i) {
                    double th = 2 * kPi * i / sides;
                    pts.push_back(reusePt(c + sk::Vec2(std::cos(th), std::sin(th)) * r));
                }
                for (int i = 0; i < sides; ++i) lines.push_back(s->addLine(pts[i], pts[(i + 1) % sides], cons));
                uint32_t circle = s->addCircle(cp, r, true);
                for (int i = 0; i + 1 < sides; ++i) s->addConstraint(sk::CType::Equal, lines[i], lines[i + 1]);
                for (uint32_t p : pts) s->addConstraint(sk::CType::PointOnObject, p, circle);
                for (uint32_t id : lines) addMade(id);
            } else if (ty == "ellipse" || ty == "ellipse_arc") {
                sk::Vec2 c{0, 0};
                read2(e["center"], c);
                double major = num(e["major"], 0);
                double minor = num(e["minor"], 0);
                double rot = num(e["rotation"], 0) * kPi / 180.0;
                if (!(major > 0) || !(minor > 0)) { out = "An ellipse needs a major and a minor radius."; return true; }
                uint32_t cp = reusePt(c);
                if (ty == "ellipse") {
                    uint32_t id = s->addEllipse(cp, major, minor, rot, cons);
                    if (!id) { out = "Could not add that ellipse."; return true; }
                    addMade(id);
                } else {
                    auto on = [&](double ang) {
                        double lx = major * std::cos(ang), ly = minor * std::sin(ang);
                        double cs = std::cos(rot), sn = std::sin(rot);
                        return c + sk::Vec2(cs * lx - sn * ly, sn * lx + cs * ly);
                    };
                    double a0 = num(e["start"], 0) * kPi / 180.0;
                    double a1 = num(e["end"], 180) * kPi / 180.0;
                    uint32_t id = s->addEllipArc(cp, major, minor, rot, reusePt(on(a0)), reusePt(on(a1)), cons);
                    if (!id) { out = "Could not add that elliptical arc."; return true; }
                    addMade(id);
                }
            } else if (ty == "slot") {
                sk::Vec2 p1, p2;
                if (!read2(e["p1"], p1) || !read2(e["p2"], p2)) { out = "A slot needs p1, p2 and width."; return true; }
                double width = num(e["width"], 0);
                sk::Vec2 d = p2 - p1;
                double len = glm::length(d);
                if (!(len > 1e-9) || !(width > 0)) { out = "The slot length and width must be above 0."; return true; }
                sk::Vec2 u = d / len, n(-u.y, u.x);
                double r = width * 0.5;
                sk::Vec2 a1 = p1 + n * r, b1 = p2 + n * r, c1 = p2 - n * r, d1 = p1 - n * r;
                uint32_t ia = reusePt(a1), ib = reusePt(b1), ic = reusePt(c1), id = reusePt(d1);
                uint32_t ip1 = reusePt(p1), ip2 = reusePt(p2);
                addMade(s->addLine(ia, ib, cons));
                addMade(s->addArc(ip2, ic, ib, cons));   // cap past p2
                addMade(s->addLine(ic, id, cons));
                addMade(s->addArc(ip1, ia, id, cons));   // cap past p1, the other way
            } else if (ty == "slot_overall") {
                sk::Vec2 p1, p2;
                if (!read2(e["p1"], p1) || !read2(e["p2"], p2)) { out = "An overall slot needs p1, p2 and width."; return true; }
                double width = num(e["width"], 0);
                sk::Vec2 d = p2 - p1;
                double len = glm::length(d);
                if (!(len > 1e-9) || !(width > 0)) { out = "The slot length and width must be above 0."; return true; }
                if (width >= len - 1e-6) { out = "An overall slot has to be longer than it is wide."; return true; }
                sk::Vec2 u = d / len, n(-u.y, u.x);
                double r = width * 0.5;
                sk::Vec2 c1 = p1 + u * r, c2 = p2 - u * r;
                uint32_t pc1 = reusePt(c1), pc2 = reusePt(c2);
                uint32_t q1 = reusePt(c1 - n * r), q2 = reusePt(c2 - n * r);
                uint32_t q3 = reusePt(c2 + n * r), q4 = reusePt(c1 + n * r);
                uint32_t l1 = s->addLine(q1, q2, cons);
                uint32_t a2 = s->addArc(pc2, q2, q3, cons);
                uint32_t l2 = s->addLine(q3, q4, cons);
                uint32_t a1 = s->addArc(pc1, q4, q1, cons);
                s->addConstraint(sk::CType::Tangent, l1, a1); s->addConstraint(sk::CType::Tangent, l1, a2);
                s->addConstraint(sk::CType::Tangent, l2, a1); s->addConstraint(sk::CType::Tangent, l2, a2);
                s->addConstraint(sk::CType::Equal, a1, a2);
                addMade(l1); addMade(a2); addMade(l2); addMade(a1);
            } else if (ty == "slot_center") {
                sk::Vec2 ctr, cap;
                if (!read2(e["center"], ctr) || !read2(e["end"], cap)) { out = "A center point slot needs center, end and width."; return true; }
                double width = num(e["width"], 0);
                sk::Vec2 d = cap - ctr;
                double half = glm::length(d);
                if (!(half > 1e-9) || !(width > 0)) { out = "The slot length and width must be above 0."; return true; }
                sk::Vec2 n(-d.y / half, d.x / half);
                double r = width * 0.5;
                sk::Vec2 c2 = cap, c1 = ctr - d;
                uint32_t mid = reusePt(ctr);
                uint32_t pc2 = reusePt(c2), pc1 = reusePt(c1);
                uint32_t axis = s->addLine(pc1, pc2, true);
                s->addConstraint(sk::CType::Midpoint, mid, axis);
                uint32_t q1 = reusePt(c1 - n * r), q2 = reusePt(c2 - n * r);
                uint32_t q3 = reusePt(c2 + n * r), q4 = reusePt(c1 + n * r);
                uint32_t l1 = s->addLine(q1, q2, cons);
                uint32_t a2 = s->addArc(pc2, q2, q3, cons);
                uint32_t l2 = s->addLine(q3, q4, cons);
                uint32_t a1 = s->addArc(pc1, q4, q1, cons);
                s->addConstraint(sk::CType::Tangent, l1, a1); s->addConstraint(sk::CType::Tangent, l1, a2);
                s->addConstraint(sk::CType::Tangent, l2, a1); s->addConstraint(sk::CType::Tangent, l2, a2);
                s->addConstraint(sk::CType::Equal, a1, a2);
                addMade(l1); addMade(a2); addMade(l2); addMade(a1);
            } else if (ty == "slot_arc") {
                sk::Vec2 p1, p2, p3;
                if (!read2(e["p1"], p1) || !read2(e["p2"], p2) || !read2(e["p3"], p3)) {
                    out = "A 3 point arc slot needs p1, p2, p3 and width."; return true;
                }
                double width = num(e["width"], 0);
                if (!(width > 0)) { out = "The width must be above 0."; return true; }
                double den = 2.0 * (p1.x * (p2.y - p3.y) + p2.x * (p3.y - p1.y) + p3.x * (p1.y - p2.y));
                if (std::abs(den) < 1e-9) { out = "Those three points are in a line."; return true; }
                double aa = glm::dot(p1, p1), bb = glm::dot(p2, p2), cc = glm::dot(p3, p3);
                sk::Vec2 o;
                o.x = (aa * (p2.y - p3.y) + bb * (p3.y - p1.y) + cc * (p1.y - p2.y)) / den;
                o.y = (aa * (p3.x - p2.x) + bb * (p1.x - p3.x) + cc * (p2.x - p1.x)) / den;
                double R = glm::length(p1 - o);
                double hw = width * 0.5;
                if (!(R > hw + 1e-6)) { out = "The slot is wider than the arc."; return true; }
                auto ang = [&](sk::Vec2 p) { return std::atan2(p.y - o.y, p.x - o.x); };
                auto wrap = [](double a) { a = std::fmod(a, 2 * kPi); if (a < 0) a += 2 * kPi; return a; };
                double rel3 = wrap(ang(p3) - ang(p1));
                double rel2 = wrap(ang(p2) - ang(p1));
                sk::Vec2 sa, sb;
                double sweep;
                if (rel2 <= rel3 + 1e-9) { sa = p1; sb = p3; sweep = rel3; }
                else { sa = p3; sb = p1; sweep = wrap(ang(p1) - ang(p3)); }
                if (sweep < 1e-4 || sweep > 2 * kPi - 1e-4) { out = "That arc does not make a slot."; return true; }
                auto rad = [&](sk::Vec2 q) { return (q - o) / R; };
                sk::Vec2 os = o + rad(sa) * (R + hw), oe = o + rad(sb) * (R + hw);
                sk::Vec2 is = o + rad(sa) * (R - hw), ie = o + rad(sb) * (R - hw);
                uint32_t cpt = reusePt(o);
                uint32_t pa = reusePt(sa), pb = reusePt(sb), pm = reusePt(p2);
                uint32_t outer = s->addArc(cpt, reusePt(os), reusePt(oe), cons);
                uint32_t inner = s->addArc(cpt, reusePt(is), reusePt(ie), cons);
                uint32_t capB = s->addArc(pb, s->find(outer)->b, s->find(inner)->b, cons);
                uint32_t capA = s->addArc(pa, s->find(inner)->a, s->find(outer)->a, cons);
                s->addConstraint(sk::CType::Tangent, outer, capA, 0, -1.0);
                s->addConstraint(sk::CType::Tangent, outer, capB, 0, -1.0);
                s->addConstraint(sk::CType::Tangent, inner, capA, 0, 1.0);
                s->addConstraint(sk::CType::Tangent, inner, capB, 0, 1.0);
                s->addConstraint(sk::CType::Equal, capA, capB);
                uint32_t spine = s->addArc(cpt, pa, pb, true);
                s->addConstraint(sk::CType::PointOnObject, pm, spine);
                addMade(outer); addMade(inner); addMade(capB); addMade(capA);
            } else {
                out = "Unknown curve type '" + ty + "'. Use line, rect, rect_3pt, circle, arc, ellipse, spline, polygon, polygon_in, slot, slot_overall, slot_center, slot_arc or point.";
                return true;
            }
        }
        s->touch();
        sk::computeDof(*s);
        doc_.touch();
        bool sketchOpen = sk_.active;
        std::string ids;
        for (size_t i = 0; i < made.size(); ++i) ids += (i ? "," : "") + std::to_string(made[i]);
        out = "entities=" + ids;
        if (textNote) out += " text_id=" + std::to_string(textNote);
        closeSketchUi();
        if (textNote) retargetSketchProfiles(s->id);
        if (!sketchOpen) {
            int fi = doc_.featureIndex(s->featureId);
            if (fi >= 0) scheduleRecompute(fi);
        }
        return true;
    }

    if (name == "extrude" || name == "revolve") {
        bool revolve = name == "revolve";
        uint32_t sid = (uint32_t)a["sketch_id"].asNumber(0);
        if (!doc_.sketches.count(sid)) { out = "No sketch with that id."; return true; }
        bool toFace = a.has("to_body") || a.has("to_face");
        bool thin = a.has("thickness");
        if (sk_.active) finishSketch();
        if (thin) {
            clearSelection();
            selProfiles_.clear();
            selFaces_.clear();
        } else {
            int which = a.has("profile") ? (int)a["profile"].asNumber(0) : -1;
            pickProfiles(sid, which);
            if (selProfiles_.empty()) { out = "That sketch has no closed profile."; return true; }
        }
        std::string terr;
        auto targets = refsFrom(a["target_bodies"], terr);
        if (!terr.empty()) { out = terr; return true; }
        if (revolve) runRevolve();
        else runExtrude();
        if (!dlg_.open) { out = revolve ? "Could not start Revolve." : "Could not start Extrude."; return true; }
        dlg_.opAuto = false;
        dlg_.op = opOf(a["operation"].asString("new"));
        dlg_.symmetric = low(a["direction"].asString()) == "symmetric" || low(a["direction"].asString()) == "two";
        if (!revolve) {
            if (!toFace && !putExpr(dlg_.dist, sizeof dlg_.dist, a["distance"], "10")) { out = "The distance expression is too long."; return true; }
            if (a.has("taper_angle")) putExpr(dlg_.taper, sizeof dlg_.taper, a["taper_angle"], "0");
            if (toFace) dlg_.extent = 1;
            if (thin && toFace) { closeFeatureDialog(false); out = "Thin uses a distance."; return true; }
        } else {
            if (a.has("angle")) putExpr(dlg_.angle, sizeof dlg_.angle, a["angle"], "360");
            if (a["axis"].isNumber()) {
                uint32_t ent = (uint32_t)a["axis"].asNumber();
                dlg_.axis = 0;
                for (size_t i = 0; i < dlg_.axisLines.size(); ++i)
                    if (dlg_.axisLines[i].first == ent) dlg_.axis = 2 + (int)i;
                if (dlg_.axis < 2) { out = "That sketch line cannot be the revolve axis."; closeFeatureDialog(false); return true; }
            } else {
                std::string ax = low(a["axis"].asString("y"));
                if (ax == "x") dlg_.axis = 1;
                else if (ax == "y") dlg_.axis = 0;
                else { out = "The revolve axis must lie in the sketch. Use X or Y, or a line entity id."; closeFeatureDialog(false); return true; }
            }
        }
        if (thin) {
            if (!putExpr(dlg_.thin, sizeof dlg_.thin, a["thickness"], "")) { closeFeatureDialog(false); out = "The thickness expression is too long."; return true; }
            std::string side = low(a["side"].asString("center"));
            if (side == "center") dlg_.thinSide = 0;
            else if (side == "one" || side == "left") dlg_.thinSide = 1;
            else if (side == "other" || side == "right") dlg_.thinSide = 2;
            else { closeFeatureDialog(false); out = "Wall side is center, one, or other."; return true; }
            int fi = doc_.featureIndex(dlg_.featureId);
            if (fi >= 0 && revolve && doc_.timeline[fi].revolve) {
                auto p = std::make_shared<RevolveParams>(*doc_.timeline[fi].revolve);
                p->sketchId = sid;
                doc_.timeline[fi].revolve = p;
            } else if (fi >= 0 && !revolve && doc_.timeline[fi].extrude) {
                auto p = std::make_shared<ExtrudeParams>(*doc_.timeline[fi].extrude);
                p->sketchId = sid;
                p->faces.clear();
                p->profilePts.clear();
                doc_.timeline[fi].extrude = p;
            }
        }
        setTargetsOnOpen(targets, revolve);
        if (!revolve && toFace) {
            std::string berr;
            auto bids = idsFrom(a["to_body"], berr);
            if (bids.empty()) { closeFeatureDialog(false); out = berr.empty() ? "Name the body the extrude stops on." : berr; return true; }
            auto bit = doc_.bodies.find(bids[0]);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            if (bit == doc_.bodies.end() || pit == doc_.parts.end() || !pit->second.shape) {
                closeFeatureDialog(false); out = "That body has no shape yet."; return true;
            }
            int face = a["to_face"].isNumber() ? (int)a["to_face"].asNumber(-1) : -1;
            if (face < 0) {
                auto sit = doc_.sketches.find(sid);
                glm::dvec3 dir(0, 0, 1);
                if (sit != doc_.sketches.end() && glm::length(sit->second.plane.normal()) > 1e-9)
                    dir = glm::normalize(sit->second.plane.normal());
                double best = -2;
                uint32_t n = kernel::faceCount(pit->second.shape);
                for (uint32_t i = 0; i < n; ++i) {
                    glm::dvec3 c, normal; bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, i, c, normal, planar) || !planar) continue;
                    double d = glm::dot(glm::normalize(glm::dmat3(bit->second.xform) * normal), dir);
                    if (d > best) { best = d; face = (int)i; }
                }
                if (best < 0.5) { closeFeatureDialog(false); out = "No flat face faces the extrude."; return true; }
            }
            dlg_.extent = 1;
            dlg_.to.feature = bit->second.featureId;
            dlg_.to.slot = doc_.bodySlot(bids[0]);
            dlg_.to.face = (uint32_t)face;
            dlg_.to.faceTotal = kernel::faceCount(pit->second.shape);
            dlg_.toName = bit->second.name;
        }
        applyDialogValues();
        std::string err = closedFeature(revolve ? "Revolve" : "Extrude");
        out = err.empty() ? (revolve ? "Revolved the profile." : "Extruded the profile.") : err;
        return true;
    }

    if (name == "rib" || name == "web") {
        bool web = name == "web";
        if (sk_.active) finishSketch();
        uint32_t sid = (uint32_t)a["sketch_id"].asNumber(0);
        if (!sid || !doc_.sketches.count(sid)) { out = "Pick an open sketch."; return true; }
        double thick = num(a["thickness"], 0);
        if (!evalErr.empty()) { out = evalErr; return true; }
        if (!(thick > 0)) { out = "Thickness must be greater than zero."; return true; }
        std::string err;
        auto refs = refsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        startToolFeature(web ? FeatureType::Web : FeatureType::Rib);
        if (!tdlg_.open) { out = web ? "Could not start the web." : "Could not start the rib."; return true; }
        tdlg_.selA = (int)sid;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%g", thick);
        tdlg_.flag = a["flip"].asBool(false);
        tdlg_.option = a["side"].asString() == "one" ? 1 : 0;
        tdlg_.targets = refs;
        applyToolDialog();
        err = closedTool(web ? "Web" : "Rib");
        out = err.empty() ? (web ? "Added the web." : "Added the rib.") : err;
        return true;
    }

    if (name == "emboss") {
        if (sk_.active) finishSketch();
        uint32_t sid = (uint32_t)a["sketch_id"].asNumber(0);
        if (!sid || !doc_.sketches.count(sid)) { out = "Pick a closed sketch."; return true; }
        double depth = num(a["depth"], 0);
        if (!evalErr.empty()) { out = evalErr; return true; }
        if (!(depth > 0)) { out = "Depth must be greater than zero."; return true; }
        std::string err;
        auto refs = refsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        startToolFeature(FeatureType::Emboss);
        if (!tdlg_.open) { out = "Could not start the emboss."; return true; }
        tdlg_.selA = (int)sid;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%g", depth);
        tdlg_.flag = a["flip"].asBool(false);
        tdlg_.option = a["deboss"].asBool(false) ? 1 : 0;
        tdlg_.targets = refs;
        applyToolDialog();
        err = closedTool("Emboss");
        out = err.empty() ? "Embossed the profile." : err;
        return true;
    }

    if (name == "boss") {
        double dia = num(a["diameter"], 0);
        double height = num(a["height"], 0);
        double hole = a.has("hole") ? num(a["hole"], 0) : 0;
        if (!evalErr.empty()) { out = evalErr; return true; }
        if (!(dia > 0)) { out = "Diameter must be greater than zero."; return true; }
        if (!(height > 0)) { out = "Height must be greater than zero."; return true; }
        if (hole < 0 || hole >= dia) { out = "The hole must be smaller than the boss."; return true; }
        std::string err;
        auto refs = refsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (refs.empty()) { out = "Name the body the boss stands on."; return true; }
        std::string ax = a["axis"].asString("z");
        int axis = ax == "x" || ax == "X" ? 0 : ax == "y" || ax == "Y" ? 1 : 2;
        startToolFeature(FeatureType::Boss);
        if (!tdlg_.open) { out = "Could not start the boss."; return true; }
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%g", num(a["x"], 0));
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%g", num(a["y"], 0));
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%g", num(a["z"], 0));
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%g", dia);
        std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%g", height);
        std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%g", hole);
        tdlg_.option = axis;
        tdlg_.targets = refs;
        applyToolDialog();
        err = closedTool("Boss");
        out = err.empty() ? "Added the boss." : err;
        return true;
    }

    if (name == "snap") {
        double length = num(a["length"], 0);
        double width = num(a["width"], 0);
        double thick = num(a["thickness"], 0);
        double hook = a.has("hook") ? num(a["hook"], 0) : 0;
        double hookLen = a.has("hook_length") ? num(a["hook_length"], 0) : 0;
        if (!evalErr.empty()) { out = evalErr; return true; }
        if (!(length > 0)) { out = "Length must be greater than zero."; return true; }
        if (!(width > 0)) { out = "Width must be greater than zero."; return true; }
        if (!(thick > 0)) { out = "Thickness must be greater than zero."; return true; }
        if (hook < 0) { out = "The catch must be zero or more."; return true; }
        if (hookLen < 0 || hookLen > length) { out = "The catch must be shorter than the beam."; return true; }
        if (hook > 0 && !(hookLen > 0)) { out = "The catch needs a length."; return true; }
        std::string err;
        auto refs = refsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (refs.empty()) { out = "Name the body the snap stands on."; return true; }
        std::string ax = a["axis"].asString("z");
        int axis = ax == "x" || ax == "X" ? 0 : ax == "y" || ax == "Y" ? 1 : 2;
        startToolFeature(FeatureType::Snap);
        if (!tdlg_.open) { out = "Could not start the snap."; return true; }
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%g", num(a["x"], 0));
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%g", num(a["y"], 0));
        std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%g", num(a["z"], 0));
        std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%g", length);
        std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%g", width);
        std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%g", thick);
        std::snprintf(tdlg_.v7, sizeof tdlg_.v7, "%g", hook);
        std::snprintf(tdlg_.v8, sizeof tdlg_.v8, "%g", hookLen);
        tdlg_.option = axis;
        tdlg_.targets = refs;
        applyToolDialog();
        err = closedTool("Snap");
        out = err.empty() ? "Added the snap." : err;
        return true;
    }

    if (name == "rest") {
        if (sk_.active) finishSketch();
        uint32_t sid = (uint32_t)a["sketch_id"].asNumber(0);
        if (!sid || !doc_.sketches.count(sid)) { out = "Pick a closed sketch."; return true; }
        double thick = num(a["thickness"], 0);
        if (!evalErr.empty()) { out = evalErr; return true; }
        if (!(thick > 0)) { out = "Thickness must be greater than zero."; return true; }
        std::string err;
        auto refs = refsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (refs.empty()) { out = "Name the body the rest stands on."; return true; }
        startToolFeature(FeatureType::Rest);
        if (!tdlg_.open) { out = "Could not start the rest."; return true; }
        tdlg_.selA = (int)sid;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%g", thick);
        tdlg_.flag = a["flip"].asBool(false);
        tdlg_.targets = refs;
        applyToolDialog();
        err = closedTool("Rest");
        out = err.empty() ? "Added the rest." : err;
        return true;
    }

    if (name == "lip") {
        if (sk_.active) finishSketch();
        uint32_t sid = (uint32_t)a["sketch_id"].asNumber(0);
        if (!sid || !doc_.sketches.count(sid)) { out = "Pick a closed sketch."; return true; }
        double width = num(a["width"], 0);
        double height = num(a["height"], 0);
        if (!evalErr.empty()) { out = evalErr; return true; }
        if (!(width > 0)) { out = "Width must be greater than zero."; return true; }
        if (!(height > 0)) { out = "Height must be greater than zero."; return true; }
        std::string err;
        auto refs = refsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (refs.empty()) { out = "Name the body the lip stands on."; return true; }
        startToolFeature(FeatureType::Lip);
        if (!tdlg_.open) { out = "Could not start the lip."; return true; }
        tdlg_.selA = (int)sid;
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%g", width);
        std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%g", height);
        tdlg_.flag = a["flip"].asBool(false);
        tdlg_.option = a["groove"].asBool(false) ? 1 : 0;
        tdlg_.targets = refs;
        applyToolDialog();
        err = closedTool("Lip");
        out = err.empty() ? "Added the lip." : err;
        return true;
    }

    if (name == "silhouette") {
        std::string err;
        auto refs = refsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (refs.empty()) { out = "Name the body to split."; return true; }
        std::string ax = a["axis"].asString("z");
        int axis = ax == "x" || ax == "X" ? 0 : ax == "y" || ax == "Y" ? 1 : 2;
        startToolFeature(FeatureType::Silhouette);
        if (!tdlg_.open) { out = "Could not start the silhouette split."; return true; }
        tdlg_.option = axis;
        tdlg_.targets = refs;
        applyToolDialog();
        err = closedTool("Silhouette");
        out = err.empty() ? "Split the body on its silhouette." : err;
        return true;
    }

    if (name == "joint") {
        std::string err;
        auto stay = refsFrom(a["stay"], err);
        if (!err.empty()) { out = err; return true; }
        auto mv = refsFrom(a["move"], err);
        if (!err.empty()) { out = err; return true; }
        if (stay.empty()) { out = "Name the body that stays."; return true; }
        if (mv.empty()) { out = "Name the body that moves."; return true; }
        if (stay[0] == mv[0]) { out = "Pick two different bodies."; return true; }
        startToolFeature(FeatureType::Joint);
        if (!tdlg_.open) { out = "Could not start the joint."; return true; }
        tdlg_.targets = {stay[0]};
        tdlg_.tools = {mv[0]};
        bool pts = a.has("stay_x") || a.has("stay_y") || a.has("stay_z") || a.has("move_x") || a.has("move_y") || a.has("move_z");
        if (pts) {
            std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%g", num(a["stay_x"], 0));
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%g", num(a["stay_y"], 0));
            std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%g", num(a["stay_z"], 0));
            std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%g", num(a["move_x"], 0));
            std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%g", num(a["move_y"], 0));
            std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%g", num(a["move_z"], 0));
        }
        std::string jt = a["type"].asString("rigid");
        bool revolute = jt == "revolute" || jt == "Revolute";
        bool slider = jt == "slider" || jt == "Slider";
        tdlg_.option = revolute ? 1 : slider ? 2 : 0;
        if (revolute || slider) {
            std::string ax = a["axis"].asString("z");
            tdlg_.option2 = ax == "x" || ax == "X" ? 0 : ax == "y" || ax == "Y" ? 1 : 2;
        }
        if (revolute) std::snprintf(tdlg_.v7, sizeof tdlg_.v7, "%g", num(a["angle"], 0));
        if (slider) std::snprintf(tdlg_.v8, sizeof tdlg_.v8, "%g", num(a["distance"], 0));
        if (!evalErr.empty()) { closeToolDialog(false); out = evalErr; return true; }
        applyToolDialog();
        err = closedTool("Joint");
        out = err.empty() ? "Joined the bodies." : err;
        return true;
    }

    if (name == "asbuilt_joint") {
        std::string err;
        auto stay = refsFrom(a["stay"], err);
        if (!err.empty()) { out = err; return true; }
        auto mv = refsFrom(a["move"], err);
        if (!err.empty()) { out = err; return true; }
        if (stay.empty()) { out = "Name the first body."; return true; }
        if (mv.empty()) { out = "Name the second body."; return true; }
        if (stay[0] == mv[0]) { out = "Pick two different bodies."; return true; }
        startToolFeature(FeatureType::AsBuilt);
        if (!tdlg_.open) { out = "Could not start the as-built joint."; return true; }
        tdlg_.targets = {stay[0]};
        tdlg_.tools = {mv[0]};
        bool pts = a.has("stay_x") || a.has("stay_y") || a.has("stay_z") || a.has("move_x") || a.has("move_y") || a.has("move_z");
        if (pts) {
            std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%g", num(a["stay_x"], 0));
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%g", num(a["stay_y"], 0));
            std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%g", num(a["stay_z"], 0));
            std::snprintf(tdlg_.v4, sizeof tdlg_.v4, "%g", num(a["move_x"], 0));
            std::snprintf(tdlg_.v5, sizeof tdlg_.v5, "%g", num(a["move_y"], 0));
            std::snprintf(tdlg_.v6, sizeof tdlg_.v6, "%g", num(a["move_z"], 0));
        }
        if (!evalErr.empty()) { closeToolDialog(false); out = evalErr; return true; }
        applyToolDialog();
        err = closedTool("AsBuilt");
        out = err.empty() ? "Recorded the as-built joint." : err;
        return true;
    }

    if (name == "rigid_group") {
        std::string err;
        auto refs = refsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (refs.size() < 2) { out = "Name at least two bodies."; return true; }
        startToolFeature(FeatureType::Rigid);
        if (!tdlg_.open) { out = "Could not start the rigid group."; return true; }
        tdlg_.targets = refs;
        applyToolDialog();
        err = closedTool("Rigid");
        out = err.empty() ? "Locked the bodies together." : err;
        return true;
    }

    if (name == "sweep" || name == "loft") {
        if (sk_.active) finishSketch();
        std::vector<uint32_t> ids;
        if (name == "sweep") {
            ids = {(uint32_t)a["profile_sketch"].asNumber(0), (uint32_t)a["path_sketch"].asNumber(0)};
            runSweep();
        } else {
            for (auto& e : a["sketch_ids"].items())
                if (e.isNumber()) ids.push_back((uint32_t)e.asNumber());
            runLoft();
        }
        if (!tdlg_.open) { out = name == "sweep" ? "Sweep needs a profile sketch and a path sketch." : "Loft needs a profile sketch."; return true; }
        tdlg_.sketchIds = ids;
        tdlg_.option = opOf(a["operation"].asString("new"));
        if (name == "sweep" && a.has("twist")) {
            if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["twist"], "0")) { closeToolDialog(false); out = "Bad twist."; return true; }
        }
        if (name == "sweep" && a.has("thickness")) {
            if (!putExpr(tdlg_.v2, sizeof tdlg_.v2, a["thickness"], "")) { closeToolDialog(false); out = "The thickness expression is too long."; return true; }
            std::string side = low(a["side"].asString("center"));
            if (side == "center") tdlg_.option2 = 0;
            else if (side == "one" || side == "left") tdlg_.option2 = 1;
            else if (side == "other" || side == "right") tdlg_.option2 = 2;
            else { closeToolDialog(false); out = "Wall side is center, one, or other."; return true; }
        }
        if (name == "sweep" && a.has("guide_sketch")) {
            uint32_t g = (uint32_t)a["guide_sketch"].asNumber(0);
            tdlg_.railIds = g ? std::vector<uint32_t>{g} : std::vector<uint32_t>{};
        }
        if (name == "loft") {
            tdlg_.railIds.clear();
            for (auto& e : a["rails"].items())
                if (e.isNumber()) tdlg_.railIds.push_back((uint32_t)e.asNumber());
            if (a.has("point")) {
                glm::dvec3 p;
                if (!read3(a["point"], p)) { closeToolDialog(false); out = "Point is [x, y, z]."; return true; }
                std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", numStr(p.x).c_str());
                std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", numStr(p.y).c_str());
                std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", numStr(p.z).c_str());
                std::string end = low(a["point_end"].asString("end"));
                if (end == "end") tdlg_.option2 = 2;
                else if (end == "start") tdlg_.option2 = 1;
                else { closeToolDialog(false); out = "Point end is start or end."; return true; }
                if (!tdlg_.railIds.empty()) { closeToolDialog(false); out = "A point end does not take rails."; return true; }
                if (tdlg_.selA > 0) { closeToolDialog(false); out = "A point end does not take a centerline."; return true; }
                if (tdlg_.sketchIds.empty()) { closeToolDialog(false); out = "Loft to a point needs a profile."; return true; }
            }
            if (a.has("centerline")) {
                uint32_t c = (uint32_t)a["centerline"].asNumber(0);
                tdlg_.selA = (int)c;
                if (c && !tdlg_.railIds.empty()) { closeToolDialog(false); out = "A centerline does not take rails."; return true; }
                if (c && tdlg_.option2 != 0) { closeToolDialog(false); out = "A point end does not take a centerline."; return true; }
            }
        }
        applyToolDialog();
        std::string base = out;
        std::string err = closedTool(name == "sweep" ? "Sweep" : "Loft");
        out = err.empty() ? (base.empty() ? (name == "sweep" ? "Swept the profile." : "Lofted the profiles.") : base) : err;
        return true;
    }

    if (name == "rotate" || name == "scale" || name == "set_transform") {
        std::string err;
        auto ids = idsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (ids.empty()) { out = "Name the bodies."; return true; }
        selectIds(ids);
        startToolFeature(FeatureType::Move);
        if (!tdlg_.open) { out = "Could not start the transform."; return true; }
        int fi = doc_.featureIndex(tdlg_.featureId);
        auto m = std::make_shared<MoveParams>(*doc_.timeline[fi].move);
        m->xExpr = m->yExpr = m->zExpr = "0";
        m->copy = a["copy"].asBool(false);
        if (name == "set_transform") {
            if (a["matrix"].size() != 16) { closeToolDialog(false); out = "matrix needs 16 numbers, row major."; return true; }
            m->hasMat = true;
            for (int i = 0; i < 16; ++i) m->mat[i] = a["matrix"][(size_t)i].asNumber();
        } else if (name == "scale") {
            double f = a.has("factor") ? num(a["factor"], 1) : 0;
            double sx = a.has("x") ? num(a["x"], 1) : (f ? f : 1);
            double sy = a.has("y") ? num(a["y"], 1) : (f ? f : 1);
            double sz = a.has("z") ? num(a["z"], 1) : (f ? f : 1);
            if (!(sx > 0) || !(sy > 0) || !(sz > 0)) { closeToolDialog(false); out = "Scale factors must be above 0."; return true; }
            m->op = 1;
            tdlg_.option = 1;
            std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", numStr(sx).c_str());
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "%s", numStr(sy).c_str());
            std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "%s", numStr(sz).c_str());
        } else {
            glm::dvec3 ax = axisVec(a["axis"], {0, 0, 1});
            m->angExpr = a["angle"].isString() ? a["angle"].asString() : numStr(num(a["angle"], 0));
            m->axExpr = numStr(ax.x); m->ayExpr = numStr(ax.y); m->azExpr = numStr(ax.z);
        }
        if (a["pivot"].isArray()) {
            glm::dvec3 p;
            if (read3(a["pivot"], p)) { m->pxExpr = numStr(p.x); m->pyExpr = numStr(p.y); m->pzExpr = numStr(p.z); }
        }
        if (!evalErr.empty()) { closeToolDialog(false); out = evalErr; return true; }
        doc_.timeline[fi].move = m;
        tdlg_.option2 = m->copy ? 1 : 0;
        if (name != "scale") {
            tdlg_.option = 0;
            std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "0");
            std::snprintf(tdlg_.v2, sizeof tdlg_.v2, "0");
            std::snprintf(tdlg_.v3, sizeof tdlg_.v3, "0");
        }
        applyToolDialog();
        std::string cerr = closedTool(name.c_str());
        out = cerr.empty() ? (name == "rotate" ? "Rotated." : name == "scale" ? "Scaled." : "Placed.") : cerr;
        return true;
    }

    if (name == "align") {
        std::string err;
        auto src = idsFrom(a["source_body"], err);
        auto dst = idsFrom(a["target_body"], err);
        if (src.empty() || dst.empty()) { out = err.empty() ? "Name the source body and the target body." : err; return true; }
        selectIds(src);
        startToolFeature(FeatureType::Align);
        if (!tdlg_.open) { out = "Could not start Align."; return true; }
        auto kindOf = [](const std::string& s) {
            std::string t = low(s);
            if (t == "edge") return 1;
            if (t == "corner" || t == "vertex") return 2;
            return 0;
        };
        auto fill = [&](AlignParams::Pick& pk, uint32_t body, const std::string& kind, int index, int end) {
            pk.kind = kindOf(kind);
            pk.ref = refOfBody(body);
            pk.sub = (uint32_t)std::max(0, index);
            pk.end = end;
            auto bit = doc_.bodies.find(body);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            if (pit != doc_.parts.end() && pit->second.shape)
                pk.total = pk.kind == 0 ? kernel::faceCount(pit->second.shape) : kernel::edgeCount(pit->second.shape);
        };
        fill(tdlg_.align.from, src[0], a["source_kind"].asString("face"), (int)a["source_index"].asNumber(0), (int)a["source_end"].asNumber(1));
        fill(tdlg_.align.to, dst[0], a["target_kind"].asString("face"), (int)a["target_index"].asNumber(0), (int)a["target_end"].asNumber(1));
        tdlg_.align.flip = a["flip"].asBool(false);
        tdlg_.align.targets = {tdlg_.align.from.ref};
        tdlg_.align.fromName = doc_.bodies[src[0]].name;
        tdlg_.align.toName = doc_.bodies[dst[0]].name;
        applyToolDialog();
        std::string cerr = closedTool("Align");
        out = cerr.empty() ? "Aligned." : cerr;
        return true;
    }

    if (name == "combine") {
        std::string err;
        auto target = idsFrom(a["target"], err);
        auto tools = idsFrom(a["tools"], err);
        if (!err.empty()) { out = err; return true; }
        if (target.empty() || tools.empty()) { out = "Name the target and at least one tool body."; return true; }
        clearSelection();
        selectBodies(target, false);
        selectBodies(tools, true);
        startToolFeature(FeatureType::Combine);
        if (!tdlg_.open) { out = "Could not start Combine."; return true; }
        tdlg_.option = std::max(1, opOf(a["operation"].asString("join")));
        tdlg_.option2 = a["keep_tools"].asBool(false) ? 1 : 0;
        applyToolDialog();
        std::string cerr = closedTool("Combine");
        out = cerr.empty() ? "Combined." : cerr;
        return true;
    }

    if (name == "pattern_on_path") {
        std::string err;
        auto ids = idsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (ids.empty()) { out = "Name the bodies."; return true; }
        bool byEdge = a.has("edge_body") || a.has("edge");
        uint32_t sid = (uint32_t)a["sketch_id"].asNumber(0);
        TargetRef edgeRef{0, 0};
        uint32_t edgeIx = 0;
        if (byEdge) {
            auto eids = idsFrom(a["edge_body"], err);
            if (!err.empty()) { out = err; return true; }
            if (eids.empty()) { out = "Name the body the edge is on."; return true; }
            if (!a.has("edge")) { out = "Pass the edge id from get_body."; return true; }
            edgeRef = refOfBody(eids[0]);
            edgeIx = (uint32_t)a["edge"].asNumber(0);
            if (!edgeRef.first) { out = "That body has no feature."; return true; }
        } else if (!doc_.sketches.count(sid)) {
            out = "No sketch with that id. Pass sketch_id, or edge_body and edge.";
            return true;
        }
        selectIds(ids);
        startToolFeature(FeatureType::Pattern);
        if (!tdlg_.open) { out = "Could not start Pattern on Path."; return true; }
        tdlg_.option2 = 2;
        if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["count"], "3")) { closeToolDialog(false); out = "Bad count."; return true; }
        if (!putExpr(tdlg_.v2, sizeof tdlg_.v2, a["distance"], "20")) { closeToolDialog(false); out = "Bad distance."; return true; }
        std::string how = low(a["distance_type"].asString("spacing"));
        tdlg_.option = (how == "extent" || how == "total") ? 1 : 0;
        std::string ori = low(a["orientation"].asString("identical"));
        tdlg_.option3 = (ori == "path" || ori == "along" || ori == "follow") ? 1 : 0;
        tdlg_.flag = a["flip"].asBool(false);
        tdlg_.pickWait = false;
        if (byEdge) {
            tdlg_.selA = tdlg_.selB = 0;
            tdlg_.pathBody = edgeRef;
            tdlg_.pathEdge = edgeIx;
            tdlg_.axisName = "Edge";
        } else {
            tdlg_.selA = (int)sid;
            tdlg_.selB = a.has("entity") ? (int)a["entity"].asNumber(0) : 0;
            tdlg_.pathBody = {0, 0};
            tdlg_.pathEdge = 0;
            tdlg_.axisName = "Sketch curve";
        }
        applyToolDialog();
        std::string cerr = closedTool("Pattern on Path");
        out = cerr.empty() ? "Pattern added." : cerr;
        return true;
    }

    if (name == "thicken") {
        std::string err;
        auto ids = idsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (ids.empty()) { out = "Name the body."; return true; }
        selectIds(ids);
        startToolFeature(FeatureType::Thicken);
        if (!tdlg_.open) { out = "Could not start Thicken."; return true; }
        if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["thickness"], "2")) { closeToolDialog(false); out = "Bad thickness."; return true; }
        std::string side = low(a["side"].asString("one"));
        tdlg_.option2 = (side == "other" || side == "in" || side == "inside") ? 1
                      : (side == "both" || side == "center" || side == "centered") ? 2 : 0;
        selFaces_.clear();
        bool any = false;
        if (a["faces"].isArray()) {
            for (auto& f : a["faces"].items())
                if (f.isNumber()) { selFaces_.insert({ids[0], (uint32_t)f.asNumber()}); any = true; }
        }
        if (!any && a.has("at")) {
            glm::dvec3 at;
            if (!read3(a["at"], at)) { closeToolDialog(false); out = "at is [x,y,z]."; return true; }
            auto bit = doc_.bodies.find(ids[0]);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            int face = (bit != doc_.bodies.end() && pit != doc_.parts.end() && pit->second.shape)
                           ? kernel::faceNear(pit->second.shape, bit->second.xform, at, 1.0) : -1;
            if (face < 0) { closeToolDialog(false); out = "That point is not on the body."; return true; }
            selFaces_.insert({ids[0], (uint32_t)face});
            any = true;
        }
        if (!any) { closeToolDialog(false); out = "Pass faces, or at, so Thicken knows which face."; return true; }
        syncToolFromSelection();
        applyToolDialog();
        std::string cerr = closedTool("Thicken");
        out = cerr.empty() ? "Thickened." : cerr;
        return true;
    }

    if (name == "circular_pattern" || name == "rect_pattern" || name == "mirror" || name == "shell" || name == "split_body") {
        std::string err;
        auto ids = idsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (ids.empty()) { out = "Name the bodies."; return true; }
        selectIds(ids);
        FeatureType ty = name == "mirror" ? FeatureType::Mirror : name == "shell" ? FeatureType::Shell
                         : name == "split_body" ? FeatureType::Split : FeatureType::Pattern;
        startToolFeature(ty);
        if (!tdlg_.open) { out = "Could not start " + name + "."; return true; }
        if (name == "circular_pattern") {
            tdlg_.option2 = 1;
            putExpr(tdlg_.v1, sizeof tdlg_.v1, a["count"], "4");
            putExpr(tdlg_.v2, sizeof tdlg_.v2, a["angle"], "360");
            glm::dvec3 ax = axisVec(a["axis"], {0, 0, 1});
            tdlg_.option = std::abs(ax.x) > 0.9 ? 0 : std::abs(ax.y) > 0.9 ? 1 : 2;
        } else if (name == "rect_pattern") {
            tdlg_.option2 = 0;
            putExpr(tdlg_.v1, sizeof tdlg_.v1, a["count_x"], "2");
            putExpr(tdlg_.v2, sizeof tdlg_.v2, a["spacing_x"], "20");
            putExpr(tdlg_.v3, sizeof tdlg_.v3, a["count_y"], "1");
            putExpr(tdlg_.v4, sizeof tdlg_.v4, a["spacing_y"], "0");
        } else if (name == "split_body" && a["sketch_id"].isNumber() && a["line"].isNumber()) {
            // Like clicking a sketch line: the cut runs through the line, square to its sketch.
            auto sit = doc_.sketches.find((uint32_t)a["sketch_id"].asNumber());
            const sk::Entity* e = sit != doc_.sketches.end() ? sit->second.find((uint32_t)a["line"].asNumber()) : nullptr;
            if (!e || e->type != sk::EntType::Line) { closeToolDialog(false); out = "That sketch line was not found (use get_sketch for ids)."; return true; }
            const sk::Sketch& s = sit->second;
            glm::dvec3 pa = s.plane.toWorld(s.pt(e->a)), pb = s.plane.toWorld(s.pt(e->b));
            glm::dvec3 nrm = glm::cross(glm::normalize(pb - pa), s.plane.normal());
            tdlg_.option = 3;
            tdlg_.axisO = pa;
            tdlg_.axisD = glm::normalize(nrm);
            tdlg_.axisName = "Sketch line";
            tdlg_.selA = (int)sit->first;
            tdlg_.selB = (int)e->id;
            tdlg_.splitManual = true;
        } else if (name == "split_body" && a["origin"].isArray() && a["normal"].isArray()) {
            // Any plane: through origin with that normal (a face's plane, for one).
            glm::dvec3 o(a["origin"][(size_t)0].asNumber(), a["origin"][(size_t)1].asNumber(), a["origin"][(size_t)2].asNumber());
            glm::dvec3 n(a["normal"][(size_t)0].asNumber(), a["normal"][(size_t)1].asNumber(), a["normal"][(size_t)2].asNumber());
            if (glm::length(n) < 1e-9) { closeToolDialog(false); out = "normal must not be zero."; return true; }
            tdlg_.option = 3;
            tdlg_.axisO = o;
            tdlg_.axisD = glm::normalize(n);
            tdlg_.axisName = "Plane";
            tdlg_.selA = tdlg_.selB = 0;
            tdlg_.splitManual = true;
        } else if (name == "mirror" || name == "split_body") {
            tdlg_.option = planeOf(a["plane"].asString("xy"));
            if (name == "split_body") tdlg_.splitManual = true;
        } else {
            if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["thickness"], "2")) { closeToolDialog(false); out = "Bad thickness."; return true; }
            std::string side = low(a["side"].asString("inside"));
            tdlg_.option2 = side == "outside" ? 1 : (side == "both" || side == "center" || side == "centered") ? 2 : 0;
            if (a["remove_faces"].isArray() && a["remove_faces"].size()) {
                selFaces_.clear();
                for (auto& f : a["remove_faces"].items())
                    if (f.isNumber()) selFaces_.insert({ids[0], (uint32_t)f.asNumber()});
                syncToolFromSelection();
            }
        }
        applyToolDialog();
        std::string base = out;
        std::string cerr = closedTool(name.c_str());
        out = cerr.empty() ? (base.empty() ? "Done." : base + "Done.") : cerr;
        return true;
    }

    if (name == "gear") {
        startToolFeature(FeatureType::Gear);
        if (!tdlg_.open) { out = "Could not start Gear."; return true; }
        std::string t = low(a["type"].asString("spur"));
        int ty = 0;
        if (t == "helical") ty = 1;
        else if (t == "herringbone") ty = 2;
        else if (t == "internal") ty = 3;
        else if (t == "rack") ty = 4;
        else if (t == "bevel") ty = 5;
        else if (t == "worm") ty = 6;
        else if (t == "worm_wheel" || t == "wormwheel") ty = 7;
        gearTypeDefaults(tdlg_.gear, ty);
        tdlg_.gear.plane = planeOf(a["plane"].asString("xy"));
        auto setv = [&](const char* key, const char* arg) {
            if (a.has(arg)) tdlg_.gear.vals[key] = a[arg].isString() ? a[arg].asString() : numStr(num(a[arg], 0));
        };
        setv("module", "module"); setv("teeth", "teeth"); setv("width", "width"); setv("bore", "bore");
        setv("pressure", "pressure_angle"); setv("helix", "helix"); setv("x", "x"); setv("y", "y");
        applyToolDialog();
        std::string cerr = closedTool("Gear");
        out = cerr.empty() ? "Gear added." : cerr;
        return true;
    }

    if (name == "blades") {
        startToolFeature(FeatureType::Blade);
        if (!tdlg_.open) { out = "Could not start Blades."; return true; }
        std::string t = low(a["type"].asString("propeller"));
        int k = 0;
        if (t == "heli" || t == "helicopter" || t == "rotor") k = 1;
        else if (t == "ducted_fan" || t == "edf" || t == "fan") k = 2;
        else if (t == "wind" || t == "turbine") k = 3;
        else if (t == "marine" || t == "boat") k = 4;
        bladeKindDefaults(tdlg_.blade, k);
        if (!a["airfoil"].asString().empty()) tdlg_.blade.airfoil = a["airfoil"].asString();
        auto setv = [&](const char* key, const char* arg) {
            if (a.has(arg)) tdlg_.blade.vals[key] = a[arg].isString() ? a[arg].asString() : numStr(num(a[arg], 0));
        };
        setv("dia", "diameter"); setv("blades", "blade_count"); setv("pitch", "pitch");
        setv("hubDia", "hub_diameter"); setv("hubLen", "hub_length"); setv("bore", "bore"); setv("chord", "chord");
        applyToolDialog();
        std::string cerr = closedTool("Blades");
        out = cerr.empty() ? "Blades added." : cerr;
        return true;
    }

    if (name == "thread") {
        std::string err;
        auto ids = idsFrom(a["body"], err);
        if (ids.empty()) { out = err.empty() ? "Name the body." : err; return true; }
        auto bit = doc_.bodies.find(ids[0]);
        auto pit = doc_.parts.find(bit->second.partId);
        if (pit == doc_.parts.end() || !pit->second.shape) { out = "That body has no solid."; return true; }
        int face = a.has("face") ? (int)a["face"].asNumber(-1) : -1;
        if (face < 0) {
            uint32_t n = kernel::faceCount(pit->second.shape);
            for (uint32_t i = 0; i < n; ++i) {
                double r = 0;
                if (std::strcmp(kernel::faceKind(pit->second.shape, i, r), "cylinder") == 0) { face = (int)i; break; }
            }
        }
        if (face < 0) { out = "No cylindrical face on that body. Pass a face id from get_body."; return true; }
        // Options first, face second: picking the face starts a preview, and a modeled preview of the
        // default thread is a long helical cut that a cosmetic thread would just throw away.
        clearSelection();
        startToolFeature(FeatureType::Thread);
        if (!tdlg_.open) { out = "Could not start Thread."; return true; }
        bool cosmetic = a["cosmetic"].asBool(false);
        tdlg_.thread.modeled = !cosmetic;
        if (a["internal"].asBool(false)) tdlg_.thread.side = 2;
        if (a.has("diameter") || a.has("pitch")) {
            tdlg_.thread.size = -1;
            if (a.has("diameter")) tdlg_.thread.diaExpr = a["diameter"].isString() ? a["diameter"].asString() : numStr(num(a["diameter"], 10));
            if (a.has("pitch")) tdlg_.thread.pitchExpr = a["pitch"].isString() ? a["pitch"].asString() : numStr(num(a["pitch"], 1.5));
        }
        selFaces_.insert({ids[0], (uint32_t)face});
        syncToolFromSelection();
        if (!tdlg_.thread.face.feature) { closeToolDialog(false); out = "That face is not round."; return true; }
        applyToolDialog();
        std::string cerr = closedTool("Thread");
        out = cerr.empty() ? (cosmetic ? "Cosmetic thread added." : "Thread cut added.") : cerr;
        return true;
    }

    if (name == "wing") {
        startToolFeature(FeatureType::Wing);
        if (!tdlg_.open) { out = "Could not start Wing."; return true; }
        if (a.has("root_chord")) tdlg_.wing.vals["rootChord"] = a["root_chord"].isString() ? a["root_chord"].asString() : numStr(num(a["root_chord"], 40));
        if (!tdlg_.wing.panels.empty()) {
            if (a.has("span")) tdlg_.wing.panels[0].span = a["span"].isString() ? a["span"].asString() : numStr(num(a["span"], 80));
            if (a.has("tip_chord")) tdlg_.wing.panels[0].tipChord = a["tip_chord"].isString() ? a["tip_chord"].asString() : numStr(num(a["tip_chord"], 30));
            if (!a["airfoil"].asString().empty()) {
                tdlg_.wing.rootAirfoil = a["airfoil"].asString();
                tdlg_.wing.panels[0].airfoil = a["airfoil"].asString();
            }
        }
        if (a.has("hollow")) tdlg_.wing.hollow = a["hollow"].asBool(false);
        if (a.has("mirror")) tdlg_.wing.mirror = a["mirror"].asBool(false);
        applyToolDialog();
        std::string cerr = closedTool("Wing");
        out = cerr.empty() ? "Wing added." : cerr;
        return true;
    }

    if (name == "dovetail") {
        startToolFeature(FeatureType::Dovetail);
        if (!tdlg_.open) { out = "Could not start Dovetail."; return true; }
        std::string t = low(a["type"].asString("key"));
        int ty = 0;
        if (t == "tab") ty = 1;
        else if (t == "sliding") ty = 2;
        else if (t == "puzzle") ty = 3;
        else if (t == "through") ty = 4;
        else if (t == "half_blind" || t == "halfblind") ty = 5;
        else if (t == "secret" || t == "secret_mitered") ty = 6;
        else if (t == "slide" || t == "machine") ty = 7;
        dovetailTypeDefaults(tdlg_.dovetail, ty);
        auto setv = [&](const char* key, const char* arg) {
            if (a.has(arg)) tdlg_.dovetail.vals[key] = a[arg].isString() ? a[arg].asString() : numStr(num(a[arg], 0));
        };
        setv("length", "length"); setv("width", "width"); setv("depth", "depth");
        setv("angle", "angle"); setv("clearance", "clearance");
        applyToolDialog();
        std::string cerr = closedTool("Dovetail");
        out = cerr.empty() ? "Dovetail added." : cerr;
        return true;
    }

    if (name == "interference") {
        std::string err;
        auto ids = idsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (ids.size() < 2) { out = "Name at least two bodies."; return true; }
        struct Item { std::string name; ShapePtr shape; glm::mat4 xf; };
        std::vector<Item> items;
        for (uint32_t id : ids) {
            auto bit = doc_.bodies.find(id);
            auto pit = doc_.parts.find(bit->second.partId);
            if (pit != doc_.parts.end() && pit->second.shape) items.push_back({bit->second.name, pit->second.shape, bit->second.xform});
        }
        if (items.size() < 2) { out = "Those bodies are not solids."; return true; }
        std::string hits;
        for (size_t i = 0; i < items.size(); ++i)
            for (size_t j = i + 1; j < items.size(); ++j) {
                if (!kernel::shapesTouch(items[i].shape, items[i].xf, items[j].shape, items[j].xf)) continue;
                try {
                    auto common = kernel::applyBool(kernel::BoolOp::Intersect, items[i].shape, {{items[j].shape, items[j].xf}});
                    if (!common.empty() && kernel::shapeDiag(common[0]) > 1e-4)
                        hits += (hits.empty() ? "" : ", ") + items[i].name + "/" + items[j].name;
                } catch (const std::exception&) {}
            }
        out = hits.empty() ? "interference=none" : "interference=" + hits;
        return true;
    }

    if (name == "measure") {
        std::string kind = low(a["kind"].asString("distance"));
        auto grabBody = [&](ShapePtr& shape, glm::mat4& xf) -> bool {
            std::string err;
            auto ids = idsFrom(a["body"].isString() ? a["body"] : a["names"], err);
            if (ids.empty()) { out = err.empty() ? "Name the body." : err; return false; }
            auto bit = doc_.bodies.find(ids[0]);
            auto pit = doc_.parts.find(bit->second.partId);
            if (pit == doc_.parts.end() || !pit->second.shape) { out = "That body has no solid."; return false; }
            shape = pit->second.shape;
            xf = bit->second.xform;
            return true;
        };
        auto none = [](bool has, const json::Value& v) { return has ? (uint32_t)v.asNumber() : 0xffffffffu; };
        if (kind == "angle" && a["points"].size() >= 3) {
            glm::dvec3 p, v, q;
            if (!read3(a["points"][(size_t)0], p) || !read3(a["points"][(size_t)1], v) || !read3(a["points"][(size_t)2], q)) {
                out = "Each point is [x,y,z]. The corner is the middle point.";
                return true;
            }
            glm::dvec3 da = p - v, db = q - v;
            if (glm::length(da) < 1e-9 || glm::length(db) < 1e-9) { out = "The points are on top of each other."; return true; }
            double c = std::clamp(std::abs(glm::dot(glm::normalize(da), glm::normalize(db))), 0.0, 1.0);
            out = "angle_deg=" + numStr(std::acos(c) * 180.0 / kPi);
            return true;
        }
        if (kind == "angle" || kind == "radius" || kind == "area" || kind == "loop") {
            ShapePtr shape;
            glm::mat4 xf(1.f);
            if (!grabBody(shape, xf)) return true;
            uint32_t face = none(a.has("face"), a["face"]);
            uint32_t edge = none(a.has("edge"), a["edge"]);
            uint32_t face2 = none(a.has("face2"), a["face2"]);
            uint32_t edge2 = none(a.has("edge2"), a["edge2"]);
            if (kind == "radius" && face == 0xffffffffu && edge == 0xffffffffu) {
                uint32_t n = kernel::faceCount(shape);
                for (uint32_t i = 0; i < n; ++i) {
                    double r = 0;
                    const char* k = kernel::faceKind(shape, i, r);
                    if (!std::strcmp(k, "cylinder") || !std::strcmp(k, "sphere") || !std::strcmp(k, "torus")) { face = i; break; }
                }
            }
            if ((kind == "area" || kind == "loop") && face == 0xffffffffu && edge == 0xffffffffu) {
                uint32_t n = kernel::faceCount(shape);
                for (uint32_t i = 0; i < n; ++i) {
                    glm::dvec3 c, normal; bool planar = false;
                    if (kernel::faceInfo(shape, i, c, normal, planar) && planar) { face = i; break; }
                }
            }
            kernel::CurveMeasure r;
            if (kind == "angle") r = kernel::measureAngle(shape, xf, face, edge, shape, xf, face2, edge2);
            else if (kind == "radius") r = kernel::measureRadius(shape, xf, face, edge);
            else if (kind == "area") r = kernel::measureArea(shape, xf, face);
            else r = kernel::measureLoop(shape, xf, face, edge);
            if (!r.ok) {
                out = kind == "angle" ? "Those two picks do not make an angle. Use two edges, or two flat faces."
                    : kind == "radius" ? "That is not a circle, a cylinder, a sphere, or a torus."
                    : kind == "area" ? "Click a face to measure its area."
                    : "That pick has no loop.";
                return true;
            }
            const char* key = kind == "angle" ? "angle_deg=" : kind == "radius" ? "radius_mm=" : kind == "area" ? "area_mm2=" : "loop_mm=";
            out = std::string(key) + numStr(r.value);
            return true;
        }
        if (a["points"].size() >= 2) {
            glm::dvec3 p, q;
            if (!read3(a["points"][(size_t)0], p) || !read3(a["points"][(size_t)1], q)) { out = "Each point is [x,y,z]."; return true; }
            out = "distance_mm=" + numStr(glm::length(q - p));
            return true;
        }
        std::string err;
        auto ids = idsFrom(a["names"], err);
        if (ids.size() < 2) { out = err.empty() ? "Give two points or two body names." : err; return true; }
        auto center = [&](uint32_t id) {
            auto bit = doc_.bodies.find(id);
            auto pit = doc_.parts.find(bit->second.partId);
            if (pit != doc_.parts.end() && pit->second.mesh && pit->second.mesh->bounds.valid())
                return glm::dvec3(pit->second.mesh->bounds.transformed(bit->second.xform).center());
            return glm::dvec3(0);
        };
        out = "distance_mm=" + numStr(glm::length(center(ids[1]) - center(ids[0])));
        return true;
    }

    if (name == "offset_plane") {
        runOffsetPlane();
        if (!tdlg_.open) { out = "Could not start the plane."; return true; }
        tdlg_.option = planeOf(a["plane"].asString("xy"));
        if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["offset"], "25")) { closeToolDialog(false); out = "Bad offset."; return true; }
        applyToolDialog();
        uint32_t id = tdlg_.featureId;
        std::string cerr = closedTool("Plane");
        if (!cerr.empty()) { out = cerr; return true; }
        auto it = doc_.constructPlanes.find(id);
        if (it == doc_.constructPlanes.end()) { out = "The plane was not created."; return true; }
        out = "plane_id=" + std::to_string(id) + " origin=" + numStr(it->second.o.x) + "," + numStr(it->second.o.y) + "," + numStr(it->second.o.z);
        return true;
    }

    if (name == "plane_at_angle" || name == "midplane") {
        bool angle = name == "plane_at_angle";
        if (angle) runAnglePlane();
        else runMidPlane();
        if (!tdlg_.open) { out = "Could not start the plane."; return true; }
        tdlg_.option = planeOf(a["plane"].asString("xy"));
        if (angle) {
            tdlg_.option2 = low(a["axis"].asString("x")) == "y" ? 1 : 0;
            if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["angle"], "45")) { closeToolDialog(false); out = "Bad angle."; return true; }
        } else if (a["other_plane"].isNumber()) {
            tdlg_.selA = (int)a["other_plane"].asNumber();
        }
        applyToolDialog();
        uint32_t id = tdlg_.featureId;
        std::string cerr = closedTool("Plane");
        if (!cerr.empty()) { out = cerr; return true; }
        auto it = doc_.constructPlanes.find(id);
        if (it == doc_.constructPlanes.end()) { out = "The plane was not created."; return true; }
        glm::dvec3 n = glm::cross(glm::dvec3(it->second.x), glm::dvec3(it->second.y));
        if (glm::length(n) > 1e-9) n = glm::normalize(n);
        out = "plane_id=" + std::to_string(id)
            + " origin=" + numStr(it->second.o.x) + "," + numStr(it->second.o.y) + "," + numStr(it->second.o.z)
            + " normal=" + numStr(n.x) + "," + numStr(n.y) + "," + numStr(n.z);
        return true;
    }

    if (name == "plane_3pt") {
        runThreePointPlane();
        if (!tdlg_.open) { out = "Could not start the plane."; return true; }
        if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["x1"], "0") || !putExpr(tdlg_.v2, sizeof tdlg_.v2, a["y1"], "0") ||
            !putExpr(tdlg_.v3, sizeof tdlg_.v3, a["z1"], "0") || !putExpr(tdlg_.v4, sizeof tdlg_.v4, a["x2"], "10") ||
            !putExpr(tdlg_.v5, sizeof tdlg_.v5, a["y2"], "0") || !putExpr(tdlg_.v6, sizeof tdlg_.v6, a["z2"], "0") ||
            !putExpr(tdlg_.v7, sizeof tdlg_.v7, a["x3"], "0") || !putExpr(tdlg_.v8, sizeof tdlg_.v8, a["y3"], "10") ||
            !putExpr(tdlg_.v9, sizeof tdlg_.v9, a["z3"], "0")) {
            closeToolDialog(false);
            out = "Bad point.";
            return true;
        }
        applyToolDialog();
        uint32_t id = tdlg_.featureId;
        std::string cerr = closedTool("Plane");
        if (!cerr.empty()) { out = cerr; return true; }
        auto it = doc_.constructPlanes.find(id);
        if (it == doc_.constructPlanes.end()) { out = "Those points lie on one line."; return true; }
        glm::dvec3 n = glm::cross(glm::dvec3(it->second.x), glm::dvec3(it->second.y));
        if (glm::length(n) > 1e-9) n = glm::normalize(n);
        out = "plane_id=" + std::to_string(id)
            + " origin=" + numStr(it->second.o.x) + "," + numStr(it->second.o.y) + "," + numStr(it->second.o.z)
            + " normal=" + numStr(n.x) + "," + numStr(n.y) + "," + numStr(n.z);
        return true;
    }

    if (name == "plane_tangent") {
        runTangentPlane();
        if (!tdlg_.open) { out = "Could not start the plane."; return true; }
        std::string err;
        auto ids = idsFrom(a["body"], err);
        if (ids.empty()) { closeToolDialog(false); out = err.empty() ? "Name the body." : err; return true; }
        auto bit = doc_.bodies.find(ids[0]);
        auto pit = doc_.parts.find(bit->second.partId);
        if (pit == doc_.parts.end() || !pit->second.shape) { closeToolDialog(false); out = "That body has no solid."; return true; }
        TargetRef r = refOfBody(ids[0]);
        if (!r.first) { closeToolDialog(false); out = "That body has no solid."; return true; }
        int face = a.has("face") ? (int)a["face"].asNumber(-1) : -1;
        if (face < 0) {
            uint32_t n = kernel::faceCount(pit->second.shape);
            for (uint32_t i = 0; i < n; ++i) {
                double rad = 0;
                if (std::strcmp(kernel::faceKind(pit->second.shape, i, rad), "cylinder") == 0) { face = (int)i; break; }
            }
        }
        kernel::RoundFace rf;
        if (face < 0 || !kernel::roundFace(pit->second.shape, bit->second.xform, (uint32_t)face, rf)) {
            closeToolDialog(false); out = "That face is not a cylinder."; return true;
        }
        tdlg_.targets = {r};
        tdlg_.selA = face;
        tdlg_.faceTotal = kernel::faceCount(pit->second.shape);
        if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["angle"], "0")) {
            closeToolDialog(false); out = "Bad angle."; return true;
        }
        applyToolDialog();
        uint32_t id = tdlg_.featureId;
        std::string cerr = closedTool("Plane");
        if (!cerr.empty()) { out = cerr; return true; }
        auto it = doc_.constructPlanes.find(id);
        if (it == doc_.constructPlanes.end()) { out = "That face is not a cylinder."; return true; }
        glm::dvec3 n = glm::cross(glm::dvec3(it->second.x), glm::dvec3(it->second.y));
        if (glm::length(n) > 1e-9) n = glm::normalize(n);
        out = "plane_id=" + std::to_string(id)
            + " origin=" + numStr(it->second.o.x) + "," + numStr(it->second.o.y) + "," + numStr(it->second.o.z)
            + " normal=" + numStr(n.x) + "," + numStr(n.y) + "," + numStr(n.z);
        return true;
    }

    if (name == "axis") {
        bool edgeMode = a.has("edge");
        bool cylMode = !edgeMode && a.has("body");
        runAxis();
        if (!tdlg_.open) { out = "Could not start the axis."; return true; }
        if (edgeMode || cylMode) {
            std::string err;
            auto ids = idsFrom(a["body"], err);
            if (ids.empty()) { closeToolDialog(false); out = err.empty() ? "Name the body." : err; return true; }
            auto bit = doc_.bodies.find(ids[0]);
            auto pit = doc_.parts.find(bit->second.partId);
            if (pit == doc_.parts.end() || !pit->second.shape) { closeToolDialog(false); out = "That body has no solid."; return true; }
            TargetRef r = refOfBody(ids[0]);
            if (!r.first) { closeToolDialog(false); out = "That body has no solid."; return true; }
            tdlg_.option = edgeMode ? 1 : 2;
            tdlg_.targets = {r};
            if (edgeMode) {
                int edge = (int)a["edge"].asNumber(-1);
                kernel::EdgeGeom g;
                if (edge < 0 || !kernel::edgeGeom(pit->second.shape, bit->second.xform, (uint32_t)edge, g) || g.type != 0) {
                    closeToolDialog(false); out = "That edge is not straight."; return true;
                }
                tdlg_.selA = edge;
                tdlg_.edgeTotal = kernel::edgeCount(pit->second.shape);
            } else {
                int face = a.has("face") ? (int)a["face"].asNumber(-1) : -1;
                if (face < 0) {
                    uint32_t n = kernel::faceCount(pit->second.shape);
                    for (uint32_t i = 0; i < n; ++i) {
                        double rad = 0;
                        if (std::strcmp(kernel::faceKind(pit->second.shape, i, rad), "cylinder") == 0) { face = (int)i; break; }
                    }
                }
                kernel::RoundFace rf;
                if (face < 0 || !kernel::roundFace(pit->second.shape, bit->second.xform, (uint32_t)face, rf)) {
                    closeToolDialog(false); out = "That face is not round."; return true;
                }
                tdlg_.selA = face;
                tdlg_.faceTotal = kernel::faceCount(pit->second.shape);
            }
        } else if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["x1"], "0") || !putExpr(tdlg_.v2, sizeof tdlg_.v2, a["y1"], "0") ||
            !putExpr(tdlg_.v3, sizeof tdlg_.v3, a["z1"], "0") || !putExpr(tdlg_.v4, sizeof tdlg_.v4, a["x2"], "0") ||
            !putExpr(tdlg_.v5, sizeof tdlg_.v5, a["y2"], "0") || !putExpr(tdlg_.v6, sizeof tdlg_.v6, a["z2"], "40")) {
            closeToolDialog(false); out = "A coordinate expression is too long."; return true;
        }
        applyToolDialog();
        uint32_t id = tdlg_.featureId;
        std::string cerr = closedTool("Axis");
        if (!cerr.empty()) { out = cerr; return true; }
        auto it = doc_.constructAxes.find(id);
        if (it == doc_.constructAxes.end()) { out = "The axis was not created."; return true; }
        out = "axis_id=" + std::to_string(id)
            + " origin=" + numStr(it->second.origin.x) + "," + numStr(it->second.origin.y) + "," + numStr(it->second.origin.z)
            + " direction=" + numStr(it->second.direction.x) + "," + numStr(it->second.direction.y) + "," + numStr(it->second.direction.z);
        return true;
    }

    if (name == "point") {
        bool vertMode = a.has("vertex");
        bool edgeMode = !vertMode && a.has("edge");
        bool faceMode = !edgeMode && !vertMode && a.has("face");
        runPoint();
        if (!tdlg_.open) { out = "Could not start the point."; return true; }
        if (edgeMode || faceMode || vertMode) {
            std::string err;
            auto ids = idsFrom(a["body"], err);
            if (ids.empty()) { closeToolDialog(false); out = err.empty() ? "Name the body." : err; return true; }
            auto bit = doc_.bodies.find(ids[0]);
            auto pit = doc_.parts.find(bit->second.partId);
            if (pit == doc_.parts.end() || !pit->second.shape) { closeToolDialog(false); out = "That body has no solid."; return true; }
            TargetRef r = refOfBody(ids[0]);
            if (!r.first) { closeToolDialog(false); out = "That body has no solid."; return true; }
            tdlg_.option = vertMode ? 3 : edgeMode ? 1 : 2;
            tdlg_.targets = {r};
            if (edgeMode) {
                int edge = (int)a["edge"].asNumber(-1);
                kernel::EdgeGeom g;
                if (edge < 0 || !kernel::edgeGeom(pit->second.shape, bit->second.xform, (uint32_t)edge, g)) {
                    closeToolDialog(false); out = "That edge is missing."; return true;
                }
                tdlg_.selA = edge;
                tdlg_.edgeTotal = kernel::edgeCount(pit->second.shape);
            } else if (vertMode) {
                int vertex = (int)a["vertex"].asNumber(-1);
                glm::dvec3 w;
                if (vertex < 0 || !kernel::vertexPoint(pit->second.shape, bit->second.xform, (uint32_t)vertex, w)) {
                    closeToolDialog(false); out = "That corner is missing."; return true;
                }
                tdlg_.selA = vertex;
                tdlg_.edgeTotal = kernel::vertexCount(pit->second.shape);
            } else {
                int face = (int)a["face"].asNumber(-1);
                glm::dvec3 c, n;
                bool planar = false;
                if (face < 0 || !kernel::faceInfo(pit->second.shape, (uint32_t)face, c, n, planar) || !planar) {
                    closeToolDialog(false); out = "That face is not flat."; return true;
                }
                tdlg_.selA = face;
                tdlg_.faceTotal = kernel::faceCount(pit->second.shape);
            }
        } else if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["x"], "0") || !putExpr(tdlg_.v2, sizeof tdlg_.v2, a["y"], "0") ||
            !putExpr(tdlg_.v3, sizeof tdlg_.v3, a["z"], "0")) {
            closeToolDialog(false); out = "A coordinate expression is too long."; return true;
        }
        applyToolDialog();
        uint32_t id = tdlg_.featureId;
        std::string cerr = closedTool("Point");
        if (!cerr.empty()) { out = cerr; return true; }
        auto it = doc_.constructPoints.find(id);
        if (it == doc_.constructPoints.end()) { out = "The point was not created."; return true; }
        out = "point_id=" + std::to_string(id)
            + " position=" + numStr(it->second.x) + "," + numStr(it->second.y) + "," + numStr(it->second.z);
        return true;
    }

    if (name == "split_face") {
        std::string err;
        auto ids = idsFrom(a["names"], err);
        if (ids.empty()) { out = err.empty() ? "Name the body to split." : err; return true; }
        selectIds(ids);
        startToolFeature(FeatureType::SplitFace);
        if (!tdlg_.open) { out = "Could not start Split Face."; return true; }
        if (a.has("sketch_id")) {
            tdlg_.selA = (int)num(a["sketch_id"], 0);
            if (tdlg_.selA <= 0) { closeToolDialog(false); out = "Pass a sketch_id."; return true; }
        } else {
            tdlg_.option = planeOf(a["plane"].asString("xy"));
            if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["offset"], "0")) { closeToolDialog(false); out = "Bad offset."; return true; }
        }
        applyToolDialog();
        std::string cerr = closedTool("Split Face");
        out = cerr.empty() ? "Split the faces." : cerr;
        return true;
    }

    // Primitives that are a sketch plus an extrude or a revolve.
    if (name == "create_cylinder" || name == "create_tube" || name == "create_wedge" || name == "create_cone" ||
        name == "create_sphere" || name == "create_torus") {
        if (sk_.active) finishSketch();
        glm::dvec3 origin(num(a["x"], 0), num(a["y"], 0), num(a["z"], 0));
        glm::dvec3 axis = axisVec(a["axis"], {0, 0, 1});
        if (!evalErr.empty()) { out = evalErr; return true; }
        bool spin = name == "create_cone" || name == "create_sphere" || name == "create_torus";
        sk::Plane pl = spin ? planeYAlong(axis, origin) : planeNormal(axis, origin);
        createSketchOn(pl, std::string());
        sk::Sketch* s = curSketch();
        if (!s) { out = "Could not start a sketch."; return true; }
        if (name == "create_cylinder" || name == "create_tube") {
            double dia = num(a["diameter"], 0), inner = num(a["inner_diameter"], 0), h = num(a["height"], 0);
            if (!(dia > 0) || !(std::abs(h) > 0)) { finishSketch(); out = "Diameter and height must be above 0."; return true; }
            if (inner < 0 || inner >= dia) { finishSketch(); out = "inner_diameter must be smaller than diameter."; return true; }
            s->addCircle(s->addPoint({0, 0}), dia * 0.5);
            if (inner > 0) s->addCircle(s->addPoint({0, 0}), inner * 0.5);
            s->touch();
            uint32_t sid = s->id;
            finishSketch();
            int which = -1;
            if (inner > 0) {
                const auto& profs = sketchProfiles(*s);
                double best = -1;
                for (size_t i = 0; i < profs.size(); ++i) {
                    if (!profs[i].holes.empty()) { which = (int)i; break; }
                    if (profs[i].area > best) { best = profs[i].area; which = (int)i; }
                }
            }
            pickProfiles(sid, which);
            runExtrude();
            if (!dlg_.open) { out = "Could not extrude the cylinder."; return true; }
            std::snprintf(dlg_.dist, sizeof dlg_.dist, "%s", numStr(h).c_str());
            dlg_.opAuto = false;
            dlg_.op = 0;
            applyDialogValues();
            std::string err = closedFeature("Cylinder");
            out = err.empty() ? (inner > 0 ? "Created a tube." : "Created a cylinder.") : err;
            return true;
        }
        if (name == "create_wedge") {
            double L = num(a["length"], 0), W = num(a["width"], 0), H = num(a["height"], 0);
            if (!(L > 0) || !(W > 0) || !(H > 0)) { finishSketch(); out = "Length, width and height must be above 0."; return true; }
            uint32_t p0 = s->addPoint({0, 0}), p1 = s->addPoint({L, 0}), p2 = s->addPoint({0, W});
            s->addLine(p0, p1); s->addLine(p1, p2); s->addLine(p2, p0);
            s->touch();
            uint32_t sid = s->id;
            finishSketch();
            pickProfiles(sid, 0);
            runExtrude();
            if (!dlg_.open) { out = "Could not extrude the wedge."; return true; }
            std::snprintf(dlg_.dist, sizeof dlg_.dist, "%s", numStr(H).c_str());
            dlg_.opAuto = false;
            dlg_.op = 0;
            applyDialogValues();
            std::string err = closedFeature("Wedge");
            out = err.empty() ? "Created a wedge." : err;
            return true;
        }
        if (name == "create_cone") {
            double rb = num(a["bottom_diameter"], 0) * 0.5, rt = num(a["top_diameter"], 0) * 0.5, h = num(a["height"], 0);
            if (!(rb > 0) || !(h > 0) || rt < 0) { finishSketch(); out = "bottom_diameter and height must be above 0."; return true; }
            uint32_t p0 = s->addPoint({0, 0}), p1 = s->addPoint({rb, 0});
            s->addLine(p0, p1);
            if (rt > 1e-9) {
                uint32_t p2 = s->addPoint({rt, h}), p3 = s->addPoint({0, h});
                s->addLine(p1, p2); s->addLine(p2, p3); s->addLine(p3, p0);
            } else {
                uint32_t p2 = s->addPoint({0, h});
                s->addLine(p1, p2); s->addLine(p2, p0);
            }
        } else if (name == "create_sphere") {
            double r = num(a["diameter"], 0) * 0.5;
            if (!(r > 0)) { finishSketch(); out = "Diameter must be above 0."; return true; }
            uint32_t c = s->addPoint({0, 0}), a0 = s->addPoint({0, -r}), b0 = s->addPoint({0, r});
            s->addArc(c, a0, b0);
            s->addLine(b0, a0);
        } else {
            double R = num(a["major_diameter"], 0) * 0.5, r = num(a["tube_diameter"], 0) * 0.5;
            if (!(R > r) || !(r > 0)) { finishSketch(); out = "major_diameter must be larger than tube_diameter, and both above 0."; return true; }
            s->addCircle(s->addPoint({R, 0}), r);
        }
        s->touch();
        uint32_t sid = s->id;
        finishSketch();
        pickProfiles(sid, -1);
        runRevolve();
        if (!dlg_.open) { out = "Could not revolve that profile."; return true; }
        std::snprintf(dlg_.angle, sizeof dlg_.angle, "360");
        dlg_.axis = 0;
        dlg_.opAuto = false;
        dlg_.op = 0;
        applyDialogValues();
        std::string err = closedFeature("Revolve");
        out = err.empty() ? "Created the shape." : err;
        return true;
    }

    if (name == "create_coil" || name == "pipe") {
        bool coil = name == "create_coil";
        startToolFeature(coil ? FeatureType::Coil : FeatureType::Pipe);
        if (!tdlg_.open) { out = coil ? "Could not start Coil." : "Could not start Pipe."; return true; }
        int fi = doc_.featureIndex(tdlg_.featureId);
        if (fi < 0 || !doc_.timeline[fi].pathSolid) { closeToolDialog(false); out = "The feature did not open."; return true; }
        auto p = std::make_shared<SweepSolidParams>(*doc_.timeline[fi].pathSolid);
        auto expr = [&](const char* key, const char* fallback) {
            if (!a.has(key) || a[key].isNull()) return std::string(fallback);
            if (a[key].isString()) return a[key].asString();
            return numStr(num(a[key], 0));
        };
        if (!evalErr.empty()) { closeToolDialog(false); out = evalErr; return true; }
        p->diaExpr = expr(coil ? "diameter" : "diameter", coil ? "20" : "10");
        p->heightExpr = expr(coil ? "height" : "length", coil ? "30" : "40");
        if (coil) {
            p->turnsExpr = expr("turns", "3");
            p->sectionExpr = expr("section", "2");
            p->leftHand = a["left_hand"].asBool(false);
        } else {
            p->innerExpr = expr("inner_diameter", "0");
            p->pathSketch = a.has("sketch_id") ? (uint32_t)a["sketch_id"].asNumber(0) : 0;
        }
        p->xExpr = expr("x", "0");
        p->yExpr = expr("y", "0");
        p->zExpr = expr("z", "0");
        glm::dvec3 ax = axisVec(a["axis"], {0, 0, 1});
        p->axis = std::abs(ax.x) > 0.9 ? 0 : std::abs(ax.y) > 0.9 ? 1 : 2;
        doc_.timeline[fi].pathSolid = p;
        fillToolDlgFromParams(doc_.timeline[fi]);
        applyToolDialog();
        std::string cerr = closedTool(coil ? "Coil" : "Pipe");
        out = cerr.empty() ? (coil ? "Created a coil." : "Created a pipe.") : cerr;
        return true;
    }

    if (name == "press_pull" || name == "offset_face" || name == "draft") {
        bool draft = name == "draft";
        std::string err;
        auto ids = idsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (ids.empty()) { out = "Name the body."; return true; }
        selectIds(ids);
        startToolFeature(draft ? FeatureType::Draft : FeatureType::PressPull);
        if (!tdlg_.open) { out = "Could not start that tool."; return true; }
        if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a[draft ? "angle" : "distance"], draft ? "5" : "1")) {
            closeToolDialog(false); out = "Bad value."; return true;
        }
        glm::dvec3 dir = axisVec(a["direction"], {0, 0, 1});
        tdlg_.option = std::abs(dir.x) > 0.9 ? 0 : std::abs(dir.y) > 0.9 ? 1 : 2;
        selFaces_.clear();
        auto addFace = [&](uint32_t body, uint32_t face) { selFaces_.insert({body, face}); };
        if (a["faces"].isArray() && a["faces"].size()) {
            for (auto& f : a["faces"].items())
                if (f.isNumber()) addFace(ids[0], (uint32_t)f.asNumber());
        } else {
            for (uint32_t id : ids) {
                auto bit = doc_.bodies.find(id);
                if (bit == doc_.bodies.end()) continue;
                auto pit = doc_.parts.find(bit->second.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::faceCount(pit->second.shape);
                int best = -1;
                double bestDot = -2;
                for (uint32_t i = 0; i < n; ++i) {
                    glm::dvec3 c, normal; bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, i, c, normal, planar) || !planar) continue;
                    glm::dvec3 nw = glm::normalize(glm::dmat3(bit->second.xform) * normal);
                    double d = glm::dot(nw, dir);
                    if (draft) {
                        if (std::abs(d) < 0.3) addFace(id, i);
                    } else if (d > bestDot) {
                        bestDot = d;
                        best = (int)i;
                    }
                }
                if (!draft && best >= 0) addFace(id, (uint32_t)best);
            }
        }
        if (selFaces_.empty()) { closeToolDialog(false); out = draft ? "No side faces to draft." : "No face matched that direction."; return true; }
        syncToolFromSelection();
        applyToolDialog();
        std::string cerr = closedTool(draft ? "Draft" : "Press Pull");
        out = cerr.empty() ? (draft ? "Drafted the faces." : "Moved the faces.") : cerr;
        return true;
    }

    if (name == "replace_face") {
        std::string err;
        auto ids = idsFrom(a["names"], err);
        if (!err.empty()) { out = err; return true; }
        if (ids.empty()) { out = "Name the body."; return true; }
        selectIds(ids);
        startToolFeature(FeatureType::ReplaceFace);
        if (!tdlg_.open) { out = "Could not start Replace Face."; return true; }
        if (!putExpr(tdlg_.v1, sizeof tdlg_.v1, a["offset"], "0")) { closeToolDialog(false); out = "Bad offset."; return true; }
        tdlg_.option = planeOf(a["plane"].asString("xy"));
        glm::dvec3 dir = tdlg_.option == 1 ? glm::dvec3(0, -1, 0) : tdlg_.option == 2 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1);
        selFaces_.clear();
        auto addFace = [&](uint32_t body, uint32_t face) { selFaces_.insert({body, face}); };
        if (a["faces"].isArray() && a["faces"].size()) {
            for (auto& f : a["faces"].items())
                if (f.isNumber()) addFace(ids[0], (uint32_t)f.asNumber());
        } else {
            for (uint32_t id : ids) {
                auto bit = doc_.bodies.find(id);
                if (bit == doc_.bodies.end()) continue;
                auto pit = doc_.parts.find(bit->second.partId);
                if (pit == doc_.parts.end() || !pit->second.shape) continue;
                uint32_t n = kernel::faceCount(pit->second.shape);
                int best = -1;
                double bestDot = -2;
                for (uint32_t i = 0; i < n; ++i) {
                    glm::dvec3 c, normal; bool planar = false;
                    if (!kernel::faceInfo(pit->second.shape, i, c, normal, planar) || !planar) continue;
                    glm::dvec3 nw = glm::normalize(glm::dmat3(bit->second.xform) * normal);
                    double d = glm::dot(nw, dir);
                    if (d > bestDot) { bestDot = d; best = (int)i; }
                }
                if (best >= 0) addFace(id, (uint32_t)best);
            }
        }
        if (selFaces_.empty()) { closeToolDialog(false); out = "No flat face matched that plane."; return true; }
        syncToolFromSelection();
        applyToolDialog();
        std::string cerr = closedTool("Replace Face");
        out = cerr.empty() ? "Replaced the faces." : cerr;
        return true;
    }

    out = "Unknown tool " + name;
    return true;
}

} // namespace kst
