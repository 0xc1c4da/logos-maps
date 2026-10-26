#include "sdk_core_client.h"

#include "logos_sdk.h"

#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_async_result.h"
#include "logos_call_error.h"
#include "logos_types.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QMetaObject>
#include <QPointer>

namespace maps::view {

namespace {

QString errorText(const logos::CallError& e)
{
    return QStringLiteral("maps.transport: %1 (%2)").arg(QString::fromStdString(e.message), QString::fromStdString(e.code));
}

}  // namespace

CoreReply replyFromLogosResult(const LogosResult& result, const logos::CallError& error)
{
    if (!error.ok()) return CoreReply{false, QJsonValue(), errorText(error)};
    return replyFromResult(result.success, QJsonValue::fromVariant(result.value), result.error.toString());
}

CoreReply replyFromBool(bool value, const logos::CallError& error)
{
    if (!error.ok()) return CoreReply{false, QJsonValue(), errorText(error)};
    return replyFromResult(value, QJsonValue(value), value ? QString() : QStringLiteral("maps.rejected: the core returned false"));
}

CoreReply replyFromString(const QString& value, const logos::CallError& error)
{
    if (!error.ok()) return CoreReply{false, QJsonValue(), errorText(error)};
    return replyFromResult(true, jsonFromText(value), QString());
}

SdkCoreClient::SdkCoreClient(MapsCore& core, LogosAPI* api, QObject* parent)
    : CoreClient(parent), m_core(core), m_api(api) {}

void SdkCoreClient::deliver(CoreCallback cb, CoreReply reply)
{
    if (!cb) return;
    QPointer<SdkCoreClient> self(this);
    QMetaObject::invokeMethod(this, [self, cb = std::move(cb), reply = std::move(reply)] {
        if (self) cb(reply);
    }, Qt::QueuedConnection);
}

void SdkCoreClient::emitEvent(const QString& topic, const QString& json)
{
    QPointer<SdkCoreClient> self(this);
    QMetaObject::invokeMethod(this, [self, topic, json] {
        if (!self) return;
        const QJsonValue v = jsonFromText(json);
        emit self->coreEvent(topic, v.toObject());
    }, Qt::QueuedConnection);
}

void SdkCoreClient::subscribe()
{
    if (m_subscribed) return;
    m_subscribed = true;
    auto forward = [this](const char* topic) {
        return [this, topic = QString::fromLatin1(topic)](const QString& json) { emitEvent(topic, json); };
    };
    m_core.onRevisionPublished(forward("revisionPublished"));
    m_core.onBasemapChanged(forward("basemapChanged"));
    m_core.onCameraIntent(forward("cameraIntent"));
    m_core.onViewOptionsChanged(forward("viewOptionsChanged"));
    m_core.onTrackRequested(forward("trackRequested"));
    m_core.onSnapshotRequested(forward("snapshotRequested"));
    m_core.onFeatureStateChanged(forward("featureStateChanged"));
    m_core.onCacheClearRequested(forward("cacheClearRequested"));
    m_core.onCatalogueChanged(forward("catalogueChanged"));
    m_core.onGroupVisibilityChanged(forward("groupVisibilityChanged"));
    m_core.onCalloutContentChanged(forward("calloutContentChanged"));
    m_core.onSubscriptionStatus([this](logos::SubStatus status, std::uint64_t) {
        const bool up = status == logos::SubStatus::Armed;
        QPointer<SdkCoreClient> self(this);
        QMetaObject::invokeMethod(this, [self, up] { if (self) emit self->connectedChanged(up); }, Qt::QueuedConnection);
    });
}

#define MAPS_SDK_CALL(method, conv, ...)                                                             \
    m_core.method##AsyncResult(__VA_ARGS__ __VA_OPT__(, ) [this, cb](auto r) { deliver(cb, conv(r.value, r.error)); })

void SdkCoreClient::openView(const QString& viewId, const QString& surfaceToken, const QString& capabilitiesJson, CoreCallback cb)
{
    MAPS_SDK_CALL(openView, replyFromLogosResult, viewId, surfaceToken, capabilitiesJson);
}

void SdkCoreClient::closeView(const QString& viewId, CoreCallback cb) { MAPS_SDK_CALL(closeView, replyFromBool, viewId); }

void SdkCoreClient::ackRevision(const QString& viewId, quint64 revision, CoreCallback cb)
{
    MAPS_SDK_CALL(ackRevision, replyFromBool, viewId, static_cast<qulonglong>(revision));
}

void SdkCoreClient::reportViewState(const QString& viewId, const QString& stateJson, CoreCallback cb)
{
    MAPS_SDK_CALL(reportViewState, replyFromBool, viewId, stateJson);
}

void SdkCoreClient::reportViewEvent(const QString& viewId, const QString& topic, const QString& payloadJson, CoreCallback cb)
{
    MAPS_SDK_CALL(reportViewEvent, replyFromBool, viewId, topic, payloadJson);
}

void SdkCoreClient::getScene(CoreCallback cb) { MAPS_SDK_CALL(getScene, replyFromLogosResult, std::optional<QString>()); }

void SdkCoreClient::getChanges(quint64 sinceRevision, CoreCallback cb)
{
    MAPS_SDK_CALL(getChanges, replyFromLogosResult, static_cast<qulonglong>(sinceRevision));
}

void SdkCoreClient::getLayerOrder(const QString& viewId, CoreCallback cb) { MAPS_SDK_CALL(getLayerOrder, replyFromLogosResult, viewId); }

void SdkCoreClient::getBasemap(const QString& id, CoreCallback cb) { MAPS_SDK_CALL(getBasemap, replyFromLogosResult, id); }

void SdkCoreClient::patchStyle(const QString& id, const QString& styleJson, CoreCallback cb)
{
    MAPS_SDK_CALL(patchStyle, replyFromLogosResult, id, styleJson);
}

void SdkCoreClient::listBasemaps(const QString& filterJson, CoreCallback cb) { MAPS_SDK_CALL(listBasemaps, replyFromString, filterJson); }

void SdkCoreClient::setBasemap(const QString& viewId, const QString& id, CoreCallback cb)
{
    MAPS_SDK_CALL(setBasemap, replyFromLogosResult, viewId, id);
}

void SdkCoreClient::setGroupVisible(const QString& viewId, const QString& session, const QString& groupId, bool visible, CoreCallback cb)
{
    MAPS_SDK_CALL(setGroupVisible, replyFromBool, viewId, session, groupId, visible);
}

void SdkCoreClient::setFeatureState(const QString& session, const QString& sourceId, const QString& featureId, const QString& stateJson, CoreCallback cb)
{
    MAPS_SDK_CALL(setFeatureState, replyFromBool, session, sourceId, featureId, stateJson);
}

void SdkCoreClient::listProviders(CoreCallback cb) { MAPS_SDK_CALL(listProviders, replyFromString); }

#undef MAPS_SDK_CALL

void SdkCoreClient::getResource(const QString& module, const QString& path, qint64 rangeStart, qint64 rangeEnd,
                                std::function<void(bool, const QByteArray&, const QString&, const QString&)> cb)
{
    if (!m_api) {
        cb(false, {}, QStringLiteral("unavailable"), QStringLiteral("no LogosAPI"));
        return;
    }
    LogosAPIClient* client = m_api->getClient(module);
    if (!client) {
        cb(false, {}, QStringLiteral("unavailable"), QStringLiteral("no client for module ") + module);
        return;
    }
    // tileProvider contract: getResource(path, rangeStart, rangeEnd) -> LogosResult{bytes}
    QVariantList args{path, QVariant::fromValue(static_cast<qlonglong>(rangeStart)), QVariant::fromValue(static_cast<qlonglong>(rangeEnd))};
    client->invokeRemoteMethodAsync(
        module, QStringLiteral("getResource"), args,
        LogosAPIClient::AsyncResultErrorCallback([cb](QVariant result, const logos::CallError& err) {
            if (!err.ok()) {
                const QString code = err.code == "timeout" ? QStringLiteral("timeout") : QStringLiteral("unavailable");
                cb(false, {}, code, QString::fromStdString(err.message));
                return;
            }
            if (result.canConvert<LogosResult>()) {
                const LogosResult lr = result.value<LogosResult>();
                if (!lr.success) {
                    const QString e = lr.error.toString();
                    const QString code = e.contains(QLatin1String("notFound"), Qt::CaseInsensitive) ? QStringLiteral("notFound") : QStringLiteral("error");
                    cb(false, {}, code, e);
                    return;
                }
                result = lr.value;
            }
            QByteArray bytes;
            if (result.typeId() == QMetaType::QByteArray) bytes = result.toByteArray();
            else if (result.typeId() == QMetaType::QString) bytes = QByteArray::fromBase64(result.toString().toLatin1());
            else if (result.typeId() == QMetaType::QVariantMap) {
                const QVariantMap m = result.toMap();
                const QVariant b = m.value(QStringLiteral("bytes"));
                bytes = b.typeId() == QMetaType::QByteArray ? b.toByteArray() : QByteArray::fromBase64(b.toString().toLatin1());
            }
            cb(true, bytes, QString(), QString());
        }));
}

}  // namespace maps::view
