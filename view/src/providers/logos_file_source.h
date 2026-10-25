// logos:// provider (docs/architecture.md M10.3, M10.4, section 12.5).
//
// logos://<module>/<path> resolves, through the provider table maps_core
// publishes (listProviders), to a module implementing the tileProvider
// contract: getResource(path, rangeStart, rangeEnd) -> bytes. The IPC call is
// made through a ResourceFetcher the owner supplies (SdkCoreClient over
// LogosAPI in the plugin; a fake in tests), so this file stays free of the
// SDK. Range requests are forwarded (rangeEnd = -1 for whole); a timeout is
// reported as Connection with retryAfter so the engine retries (section 12.5).
//
// Prefixes may also be served locally from a directory (bundled glyph packs
// under logos://maps/glyphs/, M9.8).
#pragma once

#include "providers/resource_provider.h"

#include <QJsonArray>
#include <QString>

#include <mutex>
#include <vector>

namespace maps::view {

struct FetchResult {
    bool ok = false;
    QByteArray bytes;
    QString errorCode;     // "timeout" | "notFound" | "unavailable" | other
    QString errorMessage;
};
using FetchResultCallback = std::function<void(const FetchResult&)>;
/// (module, path, rangeStart, rangeEnd, callback). The callback may run on
/// any thread; the provider marshals the response.
using ResourceFetcher = std::function<void(const QString& module, const QString& path,
                                           qint64 rangeStart, qint64 rangeEnd, FetchResultCallback)>;

class LogosFileSource final : public ResourceProvider {
public:
    explicit LogosFileSource(ResourceFetcher fetcher);

    QString scheme() const override { return QStringLiteral("logos"); }
    bool canRequest(const mbgl::Resource& resource) const override;
    std::unique_ptr<mbgl::AsyncRequest> request(const mbgl::Resource& resource,
                                                mbgl::FileSource::Callback callback) override;
    bool supportsRangeRequests() const override { return true; }

    /// Replace the provider table from maps_core.listProviders()
    /// ({"providers":[{module, prefix, ...}]} or the bare array).
    void setProviders(const QJsonArray& providers);
    /// Serve logos://<module>/<prefix>... from a local directory (bundled assets).
    void addLocalPrefix(const QString& module, const QString& prefix, const QString& directory);

    struct Target {
        QString module;
        QString prefix;
        QString localDirectory;  // non-empty = served from disk
    };
    /// Longest-prefix match for "<module>/<path>"; nullopt when unregistered.
    std::optional<Target> resolve(const QString& module, const QString& path) const;

    /// Retry delay reported on timeout (section 12.5), default 5 s.
    void setRetryAfter(mbgl::Seconds s) { m_retryAfter = s; }

private:
    ResourceFetcher m_fetcher;
    mutable std::mutex m_mutex;
    std::vector<Target> m_targets;
    mbgl::Seconds m_retryAfter{5};
};

}  // namespace maps::view
