#include "core/Params.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace kst::peval {
namespace {

struct Lexer {
    std::string src;
    size_t i = 0;
    // token: kind, text, number
    enum Kind { End, Num, Name, Op } kind = End;
    std::string text;
    double num = 0;
    void next() {
        while (i < src.size() && std::isspace((unsigned char)src[i])) ++i;
        if (i >= src.size()) { kind = End; return; }
        char c = src[i];
        if (std::isdigit((unsigned char)c) || c == '.') {
            size_t j = i;
            while (j < src.size() && (std::isdigit((unsigned char)src[j]) || src[j] == '.')) ++j;
            // exponent
            if (j < src.size() && (src[j] == 'e' || src[j] == 'E') &&
                j + 1 < src.size() && (std::isdigit((unsigned char)src[j + 1]) || src[j + 1] == '+' || src[j + 1] == '-')) {
                j += 2;
                while (j < src.size() && std::isdigit((unsigned char)src[j])) ++j;
            }
            text = src.substr(i, j - i);
            num = std::strtod(text.c_str(), nullptr);
            i = j;
            kind = Num;
            return;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < src.size() && (std::isalnum((unsigned char)src[j]) || src[j] == '_')) ++j;
            text = src.substr(i, j - i);
            i = j;
            kind = Name;
            return;
        }
        text = std::string(1, c);
        i++;
        kind = Op;
    }
};

const double kPi = 3.14159265358979323846;

struct Parser {
    Lexer lx;
    const ParamMap& params;
    bool namesOnly = false;
    std::vector<std::string> found;

    double parse() {
        lx.next();
        double v = exprSum();
        if (lx.kind != Lexer::End) fail("unexpected '" + lx.text + "'");
        return v;
    }
    [[noreturn]] void fail(const std::string& why) { throw std::runtime_error(why); }
    bool eat(const char* op) {
        if (lx.kind == Lexer::Op && lx.text == op) { lx.next(); return true; }
        return false;
    }
    double valueOf(const std::string& name) {
        if (namesOnly) { found.push_back(name); return 1; }
        auto it = params.find(name);
        if (it != params.end()) return it->second.value;
        fail("unknown name '" + name + "'");
    }
    double exprSum() {
        double v = exprMul();
        for (;;) {
            if (eat("+")) v += exprMul();
            else if (eat("-")) v -= exprMul();
            else return v;
        }
    }
    double exprMul() {
        double v = exprPow();
        for (;;) {
            if (eat("*")) v *= exprPow();
            else if (eat("/")) {
                double d = exprPow();
                if (d == 0) fail("division by zero");
                v /= d;
            } else return v;
        }
    }
    double exprPow() {
        double v = exprUnary();
        if (eat("^")) return std::pow(v, exprPow());   // right associative
        return v;
    }
    double exprUnary() {
        if (eat("-")) return -exprUnary();
        if (eat("+")) return exprUnary();
        return exprAtom();
    }
    double exprAtom() {
        if (lx.kind == Lexer::Num) { double v = lx.num; lx.next(); return v; }
        if (lx.kind == Lexer::Name) {
            std::string n = lx.text;
            std::string lower = n;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            lx.next();
            if (lx.kind == Lexer::Op && lx.text == "(") {
                lx.next();
                std::vector<double> args;
                if (!(lx.kind == Lexer::Op && lx.text == ")")) {
                    args.push_back(exprSum());
                    while (eat(",")) args.push_back(exprSum());
                }
                if (!eat(")")) fail("missing )");
                auto arg = [&](int k) { return args.size() > (size_t)k ? args[k] : 0.0; };
                auto deg = [](double d) { return d * kPi / 180.0; };
                auto rad = [](double r) { return r * 180.0 / kPi; };
                if (lower == "sin") return namesOnly ? 1 : std::sin(deg(arg(0)));
                if (lower == "cos") return namesOnly ? 1 : std::cos(deg(arg(0)));
                if (lower == "tan") return namesOnly ? 1 : std::tan(deg(arg(0)));
                if (lower == "asin") return namesOnly ? 1 : rad(std::asin(arg(0)));
                if (lower == "acos") return namesOnly ? 1 : rad(std::acos(arg(0)));
                if (lower == "atan") return namesOnly ? 1 : rad(std::atan(arg(0)));
                if (lower == "atan2") return namesOnly ? 1 : rad(std::atan2(arg(0), arg(1)));
                if (lower == "sqrt") return std::sqrt(arg(0));
                if (lower == "abs") return std::abs(arg(0));
                if (lower == "floor") return std::floor(arg(0));
                if (lower == "ceil") return std::ceil(arg(0));
                if (lower == "round") return std::round(arg(0));
                if (lower == "exp") return std::exp(arg(0));
                if (lower == "log") return std::log(arg(0));
                if (lower == "pow") return std::pow(arg(0), arg(1));
                if (lower == "hypot") return std::hypot(arg(0), arg(1));
                if (lower == "min") return args.empty() ? 0 : *std::min_element(args.begin(), args.end());
                if (lower == "max") return args.empty() ? 0 : *std::max_element(args.begin(), args.end());
                fail("unknown function '" + n + "'");
            }
            if (lower == "pi") return kPi;
            if (lower == "tau") return 2 * kPi;
            if (lower == "mm" || lower == "deg" || lower == "degree" || lower == "degrees") return 1;   // unit suffixes
            return valueOf(n);
        }
        if (eat("(")) {
            double v = exprSum();
            if (!eat(")")) fail("missing )");
            return v;
        }
        fail("expected a value");
    }
};

