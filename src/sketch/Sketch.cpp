#include "sketch/Sketch.h"
#include <atomic>
#include <algorithm>
#include <cmath>

namespace kst::sk {

uint64_t Sketch::nextRev() {
    static std::atomic<uint64_t> counter{1000};
    return ++counter;
}


namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
double wrap2pi(double a) {
    a = std::fmod(a, kTwoPi);
    if (a < 0) a += kTwoPi;
    return a;
}
}

bool Plane::intersect(const glm::dvec3& ro, const glm::dvec3& rd, Vec2& out) const {
    glm::dvec3 n = normal();
    double den = glm::dot(n, rd);
    if (std::abs(den) < 1e-12) return false;
    double t = glm::dot(n, o - ro) / den;
    out = toLocal(ro + rd * t);
    return std::isfinite(out.x) && std::isfinite(out.y);
}

Sketch::Sketch() {
    auto mk = [&](uint32_t id, double x, double y) {
        Entity e; e.id = id; e.type = EntType::Point; e.x = x; e.y = y; e.locked = true;
        ents[id] = e;
    };
    mk(kOrigin, 0, 0);
    mk(2, 1, 0);
    mk(3, 0, 1);
    Entity ax; ax.id = kXAxis; ax.type = EntType::Line; ax.a = kOrigin; ax.b = 2; ax.locked = true; ax.construction = true;
    ents[kXAxis] = ax;
    Entity ay; ay.id = kYAxis; ay.type = EntType::Line; ay.a = kOrigin; ay.b = 3; ay.locked = true; ay.construction = true;
    ents[kYAxis] = ay;
}

Vec2 Sketch::pt(uint32_t pointId) const {
    auto it = ents.find(pointId);
    return it == ents.end() ? Vec2(0) : Vec2(it->second.x, it->second.y);
}
void Sketch::setPt(uint32_t pointId, Vec2 p) {
    if (Entity* e = find(pointId)) { if (!e->locked) { e->x = p.x; e->y = p.y; } }
}

