// The ResourceProvider seam (docs/architecture.md section 10.2, M10.2).
//
// A provider answers mbgl::Resource requests for the schemes it owns. It is
// consulted by CompositeFileSource before the engine's own loader. This is
// the only place in maps_view (besides the patch set) that includes mbgl
// headers (M5.1).
#pragma once

#include <mbgl/storage/file_source.hpp>
#include <mbgl/storage/resource.hpp>
#include <mbgl/storage/response.hpp>
#include <mbgl/util/async_request.hpp>

#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace maps::view {

struct ResourceProvider {
    virtual ~ResourceProvider() = default;

    /// Scheme this provider serves ("memory", "logos"), for provider.schemes[].
    virtual QString scheme() const = 0;
    /// Whether this provider answers for `resource` (scheme or host prefix).
    virtual bool canRequest(const mbgl::Resource& resource) const = 0;
    /// Issue the request. The callback MUST be called asynchronously on the
    /// requesting thread, and MUST NOT be called once the returned request
    /// has been destroyed (mbgl::FileSource contract).
    virtual std::unique_ptr<mbgl::AsyncRequest> request(const mbgl::Resource& resource,
                                                        mbgl::FileSource::Callback callback) = 0;
    /// Whether range requests (Resource::dataRange) are honoured (M10.4).
    virtual bool supportsRangeRequests() const { return false; }

    virtual void pause() {}
    virtual void resume() {}
    virtual void setProperty(const std::string&, const mbgl::Value&) {}
};

/// Shared cancellation token for a provider request: the request object the
/// engine holds owns it; a late callback checks `alive` before delivering.
class ProviderRequest final : public mbgl::AsyncRequest {
public:
    ProviderRequest() : m_alive(std::make_shared<std::atomic<bool>>(true)) {}
    ~ProviderRequest() override { m_alive->store(false); }
    std::shared_ptr<std::atomic<bool>> token() const { return m_alive; }

private:
    std::shared_ptr<std::atomic<bool>> m_alive;
};

/// Deliver `response` to `callback` asynchronously through the current
/// thread's mbgl::Scheduler (the GUI RunLoop or the render thread's
/// QMapLibre::Scheduler). Skipped if the request was cancelled meanwhile.
void deliverAsync(std::shared_ptr<std::atomic<bool>> alive,
                  mbgl::FileSource::Callback callback,
                  mbgl::Response response);

/// Response helpers shared by providers (M10.2: fill expires so the cache
/// validates; NotFound, not Other, for missing tiles).
mbgl::Response okResponse(std::string bytes, std::optional<mbgl::Timestamp> expires = std::nullopt,
                          std::optional<std::string> etag = std::nullopt);
mbgl::Response notFoundResponse(const std::string& message);
mbgl::Response connectionErrorResponse(const std::string& message, mbgl::Seconds retryAfter);
mbgl::Response otherErrorResponse(const std::string& message);

/// Splits "scheme://rest" -> (scheme, rest); scheme is "" when absent.
std::pair<std::string, std::string> splitScheme(const std::string& url);

}  // namespace maps::view
