// Picker: rendered-feature queries at a rectangle (docs/architecture.md
// M8.12-M8.14) through engine patch P1.
//
// A pick is a 24 dp box around the tap. The engine's query returns features
// with source/sourceLayer but no style-layer id, so the picker first queries
// the box unfiltered, then resolves each hit's style layer among the
// candidates drawing that source-layer (one extra query per ambiguous
// candidate). Hits are sorted by distance to the tap, then the session
// owner's priority list is applied (stable). Queries before the style has
// loaded fail with maps.styleNotLoaded rather than being dropped.
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace QMapLibre { class Map; }

namespace maps::view {

class Picker : public QObject {
    Q_OBJECT
public:
    explicit Picker(QMapLibre::Map* map, QObject* parent = nullptr);

    void setMap(QMapLibre::Map* map);
    /// Style (re)loaded: the layer table is rebuilt lazily on the next pick.
    void setStyleLoaded(bool loaded);
    struct SceneLayer {
        QString id, source, sourceLayer, type;
    };
    /// Runtime (scene + built-in) layers, which the engine's style JSON --
    /// the document as loaded -- does not list. Replaces the previous set.
    void setSceneLayers(std::vector<SceneLayer> layers);
    bool styleLoaded() const { return m_styleLoaded; }

    /// M8.12: layer ids listed first win ties; others keep distance order.
    void setPriority(const QStringList& layerIds) { m_priority = layerIds; }
    QStringList priority() const { return m_priority; }
    void setBoxDp(double dp) { m_boxDp = dp; }
    double boxDp() const { return m_boxDp; }
    void setPropertiesByteCap(int bytes) { m_propertyCap = bytes; }
    void setMaxHits(int n) { m_maxHits = n; }
    /// M8.15 overlay fallback: hits from this source are the view's own
    /// highlight mirror and are dropped; `resolver` supplies the feature
    /// state the engine cannot hold so both paths report the same payload.
    void setStateFallback(const QString& overlaySourceId, std::function<QJsonObject(const QString& sourceId, const QString& featureId)> resolver)
    {
        m_overlaySource = overlaySourceId;
        m_stateResolver = std::move(resolver);
    }

    /// Whether P1 is compiled in (capability pick.rendered).
    static bool renderedQueriesAvailable();

    /// Asynchronous: emits picked() or failed() from the event loop.
    void pick(const QString& kind, const QPointF& px);
    /// Synchronous form (tests). Returns the payload, or nullopt with `code` set.
    std::optional<QJsonObject> pickNow(const QString& kind, const QPointF& px, QString* code = nullptr, QString* message = nullptr);

signals:
    /// {"v":1,"kind","lonLat":[lon,lat],"px":[x,y],"hits":[{session,layerId,sourceId,sourceLayer,featureId,properties,state,distancePx}]}
    void picked(const QJsonObject& payload);
    void failed(const QString& code, const QString& message);

private:
    struct StyleLayer {
        QString id, source, sourceLayer, type;
        bool scene = false;  // runtime layer (session content) vs basemap style
    };
    void ensureLayerTable();
    QJsonArray hitsFor(const QPointF& px);
    double distanceTo(const QJsonObject& geometry, const QPointF& px) const;
    QJsonValue cappedProperties(const QJsonValue& props) const;

    QMapLibre::Map* m_map;
    bool m_styleLoaded = false;
    bool m_tableDirty = true;
    std::vector<StyleLayer> m_layers;  // style order, bottom to top
    std::vector<SceneLayer> m_sceneLayers;
    QStringList m_priority;
    double m_boxDp = 24.0;
    int m_propertyCap = 4096;
    int m_maxHits = 32;
    QString m_overlaySource;
    std::function<QJsonObject(const QString&, const QString&)> m_stateResolver;
};

}  // namespace maps::view
