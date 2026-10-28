#include "sketch/Profiles.h"
#include <algorithm>
#include <functional>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace kst::sk {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2 * kPi;

struct Curve {
    uint32_t ent = 0;
    EntType type = EntType::Line;
    Vec2 a{0}, b{0};               // line endpoints
    Sketch::ArcGeom g;             // arc or circle, or the ellipse center, major, and sweep
    double minor = 0, rot = 0;     // ellipse
    bool closed = false;
    bool chord = false;            // a sampled piece of a spline
    std::vector<double> splits;    // parameters in [0,1]

    bool ellipse() const { return type == EntType::Ellipse || type == EntType::EllipArc; }
    Vec2 at(double t) const {
        if (type == EntType::Line) return a + (b - a) * t;
        double ang = g.a0 + g.sweep * t;
        if (ellipse()) {
            double lx = g.r * std::cos(ang), ly = minor * std::sin(ang);
            double cs = std::cos(rot), sn = std::sin(rot);
            return g.c + Vec2(cs * lx - sn * ly, sn * lx + cs * ly);
        }
        return g.c + Vec2(std::cos(ang), std::sin(ang)) * g.r;
    }
    Vec2 tangent(double t) const {   // unit, direction of increasing t
        if (type == EntType::Line) {
            Vec2 d = b - a;
            double l = glm::length(d);
            return l > 1e-15 ? d / l : Vec2(1, 0);
        }
        double ang = g.a0 + g.sweep * t;
        Vec2 d = ellipse()
            ? [&] {
                double lx = -g.r * std::sin(ang), ly = minor * std::cos(ang);
                double cs = std::cos(rot), sn = std::sin(rot);
                return Vec2(cs * lx - sn * ly, sn * lx + cs * ly);
            }()
            : Vec2(-std::sin(ang), std::cos(ang));
        double l = glm::length(d);
        return l > 1e-15 ? d / l : Vec2(1, 0);
    }
    double curvature() const { return type == EntType::Line ? 0.0 : 1.0 / std::max(g.r, 1e-12); }
};

struct Edge {
    int curve = 0;
    double ta = 0, tb = 1;
    int v0 = 0, v1 = 0;
    bool dead = false;
};

double wrap2pi(double a) {
    a = std::fmod(a, kTwoPi);
    if (a < 0) a += kTwoPi;
    return a;
}

// Parameter of an angle on an arc, or -1 if outside the sweep (with a small tolerance).
double arcParam(const Curve& c, double ang, double tolT) {
    if (c.closed) return wrap2pi(ang - c.g.a0) / kTwoPi;
    double rel = wrap2pi(ang - c.g.a0);
    double t = rel / c.g.sweep;
    if (t <= 1.0 + tolT) return std::min(t, 1.0);
    // Just before the start counts as the start.
    double relNeg = rel - kTwoPi;
    if (relNeg / c.g.sweep >= -tolT) return 0.0;
    return -1.0;
}

void intersectLineLine(const Curve& p, const Curve& q, std::vector<std::pair<double, double>>& out, double tolT) {
    Vec2 r = p.b - p.a, s = q.b - q.a;
    double den = r.x * s.y - r.y * s.x;
    double rl = glm::length(r), sl = glm::length(s);
    if (rl < 1e-12 || sl < 1e-12) return;
    if (std::abs(den) > 1e-10 * rl * sl) {
        Vec2 w = q.a - p.a;
        double t = (w.x * s.y - w.y * s.x) / den;
        double u = (w.x * r.y - w.y * r.x) / den;
        double tt = tolT / rl * 0 + 1e-9;
        if (t >= -tt - tolT / rl && t <= 1 + tt + tolT / rl && u >= -tt - tolT / sl && u <= 1 + tt + tolT / sl)
            out.push_back({std::clamp(t, 0.0, 1.0), std::clamp(u, 0.0, 1.0)});
        return;
    }
    // Parallel. If collinear, split each at the other's endpoints that lie inside it.
    double dist = std::abs(r.x * (q.a - p.a).y - r.y * (q.a - p.a).x) / rl;
    if (dist > 1e-7) return;
    for (int k = 0; k < 2; ++k) {
        Vec2 e = k ? q.b : q.a;
        double t = glm::dot(e - p.a, r) / (rl * rl);
        if (t > 1e-9 && t < 1 - 1e-9) out.push_back({t, (double)k});
    }
    for (int k = 0; k < 2; ++k) {
        Vec2 e = k ? p.b : p.a;
        double u = glm::dot(e - q.a, s) / (sl * sl);
        if (u > 1e-9 && u < 1 - 1e-9) out.push_back({(double)k, u});
    }
}

