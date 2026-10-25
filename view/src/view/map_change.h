// Engine MapChange -> public event mapping (docs/architecture.md M11.3).
#pragma once

#include <QMapLibre/Map>

#include <QString>

namespace maps::view {

enum class PublicEvent {
    None,            // not classified (only for codes this build does not know)
    Internal,        // classified as engine bookkeeping, deliberately no public event
    BasemapLoading,  // WillStartLoadingMap
    BasemapLoaded,   // DidFinishLoadingMap
    BasemapFailed,   // DidFailLoadingMap
    StyleReady,      // DidFinishLoadingStyle (after the replay it triggers)
    CameraChanged,   // RegionIsChanging
    CameraIdle,      // RegionDidChange / RegionDidChangeAnimated
    FrameRendered,   // DidFinishRenderingFrameFullyRendered
    Ignored,         // SourceDidChange (scene owns truth)
};

struct MapChangeMapping {
    PublicEvent event;
    const char* topic;        // maps.<topic>, or nullptr
    bool triggersReplay;      // M6.8
    bool opensBlockingWindow; // M6.8: WillStartLoadingMap
};

MapChangeMapping mapChangeMapping(QMapLibre::Map::MapChange change);
QString mapChangeName(QMapLibre::Map::MapChange change);
constexpr int kMapChangeCount = static_cast<int>(QMapLibre::Map::MapChangeSourceDidChange) + 1;

}  // namespace maps::view
