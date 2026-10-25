// Bundled basemap descriptors (§9.1 "bundled defaults"). Keyless, openly
// licensed providers only; anything needing a key is a user-added descriptor.
#pragma once

#include <nlohmann/json.hpp>

namespace maps::engine {

// Array of descriptors, each valid per Appendix C before normalisation.
nlohmann::json builtinBasemaps();

// The id setBasemap falls back to when a view opens without one.
const char* defaultBasemapId();

}  // namespace maps::engine