void intersectLineArc(const Curve& l, const Curve& a, std::vector<std::pair<double, double>>& out, double tolT) {
    Vec2 d = l.b - l.a;
    double L2 = glm::dot(d, d);
    if (L2 < 1e-18) return;
    Vec2 f = l.a - a.g.c;
    double A = L2, B = 2 * glm::dot(f, d), C = glm::dot(f, f) - a.g.r * a.g.r;
    double disc = B * B - 4 * A * C;
    double scale = std::max(1.0, B * B);
    if (disc < -1e-9 * scale) return;
    std::vector<double> ts;
    if (disc < 1e-9 * scale) ts.push_back(-B / (2 * A));
    else {
        double sq = std::sqrt(disc);
        ts.push_back((-B - sq) / (2 * A));
        ts.push_back((-B + sq) / (2 * A));
    }
    double ll = std::sqrt(L2);
    for (double t : ts) {
        if (t < -tolT / ll || t > 1 + tolT / ll) continue;
        t = std::clamp(t, 0.0, 1.0);
        Vec2 p = l.at(t) - a.g.c;
        double u = arcParam(a, std::atan2(p.y, p.x), tolT / std::max(a.g.r * a.g.sweep, 1e-9));
        if (u < 0) continue;
        out.push_back({t, u});
    }
}

void intersectArcArc(const Curve& p, const Curve& q, std::vector<std::pair<double, double>>& out, double tolT) {
    Vec2 d = q.g.c - p.g.c;
    double dl = glm::length(d);
    double r0 = p.g.r, r1 = q.g.r;
    if (dl < 1e-12) return;   // concentric: no crossing
    if (dl > r0 + r1 + 1e-9 || dl < std::abs(r0 - r1) - 1e-9) return;
    double a = (r0 * r0 - r1 * r1 + dl * dl) / (2 * dl);
    double h2 = r0 * r0 - a * a;
    double h = h2 > 0 ? std::sqrt(h2) : 0.0;
    Vec2 u = d / dl, perp(-u.y, u.x);
    Vec2 mid = p.g.c + u * a;
    int cnt = h < 1e-9 ? 1 : 2;
    for (int k = 0; k < cnt; ++k) {
        Vec2 pt = k == 0 ? mid + perp * h : mid - perp * h;
        Vec2 v0 = pt - p.g.c, v1 = pt - q.g.c;
        double t = arcParam(p, std::atan2(v0.y, v0.x), tolT / std::max(r0 * p.g.sweep, 1e-9));
        double s = arcParam(q, std::atan2(v1.y, v1.x), tolT / std::max(r1 * q.g.sweep, 1e-9));
        if (t < 0 || s < 0) continue;
        out.push_back({t, s});
    }
}

struct VertexMap {
    double tol;
    std::vector<Vec2> pos;
    std::unordered_map<long long, std::vector<int>> grid;
    explicit VertexMap(double t) : tol(t) {}
    long long key(long long ix, long long iy) const { return ix * 73856093LL ^ iy * 19349663LL; }
    int get(Vec2 p) {
        long long ix = (long long)std::floor(p.x / (tol * 4)), iy = (long long)std::floor(p.y / (tol * 4));
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy) {
                auto it = grid.find(key(ix + dx, iy + dy));
                if (it == grid.end()) continue;
                for (int id : it->second)
                    if (glm::length(pos[id] - p) <= tol) return id;
            }
        int id = (int)pos.size();
        pos.push_back(p);
        grid[key(ix, iy)].push_back(id);
        return id;
    }
};

struct HalfEdge {
    int edge = 0;
    bool fwd = true;
    int from = 0, to = 0;
    double ang = 0, kappa = 0;
};

double polyArea(const std::vector<Vec2>& p) {
    double a = 0;
    for (size_t i = 0, n = p.size(); i < n; ++i) {
        const Vec2& u = p[i];
        const Vec2& v = p[(i + 1) % n];
        a += u.x * v.y - v.x * u.y;
    }
    return a * 0.5;
}

