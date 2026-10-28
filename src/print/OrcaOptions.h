// OrcaSlicer's settings as OrcaSlicer shows them: label, unit, tooltip, choices, simple or advanced,
// and the page and group each sits in on the process, filament and printer tabs. The table is made
// from OrcaSlicer's source by tools/gen_orca_options.py (run it again after a big Orca update).
#pragma once
#include <string>
#include <vector>

namespace kst::print {

struct OrcaOptionDef {
    const char *key, *type, *label, *fullLabel, *unit, *tooltip, *category, *mode, *values, *valueLabels, *min, *max;
};
struct OrcaLayoutRow {
    const char *tab, *page, *group, *key, *line;   // line: shared row name ("Nozzle") or empty
};

const OrcaOptionDef* orcaOption(const std::string& key);
// Rows of one tab ("process", "filament", "printer") in OrcaSlicer's order.
std::vector<const OrcaLayoutRow*> orcaLayout(const std::string& tab);
std::vector<std::string> orcaPages(const std::string& tab);
std::vector<std::string> splitChoices(const char* text);   // "a\nb" -> {"a", "b"}

} // namespace kst::print
