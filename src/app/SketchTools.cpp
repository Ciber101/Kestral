// Sketch MODIFY tools that work on existing curves: sketch fillet, offset, extend, project
// (body edges into the sketch) and the point tool. Fusion's workflow throughout: click what you
// want, type a value in the box by the cursor (or click to place), Enter / click to finish.
#include "app/App.h"
#include "sketch/Text.h"
#include "kernel/Features.h"
#include "sketch/Profiles.h"
#include "core/Curves.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace kst {

using sk::CType;
using sk::Entity;
using sk::EntType;
using sk::Sketch;
using sk::Vec2;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2 * kPi;

double cross2(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
Vec2 perpL(Vec2 v) { return {-v.y, v.x}; }
double wrap2pi(double a) {
    a = std::fmod(a, kTwoPi);
    if (a < 0) a += kTwoPi;
    return a;
}

bool lineLine(Vec2 p0, Vec2 d0, Vec2 p1, Vec2 d1, Vec2& out) {
    double den = cross2(d0, d1);
    if (std::abs(den) < 1e-12) return false;
    double t = cross2(p1 - p0, d1) / den;
    out = p0 + d0 * t;
    return true;
}

// Intersections of the infinite line p + t d with a circle.
int lineCircle(Vec2 p, Vec2 d, Vec2 c, double r, Vec2 out[2]) {
    Vec2 f = p - c;
    double a = glm::dot(d, d), b = 2 * glm::dot(f, d), cc = glm::dot(f, f) - r * r;
    double disc = b * b - 4 * a * cc;
    if (disc < 0 || a < 1e-18) return 0;
    double sq = std::sqrt(disc);
    out[0] = p + d * ((-b - sq) / (2 * a));
    out[1] = p + d * ((-b + sq) / (2 * a));
    return disc < 1e-18 ? 1 : 2;
}

int circleCircle(Vec2 c0, double r0, Vec2 c1, double r1, Vec2 out[2]) {
    Vec2 d = c1 - c0;
    double L = glm::length(d);
    if (L < 1e-12 || L > r0 + r1 + 1e-9 || L < std::abs(r0 - r1) - 1e-9) return 0;
    double a = (r0 * r0 - r1 * r1 + L * L) / (2 * L);
    double h2 = std::max(0.0, r0 * r0 - a * a);
    Vec2 m = c0 + d * (a / L), n = perpL(d / L) * std::sqrt(h2);
    out[0] = m + n;
    out[1] = m - n;
    return 2;
}

// One curve of a chain in travel order.
struct Link {
    uint32_t ent = 0;
    bool rev = false;   // travelled b -> a (lines) / clockwise (arcs)
};

// Connected chain of curves through `start` (Fusion's chain selection). Joints must have exactly
// two curves; construction curves are left out unless the start is construction.
std::vector<Link> chainFrom(const Sketch& s, uint32_t start, bool& closed) {
    closed = false;
    const Entity* e0 = s.find(start);
    if (!e0 || e0->locked || e0->type == EntType::Point) return {};
    if (e0->type == EntType::Circle) {
        closed = true;
        return {{start, false}};
    }
    std::map<uint32_t, std::vector<uint32_t>> at;   // point -> curves using it as an end
    for (auto& [id, e] : s.ents) {
        if (e.locked || e.construction != e0->construction) continue;
        if (e.type == EntType::Line || e.type == EntType::Arc) {
            at[e.a].push_back(id);
            at[e.b].push_back(id);
        }
    }
    auto endOf = [&](const Link& l) { const Entity* e = s.find(l.ent); return l.rev ? e->a : e->b; };
    auto startOf = [&](const Link& l) { const Entity* e = s.find(l.ent); return l.rev ? e->b : e->a; };
    std::vector<Link> chain{{start, false}};
    std::set<uint32_t> used{start};
    // forward
    for (;;) {
        uint32_t p = endOf(chain.back());
        auto& v = at[p];
        if (v.size() != 2) break;
        uint32_t nx = v[0] == chain.back().ent ? v[1] : v[0];
        if (nx == start) { closed = true; break; }
        if (used.count(nx)) break;
        const Entity* e = s.find(nx);
        chain.push_back({nx, e->b == p});
        used.insert(nx);
    }
    if (!closed) {
        for (;;) {
            uint32_t p = startOf(chain.front());
            auto& v = at[p];
            if (v.size() != 2) break;
            uint32_t pv = v[0] == chain.front().ent ? v[1] : v[0];
            if (used.count(pv)) break;
            const Entity* e = s.find(pv);
            chain.insert(chain.begin(), Link{pv, e->a == p});
            used.insert(pv);
        }
    }
    return chain;
}

struct OffSeg {
    bool arc = false, circle = false;
    Vec2 a{0}, b{0};       // travel start / end (offset)
    Vec2 c{0};
    double r = 0;
    uint32_t src = 0;
    bool rev = false;
};

// Offsets a chain by d (positive = left of the travel direction). Neighbouring offset curves are
// trimmed / extended to meet, like Fusion. Returns false if an arc collapses.
bool offsetChain(const Sketch& s, const std::vector<Link>& chain, bool closed, double d, std::vector<OffSeg>& out) {
    out.clear();
    for (auto& l : chain) {
        const Entity* e = s.find(l.ent);
        if (!e) return false;
        OffSeg o;
        o.src = l.ent;
        o.rev = l.rev;
        if (e->type == EntType::Line) {
            Vec2 p0 = s.pt(l.rev ? e->b : e->a), p1 = s.pt(l.rev ? e->a : e->b);
            Vec2 u = p1 - p0;
            double L = glm::length(u);
            if (L < 1e-12) return false;
            Vec2 n = perpL(u / L);
            o.a = p0 + n * d;
            o.b = p1 + n * d;
        } else {
            Sketch::ArcGeom g;
            if (!s.arcGeom(*e, g)) return false;
            o.arc = true;
            o.circle = e->type == EntType::Circle;
            o.c = g.c;
            // CCW travel: left is the centre side.
            o.r = l.rev ? g.r + d : g.r - d;
            if (o.r <= 1e-6) return false;
            Vec2 pa = s.arcPoint(g, 0), pb = s.arcPoint(g, 1);
            Vec2 sa = l.rev ? pb : pa, sb = l.rev ? pa : pb;
            o.a = g.c + glm::normalize(sa - g.c) * o.r;
            o.b = g.c + glm::normalize(sb - g.c) * o.r;
        }
        out.push_back(o);
    }
    if (out.size() == 1 && out[0].circle) return true;
    // Joints
    auto joinPair = [&](OffSeg& A, OffSeg& B, Vec2 orig) {
        if (glm::length(A.b - B.a) < 1e-9) return;
        Vec2 best = (A.b + B.a) * 0.5;
        Vec2 hits[2];
        int n = 0;
        if (!A.arc && !B.arc) {
            if (lineLine(A.a, A.b - A.a, B.a, B.b - B.a, hits[0])) n = 1;
        } else if (!A.arc && B.arc) {
            n = lineCircle(A.a, A.b - A.a, B.c, B.r, hits);
        } else if (A.arc && !B.arc) {
            n = lineCircle(B.a, B.b - B.a, A.c, A.r, hits);
        } else {
            n = circleCircle(A.c, A.r, B.c, B.r, hits);
        }
        double bd = 1e300;
        for (int i = 0; i < n; ++i) {
            double dd = glm::length(hits[i] - orig);
            if (dd < bd) { bd = dd; best = hits[i]; }
        }
        A.b = B.a = best;
    };
    for (size_t i = 0; i + 1 < out.size(); ++i) {
        const Entity* e = s.find(chain[i].ent);
        uint32_t jp = chain[i].rev ? e->a : e->b;
        joinPair(out[i], out[i + 1], s.pt(jp));
    }
    if (closed && out.size() > 1) {
        const Entity* e = s.find(chain.back().ent);
        uint32_t jp = chain.back().rev ? e->a : e->b;
        joinPair(out.back(), out.front(), s.pt(jp));
    }
    return true;
}

// Closest point on an entity, and the left normal of its travel direction there.
bool closestOn(const Sketch& s, const Link& l, Vec2 q, Vec2& p, Vec2& left) {
    const Entity* e = s.find(l.ent);
    if (!e) return false;
    if (e->type == EntType::Line) {
        Vec2 a = s.pt(l.rev ? e->b : e->a), b = s.pt(l.rev ? e->a : e->b), u = b - a;
        double L2 = glm::dot(u, u);
        if (L2 < 1e-18) return false;
        double t = std::clamp(glm::dot(q - a, u) / L2, 0.0, 1.0);
        p = a + u * t;
        left = perpL(u / std::sqrt(L2));
        return true;
    }
    Sketch::ArcGeom g;
    if (!s.arcGeom(*e, g)) return false;
    s.distanceTo(*e, q, &p);
    Vec2 out = glm::normalize(p - g.c);
    left = l.rev ? out : -out;   // CCW travel: left points at the centre
    return true;
}

} // namespace

