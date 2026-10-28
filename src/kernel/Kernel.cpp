#include "kernel/Kernel.h"
#include "kernel/ShapeHandle.h"
#include "core/Log.h"
#include "platform/Platform.h"

#include <BinTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRepTools.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Message_ProgressIndicator.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <BRepTools_History.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Poly_Triangulation.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Quantity_Color.hxx>
#include <gp_Pln.hxx>
#include <RWStl.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <XSControl_TransferReader.hxx>
#include <XSControl_WorkSession.hxx>
#include <StepRepr_RepresentationItem.hxx>
#include <TCollection_HAsciiString.hxx>
#include <unordered_map>
#include <STEPCAFControl_Writer.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Controller.hxx>
#include <Standard_Version.hxx>
#if OCC_VERSION_HEX >= 0x070900
#include <DE_ShapeFixParameters.hxx>
#endif
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDF_ChildIterator.hxx>
#include <TDF_Tool.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace kst::kernel {

namespace {

std::mutex g_stepMutex;   // STEP reader global state is not fully thread safe

[[noreturn]] void rethrowOcct(const Standard_Failure& e, const char* where) {
    const char* msg = e.GetMessageString();
    throw std::runtime_error(std::string(where) + ": " + ((msg && *msg) ? msg : e.DynamicType()->Name()));
}

glm::mat4 toMat(const TopLoc_Location& loc) {
    glm::mat4 m(1.f);
    if (loc.IsIdentity()) return m;
    gp_Trsf t = loc.Transformation();
    for (int r = 1; r <= 3; ++r)
        for (int c = 1; c <= 4; ++c) m[c - 1][r - 1] = (float)t.Value(r, c);
    return m;
}

std::string labelName(const TDF_Label& l) {
    Handle(TDataStd_Name) n;
    if (!l.IsNull() && l.FindAttribute(TDataStd_Name::GetID(), n)) {
        TCollection_AsciiString a(n->Get());
        std::string s = a.ToCString();
        if (s.rfind("=>", 0) == 0) return {};   // auto generated reference name, not a real name
        return s;
    }
    return {};
}

bool toRgb(const Quantity_Color& c, glm::vec3& out) {
    Standard_Real r, g, b;
    c.Values(r, g, b, Quantity_TOC_sRGB);
    out = {(float)r, (float)g, (float)b};
    return true;
}

bool labelColor(const Handle(XCAFDoc_ColorTool)& ct, const TDF_Label& l, glm::vec3& out) {
    Quantity_Color c;
    if (ct.IsNull() || l.IsNull()) return false;
    if (ct->GetColor(l, XCAFDoc_ColorSurf, c) || ct->GetColor(l, XCAFDoc_ColorGen, c)) return toRgb(c, out);
    return false;
}

bool shapeColor(const Handle(XCAFDoc_ColorTool)& ct, const TopoDS_Shape& s, glm::vec3& out) {
    Quantity_Color c;
    if (ct.IsNull()) return false;
    if (ct->GetColor(s, XCAFDoc_ColorSurf, c) || ct->GetColor(s, XCAFDoc_ColorGen, c)) return toRgb(c, out);
    return false;
}

ShapePtr wrap(const TopoDS_Shape& s) {
    auto h = std::make_shared<ShapeHandle>();
    h->shape = s;
    return h;
}

std::string fileStem(const std::string& path) { return pathToU8(u8path(path).stem()); }

// Lets OpenCASCADE's long loops (STEP translation, repair, meshing) notice Stop, and reports how
// far along they are ("converting the geometry 37%").
class CancelProgress : public Message_ProgressIndicator {
public:
    explicit CancelProgress(const CancelToken& c, const ProgressFn* report = nullptr, std::string what = {})
        : cancel_(c), report_(report), what_(std::move(what)) {}
    Standard_Boolean UserBreak() override { return cancel_.isCancelled(); }
    void Show(const Message_ProgressScope&, const Standard_Boolean) override {
        if (!report_ || !*report_) return;
        int pct = (int)std::floor(std::clamp(GetPosition(), 0.0, 1.0) * 100.0);
        if (pct == last_) return;
        last_ = pct;
        (*report_)(what_ + " " + std::to_string(pct) + "%");
    }
private:
    const CancelToken& cancel_;
    const ProgressFn* report_;
    std::string what_;
    int last_ = -1;
};

// Only one STEP read at a time (OpenCASCADE's reader settings are global). Waits for the lock
// while saying so, and gives up when Stop is pressed.
std::unique_lock<std::mutex> lockStepReader(const CancelToken& cancel, const ProgressFn& progress) {
    std::unique_lock<std::mutex> lock(g_stepMutex, std::defer_lock);
    bool told = false;
    while (!lock.try_lock()) {
        if (cancel.isCancelled()) throw std::runtime_error("cancelled");
        if (!told && progress) { progress("waiting for another STEP file to finish reading"); told = true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return lock;
}

// Meshes turned into solids (in Fusion: Mesh > Convert) have one flat face per triangle. Merge
// every set of touching faces that lie in the same plane into one real face, so a box made of
// 12 triangles comes in as a box with 6 faces. Curved areas stay as they are (a triangle mesh
// carries no real curve to recover). Face colors follow the faces they were merged into.
TopoDS_Shape mergeTriangleFaces(const TopoDS_Shape& s, std::vector<uint32_t>& faceColors, const CancelToken& cancel,
                                const ProgressFn* progress) {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(s, TopAbs_FACE, faces);
    if (faces.Extent() < 200) return s;
    int planar = 0, triangles = 0;
    std::vector<char> isPlane((size_t)faces.Extent() + 1, 0);
    std::vector<gp_Pln> planes((size_t)faces.Extent() + 1);
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face& f = TopoDS::Face(faces(i));
        try {
            BRepAdaptor_Surface sa(f, Standard_False);
            if (sa.GetType() != GeomAbs_Plane) continue;
            planes[(size_t)i] = sa.Plane();
            isPlane[(size_t)i] = 1;
        } catch (const Standard_Failure&) { continue; }
        ++planar;
        int edges = 0;
        for (TopExp_Explorer e(f, TopAbs_EDGE); e.More() && edges < 5; e.Next()) ++edges;
        if (edges == 3) ++triangles;
    }
    // Only bodies that really are triangle meshes: almost all flat, mostly three sided faces.
    if (planar < faces.Extent() * 0.8 || triangles < faces.Extent() * 0.5) return s;
    if (cancel.isCancelled()) throw std::runtime_error("cancelled");
    // Smooth curved meshes have no two neighbors in the same plane: the merge would change nothing
    // and takes minutes on big ones. Count neighbor pairs that share a plane first.
    {
        TopTools_IndexedDataMapOfShapeListOfShape e2f;
        TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, e2f);
        int same = 0;
        for (int i = 1; i <= e2f.Extent(); ++i) {
            const TopTools_ListOfShape& fl = e2f(i);
            if (fl.Extent() != 2) continue;
            int a = faces.FindIndex(fl.First()), b = faces.FindIndex(fl.Last());
            if (a <= 0 || b <= 0 || !isPlane[(size_t)a] || !isPlane[(size_t)b]) continue;
            const gp_Pln &pa = planes[(size_t)a], &pb = planes[(size_t)b];
            if (!pa.Axis().Direction().IsParallel(pb.Axis().Direction(), 1e-6)) continue;
            if (pa.Distance(pb.Location()) > 1e-6) continue;
            ++same;
        }
        if (same < faces.Extent() / 50) return s;
    }
    if (progress && *progress) (*progress)("merging " + std::to_string(faces.Extent()) + " triangle faces");
    ShapeUpgrade_UnifySameDomain u(s, Standard_True, Standard_True, Standard_False);
    u.AllowInternalEdges(Standard_False);
    u.Build();
    TopoDS_Shape out = u.Shape();
    if (out.IsNull()) return s;
    if (!faceColors.empty()) {
        // Old face index -> new face index through the merge history.
        TopTools_IndexedMapOfShape newFaces;
        TopExp::MapShapes(out, TopAbs_FACE, newFaces);
        std::vector<uint32_t> fc((size_t)newFaces.Extent(), 0u);
        Handle(BRepTools_History) h = u.History();
        for (int i = 1; i <= faces.Extent() && i <= (int)faceColors.size(); ++i) {
            uint32_t c = faceColors[(size_t)i - 1];
            if (!c) continue;
            const TopoDS_Shape& of = faces(i);
            int ni = newFaces.FindIndex(of);
            if (ni > 0) { fc[(size_t)ni - 1] = c; continue; }
            if (h.IsNull()) continue;
            for (TopTools_ListOfShape::Iterator it(h->Modified(of)); it.More(); it.Next()) {
                ni = newFaces.FindIndex(it.Value());
                if (ni > 0 && !fc[(size_t)ni - 1]) fc[(size_t)ni - 1] = c;
            }
        }
        faceColors = std::move(fc);
    }
    log::info("Merged " + std::to_string(faces.Extent()) + " triangle faces into " + std::to_string([&] {
        int n = 0;
        for (TopExp_Explorer e(out, TopAbs_FACE); e.More(); e.Next()) ++n;
        return n;
    }()) + " faces");
    return out;
}

// ---------------------------------------------------------------- STEP via XCAF

struct StepWalker {
    Handle(XCAFDoc_ShapeTool) st;
    Handle(XCAFDoc_ColorTool) ct;
    ImportResult& r;
    const CancelToken& cancel;
    struct ProtoEntry { int part; TopLoc_Location local; bool hasColor; glm::vec3 color; std::string name; };
    std::map<std::string, std::vector<ProtoEntry>> protos;
    const ProgressFn* progress = nullptr;
    int partNo = 0;
    std::unordered_map<const void*, std::string> solidNames;   // TShape -> name of its STEP solid entity

