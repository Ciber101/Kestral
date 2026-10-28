#include "sketch/Solver.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace kst::sk {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Term {
    enum Kind { Con, ArcEq, Drag } kind = Con;
    const Constraint* con = nullptr;
    const Entity* ent = nullptr;     // arc for ArcEq
    int nres = 1;
    double weight = 1.0;
    std::vector<int> params;         // param indices this term depends on (all, including constants)
    std::vector<int> vars;           // variable indices (subset of params that are unknowns)
    uint32_t dragPoint = 0;
    Vec2 dragTarget{0, 0};
};

struct Problem {
    Sketch& sk;
    std::vector<double> vals;
    std::vector<int> varOfParam;
    std::vector<int> paramOfVar;
    std::vector<int> base;                 // entity id -> first param
    std::vector<const Entity*> ent;        // entity id -> entity
    std::vector<Term> terms;
    int nVars = 0;

    explicit Problem(Sketch& s) : sk(s) {}

    Vec2 P(uint32_t id) const { int b = base[id]; return {vals[b], vals[b + 1]}; }
    double Rad(uint32_t id) const {
        const Entity* e = ent[id];
        if (e->type == EntType::Circle) return vals[base[id]];
        return glm::length(P(e->a) - P(e->c));
    }
    Vec2 LA(const Entity* l) const { return P(l->a); }
    Vec2 LB(const Entity* l) const { return P(l->b); }
};

void addParamsOf(const Problem& pr, uint32_t id, std::vector<int>& out) {
    if (id >= pr.ent.size() || !pr.ent[id]) return;
    const Entity* e = pr.ent[id];
    auto pt = [&](uint32_t p) { if (p < pr.base.size() && pr.base[p] >= 0) { out.push_back(pr.base[p]); out.push_back(pr.base[p] + 1); } };
    switch (e->type) {
    case EntType::Point: pt(id); break;
    case EntType::Line: pt(e->a); pt(e->b); break;
    case EntType::Circle: pt(e->c); out.push_back(pr.base[id]); break;
    case EntType::Arc: pt(e->c); pt(e->a); pt(e->b); break;
    case EntType::Spline:
    case EntType::CtrlSpline: for (uint32_t p : e->pts) pt(p); break;
    }
}

bool conValid(const Problem& pr, const Constraint& c) {
    for (int k = 0; k < 3; ++k)
        if (c.e[k] && (c.e[k] >= pr.ent.size() || !pr.ent[c.e[k]])) return false;
    return true;
}

double wrapPi(double a) {
    while (a > kPi) a -= 2 * kPi;
    while (a <= -kPi) a += 2 * kPi;
    return a;
}