// ===================================================================== point

void App::sketchPointAt(const SnapInfo& si) {
    Sketch* s = curSketch();
    if (!s) return;
    pushUndo("Point");
    sketchMakePoint(*s, si);
    sketchSolve({}, false);
    sketchFinishOp();
}

void App::sketchCrossingClick() {
    Sketch* s = curSketch();
    if (!s || !sk_.cursorValid) return;
    double tol = 9.0 / std::max(sk_.ppu, 1e-9);
    uint32_t a = 0, b = 0;
    Vec2 at;
    if (!sk::nearestCrossing(*s, sk_.raw, tol, a, b, at)) {
        toast("Click where two curves cross", ToastKind::Warning);
        return;
    }
    if (!addCrossingPoint(*s, a, b, at))
        toast("That point fights the sketch's constraints", ToastKind::Warning);
}

uint32_t App::addCrossingPoint(Sketch& s, uint32_t curveA, uint32_t curveB, Vec2 at) {
    Sketch backup = s;
    pushUndo("Crossing");
    uint32_t p = s.addPoint(at);
    s.addConstraint(CType::PointOnObject, p, curveA);
    s.addConstraint(CType::PointOnObject, p, curveB);
    if (!sketchSolve({}, false)) {
        s = backup;
        undo_.dropLast();
        return 0;
    }
    sketchFinishOp();
    return p;
}

