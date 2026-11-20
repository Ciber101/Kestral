// Design tabs, like Fusion's document tabs: several designs open at once. The tab in front keeps its
// state in the usual members (doc_, undo_, selection...); the others are parked in designTabs_.
#include "app/App.h"
#include "core/SaveLoad.h"

#include <filesystem>

namespace kst {

void App::designSwap(DesignTab& t) {
    std::swap(t.doc, doc_);
    std::swap(t.undo, undo_);
    std::swap(t.dirty, dirty_);
    std::swap(t.cam, cam_);
    std::swap(t.selection, selection_);
    std::swap(t.selFaces, selFaces_);
    std::swap(t.selEdges, selEdges_);
    std::swap(t.selProfiles, selProfiles_);
    std::swap(t.selOrder, selOrder_);
    std::swap(t.isolated, isolated_);
    std::swap(t.collapsed, collapsed_);
    std::swap(t.savedMarker, savedMarker_);
    std::swap(t.asKey, asKey_);
    std::swap(t.cleanRev, cleanRev_);
    std::swap(t.recRev, recRev_);
    std::swap(t.ownRev, ownRev_);
    std::swap(t.ownAutosaved, ownAutosaved_);
}

bool App::docInUse() const {
    return !doc_.path.empty() || !doc_.timeline.empty() || !doc_.bodies.empty() || dirty_;
}

void App::switchDesignTab(int i) {
    if (designTabs_.empty()) designTabs_.resize(1);
    if (i < 0 || i >= (int)designTabs_.size() || i == designActive_) return;
    // Whatever is half done finishes first: a sketch, an open command.
    if (sk_.active) finishSketch();
    if (dlg_.open) closeFeatureDialog(true);
    if (tdlg_.open) closeToolDialog(true);
    if (sliceWs_) leaveSliceWorkspace();
    // The design going to the back has its recovery copy up to date (it cannot change back there).
    syncDirty();
    if (dirty_ && recRev_ != doc_.revision) writeRecoverySync(doc_, asKey_, recRev_);
    designSwap(designTabs_[(size_t)designActive_]);
    designSwap(designTabs_[(size_t)i]);
    designActive_ = i;
    // A rebuild running for the other design is dropped when it lands (docGen_); this design checks
    // its own (features that did not change are skipped).
    ++docGen_;
    recomputeQueuedFrom_ = -1;
    hov_ = HoverInfo{};
    hoverBody_ = hoverFace_ = 0;
    browserHover_.clear();
    startOpen_ = false;
    markSceneDirty();
    scheduleRecompute(0);
    activity();
}

int App::newDesignTab() {
    if (designTabs_.empty()) designTabs_.resize(1);
    designTabs_.push_back(DesignTab{});
    int i = (int)designTabs_.size() - 1;
    switchDesignTab(i);
    return i;
}

void App::closeDesignTab(int i) {
    if (i < 0 || i >= (int)designTabs_.size()) return;
    // Unsaved work that reaches here (Save and Don't Save are handled before) keeps its recovery copy,
    // offered on the next start. Saved or discarded work has none left.
    if (i == designActive_) {
        syncDirty();
        if (dirty_) writeRecoverySync(doc_, asKey_, recRev_);
    } else {
        DesignTab& t = designTabs_[(size_t)i];
        if (t.doc.revision != t.cleanRev) t.dirty = true;
        if (t.dirty && t.recRev != t.doc.revision) writeRecoverySync(t.doc, t.asKey, t.recRev);
    }
    if (designTabs_.size() <= 1) {   // the last one: an empty design in its place
        leaveSketchMode();
        sk_.active = false;
        doc_ = Document();
        undo_.clear();
        selection_.clear();
        isolated_.clear();
        browserHover_.clear();
        asKey_.clear();
        recRev_ = ownRev_ = 0;
        ownAutosaved_ = false;
        markClean();
        markSceneDirty();
        activity();
        return;
    }
    if (i == designActive_) {
        int next = i + 1 < (int)designTabs_.size() ? i + 1 : i - 1;
        switchDesignTab(next);
        designTabs_.erase(designTabs_.begin() + i);
        if (designActive_ > i) --designActive_;
    } else {
        designTabs_.erase(designTabs_.begin() + i);
        if (designActive_ > i) --designActive_;
    }
    activity();
}

int App::designTabFor(const std::string& path) const {
    if (path.empty()) return -1;
    std::error_code ec;
    for (int i = 0; i < (int)designTabs_.size(); ++i) {
        const std::string& p = i == designActive_ ? doc_.path : designTabs_[(size_t)i].doc.path;
        if (!p.empty() && (p == path || std::filesystem::equivalent(u8path(p), u8path(path), ec))) return i;
    }
    return -1;
}

std::string App::designTabTitle(int i) const {
    const Document& d = i == designActive_ ? doc_ : designTabs_[(size_t)i].doc;
    bool dirty = i == designActive_ ? dirty_ : designTabs_[(size_t)i].dirty;
    return d.name + (d.version > 0 ? "  v" + std::to_string(d.version) : std::string()) + (dirty ? "  *" : "");
}

} // namespace kst
