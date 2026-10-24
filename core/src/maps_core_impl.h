#pragma once

#include <cstdint>
#include <map>
#include <string>
#include "logos_module_context.h"

/**
 * maps_core -- the layer registry behind the Maps app.
 *
 * Headless: runs in its own logos_host process, under Basecamp or logosctl
 * alike. Every public method here IS the module API: callable from any other
 * module (typed, via its generated `modules().maps_core` client), from the
 * Maps view's backend, and from the CLI. The Qt plugin glue is generated
 * from this header by logos-module-builder; this class stays Qt-free.
 *
 * Data model (deliberately opaque to the core so callers own the schema):
 *   - a layer is a JSON object the caller supplies; `layerId` is the key.
 *     Suggested keys: "title", "kind" ("markers" | "geojson" | "tiles"),
 *     "style", "visible", "zIndex".
 *   - a layer's features are one GeoJSON FeatureCollection (also opaque).
 *
 * Only `bool`, `int64_t`, `std::string` and std containers of them cross the
 * module boundary (LIDL-typed); structured data travels as JSON text.
 */
class MapsCoreImpl : public LogosModuleContext
{
public:
    /// Add a layer, or replace its definition if `layerId` exists.
    /// `layerJson` must be a JSON object. Returns false (and changes
    /// nothing) if the id is empty or the payload is not an object.
    bool addLayer(const std::string& layerId, const std::string& layerJson);

    /// Remove a layer and its features. Returns false if unknown.
    bool removeLayer(const std::string& layerId);

    /// Replace the features of an existing layer with a GeoJSON
    /// FeatureCollection. Returns false if the layer is unknown.
    bool setFeatures(const std::string& layerId, const std::string& geojson);

    /// The layer definition as given to addLayer(), or "" if unknown.
    std::string getLayer(const std::string& layerId);

    /// The layer's features (GeoJSON text), "" if none were set.
    std::string getFeatures(const std::string& layerId);

    /// Every layer as one JSON array of {"id": ..., "layer": {...}} objects,
    /// in id order.
    std::string listLayers();

    /// Number of registered layers.
    int64_t layerCount();

logos_events:
    /// A layer was added (first addLayer for this id).
    void layerAdded(const std::string& layerId);
    /// A layer's definition or features changed.
    void layerChanged(const std::string& layerId);
    /// A layer was removed.
    void layerRemoved(const std::string& layerId);

private:
    struct Layer {
        std::string definition;  // JSON object text
        std::string features;    // GeoJSON text, may be empty
    };
    std::map<std::string, Layer> m_layers;
};
