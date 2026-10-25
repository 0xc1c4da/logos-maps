// SelectionOverlay: the M8.15 fallback for engines without native feature
// state (P2). Selection and hover arrive as featureStateChanged events
// (sourceId, featureId, state); when the renderer cannot hold per-feature
// state the view mirrors the affected features into a GeoJSON source in the
// reserved session maps/builtin and draws them with highlight layers.
//
// The mirror is invisible to picking: Picker drops hits from this source
// and asks stateFor() so a pick reports the same {featureId, state} on both
// paths (spec §16.5). State merging follows mbgl: a set merges keys, a null
// state removes the feature's entry.
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QStringList>

#include <map>
#include <utility>

namespace QMapLibre { class Map; }

namespace maps::view {

class SelectionOverlay : public QObject {
    Q_OBJECT
public:
    static constexpr const char* kSession = "maps/builtin";

    explicit SelectionOverlay(QMapLibre::Map* map, QObject* parent = nullptr);

    void setMap(QMapLibre::Map* map);

    /// featureStateChanged for an engine without P2. `state` null removes.
    void apply(const QString& sourceId, const QString& featureId, const QJsonValue& state);
    /// Current merged state for a feature, or an empty object.
    QJsonObject stateFor(const QString& sourceId, const QString& featureId) const;
    bool isOverlaySource(const QString& sourceId) const { return sourceId == this->sourceId(); }

    /// DidFinishLoadingStyle: re-add source and layers, re-resolve geometry.
    void onStyleLoaded();
    /// Scene layers were re-inserted: raise the overlay back to the top.
    void onSceneLayersReconciled();
    /// A source changed (scene op) -- geometry may need re-resolving.
    void onSourceUpdated(const QString& sourceId);

    QString sourceId() const;
    QStringList layerIds() const;
    bool installed() const { return m_installed; }
    int entryCount() const { return static_cast<int>(m_entries.size()); }

private:
    struct Entry {
        QJsonObject state;
        QJsonObject feature;  // the source feature (geometry + properties), once resolved
        bool resolved = false;
    };
    using Key = std::pair<QString, QString>;

    void install();
    void uninstall();
    void resolve(const QString& sourceId, Entry& e, const QString& featureId);
    void updateSource();
    QJsonObject featureCollection() const;

    QMapLibre::Map* m_map;
    std::map<Key, Entry> m_entries;
    bool m_installed = false;
    bool m_styleReady = false;
};

}  // namespace maps::view