inline double cross2(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline Vec2 unit(Vec2 d) {
    double l = glm::length(d);
    return l > 1e-12 ? d / l : Vec2(1, 0);
}

int nResiduals(const Constraint& c, const Problem& pr) {
    switch (c.type) {
    case CType::Coincident: case CType::Midpoint: case CType::Symmetric: case CType::Fix: case CType::Collinear: return 2;
    default: return 1;
    }
    (void)pr;
}

// Evaluates the residuals of a term at the current values.
void evalTerm(const Problem& pr, const Term& t, double* out) {
    if (t.kind == Term::ArcEq) {
        const Entity* e = t.ent;
        out[0] = glm::length(pr.P(e->a) - pr.P(e->c)) - glm::length(pr.P(e->b) - pr.P(e->c));
        return;
    }
    if (t.kind == Term::Drag) {
        Vec2 p = pr.P(t.dragPoint);
        out[0] = (p.x - t.dragTarget.x) * t.weight;
        out[1] = (p.y - t.dragTarget.y) * t.weight;
        return;
    }
    const Constraint& c = *t.con;
    auto E = [&](int k) { return pr.ent[c.e[k]]; };
    switch (c.type) {
    case CType::Coincident: {
        Vec2 d = pr.P(c.e[0]) - pr.P(c.e[1]);
        out[0] = d.x; out[1] = d.y;
        break;
    }
    case CType::PointOnObject: {
        Vec2 p = pr.P(c.e[0]);
        const Entity* o = E(1);
        if (o->type == EntType::Line) {
            Vec2 a = pr.LA(o), b = pr.LB(o);
            out[0] = cross2(unit(b - a), p - a);
        } else {
            Vec2 cc = pr.P(o->type == EntType::Circle ? o->c : o->c);
            out[0] = glm::length(p - cc) - pr.Rad(c.e[1]);
        }
        break;
    }
    case CType::Horizontal: case CType::Vertical: {
        Vec2 a, b;
        if (c.e[1]) { a = pr.P(c.e[0]); b = pr.P(c.e[1]); }
        else { const Entity* l = E(0); a = pr.LA(l); b = pr.LB(l); }
        out[0] = c.type == CType::Horizontal ? b.y - a.y : b.x - a.x;
        break;
    }
    case CType::Parallel: {
        const Entity *l0 = E(0), *l1 = E(1);
        out[0] = cross2(unit(pr.LB(l0) - pr.LA(l0)), unit(pr.LB(l1) - pr.LA(l1)));
        break;
    }
    case CType::Perpendicular: {
        const Entity *l0 = E(0), *l1 = E(1);
        out[0] = glm::dot(unit(pr.LB(l0) - pr.LA(l0)), unit(pr.LB(l1) - pr.LA(l1)));
        break;
    }
    case CType::Equal: {
        const Entity *o0 = E(0), *o1 = E(1);
        if (o0->type == EntType::Line && o1->type == EntType::Line)
            out[0] = glm::length(pr.LB(o0) - pr.LA(o0)) - glm::length(pr.LB(o1) - pr.LA(o1));
        else
            out[0] = pr.Rad(c.e[0]) - pr.Rad(c.e[1]);
        break;
    }
    case CType::Tangent: {
        const Entity *o0 = E(0), *o1 = E(1);
        if (o0->type == EntType::Line || o1->type == EntType::Line) {
            uint32_t lid = o0->type == EntType::Line ? c.e[0] : c.e[1];
            uint32_t cid = o0->type == EntType::Line ? c.e[1] : c.e[0];
            const Entity* l = pr.ent[lid];
            Vec2 a = pr.LA(l), b = pr.LB(l);
            Vec2 cc = pr.P(pr.ent[cid]->c);
            double s = cross2(unit(b - a), cc - a);
            out[0] = std::sqrt(s * s + 1e-18) - pr.Rad(cid);
        } else {
            Vec2 c0 = pr.P(o0->c), c1 = pr.P(o1->c);
            double r0 = pr.Rad(c.e[0]), r1 = pr.Rad(c.e[1]);
            double d = glm::length(c1 - c0);
            out[0] = c.value >= 0 ? d - (r0 + r1) : d - std::abs(r0 - r1);
        }
        break;
    }
    case CType::Midpoint: {
        const Entity* l = E(1);
        Vec2 m = (pr.LA(l) + pr.LB(l)) * 0.5, p = pr.P(c.e[0]);
        out[0] = p.x - m.x; out[1] = p.y - m.y;
        break;
    }
    case CType::Symmetric: {
        const Entity* l = E(2);
        Vec2 a = pr.LA(l), b = pr.LB(l), u = unit(b - a);
        Vec2 p0 = pr.P(c.e[0]), p1 = pr.P(c.e[1]);
        out[0] = cross2(u, (p0 + p1) * 0.5 - a);
        out[1] = glm::dot(u, p1 - p0);
        break;
    }
    case CType::Fix: {
        Vec2 p = pr.P(c.e[0]);
        out[0] = p.x - c.value; out[1] = p.y - c.value2;
        break;
    }
    case CType::Collinear: {
        const Entity *l0 = E(0), *l1 = E(1);
        Vec2 a = pr.LA(l0), u = unit(pr.LB(l0) - a);
        out[0] = cross2(u, pr.LA(l1) - a);
        out[1] = cross2(u, pr.LB(l1) - a);
        break;
    }
    case CType::Distance: out[0] = glm::length(pr.P(c.e[1]) - pr.P(c.e[0])) - c.value; break;
    case CType::HDistance: out[0] = (pr.P(c.e[1]).x - pr.P(c.e[0]).x) - c.value; break;
    case CType::VDistance: out[0] = (pr.P(c.e[1]).y - pr.P(c.e[0]).y) - c.value; break;
    case CType::Radius: out[0] = pr.Rad(c.e[0]) - c.value; break;
    case CType::Diameter: out[0] = 2.0 * pr.Rad(c.e[0]) - c.value; break;
    case CType::Angle: {
        const Entity *l0 = E(0), *l1 = E(1);
        Vec2 d0 = pr.LB(l0) - pr.LA(l0), d1 = pr.LB(l1) - pr.LA(l1);
        double th = std::atan2(cross2(d0, d1), glm::dot(d0, d1));
        out[0] = wrapPi(th - c.value);
        break;
    }
    case CType::PointLineDistance: {
        const Entity* l = E(1);
        Vec2 a = pr.LA(l), b = pr.LB(l);
        double s = cross2(unit(b - a), pr.P(c.e[0]) - a);
        out[0] = s - (c.value2 < 0 ? -c.value : c.value);
        break;
    }
    }
}

bool build(Problem& pr, const SolveOptions* opt, bool withDrag) {
    Sketch& sk = pr.sk;
    uint32_t maxId = 0;
    for (auto& [id, e] : sk.ents) maxId = std::max(maxId, id);
    pr.base.assign(maxId + 1, -1);
    pr.ent.assign(maxId + 1, nullptr);
    pr.vals.clear();
    pr.varOfParam.clear();
    pr.paramOfVar.clear();
    for (auto& [id, e] : sk.ents) {
        pr.ent[id] = &e;
        if (e.type == EntType::Point) {
            pr.base[id] = (int)pr.vals.size();
            pr.vals.push_back(e.x); pr.vals.push_back(e.y);
            pr.varOfParam.push_back(-1); pr.varOfParam.push_back(-1);
            if (!e.locked) {
                pr.varOfParam[pr.base[id]] = (int)pr.paramOfVar.size(); pr.paramOfVar.push_back(pr.base[id]);
                pr.varOfParam[pr.base[id] + 1] = (int)pr.paramOfVar.size(); pr.paramOfVar.push_back(pr.base[id] + 1);
            }
        } else if (e.type == EntType::Circle) {
            pr.base[id] = (int)pr.vals.size();
            pr.vals.push_back(e.r);
            pr.varOfParam.push_back(-1);
            if (!e.locked) { pr.varOfParam[pr.base[id]] = (int)pr.paramOfVar.size(); pr.paramOfVar.push_back(pr.base[id]); }
        }
    }
    pr.nVars = (int)pr.paramOfVar.size();
    for (auto& [id, c] : sk.cons) {
        if (!conValid(pr, c) || c.driven) continue;
        Term t;
        t.kind = Term::Con;
        t.con = &c;
        t.nres = nResiduals(c, pr);
        for (int k = 0; k < 3; ++k) if (c.e[k]) addParamsOf(pr, c.e[k], t.params);
        pr.terms.push_back(std::move(t));
    }
    for (auto& [id, e] : sk.ents) {
        if (e.type != EntType::Arc) continue;
        Term t;
        t.kind = Term::ArcEq;
        t.ent = &e;
        addParamsOf(pr, id, t.params);
        pr.terms.push_back(std::move(t));
    }
    if (withDrag && opt) {
        for (const DragPoint& d : opt->drags) {
            if (!d.point || d.point >= pr.ent.size() || !pr.ent[d.point]) continue;
            Term t;
            t.kind = Term::Drag;
            t.nres = 2;
            t.weight = opt->dragWeight;
            t.dragPoint = d.point;
            t.dragTarget = d.target;
            addParamsOf(pr, d.point, t.params);
            pr.terms.push_back(std::move(t));
        }
    }
    for (Term& t : pr.terms) {
        std::sort(t.params.begin(), t.params.end());
        t.params.erase(std::unique(t.params.begin(), t.params.end()), t.params.end());
        for (int p : t.params) if (pr.varOfParam[p] >= 0) t.vars.push_back(pr.varOfParam[p]);
    }
    return true;
}

// Local Jacobian of one term (central differences over the variables it touches).
void termJacobian(Problem& pr, const Term& t, const double* /*r0*/, std::vector<double>& B) {
    int nv = (int)t.vars.size();
    B.assign((size_t)t.nres * nv, 0.0);
    double rp[2], rm[2];
    for (int j = 0; j < nv; ++j) {
        int p = pr.paramOfVar[t.vars[j]];
        double v = pr.vals[p];
        double h = 1e-6 * std::max(1.0, std::abs(v));
        pr.vals[p] = v + h; evalTerm(pr, t, rp);
        pr.vals[p] = v - h; evalTerm(pr, t, rm);
        pr.vals[p] = v;
        for (int i = 0; i < t.nres; ++i) B[(size_t)i * nv + j] = (rp[i] - rm[i]) / (2 * h);
    }
}

bool cholesky(std::vector<double>& A, int n) {
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            double s = A[(size_t)i * n + j];
            for (int k = 0; k < j; ++k) s -= A[(size_t)i * n + k] * A[(size_t)j * n + k];
            if (i == j) {
                if (!(s > 1e-300)) return false;
                A[(size_t)i * n + i] = std::sqrt(s);
            } else {
                A[(size_t)i * n + j] = s / A[(size_t)j * n + j];
            }
        }
    }
    return true;
}

