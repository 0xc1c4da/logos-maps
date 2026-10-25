// The composite file source registered under FileSourceType::Network
// (docs/architecture.md M10.1): wraps the engine's OnlineFileSource and
// consults module-defined providers first, so provider responses pass
// through MainResourceLoader's database cache like HTTP ones.
#pragma once

#include "providers/resource_provider.h"

#include <mbgl/storage/file_source.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace maps::view {

class CompositeFileSource final : public mbgl::FileSource {
public:
    /// Called per request to obtain the provider list (the registry's
    /// snapshot), so providers added after the Map was created are seen.
    using ProviderSource = std::function<std::vector<std::shared_ptr<ResourceProvider>>()>;

    CompositeFileSource(std::unique_ptr<mbgl::FileSource> inner, ProviderSource providers);
    ~CompositeFileSource() override;

    std::unique_ptr<mbgl::AsyncRequest> request(const mbgl::Resource&, Callback) override;
    void forward(const mbgl::Resource&, const mbgl::Response&, std::function<void()>) override;
    bool supportsCacheOnlyRequests() const override;
    bool canRequest(const mbgl::Resource&) const override;
    void pause() override;
    void resume() override;
    void setProperty(const std::string&, const mapbox::base::Value&) override;
    mapbox::base::Value getProperty(const std::string&) const override;
    void setResourceTransform(mbgl::ResourceTransform) override;
    void setResourceOptions(mbgl::ResourceOptions) override;
    mbgl::ResourceOptions getResourceOptions() override;
    void setClientOptions(mbgl::ClientOptions) override;
    mbgl::ClientOptions getClientOptions() override;

    /// Which provider (if any) would take `resource`; nullptr = engine loader.
    std::shared_ptr<ResourceProvider> providerFor(const mbgl::Resource& resource) const;
    mbgl::FileSource* inner() const { return m_inner.get(); }

private:
    std::unique_ptr<mbgl::FileSource> m_inner;
    ProviderSource m_providers;
};

}  // namespace maps::view
