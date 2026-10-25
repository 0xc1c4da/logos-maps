// Scene store: SceneBatch validation and application, coalescing, the
// identity gate and the revision log (docs/architecture.md §6.4–§6.5, M11.2).
//
// How coalescing works here. A batch is applied op by op to a *working copy*
// of the session; ops that fail validation are rejected individually and
// skipped (M11.2), the rest commit together by swapping the copy in (M6.7).
// The ops published to views are the *diff* between the committed session
// before and after the batch: that is "last state wins" (M6.5) with no
// per-op bookkeeping, and a source whose data digest did not change emits no
// replaceGeoJson -- the identity gate of M6.6 falls out of the same diff. A
// batch whose diff is empty publishes no revision.
#pragma once

#include "engine/scene.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace maps::engine {

// Whole-call failure (malformed payload, forbidden session). Individual op
// failures are reported in BatchResult::rejected instead. `code` is one of
// the M11.2 codes ("maps.badArgument", "maps.unknownSession", ...).
class MapsError : public std::runtime_error {
public:
    MapsError(std::string code, const std::string& message)
        : std::runtime_error(message), code(std::move(code)) {}
    std::string code;
};

struct Limits {
    std::size_t maxBatchBytes = 4u << 20;    // §13 M13.2 default 4 MiB
    std::size_t maxImageBytes = 1u << 20;    // decoded PNG payload (base64 length * 3/4)
    std::size_t maxSessionBytes = 32u << 20; // canonical size of one session (§15.3)
    std::size_t revisionLogLength = 256;     // changesSince() window
};

struct Rejection {
    std::size_t index;
    std::string code;
    std::string message;
    json toJson() const { return json{{"index", index}, {"code", code}, {"message", message}}; }
};

struct BatchResult {
    std::uint64_t revision = 0;        // revision after the call (unchanged if nothing published)
    bool published = false;            // false when the diff was empty (identity gate)
    std::vector<std::size_t> accepted; // op indices applied
    std::vector<std::size_t> noop;     // accepted ops that changed nothing
    std::vector<Rejection> rejected;
    json ops = json::array();          // the published ops (diff), in view apply order
    json toJson() const;
};

// One published revision: the ops views apply to go from rev-1 to rev.
struct Published {
    std::uint64_t revision;
    std::string session;
    json ops;
};

class Store {
public:
    explicit Store(Limits limits = {});

    // §12.1 applyBatch. `caller` is the module identity delivered by the host
    // ("" when unknown, e.g. under logosctl or in tests). The first batch
    // naming a session creates it and records `caller` as owner; a later
    // batch from a different non-empty caller is refused (maps.badArgument).
    //
    // `batch` is either {"v":1,"ops":[...]} or a bare array of ops (v = 1).
    BatchResult applyBatch(const std::string& session, const json& batch, const std::string& caller);

    // §12.1 dropSession. Returns false if the session does not exist.
    bool dropSession(const std::string& session, const std::string& caller);

    // Host-assigned ordering between sessions (M6.10). Publishes a revision
    // (views re-resolve order) when the value changes.
    bool setSessionPriority(const std::string& session, int priority);

    const Scene& scene() const { return m_scene; }
    std::uint64_t revision() const { return m_revision; }
    const Limits& limits() const { return m_limits; }

    // {"v":1,"revision":N,"sessions":{...}} or one session's subtree.
    json sceneJson(const std::optional<std::string>& session = std::nullopt) const;

    // Ops published after `since`, oldest first:
    //   {"v":1,"from":since,"to":revision,"ops":[...]}            (incremental)
    //   {"v":1,"from":since,"to":revision,"full":true,"scene":…}   (window exceeded)
    json changesSince(std::uint64_t since) const;

private:
    struct Working;  // per-batch state
    void validateAndApply(Working& w, std::size_t index, const json& op);
    json diffSession(const Session* before, const Session& after) const;
    std::uint64_t publish(const std::string& session, json ops);

    Limits m_limits;
    Scene m_scene;
    std::uint64_t m_revision = 0;
    std::deque<Published> m_log;
};

// Order in which views apply published ops (M6.5): removes first (layers,
// then sources, then images), then putSource, replaceGeoJson, putImage,
// putLayer (in session layer order). Exposed for tests.
int opRank(const std::string& op);

}  // namespace maps::engine