void App::sketchTextAt(const SnapInfo& si) {
    Sketch* s = curSketch();
    if (!s) return;
    if (sk_.hoverEnt) {
        for (auto& n : s->notes) {
            if (std::find(n.ents.begin(), n.ents.end(), sk_.hoverEnt) == n.ents.end()) continue;
            std::snprintf(sk_.textBuf, sizeof sk_.textBuf, "%s", n.text.c_str());
            std::snprintf(sk_.textH, sizeof sk_.textH, "%.6g", n.height);
            std::snprintf(sk_.textFont, sizeof sk_.textFont, "%s", n.font.c_str());
            sk_.textNote = n.id;
            toast("Editing that text. Change it, then click Update.");
            return;
        }
    }
    std::string word = sk_.textBuf;
    if (word.empty()) { toast("Type the text first"); return; }
    double h = 10;
    try { if (sk_.textH[0]) h = peval::eval(sk_.textH, doc_.params); }
    catch (const std::runtime_error& ex) { toast(ex.what(), ToastKind::Warning); return; }
    if (!(h > 0)) { toast("Height must be above zero", ToastKind::Warning); return; }
    pushUndo("Text");
    uint32_t note = 0;
    auto ids = sk::addSketchText(*s, word, si.pos, h, sk_.construction, sk_.textFont, &note);
    if (ids.empty()) { undo_.dropLast(); toast("That text has no outlines", ToastKind::Warning); return; }
    sk_.textNote = note;
    sketchSolve({}, false);
    sketchFinishOp();
}

void App::sketchTextUpdate() {
    Sketch* s = curSketch();
    if (!s || !sk_.textNote) return;
    std::string word = sk_.textBuf;
    std::string font = sk_.textFont;
    if (word.empty()) { toast("Type the text first"); return; }
    if (font.empty()) { toast("A blank font is only for new block letters", ToastKind::Warning); return; }
    double h = 10;
    try { if (sk_.textH[0]) h = peval::eval(sk_.textH, doc_.params); }
    catch (const std::runtime_error& ex) { toast(ex.what(), ToastKind::Warning); return; }
    if (!(h > 0)) { toast("Height must be above zero", ToastKind::Warning); return; }
    pushUndo("Edit text");
    if (!sk::editSketchText(*s, sk_.textNote, word, h, font)) {
        undo_.dropLast();
        toast("Could not update that text", ToastKind::Warning);
        return;
    }
    sketchSolve({}, false);
    sketchFinishOp();
    retargetSketchProfiles(s->id);
    int fi = doc_.featureIndex(s->featureId);
    if (fi >= 0) scheduleRecompute(fi);
}

void App::retargetSketchProfiles(uint32_t sketchId) {
    auto sit = doc_.sketches.find(sketchId);
    if (sit == doc_.sketches.end()) return;
    std::vector<sk::Profile> profs = sk::findProfiles(sit->second);
    if (profs.empty()) return;
    auto inside = [&](sk::Vec2 q) {
        for (auto& p : profs)
            if (sk::pointInProfile(p, q)) return true;
        return false;
    };
    auto fresh = [&] {
        std::vector<sk::Vec2> pts;
        for (auto& p : profs) pts.push_back(profileSamplePoint(p));
        return pts;
    };
    for (auto& f : doc_.timeline) {
        if (f.type == FeatureType::Extrude && f.extrude && f.extrude->sketchId == sketchId && f.extrude->faces.empty()) {
            bool ok = !f.extrude->profilePts.empty();
            for (auto& q : f.extrude->profilePts)
                if (!inside(q)) ok = false;
            if (!ok) {
                auto p = std::make_shared<ExtrudeParams>(*f.extrude);
                p->profilePts = fresh();
                f.extrude = p;
            }
        } else if (f.type == FeatureType::Revolve && f.revolve && f.revolve->sketchId == sketchId) {
            bool ok = !f.revolve->profilePts.empty();
            for (auto& q : f.revolve->profilePts)
                if (!inside(q)) ok = false;
            if (!ok) {
                auto p = std::make_shared<RevolveParams>(*f.revolve);
                p->profilePts = fresh();
                f.revolve = p;
            }
        }
    }
}

// ===================================================================== fillet

