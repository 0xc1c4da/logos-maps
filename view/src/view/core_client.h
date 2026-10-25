// The view's window onto maps_core (docs/architecture.md section 12.1
// view-side protocol). Everything is asynchronous and completes on the GUI
// thread; replies carry the decoded JSON value or a "<code>: <message>" error.
//
// Two implementations: SdkCoreClient (src/sdk_core_client.*) over the
// generated modules().maps_core client inside ui-host, and LogosctlCoreClient
// (devtools/) over `logosctl call` / `logosctl watch` for the dev launcher.
// Tests use FakeCoreClient. The view never calls the core synchronously
// (M12.9).
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>

namespace maps::view {

struct CoreReply {
    bool ok = false;
    QJsonValue value;   // decoded StdLogosResult.value, or the method's plain return
    QString error;      // "<code>: <message>" (M11.2) when !ok
    QString code() const { return error.section(QLatin1Char(':'), 0, 0).trimmed(); }
    QJsonObject object() const { return value.toObject(); }
};
using CoreCallback = std::function<void(const CoreReply&)>;

class CoreClient : public QObject {
    Q_OBJECT
public:
    explicit CoreClient(QObject* parent = nullptr) : QObject(parent) {}

    // view-side protocol
    virtual void openView(const QString& viewId, const QString& surfaceToken, const QString& capabilitiesJson, CoreCallback cb) = 0;
    virtual void closeView(const QString& viewId, CoreCallback cb) = 0;
    virtual void ackRevision(const QString& viewId, quint64 revision, CoreCallback cb) = 0;
    virtual void reportViewState(const QString& viewId, const QString& stateJson, CoreCallback cb) = 0;
    virtual void reportViewEvent(const QString& viewId, const QString& topic, const QString& payloadJson, CoreCallback cb) = 0;

    // scene
    virtual void getScene(CoreCallback cb) = 0;
    virtual void getChanges(quint64 sinceRevision, CoreCallback cb) = 0;
    virtual void getLayerOrder(const QString& viewId, CoreCallback cb) = 0;

    // basemaps
    virtual void getBasemap(const QString& id, CoreCallback cb) = 0;
    virtual void patchStyle(const QString& id, const QString& styleJson, CoreCallback cb) = 0;
    virtual void listBasemaps(const QString& filterJson, CoreCallback cb) = 0;
    virtual void setBasemap(const QString& viewId, const QString& id, CoreCallback cb) = 0;

    // providers
    virtual void listProviders(CoreCallback cb) = 0;
    /// tileProvider contract on another module (section 12.5). Only the SDK
    /// client can reach arbitrary modules; others answer "unavailable".
    virtual void getResource(const QString& module, const QString& path, qint64 rangeStart, qint64 rangeEnd,
                             std::function<void(bool ok, const QByteArray& bytes, const QString& errorCode, const QString& message)> cb) = 0;

    /// Core-originated events the view acts on (section 12.4 / impl header):
    /// revisionPublished, basemapChanged, cameraIntent, viewOptionsChanged,
    /// trackRequested, snapshotRequested, featureStateChanged,
    /// cacheClearRequested, catalogueChanged. Subscribed by the implementation.
signals:
    void coreEvent(const QString& topic, const QJsonObject& payload);
    /// The implementation lost or regained its link to the core.
    void connectedChanged(bool connected);
};

/// Decode a StdLogosResult-shaped QVariant/JSON ({success, value, error}) or a
/// LogosResult-like pair into a CoreReply. `value` may arrive as JSON text.
CoreReply replyFromResult(bool success, const QJsonValue& value, const QString& error);
QJsonValue jsonFromText(const QString& text);

}  // namespace maps::view