uint32_t Sketch::addPoint(Vec2 p) {
    Entity e; e.id = nextId++; e.type = EntType::Point; e.x = p.x; e.y = p.y;
    ents[e.id] = e;
    touch();
    return e.id;
}
uint32_t Sketch::addLine(uint32_t a, uint32_t b, bool construction) {
    Entity e; e.id = nextId++; e.type = EntType::Line; e.a = a; e.b = b; e.construction = construction;
    ents[e.id] = e;
    touch();
    return e.id;
}
uint32_t Sketch::addCircle(uint32_t center, double r, bool construction) {
    Entity e; e.id = nextId++; e.type = EntType::Circle; e.c = center; e.r = r; e.construction = construction;
    ents[e.id] = e;
    touch();
    return e.id;
}
uint32_t Sketch::addArc(uint32_t center, uint32_t start, uint32_t end, bool construction) {
    Entity e; e.id = nextId++; e.type = EntType::Arc; e.c = center; e.a = start; e.b = end; e.construction = construction;
    ents[e.id] = e;
    touch();
    return e.id;
}
uint32_t Sketch::addEllipse(uint32_t center, double major, double minor, double rotation, bool construction) {
    if (!(major > 1e-9) || !(minor > 1e-9)) return 0;
    if (minor > major) { std::swap(major, minor); rotation += 1.5707963267948966; }
    Entity e; e.id = nextId++; e.type = EntType::Ellipse; e.c = center; e.r = major; e.x = minor; e.y = rotation;
    e.construction = construction;
    ents[e.id] = e;
    touch();
    return e.id;
}
uint32_t Sketch::addEllipArc(uint32_t center, double major, double minor, double rotation, uint32_t start, uint32_t end, bool construction) {
    if (!(major > 1e-9) || !(minor > 1e-9)) return 0;
    if (minor > major) { std::swap(major, minor); rotation += 1.5707963267948966; }
    Entity e; e.id = nextId++; e.type = EntType::EllipArc; e.c = center; e.r = major; e.x = minor; e.y = rotation;
    e.a = start; e.b = end; e.construction = construction;
    ents[e.id] = e;
    touch();
    return e.id;
}
uint32_t Sketch::addSpline(const std::vector<uint32_t>& fit, bool loop, bool construction) {
    Entity e; e.id = nextId++; e.type = EntType::Spline; e.loop = loop; e.pts = fit; e.construction = construction;
    ents[e.id] = e;
    touch();
    return e.id;
}
uint32_t Sketch::addCtrlSpline(const std::vector<uint32_t>& ctrl, bool loop, bool construction) {
    if (ctrl.size() < (loop ? 3u : 2u)) return 0;
    Entity e; e.id = nextId++; e.type = EntType::CtrlSpline; e.loop = loop; e.pts = ctrl; e.construction = construction;
    ents[e.id] = e;
    touch();
    return e.id;
}
std::vector<Vec2> Sketch::sampleFitSpline(const std::vector<Vec2>& fit, bool loop, int steps) {
    bool closed = loop && fit.size() >= 3;
    if (fit.size() < 2) return fit;
    if (steps < 1) steps = 1;
    int n = (int)fit.size();
    int spans = closed ? n : n - 1;
    auto at = [&](int i) {
        if (closed) { i %= n; if (i < 0) i += n; return fit[i]; }
        return fit[std::clamp(i, 0, n - 1)];
    };
    auto cr = [](Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, double t) {
        double t2 = t * t, t3 = t2 * t;
        return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2
                      + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
    };
    std::vector<Vec2> out;
    for (int s = 0; s < spans; ++s)
        for (int k = 0; k < steps; ++k)
            out.push_back(cr(at(s - 1), at(s), at(s + 1), at(s + 2), (double)k / steps));
    out.push_back(closed ? fit[0] : fit.back());
    return out;
}
std::vector<Vec2> Sketch::sampleCtrlSpline(const std::vector<Vec2>& ctrl, bool loop, int steps) {
    const int n = (int)ctrl.size();
    if (n < 2) return ctrl;
    if (steps < 1) steps = 1;
    if (loop && n >= 3) {
        // Uniform periodic cubic. The curve stays inside the control polygon.
        std::vector<Vec2> out;
        out.reserve((size_t)n * steps + 1);
        auto at = [&](int i) {
            i %= n;
            if (i < 0) i += n;
            return ctrl[i];
        };
        for (int s = 0; s < n; ++s) {
            Vec2 p0 = at(s - 1), p1 = at(s), p2 = at(s + 1), p3 = at(s + 2);
            for (int k = 0; k < steps; ++k) {
                double t = (double)k / steps;
                double u = 1.0 - t;
                double b0 = u * u * u / 6.0;
                double b1 = (3.0 * t * t * t - 6.0 * t * t + 4.0) / 6.0;
                double b2 = (-3.0 * t * t * t + 3.0 * t * t + 3.0 * t + 1.0) / 6.0;
                double b3 = t * t * t / 6.0;
                out.push_back(p0 * b0 + p1 * b1 + p2 * b2 + p3 * b3);
            }
        }
        out.push_back(out.front());
        return out;
    }
    // Open clamped B-spline. Degree drops to 1 or 2 when there are fewer than four points.
    // Four points is a cubic Bezier: the ends lie on the curve, the middle two only pull it.
    const int p = std::min(3, n - 1);
    const int last = n - 1;
    const int m = n + p + 1;
    std::vector<double> U(m, 0.0);
    const int interior = n - p - 1;
    for (int j = 1; j <= interior; ++j) U[p + j] = (double)j / (double)(interior + 1);
    for (int i = p + interior + 1; i < m; ++i) U[i] = 1.0;
    auto eval = [&](double t) {
        int k = p;
        if (t >= 1.0 - 1e-12) { t = 1.0; k = last; }
        else while (k < last && t >= U[k + 1]) ++k;
        Vec2 d[4];
        for (int j = 0; j <= p; ++j) d[j] = ctrl[k - p + j];
        for (int r = 1; r <= p; ++r) {
            for (int j = p; j >= r; --j) {
                double den = U[k + 1 + j - r] - U[k + j - p];
                double a = den > 1e-15 ? (t - U[k + j - p]) / den : 0.0;
                d[j] = d[j - 1] * (1.0 - a) + d[j] * a;
            }
        }
        return d[p];
    };
    const int samples = std::max(p, (n - 1) * steps);
    std::vector<Vec2> out;
    out.reserve(samples + 1);
    for (int i = 0; i <= samples; ++i) out.push_back(eval((double)i / samples));
    return out;
}
std::vector<Vec2> Sketch::splinePoly(const Entity& e, int steps) const {
    std::vector<Vec2> fit;
    for (uint32_t id : e.pts)
        if (find(id)) fit.push_back(pt(id));
    return sampleFitSpline(fit, e.loop, steps);
}
std::vector<Vec2> Sketch::ctrlSplinePoly(const Entity& e, int steps) const {
    std::vector<Vec2> ctrl;
    for (uint32_t id : e.pts)
        if (find(id)) ctrl.push_back(pt(id));
    return sampleCtrlSpline(ctrl, e.loop, steps);
}
uint32_t Sketch::addConstraint(CType t, uint32_t e0, uint32_t e1, uint32_t e2, double value) {
    Constraint c;
    c.id = nextId++;
    c.type = t;
    c.e[0] = e0; c.e[1] = e1; c.e[2] = e2;
    c.value = value;
    if (isDimension(t)) c.name = "d" + std::to_string(++dimCounter);
    cons[c.id] = c;
    touch();
    return c.id;
}