void App::sketchFilletAt(uint32_t hoverEnt, Vec2 cursor, double radius) {
    Sketch* s = curSketch();
    if (!s) return;
    const Entity* he = s->find(hoverEnt);
    if (!he) return;
    // The corner: the hovered point, or the end of the hovered line nearest the cursor.
    uint32_t P = 0;
    if (he->type == EntType::Point) P = hoverEnt;
    else if (he->type == EntType::Line)
        P = glm::length(s->pt(he->a) - cursor) < glm::length(s->pt(he->b) - cursor) ? he->a : he->b;
    if (!P) return;
    Vec2 pc = s->pt(P);
    // Two lines often meet at the same spot with two different point ids (an assistant sketch does this).
    auto isCorner = [&](uint32_t id) {
        const Entity* e = s->find(id);
        return e && e->type == EntType::Point && glm::length(s->pt(id) - pc) < 1e-4;
    };
    std::vector<uint32_t> lines;
    for (auto& [id, e] : s->ents)
        if (!e.locked && e.type == EntType::Line && (isCorner(e.a) || isCorner(e.b))) lines.push_back(id);
    if (lines.size() != 2) {
        toast("Sketch fillet: click a corner where exactly two lines meet", ToastKind::Warning);
        return;
    }
    const Entity L1 = *s->find(lines[0]), L2 = *s->find(lines[1]);
    Vec2 o1 = s->pt(isCorner(L1.a) ? L1.b : L1.a), o2 = s->pt(isCorner(L2.a) ? L2.b : L2.a);
    double len1 = glm::length(o1 - pc), len2 = glm::length(o2 - pc);
    if (len1 < 1e-9 || len2 < 1e-9) return;
    Vec2 u1 = (o1 - pc) / len1, u2 = (o2 - pc) / len2;
    double cosT = std::clamp(glm::dot(u1, u2), -1.0, 1.0);
    double theta = std::acos(cosT);
    if (theta < 1e-3 || theta > kPi - 1e-3) {
        toast("Those lines are straight in line; nothing to round", ToastKind::Warning);
        return;
    }
    // Default radius: a fifth of the shorter line, rounded. A caller can pass an exact radius.
    double rDef = radius > 1e-9 ? radius : std::min(len1, len2) * 0.2;
    if (!(radius > 1e-9)) {
        double p10 = std::pow(10.0, std::floor(std::log10(std::max(rDef, 1e-6))));
        rDef = std::max(p10, std::floor(rDef / p10) * p10);
    }
    double t = rDef / std::tan(theta * 0.5);
    if (t >= std::min(len1, len2)) {
        toast("The lines are too short for a fillet there", ToastKind::Warning);
        return;
    }
    Sketch backup = *s;
    pushUndo("Sketch Fillet");
    Vec2 T1 = pc + u1 * t, T2 = pc + u2 * t;
    Vec2 bis = glm::normalize(u1 + u2);
    Vec2 C = pc + bis * (rDef / std::sin(theta * 0.5));
    uint32_t t1 = s->addPoint(T1), t2 = s->addPoint(T2), cp = s->addPoint(C);
    // Re-attach the line ends to the tangent points.
    for (uint32_t lid : lines) {
        Entity* e = s->find(lid);
        uint32_t np = lid == lines[0] ? t1 : t2;
        if (isCorner(e->a)) e->a = np;
        else if (isCorner(e->b)) e->b = np;
    }
    uint32_t arc = cross2(T1 - C, T2 - C) > 0 ? s->addArc(cp, t1, t2, L1.construction) : s->addArc(cp, t2, t1, L1.construction);
    s->addConstraint(CType::Tangent, lines[0], arc, 0, 1.0);
    s->addConstraint(CType::Tangent, lines[1], arc, 0, 1.0);
    // The sharp corner point goes (and constraints that hung on it).
    bool usedElsewhere = false;
    for (auto& [id, e] : s->ents)
        if (!e.locked && e.type != EntType::Point && (e.a == P || e.b == P || e.c == P)) usedElsewhere = true;
    // Dimensions hung on the corner survive (Fusion keeps a virtual sharp):
    // the point stays, riding both line extensions.
    bool dimmed = false;
    for (auto& [cid, c] : s->cons)
        if (c.e[0] == P || c.e[1] == P || c.e[2] == P) dimmed = true;
    if (dimmed) {
        s->addConstraint(CType::PointOnObject, P, lines[0], 0, 0);
        s->addConstraint(CType::PointOnObject, P, lines[1], 0, 0);
    } else if (!usedElsewhere) s->deleteEntities({P});
    uint32_t dim = s->addConstraint(CType::Radius, arc, 0, 0, rDef);
    if (!sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast("That fillet conflicts with the sketch's constraints", ToastKind::Warning);
        return;
    }
    sketchFinishOp();
    // Fusion: the radius box comes up right away.
    if (s->cons.count(dim)) {
        std::snprintf(sk_.dimBuf, sizeof sk_.dimBuf, "%g", rDef);
        sk_.dimEdit = dim;
        sk_.dimFocus = true;
    }
}

