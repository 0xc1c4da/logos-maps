// Section 16.3 Provider suite.
//
// In-process mbgl FileSourceManager with the composite loader (section 14.1
// P5). Asserts: order of consultation (registered providers before the
// engine's own asset/mbtiles/pmtiles/file/database/online chain, M10.2);
// memory:// serves and honours ranges; logos:// honours range requests, maps
// a timeout to Connection + retryAfter, a missing resource to NotFound, and
// serves bundled prefixes from disk without escaping them; the cache is
// populated from provider responses with `expires` honoured; a PMTiles v3
// fixture and an MBTiles 1.3 fixture, written here, each serve z0-2 through
// the engine's built-in sources and answer NotFound beyond their range.
#include "test_support.h"

#include "providers/composite_file_source.h"
#include "providers/logos_file_source.h"
#include "providers/memory_file_source.h"
#include "providers/provider_registry.h"
#include "providers/resource_provider.h"

#include <mbgl/storage/file_source.hpp>
#include <mbgl/storage/file_source_manager.hpp>
#include <mbgl/storage/resource.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/storage/response.hpp>
#include <mbgl/util/client_options.hpp>
#include <mbgl/util/run_loop.hpp>

#include <pmtiles.hpp>
#include <sqlite3.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace maps::test;
using namespace maps::view;

