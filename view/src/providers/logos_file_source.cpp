#include "providers/logos_file_source.h"

#include "providers/provider_registry.h"

#include <mbgl/actor/scheduler.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonValue>

namespace maps::view {

namespace {

// "logos://module/path" -> (module, path); path keeps no leading slash.
bool parseLogosUrl(const std::string& url, QString& module, QString& path)
{
    auto [scheme, rest] = splitScheme(url);
    if (scheme != "logos") return false;
    const QString r = QString::fromStdString(rest);
    const int slash = r.indexOf(QLatin1Char('/'));
    if (slash < 0) { module = r; path.clear(); return !module.isEmpty(); }
    module = r.left(slash);
    path = r.mid(slash + 1);
    return !module.isEmpty();
}

}  // namespace

LogosFileSource::LogosFileSource(ResourceFetcher fetcher) : m_fetcher(std::move(fetcher)) {}

bool LogosFileSource::canRequest(const mbgl::Resource& resource) const
{
    return splitScheme(resource.url).first == "logos";
}

void LogosFileSource::setProviders(const QJsonArray& providers)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::erase_if(m_targets, [](const Target& t) { return t.localDirectory.isEmpty(); });
    for (const QJsonValue& v : providers) {
        const QJsonObject o = v.toObject();
        Target t;
        t.module = o.value(QStringLiteral("module")).toString();
        t.prefix = o.value(QStringLiteral("prefix")).toString();
        if (t.module.isEmpty()) continue;
        m_targets.push_back(std::move(t));
    }
}

void LogosFileSource::addLocalPrefix(const QString& module, const QString& prefix, const QString& directory)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_targets.push_back(Target{module, prefix, directory});
}

std::optional<LogosFileSource::Target> LogosFileSource::resolve(const QString& module, const QString& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const Target* best = nullptr;
    for (const Target& t : m_targets) {
        if (t.module != module) continue;
        if (!path.startsWith(t.prefix)) continue;
        if (!best || t.prefix.size() > best->prefix.size()) best = &t;
    }
    if (!best) return std::nullopt;
    return *best;
}

std::unique_ptr<mbgl::AsyncRequest> LogosFileSource::request(const mbgl::Resource& resource,
                                                             mbgl::FileSource::Callback callback)
{
    auto req = std::make_unique<ProviderRequest>();
    auto alive = req->token();

    QString module, path;
    if (!parseLogosUrl(resource.url, module, path)) {
        deliverAsync(alive, std::move(callback), otherErrorResponse("logos: malformed URL " + resource.url));
        return req;
    }
    const qint64 rangeStart = resource.dataRange ? static_cast<qint64>(resource.dataRange->first) : 0;
    const qint64 rangeEnd = resource.dataRange ? static_cast<qint64>(resource.dataRange->second) : -1;
    ProviderRegistry::instance().logRequest(QStringLiteral("logos"), module + QLatin1Char('/') + path, rangeStart, rangeEnd);

    const auto target = resolve(module, path);
    if (!target) {
        deliverAsync(alive, std::move(callback),
                     notFoundResponse("logos: no provider registered for " + resource.url));
        return req;
    }

    if (!target->localDirectory.isEmpty()) {
        const QString rel = path.mid(target->prefix.size());
        const QString full = QDir(target->localDirectory).filePath(rel);
        // Stay inside the directory.
        const QString canonicalDir = QFileInfo(target->localDirectory).canonicalFilePath();
        const QString canonicalFile = QFileInfo(full).canonicalFilePath();
        QFile file(full);
        if (canonicalFile.isEmpty() || !canonicalFile.startsWith(canonicalDir) || !file.open(QIODevice::ReadOnly)) {
            deliverAsync(alive, std::move(callback), notFoundResponse("logos: no such asset " + resource.url));
            return req;
        }
        QByteArray bytes = file.readAll();
        if (resource.dataRange) {
            const qint64 start = rangeStart;
            const qint64 end = std::min<qint64>(rangeEnd, bytes.size() - 1);
            bytes = start < bytes.size() ? bytes.mid(start, end - start + 1) : QByteArray();
        }
        deliverAsync(alive, std::move(callback), okResponse(bytes.toStdString()));
        return req;
    }

    if (!m_fetcher) {
        deliverAsync(alive, std::move(callback), otherErrorResponse("logos: no IPC fetcher installed"));
        return req;
    }

    // The fetcher completes on an arbitrary thread; marshal back to the
    // requester's scheduler (M15.1: the render thread's QMapLibre::Scheduler
    // for tiles, the GUI RunLoop for styles) and drop the result if the
    // request was cancelled or the scheduler is gone.
    mapbox::base::WeakPtr<mbgl::Scheduler> scheduler = mbgl::Scheduler::GetCurrent()->makeWeakPtr();
    const mbgl::Seconds retryAfter = m_retryAfter;
    m_fetcher(target->module, path, rangeStart, rangeEnd,
              [alive, callback, scheduler, retryAfter, url = resource.url](const FetchResult& result) {
                  mbgl::Response response;
                  if (result.ok) {
                      response = okResponse(result.bytes.toStdString());
                  } else if (result.errorCode == QLatin1String("timeout") ||
                             result.errorCode == QLatin1String("unavailable")) {
                      response = connectionErrorResponse("logos: " + result.errorMessage.toStdString(), retryAfter);
                  } else if (result.errorCode == QLatin1String("notFound")) {
                      response = notFoundResponse("logos: " + result.errorMessage.toStdString());
                  } else {
                      response = otherErrorResponse("logos: " + result.errorMessage.toStdString());
                  }
                  auto guard = scheduler.lock();
                  if (!scheduler) return;
                  scheduler->schedule([alive, callback, response = std::move(response)]() mutable {
                      if (alive->load()) callback(std::move(response));
                  });
              });
    return req;
}

}  // namespace maps::view
