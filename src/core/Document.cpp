#include "core/Document.h"
#include <functional>

namespace kst {

Document::Document() {
    Component root;
    root.id = newId();
    root.name = "Untitled";
    rootId = root.id;
    activeComponent = root.id;
    components[root.id] = root;
}

int Document::featureIndex(uint32_t featureId) const {
    if (featureId == 0) return -1;
    std::lock_guard<std::mutex> lk(fidx_.mu);
    auto it = fidx_.map.find(featureId);
    if (it != fidx_.map.end() && it->second < (int)timeline.size() && timeline[it->second].id == featureId) return it->second;
    // Stale or unknown: rebuild (timeline edits happen in many places; a rebuild is one pass).
    fidx_.map.clear();
    fidx_.map.reserve(timeline.size());
    for (int i = 0; i < (int)timeline.size(); ++i) fidx_.map[timeline[i].id] = i;
    it = fidx_.map.find(featureId);
    return it == fidx_.map.end() ? -1 : it->second;
}

bool Document::isFeatureActive(uint32_t featureId) const {
    if (featureId == 0) return true;
    int i = featureIndex(featureId);
    if (i < 0) return true;
    return i < markerPos && !timeline[i].suppressed;
}

bool Document::isComponentShown(uint32_t compId) const {
    int guard = 0;
    while (compId != 0 && guard++ < 10000) {
        auto it = components.find(compId);
        if (it == components.end()) return false;
        if (!it->second.visible) return false;
        compId = it->second.parentId;
    }
    return true;
}

bool Document::isBodyAlive(const Body& b) const {
    if (!isFeatureActive(b.featureId)) return false;
    if (b.consumedBy != 0) {
        int fi = featureIndex(b.consumedBy);
        if (fi >= 0 && fi < markerPos) {
            const Feature& f = timeline[fi];
            if (!f.suppressed && !f.failed) return false;
        }
    }
    return true;
}

bool Document::isBodyShown(const Body& b) const {
    if (!b.visible || !isFeatureActive(b.featureId) || !isComponentShown(b.componentId)) return false;
    // Consumed by a later, active, successful feature: its outputs replaced this body.
    if (b.consumedBy != 0) {
        int fi = featureIndex(b.consumedBy);
        if (fi >= 0 && fi < markerPos) {
            const Feature& f = timeline[fi];
            if (!f.suppressed && !f.failed) return false;
        }
    }
    return true;
}

std::vector<uint32_t> Document::featureBodies(uint32_t featureId) const {
    std::vector<uint32_t> out;
    for (auto& [id, b] : bodies)
        if (b.featureId == featureId) out.push_back(id);
    return out;   // map order = ascending id = slot order
}

uint32_t Document::bodySlot(uint32_t bodyId) const {
    auto it = bodies.find(bodyId);
    if (it == bodies.end()) return 0;
    uint32_t slot = 0;
    for (auto& [id, b] : bodies) {
        if (b.featureId != it->second.featureId) continue;
        if (id == bodyId) return slot;
        ++slot;
    }
    return 0;
}

std::vector<uint32_t> Document::childComponents(uint32_t compId) const {
    std::vector<uint32_t> out;
    for (auto& [id, c] : components)
        if (c.parentId == compId && id != rootId) out.push_back(id);
    return out;
}

std::vector<uint32_t> Document::bodiesIn(uint32_t compId) const {
    std::vector<uint32_t> out;
    for (auto& [id, b] : bodies)
        if (b.componentId == compId) out.push_back(id);
    return out;
}

void Document::collectBodies(uint32_t compId, std::vector<uint32_t>& out) const {
    // Live bodies only: ones a later feature replaced (moved, filleted...) are history, not parts.
    // One pass over components and one over bodies (designs with thousands of folders stay fast).
    std::unordered_map<uint32_t, bool> inside;
    inside.reserve(components.size());
    inside[compId] = true;
    std::function<bool(uint32_t)> under = [&](uint32_t c) -> bool {
        auto it = inside.find(c);
        if (it != inside.end()) return it->second;
        auto ct = components.find(c);
        bool r = false;
        if (ct != components.end() && c != rootId && ct->second.parentId != c) {
            inside[c] = false;   // guard against loops
            r = under(ct->second.parentId == 0 ? rootId : ct->second.parentId);
        }
        inside[c] = r;
        return r;
    };
    for (auto& [id, b] : bodies)
        if (under(b.componentId) && isBodyAlive(b)) out.push_back(id);
}

int Document::countBodiesDeep(uint32_t compId) const {
    std::vector<uint32_t> v;
    collectBodies(compId, v);
    return (int)v.size();
}

Aabb Document::bounds(bool shownOnly) const {
    Aabb box;
    for (auto& [id, b] : bodies) {
        if (shownOnly && !isBodyShown(b)) continue;
        auto pit = parts.find(b.partId);
        if (pit == parts.end() || !pit->second.mesh) continue;
        box.add(pit->second.mesh->bounds.transformed(b.xform));
    }
    return box;
}

void Document::removeBodies(const std::set<uint32_t>& ids) {
    for (uint32_t id : ids) bodies.erase(id);
    gcParts();
    touch();
}

void Document::removeComponent(uint32_t compId) {
    if (compId == rootId) return;
    for (uint32_t c : childComponents(compId)) removeComponent(c);
    for (auto it = bodies.begin(); it != bodies.end();)
        it = it->second.componentId == compId ? bodies.erase(it) : std::next(it);
    components.erase(compId);
    gcParts();
    touch();
}

void Document::gcParts() {
    std::set<uint32_t> used;
    for (auto& [id, b] : bodies) used.insert(b.partId);
    for (auto it = parts.begin(); it != parts.end();)
        it = used.count(it->first) ? std::next(it) : parts.erase(it);
}

void UndoStack::push(const Document& before, std::string label) {
    undo_.push_back({before, std::move(label)});
    if (undo_.size() > kMax) undo_.erase(undo_.begin());
    redo_.clear();
}

bool UndoStack::undo(Document& doc) {
    if (undo_.empty()) return false;
    Entry e = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({doc, e.label});
    uint64_t rev = doc.revision;
    doc = std::move(e.doc);
    doc.revision = rev + 1;
    return true;
}

bool UndoStack::redo(Document& doc) {
    if (redo_.empty()) return false;
    Entry e = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({doc, e.label});
    uint64_t rev = doc.revision;
    doc = std::move(e.doc);
    doc.revision = rev + 1;
    return true;
}

} // namespace kst