    void walk(const TDF_Label& lab, const TopLoc_Location& loc, int comp, const std::string& instName,
              bool instHasColor, glm::vec3 instColor, const std::string& occPath = std::string()) {
        if (cancel.isCancelled()) throw std::runtime_error("cancelled");
        if (XCAFDoc_ShapeTool::IsAssembly(lab)) {
            std::string nm = !instName.empty() ? instName : labelName(lab);
            int c = (int)r.comps.size();
            r.comps.push_back({nm.empty() ? "Component" : nm, comp});
            TDF_LabelSequence kids;
            XCAFDoc_ShapeTool::GetComponents(lab, kids, Standard_False);
            std::map<std::string, int> seen;   // Fusion style occurrence numbers (Wheel:1, Wheel:2)
            for (int i = 1; i <= kids.Length(); ++i) {
                TDF_Label kid = kids.Value(i), ref;
                if (!XCAFDoc_ShapeTool::GetReferredShape(kid, ref)) continue;
                TopLoc_Location kl = loc * XCAFDoc_ShapeTool::GetLocation(kid);
                std::string kn = labelName(kid);
                if (kn.empty()) kn = labelName(ref);
                glm::vec3 col = instColor;
                bool hc = labelColor(ct, kid, col) || instHasColor;
                std::string tok = kn;
                {
                    auto colon = kn.rfind(':');
                    bool numbered = colon != std::string::npos && colon + 1 < kn.size() &&
                                    std::all_of(kn.begin() + (long)colon + 1, kn.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
                    if (!numbered) tok = kn + ":" + std::to_string(++seen[kn]);
                }
                walk(ref, kl, c, kn, hc, col, occPath.empty() ? tok : occPath + "+" + tok);
            }
            return;
        }
        TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(lab);
        if (shape.IsNull()) return;
        TCollection_AsciiString entry;
        TDF_Tool::Entry(lab, entry);
        std::string key = entry.ToCString();
        auto it = protos.find(key);
        std::string baseName = !instName.empty() ? instName : labelName(lab);
        if (baseName.empty()) baseName = "Body";
        if (it == protos.end()) {
            std::vector<ProtoEntry> entries;
            TopLoc_Location shapeLoc = shape.Location();
            TopoDS_Shape base = shape.Located(TopLoc_Location());
            glm::vec3 labCol;
            bool labHas = labelColor(ct, lab, labCol);
            int idx = 0;
            // Sub shape labels of this part (Fusion: named bodies, painted faces), gathered once.
            // Looking each face up with FindSubShape scans them all every time, which took hours
            // on designs with hundreds of thousands of faces (meshes converted to solids).
            std::vector<std::pair<TopoDS_Shape, TDF_Label>> subSolids, subFaces;
            for (TDF_ChildIterator ci(lab); ci.More(); ci.Next()) {
                TopoDS_Shape cs;
                if (!XCAFDoc_ShapeTool::GetShape(ci.Value(), cs) || cs.IsNull()) continue;
                if (cs.ShapeType() == TopAbs_FACE) subFaces.push_back({cs, ci.Value()});
                else subSolids.push_back({cs, ci.Value()});
            }
            auto findSub = [&](const TopoDS_Shape& x, TDF_Label& out) {
                for (auto& [ss, sl] : subSolids)
                    if (ss.IsSame(x)) { out = sl; return true; }
                return false;
            };
            TopExp_Explorer orig(shape, TopAbs_SOLID);   // same order, original placement (for names)
            for (TopExp_Explorer ex(base, TopAbs_SOLID); ex.More(); ex.Next(), ++idx) {
                TopoDS_Shape os = orig.More() ? orig.Current() : TopoDS_Shape();
                if (orig.More()) orig.Next();
                const TopoDS_Shape& s = ex.Current();
                ImportResult::PartOut p;
                p.name = baseName;
                p.shape = wrap(s.Located(TopLoc_Location()));
                // Face colors (Fusion: a face painted a different color than its body).
                {
                    TopTools_IndexedMapOfShape fmap;
                    TopExp::MapShapes(!os.IsNull() ? os : s, TopAbs_FACE, fmap);
                    bool any = false;
                    std::vector<uint32_t> fc((size_t)fmap.Extent(), 0u);
                    auto b8 = [](float x) { return (uint32_t)std::clamp((int)std::lround(x * 255.f), 0, 255); };
                    for (auto& [fs, fl] : subFaces) {
                        int fi = fmap.FindIndex(fs);   // hashed: same face, any orientation
                        glm::vec3 col;
                        if (fi <= 0 || !labelColor(ct, fl, col)) continue;
                        fc[(size_t)fi - 1] = b8(col.r) | (b8(col.g) << 8) | (b8(col.b) << 16) | (255u << 24);
                        any = true;
                    }
                    // Colors set on faces directly (no label of their own): a few faces only, the
                    // lookup is slow per face.
                    if (fmap.Extent() <= 3000)
                        for (int fi = 1; fi <= fmap.Extent(); ++fi) {
                            glm::vec3 col;
                            if (fc[(size_t)fi - 1] || !shapeColor(ct, fmap(fi), col)) continue;
                            fc[(size_t)fi - 1] = b8(col.r) | (b8(col.g) << 8) | (b8(col.b) << 16) | (255u << 24);
                            any = true;
                        }
                    if (any) p.faceColors = std::move(fc);
                }
                {
                    ++partNo;
                    if (progress && *progress) (*progress)("building part " + std::to_string(partNo) + ": " + baseName);
                    TopoDS_Shape merged = mergeTriangleFaces(s.Located(TopLoc_Location()), p.faceColors, cancel, progress);
                    if (!merged.IsSame(s.Located(TopLoc_Location()))) p.shape = wrap(merged);
                    if (cancel.isCancelled()) throw std::runtime_error("cancelled");
                }
                int pi = (int)r.parts.size();
                r.parts.push_back(std::move(p));
                ProtoEntry pe{pi, shapeLoc * s.Location(), labHas, labCol, {}};
                glm::vec3 sc;
                if (shapeColor(ct, s, sc)) { pe.hasColor = true; pe.color = sc; }
                // Fusion writes each body of a component as a named sub shape: keep its name.
                TDF_Label sub;
                if ((!os.IsNull() && findSub(os, sub)) || findSub(s, sub)) {
                    pe.name = labelName(sub);
                    glm::vec3 subCol;
                    if (labelColor(ct, sub, subCol)) { pe.hasColor = true; pe.color = subCol; }
                }
                if (pe.name.empty()) {
                    // No name label: the solid's own STEP name (MANIFOLD_SOLID_BREP('motor 1',...)).
                    auto sn = solidNames.find((const void*)s.TShape().get());
                    if (sn == solidNames.end() && !os.IsNull()) sn = solidNames.find((const void*)os.TShape().get());
                    if (sn != solidNames.end()) pe.name = sn->second;
                }
                entries.push_back(pe);
            }
            if (entries.empty()) {
                // Surfaces or shells without a solid: keep as one surface body if it has faces.
                TopExp_Explorer fx(base, TopAbs_FACE);
                if (fx.More()) {
                    ImportResult::PartOut p;
                    p.name = baseName;
                    p.shape = wrap(base);
                    int pi = (int)r.parts.size();
                    r.parts.push_back(std::move(p));
                    entries.push_back({pi, shapeLoc, labHas, labCol, {}});
                }
            }
            it = protos.emplace(key, std::move(entries)).first;
        }
        // A part holding several bodies is a Fusion component with bodies in it: give it its own
        // folder (component) so the browser tree matches Fusion's.
        int bodyComp = comp;
        if (it->second.size() > 1) {
            bodyComp = (int)r.comps.size();
            r.comps.push_back({baseName, comp});
        }
        int n = 0;
        for (auto& pe : it->second) {
            ImportResult::BodyOut b;
            b.name = !pe.name.empty() ? pe.name : it->second.size() > 1 ? "Body" + std::to_string(++n) : baseName;
            b.part = pe.part;
            b.comp = bodyComp;
            b.occPath = occPath;
            b.compDef = labelName(lab);
            b.bodyName = pe.name;
            b.xform = toMat(loc * pe.local);
            if (instHasColor) { b.hasColor = true; b.color = instColor; }
            else if (pe.hasColor) { b.hasColor = true; b.color = pe.color; }
            r.bodies.push_back(b);
        }
    }
};

// Huge STEP files (meshes turned into solids: hundreds of thousands of flat faces) spend hours
// in OpenCASCADE's face orientation repair, which compares every face with every other one.
// For those, the usual repair runs with just that one step off (Fusion writes faces already
// oriented). Done with a small resource file the STEP reader picks up by name.
const char* kBigStepResource =
        "FromSTEP.exec.op : FixShape\n"
        "FromSTEP.FixShape.Tolerance3d   : &Runtime.Tolerance\n"
        "FromSTEP.FixShape.MaxTolerance3d  : &Runtime.MaxTolerance\n"
        "FromSTEP.FixShape.MinTolerance3d  : 1.e-7\n"
        "FromSTEP.FixShape.FixFreeShellMode  : -1\n"
        "FromSTEP.FixShape.FixFreeFaceMode  : -1\n"
        "FromSTEP.FixShape.FixFreeWireMode  : -1\n"
        "FromSTEP.FixShape.FixSameParameterMode  : -1\n"
        "FromSTEP.FixShape.FixSolidMode   : -1\n"
        "FromSTEP.FixShape.FixShellOrientationMode : -1\n"
        "FromSTEP.FixShape.CreateOpenSolidMode           :  0\n"
        "FromSTEP.FixShape.FixShellMode   : -1\n"
        "FromSTEP.FixShape.FixFaceOrientationMode : 0\n"
        "FromSTEP.FixShape.FixFaceMode   : -1\n"
        "FromSTEP.FixShape.FixWireMode   : -1\n"
        "FromSTEP.FixShape.FixOrientationMode  : -1\n"
        "FromSTEP.FixShape.FixAddNaturalBoundMode : -1\n"
        "FromSTEP.FixShape.FixMissingSeamMode  : -1\n"
        "FromSTEP.FixShape.FixSmallAreaWireMode  : -1\n"
        "FromSTEP.FixShape.RemoveSmallAreaFaceMode : -1\n"
        "FromSTEP.FixShape.FixIntersectingWiresMode : -1\n"
        "FromSTEP.FixShape.FixLoopWiresMode              : -1\n"
        "FromSTEP.FixShape.FixSplitFaceMode              : -1\n"
        "FromSTEP.FixShape.AutoCorrectPrecisionMode      :  1\n"
        "FromSTEP.FixShape.ModifyTopologyMode  :  0\n"
        "FromSTEP.FixShape.ModifyGeometryMode  :  1\n"
        "FromSTEP.FixShape.ClosedWireMode  :  1\n"
        "FromSTEP.FixShape.PreferencePCurveMode  :  1\n"
        "FromSTEP.FixShape.FixReorderMode  : -1\n"
        "FromSTEP.FixShape.FixSmallMode   : -1\n"
        "FromSTEP.FixShape.FixConnectedMode  : -1\n"
        "FromSTEP.FixShape.FixEdgeCurvesMode  : -1\n"
        "FromSTEP.FixShape.FixDegeneratedMode  : -1\n"
        "FromSTEP.FixShape.FixLackingMode  : -1\n"
        "FromSTEP.FixShape.FixSelfIntersectionMode : -1\n"
        "FromSTEP.FixShape.RemoveLoopMode                : -1\n"
        "FromSTEP.FixShape.FixReversed2dMode  : -1\n"
        "FromSTEP.FixShape.FixRemovePCurveMode  : -1\n"
        "FromSTEP.FixShape.FixRemoveCurve3dMode  : -1\n"
        "FromSTEP.FixShape.FixAddPCurveMode  : -1\n"
        "FromSTEP.FixShape.FixAddCurve3dMode  : -1\n"
        "FromSTEP.FixShape.FixSeamMode   : -1\n"
        "FromSTEP.FixShape.FixShiftedMode  : -1\n"
        "FromSTEP.FixShape.FixEdgeSameParameterMode :  0\n"
        "FromSTEP.FixShape.FixNotchedEdgesMode   : -1\n"
        "FromSTEP.FixShape.FixTailMode : 0\n"
        "FromSTEP.FixShape.MaxTailAngle : 0\n"
        "FromSTEP.FixShape.MaxTailWidth : -1\n"
        "FromSTEP.FixShape.FixSelfIntersectingEdgeMode : -1\n"
        "FromSTEP.FixShape.FixIntersectingEdgesMode : -1\n"
        "FromSTEP.FixShape.FixNonAdjacentIntersectingEdgesMode : -1\n"
        "FromSTEP.FixShape.FixVertexPositionMode : 0\n"
        "FromSTEP.FixShape.FixVertexToleranceMode : -1\n";

void useBigStepRepair(bool big) {
    STEPControl_Controller::Init();   // registers the reader settings (else setting them does nothing)
    static bool written = false;
    if (big && !written) {
        std::string dir = platform::tempDir() + "/occt";
        std::error_code ec;
        std::filesystem::create_directories(u8path(dir), ec);
        if (FILE* f = std::fopen((dir + "/KestralBigSTEP").c_str(), "wb")) {
            std::fputs(kBigStepResource, f);
            std::fclose(f);
        }
#ifdef _WIN32
        _putenv_s("CSF_KestralBigSTEPDefaults", dir.c_str());
#else
        setenv("CSF_KestralBigSTEPDefaults", dir.c_str(), 1);
#endif
        written = true;
    }
    static std::string normal = Interface_Static::CVal("read.step.resource.name") ? Interface_Static::CVal("read.step.resource.name") : "STEP";
    Interface_Static::SetCVal("read.step.resource.name", big ? "KestralBigSTEP" : normal.c_str());
}

void importStep(const std::string& path, ImportResult& r, const CancelToken& cancel, const ProgressFn& progress) {
    Handle(TDocStd_Document) doc;
    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    std::unordered_map<const void*, std::string> solidNamesTmp;
    {
        auto lock = lockStepReader(cancel, progress);
        app->NewDocument("MDTV-XCAF", doc);
        // Body names inside a multi body component live on STEP representation items, which
        // OpenCASCADE skips unless asked (Fusion writes body names there).
        Interface_Static::SetIVal("read.stepcaf.subshapes.name", 1);
        std::error_code sizeEc;
        const bool bigFile = std::filesystem::file_size(u8path(path), sizeEc) > 40ull * 1024 * 1024;
        useBigStepRepair(bigFile);   // OpenCASCADE before 7.8 (Linux distro packages)
        STEPCAFControl_Reader reader;
        reader.SetColorMode(true);
        reader.SetNameMode(true);
        reader.SetLayerMode(false);
#if OCC_VERSION_HEX >= 0x070900
        // OpenCASCADE 7.9 takes the repair settings directly instead of from a resource file.
        if (bigFile) {
            DE_ShapeFixParameters fix;
            fix.FixFaceOrientationMode = DE_ShapeFixParameters::FixMode::NotFix;
            reader.SetShapeFixParameters(fix);
        }
#endif
        {
            std::error_code ec;
            auto bytes = std::filesystem::file_size(u8path(path), ec);
            progress(ec ? std::string("reading the file") : "reading the file (" + std::to_string(std::max<uintmax_t>(bytes >> 20, 1)) + " MB)");
        }
        if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) {
            app->Close(doc);
            throw std::runtime_error("Could not read STEP file (unsupported or damaged)");
        }
        if (cancel.isCancelled()) { app->Close(doc); throw std::runtime_error("cancelled"); }
        progress("converting the geometry");
        Handle(CancelProgress) prog = new CancelProgress(cancel, &progress, "converting the geometry");
        bool transferred = reader.Transfer(doc, prog->Start());
        if (cancel.isCancelled()) { app->Close(doc); throw std::runtime_error("cancelled"); }
        if (!transferred) {
            app->Close(doc);
            throw std::runtime_error("STEP translation failed");
        }
        // Names written on the solids themselves (Fusion, Onshape): map each solid to its entity name.
        try {
            Handle(XSControl_TransferReader) tr = reader.ChangeReader().WS()->TransferReader();
            Handle(XCAFDoc_ShapeTool) st0 = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
            TDF_LabelSequence all;
            st0->GetShapes(all);
            for (int i = 1; i <= all.Length() && !tr.IsNull(); ++i) {
                TopoDS_Shape sh = XCAFDoc_ShapeTool::GetShape(all.Value(i));
                for (TopExp_Explorer ex(sh, TopAbs_SOLID); ex.More(); ex.Next()) {
                    const void* key = (const void*)ex.Current().TShape().get();
                    if (solidNamesTmp.count(key)) continue;
                    Handle(Standard_Transient) ent = tr->EntityFromShapeResult(ex.Current(), -1);
                    Handle(StepRepr_RepresentationItem) ri = Handle(StepRepr_RepresentationItem)::DownCast(ent);
                    if (ri.IsNull() || ri->Name().IsNull()) continue;
                    std::string nm = ri->Name()->ToCString();
                    if (!nm.empty()) solidNamesTmp[key] = nm;
                }
            }
        } catch (...) {}
    }
    StepWalker w{XCAFDoc_DocumentTool::ShapeTool(doc->Main()), XCAFDoc_DocumentTool::ColorTool(doc->Main()), r, cancel, {}};
    w.progress = &progress;
    w.solidNames = std::move(solidNamesTmp);
    TDF_LabelSequence roots;
    w.st->GetFreeShapes(roots);
    progress("building the parts");
    for (int i = 1; i <= roots.Length(); ++i) w.walk(roots.Value(i), TopLoc_Location(), -1, {}, false, {});
    // A single top level assembly becomes the import root itself (no extra nesting).
    if (r.comps.size() >= 1 && roots.Length() == 1 && r.comps[0].parent == -1 && XCAFDoc_ShapeTool::IsAssembly(roots.Value(1))) {
        r.rootName = r.comps[0].name;
        r.firstCompIsRoot = true;
    }
    std::lock_guard lock(g_stepMutex);
    app->Close(doc);
}

// ---------------------------------------------------------------- Mesh helpers

void addSharpEdges(MeshData& m, const std::vector<glm::vec3>& pos, const std::vector<uint32_t>& idx) {
    // Feature lines for mesh bodies: boundary edges and creases sharper than ~35 degrees.
    size_t nt = idx.size() / 3;
    std::vector<glm::vec3> tn(nt);
    for (size_t t = 0; t < nt; ++t) {
        glm::vec3 a = pos[idx[t * 3]], b = pos[idx[t * 3 + 1]], c = pos[idx[t * 3 + 2]];
        glm::vec3 n = glm::cross(b - a, c - a);
        float l = glm::length(n);
        tn[t] = l > 0 ? n / l : glm::vec3(0, 0, 1);
    }
    std::unordered_map<uint64_t, std::pair<int64_t, int64_t>> em;
    em.reserve(idx.size());
    for (size_t t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) {
            uint32_t a = idx[t * 3 + k], b = idx[t * 3 + (k + 1) % 3];
            uint64_t key = a < b ? ((uint64_t)a << 32 | b) : ((uint64_t)b << 32 | a);
            auto [it, ins] = em.try_emplace(key, (int64_t)t, (int64_t)-1);
            if (!ins) it->second.second = (int64_t)t;
        }
    const float cosLimit = std::cos(glm::radians(35.f));
    for (auto& [key, tt] : em) {
        bool sharp = tt.second < 0 || glm::dot(tn[tt.first], tn[tt.second]) < cosLimit;
        if (!sharp) continue;
        glm::vec3 a = pos[key >> 32], b = pos[key & 0xffffffffu];
        m.edgeVerts.insert(m.edgeVerts.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
        m.edgeCount++;
    }
}

MeshPtr buildFlatMesh(const std::vector<glm::vec3>& pos, const std::vector<uint32_t>& idx) {
    auto m = std::make_shared<MeshData>();
    size_t nt = idx.size() / 3;
    m->verts.reserve(nt * 18);
    m->indices.reserve(nt * 3);
    m->triFace.assign(nt, 0);
    for (size_t t = 0; t < nt; ++t) {
        glm::vec3 a = pos[idx[t * 3]], b = pos[idx[t * 3 + 1]], c = pos[idx[t * 3 + 2]];
        glm::vec3 n = glm::cross(b - a, c - a);
        float l = glm::length(n);
        n = l > 0 ? n / l : glm::vec3(0, 0, 1);
        for (auto& p : {a, b, c}) {
            m->verts.insert(m->verts.end(), {p.x, p.y, p.z, n.x, n.y, n.z});
            m->bounds.add(p);
        }
        uint32_t base = (uint32_t)(t * 3);
        m->indices.insert(m->indices.end(), {base, base + 1, base + 2});
    }
    m->faceCount = 1;
    addSharpEdges(*m, pos, idx);
    return m;
}

void importStl(const std::string& path, ImportResult& r) {
    Handle(Poly_Triangulation) tri = RWStl::ReadFile(path.c_str());
    if (tri.IsNull() || tri->NbTriangles() == 0) throw std::runtime_error("Could not read STL file (empty or damaged)");
    std::vector<glm::vec3> pos(tri->NbNodes());
    for (int i = 1; i <= tri->NbNodes(); ++i) {
        gp_Pnt p = tri->Node(i);
        pos[i - 1] = {(float)p.X(), (float)p.Y(), (float)p.Z()};
    }
    std::vector<uint32_t> idx;
    idx.reserve(tri->NbTriangles() * 3);
    for (int t = 1; t <= tri->NbTriangles(); ++t) {
        int a, b, c;
        tri->Triangle(t).Get(a, b, c);
        idx.insert(idx.end(), {(uint32_t)a - 1, (uint32_t)b - 1, (uint32_t)c - 1});
    }
    ImportResult::PartOut p;
    p.name = fileStem(path);
    p.isMesh = true;
    p.mesh = buildFlatMesh(pos, idx);
    r.parts.push_back(std::move(p));
    ImportResult::BodyOut b;
    b.name = fileStem(path);
    b.part = 0;
    r.bodies.push_back(b);
}

void importObj(const std::string& path, ImportResult& r) {
    std::ifstream f(u8path(path));
    if (!f) throw std::runtime_error("Could not open OBJ file");
    std::vector<glm::vec3> pos, outPos;
    std::vector<uint32_t> idx;
    std::string line;
    while (std::getline(f, line)) {
        if (line.size() < 2) continue;
        if (line[0] == 'v' && line[1] == ' ') {
            std::istringstream ss(line.substr(2));
            glm::vec3 p(0);
            ss >> p.x >> p.y >> p.z;
            pos.push_back(p);
        } else if (line[0] == 'f' && line[1] == ' ') {
            std::istringstream ss(line.substr(2));
            std::string tok;
            std::vector<uint32_t> poly;
            while (ss >> tok) {
                long v = std::strtol(tok.c_str(), nullptr, 10);
                if (v < 0) v = (long)pos.size() + v + 1;
                if (v >= 1 && v <= (long)pos.size()) poly.push_back((uint32_t)v - 1);
            }
            for (size_t k = 1; k + 1 < poly.size(); ++k) idx.insert(idx.end(), {poly[0], poly[k], poly[k + 1]});
        }
    }
    if (idx.empty()) throw std::runtime_error("OBJ file has no faces");
    ImportResult::PartOut p;
    p.name = fileStem(path);
    p.isMesh = true;
    p.mesh = buildFlatMesh(pos, idx);
    r.parts.push_back(std::move(p));
    ImportResult::BodyOut b;
    b.name = fileStem(path);
    r.bodies.push_back(b);
}

void appendShapeTriangles(const TopoDS_Shape& s, const glm::mat4& xf, std::vector<float>& out) {
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face& face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull()) continue;
        gp_Trsf tr = loc.Transformation();
        bool rev = face.Orientation() == TopAbs_REVERSED;
        for (int t = 1; t <= tri->NbTriangles(); ++t) {
            int ids[3];
            tri->Triangle(t).Get(ids[0], ids[1], ids[2]);
            if (rev) std::swap(ids[1], ids[2]);
            for (int k = 0; k < 3; ++k) {
                gp_Pnt p = tri->Node(ids[k]).Transformed(tr);
                glm::vec4 w = xf * glm::vec4((float)p.X(), (float)p.Y(), (float)p.Z(), 1.f);
                out.insert(out.end(), {w.x, w.y, w.z});
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------- Tessellation

namespace {
// OpenCASCADE's mesher occasionally gives up on a face (seen on Fusion exported cones whose U range
// is shifted by a full turn). That face would be a hole in the viewport, so it gets a simple
// grid mesh over its UV box instead, keeping only cells inside the face boundary.
void fallbackFaceMesh(const TopoDS_Face& face, uint32_t faceIndex, double lin, MeshData& m) {
    BRepAdaptor_Surface surf(face, Standard_True);
    double u0 = surf.FirstUParameter(), u1 = surf.LastUParameter();
    double v0 = surf.FirstVParameter(), v1 = surf.LastVParameter();
    {
        double bu0, bu1, bv0, bv1;
        BRepTools::UVBounds(face, bu0, bu1, bv0, bv1);
        u0 = bu0; u1 = bu1; v0 = bv0; v1 = bv1;
    }
    if (!(u1 > u0) || !(v1 > v0) || !std::isfinite(u1 - u0) || !std::isfinite(v1 - v0)) return;
    // Resolution from the face's real size: roughly one cell per few deflections, capped.
    gp_Pnt pa = surf.Value(u0, (v0 + v1) * 0.5), pb = surf.Value((u0 + u1) * 0.5, (v0 + v1) * 0.5), pc = surf.Value(u1, (v0 + v1) * 0.5);
    gp_Pnt qa = surf.Value((u0 + u1) * 0.5, v0), qb = surf.Value((u0 + u1) * 0.5, v1);
    double lu = pa.Distance(pb) + pb.Distance(pc), lv = qa.Distance(qb);
    bool curvedU = surf.GetType() != GeomAbs_Plane;
    int nu = std::clamp((int)(curvedU ? 96 : lu / std::max(lin * 20, 1e-3)), 4, 160);
    int nv = std::clamp((int)(lv / std::max(lin * 20, 1e-3)), 2, 64);
    BRepTopAdaptor_FClass2d cls(face, 1e-7);
    bool rev = face.Orientation() == TopAbs_REVERSED;
    std::vector<int> id((size_t)(nu + 1) * (nv + 1), -1);
    auto vert = [&](int i, int j) {
        int& slot = id[(size_t)j * (nu + 1) + i];
        if (slot >= 0) return (uint32_t)slot;
        double u = u0 + (u1 - u0) * i / nu, v = v0 + (v1 - v0) * j / nv;
        gp_Pnt p;
        gp_Vec du, dv;
        surf.D1(u, v, p, du, dv);
        gp_Vec n = du.Crossed(dv);
        if (n.Magnitude() > 1e-12) n.Normalize(); else n = gp_Vec(0, 0, 1);
        if (rev) n.Reverse();
        slot = (int)m.vertexCount();
        m.verts.insert(m.verts.end(), {(float)p.X(), (float)p.Y(), (float)p.Z(), (float)n.X(), (float)n.Y(), (float)n.Z()});
        m.bounds.add(glm::vec3((float)p.X(), (float)p.Y(), (float)p.Z()));
        return (uint32_t)slot;
    };
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            double uc = u0 + (u1 - u0) * (i + 0.5) / nu, vc = v0 + (v1 - v0) * (j + 0.5) / nv;
            if (cls.Perform(gp_Pnt2d(uc, vc)) == TopAbs_OUT) continue;
            uint32_t a = vert(i, j), b = vert(i + 1, j), c = vert(i + 1, j + 1), d = vert(i, j + 1);
            if (rev) {
                m.indices.insert(m.indices.end(), {a, c, b, a, d, c});
            } else {
                m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
            }
            m.triFace.push_back(faceIndex);
            m.triFace.push_back(faceIndex);
        }
}
} // namespace

MeshPtr tessellate(const ShapeHandle& h, double rel, double ang) {
    try {
        const TopoDS_Shape& shape = h.shape;
        Bnd_Box box;
        BRepBndLib::Add(shape, box);
        double diag = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
        double lin = std::clamp(diag * rel, 0.001, 2.0);
        BRepMesh_IncrementalMesh mesher(shape, lin, Standard_False, ang, Standard_False);

        auto m = std::make_shared<MeshData>();
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        TopTools_MapOfShape seams;
        for (int fi = 1; fi <= faces.Extent(); ++fi) {
            const TopoDS_Face& face = TopoDS::Face(faces(fi));
            for (TopExp_Explorer ee(face, TopAbs_EDGE); ee.More(); ee.Next()) {
                const TopoDS_Edge& e = TopoDS::Edge(ee.Current());
                if (BRep_Tool::IsClosed(e, face)) seams.Add(e);
            }
            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull() || tri->NbTriangles() == 0) {
                try { fallbackFaceMesh(face, (uint32_t)(fi - 1), lin, *m); } catch (...) {}
                continue;
            }
            gp_Trsf tr = loc.Transformation();
            bool hasTr = !loc.IsIdentity();
            bool rev = face.Orientation() == TopAbs_REVERSED;
            uint32_t base = (uint32_t)m->vertexCount();
            int nn = tri->NbNodes();

            // Node positions
            std::vector<glm::vec3> P(nn), N(nn, glm::vec3(0));
            for (int n = 1; n <= nn; ++n) {
                gp_Pnt p = tri->Node(n);
                if (hasTr) p.Transform(tr);
                P[n - 1] = {(float)p.X(), (float)p.Y(), (float)p.Z()};
            }
            // Triangles (wound outward)
            size_t idx0 = m->indices.size();
            for (int t = 1; t <= tri->NbTriangles(); ++t) {
                int a, b, c;
                tri->Triangle(t).Get(a, b, c);
                if (rev) std::swap(b, c);
                m->indices.insert(m->indices.end(), {base + a - 1, base + b - 1, base + c - 1});
                m->triFace.push_back((uint32_t)(fi - 1));
                glm::vec3 fn = glm::cross(P[b - 1] - P[a - 1], P[c - 1] - P[a - 1]);
                N[a - 1] += fn; N[b - 1] += fn; N[c - 1] += fn;
            }
            (void)idx0;
            // Exact surface normals where UVs exist, averaged normals otherwise.
            bool useSurf = tri->HasUVNodes();
            BRepAdaptor_Surface surf;
            if (useSurf) {
                try { surf.Initialize(face, Standard_False); } catch (const Standard_Failure&) { useSurf = false; }
            }
            for (int n = 0; n < nn; ++n) {
                glm::vec3 nrm = N[n];
                if (useSurf) {
                    try {
                        gp_Pnt2d uv = tri->UVNode(n + 1);
                        gp_Pnt p;
                        gp_Vec du, dv;
                        surf.D1(uv.X(), uv.Y(), p, du, dv);
                        gp_Vec sn = du.Crossed(dv);
                        if (sn.Magnitude() > 1e-12) {
                            sn.Normalize();
                            if (rev) sn.Reverse();
                            glm::vec3 s((float)sn.X(), (float)sn.Y(), (float)sn.Z());
                            if (glm::dot(s, nrm) >= 0.f || glm::length(nrm) == 0.f) nrm = s;
                        }
                    } catch (const Standard_Failure&) {}
                }
                float l = glm::length(nrm);
                nrm = l > 0 ? nrm / l : glm::vec3(0, 0, 1);
                m->verts.insert(m->verts.end(), {P[n].x, P[n].y, P[n].z, nrm.x, nrm.y, nrm.z});
                m->bounds.add(P[n]);
            }
        }
        m->faceCount = (uint32_t)faces.Extent();

        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(shape, TopAbs_EDGE, edges);
        for (int ei = 1; ei <= edges.Extent(); ++ei) {
            const TopoDS_Edge& e = TopoDS::Edge(edges(ei));
            if (BRep_Tool::Degenerated(e) || seams.Contains(e)) continue;
            try {
                BRepAdaptor_Curve c(e);
                GCPnts_TangentialDeflection disc(c, ang * 0.5, lin);
                for (int i = 1; i < disc.NbPoints(); ++i) {
                    gp_Pnt a = disc.Value(i), b = disc.Value(i + 1);
                    m->edgeVerts.insert(m->edgeVerts.end(), {(float)a.X(), (float)a.Y(), (float)a.Z(), (float)b.X(), (float)b.Y(), (float)b.Z()});
                }
                m->edgeCount++;
            } catch (const Standard_Failure&) {
                // Skip a bad edge, never fail the whole body for it.
            }
        }
        return m;
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "tessellation");
    }
}

// ---------------------------------------------------------------- Import

namespace {

ImportResult readImport(const std::string& path, const CancelToken& cancel, const ProgressFn& progress) {
    ImportResult r;
    r.rootName = fileStem(path);
    std::string ext = toLower(pathToU8(u8path(path).extension()));
    try {
        if (ext == ".step" || ext == ".stp") importStep(path, r, cancel, progress);
        else if (ext == ".stl") { progress("Reading STL"); importStl(path, r); }
        else if (ext == ".obj") { progress("Reading OBJ"); importObj(path, r); }
        else throw std::runtime_error("Unsupported file type: " + ext);
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "import");
    }
    if (r.bodies.empty()) throw std::runtime_error("No bodies found in file");
    return r;
}

// Display meshes and Fusion style name numbering. Results read back from the import cache are
// already numbered.
void finishImport(ImportResult& r, const CancelToken& cancel, const ProgressFn& progress, bool numberNames) {
    // Tessellate unique B-rep parts in parallel. A part that fails is skipped with a warning.
    std::vector<int> todo;
    for (int i = 0; i < (int)r.parts.size(); ++i)
        if (r.parts[i].shape && !r.parts[i].mesh) todo.push_back(i);
    if (!todo.empty()) {
        std::atomic<int> next{0}, done{0};
        std::mutex wm;
        unsigned nt = std::max(1u, std::min<unsigned>((unsigned)todo.size(), std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 2 : 1));
        auto worker = [&] {
            platform::installThreadGuard();
            for (;;) {
                int k = next++;
                if (k >= (int)todo.size() || cancel.isCancelled()) return;
                auto& p = r.parts[todo[k]];
                std::string err;
                bool ok = platform::guarded("tessellate", [&] {
                    MeshPtr mp = tessellate(*p.shape);
                    if (mp && !p.faceColors.empty() && p.faceColors.size() == mp->faceCount)
                        std::const_pointer_cast<MeshData>(mp)->faceColors = p.faceColors;
                    p.mesh = mp;
                }, err);
                if (!ok) {
                    std::lock_guard lock(wm);
                    r.warnings.push_back("Could not display '" + p.name + "': " + err);
                }
                int d = ++done;
                if (d % 8 == 0 || d == (int)todo.size())
                    progress("making the display mesh " + std::to_string(d) + " of " + std::to_string(todo.size()));
            }
        };
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < nt; ++i) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
        if (cancel.isCancelled()) throw std::runtime_error("cancelled");
    }
    // Number repeated names like Fusion does (Link:1, Link:2, ...), but only inside one folder:
    // Body1 in Motors and Body1 in Head Assembly are both just Body1, as in Fusion.
    if (numberNames) {
        std::unordered_map<std::string, int> total, seen;
        auto key = [](const ImportResult::BodyOut& b) { return std::to_string(b.comp) + "|" + b.name; };
        for (auto& b : r.bodies) total[key(b)]++;
        for (auto& b : r.bodies)
            if (total[key(b)] > 1) b.name += ":" + std::to_string(++seen[key(b)]);
    }
    // Bodies whose part failed to tessellate keep an empty mesh (still listed, still exportable).
    for (auto& p : r.parts)
        if (!p.mesh) { auto e = std::make_shared<MeshData>(); p.mesh = e; }
}