void choleskySolve(const std::vector<double>& L, int n, std::vector<double>& b) {
    for (int i = 0; i < n; ++i) {
        double s = b[i];
        for (int k = 0; k < i; ++k) s -= L[(size_t)i * n + k] * b[k];
        b[i] = s / L[(size_t)i * n + i];
    }
    for (int i = n - 1; i >= 0; --i) {
        double s = b[i];
        for (int k = i + 1; k < n; ++k) s -= L[(size_t)k * n + i] * b[k];
        b[i] = s / L[(size_t)i * n + i];
    }
}

double totalCost(Problem& pr, std::vector<double>& rbuf, double* hardMax) {
    double cost = 0, mx = 0;
    double r[2];
    for (const Term& t : pr.terms) {
        evalTerm(pr, t, r);
        for (int i = 0; i < t.nres; ++i) {
            if (!std::isfinite(r[i])) return std::numeric_limits<double>::infinity();
            cost += 0.5 * r[i] * r[i];
            if (t.kind != Term::Drag) mx = std::max(mx, std::abs(r[i]));
        }
    }
    (void)rbuf;
    if (hardMax) *hardMax = mx;
    return cost;
}

void writeBack(Problem& pr) {
    Sketch& sk = pr.sk;
    for (auto& [id, e] : sk.ents) {
        if (e.locked) continue;
        if (e.type == EntType::Point) { e.x = pr.vals[pr.base[id]]; e.y = pr.vals[pr.base[id] + 1]; }
        else if (e.type == EntType::Circle) e.r = std::abs(pr.vals[pr.base[id]]);
    }
}

} // namespace

