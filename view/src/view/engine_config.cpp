#include "view/engine_config.h"

#include "providers/provider_registry.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace maps::view {

EngineConfig EngineConfig::defaults(const QString& moduleDir)
{
    EngineConfig c;
    // Under logosctl HOME is the session's; under Basecamp it is the user's.
    // Either way the location comes from the platform, never from "~".
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    const QDir dir(base + QStringLiteral("/logos-maps/maps_view"));
    dir.mkpath(QStringLiteral("."));
    c.cachePath = dir.filePath(QStringLiteral("cache.db"));
    if (!moduleDir.isEmpty() && QFileInfo(moduleDir + QStringLiteral("/assets")).isDir())
        c.assetPath = moduleDir + QStringLiteral("/assets");
    return c;
}

namespace {
EngineConfig& processDefaultStorage()
{
    static EngineConfig config = EngineConfig::defaults(QString());
    return config;
}
}  // namespace

const EngineConfig& EngineConfig::processDefault() { return processDefaultStorage(); }
void EngineConfig::setProcessDefault(const EngineConfig& config) { processDefaultStorage() = config; }

void EngineConfig::applyOptions(const QJsonObject& options)
{
    const QJsonObject cache = options.value(QStringLiteral("cache")).toObject();
    if (cache.contains(QStringLiteral("path"))) cachePath = cache.value(QStringLiteral("path")).toString();
    if (cache.contains(QStringLiteral("maxBytes")))
        cacheMaxBytes = static_cast<unsigned>(cache.value(QStringLiteral("maxBytes")).toDouble());
    const QJsonObject fonts = options.value(QStringLiteral("fonts")).toObject();
    if (fonts.contains(QStringLiteral("localFamily"))) localFontFamily = fonts.value(QStringLiteral("localFamily")).toString();
    const QJsonObject keys = options.value(QStringLiteral("keys")).toObject();
    if (keys.contains(QStringLiteral("apiKey"))) apiKey = keys.value(QStringLiteral("apiKey")).toString();
    if (keys.contains(QStringLiteral("apiBaseUrl"))) apiBaseUrl = keys.value(QStringLiteral("apiBaseUrl")).toString();
}

QMapLibre::Settings EngineConfig::toSettings() const
{
    QMapLibre::Settings s;
    s.setContextMode(QMapLibre::Settings::SharedGLContext);
    s.setCacheDatabasePath(cachePath.isEmpty() ? QStringLiteral(":memory:") : cachePath);
    s.setCacheDatabaseMaximumSize(cacheMaxBytes);
    if (!assetPath.isEmpty()) s.setAssetPath(assetPath);
    if (!localFontFamily.isEmpty()) s.setLocalFontFamily(localFontFamily);
    if (!apiKey.isEmpty()) s.setApiKey(apiKey);
    if (!apiBaseUrl.isEmpty()) s.setApiBaseUrl(apiBaseUrl);
    if (installProviderHook) s.setFileSourceFactoryHook(ProviderRegistry::instance().hook());
    return s;
}

QJsonObject EngineConfig::toJson() const
{
    return QJsonObject{{QStringLiteral("cache"), QJsonObject{{QStringLiteral("path"), cachePath},
                                                             {QStringLiteral("maxBytes"), static_cast<double>(cacheMaxBytes)}}},
                       {QStringLiteral("assetPath"), assetPath},
                       {QStringLiteral("fonts"), QJsonObject{{QStringLiteral("localFamily"), localFontFamily}}}};
}

}  // namespace maps::view