// ---- Import cache: a STEP file read once is kept as OpenCASCADE's binary format (with its
// display triangles), so opening the design again skips the slow STEP translation.

constexpr uint32_t kCacheMagic = 0x504D494Bu;   // "KIMP"
constexpr uint32_t kCacheVersion = 2;   // 2: solid entity names
constexpr uintmax_t kCacheMinBytes = 5ull << 20;     // smaller files read fast enough as they are
constexpr uintmax_t kWorkerMinBytes = 20ull << 20;   // bigger ones are read by a separate Kestral process

void putU32(std::ostream& o, uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); }
void putF(std::ostream& o, float v) { o.write(reinterpret_cast<const char*>(&v), 4); }
void putS(std::ostream& o, const std::string& s) {
    putU32(o, (uint32_t)s.size());
    o.write(s.data(), (std::streamsize)s.size());
}
uint32_t getU32(std::istream& i) {
    uint32_t v = 0;
    i.read(reinterpret_cast<char*>(&v), 4);
    if (!i) throw std::runtime_error("import cache is cut short");
    return v;
}
float getF(std::istream& i) {
    float v = 0;
    i.read(reinterpret_cast<char*>(&v), 4);
    if (!i) throw std::runtime_error("import cache is cut short");
    return v;
}
std::string getS(std::istream& i) {
    uint32_t n = getU32(i);
    if (n > (64u << 20)) throw std::runtime_error("import cache is damaged");
    std::string s(n, '\0');
    i.read(s.data(), n);
    if (!i) throw std::runtime_error("import cache is cut short");
    return s;
}

