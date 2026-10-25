// Per-view engine configuration (docs/architecture.md M5.8, section 15.3).
#pragma once

#include <QMapLibre/Settings>

#include <QJsonObject>
#include <QString>

namespace maps::view {

struct EngineConfig {
    QString cachePath;              // on-disk cache database (never ":memory:" in production)
    unsigned cacheMaxBytes = 512u * 1024u * 1024u;  // desktop default (M15.2)
    QString assetPath;              // bundled assets (glyph packs, sprites)
    QString localFontFamily;        // CJK ideographs (M9.8)
    QString apiKey;                 // only when a descriptor requires it
    QString apiBaseUrl;
    bool installProviderHook = true;

    /// Cache under the platform cache location (QStandardPaths), never "~"
    /// (section 15.3). `moduleDir` is the plugin's install directory, whose
    /// assets/ subdirectory (if any) becomes assetPath.
    static EngineConfig defaults(const QString& moduleDir);

    /// The configuration MapItems created from QML start with (they have no
    /// chance to be configured before componentComplete creates the map).
    static const EngineConfig& processDefault();
    static void setProcessDefault(const EngineConfig& config);

    /// Apply the `cache`/`fonts`/`keys` members of a setViewOptions payload.
    void applyOptions(const QJsonObject& options);

    /// Build the Settings a QMapLibre::Map is constructed with: shared GL
    /// context, cache path/size, asset path, local font, and the provider
    /// hook (section 14.1 P5) when installProviderHook.
    QMapLibre::Settings toSettings() const;

    QJsonObject toJson() const;
};

}  // namespace maps::view