bool Sketch::hasConstraint(CType t, uint32_t e0, uint32_t e1, uint32_t e2) const {
    for (auto& [id, c] : cons) {
        if (c.type != t) continue;
        if (c.e[0] == e0 && c.e[1] == e1 && c.e[2] == e2) return true;
        if ((t == CType::Coincident || t == CType::Parallel || t == CType::Perpendicular || t == CType::Equal ||
             t == CType::Collinear || t == CType::Tangent) && c.e[0] == e1 && c.e[1] == e0 && c.e[2] == e2) return true;
    }
    return false;
}

double Sketch::measureDimension(const Constraint& c) const {
    auto ptok = [&](uint32_t id, Vec2& o) {
        const Entity* e = find(id);
        if (!e || e->type != EntType::Point) return false;
        o = pt(id);
        return true;
    };
    auto rad = [&](uint32_t id, double& r) {
        const Entity* e = find(id);
        if (!e) return false;
        if (e->type == EntType::Circle || e->type == EntType::Ellipse || e->type == EntType::EllipArc) { r = e->r; return r > 0; }
        if (e->type == EntType::Arc) {
            Vec2 cpt, a;
            if (!ptok(e->c, cpt) || !ptok(e->a, a)) return false;
            r = glm::length(a - cpt);
            return r > 0;
        }
        return false;
    };
    switch (c.type) {
    case CType::Distance: {
        Vec2 a, b;
        if (!ptok(c.e[0], a) || !ptok(c.e[1], b)) return c.value;
        return glm::length(b - a);
    }
    case CType::HDistance: {
        Vec2 a, b;
        if (!ptok(c.e[0], a) || !ptok(c.e[1], b)) return c.value;
        return b.x - a.x;
    }
    case CType::VDistance: {
        Vec2 a, b;
        if (!ptok(c.e[0], a) || !ptok(c.e[1], b)) return c.value;
        return b.y - a.y;
    }
    case CType::Radius: {
        double r = 0;
        return rad(c.e[0], r) ? r : c.value;
    }
    case CType::Diameter: {
        double r = 0;
        return rad(c.e[0], r) ? 2.0 * r : c.value;
    }
    case CType::Angle: {
        const Entity* l0 = find(c.e[0]);
        const Entity* l1 = find(c.e[1]);
        if (!l0 || !l1 || l0->type != EntType::Line || l1->type != EntType::Line) return c.value;
        Vec2 d0 = pt(l0->b) - pt(l0->a), d1 = pt(l1->b) - pt(l1->a);
        return std::atan2(d0.x * d1.y - d0.y * d1.x, glm::dot(d0, d1));
    }
    case CType::PointLineDistance: {
        const Entity* l = find(c.e[1]);
        Vec2 p;
        if (!ptok(c.e[0], p) || !l || l->type != EntType::Line) return c.value;
        Vec2 a = pt(l->a), b = pt(l->b), d = b - a;
        double L = glm::length(d);
        if (L < 1e-12) return c.value;
        Vec2 u = d / L;
        return std::abs(u.x * (p.y - a.y) - u.y * (p.x - a.x));
    }
    default: return c.value;
    }
}

