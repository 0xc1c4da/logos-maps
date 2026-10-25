// Shared helpers for the maps_engine unit tests.
#pragma once

#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mt {  // short, and no clash with the engine namespace

using nlohmann::json;

inline std::string join(const std::vector<std::string>& v, const char* sep = ",") {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

inline std::string fixturePath(const std::string& relative) {
    return std::string(MAPS_TEST_FIXTURES_DIR) + "/" + relative;
}

inline std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline json readJsonFixture(const std::string& relative) {
    return json::parse(readFile(fixturePath(relative)));
}

// A minimal FeatureCollection with n point features carrying ids 0..n-1.
inline json points(int n, double offset = 0.0) {
    json features = json::array();
    for (int i = 0; i < n; ++i) {
        features.push_back({{"type", "Feature"}, {"id", i},
                            {"geometry", {{"type", "Point"}, {"coordinates", {offset + i, offset}}}},
                            {"properties", {{"n", i}}}});
    }
    return json{{"type", "FeatureCollection"}, {"features", features}};
}

inline json op(const char* kind, json fields = json::object()) {
    fields["op"] = kind;
    return fields;
}

// Ids of the ops in a published diff, as "op:id" tokens in order.
inline std::vector<std::string> opTokens(const json& ops) {
    std::vector<std::string> out;
    for (const auto& o : ops) {
        std::string id;
        if (o.contains("id")) id = o["id"].get<std::string>();
        else if (o.contains("sourceId")) id = o["sourceId"].get<std::string>();
        else if (o.contains("session")) id = o["session"].get<std::string>();
        out.push_back(o["op"].get<std::string>() + ":" + id);
    }
    return out;
}

}  // namespace mt
