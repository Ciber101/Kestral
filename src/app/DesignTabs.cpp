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
    if (designTabs_.size() <= 1) {   // the last one: an empty design in its place
        newDocument();
        return;
    }
    if (i == designActive_) {
        // Keep unsaved work: it goes to the autosave like closing Kestral does.
        if (dirty_ && !doc_.timeline.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(u8path(autosavePath()).parent_path(), ec);
            saveDocument(doc_, autosavePath());
        }
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
