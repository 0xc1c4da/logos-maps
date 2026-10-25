#include "providers/composite_file_source.h"

#include "providers/provider_registry.h"

#include <mbgl/util/chrono.hpp>
#include <mbgl/util/timer.hpp>

namespace maps::view {

CompositeFileSource::CompositeFileSource(std::unique_ptr<mbgl::FileSource> inner, ProviderSource providers)
    : m_inner(std::move(inner)), m_providers(std::move(providers)) {}

CompositeFileSource::~CompositeFileSource() = default;

std::shared_ptr<ResourceProvider> CompositeFileSource::providerFor(const mbgl::Resource& resource) const
{
    for (const auto& p : m_providers()) {
        if (p && p->canRequest(resource)) return p;
    }
    return nullptr;
}

namespace {

// A provider request held back until the cached copy expires. The Network
// slot's contract (OnlineFileRequest::activate): when MainResourceLoader
// has just served a fresh response from the database it still issues a
// revalidation with `priorExpires` set, and the network source is expected
// to sit on it until then (cancelled when the tile is dropped). Requests
// reach the Network slot on the loader's RunLoop thread, so util::Timer is
// available.
class DeferredProviderRequest final : public mbgl::AsyncRequest {
public:
    mbgl::util::Timer timer;
    std::unique_ptr<mbgl::AsyncRequest> inner;
};

}  // namespace

std::unique_ptr<mbgl::AsyncRequest> CompositeFileSource::request(const mbgl::Resource& resource, Callback callback)
{
    // Providers first (M10.1), then the engine's own order: asset, mbtiles,
    // pmtiles, file, database, network (main_resource_loader.cpp:69-117).
    if (auto p = providerFor(resource)) {
        if (p->scheme() != QLatin1String("logos")) {  // logos:// logs itself with the resolved module
            ProviderRegistry::instance().logRequest(p->scheme(), QString::fromStdString(splitScheme(resource.url).second),
                                                    resource.dataRange ? static_cast<qint64>(resource.dataRange->first) : 0,
                                                    resource.dataRange ? static_cast<qint64>(resource.dataRange->second) : -1);
        }
        const mbgl::Timestamp now = mbgl::util::now();
        if (resource.priorExpires && *resource.priorExpires > now) {
            auto deferred = std::make_unique<DeferredProviderRequest>();
            DeferredProviderRequest* raw = deferred.get();
            deferred->timer.start(*resource.priorExpires - now, mbgl::Duration::zero(),
                                  [raw, p, resource, callback = std::move(callback)]() mutable {
                                      raw->inner = p->request(resource, std::move(callback));
                                  });
            return deferred;
        }
        return p->request(resource, std::move(callback));
    }
    if (m_inner) return m_inner->request(resource, std::move(callback));
    auto req = std::make_unique<ProviderRequest>();
    deliverAsync(req->token(), std::move(callback), otherErrorResponse("no resource loader"));
    return req;
}

void CompositeFileSource::forward(const mbgl::Resource& resource, const mbgl::Response& response, std::function<void()> cb)
{
    if (m_inner) m_inner->forward(resource, response, std::move(cb));
    else if (cb) cb();
}

bool CompositeFileSource::supportsCacheOnlyRequests() const
{
    return m_inner ? m_inner->supportsCacheOnlyRequests() : false;
}

bool CompositeFileSource::canRequest(const mbgl::Resource& resource) const
{
    // Same contract as OnlineFileSource (the slot this wraps): CacheOnly
    // requests are the database's business, never a provider's.
    if (!resource.hasLoadingMethod(mbgl::Resource::LoadingMethod::Network)) return false;
    if (providerFor(resource)) return true;
    return m_inner ? m_inner->canRequest(resource) : false;
}

void CompositeFileSource::pause()
{
    for (const auto& p : m_providers()) if (p) p->pause();
    if (m_inner) m_inner->pause();
}

void CompositeFileSource::resume()
{
    for (const auto& p : m_providers()) if (p) p->resume();
    if (m_inner) m_inner->resume();
}

void CompositeFileSource::setProperty(const std::string& key, const mapbox::base::Value& value)
{
    for (const auto& p : m_providers()) if (p) p->setProperty(key, value);
    if (m_inner) m_inner->setProperty(key, value);
}

mapbox::base::Value CompositeFileSource::getProperty(const std::string& key) const
{
    return m_inner ? m_inner->getProperty(key) : mapbox::base::Value{};
}

void CompositeFileSource::setResourceTransform(mbgl::ResourceTransform transform)
{
    if (m_inner) m_inner->setResourceTransform(std::move(transform));
}

void CompositeFileSource::setResourceOptions(mbgl::ResourceOptions options)
{
    if (m_inner) m_inner->setResourceOptions(std::move(options));
}

mbgl::ResourceOptions CompositeFileSource::getResourceOptions()
{
    return m_inner ? m_inner->getResourceOptions() : mbgl::ResourceOptions();
}

void CompositeFileSource::setClientOptions(mbgl::ClientOptions options)
{
    if (m_inner) m_inner->setClientOptions(std::move(options));
}

mbgl::ClientOptions CompositeFileSource::getClientOptions()
{
    return m_inner ? m_inner->getClientOptions() : mbgl::ClientOptions();
}

}  // namespace maps::view