std::string importCachePath(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = u8path(path);
    uintmax_t size = fs::file_size(p, ec);
    if (ec) return {};
    auto mt = fs::last_write_time(p, ec);
    if (ec) return {};
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* d, size_t n) {
        auto* b = static_cast<const unsigned char*>(d);
        for (size_t k = 0; k < n; ++k) { h ^= b[k]; h *= 1099511628211ull; }
    };
    std::string full = pathToU8(fs::absolute(p, ec));
    mix(full.data(), full.size());
    mix(&size, sizeof size);
    long long t = (long long)mt.time_since_epoch().count();
    mix(&t, sizeof t);
    mix(&kCacheVersion, sizeof kCacheVersion);
    char name[40];
    std::snprintf(name, sizeof name, "%016llx.kimp", (unsigned long long)h);
    return platform::tempDir() + "/importcache/" + name;
}

bool saveImportCache(const ImportResult& r, const std::string& file) {
    namespace fs = std::filesystem;
    for (auto& p : r.parts)
        if (!p.shape || p.isMesh) return false;   // STEP results only: every part is a B-rep
    std::error_code ec;
    fs::create_directories(u8path(file).parent_path(), ec);
    std::string tmp = file + ".part";
    {
        std::ofstream o(u8path(tmp), std::ios::binary | std::ios::trunc);
        if (!o) return false;
        putU32(o, kCacheMagic);
        putU32(o, kCacheVersion);
        putS(o, r.rootName);
        putU32(o, r.firstCompIsRoot ? 1u : 0u);
        putU32(o, (uint32_t)r.comps.size());
        for (auto& c : r.comps) { putS(o, c.name); putU32(o, (uint32_t)(c.parent + 1)); }
        putU32(o, (uint32_t)r.bodies.size());
        for (auto& b : r.bodies) {
            putS(o, b.name);
            putU32(o, (uint32_t)b.part);
            putU32(o, (uint32_t)(b.comp + 1));
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k < 4; ++k) putF(o, b.xform[c][k]);
            putU32(o, b.hasColor ? 1u : 0u);
            putF(o, b.color.r); putF(o, b.color.g); putF(o, b.color.b);
            putS(o, b.occPath);
            putS(o, b.compDef);
            putS(o, b.bodyName);
        }
        putU32(o, (uint32_t)r.warnings.size());
        for (auto& w : r.warnings) putS(o, w);
        putU32(o, (uint32_t)r.parts.size());
        for (auto& p : r.parts) {
            putS(o, p.name);
            putU32(o, (uint32_t)p.faceColors.size());
            if (!p.faceColors.empty()) o.write(reinterpret_cast<const char*>(p.faceColors.data()), (std::streamsize)(p.faceColors.size() * 4));
        }
        BRep_Builder bb;
        TopoDS_Compound all;
        bb.MakeCompound(all);
        for (auto& p : r.parts) bb.Add(all, p.shape->shape);
        try {
            BinTools::Write(all, o, Standard_True, Standard_False, BinTools_FormatVersion_CURRENT);
        } catch (const Standard_Failure&) {
            o.close();
            fs::remove(u8path(tmp), ec);
            return false;
        }
        if (!o) { o.close(); fs::remove(u8path(tmp), ec); return false; }
    }
    fs::rename(u8path(tmp), u8path(file), ec);
    if (ec) { fs::remove(u8path(tmp), ec); return false; }
    return true;
}

