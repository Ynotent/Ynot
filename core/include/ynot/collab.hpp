// Real-time collaboration & multi-device sync.
//
// Every edit is an Operation addressed into the project JSON document
// (the same document the file format stores). Operations carry a hybrid
// logical clock timestamp; replicas materialize state by applying the full set
// of known operations in timestamp order. Any two replicas that have received
// the same operations — in any order, over any transport — hold identical
// projects. This is what lets an iPhone, an Android tablet, a Windows PC and a
// Mac edit one song together, and lets offline edits merge on reconnect.
//
// Conflict rules (deterministic on every platform):
//   * concurrent Set on the same field: the later timestamp wins
//   * Set/Insert targeting something already removed: ignored (remove wins)
//   * Inserts never conflict (ids are globally unique)
//
// Audio is NOT sent through operations. Recordings are uploaded as immutable
// content-addressed blobs (media.hpp) and referenced by id.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ynot/json.hpp"
#include "ynot/model.hpp"

namespace ynot {

struct Hlc {
    int64_t wallMs = 0;
    uint32_t counter = 0;
    std::string actor;  // device/session id, tie-breaker
    bool operator<(const Hlc& o) const;
    bool operator==(const Hlc& o) const {
        return wallMs == o.wallMs && counter == o.counter && actor == o.actor;
    }
    std::string key() const;  // sortable, unique string
};

struct PathSeg {
    std::string collection;  // e.g. "tracks", "audioClips"
    std::string id;
};
using DocPath = std::vector<PathSeg>;

enum class OpKind { Set, Insert, Remove };

struct Operation {
    Hlc ts;
    OpKind kind = OpKind::Set;
    DocPath path;     // object being edited ([] = project root)
    std::string key;  // Set: field name. Insert/Remove: collection name
    Json value;       // Set: new value. Insert: element (must have "id"). Remove: id string

    Json toJson() const;
    static Operation fromJson(const Json& j);
};

// Applies one op to a project document. Returns false if it was a no-op.
bool applyOperation(Json& doc, const Operation& op);

class Replica {
public:
    // `clock` returns wall time in ms; injectable for tests.
    Replica(std::string actorId, Json baseDocument, int64_t (*clock)() = nullptr);

    // Local edits: applied immediately, returned for broadcast to peers/cloud.
    Operation set(const DocPath& path, const std::string& key, Json value);
    Operation insert(const DocPath& path, const std::string& collection, Json element);
    Operation remove(const DocPath& path, const std::string& collection, const std::string& id);

    // Remote ops (from relay or another device). Idempotent; any order.
    void receive(const Operation& op);

    const Json& document() const { return doc_; }
    Project project() const;
    size_t opCount() const { return ops_.size(); }
    std::vector<Operation> opsSince(const std::string& afterKey) const;  // for catch-up
    // Fold all ops into the base snapshot. Call only once the server confirms
    // every collaborator has acknowledged them.
    void compact();

private:
    Operation stamp(OpKind kind, const DocPath& path, const std::string& key, Json value);
    void observe(const Hlc& remote);
    void rematerialize();

    std::string actor_;
    Json base_;
    Json doc_;
    std::map<std::string, Operation> ops_;  // keyed by Hlc::key(), i.e. sorted
    std::string lastAppliedKey_;
    std::string compactedThrough_;
    Hlc clock_;
    int64_t (*wall_)();
};

// Convenience paths.
inline DocPath trackPath(const std::string& trackId) { return {{"tracks", trackId}}; }
inline DocPath clipPath(const std::string& trackId, const std::string& collection,
                        const std::string& clipId) {
    return {{"tracks", trackId}, {collection, clipId}};
}

}  // namespace ynot