void App::sketchChamferAt(uint32_t hoverEnt, Vec2 cursor, double distance) {
    Sketch* s = curSketch();
    if (!s) return;
    const Entity* he = s->find(hoverEnt);
    if (!he) return;
    uint32_t P = 0;
    if (he->type == EntType::Point) P = hoverEnt;
    else if (he->type == EntType::Line)
        P = glm::length(s->pt(he->a) - cursor) < glm::length(s->pt(he->b) - cursor) ? he->a : he->b;
    if (!P) return;
    Vec2 pc = s->pt(P);
    auto isCorner = [&](uint32_t id) {
        const Entity* e = s->find(id);
        return e && e->type == EntType::Point && glm::length(s->pt(id) - pc) < 1e-4;
    };
    std::vector<uint32_t> lines;
    for (auto& [id, e] : s->ents)
        if (!e.locked && e.type == EntType::Line && (isCorner(e.a) || isCorner(e.b))) lines.push_back(id);
    if (lines.size() != 2) {
        toast("Sketch chamfer: click a corner where exactly two lines meet", ToastKind::Warning);
        return;
    }
    const Entity L1 = *s->find(lines[0]), L2 = *s->find(lines[1]);
    Vec2 o1 = s->pt(isCorner(L1.a) ? L1.b : L1.a), o2 = s->pt(isCorner(L2.a) ? L2.b : L2.a);
    double len1 = glm::length(o1 - pc), len2 = glm::length(o2 - pc);
    if (len1 < 1e-9 || len2 < 1e-9) return;
    Vec2 u1 = (o1 - pc) / len1, u2 = (o2 - pc) / len2;
    double cosT = std::clamp(glm::dot(u1, u2), -1.0, 1.0);
    double theta = std::acos(cosT);
    if (theta < 1e-3 || theta > kPi - 1e-3) {
        toast("Those lines are straight in line; nothing to bevel", ToastKind::Warning);
        return;
    }
    double d = distance > 1e-9 ? distance : std::min(len1, len2) * 0.2;
    if (!(distance > 1e-9)) {
        double p10 = std::pow(10.0, std::floor(std::log10(std::max(d, 1e-6))));
        d = std::max(p10, std::floor(d / p10) * p10);
    }
    if (d >= std::min(len1, len2) - 1e-9) {
        toast("The lines are too short for a chamfer there", ToastKind::Warning);
        return;
    }
    Sketch backup = *s;
    pushUndo("Sketch Chamfer");
    Vec2 T1 = pc + u1 * d, T2 = pc + u2 * d;
    uint32_t t1 = s->addPoint(T1), t2 = s->addPoint(T2);
    for (uint32_t lid : lines) {
        Entity* e = s->find(lid);
        uint32_t np = lid == lines[0] ? t1 : t2;
        if (isCorner(e->a)) e->a = np;
        else if (isCorner(e->b)) e->b = np;
    }
    s->addLine(t1, t2, L1.construction);
    s->addConstraint(CType::PointOnObject, P, lines[0]);
    s->addConstraint(CType::PointOnObject, P, lines[1]);
    uint32_t dim = s->addConstraint(CType::Distance, P, t1, 0, d);
    s->addConstraint(CType::Distance, P, t2, 0, d);
    if (!sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast("That chamfer conflicts with the sketch's constraints", ToastKind::Warning);
        return;
    }
    sketchFinishOp();
    if (s->cons.count(dim)) {
        std::snprintf(sk_.dimBuf, sizeof sk_.dimBuf, "%g", d);
        sk_.dimEdit = dim;
        sk_.dimFocus = true;
    }
}

// ===================================================================== extend

void App::sketchExtendAt(uint32_t ent, Vec2 cursor) {
    Sketch* s = curSketch();
    if (!s) return;
    const Entity* le = s->find(ent);
    if (!le || le->type != EntType::Line || le->locked) {
        toast("Extend works on lines", ToastKind::Warning);
        return;
    }
    Vec2 A = s->pt(le->a), B = s->pt(le->b);
    bool atB = glm::length(cursor - B) < glm::length(cursor - A);
    Vec2 from = atB ? B : A, dir = glm::normalize(atB ? B - A : A - B);
    double best = 1e300;
    Vec2 hit(0);
    for (auto& [id, e] : s->ents) {
        if (id == ent || e.type == EntType::Point) continue;
        Vec2 hs[2];
        int n = 0;
        if (e.type == EntType::Line) {
            Vec2 p = s->pt(e.a), q = s->pt(e.b);
            Vec2 h;
            if (e.locked) {
                if (lineLine(from, dir, p, q - p, h)) { hs[0] = h; n = 1; }
            } else if (lineLine(from, dir, p, q - p, h)) {
                double tt = glm::dot(h - p, q - p) / std::max(glm::dot(q - p, q - p), 1e-18);
                if (tt >= -1e-9 && tt <= 1 + 1e-9) { hs[0] = h; n = 1; }
            }
        } else {
            Sketch::ArcGeom g;
            if (!s->arcGeom(e, g)) continue;
            n = lineCircle(from, dir, g.c, g.r, hs);
            if (e.type == EntType::Arc) {
                int k = 0;
                for (int i = 0; i < n; ++i) {
                    double ang = wrap2pi(std::atan2(hs[i].y - g.c.y, hs[i].x - g.c.x) - g.a0);
                    if (ang <= g.sweep + 1e-9) hs[k++] = hs[i];
                }
                n = k;
            }
        }
        for (int i = 0; i < n; ++i) {
            double t = glm::dot(hs[i] - from, dir);
            if (t > 1e-6 && t < best) { best = t; hit = hs[i]; }
        }
    }
    if (best >= 1e300) {
        toast("Nothing to extend to in that direction", ToastKind::Warning);
        return;
    }
    pushUndo("Extend");
    uint32_t endPt = atB ? le->b : le->a;
    // If other curves share that end, give the line its own end point first.
    bool shared = false;
    for (auto& [id, e] : s->ents)
        if (id != ent && !e.locked && e.type != EntType::Point && (e.a == endPt || e.b == endPt || e.c == endPt)) shared = true;
    if (shared) {
        uint32_t np = s->addPoint(hit);
        Entity* m = s->find(ent);
        if (atB) m->b = np;
        else m->a = np;
    } else {
        s->setPt(endPt, hit);
    }
    sketchSolve({}, false);
    sketchFinishOp();
}

