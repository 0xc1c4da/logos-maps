#include "providers/memory_file_source.h"

#include <QStringList>

namespace maps::view {

std::string MemoryFileSource::key(const std::string& url)
{
    auto [scheme, rest] = splitScheme(url);
    return rest;
}

bool MemoryFileSource::canRequest(const mbgl::Resource& resource) const
{
    return splitScheme(resource.url).first == "memory";
}

std::unique_ptr<mbgl::AsyncRequest> MemoryFileSource::request(const mbgl::Resource& resource,
                                                              mbgl::FileSource::Callback callback)
{
    auto req = std::make_unique<ProviderRequest>();
    mbgl::Response response;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requested << QString::fromStdString(resource.url);
        auto it = m_entries.find(key(resource.url));
        if (it == m_entries.end()) {
            response = notFoundResponse("memory: no entry for " + resource.url);
        } else if (resource.dataRange) {
            const auto& [start, end] = *resource.dataRange;  // inclusive, per mbgl
            const std::string& all = it->second;
            if (start >= all.size()) {
                response = notFoundResponse("memory: range start beyond end");
            } else {
                const auto last = std::min<std::uint64_t>(end, all.size() - 1);
                response = okResponse(all.substr(start, last - start + 1));
            }
        } else {
            response = okResponse(it->second);
        }
    }
    // Far-future expiry: memory entries are immutable until replaced.
    if (!response.error) response.expires = mbgl::util::now() + mbgl::Seconds(365 * 24 * 3600);
    deliverAsync(req->token(), std::move(callback), std::move(response));
    return req;
}

void MemoryFileSource::put(const QString& url, const QByteArray& bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries[key(url.toStdString())] = bytes.toStdString();
}

void MemoryFileSource::remove(const QString& url)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.erase(key(url.toStdString()));
}

bool MemoryFileSource::contains(const QString& url) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.count(key(url.toStdString())) > 0;
}

void MemoryFileSource::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_requested.clear();
}

std::size_t MemoryFileSource::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

QStringList MemoryFileSource::requested() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_requested;
}

}  // namespace maps::view