void Sketch::deleteEntities(const std::set<uint32_t>& ids) {
    std::set<uint32_t> dead;
    for (uint32_t id : ids) {
        const Entity* e = find(id);
        if (!e || e->locked) continue;
        dead.insert(id);
    }
    // A deleted point takes every curve that uses it.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [id, e] : ents) {
            if (dead.count(id) || e.locked) continue;
            bool kill = false;
            if (e.type == EntType::Line) kill = dead.count(e.a) || dead.count(e.b);
            else if (e.type == EntType::Circle || e.type == EntType::Ellipse) kill = dead.count(e.c);
            else if (e.type == EntType::Arc || e.type == EntType::EllipArc) kill = dead.count(e.c) || dead.count(e.a) || dead.count(e.b);
            else if (e.type == EntType::Spline || e.type == EntType::CtrlSpline) {
                for (uint32_t p : e.pts) if (dead.count(p)) kill = true;
            }
            if (kill) { dead.insert(id); changed = true; }
        }
    }
    // Orphaned points (only used by deleted curves) go too.
    std::set<uint32_t> used;
    for (auto& [id, e] : ents) {
        if (dead.count(id) || e.type == EntType::Point) continue;
        if (e.type == EntType::Line) { used.insert(e.a); used.insert(e.b); }
        else if (e.type == EntType::Circle) used.insert(e.c);
        else if (e.type == EntType::Spline || e.type == EntType::CtrlSpline) { for (uint32_t p : e.pts) used.insert(p); }
        else { used.insert(e.c); used.insert(e.a); used.insert(e.b); }
    }
    for (auto& [id, e] : ents) {
        if (e.type != EntType::Point || e.locked || dead.count(id)) continue;
        // Keep points that are not part of any curve but were not asked to be removed.
        bool wasUsedByDead = false;
        for (uint32_t d : dead) {
            const Entity* de = find(d);
            if (!de) continue;
            if ((de->type == EntType::Line && (de->a == id || de->b == id)) ||
                (de->type == EntType::Circle && de->c == id) ||
                (de->type == EntType::Ellipse && de->c == id) ||
                ((de->type == EntType::Arc || de->type == EntType::EllipArc) && (de->c == id || de->a == id || de->b == id)) ||
                ((de->type == EntType::Spline || de->type == EntType::CtrlSpline) && std::find(de->pts.begin(), de->pts.end(), id) != de->pts.end())) { wasUsedByDead = true; break; }
        }
        if (wasUsedByDead && !used.count(id)) dead.insert(id);
    }
    for (uint32_t d : dead) ents.erase(d);
    for (auto it = cons.begin(); it != cons.end();) {
        bool kill = false;
        for (int k = 0; k < 3; ++k)
            if (it->second.e[k] && !ents.count(it->second.e[k])) kill = true;
        if (kill) it = cons.erase(it);
        else ++it;
    }
    touch();
}

