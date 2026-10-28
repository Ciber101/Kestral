// Robot export: URDF and USD for Isaac Sim / Isaac Lab (and ROS). Links are components, joints are kept
// in the document (doc_.robot) and placed in the design's current pose, which becomes the zero position.
// Masses, centers of mass and inertias come from the solids (bodies with a set mass, like bought servos,
// get that mass spread over their volume). Meshes are written per body, in meters, in the link's frame.
#include "app/App.h"
#include "core/Json.h"
#include "core/Log.h"
#include "kernel/Features.h"
#include "platform/Platform.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <tuple>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace kst {
using json::Value;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string lowerStr(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string ident(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += std::isalnum(c) ? (char)c : '_';
    while (o.find("__") != std::string::npos) o.replace(o.find("__"), 2, "_");
    while (!o.empty() && o.back() == '_') o.pop_back();
    if (o.empty()) o = "link";
    if (std::isdigit((unsigned char)o[0])) o = "_" + o;
    return o;
}

std::string f6(double v) {
    char b[48];
    if (std::abs(v) < 1e-15) v = 0;
    std::snprintf(b, sizeof b, "%.9g", v);
    return b;
}

std::string xmlEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

struct MassProps {
    double vol = 0;            // mm3
    glm::dvec3 com{0};         // world mm
    glm::dmat3 cov{0};         // integral of x x^T dV about the world origin, mm5
};

// Volume, center and second moments of a closed triangle mesh (divergence theorem, tetrahedra from the origin).
MassProps meshMass(const std::vector<glm::dvec3>& v, const std::vector<uint32_t>& idx) {
    MassProps m;
    glm::dvec3 s1(0);
    glm::dmat3 c(0);
    double vol = 0;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const glm::dvec3 &a = v[idx[t]], &b = v[idx[t + 1]], &d = v[idx[t + 2]];
        double det = glm::dot(a, glm::cross(b, d));
        vol += det / 6.0;
        s1 += det / 24.0 * (a + b + d);
        glm::dvec3 sum = a + b + d;
        glm::dmat3 o = glm::outerProduct(a, a) + glm::outerProduct(b, b) + glm::outerProduct(d, d) + glm::outerProduct(sum, sum);
        c += o * (det / 120.0);
    }
    if (vol < 0) { vol = -vol; s1 = -s1; c = -c; }   // inside out mesh
    m.vol = vol;
    m.com = vol > 1e-9 ? s1 / vol : glm::dvec3(0);
    m.cov = c;
    return m;
}

// Eigen decomposition of a symmetric 3x3 (Jacobi). Columns of V are the axes.
void eigenSym(glm::dmat3 A, glm::dvec3& w, glm::dmat3& V) {
    V = glm::dmat3(1.0);
    for (int sweep = 0; sweep < 50; ++sweep) {
        double off = A[0][1] * A[0][1] + A[0][2] * A[0][2] + A[1][2] * A[1][2];
        if (off < 1e-30) break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(A[p][q]) < 1e-300) continue;
                double theta = (A[q][q] - A[p][p]) / (2 * A[p][q]);
                double t = (theta >= 0 ? 1 : -1) / (std::abs(theta) + std::sqrt(theta * theta + 1));
                double cs = 1 / std::sqrt(t * t + 1), sn = t * cs;
                glm::dmat3 R(1.0);
                R[p][p] = cs; R[q][q] = cs; R[q][p] = sn; R[p][q] = -sn;   // R[col][row]
                A = glm::transpose(R) * A * R;
                V = V * R;
            }
    }
    w = {A[0][0], A[1][1], A[2][2]};
}

glm::dquat quatFromMat(const glm::dmat3& m) {
    glm::dquat q = glm::quat_cast(m);
    return glm::normalize(q);
}

// Rotation that turns +X onto dir.
glm::dquat xTo(const glm::dvec3& dirIn) {
    glm::dvec3 d = glm::normalize(dirIn);
    glm::dvec3 x(1, 0, 0);
    double c = glm::dot(x, d);
    if (c > 1 - 1e-12) return glm::dquat(1, 0, 0, 0);
    if (c < -1 + 1e-12) return glm::dquat(0, 0, 0, 1);   // 180 about Z
    glm::dvec3 ax = glm::normalize(glm::cross(x, d));
    double ang = std::acos(std::clamp(c, -1.0, 1.0));
    return glm::angleAxis(ang, ax);
}

void writeStl(const std::string& path, const std::vector<glm::dvec3>& v, const std::vector<uint32_t>& idx) {
    std::ofstream f(u8path(path), std::ios::binary);
    char header[80] = "Kestral robot export";
    f.write(header, 80);
    uint32_t n = (uint32_t)(idx.size() / 3);
    f.write((const char*)&n, 4);
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        glm::dvec3 a = v[idx[t]], b = v[idx[t + 1]], c = v[idx[t + 2]];
        glm::dvec3 nn = glm::cross(b - a, c - a);
        double l = glm::length(nn);
        if (l > 0) nn /= l;
        float rec[12] = {(float)nn.x, (float)nn.y, (float)nn.z, (float)a.x, (float)a.y, (float)a.z,
                         (float)b.x, (float)b.y, (float)b.z, (float)c.x, (float)c.y, (float)c.z};
        f.write((const char*)rec, sizeof rec);
        uint16_t attr = 0;
        f.write((const char*)&attr, 2);
    }
}

} // namespace

// ===================================================================== components by path

std::string App::compPath(uint32_t cid) const {
    std::string path;
    for (int guard = 0; cid && cid != doc_.rootId && guard < 64; ++guard) {
        auto it = doc_.components.find(cid);
        if (it == doc_.components.end()) break;
        path = path.empty() ? it->second.name : it->second.name + "/" + path;
        cid = it->second.parentId;
    }
    return path;
}

uint32_t App::compByPath(const std::string& p) const {
    if (p.empty()) return 0;
    std::string want = lowerStr(p);
    uint32_t byName = 0;
    int nameHits = 0;
    for (auto& [cid, c] : doc_.components) {
        if (cid == doc_.rootId) continue;
        if (lowerStr(compPath(cid)) == want) return cid;
        if (lowerStr(c.name) == want) { byName = cid; ++nameHits; }
    }
    return nameHits == 1 ? byName : 0;
}

// ===================================================================== building the robot

struct App::RobotModel {
    struct BodyPart {
        uint32_t body = 0;
        std::string name;
        glm::vec3 color{0.7f};
        std::vector<glm::dvec3> v;   // world mm
        std::vector<uint32_t> idx;
        double massG = 0;
        MassProps mp;
        uint32_t group = 0;          // bought part (component with a set mass) it belongs to, 0 = printed
        std::string groupName;
    };
    struct Link {
        std::string name, path;
        uint32_t comp = 0;
        int parentJoint = -1;      // index in joints, -1 for the base
        glm::dvec3 origin{0};      // link frame origin, world mm (axes stay world aligned)
        std::vector<BodyPart> parts;
        double massG = 0;
        glm::dvec3 com{0};         // world mm
        glm::dmat3 inertia{0};     // about com, g mm2, world axes
    };
    std::vector<Link> links;
    std::vector<RobotJoint> joints;
    std::vector<int> jointParent, jointChild;   // link indices
    std::vector<std::string> warnings;
    std::string error;
    double totalG = 0;
    double lowestZ = 0;     // world mm, lowest point of the robot
    glm::dvec3 com{0};
};

