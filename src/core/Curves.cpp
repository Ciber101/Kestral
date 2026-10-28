#include "core/Curves.h"
#include "sketch/Profiles.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace kst {
namespace {

const double kTwoPi = 6.283185307179586476925;

// Build the exact segment for one loop edge (entity restricted to [ta, tb]), in world space.
bool segFor(const sk::Sketch& s, const sk::ProfEdge& pe, const sk::Plane& pl, bool loopCcw, CurveSeg& out) {
    const sk::Entity* e = s.find(pe.ent);
    if (!e && !pe.chord) return false;
    if (pe.chord) {
        bool fwd = pe.tb >= pe.ta;
        double t0 = fwd ? pe.ta : pe.tb, t1 = fwd ? pe.tb : pe.ta;
        sk::Vec2 p0 = pe.ca + (pe.cb - pe.ca) * t0, p1 = pe.ca + (pe.cb - pe.ca) * t1;
        out.kind = CurveSeg::Kind::Line;
        out.a = pl.toWorld(p0);
        out.b = pl.toWorld(p1);
        out.normal = pl.normal();
        out.back = !fwd;
        return glm::length(p1 - p0) > 1e-9;
    }
    bool fwd = pe.tb >= pe.ta;
    double t0 = fwd ? pe.ta : pe.tb, t1 = fwd ? pe.tb : pe.ta;
    out.normal = pl.normal();   // every segment records its plane, lines included
    if (e->type == sk::EntType::Line) {
        sk::Vec2 A = s.pt(e->a), B = s.pt(e->b);
        sk::Vec2 p0 = A + (B - A) * t0, p1 = A + (B - A) * t1;
        out.kind = CurveSeg::Kind::Line;
        out.a = pl.toWorld(p0);
        out.b = pl.toWorld(p1);
        out.back = !fwd;
        return glm::length(p1 - p0) > 1e-9;
    }
    if (e->type == sk::EntType::Ellipse || e->type == sk::EntType::EllipArc) {
        sk::Sketch::EllipseGeom eg;
        if (!s.ellipseGeom(*e, eg)) return false;
        bool full = e->type == sk::EntType::Ellipse && (t1 - t0) >= 1.0 - 1e-9;
        sk::Vec2 p0 = s.ellipsePoint(eg, t0), p1 = s.ellipsePoint(eg, t1);
        sk::Sketch::EllipseGeom tipG = eg;
        tipG.a0 = 0;
        tipG.sweep = kTwoPi;
        sk::Vec2 tip = s.ellipsePoint(tipG, 0);
        out.center = pl.toWorld(eg.c);
        out.a = pl.toWorld(p0);
        out.b = pl.toWorld(p1);
        out.radius = eg.major;
        out.minor = eg.minor;
        glm::dvec3 dir = pl.toWorld(tip) - out.center;
        if (glm::length(dir) < 1e-9) return false;
        out.majorDir = glm::normalize(dir);
        out.ang0 = eg.a0 + eg.sweep * t0;
        out.sweep = (t1 - t0) * eg.sweep;
        if (full) {
            out.kind = CurveSeg::Kind::Ellipse;
            out.back = !loopCcw;
            return true;
        }
        out.kind = CurveSeg::Kind::EllipArc;
        out.back = !fwd;
        return out.sweep > 1e-7 && out.sweep < kTwoPi - 1e-7;
    }
    sk::Sketch::ArcGeom g;
    if (!s.arcGeom(*e, g)) return false;
    bool fullCircle = e->type == sk::EntType::Circle && (t1 - t0) >= 1.0 - 1e-9;
    sk::Vec2 p0 = s.arcPoint(g, t0), p1 = s.arcPoint(g, t1);
    out.center = pl.toWorld(g.c);
    out.radius = g.r;
    out.a = pl.toWorld(p0);
    out.b = pl.toWorld(p1);
    if (fullCircle) {
        out.kind = CurveSeg::Kind::Circle;
        out.back = !loopCcw;   // holes wind clockwise: reverse the edge
        return true;
    }
    out.kind = CurveSeg::Kind::Arc;
    out.back = !fwd;
    double ang = (t1 - t0) * g.sweep;
    return ang > 1e-7 && ang < kTwoPi - 1e-7;
}

std::vector<CurveSeg> loopSegs(const sk::Sketch& s, const sk::Loop& loop, const sk::Plane& pl) {
    std::vector<CurveSeg> segs;
    segs.reserve(loop.edges.size());
    for (auto& pe : loop.edges) {
        CurveSeg c;
        if (segFor(s, pe, pl, loop.area > 0, c)) segs.push_back(c);
    }
    return segs;
}

} // namespace

namespace {
bool regionOf(const sk::Sketch& s, const sk::Profile& p, RegionLoops& region) {
    std::vector<CurveSeg> outer = loopSegs(s, p.outer, s.plane);
    if (outer.empty()) return false;
    if (outer.size() == 1 && outer[0].kind != CurveSeg::Kind::Circle && outer[0].kind != CurveSeg::Kind::Ellipse) return false;
    region.push_back(std::move(outer));
    for (auto& h : p.holes) {
        std::vector<CurveSeg> hole = loopSegs(s, h, s.plane);
        if (hole.empty()) continue;
        region.push_back(std::move(hole));
    }
    return true;
}
} // namespace