bool loadImportCache(const std::string& file, ImportResult& r) {
    std::error_code ec;
    if (file.empty() || !std::filesystem::exists(u8path(file), ec)) return false;
    try {
        std::ifstream in(u8path(file), std::ios::binary);
        if (!in || getU32(in) != kCacheMagic || getU32(in) != kCacheVersion) return false;
        ImportResult out;
        out.rootName = getS(in);
        out.firstCompIsRoot = getU32(in) != 0;
        uint32_t nc = getU32(in);
        for (uint32_t i = 0; i < nc; ++i) {
            ImportResult::CompOut c;
            c.name = getS(in);
            c.parent = (int)getU32(in) - 1;
            out.comps.push_back(c);
        }
        uint32_t nb = getU32(in);
        for (uint32_t i = 0; i < nb; ++i) {
            ImportResult::BodyOut b;
            b.name = getS(in);
            b.part = (int)getU32(in);
            b.comp = (int)getU32(in) - 1;
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k < 4; ++k) b.xform[c][k] = getF(in);
            b.hasColor = getU32(in) != 0;
            b.color.r = getF(in); b.color.g = getF(in); b.color.b = getF(in);
            b.occPath = getS(in);
            b.compDef = getS(in);
            b.bodyName = getS(in);
            out.bodies.push_back(std::move(b));
        }
        uint32_t nw = getU32(in);
        for (uint32_t i = 0; i < nw; ++i) out.warnings.push_back(getS(in));
        uint32_t np = getU32(in);
        for (uint32_t i = 0; i < np; ++i) {
            ImportResult::PartOut p;
            p.name = getS(in);
            uint32_t nf = getU32(in);
            if (nf > (64u << 20)) return false;
            p.faceColors.resize(nf);
            if (nf) {
                in.read(reinterpret_cast<char*>(p.faceColors.data()), (std::streamsize)nf * 4);
                if (!in) return false;
            }
            out.parts.push_back(std::move(p));
        }
        TopoDS_Shape all;
        BinTools::Read(all, in);
        if (all.IsNull()) return false;
        uint32_t k = 0;
        for (TopoDS_Iterator it(all); it.More(); it.Next(), ++k) {
            if (k >= np) return false;
            out.parts[k].shape = wrap(it.Value());
        }
        if (k != np) return false;
        for (auto& b : out.bodies)
            if (b.part < 0 || b.part >= (int)np) return false;
        if (out.bodies.empty()) return false;
        r = std::move(out);
        return true;
    } catch (...) {
        return false;
    }
}

