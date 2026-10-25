// FakeCoreClient: an in-process stand-in for maps_core with the view-side
// protocol the ViewController needs. Holds a scene (sessions -> sources /
// images / layers) and a change log, answers getLayerOrder in publication
// order with no anchors, serves one raster basemap whose style the test
// provides, and records every ack / reported event / view state.
#pragma once

#include "view/core_client.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>

#include <map>
#include <vector>

namespace maps::test {

class FakeCoreClient final : public maps::view::CoreClient {
    Q_OBJECT
public:
    explicit FakeCoreClient(QObject* parent = nullptr);

    // ── test-side controls ──────────────────────────────────────────────
    /// Register a basemap: `style` is handed back by getBasemap (raster
    /// path, M9.4); when empty the descriptor carries `style_url`.
    void addBasemap(const QString& id, const QJsonObject& style, const QString& styleUrl = QString());
    void setDefaultBasemap(const QString& id) { m_defaultBasemap = id; }
    /// Append ops to the log as one revision and emit revisionPublished.
    quint64 publish(const QJsonArray& ops, bool emitEvent = true);
    /// Emit any core event.
    void emitCoreEvent(const QString& topic, QJsonObject payload);
    /// Reply latency in ms (0 = next event-loop turn).
    void setLatency(int ms) { m_latency = ms; }
    /// Fail the next call of `method` with this error.
    void failNext(const QString& method, const QString& error) { m_failNext[method] = error; }

    QJsonObject scene() const;
    quint64 revision() const { return m_revision; }
    QStringList layerOrder() const { return m_layerOrder; }
    void setLayerOrder(const QStringList& order) { m_layerOrder = order; }

    // ── recorded ───────────────────────────────────────────────────────
    struct OpenedView { QString viewId, surfaceToken; QJsonObject capabilities; };
    std::vector<OpenedView> opened;
    QStringList closed;
    std::vector<std::pair<QString, quint64>> acks;
    std::vector<std::pair<QString, QJsonObject>> events;  // topic, payload
    std::vector<QJsonObject> viewStates;
    QStringList calls;  // every method name in call order
    std::vector<std::pair<QString, QString>> patchedStyles;  // id, style text

    bool hasEvent(const QString& topic) const;
    QJsonObject lastEvent(const QString& topic) const;
    int eventCount(const QString& topic) const;
    bool acked(quint64 revision) const;

    // ── CoreClient ─────────────────────────────────────────────────────
    void openView(const QString& viewId, const QString& surfaceToken, const QString& capabilitiesJson, maps::view::CoreCallback cb) override;
    void closeView(const QString& viewId, maps::view::CoreCallback cb) override;
    void ackRevision(const QString& viewId, quint64 revision, maps::view::CoreCallback cb) override;
    void reportViewState(const QString& viewId, const QString& stateJson, maps::view::CoreCallback cb) override;
    void reportViewEvent(const QString& viewId, const QString& topic, const QString& payloadJson, maps::view::CoreCallback cb) override;
    void getScene(maps::view::CoreCallback cb) override;
    void getChanges(quint64 sinceRevision, maps::view::CoreCallback cb) override;
    void getLayerOrder(const QString& viewId, maps::view::CoreCallback cb) override;
    void getBasemap(const QString& id, maps::view::CoreCallback cb) override;
    void patchStyle(const QString& id, const QString& styleJson, maps::view::CoreCallback cb) override;
    void listBasemaps(const QString& filterJson, maps::view::CoreCallback cb) override;
    void setBasemap(const QString& viewId, const QString& id, maps::view::CoreCallback cb) override;
    void listProviders(maps::view::CoreCallback cb) override;
    void getResource(const QString& module, const QString& path, qint64 rangeStart, qint64 rangeEnd,
                     std::function<void(bool, const QByteArray&, const QString&, const QString&)> cb) override;

    /// Resources served by getResource: "<module>/<path>" -> bytes.
    std::map<QString, QByteArray> resources;
    QJsonArray providers;  // listProviders payload

private:
    void reply(const QString& method, maps::view::CoreCallback cb, bool ok, QJsonValue value, const QString& error = QString());
    void applyOpToScene(const QJsonObject& op);
    static QString sessionOf(const QString& globalId);

    struct Session {
        QJsonObject sources;
        QJsonObject images;
        QJsonArray layers;
    };
    std::map<QString, Session> m_sessions;
    std::vector<std::pair<quint64, QJsonArray>> m_log;  // revision, ops
    quint64 m_revision = 0;
    QStringList m_layerOrder;
    std::map<QString, QJsonObject> m_basemaps;  // id -> {descriptor, style?}
    QString m_defaultBasemap;
    std::map<QString, QString> m_viewBasemap;
    std::map<QString, QString> m_failNext;
    int m_latency = 0;
};

}  // namespace maps::test
