// Slice tabs: every design you slice gets its own tab (with an X), like OrcaSlicer projects. The tab
// in front keeps its state in slice_ (and print_.last); the others are parked in sliceDocs_. A tab
// whose design is not the open one shows its plate as it was (a snapshot) and its sliced result.
#include "app/App.h"

#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>

namespace kst {

std::string App::sliceDocKey() const {
    return doc_.path.empty() ? "untitled:" + doc_.name : doc_.path;
}

int App::sliceTabFor(const std::string& key) const {
    for (int i = 0; i < (int)sliceDocs_.size(); ++i)
        if (sliceDocs_[(size_t)i].key == key) return i;
    for (int i = 0; i < (int)sliceDocs_.size(); ++i)
        if (sliceDocs_[(size_t)i].members.count(key)) return i;
    return -1;
}

bool App::sliceTabOwns(int tab, const std::string& key) const {
    if (tab < 0 || tab >= (int)sliceDocs_.size()) return false;
    return sliceDocs_[(size_t)tab].key == key || sliceDocs_[(size_t)tab].members.count(key) != 0;
}

bool App::plateOwns(const std::string& key) const {
    if (sliceActive_ < 0 || sliceActive_ >= (int)sliceDocs_.size()) return key == sliceDocKey();
    return sliceTabOwns(sliceActive_, key);
}

// Swaps the design specific part of the slice state with a parked tab.
void App::sliceSwap(SliceDocState& d) {
    std::swap(d.plate, slice_.plate);
    std::swap(d.nextObj, slice_.nextObj);
    std::swap(d.plateSelSet, slice_.plateSelSet);
    std::swap(d.plateOnly, slice_.plateOnly);
    std::swap(d.plates, slice_.plates);
    std::swap(d.activePlate, slice_.activePlate);
    std::swap(d.plateRev, slice_.plateRev);
    std::swap(d.plateSel, slice_.plateSel);
    std::swap(d.plateSelCopy, slice_.plateSelCopy);
    std::swap(d.paths, slice_.paths);
    std::swap(d.meshes, slice_.meshes);
    std::swap(d.meshesTool, slice_.meshesTool);
    std::swap(d.toolColors, slice_.toolColors);
    std::swap(d.ex, slice_.ex);
    std::swap(d.pathsFile, slice_.pathsFile);
    std::swap(d.layerLo, slice_.layerLo);
    std::swap(d.layerHi, slice_.layerHi);
    std::swap(d.moveHi, slice_.moveHi);
    std::swap(d.preview, slice_.preview);
    std::swap(d.slicedRev, slice_.slicedRev);
    std::swap(d.error, slice_.error);
    std::swap(d.live, slice_.live);
    std::lock_guard lock(print_.m);
    std::swap(d.last, print_.last);
    std::swap(d.lastName, print_.lastName);
    std::swap(d.lastChanges, print_.lastChanges);
}

// Parts carry their own geometry now: nothing to remember when a tab goes to the back.
void App::sliceSnapshot() {}

void App::sliceActivate(int i) {
    if (i < 0 || i >= (int)sliceDocs_.size()) return;
    if (i != sliceActive_) {
        if (sliceActive_ >= 0) {
            sliceSnapshot();
            sliceSwap(sliceDocs_[(size_t)sliceActive_]);
        }
        sliceSwap(sliceDocs_[(size_t)i]);
        sliceActive_ = i;
        slice_.framed = false;
        slice_.plateDrag = false;
        slice_.layMode = false;
        slice_.meshesScalar.clear();
        slice_.scalarMode = -1;
    }
    slice_.live = sliceTabOwns(i, sliceDocKey());
    std::string pend;
    std::swap(pend, sliceDocs_[(size_t)i].pendingPreview);
    if (!pend.empty()) { slice_.preview = true; loadSlicePreview(pend); }
    sceneDirty_ = true;
}

void App::enterSliceTab(int i) {
    if (i < 0 || i >= (int)sliceDocs_.size()) return;
    enterSliceWorkspace(i);
}

void App::closeSliceTab(int i) {
    if (i < 0 || i >= (int)sliceDocs_.size()) return;
    if (i == sliceActive_) {
        SliceDocState gone;
        sliceSwap(gone);   // the front state is now empty; the closed tab's state goes with `gone`
        sliceActive_ = -1;
        sliceDocs_.erase(sliceDocs_.begin() + i);
        if (sliceWs_ && !sliceDocs_.empty()) {
            int next = sliceTabFor(sliceDocKey());   // the open design's tab first
            if (next < 0) next = std::min(i, (int)sliceDocs_.size() - 1);
            sliceActivate(next);
        } else if (sliceWs_) {
            leaveSliceWorkspace();
        }
    } else {
        sliceDocs_.erase(sliceDocs_.begin() + i);
        if (sliceActive_ > i) --sliceActive_;
    }
    sceneDirty_ = true;
    activity();
}

// Combine: every part of one tab goes onto another tab's plate (the plate in front there), packed
// with what is there already. The tab it came from closes.
void App::sliceMergeTabs(int from, int into) {
    if (from < 0 || into < 0 || from == into || from >= (int)sliceDocs_.size() || into >= (int)sliceDocs_.size()) return;
    // Park the front tab so both tabs' parts sit in sliceDocs_, then bring the target in front.
    if (sliceActive_ == from) {
        sliceSwap(sliceDocs_[(size_t)from]);
        sliceActive_ = -1;
    }
    bool wasWs = sliceWs_;
    if (!sliceWs_) enterSliceWorkspace(into);
    else sliceActivate(into);
    auto& src = sliceDocs_[(size_t)from];
    int moved = 0;
    for (auto& [oid, o] : src.plate) {
        if (!o.onPlate || !o.mesh) continue;
        uint32_t nid = slice_.nextObj++;
        SliceTab::PlateObj c = o;
        c.plateOf.assign(c.pos.size(), slice_.activePlate);
        // Its filament: the same one here (profile and color), made when this tab has none like it.
        if (!c.autoFil && c.fil > 0 && c.fil < (int)src.ex.fils.size()) {
            const auto& sf = src.ex.fils[(size_t)c.fil];
            sliceFilsEnsure();
            int found = -1;
            for (int f = 0; f < (int)slice_.ex.fils.size(); ++f)
                if (slice_.ex.fils[(size_t)f].preset == sf.preset && glm::length(slice_.ex.fils[(size_t)f].color - sf.color) < 0.07f) found = f;
            if (found < 0) found = sliceAddFil(sf.preset.empty() ? slice_.filament : sf.preset, sf.color, false);
            c.fil = std::max(0, found);
        }
        slice_.plate[nid] = std::move(c);
        ++moved;
    }
    std::string fromName = src.name;
    // The designs of the tab it came from now belong here (their Slice goes to this tab).
    auto& dst = sliceDocs_[(size_t)into];
    dst.members.insert(src.key);
    for (auto& k : src.members) dst.members.insert(k);
    dst.members.erase(dst.key);
    if (dst.name.find(src.name) == std::string::npos) dst.name += " + " + src.name;
    int intoIdx = into;
    sliceDocs_.erase(sliceDocs_.begin() + from);
    if (from < intoIdx) --intoIdx;
    sliceActive_ = intoIdx;
    sliceAutoFils();
    plateArrangeOne(slice_.activePlate);
    slice_.framed = false;
    sceneDirty_ = true;
    (void)wasWs;
    toast(std::to_string(moved) + " part" + (moved == 1 ? "" : "s") + " from " + fromName + " joined this plate", ToastKind::Success);
}

// Plates sit in rows like OrcaSlicer's: plate 0 at the origin, the next ones to the right, then a
// new row below.
glm::vec2 App::plateOffset(int plate) const {
    if (plate <= 0) return glm::vec2(0);
    Aabb b = plateBedRect();
    float w = (b.max.x - b.min.x) * 1.25f, h = (b.max.y - b.min.y) * 1.25f;
    int cols = std::max(1, (int)std::ceil(std::sqrt((double)std::max(1, slice_.plates))));
    return glm::vec2((plate % cols) * w, -(plate / cols) * h);
}

int App::plateAt(glm::vec2 world) const {
    Aabb b = plateBedRect();
    glm::vec2 c(b.center());
    int best = 0;
    float bd = 1e30f;
    for (int k = 0; k < slice_.plates; ++k) {
        glm::vec2 o = plateOffset(k) + c;
        float d = glm::length(world - o);
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

} // namespace kst