// ===================================================================== offset

void App::sketchOffsetPick(uint32_t ent) {
    Sketch* s = curSketch();
    if (!s) return;
    bool closed = false;
    auto chain = chainFrom(*s, ent, closed);
    if (chain.empty()) return;
    sk_.picks.clear();
    for (auto& l : chain) sk_.picks.push_back(l.ent);
    sk_.offsetClosed = closed;
    sk_.offsetRev.clear();
    for (auto& l : chain) sk_.offsetRev.push_back(l.rev ? 1 : 0);
    sk_.step = 1;
}

double App::sketchOffsetDistance(Vec2 cursor) const {
    auto* s = const_cast<App*>(this)->curSketch();
    if (!s || sk_.picks.empty()) return 0;
    double best = 1e300, sign = 1;
    for (size_t i = 0; i < sk_.picks.size(); ++i) {
        Link l{sk_.picks[i], i < sk_.offsetRev.size() && sk_.offsetRev[i]};
        Vec2 p, left;
        if (!closestOn(*s, l, cursor, p, left)) continue;
        double d = glm::length(cursor - p);
        if (d < best) {
            best = d;
            sign = glm::dot(cursor - p, left) >= 0 ? 1 : -1;
        }
    }
    return best >= 1e300 ? 0 : best * sign;
}

bool App::sketchOffsetPreview(double d, std::vector<std::pair<Vec2, Vec2>>& lines,
                              std::vector<std::tuple<Vec2, double, double, double>>& arcs) const {
    auto* s = const_cast<App*>(this)->curSketch();
    if (!s || sk_.picks.empty()) return false;
    std::vector<Link> chain;
    for (size_t i = 0; i < sk_.picks.size(); ++i) chain.push_back({sk_.picks[i], i < sk_.offsetRev.size() && sk_.offsetRev[i]});
    std::vector<OffSeg> segs;
    if (!offsetChain(*s, chain, sk_.offsetClosed, d, segs)) return false;
    lines.clear();
    arcs.clear();
    for (auto& o : segs) {
        if (!o.arc) { lines.push_back({o.a, o.b}); continue; }
        if (o.circle) { arcs.push_back({o.c, o.r, 0.0, kTwoPi}); continue; }
        // CCW from the natural start: travel order reversed for clockwise links.
        Vec2 a = o.rev ? o.b : o.a, b = o.rev ? o.a : o.b;
        double a0 = std::atan2(a.y - o.c.y, a.x - o.c.x);
        double sw = wrap2pi(std::atan2(b.y - o.c.y, b.x - o.c.x) - a0);
        if (sw < 1e-9) sw = kTwoPi;
        arcs.push_back({o.c, o.r, a0, sw});
    }
    return true;
}

