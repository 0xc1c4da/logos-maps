#include "view/style_loader.h"

#include "providers/provider_registry.h"

#include <QMapLibre/Map>

#include <QJsonDocument>
#include <QUrl>

namespace maps::view {

StyleLoader::StyleLoader(QMapLibre::Map* map, CoreClient* core, QObject* parent)
    : QObject(parent), m_map(map), m_core(core) {}

void StyleLoader::load(const QString& basemapId)
{
    if (!m_core) {
        fail(basemapId, QStringLiteral("no core client"));
        return;
    }
    const int generation = ++m_generation;
    m_loading = basemapId;
    emit basemapLoading(basemapId);
    m_core->getBasemap(basemapId, [this, basemapId, generation](const CoreReply& reply) {
        if (generation != m_generation) return;
        if (!reply.ok) {
            fail(basemapId, reply.code() + QStringLiteral(": ") + reply.error);
            return;
        }
        const QJsonObject out = reply.object();
        m_descriptor = out.value(QStringLiteral("descriptor")).toObject();
        emit descriptorChanged(m_descriptor);
        emit attributionChanged(attributionHtml());
        if (out.contains(QStringLiteral("style"))) {
            // Raster descriptors come with a synthesised style (M9.4); it is
            // already canonical, no patch step.
            const QString style = QString::fromUtf8(QJsonDocument(out.value(QStringLiteral("style")).toObject()).toJson(QJsonDocument::Compact));
            applyStyleJson(style, basemapId);
            return;
        }
        const QString styleUrl = m_descriptor.value(QStringLiteral("style_url")).toString();
        if (styleUrl.isEmpty()) {
            fail(basemapId, QStringLiteral("descriptor has neither style nor style_url"));
            return;
        }
        fetchAndPatch(basemapId, styleUrl);
    });
}

void StyleLoader::fetchAndPatch(const QString& id, const QString& styleUrl)
{
    const int generation = m_generation;
    ProviderRegistry::instance().fetch(QUrl(styleUrl), [this, id, generation](bool ok, const QByteArray& bytes, const QString& error) {
        if (generation != m_generation) return;
        if (!ok) {
            fail(id, QStringLiteral("style fetch failed: %1").arg(error));
            return;
        }
        patchAndApply(id, QString::fromUtf8(bytes));
    });
}

void StyleLoader::patchAndApply(const QString& id, const QString& styleJson)
{
    const int generation = m_generation;
    m_core->patchStyle(id, styleJson, [this, id, generation](const CoreReply& reply) {
        if (generation != m_generation) return;
        if (!reply.ok) {
            fail(id, reply.code() + QStringLiteral(": ") + reply.error);
            return;
        }
        applyStyleJson(QString::fromUtf8(QJsonDocument(reply.object()).toJson(QJsonDocument::Compact)), id);
    });
}

void StyleLoader::applyStyleJson(const QString& styleJson, const QString& label)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(styleJson.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        fail(label, QStringLiteral("style is not a JSON object: %1").arg(err.errorString()));
        return;
    }
    if (!m_map) {
        fail(label, QStringLiteral("no map"));
        return;
    }
    m_lastStyle = styleJson;
    m_current = label;
    m_loading.clear();
    m_map->setStyleJson(styleJson);
    emit styleApplied(label);
}

void StyleLoader::fail(const QString& id, const QString& reason)
{
    if (m_loading == id) m_loading.clear();
    emit basemapFailed(id, reason);
    if (m_core && !m_viewId.isEmpty()) {
        const QJsonObject payload{{QStringLiteral("v"), 1}, {QStringLiteral("id"), id}, {QStringLiteral("reason"), reason}};
        m_core->reportViewEvent(m_viewId, QStringLiteral("basemapFailed"),
                                QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)), [](const CoreReply&) {});
    }
}

QString StyleLoader::attributionHtml() const
{
    QStringList parts;
    if (!m_engineCopyrights.isEmpty()) parts << m_engineCopyrights;
    const QJsonObject attribution = m_descriptor.value(QStringLiteral("attribution")).toObject();
    for (auto it = attribution.begin(); it != attribution.end(); ++it) {
        const QString entry = QStringLiteral("<a href=\"%1\">%2</a>").arg(it.value().toString().toHtmlEscaped(), it.key().toHtmlEscaped());
        if (!m_engineCopyrights.contains(it.key())) parts << entry;
    }
    return parts.join(QStringLiteral(" | "));
}

void StyleLoader::setEngineCopyrights(const QString& html)
{
    if (m_engineCopyrights == html) return;
    m_engineCopyrights = html;
    emit attributionChanged(attributionHtml());
}

}  // namespace maps::view