bool Sketch::circleGeom(const Entity& e, Vec2& c, double& r) const {
    if (e.type == EntType::Circle) { c = pt(e.c); r = e.r; return true; }
    if (e.type == EntType::Arc) { c = pt(e.c); r = glm::length(pt(e.a) - c); return true; }
    return false;
}

bool Sketch::arcGeom(const Entity& e, ArcGeom& g) const {
    if (e.type == EntType::Circle) {
        g.c = pt(e.c); g.r = e.r; g.a0 = 0; g.sweep = kTwoPi;
        return true;
    }
    if (e.type != EntType::Arc) return false;
    g.c = pt(e.c);
    Vec2 a = pt(e.a) - g.c, b = pt(e.b) - g.c;
    g.r = glm::length(a);
    g.a0 = std::atan2(a.y, a.x);
    double a1 = std::atan2(b.y, b.x);
    g.sweep = wrap2pi(a1 - g.a0);
    if (g.sweep < 1e-9) g.sweep = kTwoPi;
    return true;
}

Vec2 Sketch::arcPoint(const ArcGeom& g, double t) const {
    double a = g.a0 + g.sweep * t;
    return g.c + Vec2(std::cos(a), std::sin(a)) * g.r;
}

bool Sketch::ellipseGeom(const Entity& e, EllipseGeom& g) const {
    if (e.type != EntType::Ellipse && e.type != EntType::EllipArc) return false;
    if (!find(e.c) || !(e.r > 1e-9) || !(e.x > 1e-9)) return false;
    g.c = pt(e.c);
    g.major = e.r;
    g.minor = e.x;
    g.rot = e.y;
    g.a0 = 0;
    g.sweep = kTwoPi;
    if (e.type == EntType::EllipArc) {
        if (!find(e.a) || !find(e.b)) return false;
        auto ecc = [&](Vec2 p) {
            Vec2 d = p - g.c;
            double cs = std::cos(g.rot), sn = std::sin(g.rot);
            double lx = d.x * cs + d.y * sn;
            double ly = -d.x * sn + d.y * cs;
            return std::atan2(ly / g.minor, lx / g.major);
        };
        g.a0 = ecc(pt(e.a));
        g.sweep = wrap2pi(ecc(pt(e.b)) - g.a0);
        if (g.sweep < 1e-9) g.sweep = kTwoPi;
    }
    return true;
}

Vec2 Sketch::ellipsePoint(const EllipseGeom& g, double t) const {
    double ang = g.a0 + g.sweep * t;
    double lx = g.major * std::cos(ang), ly = g.minor * std::sin(ang);
    double cs = std::cos(g.rot), sn = std::sin(g.rot);
    return g.c + Vec2(cs * lx - sn * ly, sn * lx + cs * ly);
}