bool pointInPoly(const std::vector<Vec2>& poly, Vec2 q) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2& a = poly[i];
        const Vec2& b = poly[j];
        if (((a.y > q.y) != (b.y > q.y)) && (q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x)) in = !in;
    }
    return in;
}

// ------------------------------------------------------------------ triangulation (ear clipping with hole bridges)

double cross3(Vec2 a, Vec2 b, Vec2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

bool segCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    double d1 = cross3(a, b, c), d2 = cross3(a, b, d), d3 = cross3(c, d, a), d4 = cross3(c, d, b);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

bool segHitsLoop(Vec2 a, Vec2 b, const std::vector<Vec2>& poly) {
    size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2& c = poly[i];
        const Vec2& d = poly[(i + 1) % n];
        if (glm::length(c - a) < 1e-12 || glm::length(d - a) < 1e-12 || glm::length(c - b) < 1e-12 || glm::length(d - b) < 1e-12) continue;
        if (segCross(a, b, c, d)) return true;
    }
    return false;
}

void earClip(std::vector<Vec2> poly, std::vector<Vec2>& tris) {
    int n = (int)poly.size();
    if (n < 3) return;
    std::vector<int> prev(n), next(n);
    for (int i = 0; i < n; ++i) { prev[i] = (i + n - 1) % n; next[i] = (i + 1) % n; }
    int remaining = n, cur = 0, guard = 0;
    auto inside = [&](Vec2 p, Vec2 a, Vec2 b, Vec2 c) {
        double d1 = cross3(a, b, p), d2 = cross3(b, c, p), d3 = cross3(c, a, p);
        return d1 > 1e-12 && d2 > 1e-12 && d3 > 1e-12;
    };
    while (remaining > 3 && guard < n * n + 16) {
        ++guard;
        bool clipped = false;
        int start = cur;
        double bestAbs = 1e300;
        int bestFallback = -1;
        do {
            int p = prev[cur], nx = next[cur];
            double cr = cross3(poly[p], poly[cur], poly[nx]);
            if (std::abs(cr) < bestAbs) { bestAbs = std::abs(cr); bestFallback = cur; }
            if (cr > 1e-14) {
                bool ear = true;
                for (int k = next[nx]; k != p; k = next[k]) {
                    const Vec2& q = poly[k];
                    if (glm::length(q - poly[p]) < 1e-12 || glm::length(q - poly[cur]) < 1e-12 || glm::length(q - poly[nx]) < 1e-12) continue;
                    if (inside(q, poly[p], poly[cur], poly[nx])) { ear = false; break; }
                }
                if (ear) {
                    tris.push_back(poly[p]); tris.push_back(poly[cur]); tris.push_back(poly[nx]);
                    next[p] = nx; prev[nx] = p;
                    --remaining;
                    cur = nx;
                    clipped = true;
                    break;
                }
            }
            cur = nx;
        } while (cur != start);
        if (!clipped) {
            // Degenerate geometry: drop the flattest vertex so we always make progress.
            int p = prev[bestFallback], nx = next[bestFallback];
            next[p] = nx; prev[nx] = p;
            --remaining;
            cur = nx;
        }
    }
    if (remaining == 3) {
        int p = prev[cur], nx = next[cur];
        if (cross3(poly[p], poly[cur], poly[nx]) > 1e-14) {
            tris.push_back(poly[p]); tris.push_back(poly[cur]); tris.push_back(poly[nx]);
        }
    }
}