void refreshDriven(Sketch& sk) {
    for (auto& [id, c] : sk.cons) {
        if (!c.driven || !isDimension(c.type)) continue;
        double m = sk.measureDimension(c);
        if (std::isfinite(m)) c.value = m;
    }
}

SolveResult solve(Sketch& sk, const SolveOptions& opt) {
    SolveResult res;
    try {
        Problem pr(sk);
        build(pr, &opt, !opt.drags.empty());
        const int n = pr.nVars;
        std::vector<double> saved = pr.vals;
        if (n == 0 || pr.terms.empty()) { res.ok = true; refreshDriven(sk); if (opt.computeDof) computeDof(sk); return res; }

        std::vector<double> A((size_t)n * n), g(n), delta(n), L, B, rbuf;
        double lambda = 1e-3;
        double hard = 0;
        double cost = totalCost(pr, rbuf, &hard);
        if (!std::isfinite(cost)) { res.ok = false; return res; }
        int it = 0;
        for (; it < opt.maxIter; ++it) {
            // Assemble normal equations
            std::fill(A.begin(), A.end(), 0.0);
            std::fill(g.begin(), g.end(), 0.0);
            double r[2];
            for (const Term& t : pr.terms) {
                int nv = (int)t.vars.size();
                if (nv == 0) continue;
                evalTerm(pr, t, r);
                termJacobian(pr, t, r, B);
                for (int i = 0; i < t.nres; ++i) {
                    const double* row = &B[(size_t)i * nv];
                    for (int a = 0; a < nv; ++a) {
                        g[t.vars[a]] += row[a] * r[i];
                        for (int b = 0; b < nv; ++b) A[(size_t)t.vars[a] * n + t.vars[b]] += row[a] * row[b];
                    }
                }
            }
            double gmax = 0;
            for (int i = 0; i < n; ++i) gmax = std::max(gmax, std::abs(g[i]));
            if (gmax < 1e-13) break;

            bool accepted = false;
            for (int tries = 0; tries < 12; ++tries) {
                L = A;
                for (int i = 0; i < n; ++i) L[(size_t)i * n + i] += lambda * std::max(A[(size_t)i * n + i], 1e-6) + 1e-12;
                if (!cholesky(L, n)) { lambda *= 10; continue; }
                for (int i = 0; i < n; ++i) delta[i] = -g[i];
                choleskySolve(L, n, delta);
                std::vector<double> old = pr.vals;
                double stepMax = 0;
                for (int i = 0; i < n; ++i) {
                    pr.vals[pr.paramOfVar[i]] += delta[i];
                    stepMax = std::max(stepMax, std::abs(delta[i]));
                }
                double h2 = 0;
                double c2 = totalCost(pr, rbuf, &h2);
                if (std::isfinite(c2) && c2 <= cost) {
                    bool tiny = stepMax < 1e-13 || (cost - c2) < 1e-30 * (1.0 + cost);
                    cost = c2; hard = h2;
                    lambda = std::max(lambda * 0.3, 1e-12);
                    accepted = true;
                    if (tiny) it = opt.maxIter;
                    break;
                }
                pr.vals = old;
                lambda *= 6;
            }
            if (!accepted) break;
            if (cost < 1e-26) break;
        }
        res.iterations = it;
        totalCost(pr, rbuf, &hard);
        res.maxResidual = hard;
        bool finite = true;
        for (double v : pr.vals) if (!std::isfinite(v)) finite = false;
        if (!finite || hard > 1e-6) {
            res.ok = false;
            // Report constraints that are still violated.
            double r[2];
            for (const Term& t : pr.terms) {
                if (t.kind != Term::Con) continue;
                evalTerm(pr, t, r);
                for (int i = 0; i < t.nres; ++i)
                    if (!std::isfinite(r[i]) || std::abs(r[i]) > 1e-5) { res.bad.push_back(t.con->id); break; }
            }
            return res;   // geometry untouched
        }
        writeBack(pr);
        refreshDriven(sk);
        sk.touch();
        if (opt.computeDof) computeDof(sk);
    } catch (...) {
        res.ok = false;
    }
    return res;
}