void App::sketchOffsetCommit(double d) {
    Sketch* s = curSketch();
    if (!s || sk_.picks.empty() || std::abs(d) < 1e-9) return;
    std::vector<Link> chain;
    for (size_t i = 0; i < sk_.picks.size(); ++i) chain.push_back({sk_.picks[i], i < sk_.offsetRev.size() && sk_.offsetRev[i]});
    std::vector<OffSeg> segs;
    if (!offsetChain(*s, chain, sk_.offsetClosed, d, segs)) {
        toast("That offset collapses an arc. Try a smaller distance.", ToastKind::Warning);
        return;
    }
    Sketch backup = *s;
    pushUndo("Offset");
    bool cons = s->find(chain[0].ent) && s->find(chain[0].ent)->construction;
    // Shared joint points: segment i end == segment i+1 start.
    std::vector<uint32_t> startPt(segs.size(), 0), endPt(segs.size(), 0);
    for (size_t i = 0; i < segs.size(); ++i) {
        if (segs[i].circle) continue;
        startPt[i] = i > 0 && endPt[i - 1] ? endPt[i - 1] : s->addPoint(segs[i].a);
        bool lastClosing = sk_.offsetClosed && i + 1 == segs.size() && startPt[0];
        endPt[i] = lastClosing ? startPt[0] : s->addPoint(segs[i].b);
    }
    bool dimDone = false;
    for (size_t i = 0; i < segs.size(); ++i) {
        const OffSeg& o = segs[i];
        const Entity* src = s->find(o.src);
        if (!src) continue;
        if (o.circle) {
            uint32_t c = s->addCircle(src->c, o.r, cons);   // same centre point: concentric
            (void)c;
            continue;
        }
        if (!o.arc) {
            uint32_t ln = s->addLine(startPt[i], endPt[i], cons);
            s->addConstraint(CType::Parallel, ln, o.src);
            if (!dimDone) {
                // One editable offset distance, like Fusion's offset dimension.
                Vec2 a = s->pt(src->a), b = s->pt(src->b), u = b - a;
                double sd = cross2(u / glm::length(u), s->pt(startPt[i]) - a);
                s->addConstraint(CType::PointLineDistance, startPt[i], o.src, 0, std::abs(sd));
                auto& cc = s->cons.rbegin()->second;
                cc.value2 = sd < 0 ? -1.0 : 1.0;
                dimDone = true;
            }
        } else {
            // Arcs keep the original centre point (concentric) and run counter clockwise.
            uint32_t a = o.rev ? endPt[i] : startPt[i], b = o.rev ? startPt[i] : endPt[i];
            s->addArc(src->c, a, b, cons);
        }
    }
    if (!sketchSolve({}, false)) {
        *s = backup;
        undo_.dropLast();
        toast("That offset conflicts with the sketch's constraints", ToastKind::Warning);
        return;
    }
    sketchFinishOp();
    sk_.picks.clear();
    sk_.step = 0;
}

// ===================================================================== project

void App::sketchProjectClick() {
    Sketch* s = curSketch();
    if (!s) return;
    const HoverInfo h = hov_;
    if (h.kind != HoverKind::Edge && h.kind != HoverKind::Face) {
        toast("Project: click a body edge or face", ToastKind::Warning);
        return;
    }
    auto bit = doc_.bodies.find(h.body);
    if (bit == doc_.bodies.end()) return;
    auto pit = doc_.parts.find(bit->second.partId);
    if (pit == doc_.parts.end() || !pit->second.shape) return;
    std::vector<uint32_t> edges = h.kind == HoverKind::Edge ? std::vector<uint32_t>{h.index} : kernel::faceEdges(pit->second.shape, h.index);
    pushUndo("Project");
    if (!projectEdgesInto(*s, h.body, edges)) {
        undo_.dropLast();
        toast("Nothing to project (edges seen end on project to a point)", ToastKind::Warning);
        return;
    }
    sketchSolve({}, false);
    sketchFinishOp();
}

void App::sketchIntersectClick() {
    Sketch* s = curSketch();
    if (!s) return;
    const HoverInfo h = hov_;
    if (h.kind != HoverKind::Body && h.kind != HoverKind::Face && h.kind != HoverKind::Edge) {
        toast("Intersect: click a body", ToastKind::Warning);
        return;
    }
    pushUndo("Intersect");
    if (!intersectBodyInto(*s, h.body)) {
        undo_.dropLast();
        toast("The body does not cross the sketch plane", ToastKind::Warning);
        return;
    }
    sketchSolve({}, false);
    sketchFinishOp();
}

// Projects edges, already in world space, into the sketch. Shared ends become one point.
int curvesFromGeoms(Sketch& s, const std::vector<kernel::EdgeGeom>& geoms) {
    const sk::Plane& pl = s.plane;
    glm::dvec3 pn = pl.normal();
    std::map<std::pair<long long, long long>, uint32_t> pts;
    auto pointAt = [&](Vec2 q) {
        auto key = std::make_pair(std::llround(q.x * 1e5), std::llround(q.y * 1e5));
        auto it = pts.find(key);
        if (it != pts.end()) return it->second;
        uint32_t id = s.addPoint(q);
        s.addConstraint(CType::Fix, id, 0, 0, q.x);
        s.cons.rbegin()->second.value2 = q.y;
        pts[key] = id;
        return id;
    };
    int made = 0;
    for (const kernel::EdgeGeom& g : geoms) {
        bool flat = (g.type == 1 || g.type == 2) && std::abs(std::abs(glm::dot(glm::normalize(g.normal), pn)) - 1.0) < 1e-6;
        if (g.type == 0) {
            Vec2 a = pl.toLocal(g.a), b = pl.toLocal(g.b);
            if (glm::length(b - a) < 1e-6) continue;
            s.addLine(pointAt(a), pointAt(b));
            ++made;
        } else if (flat && g.type == 1) {
            uint32_t c = pointAt(pl.toLocal(g.center));
            uint32_t ci = s.addCircle(c, g.radius);
            s.addConstraint(CType::Diameter, ci, 0, 0, g.radius * 2);
            ++made;
        } else if (flat && g.type == 2) {
            bool same = glm::dot(glm::normalize(g.normal), pn) > 0;
            Vec2 a = pl.toLocal(g.a), b = pl.toLocal(g.b), c = pl.toLocal(g.center);
            uint32_t cp = pointAt(c);
            if (same) s.addArc(cp, pointAt(a), pointAt(b));
            else s.addArc(cp, pointAt(b), pointAt(a));
            ++made;
        } else {
            uint32_t prev = 0;
            for (auto& w : g.pts) {
                Vec2 q = pl.toLocal(w);
                uint32_t id = pointAt(q);
                if (prev && prev != id) { s.addLine(prev, id); ++made; }
                prev = id;
            }
        }
    }
    return made;
}

