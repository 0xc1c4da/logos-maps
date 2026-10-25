// The M11.1 capability report for this view (docs/architecture.md section 11).
// Compile-time facts come from the engine's qmaplibre_patchset.h (one define
// per applied section 14.1 patch); runtime facts (transport, providers) are
// filled in by the caller.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace maps::view {

struct CapabilityInputs {
    QString transport = QStringLiteral("none");  // wayland-dmabuf | wayland-shm | inproc | none
    QStringList providerSchemes;
    bool providerRangeRequests = true;
    bool customLayers = true;   // QMapLibre::Map::addCustomLayer exists in the binding
    bool offlineRegions = true; // engine DatabaseFileSource + offline.hpp always built
};

/// Which section 14.1 patches the engine was built with, e.g. ["P1","P4",...].
QStringList enginePatchSet();
/// The MLNQT commit the engine was built from (MAPS_ENGINE_BINDING_REV), or "unknown".
QString engineBindingRevision();

bool hasQueryRenderedFeatures();  // P1
bool hasFeatureState();           // P2
bool hasImageMetadata();          // P4
bool hasFileSourceHook();         // P5
bool hasLayerZoomVisibility();    // P6
bool hasExportedTextureNode();    // P7
bool hasCachePathPrefetch();      // P9
bool hasPlacementTransitionsToggle();  // P10 (idle map renders zero frames)

/// The nested capability object (keys are dotted in the spec table and nested
/// here, matching core/src/engine/capability.h getPath/setPath).
QJsonObject capabilities(const CapabilityInputs& inputs);

}  // namespace maps::view
