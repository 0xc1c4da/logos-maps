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
    // What LogosViewSurface::transport() reports (RFC-8): "wayland" (the
    // shm/dmabuf kind is the host's to observe), "channel-shm",
    // "channel-d3d11", "channel-iosurface", "inprocess" or "none".
    QString transport = QStringLiteral("none");
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
bool hasTextureNodeFramebufferRestore();  // P11 (texture node leaves the scene graph's FBO alone)
bool hasSharedContextProgramRebind();  // P12 (shader re-bound every frame of the shared GL context)

/// The nested capability object (keys are dotted in the spec table and nested
/// here, matching core/src/engine/capability.h getPath/setPath).
QJsonObject capabilities(const CapabilityInputs& inputs);

}  // namespace maps::view
