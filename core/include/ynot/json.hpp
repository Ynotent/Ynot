// Minimal, dependency-free JSON value used by the project format and the
// collaboration layer. Objects keep insertion order so saved projects diff
// cleanly in version control and serialize identically on every platform.
// Copies have value semantics (copy-on-write internally).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ynot {

class Json;
using JsonArray = std::vector<Json>;
using JsonObject = std::vector<std::pair<std::string, Json>>;

struct JsonError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : v_(b) {}
    Json(int n) : v_(static_cast<double>(n)) {}
    Json(int64_t n) : v_(static_cast<double>(n)) {}
    Json(uint32_t n) : v_(static_cast<double>(n)) {}
    Json(double n) : v_(n) {}
    Json(const char* s) : v_(std::string(s)) {}
    Json(std::string s) : v_(std::move(s)) {}
    Json(JsonArray a) : v_(std::make_shared<JsonArray>(std::move(a))) {}
    Json(JsonObject o) : v_(std::make_shared<JsonObject>(std::move(o))) {}

    static Json array() { return Json(JsonArray{}); }
    static Json object() { return Json(JsonObject{}); }

    Type type() const;
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isNumber() const { return type() == Type::Number; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    bool asBool() const;
    double asNumber() const;
    int64_t asInt() const;
    const std::string& asString() const;
    const JsonArray& asArray() const;
    JsonArray& asArray();
    const JsonObject& asObject() const;
    JsonObject& asObject();

    // Object helpers.
    bool contains(const std::string& key) const;
    const Json& at(const std::string& key) const;          // throws if missing
    Json& operator[](const std::string& key);              // inserts null if missing
    const Json* find(const std::string& key) const;
    bool erase(const std::string& key);

    // Typed getters with defaults (for tolerant loading of older files).
    double getNumber(const std::string& key, double def) const;
    int64_t getInt(const std::string& key, int64_t def) const;
    bool getBool(const std::string& key, bool def) const;
    std::string getString(const std::string& key, const std::string& def) const;

    // Array helpers.
    void push(Json v);
    size_t size() const;


    std::string dump(int indent = -1) const;
    static Json parse(const std::string& text);

    bool operator==(const Json& other) const;
    bool operator!=(const Json& other) const { return !(*this == other); }

private:
    std::variant<std::monostate, bool, double, std::string,
                 std::shared_ptr<JsonArray>, std::shared_ptr<JsonObject>>
        v_;
    void dumpTo(std::string& out, int indent, int depth) const;
    void detach();  // copy-on-write: unshare before mutating
};

}  // namespace ynot
