// StyleLoader: basemap loading and switching (docs/architecture.md M9.4,
// M9.9). The style JSON is fetched through the provider layer -- never by the
// engine directly -- then patched by maps_core (M9.6, cached per fingerprint)
// and handed to Map::setStyleJson. A fetch/patch/parse failure leaves the
// previous style in place and surfaces as basemapFailed.
#pragma once

#include "view/core_client.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace QMapLibre { class Map; }

namespace maps::view {

class StyleLoader : public QObject {
    Q_OBJECT
public:
    StyleLoader(QMapLibre::Map* map, CoreClient* core, QObject* parent = nullptr);

    void setMap(QMapLibre::Map* map) { m_map = map; }
    void setViewId(const QString& viewId) { m_viewId = viewId; }

    QString currentBasemap() const { return m_current; }
    QString loadingBasemap() const { return m_loading; }
    QJsonObject descriptor() const { return m_descriptor; }
    /// The last style JSON handed to the engine (patched).
    QString lastStyleJson() const { return m_lastStyle; }

    /// Attribution composed from the descriptor and the engine's copyrights
    /// (M9.9): plain text entries, HTML from the engine.
    QString attributionHtml() const;
    void setEngineCopyrights(const QString& html);

public slots:
    void load(const QString& basemapId);
    /// Apply a style JSON directly (tests, dev launcher --style).
    void applyStyleJson(const QString& styleJson, const QString& label = QString());

signals:
    void basemapLoading(const QString& id);
    /// setStyleJson was called; the engine's DidFinishLoadingMap follows.
    void styleApplied(const QString& id);
    void basemapFailed(const QString& id, const QString& reason);
    void descriptorChanged(const QJsonObject& descriptor);
    void attributionChanged(const QString& html);

private:
    void fetchAndPatch(const QString& id, const QString& styleUrl);
    void patchAndApply(const QString& id, const QString& styleJson);
    void fail(const QString& id, const QString& reason);

    QMapLibre::Map* m_map;
    CoreClient* m_core;
    QString m_viewId;
    QString m_current;
    QString m_loading;
    QJsonObject m_descriptor;
    QString m_lastStyle;
    QString m_engineCopyrights;
    int m_generation = 0;  // ignores replies from superseded loads
};

}  // namespace maps::view
