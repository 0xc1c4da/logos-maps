#include "view/map_change.h"

namespace maps::view {

using MC = QMapLibre::Map::MapChange;

MapChangeMapping mapChangeMapping(MC change)
{
    switch (change) {
    case MC::MapChangeRegionWillChange:
    case MC::MapChangeRegionWillChangeAnimated:
        return {PublicEvent::Internal, nullptr, false, false};
    case MC::MapChangeRegionIsChanging:
        return {PublicEvent::CameraChanged, "cameraChanged", false, false};
    case MC::MapChangeRegionDidChange:
    case MC::MapChangeRegionDidChangeAnimated:
        return {PublicEvent::CameraIdle, "cameraIdle", false, false};
    case MC::MapChangeWillStartLoadingMap:
        return {PublicEvent::BasemapLoading, "basemapLoading", false, true};
    case MC::MapChangeDidFinishLoadingMap:
        return {PublicEvent::BasemapLoaded, "basemapLoaded", false, false};
    case MC::MapChangeDidFailLoadingMap:
        return {PublicEvent::BasemapFailed, "basemapFailed", false, false};
    case MC::MapChangeWillStartRenderingFrame:
    case MC::MapChangeDidFinishRenderingFrame:
    case MC::MapChangeWillStartRenderingMap:
    case MC::MapChangeDidFinishRenderingMap:
    case MC::MapChangeDidFinishRenderingMapFullyRendered:
        return {PublicEvent::Internal, nullptr, false, false};
    case MC::MapChangeDidFinishRenderingFrameFullyRendered:
        return {PublicEvent::FrameRendered, "frameRendered", false, false};
    case MC::MapChangeDidFinishLoadingStyle:
        return {PublicEvent::StyleReady, "styleReady", true, false};
    case MC::MapChangeSourceDidChange:
        return {PublicEvent::Ignored, nullptr, false, false};
    }
    return {PublicEvent::None, nullptr, false, false};
}

QString mapChangeName(MC change)
{
    static const char* names[] = {
        "RegionWillChange", "RegionWillChangeAnimated", "RegionIsChanging", "RegionDidChange",
        "RegionDidChangeAnimated", "WillStartLoadingMap", "DidFinishLoadingMap", "DidFailLoadingMap",
        "WillStartRenderingFrame", "DidFinishRenderingFrame", "DidFinishRenderingFrameFullyRendered",
        "WillStartRenderingMap", "DidFinishRenderingMap", "DidFinishRenderingMapFullyRendered",
        "DidFinishLoadingStyle", "SourceDidChange"};
    const int i = static_cast<int>(change);
    if (i < 0 || i >= kMapChangeCount) return QStringLiteral("Unknown(%1)").arg(i);
    return QString::fromLatin1(names[i]);
}

}  // namespace maps::view
