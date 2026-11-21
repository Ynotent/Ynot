#include "ynot/collab.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>

#include "ynot/project_format.hpp"

namespace ynot {

bool Hlc::operator<(const Hlc& o) const {
    if (wallMs != o.wallMs) return wallMs < o.wallMs;
    if (counter != o.counter) return counter < o.counter;
    return actor < o.actor;
}

std::string Hlc::key() const {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%015lld.%08x.", static_cast<long long>(wallMs), counter);
    return std::string(buf) + actor;
}

static const char* kindName(OpKind k) {
    switch (k) {
        case OpKind::Set: return "set";
        case OpKind::Insert: return "insert";
        case OpKind::Remove: return "remove";
    }
    return "set";
}

Json Operation::toJson() const {
    Json j = Json::object();
    j["t"] = JsonArray{Json(ts.wallMs), Json(static_cast<int64_t>(ts.counter)), Json(ts.actor)};
    j["op"] = kindName(kind);
    Json p = Json::array();
    for (auto& s : path) p.push(JsonArray{Json(s.collection), Json(s.id)});
    j["path"] = p;
    j["key"] = key;
    j["value"] = value;
    return j;
}

Operation Operation::fromJson(const Json& j) {
    Operation op;
    auto& t = j.at("t").asArray();
    if (t.size() != 3) throw std::runtime_error("op: bad timestamp");
    op.ts = {t[0].asInt(), static_cast<uint32_t>(t[1].asInt()), t[2].asString()};
    auto k = j.at("op").asString();
    op.kind = k == "insert" ? OpKind::Insert : k == "remove" ? OpKind::Remove : OpKind::Set;
    for (auto& s : j.at("path").asArray()) {
        auto& a = s.asArray();
        op.path.push_back({a.at(0).asString(), a.at(1).asString()});
    }
    op.key = j.at("key").asString();
    op.value = j.contains("value") ? j.at("value") : Json();
    return op;
}

namespace {
Json* findById(Json& arr, const std::string& id) {
    if (!arr.isArray()) return nullptr;
    for (auto& e : arr.asArray())
        if (e.isObject() && e.getString("id", "") == id) return &e;
    return nullptr;
}

Json* resolve(Json& doc, const DocPath& path) {
    Json* node = &doc;
    for (auto& seg : path) {
        if (!node->isObject() || !node->contains(seg.collection)) return nullptr;
        node = findById((*node)[seg.collection], seg.id);
        if (!node) return nullptr;
    }
    return node;
}

// Fields that only make sense as whole-object replacements or are structural.
bool isProtectedKey(const std::string& k) { return k == "id" || k == "format" || k == "formatVersion"; }
}  // namespace

bool applyOperation(Json& doc, const Operation& op) {
    Json* node = resolve(doc, op.path);
    if (!node || !node->isObject()) return false;
    switch (op.kind) {
        case OpKind::Set:
            if (isProtectedKey(op.key)) return false;
            (*node)[op.key] = op.value;
            return true;
        case OpKind::Insert: {
            if (!op.value.isObject() || op.value.getString("id", "").empty()) return false;
            Json& coll = (*node)[op.key];
            if (coll.isNull()) coll = Json::array();
            if (!coll.isArray() || findById(coll, op.value.getString("id", ""))) return false;
            coll.push(op.value);
            return true;
        }
        case OpKind::Remove: {
            if (!op.value.isString() || !node->contains(op.key)) return false;
            Json& coll = (*node)[op.key];
            if (!coll.isArray()) return false;
            auto& arr = coll.asArray();
            for (auto it = arr.begin(); it != arr.end(); ++it)
                if (it->isObject() && it->getString("id", "") == op.value.asString()) {
                    arr.erase(it);
                    return true;
                }
            return false;
        }
    }
    return false;
}

static int64_t systemWallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Replica::Replica(std::string actorId, Json baseDocument, int64_t (*clock)())
    : actor_(std::move(actorId)), base_(std::move(baseDocument)), doc_(base_), wall_(clock ? clock : systemWallMs) {
    clock_.actor = actor_;
}

void Replica::observe(const Hlc& remote) {
    int64_t now = wall_();
    int64_t prev = clock_.wallMs;
    clock_.wallMs = std::max({prev, remote.wallMs, now});
    if (clock_.wallMs == prev && clock_.wallMs == remote.wallMs)
        clock_.counter = std::max(clock_.counter, remote.counter) + 1;
    else if (clock_.wallMs == prev)
        clock_.counter += 1;
    else if (clock_.wallMs == remote.wallMs)
        clock_.counter = remote.counter + 1;
    else
        clock_.counter = 0;
}

Operation Replica::stamp(OpKind kind, const DocPath& path, const std::string& key, Json value) {
    int64_t now = wall_();
    if (now > clock_.wallMs) { clock_.wallMs = now; clock_.counter = 0; }
    else clock_.counter += 1;
    Operation op{clock_, kind, path, key, std::move(value)};
    op.ts.actor = actor_;
    receive(op);
    return op;
}

Operation Replica::set(const DocPath& path, const std::string& key, Json value) {
    return stamp(OpKind::Set, path, key, std::move(value));
}
Operation Replica::insert(const DocPath& path, const std::string& collection, Json element) {
    return stamp(OpKind::Insert, path, collection, std::move(element));
}
Operation Replica::remove(const DocPath& path, const std::string& collection, const std::string& id) {
    return stamp(OpKind::Remove, path, collection, Json(id));
}

void Replica::receive(const Operation& op) {
    std::string k = op.ts.key();
    if (ops_.count(k)) return;  // duplicate delivery
    if (!compactedThrough_.empty() && k <= compactedThrough_) return;  // already folded into base
    if (op.ts.actor != actor_) observe(op.ts);
    ops_.emplace(k, op);
    if (lastAppliedKey_.empty() || lastAppliedKey_ < k) {
        applyOperation(doc_, op);  // fast path: in-order arrival
        lastAppliedKey_ = k;
    } else {
        rematerialize();  // arrived late: replay to keep the deterministic order
    }
}

void Replica::rematerialize() {
    doc_ = base_;
    for (auto& [k, op] : ops_) applyOperation(doc_, op);
    lastAppliedKey_ = ops_.empty() ? "" : ops_.rbegin()->first;
}

Project Replica::project() const { return projectFromJson(doc_); }

std::vector<Operation> Replica::opsSince(const std::string& afterKey) const {
    std::vector<Operation> v;
    for (auto it = ops_.upper_bound(afterKey); it != ops_.end(); ++it) v.push_back(it->second);
    return v;
}

void Replica::compact() {
    base_ = doc_;
    if (!lastAppliedKey_.empty()) compactedThrough_ = lastAppliedKey_;
    ops_.clear();
}

}  // namespace ynot
