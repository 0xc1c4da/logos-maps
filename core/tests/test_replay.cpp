// §16.2 Scene replay: a fake view applies the published op stream (live, or
// late-joined through changesSince) and must converge to the store's scene.
// Fixtures under fixtures/replay/*.json drive the batches; the expected final
// scene digest and published op tokens are pinned in *.expected.json so that
// any change to coalescing or ordering is a visible fixture change.
#include <logos_test.h>

#include "engine/canon.h"
#include "engine/store.h"
#include "test_util.h"

#include <iostream>
#include <map>

using nlohmann::json;
using namespace maps::engine;

namespace {

// What a view keeps per session after applying ops. Mirrors Session minus
// owner/priority (which travel separately) so the projection comparison is exact.
struct FakeView {
    std::map<std::string, json> sources;  // gid -> {id,type,def,data,dataDigest}
    std::map<std::string, json> images;   // gid -> image json
    std::map<std::string, std::vector<json>> layers;  // session -> ordered layers

    void apply(const json& op) {
        const std::string kind = op["op"].get<std::string>();
        if (kind == "dropSession") {
            const std::string s = op["session"].get<std::string>();
            for (auto it = sources.begin(); it != sources.end();) it = (sessionOf(it->first) == s) ? sources.erase(it) : std::next(it);
            for (auto it = images.begin(); it != images.end();) it = (sessionOf(it->first) == s) ? images.erase(it) : std::next(it);
            layers.erase(s);
        } else if (kind == "setSessionPriority") {
            // order-only; nothing in the projection
        } else if (kind == "removeLayer") {
            auto& v = layers[sessionOf(op["id"].get<std::string>())];
            v.erase(std::remove_if(v.begin(), v.end(), [&](const json& l) { return l["id"] == op["id"]; }), v.end());
        } else if (kind == "removeSource") {
            sources.erase(op["id"].get<std::string>());
        } else if (kind == "removeImage") {
            images.erase(op["id"].get<std::string>());
        } else if (kind == "putSource") {
            json& s = sources[op["id"].get<std::string>()];
            s["id"] = op["id"]; s["type"] = op["type"]; s["def"] = op["def"];
        } else if (kind == "replaceGeoJson") {
            json& s = sources.at(op["sourceId"].get<std::string>());
            s["data"] = op["data"]; s["dataDigest"] = op["digest"];
        } else if (kind == "putImage") {
            json img = op; img.erase("op"); img.erase("session"); img.erase("revision");
            images[op["id"].get<std::string>()] = img;
        } else if (kind == "putLayer") {
            json l = op; l.erase("op"); l.erase("session"); l.erase("revision");
            auto& v = layers[sessionOf(l["id"].get<std::string>())];
            auto it = std::find_if(v.begin(), v.end(), [&](const json& x) { return x["id"] == l["id"]; });
            if (it != v.end()) *it = l; else v.push_back(l);
        } else {
            throw std::runtime_error("fake view: unknown op " + kind);
        }
    }

    // A published putLayer stream re-sends every layer of a session whose
    // order changed; replaying in order therefore rebuilds the order only if
    // we re-append. Normalise: when a batch re-sends layers, rebuild from the
    // ops' order. We approximate by sorting sessions' layers by the order of
    // the *last* putLayer burst, which is what applyRevision below does.
    void applyRevision(const json& ops) {
        std::map<std::string, std::vector<std::string>> burst;  // session -> ids in op order
        for (const auto& op : ops) {
            apply(op);
            if (op["op"] == "putLayer") burst[sessionOf(op["id"].get<std::string>())].push_back(op["id"].get<std::string>());
        }
        for (auto& [session, ids] : burst) {
            auto& v = layers[session];
            if (ids.size() != v.size()) continue;  // partial update: order unchanged by contract
            std::vector<json> reordered;
            for (const auto& id : ids)
                reordered.push_back(*std::find_if(v.begin(), v.end(), [&](const json& x) { return x["id"] == id; }));
            v = reordered;
        }
    }

    json projection() const {
        json out = json::object();
        for (const auto& [gid, s] : sources) out[sessionOf(gid)]["sources"][gid] = s;
        for (const auto& [gid, i] : images) out[sessionOf(gid)]["images"][gid] = i;
        for (const auto& [session, ls] : layers) if (!ls.empty()) out[session]["layers"] = ls;
        // Sessions with nothing left are dropped from the projection.
        for (auto it = out.begin(); it != out.end();) {
            const bool empty = !it->contains("sources") && !it->contains("images") && !it->contains("layers");
            it = empty ? out.erase(it) : std::next(it);
        }
        return out;
    }
};

json storeProjection(const Store& st) {
    json out = json::object();
    for (const auto& [name, s] : st.scene().sessions) {
        if (s.sources.empty() && s.images.empty() && s.layers.empty()) continue;
        json j = s.toJson();
        json p = json::object();
        if (!s.sources.empty()) p["sources"] = j["sources"];
        if (!s.images.empty()) p["images"] = j["images"];
        if (!s.layers.empty()) p["layers"] = j["layers"];
        out[name] = p;
    }
    return out;
}

struct Run {
    Store store;
    FakeView live;
    std::vector<std::string> tokens;
};

void runFixture(const json& fx, Run& run) {
    for (const auto& b : fx["batches"]) {
        const std::string session = b["session"].get<std::string>();
        const std::string caller = b.value("caller", "");
        BatchResult r = run.store.applyBatch(session, b["ops"], caller);
        if (b.contains("expectPublished")) LOGOS_ASSERT_EQ(r.published, b["expectPublished"].get<bool>());
        json rej = json::array();
        for (const auto& x : r.rejected) rej.push_back(x.index);
        LOGOS_ASSERT_EQ(rej, b.value("expectRejected", json::array()));
        if (r.published) {
            run.live.applyRevision(r.ops);
            for (const auto& t : mt::opTokens(r.ops)) run.tokens.push_back(std::to_string(r.revision) + "/" + t);
        }
    }
}

}  // namespace

