// M3: extrude / revolve features, the parametric recompute engine, user parameters,
// and the native .kst document files with autosave and crash recovery.
#include "app/App.h"
#include "app/MechTools.h"
#include "core/Curves.h"
#include "core/Log.h"
#include "core/SaveLoad.h"
#include "kernel/Features.h"
#include "platform/Platform.h"
#include "sketch/Profiles.h"
#include "sketch/Solver.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace kst {

namespace {

const uint64_t kFnvSeed = 1469598103934665603ull;
const uint64_t kFnvPrime = 1099511628211ull;

uint64_t mixU64(uint64_t h, uint64_t v) { return (h ^ v) * kFnvPrime; }
uint64_t hashStr(uint64_t h, const std::string& s) {
    for (char c : s) h = (h ^ (uint8_t)c) * kFnvPrime;
    return h;
}
uint64_t hashDbl(uint64_t h, double v) {
    uint64_t b;
    std::memcpy(&b, &v, 8);
    return mixU64(h, b);
}

std::string fmtG(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// Finalizes a signature with the referenced parameter values.
uint64_t hashParams(uint64_t h, const Document& d, const std::set<std::string>& names) {
    for (auto& n : names) {
        auto it = d.params.find(n);
        h = hashStr(h, n);
        h = hashDbl(h, it == d.params.end() ? 0.0 : it->second.value);
    }
    return h;
}

// A sketch's content (geometry and plane), the same in every session: its rev is a session
// counter, and the feature disk cache needs keys that survive closing and opening designs.
uint64_t sketchSig(const sk::Sketch& sk) {
    static std::mutex m;
    static std::unordered_map<uint64_t, uint64_t> memo;   // rev -> content signature
    {
        std::lock_guard lk(m);
        auto it = memo.find(sk.rev);
        if (it != memo.end()) return it->second;
    }
    uint64_t h = kFnvSeed;
    for (int k = 0; k < 3; ++k) {
        h = hashDbl(h, sk.plane.o[k]);
        h = hashDbl(h, sk.plane.x[k]);
        h = hashDbl(h, sk.plane.y[k]);
    }
    for (auto& [id, e] : sk.ents) {
        h = mixU64(h, id);
        h = mixU64(h, (uint64_t)e.type);
        h = hashDbl(h, e.x);
        h = hashDbl(h, e.y);
        h = hashDbl(h, e.r);
        h = mixU64(h, ((uint64_t)e.a << 32) ^ e.b);
        h = mixU64(h, e.c);
        h = mixU64(h, (e.construction ? 1u : 0u) | (e.loop ? 2u : 0u) | (e.locked ? 4u : 0u));
        for (uint32_t p : e.pts) h = mixU64(h, p);
    }
    std::lock_guard lk(m);
    if (memo.size() > 20000) memo.clear();
    memo[sk.rev] = h;
    return h;
}

// Signature of one feature's own inputs (no upstream chain). Deterministic across sessions
// is not required, only across recomputes of the same session.
uint64_t ownSig(const Document& d, const Feature& f) {
    uint64_t h = kFnvSeed;
    h = mixU64(h, (uint64_t)f.type);
    if (f.type == FeatureType::Sketch) {
        auto it = d.sketches.find(f.sketchId);
        if (it != d.sketches.end()) {
            h = mixU64(h, sketchSig(it->second));
        }
        return h;
    }
    if (f.type == FeatureType::Import) {
        h = hashStr(h, f.importPath);
        std::error_code ec;
        if (std::filesystem::exists(u8path(f.importPath), ec)) {
            auto t = std::filesystem::last_write_time(u8path(f.importPath), ec);
            if (!ec) h = mixU64(h, (uint64_t)t.time_since_epoch().count());
            auto sz = std::filesystem::file_size(u8path(f.importPath), ec);
            if (!ec) h = mixU64(h, sz);
        } else if (const char* fake = std::getenv("KESTRAL_IMPORT_STAMP")) {
            // Debug: "fid:ticks:size" stands in for a file on another computer (to work out its chain).
            unsigned long long fid = 0, ticks = 0, size = 0;
            if (std::sscanf(fake, "%llu:%llu:%llu", &fid, &ticks, &size) == 3 && fid == f.id) {
                h = mixU64(h, (uint64_t)ticks);
                h = mixU64(h, (uint64_t)size);
            }
        }
        return h;
    }
    if ((f.type == FeatureType::Rib && f.rib) || (f.type == FeatureType::Web && f.web)) {
        const RibParams& rp = f.type == FeatureType::Web ? *f.web : *f.rib;
        h = mixU64(h, rp.sketchId);
        h = hashStr(h, rp.thickExpr);
        h = mixU64(h, rp.flip ? 1 : 0);
        h = mixU64(h, (uint64_t)rp.side);
        for (auto& t : rp.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        auto sit = d.sketches.find(rp.sketchId);
        if (sit != d.sketches.end()) h = mixU64(h, sketchSig(sit->second));
        std::set<std::string> ns;
        for (auto& n : peval::names(rp.thickExpr)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Emboss && f.emboss) {
        const EmbossParams& ep = *f.emboss;
        h = mixU64(h, ep.sketchId);
        h = hashStr(h, ep.depthExpr);
        h = mixU64(h, ep.deboss ? 1 : 0);
        h = mixU64(h, ep.flip ? 1 : 0);
        for (auto& t : ep.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        auto sit = d.sketches.find(ep.sketchId);
        if (sit != d.sketches.end()) h = mixU64(h, sketchSig(sit->second));
        std::set<std::string> ns;
        for (auto& n : peval::names(ep.depthExpr)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Boss && f.boss) {
        const BossParams& bp = *f.boss;
        h = hashStr(h, bp.xExpr);
        h = hashStr(h, bp.yExpr);
        h = hashStr(h, bp.zExpr);
        h = hashStr(h, bp.diaExpr);
        h = hashStr(h, bp.heightExpr);
        h = hashStr(h, bp.holeExpr);
        h = mixU64(h, (uint64_t)bp.axis);
        for (auto& t : bp.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        std::set<std::string> ns;
        for (auto& e : {bp.xExpr, bp.yExpr, bp.zExpr, bp.diaExpr, bp.heightExpr, bp.holeExpr})
            for (auto& n : peval::names(e)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Snap && f.snap) {
        const SnapParams& sp = *f.snap;
        h = hashStr(h, sp.xExpr);
        h = hashStr(h, sp.yExpr);
        h = hashStr(h, sp.zExpr);
        h = hashStr(h, sp.lengthExpr);
        h = hashStr(h, sp.widthExpr);
        h = hashStr(h, sp.thickExpr);
        h = hashStr(h, sp.hookExpr);
        h = hashStr(h, sp.hookLenExpr);
        h = mixU64(h, (uint64_t)sp.axis);
        for (auto& t : sp.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        std::set<std::string> ns;
        for (auto& e : {sp.xExpr, sp.yExpr, sp.zExpr, sp.lengthExpr, sp.widthExpr, sp.thickExpr, sp.hookExpr, sp.hookLenExpr})
            for (auto& n : peval::names(e)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Rest && f.rest) {
        const RestParams& rp = *f.rest;
        h = mixU64(h, rp.sketchId);
        h = hashStr(h, rp.thickExpr);
        h = mixU64(h, rp.flip ? 1 : 0);
        for (auto& t : rp.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        auto sit = d.sketches.find(rp.sketchId);
        if (sit != d.sketches.end()) h = mixU64(h, sketchSig(sit->second));
        std::set<std::string> ns;
        for (auto& n : peval::names(rp.thickExpr)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Lip && f.lip) {
        const LipParams& lp = *f.lip;
        h = mixU64(h, lp.sketchId);
        h = hashStr(h, lp.widthExpr);
        h = hashStr(h, lp.heightExpr);
        h = mixU64(h, lp.flip ? 1 : 0);
        h = mixU64(h, lp.groove ? 1 : 0);
        for (auto& t : lp.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        auto sit = d.sketches.find(lp.sketchId);
        if (sit != d.sketches.end()) h = mixU64(h, sketchSig(sit->second));
        std::set<std::string> ns;
        for (auto& e : {lp.widthExpr, lp.heightExpr})
            for (auto& n : peval::names(e)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Silhouette && f.silhouette) {
        const SilhouetteParams& sp = *f.silhouette;
        h = mixU64(h, (uint64_t)sp.axis);
        for (auto& t : sp.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        return h;
    }
    if ((f.type == FeatureType::Joint && f.joint) || (f.type == FeatureType::AsBuilt && f.asbuilt)) {
        const JointParams& jp = f.type == FeatureType::AsBuilt ? *f.asbuilt : *f.joint;
        h = mixU64(h, (uint64_t)jp.kind);
        h = mixU64(h, (uint64_t)jp.axis);
        h = mixU64(h, jp.ground.first); h = mixU64(h, jp.ground.second);
        h = mixU64(h, jp.move.first); h = mixU64(h, jp.move.second);
        for (auto* e : {&jp.gx, &jp.gy, &jp.gz, &jp.mx, &jp.my, &jp.mz, &jp.angleExpr, &jp.slideExpr}) h = hashStr(h, *e);
        std::set<std::string> ns;
        for (auto* e : {&jp.gx, &jp.gy, &jp.gz, &jp.mx, &jp.my, &jp.mz, &jp.angleExpr, &jp.slideExpr})
            for (auto& n : peval::names(*e)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Rigid && f.rigid) {
        for (auto& t : f.rigid->members) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        return h;
    }
    // M4 payloads.
    if (f.type == FeatureType::Fillet || f.type == FeatureType::Chamfer || f.type == FeatureType::Shell ||
        f.type == FeatureType::PressPull || f.type == FeatureType::DeleteFace || f.type == FeatureType::Draft ||
        f.type == FeatureType::ReplaceFace || f.type == FeatureType::Thicken) {
        if (!f.modify) return h;
        h = mixU64(h, (uint64_t)f.modify->kind);
        h = mixU64(h, (uint64_t)f.modify->axis);
        h = mixU64(h, (uint64_t)f.modify->side);
        h = hashStr(h, f.modify->valueExpr);
        h = hashStr(h, f.modify->endExpr);
        h = mixU64(h, f.modify->edgeTotal);
        h = mixU64(h, f.modify->faceTotal);
        for (auto& t : f.modify->targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        for (auto& e : f.modify->edges) { h = mixU64(h, e.first); h = mixU64(h, e.second); }
        for (auto& fc : f.modify->faces) { h = mixU64(h, fc.first); h = mixU64(h, fc.second); }
        std::set<std::string> ns;
        for (auto& n : peval::names(f.modify->valueExpr)) ns.insert(n);
        for (auto& n : peval::names(f.modify->endExpr)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if ((f.type == FeatureType::Coil || f.type == FeatureType::Pipe) && f.pathSolid) {
        const SweepSolidParams& p = *f.pathSolid;
        h = mixU64(h, (uint64_t)p.axis);
        h = mixU64(h, p.leftHand ? 1 : 0);
        h = mixU64(h, p.pathSketch);
        for (auto& e : {p.diaExpr, p.heightExpr, p.turnsExpr, p.sectionExpr, p.innerExpr, p.xExpr, p.yExpr, p.zExpr})
            h = hashStr(h, e);
        std::set<std::string> ns;
        for (auto& e : {p.diaExpr, p.heightExpr, p.turnsExpr, p.sectionExpr, p.innerExpr, p.xExpr, p.yExpr, p.zExpr})
            for (auto& n : peval::names(e)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Pattern && f.pattern) {
        const PatternParams& p = *f.pattern;
        h = mixU64(h, p.onPath ? 2 : p.circular ? 1 : 0);
        if (p.onPath) {
            h = mixU64(h, p.pathSketch);
            h = mixU64(h, p.pathEnt);
            h = mixU64(h, (uint64_t)p.distType);
            h = mixU64(h, (uint64_t)p.orient);
            h = mixU64(h, p.flip ? 1 : 0);
            h = mixU64(h, p.pathBody.first);
            h = mixU64(h, p.pathBody.second);
            h = mixU64(h, p.pathEdge);
            if (p.pathSketch) {
                auto sit = d.sketches.find(p.pathSketch);
                if (sit != d.sketches.end()) h = mixU64(h, sketchSig(sit->second));
            }
        }
        h = hashStr(h, p.countExpr);
        h = hashStr(h, p.spacingExpr);
        h = hashStr(h, p.countYExpr);
        h = hashStr(h, p.spacingYExpr);
        h = mixU64(h, (uint64_t)p.axis);
        for (int k = 0; k < 3; ++k) { h = hashDbl(h, p.axisO[k]); h = hashDbl(h, p.axisD[k]); }
        h = hashStr(h, p.angleExpr);
        for (auto& t : p.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        std::set<std::string> ns;
        for (auto& e : {p.countExpr, p.spacingExpr, p.countYExpr, p.spacingYExpr, p.angleExpr})
            for (auto& n : peval::names(e)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Mirror && f.mirror) {
        h = mixU64(h, (uint64_t)f.mirror->plane);
        for (int k = 0; k < 3; ++k) { h = hashDbl(h, f.mirror->planeO[k]); h = hashDbl(h, f.mirror->planeN[k]); }
        h = mixU64(h, 0x6d6972);   // mirrors baked into the body frame (older results were world only)
        for (auto& t : f.mirror->targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        return h;
    }
    if (f.type == FeatureType::Combine && f.combine) {
        h = mixU64(h, (uint64_t)f.combine->op);
        h = mixU64(h, f.combine->tool.first);
        h = mixU64(h, f.combine->tool.second);
        for (auto& t : f.combine->targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        for (auto& t : f.combine->tools) { h = mixU64(h, t.first * 31 + 7); h = mixU64(h, t.second); }
        h = mixU64(h, f.combine->keepTools ? 3 : 4);
        return h;
    }
    if (f.type == FeatureType::Remove && f.remove) {
        for (auto& t : f.remove->targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        return h;
    }
    if (f.type == FeatureType::Gear && f.gear) {
        const GearParams& g = *f.gear;
        h = mixU64(h, (uint64_t)g.type);
        h = mixU64(h, (uint64_t)g.plane);
        h = mixU64(h, (uint64_t)g.sizeBy);
        h = mixU64(h, g.leftHand ? 1 : 0);
        std::set<std::string> ns;
        for (auto& [k, v] : g.vals) {
            h = hashStr(h, k);
            h = hashStr(h, v);
            for (auto& n : peval::names(v)) ns.insert(n);
        }
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Blade && f.blade) {
        const BladeParams& b = *f.blade;
        h = mixU64(h, 0x626c6164);
        h = mixU64(h, (uint64_t)b.kind);
        h = mixU64(h, (uint64_t)b.plane);
        h = mixU64(h, b.leftHand ? 1 : 0);
        h = hashStr(h, b.airfoil);
        std::set<std::string> ns;
        for (auto& [k, v] : b.vals) {
            h = hashStr(h, k);
            h = hashStr(h, v);
            for (auto& n : peval::names(v)) ns.insert(n);
        }
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Dovetail && f.dovetail) {
        const DovetailParams& dv = *f.dovetail;
        h = mixU64(h, 0x646f7665);
        h = mixU64(h, (uint64_t)dv.type);
        h = mixU64(h, (uint64_t)dv.plane);
        for (auto& t : dv.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        std::set<std::string> ns;
        for (auto& [k, v] : dv.vals) {
            h = hashStr(h, k);
            h = hashStr(h, v);
            for (auto& n : peval::names(v)) ns.insert(n);
        }
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Wing && f.wing) {
        const WingParams& w = *f.wing;
        h = mixU64(h, 0x77696e67);
        h = mixU64(h, (w.mirror ? 1 : 0) | (w.hollow ? 2 : 0));
        h = hashStr(h, w.rootAirfoil);
        std::set<std::string> ns;
        for (auto& [k, v] : w.vals) {
            h = hashStr(h, k);
            h = hashStr(h, v);
            for (auto& n : peval::names(v)) ns.insert(n);
        }
        for (auto& p : w.panels)
            for (auto* e : {&p.span, &p.tipChord, &p.sweep, &p.dihedral, &p.twist, &p.airfoil}) {
                h = hashStr(h, *e);
                if (e != &p.airfoil)
                    for (auto& n : peval::names(*e)) ns.insert(n);
            }
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Thread && f.thread) {
        const ThreadParams& t = *f.thread;
        h = mixU64(h, t.face.feature); h = mixU64(h, t.face.slot); h = mixU64(h, t.face.face); h = mixU64(h, t.face.faceTotal);
        h = mixU64(h, (uint64_t)t.family);
        h = mixU64(h, (uint64_t)(int64_t)t.size);
        h = mixU64(h, (uint64_t)t.side);
        h = mixU64(h, (t.modeled ? 1 : 0) | (t.leftHand ? 2 : 0) | (t.flip ? 4 : 0) | (t.fullLength ? 8 : 0) | (t.hole ? 16 : 0));
        std::set<std::string> ns;
        for (auto& e : {t.lengthExpr, t.offsetExpr, t.startsExpr, t.diaExpr, t.pitchExpr}) {
            h = hashStr(h, e);
            for (auto& n : peval::names(e)) ns.insert(n);
        }
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Split && f.split) {
        const SplitParams& sp = *f.split;
        h = mixU64(h, (uint64_t)sp.plane);
        for (int i = 0; i < 3; ++i) { h = hashDbl(h, sp.toolO[i]); h = hashDbl(h, sp.toolN[i]); }
        h = hashStr(h, sp.toolName);
        h = mixU64(h, sp.sketchId);
        h = mixU64(h, sp.sketchEnt);
        if (sp.sketchId) {
            auto sit = d.sketches.find(sp.sketchId);
            if (sit != d.sketches.end()) h = mixU64(h, sketchSig(sit->second));
        }
        for (auto& t : sp.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        return h;
    }
    if (f.type == FeatureType::SplitFace && f.splitFace) {
        const SplitFaceParams& sp = *f.splitFace;
        h = mixU64(h, (uint64_t)sp.plane);
        h = hashStr(h, sp.offsetExpr);
        h = mixU64(h, sp.faceTotal);
        h = mixU64(h, sp.sketchId);
        if (sp.sketchId) {
            auto sit = d.sketches.find(sp.sketchId);
            if (sit != d.sketches.end()) h = mixU64(h, sketchSig(sit->second));
        }
        for (uint32_t fi : sp.faces) h = mixU64(h, fi);
        for (auto& t : sp.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        std::set<std::string> ns;
        for (auto& n : peval::names(sp.offsetExpr)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Align && f.align) {
        const AlignParams& a = *f.align;
        h = mixU64(h, 0x616c676e);
        h = mixU64(h, a.flip ? 1 : 0);
        h = hashStr(h, a.angleExpr);
        h = hashStr(h, a.offsetExpr);
        auto pk = [&](const AlignParams::Pick& q) {
            h = mixU64(h, (uint64_t)q.kind);
            h = mixU64(h, q.ref.first); h = mixU64(h, q.ref.second);
            h = mixU64(h, q.sub); h = mixU64(h, q.total); h = mixU64(h, (uint64_t)q.end);
        };
        pk(a.from); pk(a.to);
        for (auto& t : a.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        std::set<std::string> ns;
        for (auto& n : peval::names(a.angleExpr)) ns.insert(n);
        for (auto& n : peval::names(a.offsetExpr)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if ((f.type == FeatureType::Sweep || f.type == FeatureType::Loft) && f.multi) {
        for (uint32_t sid : f.multi->sketchIds) {
            h = mixU64(h, sid);
            auto sit = d.sketches.find(sid);
            if (sit != d.sketches.end()) h = mixU64(h, sketchSig(sit->second));
        }
        h = mixU64(h, (uint64_t)f.multi->op);
        for (auto& t : f.multi->targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        h = hashStr(h, f.multi->twistExpr);
        h = hashStr(h, f.multi->thinExpr);
        h = mixU64(h, (uint64_t)f.multi->thinSide);
        h = mixU64(h, (uint64_t)f.multi->pointEnd);
        h = hashStr(h, f.multi->pointX);
        h = hashStr(h, f.multi->pointY);
        h = hashStr(h, f.multi->pointZ);
        if (f.multi->centerId) {
            h = mixU64(h, f.multi->centerId);
            auto cit = d.sketches.find(f.multi->centerId);
            if (cit != d.sketches.end()) h = mixU64(h, sketchSig(cit->second));
        }
        for (uint32_t sid : f.multi->railIds) {
            h = mixU64(h, sid);
            auto sit = d.sketches.find(sid);
            if (sit != d.sketches.end()) h = mixU64(h, sketchSig(sit->second));
        }
        std::set<std::string> ns;
        for (auto& n : peval::names(f.multi->twistExpr)) ns.insert(n);
        for (auto& n : peval::names(f.multi->thinExpr)) ns.insert(n);
        for (auto& n : peval::names(f.multi->pointX)) ns.insert(n);
        for (auto& n : peval::names(f.multi->pointY)) ns.insert(n);
        for (auto& n : peval::names(f.multi->pointZ)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Move && f.move) {
        const MoveParams& mv = *f.move;
        const std::string* es[] = {&mv.xExpr, &mv.yExpr, &mv.zExpr, &mv.rxExpr, &mv.ryExpr, &mv.rzExpr,
                                   &mv.angExpr, &mv.axExpr, &mv.ayExpr, &mv.azExpr, &mv.pxExpr, &mv.pyExpr,
                                   &mv.pzExpr, &mv.sxExpr, &mv.syExpr, &mv.szExpr};
        for (auto* e : es) h = hashStr(h, *e);
        h = mixU64(h, mv.copy ? 1 : 0);
        h = mixU64(h, mv.together ? 1 : 0);
        h = mixU64(h, (uint64_t)mv.op);
        h = mixU64(h, mv.hasMat ? 1 : 0);
        if (mv.hasMat)
            for (int i = 0; i < 16; ++i) h = hashDbl(h, mv.mat[i]);
        for (auto& t : mv.targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        std::set<std::string> ns;
        for (auto* e : es)
            for (auto& n : peval::names(*e)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::MeshConvert && f.modify) {
        for (auto& t : f.modify->targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        return h;
    }
    if (f.type == FeatureType::ConstructPlane && f.cplane) {
        h = mixU64(h, (uint64_t)f.cplane->kind);
        h = mixU64(h, (uint64_t)f.cplane->base);
        h = mixU64(h, (uint64_t)f.cplane->axis);
        h = mixU64(h, f.cplane->refPlane);
        h = hashStr(h, f.cplane->offsetExpr);
        if (f.cplane->kind == 3) {
            for (auto* e : {&f.cplane->x1, &f.cplane->y1, &f.cplane->z1, &f.cplane->x2, &f.cplane->y2, &f.cplane->z2,
                            &f.cplane->x3, &f.cplane->y3, &f.cplane->z3})
                h = hashStr(h, *e);
        }
        if (f.cplane->kind == 4) {
            h = mixU64(h, f.cplane->faceRef.first);
            h = mixU64(h, f.cplane->faceRef.second);
            h = mixU64(h, f.cplane->faceSub);
            h = mixU64(h, f.cplane->faceTotal);
            h = hashStr(h, f.cplane->angleExpr);
        }
        std::set<std::string> ns;
        for (auto& n : peval::names(f.cplane->offsetExpr)) ns.insert(n);
        if (f.cplane->kind == 3) {
            for (auto* e : {&f.cplane->x1, &f.cplane->y1, &f.cplane->z1, &f.cplane->x2, &f.cplane->y2, &f.cplane->z2,
                            &f.cplane->x3, &f.cplane->y3, &f.cplane->z3})
                for (auto& n : peval::names(*e)) ns.insert(n);
        }
        if (f.cplane->kind == 4)
            for (auto& n : peval::names(f.cplane->angleExpr)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::ConstructAxis && f.caxis) {
        h = mixU64(h, (uint64_t)f.caxis->kind);
        h = mixU64(h, f.caxis->ref.first);
        h = mixU64(h, f.caxis->ref.second);
        h = mixU64(h, f.caxis->sub);
        h = mixU64(h, f.caxis->subTotal);
        for (auto* e : {&f.caxis->ax, &f.caxis->ay, &f.caxis->az, &f.caxis->bx, &f.caxis->by, &f.caxis->bz})
            h = hashStr(h, *e);
        std::set<std::string> ns;
        for (auto* e : {&f.caxis->ax, &f.caxis->ay, &f.caxis->az, &f.caxis->bx, &f.caxis->by, &f.caxis->bz})
            for (auto& n : peval::names(*e)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::ConstructPoint && f.cpoint) {
        h = mixU64(h, (uint64_t)f.cpoint->kind);
        h = mixU64(h, f.cpoint->ref.first);
        h = mixU64(h, f.cpoint->ref.second);
        h = mixU64(h, f.cpoint->sub);
        h = mixU64(h, f.cpoint->subTotal);
        for (auto* e : {&f.cpoint->x, &f.cpoint->y, &f.cpoint->z}) h = hashStr(h, *e);
        std::set<std::string> ns;
        for (auto* e : {&f.cpoint->x, &f.cpoint->y, &f.cpoint->z})
            for (auto& n : peval::names(*e)) ns.insert(n);
        return hashParams(h, d, ns);
    }
    if (f.type == FeatureType::Hole && f.hole) {
        h = hashStr(h, f.hole->diaExpr);
        h = hashStr(h, f.hole->depthExpr);
        for (auto& t : f.hole->targets) { h = mixU64(h, t.first); h = mixU64(h, t.second); }
        h = mixU64(h, f.hole->sketchId);
        auto sit2 = d.sketches.find(f.hole->sketchId);
        if (sit2 != d.sketches.end()) h = mixU64(h, sketchSig(sit2->second));
        h = mixU64(h, (uint64_t)f.hole->kind);
        h = mixU64(h, f.hole->tapped ? 1 : 0);
        for (auto& e : {f.hole->cbDiaExpr, f.hole->cbDepthExpr, f.hole->csDiaExpr, f.hole->csAngleExpr,
                        f.hole->spotDiaExpr, f.hole->spotDepthExpr, f.hole->drillExpr, f.hole->pitchExpr}) h = hashStr(h, e);
        for (auto& pt : f.hole->points)
            for (int k = 0; k < 3; ++k) { h = hashDbl(h, pt.pos[k]); h = hashDbl(h, pt.dir[k]); }
        std::set<std::string> ns;
        for (auto& e : {f.hole->diaExpr, f.hole->depthExpr, f.hole->cbDiaExpr, f.hole->cbDepthExpr, f.hole->csDiaExpr, f.hole->csAngleExpr,
                        f.hole->spotDiaExpr, f.hole->spotDepthExpr, f.hole->drillExpr, f.hole->pitchExpr})
            if (!e.empty())
                for (auto& n : peval::names(e)) ns.insert(n);
        return hashParams(h, d, ns);
    }

    // Parametric solid features: settings + sketch rev + referenced parameter values.
    uint32_t sketchId = 0;
    std::string e1, e2;
    int op = 0;
    bool flip = false;
    std::vector<TargetRef> targets;
    int axis = 0;
    uint32_t axisEnt = 0;
    if (f.type == FeatureType::Extrude && f.extrude) {
        sketchId = f.extrude->sketchId;
        e1 = f.extrude->distExpr;
        e2 = f.extrude->taperExpr;
        op = f.extrude->op;
        flip = f.extrude->flip;
        h = mixU64(h, f.extrude->symmetric ? 1 : 0);
        targets = f.extrude->targets;
        for (auto& q : f.extrude->profilePts) { h = hashDbl(h, q.x); h = hashDbl(h, q.y); }
        for (auto& fr : f.extrude->faces) {
            h = mixU64(h, fr.feature); h = mixU64(h, fr.slot); h = mixU64(h, fr.face); h = mixU64(h, fr.faceTotal);
        }
        h = mixU64(h, (uint64_t)f.extrude->extent);
        h = mixU64(h, f.extrude->to.feature);
        h = mixU64(h, f.extrude->to.slot);
        h = mixU64(h, f.extrude->to.face);
        h = mixU64(h, f.extrude->to.faceTotal);
        for (auto& [msid, mpts] : f.extrude->moreSketches) {   // only when used: old steps hash as before
            h = mixU64(h, msid);
            for (auto& q : mpts) { h = hashDbl(h, q.x); h = hashDbl(h, q.y); }
            auto mit = d.sketches.find(msid);
            h = mixU64(h, mit != d.sketches.end() ? sketchSig(mit->second) : 0xdead);
        }
    } else if (f.type == FeatureType::Revolve && f.revolve) {
        sketchId = f.revolve->sketchId;
        e1 = f.revolve->angleExpr;
        op = f.revolve->op;
        flip = f.revolve->flip;
        axis = f.revolve->axis;
        axisEnt = f.revolve->axisEnt;
        targets = f.revolve->targets;
        for (auto& q : f.revolve->profilePts) { h = hashDbl(h, q.x); h = hashDbl(h, q.y); }
    } else {
        return h;
    }
    h = hashStr(h, e1);
    h = hashStr(h, e2);
    std::string thin = f.type == FeatureType::Extrude ? f.extrude->thinExpr : f.revolve->thinExpr;
    int thinSide = f.type == FeatureType::Extrude ? f.extrude->thinSide : f.revolve->thinSide;
    h = hashStr(h, thin);
    h = mixU64(h, (uint64_t)thinSide);
    h = mixU64(h, (uint64_t)op);
    h = mixU64(h, flip ? 1 : 0);
    h = mixU64(h, (uint64_t)axis);
    h = mixU64(h, axisEnt);
    for (auto& t : targets) {
        h = mixU64(h, t.first);
        h = mixU64(h, t.second);
    }

    // Referenced parameter values: feature expressions + every dimension expression in the sketch.
    std::set<std::string> names;
    for (auto& n : peval::names(e1)) names.insert(n);
    for (auto& n : peval::names(e2)) names.insert(n);
    for (auto& n : peval::names(thin)) names.insert(n);
    auto sit = d.sketches.find(sketchId);
    if (sit != d.sketches.end()) {
        h = mixU64(h, sketchSig(sit->second));
        for (auto& [cid, c] : sit->second.cons)
            if (!c.expr.empty())
                for (auto& n : peval::names(c.expr)) names.insert(n);
    } else {
        h = mixU64(h, 0xdead);
    }
    for (auto& n : names) {
        auto it = d.params.find(n);
        h = hashStr(h, n);
        h = hashDbl(h, it == d.params.end() ? 0.0 : it->second.value);
    }
    return h;
}

kernel::BoolOp boolOpOf(int op) {
    switch (op) {
    case 1: return kernel::BoolOp::Join;
    case 2: return kernel::BoolOp::Cut;
    case 3: return kernel::BoolOp::Intersect;
    default: return kernel::BoolOp::NewBody;
    }
}

} // namespace

// ===================================================================== results

struct App::RecomputeResults {
    uint64_t baseRev = 0;
    struct Item {
        uint32_t featureId = 0;
        uint64_t sig = 0, chainSig = 0;
        bool ok = false;
        std::string error;
        std::string warn;           // partial success report (fillet fallbacks)
        double ms = 0;
        std::vector<ShapePtr> shapes;
        std::vector<MeshPtr> meshes;
        std::vector<std::string> names;
        std::vector<glm::mat4> xforms;               // per output, identity default
        std::vector<TargetRef> instanceRefs;         // per output: instance another body's part (0,0 = new geometry)
        std::vector<TargetRef> consumed;             // bodies these outputs replace
        bool isImport = false;
        kernel::ImportResult import;
        MeshPtr cutPreview;                          // a cut's tool shape, shown red while its dialog is open
    };
    std::vector<Item> items;
    int keptSaved = 0;          // steps shown with their saved shapes although their inputs changed
    std::string keptFirst;      // the first such step, for the message
};

namespace {

// A boolean target resolved to actual geometry: from this pass's fresh results first, then from
// the snapshot. Imports resolve through their fresh ImportResult so chains work right after load.
struct ResolvedTarget {
    ShapePtr shape;
    MeshPtr mesh;
    glm::mat4 xform{1.f};
    std::string name;
};

bool resolveTarget(const Document& snap, const std::vector<App::RecomputeResults::Item>& done,
                   const TargetRef& r, ResolvedTarget& out) {
    for (auto& it : done) {
        if (it.featureId != r.first) continue;
        if (it.isImport) {
            if (r.second >= it.import.bodies.size()) return false;
            const auto& bo = it.import.bodies[r.second];
            if (bo.part < 0 || bo.part >= (int)it.import.parts.size()) return false;
            out.shape = it.import.parts[bo.part].shape;
            out.mesh = it.import.parts[bo.part].mesh;
            out.xform = bo.xform;
            out.name = bo.name;
            return (bool)out.shape || (bool)out.mesh;
        }
        if (r.second >= it.shapes.size()) return false;
        out.name = r.second < it.names.size() ? it.names[r.second] : std::string();
        // A body the user renamed keeps that name down the chain, also on the first pass after opening.
        auto oit = snap.overrides.find(Document::overrideKey(r.first, r.second));
        if (oit != snap.overrides.end() && !oit->second.name.empty()) out.name = oit->second.name;
        if (!it.shapes[r.second] && r.second < it.instanceRefs.size() && it.instanceRefs[r.second].first) {
            // A moved / patterned copy computed in this pass: its geometry is the original's part.
            ResolvedTarget base;
            if (!resolveTarget(snap, done, it.instanceRefs[r.second], base)) return false;
            out.shape = base.shape;
            out.mesh = base.mesh;
            out.xform = r.second < it.xforms.size() ? it.xforms[r.second] : base.xform;
            return (bool)out.shape || (bool)out.mesh;
        }
        out.shape = it.shapes[r.second];
        out.xform = r.second < it.xforms.size() ? it.xforms[r.second] : glm::mat4(1.f);
        return (bool)out.shape;
    }
    // Cached feature: its bodies are already in the snapshot.
    std::vector<uint32_t> ids = snap.featureBodies(r.first);
    if (r.second >= ids.size()) return false;
    auto bit = snap.bodies.find(ids[r.second]);
    if (bit == snap.bodies.end()) return false;
    auto pit = snap.parts.find(bit->second.partId);
    if (pit == snap.parts.end() || !pit->second.shape) return false;
    out.shape = pit->second.shape;
    out.mesh = pit->second.mesh;
    out.xform = bit->second.xform;
    out.name = bit->second.name;
    return true;
}

// Whether a step's output is shown now (eye on, folder shown). Fresh outputs take their visibility
// from the body they came from (as the document will once the pass lands), unless a saved override
// says otherwise. Hidden parts are never changed by a join, cut or hole.
bool targetShown(const Document& snap, const std::vector<App::RecomputeResults::Item>& done, TargetRef t) {
    std::function<bool(uint32_t, uint32_t, int)> shown = [&](uint32_t fid, uint32_t slot, int depth) -> bool {
        if (const BodyOverride* o = snap.findOverride(fid, slot)) return o->visible;
        const App::RecomputeResults::Item* fr = nullptr;
        for (auto& it : done)
            if (it.featureId == fid) fr = &it;
        std::vector<uint32_t> bids = snap.featureBodies(fid);
        if (!fr && slot < bids.size()) {
            const Body& hb = snap.bodies.at(bids[slot]);
            return hb.visible && snap.isComponentShown(hb.componentId);
        }
        if (fr && fr->isImport) {
            // A fresh import (every rebuild from a file): hidden when one of its folders is hidden,
            // as the bodies already shown would say. Without this a rebuild cut parts that were
            // hidden while you worked (the motor in a hidden import folder).
            const auto& imp = fr->import;
            if (slot >= imp.bodies.size()) return true;
            const Feature* ff = nullptr;
            for (auto& t : snap.timeline) if (t.id == fid) ff = &t;
            std::string ext = ff ? toLower(pathToU8(u8path(ff->importPath).extension())) : std::string();
            bool isStep = ext == ".step" || ext == ".stp";
            if (isStep && snap.compHidden.count(Document::overrideKey(fid, 0))) return false;
            int c = imp.bodies[slot].comp;
            for (int guard = 0; c >= 0 && c < (int)imp.comps.size() && guard < 256; ++guard) {
                uint32_t cs = (c == 0 && imp.firstCompIsRoot) ? 0u
                            : (uint32_t)((isStep ? 1 : 0) + c - (imp.firstCompIsRoot ? 1 : 0));
                if (snap.compHidden.count(Document::overrideKey(fid, cs))) return false;
                c = imp.comps[c].parent;
            }
            return true;
        }
        if (!fr || depth > 64) return true;
        TargetRef r{0, 0};
        if (slot < fr->instanceRefs.size() && fr->instanceRefs[slot].first) r = fr->instanceRefs[slot];
        else if (slot < fr->consumed.size()) r = fr->consumed[slot];
        else if (!fr->consumed.empty()) r = fr->consumed[0];
        return r.first ? shown(r.first, r.second, depth + 1) : true;
    };
    return shown(t.first, t.second, 0);
}

// Bodies alive just before feature `f` (made earlier, not yet replaced) that touch any of the tool
// shapes: what Fusion's Join / Cut pick automatically when you do not choose bodies yourself.
std::vector<TargetRef> autoTargets(const Document& snap, const std::vector<App::RecomputeResults::Item>& done,
                                   const Feature& f, const std::vector<ShapePtr>& tools, std::vector<bool>* hiddenOut = nullptr) {
    std::vector<TargetRef> out;
    int fi = snap.featureIndex(f.id);
    if (fi < 0) return out;
    for (int j = 0; j < fi; ++j) {
        const Feature& src = snap.timeline[j];
        if (src.suppressed) continue;
        // Bodies recomputed in this same pass (always the case right after opening a file) are in
        // `done`, not yet in the snapshot.
        const App::RecomputeResults::Item* fresh = nullptr;
        for (auto& it : done)
            if (it.featureId == src.id) fresh = &it;
        std::vector<uint32_t> ids = snap.featureBodies(src.id);
        size_t count = fresh ? (fresh->isImport ? fresh->import.bodies.size() : fresh->shapes.size()) : ids.size();
        for (uint32_t k = 0; k < count; ++k) {
            // Hidden bodies (eye off, on the body or its folder) are reported, so the caller passes
            // them through untouched (a join or cut never changes a part you hid). They stay in the
            // list, so the step's output slots keep their old numbers.
            bool hiddenNow = !targetShown(snap, done, {src.id, k});
            const char* atDbg = std::getenv("KESTRAL_AT");
            bool dbg = atDbg && (uint32_t)std::atoi(atDbg) == f.id;
            if (fresh) {
                if (!fresh->ok) { if (dbg) std::fprintf(stderr, "AT %u: %u:%u fresh failed\n", f.id, src.id, k); break; }
                bool used = false;
                for (auto& it : done) {
                    if (it.featureId == f.id || !it.ok) continue;
                    int ci = snap.featureIndex(it.featureId);
                    if (ci < 0 || ci >= fi) continue;
                    for (auto& c : it.consumed)
                        if (c.first == src.id && c.second == k) used = true;
                }
                if (used) { if (dbg) std::fprintf(stderr, "AT %u: %u:%u used earlier this pass\n", f.id, src.id, k); continue; }
            } else {
                const Body& b = snap.bodies.at(ids[k]);
                if (b.consumedBy != 0 && b.consumedBy != f.id) {
                    int ci = snap.featureIndex(b.consumedBy);
                    if (ci >= 0 && ci < fi && !snap.timeline[ci].suppressed && !snap.timeline[ci].failed) { if (dbg) std::fprintf(stderr, "AT %u: %u:%u consumed by %u\n", f.id, src.id, k, b.consumedBy); continue; }
                }
            }
            ResolvedTarget rt;
            if (!resolveTarget(snap, done, {src.id, k}, rt) || !rt.shape) continue;
            if (dbg) std::fprintf(stderr, "AT %u: %u:%u alive '%s' hidden %d\n", f.id, src.id, k, rt.name.c_str(), (int)hiddenNow);
            for (auto& t : tools) {
                if (kernel::shapesTouch(t, glm::mat4(1.f), rt.shape, rt.xform)) {
                    if (dbg) std::fprintf(stderr, "AT %u:   touches\n", f.id);
                    out.push_back({src.id, k});
                    if (hiddenOut) hiddenOut->push_back(hiddenNow);
                    break;
                }
            }
        }
    }
    return out;
}

// A polyline a pattern can walk. Closed paths omit the repeated end point.
struct PathCurve {
    uint32_t id = 0;
    std::vector<glm::dvec3> pts;
    bool closed = false;
    bool used = false;
};

bool nearEnd(const glm::dvec3& a, const glm::dvec3& b) { return glm::length(a - b) < 0.05; }

double pathLength(const std::vector<glm::dvec3>& pts, bool closed) {
    int n = (int)pts.size();
    if (n < 2) return 0;
    int segs = closed ? n : n - 1;
    double len = 0;
    for (int i = 0; i < segs; ++i) len += glm::length(pts[(i + 1) % n] - pts[i]);
    return len;
}

void takeCurve(const sk::Sketch& s, const sk::Entity& e, std::vector<PathCurve>& out) {
    if (e.locked || e.construction || e.type == sk::EntType::Point) return;
    PathCurve c;
    c.id = e.id;
    auto w = [&](sk::Vec2 p) { return s.plane.toWorld(p); };
    if (e.type == sk::EntType::Line) {
        glm::dvec3 a = w(s.pt(e.a)), b = w(s.pt(e.b));
        if (glm::length(b - a) < 1e-6) return;
        c.pts = {a, b};
    } else if (e.type == sk::EntType::Arc || e.type == sk::EntType::Circle) {
        sk::Sketch::ArcGeom g;
        if (!s.arcGeom(e, g) || !(g.r > 1e-6)) return;
        constexpr double kPi = 3.14159265358979323846;
        int n = e.type == sk::EntType::Circle ? 64 : std::max(8, (int)std::ceil(std::abs(g.sweep) / (kPi / 32.0)));
        for (int i = 0; i <= n; ++i) c.pts.push_back(w(s.arcPoint(g, (double)i / n)));
        c.closed = e.type == sk::EntType::Circle;
        if (c.closed && c.pts.size() > 2 && nearEnd(c.pts.front(), c.pts.back())) c.pts.pop_back();
    } else if (e.type == sk::EntType::Spline || e.type == sk::EntType::CtrlSpline) {
        auto poly = e.type == sk::EntType::CtrlSpline ? s.ctrlSplinePoly(e, 8) : s.splinePoly(e, 8);
        for (auto& p : poly) c.pts.push_back(w(p));
        c.closed = e.loop;
        if (c.closed && c.pts.size() > 2 && nearEnd(c.pts.front(), c.pts.back())) c.pts.pop_back();
    }
    if (c.pts.size() >= 2) out.push_back(std::move(c));
}

void chainPath(std::vector<PathCurve>& curves, int seed, std::vector<glm::dvec3>& pts, bool& closed) {
    pts = curves[seed].pts;
    closed = curves[seed].closed;
    curves[seed].used = true;
    if (closed) return;
    bool grew = true;
    while (grew) {
        grew = false;
        for (auto& c : curves) {
            if (c.used || c.closed || c.pts.size() < 2) continue;
            bool backF = nearEnd(pts.back(), c.pts.front());
            bool backB = nearEnd(pts.back(), c.pts.back());
            bool frontB = nearEnd(pts.front(), c.pts.back());
            bool frontF = nearEnd(pts.front(), c.pts.front());
            std::vector<glm::dvec3> extra = c.pts;
            if (backF || backB) {
                if (backB) std::reverse(extra.begin(), extra.end());
                extra.erase(extra.begin());
                pts.insert(pts.end(), extra.begin(), extra.end());
            } else if (frontB || frontF) {
                if (frontF) std::reverse(extra.begin(), extra.end());
                extra.pop_back();
                pts.insert(pts.begin(), extra.begin(), extra.end());
            } else continue;
            c.used = true;
            grew = true;
            if (pts.size() > 2 && nearEnd(pts.front(), pts.back())) {
                pts.pop_back();
                closed = true;
                return;
            }
        }
    }
}

bool pathAt(const std::vector<glm::dvec3>& pts, bool closed, double dist, glm::dvec3& pos, glm::dvec3& tan) {
    double len = pathLength(pts, closed);
    int n = (int)pts.size();
    if (n < 2 || !(len > 1e-9)) return false;
    if (closed) {
        dist = std::fmod(dist, len);
        if (dist < 0) dist += len;
    } else dist = std::clamp(dist, 0.0, len);
    int segs = closed ? n : n - 1;
    double run = 0;
    for (int i = 0; i < segs; ++i) {
        glm::dvec3 a = pts[i], b = pts[(i + 1) % n], d = b - a;
        double sl = glm::length(d);
        if (sl < 1e-12) continue;
        if (run + sl >= dist - 1e-8 || i == segs - 1) {
            double u = std::clamp((dist - run) / sl, 0.0, 1.0);
            pos = a + d * u;
            tan = d / sl;
            return true;
        }
        run += sl;
    }
    return false;
}

glm::mat4 turnAlong(const glm::dvec3& from, const glm::dvec3& to) {
    glm::dvec3 a = glm::normalize(from), b = glm::normalize(to);
    double c = std::clamp(glm::dot(a, b), -1.0, 1.0);
    if (c > 1.0 - 1e-8) return glm::mat4(1.f);
    glm::dvec3 axis;
    float ang;
    if (c < -1.0 + 1e-8) {
        axis = glm::abs(a.z) < 0.9 ? glm::cross(a, glm::dvec3(0, 0, 1)) : glm::cross(a, glm::dvec3(1, 0, 0));
        ang = 3.14159265f;
    } else {
        axis = glm::cross(a, b);
        ang = (float)std::acos(c);
    }
    axis = glm::normalize(axis);
    return glm::rotate(glm::mat4(1.f), ang, glm::vec3(axis));
}

// The whole pass runs on a worker thread against a document snapshot. It only touches the
// snapshot and fills results; the main thread applies them.
bool componentGrounded(const Document& snap, uint32_t compId) {
    for (int guard = 0; compId && guard < 64; ++guard) {
        auto it = snap.components.find(compId);
        if (it == snap.components.end()) return false;
        if (it->second.grounded) return true;
        if (compId == snap.rootId) return false;
        compId = it->second.parentId;
    }
    return false;
}

bool refGrounded(const Document& snap, const TargetRef& ref) {
    auto ids = snap.featureBodies(ref.first);
    if (ref.second >= ids.size()) return false;
    auto bit = snap.bodies.find(ids[ref.second]);
    if (bit == snap.bodies.end()) return false;
    return componentGrounded(snap, bit->second.componentId);
}

// An imported file that moved (a design copied to another computer): look for it by name next to the design,
// in a Parts folder beside or above it, then in designs/Parts.
std::string findImportFile(const Document& snap, const std::string& stored) {
    std::error_code ec;
    if (stored.empty() || std::filesystem::exists(u8path(stored), ec)) return stored;
    size_t cut = stored.find_last_of("/\\");
    std::string name = cut == std::string::npos ? stored : stored.substr(cut + 1);
    std::vector<std::filesystem::path> dirs;
    if (!snap.path.empty()) {
        auto d = u8path(snap.path).parent_path();
        dirs = {d, d / "Parts", d.parent_path() / "Parts", d.parent_path().parent_path() / "Parts"};
    }
    dirs.push_back(u8path(platform::designsDir()) / "Parts");
    for (auto& d : dirs)
        if (std::filesystem::exists(d / u8path(name), ec)) return pathToU8(d / u8path(name));
    return stored;
}

// A body name nobody else has: copies and pattern instances must be told apart by name (tools and
// people pick bodies by name). Bodies this feature made before do not count.
std::string freeBodyName(const Document& snap, uint32_t fid, const std::vector<std::string>& made, const std::string& want) {
    auto taken = [&](const std::string& c) {
        for (auto& [bid, b] : snap.bodies)
            if (b.featureId != fid && b.name == c) return true;
        return std::find(made.begin(), made.end(), c) != made.end();
    };
    if (!taken(want)) return want;
    for (int k = 2; k < 1000; ++k) {
        std::string c = want + " (" + std::to_string(k) + ")";
        if (!taken(c)) return c;
    }
    return want;
}

void computeFeature(const Document& snap, const std::vector<App::RecomputeResults::Item>& done,
                    const Feature& f, App::RecomputeResults::Item& item,
                    const CancelToken& cancel, JobSystem* jobs) {
    if (f.type == FeatureType::Import) {
        std::string nm = pathToU8(u8path(f.importPath).filename());
        item.isImport = true;
        item.import = kernel::importFile(findImportFile(snap, f.importPath), cancel,
                                         [jobs, nm](const std::string& s) { jobs->setStatus(nm + ": " + s); });
        return;
    }
    if (f.type != FeatureType::Extrude && f.type != FeatureType::Revolve &&
        f.type != FeatureType::Fillet && f.type != FeatureType::Chamfer &&
        f.type != FeatureType::Shell && f.type != FeatureType::PressPull &&
        f.type != FeatureType::Pattern && f.type != FeatureType::Mirror &&
        f.type != FeatureType::Combine && f.type != FeatureType::Split &&
        f.type != FeatureType::Hole && f.type != FeatureType::Sweep &&
        f.type != FeatureType::Loft && f.type != FeatureType::Move &&
        f.type != FeatureType::MeshConvert && f.type != FeatureType::ConstructPlane &&
        f.type != FeatureType::Remove && f.type != FeatureType::DeleteFace &&
        f.type != FeatureType::Gear && f.type != FeatureType::Thread && f.type != FeatureType::Wing &&
        f.type != FeatureType::Dovetail && f.type != FeatureType::Blade &&
        f.type != FeatureType::Align && f.type != FeatureType::Draft &&
        f.type != FeatureType::Coil && f.type != FeatureType::Pipe &&
        f.type != FeatureType::ReplaceFace &&
        f.type != FeatureType::ConstructAxis && f.type != FeatureType::ConstructPoint &&
        f.type != FeatureType::SplitFace && f.type != FeatureType::Rib && f.type != FeatureType::Web &&
        f.type != FeatureType::Emboss && f.type != FeatureType::Boss && f.type != FeatureType::Snap &&
        f.type != FeatureType::Rest && f.type != FeatureType::Lip && f.type != FeatureType::Silhouette &&
        f.type != FeatureType::Joint && f.type != FeatureType::AsBuilt && f.type != FeatureType::Rigid &&
        f.type != FeatureType::Thicken)
        return;   // Sketch etc: no geometry
    if (f.type == FeatureType::Extrude && !f.extrude) return;
    if (f.type == FeatureType::Revolve && !f.revolve) return;
    if ((f.type == FeatureType::Fillet || f.type == FeatureType::Chamfer || f.type == FeatureType::Shell ||
         f.type == FeatureType::PressPull || f.type == FeatureType::DeleteFace || f.type == FeatureType::Draft ||
         f.type == FeatureType::ReplaceFace || f.type == FeatureType::Thicken) && !f.modify)
        return;
    if ((f.type == FeatureType::Coil || f.type == FeatureType::Pipe) && !f.pathSolid) return;
    if (f.type == FeatureType::Pattern && !f.pattern) return;
    if (f.type == FeatureType::Mirror && !f.mirror) return;
    if (f.type == FeatureType::Combine && !f.combine) return;
    if (f.type == FeatureType::Split && !f.split) return;
    if (f.type == FeatureType::SplitFace && !f.splitFace) return;
    if (f.type == FeatureType::Rib && !f.rib) return;
    if (f.type == FeatureType::Web && !f.web) return;
    if (f.type == FeatureType::Emboss && !f.emboss) return;
    if (f.type == FeatureType::Boss && !f.boss) return;
    if (f.type == FeatureType::Snap && !f.snap) return;
    if (f.type == FeatureType::Rest && !f.rest) return;
    if (f.type == FeatureType::Lip && !f.lip) return;
    if (f.type == FeatureType::Silhouette && !f.silhouette) return;
    if (f.type == FeatureType::Joint && !f.joint) return;
    if (f.type == FeatureType::AsBuilt && !f.asbuilt) return;
    if (f.type == FeatureType::Rigid && !f.rigid) return;
    if (f.type == FeatureType::Align && !f.align) return;
    if (f.type == FeatureType::Hole && !f.hole) return;
    if ((f.type == FeatureType::Sweep || f.type == FeatureType::Loft) && !f.multi) return;
    if (f.type == FeatureType::Move && !f.move) return;
    if (f.type == FeatureType::MeshConvert && !f.modify) return;
    if (f.type == FeatureType::ConstructPlane && !f.cplane) return;
    if (f.type == FeatureType::ConstructPlane) return;   // planes are data, derived on the main thread
    if (f.type == FeatureType::ConstructAxis && !f.caxis) return;
    if (f.type == FeatureType::ConstructAxis) return;
    if (f.type == FeatureType::ConstructPoint && !f.cpoint) return;
    if (f.type == FeatureType::ConstructPoint) return;
    if (f.type == FeatureType::Remove) {
        if (!f.remove) return;
        for (auto& r : f.remove->targets) item.consumed.push_back(r);   // gone from here on, nothing new
        return;
    }

    auto resolve = [&](const TargetRef& r, ResolvedTarget& out) {
        if (!resolveTarget(snap, done, r, out))
            throw std::runtime_error("A target body no longer exists. Edit the feature and select new targets.");
        if (!out.shape)
            throw std::runtime_error("A target body has no solid geometry (mesh bodies cannot be modified yet)");
    };

    // ---- gear / thread ----------------------------------------------------------------
    if (f.type == FeatureType::Gear) {
        if (!f.gear) return;
        ShapePtr g = kernel::makeGear(gearSpecOf(*f.gear, snap.params));
        item.shapes.push_back(g);
        item.meshes.push_back(kernel::tessellate(*g));
        item.names.push_back(f.name);
        item.xforms.push_back(gearPlacement(*f.gear, snap.params));
        return;
    }
    if (f.type == FeatureType::Wing) {
        if (!f.wing) return;
        ShapePtr w = kernel::makeWing(wingSpecOf(*f.wing, snap.params));
        item.shapes.push_back(w);
        item.meshes.push_back(kernel::tessellate(*w));
        item.names.push_back(f.name);
        item.xforms.push_back(glm::mat4(1.f));
        return;
    }
    if (f.type == FeatureType::Blade) {
        if (!f.blade) return;
        kernel::BladeParts parts = kernel::makeBlades(bladeSpecOf(*f.blade, snap.params));
        glm::mat4 place = bladePlacement(*f.blade, snap.params);
        for (auto* s : {&parts.rotor, &parts.duct}) {
            if (!*s) continue;
            item.shapes.push_back(*s);
            item.meshes.push_back(kernel::tessellate(**s));
            item.names.push_back(s == &parts.duct ? f.name + " duct" : f.name);
            item.xforms.push_back(place);
        }
        return;
    }
    if (f.type == FeatureType::Dovetail) {
        if (!f.dovetail) return;
        const DovetailParams& dv = *f.dovetail;
        kernel::DovetailSpec spec = dovetailSpecOf(dv, snap.params);
        glm::mat4 place = dovetailPlacement(dv, snap.params);
        kernel::DovetailParts parts = kernel::makeDovetail(spec);
        auto addBody = [&](const ShapePtr& s, const std::string& name, const glm::mat4& xf) {
            item.shapes.push_back(s);
            item.meshes.push_back(kernel::tessellate(*s));
            item.names.push_back(name);
            item.xforms.push_back(xf);
        };
        if (!kernel::dovetailIsConnector(spec.type)) {
            for (auto& [name, s] : parts.bodies) addBody(s, f.name + " " + name, place);
            return;
        }
        bool key = spec.type == kernel::DovetailType::Key;
        if (dv.targets.empty() || key) addBody(parts.male, f.name + (key ? " key" : ""), place);
        ShapePtr male = kernel::placeShape(parts.male, place), female = kernel::placeShape(parts.female, place);
        for (size_t i = 0; i < dv.targets.size(); ++i) {
            ResolvedTarget rt;
            resolve(dv.targets[i], rt);
            bool join = i == 0 && !key;
            auto outs = kernel::applyBool(join ? kernel::BoolOp::Join : kernel::BoolOp::Cut, join ? male : female, {{rt.shape, rt.xform}});
            for (auto& o : outs) addBody(o, rt.name.empty() ? f.name : rt.name, glm::mat4(1.f));
            item.consumed.push_back(dv.targets[i]);
        }
        return;
    }
    if (f.type == FeatureType::Coil || f.type == FeatureType::Pipe) {
        const SweepSolidParams& p = *f.pathSolid;
        auto ev = [&](const std::string& e) { return peval::eval(e, snap.params); };
        glm::dvec3 origin(ev(p.xExpr), ev(p.yExpr), ev(p.zExpr));
        glm::dvec3 axis = p.axis == 0 ? glm::dvec3(1, 0, 0) : p.axis == 1 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
        ShapePtr solid;
        if (f.type == FeatureType::Coil) {
            solid = kernel::makeCoil(origin, axis, ev(p.diaExpr), ev(p.heightExpr), ev(p.turnsExpr), ev(p.sectionExpr), p.leftHand);
        } else if (p.pathSketch) {
            auto sit = snap.sketches.find(p.pathSketch);
            if (sit == snap.sketches.end()) throw std::runtime_error("The pipe path sketch is missing");
            solid = kernel::makePipe(chainedPath(sit->second), ev(p.diaExpr) * 0.5, ev(p.innerExpr) * 0.5);
        } else {
            double len = ev(p.heightExpr);
            if (!(std::abs(len) > 0)) throw std::runtime_error("Pipe length must be above 0, or pick a path sketch");
            CurveSeg seg;
            seg.kind = CurveSeg::Kind::Line;
            seg.a = origin;
            seg.b = origin + axis * len;
            seg.normal = std::abs(axis.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
            solid = kernel::makePipe({seg}, ev(p.diaExpr) * 0.5, ev(p.innerExpr) * 0.5);
        }
        item.shapes.push_back(solid);
        item.meshes.push_back(kernel::tessellate(*solid));
        item.names.push_back(f.name);
        item.xforms.push_back(glm::mat4(1.f));
        return;
    }
    if (f.type == FeatureType::Thread) {
        if (!f.thread) return;
        const ThreadParams& t = *f.thread;
        if (!t.face.feature || !t.modeled) return;   // still picking, or cosmetic (drawn only)
        ResolvedTarget rt;
        TargetRef ref{t.face.feature, t.face.slot};
        resolve(ref, rt);
        if (t.face.faceTotal && kernel::faceCount(rt.shape) != t.face.faceTotal)
            throw std::runtime_error("The body changed shape, so the picked face no longer matches. Edit the thread and pick the face again.");
        kernel::RoundFace rf;
        if (!kernel::roundFace(rt.shape, rt.xform, t.face.face, rf))
            throw std::runtime_error("The picked face is not round. Edit the thread and pick a shaft or hole.");
        std::string tw;
        ShapePtr out = kernel::cutThread(rt.shape, rt.xform, threadCutOf(t, rf, snap.params), &tw);
        if (!tw.empty()) item.warn = f.name + ": " + tw;
        item.shapes.push_back(out);
        item.meshes.push_back(kernel::tessellate(*out));
        item.names.push_back(rt.name.empty() ? f.name : rt.name);
        item.xforms.push_back(glm::mat4(1.f));
        item.consumed.push_back(ref);
        return;
    }

    // ---- fillet / chamfer / shell / press pull --------------------------------------
    if (f.type == FeatureType::Fillet || f.type == FeatureType::Chamfer || f.type == FeatureType::Shell ||
        f.type == FeatureType::PressPull || f.type == FeatureType::DeleteFace || f.type == FeatureType::Draft ||
        f.type == FeatureType::ReplaceFace || f.type == FeatureType::Thicken) {
        const ModifyParams& m = *f.modify;
        bool fullRound = f.type == FeatureType::Fillet && m.side == 1;
        double v = f.type == FeatureType::DeleteFace ? 0.0 : peval::eval(m.valueExpr, snap.params);
        if (!fullRound && f.type != FeatureType::DeleteFace && f.type != FeatureType::ReplaceFace && std::abs(v) < 1e-9) return;   // waiting for a value
        for (size_t ti = 0; ti < m.targets.size(); ++ti) {
            ResolvedTarget rt;
            resolve(m.targets[ti], rt);
            uint32_t total = kernel::edgeCount(rt.shape);
            if (m.edgeTotal != 0 && total != m.edgeTotal)
                throw std::runtime_error("The body changed shape, so the stored edge picks no longer match. Edit the feature and reselect them.");
            std::vector<uint32_t> edgeIdx, faceIdx;
            for (auto& e : m.edges) if (e.first == ti) edgeIdx.push_back(e.second);
            for (auto& fc : m.faces) if (fc.first == ti) faceIdx.push_back(fc.second);
            if (f.type == FeatureType::Thicken) {
                if (faceIdx.empty()) return;
                if (m.faceTotal != 0 && kernel::faceCount(rt.shape) != m.faceTotal)
                    throw std::runtime_error("The body changed shape, so the picked face no longer matches. Edit the feature and pick it again.");
                if (!(v > 1e-6)) throw std::runtime_error("Thickness must be greater than zero");
                ShapePtr wall = kernel::thickenFaces(rt.shape, rt.xform, faceIdx, v, m.side);
                item.shapes.push_back(wall);
                item.meshes.push_back(kernel::tessellate(*wall));
                item.names.push_back(f.name);
                item.xforms.push_back(glm::mat4(1.f));
                continue;
            }
            ShapePtr out;
            if (f.type == FeatureType::Fillet) {
                if (m.side == 1) {
                    if (faceIdx.empty()) throw std::runtime_error("Click the face to replace");
                    out = kernel::fullRoundFace(rt.shape, faceIdx[0]);
                } else {
                kernel::FilletReport rep;
                double r2 = v;
                if (!m.endExpr.empty()) {
                    r2 = peval::eval(m.endExpr, snap.params);
                    if (!(r2 > 1e-6)) throw std::runtime_error("End radius must be greater than zero");
                }
                out = kernel::filletEdges(rt.shape, edgeIdx, v, rep, r2);
                if (rep.partial) {
                    item.warn = f.name + ": some edges could not take radius " + fmtG(v) + ":";
                    for (auto& fe : rep.failed)
                        item.warn += "\n  Edge " + std::to_string(fe.edge) + ": max " + fmtG(fe.maxRadius);
                }
                }
            } else if (f.type == FeatureType::Chamfer) {
                double d2 = v;
                if (!m.endExpr.empty()) {
                    d2 = peval::eval(m.endExpr, snap.params);
                    if (!(d2 > 1e-6)) throw std::runtime_error("The second distance must be greater than zero");
                }
                out = kernel::chamferEdges(rt.shape, edgeIdx, v, d2);
            } else if (f.type == FeatureType::Shell) {
                out = kernel::shellBody(rt.shape, faceIdx, v, m.side);
            } else if (f.type == FeatureType::DeleteFace) {
                out = kernel::deleteFaces(rt.shape, faceIdx);
            } else if (f.type == FeatureType::Draft) {
                glm::dvec3 pull = m.axis == 0 ? glm::dvec3(1, 0, 0) : m.axis == 1 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
                out = kernel::draftFaces(rt.shape, faceIdx, v, pull);
            } else if (f.type == FeatureType::ReplaceFace) {
                if (faceIdx.empty()) throw std::runtime_error("Select the faces to replace");
                glm::dvec3 planeN = m.axis == 1 ? glm::dvec3(0, -1, 0) : m.axis == 2 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1);
                glm::dvec3 c, n;
                bool planar = false;
                if (!kernel::faceInfo(rt.shape, faceIdx[0], c, n, planar) || !planar)
                    throw std::runtime_error("Replace Face needs a flat face");
                glm::dvec3 cw = glm::dvec3(rt.xform * glm::dvec4(c, 1.0));
                glm::dvec3 nw = glm::normalize(glm::dmat3(rt.xform) * n);
                double denom = glm::dot(planeN, nw);
                if (std::abs(denom) < 0.2) throw std::runtime_error("That face is not parallel to the plane");
                out = kernel::offsetFaces(rt.shape, faceIdx, (v - glm::dot(planeN, cw)) / denom);
            } else {
                out = kernel::offsetFaces(rt.shape, faceIdx, v);
            }
            item.shapes.push_back(out);
            item.meshes.push_back(kernel::tessellate(*out));
            item.names.push_back(rt.name.empty() ? f.name : rt.name);
            item.xforms.push_back(rt.xform);
            item.consumed.push_back(m.targets[ti]);
        }
        return;
    }

    // ---- patterns: pure instances of the target part, zero new kernel geometry ---------
    if (f.type == FeatureType::Pattern) {
        const PatternParams& p = *f.pattern;
        if (p.onPath) {
            std::vector<glm::dvec3> pts;
            bool closed = false;
            if (p.pathSketch) {
                auto sit = snap.sketches.find(p.pathSketch);
                if (sit == snap.sketches.end()) throw std::runtime_error("The sketch used as a path is missing");
                std::vector<PathCurve> curves;
                for (auto& [id, e] : sit->second.ents) takeCurve(sit->second, e, curves);
                int seed = -1;
                if (p.pathEnt) {
                    for (int i = 0; i < (int)curves.size(); ++i)
                        if (curves[i].id == p.pathEnt) seed = i;
                    if (seed < 0) throw std::runtime_error("The curve used as a path is gone. Edit the pattern and pick it again.");
                } else {
                    double best = -1;
                    for (int i = 0; i < (int)curves.size(); ++i) {
                        double L = pathLength(curves[i].pts, curves[i].closed);
                        if (L > best) { best = L; seed = i; }
                    }
                }
                if (seed < 0) throw std::runtime_error("That sketch has no curve to follow");
                chainPath(curves, seed, pts, closed);
            } else if (p.pathBody.first) {
                ResolvedTarget rt;
                resolve(p.pathBody, rt);
                kernel::EdgeGeom g;
                if (!kernel::edgeGeom(rt.shape, rt.xform, p.pathEdge, g) || g.pts.size() < 2)
                    throw std::runtime_error("The edge used as a path is gone. Edit the pattern and pick it again.");
                pts = g.pts;
                if (pts.size() > 3 && nearEnd(pts.front(), pts.back())) { pts.pop_back(); closed = true; }
            } else throw std::runtime_error("Pick a sketch curve or an edge to follow");
            if (p.flip && !pts.empty()) std::reverse(pts.begin(), pts.end());
            double length = pathLength(pts, closed);
            if (!(length > 1e-6)) throw std::runtime_error("That path is too short");
            long n = std::max(1L, std::lround(peval::eval(p.countExpr, snap.params)));
            if (n > 5000) throw std::runtime_error("That is more than 5000 instances; reduce the count");
            double dist = peval::eval(p.spacingExpr, snap.params);
            if (n > 1 && std::abs(dist) < 1e-9) throw std::runtime_error("Distance must not be zero");
            double step = n > 1 ? (p.distType == 1 ? dist / (n - 1) : dist) : 0;
            double reach = step * std::max(0L, n - 1);
            if (!closed && reach > length + std::max(0.5, 0.002 * length))
                throw std::runtime_error("The path is shorter than the pattern");
            if (closed && n > 1 && std::abs(std::fmod(step, length)) < 0.05)
                throw std::runtime_error("Two copies land on the same spot");
            glm::dvec3 p0, t0;
            if (!pathAt(pts, closed, 0, p0, t0)) throw std::runtime_error("That path is too short");
            for (const TargetRef& ref : p.targets) {
                ResolvedTarget rt;
                resolve(ref, rt);
                std::string base = rt.name.empty() ? f.name : rt.name;
                for (long ix = 0; ix < n; ++ix) {
                    glm::mat4 x(1.f);
                    if (ix != 0) {
                        glm::dvec3 pi, ti;
                        if (!pathAt(pts, closed, step * ix, pi, ti))
                            throw std::runtime_error("The path is shorter than the pattern");
                        if (p.orient == 1)
                            x = glm::translate(glm::mat4(1.f), glm::vec3(pi)) * turnAlong(t0, ti) *
                                glm::translate(glm::mat4(1.f), glm::vec3(-p0));
                        else
                            x = glm::translate(glm::mat4(1.f), glm::vec3(pi - p0));
                    }
                    item.shapes.push_back(nullptr);
                    item.meshes.push_back(nullptr);
                    item.names.push_back(ix == 0 ? base : freeBodyName(snap, f.id, item.names, base + ":" + std::to_string(ix)));
                    item.xforms.push_back(x * rt.xform);
                    item.instanceRefs.push_back(ref);
                }
                item.consumed.push_back(ref);
            }
            return;
        }
        long nx = std::max(1L, std::lround(peval::eval(p.countExpr, snap.params)));
        double sx = peval::eval(p.spacingExpr, snap.params);
        long ny = 1;
        double sy = 0;
        if (!p.circular) {
            ny = std::max(1L, std::lround(peval::eval(p.countYExpr, snap.params)));
            sy = peval::eval(p.spacingYExpr, snap.params);
        }
        if (nx * ny > 5000) throw std::runtime_error("That is more than 5000 instances; reduce the count");
        double total = 360.0, step = 0;
        if (p.circular) {
            total = peval::eval(p.angleExpr, snap.params);
            if (std::abs(total) < 1e-6) throw std::runtime_error("Angle must not be zero");
            step = nx > 1 ? total / nx : 0.0;
        }
        glm::dvec3 axis = p.axis == 0 ? glm::dvec3(1, 0, 0) : p.axis == 1 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
        glm::dvec3 axisO(0);
        if (p.axis == 3 && glm::length(p.axisD) > 1e-9) { axis = glm::normalize(p.axisD); axisO = p.axisO; }
        for (const TargetRef& ref : p.targets) {
            ResolvedTarget rt;
            resolve(ref, rt);
            std::string base = rt.name.empty() ? f.name : rt.name;
            long n = 0;
            for (long iy = 0; iy < ny; ++iy) {
                for (long ix = 0; ix < nx; ++ix) {
                    glm::mat4 x(1.f);
                    if (p.circular) {
                        if (ix != 0)
                            x = glm::translate(glm::mat4(1.f), glm::vec3(axisO)) *
                                glm::rotate(glm::mat4(1.f), (float)glm::radians(step * ix), glm::vec3(axis)) *
                                glm::translate(glm::mat4(1.f), -glm::vec3(axisO));
                    } else if (ix != 0 || iy != 0) {
                        x = glm::translate(glm::mat4(1.f), glm::vec3((float)(sx * ix), (float)(sy * iy), 0.f));
                    }
                    item.shapes.push_back(nullptr);   // instances share the target part: no new geometry
                    item.meshes.push_back(nullptr);
                    item.names.push_back(n == 0 ? base : freeBodyName(snap, f.id, item.names, base + ":" + std::to_string(n)));
                    item.xforms.push_back(x * rt.xform);   // copies move in world space
                    item.instanceRefs.push_back(ref);
                    ++n;
                }
            }
            item.consumed.push_back(ref);
        }
        return;
    }

    // ---- mirror / split / combine / hole ----------------------------------------------
    if (f.type == FeatureType::Mirror) {
        const MirrorParams& mp = *f.mirror;
        glm::dvec3 n = mp.plane == 0 ? glm::dvec3(0, 0, 1) : mp.plane == 1 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        glm::dvec3 o(0);
        if (mp.plane == 3) {
            o = mp.planeO;
            n = glm::normalize(mp.planeN);
        }
        for (const TargetRef& ref : mp.targets) {
            ResolvedTarget rt;
            resolve(ref, rt);
            // Mirror in the body's own frame (the world plane brought into it), keep its placement.
            glm::dmat4 xf(rt.xform), inv = glm::inverse(xf);
            glm::dvec3 lo = glm::dvec3(inv * glm::dvec4(o, 1.0));
            glm::dvec3 ln = glm::normalize(glm::transpose(glm::dmat3(xf)) * n);
            ShapePtr out = kernel::mirrorShape(rt.shape, lo, ln);
            item.shapes.push_back(out);
            item.meshes.push_back(kernel::tessellate(*out));
            // The mirrored copy sits beside its source, so it gets its own name: Right and Left swap,
            // otherwise "Body (1)" like a pasted copy.
            std::string base = rt.name.empty() ? f.name : rt.name, want = base;
            for (auto [a, b] : {std::pair<const char*, const char*>{"Right", "Left"}, {"Left", "Right"},
                                {"right", "left"}, {"left", "right"}}) {
                size_t p = base.find(a);
                if (p != std::string::npos) { want = base.substr(0, p) + b + base.substr(p + std::strlen(a)); break; }
            }
            if (want == base) want = base + " (1)";
            item.names.push_back(freeBodyName(snap, f.id, item.names, want));
            item.xforms.push_back(rt.xform);
            item.instanceRefs.push_back(ref);   // keeps the source component; the shape itself is new
        }
        return;
    }

    if (f.type == FeatureType::Split) {
        const SplitParams& sp = *f.split;
        glm::dvec3 o(0), n;
        bool world = false;
        if (sp.plane == 3) {
            world = true;
            if (sp.sketchId) {
                auto sit = snap.sketches.find(sp.sketchId);
                if (sit == snap.sketches.end()) throw std::runtime_error("The sketch used to split is missing");
                const sk::Sketch& s = sit->second;
                const sk::Entity* e = s.find(sp.sketchEnt);
                if (!e || e->type != sk::EntType::Line) throw std::runtime_error("The sketch line used to split is gone. Edit the split and pick it again.");
                glm::dvec3 a = s.plane.toWorld(s.pt(e->a)), b = s.plane.toWorld(s.pt(e->b));
                glm::dvec3 dir = b - a;
                if (glm::length(dir) < 1e-9) throw std::runtime_error("The sketch line is too short to split with");
                n = glm::cross(glm::normalize(dir), s.plane.normal());
                if (glm::length(n) < 1e-9) throw std::runtime_error("That sketch line cannot define a cut");
                n = glm::normalize(n);
                o = a;
            } else {
                o = sp.toolO;
                n = sp.toolN;
                if (glm::length(n) < 1e-9) throw std::runtime_error("Pick a face, an edge or a sketch line to split with");
                n = glm::normalize(n);
            }
        } else {
            n = sp.plane == 0 ? glm::dvec3(0, 0, 1) : sp.plane == 1 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        }
        for (const TargetRef& ref : sp.targets) {
            ResolvedTarget rt;
            resolve(ref, rt);
            glm::dvec3 lo = o, ln = n;
            if (world) {
                glm::dmat4 xf(rt.xform), inv = glm::inverse(xf);
                lo = glm::dvec3(inv * glm::dvec4(o, 1.0));
                ln = glm::normalize(glm::transpose(glm::dmat3(xf)) * n);
            }
            auto pieces = kernel::splitByPlane(rt.shape, lo, ln);
            int k = 0;
            for (auto& pc : pieces) {
                item.shapes.push_back(pc);
                item.meshes.push_back(kernel::tessellate(*pc));
                // The first piece keeps the body's name (splits used to stack " 1 1 1" on it).
                std::string base = rt.name.empty() ? f.name : rt.name;
                item.names.push_back(k++ == 0 ? base : freeBodyName(snap, f.id, item.names, base + " " + std::to_string(k)));
                item.xforms.push_back(rt.xform);
            }
            item.consumed.push_back(ref);
        }
        return;
    }
    if (f.type == FeatureType::SplitFace) {
        const SplitFaceParams& sp = *f.splitFace;
        if (sp.targets.empty()) return;
        if (!sp.sketchId && sp.offsetExpr.empty()) return;
        std::vector<CurveSeg> curve;
        glm::dvec3 curveN(0, 0, 1);
        if (sp.sketchId) {
            auto sit = snap.sketches.find(sp.sketchId);
            if (sit == snap.sketches.end()) throw std::runtime_error("The split sketch is missing");
            curve = chainedPath(sit->second);
            if (curve.empty()) throw std::runtime_error("The sketch has no curve");
            curveN = sit->second.plane.normal();
            if (glm::length(curveN) < 1e-9) throw std::runtime_error("The sketch has no direction to project along");
        }
        double off = sp.sketchId ? 0.0 : peval::eval(sp.offsetExpr, snap.params);
        glm::dvec3 n = sp.plane == 1 ? glm::dvec3(0, -1, 0) : sp.plane == 2 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1);
        glm::dvec3 o = n * off;
        for (const TargetRef& ref : sp.targets) {
            ResolvedTarget rt;
            resolve(ref, rt);
            if (sp.faceTotal && kernel::faceCount(rt.shape) != sp.faceTotal)
                throw std::runtime_error("The body changed shape, so the faces no longer match. Edit Split Face and pick them again.");
            glm::dmat4 xf(rt.xform), inv = glm::inverse(xf);
            ShapePtr out;
            if (sp.sketchId) {
                auto toP = [&](glm::dvec3 p) { return glm::dvec3(inv * glm::dvec4(p, 1.0)); };
                auto toD = [&](glm::dvec3 d) { return glm::dvec3(glm::transpose(glm::dmat3(xf)) * d); };
                std::vector<CurveSeg> local = curve;
                for (auto& c : local) {
                    c.a = toP(c.a);
                    c.b = toP(c.b);
                    c.center = toP(c.center);
                    glm::dvec3 cn = toD(c.normal);
                    if (glm::length(cn) > 1e-9) c.normal = glm::normalize(cn);
                }
                out = kernel::splitFacesByCurve(rt.shape, local, toD(curveN), sp.faces);
            } else {
                glm::dvec3 lo = glm::dvec3(inv * glm::dvec4(o, 1.0));
                glm::dvec3 ln = glm::normalize(glm::transpose(glm::dmat3(xf)) * n);
                out = kernel::splitFaces(rt.shape, lo, ln, sp.faces);
            }
            item.shapes.push_back(out);
            item.meshes.push_back(kernel::tessellate(*out));
            item.names.push_back(rt.name.empty() ? f.name : rt.name);
            item.xforms.push_back(rt.xform);
            item.consumed.push_back(ref);
        }
        return;
    }
    if (f.type == FeatureType::Align) {
        const AlignParams& a = *f.align;
        if (!a.from.ref.first || !a.to.ref.first) return;
        // oppose: two flat faces mate (normals end up opposite). Circles and round faces line their axes up.
        auto snapOf = [&](const AlignParams::Pick& pk, glm::dvec3& p, glm::dvec3& dir, bool& hasDir, bool& oppose) {
            hasDir = false;
            oppose = false;
            ResolvedTarget rt;
            resolve(pk.ref, rt);
            glm::dmat4 xf(rt.xform);
            if (pk.kind == 0) {
                if (pk.total && kernel::faceCount(rt.shape) != pk.total)
                    throw std::runtime_error("The body changed shape, so the align face no longer matches. Edit Align and pick again.");
                glm::dvec3 axO, axD;
                if (kernel::faceRoundAxis(rt.shape, pk.sub, axO, axD)) {
                    p = glm::dvec3(xf * glm::dvec4(axO, 1.0));
                    dir = glm::normalize(glm::dmat3(xf) * axD);
                    hasDir = true;
                    return;
                }
                glm::dvec3 c, n;
                bool planar = false;
                if (!kernel::faceInfo(rt.shape, pk.sub, c, n, planar))
                    throw std::runtime_error("The align face is gone. Edit Align and pick again.");
                p = glm::dvec3(xf * glm::dvec4(c, 1.0));
                dir = glm::normalize(glm::dmat3(xf) * n);
                hasDir = true;
                oppose = planar;
            } else {
                if (std::getenv("KESTRAL_CHECK")) std::fprintf(stderr, "ALIGN %s ref %u:%u edges now %u saved %u\n", f.name.c_str(), pk.ref.first, pk.ref.second, kernel::edgeCount(rt.shape), pk.total);
                if (pk.total && kernel::edgeCount(rt.shape) != pk.total)
                    throw std::runtime_error("The body changed shape, so the align edge no longer matches. Edit Align and pick again.");
                kernel::EdgeGeom g;
                if (!kernel::edgeGeom(rt.shape, rt.xform, pk.sub, g))
                    throw std::runtime_error("The align edge is gone. Edit Align and pick again.");
                if (pk.kind == 2) {
                    p = pk.end == 2 ? g.b : g.a;
                } else if (g.type == 1 || g.type == 2) {
                    // The outline of a round face: the circle's own center, and its axis.
                    p = g.center;
                    dir = glm::length(g.normal) > 1e-9 ? glm::normalize(g.normal) : glm::dvec3(0, 0, 1);
                    hasDir = true;
                } else if (g.type == 0) {
                    p = (g.a + g.b) * 0.5;
                    dir = glm::normalize(g.b - g.a);
                    hasDir = glm::length(g.b - g.a) > 1e-9;
                } else if (!g.pts.empty()) {
                    p = g.pts[g.pts.size() / 2];
                } else {
                    p = (g.a + g.b) * 0.5;
                }
            }
        };
        glm::dvec3 fp, fd, tp, td;
        bool fdir = false, tdir = false, fOpp = false, tOpp = false;
        snapOf(a.from, fp, fd, fdir, fOpp);
        snapOf(a.to, tp, td, tdir, tOpp);
        // All in double precision: one rounding to float at the very end, on the final placement.
        glm::dmat4 R(1.0);
        glm::dvec3 axis = tdir ? glm::normalize(td) : glm::dvec3(0);
        if (fdir && tdir && a.from.kind != 2 && a.to.kind != 2) {
            glm::dvec3 b = (fOpp && tOpp) ? -td : td;   // flat faces mate; circles and edges run the same way
            if (a.flip) b = -b;
            glm::dvec3 u = glm::normalize(fd), v = glm::normalize(b);
            glm::dvec3 c = glm::cross(u, v);
            double s = glm::length(c), dd = glm::dot(u, v);
            if (s < 1e-12) {
                if (dd < 0) {
                    // Half turn about any axis square to u: take the one that disturbs the part least.
                    glm::dvec3 ax = std::abs(u.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
                    ax = glm::normalize(glm::cross(u, ax));
                    R = glm::rotate(glm::dmat4(1.0), glm::pi<double>(), ax);
                }
            } else {
                R = glm::rotate(glm::dmat4(1.0), std::atan2(s, dd), glm::normalize(c));
            }
        } else if (a.flip && tdir) {
            R = glm::rotate(glm::dmat4(1.0), glm::pi<double>(), std::abs(axis.z) < 0.9 ? glm::normalize(glm::cross(axis, glm::dvec3(0, 0, 1)))
                                                                                       : glm::normalize(glm::cross(axis, glm::dvec3(1, 0, 0))));
        }
        double angle = a.angleExpr.empty() ? 0.0 : peval::eval(a.angleExpr, snap.params);
        double offset = a.offsetExpr.empty() ? 0.0 : peval::eval(a.offsetExpr, snap.params);
        if (std::abs(angle) > 1e-12 && !tdir) throw std::runtime_error("Angle needs a destination with a direction (a face, a circle or an edge)");
        if (std::abs(offset) > 1e-12 && !tdir) throw std::runtime_error("Offset needs a destination with a direction (a face, a circle or an edge)");
        glm::dmat4 spin = std::abs(angle) > 1e-12 ? glm::rotate(glm::dmat4(1.0), glm::radians(angle), axis) : glm::dmat4(1.0);
        glm::dvec3 land = tp + axis * offset;
        glm::dmat4 Td = glm::translate(glm::dmat4(1.0), land) * spin * R * glm::translate(glm::dmat4(1.0), -fp);
        for (const TargetRef& ref : a.targets) {
            if (ref == a.to.ref) continue;
            ResolvedTarget rt;
            resolveTarget(snap, done, ref, rt);
            item.shapes.push_back(nullptr);
            item.meshes.push_back(nullptr);
            item.names.push_back(rt.name.empty() ? f.name : rt.name);
            item.xforms.push_back(glm::mat4(Td * glm::dmat4(rt.xform)));
            item.instanceRefs.push_back(ref);
            item.consumed.push_back(ref);
        }
        return;
    }

    if (f.type == FeatureType::Combine) {
        const CombineParams& cp = *f.combine;
        if (cp.tools.empty() && cp.tool.first != 0) {
            // Files from before tool lists: one tool applied to every target.
            ResolvedTarget tool;
            resolve(cp.tool, tool);
            std::vector<kernel::BoolTarget> targets;
            std::string firstName;
            for (size_t i = 0; i < cp.targets.size(); ++i) {
                ResolvedTarget rt;
                resolve(cp.targets[i], rt);
                if (i == 0) firstName = rt.name;
                targets.push_back({rt.shape, rt.xform});
            }
            auto outs = kernel::applyBool(boolOpOf(cp.op), kernel::placeShape(tool.shape, tool.xform), targets);
            for (size_t i = 0; i < outs.size(); ++i) {
                item.shapes.push_back(outs[i]);
                item.meshes.push_back(kernel::tessellate(*outs[i]));
                item.names.push_back(i == 0 && !firstName.empty() ? firstName : f.name);
                item.xforms.push_back(glm::mat4(1.f));
            }
            item.consumed.push_back(cp.tool);
            for (auto& t : cp.targets) item.consumed.push_back(t);
            return;
        }
        if (cp.targets.empty() || cp.tools.empty()) return;   // still picking
        ResolvedTarget target;
        resolve(cp.targets[0], target);
        // All tools fused into one, in world space.
        std::vector<ShapePtr> placed;
        for (auto& t : cp.tools) {
            ResolvedTarget rt;
            resolve(t, rt);
            placed.push_back(kernel::placeShape(rt.shape, rt.xform));
        }
        ShapePtr tool = placed[0];
        if (placed.size() > 1) {
            std::vector<kernel::BoolTarget> ts;
            for (size_t i = 1; i < placed.size(); ++i) ts.push_back({placed[i], glm::mat4(1.f)});
            tool = kernel::applyBool(kernel::BoolOp::Join, tool, ts)[0];
        }
        auto outs = kernel::applyBool(boolOpOf(cp.op), tool, {{target.shape, target.xform}});
        if (f.rules >= 1)   // flush faces become one face (steps made from fix1064 on)
            for (auto& o : outs) o = kernel::mergeFlushFaces(o);
        for (size_t i = 0; i < outs.size(); ++i) {
            item.shapes.push_back(outs[i]);
            item.meshes.push_back(kernel::tessellate(*outs[i]));
            item.names.push_back(target.name.empty() ? f.name : target.name);
            item.xforms.push_back(glm::mat4(1.f));
        }
        item.consumed.push_back(cp.targets[0]);
        if (!cp.keepTools)
            for (auto& t : cp.tools) item.consumed.push_back(t);
        return;
    }

    if (f.type == FeatureType::Hole) {
        const HoleParams& hp = *f.hole;
        struct HoleCircle { glm::dvec3 c; double r; glm::dvec3 dir; bool fromSketch; };
        std::vector<HoleCircle> holes;
        double dia = 0;
        bool hasDia = !hp.diaExpr.empty();
        if (hasDia) {
            dia = peval::eval(hp.diaExpr, snap.params);
            if (dia <= 1e-6) throw std::runtime_error("Diameter must be greater than zero");
        }
        if (hp.sketchId) {
            auto sit = snap.sketches.find(hp.sketchId);
            if (sit == snap.sketches.end()) return;   // still picking
            const sk::Sketch& sk = sit->second;
            for (auto& [id, e] : sk.ents) {
                if (e.type != sk::EntType::Circle || e.construction) continue;
                sk::Vec2 cv;
                double r;
                if (!sk.circleGeom(e, cv, r) || r <= 1e-6) continue;
                holes.push_back({sk.plane.toWorld(cv), hasDia ? dia * 0.5 : r, sk.plane.normal(), true});
            }
            if (holes.empty()) throw std::runtime_error("Draw circles in the sketch to place holes");
        } else {
            if (hp.points.empty()) return;   // still placing
            if (!hasDia) throw std::runtime_error("Give the hole a diameter");
            for (auto& pt : hp.points) {
                glm::dvec3 d = glm::length(pt.dir) > 1e-12 ? glm::normalize(pt.dir) : glm::dvec3(0, 0, -1);
                holes.push_back({pt.pos, dia * 0.5, d, false});
            }
        }
        bool through = hp.depthExpr.empty();
        double depth = through ? 0 : peval::eval(hp.depthExpr, snap.params);
        if (!through && depth <= 1e-6) throw std::runtime_error("Depth must be greater than zero");
        double cbR = 0, cbDepth = 0, csR = 0, csHalf = 0;
        if (hp.kind == 1) {
            cbR = peval::eval(hp.cbDiaExpr, snap.params) * 0.5;
            cbDepth = peval::eval(hp.cbDepthExpr, snap.params);
            if (cbDepth <= 1e-6) throw std::runtime_error("Counterbore depth must be greater than zero");
        } else         if (hp.kind == 2) {
            csR = peval::eval(hp.csDiaExpr, snap.params) * 0.5;
            csHalf = peval::eval(hp.csAngleExpr, snap.params) * 0.5;
            if (csHalf <= 1 || csHalf >= 89) throw std::runtime_error("Countersink angle must be between 2 and 178 degrees");
        }
        double spotR = 0, spotD = 0, drill = 0, pitch = 0;
        if (!hp.spotDiaExpr.empty() && !hp.spotDepthExpr.empty()) {
            spotR = peval::eval(hp.spotDiaExpr, snap.params) * 0.5;
            spotD = peval::eval(hp.spotDepthExpr, snap.params);
            if (spotD < 0 || spotR < 0) throw std::runtime_error("Spotface size must be zero or more");
            if (!(spotR > 1e-6) || !(spotD > 1e-6)) { spotR = 0; spotD = 0; }
        }
        if (!through && !hp.drillExpr.empty()) {
            drill = peval::eval(hp.drillExpr, snap.params);
            if (drill <= 1 || drill >= 179) throw std::runtime_error("Drill point angle must be between 2 and 178 degrees");
        }
        if (hp.tapped) {
            if (hp.pitchExpr.empty()) throw std::runtime_error("A tapped hole needs a pitch");
            pitch = peval::eval(hp.pitchExpr, snap.params);
            if (!(pitch > 1e-6)) throw std::runtime_error("Pitch must be greater than zero");
        }
        auto circleLoop = [](const glm::dvec3& c, const glm::dvec3& dir, double r) {
            RegionLoops loop;
            CurveSeg cs;
            cs.kind = CurveSeg::Kind::Circle;
            cs.center = c;
            cs.normal = dir;
            cs.radius = r;
            cs.a = c + glm::normalize(glm::cross(dir, std::abs(dir.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0))) * r;
            loop.push_back({cs});
            return loop;
        };
        auto cylinder = [&](const glm::dvec3& c, double r, const glm::dvec3& dir, double len, bool sym) {
            return kernel::extrudeRegion(circleLoop(c, dir, r), dir, len, sym, 0);
        };
        auto fuseAll = [&](std::vector<ShapePtr> v) {
            ShapePtr t = v[0];
            if (v.size() > 1) {
                std::vector<kernel::BoolTarget> ts;
                for (size_t i = 1; i < v.size(); ++i) ts.push_back({v[i], glm::mat4(1.f)});
                t = kernel::applyBool(kernel::BoolOp::Join, t, ts)[0];
            }
            return t;
        };
        // Which bodies: picked ones, else every body the (through) holes touch.
        std::vector<ShapePtr> probes;
        for (auto& h : holes) probes.push_back(cylinder(h.c, h.r, h.dir, 100000, true));
        ShapePtr probe = fuseAll(probes);
        std::vector<bool> hiddenT;
        std::vector<TargetRef> tids = hp.targets.empty() ? autoTargets(snap, done, f, {probe}, &hiddenT) : hp.targets;
        if (!hp.targets.empty() && f.rules >= 1) for (auto& t : tids) hiddenT.push_back(!targetShown(snap, done, t));
        if (tids.empty()) throw std::runtime_error("The holes do not touch any body");
        std::vector<ResolvedTarget> rts(tids.size());
        for (size_t i = 0; i < tids.size(); ++i) resolve(tids[i], rts[i]);
        std::vector<ShapePtr> cutters;
        std::vector<glm::dvec3> drillDir;
        for (auto& h : holes) {
            glm::dvec3 d = h.dir;
            if (h.fromSketch) {
                // Drill into the material: the side of the sketch where the body is.
                double eps = std::max(1e-3, h.r * 0.01);
                bool below = false;
                for (auto& rt : rts)
                    if (kernel::pointInside(rt.shape, rt.xform, h.c - d * eps)) below = true;
                d = below ? -d : d;
                if (through) { cutters.push_back(cylinder(h.c, h.r, h.dir, 100000, true)); }
            }
            drillDir.push_back(d);
            // Starts a little outside the surface so the cut is clean.
            double pad = std::max(0.05, h.r * 0.05);
            glm::dvec3 start = h.c - d * pad;
            if (!h.fromSketch || !through) cutters.push_back(cylinder(start, h.r, d, through ? 100000 : depth + pad, false));
            if (hp.kind == 1 && cbR > h.r) cutters.push_back(cylinder(start, cbR, d, cbDepth + pad, false));
            if (hp.kind == 2 && csR > h.r) {
                // Cone from the hole wall out to the countersink edge at the surface (plus a lip above it).
                double coneLen = (csR - h.r) / std::tan(csHalf * 3.14159265358979 / 180.0);
                glm::dvec3 tip = h.c + d * coneLen;
                cutters.push_back(kernel::extrudeRegion(circleLoop(tip, -d, h.r), -d, coneLen, false, csHalf));
                cutters.push_back(cylinder(h.c, csR, -d, pad, false));
            }
            if (spotR > h.r) cutters.push_back(cylinder(start, spotR, d, spotD + pad, false));
            if (!through && drill > 1 && h.r > 0.05) {
                double half = drill * 0.5;
                double coneLen = (h.r - 0.02) / std::tan(half * 3.14159265358979 / 180.0);
                glm::dvec3 tip = h.c + d * (depth + coneLen);
                cutters.push_back(kernel::extrudeRegion(circleLoop(tip, -d, 0.02), -d, coneLen + 0.05, false, half));
            }
        }
        ShapePtr tool = fuseAll(cutters);
        for (size_t i = 0; i < tids.size(); ++i) {
            if (i < hiddenT.size() && hiddenT[i]) {
                // A hidden part the hole line passes through: left as it was.
                item.shapes.push_back(rts[i].shape);
                item.meshes.push_back(rts[i].mesh ? rts[i].mesh : kernel::tessellate(*rts[i].shape));
                item.names.push_back(rts[i].name.empty() ? f.name : rts[i].name);
                item.xforms.push_back(rts[i].xform);
                item.consumed.push_back(tids[i]);
                continue;
            }
            auto outs = kernel::applyBool(kernel::BoolOp::Cut, tool, {{rts[i].shape, rts[i].xform}});
            if (hp.tapped) {
                const threads::Family& fam = threads::families()[0];
                for (auto& o : outs) {
                    for (size_t hi = 0; hi < holes.size(); ++hi) {
                        kernel::ThreadCut tc;
                        tc.origin = holes[hi].c;
                        tc.dir = drillDir[hi];
                        // A through thread only needs to reach the far side of this body: a long helix
                        // cut into a big body takes minutes.
                        double reach = depth;
                        if (through) {
                            reach = std::min(kernel::shapeDiag(rts[i].shape), pitch * 180.0);
                            // The material around the hole line: a slim cylinder kept where it is inside the body.
                            Aabb bb;
                            try {
                                ShapePtr probeCyl = cylinder(holes[hi].c - tc.dir * pitch, holes[hi].r * 1.6, tc.dir, reach + pitch, false);
                                auto near = kernel::applyBool(kernel::BoolOp::Intersect, probeCyl, {{o, glm::mat4(1.f)}});
                                for (auto& n : near) if (n) bb.add(kernel::shapeBounds(n, glm::mat4(1.f)));
                            } catch (...) {}
                            if (!bb.valid()) bb = kernel::shapeBounds(o, glm::mat4(1.f));
                            if (bb.valid()) {
                                double far = 0;
                                for (int c = 0; c < 8; ++c) {
                                    glm::dvec3 p((c & 1) ? bb.max.x : bb.min.x, (c & 2) ? bb.max.y : bb.min.y,
                                                 (c & 4) ? bb.max.z : bb.min.z);
                                    far = std::max(far, glm::dot(p - holes[hi].c, glm::normalize(tc.dir)));
                                }
                                reach = std::min(reach, far + pitch);
                            }
                        }
                        tc.length = std::max(reach, pitch);
                        tc.pitch = pitch;
                        tc.starts = 1;
                        double major = 2.0 * (holes[hi].r + fam.hInt * pitch);
                        tc.groove = threads::groove(fam, major, pitch, true, holes[hi].r);
                        std::string tw;
                        o = kernel::cutThread(o, glm::mat4(1.f), tc, &tw);
                        if (!tw.empty()) item.warn = f.name + ": " + tw;
                    }
                }
            }
            for (auto& o : outs) {
                item.shapes.push_back(o);
                item.meshes.push_back(kernel::tessellate(*o));
                item.names.push_back(rts[i].name.empty() ? f.name : rts[i].name);
                item.xforms.push_back(glm::mat4(1.f));
            }
            item.consumed.push_back(tids[i]);
        }
        return;
    }

    // ---- rib / web: an open sketch becomes a wall that grows until it meets a body -------
    if (f.type == FeatureType::Rib || f.type == FeatureType::Web) {
        bool web = f.type == FeatureType::Web;
        const RibParams& rp = web ? *f.web : *f.rib;
        auto sit = snap.sketches.find(rp.sketchId);
        if (sit == snap.sketches.end()) throw std::runtime_error("Pick an open sketch");
        std::vector<CurveSeg> chain = chainedPath(sit->second);
        if (chain.empty()) throw std::runtime_error("The sketch has no curve");
        double thick = peval::eval(rp.thickExpr, snap.params);
        if (!(thick > 1e-6)) throw std::runtime_error("Thickness must be greater than zero");
        glm::dvec3 n = glm::normalize(sit->second.plane.normal());
        glm::dvec3 dir = rp.flip ? -n : n;
        std::vector<TargetRef> tids = rp.targets;
        if (tids.empty()) {
            ShapePtr probe = web ? kernel::webExtrusion(chain, n, thick, rp.side, rp.flip, 10000)
                                 : kernel::ribExtrusion(chain, n, thick, rp.side, dir, 10000);
            tids = autoTargets(snap, done, f, {probe});
        }
        if (tids.empty()) throw std::runtime_error(web ? "The web does not meet a body" : "The rib does not meet a body");
        ResolvedTarget rt;
        resolve(tids[0], rt);
        ShapePtr out = web ? kernel::webToNext(chain, sit->second.plane.o, n, thick, rp.side, rp.flip, rt.shape, rt.xform)
                           : kernel::ribToNext(chain, sit->second.plane.o, n, thick, rp.side, dir, rt.shape, rt.xform);
        item.shapes.push_back(out);
        item.meshes.push_back(kernel::tessellate(*out));
        item.names.push_back(rt.name.empty() ? f.name : rt.name);
        item.xforms.push_back(glm::mat4(1.f));
        item.consumed.push_back(tids[0]);
        return;
    }

    // ---- emboss: closed profile projected onto a body, then raised or cut ----------------
    if (f.type == FeatureType::Emboss) {
        const EmbossParams& ep = *f.emboss;
        auto sit = snap.sketches.find(ep.sketchId);
        if (sit == snap.sketches.end()) throw std::runtime_error("Pick a closed sketch");
        auto regions = sketchRegions(sit->second);
        if (regions.empty()) throw std::runtime_error("Emboss needs a closed profile");
        double depth = peval::eval(ep.depthExpr, snap.params);
        if (!(depth > 1e-6)) throw std::runtime_error("Depth must be greater than zero");
        glm::dvec3 n = glm::normalize(sit->second.plane.normal());
        glm::dvec3 dir = ep.flip ? -n : n;
        std::vector<TargetRef> tids = ep.targets;
        if (tids.empty()) {
            ShapePtr probe = kernel::extrudeRegion(regions[0], dir, 10000, false, 0);
            tids = autoTargets(snap, done, f, {probe});
            if (tids.empty()) {   // the body is behind the sketch: look the other way
                probe = kernel::extrudeRegion(regions[0], -dir, 10000, false, 0);
                tids = autoTargets(snap, done, f, {probe});
            }
        }
        if (tids.empty()) throw std::runtime_error("The emboss does not meet a body");
        ResolvedTarget rt;
        resolve(tids[0], rt);
        ShapePtr out = kernel::embossOnBody(regions, sit->second.plane.o, dir, depth, ep.deboss, rt.shape, rt.xform);
        item.shapes.push_back(out);
        item.meshes.push_back(kernel::tessellate(*out));
        item.names.push_back(rt.name.empty() ? f.name : rt.name);
        item.xforms.push_back(glm::mat4(1.f));
        item.consumed.push_back(tids[0]);
        return;
    }

    // ---- boss: a round post on a face, optional hole ------------------------------------
    if (f.type == FeatureType::Boss) {
        const BossParams& bp = *f.boss;
        auto ev = [&](const std::string& e) { return peval::eval(e, snap.params); };
        glm::dvec3 origin(ev(bp.xExpr), ev(bp.yExpr), ev(bp.zExpr));
        double dia = ev(bp.diaExpr), height = ev(bp.heightExpr);
        double hole = bp.holeExpr.empty() ? 0 : ev(bp.holeExpr);
        glm::dvec3 axis = bp.axis == 0 ? glm::dvec3(1, 0, 0) : bp.axis == 1 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
        if (bp.targets.empty()) return;
        ResolvedTarget rt;
        resolve(bp.targets[0], rt);
        ShapePtr out = kernel::bossOnBody(origin, axis, dia, height, hole, rt.shape, rt.xform);
        item.shapes.push_back(out);
        item.meshes.push_back(kernel::tessellate(*out));
        item.names.push_back(rt.name.empty() ? f.name : rt.name);
        item.xforms.push_back(glm::mat4(1.f));
        item.consumed.push_back(bp.targets[0]);
        return;
    }

    // ---- snap: a cantilever hook on a face ----------------------------------------------
    if (f.type == FeatureType::Snap) {
        const SnapParams& sp = *f.snap;
        auto ev = [&](const std::string& e) { return peval::eval(e, snap.params); };
        glm::dvec3 origin(ev(sp.xExpr), ev(sp.yExpr), ev(sp.zExpr));
        double length = ev(sp.lengthExpr), width = ev(sp.widthExpr), thick = ev(sp.thickExpr);
        double hook = sp.hookExpr.empty() ? 0 : ev(sp.hookExpr);
        double hookLen = sp.hookLenExpr.empty() ? 0 : ev(sp.hookLenExpr);
        glm::dvec3 axis = sp.axis == 0 ? glm::dvec3(1, 0, 0) : sp.axis == 1 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
        if (sp.targets.empty()) return;
        ResolvedTarget rt;
        resolve(sp.targets[0], rt);
        ShapePtr out = kernel::snapOnBody(origin, axis, length, width, thick, hook, hookLen, rt.shape, rt.xform);
        item.shapes.push_back(out);
        item.meshes.push_back(kernel::tessellate(*out));
        item.names.push_back(rt.name.empty() ? f.name : rt.name);
        item.xforms.push_back(glm::mat4(1.f));
        item.consumed.push_back(sp.targets[0]);
        return;
    }

    // ---- rest: a flat pad whose sketch is one face --------------------------------------
    if (f.type == FeatureType::Rest) {
        const RestParams& rp = *f.rest;
        auto sit = snap.sketches.find(rp.sketchId);
        if (sit == snap.sketches.end()) throw std::runtime_error("Pick a closed sketch");
        auto regions = sketchRegions(sit->second);
        if (regions.empty()) throw std::runtime_error("Rest needs a closed profile");
        double thick = peval::eval(rp.thickExpr, snap.params);
        if (!(thick > 1e-6)) throw std::runtime_error("Thickness must be greater than zero");
        glm::dvec3 n = glm::normalize(sit->second.plane.normal());
        glm::dvec3 dir = rp.flip ? -n : n;
        if (rp.targets.empty()) return;
        ResolvedTarget rt;
        resolve(rp.targets[0], rt);
        ShapePtr out = kernel::restOnBody(regions, dir, thick, rt.shape, rt.xform);
        item.shapes.push_back(out);
        item.meshes.push_back(kernel::tessellate(*out));
        item.names.push_back(rt.name.empty() ? f.name : rt.name);
        item.xforms.push_back(glm::mat4(1.f));
        item.consumed.push_back(rp.targets[0]);
        return;
    }

    // ---- lip: a ridge around a closed profile -------------------------------------------
    if (f.type == FeatureType::Lip) {
        const LipParams& lp = *f.lip;
        if (!lp.sketchId) return;
        auto sit = snap.sketches.find(lp.sketchId);
        if (sit == snap.sketches.end()) throw std::runtime_error("Pick a closed sketch");
        auto regions = sketchRegions(sit->second);
        if (regions.empty()) throw std::runtime_error("Lip needs a closed profile");
        double width = peval::eval(lp.widthExpr, snap.params);
        double height = peval::eval(lp.heightExpr, snap.params);
        if (!(width > 1e-6)) throw std::runtime_error("Width must be greater than zero");
        if (!(height > 1e-6)) throw std::runtime_error("Height must be greater than zero");
        glm::dvec3 n = glm::normalize(sit->second.plane.normal());
        glm::dvec3 dir = lp.flip ? -n : n;
        if (lp.targets.empty()) return;
        ResolvedTarget rt;
        resolve(lp.targets[0], rt);
        ShapePtr out = kernel::lipOnBody(regions, dir, width, height, lp.groove, rt.shape, rt.xform);
        item.shapes.push_back(out);
        item.meshes.push_back(kernel::tessellate(*out));
        item.names.push_back(rt.name.empty() ? f.name : rt.name);
        item.xforms.push_back(glm::mat4(1.f));
        item.consumed.push_back(lp.targets[0]);
        return;
    }

    // ---- silhouette split: cut on the outline seen along an axis ------------------------
    if (f.type == FeatureType::Silhouette) {
        const SilhouetteParams& sp = *f.silhouette;
        if (sp.targets.empty()) return;
        glm::dvec3 axis = sp.axis == 0 ? glm::dvec3(1, 0, 0) : sp.axis == 1 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
        ResolvedTarget rt;
        resolve(sp.targets[0], rt);
        auto pieces = kernel::silhouetteSplit(rt.shape, axis, rt.xform);
        int k = 0;
        for (auto& pc : pieces) {
            item.shapes.push_back(pc);
            item.meshes.push_back(kernel::tessellate(*pc));
            std::string base = rt.name.empty() ? f.name : rt.name;
            item.names.push_back(k++ == 0 ? base : freeBodyName(snap, f.id, item.names, base + " " + std::to_string(k)));
            item.xforms.push_back(glm::mat4(1.f));
        }
        item.consumed.push_back(sp.targets[0]);
        return;
    }

    // ---- joint: move one body so a point on it lands on a point on another --------------
    if (f.type == FeatureType::Joint) {
        const JointParams& jp = *f.joint;
        if (!jp.ground.first || !jp.move.first) return;
        if (jp.ground == jp.move) throw std::runtime_error("Pick two different bodies");
        if (refGrounded(snap, jp.move)) throw std::runtime_error("That component is grounded");
        ResolvedTarget stay, mover;
        if (!resolveTarget(snap, done, jp.ground, stay)) throw std::runtime_error("The body that stays is gone");
        if (!resolveTarget(snap, done, jp.move, mover)) throw std::runtime_error("The body that moves is gone");
        auto centerOf = [&](const ResolvedTarget& rt) {
            if (rt.mesh && rt.mesh->bounds.valid()) return glm::dvec3(rt.mesh->bounds.transformed(rt.xform).center());
            if (rt.shape) {
                Aabb bb = kernel::shapeBounds(rt.shape, rt.xform);
                if (bb.valid()) return glm::dvec3(bb.center());
            }
            throw std::runtime_error("That body has no size");
        };
        auto ev = [&](const std::string& e) { return e.empty() ? 0.0 : peval::eval(e, snap.params); };
        auto pointOf = [&](const ResolvedTarget& rt, const std::string& x, const std::string& y, const std::string& z) {
            if (x.empty() && y.empty() && z.empty()) return centerOf(rt);
            return glm::dvec3(ev(x), ev(y), ev(z));
        };
        glm::dvec3 stayPt = pointOf(stay, jp.gx, jp.gy, jp.gz);
        glm::dvec3 delta = stayPt - pointOf(mover, jp.mx, jp.my, jp.mz);
        glm::mat4 placed = glm::translate(glm::mat4(1.f), glm::vec3(delta));
        if (jp.kind == 1) {
            double ang = jp.angleExpr.empty() ? 0.0 : peval::eval(jp.angleExpr, snap.params);
            glm::vec3 ax = jp.axis == 0 ? glm::vec3(1, 0, 0) : jp.axis == 1 ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
            glm::vec3 pivot((float)stayPt.x, (float)stayPt.y, (float)stayPt.z);
            glm::mat4 spin = glm::translate(glm::mat4(1.f), pivot)
                           * glm::rotate(glm::mat4(1.f), glm::radians((float)ang), ax)
                           * glm::translate(glm::mat4(1.f), -pivot);
            placed = spin * placed;
        } else if (jp.kind == 2) {
            double dist = jp.slideExpr.empty() ? 0.0 : peval::eval(jp.slideExpr, snap.params);
            glm::vec3 ax = jp.axis == 0 ? glm::vec3(1, 0, 0) : jp.axis == 1 ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
            placed = glm::translate(glm::mat4(1.f), ax * (float)dist) * placed;
        }
        glm::mat4 M = placed * mover.xform;
        item.shapes.push_back(nullptr);
        item.meshes.push_back(nullptr);
        item.names.push_back(mover.name.empty() ? f.name : mover.name);
        item.xforms.push_back(M);
        item.instanceRefs.push_back(jp.move);
        item.consumed.push_back(jp.move);
        bool grouped = false;
        size_t nMem = 0;
        for (auto& ft : snap.timeline)
            if (ft.id == jp.move.first && ft.type == FeatureType::Rigid && ft.rigid) {
                grouped = true;
                nMem = ft.rigid->members.size();
            }
        if (grouped && jp.ground.first == jp.move.first)
            throw std::runtime_error("Those bodies are already a rigid group");
        if (grouped) {
            for (uint32_t s = 0; s < nMem; ++s) {
                TargetRef sib{jp.move.first, s};
                if (sib == jp.move) continue;
                ResolvedTarget other;
                if (!resolveTarget(snap, done, sib, other)) throw std::runtime_error("A grouped body is gone");
                item.shapes.push_back(nullptr);
                item.meshes.push_back(nullptr);
                item.names.push_back(other.name.empty() ? f.name : other.name);
                item.xforms.push_back(placed * other.xform);
                item.instanceRefs.push_back(sib);
                item.consumed.push_back(sib);
            }
        }
        return;
    }

    // ---- as-built joint: both bodies stay. The lock is recorded at the current pose. ----
    if (f.type == FeatureType::AsBuilt) {
        const JointParams& jp = *f.asbuilt;
        if (!jp.ground.first || !jp.move.first) return;
        if (jp.ground == jp.move) throw std::runtime_error("Pick two different bodies");
        ResolvedTarget stay, mover;
        if (!resolveTarget(snap, done, jp.ground, stay)) throw std::runtime_error("The body that stays is gone");
        if (!resolveTarget(snap, done, jp.move, mover)) throw std::runtime_error("The body that moves is gone");
        if (!stay.mesh && !stay.shape) throw std::runtime_error("The body that stays is gone");
        item.shapes.push_back(nullptr);
        item.meshes.push_back(nullptr);
        item.names.push_back(mover.name.empty() ? f.name : mover.name);
        item.xforms.push_back(mover.xform);
        item.instanceRefs.push_back(jp.move);
        item.consumed.push_back(jp.move);
        return;
    }

    // ---- rigid group: bodies stay. A later Move or Joint of one carries the rest. ------
    if (f.type == FeatureType::Rigid) {
        const RigidParams& rp = *f.rigid;
        if (rp.members.size() < 2) return;
        for (const TargetRef& ref : rp.members) {
            ResolvedTarget rt;
            if (!resolveTarget(snap, done, ref, rt)) throw std::runtime_error("A grouped body is gone");
            item.shapes.push_back(nullptr);
            item.meshes.push_back(nullptr);
            item.names.push_back(rt.name.empty() ? f.name : rt.name);
            item.xforms.push_back(rt.xform);
            item.instanceRefs.push_back(ref);
            item.consumed.push_back(ref);
        }
        return;
    }

    // ---- sweep / loft -------------------------------------------------------------------
    if (f.type == FeatureType::Sweep || f.type == FeatureType::Loft) {
        const MultiSketchParams& mp = *f.multi;
        auto finishTool = [&](const ShapePtr& tool) {
            if (mp.op == 0) {
                item.shapes.push_back(tool);
                item.meshes.push_back(kernel::tessellate(*tool));
                item.names.push_back(f.name);
                return;
            }
            std::vector<kernel::BoolTarget> targets;
            std::string firstName;
            std::vector<TargetRef> tids = mp.targets.empty() ? autoTargets(snap, done, f, {tool}) : mp.targets;
            if (tids.empty()) {
                if (mp.op != 1) throw std::runtime_error("The result does not touch any body");
                item.shapes.push_back(tool);
                item.meshes.push_back(kernel::tessellate(*tool));
                item.names.push_back(f.name);
                return;
            }
            for (size_t i = 0; i < tids.size(); ++i) {
                ResolvedTarget rt;
                resolve(tids[i], rt);
                if (i == 0) firstName = rt.name;
                targets.push_back({rt.shape, rt.xform});
                item.consumed.push_back(tids[i]);
            }
            auto outs = kernel::applyBool(boolOpOf(mp.op), tool, targets);
            for (size_t i = 0; i < outs.size(); ++i) {
                item.shapes.push_back(outs[i]);
                item.meshes.push_back(kernel::tessellate(*outs[i]));
                item.names.push_back(i == 0 && !firstName.empty() ? firstName : f.name);
            }
        };
        if (f.type == FeatureType::Sweep) {
            if (mp.sketchIds.size() < 2) throw std::runtime_error("Pick a profile sketch and a path sketch");
            auto pit = snap.sketches.find(mp.sketchIds[0]);
            auto pathIt = snap.sketches.find(mp.sketchIds[1]);
            if (pit == snap.sketches.end() || pathIt == snap.sketches.end())
                throw std::runtime_error("A sketch this feature uses is missing");
            auto regions = sketchRegions(pit->second);
            std::vector<CurveSeg> spine = chainedPath(pathIt->second);
            if (spine.empty()) throw std::runtime_error("The path sketch has no geometry");
            double twist = mp.twistExpr.empty() ? 0 : peval::eval(mp.twistExpr, snap.params);
            std::vector<CurveSeg> guide;
            if (!mp.railIds.empty()) {
                auto git = snap.sketches.find(mp.railIds[0]);
                if (git == snap.sketches.end()) throw std::runtime_error("The guide sketch is missing");
                guide = chainedPath(git->second);
                if (guide.empty()) throw std::runtime_error("The guide sketch has no curve");
            }
            if (!mp.thinExpr.empty()) {
                double thick = peval::eval(mp.thinExpr, snap.params);
                int side = mp.thinSide < 0 ? 0 : mp.thinSide > 2 ? 2 : mp.thinSide;
                std::vector<CurveSeg> chain = chainedPath(pit->second);
                if (chain.empty()) throw std::runtime_error("Thin needs an open curve");
                finishTool(kernel::sweepRegion(kernel::thinProfile(chain, pit->second.plane.normal(), thick, side), spine, twist, guide));
            } else {
                if (regions.empty()) throw std::runtime_error("The profile sketch has no closed profile");
                finishTool(kernel::sweepRegion(regions[0], spine, twist, guide));
            }
        } else {
            bool pointed = mp.pointEnd == 1 || mp.pointEnd == 2;
            if (pointed && mp.sketchIds.empty()) throw std::runtime_error("Loft to a point needs a profile");
            if (!pointed && mp.sketchIds.size() < 2) throw std::runtime_error("Loft needs at least two profile sketches");
            if (pointed && !mp.railIds.empty()) throw std::runtime_error("A point end does not take rails");
            if (pointed && mp.centerId) throw std::runtime_error("A point end does not take a centerline");
            if (mp.centerId && !mp.railIds.empty()) throw std::runtime_error("A centerline does not take rails");
            std::vector<RegionLoops> sections;
            for (uint32_t sid : mp.sketchIds) {
                auto pit = snap.sketches.find(sid);
                if (pit == snap.sketches.end()) throw std::runtime_error("A sketch this feature uses is missing");
                auto regions = sketchRegions(pit->second);
                if (regions.empty()) throw std::runtime_error("Sketch '" + pit->second.name + "' has no closed profile");
                sections.push_back(regions[0]);
            }
            std::vector<std::vector<CurveSeg>> rails;
            for (uint32_t sid : mp.railIds) {
                if (std::find(mp.sketchIds.begin(), mp.sketchIds.end(), sid) != mp.sketchIds.end()) continue;
                auto rit = snap.sketches.find(sid);
                if (rit == snap.sketches.end()) throw std::runtime_error("A rail sketch is missing");
                std::vector<CurveSeg> path = chainedPath(rit->second);
                if (path.empty()) throw std::runtime_error("A rail sketch has no curve");
                rails.push_back(std::move(path));
            }
            glm::dvec3 tip(0);
            if (pointed) {
                if (mp.pointX.empty() || mp.pointY.empty() || mp.pointZ.empty())
                    throw std::runtime_error("Type the point");
                tip = {peval::eval(mp.pointX, snap.params), peval::eval(mp.pointY, snap.params), peval::eval(mp.pointZ, snap.params)};
            }
            std::vector<CurveSeg> center;
            if (mp.centerId) {
                auto cit = snap.sketches.find(mp.centerId);
                if (cit == snap.sketches.end()) throw std::runtime_error("The centerline sketch is missing");
                center = chainedPath(cit->second);
                if (center.empty()) throw std::runtime_error("The centerline has no curve");
            }
            finishTool(kernel::loftRegions(sections, rails, pointed, tip, mp.pointEnd == 1, center));
        }
        return;
    }

    // ---- move / copy: pure transforms, instanced like patterns -------------------------
    if (f.type == FeatureType::Move) {
        const MoveParams& mv = *f.move;
        auto ev = [&](const std::string& e, double def) {
            return e.empty() ? def : peval::eval(e, snap.params);
        };
        double dx = ev(mv.xExpr, 0), dy = ev(mv.yExpr, 0), dz = ev(mv.zExpr, 0);
        double rx = ev(mv.rxExpr, 0), ry = ev(mv.ryExpr, 0), rz = ev(mv.rzExpr, 0);
        double ang = ev(mv.angExpr, 0);
        glm::dvec3 axis(ev(mv.axExpr, 0), ev(mv.ayExpr, 0), ev(mv.azExpr, 0));
        double sx = ev(mv.sxExpr, 1), sy = ev(mv.syExpr, 1), sz = ev(mv.szExpr, 1);
        bool pivotGiven = !mv.pxExpr.empty() || !mv.pyExpr.empty() || !mv.pzExpr.empty();
        glm::dvec3 given(ev(mv.pxExpr, 0), ev(mv.pyExpr, 0), ev(mv.pzExpr, 0));
        glm::mat4 R(1.f);
        if (glm::length(axis) > 1e-12 && std::abs(ang) > 1e-12)
            R = glm::rotate(glm::mat4(1.f), (float)glm::radians(ang), glm::vec3(glm::normalize(axis)));
        else if (std::abs(rx) + std::abs(ry) + std::abs(rz) > 1e-12)
            R = glm::rotate(glm::mat4(1.f), (float)glm::radians(rz), glm::vec3(0, 0, 1)) *
                glm::rotate(glm::mat4(1.f), (float)glm::radians(ry), glm::vec3(0, 1, 0)) *
                glm::rotate(glm::mat4(1.f), (float)glm::radians(rx), glm::vec3(1, 0, 0));
        glm::mat4 S = glm::scale(glm::mat4(1.f), glm::vec3((float)sx, (float)sy, (float)sz));
        glm::mat4 T = glm::translate(glm::mat4(1.f), glm::vec3((float)dx, (float)dy, (float)dz));
        if (!mv.copy) {
            for (const TargetRef& ref : mv.targets)
                if (refGrounded(snap, ref)) throw std::runtime_error("That component is grounded");
        }
        glm::mat4 placed(1.f);
        if (mv.hasMat) {
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) placed[c][r] = (float)mv.mat[r * 4 + c];
        }
        std::vector<TargetRef> refs = mv.targets;
        std::vector<TargetRef> extra;
        if (!mv.hasMat && mv.op == 0) {
            for (const TargetRef& ref : mv.targets) {
                const Feature* rg = nullptr;
                for (auto& ft : snap.timeline)
                    if (ft.id == ref.first && ft.type == FeatureType::Rigid && ft.rigid) { rg = &ft; break; }
                if (!rg) continue;
                for (uint32_t s = 0; s < rg->rigid->members.size(); ++s) {
                    TargetRef sib{rg->id, s};
                    bool seen = false;
                    for (auto& e : refs) if (e == sib) seen = true;
                    for (auto& e : extra) if (e == sib) seen = true;
                    if (!seen) extra.push_back(sib);
                }
            }
        }
        glm::mat4 sharedW(1.f);
        bool useShared = false;
        if (mv.together && !mv.hasMat && !pivotGiven) {
            // One turn for all of them, about the center of their combined bounds.
            Aabb all;
            for (const TargetRef& ref : refs) {
                ResolvedTarget rt;
                if (!resolveTarget(snap, done, ref, rt)) continue;
                if (rt.mesh && rt.mesh->bounds.valid()) all.add(rt.mesh->bounds.transformed(rt.xform));
                else if (rt.shape) all.add(kernel::shapeBounds(rt.shape, rt.xform));
            }
            glm::vec3 pv = all.valid() ? all.center() : glm::vec3(0.f);
            sharedW = T * glm::translate(glm::mat4(1.f), pv) * R * S * glm::translate(glm::mat4(1.f), -pv);
            useShared = true;
            refs.insert(refs.end(), extra.begin(), extra.end());
        } else if (!extra.empty()) {
            ResolvedTarget first;
            if (!resolveTarget(snap, done, refs[0], first)) throw std::runtime_error("A grouped body is gone");
            glm::dvec3 pivot = given;
            if (!pivotGiven) {
                if (first.mesh && first.mesh->bounds.valid()) pivot = first.mesh->bounds.transformed(first.xform).center();
                else if (first.shape) {
                    Aabb bb = kernel::shapeBounds(first.shape, first.xform);
                    if (bb.valid()) pivot = bb.center();
                }
            }
            glm::vec3 pv(pivot);
            sharedW = T * glm::translate(glm::mat4(1.f), pv) * R * S * glm::translate(glm::mat4(1.f), -pv);
            useShared = true;
            refs.insert(refs.end(), extra.begin(), extra.end());
        }
        for (const TargetRef& ref : refs) {
            ResolvedTarget rt;
            resolveTarget(snap, done, ref, rt);   // mesh bodies may move too: no solid check
            glm::mat4 M;
            if (mv.hasMat) {
                M = placed;
            } else if (useShared) {
                M = sharedW * rt.xform;
            } else {
                glm::dvec3 pivot = given;
                if (!pivotGiven) {
                    if (rt.mesh && rt.mesh->bounds.valid()) pivot = rt.mesh->bounds.transformed(rt.xform).center();
                    else if (rt.shape) {
                        Aabb bb = kernel::shapeBounds(rt.shape, rt.xform);
                        if (bb.valid()) pivot = bb.center();
                    }
                }
                glm::vec3 pv(pivot);
                M = T * glm::translate(glm::mat4(1.f), pv) * R * S * glm::translate(glm::mat4(1.f), -pv);
                M = M * rt.xform;
            }
            std::string nm = rt.name.empty() ? f.name : rt.name;
            // A solid's placement must stay rigid (OCCT refuses a scaled location, and every later cut,
            // fillet or boolean reads it), so a scale is baked into the geometry itself.
            glm::vec3 c0(M[0]), c1(M[1]), c2(M[2]);
            bool scaled = std::abs(glm::length(c0) - 1.f) > 1e-5f || std::abs(glm::length(c1) - 1.f) > 1e-5f ||
                          std::abs(glm::length(c2) - 1.f) > 1e-5f;
            if (scaled && rt.shape) {
                ShapePtr out = kernel::placeShape(rt.shape, M);
                item.shapes.push_back(out);
                item.meshes.push_back(kernel::tessellate(*out));
                item.xforms.push_back(glm::mat4(1.f));
            } else {
                item.shapes.push_back(nullptr);
                item.meshes.push_back(nullptr);
                item.xforms.push_back(M);
            }
            if (mv.copy) {
                // Fusion names a pasted copy "Body1 (1)"; the next copy of Body1 is "Body1 (2)", so
                // every copy can be told apart by its name.
                auto taken = [&](const std::string& c) {
                    for (auto& [bid, b] : snap.bodies)
                        if (b.featureId != f.id && b.name == c) return true;
                    return std::find(item.names.begin(), item.names.end(), c) != item.names.end();
                };
                int k = 1;
                while (k < 1000 && taken(nm + " (" + std::to_string(k) + ")")) ++k;
                item.names.push_back(nm + " (" + std::to_string(k) + ")");
            } else {
                item.names.push_back(nm);
            }
            item.instanceRefs.push_back(ref);
            if (!mv.copy) item.consumed.push_back(ref);
        }
        return;
    }

    // ---- mesh to solid -----------------------------------------------------------------
    if (f.type == FeatureType::MeshConvert) {
        const ModifyParams& m = *f.modify;
        for (const TargetRef& ref : m.targets) {
            ResolvedTarget rt;
            resolveTarget(snap, done, ref, rt);
            if (rt.shape) {
                item.warn = f.name + ": '" + rt.name + "' is already a solid";
                item.shapes.push_back(rt.shape);
                item.meshes.push_back(kernel::tessellate(*rt.shape));
                item.names.push_back(rt.name);
                item.xforms.push_back(rt.xform);
                continue;
            }
            if (!rt.mesh) throw std::runtime_error("That body has no mesh to convert");
            ShapePtr out = kernel::meshToSolid(*rt.mesh);
            item.shapes.push_back(out);
            item.meshes.push_back(kernel::tessellate(*out));
            item.names.push_back(rt.name.empty() ? f.name : rt.name);
            item.consumed.push_back(ref);
        }
        return;
    }

    uint32_t sketchId = f.type == FeatureType::Extrude ? f.extrude->sketchId : f.revolve->sketchId;
    int op = f.type == FeatureType::Extrude ? f.extrude->op : f.revolve->op;

    std::vector<ShapePtr> tools;
    if (f.type == FeatureType::Extrude) {
        const ExtrudeParams& x = *f.extrude;
        if (x.faces.empty() && sketchId == 0) return;   // dialog open, nothing picked yet
        double dist = 0;
        bool reverse = false;
        if (x.extent == 1) {
            if (!x.to.feature) return;   // waiting for the face
            if (!x.faces.empty()) throw std::runtime_error("To Face starts from a sketch profile");
            ResolvedTarget stop;
            resolve({x.to.feature, x.to.slot}, stop);
            if (x.to.faceTotal && kernel::faceCount(stop.shape) != x.to.faceTotal)
                throw std::runtime_error("The body changed shape, so the stop face no longer matches. Edit the extrude and pick it again.");
            glm::dvec3 c, n;
            bool planar = false;
            if (!kernel::faceInfo(stop.shape, x.to.face, c, n, planar) || !planar)
                throw std::runtime_error("The extrude can stop on a flat face");
            auto sit = snap.sketches.find(sketchId);
            if (sit == snap.sketches.end()) throw std::runtime_error("The sketch this feature uses is missing");
            glm::dvec3 dir = sit->second.plane.normal();
            if (glm::length(dir) < 1e-9) throw std::runtime_error("The sketch plane has no direction");
            dir = glm::normalize(dir);
            if (x.flip) dir = -dir;
            glm::dvec3 cw = glm::dvec3(stop.xform * glm::dvec4(c, 1.0));
            glm::dvec3 nw = glm::normalize(glm::dmat3(stop.xform) * n);
            double denom = glm::dot(nw, dir);
            if (std::abs(denom) < 0.2) throw std::runtime_error("That face does not face the extrude");
            dist = glm::dot(nw, cw - sit->second.plane.o) / denom;
            if (!(dist > 1e-4)) throw std::runtime_error("That face is behind the profile");
            reverse = x.flip;
        } else {
            dist = peval::eval(x.distExpr, snap.params);
            if (std::abs(dist) < 1e-9) return;   // waiting for a distance: no preview yet, not an error
            reverse = (dist < 0) != x.flip;
            dist = std::abs(dist);
        }
        double taper = x.taperExpr.empty() ? 0.0 : peval::eval(x.taperExpr, snap.params);
        if (std::abs(taper) >= 89.0) throw std::runtime_error("Taper angle must be under 89 degrees");
        if (!x.faces.empty()) {
            if (!x.thinExpr.empty()) throw std::runtime_error("Thin starts from a sketch curve");
            for (const FaceRef& fr : x.faces) {
                ResolvedTarget rt;
                resolve({fr.feature, fr.slot}, rt);
                if (fr.faceTotal != 0 && kernel::faceCount(rt.shape) != fr.faceTotal)
                    throw std::runtime_error("The body changed shape, so the picked face no longer matches. Edit the feature and pick it again.");
                tools.push_back(kernel::extrudeFace(rt.shape, rt.xform, fr.face, reverse ? -dist : dist, x.symmetric));
            }
        } else {
            auto sit = snap.sketches.find(sketchId);
            if (sit == snap.sketches.end()) throw std::runtime_error("The sketch this feature uses is missing");
            const sk::Sketch& sk = sit->second;
            glm::dvec3 dir = sk.plane.normal();
            if (reverse) dir = -dir;
            if (!x.thinExpr.empty()) {
                if (x.extent == 1) throw std::runtime_error("Thin uses a distance");
                double thick = peval::eval(x.thinExpr, snap.params);
                int side = x.thinSide < 0 ? 0 : x.thinSide > 2 ? 2 : x.thinSide;
                std::vector<CurveSeg> chain = chainedPath(sk);
                if (chain.empty()) throw std::runtime_error("Thin needs an open curve");
                tools.push_back(kernel::extrudeRegion(kernel::thinProfile(chain, sk.plane.normal(), thick, side), dir, dist, x.symmetric, taper));
            } else {
                std::vector<RegionLoops> regions = sketchRegionsAt(sk, x.profilePts);
                if (regions.empty())
                    throw std::runtime_error(x.profilePts.empty() ? "The sketch has no closed profile"
                                                                  : "The picked profiles no longer exist. Edit the feature and pick them again.");
                for (auto& rg : regions) tools.push_back(kernel::extrudeRegion(rg, dir, dist, x.symmetric, taper));
                // Profiles from other sketches: same distance, each along its own sketch's normal
                // (the flip follows the main sketch: a sketch facing the other way flips too).
                for (auto& [msid, mpts] : x.moreSketches) {
                    auto mit = snap.sketches.find(msid);
                    if (mit == snap.sketches.end()) throw std::runtime_error("A sketch this extrude uses is missing");
                    glm::dvec3 mdir = mit->second.plane.normal();
                    if (glm::dot(mdir, sk.plane.normal()) < 0) mdir = -mdir;
                    if (reverse) mdir = -mdir;
                    std::vector<RegionLoops> mr = sketchRegionsAt(mit->second, mpts);
                    if (mr.empty()) throw std::runtime_error("The picked profiles in " + mit->second.name + " no longer exist. Edit the feature and pick them again.");
                    for (auto& rg : mr) tools.push_back(kernel::extrudeRegion(rg, mdir, dist, x.symmetric, taper));
                }
            }
        }
    } else {
        auto sit = snap.sketches.find(sketchId);
        if (sit == snap.sketches.end()) throw std::runtime_error("The sketch this feature uses is missing");
        const sk::Sketch& sk = sit->second;
        const RevolveParams& r = *f.revolve;
        std::vector<RegionLoops> regions;
        if (r.thinExpr.empty()) {
            regions = sketchRegionsAt(sk, r.profilePts);
            if (regions.empty()) throw std::runtime_error("The sketch has no closed profile");
        }
        double ang = peval::eval(r.angleExpr, snap.params);
        if (ang <= 1e-6 || ang > 360.0 + 1e-6) throw std::runtime_error("Angle must be between 0 and 360 degrees");
        glm::dvec3 O, D;
        if (r.axis == 1) {
            O = sk.plane.o;
            D = sk.plane.x;
        } else if (r.axis >= 2 && r.axisEnt) {
            const sk::Entity* e = sk.find(r.axisEnt);
            if (!e || e->type != sk::EntType::Line) throw std::runtime_error("The revolve axis line no longer exists");
            O = sk.plane.toWorld(sk.pt(e->a));
            D = sk.plane.toWorld(sk.pt(e->b)) - O;
        } else {
            O = sk.plane.o;
            D = sk.plane.y;
        }
        if (r.flip) D = -D;
        if (!r.thinExpr.empty()) {
            double thick = peval::eval(r.thinExpr, snap.params);
            int side = r.thinSide < 0 ? 0 : r.thinSide > 2 ? 2 : r.thinSide;
            std::vector<CurveSeg> chain = chainedPath(sk);
            if (chain.empty()) throw std::runtime_error("Thin needs an open curve");
            tools.push_back(kernel::revolveRegion(kernel::thinProfile(chain, sk.plane.normal(), thick, side), O, D, ang));
        } else {
            for (auto& rg : regions) tools.push_back(kernel::revolveRegion(rg, O, D, ang));
        }
    }

    // New Body: one body per disjoint profile region, like Fusion. Boolean ops: one fused tool.
    std::vector<ShapePtr> outs;
    std::vector<std::string> names;
    std::vector<glm::mat4> outX;
    if (op == 0) {
        for (size_t i = 0; i < tools.size(); ++i) {
            outs.push_back(tools[i]);
            names.push_back(tools.size() > 1 ? f.name + " " + std::to_string(i + 1) : f.name);
        }
    } else {
        ShapePtr tool = tools[0];
        if (tools.size() > 1) {
            std::vector<kernel::BoolTarget> ts;
            for (size_t i = 1; i < tools.size(); ++i) ts.push_back({tools[i], glm::mat4(1.f)});
            tool = kernel::applyBool(kernel::BoolOp::Join, tool, ts)[0];
        }
        std::vector<TargetRef> tids = f.type == FeatureType::Extrude ? f.extrude->targets : f.revolve->targets;
        // Join from a body's own faces grows that body only: other parts it happens to touch stay
        // separate (they used to be merged in).
        if (tids.empty() && op == 1 && f.type == FeatureType::Extrude && !f.extrude->faces.empty())
            for (auto& fr : f.extrude->faces) {
                TargetRef r{fr.feature, fr.slot};
                if (std::find(tids.begin(), tids.end(), r) == tids.end()) tids.push_back(r);
            }
        std::vector<bool> hiddenT;
        if (tids.empty()) tids = autoTargets(snap, done, f, {tool}, &hiddenT);
        else if (f.rules >= 1) for (auto& t : tids) hiddenT.push_back(!targetShown(snap, done, t));   // chosen ones too (steps made from fix1064 on)
        if (op == 2) item.cutPreview = kernel::tessellate(*tool);
        std::vector<TargetRef> passThrough;   // hidden parts it touches: left as they were
        std::vector<TargetRef> allT = tids;
        if (!hiddenT.empty()) {
            std::vector<TargetRef> keep;
            for (size_t i = 0; i < tids.size(); ++i) (i < hiddenT.size() && hiddenT[i] ? passThrough : keep).push_back(tids[i]);
            tids = keep;
            if (op == 1 && tids.empty()) { passThrough.clear(); allT.clear(); }
        }
        // Puts the untouched hidden parts back among the outputs, in their old places, so the
        // step's outputs keep the numbers later steps know them by.
        auto addPassThrough = [&](std::vector<ShapePtr>& o, std::vector<std::string>& nm, std::vector<glm::mat4>& xf) {
            if (passThrough.empty()) return;
            std::vector<ShapePtr> o2;
            std::vector<std::string> n2;
            std::vector<glm::mat4> x2;
            size_t vi = 0;
            bool perTarget = op != 1 && o.size() == tids.size();
            for (auto& r : allT) {
                bool hidden = std::find(passThrough.begin(), passThrough.end(), r) != passThrough.end();
                if (hidden) {
                    ResolvedTarget rt;
                    resolve(r, rt);
                    o2.push_back(rt.shape);
                    n2.push_back(rt.name.empty() ? f.name : rt.name);
                    x2.push_back(rt.xform);
                } else if (perTarget && vi < o.size()) {
                    o2.push_back(o[vi]);
                    n2.push_back(vi < nm.size() ? nm[vi] : f.name);
                    x2.push_back(glm::mat4(1.f));
                    ++vi;
                }
            }
            // Each output's source (its look and visibility come from it): hidden pass throughs
            // name theirs in instanceRefs, the rest go by the consumed list.
            if (perTarget) item.consumed = allT;
            else for (auto& h : passThrough) item.consumed.push_back(h);
            if (!perTarget) {
                // A join (one result) or a cut that split something: results first, hidden parts after.
                std::vector<ShapePtr> o3 = o;
                std::vector<std::string> n3 = nm;
                std::vector<glm::mat4> x3(o.size(), glm::mat4(1.f));
                for (size_t i = 0; i < o2.size(); ++i) { o3.push_back(o2[i]); n3.push_back(n2[i]); x3.push_back(x2[i]); }
                o = o3; nm = n3; xf = x3;
            } else {
                o = o2; nm = n2; xf = x2;
            }
            item.instanceRefs.assign(o.size(), TargetRef{0, 0});
            size_t k = perTarget ? 0 : o.size() - passThrough.size();
            for (size_t i = 0; i < allT.size() && k < o.size(); ++i) {
                bool hidden = std::find(passThrough.begin(), passThrough.end(), allT[i]) != passThrough.end();
                if (perTarget) { if (hidden) item.instanceRefs[k] = allT[i]; ++k; }
                else if (hidden) item.instanceRefs[k++] = allT[i];
            }
        };
        if (tids.empty()) {
            if (op == 1) {
                // Join with nothing to join: Fusion makes a new body.
                outs.push_back(tool);
                names.push_back(f.name);
            } else if (passThrough.empty()) {
                throw std::runtime_error(op == 2 ? "Nothing to cut: the cut does not touch any body"
                                                 : "Nothing to intersect: the shape does not touch any body");
            } else {
                item.warn = f.name + ": it only touches hidden parts, so they are left as they were.";
            }
        } else {
            std::vector<kernel::BoolTarget> targets;
            std::vector<std::string> tnames;
            for (const TargetRef& ref : tids) {
                ResolvedTarget rt;
                resolve(ref, rt);
                targets.push_back({rt.shape, rt.xform});
                tnames.push_back(rt.name.empty() ? f.name : rt.name);
                item.consumed.push_back(ref);
            }
            outs = kernel::applyBool(boolOpOf(op), tool, targets);
            for (size_t i = 0; i < outs.size(); ++i) names.push_back(i < tnames.size() ? tnames[i] : f.name);
        }
        outX.assign(outs.size(), glm::mat4(1.f));
        addPassThrough(outs, names, outX);
    }

    for (size_t i = 0; i < outs.size(); ++i) {
        item.shapes.push_back(outs[i]);
        item.meshes.push_back(kernel::tessellate(*outs[i]));
        if (!outX.empty()) item.xforms.push_back(i < outX.size() ? outX[i] : glm::mat4(1.f));
    }
    item.names = std::move(names);
}

// When this file was built: cached results of an older build are never reused.
const uint64_t kLogicStamp = [] {
    uint64_t h = 1469598103934665603ull;
    for (const char* c = __DATE__ " " __TIME__ " logic"; *c; ++c) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    return h;
}();

void runRecomputePass(const Document& snap, App::RecomputeResults& out, const CancelToken& cancel, JobSystem* jobs) {
    uint64_t running = kFnvSeed;
    int total = 0;
    for (int i = 0; i < snap.markerPos && i < (int)snap.timeline.size(); ++i)
        if (!snap.timeline[i].suppressed) ++total;
    int done = 0;
    // Which step used up which body, so a second step that changes the same body can be spotted.
    // That happens when a step failed, was tried again as a new step, and the first try starts
    // working later (a kernel fix): both then made a copy of the body, stacked in the same place.
    std::map<TargetRef, std::vector<int>> takers;           // body -> timeline indexes of the steps that used it up
    std::map<uint32_t, std::vector<TargetRef>> usedBefore;  // step id -> bodies it used up last time
    for (auto& [bid, b] : snap.bodies) {
        if (!b.consumedBy) continue;
        std::vector<uint32_t> ids = snap.featureBodies(b.featureId);
        for (uint32_t k = 0; k < ids.size(); ++k)
            if (ids[k] == bid) usedBefore[b.consumedBy].push_back({b.featureId, k});
    }
    std::vector<uint64_t> chainAt(snap.timeline.size(), 0), ownAt(snap.timeline.size(), 0);
    // Once a pinned result is used, the disk cache is off for the rest of the pass: its key only
    // knows the steps' settings, not that a pin changed what they are built from, so a result
    // cached before the pin existed would come back.
    bool afterPin = false;
    bool afterRebuiltFromSteps = false;
    for (int i = 0; i < snap.markerPos && i < (int)snap.timeline.size(); ++i) {
        if (cancel.isCancelled()) break;
        const Feature& f = snap.timeline[i];
        if (f.suppressed) continue;
        uint64_t own = ownSig(snap, f);
        uint64_t chain = mixU64(running, own);
        chainAt[i] = chain;
        ownAt[i] = own;
        ++done;
        if (chain == f.chainSig && (!f.failed || f.type != FeatureType::Import)) {
            auto ub = usedBefore.find(f.id);
            if (ub != usedBefore.end())
                for (auto& r : ub->second) takers[r].push_back(i);
        }
        // A step that failed with these same inputs fails again: skip it too (a failed slow thread
        // was recomputed on every later change). Imports retry, the file may be back.
        if (chain == f.chainSig && (!f.failed || f.type != FeatureType::Import)) {
            running = chain;
            continue;   // inputs unchanged since the last compute: keep the cached bodies
        }
        App::RecomputeResults::Item item;
        item.featureId = f.id;
        item.sig = own;
        item.chainSig = chain;
        if (std::getenv("KESTRAL_CHAINS")) std::fprintf(stderr, "CHAIN %u %s %016llx\n", f.id, f.name.c_str(), (unsigned long long)chain);
        auto t0 = std::chrono::steady_clock::now();
        std::string err;
        bool ok = false;
        bool fromDisk = false;
        if (f.type != FeatureType::Import && f.type != FeatureType::Sketch) {
            kernel::FeatureCacheData cd;
            bool pinned = false;
            if (!snap.path.empty() && !snap.skipSavedShapes) {
                // Saved shapes first (kept with the design on every save, by the step's inputs). On
                // the first build after opening, a step whose inputs read differently now (an
                // imported file's date, a newer Kestral) still shows what was saved.
                std::filesystem::path dp = u8path(snap.path);
                std::filesystem::path gdir = dp.parent_path() / ".kestral_versions" / dp.stem() / "geo";
                auto geoFile = [&](uint64_t c) {
                    char nm[48];
                    std::snprintf(nm, sizeof nm, "%u-%016llx.kfc", f.id, (unsigned long long)c);
                    return pathToU8(gdir / nm);
                };
                pinned = kernel::loadPinnedResult(geoFile(chain), cd);
                if (!pinned && snap.fromFile && f.savedChain && f.savedChain != chain && !afterRebuiltFromSteps) {
                    pinned = kernel::loadPinnedResult(geoFile(f.savedChain), cd);
                    if (pinned) {
                        if (!out.keptSaved) out.keptFirst = f.name;
                        ++out.keptSaved;
                    }
                }
            }
            if (!pinned && !snap.path.empty() && !snap.skipSavedShapes) {
                std::filesystem::path dp = u8path(snap.path);
                std::filesystem::path pdir = dp.parent_path() / ".kestral_versions" / dp.stem() / "pins";
                std::string pin = pathToU8(pdir / (std::to_string(f.id) + ".kfc"));
                // A pin holds while everything before the step is unchanged (its .chain file); edit
                // anything upstream and the step computes normally again.
                std::ifstream cf(pdir / (std::to_string(f.id) + ".chain"));
                std::string want;
                if (cf) cf >> want;
                char have[24];
                std::snprintf(have, sizeof have, "%016llx", (unsigned long long)chain);
                // "auto": the pin was made elsewhere (file dates differ per computer): take this
                // computer's chain on first use, so later upstream edits still drop it.
                if (want == "auto") {
                    cf.close();
                    std::ofstream wf(pdir / (std::to_string(f.id) + ".chain"), std::ios::trunc);
                    wf << have << "\n";
                    want = have;
                }
                if (want.empty() || want == have) pinned = kernel::loadPinnedResult(pin, cd);
            }
            if (pinned) afterPin = true;
            if (pinned || (!afterPin && kernel::loadFeatureCache(chain ^ kLogicStamp, cd))) {
                fromDisk = ok = platform::guarded("feature cache", [&] {
                    if (!pinned) item.warn = cd.warn;
                    item.names = cd.names;
                    item.xforms = cd.xforms;
                    item.instanceRefs = cd.instanceRefs;
                    item.consumed = cd.consumed;
                    item.shapes = cd.shapes;
                    for (auto& sh : item.shapes) item.meshes.push_back(sh ? kernel::tessellate(*sh) : MeshPtr());
                }, err);
                if (!ok) { item = {}; item.featureId = f.id; item.sig = own; item.chainSig = chain; err.clear(); }
            }
        }
        kernel::setCopyBeforeUnify(f.rules >= 2);
        if (!fromDisk) {
            ok = platform::guarded("feature", [&] { computeFeature(snap, out.items, f, item, cancel, jobs); }, err);
            // A step built fresh (no saved shapes for it) feeds the ones after it: from here on
            // older saved shapes would not fit what it made.
            if (f.type != FeatureType::Sketch && f.type != FeatureType::Import) afterRebuiltFromSteps = true;
        }
        kernel::setCopyBeforeUnify(false);
        item.ok = ok;
        item.error = err;
        item.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        // Slow results go to the disk cache, so the next open of this design is quick.
        if (ok && !fromDisk && !afterPin && !item.isImport && item.ms > 400 && !cancel.isCancelled()) {
            kernel::FeatureCacheData cd;
            cd.warn = item.warn;
            cd.shapes = item.shapes;
            cd.names = item.names;
            cd.xforms = item.xforms;
            cd.instanceRefs = item.instanceRefs;
            cd.consumed = item.consumed;
            std::string e2;
            platform::guarded("feature cache save", [&] { kernel::saveFeatureCache(chain ^ kLogicStamp, cd); }, e2);
        }
        if (std::getenv("KESTRAL_PROFILE")) std::fprintf(stderr, "FEATURE %s %.0f ms\n", f.name.c_str(), item.ms);
        if (std::getenv("KESTRAL_CHECK")) {
            for (size_t k = 0; k < item.shapes.size(); ++k) {
                if (!item.shapes[k]) continue;
                std::fprintf(stderr, "CHECK %s[%zu]: %s faces=%u vol=%.3f\n", f.name.c_str(), k,
                             kernel::shapeIsValid(item.shapes[k]) ? "valid" : "INVALID", kernel::faceCount(item.shapes[k]),
                             kernel::solidVolume(item.shapes[k]));
            }
            if (!ok) std::fprintf(stderr, "CHECK %s failed: %s\n", f.name.c_str(), err.c_str());
            for (auto& c : item.consumed) std::fprintf(stderr, "CHECK %s uses %u:%u\n", f.name.c_str(), c.first, c.second);
        }
        if (item.ok)
            for (const TargetRef& r : item.consumed) takers[r].push_back(i);
        out.items.push_back(std::move(item));
        jobs->setStatus("Recomputing " + std::to_string(done) + " / " + std::to_string(total));
        running = chain;
    }
    if (cancel.isCancelled()) return;
    // Two steps that both change one body each made their own copy of it (stacked in one place).
    // The try whose result nothing else builds on is left out; when neither is built on, the
    // later one stays. When both are built on, both stay (nothing is broken to fix the other).
    auto builtOn = [&](int idx) {
        uint32_t fid = snap.timeline[(size_t)idx].id;
        for (auto& [ref, who] : takers)
            if (ref.first == fid && !who.empty()) return true;
        return false;
    };
    std::set<int> leaveOut;
    for (auto& [ref, who] : takers) {
        std::vector<int> w = who;
        std::sort(w.begin(), w.end());
        w.erase(std::unique(w.begin(), w.end()), w.end());
        if (w.size() < 2) continue;
        std::vector<int> used;
        for (int x : w) if (builtOn(x)) used.push_back(x);
        int keep = used.empty() ? w.back() : -1;
        for (int x : w)
            if (x != keep && std::find(used.begin(), used.end(), x) == used.end()) leaveOut.insert(x);
    }
    for (int x : leaveOut) {
        const Feature& a = snap.timeline[(size_t)x];
        App::RecomputeResults::Item skip;
        skip.featureId = a.id;
        skip.sig = ownAt[(size_t)x];
        skip.chainSig = chainAt[(size_t)x];
        skip.ok = true;
        skip.warn = a.name + " is left out: a later step changes the same body (it was an earlier try).";
        out.items.push_back(std::move(skip));
    }
}

} // namespace

void App::refreshSignatures() {
    uint64_t running = kFnvSeed;
    for (int i = 0; i < doc_.markerPos && i < (int)doc_.timeline.size(); ++i) {
        Feature& f = doc_.timeline[i];
        if (f.suppressed) continue;
        uint64_t own = ownSig(doc_, f);
        running = mixU64(running, own);
        f.sig = own;
        f.chainSig = running;
    }
}

// ===================================================================== scheduling

void App::scheduleRecompute(int fromIndex) {
    if (recomputeQueuedFrom_ < 0 || fromIndex < recomputeQueuedFrom_) recomputeQueuedFrom_ = fromIndex;
    pumpRecompute();
}

void App::pumpRecompute() {
    if (recomputeQueuedFrom_ < 0 || recomputeRunning_ || !jobs_) return;
    recomputeQueuedFrom_ = -1;
    recomputeRunning_ = true;
    auto res = std::make_shared<RecomputeResults>();
    res->baseRev = doc_.revision;
    Document snap = doc_;
    auto cancel = std::make_shared<CancelToken>();
    JobSystem* jobs = jobs_.get();
    jobs_->setStatus("Recomputing");
    uint64_t gen = docGen_;
    jobs_->submit("recompute",
        [this, jobs, snap, res, cancel, gen] {
            runRecomputePass(snap, *res, *cancel, jobs);
            jobs->post([this, jobs, res, gen] {
                jobs->setStatus("");
                recomputeRunning_ = false;
                if (gen != docGen_) {   // computed for a design that is no longer in front
                    scheduleRecompute(0);
                    return;
                }
                if (res->baseRev != doc_.revision) {
                    scheduleRecompute(0);   // the document changed while computing: redo with fresh state
                    return;
                }
                recomputeResults_ = res;
                applyRecomputeResults();
                recomputeResults_.reset();
                pumpRecompute();
            });
        },
        [this, jobs](const std::string& err) {
            jobs->setStatus("");
            recomputeRunning_ = false;
            toast("Recompute failed: " + err, ToastKind::Error);
        });
}

bool App::sketchInUse(uint32_t sid) const {
    for (const Feature& f : doc_.timeline) {
        if (f.suppressed) continue;
        if ((f.extrude && f.extrude->faces.empty() && f.extrude->sketchId == sid) || (f.revolve && f.revolve->sketchId == sid) ||
            (f.emboss && f.emboss->sketchId == sid) || (f.rib && f.rib->sketchId == sid) || (f.web && f.web->sketchId == sid) ||
            (f.rest && f.rest->sketchId == sid) || (f.lip && f.lip->sketchId == sid) || (f.hole && f.hole->sketchId == sid) ||
            (f.splitFace && f.splitFace->sketchId == sid))
            return true;
        if (f.multi && (std::find(f.multi->sketchIds.begin(), f.multi->sketchIds.end(), sid) != f.multi->sketchIds.end() ||
                        std::find(f.multi->railIds.begin(), f.multi->railIds.end(), sid) != f.multi->railIds.end()))
            return true;
    }
    return false;
}

void App::applyRecomputeResults() {
    wdPhase_.store("applying recomputed geometry");
    {   // A sketch is hidden when a feature uses it (like Fusion), so its lines stop drawing on the
        // body. Each new use hides it again; show it yourself and it stays shown for that use.
        uint32_t fid = 0;
        auto use = [&](uint32_t sid) {
            uint64_t key = ((uint64_t)fid << 32) | sid;
            if (!sid || doc_.autoHidden.count(key) || !settings_.hideUsedSketches) return;
            auto it = doc_.sketches.find(sid);
            if (it == doc_.sketches.end()) return;
            it->second.visible = false;
            doc_.autoHidden.insert(key);
            markSceneDirty();
        };
        for (int i = 0; i < doc_.markerPos && i < (int)doc_.timeline.size(); ++i) {
            const Feature& f = doc_.timeline[i];
            fid = f.id;
            if (f.extrude && f.extrude->faces.empty()) use(f.extrude->sketchId);
            if (f.revolve) use(f.revolve->sketchId);
            if (f.emboss) use(f.emboss->sketchId);
            if (f.rib) use(f.rib->sketchId);
            if (f.web) use(f.web->sketchId);
            if (f.rest) use(f.rest->sketchId);
            if (f.lip) use(f.lip->sketchId);
            if (f.hole) use(f.hole->sketchId);
            if (f.splitFace) use(f.splitFace->sketchId);
            if (f.multi) { for (auto s : f.multi->sketchIds) use(s); for (auto s : f.multi->railIds) use(s); }
        }
    }
    auto& res = *recomputeResults_;
    bool any = false;
    bool frozeTargets = false;
    for (auto& item : res.items) {
        int fi = doc_.featureIndex(item.featureId);
        if (fi < 0 || fi >= doc_.markerPos) continue;   // deleted or rolled back while computing
        Feature& f = doc_.timeline[fi];
        if (pendingDeleteFace_ == f.id) {
            pendingDeleteFace_ = 0;
            if (!item.ok) deleteFaceError_ = item.error.empty() ? std::string("Those faces could not be deleted.") : item.error;
        }
        if (dlg_.open && dlg_.featureId == f.id) {
            cutPreview_ = item.cutPreview;
            markSceneDirty();
        }
        if (freezeTargets_.count(f.id) && !(dlg_.open && dlg_.featureId == f.id)) {
            freezeTargets_.erase(f.id);
            if (item.ok && !item.consumed.empty()) {
                // Only the parts it really changed: hidden ones it passed by stay out, or a later
                // rebuild would cut them as chosen targets.
                std::vector<TargetRef> used = item.consumed;
                for (auto& h : item.instanceRefs)
                    if (h.first) used.erase(std::remove(used.begin(), used.end(), h), used.end());
                if (!used.empty()) {
                    if (f.extrude && f.extrude->targets.empty()) { auto p = std::make_shared<ExtrudeParams>(*f.extrude); p->targets = used; f.extrude = p; }
                    if (f.revolve && f.revolve->targets.empty()) { auto p = std::make_shared<RevolveParams>(*f.revolve); p->targets = used; f.revolve = p; }
                }
                frozeTargets = true;
            }
        }
        f.sig = item.sig;
        f.chainSig = item.chainSig;
        if (item.ok && !item.isImport && !item.shapes.empty()) {
            // Kept for the next save: every step's shapes go with the design (see saveStepShapes).
            kernel::FeatureCacheData d;
            d.warn = item.warn;
            d.shapes = item.shapes;
            d.names = item.names;
            d.xforms = item.xforms;
            d.instanceRefs = item.instanceRefs;
            d.consumed = item.consumed;
            auto& m = stepShapes_[f.id];
            m[item.chainSig] = std::move(d);
            while (m.size() > 4) m.erase(m.begin());
        }
        f.lastMs = item.ms;
        f.failed = !item.ok;
        f.error = item.ok ? std::string() : item.error;
        any = true;

        // Drop the old outputs (keep body ids per slot so later features keep their targets).
        std::vector<uint32_t> oldIds = doc_.featureBodies(f.id);
        std::vector<uint32_t> oldParts;
        for (uint32_t id : oldIds) {
            auto bit = doc_.bodies.find(id);
            if (bit != doc_.bodies.end()) oldParts.push_back(bit->second.partId);
            doc_.bodies.erase(id);   // ids are reused per slot below, so selections survive
        }
        if (item.isImport)
            for (auto it = doc_.components.begin(); it != doc_.components.end();)
                it = it->second.featureId == f.id ? doc_.components.erase(it) : std::next(it);

        if (!item.ok) {
            // A failed feature consumes nothing: its targets stay visible.
            for (auto& [id, b] : doc_.bodies)
                if (b.consumedBy == f.id) b.consumedBy = 0;
            continue;   // feature turns red in the timeline, model keeps everything else
        }

        // Each output stays in the component (browser folder) of the body it came from: a moved,
        // filleted or patterned part keeps its place in the assembly tree.
        auto compOf = [&](const TargetRef& r) -> uint32_t {
            std::vector<uint32_t> ids = doc_.featureBodies(r.first);
            if (r.second >= ids.size()) return 0;
            auto bit = doc_.bodies.find(ids[r.second]);
            return bit != doc_.bodies.end() ? bit->second.componentId : 0;
        };
        std::vector<uint32_t> outComp(item.shapes.size(), 0);
        for (size_t k = 0; k < item.shapes.size(); ++k) {
            if (k < item.instanceRefs.size() && item.instanceRefs[k].first) outComp[k] = compOf(item.instanceRefs[k]);
            else if (k < item.consumed.size()) outComp[k] = compOf(item.consumed[k]);
            else if (!item.consumed.empty()) outComp[k] = compOf(item.consumed[0]);
            if (outComp[k] && !doc_.components.count(outComp[k])) outComp[k] = 0;
        }

        // The body each output came from (for its appearance), looked up before consumption changes.
        std::vector<Body> srcBodies(item.shapes.size());
        std::vector<bool> hasSrc(item.shapes.size(), false);
        for (size_t k = 0; k < item.shapes.size(); ++k) {
            TargetRef r{0, 0};
            if (k < item.instanceRefs.size() && item.instanceRefs[k].first) r = item.instanceRefs[k];
            else if (k < item.consumed.size()) r = item.consumed[k];
            else if (!item.consumed.empty()) r = item.consumed[0];
            if (!r.first) continue;
            std::vector<uint32_t> ids = doc_.featureBodies(r.first);
            if (r.second >= ids.size()) continue;
            auto bit = doc_.bodies.find(ids[r.second]);
            if (bit == doc_.bodies.end()) continue;
            srcBodies[k] = bit->second;
            hasSrc[k] = true;
        }
        auto srcOf = [&](size_t k) -> const Body* { return k < hasSrc.size() && hasSrc[k] ? &srcBodies[k] : nullptr; };

        // Fresh consumption list (feature may pick different targets after an edit).
        for (auto& [id, b] : doc_.bodies)
            if (b.consumedBy == f.id) b.consumedBy = 0;
        for (const TargetRef& ref : item.consumed) {
            std::vector<uint32_t> ids = doc_.featureBodies(ref.first);
            if (ref.second < ids.size()) doc_.bodies[ids[ref.second]].consumedBy = f.id;
        }

        if (item.isImport) {
            addImportToDoc(item.import, f.id, f.importPath);
            continue;
        }
        for (size_t k = 0; k < item.shapes.size(); ++k) {
            bool linked = k < item.instanceRefs.size() && item.instanceRefs[k].first != 0;
            bool shapeNull = !item.shapes[k];
            bool isInstance = linked && shapeNull;
            uint32_t bid = k < oldIds.size() ? oldIds[k] : doc_.newId();
            uint32_t reuse = 0;
            if (isInstance) {
                TargetRef ir = item.instanceRefs[k];
                std::vector<uint32_t> ids = doc_.featureBodies(ir.first);
                if (ir.second < ids.size()) {
                    auto bit = doc_.bodies.find(ids[ir.second]);
                    if (bit != doc_.bodies.end()) reuse = bit->second.partId;
                }
            }
            uint32_t pid;
            if (isInstance && reuse != 0 && doc_.parts.count(reuse)) {
                pid = reuse;   // instance an existing part (patterns): no new geometry uploaded
            } else if (isInstance) {
                continue;      // the instanced part no longer exists: drop this instance quietly
            } else {
                pid = k < oldParts.size() ? oldParts[k] : doc_.newId();
                Part p;
                p.id = pid;
                p.name = item.names[k];
                p.shape = item.shapes[k];
                p.mesh = item.meshes[k];
                doc_.parts[pid] = p;
            }
            Body b;
            b.id = bid;
            b.name = item.names[k];
            b.partId = pid;
            b.componentId = outComp[k] ? outComp[k] : doc_.rootId;
            if (!outComp[k]) {
                auto fit = doc_.featureFolder.find(f.id);
                if (fit != doc_.featureFolder.end() && doc_.components.count(fit->second)) b.componentId = fit->second;
            }
            b.featureId = f.id;
            b.xform = k < item.xforms.size() ? item.xforms[k] : glm::mat4(1.f);
            // Appearance comes from the body it was made from (a moved, filleted or patterned part
            // keeps its look); the user's own choice for this output wins.
            bool sameTopology = isInstance;
            if (const Body* src = srcOf(k)) {
                b.color = src->color;
                b.mat = src->mat;
                b.visible = src->visible;   // a feature that replaces a hidden body keeps it hidden
                if (sameTopology) b.faceLooks = src->faceLooks;
            }
            if (const BodyOverride* o = doc_.findOverride(f.id, (uint32_t)k)) {
                if (!o->name.empty()) b.name = o->name;
                if (o->hasColor) {
                    b.color = o->color;
                    b.mat = o->mat;
                    b.faceLooks = o->faceLooks;
                }
                b.visible = o->visible;
            } else if (b.name == f.name || b.name.rfind(f.name + " ", 0) == 0) {
                // A brand new body: Fusion style name (Body1, Body2...), remembered from now on.
                int n = 0;
                for (auto& [oid, ob] : doc_.bodies)
                    if (ob.name.rfind("Body", 0) == 0) n = std::max(n, std::atoi(ob.name.c_str() + 4));
                for (auto& [key, ov] : doc_.overrides)
                    if (ov.name.rfind("Body", 0) == 0) n = std::max(n, std::atoi(ov.name.c_str() + 4));
                b.name = "Body" + std::to_string(n + 1);
                BodyOverride o;
                o.name = b.name;
                o.color = b.color;
                o.visible = b.visible;
                doc_.setOverride(f.id, (uint32_t)k, o);
            }
            doc_.bodies[bid] = b;
            if (std::getenv("KESTRAL_DEBUG_SLOTS"))
                fprintf(stderr, "[slots] feature %u (%s) slot %zu body %u '%s' vis %d%s\n",
                        f.id, f.name.c_str(), k, bid, b.name.c_str(), (int)b.visible,
                        srcOf(k) ? " (inherited)" : "");
        }
    }
    if (any) {
        for (auto it = selection_.begin(); it != selection_.end();)
            it = doc_.bodies.count(*it) ? std::next(it) : selection_.erase(it);
        doc_.gcParts();
        doc_.touch();
        // Rebuilt geometry is not an edit (opening a design rebuilds it): edits mark dirty_ themselves.
        markSceneDirty();
        for (auto& item : res.items)
            if (item.ok && !item.warn.empty()) toast(item.warn, ToastKind::Warning);
        // Construction planes are cheap data: derive them from their features directly.
        for (auto& ft : doc_.timeline) {
            if (ft.type == FeatureType::ConstructPlane && ft.cplane) updateConstructPlane(ft);
            if (ft.type == FeatureType::ConstructAxis && ft.caxis) updateConstructAxis(ft);
            if (ft.type == FeatureType::ConstructPoint && ft.cpoint) updateConstructPoint(ft);
        }
    }
    // Fixing an extrude's bodies changes its inputs but not its result: record the new signatures
    // so nothing recomputes for it.
    if (frozeTargets) {
        std::map<uint32_t, uint64_t> before;
        for (auto& f : doc_.timeline) before[f.id] = f.chainSig;
        refreshSignatures();
        // Same shapes, new inputs (the bodies were fixed): the kept shapes follow the new chain.
        for (auto& f : doc_.timeline) {
            auto it = stepShapes_.find(f.id);
            if (it == stepShapes_.end() || before[f.id] == f.chainSig) continue;
            auto old = it->second.find(before[f.id]);
            if (old != it->second.end()) it->second[f.chainSig] = old->second;
        }
        dirty_ = true;
    }
    if (doc_.fromFile || doc_.skipSavedShapes) {
        doc_.fromFile = false;
        doc_.skipSavedShapes = false;
        if (res.keptSaved > 0)
            toast("Showing the saved shapes of " + std::to_string(res.keptSaved) + " step" + (res.keptSaved > 1 ? "s" : "") +
                  " (from " + res.keptFirst + " on): an imported file changed or Kestral was updated since the save, so building them "
                  "again could come out different. Use Rebuild From Steps (Inspect) if you want them rebuilt.", ToastKind::Info);
    }
    if (!deleteFaceError_.empty()) {
        // Nothing was deleted: say why, and leave no red step behind.
        std::string why = deleteFaceError_;
        deleteFaceError_.clear();
        undo();
        toast("Delete Face did not work: " + why, ToastKind::Warning);
    }
}

void App::updateConstructPlane(const Feature& ft) {
    const ConstructPlaneParams& cp = *ft.cplane;
    sk::Plane pl;
    glm::dvec3 n;
    if (cp.base == 1) {
        pl.x = {1, 0, 0};
        pl.y = {0, 0, 1};
        n = {0, -1, 0};
        pl.name = "XZ";
    } else if (cp.base == 2) {
        pl.x = {0, 1, 0};
        pl.y = {0, 0, 1};
        n = {1, 0, 0};
        pl.name = "YZ";
    } else {
        pl.x = {1, 0, 0};
        pl.y = {0, 1, 0};
        n = {0, 0, 1};
        pl.name = "XY";
    }
    double off = 0;
    try {
        off = peval::eval(cp.offsetExpr, doc_.params);
    } catch (const std::runtime_error&) {
    }
    if (cp.kind == 1) {
        const double kPi = 3.14159265358979323846;
        glm::dvec3 ax = cp.axis == 1 ? glm::dvec3(pl.y) : glm::dvec3(pl.x);
        double ang = off * kPi / 180.0;
        double c = std::cos(ang), s = std::sin(ang);
        auto rot = [&](glm::dvec3 v) {
            return v * c + glm::cross(ax, v) * s + ax * glm::dot(ax, v) * (1.0 - c);
        };
        glm::dvec3 nn = rot(n);
        if (glm::length(nn) > 1e-9) nn = glm::normalize(nn);
        if (cp.axis == 1) {
            pl.y = ax;
            pl.x = glm::normalize(glm::cross(glm::dvec3(pl.y), nn));
        } else {
            pl.x = ax;
            pl.y = glm::normalize(glm::cross(nn, glm::dvec3(pl.x)));
        }
        pl.o = {0, 0, 0};
        pl.name = "Angle";
    } else if (cp.kind == 3) {
        auto ev = [&](const std::string& e) {
            try { return peval::eval(e, doc_.params); }
            catch (const std::runtime_error&) { return 0.0; }
        };
        glm::dvec3 a(ev(cp.x1), ev(cp.y1), ev(cp.z1));
        glm::dvec3 b(ev(cp.x2), ev(cp.y2), ev(cp.z2));
        glm::dvec3 c(ev(cp.x3), ev(cp.y3), ev(cp.z3));
        glm::dvec3 u = b - a, v = c - a;
        glm::dvec3 nrm = glm::cross(u, v);
        if (glm::length(u) < 1e-6 || glm::length(nrm) < 1e-6) {
            doc_.constructPlanes.erase(ft.id);
            return;
        }
        nrm = glm::normalize(nrm);
        glm::dvec3 xx = glm::normalize(u);
        glm::dvec3 yy = glm::normalize(glm::cross(nrm, xx));
        pl.o = a;
        pl.x = xx;
        pl.y = yy;
        pl.name = "3 Points";
        doc_.constructPlanes[ft.id] = pl;
        return;
    } else if (cp.kind == 4) {
        auto fail = [&]() {
            doc_.constructPlanes.erase(ft.id);
        };
        if (!cp.faceRef.first) { fail(); return; }
        std::vector<uint32_t> ids = doc_.featureBodies(cp.faceRef.first);
        if (cp.faceRef.second >= ids.size()) { fail(); return; }
        auto bit = doc_.bodies.find(ids[cp.faceRef.second]);
        auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
        kernel::RoundFace rf;
        if (pit == doc_.parts.end() || !pit->second.shape ||
            !kernel::roundFace(pit->second.shape, bit->second.xform, cp.faceSub, rf)) {
            fail();
            return;
        }
        glm::dvec3 axis = glm::length(rf.dir) > 1e-9 ? glm::normalize(rf.dir) : glm::dvec3(0, 0, 1);
        glm::dvec3 ref = std::abs(axis.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
        glm::dvec3 u = glm::normalize(glm::cross(axis, ref));
        glm::dvec3 v = glm::normalize(glm::cross(axis, u));
        double ang = 0;
        try { ang = peval::eval(cp.angleExpr.empty() ? "0" : cp.angleExpr, doc_.params) * 3.14159265358979323846 / 180.0; }
        catch (const std::runtime_error&) {}
        glm::dvec3 radial = u * std::cos(ang) + v * std::sin(ang);
        if (glm::length(radial) < 1e-9) { fail(); return; }
        radial = glm::normalize(radial);
        pl.o = rf.origin + radial * rf.radius;
        pl.x = axis;
        pl.y = glm::normalize(glm::cross(radial, axis));
        pl.name = "Tangent";
        doc_.constructPlanes[ft.id] = pl;
        return;
    } else if (cp.kind == 2) {
        auto it = doc_.constructPlanes.find(cp.refPlane);
        if (it != doc_.constructPlanes.end() && it->first != ft.id) {
            glm::dvec3 n2 = glm::cross(glm::dvec3(it->second.x), glm::dvec3(it->second.y));
            if (glm::length(n2) > 1e-9) {
                n2 = glm::normalize(n2);
                if (glm::dot(n, n2) < 0) n2 = -n2;
                if (std::abs(glm::dot(n, n2)) > 0.999) {
                    double d0 = glm::dot(glm::dvec3(pl.o), n);
                    double d1 = glm::dot(glm::dvec3(it->second.o), n);
                    pl.o = n * (0.5 * (d0 + d1));
                } else {
                    glm::dvec3 dir = glm::cross(n, n2);
                    double d0 = glm::dot(n, glm::dvec3(pl.o));
                    double d1 = glm::dot(n2, glm::dvec3(it->second.o));
                    glm::dvec3 p = glm::cross(n2, dir) * d0 + glm::cross(dir, n) * d1;
                    if (glm::dot(dir, dir) > 1e-12) p /= glm::dot(dir, dir);
                    glm::dvec3 nb = glm::normalize(n + n2);
                    dir = glm::normalize(dir);
                    pl.o = p;
                    pl.x = dir;
                    pl.y = glm::normalize(glm::cross(nb, dir));
                }
                pl.name = "Mid";
            }
        }
    } else {
        pl.o = n * off;
    }
    doc_.constructPlanes[ft.id] = pl;
}

namespace {
double evalOr(const std::string& e, const ParamMap& params) {
    try { return peval::eval(e, params); }
    catch (const std::runtime_error&) { return 0; }
}
} // namespace

void App::updateConstructAxis(const Feature& ft) {
    const ConstructAxisParams& a = *ft.caxis;
    ConstructAxis ax;
    if (a.kind == 0) {
        ax.origin = {evalOr(a.ax, doc_.params), evalOr(a.ay, doc_.params), evalOr(a.az, doc_.params)};
        glm::dvec3 b(evalOr(a.bx, doc_.params), evalOr(a.by, doc_.params), evalOr(a.bz, doc_.params));
        glm::dvec3 d = b - ax.origin;
        if (glm::length(d) > 1e-6) ax.direction = glm::normalize(d);
    } else if (a.ref.first) {
        std::vector<uint32_t> ids = doc_.featureBodies(a.ref.first);
        if (a.ref.second < ids.size()) {
            auto bit = doc_.bodies.find(ids[a.ref.second]);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            if (pit != doc_.parts.end() && pit->second.shape) {
                if (a.kind == 1) {
                    kernel::EdgeGeom g;
                    if (kernel::edgeGeom(pit->second.shape, bit->second.xform, a.sub, g) && g.type == 0) {
                        glm::dvec3 d = g.b - g.a;
                        if (glm::length(d) > 1e-6) {
                            ax.origin = g.a;
                            ax.direction = glm::normalize(d);
                        }
                    }
                } else {
                    kernel::RoundFace rf;
                    if (kernel::roundFace(pit->second.shape, bit->second.xform, a.sub, rf)) {
                        ax.origin = rf.origin;
                        ax.direction = rf.dir;
                    }
                }
            }
        }
    }
    doc_.constructAxes[ft.id] = ax;
}

void App::updateConstructPoint(const Feature& ft) {
    const ConstructPointParams& p = *ft.cpoint;
    glm::dvec3 at{evalOr(p.x, doc_.params), evalOr(p.y, doc_.params), evalOr(p.z, doc_.params)};
    if (p.kind != 0 && p.ref.first) {
        std::vector<uint32_t> ids = doc_.featureBodies(p.ref.first);
        if (p.ref.second < ids.size()) {
            auto bit = doc_.bodies.find(ids[p.ref.second]);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            if (pit != doc_.parts.end() && pit->second.shape) {
                if (p.kind == 1) {
                    kernel::EdgeGeom g;
                    if (kernel::edgeGeom(pit->second.shape, bit->second.xform, p.sub, g)) {
                        if (g.type == 0) at = (g.a + g.b) * 0.5;
                        else if (!g.pts.empty()) at = g.pts[g.pts.size() / 2];
                    }
                } else if (p.kind == 3) {
                    glm::dvec3 w;
                    if (!kernel::vertexPoint(pit->second.shape, bit->second.xform, p.sub, w)) {
                        doc_.constructPoints.erase(ft.id);
                        return;
                    }
                    at = w;
                } else {
                    glm::dvec3 c, n;
                    bool planar = false;
                    if (kernel::faceInfo(pit->second.shape, p.sub, c, n, planar) && planar) {
                        glm::dmat4 xf(bit->second.xform);
                        at = glm::dvec3(xf * glm::dvec4(c, 1.0));
                    }
                }
            }
        }
    }
    doc_.constructPoints[ft.id] = at;
}

// ===================================================================== feature creation

uint32_t App::pickSketchForFeature() {
    if (sk_.active) return sk_.sketchId;
    uint32_t bestSid = 0;
    for (int i = 0; i < doc_.markerPos && i < (int)doc_.timeline.size(); ++i)
        if (doc_.timeline[i].type == FeatureType::Sketch) bestSid = doc_.timeline[i].sketchId;
    return bestSid;
}

void App::runExtrude() { startSolidFeature(false); }
void App::runRevolve() { startSolidFeature(true); }

namespace {
// Fusion's order in the Operation list.
const char* kOpNames[] = {"New Body", "Join", "Cut", "Intersect"};
const int kOpOrder[] = {1, 2, 3, 0};
} // namespace

void App::startSolidFeature(bool revolve) {
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (measureMode_) runMeasure();

    // What to extrude: the sketch being edited, else the picked profiles / flat faces.
    // An open curve is kept so a wall thickness can turn it into a solid.
    uint32_t openSid = 0;
    if (sk_.active) {
        uint32_t sid = sk_.sketchId;
        finishSketch();
        auto sit = doc_.sketches.find(sid);
        if (sit == doc_.sketches.end()) return;
        const auto& profs = sketchProfiles(sit->second);
        clearSelection();
        if (profs.size() == 1) selProfiles_.insert({sid, 0});
        else if (profs.empty()) {
            if (chainedPath(sit->second).empty()) {
                toast("That sketch has no closed profile yet", ToastKind::Warning);
                return;
            }
            openSid = sid;
        }
    }
    if (revolve) {
        selFaces_.clear();
        if (selProfiles_.empty()) {
            uint32_t sid = pickSketchForFeature();
            auto sit = doc_.sketches.find(sid);
            if (sit != doc_.sketches.end() && sketchProfiles(sit->second).size() == 1) selProfiles_.insert({sid, 0});
        }
    }
    // Faces that are not flat cannot be extruded: drop them quietly.
    for (auto it = selFaces_.begin(); it != selFaces_.end();) {
        auto bit = doc_.bodies.find(it->first);
        auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
        glm::dvec3 c, n;
        bool planar = false;
        bool ok = pit != doc_.parts.end() && pit->second.shape && kernel::faceInfo(pit->second.shape, it->second, c, n, planar) && planar;
        it = ok ? std::next(it) : selFaces_.erase(it);
    }
    selEdges_.clear();
    selection_.clear();

    pushUndo(revolve ? "Revolve" : "Extrude");
    Feature f;
    f.id = doc_.newId();
    f.type = revolve ? FeatureType::Revolve : FeatureType::Extrude;
    int sameType = 0;
    for (auto& ft : doc_.timeline)
        if (ft.type == f.type) ++sameType;
    f.name = (revolve ? "Revolve" : "Extrude") + std::to_string(sameType + 1);
    if (revolve) f.revolve = std::make_shared<RevolveParams>();
    else f.extrude = std::make_shared<ExtrudeParams>();
    int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
    doc_.timeline.insert(doc_.timeline.begin() + pos, f);
    doc_.markerPos = pos + 1;
    doc_.touch();
    dirty_ = true;
    stampActiveComponent(f.id);

    dlg_ = FeatureDlg{};
    dlg_.open = true;
    dlg_.featureId = f.id;
    dlg_.isRevolve = revolve;
    dlg_.focus = false;
    std::snprintf(dlg_.angle, sizeof dlg_.angle, "360");
    std::snprintf(dlg_.dist, sizeof dlg_.dist, "0");   // Fusion: nothing grows until you drag or type
    dlg_.op = 0;
    activePickFilter_ = revolve ? PickProfile : (PickProfile | PickFace);
    markSceneDirty();
    syncExtrudeFromSelection();   // reads the picks, sets a sensible distance and operation
    if (openSid) {
        int fi = doc_.featureIndex(dlg_.featureId);
        if (fi >= 0) {
            if (revolve && doc_.timeline[fi].revolve) {
                auto p = std::make_shared<RevolveParams>(*doc_.timeline[fi].revolve);
                p->sketchId = openSid;
                doc_.timeline[fi].revolve = p;
            } else if (!revolve && doc_.timeline[fi].extrude) {
                auto p = std::make_shared<ExtrudeParams>(*doc_.timeline[fi].extrude);
                p->sketchId = openSid;
                doc_.timeline[fi].extrude = p;
            }
        }
        toast(revolve ? "Revolve: type a wall thickness for the open curve" : "Extrude: type a wall thickness for the open curve");
    } else if (!revolve && selProfiles_.empty() && selFaces_.empty())
        toast("Extrude: click the profiles or flat faces to extrude");
}

void App::syncExtrudeFromSelection() {
    if (!dlg_.open) return;
    int fi = doc_.featureIndex(dlg_.featureId);
    if (fi < 0) return;
    Feature& f = doc_.timeline[fi];

    uint32_t sid = 0;
    std::vector<sk::Vec2> pts;
    std::vector<std::pair<uint32_t, std::vector<sk::Vec2>>> more;   // profiles in other sketches
    for (auto& [skid, pi] : selProfiles_) {
        if (sid == 0) sid = skid;
        auto sit = doc_.sketches.find(skid);
        if (sit == doc_.sketches.end()) continue;
        const auto& profs = sketchProfiles(sit->second);
        if (pi >= profs.size()) continue;
        if (skid == sid) { pts.push_back(profileSamplePoint(profs[pi])); continue; }
        if (dlg_.isRevolve) continue;   // revolve: one sketch (its axis lives there)
        auto mit = std::find_if(more.begin(), more.end(), [&](auto& m) { return m.first == skid; });
        if (mit == more.end()) { more.push_back({skid, {}}); mit = more.end() - 1; }
        mit->second.push_back(profileSamplePoint(profs[pi]));
    }
    std::vector<FaceRef> faces;
    if (!dlg_.isRevolve) {
        for (auto& [bid, fidx] : selFaces_) {
            auto bit = doc_.bodies.find(bid);
            if (bit == doc_.bodies.end() || bit->second.featureId == 0 || bit->second.featureId == f.id) continue;
            auto pit = doc_.parts.find(bit->second.partId);
            if (pit == doc_.parts.end() || !pit->second.shape) continue;
            FaceRef fr;
            fr.feature = bit->second.featureId;
            fr.slot = doc_.bodySlot(bid);
            fr.face = fidx;
            fr.faceTotal = kernel::faceCount(pit->second.shape);
            faces.push_back(fr);
        }
    }

    // Manipulator origin and direction, and which side of the profile has material.
    glm::dvec3 origin(0), normal(0, 0, 1);
    bool have = false;
    double size = 20;
    if (sid && !pts.empty()) {
        const sk::Sketch& sk = doc_.sketches[sid];
        origin = sk.plane.toWorld(pts[0]);
        normal = sk.plane.normal();
        sk::Vec2 lo, hi;
        sk.bounds(lo, hi);
        size = glm::length(hi - lo);
        have = true;
    } else if (!faces.empty()) {
        auto bid = *selFaces_.begin();
        auto bit = doc_.bodies.find(bid.first);
        auto pit = doc_.parts.find(bit->second.partId);
        glm::dvec3 c, n;
        bool planar;
        if (kernel::faceInfo(pit->second.shape, bid.second, c, n, planar)) {
            glm::dmat4 xf(bit->second.xform);
            origin = glm::dvec3(xf * glm::dvec4(c, 1.0));
            normal = glm::normalize(glm::dvec3(xf * glm::dvec4(n, 0.0)));
            size = kernel::shapeDiag(pit->second.shape) * 0.5;
            have = true;
        }
    }
    if (have && !dlg_.editing) {
        // Probe just in front of and behind the profile for existing material.
        double eps = std::max(1e-3, size * 1e-3);
        dlg_.inFront = dlg_.inBack = false;
        for (auto& [id, b] : doc_.bodies) {
            if (!bodyShown(b) || b.featureId == f.id) continue;
            auto pit = doc_.parts.find(b.partId);
            if (pit == doc_.parts.end() || !pit->second.shape || !pit->second.mesh) continue;
            Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
            glm::vec3 o3(origin);
            if (o3.x < bb.min.x - 1 || o3.y < bb.min.y - 1 || o3.z < bb.min.z - 1 || o3.x > bb.max.x + 1 || o3.y > bb.max.y + 1 || o3.z > bb.max.z + 1) continue;
            if (kernel::pointInside(pit->second.shape, b.xform, origin + normal * eps)) dlg_.inFront = true;
            if (kernel::pointInside(pit->second.shape, b.xform, origin - normal * eps)) dlg_.inBack = true;
        }
        dlg_.probeO = origin;
        dlg_.probeN = normal;
        // The distance stays 0 until the user drags the arrow or types a value (Fusion).
        (void)size;
    }
    if (dlg_.opAuto && !dlg_.editing) {
        double dv = 0;
        try { dv = peval::eval(dlg_.dist, doc_.params); } catch (const std::runtime_error&) {}
        extrudeAutoOp(dv);
        if (dlg_.isRevolve) dlg_.op = (dlg_.inFront || dlg_.inBack) ? 1 : 0;
    }

    if (dlg_.isRevolve && f.revolve) {
        auto p = std::make_shared<RevolveParams>(*f.revolve);
        if (sid) p->sketchId = sid;
        p->profilePts = pts;
        f.revolve = p;
        // Axis choices: the sketch's lines.
        dlg_.axisLines.clear();
        auto sit = doc_.sketches.find(p->sketchId);
        if (sit != doc_.sketches.end())
            for (auto& [id, e] : sit->second.ents) {
                if (e.type != sk::EntType::Line || e.locked) continue;
                sk::Vec2 a = sit->second.pt(e.a), b = sit->second.pt(e.b);
                char buf[96];
                std::snprintf(buf, sizeof buf, "Line (%.4g, %.4g) - (%.4g, %.4g)%s", a.x, a.y, b.x, b.y, e.construction ? "  construction" : "");
                dlg_.axisLines.push_back({id, buf});
            }
    } else if (f.extrude) {
        auto p = std::make_shared<ExtrudeParams>(*f.extrude);
        p->sketchId = faces.empty() ? sid : 0;
        p->profilePts = faces.empty() ? pts : std::vector<sk::Vec2>{};
        p->moreSketches = faces.empty() ? more : decltype(more){};
        p->faces = faces;
        if (dlg_.extent == 1 && !selFaces_.empty()) {
            auto picked = *selFaces_.begin();
            auto bit = doc_.bodies.find(picked.first);
            auto pit = bit != doc_.bodies.end() ? doc_.parts.find(bit->second.partId) : doc_.parts.end();
            if (bit != doc_.bodies.end() && pit != doc_.parts.end() && pit->second.shape && bit->second.featureId && bit->second.featureId != f.id) {
                dlg_.to.feature = bit->second.featureId;
                dlg_.to.slot = doc_.bodySlot(picked.first);
                dlg_.to.face = picked.second;
                dlg_.to.faceTotal = kernel::faceCount(pit->second.shape);
                dlg_.toName = bit->second.name;
                selFaces_.clear();
                p->faces.clear();
            }
        }
        p->extent = dlg_.extent;
        p->to = dlg_.to;
        f.extrude = p;
        if (have) setManip(1, origin, normal * (dlg_.flip ? -1.0 : 1.0), 0, true);
        else manip_.active = false;
    }
    dlg_.hiddenSketch = sid;
    applyDialogValues();
}

void App::editFeature(uint32_t featureId) {
    int fi = doc_.featureIndex(featureId);
    if (fi < 0 || fi >= doc_.markerPos) return;
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sk_.active) finishSketch();
    Feature& f0 = doc_.timeline[fi];
    if (f0.type == FeatureType::Sketch) { editSketch(f0.sketchId); return; }
    pushUndo("Edit " + f0.name);
    // Fusion rolls the timeline to the feature while you edit it.
    if (fi + 1 < doc_.markerPos) {
        savedMarker_ = doc_.markerPos;
        doc_.markerPos = fi + 1;
        doc_.touch();
        markSceneDirty();
    }
    Feature& f = doc_.timeline[fi];
    clearSelection();
    if ((f.type == FeatureType::Extrude && f.extrude) || (f.type == FeatureType::Revolve && f.revolve)) {
        bool rev = f.type == FeatureType::Revolve;
        dlg_ = FeatureDlg{};
        dlg_.open = true;
        dlg_.editing = true;
        dlg_.opAuto = false;
        dlg_.featureId = f.id;
        dlg_.isRevolve = rev;
        dlg_.focus = false;
        // Re-select what the feature was built from.
        uint32_t sid = rev ? f.revolve->sketchId : f.extrude->sketchId;
        const std::vector<sk::Vec2>& pts = rev ? f.revolve->profilePts : f.extrude->profilePts;
        auto sit = doc_.sketches.find(sid);
        if (sit != doc_.sketches.end()) {
            sit->second.visible = true;
            const auto& profs = sketchProfiles(sit->second);
            for (size_t i = 0; i < profs.size(); ++i) {
                bool use = pts.empty();
                for (auto& q : pts)
                    if (sk::pointInProfile(profs[i], q)) use = true;
                if (use) selProfiles_.insert({sid, (uint32_t)i});
            }
        }
        if (!rev) {
            for (auto& [msid, mpts] : f.extrude->moreSketches) {
                auto mit = doc_.sketches.find(msid);
                if (mit == doc_.sketches.end()) continue;
                mit->second.visible = true;
                const auto& mprofs = sketchProfiles(mit->second);
                for (size_t i = 0; i < mprofs.size(); ++i)
                    for (auto& q : mpts)
                        if (sk::pointInProfile(mprofs[i], q)) { selProfiles_.insert({msid, (uint32_t)i}); break; }
            }
            for (auto& fr : f.extrude->faces) {
                std::vector<uint32_t> ids = doc_.featureBodies(fr.feature);
                if (fr.slot < ids.size()) selFaces_.insert({ids[fr.slot], fr.face});
            }
            std::snprintf(dlg_.dist, sizeof dlg_.dist, "%s", f.extrude->distExpr.c_str());
            std::snprintf(dlg_.taper, sizeof dlg_.taper, "%s", f.extrude->taperExpr.c_str());
            std::snprintf(dlg_.thin, sizeof dlg_.thin, "%s", f.extrude->thinExpr.c_str());
            dlg_.thinSide = f.extrude->thinSide;
            dlg_.op = f.extrude->op;
            dlg_.flip = f.extrude->flip;
            dlg_.symmetric = f.extrude->symmetric;
            dlg_.extent = f.extrude->extent;
            dlg_.to = f.extrude->to;
        } else {
            std::snprintf(dlg_.angle, sizeof dlg_.angle, "%s", f.revolve->angleExpr.c_str());
            std::snprintf(dlg_.thin, sizeof dlg_.thin, "%s", f.revolve->thinExpr.c_str());
            dlg_.thinSide = f.revolve->thinSide;
            dlg_.op = f.revolve->op;
            dlg_.flip = f.revolve->flip;
        }
        activePickFilter_ = rev ? PickProfile : (PickProfile | PickFace);
        syncExtrudeFromSelection();
        if (rev) {
            dlg_.axis = f.revolve->axis < 2 ? f.revolve->axis : 0;
            for (size_t i = 0; i < dlg_.axisLines.size(); ++i)
                if (f.revolve->axis >= 2 && dlg_.axisLines[i].first == f.revolve->axisEnt) dlg_.axis = 2 + (int)i;
        }
        markSceneDirty();
        return;
    }
    editToolFeature(f);
}

void App::applyDialogValues() {
    int fi = doc_.featureIndex(dlg_.featureId);
    if (fi < 0) return;
    Feature& f = doc_.timeline[fi];
    if (dlg_.isRevolve && f.revolve) {
        auto p = std::make_shared<RevolveParams>(*f.revolve);
        p->angleExpr = dlg_.angle;
        p->thinExpr = dlg_.thin;
        p->thinSide = dlg_.thinSide;
        p->axis = dlg_.axis < 2 ? dlg_.axis : 2;
        p->axisEnt = dlg_.axis >= 2 && (size_t)(dlg_.axis - 2) < dlg_.axisLines.size() ? dlg_.axisLines[dlg_.axis - 2].first : 0;
        p->op = dlg_.op;
        p->flip = dlg_.flip;
        f.revolve = p;
    } else if (!dlg_.isRevolve && f.extrude) {
        if (dlg_.opAuto && !dlg_.editing) {
            // A typed distance picks the operation the same way a drag does (into material = cut).
            try {
                double d = peval::eval(dlg_.dist, doc_.params);
                if (std::abs(d) > 1e-9) extrudeAutoOp(d);
            } catch (const std::runtime_error&) {}
        }
        auto p = std::make_shared<ExtrudeParams>(*f.extrude);
        p->distExpr = dlg_.dist;
        p->taperExpr = dlg_.taper;
        p->thinExpr = dlg_.thin;
        p->thinSide = dlg_.thinSide;
        p->op = dlg_.op;
        p->flip = dlg_.flip;
        p->symmetric = dlg_.symmetric && dlg_.extent == 0;
        p->extent = dlg_.extent;
        p->to = dlg_.to;
        f.extrude = p;
        // Keep the arrow at the end of the extrusion.
        try {
            double v = peval::eval(dlg_.dist, doc_.params);
            if (manip_.active && manip_.target == 1 && !manip_.dragging) manip_.value = dlg_.symmetric ? v * 0.5 : v;
        } catch (const std::runtime_error&) {}
    }
    doc_.touch();
    dirty_ = true;
    scheduleRecompute(fi);
}

void App::extrudeAutoOp(double d) {
    bool fwd = (d >= 0) != dlg_.flip;
    bool ahead = fwd ? dlg_.inFront : dlg_.inBack, behind = fwd ? dlg_.inBack : dlg_.inFront;
    if (!ahead && !behind && std::abs(d) > 1e-6 && !dlg_.isRevolve) {
        // Nothing touches the profile itself: material part way along the drag still means a cut.
        glm::dvec3 dir = dlg_.probeN * ((d >= 0) != dlg_.flip ? 1.0 : -1.0);
        double len = std::abs(d);
        for (auto& [id, b] : doc_.bodies) {
            if (ahead) break;
            if (!bodyShown(b) || b.featureId == dlg_.featureId) continue;
            auto pit = doc_.parts.find(b.partId);
            if (pit == doc_.parts.end() || !pit->second.shape || !pit->second.mesh) continue;
            Aabb bb = pit->second.mesh->bounds.transformed(b.xform);
            for (int k = 1; k <= 12 && !ahead; ++k) {
                glm::dvec3 q = dlg_.probeO + dir * (len * k / 12.0);
                glm::vec3 q3(q);
                if (q3.x < bb.min.x || q3.y < bb.min.y || q3.z < bb.min.z || q3.x > bb.max.x || q3.y > bb.max.y || q3.z > bb.max.z) continue;
                if (kernel::pointInside(pit->second.shape, b.xform, q)) ahead = true;
            }
        }
    }
    dlg_.op = ahead ? 2 : behind ? 1 : 0;
}

void App::manipCommit(double v, bool final) {
    if (manip_.target == 1 && dlg_.open) {
        double d = dlg_.symmetric ? v * 2.0 : v;
        std::snprintf(dlg_.dist, sizeof dlg_.dist, "%s", fmtG(d).c_str());
        if (dlg_.opAuto) extrudeAutoOp(d);
        applyDialogValues();
    } else if (manip_.target == 2 && tdlg_.open) {
        std::snprintf(tdlg_.v1, sizeof tdlg_.v1, "%s", fmtG(v).c_str());
        applyToolDialog();
    }
    (void)final;
}

void App::restoreMarkerAfterEdit() {
    if (savedMarker_ >= 0) {
        int m = std::min(savedMarker_, (int)doc_.timeline.size());
        savedMarker_ = -1;
        setMarker(m);
    }
}

void App::closeFeatureDialog(bool ok) {
    if (!dlg_.open) return;
    dlg_.open = false;
    manip_ = Manip{};
    manipShiftFid_ = UINT32_MAX;
    if (cutPreview_) { cutPreview_.reset(); markSceneDirty(); }
    activePickFilter_ = PickBody | PickFace | PickEdge | PickProfile;
    int fi = doc_.featureIndex(dlg_.featureId);
    if (!ok) {
        savedMarker_ = -1;
        clearSelection();
        undo();   // back to before the command (also restores the timeline marker)
        return;
    }
    if (fi >= 0) {
        Feature& f = doc_.timeline[fi];
        bool empty = (f.extrude && f.extrude->sketchId == 0 && f.extrude->faces.empty()) ||
                     (f.revolve && f.revolve->sketchId == 0);
        if (empty) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Nothing was picked, so nothing was created");
            return;
        }
        double dv = 1;
        if (f.extrude && !dlg_.editing) { try { dv = peval::eval(f.extrude->distExpr, doc_.params); } catch (const std::runtime_error&) {} }
        bool toFace = f.extrude && f.extrude->extent == 1;
        if (f.extrude && !dlg_.editing && toFace && !f.extrude->to.feature) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("Click the face to stop on");
            return;
        }
        if (f.extrude && !dlg_.editing && !toFace && std::abs(dv) < 1e-9) {
            savedMarker_ = -1;
            clearSelection();
            undo();
            toast("No distance was set, so nothing was created");
            return;
        }
        if (f.failed && !f.error.empty()) toast(f.name + ": " + f.error, ToastKind::Warning);
        // The bodies it joins or cuts are fixed at OK, so later changes elsewhere never make it
        // grab other parts on a recompute.
        if (!dlg_.editing && ((f.extrude && f.extrude->op != 0 && f.extrude->targets.empty()) || (f.revolve && f.revolve->op != 0 && f.revolve->targets.empty())))
            freezeTargets_.insert(f.id);
        // Fusion hides a sketch once a feature has used it.
        auto sit = doc_.sketches.find(dlg_.hiddenSketch);
        if (sit != doc_.sketches.end() && !dlg_.editing && settings_.hideUsedSketches) sit->second.visible = false;
    }
    clearSelection();
    restoreMarkerAfterEdit();
    doc_.touch();
}

// ===================================================================== feature dialog

namespace {

// Fusion style label column + field row.
void rowLabel(const char* label, float S) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(110 * S);
}

} // namespace

void App::drawFeatureDialog() {
    if (!dlg_.open) return;
    int fi = doc_.featureIndex(dlg_.featureId);
    if (fi < 0) { dlg_.open = false; manip_ = Manip{}; return; }
    Feature& f = doc_.timeline[fi];
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 150 * S, vp->Pos.y + 118 * S), ImGuiCond_Appearing, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(320 * S, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * S, 10 * S));
    bool open = true;
    const char* title = dlg_.isRevolve ? "REVOLVE###featdlg" : "EXTRUDE###featdlg";
    ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    auto commit = [this]() { applyDialogValues(); };
    bool ok = false, cancel = false;
    auto enterKey = [] { return ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter); };

    if (f.failed && !f.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
        ImGui::TextWrapped("%s", f.error.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // Profiles / faces picked in the canvas.
    rowLabel("Profile", S);
    size_t np = selProfiles_.size(), nf = dlg_.isRevolve ? 0 : selFaces_.size();
    if (np + nf == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 95, 40, 255));
        ImGui::TextUnformatted(dlg_.isRevolve ? "Select a profile" : "Select profiles or faces");
        ImGui::PopStyleColor();
    } else {
        std::string t = np ? std::to_string(np) + (np == 1 ? " profile" : " profiles") : "";
        if (nf) t += (t.empty() ? "" : ", ") + std::to_string(nf) + (nf == 1 ? " face" : " faces");
        ImGui::TextUnformatted(t.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x##clrprof")) { clearSelection(); }
        ImGui::SetItemTooltip("Clear the picks");
    }

    if (!dlg_.isRevolve) {
        rowLabel("Direction", S);
        const char* dirs[] = {"One Side", "Symmetric"};
        ImGui::SetNextItemWidth(-1);
        int d = dlg_.symmetric ? 1 : 0;
        if (ImGui::Combo("##dir", &d, dirs, 2)) { dlg_.symmetric = d == 1; commit(); }

        rowLabel("Extent", S);
        const char* extents[] = {"Distance", "To Face"};
        ImGui::SetNextItemWidth(-1);
        int ex = dlg_.extent == 1 ? 1 : 0;
        if (ImGui::Combo("##extent", &ex, extents, 2)) { dlg_.extent = ex; commit(); }
        if (dlg_.extent == 1) {
            if (dlg_.to.feature)
                ImGui::TextDisabled("Stops on %s", dlg_.toName.empty() ? "the face" : dlg_.toName.c_str());
            else
                ImGui::TextDisabled("Click the flat face to stop on");
        }

        if (dlg_.extent == 0) {
        rowLabel("Distance", S);
        ImGui::SetNextItemWidth(-1);
        if (dlg_.focus) { ImGui::SetKeyboardFocusHere(); dlg_.focus = false; }
        ImGui::InputText("##dist", dlg_.dist, sizeof dlg_.dist, ImGuiInputTextFlags_AutoSelectAll);
        bool distDone = ImGui::IsItemDeactivated() && enterKey();
        if (ImGui::IsItemEdited()) {
            // Live preview as you type, like Fusion. Bad partial input is simply ignored.
            try {
                double v = peval::eval(dlg_.dist, doc_.params);
                if (std::abs(v) > 1e-9) {
                    if (dlg_.opAuto) extrudeAutoOp(v);
                    commit();
                }
            } catch (const std::runtime_error&) {}
        }
        if (distDone) ok = true;
        try {
            double v = peval::eval(dlg_.dist, doc_.params);
            if (!peval::plainNumber(dlg_.dist)) ImGui::TextDisabled("%*s= %s mm", (int)(18), "", fmtG(v).c_str());
        } catch (const std::runtime_error&) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
            ImGui::TextUnformatted("Not a valid value");
            ImGui::PopStyleColor();
        }
        }

        rowLabel("Taper Angle", S);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##taper", "0 deg", dlg_.taper, sizeof dlg_.taper, ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivatedAfterEdit()) commit();
        if (ImGui::IsItemDeactivated() && enterKey()) ok = true;

        rowLabel("Flip", S);
        if (ImGui::Checkbox("##flip", &dlg_.flip)) {
            if (manip_.active) manip_.dir = -manip_.dir;
            commit();
        }
    } else {
        rowLabel("Axis", S);
        const char* axisNames[2] = {"Sketch Y axis", "Sketch X axis"};
        std::string cur = dlg_.axis < 2 ? axisNames[dlg_.axis] :
                          (size_t)(dlg_.axis - 2) < dlg_.axisLines.size() ? dlg_.axisLines[dlg_.axis - 2].second : "Sketch line";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##axis", cur.c_str())) {
            for (int i = 0; i < 2; ++i)
                if (ImGui::Selectable(axisNames[i], dlg_.axis == i)) { dlg_.axis = i; commit(); }
            for (size_t i = 0; i < dlg_.axisLines.size(); ++i) {
                bool sel = dlg_.axis == 2 + (int)i;
                if (ImGui::Selectable(dlg_.axisLines[i].second.c_str(), sel)) { dlg_.axis = 2 + (int)i; commit(); }
            }
            ImGui::EndCombo();
        }
        rowLabel("Angle", S);
        ImGui::SetNextItemWidth(-1);
        if (dlg_.focus) { ImGui::SetKeyboardFocusHere(); dlg_.focus = false; }
        ImGui::InputText("##ang", dlg_.angle, sizeof dlg_.angle, ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivatedAfterEdit()) commit();
        if (ImGui::IsItemDeactivated() && enterKey()) ok = true;
        rowLabel("Flip", S);
        if (ImGui::Checkbox("##flip", &dlg_.flip)) commit();
    }

    rowLabel("Wall", S);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##thin", "blank = solid", dlg_.thin, sizeof dlg_.thin, ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemDeactivatedAfterEdit()) commit();
    if (dlg_.thin[0]) {
        rowLabel("Wall Side", S);
        const char* sides[] = {"Center", "One Side", "Other Side"};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##thinside", &dlg_.thinSide, sides, 3)) commit();
        ImGui::TextDisabled("An open curve becomes a wall, then the feature runs.");
    }

    rowLabel("Operation", S);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##op", kOpNames[dlg_.op])) {
        for (int k = 0; k < 4; ++k) {
            int i = kOpOrder[k];
            if (ImGui::Selectable(kOpNames[i], dlg_.op == i)) { dlg_.op = i; dlg_.opAuto = false; commit(); }
        }
        ImGui::EndCombo();
    }
    if (dlg_.op != 0) {
        bool fromFaces = dlg_.featureId && doc_.featureIndex(dlg_.featureId) >= 0 && doc_.timeline[doc_.featureIndex(dlg_.featureId)].extrude &&
                         !doc_.timeline[doc_.featureIndex(dlg_.featureId)].extrude->faces.empty();
        ImGui::TextDisabled(dlg_.op == 1 && fromFaces ? "Bodies: the body of the picked face" : "Bodies: the ones it touches now (kept after OK)");
    }
    ImGui::Checkbox("Snap to faces", &settings_.manipSnap);
    ImGui::SetItemTooltip("Dragging the arrow near another flat face stops it on that face's plane.\nHold Alt while dragging to skip snapping.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    float bw = (ImGui::GetContentRegionAvail().x - 8 * S) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 0.42f, 0.21f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.55f, 0.96f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("OK", ImVec2(bw, 28 * S))) ok = true;
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 8 * S);
    if (ImGui::Button("Cancel", ImVec2(bw, 28 * S))) cancel = true;
    ImGui::End();
    ImGui::PopStyleVar();
    if (!open) cancel = true;
    if (ok) closeFeatureDialog(true);
    else if (cancel) closeFeatureDialog(false);
}

// ===================================================================== parameters

void App::openParamsDialog() {
    paramsOpen_ = true;
    paramsErr_.clear();
    paramsUndoKey_.clear();
}

void App::resolveParametricSketches() {
    const double kPi = 3.14159265358979323846;
    for (auto& [sid, s] : doc_.sketches) {
        bool changed = false;
        for (auto& [cid, c] : s.cons) {
            if (c.expr.empty()) continue;
            double v;
            try {
                v = peval::eval(c.expr, doc_.params);
            } catch (const std::runtime_error&) {
                continue;
            }
            if (c.type == sk::CType::Angle) v = v * kPi / 180.0;
            else if (c.type == sk::CType::Radius || c.type == sk::CType::Diameter) v = std::abs(v);
            if (v != c.value) {
                c.value = v;
                changed = true;
            }
        }
        if (changed) {
            sk::SolveOptions o;
            o.maxIter = 120;
            sk::solve(s, o);
            s.touch();
        }
    }
}

namespace {
// Replaces a parameter name inside expressions (word boundaries only).
void renameInExpr(std::string& e, const std::string& from, const std::string& to) {
    if (e.empty() || from == to) return;
    size_t pos = 0;
    while ((pos = e.find(from, pos)) != std::string::npos) {
        bool lo = pos == 0 || !(std::isalnum((unsigned char)e[pos - 1]) || e[pos - 1] == '_');
        size_t end = pos + from.size();
        bool ro = end >= e.size() || !(std::isalnum((unsigned char)e[end]) || e[end] == '_');
        if (lo && ro) {
            e.replace(pos, from.size(), to);
            pos += to.size();
        } else {
            pos = end;
        }
    }
}
}

void App::drawParamsDialog() {
    if (!paramsOpen_) return;
    const float S = uiScale_;
    ImGui::SetNextWindowSize(ImVec2(560 * S, 380 * S), ImGuiCond_Appearing);
    if (!ImGui::Begin("Parameters", &paramsOpen_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    auto apply = [&](const char* undoLabel) {
        ParamMap test = doc_.params;
        std::string err;
        if (!updateParamValues(test, err)) {
            paramsErr_ = err;
            toast(err, ToastKind::Warning);
            return false;
        }
        pushUndo(undoLabel);
        doc_.params = std::move(test);
        paramsErr_.clear();
        resolveParametricSketches();
        scheduleRecompute(0);
        dirty_ = true;
        return true;
    };

    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted("User Parameters");
    ImGui::PopFont();
    ImGui::SameLine(0, 8 * S);
    ImGui::TextDisabled("mm or degrees. Use these in any dimension or distance.");
    ImGui::Spacing();

    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##pt", 5, tf)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Expression");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Comment");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        std::vector<std::string> names;
        for (auto& [n, p] : doc_.params) names.push_back(n);
        for (auto& n : names) {
            auto it = doc_.params.find(n);
            if (it == doc_.params.end()) continue;
            Param& p = it->second;
            ImGui::PushID(n.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##name", &p.name, ImGuiInputTextFlags_EnterReturnsTrue)) {
                std::string nn = p.name;
                bool valid = !nn.empty() && (bool)std::isalpha((unsigned char)nn[0]);
                for (char c : nn)
                    if (!std::isalnum((unsigned char)c) && c != '_') valid = false;
                if (valid && !doc_.params.count(nn)) {
                    Param np = p;
                    np.name = nn;
                    pushUndo("Rename parameter");
                    doc_.params.erase(n);
                    doc_.params[nn] = np;
                    for (auto& [sid, s] : doc_.sketches)
                        for (auto& [cid, c] : s.cons) renameInExpr(c.expr, n, nn);
                    for (auto& ft : doc_.timeline) {
                        if (ft.extrude) {
                            renameInExpr(ft.extrude->distExpr, n, nn);
                            renameInExpr(ft.extrude->taperExpr, n, nn);
                        }
                        if (ft.revolve) renameInExpr(ft.revolve->angleExpr, n, nn);
                    }
                    resolveParametricSketches();
                    scheduleRecompute(0);
                } else {
                    p.name = n;   // snap back
                    toast("Names start with a letter and must be unique", ToastKind::Warning);
                }
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##expr", &p.expr, ImGuiInputTextFlags_EnterReturnsTrue)) apply("Parameter value");
            if (ImGui::IsItemDeactivatedAfterEdit()) apply("Parameter value");
            ImGui::TableSetColumnIndex(2);
            char buf[32];
            std::snprintf(buf, sizeof buf, "%s", fmtG(p.value).c_str());
            ImGui::TextDisabled("%s", buf);
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##cmt", &p.comment)) dirty_ = true;
            ImGui::TableSetColumnIndex(4);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
            if (ImGui::SmallButton("x")) {
                pushUndo("Delete parameter");
                doc_.params.erase(n);
                resolveParametricSketches();
                scheduleRecompute(0);
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("+ Add Parameter", ImVec2(140 * S, 26 * S))) {
        pushUndo("Add parameter");
        std::string n = nextParamName(doc_.params);
        doc_.params[n] = Param{n, "10", "", 10};
        newParamSel_ = -1;
        scheduleRecompute(0);
    }
    ImGui::SameLine();
    if (!paramsErr_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 60, 50, 255));
        ImGui::TextWrapped("%s", paramsErr_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

void App::syncBodyOverride(uint32_t bodyId) {
    auto it = doc_.bodies.find(bodyId);
    if (it == doc_.bodies.end() || it->second.featureId == 0) return;
    BodyOverride o;
    o.name = it->second.name;
    o.color = it->second.color;
    o.hasColor = true;
    o.mat = it->second.mat;
    o.faceLooks = it->second.faceLooks;
    o.visible = it->second.visible;
    doc_.setOverride(it->second.featureId, doc_.bodySlot(bodyId), o);
}

// ===================================================================== files

std::string App::autosavePath() const { return platform::appDataDir() + "/autosave/recovery.kst"; }

void App::saveDocumentNow() {
    if (doc_.path.empty()) {
        saveDocumentAs();
        return;
    }
    // Nothing changed since the last save: no new version (an older version opened can still be
    // saved as the newest).
    if (!dirty_ && doc_.version > 0 && doc_.version >= latestVersion(doc_.path)) {
        toast("No changes to save (" + doc_.name + " v" + std::to_string(doc_.version) + ")");
        saveStepShapes();   // still keeps any step shapes not stored yet (designs saved before fix1068)
        return;
    }
    // Every Ctrl+S is a new version. The file holds the newest; each version is also kept in the
    // hidden versions folder beside it, so older ones can be opened from the Data panel.
    int prevVersion = doc_.version;
    doc_.version = std::max(doc_.version, latestVersion(doc_.path)) + 1;
    if (saveDocument(doc_, doc_.path)) {
        dirty_ = false;
        std::error_code ec;
        std::filesystem::remove(u8path(autosavePath()), ec);
        std::string vdir = versionsDir(doc_.path);
        std::filesystem::create_directories(u8path(vdir), ec);
#ifdef _WIN32
        platform::setHidden(pathToU8(u8path(vdir).parent_path()));
#endif
        std::filesystem::copy_file(u8path(doc_.path), u8path(vdir + "/v" + std::to_string(doc_.version) + ".kst"),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        toast("Saved " + doc_.name + "  v" + std::to_string(doc_.version), ToastKind::Success);
        saveStepShapes();
        thumbFromDoc(doc_.path);
        settings_.addRecent(doc_.path);
        settings_.save(settingsPath_);
    } else {
        doc_.version = prevVersion;
        toast("Could not write " + doc_.path + " (folder missing or read only?)", ToastKind::Error);
    }
}

// <folder>/.kestral_versions/<design name>
// Every step's shapes go with the design, stored by the step's inputs (so versions share them).
// Opening a version shows exactly these shapes; nothing is rebuilt until you change something.
void App::saveStepShapes() {
    if (doc_.path.empty()) return;
    std::filesystem::path gdir = u8path(versionsDir(doc_.path)) / "geo";
    std::vector<std::pair<std::string, kernel::FeatureCacheData>> todo;
    std::error_code ec;
    for (int i = 0; i < doc_.markerPos && i < (int)doc_.timeline.size(); ++i) {
        const Feature& f = doc_.timeline[i];
        if (f.suppressed || f.failed || f.type == FeatureType::Import || f.type == FeatureType::Sketch || !f.chainSig) continue;
        auto it = stepShapes_.find(f.id);
        if (it == stepShapes_.end()) continue;
        auto d = it->second.find(f.chainSig);
        if (d == it->second.end()) continue;
        char nm[48];
        std::snprintf(nm, sizeof nm, "%u-%016llx.kfc", f.id, (unsigned long long)f.chainSig);
        std::string file = pathToU8(gdir / nm);
        if (std::filesystem::exists(u8path(file), ec)) continue;
        todo.push_back({file, d->second});
    }
    if (todo.empty()) return;
    jobs_->submit("saving shapes", [todo = std::move(todo)] {
        for (auto& [file, d] : todo) kernel::saveStepResult(file, d);
    });
}

void App::rebuildFromSteps() {
    // Builds every step again from its settings, ignoring the saved shapes (they stay on disk).
    doc_.skipSavedShapes = true;
    for (auto& f : doc_.timeline) f.chainSig = 0;
    toast("Rebuilding every step from its settings");
    scheduleRecompute(0);
}

std::string App::versionsDir(const std::string& designPath) const {
    std::filesystem::path p = u8path(designPath);
    return pathToU8(p.parent_path() / ".kestral_versions" / p.stem());
}

int App::latestVersion(const std::string& designPath) const {
    int best = 0;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(u8path(versionsDir(designPath)), ec)) {
        std::string n = pathToU8(e.path().stem());
        if (n.size() > 1 && n[0] == 'v') best = std::max(best, std::atoi(n.c_str() + 1));
    }
    return best;
}

std::vector<int> App::designVersions(const std::string& designPath) const {
    std::vector<int> out;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(u8path(versionsDir(designPath)), ec)) {
        std::string n = pathToU8(e.path().stem());
        if (n.size() > 1 && n[0] == 'v' && pathToU8(e.path().extension()) == ".kst") out.push_back(std::atoi(n.c_str() + 1));
    }
    std::sort(out.rbegin(), out.rend());
    return out;
}

// Opens an older version in place of the design, in the same tab. The tab and the Data panel show
// that version; the next Ctrl+S saves it as the newest version. Unsaved changes are kept in the
// versions folder first so nothing is lost.
void App::openDesignVersion(const std::string& designPath, int v) {
    std::string file = versionsDir(designPath) + "/v" + std::to_string(v) + ".kst";
    auto r = loadDocument(file);
    if (!r.ok) {
        toast("Could not open v" + std::to_string(v) + ": " + r.error, ToastKind::Error);
        return;
    }
    std::string kept;
    if (int t = designTabFor(designPath); t >= 0) {
        if (t != designActive_) switchDesignTab(t);
        if (dirty_) {
            kept = versionsDir(designPath) + "/unsaved changes.kst";
            if (!saveDocument(doc_, kept)) kept.clear();
        }
    } else if (docInUse()) {
        newDesignTab();
    }
    leaveSketchMode();
    sk_.active = false;
    doc_ = std::move(r.doc);
    doc_.path = designPath;
    doc_.name = pathToU8(u8path(designPath).stem());
    doc_.components[doc_.rootId].name = doc_.name;
    doc_.version = v;
    undo_.clear();
    selection_.clear();
    isolated_.clear();
    browserHover_.clear();
    dirty_ = false;
    markSceneDirty();
    rebuildScene();
    scheduleRecompute(0);
    std::string msg = "Opened v" + std::to_string(v) + " of " + doc_.name + ". Ctrl+S saves it as v" +
                      std::to_string(std::max(v, latestVersion(designPath)) + 1) + ".";
    if (!kept.empty()) msg += " Your unsaved changes were kept in " + kept + ".";
    toast(msg, ToastKind::Success);
}

std::string App::designFolder() const {
    // Where Save / Open start: this design's own folder, else the last one used, else
    // the designs folder inside the Kestral folder.
    std::error_code ec;
    if (!doc_.path.empty()) {
        std::string d = pathToU8(u8path(doc_.path).parent_path());
        if (std::filesystem::is_directory(u8path(d), ec)) return d;
    }
    if (!settings_.lastDesignDir.empty() && std::filesystem::is_directory(u8path(settings_.lastDesignDir), ec))
        return settings_.lastDesignDir;
    return platform::designsDir();
}

void App::saveDocumentAs() {
    std::string p = platform::saveFileDialog("Save Design", "Kestral design (*.kst)", "*.kst", "kst", doc_.name + ".kst", designFolder());
    if (p.empty()) return;
    settings_.lastDesignDir = pathToU8(u8path(p).parent_path());
    doc_.path = p;
    doc_.name = pathToU8(u8path(p).stem());
    doc_.components[doc_.rootId].name = doc_.name;
    doc_.touch();
    markSceneDirty();
    saveDocumentNow();
}

void App::openDocumentDialog() {
    std::string p = platform::openFileDialog("Open Design", "Kestral designs (*.kst)", "*.kst", designFolder());
    if (p.empty()) return;
    settings_.lastDesignDir = pathToU8(u8path(p).parent_path());
    openDocumentPath(p);
}

void App::openDocumentPath(const std::string& path) {
    // Already open in a tab: bring it in front. Otherwise it gets its own tab (like Fusion) when the
    // one in front holds a design.
    if (int t = designTabFor(path); t >= 0) { switchDesignTab(t); return; }
    auto r = loadDocument(path);
    if (!r.ok) {
        std::string at = r.line > 1 ? " (line " + std::to_string(r.line) + ")" : "";
        toast("Could not open: " + r.error + at, ToastKind::Error);
        return;
    }
    if (docInUse()) newDesignTab();
    leaveSketchMode();
    sk_.active = false;
    doc_ = std::move(r.doc);
    doc_.path = path;
    doc_.name = pathToU8(u8path(path).stem());
    doc_.components[doc_.rootId].name = doc_.name;
    undo_.clear();
    selection_.clear();
    isolated_.clear();
    browserHover_.clear();
    dirty_ = false;
    markSceneDirty();
    rebuildScene();
    float a = vk_.extent.height ? (float)vk_.extent.width / (float)vk_.extent.height : 1.f;
    cam_.home(sceneBounds_, a);
    settings_.addRecent(path);
    settings_.save(settingsPath_);
    thumbAfterRebuild_ = path;   // refresh its preview once the geometry is rebuilt
    toast("Opened " + doc_.name + ". Geometry is rebuilding in the background.", ToastKind::Success);
    scheduleRecompute(0);
}

void App::checkRecovery() {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string newest;
    fs::file_time_type bestT = fs::file_time_type::min();
    for (const char* name : {"autosave/recovery.kst", "autosave/crash.kst"}) {
        std::string p = platform::appDataDir() + "/" + name;
        if (!fs::exists(u8path(p), ec)) continue;
        auto t = fs::last_write_time(u8path(p), ec);
        if (ec) continue;
        if (t > bestT) { bestT = t; newest = p; }
    }
    if (newest.empty()) return;
    std::string clean = platform::appDataDir() + "/clean_exit";
    if (fs::exists(u8path(clean), ec)) {
        auto t = fs::last_write_time(u8path(clean), ec);
        if (!ec && t >= bestT) return;   // nothing newer than the last clean exit
    }
    recoveryFile_ = newest;
}

void App::updateAutosave() {
    if (modelTest_ || uiTest_) return;   // a self test must not overwrite the recovery file
    if (!dirty_) {
        lastSaveActivity_ = time_;
        return;
    }
    if (settings_.autosaveSec <= 0 || autosaveBusy_) return;
    if (time_ - lastSaveActivity_ < std::max(5.f, settings_.autosaveSec)) return;
    lastSaveActivity_ = time_;
    // Serialize here (a fraction of a millisecond, no geometry), write on a worker.
    std::string data;
    if (!serializeDocument(doc_, data)) return;
    std::string rec = autosavePath();
    // Mid command (a dialog open) only the recovery copy is written: your file gets finished work.
    std::string own = settings_.autosaveToFile && !doc_.path.empty() && !dlg_.open && !tdlg_.open ? doc_.path : std::string();
    uint64_t rev = doc_.revision;
    autosaveBusy_ = true;
    JobSystem* jobs = jobs_.get();
    jobs_->submit("autosave", [this, jobs, data, rec, own, rev] {
        std::error_code ec;
        std::filesystem::create_directories(u8path(rec).parent_path(), ec);
        bool okRec = writeFileAtomic(rec, data);
        bool okOwn = own.empty() || writeFileAtomic(own, data);
        jobs->post([this, okRec, okOwn, own, rev] {
            autosaveBusy_ = false;
            if (!own.empty() && okOwn && doc_.revision == rev) {
                dirty_ = false;   // the file on disk now matches what you see
                lastAutosaveTime_ = time_;
            }
            if (!okOwn) toast("Autosave could not write " + own + ". Your work is still in the recovery copy.", ToastKind::Warning);
            else if (!okRec) log::warn("Recovery autosave failed");
        });
    }, [this](const std::string& err) {
        autosaveBusy_ = false;
        log::warn("Autosave failed: " + err);
    });
}

void App::drawRecoveryPrompt() {
    if (recoveryFile_.empty()) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.35f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Recovered work", nullptr,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    std::error_code ec;
    auto t = std::filesystem::last_write_time(u8path(recoveryFile_), ec);
    std::time_t tt = ec ? std::time(nullptr) : std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(t));
    char when[64] = "unknown time";
    if (!ec) {
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        std::strftime(when, sizeof when, "%H:%M", &tmv);
    }
    ImGui::Text("Kestral found autosaved work from %s.\nRestore it?", when);
    ImGui::Spacing();
    float bw = 110 * S;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.29f, 0.63f, 0.36f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("Restore", ImVec2(bw, 28 * S))) {
        std::string p = recoveryFile_;
        recoveryFile_.clear();
        openDocumentPath(p);
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(bw, 28 * S))) {
        std::error_code ec2;
        std::filesystem::remove(u8path(recoveryFile_), ec2);
        recoveryFile_.clear();
    }
    ImGui::End();
}

} // namespace kst
