#include "ynot/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ynot {

Json::Type Json::type() const {
    switch (v_.index()) {
        case 0: return Type::Null;
        case 1: return Type::Bool;
        case 2: return Type::Number;
        case 3: return Type::String;
        case 4: return Type::Array;
        default: return Type::Object;
    }
}

void Json::detach() {
    if (auto* a = std::get_if<std::shared_ptr<JsonArray>>(&v_)) {
        if (a->use_count() > 1) *a = std::make_shared<JsonArray>(**a);
    } else if (auto* o = std::get_if<std::shared_ptr<JsonObject>>(&v_)) {
        if (o->use_count() > 1) *o = std::make_shared<JsonObject>(**o);
    }
}

bool Json::asBool() const {
    if (!isBool()) throw JsonError("json: expected bool");
    return std::get<bool>(v_);
}
double Json::asNumber() const {
    if (!isNumber()) throw JsonError("json: expected number");
    return std::get<double>(v_);
}
int64_t Json::asInt() const { return static_cast<int64_t>(std::llround(asNumber())); }
const std::string& Json::asString() const {
    if (!isString()) throw JsonError("json: expected string");
    return std::get<std::string>(v_);
}
const JsonArray& Json::asArray() const {
    if (!isArray()) throw JsonError("json: expected array");
    return *std::get<std::shared_ptr<JsonArray>>(v_);
}
JsonArray& Json::asArray() {
    if (!isArray()) throw JsonError("json: expected array");
    detach();
    return *std::get<std::shared_ptr<JsonArray>>(v_);
}
const JsonObject& Json::asObject() const {
    if (!isObject()) throw JsonError("json: expected object");
    return *std::get<std::shared_ptr<JsonObject>>(v_);
}
JsonObject& Json::asObject() {
    if (!isObject()) throw JsonError("json: expected object");
    detach();
    return *std::get<std::shared_ptr<JsonObject>>(v_);
}

const Json* Json::find(const std::string& key) const {
    if (!isObject()) return nullptr;
    for (auto& [k, v] : asObject())
        if (k == key) return &v;
    return nullptr;
}
bool Json::contains(const std::string& key) const { return find(key) != nullptr; }
const Json& Json::at(const std::string& key) const {
    if (auto* p = find(key)) return *p;
    throw JsonError("json: missing key '" + key + "'");
}
Json& Json::operator[](const std::string& key) {
    if (isNull()) *this = Json::object();
    auto& obj = asObject();
    for (auto& [k, v] : obj)
        if (k == key) return v;
    obj.emplace_back(key, Json());
    return obj.back().second;
}
bool Json::erase(const std::string& key) {
    if (!isObject()) return false;
    auto& obj = asObject();
    for (auto it = obj.begin(); it != obj.end(); ++it)
        if (it->first == key) { obj.erase(it); return true; }
    return false;
}

double Json::getNumber(const std::string& key, double def) const {
    auto* p = find(key);
    return (p && p->isNumber()) ? p->asNumber() : def;
}
int64_t Json::getInt(const std::string& key, int64_t def) const {
    auto* p = find(key);
    return (p && p->isNumber()) ? p->asInt() : def;
}
bool Json::getBool(const std::string& key, bool def) const {
    auto* p = find(key);
    return (p && p->isBool()) ? p->asBool() : def;
}
std::string Json::getString(const std::string& key, const std::string& def) const {
    auto* p = find(key);
    return (p && p->isString()) ? p->asString() : def;
}

void Json::push(Json v) {
    if (isNull()) *this = Json::array();
    asArray().push_back(std::move(v));
}
size_t Json::size() const {
    if (isArray()) return asArray().size();
    if (isObject()) return asObject().size();
    return 0;
}