constexpr uint32_t kFeatMagic = 0x5446454Bu;   // "KEFT"
constexpr uint32_t kFeatVersion = 1;

std::string featureCachePath(uint64_t key) {
    char name[40];
    std::snprintf(name, sizeof name, "%016llx.kfc", (unsigned long long)key);
    return platform::tempDir() + "/featcache/" + name;
}

// Keeps the feature cache under about 2 GB: oldest files go first.
void pruneFeatureCache() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = u8path(platform::tempDir() + "/featcache");
    std::vector<std::pair<fs::file_time_type, std::pair<fs::path, uintmax_t>>> files;
    uintmax_t total = 0;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        uintmax_t sz = e.file_size(ec);
        files.push_back({e.last_write_time(ec), {e.path(), sz}});
        total += sz;
    }
    // Results nobody opened for a week go first (opening a design touches its files), then the
    // oldest while the cache is over its size.
    auto now = fs::file_time_type::clock::now();
    for (auto& f : files)
        if (now - f.first > std::chrono::hours(24 * 7)) {
            fs::remove(f.second.first, ec);
            total -= f.second.second;
            f.second.second = 0;
        }
    constexpr uintmax_t kMax = 4ull << 30;
    if (total <= kMax) return;
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& f : files) {
        if (total <= kMax * 3 / 4) break;
        if (!f.second.second) continue;
        fs::remove(f.second.first, ec);
        total -= f.second.second;
    }
}

} // namespace

bool saveFeatureCache(uint64_t key, const FeatureCacheData& d) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string file = featureCachePath(key);
    fs::create_directories(u8path(file).parent_path(), ec);
    std::string tmp = file + ".part";
    {
        std::ofstream o(u8path(tmp), std::ios::binary | std::ios::trunc);
        if (!o) return false;
        putU32(o, kFeatMagic);
        putU32(o, kFeatVersion);
        o.write(reinterpret_cast<const char*>(&key), 8);
        putS(o, d.warn);
        putU32(o, (uint32_t)d.shapes.size());
        BRep_Builder bb;
        TopoDS_Compound all;
        bb.MakeCompound(all);
        for (size_t i = 0; i < d.shapes.size(); ++i) {
            putU32(o, d.shapes[i] ? 1u : 0u);
            putS(o, i < d.names.size() ? d.names[i] : std::string());
            glm::mat4 x = i < d.xforms.size() ? d.xforms[i] : glm::mat4(1.f);
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k < 4; ++k) putF(o, x[c][k]);
            auto ir = i < d.instanceRefs.size() ? d.instanceRefs[i] : std::pair<uint32_t, uint32_t>{0, 0};
            putU32(o, ir.first); putU32(o, ir.second);
            if (d.shapes[i]) bb.Add(all, d.shapes[i]->shape);
        }
        putU32(o, (uint32_t)d.xforms.size());
        putU32(o, (uint32_t)d.instanceRefs.size());
        putU32(o, (uint32_t)d.consumed.size());
        for (auto& c : d.consumed) { putU32(o, c.first); putU32(o, c.second); }
        try {
            BinTools::Write(all, o, Standard_True, Standard_False, BinTools_FormatVersion_CURRENT);
        } catch (const Standard_Failure&) {
            o.close();
            fs::remove(u8path(tmp), ec);
            return false;
        }
        if (!o) { o.close(); fs::remove(u8path(tmp), ec); return false; }
    }
    fs::rename(u8path(tmp), u8path(file), ec);
    if (ec) { fs::remove(u8path(tmp), ec); return false; }
    pruneFeatureCache();
    return true;
}