void triangulate(const std::vector<Vec2>& outerIn, const std::vector<const std::vector<Vec2>*>& holesIn, std::vector<Vec2>& tris) {
    std::vector<Vec2> poly = outerIn;
    if (polyArea(poly) < 0) std::reverse(poly.begin(), poly.end());
    std::vector<std::vector<Vec2>> holes;
    for (auto* h : holesIn) {
        std::vector<Vec2> hp = *h;
        if (polyArea(hp) > 0) std::reverse(hp.begin(), hp.end());   // holes clockwise
        holes.push_back(std::move(hp));
    }
    std::sort(holes.begin(), holes.end(), [](const auto& A, const auto& B) {
        double ma = -1e300, mb = -1e300;
        for (auto& p : A) ma = std::max(ma, p.x);
        for (auto& p : B) mb = std::max(mb, p.x);
        return ma > mb;
    });
    for (size_t hi = 0; hi < holes.size(); ++hi) {
        auto& hole = holes[hi];
        if (hole.size() < 3) continue;
        size_t im = 0;
        for (size_t i = 1; i < hole.size(); ++i) if (hole[i].x > hole[im].x) im = i;
        Vec2 M = hole[im];
        std::vector<size_t> order(poly.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return glm::length(poly[a] - M) < glm::length(poly[b] - M); });
        long chosen = -1;
        for (size_t idx : order) {
            Vec2 V = poly[idx];
            if (segHitsLoop(M, V, poly)) continue;
            bool bad = false;
            for (size_t hj = hi; hj < holes.size() && !bad; ++hj) bad = segHitsLoop(M, V, holes[hj]);
            if (bad) continue;
            if (!pointInPoly(poly, (M + V) * 0.5)) continue;
            chosen = (long)idx;
            break;
        }
        if (chosen < 0) continue;   // cannot bridge: leave this hole filled (display only)
        std::vector<Vec2> merged;
        merged.reserve(poly.size() + hole.size() + 2);
        for (size_t i = 0; i <= (size_t)chosen; ++i) merged.push_back(poly[i]);
        for (size_t k = 0; k <= hole.size(); ++k) merged.push_back(hole[(im + k) % hole.size()]);
        for (size_t i = (size_t)chosen; i < poly.size(); ++i) merged.push_back(poly[i]);
        poly.swap(merged);
    }
    if (poly.size() > 4000) {   // safety: never spend seconds on a display fill
        for (size_t i = 1; i + 1 < poly.size(); ++i) { tris.push_back(poly[0]); tris.push_back(poly[i]); tris.push_back(poly[i + 1]); }
        return;
    }
    earClip(poly, tris);
}

} // namespace

bool pointInProfile(const Profile& p, Vec2 q) {
    if (p.outer.poly.size() < 3 || !pointInPoly(p.outer.poly, q)) return false;
    for (auto& h : p.holes)
        if (h.poly.size() >= 3 && pointInPoly(h.poly, q)) return false;
    return true;
}

namespace {
bool makeCurve(const Sketch& sk, const Entity& e, Curve& c) {
    c.ent = e.id;
    c.type = e.type;
    if (e.type == EntType::Line) {
        c.a = sk.pt(e.a); c.b = sk.pt(e.b);
        return glm::length(c.b - c.a) >= 1e-9;
    }
    if (e.type == EntType::Circle || e.type == EntType::Arc) {
        if (!sk.arcGeom(e, c.g) || c.g.r < 1e-9) return false;
        c.closed = e.type == EntType::Circle;
        return true;
    }
    if (e.type == EntType::Ellipse || e.type == EntType::EllipArc) {
        Sketch::EllipseGeom eg;
        if (!sk.ellipseGeom(e, eg)) return false;
        c.g.c = eg.c; c.g.r = eg.major; c.g.a0 = eg.a0; c.g.sweep = eg.sweep;
        c.minor = eg.minor; c.rot = eg.rot;
        c.closed = e.type == EntType::Ellipse;
        return true;
    }
    return false;
}
} // namespace

std::vector<double> curveSplitParams(const Sketch& sk, uint32_t ent) {
    std::vector<double> out;
    try {
        const Entity* e = sk.find(ent);
        Curve me;
        if (!e || !makeCurve(sk, *e, me)) return out;
        if (!me.closed) { out.push_back(0.0); out.push_back(1.0); }
        std::vector<std::pair<double, double>> hits;
        for (auto& [id, o] : sk.ents) {
            if (id == ent || o.locked) continue;
            Curve other;
            if (!makeCurve(sk, o, other)) continue;
            hits.clear();
            const double tol = 1e-7;
            if (me.type == EntType::Line && other.type == EntType::Line) intersectLineLine(me, other, hits, tol);
            else if (me.type == EntType::Line) intersectLineArc(me, other, hits, tol);
            else if (other.type == EntType::Line) {
                std::vector<std::pair<double, double>> tmp;
                intersectLineArc(other, me, tmp, tol);
                for (auto& h : tmp) hits.push_back({h.second, h.first});
            } else intersectArcArc(me, other, hits, tol);
            for (auto& h : hits) out.push_back(h.first);
        }
        std::sort(out.begin(), out.end());
        std::vector<double> uniq;
        for (double t : out) {
            if (me.closed && t >= 1.0 - 1e-9) t = 0.0;
            if (uniq.empty() || std::abs(uniq.back() - t) > 1e-9) uniq.push_back(t);
        }
        if (me.closed) { std::sort(uniq.begin(), uniq.end()); uniq.erase(std::unique(uniq.begin(), uniq.end(), [](double a, double b) { return std::abs(a - b) < 1e-9; }), uniq.end()); }
        out = std::move(uniq);
    } catch (...) { out.clear(); }
    return out;
}

