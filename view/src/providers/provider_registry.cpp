#include "providers/provider_registry.h"

#include "providers/composite_file_source.h"

#include <mbgl/storage/file_source_manager.hpp>

#include <QCoreApplication>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <set>

namespace maps::view {

ProviderRegistry& ProviderRegistry::instance()
{
    static ProviderRegistry* registry = new ProviderRegistry();  // process lifetime
    return *registry;
}

ProviderRegistry::ProviderRegistry() = default;

void ProviderRegistry::add(std::shared_ptr<ResourceProvider> provider)
{
    if (!provider) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const QString scheme = provider->scheme();
    std::erase_if(m_providers, [&](const auto& p) { return p->scheme() == scheme; });
    m_providers.push_back(std::move(provider));
}

void ProviderRegistry::remove(const QString& scheme)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::erase_if(m_providers, [&](const auto& p) { return p->scheme() == scheme; });
}

void ProviderRegistry::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_providers.clear();
}

std::vector<std::shared_ptr<ResourceProvider>> ProviderRegistry::providers() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_providers;
}

std::shared_ptr<ResourceProvider> ProviderRegistry::find(const QString& scheme) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& p : m_providers) if (p->scheme() == scheme) return p;
    return nullptr;
}

QStringList ProviderRegistry::schemes() const
{
    // Engine built-ins (section 10.1 dispatch) plus registered providers.
    std::set<QString> all = {QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("file"),
                             QStringLiteral("asset"), QStringLiteral("mbtiles"), QStringLiteral("pmtiles")};
    for (const auto& p : providers()) all.insert(p->scheme());
    QStringList out;
    for (const auto& s : all) out << s;
    return out;
}

bool ProviderRegistry::rangeRequests() const
{
    // The engine's http and pmtiles paths do byte ranges; a logos provider
    // must (M10.4). False only if some registered provider cannot.
    for (const auto& p : providers()) if (!p->supportsRangeRequests()) return false;
    return true;
}

QMapLibre::Settings::FileSourceFactoryHook ProviderRegistry::hook()
{
    return [this](mbgl::FileSourceManager& manager) {
        if (m_hookInstalled) return;
        m_hookInstalled = true;
        // The composite sits in the Network slot, wrapping the engine's
        // OnlineFileSource, rather than replacing the ResourceLoader: the
        // default MainResourceLoader keeps running in front of it, so
        // provider responses get the same database cache, expiry check and
        // prior-etag/modified revalidation as HTTP ones
        // (main_resource_loader.cpp: database -> requestFromNetwork ->
        // databaseFileSource->forward). asset/file/mbtiles/pmtiles are
        // still answered by the loader before the Network slot is reached.
        auto inner = manager.unRegisterFileSourceFactory(mbgl::FileSourceType::Network);
        manager.registerFileSourceFactory(
            mbgl::FileSourceType::Network,
            [this, inner = std::move(inner)](const mbgl::ResourceOptions& ro, const mbgl::ClientOptions& co)
                -> std::unique_ptr<mbgl::FileSource> {
                std::unique_ptr<mbgl::FileSource> online = inner ? inner(ro, co) : nullptr;
                return std::make_unique<CompositeFileSource>(std::move(online), [this] { return providers(); });
            });
    };
}

QNetworkAccessManager* ProviderRegistry::network()
{
    if (!m_network) m_network = new QNetworkAccessManager(this);
    return m_network;
}

void ProviderRegistry::fetch(const QUrl& url, FetchCallback callback)
{
    const QString scheme = url.scheme();
    if (auto p = find(scheme)) {
        mbgl::Resource resource(mbgl::Resource::Kind::Style, url.toString().toStdString());
        if (p->canRequest(resource)) {
            // Keep the request alive until the callback fires (the engine
            // contract: destroying it cancels).
            auto holder = std::make_shared<std::unique_ptr<mbgl::AsyncRequest>>();
            *holder = p->request(resource, [holder, callback](mbgl::Response response) {
                if (response.error) {
                    callback(false, QByteArray(), QString::fromStdString(response.error->message));
                } else {
                    const QByteArray bytes = response.data ? QByteArray::fromStdString(*response.data) : QByteArray();
                    callback(true, bytes, QString());
                }
                holder->reset();
            });
            return;
        }
    }
    if (scheme == QLatin1String("file") || scheme.isEmpty()) {
        const QString path = scheme.isEmpty() ? url.toString() : url.toLocalFile();
        QFile file(path);
        const bool ok = file.open(QIODevice::ReadOnly);
        const QByteArray bytes = ok ? file.readAll() : QByteArray();
        const QString error = ok ? QString() : file.errorString();
        QTimer::singleShot(0, this, [callback, ok, bytes, error] { callback(ok, bytes, error); });
        return;
    }
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = network()->get(request);
        connect(reply, &QNetworkReply::finished, this, [reply, callback] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                callback(false, QByteArray(), reply->errorString());
            } else {
                callback(true, reply->readAll(), QString());
            }
        });
        return;
    }
    QTimer::singleShot(0, this, [callback, scheme] {
        callback(false, QByteArray(), QStringLiteral("maps.providerError(%1): no provider for scheme").arg(scheme));
    });
}

void ProviderRegistry::logRequest(const QString& scheme, const QString& path, qint64 rangeStart, qint64 rangeEnd)
{
    emit requestLogged(scheme, path, rangeStart, rangeEnd);
}

}  // namespace maps::view