App::RobotModel App::buildRobotModel() const {
    RobotModel m;
    const RobotDef& R = doc_.robot;
    uint32_t base = compByPath(R.base);
    if (!base) { m.error = R.base.empty() ? "Pick the base link (the component the robot hangs from, like the pelvis)." : "No component named " + R.base + "."; return m; }
    std::map<uint32_t, int> linkOf;   // component -> link index
    auto addLink = [&](uint32_t cid) {
        if (linkOf.count(cid)) return linkOf[cid];
        RobotModel::Link L;
        L.comp = cid;
        L.path = compPath(cid);
        L.name = ident(doc_.components.at(cid).name);
        m.links.push_back(L);
        return linkOf[cid] = (int)m.links.size() - 1;
    };
    addLink(base);
    std::set<std::string> jnames;
    for (auto& j : R.joints) {
        uint32_t pc = compByPath(j.parent), cc = compByPath(j.child);
        if (!pc) { m.warnings.push_back("Joint " + j.name + ": no component " + j.parent + " (skipped)."); continue; }
        if (!cc) { m.warnings.push_back("Joint " + j.name + ": no component " + j.child + " (skipped)."); continue; }
        if (pc == cc) { m.warnings.push_back("Joint " + j.name + " joins a component to itself (skipped)."); continue; }
        if (cc == base) { m.warnings.push_back("Joint " + j.name + " makes the base a child (skipped)."); continue; }
        if (linkOf.count(cc) && m.links[linkOf[cc]].parentJoint >= 0) {
            m.warnings.push_back("Joint " + j.name + ": " + j.child + " already has a parent joint (skipped). A URDF is a tree.");
            continue;
        }
        if (glm::length(j.axis) < 1e-9 && j.type != 0) { m.warnings.push_back("Joint " + j.name + " has no axis (skipped)."); continue; }
        int pl = addLink(pc), cl = addLink(cc);
        RobotJoint jj = j;
        if (jj.name.empty()) jj.name = m.links[cl].name + "_joint";
        jj.name = ident(jj.name);
        while (jnames.count(jj.name)) jj.name += "_2";
        jnames.insert(jj.name);
        if (jj.type != 0) jj.axis = glm::normalize(jj.axis);
        m.joints.push_back(jj);
        m.jointParent.push_back(pl);
        m.jointChild.push_back(cl);
        m.links[cl].parentJoint = (int)m.joints.size() - 1;
        m.links[cl].origin = jj.origin;
    }
    // Every link must reach the base.
    std::vector<bool> reach(m.links.size(), false);
    reach[0] = true;
    for (size_t pass = 0; pass < m.links.size() + 1; ++pass)
        for (size_t j = 0; j < m.joints.size(); ++j)
            if (reach[m.jointParent[j]]) reach[m.jointChild[j]] = true;
    for (size_t i = 0; i < m.links.size(); ++i)
        if (!reach[i]) m.warnings.push_back("Link " + m.links[i].path + " does not connect to the base through joints.");
    // Link names unique.
    std::set<std::string> lnames;
    for (auto& L : m.links) {
        std::string n = L.name;
        for (int k = 2; lnames.count(n); ++k) n = L.name + "_" + std::to_string(k);
        L.name = n;
        lnames.insert(n);
    }
    // Which link owns each body: the nearest component above it that is a link.
    auto ownerLink = [&](uint32_t cid) -> int {
        for (int guard = 0; cid && guard < 64; ++guard) {
            if (auto it = linkOf.find(cid); it != linkOf.end()) return it->second;
            auto cit = doc_.components.find(cid);
            if (cit == doc_.components.end() || cid == doc_.rootId) break;
            cid = cit->second.parentId;
        }
        return -1;
    };
    // The set mass that applies to a body: the nearest component above it with a mass, inside its link.
    std::map<uint32_t, double> massComp, massBody;
    for (auto& [p, g] : R.massG) {
        uint32_t c = compByPath(p);
        if (c) { massComp[c] = g; continue; }
        bool found = false;
        for (auto& [bid, b] : doc_.bodies)
            if (doc_.isBodyAlive(b) && lowerStr(aiBodyPath(bid)) == lowerStr(p)) { massBody[bid] = g; found = true; }
        if (!found) m.warnings.push_back("Mass for " + p + ": no such component or body.");
    }
    auto massOwner = [&](uint32_t cid) -> uint32_t {
        for (int guard = 0; cid && guard < 64; ++guard) {
            if (massComp.count(cid)) return cid;
            if (linkOf.count(cid)) return 0;
            auto cit = doc_.components.find(cid);
            if (cit == doc_.components.end() || cid == doc_.rootId) break;
            cid = cit->second.parentId;
        }
        return 0;
    };
    std::map<uint32_t, double> massCompVol;   // component -> volume of the bodies sharing its mass
    int skipped = 0;
    std::vector<std::pair<int, RobotModel::BodyPart>> pending;
    std::vector<uint32_t> pendingOwner;
    for (auto& [id, b] : doc_.bodies) {
        if (!doc_.isBodyAlive(b) || !doc_.isBodyShown(b)) continue;
        int li = ownerLink(b.componentId);
        if (li < 0) { ++skipped; continue; }
        auto pit = doc_.parts.find(b.partId);
        if (pit == doc_.parts.end() || !pit->second.mesh) continue;
        const MeshData& md = *pit->second.mesh;
        RobotModel::BodyPart bp;
        bp.body = id;
        bp.name = ident(b.name);
        bp.color = b.color;
        bp.v.reserve(md.vertexCount());
        glm::dmat4 xf(b.xform);
        for (size_t i = 0; i < md.vertexCount(); ++i)
            bp.v.push_back(glm::dvec3(xf * glm::dvec4(md.verts[i * 6], md.verts[i * 6 + 1], md.verts[i * 6 + 2], 1.0)));
        bp.idx = md.indices;
        bp.mp = meshMass(bp.v, bp.idx);
        uint32_t mo = massOwner(b.componentId);
        if (mo && !massBody.count(id)) massCompVol[mo] += bp.mp.vol;
        pending.push_back({li, std::move(bp)});
        pendingOwner.push_back(mo);
    }
    if (skipped) m.warnings.push_back(std::to_string(skipped) + " shown bodies are outside every link and were left out.");
    for (size_t i = 0; i < pending.size(); ++i) {
        auto& [li, bp] = pending[i];
        uint32_t mo = pendingOwner[i];
        if (auto mb = massBody.find(bp.body); mb != massBody.end()) {
            bp.massG = mb->second;
            bp.group = bp.body + 0x80000000u;   // its own group: merged look, one collision box
            bp.groupName = bp.name;
        } else if (mo) {
            bp.massG = massCompVol[mo] > 0 ? massComp[mo] * bp.mp.vol / massCompVol[mo] : 0;
            // an imported file (bought part) is merged for looks with one collision box; your own parts keep their shapes
            bool imported = doc_.components.count(mo) && doc_.components.at(mo).featureId != 0;
            if (imported) bp.group = mo;
            bp.groupName = ident(doc_.components.count(mo) ? doc_.components.at(mo).name : "part");
        }
        else bp.massG = R.density * bp.mp.vol / 1000.0;
        m.links[li].parts.push_back(std::move(bp));
    }
    // Link mass properties (g, mm, world axes).
    double tot = 0;
    glm::dvec3 tcom(0);
    for (auto& L : m.links) {
        double M = 0;
        glm::dvec3 s(0);
        glm::dmat3 C(0);
        for (auto& p : L.parts) {
            if (p.mp.vol <= 1e-9 || p.massG <= 0) continue;
            double rho = p.massG / p.mp.vol;
            M += p.massG;
            s += p.massG * p.mp.com;
            C += p.mp.cov * rho;
        }
        if (M <= 0) {
            m.warnings.push_back("Link " + L.path + " has no mass (no solid bodies). Given 1 g so simulators accept it.");
            L.massG = 1;
            L.com = L.origin;
            L.inertia = glm::dmat3(1.0) * 1.0;
            continue;
        }
        glm::dvec3 com = s / M;
        glm::dmat3 Cc = C - M * glm::outerProduct(com, com);
        double tr = Cc[0][0] + Cc[1][1] + Cc[2][2];
        L.inertia = glm::dmat3(1.0) * tr - Cc;
        L.massG = M;
        L.com = com;
        tot += M;
        tcom += M * com;
    }
    // Base frame at the base's center of mass.
    m.links[0].origin = m.links[0].com;
    double lz = 1e300;
    for (auto& L : m.links)
        for (auto& p : L.parts)
            for (auto& q : p.v) lz = std::min(lz, q.z);
    m.lowestZ = lz < 1e299 ? lz : 0;
    m.totalG = tot;
    m.com = tot > 0 ? tcom / tot : glm::dvec3(0);
    // Robot frame (ROS, Isaac): x forward, y left, z up. Turn the design about Z so its front becomes +X.
    glm::dvec3 fwd = R.front == 0 ? glm::dvec3(1, 0, 0) : R.front == 1 ? glm::dvec3(-1, 0, 0) : R.front == 2 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, -1, 0);
    glm::dvec3 left = glm::cross(glm::dvec3(0, 0, 1), fwd);
    glm::dmat3 Rw = glm::transpose(glm::dmat3(fwd, left, glm::dvec3(0, 0, 1)));   // world -> robot
    auto tv = [&](glm::dvec3& v) { v = Rw * v; };
    for (auto& L : m.links) {
        tv(L.origin);
        tv(L.com);
        L.inertia = Rw * L.inertia * glm::transpose(Rw);
        for (auto& p : L.parts) for (auto& q : p.v) tv(q);
    }
    for (auto& J : m.joints) { tv(J.origin); tv(J.axis); }
    tv(m.com);
    return m;
}

