#include "print/OrcaOptions.h"

#include <map>

namespace kst::print {

namespace {
#include "print/OrcaOptions.inc"
}

const OrcaOptionDef* orcaOption(const std::string& key) {
    static const std::map<std::string, const OrcaOptionDef*> index = [] {
        std::map<std::string, const OrcaOptionDef*> m;
        for (auto& d : kOrcaOptions) m[d.key] = &d;
        return m;
    }();
    auto it = index.find(key);
    return it == index.end() ? nullptr : it->second;
}

std::vector<const OrcaLayoutRow*> orcaLayout(const std::string& tab) {
    std::vector<const OrcaLayoutRow*> out;
    for (auto& r : kOrcaLayout)
        if (tab == r.tab) out.push_back(&r);
    return out;
}

std::vector<std::string> orcaPages(const std::string& tab) {
    std::vector<std::string> out;
    for (auto& r : kOrcaLayout) {
        if (tab != r.tab) continue;
        std::string p = r.page;
        if (p == "Dependencies" || p == "Notes") continue;   // profile bookkeeping, not settings
        if (out.empty() || out.back() != p) {
            bool seen = false;
            for (auto& q : out) seen |= q == p;
            if (!seen) out.push_back(p);
        }
    }
    return out;
}

std::vector<std::string> splitChoices(const char* text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = text; p && *p; ++p) {
        if (*p == '\n') { out.push_back(cur); cur.clear(); }
        else cur += *p;
    }
    if (!cur.empty() || (text && *text)) out.push_back(cur);
    return out;
}

} // namespace kst::print
