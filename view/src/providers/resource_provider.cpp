#include "providers/resource_provider.h"

#include <mbgl/actor/scheduler.hpp>


namespace maps::view {

void deliverAsync(std::shared_ptr<std::atomic<bool>> alive,
                  mbgl::FileSource::Callback callback,
                  mbgl::Response response)
{
    // Tile requests are issued from the render thread, where the binding
    // installs a QMapLibre::Scheduler (map_renderer.cpp) that is drained on the
    // next render pass; style/sprite requests come from the GUI thread, whose
    // scheduler is the Qt-integrated mbgl::util::RunLoop. Both are reached
    // through the generic Scheduler (RunLoop::Get() casts blindly and would
    // treat the render thread's scheduler as a RunLoop). This is what
    // mbgl::FileSourceRequest does with its Mailbox as well.
    mbgl::Scheduler* scheduler = mbgl::Scheduler::GetCurrent();
    scheduler->schedule(
        [alive = std::move(alive), callback = std::move(callback), response = std::move(response)]() mutable {
            if (alive->load()) callback(std::move(response));
        });
}

mbgl::Response okResponse(std::string bytes, std::optional<mbgl::Timestamp> expires,
                          std::optional<std::string> etag)
{
    mbgl::Response r;
    r.data = std::make_shared<const std::string>(std::move(bytes));
    r.expires = expires ? expires : std::optional<mbgl::Timestamp>(mbgl::util::now() + mbgl::Seconds(3600));
    r.etag = std::move(etag);
    return r;
}

mbgl::Response notFoundResponse(const std::string& message)
{
    mbgl::Response r;
    r.error = std::make_unique<mbgl::Response::Error>(mbgl::Response::Error::Reason::NotFound, message);
    return r;
}

mbgl::Response connectionErrorResponse(const std::string& message, mbgl::Seconds retryAfter)
{
    mbgl::Response r;
    r.error = std::make_unique<mbgl::Response::Error>(mbgl::Response::Error::Reason::Connection, message,
                                                      mbgl::util::now() + retryAfter);
    return r;
}

mbgl::Response otherErrorResponse(const std::string& message)
{
    mbgl::Response r;
    r.error = std::make_unique<mbgl::Response::Error>(mbgl::Response::Error::Reason::Other, message);
    return r;
}

std::pair<std::string, std::string> splitScheme(const std::string& url)
{
    const auto pos = url.find("://");
    if (pos == std::string::npos) return {"", url};
    return {url.substr(0, pos), url.substr(pos + 3)};
}

}  // namespace maps::view