std::string App::robotSummary(const RobotModel& m) const {
    std::ostringstream o;
    if (!m.error.empty()) return m.error;
    char b[200];
    std::snprintf(b, sizeof b, "%zu links, %zu joints, %.0f g total, center of mass at %.1f, %.1f, %.1f mm (robot frame: x forward, y left).", m.links.size(),
                  m.joints.size(), m.totalG, m.com.x, m.com.y, m.com.z);
    o << b;
    std::snprintf(b, sizeof b, " Base frame (%s) sits %.4f m above the robot's lowest point: spawn it at that height.",
                  m.links[0].name.c_str(), (m.links[0].origin.z - m.lowestZ) / 1000.0);
    o << b;
    for (auto& w : m.warnings) o << "\n" << w;
    return o.str();
}

// ===================================================================== writing

std::string App::exportRobot(const std::string& urdfPathIn, std::string& report) {
    RobotModel m = buildRobotModel();
    if (!m.error.empty()) return m.error;
    std::string urdfPath = urdfPathIn;
    if (lowerStr(pathToU8(u8path(urdfPath).extension())) != ".urdf") urdfPath += ".urdf";
    fs::path dir = u8path(urdfPath).parent_path();
    std::string stem = pathToU8(u8path(urdfPath).stem());
    std::error_code ec;
    fs::path meshDir = dir / (stem + "_meshes");
    fs::create_directories(meshDir, ec);
    if (ec) return "Could not make " + pathToU8(meshDir) + ": " + ec.message();
    std::string robotName = ident(doc_.robot.name.empty() ? doc_.name : doc_.robot.name);
    const double mm = 0.001;
    auto vec = [&](const glm::dvec3& p) { return f6(p.x) + " " + f6(p.y) + " " + f6(p.z); };

    // What gets written per link: printed bodies one by one (mesh collision), bought parts (a component with a
    // set mass, often hundreds of tiny bodies) merged per color for looks with one box for collision.
    struct Unit {
        std::string name;
        glm::vec3 color{0.7f};
        std::vector<glm::dvec3> v;   // link frame, m
        std::vector<uint32_t> idx;
        bool meshCollision = true;
        bool box = false;            // collision box only (no visual)
        glm::dvec3 lo{0}, hi{0};     // box, link frame, m
    };
    std::vector<std::vector<Unit>> units(m.links.size());
    for (size_t li = 0; li < m.links.size(); ++li) {
        auto& L = m.links[li];
        std::map<std::pair<uint32_t, uint32_t>, size_t> merged;   // (group, color) -> unit
        std::map<uint32_t, size_t> boxes;
        auto colKey = [](glm::vec3 c) { return (uint32_t)(c.r * 255) << 16 | (uint32_t)(c.g * 255) << 8 | (uint32_t)(c.b * 255); };
        for (auto& p : L.parts) {
            Unit* un = nullptr;
            if (p.group) {
                auto key = std::make_pair(p.group, colKey(p.color));
                auto it = merged.find(key);
                if (it == merged.end()) {
                    Unit nu;
                    nu.name = p.groupName + "_" + std::to_string(merged.size());
                    nu.color = p.color;
                    nu.meshCollision = false;
                    units[li].push_back(nu);
                    it = merged.emplace(key, units[li].size() - 1).first;
                }
                un = &units[li][it->second];
                auto bt = boxes.find(p.group);
                if (bt == boxes.end()) {
                    Unit b;
                    b.name = p.groupName + "_collision";
                    b.box = true;
                    b.meshCollision = false;
                    b.lo = glm::dvec3(1e30);
                    b.hi = glm::dvec3(-1e30);
                    units[li].push_back(b);
                    bt = boxes.emplace(p.group, units[li].size() - 1).first;
                    un = &units[li][it->second];
                }
                Unit& bx = units[li][bt->second];
                for (auto& q : p.v) {
                    glm::dvec3 w = (q - L.origin) * mm;
                    bx.lo = glm::min(bx.lo, w);
                    bx.hi = glm::max(bx.hi, w);
                }
            } else {
                Unit nu;
                nu.name = p.name;
                nu.color = p.color;
                units[li].push_back(nu);
                un = &units[li].back();
            }
            uint32_t base = (uint32_t)un->v.size();
            for (auto& q : p.v) un->v.push_back((q - L.origin) * mm);
            for (uint32_t i : p.idx) un->idx.push_back(base + i);
        }
        // Bought parts are for looks in the simulator: simplify them (vertex clustering on a 0.6 mm grid).
        for (auto& un : units[li]) {
            if (un.meshCollision || un.box || un.idx.size() < 3000) continue;
            const double cell = 0.0006;   // m
            std::map<std::tuple<long long, long long, long long>, uint32_t> grid;
            std::vector<glm::dvec3> nv;
            std::vector<int> cntv;
            std::vector<uint32_t> remap(un.v.size());
            for (size_t i = 0; i < un.v.size(); ++i) {
                auto key = std::make_tuple((long long)std::floor(un.v[i].x / cell), (long long)std::floor(un.v[i].y / cell), (long long)std::floor(un.v[i].z / cell));
                auto it = grid.find(key);
                if (it == grid.end()) {
                    it = grid.emplace(key, (uint32_t)nv.size()).first;
                    nv.push_back(glm::dvec3(0));
                    cntv.push_back(0);
                }
                nv[it->second] += un.v[i];
                cntv[it->second]++;
                remap[i] = it->second;
            }
            for (size_t i = 0; i < nv.size(); ++i) nv[i] /= (double)cntv[i];
            std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
            std::vector<uint32_t> ni;
            for (size_t t = 0; t + 2 < un.idx.size(); t += 3) {
                uint32_t a = remap[un.idx[t]], b = remap[un.idx[t + 1]], c = remap[un.idx[t + 2]];
                if (a == b || b == c || a == c) continue;
                std::array<uint32_t, 3> k{a, b, c};
                std::sort(k.begin(), k.end());
                if (!seen.insert({k[0], k[1], k[2]}).second) continue;
                ni.push_back(a); ni.push_back(b); ni.push_back(c);
            }
            un.v = std::move(nv);
            un.idx = std::move(ni);
        }
        std::set<std::string> taken;
        for (auto& un : units[li]) {
            std::string n = un.name;
            for (int k = 2; taken.count(n); ++k) n = un.name + "_" + std::to_string(k);
            un.name = n;
            taken.insert(n);
        }
    }

    // ---- URDF
    std::ostringstream u;
    u << "<?xml version=\"1.0\"?>\n<!-- Made by Kestral. Units: meters, kilograms, radians. Zero pose = the design as drawn. -->\n";
    {
        char sp[160];
        std::snprintf(sp, sizeof sp, "<!-- Spawn height: the base link frame is %.4f m above the lowest point (feet on the ground). -->\n",
                      (m.links[0].origin.z - m.lowestZ) / 1000.0);
        u << sp;
    }
    u << "<robot name=\"" << xmlEsc(robotName) << "\">\n";
    std::map<std::string, int> matIds;
    for (auto& L : m.links) {
        u << "  <link name=\"" << L.name << "\">\n";
        glm::dvec3 c = (L.com - L.origin) * mm;
        glm::dmat3 I = L.inertia * 1e-9;   // g mm2 -> kg m2
        u << "    <inertial>\n      <origin xyz=\"" << vec(c) << "\" rpy=\"0 0 0\"/>\n      <mass value=\"" << f6(L.massG / 1000.0) << "\"/>\n";
        u << "      <inertia ixx=\"" << f6(I[0][0]) << "\" ixy=\"" << f6(I[1][0]) << "\" ixz=\"" << f6(I[2][0]) << "\" iyy=\"" << f6(I[1][1])
          << "\" iyz=\"" << f6(I[2][1]) << "\" izz=\"" << f6(I[2][2]) << "\"/>\n    </inertial>\n";
        size_t li = &L - &m.links[0];
        for (auto& un : units[li]) {
            if (un.box) {
                glm::dvec3 c = (un.lo + un.hi) * 0.5, sz = un.hi - un.lo;
                u << "    <collision name=\"" << un.name << "\">\n      <origin xyz=\"" << vec(c) << "\" rpy=\"0 0 0\"/>\n      <geometry><box size=\""
                  << vec(sz) << "\"/></geometry>\n    </collision>\n";
                continue;
            }
            std::string file = L.name + "_" + un.name + ".stl";
            writeStl(pathToU8(meshDir / file), un.v, un.idx);
            char col[64];
            std::snprintf(col, sizeof col, "%.3f %.3f %.3f 1", un.color.r, un.color.g, un.color.b);
            std::string rel = stem + "_meshes/" + file;
            u << "    <visual name=\"" << un.name << "\">\n      <origin xyz=\"0 0 0\" rpy=\"0 0 0\"/>\n      <geometry><mesh filename=\"" << xmlEsc(rel)
              << "\"/></geometry>\n      <material name=\"" << L.name << "_" << un.name << "\"><color rgba=\"" << col << "\"/></material>\n    </visual>\n";
            if (un.meshCollision)
                u << "    <collision name=\"" << un.name << "\">\n      <origin xyz=\"0 0 0\" rpy=\"0 0 0\"/>\n      <geometry><mesh filename=\"" << xmlEsc(rel)
                  << "\"/></geometry>\n    </collision>\n";
        }
        u << "  </link>\n";
    }
    for (size_t j = 0; j < m.joints.size(); ++j) {
        const RobotJoint& J = m.joints[j];
        const auto& P = m.links[m.jointParent[j]];
        const char* type = J.type == 0 ? "fixed" : J.type == 2 ? "continuous" : "revolute";
        u << "  <joint name=\"" << J.name << "\" type=\"" << type << "\">\n";
        u << "    <parent link=\"" << P.name << "\"/>\n    <child link=\"" << m.links[m.jointChild[j]].name << "\"/>\n";
        u << "    <origin xyz=\"" << vec((J.origin - P.origin) * mm) << "\" rpy=\"0 0 0\"/>\n";
        if (J.type != 0) {
            u << "    <axis xyz=\"" << vec(J.axis) << "\"/>\n";
            u << "    <limit effort=\"" << f6(J.effort) << "\" velocity=\"" << f6(J.velocity) << "\"";
            if (J.type == 1) u << " lower=\"" << f6(J.lower * kPi / 180) << "\" upper=\"" << f6(J.upper * kPi / 180) << "\"";
            u << "/>\n    <dynamics damping=\"" << f6(J.damping) << "\" friction=\"0\"/>\n";
        }
        u << "  </joint>\n";
    }
    u << "</robot>\n";
    {
        std::ofstream f(u8path(urdfPath), std::ios::binary);
        f << u.str();
        if (!f) return "Could not write " + urdfPath;
    }

    // ---- USD (ASCII, UsdPhysics)
    std::string usdPath = pathToU8(dir / (stem + ".usda"));
    std::ostringstream d;
    d << "#usda 1.0\n(\n    defaultPrim = \"" << robotName << "\"\n    metersPerUnit = 1\n    kilogramsPerUnit = 1\n    upAxis = \"Z\"\n"
      << "    doc = \"Made by Kestral. Zero pose = the design as drawn.\"\n)\n\n";
    d << "def Xform \"" << robotName << "\" (\n    prepend apiSchemas = [\"PhysicsArticulationRootAPI\", \"PhysxArticulationAPI\"]\n)\n{\n";
    d << "    bool physxArticulation:enabledSelfCollisions = 0\n";
    d << "    int physxArticulation:solverPositionIterationCount = 8\n";
    d << "    int physxArticulation:solverVelocityIterationCount = 4\n";
    // one rubber-like contact material for every collision shape (TPU feet on a floor)
    d << "    def Scope \"Looks\"\n    {\n        def Material \"contact\" (\n            prepend apiSchemas = [\"PhysicsMaterialAPI\"]\n        )\n        {\n"
      << "            float physics:staticFriction = 1.0\n            float physics:dynamicFriction = 0.9\n            float physics:restitution = 0\n        }\n    }\n";
    const std::string matRel = "            rel material:binding:physics = </" + robotName + "/Looks/contact> (\n                bindMaterialAs = \"weakerThanDescendants\"\n            )\n";
    for (auto& L : m.links) {
        glm::dvec3 o = L.origin * mm;
        d << "    def Xform \"" << L.name << "\" (\n        prepend apiSchemas = [\"PhysicsRigidBodyAPI\", \"PhysicsMassAPI\"]\n    )\n    {\n";
        d << "        double3 xformOp:translate = (" << f6(o.x) << ", " << f6(o.y) << ", " << f6(o.z) << ")\n";
        d << "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n";
        glm::dvec3 w;
        glm::dmat3 V;
        eigenSym(L.inertia * 1e-9, w, V);
        if (glm::determinant(V) < 0) { V[2] = -V[2]; }
        glm::dquat q = quatFromMat(V);
        glm::dvec3 c = (L.com - L.origin) * mm;
        d << "        float physics:mass = " << f6(L.massG / 1000.0) << "\n";
        d << "        point3f physics:centerOfMass = (" << f6(c.x) << ", " << f6(c.y) << ", " << f6(c.z) << ")\n";
        d << "        float3 physics:diagonalInertia = (" << f6(std::max(w.x, 1e-12)) << ", " << f6(std::max(w.y, 1e-12)) << ", " << f6(std::max(w.z, 1e-12)) << ")\n";
        d << "        quatf physics:principalAxes = (" << f6(q.w) << ", " << f6(q.x) << ", " << f6(q.y) << ", " << f6(q.z) << ")\n";
        size_t li = &L - &m.links[0];
        for (auto& un : units[li]) {
            if (un.box) {
                glm::dvec3 c = (un.lo + un.hi) * 0.5, h = (un.hi - un.lo) * 0.5;
                d << "        def Cube \"" << un.name << "\" (\n            prepend apiSchemas = [\"PhysicsCollisionAPI\", \"MaterialBindingAPI\"]\n        )\n        {\n" << matRel;
                d << "            double size = 2\n            uniform token purpose = \"guide\"\n";
                d << "            double3 xformOp:translate = (" << f6(c.x) << ", " << f6(c.y) << ", " << f6(c.z) << ")\n";
                d << "            float3 xformOp:scale = (" << f6(std::max(h.x, 1e-4)) << ", " << f6(std::max(h.y, 1e-4)) << ", " << f6(std::max(h.z, 1e-4)) << ")\n";
                d << "            uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:scale\"]\n        }\n";
                continue;
            }
            d << "        def Mesh \"" << un.name << "\"";
            if (un.meshCollision) d << " (\n            prepend apiSchemas = [\"PhysicsCollisionAPI\", \"PhysicsMeshCollisionAPI\", \"MaterialBindingAPI\"]\n        )";
            d << "\n        {\n";
            if (un.meshCollision) d << matRel;
            if (un.meshCollision) d << "            uniform token physics:approximation = \"convexHull\"\n";
            d << "            uniform token subdivisionScheme = \"none\"\n";
            d << "            color3f[] primvars:displayColor = [(" << f6(un.color.r) << ", " << f6(un.color.g) << ", " << f6(un.color.b) << ")]\n";
            d << "            int[] faceVertexCounts = [";
            size_t nt = un.idx.size() / 3;
            for (size_t t = 0; t < nt; ++t) d << (t ? ", 3" : "3");
            d << "]\n            int[] faceVertexIndices = [";
            for (size_t t = 0; t < un.idx.size(); ++t) d << (t ? ", " : "") << un.idx[t];
            d << "]\n            point3f[] points = [";
            for (size_t t = 0; t < un.v.size(); ++t)
                d << (t ? ", (" : "(") << f6(un.v[t].x) << ", " << f6(un.v[t].y) << ", " << f6(un.v[t].z) << ")";
            d << "]\n        }\n";
        }
        d << "    }\n";
    }
    d << "    def Scope \"joints\"\n    {\n";
    for (size_t j = 0; j < m.joints.size(); ++j) {
        const RobotJoint& J = m.joints[j];
        const auto& P = m.links[m.jointParent[j]];
        const auto& C = m.links[m.jointChild[j]];
        glm::dvec3 lp0 = (J.origin - P.origin) * mm, lp1 = (J.origin - C.origin) * mm;
        if (J.type == 0) {
            d << "        def PhysicsFixedJoint \"" << J.name << "\"\n        {\n";
        } else {
            d << "        def PhysicsRevoluteJoint \"" << J.name << "\" (\n            prepend apiSchemas = [\"PhysicsDriveAPI:angular\", \"PhysicsJointStateAPI:angular\"]\n        )\n        {\n";
            d << "            uniform token physics:axis = \"X\"\n";
            if (J.type == 1)
                d << "            float physics:lowerLimit = " << f6(J.lower) << "\n            float physics:upperLimit = " << f6(J.upper) << "\n";
            // USD angular drives are per degree.
            d << "            uniform token drive:angular:physics:type = \"force\"\n";
            d << "            float drive:angular:physics:stiffness = " << f6(J.stiffness * kPi / 180) << "\n";
            d << "            float drive:angular:physics:damping = " << f6(J.damping * kPi / 180) << "\n";
            d << "            float drive:angular:physics:maxForce = " << f6(J.effort) << "\n";
            d << "            float drive:angular:physics:targetPosition = 0\n";
            d << "            float physxJoint:maxJointVelocity = " << f6(J.velocity * 180 / kPi) << "\n";
        }
        glm::dquat r = J.type == 0 ? glm::dquat(1, 0, 0, 0) : xTo(J.axis);
        d << "            rel physics:body0 = </" << robotName << "/" << P.name << ">\n";
        d << "            rel physics:body1 = </" << robotName << "/" << C.name << ">\n";
        d << "            point3f physics:localPos0 = (" << f6(lp0.x) << ", " << f6(lp0.y) << ", " << f6(lp0.z) << ")\n";
        d << "            point3f physics:localPos1 = (" << f6(lp1.x) << ", " << f6(lp1.y) << ", " << f6(lp1.z) << ")\n";
        d << "            quatf physics:localRot0 = (" << f6(r.w) << ", " << f6(r.x) << ", " << f6(r.y) << ", " << f6(r.z) << ")\n";
        d << "            quatf physics:localRot1 = (" << f6(r.w) << ", " << f6(r.x) << ", " << f6(r.y) << ", " << f6(r.z) << ")\n";
        d << "        }\n";
    }
    d << "    }\n}\n";
    {
        std::ofstream f(u8path(usdPath), std::ios::binary);
        f << d.str();
        if (!f) return "Could not write " + usdPath;
    }
    // ---- Isaac Lab articulation config (a starting point: spawn height, joints, drives)
    std::string cfgPath = pathToU8(dir / (stem + "_isaaclab.py"));
    {
        double spawn = (m.links[0].origin.z - m.lowestZ) / 1000.0 + 0.002;
        std::ostringstream c;
        c << "# Isaac Lab articulation for " << robotName << ", written by Kestral. Isaac Lab 2.x names\n"
          << "# (older versions: effort_limit / velocity_limit instead of the *_sim names).\n"
          << "import os\n\nimport isaaclab.sim as sim_utils\nfrom isaaclab.actuators import ImplicitActuatorCfg\nfrom isaaclab.assets import ArticulationCfg\n\n"
          << "HERE = os.path.dirname(os.path.abspath(__file__))\n"
          << "USD_PATH = os.path.join(HERE, \"" << stem << ".usda\")\n"
          << "URDF_PATH = os.path.join(HERE, \"" << stem << ".urdf\")\n\n"
          << "JOINT_NAMES = [\n";
        for (auto& J : m.joints) if (J.type != 0) c << "    \"" << J.name << "\",\n";
        c << "]\n\n";
        c << ident(robotName) << "_CFG = ArticulationCfg(\n"
          << "    spawn=sim_utils.UrdfFileCfg(\n"
          << "        asset_path=URDF_PATH,\n"
          << "        fix_base=False,\n"
          << "        merge_fixed_joints=True,\n"
          << "        activate_contact_sensors=True,\n"
          << "        rigid_props=sim_utils.RigidBodyPropertiesCfg(disable_gravity=False, max_depenetration_velocity=1.0),\n"
          << "        articulation_props=sim_utils.ArticulationRootPropertiesCfg(\n"
          << "            enabled_self_collisions=False, solver_position_iteration_count=8, solver_velocity_iteration_count=4),\n"
          << "        joint_drive=sim_utils.UrdfConverterCfg.JointDriveCfg(\n"
          << "            gains=sim_utils.UrdfConverterCfg.JointDriveCfg.PDGainsCfg(stiffness=20.0, damping=0.5)),\n"
          << "    ),\n";
        char sp[64];
        std::snprintf(sp, sizeof sp, "%.4f", spawn);
        c << "    init_state=ArticulationCfg.InitialStateCfg(pos=(0.0, 0.0, " << sp << "), joint_pos={\".*\": 0.0}),\n"
          << "    soft_joint_pos_limit_factor=0.95,\n"
          << "    actuators={\n";
        // one actuator group per distinct (effort, velocity, stiffness, damping)
        std::map<std::tuple<double, double, double, double>, std::vector<std::string>> groups;
        for (auto& J : m.joints)
            if (J.type != 0) groups[{J.effort, J.velocity, J.stiffness, J.damping}].push_back(J.name);
        int gi = 0;
        for (auto& [k, names] : groups) {
            c << "        \"group" << gi++ << "\": ImplicitActuatorCfg(\n            joint_names_expr=[";
            for (size_t i = 0; i < names.size(); ++i) c << (i ? ", " : "") << "\"" << names[i] << "\"";
            c << "],\n            effort_limit_sim=" << f6(std::get<0>(k)) << ", velocity_limit_sim=" << f6(std::get<1>(k))
              << ", stiffness=" << f6(std::get<2>(k)) << ", damping=" << f6(std::get<3>(k)) << ", armature=0.01),\n";
        }
        c << "    },\n)\n";
        std::ofstream f(u8path(cfgPath), std::ios::binary);
        f << c.str();
    }
    report = robotSummary(m) + "\nWrote " + urdfPath + ", " + usdPath + ", " + cfgPath + " and the meshes in " + pathToU8(meshDir) + ".";
    log::info("Robot export: " + report);
    return {};
}