std::vector<RegionLoops> sketchRegions(const sk::Sketch& s) { return sketchRegionsAt(s, {}); }

std::vector<RegionLoops> sketchRegionsAt(const sk::Sketch& s, const std::vector<sk::Vec2>& pts) {
    std::vector<RegionLoops> out;
    std::vector<sk::Profile> profs = sk::findProfiles(s);
    std::vector<char> use(profs.size(), pts.empty() ? 1 : 0);
    for (auto& q : pts) {
        int best = -1;
        double area = 1e300;
        for (size_t i = 0; i < profs.size(); ++i) {
            if (!sk::pointInProfile(profs[i], q)) continue;
            if (std::abs(profs[i].area) < area) { area = std::abs(profs[i].area); best = (int)i; }
        }
        if (best >= 0) use[best] = 1;
    }
    for (size_t i = 0; i < profs.size(); ++i) {
        if (!use[i]) continue;
        RegionLoops region;
        if (regionOf(s, profs[i], region)) out.push_back(std::move(region));
    }
    return out;
}

sk::Vec2 profileSamplePoint(const sk::Profile& p) {
    // Centroid of the largest triangle: always inside the region, away from its edges.
    sk::Vec2 best = p.lo;
    double bestA = -1;
    for (size_t i = 0; i + 2 < p.tris.size(); i += 3) {
        sk::Vec2 a = p.tris[i], b = p.tris[i + 1], c = p.tris[i + 2];
        double ar = std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
        if (ar > bestA) { bestA = ar; best = (a + b + c) / 3.0; }
    }
    return best;
}

std::vector<CurveSeg> chainedPath(const sk::Sketch& s) {
    std::vector<uint32_t> ents;
    for (auto& [id, e] : s.ents)
        if (e.type != sk::EntType::Point && !e.locked && !e.construction) ents.push_back(id);
    if (ents.empty()) return {};

    auto ptA = [&](uint32_t id) -> sk::Vec2 {
        const sk::Entity* e = s.find(id);
        if (e->type == sk::EntType::Line) return s.pt(e->a);
        sk::Sketch::ArcGeom g;
        s.arcGeom(*e, g);
        return s.arcPoint(g, 0.0);
    };
    auto ptB = [&](uint32_t id) -> sk::Vec2 {
        const sk::Entity* e = s.find(id);
        if (e->type == sk::EntType::Line) return s.pt(e->b);
        sk::Sketch::ArcGeom g;
        s.arcGeom(*e, g);
        return s.arcPoint(g, 1.0);
    };

    std::vector<std::pair<uint32_t, bool>> chain{{ents.front(), false}};
    ents.erase(ents.begin());
    bool reversedOnce = false;
    while (!ents.empty()) {
        auto [last, lastFlip] = chain.back();
        sk::Vec2 open = lastFlip ? ptA(last) : ptB(last);
        double best = 1e-7;
        size_t bi = SIZE_MAX;
        bool attachFlipped = false;
        for (size_t i = 0; i < ents.size(); ++i) {
            double da = glm::length(ptA(ents[i]) - open);
            double db = glm::length(ptB(ents[i]) - open);
            if (da < best) {
                best = da;
                bi = i;
                attachFlipped = false;
            } else if (db < best) {
                best = db;
                bi = i;
                attachFlipped = true;
            }
        }
        if (bi == SIZE_MAX) {
            if (reversedOnce)
                throw std::runtime_error("The path sketch is not one connected chain of curves");
            reversedOnce = true;
            std::reverse(chain.begin(), chain.end());
            for (auto& c : chain) c.second = !c.second;
            continue;
        }
        chain.push_back({ents[bi], attachFlipped});
        ents.erase(ents.begin() + bi);
    }

    std::vector<CurveSeg> segs;
    for (auto& [id, flipped] : chain) {
        const sk::Entity* e = s.find(id);
        if (!e) continue;
        CurveSeg c;
        c.normal = s.plane.normal();
        if (e->type == sk::EntType::Line) {
            c.kind = CurveSeg::Kind::Line;
            sk::Vec2 a = s.pt(e->a), b = s.pt(e->b);
            if (flipped) std::swap(a, b);
            c.a = s.plane.toWorld(a);
            c.b = s.plane.toWorld(b);
            segs.push_back(c);
        } else if (e->type == sk::EntType::Arc || e->type == sk::EntType::Circle) {
            sk::Sketch::ArcGeom g;
            if (!s.arcGeom(*e, g)) continue;
            c.kind = e->type == sk::EntType::Circle ? CurveSeg::Kind::Circle : CurveSeg::Kind::Arc;
            c.center = s.plane.toWorld(g.c);
            c.radius = g.r;
            // Arcs keep their natural (counter clockwise) endpoints; `back` reverses the edge in
            // the wire. Swapping a and b as well would build the complementary arc.
            c.a = s.plane.toWorld(s.arcPoint(g, 0.0));
            c.b = s.plane.toWorld(s.arcPoint(g, 1.0));
            c.back = flipped;
            segs.push_back(c);
        }
    }
    return segs;
}

} // namespace kst