LOGOS_TEST(replay_live_view_converges_to_store_scene) {
    const json fx = mt::readJsonFixture("replay/route-and-pois.json");
    Run run;
    runFixture(fx, run);
    LOGOS_ASSERT_EQ(canonical(run.live.projection()), canonical(storeProjection(run.store)));
}

LOGOS_TEST(replay_late_view_catches_up_via_changes_since) {
    const json fx = mt::readJsonFixture("replay/route-and-pois.json");
    Run run;
    runFixture(fx, run);
    // A view that joins at revision 0 and pulls the log.
    FakeView late;
    json changes = run.store.changesSince(0);
    LOGOS_ASSERT_FALSE(changes.value("full", false));
    std::map<std::uint64_t, json> byRev;
    for (const auto& op : changes["ops"]) byRev[op["revision"].get<std::uint64_t>()].push_back(op);
    for (const auto& [rev, ops] : byRev) late.applyRevision(ops);
    LOGOS_ASSERT_EQ(canonical(late.projection()), canonical(storeProjection(run.store)));

    // A view that joins at revision 2 with a snapshot and then the delta.
    Run run2;
    FakeView mid;
    std::size_t i = 0;
    for (const auto& b : fx["batches"]) {
        run2.store.applyBatch(b["session"].get<std::string>(), b["ops"], b.value("caller", ""));
        if (++i == 2) break;
    }
    const json snap = run2.store.sceneJson();
    const std::uint64_t snapRev = snap["revision"].get<std::uint64_t>();
    for (const auto& [name, s] : snap["sessions"].items()) {
        for (const auto& [gid, src] : s["sources"].items()) mid.sources[gid] = src;
        for (const auto& [gid, img] : s["images"].items()) mid.images[gid] = img;
        for (const auto& l : s["layers"]) mid.layers[name].push_back(l);
    }
    for (; i < fx["batches"].size(); ++i) {
        const json& b = fx["batches"][i];
        run2.store.applyBatch(b["session"].get<std::string>(), b["ops"], b.value("caller", ""));
    }
    json delta = run2.store.changesSince(snapRev);
    std::map<std::uint64_t, json> byRev2;
    for (const auto& op : delta["ops"]) byRev2[op["revision"].get<std::uint64_t>()].push_back(op);
    for (const auto& [rev, ops] : byRev2) mid.applyRevision(ops);
    LOGOS_ASSERT_EQ(canonical(mid.projection()), canonical(storeProjection(run2.store)));
}

LOGOS_TEST(replay_is_deterministic_and_matches_pinned_expectation) {
    const json fx = mt::readJsonFixture("replay/route-and-pois.json");
    Run a, b;
    runFixture(fx, a);
    runFixture(fx, b);
    LOGOS_ASSERT_EQ(mt::join(a.tokens, "\n"), mt::join(b.tokens, "\n"));
    LOGOS_ASSERT_EQ(a.store.revision(), b.store.revision());

    json actual = {{"v", 1},
                   {"revision", a.store.revision()},
                   {"sceneDigest", digest(storeProjection(a.store))},
                   {"publishedOps", a.tokens}};
    json expected;
    try {
        expected = mt::readJsonFixture("replay/route-and-pois.expected.json");
    } catch (const std::exception&) {
        std::cerr << "missing expectation; actual is:\n" << actual.dump(2) << "\n";
        throw;
    }
    if (actual != expected) std::cerr << "actual:\n" << actual.dump(2) << "\n";
    LOGOS_ASSERT_EQ(actual, expected);
}

LOGOS_TEST(replay_after_style_reload_is_full_scene_in_apply_order) {
    // M6.8: on style reload a view re-applies the full scene from the current
    // revision. The full form of changesSince must therefore carry every
    // session; apply order is sources -> images -> layers per session.
    Limits lim;
    lim.revisionLogLength = 1;
    const json fx = mt::readJsonFixture("replay/route-and-pois.json");
    Store st(lim);
    for (const auto& b : fx["batches"]) st.applyBatch(b["session"].get<std::string>(), b["ops"], b.value("caller", ""));
    json full = st.changesSince(1);
    LOGOS_ASSERT_TRUE(full.value("full", false));
    LOGOS_ASSERT_EQ(full["scene"]["revision"], json(st.revision()));
    LOGOS_ASSERT_EQ(canonical(full["scene"]["sessions"]), canonical(st.scene().toJson()["sessions"]));
}