// ===================================================================== assistant tool

std::string App::robotTool(const std::string& argsJson) {
    Value a = Value::parse(argsJson);
    std::string act = lowerStr(a["action"].asString("show"));
    RobotDef& R = doc_.robot;
    auto vec3 = [](const Value& v, glm::dvec3& out) {
        if (v.isArray() && v.size() == 3) { out = {v[(size_t)0].asNumber(0), v[(size_t)1].asNumber(0), v[(size_t)2].asNumber(0)}; return true; }
        if (v.isString()) {
            std::string s = lowerStr(v.asString());
            double sg = 1;
            if (!s.empty() && s[0] == '-') { sg = -1; s = s.substr(1); }
            if (s == "x") { out = {sg, 0, 0}; return true; }
            if (s == "y") { out = {0, sg, 0}; return true; }
            if (s == "z") { out = {0, 0, sg}; return true; }
        }
        return false;
    };
    auto changed = [&](const char* what) {
        pushUndo(what);
        doc_.touch();
        dirty_ = true;
    };
    if (act == "show") {
        Value d = Value::object();
        d.set("name", R.name);
        d.set("base", R.base);
        d.set("density_g_cm3", R.density);
        Value ms = Value::object();
        for (auto& [p, g] : R.massG) ms.set(p, g);
        d.set("masses_g", ms);
        Value js = Value::array();
        for (auto& j : R.joints) {
            Value o = Value::object();
            o.set("name", j.name);
            o.set("parent", j.parent);
            o.set("child", j.child);
            o.set("type", j.type == 0 ? "fixed" : j.type == 2 ? "continuous" : "revolute");
            char b[160];
            std::snprintf(b, sizeof b, "%.3f, %.3f, %.3f", j.origin.x, j.origin.y, j.origin.z);
            o.set("origin", b);
            std::snprintf(b, sizeof b, "%.4f, %.4f, %.4f", j.axis.x, j.axis.y, j.axis.z);
            o.set("axis", b);
            o.set("lower", j.lower);
            o.set("upper", j.upper);
            o.set("effort", j.effort);
            js.push(o);
        }
        d.set("joints", js);
        RobotModel m = buildRobotModel();
        d.set("check", robotSummary(m));
        if (m.error.empty()) {
            Value ls = Value::array();
            for (auto& L : m.links) {
                Value o = Value::object();
                o.set("link", L.name);
                o.set("component", L.path);
                o.set("mass_g", std::round(L.massG * 10) / 10);
                ls.push(o);
            }
            d.set("links", ls);
        }
        return d.dump();
    }
    if (act == "set") {
        if (a.has("name")) R.name = a["name"].asString();
        if (a.has("base")) {
            std::string b = a["base"].asString();
            if (!b.empty() && !compByPath(b)) return "No component named " + b + ". Use a folder path from get_design.";
            R.base = b.empty() ? b : compPath(compByPath(b));
        }
        if (a.has("density")) R.density = std::max(0.01, a["density"].asNumber(R.density));
        if (a.has("front")) {
            std::string f = lowerStr(a["front"].asString());
            int v = f == "x" || f == "+x" ? 0 : f == "-x" ? 1 : f == "y" || f == "+y" ? 2 : f == "-y" ? 3 : -1;
            if (v < 0) return "front is x, -x, y or -y (the way the robot faces in the design).";
            R.front = v;
        }
        changed("Robot settings");
        return "Robot settings saved. " + robotSummary(buildRobotModel());
    }
    if (act == "mass") {
        std::string c = a["component"].asString();
        uint32_t cid = compByPath(c);
        double g = a["grams"].asNumber(0);
        if (!cid) {
            // a single body (by path) can carry a mass too
            for (auto& [bid, b] : doc_.bodies)
                if (doc_.isBodyAlive(b) && (lowerStr(aiBodyPath(bid)) == lowerStr(c) || lowerStr(b.name) == lowerStr(c))) {
                    std::string bp = aiBodyPath(bid);
                    if (g > 0) R.massG[bp] = g;
                    else R.massG.erase(bp);
                    changed("Robot mass");
                    return g > 0 ? "Mass of " + bp + " set to " + f6(g) + " g." : "Mass of " + bp + " now comes from its volume.";
                }
            return "No component or body named " + c + ".";
        }
        if (g > 0) R.massG[compPath(cid)] = g;
        else R.massG.erase(compPath(cid));
        changed("Robot mass");
        return g > 0 ? "Mass of " + compPath(cid) + " set to " + f6(g) + " g." : "Mass of " + compPath(cid) + " now comes from its volume.";
    }
    if (act == "joint") {
        RobotJoint j;
        std::string nm = a["name"].asString();
        auto it = std::find_if(R.joints.begin(), R.joints.end(), [&](const RobotJoint& x) { return !nm.empty() && lowerStr(x.name) == lowerStr(nm); });
        if (it != R.joints.end()) j = *it;
        j.name = nm;
        if (a.has("parent")) {
            uint32_t c = compByPath(a["parent"].asString());
            if (!c) return "No component named " + a["parent"].asString() + ".";
            j.parent = compPath(c);
        }
        if (a.has("child")) {
            uint32_t c = compByPath(a["child"].asString());
            if (!c) return "No component named " + a["child"].asString() + ".";
            j.child = compPath(c);
        }
        if (j.parent.empty() || j.child.empty()) return "A joint needs parent and child components.";
        if (a.has("type")) {
            std::string t = lowerStr(a["type"].asString());
            j.type = t == "fixed" ? 0 : t == "continuous" ? 2 : 1;
        }
        if (a.has("origin") && !vec3(a["origin"], j.origin)) return "origin is [x, y, z] in mm.";
        if (a.has("axis") && !vec3(a["axis"], j.axis)) return "axis is [dx, dy, dz] or x, y, z (with - for the other way).";
        if (a.has("lower")) j.lower = a["lower"].asNumber(j.lower);
        if (a.has("upper")) j.upper = a["upper"].asNumber(j.upper);
        if (a.has("effort")) j.effort = a["effort"].asNumber(j.effort);
        if (a.has("velocity")) j.velocity = a["velocity"].asNumber(j.velocity);
        if (a.has("stiffness")) j.stiffness = a["stiffness"].asNumber(j.stiffness);
        if (a.has("damping")) j.damping = a["damping"].asNumber(j.damping);
        if (j.lower > j.upper) std::swap(j.lower, j.upper);
        if (j.name.empty()) j.name = doc_.components.count(compByPath(j.child)) ? doc_.components.at(compByPath(j.child)).name + " joint" : "joint";
        if (it != R.joints.end()) *it = j;
        else R.joints.push_back(j);
        if (R.base.empty()) R.base = j.parent;
        changed("Robot joint");
        return "Joint " + j.name + " saved. " + robotSummary(buildRobotModel());
    }
    if (act == "remove_joint") {
        std::string nm = lowerStr(a["name"].asString());
        size_t n0 = R.joints.size();
        R.joints.erase(std::remove_if(R.joints.begin(), R.joints.end(), [&](const RobotJoint& x) { return lowerStr(x.name) == nm; }), R.joints.end());
        if (R.joints.size() == n0) return "No joint named " + a["name"].asString() + ".";
        changed("Robot joint removed");
        return "Removed.";
    }
    if (act == "export") {
        std::string path = a["path"].asString();
        if (path.empty()) {
            std::string dir = platform::designsDir() + "/Robots/" + ident(R.name.empty() ? doc_.name : R.name);
            path = dir + "/" + ident(R.name.empty() ? doc_.name : R.name) + ".urdf";
        }
        std::string report;
        std::string err = exportRobot(path, report);
        return err.empty() ? report : "Export failed: " + err;
    }
    return "action is show, set, mass, joint, remove_joint or export.";
}