// Projects body edges into a sketch as fixed curves. Shared ends become one point. Returns the
// number of curves made.
int App::projectEdgesInto(Sketch& sref, uint32_t bodyId, const std::vector<uint32_t>& edges) {
    Sketch* s = &sref;
    auto bit = doc_.bodies.find(bodyId);
    if (bit == doc_.bodies.end()) return 0;
    auto pit = doc_.parts.find(bit->second.partId);
    if (pit == doc_.parts.end() || !pit->second.shape) return 0;
    std::vector<kernel::EdgeGeom> geoms;
    for (uint32_t ei : edges) {
        kernel::EdgeGeom g;
        if (!kernel::edgeGeom(pit->second.shape, bit->second.xform, ei, g)) continue;
        geoms.push_back(std::move(g));
    }
    return curvesFromGeoms(*s, geoms);
}

int App::intersectBodyInto(Sketch& sref, uint32_t bodyId) {
    auto bit = doc_.bodies.find(bodyId);
    if (bit == doc_.bodies.end()) return 0;
    auto pit = doc_.parts.find(bit->second.partId);
    if (pit == doc_.parts.end() || !pit->second.shape) return 0;
    const sk::Plane& pl = sref.plane;
    auto geoms = kernel::sectionEdges(pit->second.shape, bit->second.xform, pl.o, pl.normal());
    if (geoms.empty()) return 0;
    return curvesFromGeoms(sref, geoms);
}

void App::sketchProjectFaceClick() {
    Sketch* s = curSketch();
    if (!s) return;
    const HoverInfo h = hov_;
    if (h.kind != HoverKind::Face) {
        toast("Onto Face: click a face", ToastKind::Warning);
        return;
    }
    std::vector<uint32_t> ids;
    bool other = false;
    for (uint32_t id : sk_.selEnt) {
        const Entity* e = s->find(id);
        if (!e || e->locked || e->type == EntType::Point) continue;
        if (e->type != EntType::Line && e->type != EntType::Arc && e->type != EntType::Circle) { other = true; continue; }
        ids.push_back(id);
    }
    if (ids.empty()) {
        toast(other ? "Onto Face works on lines, arcs, and circles" : "Select the curves first, then click a face", ToastKind::Warning);
        return;
    }
    pushUndo("Onto Face");
    if (projectCurvesOnFace(*s, h.body, (int)h.index, ids, s->plane.normal()) <= 0) {
        undo_.dropLast();
        toast("The curves do not meet that face", ToastKind::Warning);
        return;
    }
    sketchSolve({}, false);
    sketchFinishOp();
}

int App::projectCurvesOnFace(Sketch& sref, uint32_t bodyId, int face, const std::vector<uint32_t>& ents, const glm::dvec3& dir) {
    auto bit = doc_.bodies.find(bodyId);
    if (bit == doc_.bodies.end() || face < 0) return 0;
    auto pit = doc_.parts.find(bit->second.partId);
    if (pit == doc_.parts.end() || !pit->second.shape) return 0;
    const sk::Plane& pl = sref.plane;
    bool other = false;
    std::vector<kernel::DropCurve> curves;
    auto take = [&](const Entity& e) {
        if (e.locked || e.type == EntType::Point) return;
        kernel::DropCurve c;
        if (e.type == EntType::Line) {
            c.type = 0;
            c.a = pl.toWorld(sref.pt(e.a));
            c.b = pl.toWorld(sref.pt(e.b));
        } else if (e.type == EntType::Circle || e.type == EntType::Arc) {
            c.type = e.type == EntType::Circle ? 1 : 2;
            c.center = pl.toWorld(sref.pt(e.c));
            c.normal = pl.normal();
            c.radius = e.type == EntType::Circle ? e.r : glm::length(sref.pt(e.a) - sref.pt(e.c));
            if (e.type == EntType::Arc) {
                c.a = pl.toWorld(sref.pt(e.a));
                c.b = pl.toWorld(sref.pt(e.b));
            }
        } else { other = true; return; }
        curves.push_back(c);
    };
    if (ents.empty()) {
        for (auto& [id, e] : sref.ents) take(e);
    } else {
        for (uint32_t id : ents) {
            const Entity* e = sref.find(id);
            if (e) take(*e);
        }
    }
    if (curves.empty()) return other ? -1 : 0;
    auto geoms = kernel::projectOnFace(pit->second.shape, bit->second.xform, (uint32_t)face, curves, dir);
    if (geoms.empty()) return 0;
    return curvesFromGeoms(sref, geoms);
}

} // namespace kst
