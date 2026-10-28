#include "core/SaveLoad.h"
#include "core/Log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace kst {
namespace {

std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '|') o += "\\p";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        char n = s[++i];
        if (n == 'p') o += '|';
        else if (n == 'n') o += '\n';
        else if (n == '\\') o += '\\';
        else o += n;
    }
    return o;
}

std::string num(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.17g", v);
    return b;
}

// Splits on unescaped '|'.
std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) { cur += s[i]; cur += s[i + 1]; ++i; continue; }
        if (s[i] == '|') { out.push_back(cur); cur.clear(); continue; }
        cur += s[i];
    }
    out.push_back(cur);
    return out;
}

const char* typeName(FeatureType t) {
    switch (t) {
    case FeatureType::Import: return "import";
    case FeatureType::Sketch: return "sketch";
    case FeatureType::Extrude: return "extrude";
    case FeatureType::Revolve: return "revolve";
    case FeatureType::Fillet: return "fillet";
    case FeatureType::Chamfer: return "chamfer";
    case FeatureType::Shell: return "shell";
    case FeatureType::PressPull: return "presspull";
    case FeatureType::DeleteFace: return "deleteface";
    case FeatureType::Pattern: return "pattern";
    case FeatureType::Mirror: return "mirror";
    case FeatureType::Combine: return "combine";
    case FeatureType::Split: return "split";
    case FeatureType::SplitFace: return "splitface";
    case FeatureType::Hole: return "hole";
    case FeatureType::Sweep: return "sweep";
    case FeatureType::Loft: return "loft";
    case FeatureType::Move: return "move";
    case FeatureType::MeshConvert: return "meshconv";
    case FeatureType::ConstructPlane: return "cplane";
    case FeatureType::Remove: return "remove";
    case FeatureType::Gear: return "gear";
    case FeatureType::Thread: return "thread";
    case FeatureType::Wing: return "wing";
    case FeatureType::Dovetail: return "dovetail";
    case FeatureType::Blade: return "blade";
    case FeatureType::Align: return "align";
    case FeatureType::Draft: return "draft";
    case FeatureType::Coil: return "coil";
    case FeatureType::Pipe: return "pipe";
    case FeatureType::ReplaceFace: return "replaceface";
    case FeatureType::ConstructAxis: return "caxis";
    case FeatureType::ConstructPoint: return "cpoint";
    case FeatureType::Rib: return "rib";
    case FeatureType::Web: return "web";
    case FeatureType::Emboss: return "emboss";
    case FeatureType::Boss: return "boss";
    case FeatureType::Snap: return "snap";
    case FeatureType::Rest: return "rest";
    case FeatureType::Lip: return "lip";
    case FeatureType::Silhouette: return "silhouette";
    case FeatureType::Joint: return "joint";
    case FeatureType::AsBuilt: return "asbuilt";
    case FeatureType::Rigid: return "rigid";
    case FeatureType::Thicken: return "thicken";
    default: return "other";
    }
}

std::string joinEF(const std::vector<std::pair<uint32_t, uint32_t>>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += (i ? "," : "") + std::to_string(v[i].first) + "." + std::to_string(v[i].second);
    return s;
}

std::vector<std::pair<uint32_t, uint32_t>> parseEF(const std::string& str) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    std::stringstream ss(str);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        auto dot = tok.find('.');
        if (dot == std::string::npos) continue;
        try {
            out.push_back({(uint32_t)std::stoul(tok.substr(0, dot)), (uint32_t)std::stoul(tok.substr(dot + 1))});
        } catch (...) {}
    }
    return out;
}

std::string joinRefs(const std::vector<TargetRef>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += (i ? "," : "") + std::to_string(v[i].first) + ":" + std::to_string(v[i].second);
    return s;
}

std::string joinPts(const std::vector<sk::Vec2>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ";" : "") + num(v[i].x) + "," + num(v[i].y);
    return s;
}

std::vector<sk::Vec2> parsePts(const std::string& str) {
    std::vector<sk::Vec2> out;
    std::stringstream ss(str);
    std::string tok;
    while (std::getline(ss, tok, ';')) {
        auto c = tok.find(',');
        if (c == std::string::npos) continue;
        try { out.push_back({std::stod(tok.substr(0, c)), std::stod(tok.substr(c + 1))}); } catch (...) {}
    }
    return out;
}

std::string joinFaces(const std::vector<FaceRef>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += (i ? ";" : "") + std::to_string(v[i].feature) + ":" + std::to_string(v[i].slot) + ":" +
             std::to_string(v[i].face) + ":" + std::to_string(v[i].faceTotal);
    return s;
}

std::vector<FaceRef> parseFaces(const std::string& str) {
    std::vector<FaceRef> out;
    std::stringstream ss(str);
    std::string tok;
    while (std::getline(ss, tok, ';')) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (std::sscanf(tok.c_str(), "%u:%u:%u:%u", &a, &b, &c, &d) >= 3) out.push_back({a, b, c, d});
    }
    return out;
}

std::vector<uint32_t> parseIds(const std::string& str) {
    std::vector<uint32_t> out;
    std::stringstream ss(str);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        try {
            out.push_back((uint32_t)std::stoul(tok));
        } catch (...) {}
    }
    return out;
}

std::vector<TargetRef> parseRefs(const std::string& s) {
    std::vector<TargetRef> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        auto colon = tok.find(':');
        if (colon == std::string::npos) continue;
        try {
            out.push_back({(uint32_t)std::stoul(tok.substr(0, colon)), (uint32_t)std::stoul(tok.substr(colon + 1))});
        } catch (...) {}
    }
    return out;
}

} // namespace

