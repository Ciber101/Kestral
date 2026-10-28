#include "core/Json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace kst::json {

namespace {
const Value kNull;

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool fail(const char* m) {
        if (err.empty()) err = std::string(m) + " at " + std::to_string(i);
        return false;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return fail("bad escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return fail("bad hex");
        }
        return true;
    }
    bool str(std::string& out) {
        if (s[i] != '"') return fail("expected string");
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    i += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(Value& v, int depth) {
        if (depth > 200) return fail("too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        if (c == '{') {
            ++i;
            v = Value::object();
            ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                ws();
                std::string k;
                if (i >= s.size() || !str(k)) return fail("expected key");
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected :");
                ++i;
                Value child;
                if (!value(child, depth + 1)) return false;
                v.set(k, std::move(child));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return fail("expected , or }");
            }
        }
        if (c == '[') {
            ++i;
            v = Value::array();
            ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                Value child;
                if (!value(child, depth + 1)) return false;
                v.push(std::move(child));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return fail("expected , or ]");
            }
        }
        if (c == '"') {
            std::string out;
            if (!str(out)) return false;
            v = Value(std::move(out));
            return true;
        }
        if (s.compare(i, 4, "true") == 0) { i += 4; v = Value(true); return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; v = Value(false); return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; v = Value(); return true; }
        // Number
        size_t start = i;
        if (s[i] == '-') ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
        if (i == start) return fail("unexpected character");
        v = Value(std::strtod(s.c_str() + start, nullptr));
        return true;
    }
};
} // namespace

const Value& Value::operator[](size_t i) const {
    if (type_ != Type::Array || i >= a_.size()) return kNull;
    return a_[i];
}

Value& Value::push(Value v) {
    if (type_ != Type::Array) { *this = array(); }
    a_.push_back(std::move(v));
    return a_.back();
}

bool Value::has(const std::string& k) const {
    if (type_ != Type::Object) return false;
    for (auto& [key, v] : o_)
        if (key == k) return true;
    return false;
}

const Value& Value::operator[](const std::string& k) const {
    if (type_ != Type::Object) return kNull;
    for (auto& [key, v] : o_)
        if (key == k) return v;
    return kNull;
}

Value& Value::set(const std::string& k, Value v) {
    if (type_ != Type::Object) { *this = object(); }
    for (auto& [key, old] : o_)
        if (key == k) { old = std::move(v); return old; }
    o_.emplace_back(k, std::move(v));
    return o_.back().second;
}

void Value::dumpTo(std::string& out) const {
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number: {
        if (!std::isfinite(n_)) { out += "0"; break; }
        char buf[32];
        if (n_ == std::floor(n_) && std::abs(n_) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", n_);
        else std::snprintf(buf, sizeof buf, "%.10g", n_);
        out += buf;
        break;
    }
    case Type::String: {
        out += '"';
        for (unsigned char c : s_) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
            }
        }
        out += '"';
        break;
    }
    case Type::Array: {
        out += '[';
        for (size_t k = 0; k < a_.size(); ++k) {
            if (k) out += ',';
            a_[k].dumpTo(out);
        }
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        bool first = true;
        for (auto& [k, v] : o_) {
            if (!first) out += ',';
            first = false;
            Value(k).dumpTo(out);
            out += ':';
            v.dumpTo(out);
        }
        out += '}';
        break;
    }
    }
}

std::string Value::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

Value Value::parse(const std::string& text, std::string* err) {
    Parser p{text};
    Value v;
    if (!p.value(v, 0)) {
        if (err) *err = p.err;
        return Value();
    }
    return v;
}

} // namespace kst::json
