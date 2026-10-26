// SdkCoreClient: CoreClient over the generated typed MapsCore client the
// ui-host hands the backend (modules().maps_core) plus the raw LogosAPI for
// tileProvider modules (section 12.5). Every call is the *AsyncResult form
// (M12.9) and every reply is marshalled to the GUI thread.
#pragma once

#include "view/core_client.h"

class LogosAPI;
class MapsCore;
struct LogosResult;
namespace logos { struct CallError; }

namespace maps::view {

class SdkCoreClient final : public CoreClient {
    Q_OBJECT
public:
    SdkCoreClient(MapsCore& core, LogosAPI* api, QObject* parent = nullptr);

    /// Subscribe to the core events the view acts on. Call once modules()
    /// is live (onContextReady).
    void subscribe();

    void openView(const QString& viewId, const QString& surfaceToken, const QString& capabilitiesJson, CoreCallback cb) override;
    void closeView(const QString& viewId, CoreCallback cb) override;
    void ackRevision(const QString& viewId, quint64 revision, CoreCallback cb) override;
    void reportViewState(const QString& viewId, const QString& stateJson, CoreCallback cb) override;
    void reportViewEvent(const QString& viewId, const QString& topic, const QString& payloadJson, CoreCallback cb) override;
    void getScene(CoreCallback cb) override;
    void getChanges(quint64 sinceRevision, CoreCallback cb) override;
    void getLayerOrder(const QString& viewId, CoreCallback cb) override;
    void getBasemap(const QString& id, CoreCallback cb) override;
    void patchStyle(const QString& id, const QString& styleJson, CoreCallback cb) override;
    void listBasemaps(const QString& filterJson, CoreCallback cb) override;
    void setBasemap(const QString& viewId, const QString& id, CoreCallback cb) override;
    void setGroupVisible(const QString& viewId, const QString& session, const QString& groupId, bool visible, CoreCallback cb) override;
    void setFeatureState(const QString& session, const QString& sourceId, const QString& featureId, const QString& stateJson, CoreCallback cb) override;
    void listProviders(CoreCallback cb) override;
    void getResource(const QString& module, const QString& path, qint64 rangeStart, qint64 rangeEnd,
                     std::function<void(bool, const QByteArray&, const QString&, const QString&)> cb) override;

private:
    // Marshal a reply onto this object's (GUI) thread.
    void deliver(CoreCallback cb, CoreReply reply);
    void emitEvent(const QString& topic, const QString& json);

    MapsCore& m_core;
    LogosAPI* m_api;
    bool m_subscribed = false;
};

CoreReply replyFromLogosResult(const LogosResult& result, const logos::CallError& error);
CoreReply replyFromBool(bool value, const logos::CallError& error);
CoreReply replyFromString(const QString& value, const logos::CallError& error);

}  // namespace maps::view