// ===================================================================== window

namespace {
struct MassPreset { const char* name; double g; };
const MassPreset kPresets[] = {
    {"Feetech STS3215 servo", 55}, {"Feetech STS3250 servo", 72}, {"Dynamixel XL430", 57}, {"Dynamixel XC330", 23},
    {"MG90S 9 g servo", 13.4}, {"Raspberry Pi 5", 46}, {"Raspberry Pi 4", 46}, {"Jetson Orin Nano dev kit", 176},
    {"ESP32 dev board", 10}, {"Waveshare Bus Servo Adapter (A)", 12}, {"BNO085 IMU board", 3},
    {"Pololu D36V50F5 regulator", 8}, {"3S LiPo 2200 mAh (short)", 140}, {"2S LiPo 2200 mAh", 110}, {"XT60 connector", 4},
};
} // namespace

void App::drawRobotOverlay() {
    // Joint axes in the view while the robot window is open: a line along each axis through its origin.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float S = uiScale_;
    double len = std::max(20.0, sceneBounds_.diag() * 0.04);
    for (size_t i = 0; i < doc_.robot.joints.size(); ++i) {
        const RobotJoint& j = doc_.robot.joints[i];
        glm::dvec3 ax = glm::length(j.axis) > 1e-9 ? glm::normalize(j.axis) : glm::dvec3(1, 0, 0);
        ImVec2 a, b, c;
        if (!worldToScreen(j.origin - ax * len, a) || !worldToScreen(j.origin + ax * len, b) || !worldToScreen(j.origin, c)) continue;
        bool sel = (int)i == robotSel_;
        ImU32 col = j.type == 0 ? IM_COL32(150, 150, 160, 220) : sel ? IM_COL32(255, 200, 40, 255) : IM_COL32(242, 140, 40, 230);
        dl->AddLine(a, b, col, (sel ? 4.f : 2.5f) * S);
        dl->AddCircleFilled(b, 4 * S, col);
        dl->AddCircle(c, 5 * S, col, 0, 2 * S);
        if (sel || robotLabels_) dl->AddText(ImVec2(c.x + 7 * S, c.y - 7 * S), col, j.name.c_str());
    }
}