Parser make(const std::string& e, const ParamMap& p, bool namesOnly) {
    Parser ps{Lexer{e, 0}, p, namesOnly, {}};
    return ps;
}

} // namespace

double eval(const std::string& expr, const ParamMap& params) {
    Parser ps = make(expr, params, false);
    return ps.parse();
}

std::vector<std::string> names(const std::string& expr) {
    ParamMap empty;
    Parser ps = make(expr, empty, true);
    try {
        double ignore = ps.parse();
        (void)ignore;
    } catch (const std::runtime_error&) {
        // return whatever we collected; callers use this for change detection only
    }
    std::sort(ps.found.begin(), ps.found.end());
    ps.found.erase(std::unique(ps.found.begin(), ps.found.end()), ps.found.end());
    return ps.found;
}

bool plainNumber(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (std::isalpha((unsigned char)c) || c == '_') return false;
    try {
        double parsed = std::stod(s);
        (void)parsed;
    } catch (...) {
        return false;
    }
    // also reject garbage like "1 2"
    Lexer lx{s, 0};
    lx.next();
    if (lx.kind != Lexer::Num) return false;
    lx.next();
    return lx.kind == Lexer::End;
}

} // namespace kst::peval

namespace kst {

bool updateParamValues(ParamMap& params, std::string& err) {
    // Iterate to a fixed point so params can reference other params in any order.
    bool ok = true;
    for (int pass = 0; pass < (int)params.size() + 1; ++pass) {
        bool changed = false;
        for (auto& [name, p] : params) {
            double v;
            try {
                v = peval::eval(p.expr, params);
            } catch (const std::runtime_error& e) {
                err = "Parameter '" + name + "': " + e.what();
                ok = false;
                continue;
            }
            if (v != p.value) { p.value = v; changed = true; }
        }
        if (!changed) break;
        if (pass == (int)params.size()) {
            err = "Parameters form a dependency cycle";
            ok = false;
        }
    }
    return ok;
}

std::string nextParamName(const ParamMap& params, const std::string& base) {
    for (int n = 1;; ++n) {
        std::string cand = base + (n > 1 ? std::to_string(n) : "");
        bool valid = true;
        for (char c : cand)
            if (!std::isalnum((unsigned char)c) && c != '_') valid = false;
        if (!std::isalpha((unsigned char)cand[0])) valid = false;
        if (valid && !params.count(cand)) return cand;
        if (n > 9999) return cand + "_x";
    }
}

std::string paramValuesKey(const ParamMap& params, const std::vector<std::string>& names) {
    std::string s;
    for (auto& n : names) {
        auto it = params.find(n);
        if (it == params.end()) continue;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", it->second.value);
        s += n + "=" + buf + ";";
    }
    return s;
}

} // namespace kst
