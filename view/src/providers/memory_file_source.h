// memory:// provider (docs/architecture.md M10.3): an in-process map of
// bytes. Tests and synthetic overlays; also where bundled fixtures live.
#pragma once

#include "providers/resource_provider.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <map>
#include <mutex>

namespace maps::view {

class MemoryFileSource final : public ResourceProvider {
public:
    QString scheme() const override { return QStringLiteral("memory"); }
    bool canRequest(const mbgl::Resource& resource) const override;
    std::unique_ptr<mbgl::AsyncRequest> request(const mbgl::Resource& resource,
                                                mbgl::FileSource::Callback callback) override;
    bool supportsRangeRequests() const override { return true; }

    /// Store bytes under a full "memory://..." URL (or the part after the
    /// scheme). Template URLs are expanded by the engine before they get here.
    void put(const QString& url, const QByteArray& bytes);
    void remove(const QString& url);
    bool contains(const QString& url) const;
    void clear();
    std::size_t size() const;

    /// Requests seen, for tests (ordered).
    QStringList requested() const;

private:
    static std::string key(const std::string& url);

    mutable std::mutex m_mutex;
    std::map<std::string, std::string> m_entries;
    QStringList m_requested;
};

}  // namespace maps::view
