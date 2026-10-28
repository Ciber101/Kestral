#pragma once
// Small JSON value with parse and dump: enough for talking to AI model APIs, no dependency.
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kst::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), b_(b) {}
    Value(int n) : type_(Type::Number), n_(n) {}
    Value(double n) : type_(Type::Number), n_(n) {}
    Value(const char* s) : type_(Type::String), s_(s) {}
    Value(std::string s) : type_(Type::String), s_(std::move(s)) {}

    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
    double asNumber(double def = 0) const { return type_ == Type::Number ? n_ : def; }
    std::string asString(const std::string& def = std::string()) const { return type_ == Type::String ? s_ : def; }

    // Arrays
    size_t size() const { return type_ == Type::Array ? a_.size() : type_ == Type::Object ? o_.size() : 0; }
    const Value& operator[](size_t i) const;
    Value& push(Value v);
    const std::vector<Value>& items() const { return a_; }

    // Objects (insertion order kept, which some APIs like for readability)
    bool has(const std::string& k) const;
    const Value& operator[](const std::string& k) const;
    const Value& operator[](const char* k) const { return (*this)[std::string(k)]; }
    Value& set(const std::string& k, Value v);
    const std::vector<std::pair<std::string, Value>>& members() const { return o_; }

    std::string dump() const;
    // Parses text; on failure returns Null and fills err.
    static Value parse(const std::string& text, std::string* err = nullptr);

private:
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> a_;
    std::vector<std::pair<std::string, Value>> o_;
    void dumpTo(std::string& out) const;
};

} // namespace kst::json
