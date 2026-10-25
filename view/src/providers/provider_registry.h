// Process-wide provider registry and the engine hook that installs the
// composite loader (docs/architecture.md section 10.2, M10.1, M10.3).
//
// mbgl::FileSourceManager is a process singleton, so providers are too: every
// QMapLibre::Map in this process (one per Logos.Surface) shares them. The
// registry is thread-safe; providers are consulted on the engine's request
// thread.
#pragma once

#include "providers/resource_provider.h"

#include <QMapLibre/Settings>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class QNetworkAccessManager;

namespace maps::view {

class ProviderRegistry : public QObject {
    Q_OBJECT
public:
    static ProviderRegistry& instance();

    void add(std::shared_ptr<ResourceProvider> provider);
    void remove(const QString& scheme);
    void clear();
    std::vector<std::shared_ptr<ResourceProvider>> providers() const;
    std::shared_ptr<ResourceProvider> find(const QString& scheme) const;
    /// Schemes the view can serve (M11.1 provider.schemes[]): engine built-ins
    /// plus every registered provider, sorted and unique.
    QStringList schemes() const;
    bool rangeRequests() const;

    /// The Settings hook (section 14.1 P5) that wraps the engine's default
    /// Network file source in a CompositeFileSource consulting this registry. Idempotent
    /// per process: the factory is replaced once, later calls are no-ops.
    QMapLibre::Settings::FileSourceFactoryHook hook();
    bool hookInstalled() const { return m_hookInstalled; }

    /// Fetch a whole resource outside the engine (M9.4: the style JSON is
    /// fetched through the provider layer, never by the engine). Providers
    /// first; http(s) and file:// through Qt. Callback on the calling thread.
    using FetchCallback = std::function<void(bool ok, const QByteArray& bytes, const QString& error)>;
    void fetch(const QUrl& url, FetchCallback callback);

    /// Record a request for maps.providerRequest metrics (M12.4).
    void logRequest(const QString& scheme, const QString& path, qint64 rangeStart, qint64 rangeEnd);

signals:
    void requestLogged(const QString& scheme, const QString& path, qint64 rangeStart, qint64 rangeEnd);

private:
    ProviderRegistry();
    QNetworkAccessManager* network();

    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<ResourceProvider>> m_providers;
    bool m_hookInstalled = false;
    QNetworkAccessManager* m_network = nullptr;
};

}  // namespace maps::view
