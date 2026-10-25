// Layer order resolution (docs/architecture.md §6.6, M6.10).
//
// Input: the scene and the current basemap's anchor map (anchor name ->
// basemap layer id). Output: the complete list of scene layers in bottom-to-
// top order, each with the concrete basemap layer id to insert *before* (or
// empty for "top"). Views feed this straight into `addLayer(id, params,
// before)` and never interpret placements themselves.
//
// Ordering rules:
//   * Layers group by anchor. Within a group, sessions order by priority
//     (lower first = further down), then by session name, then by the
//     session's own layer order.
//   * `before X` / `above X` inherit X's group and sit immediately before /
//     after X; chains resolve transitively, cycles and unknown X fall back to
//     `top` and are reported.
//   * An anchor the basemap lacks resolves to `top` and is reported once per
//     resolution in `missingAnchors`.
#pragma once

#include "engine/scene.h"

#include <string>
#include <vector>

namespace maps::engine {

struct ResolvedLayer {
    std::string layerId;  // global id
    std::string before;   // basemap layer id, or "" for top
    std::string anchor;   // resolved anchor name ("top" when none)
};

struct ResolvedOrder {
    std::vector<ResolvedLayer> layers;          // bottom-to-top apply order
    std::vector<std::string> missingAnchors;    // anchor names the basemap lacked (sorted, unique)
    std::vector<std::string> danglingRefs;      // before/above targets not found (sorted, unique)
    json toJson() const;
};

// `anchors` is the descriptor's `anchors` object: {"labels": "<id>", "routes": "<id>", ...}.
// An anchor whose value is "" is known and resolves to top (raster basemaps, M9.2).
ResolvedOrder resolveOrder(const Scene& scene, const json& anchors);

}  // namespace maps::engine
