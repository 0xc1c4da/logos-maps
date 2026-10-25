// Canonical JSON text and digests for the identity gate (docs/architecture.md
// M6.6) and for fixture comparison (§16.2).
//
// Unlike quests (RFC 8785, no floats) the maps scene is full of IEEE doubles
// (coordinates), so the canonical form is nlohmann's compact dump: members
// sorted by key (nlohmann::json objects are std::map-ordered), shortest
// round-trip doubles, no insignificant whitespace. Two scene values with the
// same canonical text are the same scene value; that is all M6.6 needs.
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace maps::engine {

std::string canonical(const nlohmann::json& value);

// Lowercase hex SHA-256 of `bytes` (OpenSSL libcrypto).
std::string sha256Hex(std::string_view bytes);

// sha256Hex(canonical(value)).
std::string digest(const nlohmann::json& value);

}  // namespace maps::engine