double Sketch::distanceTo(const Entity& e, Vec2 p, Vec2* closest) const {
    if (e.type == EntType::Ellipse || e.type == EntType::EllipArc) {
        EllipseGeom g;
        if (!ellipseGeom(e, g)) return 1e30;
        double best = 1e30;
        Vec2 q = g.c;
        const int n = 64;
        Vec2 prev = ellipsePoint(g, 0);
        for (int i = 1; i <= n; ++i) {
            Vec2 b = ellipsePoint(g, (double)i / n), d = b - prev;
            double L2 = glm::dot(d, d);
            double t = L2 > 1e-18 ? std::clamp(glm::dot(p - prev, d) / L2, 0.0, 1.0) : 0.0;
            Vec2 hit = prev + d * t;
            double dist = glm::length(p - hit);
            if (dist < best) { best = dist; q = hit; }
            prev = b;
        }
        if (closest) *closest = q;
        return best;
    }
    if (e.type == EntType::Spline || e.type == EntType::CtrlSpline) {
        auto poly = e.type == EntType::CtrlSpline ? ctrlSplinePoly(e, 8) : splinePoly(e, 8);
        double best = 1e30;
        Vec2 q = poly.empty() ? Vec2(0) : poly[0];
        for (size_t i = 0; i + 1 < poly.size(); ++i) {
            Vec2 a = poly[i], b = poly[i + 1], d = b - a;
            double L2 = glm::dot(d, d);
            double t = L2 > 1e-18 ? std::clamp(glm::dot(p - a, d) / L2, 0.0, 1.0) : 0.0;
            Vec2 hit = a + d * t;
            double dist = glm::length(p - hit);
            if (dist < best) { best = dist; q = hit; }
        }
        if (closest) *closest = q;
        return best;
    }
    if (e.type == EntType::Point) {
        Vec2 q(e.x, e.y);
        if (closest) *closest = q;
        return glm::length(p - q);
    }
    if (e.type == EntType::Line) {
        Vec2 a = pt(e.a), b = pt(e.b), d = b - a;
        double L2 = glm::dot(d, d);
        double t = L2 > 1e-18 ? std::clamp(glm::dot(p - a, d) / L2, 0.0, 1.0) : 0.0;
        Vec2 q = a + d * t;
        if (closest) *closest = q;
        return glm::length(p - q);
    }
    ArcGeom g;
    if (!arcGeom(e, g)) return 1e30;
    Vec2 v = p - g.c;
    double len = glm::length(v);
    double ang = std::atan2(v.y, v.x);
    double rel = wrap2pi(ang - g.a0);
    Vec2 q;
    if (rel <= g.sweep + 1e-12 || e.type == EntType::Circle) {
        q = len > 1e-12 ? g.c + v / len * g.r : g.c + Vec2(g.r, 0);
    } else {
        Vec2 s = arcPoint(g, 0), t = arcPoint(g, 1);
        q = glm::length(p - s) < glm::length(p - t) ? s : t;
    }
    if (closest) *closest = q;
    return glm::length(p - q);
}

Vec2 Sketch::center(const Entity& e) const {
    switch (e.type) {
    case EntType::Point: return {e.x, e.y};
    case EntType::Line: return (pt(e.a) + pt(e.b)) * 0.5;
    case EntType::Spline:
    case EntType::CtrlSpline: {
        if (e.pts.empty()) return {0, 0};
        Vec2 s(0);
        int n = 0;
        for (uint32_t id : e.pts) if (find(id)) { s += pt(id); ++n; }
        return n ? s / (double)n : Vec2(0);
    }
    default: return pt(e.c);
    }
}

void Sketch::bounds(Vec2& lo, Vec2& hi) const {
    lo = Vec2(1e30); hi = Vec2(-1e30);
    auto add = [&](Vec2 p) { lo = glm::min(lo, p); hi = glm::max(hi, p); };
    for (auto& [id, e] : ents) {
        if (e.locked) continue;
        if (e.type == EntType::Point) add({e.x, e.y});
        else if (e.type == EntType::Spline || e.type == EntType::CtrlSpline) {
            auto poly = e.type == EntType::CtrlSpline ? ctrlSplinePoly(e, 8) : splinePoly(e, 4);
            for (auto& p : poly) add(p);
        }
        else if (e.type == EntType::Circle || e.type == EntType::Arc) {
            ArcGeom g;
            if (arcGeom(e, g)) { add(g.c - Vec2(g.r)); add(g.c + Vec2(g.r)); }
        } else if (e.type == EntType::Ellipse || e.type == EntType::EllipArc) {
            EllipseGeom g;
            if (ellipseGeom(e, g))
                for (int i = 0; i < 16; ++i) add(ellipsePoint(g, (double)i / 16.0));
        }
    }
}

int Sketch::geometryCount() const {
    int n = 0;
    for (auto& [id, e] : ents) if (e.type != EntType::Point && !e.locked) ++n;
    return n;
}

} // namespace kst::sk