namespace {

// ── a recording inner FileSource standing in for the engine's loader ───────
class RecordingSource final : public mbgl::FileSource {
public:
    std::vector<std::string> urls;
    std::unique_ptr<mbgl::AsyncRequest> request(const mbgl::Resource& resource, Callback callback) override
    {
        urls.push_back(resource.url);
        auto req = std::make_unique<ProviderRequest>();
        deliverAsync(req->token(), std::move(callback), okResponse("inner:" + resource.url));
        return req;
    }
    bool canRequest(const mbgl::Resource&) const override { return true; }
    void setResourceOptions(mbgl::ResourceOptions o) override { m_ro = std::move(o); }
    mbgl::ResourceOptions getResourceOptions() override { return m_ro.clone(); }
    void setClientOptions(mbgl::ClientOptions o) override { m_co = std::move(o); }
    mbgl::ClientOptions getClientOptions() override { return m_co.clone(); }

private:
    mbgl::ResourceOptions m_ro;
    mbgl::ClientOptions m_co;
};

// A provider whose responses expire in the past (cache revalidation path).
class StaleProvider final : public ResourceProvider {
public:
    int hits = 0;
    QString scheme() const override { return QStringLiteral("stale"); }
    bool canRequest(const mbgl::Resource& r) const override { return r.url.rfind("stale://", 0) == 0; }
    std::unique_ptr<mbgl::AsyncRequest> request(const mbgl::Resource& r, mbgl::FileSource::Callback cb) override
    {
        ++hits;
        auto req = std::make_unique<ProviderRequest>();
        deliverAsync(req->token(), std::move(cb), okResponse("stale:" + r.url, mbgl::util::now() - std::chrono::hours(1), "etag-stale"));
        return req;
    }
};

struct Captured {
    bool done = false;
    mbgl::Response response;
};

/// Issue a request on `source` and pump until it answers.
Captured fetch(mbgl::FileSource& source, const mbgl::Resource& resource, int timeoutMs = 10000)
{
    auto cap = std::make_shared<Captured>();
    std::unique_ptr<mbgl::AsyncRequest> req = source.request(resource, [cap](mbgl::Response r) {
        cap->response = std::move(r);
        cap->done = true;
    });
    waitFor([cap] { return cap->done; }, timeoutMs);
    return *cap;
}

/// Same, for a provider (not itself a FileSource).
Captured fetch(ResourceProvider& provider, const mbgl::Resource& resource, int timeoutMs = 10000)
{
    auto cap = std::make_shared<Captured>();
    std::unique_ptr<mbgl::AsyncRequest> req = provider.request(resource, [cap](mbgl::Response r) {
        cap->response = std::move(r);
        cap->done = true;
    });
    waitFor([cap] { return cap->done; }, timeoutMs);
    return *cap;
}

std::string dataOf(const Captured& c) { return c.response.data ? *c.response.data : std::string(); }

// ── fixtures ───────────────────────────────────────────────────────────────

QByteArray tilePng(int z, int x, int y) { return pngTile(64, QColor(60 + 60 * z, 40 * x + 20, 40 * y + 20)); }

bool writePmtiles(const QString& path)
{
    std::vector<pmtiles::entryv3> entries;
    std::string tileData;
    for (int z = 0; z <= 2; ++z) {
        for (int x = 0; x < (1 << z); ++x) {
            for (int y = 0; y < (1 << z); ++y) {
                const QByteArray png = tilePng(z, x, y);
                entries.emplace_back(pmtiles::zxy_to_tileid(static_cast<uint8_t>(z), x, y), tileData.size(), static_cast<uint32_t>(png.size()), 1);
                tileData.append(png.constData(), png.size());
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.tile_id < b.tile_id; });
    const std::string rootDir = pmtiles::serialize_directory(entries);
    const std::string metadata = "{\"name\":\"fixture\"}";

    pmtiles::headerv3 h{};
    h.root_dir_offset = 127;
    h.root_dir_bytes = rootDir.size();
    h.json_metadata_offset = h.root_dir_offset + h.root_dir_bytes;
    h.json_metadata_bytes = metadata.size();
    h.leaf_dirs_offset = h.json_metadata_offset + h.json_metadata_bytes;
    h.leaf_dirs_bytes = 0;
    h.tile_data_offset = h.leaf_dirs_offset;
    h.tile_data_bytes = tileData.size();
    h.addressed_tiles_count = entries.size();
    h.tile_entries_count = entries.size();
    h.tile_contents_count = entries.size();
    h.clustered = true;
    h.internal_compression = pmtiles::COMPRESSION_NONE;
    h.tile_compression = pmtiles::COMPRESSION_NONE;
    h.tile_type = pmtiles::TILETYPE_PNG;
    h.min_zoom = 0;
    h.max_zoom = 2;
    h.min_lon_e7 = -1800000000;
    h.min_lat_e7 = -850000000;
    h.max_lon_e7 = 1800000000;
    h.max_lat_e7 = 850000000;
    h.center_zoom = 0;
    h.center_lon_e7 = 0;
    h.center_lat_e7 = 0;
    const std::string header = h.serialize();
    if (header.size() != 127) return false;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(header.data(), static_cast<qint64>(header.size()));
    f.write(rootDir.data(), static_cast<qint64>(rootDir.size()));
    f.write(metadata.data(), static_cast<qint64>(metadata.size()));
    f.write(tileData.data(), static_cast<qint64>(tileData.size()));
    return true;
}

bool writeMbtiles(const QString& path)
{
    sqlite3* db = nullptr;
    if (sqlite3_open(path.toUtf8().constData(), &db) != SQLITE_OK) return false;
    auto exec = [&](const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; };
    bool ok = exec("CREATE TABLE metadata (name text, value text);") &&
              exec("CREATE TABLE tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob);") &&
              exec("INSERT INTO metadata VALUES ('name','fixture'),('format','png'),('minzoom','0'),('maxzoom','2'),"
                   "('bounds','-180,-85,180,85'),('type','baselayer'),('version','1.3');");
    sqlite3_stmt* stmt = nullptr;
    ok = ok && sqlite3_prepare_v2(db, "INSERT INTO tiles VALUES (?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK;
    for (int z = 0; ok && z <= 2; ++z) {
        for (int x = 0; x < (1 << z); ++x) {
            for (int y = 0; y < (1 << z); ++y) {
                const QByteArray png = tilePng(z, x, y);
                sqlite3_reset(stmt);
                sqlite3_bind_int(stmt, 1, z);
                sqlite3_bind_int(stmt, 2, x);
                sqlite3_bind_int(stmt, 3, (1 << z) - 1 - y);  // MBTiles rows are TMS
                sqlite3_bind_blob(stmt, 4, png.constData(), png.size(), SQLITE_TRANSIENT);
                ok = sqlite3_step(stmt) == SQLITE_DONE;
            }
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok;
}

QString tilesTemplate(const Captured& tilejson)
{
    const QJsonObject tj = QJsonDocument::fromJson(QByteArray::fromStdString(dataOf(tilejson))).object();
    return tj.value(QStringLiteral("tiles")).toArray().first().toString();
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    // The engine's file sources need a run loop on the requesting thread;
    // the Qt flavour drives it from the application event loop.
    mbgl::util::RunLoop loop;

    ProviderRegistry& registry = ProviderRegistry::instance();
    registry.clear();

    // ── order of consultation ────────────────────────────────────────────
    {
        auto memory = std::make_shared<MemoryFileSource>();
        memory->put(QStringLiteral("memory://a.json"), "A");
        int fetcherCalls = 0;
        auto logos = std::make_shared<LogosFileSource>([&](const QString&, const QString&, qint64, qint64, FetchResultCallback cb) {
            ++fetcherCalls;
            FetchResult r;
            r.ok = true;
            r.bytes = "L";
            cb(r);
        });
        logos->setProviders(QJsonArray{QJsonObject{{QStringLiteral("module"), QStringLiteral("tiles")}, {QStringLiteral("prefix"), QString()}}});
        auto inner = std::make_unique<RecordingSource>();
        RecordingSource* innerPtr = inner.get();
        CompositeFileSource composite(std::move(inner), [&] { return std::vector<std::shared_ptr<ResourceProvider>>{memory, logos}; });

        CHECK(dataOf(fetch(composite, mbgl::Resource::style("memory://a.json"))) == "A");
        CHECK(dataOf(fetch(composite, mbgl::Resource::style("logos://tiles/x"))) == "L");
        CHECK(fetcherCalls == 1);
        CHECK(innerPtr->urls.empty());
        for (const char* url : {"https://example.invalid/style.json", "file:///tmp/x", "asset://sprite", "mbtiles:///tmp/x.mbtiles", "pmtiles:///tmp/x.pmtiles"}) {
            CHECK_MSG(dataOf(fetch(composite, mbgl::Resource::style(url))) == std::string("inner:") + url, QString::fromLatin1(url));
        }
        CHECK(innerPtr->urls.size() == 5);
        CHECK(composite.canRequest(mbgl::Resource::style("memory://anything")));
        CHECK(composite.canRequest(mbgl::Resource::style("https://anything")));

        // The registry lists built-ins and registered schemes for M11.1.
        registry.add(memory);
        registry.add(logos);
        const QStringList schemes = registry.schemes();
        for (const char* s : {"http", "https", "file", "asset", "mbtiles", "pmtiles", "memory", "logos"}) CHECK_MSG(schemes.contains(QLatin1String(s)), QLatin1String(s));
        CHECK(registry.rangeRequests());
        registry.clear();
    }

    // ── memory:// ────────────────────────────────────────────────────────
    {
        MemoryFileSource memory;
        memory.put(QStringLiteral("memory://blob"), QByteArray("0123456789"));
        CHECK(dataOf(fetch(memory, mbgl::Resource::style("memory://blob"))) == "0123456789");
        mbgl::Resource ranged(mbgl::Resource::Kind::Source, "memory://blob");
        ranged.dataRange = std::make_pair<uint64_t, uint64_t>(2, 5);
        CHECK(dataOf(fetch(memory, ranged)) == "2345");
        const Captured missing = fetch(memory, mbgl::Resource::style("memory://nope"));
        CHECK(missing.response.error && missing.response.error->reason == mbgl::Response::Error::Reason::NotFound);
        const Captured fresh = fetch(memory, mbgl::Resource::style("memory://blob"));
        CHECK(fresh.response.expires && *fresh.response.expires > mbgl::util::now());
        CHECK(memory.requested().size() == 4);
        CHECK(!memory.canRequest(mbgl::Resource::style("https://x")));
    }

    // ── logos:// ─────────────────────────────────────────────────────────
    {
        struct Call { QString module, path; qint64 start, end; };
        std::vector<Call> calls;
        QString mode = QStringLiteral("ok");
        LogosFileSource logos([&](const QString& module, const QString& path, qint64 start, qint64 end, FetchResultCallback cb) {
            calls.push_back({module, path, start, end});
            FetchResult r;
            if (mode == QLatin1String("ok")) {
                r.ok = true;
                r.bytes = QByteArray("abcdefghij");
                if (start >= 0 && end >= start) r.bytes = r.bytes.mid(static_cast<int>(start), static_cast<int>(end - start + 1));
            } else {
                r.errorCode = mode;
                r.errorMessage = QStringLiteral("simulated ") + mode;
            }
            // Complete from another thread, as IPC would.
            std::thread([cb, r] { cb(r); }).detach();
        });
        logos.setRetryAfter(mbgl::Seconds(7));
        logos.setProviders(QJsonArray{
            QJsonObject{{QStringLiteral("session"), QStringLiteral("osm")}, {QStringLiteral("scheme"), QStringLiteral("logos")},
                        {QStringLiteral("module"), QStringLiteral("osm_tiles")}, {QStringLiteral("prefix"), QString()}},
            QJsonObject{{QStringLiteral("session"), QStringLiteral("osm")}, {QStringLiteral("scheme"), QStringLiteral("logos")},
                        {QStringLiteral("module"), QStringLiteral("osm_tiles")}, {QStringLiteral("prefix"), QStringLiteral("hires/")}},
        });
        CHECK(logos.canRequest(mbgl::Resource::style("logos://osm_tiles/1/2/3.pbf")));
        CHECK(!logos.canRequest(mbgl::Resource::style("https://osm_tiles/1")));
        CHECK(logos.supportsRangeRequests());

        CHECK(dataOf(fetch(logos, mbgl::Resource::style("logos://osm_tiles/1/2/3.pbf"))) == "abcdefghij");
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].module == QStringLiteral("osm_tiles") && calls[0].path == QStringLiteral("1/2/3.pbf"));
        CHECK(calls[0].start == 0 && calls[0].end == -1);

        mbgl::Resource ranged(mbgl::Resource::Kind::Source, "logos://osm_tiles/hires/archive.pmtiles");
        ranged.dataRange = std::make_pair<uint64_t, uint64_t>(3, 6);
        CHECK(dataOf(fetch(logos, ranged)) == "defg");
        REQUIRE(calls.size() == 2);
        CHECK(calls[1].start == 3 && calls[1].end == 6);
        const auto target = logos.resolve(QStringLiteral("osm_tiles"), QStringLiteral("hires/archive.pmtiles"));
        CHECK(target && target->prefix == QStringLiteral("hires/"));

        mode = QStringLiteral("timeout");
        Captured t = fetch(logos, mbgl::Resource::style("logos://osm_tiles/slow"));
        REQUIRE(t.response.error != nullptr);
        CHECK(t.response.error->reason == mbgl::Response::Error::Reason::Connection);
        {
            // retryAfter is an absolute time: now + the configured 7 s (within slack).
            REQUIRE(t.response.error->retryAfter.has_value());
            const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(*t.response.error->retryAfter - mbgl::util::now()).count();
            CHECK_MSG(delta > 4000 && delta <= 7000, QStringLiteral("retryAfter in %1 ms").arg(delta));
        }

        mode = QStringLiteral("unavailable");
        Captured u = fetch(logos, mbgl::Resource::style("logos://osm_tiles/down"));
        REQUIRE(u.response.error != nullptr);
        CHECK(u.response.error->reason == mbgl::Response::Error::Reason::Connection);

        mode = QStringLiteral("notFound");
        Captured n = fetch(logos, mbgl::Resource::style("logos://osm_tiles/9/9/9.pbf"));
        REQUIRE(n.response.error != nullptr);
        CHECK(n.response.error->reason == mbgl::Response::Error::Reason::NotFound);

        // No provider for the module: NotFound without an IPC call.
        const size_t before = calls.size();
        Captured none = fetch(logos, mbgl::Resource::style("logos://unknown_module/x"));
        CHECK(none.response.error && none.response.error->reason == mbgl::Response::Error::Reason::NotFound);
        CHECK(calls.size() == before);

        // Bundled prefix from disk, confined to its directory.
        const QString glyphs = tmp.filePath(QStringLiteral("glyphs"));
        QDir().mkpath(glyphs);
        QFile g(glyphs + QStringLiteral("/Noto/0-255.pbf"));
        QDir().mkpath(glyphs + QStringLiteral("/Noto"));
        REQUIRE(g.open(QIODevice::WriteOnly));
        g.write("GLYPHS");
        g.close();
        QFile secret(tmp.filePath(QStringLiteral("secret.txt")));
        REQUIRE(secret.open(QIODevice::WriteOnly));
        secret.write("SECRET");
        secret.close();
        logos.addLocalPrefix(QStringLiteral("maps"), QStringLiteral("glyphs/"), glyphs);
        CHECK(dataOf(fetch(logos, mbgl::Resource::style("logos://maps/glyphs/Noto/0-255.pbf"))) == "GLYPHS");
        Captured esc = fetch(logos, mbgl::Resource::style("logos://maps/glyphs/../secret.txt"));
        CHECK(esc.response.error && esc.response.error->reason == mbgl::Response::Error::Reason::NotFound);
        CHECK(calls.size() == before);  // local prefixes never go to IPC
        mbgl::Resource gr(mbgl::Resource::Kind::Glyphs, "logos://maps/glyphs/Noto/0-255.pbf");
        gr.dataRange = std::make_pair<uint64_t, uint64_t>(1, 3);
        CHECK(dataOf(fetch(logos, gr)) == "LYP");
        // setProviders keeps local prefixes, replaces IPC ones.
        logos.setProviders(QJsonArray{});
        CHECK(logos.resolve(QStringLiteral("maps"), QStringLiteral("glyphs/x")).has_value());
        CHECK(!logos.resolve(QStringLiteral("osm_tiles"), QStringLiteral("x")).has_value());
    }

    // ── the engine's loader through the P5 hook: cache + fixtures ────────
    {
        auto memory = std::make_shared<MemoryFileSource>();
        auto stale = std::make_shared<StaleProvider>();
        registry.add(memory);
        registry.add(stale);
        // Install the composite factory as Settings would (P5).
        registry.hook()(*mbgl::FileSourceManager::get());
        CHECK(registry.hookInstalled());

        const std::string cachePath = tmp.filePath(QStringLiteral("cache.db")).toStdString();
        mbgl::ResourceOptions ro;
        ro.withCachePath(cachePath).withMaximumCacheSize(50u * 1024u * 1024u);
        std::shared_ptr<mbgl::FileSource> loader = mbgl::FileSourceManager::get()->getFileSource(mbgl::FileSourceType::ResourceLoader, ro, mbgl::ClientOptions());
        REQUIRE(loader != nullptr);

        // Provider responses are served and cached (fresh: no second provider hit).
        memory->put(QStringLiteral("memory://sprite.json"), "{\"a\":1}");
        CHECK(dataOf(fetch(*loader, mbgl::Resource::style("memory://sprite.json"))) == "{\"a\":1}");
        CHECK(memory->requested().size() == 1);
        memory->remove(QStringLiteral("memory://sprite.json"));
        // Still answered: from the cache, without asking the provider.
        CHECK(dataOf(fetch(*loader, mbgl::Resource::style("memory://sprite.json"))) == "{\"a\":1}");
        CHECK_MSG(memory->requested().size() == 1, QStringLiteral("provider asked again: %1").arg(memory->requested().size()));
        mbgl::Resource cacheOnly(mbgl::Resource::Kind::Style, "memory://sprite.json", {}, mbgl::Resource::LoadingMethod::CacheOnly);
        CHECK(dataOf(fetch(*loader, cacheOnly)) == "{\"a\":1}");

        // Expired responses are revalidated: the provider is asked again.
        CHECK(dataOf(fetch(*loader, mbgl::Resource::style("stale://thing"))) == "stale:stale://thing");
        CHECK(stale->hits == 1);
        Captured again = fetch(*loader, mbgl::Resource::style("stale://thing"));
        CHECK(dataOf(again) == "stale:stale://thing" || again.response.notModified);
        // The cached copy is delivered first; the revalidation hit follows asynchronously.
        waitFor([&] { return stale->hits == 2; }, 10000);
        CHECK_MSG(stale->hits == 2, QStringLiteral("stale hits: %1").arg(stale->hits));

        // PMTiles v3 fixture via pmtiles://
        const QString pm = tmp.filePath(QStringLiteral("fixture.pmtiles"));
        REQUIRE(writePmtiles(pm));
        // The inner URL is itself a resource URL (file://, http://, or a provider
        // scheme); a bare path would be sent to the network slot.
        const std::string pmUrl = "pmtiles://file://" + pm.toStdString();
        Captured pmJson = fetch(*loader, mbgl::Resource::source(pmUrl));
        CHECK_MSG(!pmJson.response.error, pmJson.response.error ? QString::fromStdString(pmJson.response.error->message) : QString());
        const QString pmTemplate = tilesTemplate(pmJson);
        CHECK_MSG(pmTemplate.startsWith(QStringLiteral("pmtiles://")), pmTemplate);
        for (int z = 0; z <= 2; ++z) {
            for (int x = 0; x < (1 << z); ++x) {
                for (int y = 0; y < (1 << z); ++y) {
                    Captured t = fetch(*loader, mbgl::Resource::tile(pmTemplate.toStdString(), 1.0f, x, y, static_cast<int8_t>(z), mbgl::Tileset::Scheme::XYZ));
                    CHECK_MSG(dataOf(t) == tilePng(z, x, y).toStdString(), QStringLiteral("pmtiles %1/%2/%3").arg(z).arg(x).arg(y));
                }
            }
        }
        Captured pmMissing = fetch(*loader, mbgl::Resource::tile(pmTemplate.toStdString(), 1.0f, 0, 0, 3, mbgl::Tileset::Scheme::XYZ));
        CHECK(pmMissing.response.noContent || (pmMissing.response.error && pmMissing.response.error->reason == mbgl::Response::Error::Reason::NotFound));

        // MBTiles 1.3 fixture via mbtiles://
        const QString mb = tmp.filePath(QStringLiteral("fixture.mbtiles"));
        REQUIRE(writeMbtiles(mb));
        const std::string mbUrl = "mbtiles://" + mb.toStdString();
        Captured mbJson = fetch(*loader, mbgl::Resource::source(mbUrl));
        CHECK_MSG(!mbJson.response.error, mbJson.response.error ? QString::fromStdString(mbJson.response.error->message) : QString());
        const QString mbTemplate = tilesTemplate(mbJson);
        CHECK_MSG(mbTemplate.startsWith(QStringLiteral("mbtiles://")), mbTemplate);
        for (int z = 0; z <= 2; ++z) {
            for (int x = 0; x < (1 << z); ++x) {
                for (int y = 0; y < (1 << z); ++y) {
                    Captured t = fetch(*loader, mbgl::Resource::tile(mbTemplate.toStdString(), 1.0f, x, y, static_cast<int8_t>(z), mbgl::Tileset::Scheme::XYZ));
                    CHECK_MSG(dataOf(t) == tilePng(z, x, y).toStdString(), QStringLiteral("mbtiles %1/%2/%3").arg(z).arg(x).arg(y));
                }
            }
        }
        Captured mbMissing = fetch(*loader, mbgl::Resource::tile(mbTemplate.toStdString(), 1.0f, 0, 0, 3, mbgl::Tileset::Scheme::XYZ));
        CHECK(mbMissing.response.noContent || (mbMissing.response.error && mbMissing.response.error->reason == mbgl::Response::Error::Reason::NotFound));

        // The request log the capability report and dev tooling read.
        int logged = 0;
        QObject::connect(&registry, &ProviderRegistry::requestLogged, [&](const QString& scheme, const QString&, qint64, qint64) { if (scheme == QLatin1String("memory")) ++logged; });
        memory->put(QStringLiteral("memory://logged"), "x");
        fetch(*loader, mbgl::Resource::style("memory://logged"));
        CHECK(logged >= 1);
        // Let the database thread land the forwarded write before teardown
        // (CacheOnly is answered by the same thread, in order).
        mbgl::Resource loggedCacheOnly(mbgl::Resource::Kind::Style, "memory://logged", {}, mbgl::Resource::LoadingMethod::CacheOnly);
        CHECK(dataOf(fetch(*loader, loggedCacheOnly)) == "x");

        // Upstream keeps the ResourceLoader alive through PMTilesFileSource
        // (it holds the loader it was created by: pmtiles_file_source.cpp
        // getFileSource), so cache.db stays open and QTemporaryDir cannot
        // remove the directory; that is expected.
        loader.reset();
        registry.clear();
    }

    return finish();
}
