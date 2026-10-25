// Canonical text and digests (M6.6 identity gate input).
#include <logos_test.h>

#include "engine/canon.h"

using nlohmann::json;
using namespace maps::engine;

LOGOS_TEST(canon_sorts_members_and_strips_whitespace) {
    const json v = json::parse(R"({ "b": 1, "a": { "z": true, "y": null }, "c": [ 1, "x", false ] })");
    LOGOS_ASSERT_EQ(canonical(v), std::string(R"({"a":{"y":null,"z":true},"b":1,"c":[1,"x",false]})"));
}

LOGOS_TEST(canon_keeps_doubles_round_trippable) {
    const json v = json::parse(R"({"lon": 13.404954, "lat": 52.5200066, "z": 1.0})");
    const std::string c = canonical(v);
    LOGOS_ASSERT_EQ(json::parse(c), v);
    // Same value parsed from differently formatted text canonicalises identically.
    LOGOS_ASSERT_EQ(canonical(json::parse("{\"lat\":52.5200066,\n \"lon\":13.404954,\"z\":1.0}")), c);
}

LOGOS_TEST(sha256_known_vector) {
    LOGOS_ASSERT_EQ(sha256Hex("abc"),
                    std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    LOGOS_ASSERT_EQ(sha256Hex(""),
                    std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

LOGOS_TEST(digest_is_order_insensitive_for_objects) {
    LOGOS_ASSERT_EQ(digest(json::parse(R"({"a":1,"b":[1,2]})")), digest(json::parse(R"({"b":[1,2],"a":1})")));
    LOGOS_ASSERT_NE(digest(json::parse(R"({"a":1,"b":[1,2]})")), digest(json::parse(R"({"a":1,"b":[2,1]})")));
}