namespace {
struct CrossHit { uint32_t a = 0, b = 0; Vec2 at{0}; };

void allCrossings(const Sketch& sk, std::vector<CrossHit>& out) {
    std::vector<Curve> curves;
    for (auto& [id, e] : sk.ents) {
        if (e.type == EntType::Ellipse || e.type == EntType::EllipArc || e.type == EntType::Spline || e.type == EntType::CtrlSpline)
            continue;
        Curve c;
        if (!makeCurve(sk, e, c)) continue;
        curves.push_back(c);
    }
    const double tol = 1e-7;
    for (size_t i = 0; i < curves.size(); ++i) {
        for (size_t j = i + 1; j < curves.size(); ++j) {
            const Curve& A = curves[i];
            const Curve& B = curves[j];
            const Entity* ei = sk.find(A.ent);
            const Entity* ej = sk.find(B.ent);
            if (ei && ej && ei->locked && ej->locked) continue;
            std::vector<std::pair<double, double>> hits;
            if (A.type == EntType::Line && B.type == EntType::Line) {
                Vec2 r = A.b - A.a, s = B.b - B.a;
                double rl = glm::length(r), sl = glm::length(s);
                if (rl < 1e-12 || sl < 1e-12) continue;
                if (std::abs(r.x * s.y - r.y * s.x) <= 1e-10 * rl * sl) continue;
                intersectLineLine(A, B, hits, tol);
            } else if (A.type == EntType::Line) intersectLineArc(A, B, hits, tol);
            else if (B.type == EntType::Line) {
                std::vector<std::pair<double, double>> tmp;
                intersectLineArc(B, A, tmp, tol);
                for (auto& h : tmp) hits.push_back({h.second, h.first});
            } else intersectArcArc(A, B, hits, tol);
            for (auto& h : hits) {
                Vec2 p = A.at(h.first);
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) continue;
                out.push_back({A.ent, B.ent, p});
            }
        }
    }
}
} // namespace

bool nearestCrossing(const Sketch& sk, Vec2 q, double maxDist, uint32_t& entA, uint32_t& entB, Vec2& at) {
    std::vector<CrossHit> hits;
    allCrossings(sk, hits);
    double best = maxDist;
    bool found = false;
    for (const CrossHit& h : hits) {
        double d = glm::length(h.at - q);
        if (d <= best) { best = d; found = true; entA = h.a; entB = h.b; at = h.at; }
    }
    return found;
}

void crossingsOf(const Sketch& sk, uint32_t a, uint32_t b, std::vector<Vec2>& out) {
    out.clear();
    std::vector<CrossHit> hits;
    allCrossings(sk, hits);
    for (const CrossHit& h : hits)
        if ((h.a == a && h.b == b) || (h.a == b && h.b == a)) out.push_back(h.at);
}

double curveParamAt(const Sketch& sk, uint32_t ent, Vec2 q) {
    const Entity* e = sk.find(ent);
    Curve c;
    if (!e || !makeCurve(sk, *e, c)) return 0.0;
    if (c.type == EntType::Line) {
        Vec2 d = c.b - c.a;
        return std::clamp(glm::dot(q - c.a, d) / glm::dot(d, d), 0.0, 1.0);
    }
    Vec2 v = q - c.g.c;
    double rel = wrap2pi(std::atan2(v.y, v.x) - c.g.a0);
    if (c.closed) return rel / kTwoPi;
    double t = rel / c.g.sweep;
    if (t > 1.0) {   // beyond the end: snap to the nearer end
        double over = rel - c.g.sweep, under = kTwoPi - rel;
        return over < under ? 1.0 : 0.0;
    }
    return t;
}

