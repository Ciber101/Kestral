// The Slice tab's filaments, the way OrcaSlicer lists them: filament 1, 2, 3... each with a profile
// and a color. Parts print with one of them. Kestral picks it from the part's color (the same color
// always lands on the same filament, so two red parts never make a prime tower), and the user can
// pick another one (Objects list, right-click menu, or the assistant).
#include "app/App.h"
#include "core/Json.h"

#include <algorithm>
#include <cmath>

namespace kst {

namespace {

const glm::vec3 kDefaultGrey{0.72f, 0.74f, 0.77f};

float colorDist(glm::vec3 a, glm::vec3 b) { return glm::length(a - b); }

// Orca's palette for new filaments.
const glm::vec3 kPalette[] = {{0.0f, 0.68f, 0.26f}, {0.95f, 0.85f, 0.1f}, {0.12f, 0.45f, 0.95f}, {0.92f, 0.16f, 0.16f},
                              {0.95f, 0.95f, 0.95f}, {0.1f, 0.1f, 0.1f},  {0.6f, 0.2f, 0.75f},  {1.0f, 0.42f, 0.21f}};

} // namespace

bool App::isPlainColor(glm::vec3 c) const { return colorDist(c, kDefaultGrey) < 0.02f; }

glm::vec3 App::bodyPrintColor(const Body& b) const {
    // The color the part shows: its own, or the one most of its faces are painted with.
    if (b.faceLooks.empty()) return b.color;
    std::map<std::tuple<int, int, int>, int> votes;
    auto key = [](glm::vec3 c) { return std::make_tuple((int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255)); };
    uint32_t faces = 0;
    auto pit = doc_.parts.find(b.partId);
    if (pit != doc_.parts.end() && pit->second.mesh) faces = pit->second.mesh->faceCount;
    int painted = 0;
    for (auto& [f, fl] : b.faceLooks) { ++votes[key(fl.color)]; ++painted; }
    if (faces > (uint32_t)painted) votes[key(b.color)] += (int)faces - painted;
    auto best = std::max_element(votes.begin(), votes.end(), [](auto& a, auto& c) { return a.second < c.second; });
    auto [r, g, bl] = best->first;
    return glm::vec3(r / 255.f, g / 255.f, bl / 255.f);
}

std::string App::sliceFilPreset(int i) const {
    if (i <= 0 || i >= (int)slice_.ex.fils.size() || slice_.ex.fils[(size_t)i].preset.empty()) return slice_.filament;
    return slice_.ex.fils[(size_t)i].preset;
}

glm::vec3 App::sliceFilColor(int i) const {
    if (i >= 0 && i < (int)slice_.ex.fils.size()) return slice_.ex.fils[(size_t)i].color;
    return sliceFilamentColor(0);
}

glm::vec3 App::presetColor(const std::string& name) {
    // A filament profile's own color (#RRGGBB), read once.
    auto c = presetColors_.find(name);
    if (c != presetColors_.end()) return c->second;
    glm::vec3 out(1.f, 0.42f, 0.21f);
    if (slice_.setup) {
        if (const print::Preset* p = print::findPreset(*slice_.setup, "filament", name)) {
            std::string err;
            json::Value v = json::Value::parse(print::resolvePreset(*slice_.setup, *p, err));
            const json::Value& fc = v["filament_colour"];
            std::string s = fc.isArray() && fc.size() ? fc[(size_t)0].asString() : fc.asString();
            if (s.size() >= 7 && s[0] == '#') {
                unsigned u = (unsigned)std::strtoul(s.substr(1, 6).c_str(), nullptr, 16);
                out = glm::vec3(((u >> 16) & 255) / 255.f, ((u >> 8) & 255) / 255.f, (u & 255) / 255.f);
            }
        }
    }
    presetColors_[name] = out;
    return out;
}

void App::sliceFilsEnsure() {
    auto& f = slice_.ex.fils;
    if (f.empty()) {
        SliceTab::Fil one;
        one.color = sliceFilamentColor(0);
        f.push_back(one);
    }
    if (slice_.ex.flush.size() != f.size() * f.size()) sliceFlushDefaults();
}

int App::sliceAddFil(const std::string& preset, glm::vec3 color, bool autoMade) {
    sliceFilsEnsure();
    auto& f = slice_.ex.fils;
    if (f.size() >= 16) return -1;
    SliceTab::Fil n;
    n.preset = preset == slice_.filament ? std::string() : preset;
    n.color = color;
    n.autoMade = autoMade;
    f.push_back(n);
    sliceFlushDefaults();
    sceneDirty_ = true;
    return (int)f.size() - 1;
}

int App::sliceNewFilColor() const {
    // The next palette color nobody uses yet.
    for (int k = 0; k < (int)(sizeof kPalette / sizeof kPalette[0]); ++k) {
        bool used = false;
        for (auto& f : slice_.ex.fils) used |= colorDist(f.color, kPalette[k]) < 0.05f;
        if (!used) return k;
    }
    return (int)slice_.ex.fils.size() % (int)(sizeof kPalette / sizeof kPalette[0]);
}

glm::vec3 App::slicePaletteColor(int k) const { return kPalette[std::clamp(k, 0, (int)(sizeof kPalette / sizeof kPalette[0]) - 1)]; }

void App::sliceRemoveFil(int i) {
    auto& f = slice_.ex.fils;
    if (i <= 0 || i >= (int)f.size()) return;   // filament 1 always stays
    f.erase(f.begin() + i);
    auto fix = [&](int& v) { v = v == i ? 0 : v > i ? v - 1 : v; };
    for (auto& [id, o] : slice_.plate) fix(o.fil);
    sliceFlushDefaults();
    sceneDirty_ = true;
}

int App::sliceFilForColor(glm::vec3 c) {
    sliceFilsEnsure();
    if (isPlainColor(c)) return 0;   // a part nobody colored prints with filament 1
    auto& f = slice_.ex.fils;
    int best = -1;
    float bd = 0.07f;   // the same color, give or take rounding
    for (int i = 0; i < (int)f.size(); ++i) {
        float d = colorDist(f[(size_t)i].color, c);
        if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0) return best;
    int n = sliceAddFil(std::string(), c, true);
    return n < 0 ? 0 : n;
}

void App::sliceAutoFils() {
    sliceFilsEnsure();
    for (auto& [id, o] : slice_.plate) {
        if (o.fil < 0 || o.fil >= (int)slice_.ex.fils.size()) o.fil = 0;
        if (o.autoFil) o.fil = o.useColor ? sliceFilForColor(o.color) : 0;
    }
    // Filaments Kestral made for a color that no part has any more go away.
    auto& f = slice_.ex.fils;
    for (int i = (int)f.size() - 1; i >= 1; --i) {
        if (!f[(size_t)i].autoMade) continue;
        bool used = false;
        for (auto& [id, o] : slice_.plate) used |= o.fil == i;
        if (!used) sliceRemoveFil(i);
    }
}

void App::sliceFlushDefaults() {
    // Orca's starting flushing volumes: more when going from a dark color to a light one.
    auto& f = slice_.ex.fils;
    size_t n = f.size();
    std::vector<float> old = slice_.ex.flush;
    size_t oldN = (size_t)std::lround(std::sqrt((double)old.size()));
    slice_.ex.flush.assign(n * n, 0.f);
    auto lum = [](glm::vec3 c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; };
    for (size_t a = 0; a < n; ++a)
        for (size_t b = 0; b < n; ++b) {
            if (a == b) continue;
            glm::vec3 ca = f[a].color, cb = f[b].color;
            float dl = std::max(0.f, lum(cb) - lum(ca));
            float dc = colorDist(ca, cb) / 1.732f;
            float v = 107.f + 520.f * dl + 180.f * dc;
            slice_.ex.flush[a * n + b] = std::round(std::clamp(v, 50.f, 900.f));
        }
    // Values the user typed stay (for the filaments still there).
    if (oldN * oldN == old.size() && oldN == n && !slice_.ex.flushAuto) slice_.ex.flush = old;
}

} // namespace kst