void computeDof(Sketch& sk) {
    sk.defined.clear();
    sk.dof = -1;
    try {
        Problem pr(sk);
        build(pr, nullptr, false);
        const int n = pr.nVars;
        int m = 0;
        for (const Term& t : pr.terms) m += t.nres;
        if (n == 0) { sk.dof = 0; return; }
        std::vector<double> J((size_t)std::max(m, 1) * n, 0.0), B;
        int row = 0;
        double r[2];
        for (const Term& t : pr.terms) {
            int nv = (int)t.vars.size();
            if (nv) {
                evalTerm(pr, t, r);
                termJacobian(pr, t, r, B);
                for (int i = 0; i < t.nres; ++i)
                    for (int a = 0; a < nv; ++a) J[(size_t)(row + i) * n + t.vars[a]] = B[(size_t)i * nv + a];
            }
            row += t.nres;
        }
        // Normalize rows so tolerances are scale independent.
        for (int i = 0; i < m; ++i) {
            double mx = 0;
            for (int j = 0; j < n; ++j) mx = std::max(mx, std::abs(J[(size_t)i * n + j]));
            if (mx > 1e-12) for (int j = 0; j < n; ++j) J[(size_t)i * n + j] /= mx;
        }
        // Reduced row echelon form with partial pivoting.
        std::vector<int> pivotCol;
        int rank = 0;
        const double tol = 1e-7;
        for (int col = 0; col < n && rank < m; ++col) {
            int best = rank;
            double bv = std::abs(J[(size_t)rank * n + col]);
            for (int i = rank + 1; i < m; ++i) {
                double v = std::abs(J[(size_t)i * n + col]);
                if (v > bv) { bv = v; best = i; }
            }
            if (bv < tol) continue;
            if (best != rank)
                for (int j = 0; j < n; ++j) std::swap(J[(size_t)best * n + j], J[(size_t)rank * n + j]);
            double pv = J[(size_t)rank * n + col];
            for (int j = 0; j < n; ++j) J[(size_t)rank * n + j] /= pv;
            for (int i = 0; i < m; ++i) {
                if (i == rank) continue;
                double f = J[(size_t)i * n + col];
                if (std::abs(f) < 1e-15) continue;
                for (int j = 0; j < n; ++j) J[(size_t)i * n + j] -= f * J[(size_t)rank * n + j];
            }
            pivotCol.push_back(col);
            ++rank;
        }
        sk.dof = n - rank;
        std::vector<char> isPivot(n, 0);
        for (int c : pivotCol) isPivot[c] = 1;
        std::vector<char> undetermined(n, 0);
        for (int j = 0; j < n; ++j) if (!isPivot[j]) undetermined[j] = 1;
        for (int i = 0; i < rank; ++i) {
            for (int j = 0; j < n; ++j) {
                if (!isPivot[j] && std::abs(J[(size_t)i * n + j]) > 1e-7) { undetermined[pivotCol[i]] = 1; break; }
            }
        }
        auto varFree = [&](int param) {
            int v = pr.varOfParam[param];
            return v >= 0 && undetermined[v];
        };
        auto ptDefined = [&](uint32_t id) {
            return id < pr.base.size() && pr.base[id] >= 0 && !varFree(pr.base[id]) && !varFree(pr.base[id] + 1);
        };
        for (auto& [id, e] : sk.ents) {
            bool d = false;
            switch (e.type) {
            case EntType::Point: d = ptDefined(id); break;
            case EntType::Line: d = ptDefined(e.a) && ptDefined(e.b); break;
            case EntType::Circle: d = ptDefined(e.c) && !varFree(pr.base[id]); break;
            case EntType::Arc: d = ptDefined(e.c) && ptDefined(e.a) && ptDefined(e.b); break;
            case EntType::Spline:
            case EntType::CtrlSpline: {
                d = !e.pts.empty();
                for (uint32_t p : e.pts) d = d && ptDefined(p);
                break;
            }
            }
            if (d) sk.defined.insert(id);
        }
    } catch (...) {
        sk.dof = -1;
    }
}

} // namespace kst::sk