bool Json::operator==(const Json& o) const {
    if (type() != o.type()) return false;
    switch (type()) {
        case Type::Null: return true;
        case Type::Bool: return asBool() == o.asBool();
        case Type::Number: return asNumber() == o.asNumber();
        case Type::String: return asString() == o.asString();
        case Type::Array: return asArray() == o.asArray();
        case Type::Object: {
            auto& a = asObject();
            auto& b = o.asObject();
            if (a.size() != b.size()) return false;
            for (auto& [k, v] : a) {
                auto* other = o.find(k);
                if (!other || *other != v) return false;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- dump

static void escapeString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

static void formatNumber(std::string& out, double d) {
    if (!std::isfinite(d)) { out += "null"; return; }
    if (d == std::floor(d) && std::fabs(d) < 9.007199254740992e15) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(d));
        out += buf;
        return;
    }
    // Shortest representation that round-trips; locale-independent ('.' only).
    char buf[40];
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    for (char* p = buf; *p; ++p)
        if (*p == ',') *p = '.';
    out += buf;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * d), ' ');
    };
    switch (type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += asBool() ? "true" : "false"; break;
        case Type::Number: formatNumber(out, asNumber()); break;
        case Type::String: escapeString(out, asString()); break;
        case Type::Array: {
            auto& a = asArray();
            if (a.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                a[i].dumpTo(out, indent, depth + 1);
            }
            newline(depth);
            out += ']';
            break;
        }
        case Type::Object: {
            auto& o = asObject();
            if (o.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < o.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                escapeString(out, o[i].first);
                out += indent < 0 ? ":" : ": ";
                o[i].second.dumpTo(out, indent, depth + 1);
            }
            newline(depth);
            out += '}';
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------- parse

namespace {
struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;

    [[noreturn]] void fail(const char* what) {
        throw JsonError(std::string("json parse error at offset ") + std::to_string(i) + ": " + what);
    }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    bool lit(const char* l) {
        size_t n = std::char_traits<char>::length(l);
        if (s.compare(i, n, l) == 0) { i += n; return true; }
        return false;
    }
    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    uint32_t hex4() {
        if (i + 4 > s.size()) fail("bad \\u escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else fail("bad hex digit");
        }
        return v;
    }
    std::string str() {
        if (s[i] != '"') fail("expected string");
        ++i;
        std::string out;
        while (true) {
            if (i >= s.size()) fail("unterminated string");
            char c = s[i++];
            if (c == '"') break;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) fail("bad escape");
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
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && lit("\\u")) {
                        uint32_t lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
        return out;
    }
    Json value() {
        if (++depth > 256) fail("nesting too deep");
        ws();
        if (i >= s.size()) fail("unexpected end");
        Json result;
        char c = s[i];
        if (c == '{') {
            ++i;
            result = Json::object();
            auto& obj = result.asObject();
            ws();
            if (i < s.size() && s[i] == '}') { ++i; }
            else {
                while (true) {
                    ws();
                    std::string k = str();
                    ws();
                    if (i >= s.size() || s[i] != ':') fail("expected ':'");
                    ++i;
                    obj.emplace_back(std::move(k), value());
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    fail("expected ',' or '}'");
                }
            }
        } else if (c == '[') {
            ++i;
            result = Json::array();
            auto& arr = result.asArray();
            ws();
            if (i < s.size() && s[i] == ']') { ++i; }
            else {
                while (true) {
                    arr.push_back(value());
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    fail("expected ',' or ']'");
                }
            }
        } else if (c == '"') {
            result = Json(str());
        } else if (lit("true")) {
            result = Json(true);
        } else if (lit("false")) {
            result = Json(false);
        } else if (lit("null")) {
            result = Json();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = i;
            if (s[i] == '-') ++i;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                                    s[i] == 'E' || s[i] == '+' || s[i] == '-'))
                ++i;
            std::string num = s.substr(start, i - start);
            char* end = nullptr;
            double d = std::strtod(num.c_str(), &end);
            if (!end || *end != '\0') fail("bad number");
            result = Json(d);
        } else {
            fail("unexpected character");
        }
        --depth;
        return result;
    }
};
}  // namespace

Json Json::parse(const std::string& text) {
    Parser p{text};
    Json v = p.value();
    p.ws();
    if (p.i != text.size()) p.fail("trailing characters");
    return v;
}

}  // namespace ynot
