#pragma once
#include <map>
#include <string>
#include <vector>

namespace kst {

// One user parameter. `expr` is what the user typed ("width / 2", "10mm" style plain math);
// `value` is the resolved number in mm (or degrees for angle fields where used).
struct Param {
    std::string name;
    std::string expr = "0";
    std::string comment;
    double value = 0;
};

using ParamMap = std::map<std::string, Param>;

// Evaluates a math expression: numbers, parameter names, + - * / ^ ( ), unary minus,
// and functions sin cos tan asin acos atan atan2 sqrt abs floor ceil round exp log min max hypot.
// Trigonometry works in degrees (CAD habit). Throws std::runtime_error on bad input.
namespace peval {
double eval(const std::string& expr, const ParamMap& params);
// Parameter names referenced by the expression (for surgical recompute).
std::vector<std::string> names(const std::string& expr);
// True if the string is just a plain number (no names, no operators beyond sign).
bool plainNumber(const std::string& s);
} // namespace peval

// Re-evaluates every parameter in dependency order. Params may reference other params.
// On error returns false with a message naming the parameter; values are left best effort.
bool updateParamValues(ParamMap& params, std::string& err);

// Unique valid parameter name suggestion ("width", "width1", "width2"...).
std::string nextParamName(const ParamMap& params, const std::string& base = "param");

// Serializes name=value pairs of the given names (sorted) for change detection.
std::string paramValuesKey(const ParamMap& params, const std::vector<std::string>& names);

} // namespace kst