std::vector<Profile> findProfiles(const Sketch& sk) {
    std::vector<Profile> result;
    try {
        // ---- curves
        std::vector<Curve> curves;
        for (auto& [id, e] : sk.ents) {
            if (e.construction || e.locked) continue;
            Curve c;
            c.ent = id;
            c.type = e.type;
            if (e.type == EntType::Line) {
                c.a = sk.pt(e.a); c.b = sk.pt(e.b);
                if (glm::length(c.b - c.a) < 1e-9) continue;
            } else if (e.type == EntType::Circle || e.type == EntType::Arc) {
                if (!sk.arcGeom(e, c.g) || c.g.r < 1e-9) continue;
                c.closed = e.type == EntType::Circle;
            } else if (e.type == EntType::Ellipse || e.type == EntType::EllipArc) {
                Sketch::EllipseGeom eg;
                if (!sk.ellipseGeom(e, eg)) continue;
                c.g.c = eg.c; c.g.r = eg.major; c.g.a0 = eg.a0; c.g.sweep = eg.sweep;
                c.minor = eg.minor; c.rot = eg.rot;
                c.closed = e.type == EntType::Ellipse;
            } else if (e.type == EntType::Spline || e.type == EntType::CtrlSpline) {
                auto poly = e.type == EntType::CtrlSpline ? sk.ctrlSplinePoly(e, 8) : sk.splinePoly(e, 8);
                for (size_t i = 0; i + 1 < poly.size(); ++i) {
                    if (glm::length(poly[i + 1] - poly[i]) < 1e-9) continue;
                    Curve piece;
                    piece.ent = id;
                    piece.type = EntType::Line;
                    piece.a = poly[i];
                    piece.b = poly[i + 1];
                    piece.chord = true;
                    curves.push_back(std::move(piece));
                }
                continue;
            } else continue;
            curves.push_back(std::move(c));
        }
        if (curves.empty()) return result;

        Vec2 lo(1e30), hi(-1e30);
        for (auto& c : curves) {
            if (c.type == EntType::Line) { lo = glm::min(lo, glm::min(c.a, c.b)); hi = glm::max(hi, glm::max(c.a, c.b)); }
            else { lo = glm::min(lo, c.g.c - Vec2(c.g.r)); hi = glm::max(hi, c.g.c + Vec2(c.g.r)); }
        }
        double scale = std::max(1.0, glm::length(hi - lo));
        double tolV = 1e-6 * scale;
        double tolT = tolV;

        for (auto& c : curves) { c.splits.push_back(0.0); if (!c.closed) c.splits.push_back(1.0); }

        // ---- intersections
        std::vector<std::pair<double, double>> hits;
        for (size_t i = 0; i < curves.size(); ++i) {
            for (size_t j = i + 1; j < curves.size(); ++j) {
                Curve &p = curves[i], &q = curves[j];
                hits.clear();
                if (p.type == EntType::Line && q.type == EntType::Line) intersectLineLine(p, q, hits, tolT);
                else if (p.type == EntType::Line) intersectLineArc(p, q, hits, tolT);
                else if (q.type == EntType::Line) {
                    std::vector<std::pair<double, double>> tmp;
                    intersectLineArc(q, p, tmp, tolT);
                    for (auto& h : tmp) hits.push_back({h.second, h.first});
                } else intersectArcArc(p, q, hits, tolT);
                for (auto& h : hits) { p.splits.push_back(h.first); q.splits.push_back(h.second); }
            }
        }

        // ---- vertices and edges
        VertexMap vm(tolV);
        std::vector<Edge> edges;
        for (size_t ci = 0; ci < curves.size(); ++ci) {
            Curve& c = curves[ci];
            std::sort(c.splits.begin(), c.splits.end());
            struct S { double t; int v; };
            std::vector<S> list;
            for (double t : c.splits) {
                if (c.closed && t >= 1.0 - 1e-12) t = 0.0;
                int v = vm.get(c.at(t));
                if (!list.empty() && list.back().v == v) continue;
                // Different parameter, same vertex (only possible for the circle seam).
                bool dup = false;
                for (auto& s : list) if (s.v == v) { dup = true; break; }
                if (dup) continue;
                list.push_back({t, v});
            }
            if (c.closed) {
                if (list.size() == 1) {
                    Edge e; e.curve = (int)ci; e.ta = list[0].t; e.tb = list[0].t + 1.0; e.v0 = e.v1 = list[0].v;
                    edges.push_back(e);
                } else {
                    for (size_t k = 0; k < list.size(); ++k) {
                        Edge e; e.curve = (int)ci;
                        e.ta = list[k].t;
                        e.tb = k + 1 < list.size() ? list[k + 1].t : list[0].t + 1.0;
                        e.v0 = list[k].v; e.v1 = list[(k + 1) % list.size()].v;
                        edges.push_back(e);
                    }
                }
            } else {
                for (size_t k = 0; k + 1 < list.size(); ++k) {
                    Edge e; e.curve = (int)ci;
                    e.ta = list[k].t; e.tb = list[k + 1].t;
                    e.v0 = list[k].v; e.v1 = list[k + 1].v;
                    if (e.v0 == e.v1) continue;
                    edges.push_back(e);
                }
            }
        }
        auto edgeMid = [&](const Edge& e) { return curves[e.curve].at(0.5 * (e.ta + e.tb)); };
        // Drop duplicate (overlapping) edges.
        for (size_t i = 0; i < edges.size(); ++i) {
            if (edges[i].dead) continue;
            for (size_t j = i + 1; j < edges.size(); ++j) {
                if (edges[j].dead) continue;
                bool sameV = (edges[i].v0 == edges[j].v0 && edges[i].v1 == edges[j].v1) || (edges[i].v0 == edges[j].v1 && edges[i].v1 == edges[j].v0);
                if (sameV && glm::length(edgeMid(edges[i]) - edgeMid(edges[j])) < tolV * 10) edges[j].dead = true;
            }
        }
        // Prune dangling edges repeatedly.
        const int nv = (int)vm.pos.size();
        for (bool changed = true; changed;) {
            changed = false;
            std::vector<int> deg(nv, 0);
            for (auto& e : edges) if (!e.dead) { deg[e.v0]++; deg[e.v1]++; }
            for (auto& e : edges) {
                if (e.dead) continue;
                if (e.v0 != e.v1 && (deg[e.v0] < 2 || deg[e.v1] < 2)) { e.dead = true; changed = true; }
            }
        }

        // ---- half edges, sorted around vertices
        std::vector<HalfEdge> hes;
        std::vector<int> edgeIndexMap;
        for (size_t i = 0; i < edges.size(); ++i) {
            const Edge& e = edges[i];
            if (e.dead) continue;
            const Curve& c = curves[e.curve];
            HalfEdge f, b;
            f.edge = b.edge = (int)i;
            f.fwd = true; b.fwd = false;
            f.from = e.v0; f.to = e.v1;
            b.from = e.v1; b.to = e.v0;
            Vec2 tf = c.tangent(e.ta), tbk = -c.tangent(e.tb);
            f.ang = std::atan2(tf.y, tf.x);
            b.ang = std::atan2(tbk.y, tbk.x);
            f.kappa = c.curvature();
            b.kappa = -c.curvature();
            hes.push_back(f);
            hes.push_back(b);
        }
        if (hes.empty()) return result;
        const int H = (int)hes.size();
        std::vector<std::vector<int>> out(nv);
        for (int h = 0; h < H; ++h) out[hes[h].from].push_back(h);
        std::vector<int> posInVertex(H, 0);
        for (int v = 0; v < nv; ++v) {
            auto& list = out[v];
            std::sort(list.begin(), list.end(), [&](int x, int y) {
                double da = hes[x].ang - hes[y].ang;
                if (std::abs(da) > 1e-9) return da < 0;
                return hes[x].kappa < hes[y].kappa;
            });
            for (size_t k = 0; k < list.size(); ++k) posInVertex[list[k]] = (int)k;
        }
        auto twin = [](int h) { return h ^ 1; };
        auto nextHe = [&](int h) {
            int t = twin(h);
            const auto& list = out[hes[t].from];
            int k = posInVertex[t];
            int n = (int)list.size();
            return list[(k + n - 1) % n];
        };

        // ---- trace loops
        struct RawLoop { std::vector<int> hs; Loop loop; int comp = -1; };
        std::vector<RawLoop> loops;
        std::vector<char> used(H, 0);
        auto sampleEdge = [&](const Edge& e, bool fwd, std::vector<Vec2>& poly) {
            const Curve& c = curves[e.curve];
            double t0 = fwd ? e.ta : e.tb, t1 = fwd ? e.tb : e.ta;
            int n = 1;
            if (c.type != EntType::Line) n = std::max(2, (int)std::ceil(std::abs(t1 - t0) * c.g.sweep / (kPi / 60.0)));
            for (int k = 0; k < n; ++k) poly.push_back(c.at(t0 + (t1 - t0) * k / n));
        };
        for (int h0 = 0; h0 < H; ++h0) {
            if (used[h0]) continue;
            RawLoop rl;
            int h = h0, guard = 0;
            do {
                used[h] = 1;
                rl.hs.push_back(h);
                h = nextHe(h);
            } while (h != h0 && ++guard < H + 2);
            if (h != h0) continue;   // inconsistent topology, skip
            for (int hh : rl.hs) {
                const Edge& e = edges[hes[hh].edge];
                const Curve& c = curves[e.curve];
                ProfEdge pe;
                pe.ent = c.ent;
                pe.ta = hes[hh].fwd ? e.ta : e.tb;
                pe.tb = hes[hh].fwd ? e.tb : e.ta;
                if (c.chord) { pe.chord = true; pe.ca = c.a; pe.cb = c.b; }
                rl.loop.edges.push_back(pe);
                sampleEdge(e, hes[hh].fwd, rl.loop.poly);
            }
            rl.loop.area = polyArea(rl.loop.poly);
            loops.push_back(std::move(rl));
        }

        // ---- components (union find over vertices)
        std::vector<int> parent(nv);
        std::iota(parent.begin(), parent.end(), 0);
        std::function<int(int)> find = [&](int x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
        for (auto& e : edges) if (!e.dead) parent[find(e.v0)] = find(e.v1);
        for (auto& rl : loops) rl.comp = find(hes[rl.hs[0]].from);

        struct CompOuter { int loop = -1; double area = 0; };
        std::unordered_map<int, CompOuter> outers;
        std::vector<int> faces;
        double areaEps = 1e-10 * scale * scale;
        for (int i = 0; i < (int)loops.size(); ++i) {
            double a = loops[i].loop.area;
            if (a > areaEps) faces.push_back(i);
            else if (a < -areaEps) {
                auto& co = outers[loops[i].comp];
                if (co.loop < 0 || a < co.area) { co.loop = i; co.area = a; }
            }
        }
        std::unordered_map<int, std::vector<int>> holesOf;   // face loop index -> hole loop indices
        for (auto& [comp, co] : outers) {
            if (co.loop < 0) continue;
            const auto& poly = loops[co.loop].loop.poly;
            if (poly.empty()) continue;
            Vec2 q = poly[0];
            int best = -1;
            double bestArea = 1e300;
            for (int f : faces) {
                if (loops[f].comp == comp) continue;
                if (loops[f].loop.area < bestArea && pointInPoly(loops[f].loop.poly, q)) { best = f; bestArea = loops[f].loop.area; }
            }
            if (best >= 0) holesOf[best].push_back(co.loop);
        }

        for (int f : faces) {
            Profile p;
            p.outer = loops[f].loop;
            double a = p.outer.area;
            std::vector<const std::vector<Vec2>*> hp;
            for (int hidx : holesOf[f]) {
                p.holes.push_back(loops[hidx].loop);
                a += loops[hidx].loop.area;   // negative
            }
            for (auto& h : p.holes) hp.push_back(&h.poly);
            p.area = a;
            triangulate(p.outer.poly, hp, p.tris);
            p.lo = Vec2(1e30); p.hi = Vec2(-1e30);
            for (auto& q : p.outer.poly) { p.lo = glm::min(p.lo, q); p.hi = glm::max(p.hi, q); }
            result.push_back(std::move(p));
        }
        std::sort(result.begin(), result.end(), [](const Profile& a, const Profile& b) { return a.area > b.area; });
    } catch (...) {
        result.clear();
    }
    return result;
}

} // namespace kst::sk
