#include "view/capabilities.h"

#if __has_include(<QMapLibre/qmaplibre_patchset.h>)
#include <QMapLibre/qmaplibre_patchset.h>
#endif

#include <QJsonArray>
#include <QtGlobal>

namespace maps::view {

namespace {
constexpr bool kP1 =
#ifdef MAPS_ENGINE_HAS_P1_QUERY_RENDERED_FEATURES
    true;
#else
    false;
#endif
constexpr bool kP2 =
#ifdef MAPS_ENGINE_HAS_P2_FEATURE_STATE
    true;
#else
    false;
#endif
constexpr bool kP4 =
#ifdef MAPS_ENGINE_HAS_P4_ADD_IMAGE_METADATA
    true;
#else
    false;
#endif
constexpr bool kP5 =
#ifdef MAPS_ENGINE_HAS_P5_FILE_SOURCE_FACTORY_HOOK
    true;
#else
    false;
#endif
constexpr bool kP6 =
#ifdef MAPS_ENGINE_HAS_P6_LAYER_ZOOM_VISIBILITY
    true;
#else
    false;
#endif
constexpr bool kP7 =
#ifdef MAPS_ENGINE_HAS_P7_EXPORT_TEXTURE_NODE
    true;
#else
    false;
#endif
constexpr bool kP9 =
#ifdef MAPS_ENGINE_HAS_P9_CACHE_PATH_PREFETCH
    true;
#else
    false;
#endif
constexpr bool kP10 =
#ifdef MAPS_ENGINE_HAS_P10_PLACEMENT_TRANSITIONS
    true;
#else
    false;
#endif
constexpr bool kP11 =
#ifdef MAPS_ENGINE_HAS_P11_TEXTURE_NODE_FBO_RESTORE
    true;
#else
    false;
#endif
constexpr bool kP12 =
#ifdef MAPS_ENGINE_HAS_P12_SHARED_CONTEXT_PROGRAM_REBIND
    true;
#else
    false;
#endif
}  // namespace

bool hasQueryRenderedFeatures() { return kP1; }
bool hasFeatureState() { return kP2; }
bool hasImageMetadata() { return kP4; }
bool hasFileSourceHook() { return kP5; }
bool hasLayerZoomVisibility() { return kP6; }
bool hasExportedTextureNode() { return kP7; }
bool hasCachePathPrefetch() { return kP9; }
bool hasPlacementTransitionsToggle() { return kP10; }
bool hasTextureNodeFramebufferRestore() { return kP11; }
bool hasSharedContextProgramRebind() { return kP12; }

QStringList enginePatchSet()
{
    QStringList out;
    if (kP1) out << QStringLiteral("P1");
    if (kP4) out << QStringLiteral("P4");
    if (kP5) out << QStringLiteral("P5");
    if (kP7) out << QStringLiteral("P7");
    if (kP9) out << QStringLiteral("P9");
    if (kP2) out << QStringLiteral("P2");
    if (kP6) out << QStringLiteral("P6");
    if (kP10) out << QStringLiteral("P10");
    if (kP11) out << QStringLiteral("P11");
    if (kP12) out << QStringLiteral("P12");
    return out;
}

QString engineBindingRevision()
{
#ifdef MAPS_ENGINE_BINDING_REV
    return QStringLiteral(MAPS_ENGINE_BINDING_REV);
#else
    return QStringLiteral("unknown");
#endif
}

QJsonObject capabilities(const CapabilityInputs& in)
{
    QJsonObject engine{{QStringLiteral("version"), QStringLiteral("maplibre-native-qt")},
                       {QStringLiteral("bindingRevision"), engineBindingRevision()}};
    QJsonObject binding{{QStringLiteral("version"), engineBindingRevision().left(7)}};
    QJsonObject render{{QStringLiteral("customLayers"), in.customLayers},
                       {QStringLiteral("idleZeroFrames"), kP10},
                       {QStringLiteral("terrain"), true},
                       {QStringLiteral("fillExtrusion"), true},
                       {QStringLiteral("hillshade"), true}};
    QJsonObject scene{{QStringLiteral("featureState"), kP2},
                      {QStringLiteral("patchFeatures"), false},  // P3 deferred
                      {QStringLiteral("imageMetadata"), kP4}};
    QJsonObject pick{{QStringLiteral("renderedFeatures"), kP1},
                     {QStringLiteral("sourceFeatures"), kP1},
                     {QStringLiteral("boxDp"), 24},
                     {QStringLiteral("kinds"), QJsonArray{QStringLiteral("tap"), QStringLiteral("longPress"), QStringLiteral("context")}}};
    // Section 8: one detector for touch, handler stacks for pointer input.
    QJsonObject gestures{{QStringLiteral("touch"), true},
                         {QStringLiteral("pointer"), true},
                         {QStringLiteral("keyboard"), true},
                         {QStringLiteral("settings"), QJsonArray{QStringLiteral("rotateEnabled"), QStringLiteral("tiltEnabled"), QStringLiteral("zoomEnabled"),
                                                                QStringLiteral("scrollEnabled"), QStringLiteral("doubleTapEnabled"), QStringLiteral("quickZoomEnabled"),
                                                                QStringLiteral("twoFingerTapEnabled"), QStringLiteral("flingEnabled"), QStringLiteral("northLock"),
                                                                QStringLiteral("maxPitch"), QStringLiteral("focalPoint"), QStringLiteral("rotateWhileScaling"),
                                                                QStringLiteral("minZoom"), QStringLiteral("maxZoom"), QStringLiteral("mouseAsTouchScreen")}},
                         {QStringLiteral("builtins"), QJsonArray{QStringLiteral("puck"), QStringLiteral("scaleBar"), QStringLiteral("compass"), QStringLiteral("attribution")}}};
    QJsonObject provider{{QStringLiteral("schemes"), QJsonArray::fromStringList(in.providerSchemes)},
                         {QStringLiteral("rangeRequests"), in.providerRangeRequests}};
    QJsonObject offline{{QStringLiteral("regions"), in.offlineRegions}};
    QJsonObject surface{{QStringLiteral("transport"), in.transport}};
    QJsonObject limits{{QStringLiteral("maxImageSize"), 4096},
                       {QStringLiteral("maxBatchBytes"), 4 * 1024 * 1024},
                       {QStringLiteral("maxPickHits"), 64}};
    return QJsonObject{{QStringLiteral("v"), 1},
                       {QStringLiteral("engine"), engine},
                       {QStringLiteral("binding"), binding},
                       {QStringLiteral("patchSet"), QJsonArray::fromStringList(enginePatchSet())},
                       {QStringLiteral("render"), render},
                       {QStringLiteral("scene"), scene},
                       {QStringLiteral("pick"), pick},
                       {QStringLiteral("gestures"), gestures},
                       {QStringLiteral("provider"), provider},
                       {QStringLiteral("offline"), offline},
                       {QStringLiteral("surface"), surface},
                       {QStringLiteral("limits"), limits}};
}

}  // namespace maps::view