void App::drawRobotWindow() {
    if (!robotOpen_) return;
    drawRobotOverlay();
    const float S = uiScale_;
    ImGui::SetNextWindowSize(ImVec2(600 * S, 680 * S), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Robot export (URDF / USD)###robotwin", &robotOpen_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    RobotDef& R = doc_.robot;
    // Links are your own components (ones a feature made, like the folders inside an imported file, are not).
    std::vector<std::string> linkComps;
    std::vector<uint32_t> boughtRoots;   // top folder of each imported file
    for (auto& [cid, c] : doc_.components) {
        if (cid == doc_.rootId) continue;
        if (c.featureId == 0) { linkComps.push_back(compPath(cid)); continue; }
        auto pit = doc_.components.find(c.parentId);
        bool top = pit == doc_.components.end() || pit->second.featureId != c.featureId;
        if (top) boughtRoots.push_back(cid);
    }
    std::sort(linkComps.begin(), linkComps.end());
    bool edited = false;
    auto compCombo = [&](const char* id, std::string& val) {
        bool ch = false;
        if (ImGui::BeginCombo(id, val.empty() ? "(pick a component)" : val.c_str())) {
            for (auto& c : linkComps)
                if (ImGui::Selectable(c.c_str(), c == val)) { val = c; ch = true; }
            ImGui::EndCombo();
        }
        return ch;
    };
    auto linkOfBody = [&](uint32_t bid) -> std::string {
        auto bit = doc_.bodies.find(bid);
        if (bit == doc_.bodies.end()) return {};
        uint32_t c = bit->second.componentId;
        for (int g = 0; c && c != doc_.rootId && g < 64; ++g) {
            auto it = doc_.components.find(c);
            if (it == doc_.components.end()) break;
            if (it->second.featureId == 0) return compPath(c);
            c = it->second.parentId;
        }
        return {};
    };
    ImGui::TextWrapped("1. Make each moving part a component (a link). 2. Pick the base link. 3. Add a joint for every servo: "
                       "select a round edge on its shaft or horn and press From selected edge, then pick parent and child. "
                       "4. Give bought parts their real mass. 5. Export. The pose the design is in now is the zero pose.");
    ImGui::Separator();
    ImGui::SetNextItemWidth(220 * S);
    edited |= ImGui::InputText("Robot name", &R.name);
    ImGui::SetNextItemWidth(300 * S);
    edited |= compCombo("Base link", R.base);
    {
        const char* fronts[] = {"+X", "-X", "+Y", "-Y (front view)"};
        ImGui::SetNextItemWidth(160 * S);
        edited |= ImGui::Combo("Robot faces", &R.front, fronts, 4);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The export turns the robot so this way is +X (forward), the ROS and Isaac convention.");
    }
    ImGui::SetNextItemWidth(120 * S);
    edited |= ImGui::InputDouble("Printed density, g/cm3", &R.density, 0, 0, "%.2f");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bodies without a set mass get volume x density. PLA or PETG at about 30%% infill: 0.8 to 0.9.");

    // ---- bought parts
    if (ImGui::CollapsingHeader("Bought parts (imported files)", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (boughtRoots.empty()) ImGui::TextDisabled("No imported parts. Insert the maker's STEP files (servos, boards) to get exact shapes.");
        if (ImGui::BeginTable("##bought", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                              ImVec2(0, std::min(260.f * S, (boughtRoots.size() + 1) * 26.f * S + 8)))) {
            ImGui::TableSetupColumn("Part");
            ImGui::TableSetupColumn("File");
            ImGui::TableSetupColumn("Mass, g", ImGuiTableColumnFlags_WidthFixed, 90 * S);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 150 * S);
            ImGui::TableHeadersRow();
            for (uint32_t cid : boughtRoots) {
                std::string path = compPath(cid);
                std::string file;
                int fi = doc_.featureIndex(doc_.components[cid].featureId);
                if (fi >= 0) {
                    const std::string& ip = doc_.timeline[fi].importPath;
                    size_t cut = ip.find_last_of("/\\");
                    file = cut == std::string::npos ? ip : ip.substr(cut + 1);
                }
                ImGui::PushID((int)cid);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(path.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", file.c_str());
                ImGui::TableNextColumn();
                double g = R.massG.count(path) ? R.massG[path] : 0.0;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputDouble("##g", &g, 0, 0, g > 0 ? "%.1f" : "from volume")) {
                    if (g > 0) R.massG[path] = g; else R.massG.erase(path);
                    edited = true;
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Preset")) ImGui::OpenPopup("##preset");
                if (ImGui::BeginPopup("##preset")) {
                    for (auto& pr : kPresets) {
                        char lab[96];
                        std::snprintf(lab, sizeof lab, "%s  (%g g)", pr.name, pr.g);
                        if (ImGui::MenuItem(lab)) { R.massG[path] = pr.g; edited = true; }
                    }
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                if (!file.empty() && ImGui::SmallButton("Same for all")) {
                    // every imported copy of this file gets this mass
                    double gg = R.massG.count(path) ? R.massG[path] : 0.0;
                    for (uint32_t o : boughtRoots) {
                        int ofi = doc_.featureIndex(doc_.components[o].featureId);
                        if (ofi < 0) continue;
                        const std::string& ip = doc_.timeline[ofi].importPath;
                        if (ip.size() >= file.size() && ip.compare(ip.size() - file.size(), file.size(), file) == 0) {
                            if (gg > 0) R.massG[compPath(o)] = gg; else R.massG.erase(compPath(o));
                        }
                    }
                    edited = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Give every imported %s this mass", file.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        // Other set masses (your own components or single bodies, like a battery drawn as a box)
        std::vector<std::string> others;
        for (auto& [p, g] : R.massG) {
            bool isRoot = false;
            for (uint32_t c : boughtRoots) if (compPath(c) == p) { isRoot = true; break; }
            if (!isRoot) others.push_back(p);
        }
        for (auto& p : others) {
            ImGui::PushID(p.c_str());
            ImGui::SetNextItemWidth(90 * S);
            double g = R.massG[p];
            if (ImGui::InputDouble("##og", &g, 0, 0, "%.1f g")) { if (g > 0) R.massG[p] = g; edited = true; }
            ImGui::SameLine();
            ImGui::TextUnformatted(p.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) { R.massG.erase(p); edited = true; }
            ImGui::PopID();
        }
        if (ImGui::Button("Set mass of selected body or its folder") ) {
            if (selection_.empty()) toast("Select a body first (a battery drawn as a box, a part without a file...).", ToastKind::Warning);
            else {
                uint32_t bid = *selection_.begin();
                robotMassFor_ = aiBodyPath(bid);
                ImGui::OpenPopup("##massfor");
            }
        }
        if (ImGui::BeginPopup("##massfor")) {
            ImGui::TextUnformatted(robotMassFor_.c_str());
            static double gIn = 100;
            ImGui::SetNextItemWidth(100 * S);
            ImGui::InputDouble("grams", &gIn, 0, 0, "%.1f");
            if (ImGui::Button("This body")) { R.massG[robotMassFor_] = gIn; edited = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            size_t slash = robotMassFor_.rfind('/');
            if (slash != std::string::npos && ImGui::Button("Its folder")) { R.massG[robotMassFor_.substr(0, slash)] = gIn; edited = true; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }

    // ---- joints
    if (ImGui::CollapsingHeader("Joints", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto fromEdge = [&](RobotJoint& j) {
            if (selEdges_.empty()) return false;
            auto [bid, e] = *selEdges_.begin();
            auto bit = doc_.bodies.find(bid);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            glm::dvec3 org, dir;
            if (pit == doc_.parts.end() || !pit->second.shape || !kernel::edgeAxis(pit->second.shape, e, org, dir)) return false;
            glm::dmat4 xf(bit->second.xform);
            j.origin = glm::dvec3(xf * glm::dvec4(org, 1.0));
            j.axis = glm::normalize(glm::dmat3(xf) * dir);
            return true;
        };
        if (ImGui::Button("Add joint")) {
            RobotJoint j;
            j.name = "joint" + std::to_string(R.joints.size() + 1);
            j.parent = R.base;
            R.joints.push_back(j);
            robotSel_ = (int)R.joints.size() - 1;
            edited = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add from selected round edge")) {
            RobotJoint j;
            if (!fromEdge(j)) toast("Select a circular edge on a servo shaft or horn first.", ToastKind::Warning);
            else {
                // the servo's own link is the parent (the servo case sits in it)
                j.parent = linkOfBody(selEdges_.begin()->first);
                j.name = "joint" + std::to_string(R.joints.size() + 1);
                R.joints.push_back(j);
                robotSel_ = (int)R.joints.size() - 1;
                edited = true;
                toast("Joint added at the edge's axis. Pick its child link.", ToastKind::Success);
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox("Names in view", &robotLabels_);
        int removeAt = -1;
        for (size_t i = 0; i < R.joints.size(); ++i) {
            RobotJoint& j = R.joints[i];
            ImGui::PushID((int)i);
            bool open = (int)i == robotSel_;
            std::string label = (j.name.empty() ? std::string("joint") : j.name) + "   " + (j.parent.empty() ? "?" : j.parent) + "  >  " +
                                (j.child.empty() ? "?" : j.child);
            if (ImGui::Selectable(label.c_str(), open)) robotSel_ = open ? -1 : (int)i;
            if (open) {
                ImGui::Indent(12 * S);
                ImGui::SetNextItemWidth(220 * S);
                edited |= ImGui::InputText("Name", &j.name);
                ImGui::SetNextItemWidth(300 * S);
                edited |= compCombo("Parent link", j.parent);
                ImGui::SetNextItemWidth(300 * S);
                edited |= compCombo("Child link", j.child);
                const char* types[] = {"Fixed", "Revolute", "Continuous"};
                ImGui::SetNextItemWidth(140 * S);
                edited |= ImGui::Combo("Type", &j.type, types, 3);
                double o[3] = {j.origin.x, j.origin.y, j.origin.z}, ax[3] = {j.axis.x, j.axis.y, j.axis.z};
                ImGui::SetNextItemWidth(300 * S);
                if (ImGui::InputScalarN("Origin, mm", ImGuiDataType_Double, o, 3, nullptr, nullptr, "%.2f")) { j.origin = {o[0], o[1], o[2]}; edited = true; }
                ImGui::SetNextItemWidth(300 * S);
                if (ImGui::InputScalarN("Axis", ImGuiDataType_Double, ax, 3, nullptr, nullptr, "%.3f")) { j.axis = {ax[0], ax[1], ax[2]}; edited = true; }
                if (ImGui::Button("From selected round edge")) {
                    if (fromEdge(j)) edited = true;
                    else toast("Select a circular edge on the shaft or horn first.", ToastKind::Warning);
                }
                ImGui::SameLine();
                if (ImGui::Button("Flip axis")) { j.axis = -j.axis; std::swap(j.lower, j.upper); j.lower = -j.lower; j.upper = -j.upper; edited = true; }
                ImGui::SetNextItemWidth(90 * S);
                edited |= ImGui::InputDouble("Lower, deg", &j.lower, 0, 0, "%.1f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90 * S);
                edited |= ImGui::InputDouble("Upper, deg", &j.upper, 0, 0, "%.1f");
                ImGui::SetNextItemWidth(90 * S);
                edited |= ImGui::InputDouble("Torque, N m", &j.effort, 0, 0, "%.2f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90 * S);
                edited |= ImGui::InputDouble("Speed, rad/s", &j.velocity, 0, 0, "%.2f");
                ImGui::SetNextItemWidth(90 * S);
                edited |= ImGui::InputDouble("Stiffness", &j.stiffness, 0, 0, "%.2f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90 * S);
                edited |= ImGui::InputDouble("Damping", &j.damping, 0, 0, "%.2f");
                if (ImGui::Button("STS3215 12 V values")) { j.effort = 2.94; j.velocity = 4.7; edited = true; }
                ImGui::SameLine();
                if (ImGui::Button("Same values for all joints")) {
                    for (auto& o2 : R.joints) { o2.effort = j.effort; o2.velocity = j.velocity; o2.stiffness = j.stiffness; o2.damping = j.damping; }
                    edited = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Remove")) removeAt = (int)i;
                ImGui::Unindent(12 * S);
                ImGui::Spacing();
            }
            ImGui::PopID();
        }
        if (removeAt >= 0) { R.joints.erase(R.joints.begin() + removeAt); robotSel_ = -1; edited = true; }
    }
    if (edited) {
        doc_.touch();
        dirty_ = true;
        robotCheckT_ = -1;
    }

    // ---- check
    if (ImGui::CollapsingHeader("Check", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Reads every mesh: only when something changed (or on Check), never every frame.
        bool run = ImGui::Button("Check now");
        if (run || (robotCheckT_ < 0 && time_ - robotEditT_ > 0.6)) {
            RobotModel m = buildRobotModel();
            robotCheck_ = robotSummary(m);
            robotLinks_.clear();
            if (m.error.empty())
                for (auto& L : m.links) {
                    char b[200];
                    std::snprintf(b, sizeof b, "%-22s %7.1f g   %zu bodies", L.name.c_str(), L.massG, L.parts.size());
                    robotLinks_.push_back(b);
                }
            robotCheckT_ = time_;
        }
        if (edited) robotEditT_ = time_;
        ImGui::TextWrapped("%s", robotCheck_.c_str());
        if (!robotLinks_.empty() && ImGui::TreeNode("Links")) {
            for (auto& l : robotLinks_) ImGui::TextUnformatted(l.c_str());
            ImGui::TreePop();
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Export URDF + USD + Isaac Lab config...", ImVec2(320 * S, 30 * S))) {
        std::string nm = ident(R.name.empty() ? doc_.name : R.name);
        std::string dir = platform::designsDir() + "/Robots/" + nm;
        std::error_code ec;
        fs::create_directories(u8path(dir), ec);
        std::string p = platform::saveFileDialog("Export robot", "URDF (*.urdf)", "*.urdf", "urdf", nm + ".urdf", dir);
        if (!p.empty()) {
            std::string report, err = exportRobot(p, report);
            if (err.empty()) {
                toast("Robot exported (URDF, USD and Isaac Lab config)", ToastKind::Success);
                platform::revealInExplorer(p);
            } else {
                toast(err, ToastKind::Error);
            }
        }
    }
    ImGui::End();
}

} // namespace kst
