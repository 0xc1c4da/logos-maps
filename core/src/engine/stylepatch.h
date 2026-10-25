// Declarative style patching (docs/architecture.md §9.3, M9.6–M9.7).
//
// Ops (each an object with "op" plus fields):
//   glyphs        {value}                      replace style.glyphs
//   sprite        {value}                      replace style.sprite
//   sourceUrl     {source, url | tiles[]}      rewrite one source's url/tiles (drops the other)
//   fontStack     {from?: [..], to: [..]}      rewrite text-font arrays; without `from` every
//                                              literal text-font becomes `to`
//   labelLanguage {lang, key?: "name:{lang}"}  text-field -> ["coalesce",["get",key],["get","name"]];
//                                              lang "local" restores ["get","name"]
//   hideLayers    {ids?: [..], prefixes?: [..]} layout.visibility = "none"
//   setPaint      {layer | layers[], props{}}   merge paint props (null deletes)
//   setLayout     {layer | layers[], props{}}   merge layout props (null deletes)
//   jsonPatch     {patch: [RFC 6902 ops]}      escape hatch
//
// Patching never renames or reorders layers (M9.7) and is idempotent: applying
// the same list twice yields the same style.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace maps::engine {

using json = nlohmann::json;

struct PatchReport {
    std::vector<std::string> warnings;  // e.g. unknown source, no-op hideLayers
    std::size_t layersTouched = 0;
};

// Throws std::invalid_argument on a malformed op or an unknown op name.
json applyStylePatch(json style, const json& ops, PatchReport* report = nullptr);

// The expression labelLanguage installs for `lang`, given the lang_key template.
json labelExpression(const std::string& lang, const std::string& langKeyTemplate = "name:{lang}");

}  // namespace maps::engine