bool writeFileAtomic(const std::string& path, const std::string& data) {
    // Write a temp file next to the target, then swap it in. A crash or power cut mid save leaves
    // the old file untouched, never a half written one.
    std::string tmp = path + ".saving";
    try {
        {
            std::ofstream f(u8path(tmp), std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f.write(data.data(), (std::streamsize)data.size());
            f.flush();
            if (!f) {
                f.close();
                std::error_code ec;
                std::filesystem::remove(u8path(tmp), ec);
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(u8path(tmp), u8path(path), ec);
        if (ec) {
            // Some sync tools hold the target open: fall back to a copy over it.
            std::filesystem::copy_file(u8path(tmp), u8path(path), std::filesystem::copy_options::overwrite_existing, ec);
            std::error_code ec2;
            std::filesystem::remove(u8path(tmp), ec2);
            return !ec;
        }
        return true;
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(u8path(tmp), ec);
        return false;
    }
}

bool saveDocument(const Document& doc, const std::string& path) {
    std::string data;
    if (!serializeDocument(doc, data)) return false;
    return writeFileAtomic(path, data);
}

bool serializeDocument(const Document& doc, std::string& out) {
    try {
        std::ostringstream f;
        f << "KestrelDoc 1\n";
        f << "doc|" << esc(doc.name) << "\n";
        f << "ver|" << doc.version << "\n";
        f << "marker|" << doc.markerPos << "\n";
        f << "sec|" << (doc.section.on ? 1 : 0) << "|" << doc.section.axis << "|"
          << num(doc.section.offset) << "|" << (doc.section.flip ? 1 : 0) << "\n";
        f << "nextid|" << 0 << "\n";   // ids are file local, refreshed on load
        for (auto& [name, p] : doc.params)
            f << "param|" << esc(name) << "|" << esc(p.expr) << "|" << esc(p.comment) << "\n";
        for (auto& [sid, s] : doc.sketches) {
            f << "sketch|" << sid << "|" << s.featureId << "|" << esc(s.name) << "|" << (s.visible ? 1 : 0)
              << "|" << num(s.plane.o.x) << " " << num(s.plane.o.y) << " " << num(s.plane.o.z)
              << "|" << num(s.plane.x.x) << " " << num(s.plane.x.y) << " " << num(s.plane.x.z)
              << "|" << num(s.plane.y.x) << " " << num(s.plane.y.y) << " " << num(s.plane.y.z)
              << "|" << esc(s.plane.name) << "\n";
            for (auto& [id, e] : s.ents) {
                if (id < sk::Sketch::kFirstUser) continue;   // origin + axes are implicit
                f << "ent|" << sid << "|" << id << "|" << (int)e.type
                  << "|" << num(e.x) << "|" << num(e.y) << "|" << num(e.r)
                  << "|" << e.a << "|" << e.b << "|" << e.c
                  << "|" << (e.construction ? 1 : 0);
                if (e.type == sk::EntType::Spline || e.type == sk::EntType::CtrlSpline) {
                    f << "|" << (e.loop ? 1 : 0) << "|";
                    for (size_t i = 0; i < e.pts.size(); ++i) f << (i ? "," : "") << e.pts[i];
                }
                f << "\n";
            }
            for (auto& n : s.notes) {
                f << "stxt|" << sid << "|" << n.id << "|" << esc(n.text) << "|" << esc(n.font) << "|"
                  << num(n.height) << "|" << num(n.origin.x) << "|" << num(n.origin.y) << "|"
                  << (n.construction ? 1 : 0) << "|";
                for (size_t i = 0; i < n.ents.size(); ++i) f << (i ? "," : "") << n.ents[i];
                f << "\n";
            }
            for (auto& [id, c] : s.cons)
                f << "con|" << sid << "|" << id << "|" << (int)c.type
                  << "|" << c.e[0] << "|" << c.e[1] << "|" << c.e[2]
                  << "|" << num(c.value) << "|" << num(c.value2)
                  << "|" << esc(c.name) << "|" << num(c.label.x) << "|" << num(c.label.y)
                  << "|" << (c.labelSet ? 1 : 0) << "|" << esc(c.expr) << "|" << (c.driven ? 1 : 0) << "\n";
            f << "sketchend|" << sid << "|" << s.nextId << "\n";
        }
        for (auto& ft : doc.timeline) {
            f << "feat|" << ft.id << "|" << typeName(ft.type) << "|" << esc(ft.name)
              << "|" << (ft.suppressed ? 1 : 0) << "|" << ft.sketchId << "|" << esc(ft.importPath) << "\n";
            if (ft.type == FeatureType::Extrude && ft.extrude)
                f << "xpar|" << ft.id << "|" << ft.extrude->sketchId << "|" << esc(ft.extrude->distExpr) << "|"
                  << esc(ft.extrude->taperExpr) << "|" << ft.extrude->op << "|" << (ft.extrude->flip ? 1 : 0)
                  << "|" << (ft.extrude->symmetric ? 1 : 0) << "|" << joinRefs(ft.extrude->targets)
                  << "|" << joinPts(ft.extrude->profilePts) << "|" << joinFaces(ft.extrude->faces)
                  << "|" << ft.extrude->extent << "|" << ft.extrude->to.feature << "|" << ft.extrude->to.slot
                  << "|" << ft.extrude->to.face << "|" << ft.extrude->to.faceTotal
                  << "|" << esc(ft.extrude->thinExpr) << "|" << ft.extrude->thinSide << "\n";
            if (ft.type == FeatureType::Revolve && ft.revolve)
                f << "rpar|" << ft.id << "|" << ft.revolve->sketchId << "|" << esc(ft.revolve->angleExpr) << "|"
                  << ft.revolve->axis << "|" << ft.revolve->axisEnt << "|" << ft.revolve->op << "|"
                  << (ft.revolve->flip ? 1 : 0) << "|" << joinRefs(ft.revolve->targets) << "|" << joinPts(ft.revolve->profilePts)
                  << "|" << esc(ft.revolve->thinExpr) << "|" << ft.revolve->thinSide << "\n";
            if ((ft.type == FeatureType::Fillet || ft.type == FeatureType::Chamfer || ft.type == FeatureType::Shell ||
                 ft.type == FeatureType::PressPull || ft.type == FeatureType::MeshConvert || ft.type == FeatureType::DeleteFace ||
                 ft.type == FeatureType::Draft || ft.type == FeatureType::ReplaceFace || ft.type == FeatureType::Thicken) && ft.modify)
                f << "mpar|" << ft.id << "|" << ft.modify->kind << "|" << esc(ft.modify->valueExpr) << "|"
                  << ft.modify->edgeTotal << "|" << ft.modify->faceTotal << "|" << joinRefs(ft.modify->targets) << "|"
                  << joinEF(ft.modify->edges) << "|" << joinEF(ft.modify->faces) << "|" << ft.modify->axis
                  << "|" << ft.modify->side << "|" << esc(ft.modify->endExpr) << "\n";
            if ((ft.type == FeatureType::Coil || ft.type == FeatureType::Pipe) && ft.pathSolid) {
                const SweepSolidParams& s = *ft.pathSolid;
                f << "solid|" << ft.id << "|" << s.axis << "|" << (s.leftHand ? 1 : 0) << "|" << s.pathSketch << "|"
                  << esc(s.diaExpr) << "|" << esc(s.heightExpr) << "|" << esc(s.turnsExpr) << "|" << esc(s.sectionExpr) << "|"
                  << esc(s.innerExpr) << "|" << esc(s.xExpr) << "|" << esc(s.yExpr) << "|" << esc(s.zExpr) << "\n";
            }
            if ((ft.type == FeatureType::Rib && ft.rib) || (ft.type == FeatureType::Web && ft.web)) {
                const RibParams& r = ft.type == FeatureType::Web ? *ft.web : *ft.rib;
                f << (ft.type == FeatureType::Web ? "webpar|" : "ribpar|") << ft.id << "|" << r.sketchId << "|" << esc(r.thickExpr) << "|"
                  << (r.flip ? 1 : 0) << "|" << r.side << "|" << joinRefs(r.targets) << "\n";
            }
            if (ft.type == FeatureType::Emboss && ft.emboss) {
                const EmbossParams& e = *ft.emboss;
                f << "embpar|" << ft.id << "|" << e.sketchId << "|" << esc(e.depthExpr) << "|"
                  << (e.deboss ? 1 : 0) << "|" << (e.flip ? 1 : 0) << "|" << joinRefs(e.targets) << "\n";
            }
            if (ft.type == FeatureType::Boss && ft.boss) {
                const BossParams& b = *ft.boss;
                f << "bospar|" << ft.id << "|" << esc(b.xExpr) << "|" << esc(b.yExpr) << "|" << esc(b.zExpr) << "|"
                  << esc(b.diaExpr) << "|" << esc(b.heightExpr) << "|" << esc(b.holeExpr) << "|"
                  << b.axis << "|" << joinRefs(b.targets) << "\n";
            }
            if (ft.type == FeatureType::Snap && ft.snap) {
                const SnapParams& s = *ft.snap;
                f << "snppar|" << ft.id << "|" << esc(s.xExpr) << "|" << esc(s.yExpr) << "|" << esc(s.zExpr) << "|"
                  << esc(s.lengthExpr) << "|" << esc(s.widthExpr) << "|" << esc(s.thickExpr) << "|"
                  << esc(s.hookExpr) << "|" << esc(s.hookLenExpr) << "|" << s.axis << "|" << joinRefs(s.targets) << "\n";
            }
            if (ft.type == FeatureType::Rest && ft.rest) {
                const RestParams& r = *ft.rest;
                f << "rstpar|" << ft.id << "|" << r.sketchId << "|" << esc(r.thickExpr) << "|"
                  << (r.flip ? 1 : 0) << "|" << joinRefs(r.targets) << "\n";
            }
            if (ft.type == FeatureType::Lip && ft.lip) {
                const LipParams& lip = *ft.lip;
                f << "lippar|" << ft.id << "|" << lip.sketchId << "|" << esc(lip.widthExpr) << "|"
                  << esc(lip.heightExpr) << "|" << (lip.flip ? 1 : 0) << "|" << (lip.groove ? 1 : 0) << "|"
                  << joinRefs(lip.targets) << "\n";
            }
            if (ft.type == FeatureType::Silhouette && ft.silhouette) {
                const SilhouetteParams& s = *ft.silhouette;
                f << "silpar|" << ft.id << "|" << s.axis << "|" << joinRefs(s.targets) << "\n";
            }
            if ((ft.type == FeatureType::Joint && ft.joint) || (ft.type == FeatureType::AsBuilt && ft.asbuilt)) {
                const JointParams& j = ft.type == FeatureType::AsBuilt ? *ft.asbuilt : *ft.joint;
                f << (ft.type == FeatureType::AsBuilt ? "abjpar|" : "jntpar|") << ft.id << "|" << j.kind << "|"
                  << esc(j.gx) << "|" << esc(j.gy) << "|" << esc(j.gz) << "|"
                  << esc(j.mx) << "|" << esc(j.my) << "|" << esc(j.mz) << "|"
                  << j.ground.first << "." << j.ground.second << "|" << j.move.first << "." << j.move.second << "|"
                  << esc(j.angleExpr) << "|" << j.axis << "|" << esc(j.slideExpr) << "\n";
            }
            if (ft.type == FeatureType::Rigid && ft.rigid)
                f << "rgpar|" << ft.id << "|" << joinRefs(ft.rigid->members) << "\n";
            if (ft.type == FeatureType::Pattern && ft.pattern) {
                const PatternParams& q = *ft.pattern;
                int mode = q.onPath ? 2 : q.circular ? 1 : 0;
                f << "ppar|" << ft.id << "|" << mode << "|" << esc(q.countExpr) << "|"
                  << esc(q.spacingExpr) << "|" << esc(q.countYExpr) << "|" << esc(q.spacingYExpr)
                  << "|" << q.axis << "|" << esc(q.angleExpr) << "|" << joinRefs(q.targets)
                  << "|" << q.axisO.x << "|" << q.axisO.y << "|" << q.axisO.z
                  << "|" << q.axisD.x << "|" << q.axisD.y << "|" << q.axisD.z
                  << "|" << q.pathSketch << "|" << q.pathEnt << "|" << q.distType << "|" << q.orient
                  << "|" << (q.flip ? 1 : 0) << "|" << q.pathBody.first << "|" << q.pathBody.second
                  << "|" << q.pathEdge << "\n";
            }
            if (ft.type == FeatureType::Mirror && ft.mirror)
                f << "mpar2|" << ft.id << "|" << ft.mirror->plane << "|" << joinRefs(ft.mirror->targets) << "|"
                  << num(ft.mirror->planeO.x) << " " << num(ft.mirror->planeO.y) << " " << num(ft.mirror->planeO.z) << "|"
                  << num(ft.mirror->planeN.x) << " " << num(ft.mirror->planeN.y) << " " << num(ft.mirror->planeN.z) << "|"
                  << esc(ft.mirror->planeName) << "\n";
            if (ft.type == FeatureType::Combine && ft.combine)
                f << "cpar|" << ft.id << "|" << ft.combine->op << "|" << ft.combine->tool.first << ":" << ft.combine->tool.second
                  << "|" << joinRefs(ft.combine->targets) << "|" << joinRefs(ft.combine->tools) << "|" << (ft.combine->keepTools ? 1 : 0) << "\n";
            if (ft.type == FeatureType::SplitFace && ft.splitFace) {
                std::string faces;
                for (size_t i = 0; i < ft.splitFace->faces.size(); ++i)
                    faces += (i ? "," : "") + std::to_string(ft.splitFace->faces[i]);
                f << "sfpar|" << ft.id << "|" << ft.splitFace->plane << "|" << esc(ft.splitFace->offsetExpr) << "|"
                  << joinRefs(ft.splitFace->targets) << "|" << ft.splitFace->faceTotal << "|" << faces << "|"
                  << ft.splitFace->sketchId << "\n";
            }
            if (ft.type == FeatureType::Split && ft.split)
                f << "spar|" << ft.id << "|" << ft.split->plane << "|" << joinRefs(ft.split->targets) << "|"
                  << num(ft.split->toolO.x) << "," << num(ft.split->toolO.y) << "," << num(ft.split->toolO.z) << "|"
                  << num(ft.split->toolN.x) << "," << num(ft.split->toolN.y) << "," << num(ft.split->toolN.z) << "|"
                  << esc(ft.split->toolName) << "|" << ft.split->sketchId << "|" << ft.split->sketchEnt << "\n";
            if (ft.type == FeatureType::Align && ft.align) {
                const AlignParams& a = *ft.align;
                auto pk = [](const AlignParams::Pick& q) {
                    return std::to_string(q.kind) + "|" + std::to_string(q.ref.first) + "|" + std::to_string(q.ref.second) + "|" +
                           std::to_string(q.sub) + "|" + std::to_string(q.total) + "|" + std::to_string(q.end);
                };
                f << "algn|" << ft.id << "|" << (a.flip ? 1 : 0) << "|" << pk(a.from) << "|" << pk(a.to) << "|"
                  << esc(a.fromName) << "|" << esc(a.toName) << "|" << joinRefs(a.targets) << "\n";
            }
            if (ft.type == FeatureType::Hole && ft.hole)
            {
                f << "hpar|" << ft.id << "|" << ft.hole->sketchId << "|" << esc(ft.hole->diaExpr) << "|"
                  << esc(ft.hole->depthExpr) << "|" << joinRefs(ft.hole->targets) << "\n";
                // Placed holes and hole types (older versions just skip this line).
                std::ostringstream pts;
                pts.precision(17);
                for (auto& hp : ft.hole->points)
                    pts << (pts.tellp() > 0 ? ";" : "") << hp.pos.x << "," << hp.pos.y << "," << hp.pos.z << "," << hp.dir.x << ","
                        << hp.dir.y << "," << hp.dir.z;
                f << "hpt|" << ft.id << "|" << ft.hole->kind << "|" << esc(ft.hole->cbDiaExpr) << "|" << esc(ft.hole->cbDepthExpr) << "|"
                  << esc(ft.hole->csDiaExpr) << "|" << esc(ft.hole->csAngleExpr) << "|" << pts.str() << "|"
                  << esc(ft.hole->spotDiaExpr) << "|" << esc(ft.hole->spotDepthExpr) << "|" << esc(ft.hole->drillExpr) << "|"
                  << (ft.hole->tapped ? 1 : 0) << "|" << esc(ft.hole->pitchExpr) << "\n";
            }
            if ((ft.type == FeatureType::Sweep || ft.type == FeatureType::Loft) && ft.multi) {
                std::string ids;
                for (size_t i = 0; i < ft.multi->sketchIds.size(); ++i)
                    ids += (i ? "," : "") + std::to_string(ft.multi->sketchIds[i]);
                std::string rails;
                for (size_t i = 0; i < ft.multi->railIds.size(); ++i)
                    rails += (i ? "," : "") + std::to_string(ft.multi->railIds[i]);
                f << "msk|" << ft.id << "|" << ids << "|" << ft.multi->op << "|" << joinRefs(ft.multi->targets)
                  << "|" << esc(ft.multi->twistExpr) << "|" << rails
                  << "|" << esc(ft.multi->thinExpr) << "|" << ft.multi->thinSide
                  << "|" << ft.multi->pointEnd << "|" << esc(ft.multi->pointX) << "|" << esc(ft.multi->pointY)
                  << "|" << esc(ft.multi->pointZ) << "|" << ft.multi->centerId << "\n";
            }
            if (ft.type == FeatureType::Move && ft.move) {
                const MoveParams& m = *ft.move;
                f << "mvp|" << ft.id << "|" << esc(m.xExpr) << "|" << esc(m.yExpr) << "|"
                  << esc(m.zExpr) << "|" << (m.copy ? 1 : 0) << "|" << joinRefs(m.targets) << "|"
                  << esc(m.rxExpr) << "|" << esc(m.ryExpr) << "|" << esc(m.rzExpr) << "|"
                  << esc(m.angExpr) << "|" << esc(m.axExpr) << "|" << esc(m.ayExpr) << "|" << esc(m.azExpr) << "|"
                  << esc(m.pxExpr) << "|" << esc(m.pyExpr) << "|" << esc(m.pzExpr) << "|"
                  << esc(m.sxExpr) << "|" << esc(m.syExpr) << "|" << esc(m.szExpr) << "|"
                  << (m.hasMat ? 1 : 0);
                for (int i = 0; i < 16; ++i) f << "|" << m.mat[i];
                f << "|" << m.op << "\n";
            }
            if (ft.type == FeatureType::Remove && ft.remove)
                f << "rmv|" << ft.id << "|" << joinRefs(ft.remove->targets) << "\n";
            if (ft.type == FeatureType::ConstructPlane && ft.cplane)
                f << "cpl|" << ft.id << "|" << ft.cplane->base << "|" << esc(ft.cplane->offsetExpr)
                  << "|" << ft.cplane->kind << "|" << ft.cplane->axis << "|" << ft.cplane->refPlane
                  << "|" << esc(ft.cplane->x1) << "|" << esc(ft.cplane->y1) << "|" << esc(ft.cplane->z1)
                  << "|" << esc(ft.cplane->x2) << "|" << esc(ft.cplane->y2) << "|" << esc(ft.cplane->z2)
                  << "|" << esc(ft.cplane->x3) << "|" << esc(ft.cplane->y3) << "|" << esc(ft.cplane->z3)
                  << "|" << ft.cplane->faceRef.first << ":" << ft.cplane->faceRef.second
                  << "|" << ft.cplane->faceSub << "|" << ft.cplane->faceTotal
                  << "|" << esc(ft.cplane->angleExpr) << "\n";
            if (ft.type == FeatureType::ConstructAxis && ft.caxis) {
                const ConstructAxisParams& a = *ft.caxis;
                f << "cax|" << ft.id << "|" << esc(a.ax) << "|" << esc(a.ay) << "|" << esc(a.az) << "|"
                  << esc(a.bx) << "|" << esc(a.by) << "|" << esc(a.bz) << "|" << a.kind << "|"
                  << a.ref.first << ":" << a.ref.second << "|" << a.sub << "|" << a.subTotal << "\n";
            }
            if (ft.type == FeatureType::ConstructPoint && ft.cpoint) {
                const ConstructPointParams& p = *ft.cpoint;
                f << "cpt|" << ft.id << "|" << esc(p.x) << "|" << esc(p.y) << "|" << esc(p.z) << "|"
                  << p.kind << "|" << p.ref.first << ":" << p.ref.second << "|" << p.sub << "|" << p.subTotal << "\n";
            }
            if (ft.type == FeatureType::Gear && ft.gear) {
                std::string kv;
                for (auto& [k, v] : ft.gear->vals) kv += (kv.empty() ? "" : ";") + k + "=" + v;
                f << "gearp|" << ft.id << "|" << ft.gear->type << "|" << ft.gear->plane << "|" << ft.gear->sizeBy << "|"
                  << (ft.gear->leftHand ? 1 : 0) << "|" << esc(kv) << "\n";
            }
            if (ft.type == FeatureType::Blade && ft.blade) {
                const BladeParams& b = *ft.blade;
                std::string kv;
                for (auto& [k, v] : b.vals) kv += (kv.empty() ? "" : ";") + k + "=" + v;
                f << "bladep|" << ft.id << "|" << b.kind << "|" << b.plane << "|" << (b.leftHand ? 1 : 0) << "|" << esc(b.airfoil) << "|"
                  << esc(kv) << "\n";
            }
            if (ft.type == FeatureType::Dovetail && ft.dovetail) {
                const DovetailParams& d = *ft.dovetail;
                std::string kv;
                for (auto& [k, v] : d.vals) kv += (kv.empty() ? "" : ";") + k + "=" + v;
                f << "dovp|" << ft.id << "|" << d.type << "|" << d.plane << "|" << joinRefs(d.targets) << "|" << esc(kv) << "\n";
            }
            if (ft.type == FeatureType::Wing && ft.wing) {
                const WingParams& w = *ft.wing;
                std::string kv;
                for (auto& [k, v] : w.vals) kv += (kv.empty() ? "" : ";") + k + "=" + v;
                f << "wingp|" << ft.id << "|" << (w.mirror ? 1 : 0) << "|" << (w.hollow ? 1 : 0) << "|" << esc(w.rootAirfoil) << "|"
                  << esc(kv) << "\n";
                for (auto& p : w.panels)
                    f << "wingpn|" << ft.id << "|" << esc(p.span) << "|" << esc(p.tipChord) << "|" << esc(p.sweep) << "|"
                      << esc(p.dihedral) << "|" << esc(p.twist) << "|" << esc(p.airfoil) << "\n";
            }
            if (ft.type == FeatureType::Thread && ft.thread) {
                const ThreadParams& t = *ft.thread;
                f << "thrp|" << ft.id << "|" << t.face.feature << "|" << t.face.slot << "|" << t.face.face << "|" << t.face.faceTotal
                  << "|" << t.family << "|" << t.size << "|" << t.side << "|" << (t.modeled ? 1 : 0) << "|" << (t.leftHand ? 1 : 0)
                  << "|" << (t.flip ? 1 : 0) << "|" << (t.fullLength ? 1 : 0) << "|" << esc(t.lengthExpr) << "|" << esc(t.offsetExpr)
                  << "|" << esc(t.startsExpr) << "|" << esc(t.diaExpr) << "|" << esc(t.pitchExpr) << "|" << num(t.axisO.x) << " "
                  << num(t.axisO.y) << " " << num(t.axisO.z) << "|" << num(t.axisD.x) << " " << num(t.axisD.y) << " " << num(t.axisD.z)
                  << "|" << num(t.radius) << "|" << num(t.faceLen) << "|" << (t.hole ? 1 : 0) << "\n";
            }
        }
        for (auto& [cid, c] : doc.components) {
            if (cid == doc.rootId) continue;
            f << "comp|" << cid << "|" << c.parentId << "|" << esc(c.name) << "|" << (c.visible ? 1 : 0)
              << "|" << c.featureId << "|" << (c.grounded ? 1 : 0) << "\n";
        }
        if (!doc.robot.empty()) {
            const RobotDef& r = doc.robot;
            f << "robot|" << esc(r.name) << "|" << esc(r.base) << "|" << num(r.density) << "|" << r.front << "\n";
            for (auto& [cp, g] : r.massG) f << "rmass|" << esc(cp) << "|" << num(g) << "\n";
            for (auto& j : r.joints)
                f << "rjoint|" << esc(j.name) << "|" << esc(j.parent) << "|" << esc(j.child) << "|" << j.type << "|" << num(j.origin.x) << " "
                  << num(j.origin.y) << " " << num(j.origin.z) << "|" << num(j.axis.x) << " " << num(j.axis.y) << " " << num(j.axis.z)
                  << "|" << num(j.lower) << "|" << num(j.upper) << "|" << num(j.effort) << "|" << num(j.velocity) << "|"
                  << num(j.stiffness) << "|" << num(j.damping) << "\n";
        }
        for (auto& [key, nm] : doc.compNames)
            f << "cname|" << (uint32_t)(key >> 32) << "|" << (uint32_t)key << "|" << esc(nm) << "\n";
        for (auto key : doc.compHidden)
            f << "chid|" << (uint32_t)(key >> 32) << "|" << (uint32_t)key << "\n";
        for (auto pid : doc.hiddenPlanes) f << "phid|" << pid << "\n";
        for (auto key : doc.autoHidden) f << "skah2|" << (uint32_t)(key >> 32) << "|" << (uint32_t)key << "\n";
        for (auto& [fid, cid] : doc.featureFolder)
            f << "ffold|" << fid << "|" << cid << "\n";
        if (doc.activeComponent && doc.activeComponent != doc.rootId)
            f << "cact|" << doc.activeComponent << "\n";
        for (auto& [key, o] : doc.overrides) {
            uint32_t fid = (uint32_t)(key >> 32), slot = (uint32_t)key;
            f << "override|" << fid << "|" << slot << "|" << esc(o.name) << "|" << (o.hasColor ? 1 : 0)
              << "|" << num(o.color.r) << " " << num(o.color.g) << " " << num(o.color.b)
              << "|" << (o.visible ? 1 : 0) << "\n";
            auto props = [&](const MatProps& m) {
                return num(m.opacity) + " " + num(m.metal) + " " + num(m.rough) + " " + std::to_string((int)m.pattern) + " " +
                       num(m.color2.r) + " " + num(m.color2.g) + " " + num(m.color2.b) + " " + num(m.scale) + " " + num(m.glow) + " " +
                       (m.fileColors ? "1" : "0");
            };
            if (o.hasColor && !(o.mat == MatProps{}))
                f << "look|" << fid << "|" << slot << "|" << props(o.mat) << "|" << esc(o.mat.name) << "\n";
            for (auto& [face, fl] : o.faceLooks)
                f << "flook|" << fid << "|" << slot << "|" << face << "|" << num(fl.color.r) << " " << num(fl.color.g) << " "
                  << num(fl.color.b) << "|" << props(fl.mat) << "|" << esc(fl.mat.name) << "\n";
        }
        out = f.str();
        return true;
    } catch (...) {
        return false;
    }
}

LoadResult loadDocument(const std::string& path) {
    LoadResult r;
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) {
        r.error = "Could not open file";
        return r;
    }
    Document& doc = r.doc;
    std::string header;
    std::getline(f, header);
    if (header != "KestrelDoc 1") {
        r.error = "Not a Kestral file (or newer version)";
        return r;
    }
    std::string line;
    int ln = 1;
    sk::Sketch* curSketch = nullptr;
    auto bad = [&](const std::string& why) {
        r.ok = false;
        r.error = why;
        r.line = ln;
        return;
    };
    auto nums = [](const std::string& s) {
        std::vector<double> out;
        std::stringstream ss(s);
        double v;
        while (ss >> v) out.push_back(v);
        return out;
    };
    while (std::getline(f, line)) {
        ++ln;
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> p = split(line);
        const std::string& k = p[0];
        try {
            if (k == "doc") {
                doc.name = unesc(p.at(1));
                doc.components[doc.rootId].name = doc.name;
            } else if (k == "ver") {
                doc.version = std::max(0, std::stoi(p.at(1)));
            } else if (k == "marker") {
                doc.markerPos = std::stoi(p.at(1));
            } else if (k == "sec") {
                doc.section.on = p.at(1) == "1";
                doc.section.axis = std::clamp(std::stoi(p.at(2)), 0, 2);
                doc.section.offset = std::stod(p.at(3));
                doc.section.flip = p.at(4) == "1";
            } else if (k == "nextid") {
                // ignored: ids are remapped below
            } else if (k == "param") {
                Param pm;
                pm.name = unesc(p.at(1));
                pm.expr = unesc(p.at(2));
                if (p.size() > 3) pm.comment = unesc(p[3]);
                doc.params[pm.name] = pm;
            } else if (k == "sketch") {
                uint32_t sid = std::stoul(p.at(1));
                sk::Sketch s;
                s.id = sid;
                s.featureId = std::stoul(p.at(2));
                s.name = unesc(p.at(3));
                s.visible = p.at(4) == "1";
                std::vector<double> o = nums(p.at(5)), x = nums(p.at(6)), y = nums(p.at(7));
                if (o.size() != 3 || x.size() != 3 || y.size() != 3) { bad("Bad sketch plane"); return r; }
                s.plane.o = {o[0], o[1], o[2]};
                s.plane.x = {x[0], x[1], x[2]};
                s.plane.y = {y[0], y[1], y[2]};
                if (p.size() > 8) s.plane.name = unesc(p[8]);
                doc.sketches[sid] = std::move(s);
                curSketch = &doc.sketches[sid];
            } else if (k == "ent") {
                if (!curSketch) { bad("Entity outside a sketch"); return r; }
                uint32_t id = std::stoul(p.at(2));
                sk::Entity e;
                e.id = id;
                e.type = (sk::EntType)std::stoi(p.at(3));
                e.x = std::stod(p.at(4));
                e.y = std::stod(p.at(5));
                e.r = std::stod(p.at(6));
                e.a = std::stoul(p.at(7));
                e.b = std::stoul(p.at(8));
                e.c = std::stoul(p.at(9));
                e.construction = p.at(10) == "1";
                if ((e.type == sk::EntType::Spline || e.type == sk::EntType::CtrlSpline) && p.size() > 12) {
                    e.loop = p[11] == "1";
                    std::stringstream ss(p[12]);
                    for (std::string t; std::getline(ss, t, ',');)
                        if (!t.empty()) e.pts.push_back((uint32_t)std::stoul(t));
                }
                curSketch->ents[id] = e;
                if (id >= curSketch->nextId) curSketch->nextId = id + 1;
            } else if (k == "stxt") {
                if (!curSketch) { bad("Text outside a sketch"); return r; }
                sk::Sketch::TextNote n;
                n.id = std::stoul(p.at(2));
                n.text = unesc(p.at(3));
                n.font = unesc(p.at(4));
                n.height = std::stod(p.at(5));
                n.origin = {std::stod(p.at(6)), std::stod(p.at(7))};
                n.construction = p.at(8) == "1";
                if (p.size() > 9) {
                    std::stringstream ss(p[9]);
                    for (std::string t; std::getline(ss, t, ',');)
                        if (!t.empty()) n.ents.push_back((uint32_t)std::stoul(t));
                }
                if (n.id >= curSketch->nextNote) curSketch->nextNote = n.id + 1;
                curSketch->notes.push_back(std::move(n));
            } else if (k == "con") {
                if (!curSketch) { bad("Constraint outside a sketch"); return r; }
                sk::Constraint c;
                c.id = std::stoul(p.at(2));
                c.type = (sk::CType)std::stoi(p.at(3));
                c.e[0] = std::stoul(p.at(4));
                c.e[1] = std::stoul(p.at(5));
                c.e[2] = std::stoul(p.at(6));
                c.value = std::stod(p.at(7));
                c.value2 = std::stod(p.at(8));
                c.name = unesc(p.at(9));
                c.label.x = std::stod(p.at(10));
                c.label.y = std::stod(p.at(11));
                c.labelSet = p.at(12) == "1";
                if (p.size() > 13) c.expr = unesc(p[13]);
                if (p.size() > 14) c.driven = p[14] == "1";
                curSketch->cons[c.id] = c;
            } else if (k == "sketchend") {
                curSketch->nextId = std::stoul(p.at(2));
                curSketch->touch();
                curSketch = nullptr;
            } else if (k == "feat") {
                Feature ft;
                ft.id = std::stoul(p.at(1));
                std::string t = p.at(2);
                ft.type = t == "import" ? FeatureType::Import : t == "sketch" ? FeatureType::Sketch :
                          t == "extrude" ? FeatureType::Extrude : t == "revolve" ? FeatureType::Revolve :
                          t == "fillet" ? FeatureType::Fillet : t == "chamfer" ? FeatureType::Chamfer :
                          t == "shell" ? FeatureType::Shell : t == "presspull" ? FeatureType::PressPull : t == "deleteface" ? FeatureType::DeleteFace :
                          t == "pattern" ? FeatureType::Pattern : t == "mirror" ? FeatureType::Mirror :
                          t == "combine" ? FeatureType::Combine : t == "split" ? FeatureType::Split :
                          t == "splitface" ? FeatureType::SplitFace :
                          t == "hole" ? FeatureType::Hole : t == "sweep" ? FeatureType::Sweep :
                          t == "loft" ? FeatureType::Loft : t == "move" ? FeatureType::Move :
                          t == "meshconv" ? FeatureType::MeshConvert :
                          t == "cplane" ? FeatureType::ConstructPlane : t == "remove" ? FeatureType::Remove :
                          t == "gear" ? FeatureType::Gear : t == "thread" ? FeatureType::Thread : t == "wing" ? FeatureType::Wing : t == "dovetail" ? FeatureType::Dovetail :
                          t == "blade" ? FeatureType::Blade : t == "align" ? FeatureType::Align :
                          t == "draft" ? FeatureType::Draft : t == "coil" ? FeatureType::Coil :
                          t == "pipe" ? FeatureType::Pipe : t == "replaceface" ? FeatureType::ReplaceFace :
                          t == "caxis" ? FeatureType::ConstructAxis : t == "cpoint" ? FeatureType::ConstructPoint :
                          t == "rib" ? FeatureType::Rib : t == "web" ? FeatureType::Web :
                          t == "emboss" ? FeatureType::Emboss : t == "boss" ? FeatureType::Boss :
                          t == "snap" ? FeatureType::Snap : t == "rest" ? FeatureType::Rest :
                          t == "lip" ? FeatureType::Lip : t == "silhouette" ? FeatureType::Silhouette :
                          t == "thicken" ? FeatureType::Thicken :
                          t == "joint" ? FeatureType::Joint : t == "asbuilt" ? FeatureType::AsBuilt :
                          t == "rigid" ? FeatureType::Rigid : FeatureType::Other;
                ft.name = unesc(p.at(3));
                ft.suppressed = p.at(4) == "1";
                ft.sketchId = std::stoul(p.at(5));
                ft.importPath = unesc(p.at(6));
                doc.timeline.push_back(std::move(ft));
            } else if (k == "xpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto x = std::make_shared<ExtrudeParams>();
                x->sketchId = std::stoul(p.at(2));
                x->distExpr = unesc(p.at(3));
                x->taperExpr = unesc(p.at(4));
                x->op = std::stoi(p.at(5));
                x->flip = p.at(6) == "1";
                x->symmetric = p.at(7) == "1";
                x->targets = parseRefs(p.at(8));
                if (p.size() > 9) x->profilePts = parsePts(p[9]);
                if (p.size() > 10) x->faces = parseFaces(p[10]);
                if (p.size() > 15) {
                    x->extent = std::stoi(p[11]);
                    x->to.feature = (uint32_t)std::stoul(p[12]);
                    x->to.slot = (uint32_t)std::stoul(p[13]);
                    x->to.face = (uint32_t)std::stoul(p[14]);
                    x->to.faceTotal = (uint32_t)std::stoul(p[15]);
                }
                if (p.size() > 16) x->thinExpr = unesc(p[16]);
                if (p.size() > 17) x->thinSide = std::stoi(p[17]);
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.extrude = x;
            } else if (k == "mpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto m = std::make_shared<ModifyParams>();
                m->kind = std::stoi(p.at(2));
                m->valueExpr = unesc(p.at(3));
                m->edgeTotal = std::stoul(p.at(4));
                m->faceTotal = std::stoul(p.at(5));
                m->targets = parseRefs(p.at(6));
                m->edges = parseEF(p.at(7));
                m->faces = parseEF(p.at(8));
                if (p.size() > 9) m->axis = std::stoi(p[9]);
                if (p.size() > 10) m->side = std::stoi(p[10]);
                if (p.size() > 11) m->endExpr = unesc(p[11]);
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.modify = m;
            } else if (k == "solid") {
                uint32_t fid = std::stoul(p.at(1));
                auto s = std::make_shared<SweepSolidParams>();
                s->axis = std::stoi(p.at(2));
                s->leftHand = p.at(3) == "1";
                s->pathSketch = std::stoul(p.at(4));
                s->diaExpr = unesc(p.at(5));
                s->heightExpr = unesc(p.at(6));
                s->turnsExpr = unesc(p.at(7));
                s->sectionExpr = unesc(p.at(8));
                s->innerExpr = unesc(p.at(9));
                s->xExpr = unesc(p.at(10));
                s->yExpr = unesc(p.at(11));
                s->zExpr = unesc(p.at(12));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.pathSolid = s;
            } else if (k == "ribpar" || k == "webpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto r = std::make_shared<RibParams>();
                r->sketchId = std::stoul(p.at(2));
                r->thickExpr = unesc(p.at(3));
                r->flip = p.at(4) == "1";
                r->side = std::stoi(p.at(5));
                r->targets = parseRefs(p.at(6));
                for (auto& ft : doc.timeline) {
                    if (ft.id != fid) continue;
                    if (k == "webpar") ft.web = r;
                    else ft.rib = r;
                }
            } else if (k == "embpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto e = std::make_shared<EmbossParams>();
                e->sketchId = std::stoul(p.at(2));
                e->depthExpr = unesc(p.at(3));
                e->deboss = p.at(4) == "1";
                e->flip = p.at(5) == "1";
                e->targets = parseRefs(p.at(6));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.emboss = e;
            } else if (k == "bospar") {
                uint32_t fid = std::stoul(p.at(1));
                auto b = std::make_shared<BossParams>();
                b->xExpr = unesc(p.at(2));
                b->yExpr = unesc(p.at(3));
                b->zExpr = unesc(p.at(4));
                b->diaExpr = unesc(p.at(5));
                b->heightExpr = unesc(p.at(6));
                b->holeExpr = unesc(p.at(7));
                b->axis = std::stoi(p.at(8));
                b->targets = parseRefs(p.at(9));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.boss = b;
            } else if (k == "snppar") {
                uint32_t fid = std::stoul(p.at(1));
                auto s = std::make_shared<SnapParams>();
                s->xExpr = unesc(p.at(2));
                s->yExpr = unesc(p.at(3));
                s->zExpr = unesc(p.at(4));
                s->lengthExpr = unesc(p.at(5));
                s->widthExpr = unesc(p.at(6));
                s->thickExpr = unesc(p.at(7));
                s->hookExpr = unesc(p.at(8));
                s->hookLenExpr = unesc(p.at(9));
                s->axis = std::stoi(p.at(10));
                s->targets = parseRefs(p.at(11));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.snap = s;
            } else if (k == "rstpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto r = std::make_shared<RestParams>();
                r->sketchId = std::stoul(p.at(2));
                r->thickExpr = unesc(p.at(3));
                r->flip = p.at(4) == "1";
                r->targets = parseRefs(p.at(5));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.rest = r;
            } else if (k == "lippar") {
                uint32_t fid = std::stoul(p.at(1));
                auto lip = std::make_shared<LipParams>();
                lip->sketchId = std::stoul(p.at(2));
                lip->widthExpr = unesc(p.at(3));
                lip->heightExpr = unesc(p.at(4));
                lip->flip = p.at(5) == "1";
                lip->groove = p.at(6) == "1";
                lip->targets = parseRefs(p.at(7));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.lip = lip;
            } else if (k == "silpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto s = std::make_shared<SilhouetteParams>();
                s->axis = std::stoi(p.at(2));
                s->targets = parseRefs(p.at(3));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.silhouette = s;
            } else if (k == "jntpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto j = std::make_shared<JointParams>();
                j->kind = std::stoi(p.at(2));
                j->gx = unesc(p.at(3));
                j->gy = unesc(p.at(4));
                j->gz = unesc(p.at(5));
                j->mx = unesc(p.at(6));
                j->my = unesc(p.at(7));
                j->mz = unesc(p.at(8));
                auto oneRef = [](const std::string& s) {
                    auto dot = s.find('.');
                    if (dot == std::string::npos) return TargetRef{0, 0};
                    return TargetRef{ (uint32_t)std::stoul(s.substr(0, dot)), (uint32_t)std::stoul(s.substr(dot + 1)) };
                };
                j->ground = oneRef(p.at(9));
                j->move = oneRef(p.at(10));
                if (p.size() > 12) {
                    j->angleExpr = unesc(p.at(11));
                    j->axis = std::stoi(p.at(12));
                }
                if (p.size() > 13) j->slideExpr = unesc(p.at(13));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.joint = j;
            } else if (k == "abjpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto j = std::make_shared<JointParams>();
                j->kind = std::stoi(p.at(2));
                j->gx = unesc(p.at(3));
                j->gy = unesc(p.at(4));
                j->gz = unesc(p.at(5));
                j->mx = unesc(p.at(6));
                j->my = unesc(p.at(7));
                j->mz = unesc(p.at(8));
                auto oneRef = [](const std::string& s) {
                    auto dot = s.find('.');
                    if (dot == std::string::npos) return TargetRef{0, 0};
                    return TargetRef{ (uint32_t)std::stoul(s.substr(0, dot)), (uint32_t)std::stoul(s.substr(dot + 1)) };
                };
                j->ground = oneRef(p.at(9));
                j->move = oneRef(p.at(10));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.asbuilt = j;
            } else if (k == "rgpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto r = std::make_shared<RigidParams>();
                r->members = parseRefs(p.at(2));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.rigid = r;
            } else if (k == "ppar") {
                uint32_t fid = std::stoul(p.at(1));
                auto q = std::make_shared<PatternParams>();
                int mode = std::stoi(p.at(2));
                q->circular = mode == 1;
                q->onPath = mode == 2;
                q->countExpr = unesc(p.at(3));
                q->spacingExpr = unesc(p.at(4));
                q->countYExpr = unesc(p.at(5));
                q->spacingYExpr = unesc(p.at(6));
                q->axis = std::stoi(p.at(7));
                q->angleExpr = unesc(p.at(8));
                q->targets = parseRefs(p.at(9));
                if (p.size() >= 16) {
                    q->axisO = {std::stod(p[10]), std::stod(p[11]), std::stod(p[12])};
                    q->axisD = {std::stod(p[13]), std::stod(p[14]), std::stod(p[15])};
                }
                if (p.size() >= 24) {
                    q->pathSketch = std::stoul(p[16]);
                    q->pathEnt = std::stoul(p[17]);
                    q->distType = std::stoi(p[18]);
                    q->orient = std::stoi(p[19]);
                    q->flip = p[20] == "1";
                    q->pathBody = {std::stoul(p[21]), std::stoul(p[22])};
                    q->pathEdge = std::stoul(p[23]);
                }
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.pattern = q;
            } else if (k == "mpar2") {
                uint32_t fid = std::stoul(p.at(1));
                auto m = std::make_shared<MirrorParams>();
                m->plane = std::stoi(p.at(2));
                m->targets = parseRefs(p.at(3));
                if (p.size() > 5) {
                    std::vector<double> o = nums(p[4]), n = nums(p[5]);
                    if (o.size() == 3) m->planeO = {o[0], o[1], o[2]};
                    if (n.size() == 3) m->planeN = {n[0], n[1], n[2]};
                }
                if (p.size() > 6) m->planeName = unesc(p[6]);
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.mirror = m;
            } else if (k == "cpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto c = std::make_shared<CombineParams>();
                c->op = std::stoi(p.at(2));
                auto colon = p.at(3).find(':');
                if (colon != std::string::npos) {
                    c->tool.first = (uint32_t)std::stoul(p.at(3).substr(0, colon));
                    c->tool.second = (uint32_t)std::stoul(p.at(3).substr(colon + 1));
                }
                c->targets = parseRefs(p.at(4));
                if (p.size() > 5) c->tools = parseRefs(p[5]);
                if (p.size() > 6) c->keepTools = p[6] == "1";
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.combine = c;
            } else if (k == "spar") {
                uint32_t fid = std::stoul(p.at(1));
                auto sp = std::make_shared<SplitParams>();
                sp->plane = std::stoi(p.at(2));
                sp->targets = parseRefs(p.at(3));
                if (p.size() > 7) {
                    auto xyz = [&](const std::string& s, glm::dvec3& o) {
                        std::stringstream ss(s);
                        std::string t;
                        int i = 0;
                        while (std::getline(ss, t, ',') && i < 3) o[i++] = std::stod(t);
                    };
                    xyz(p.at(4), sp->toolO);
                    xyz(p.at(5), sp->toolN);
                    sp->toolName = unesc(p.at(6));
                    sp->sketchId = std::stoul(p.at(7));
                    if (p.size() > 8) sp->sketchEnt = std::stoul(p.at(8));
                }
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.split = sp;
            } else if (k == "sfpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto m = std::make_shared<SplitFaceParams>();
                m->plane = std::stoi(p.at(2));
                m->offsetExpr = unesc(p.at(3));
                m->targets = parseRefs(p.at(4));
                if (p.size() > 5) m->faceTotal = (uint32_t)std::stoul(p[5]);
                if (p.size() > 6) {
                    std::stringstream ss(p[6]);
                    for (std::string t; std::getline(ss, t, ',');)
                        if (!t.empty()) m->faces.push_back((uint32_t)std::stoul(t));
                }
                if (p.size() > 7 && !p[7].empty()) m->sketchId = (uint32_t)std::stoul(p[7]);
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.splitFace = m;
            } else if (k == "algn") {
                uint32_t fid = std::stoul(p.at(1));
                auto a = std::make_shared<AlignParams>();
                a->flip = p.at(2) == "1";
                auto pk = [&](AlignParams::Pick& q, int base) {
                    q.kind = std::stoi(p.at(base));
                    q.ref = {std::stoul(p.at(base + 1)), std::stoul(p.at(base + 2))};
                    q.sub = std::stoul(p.at(base + 3));
                    q.total = std::stoul(p.at(base + 4));
                    q.end = std::stoi(p.at(base + 5));
                };
                pk(a->from, 3);
                pk(a->to, 9);
                a->fromName = unesc(p.at(15));
                a->toName = unesc(p.at(16));
                a->targets = parseRefs(p.at(17));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.align = a;
            } else if (k == "hpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto h = std::make_shared<HoleParams>();
                h->sketchId = std::stoul(p.at(2));
                h->diaExpr = unesc(p.at(3));
                h->depthExpr = unesc(p.at(4));
                h->targets = parseRefs(p.at(5));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.hole = h;
            } else if (k == "hpt") {
                uint32_t fid = std::stoul(p.at(1));
                for (auto& ft : doc.timeline) {
                    if (ft.id != fid || !ft.hole) continue;
                    auto h = std::make_shared<HoleParams>(*ft.hole);
                    h->kind = std::stoi(p.at(2));
                    h->cbDiaExpr = unesc(p.at(3));
                    h->cbDepthExpr = unesc(p.at(4));
                    h->csDiaExpr = unesc(p.at(5));
                    h->csAngleExpr = unesc(p.at(6));
                    std::istringstream ps(p.size() > 7 ? p.at(7) : std::string());
                    for (std::string one; std::getline(ps, one, ';');) {
                        HoleParams::Point pt;
                        double v[6];
                        if (std::sscanf(one.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                            pt.pos = {v[0], v[1], v[2]};
                            pt.dir = {v[3], v[4], v[5]};
                            h->points.push_back(pt);
                        }
                    }
                    if (p.size() > 8) h->spotDiaExpr = unesc(p[8]);
                    if (p.size() > 9) h->spotDepthExpr = unesc(p[9]);
                    if (p.size() > 10) h->drillExpr = unesc(p[10]);
                    if (p.size() > 11) h->tapped = p[11] == "1";
                    if (p.size() > 12) h->pitchExpr = unesc(p[12]);
                    ft.hole = h;
                }
            } else if (k == "msk") {
                uint32_t fid = std::stoul(p.at(1));
                auto m = std::make_shared<MultiSketchParams>();
                m->sketchIds = parseIds(p.at(2));
                m->op = std::stoi(p.at(3));
                m->targets = parseRefs(p.at(4));
                if (p.size() > 5) m->twistExpr = unesc(p[5]);
                if (p.size() > 6) m->railIds = parseIds(p[6]);
                if (p.size() > 7) m->thinExpr = unesc(p[7]);
                if (p.size() > 8) m->thinSide = std::stoi(p[8]);
                if (p.size() > 9) m->pointEnd = std::stoi(p[9]);
                if (p.size() > 12) {
                    m->pointX = unesc(p[10]);
                    m->pointY = unesc(p[11]);
                    m->pointZ = unesc(p[12]);
                }
                if (p.size() > 13) m->centerId = (uint32_t)std::stoul(p[13]);
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.multi = m;
            } else if (k == "mvp") {
                uint32_t fid = std::stoul(p.at(1));
                auto m = std::make_shared<MoveParams>();
                m->xExpr = unesc(p.at(2));
                m->yExpr = unesc(p.at(3));
                m->zExpr = unesc(p.at(4));
                m->copy = p.at(5) == "1";
                m->targets = parseRefs(p.at(6));
                if (p.size() > 9) { m->rxExpr = unesc(p[7]); m->ryExpr = unesc(p[8]); m->rzExpr = unesc(p[9]); }
                if (p.size() > 13) {
                    m->angExpr = unesc(p[10]); m->axExpr = unesc(p[11]); m->ayExpr = unesc(p[12]); m->azExpr = unesc(p[13]);
                }
                if (p.size() > 16) { m->pxExpr = unesc(p[14]); m->pyExpr = unesc(p[15]); m->pzExpr = unesc(p[16]); }
                if (p.size() > 19) { m->sxExpr = unesc(p[17]); m->syExpr = unesc(p[18]); m->szExpr = unesc(p[19]); }
                if (p.size() > 20) m->hasMat = p[20] == "1";
                if (p.size() > 36)
                    for (int i = 0; i < 16; ++i) m->mat[i] = std::atof(p[21 + i].c_str());
                if (p.size() > 37) m->op = std::stoi(p[37]);
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.move = m;
            } else if (k == "mcv") {
                uint32_t fid = std::stoul(p.at(1));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid && ft.modify) { /* covered by mpar load */ }
            } else if (k == "rmv") {
                uint32_t fid = std::stoul(p.at(1));
                auto m = std::make_shared<RemoveParams>();
                m->targets = parseRefs(p.at(2));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.remove = m;
            } else if (k == "cpl") {
                uint32_t fid = std::stoul(p.at(1));
                auto m = std::make_shared<ConstructPlaneParams>();
                m->base = std::stoi(p.at(2));
                m->offsetExpr = unesc(p.at(3));
                if (p.size() > 4) m->kind = std::stoi(p[4]);
                if (p.size() > 5) m->axis = std::stoi(p[5]);
                if (p.size() > 6) m->refPlane = (uint32_t)std::stoul(p[6]);
                if (p.size() > 15) {
                    m->x1 = unesc(p[7]); m->y1 = unesc(p[8]); m->z1 = unesc(p[9]);
                    m->x2 = unesc(p[10]); m->y2 = unesc(p[11]); m->z2 = unesc(p[12]);
                    m->x3 = unesc(p[13]); m->y3 = unesc(p[14]); m->z3 = unesc(p[15]);
                }
                if (p.size() > 19) {
                    auto c = p[16].find(':');
                    if (c != std::string::npos)
                        m->faceRef = { (uint32_t)std::stoul(p[16].substr(0, c)), (uint32_t)std::stoul(p[16].substr(c + 1)) };
                    m->faceSub = (uint32_t)std::stoul(p[17]);
                    m->faceTotal = (uint32_t)std::stoul(p[18]);
                    m->angleExpr = unesc(p[19]);
                }
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.cplane = m;
            } else if (k == "cax") {
                uint32_t fid = std::stoul(p.at(1));
                auto m = std::make_shared<ConstructAxisParams>();
                if (p.size() > 7) {
                    m->ax = unesc(p[2]); m->ay = unesc(p[3]); m->az = unesc(p[4]);
                    m->bx = unesc(p[5]); m->by = unesc(p[6]); m->bz = unesc(p[7]);
                }
                if (p.size() > 8) m->kind = std::stoi(p[8]);
                if (p.size() > 9) {
                    auto c = p[9].find(':');
                    if (c != std::string::npos)
                        m->ref = {(uint32_t)std::stoul(p[9].substr(0, c)), (uint32_t)std::stoul(p[9].substr(c + 1))};
                }
                if (p.size() > 10) m->sub = (uint32_t)std::stoul(p[10]);
                if (p.size() > 11) m->subTotal = (uint32_t)std::stoul(p[11]);
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.caxis = m;
            } else if (k == "cpt") {
                uint32_t fid = std::stoul(p.at(1));
                auto m = std::make_shared<ConstructPointParams>();
                if (p.size() > 4) { m->x = unesc(p[2]); m->y = unesc(p[3]); m->z = unesc(p[4]); }
                if (p.size() > 5) m->kind = std::stoi(p[5]);
                if (p.size() > 6) {
                    auto c = p[6].find(':');
                    if (c != std::string::npos)
                        m->ref = {(uint32_t)std::stoul(p[6].substr(0, c)), (uint32_t)std::stoul(p[6].substr(c + 1))};
                }
                if (p.size() > 7) m->sub = (uint32_t)std::stoul(p[7]);
                if (p.size() > 8) m->subTotal = (uint32_t)std::stoul(p[8]);
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.cpoint = m;
            } else if (k == "gearp") {
                uint32_t fid = std::stoul(p.at(1));
                auto g = std::make_shared<GearParams>();
                g->type = std::stoi(p.at(2));
                g->plane = std::stoi(p.at(3));
                g->sizeBy = std::stoi(p.at(4));
                g->leftHand = p.at(5) == "1";
                std::stringstream ss(unesc(p.at(6)));
                for (std::string kv; std::getline(ss, kv, ';');) {
                    auto eq = kv.find('=');
                    if (eq != std::string::npos) g->vals[kv.substr(0, eq)] = kv.substr(eq + 1);
                }
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.gear = g;
            } else if (k == "bladep") {
                uint32_t fid = std::stoul(p.at(1));
                auto b = std::make_shared<BladeParams>();
                b->kind = std::stoi(p.at(2));
                b->plane = std::stoi(p.at(3));
                b->leftHand = p.at(4) == "1";
                b->airfoil = unesc(p.at(5));
                std::stringstream ss(unesc(p.at(6)));
                for (std::string kv; std::getline(ss, kv, ';');) {
                    auto eq = kv.find('=');
                    if (eq != std::string::npos) b->vals[kv.substr(0, eq)] = kv.substr(eq + 1);
                }
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.blade = b;
            } else if (k == "dovp") {
                uint32_t fid = std::stoul(p.at(1));
                auto d = std::make_shared<DovetailParams>();
                d->type = std::stoi(p.at(2));
                d->plane = std::stoi(p.at(3));
                d->targets = parseRefs(p.at(4));
                std::stringstream ss(unesc(p.at(5)));
                for (std::string kv; std::getline(ss, kv, ';');) {
                    auto eq = kv.find('=');
                    if (eq != std::string::npos) d->vals[kv.substr(0, eq)] = kv.substr(eq + 1);
                }
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.dovetail = d;
            } else if (k == "wingp") {
                uint32_t fid = std::stoul(p.at(1));
                auto w = std::make_shared<WingParams>();
                w->mirror = p.at(2) == "1";
                w->hollow = p.at(3) == "1";
                w->rootAirfoil = unesc(p.at(4));
                std::stringstream ss(unesc(p.at(5)));
                for (std::string kv; std::getline(ss, kv, ';');) {
                    auto eq = kv.find('=');
                    if (eq != std::string::npos) w->vals[kv.substr(0, eq)] = kv.substr(eq + 1);
                }
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.wing = w;
            } else if (k == "wingpn") {
                uint32_t fid = std::stoul(p.at(1));
                WingParams::Panel pn;
                pn.span = unesc(p.at(2));
                pn.tipChord = unesc(p.at(3));
                pn.sweep = unesc(p.at(4));
                pn.dihedral = unesc(p.at(5));
                pn.twist = unesc(p.at(6));
                pn.airfoil = unesc(p.at(7));
                for (auto& ft : doc.timeline)
                    if (ft.id == fid && ft.wing) {
                        auto w = std::make_shared<WingParams>(*ft.wing);
                        w->panels.push_back(pn);
                        ft.wing = w;
                    }
            } else if (k == "thrp") {
                uint32_t fid = std::stoul(p.at(1));
                auto t = std::make_shared<ThreadParams>();
                t->face.feature = std::stoul(p.at(2));
                t->face.slot = std::stoul(p.at(3));
                t->face.face = std::stoul(p.at(4));
                t->face.faceTotal = std::stoul(p.at(5));
                t->family = std::stoi(p.at(6));
                t->size = std::stoi(p.at(7));
                t->side = std::stoi(p.at(8));
                t->modeled = p.at(9) == "1";
                t->leftHand = p.at(10) == "1";
                t->flip = p.at(11) == "1";
                t->fullLength = p.at(12) == "1";
                t->lengthExpr = unesc(p.at(13));
                t->offsetExpr = unesc(p.at(14));
                t->startsExpr = unesc(p.at(15));
                t->diaExpr = unesc(p.at(16));
                t->pitchExpr = unesc(p.at(17));
                std::vector<double> o = nums(p.at(18)), d = nums(p.at(19));
                if (o.size() == 3) t->axisO = {o[0], o[1], o[2]};
                if (d.size() == 3) t->axisD = {d[0], d[1], d[2]};
                t->radius = std::stod(p.at(20));
                t->faceLen = std::stod(p.at(21));
                t->hole = p.at(22) == "1";
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.thread = t;
            } else if (k == "rpar") {
                uint32_t fid = std::stoul(p.at(1));
                auto v = std::make_shared<RevolveParams>();
                v->sketchId = std::stoul(p.at(2));
                v->angleExpr = unesc(p.at(3));
                v->axis = std::stoi(p.at(4));
                v->axisEnt = std::stoul(p.at(5));
                v->op = std::stoi(p.at(6));
                v->flip = p.at(7) == "1";
                v->targets = parseRefs(p.at(8));
                if (p.size() > 9) v->profilePts = parsePts(p[9]);
                if (p.size() > 10) v->thinExpr = unesc(p[10]);
                if (p.size() > 11) v->thinSide = std::stoi(p[11]);
                for (auto& ft : doc.timeline)
                    if (ft.id == fid) ft.revolve = v;
            } else if (k == "comp") {
                uint32_t cid = std::stoul(p.at(1));
                Component c;
                c.id = cid;
                c.parentId = std::stoul(p.at(2));
                c.name = unesc(p.at(3));
                c.visible = p.at(4) == "1";
                c.featureId = std::stoul(p.at(5));
                if (p.size() > 6) c.grounded = p.at(6) == "1";
                doc.components[cid] = c;
            } else if (k == "cname") {
                doc.compNames[Document::overrideKey(std::stoul(p.at(1)), std::stoul(p.at(2)))] = unesc(p.at(3));
            } else if (k == "chid") {
                doc.compHidden.insert(Document::overrideKey(std::stoul(p.at(1)), std::stoul(p.at(2))));
            } else if (k == "skah2") {
                doc.autoHidden.insert(((uint64_t)std::stoul(p.at(1)) << 32) | std::stoul(p.at(2)));
            } else if (k == "phid") {
                doc.hiddenPlanes.insert(std::stoul(p.at(1)));
            } else if (k == "robot") {
                doc.robot.name = unesc(p.at(1));
                doc.robot.base = unesc(p.at(2));
                doc.robot.density = std::stod(p.at(3));
                if (p.size() > 4) doc.robot.front = std::clamp(std::stoi(p.at(4)), 0, 3);
            } else if (k == "rmass") {
                doc.robot.massG[unesc(p.at(1))] = std::stod(p.at(2));
            } else if (k == "rjoint") {
                RobotJoint j;
                j.name = unesc(p.at(1));
                j.parent = unesc(p.at(2));
                j.child = unesc(p.at(3));
                j.type = std::stoi(p.at(4));
                auto o = nums(p.at(5)), ax = nums(p.at(6));
                if (o.size() == 3) j.origin = {o[0], o[1], o[2]};
                if (ax.size() == 3) j.axis = {ax[0], ax[1], ax[2]};
                j.lower = std::stod(p.at(7));
                j.upper = std::stod(p.at(8));
                j.effort = std::stod(p.at(9));
                j.velocity = std::stod(p.at(10));
                if (p.size() > 12) { j.stiffness = std::stod(p.at(11)); j.damping = std::stod(p.at(12)); }
                doc.robot.joints.push_back(j);
            } else if (k == "ffold") {
                doc.featureFolder[std::stoul(p.at(1))] = std::stoul(p.at(2));
            } else if (k == "cact") {
                doc.activeComponent = std::stoul(p.at(1));
            } else if (k == "override") {
                uint32_t fid = std::stoul(p.at(1)), slot = std::stoul(p.at(2));
                BodyOverride o;
                o.name = unesc(p.at(3));
                o.hasColor = p.at(4) == "1";
                std::vector<double> rgb = nums(p.at(5));
                if (rgb.size() == 3) o.color = {(float)rgb[0], (float)rgb[1], (float)rgb[2]};
                o.visible = p.at(6) == "1";
                doc.overrides[Document::overrideKey(fid, slot)] = o;
            } else if (k == "look" || k == "flook") {
                // Appearance (newer files). Lines refer to an override written just before them.
                uint32_t fid = std::stoul(p.at(1)), slot = std::stoul(p.at(2));
                auto oit = doc.overrides.find(Document::overrideKey(fid, slot));
                if (oit != doc.overrides.end()) {
                    size_t at = k == "look" ? 3 : 5;
                    MatProps m;
                    std::vector<double> v = nums(p.at(at));
                    if (v.size() >= 9) {
                        m.opacity = (float)std::clamp(v[0], 0.02, 1.0);
                        m.metal = (float)std::clamp(v[1], 0.0, 1.0);
                        m.rough = (float)std::clamp(v[2], 0.0, 1.0);
                        int pt = (int)v[3];
                        m.pattern = pt >= 0 && pt < (int)MatPattern::Count ? (MatPattern)pt : MatPattern::None;
                        m.color2 = {(float)v[4], (float)v[5], (float)v[6]};
                        m.scale = (float)std::max(0.01, v[7]);
                        m.glow = (float)std::max(0.0, v[8]);
                        if (v.size() >= 10) m.fileColors = v[9] != 0.0;
                    }
                    if (p.size() > at + 1) m.name = unesc(p[at + 1]);
                    if (k == "look") {
                        oit->second.mat = m;
                    } else {
                        FaceLook fl;
                        std::vector<double> rgb = nums(p.at(4));
                        if (rgb.size() == 3) fl.color = {(float)rgb[0], (float)rgb[1], (float)rgb[2]};
                        fl.mat = m;
                        oit->second.faceLooks[(uint32_t)std::stoul(p.at(3))] = fl;
                    }
                }
            }
        } catch (const std::exception&) {
            bad("Malformed line");
            return r;
        }
    }
    // Refresh ids so new features never collide with loaded ones.
    uint32_t maxId = doc.rootId;
    for (auto& [id, s] : doc.sketches) {
        maxId = std::max({maxId, id, s.featureId});
        for (auto& [eid, e] : s.ents) maxId = std::max(maxId, eid);
        for (auto& [cid, c] : s.cons) maxId = std::max(maxId, cid);
    }
    for (auto& ft : doc.timeline) maxId = std::max(maxId, ft.id);
    for (auto& [cid, c] : doc.components) maxId = std::max(maxId, cid);
    doc.remapNextId(maxId);
    doc.markerPos = std::clamp(doc.markerPos, 0, (int)doc.timeline.size());
    std::string err;
    updateParamValues(doc.params, err);   // best effort; bad params surface in the dialog
    r.ok = true;
    return r;
}

} // namespace kst