bool loadFeatureCache(uint64_t key, FeatureCacheData& d) {
    std::error_code ec;
    std::string file = featureCachePath(key);
    if (!std::filesystem::exists(u8path(file), ec)) return false;
    try {
        std::ifstream in(u8path(file), std::ios::binary);
        if (!in || getU32(in) != kFeatMagic || getU32(in) != kFeatVersion) return false;
        uint64_t k2 = 0;
        in.read(reinterpret_cast<char*>(&k2), 8);
        if (!in || k2 != key) return false;
        FeatureCacheData out;
        out.warn = getS(in);
        uint32_t n = getU32(in);
        if (n > 100000) return false;
        std::vector<bool> has(n);
        for (uint32_t i = 0; i < n; ++i) {
            has[i] = getU32(in) != 0;
            out.names.push_back(getS(in));
            glm::mat4 x;
            for (int c = 0; c < 4; ++c)
                for (int kk = 0; kk < 4; ++kk) x[c][kk] = getF(in);
            out.xforms.push_back(x);
            uint32_t a = getU32(in), b = getU32(in);
            out.instanceRefs.push_back({a, b});
        }
        uint32_t nx = getU32(in), ni = getU32(in), nc = getU32(in);
        if (nx < out.xforms.size()) out.xforms.resize(nx);
        if (ni < out.instanceRefs.size()) out.instanceRefs.resize(ni);
        if (nc > 100000) return false;
        for (uint32_t i = 0; i < nc; ++i) { uint32_t a = getU32(in), b = getU32(in); out.consumed.push_back({a, b}); }
        TopoDS_Shape all;
        BinTools::Read(all, in);
        TopoDS_Iterator it;
        if (!all.IsNull()) it.Initialize(all);
        for (uint32_t i = 0; i < n; ++i) {
            if (!has[i]) { out.shapes.push_back(nullptr); continue; }
            if (all.IsNull() || !it.More()) return false;
            out.shapes.push_back(wrap(it.Value()));
            it.Next();
        }
        // Touch it so pruning keeps the files in use.
        std::filesystem::last_write_time(u8path(file), std::filesystem::file_time_type::clock::now(), ec);
        d = std::move(out);
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

std::string readSmallText(const std::string& file) {
    std::ifstream f(u8path(file), std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// Reads a big STEP file in a second Kestral process. Stop ends that process at once (an
// OpenCASCADE loop inside this one could not be interrupted), and a crash in it never takes
// Kestral down. False when the helper could not start (the caller reads the file itself).
bool importInWorker(const std::string& path, const std::string& cache, const CancelToken& cancel,
                    const ProgressFn& progress, ImportResult& r) {
    std::string exe = platform::selfExe();
    if (exe.empty() || cache.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(u8path(cache).parent_path(), ec);
    std::string prog = cache + ".progress";
    std::filesystem::remove(u8path(prog), ec);
    platform::ProcResult pr;
    std::atomic<bool> finished{false};
    std::thread t([&] {
        pr = platform::runProcess(exe, {"--import-worker", path, cache, prog}, 0, &cancel.cancelled);
        finished = true;
    });
    std::string last;
    while (!finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::string s = readSmallText(prog);
        if (!s.empty() && s != last && s.rfind("ERROR ", 0) != 0) {
            last = s;
            if (progress) progress(s);
        }
    }
    t.join();
    std::string tail = readSmallText(prog);
    std::filesystem::remove(u8path(prog), ec);
    if (cancel.isCancelled()) throw std::runtime_error("cancelled");
    if (pr.exitCode != 0) {
        if (tail.rfind("ERROR ", 0) == 0) throw std::runtime_error(tail.substr(6));
        if (pr.error.rfind("Could not start", 0) == 0) return false;
        throw std::runtime_error("The STEP reader stopped unexpectedly (code " + std::to_string(pr.exitCode) + ")");
    }
    if (!loadImportCache(cache, r)) throw std::runtime_error("Could not read the converted file back");
    return true;
}

} // namespace

ImportResult importFile(const std::string& path, const CancelToken& cancel, const ProgressFn& progress) {
    std::string ext = toLower(pathToU8(u8path(path).extension()));
    bool step = ext == ".step" || ext == ".stp";
    std::error_code ec;
    uintmax_t bytes = std::filesystem::file_size(u8path(path), ec);
    if (ec) bytes = 0;
    std::string cache = step && bytes >= kCacheMinBytes ? importCachePath(path) : std::string();
    if (!cache.empty()) {
        ImportResult r;
        if (loadImportCache(cache, r)) {
            if (progress) progress("reading the saved copy");
            finishImport(r, cancel, progress, false);
            return r;
        }
        if (bytes >= kWorkerMinBytes && importInWorker(path, cache, cancel, progress, r)) {
            finishImport(r, cancel, progress, false);
            return r;
        }
    }
    ImportResult r = readImport(path, cancel, progress);
    finishImport(r, cancel, progress, true);
    if (!cache.empty()) saveImportCache(r, cache);
    return r;
}

int runImportWorker(const std::string& path, const std::string& cache, const std::string& progressFile) {
    platform::installThreadGuard();
    auto say = [&](const std::string& s) {
        std::ofstream f(u8path(progressFile), std::ios::binary | std::ios::trunc);
        f << s;
    };
    CancelToken never;
    std::string err;
    int code = 0;
    bool ok = platform::guarded("import worker", [&] {
        ImportResult r = readImport(path, never, say);
        finishImport(r, never, say, true);
        say("saving the converted file");
        if (!saveImportCache(r, cache)) { say("ERROR Could not save the converted file"); code = 3; }
    }, err);
    if (!ok) {
        say("ERROR " + (err.empty() ? std::string("could not read the file") : err));
        return 2;
    }
    return code;
}

// ---------------------------------------------------------------- Export

void exportStl(const std::vector<ExportItem>& items, const std::string& path, const CancelToken& cancel) {
    std::vector<float> tris;   // 9 floats per triangle
    try {
        for (auto& it : items) {
            if (cancel.isCancelled()) throw std::runtime_error("cancelled");
            if (it.shape) {
                BRepBuilderAPI_Copy cp(it.shape->shape, Standard_True, Standard_False);
                TopoDS_Shape s = cp.Shape();
                Bnd_Box box;
                BRepBndLib::Add(s, box);
                double diag = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
                double lin = std::clamp(diag * 0.0002, 0.002, 0.01);
                BRepMesh_IncrementalMesh mesher(s, lin, Standard_False, 0.1, Standard_True);
                appendShapeTriangles(s, it.xform, tris);
            } else if (it.mesh) {
                auto& m = *it.mesh;
                for (size_t i = 0; i < m.indices.size(); ++i) {
                    const float* v = &m.verts[m.indices[i] * 6];
                    glm::vec4 w = it.xform * glm::vec4(v[0], v[1], v[2], 1.f);
                    tris.insert(tris.end(), {w.x, w.y, w.z});
                }
            }
        }
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "export");
    }
    if (tris.empty()) throw std::runtime_error("Nothing to export");
    std::ofstream f(u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write " + path);
    char header[80] = {};
    std::snprintf(header, sizeof header, "Kestral CAD binary STL");
    f.write(header, 80);
    uint32_t n = (uint32_t)(tris.size() / 9);
    f.write(reinterpret_cast<const char*>(&n), 4);
    std::vector<char> buf(50);
    for (uint32_t t = 0; t < n; ++t) {
        const float* p = &tris[t * 9];
        glm::vec3 a(p[0], p[1], p[2]), b(p[3], p[4], p[5]), c(p[6], p[7], p[8]);
        glm::vec3 nn = glm::cross(b - a, c - a);
        float l = glm::length(nn);
        nn = l > 0 ? nn / l : glm::vec3(0);
        float rec[12] = {nn.x, nn.y, nn.z, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]};
        std::memcpy(buf.data(), rec, 48);
        buf[48] = buf[49] = 0;
        f.write(buf.data(), 50);
    }
    if (!f) throw std::runtime_error("Write failed (disk full?)");
}

// ---------------------------------------------------------------- STEP / 3MF

void exportStep(const std::vector<ExportItem>& items, const std::string& path, const CancelToken& cancel) {
    try {
        // Through an XCAF document so every body keeps its name and color (Fusion and slicers
        // show "Body1", not "Open CASCADE STEP"). A body in a component is nested under that
        // folder, and the whole file sits in one assembly so the folders survive a reimport.
        Handle(TDocStd_Document) doc;
        XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
        Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());
        bool nested = false;
        for (auto& it : items)
            if (it.shape && !it.component.empty()) nested = true;
        auto placed = [&](const ExportItem& it) {
            gp_Trsf tr;
            const glm::mat4& m = it.xform;
            tr.SetValues(m[0][0], m[1][0], m[2][0], m[3][0],
                         m[0][1], m[1][1], m[2][1], m[3][1],
                         m[0][2], m[1][2], m[2][2], m[3][2]);
            return it.shape->shape.Moved(TopLoc_Location(tr));
        };
        auto nameOf = [](const ExportItem& it, int n) {
            return it.name.empty() ? "Body" + std::to_string(n) : it.name;
        };
        int n = 0;
        if (!nested) {
            for (auto& it : items) {
                if (cancel.isCancelled()) throw std::runtime_error("cancelled");
                if (!it.shape) continue;
                TDF_Label l = st->AddShape(placed(it), false);
                std::string nm = nameOf(it, n + 1);
                TDataStd_Name::Set(l, TCollection_ExtendedString(nm.c_str(), true));
                ct->SetColor(l, Quantity_Color(it.color.r, it.color.g, it.color.b, Quantity_TOC_RGB), XCAFDoc_ColorSurf);
                ++n;
            }
        } else {
            auto makeAsm = [&](const std::string& nm) {
                TopoDS_Compound compound;
                BRep_Builder builder;
                builder.MakeCompound(compound);
                TDF_Label lab = st->AddShape(compound, true);
                TDataStd_Name::Set(lab, TCollection_ExtendedString(nm.c_str(), true));
                return lab;
            };
            TDF_Label root = makeAsm("Product");
            std::map<std::string, TDF_Label> folders;
            folders[""] = root;
            std::function<TDF_Label(const std::string&)> folder = [&](const std::string& comp) -> TDF_Label {
                if (comp.empty()) return root;
                auto it = folders.find(comp);
                if (it != folders.end()) return it->second;
                auto slash = comp.rfind('/');
                std::string parent = slash == std::string::npos ? std::string() : comp.substr(0, slash);
                std::string nm = slash == std::string::npos ? comp : comp.substr(slash + 1);
                TDF_Label parentLab = folder(parent);
                TDF_Label lab = makeAsm(nm);
                TDF_Label inst = st->AddComponent(parentLab, lab, TopLoc_Location());
                if (inst.IsNull()) throw std::runtime_error("Could not add component " + nm);
                TDataStd_Name::Set(inst, TCollection_ExtendedString(nm.c_str(), true));
                folders[comp] = lab;
                return lab;
            };
            for (auto& it : items) {
                if (cancel.isCancelled()) throw std::runtime_error("cancelled");
                if (!it.shape) continue;
                TDF_Label parent = folder(it.component);
                TDF_Label inst = st->AddComponent(parent, placed(it), false);
                if (inst.IsNull()) throw std::runtime_error("Could not add " + nameOf(it, n + 1));
                std::string nm = nameOf(it, n + 1);
                TCollection_ExtendedString en(nm.c_str(), true);
                TDataStd_Name::Set(inst, en);
                TDF_Label ref;
                if (XCAFDoc_ShapeTool::GetReferredShape(inst, ref) && !ref.IsNull()) {
                    TDataStd_Name::Set(ref, en);
                    ct->SetColor(ref, Quantity_Color(it.color.r, it.color.g, it.color.b, Quantity_TOC_RGB), XCAFDoc_ColorSurf);
                }
                ++n;
            }
            st->UpdateAssemblies();
        }
        if (n == 0) throw std::runtime_error("Nothing solid to export (mesh bodies are skipped in STEP)");
        Interface_Static::SetIVal("write.stepcaf.subshapes.name", 1);
        STEPCAFControl_Writer writer;
        writer.SetNameMode(true);
        writer.SetColorMode(true);
        if (!writer.Transfer(doc, STEPControl_AsIs))
            throw std::runtime_error("STEP transfer failed on one of the bodies");
        if (writer.Write(path.c_str()) != IFSelect_RetDone)
            throw std::runtime_error("Could not write the STEP file");
        XCAFApp_Application::GetApplication()->Close(doc);
    } catch (const Standard_Failure& e) {
        rethrowOcct(e, "STEP export");
    }
}

namespace {

uint32_t crc32(const void* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void putU16(std::string& s, uint16_t v) { s.push_back(char(v & 0xFF)); s.push_back(char(v >> 8)); }
void putU32(std::string& s, uint32_t v) {
    s.push_back(char(v & 0xFF));
    s.push_back(char((v >> 8) & 0xFF));
    s.push_back(char((v >> 16) & 0xFF));
    s.push_back(char((v >> 24) & 0xFF));
}

// Minimal OPC zip writer with STORED (uncompressed) entries: 3MF allows it and slicers accept it.
struct ZipWriter {
    std::string out;
    struct Entry { uint32_t crc = 0, size = 0, offset = 0; std::string name; };
    std::vector<Entry> entries;
    void add(const std::string& name, const std::string& data) {
        Entry e;
        e.crc = crc32(data.data(), data.size());
        e.size = (uint32_t)data.size();
        e.offset = (uint32_t)out.size();
        e.name = name;
        putU32(out, 0x04034b50);
        putU16(out, 20);
        putU16(out, 0);
        putU16(out, 0);
        putU16(out, 0); putU16(out, 33);   // time, date
        putU32(out, e.crc);
        putU32(out, e.size);
        putU32(out, e.size);
        putU16(out, (uint16_t)name.size());
        putU16(out, 0);
        out += name;
        out += data;
        entries.push_back(e);
    }
    std::string finish() {
        uint32_t cdStart = (uint32_t)out.size();
        for (auto& e : entries) {
            putU32(out, 0x02014b50);
            putU16(out, 20); putU16(out, 20);
            putU16(out, 0); putU16(out, 0);
            putU16(out, 0); putU16(out, 33);   // time, date (1980-01-01): these were missing, so readers rejected the file
            putU32(out, e.crc); putU32(out, e.size); putU32(out, e.size);
            putU16(out, (uint16_t)e.name.size());
            putU16(out, 0); putU16(out, 0); putU16(out, 0); putU16(out, 0);
            putU32(out, 0);
            putU32(out, e.offset);
            out += e.name;
        }
        uint32_t cdSize = (uint32_t)out.size() - cdStart;
        putU32(out, 0x06054b50);
        putU16(out, 0); putU16(out, 0);
        putU16(out, (uint16_t)entries.size());
        putU16(out, (uint16_t)entries.size());
        putU32(out, cdSize);
        putU32(out, cdStart);
        putU16(out, 0);
        return out;
    }
};

} // namespace

void export3mf(const std::vector<ExportItem>& items, const std::string& path, const CancelToken& cancel) {
    // One 3MF object per body, with shared (welded) vertices, as slicers expect.
    auto xmlEsc = [](const std::string& in) {
        std::string o;
        for (char c : in) {
            if (c == '&') o += "&amp;";
            else if (c == '<') o += "&lt;";
            else if (c == '>') o += "&gt;";
            else if (c == '"') o += "&quot;";
            else if ((unsigned char)c >= 32 || c == '\t') o += c;
        }
        return o;
    };
    std::string objects, build, objCfg;
    int objId = 0;
    char buf[200];
    for (auto& it : items) {
        if (cancel.isCancelled()) throw std::runtime_error("cancelled");
        std::vector<float> tris;
        const bool painted = it.mesh && (!it.triColor.empty() || !it.triSupport.empty() || !it.triSeam.empty());
        if (it.shape && !painted) {
            BRepBuilderAPI_Copy cp(it.shape->shape, Standard_True, Standard_False);
            TopoDS_Shape sh = cp.Shape();
            Bnd_Box box;
            BRepBndLib::Add(sh, box);
            double diag = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
            double lin = std::clamp(diag * 0.0004, 0.004, 0.02);
            BRepMesh_IncrementalMesh mesher(sh, lin, Standard_False, 0.1, Standard_True);
            appendShapeTriangles(sh, it.xform, tris);
        } else if (it.mesh) {
            auto& m = *it.mesh;
            for (size_t i = 0; i < m.indices.size(); ++i) {
                const float* v = &m.verts[m.indices[i] * 6];
                glm::vec4 w = it.xform * glm::vec4(v[0], v[1], v[2], 1.f);
                tris.insert(tris.end(), {w.x, w.y, w.z});
            }
        }
        if (tris.size() < 9) continue;
        // Weld identical corners (0.1 micron grid) and drop triangles that collapse.
        std::unordered_map<uint64_t, uint32_t> index;
        std::vector<float> verts;
        std::vector<uint32_t> faces;
        std::vector<uint32_t> faceSrc;   // source triangle of each kept face (painting)
        auto vid = [&](size_t k) {
            long long qx = std::llround(tris[k] * 10000.0), qy = std::llround(tris[k + 1] * 10000.0), qz = std::llround(tris[k + 2] * 10000.0);
            uint64_t key = (uint64_t)(qx * 73856093LL) ^ ((uint64_t)(qy * 19349663LL) << 1) ^ ((uint64_t)(qz * 83492791LL) << 2);
            auto f = index.find(key);
            if (f != index.end()) {
                const float* p = &verts[f->second * 3];
                if (std::llround(p[0] * 10000.0) == qx && std::llround(p[1] * 10000.0) == qy && std::llround(p[2] * 10000.0) == qz) return f->second;
                // Hash clash: keep it as its own vertex.
                uint32_t n = (uint32_t)(verts.size() / 3);
                verts.insert(verts.end(), {tris[k], tris[k + 1], tris[k + 2]});
                return n;
            }
            uint32_t n = (uint32_t)(verts.size() / 3);
            verts.insert(verts.end(), {tris[k], tris[k + 1], tris[k + 2]});
            index.emplace(key, n);
            return n;
        };
        for (size_t t = 0; t + 8 < tris.size(); t += 9) {
            uint32_t a = vid(t), b = vid(t + 3), c = vid(t + 6);
            if (a == b || b == c || a == c) continue;
            faces.insert(faces.end(), {a, b, c});
            faceSrc.push_back((uint32_t)(t / 9));
        }
        if (faces.empty()) continue;
        ++objId;
        std::string nm = it.name.empty() ? "Body " + std::to_string(objId) : it.name;
        objects += "<object id=\"" + std::to_string(objId) + "\" name=\"" + xmlEsc(nm) + "\" type=\"model\"><mesh><vertices>";
        for (size_t i = 0; i + 2 < verts.size(); i += 3) {
            std::snprintf(buf, sizeof buf, "<vertex x=\"%.6f\" y=\"%.6f\" z=\"%.6f\"/>", verts[i], verts[i + 1], verts[i + 2]);
            objects += buf;
        }
        objects += "</vertices><triangles>";
        // Orca's painting codes: a whole triangle in state s is "4" (1), "8" (2), else "<s-3>C".
        auto code = [](int st) -> std::string {
            if (st <= 0) return {};
            if (st == 1) return "4";
            if (st == 2) return "8";
            char b[8];
            std::snprintf(b, sizeof b, "%XC", std::min(st - 3, 15));
            return b;
        };
        for (size_t i = 0; i + 2 < faces.size(); i += 3) {
            std::snprintf(buf, sizeof buf, "<triangle v1=\"%u\" v2=\"%u\" v3=\"%u\"", faces[i], faces[i + 1], faces[i + 2]);
            objects += buf;
            if (painted) {
                uint32_t src = faceSrc[i / 3];
                if (src < it.triColor.size() && it.triColor[src]) objects += " paint_color=\"" + code(it.triColor[src]) + "\"";
                if (src < it.triSupport.size() && it.triSupport[src]) objects += " paint_supports=\"" + code(it.triSupport[src]) + "\"";
                if (src < it.triSeam.size() && it.triSeam[src]) objects += " paint_seam=\"" + code(it.triSeam[src]) + "\"";
            }
            objects += "/>";
        }
        objects += "</triangles></mesh></object>";
        build += "<item objectid=\"" + std::to_string(objId) + "\"/>";
        if (it.extruder > 0 || !it.settings.empty()) {   // OrcaSlicer's per object filament and settings (Metadata/model_settings.config)
            objCfg += "<object id=\"" + std::to_string(objId) + "\"><metadata key=\"name\" value=\"" + xmlEsc(nm) + "\"/>";
            if (it.extruder > 0) objCfg += "<metadata key=\"extruder\" value=\"" + std::to_string(it.extruder) + "\"/>";
            for (auto& [k, v] : it.settings) objCfg += "<metadata key=\"" + xmlEsc(k) + "\" value=\"" + xmlEsc(v) + "\"/>";
            objCfg += "</object>";
        }
    }
    if (objId == 0) throw std::runtime_error("Nothing to export");
    std::string model;
    model += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    model += "<model unit=\"millimeter\" xml:lang=\"en-US\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">";
    model += "<metadata name=\"Application\">Kestral</metadata>";
    model += "<resources>" + objects + "</resources><build>" + build + "</build></model>";

    ZipWriter zip;
    std::string ct = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    ct += "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">";
    ct += "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>";
    ct += "<Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>";
    ct += "</Types>";
    zip.add("[Content_Types].xml", ct);
    std::string rels = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    rels += "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    rels += "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" ";
    rels += "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>";
    rels += "</Relationships>";
    zip.add("_rels/.rels", rels);
    zip.add("3D/3dmodel.model", model);
    if (!objCfg.empty())
        zip.add("Metadata/model_settings.config", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<config>" + objCfg + "</config>");
    std::string data = zip.finish();
    std::ofstream f(u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write " + path);
    f.write(data.data(), (std::streamsize)data.size());
    if (!f) throw std::runtime_error("Write failed (disk full?)");
}

bool facePlane(const ShapeHandle& h, uint32_t faceIndex, glm::dvec3& o, glm::dvec3& n) {
    try {
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(h.shape, TopAbs_FACE, faces);
        if ((int)faceIndex + 1 > faces.Extent()) return false;
        const TopoDS_Face& f = TopoDS::Face(faces((int)faceIndex + 1));
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Plane) return false;
        gp_Pln pl = s.Plane();
        gp_Ax3 ax = pl.Position();
        gp_Dir d = ax.Direction();
        if (f.Orientation() == TopAbs_REVERSED) d.Reverse();
        gp_Pnt p = ax.Location();
        o = {p.X(), p.Y(), p.Z()};
        n = {d.X(), d.Y(), d.Z()};
        return true;
    } catch (...) {
        return false;
    }
}

std::string kernelVersion() { return OCC_VERSION_COMPLETE; }

} // namespace kst::kernel
