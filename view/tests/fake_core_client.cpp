#include "fake_core_client.h"

#include <QJsonDocument>

#include <cstdio>

namespace maps::test {

using maps::view::CoreCallback;
using maps::view::CoreReply;

FakeCoreClient::FakeCoreClient(QObject* parent) : CoreClient(parent) {}

// ── controls ─────────────────────────────────────────────────────────────

void FakeCoreClient::addBasemap(const QString& id, const QJsonObject& style, const QString& styleUrl)
{
    QJsonObject descriptor{
        {QStringLiteral("v"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), id},
        {QStringLiteral("format"), style.isEmpty() ? QStringLiteral("mapbox-gl") : QStringLiteral("raster")},
        {QStringLiteral("attribution"), QJsonObject{{QStringLiteral("Test tiles"), QStringLiteral("https://example.invalid/")}}},
        {QStringLiteral("fingerprint"), QStringLiteral("fp-") + id},
        {QStringLiteral("anchors"), QJsonObject{}},
    };
    if (!styleUrl.isEmpty()) descriptor.insert(QStringLiteral("style_url"), styleUrl);
    QJsonObject entry{{QStringLiteral("descriptor"), descriptor}};
    if (!style.isEmpty()) entry.insert(QStringLiteral("style"), style);
    m_basemaps[id] = entry;
    if (m_defaultBasemap.isEmpty()) m_defaultBasemap = id;
}

void FakeCoreClient::setDescriptorExtras(const QString& id, const QJsonObject& extras)
{
    auto it = m_basemaps.find(id);
    if (it == m_basemaps.end()) return;
    QJsonObject descriptor = it->second.value(QStringLiteral("descriptor")).toObject();
    for (auto e = extras.begin(); e != extras.end(); ++e) descriptor.insert(e.key(), e.value());
    it->second.insert(QStringLiteral("descriptor"), descriptor);
}

quint64 FakeCoreClient::publish(const QJsonArray& ops, bool emitEvent)
{
    ++m_revision;
    for (const QJsonValue& v : ops) applyOpToScene(v.toObject());
    m_log.emplace_back(m_revision, ops);
    if (emitEvent) emitCoreEvent(QStringLiteral("revisionPublished"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("revision"), static_cast<double>(m_revision)}});
    return m_revision;
}

void FakeCoreClient::emitCoreEvent(const QString& topic, QJsonObject payload)
{
    if (!payload.contains(QStringLiteral("v"))) payload.insert(QStringLiteral("v"), 1);
    QTimer::singleShot(m_latency, this, [this, topic, payload] { emit coreEvent(topic, payload); });
}

QString FakeCoreClient::sessionOf(const QString& globalId)
{
    const int colon = globalId.indexOf(QLatin1Char(':'));
    return colon < 0 ? QStringLiteral("default") : globalId.left(colon);
}

void FakeCoreClient::applyOpToScene(const QJsonObject& op)
{
    const QString kind = op.value(QStringLiteral("op")).toString();
    const QString id = op.value(QStringLiteral("id")).toString();
    if (kind == QLatin1String("putSource")) {
        Session& s = m_sessions[sessionOf(id)];
        QJsonObject src{{QStringLiteral("id"), id}, {QStringLiteral("type"), op.value(QStringLiteral("type"))}, {QStringLiteral("def"), op.value(QStringLiteral("def"))}};
        if (op.contains(QStringLiteral("data"))) src.insert(QStringLiteral("data"), op.value(QStringLiteral("data")));
        else if (s.sources.contains(id)) src.insert(QStringLiteral("data"), s.sources.value(id).toObject().value(QStringLiteral("data")));
        s.sources.insert(id, src);
    } else if (kind == QLatin1String("replaceGeoJson")) {
        const QString sid = op.value(QStringLiteral("sourceId")).toString();
        Session& s = m_sessions[sessionOf(sid)];
        QJsonObject src = s.sources.value(sid).toObject();
        if (src.isEmpty()) {
            src.insert(QStringLiteral("id"), sid);
            src.insert(QStringLiteral("type"), QStringLiteral("geojson"));
            src.insert(QStringLiteral("def"), QJsonObject{});
        }
        src.insert(QStringLiteral("data"), op.value(QStringLiteral("data")));
        s.sources.insert(sid, src);
    } else if (kind == QLatin1String("removeSource")) {
        m_sessions[sessionOf(id)].sources.remove(id);
    } else if (kind == QLatin1String("putImage")) {
        m_sessions[sessionOf(id)].images.insert(id, op);
    } else if (kind == QLatin1String("removeImage")) {
        m_sessions[sessionOf(id)].images.remove(id);
    } else if (kind == QLatin1String("putLayer")) {
        Session& s = m_sessions[sessionOf(id)];
        QJsonArray layers;
        bool replaced = false;
        for (const QJsonValue& l : s.layers) {
            if (l.toObject().value(QStringLiteral("id")).toString() == id) {
                layers.append(op);
                replaced = true;
            } else {
                layers.append(l);
            }
        }
        if (!replaced) layers.append(op);
        s.layers = layers;
        if (!m_layerOrder.contains(id)) m_layerOrder << id;
    } else if (kind == QLatin1String("removeLayer")) {
        Session& s = m_sessions[sessionOf(id)];
        QJsonArray layers;
        for (const QJsonValue& l : s.layers) if (l.toObject().value(QStringLiteral("id")).toString() != id) layers.append(l);
        s.layers = layers;
        m_layerOrder.removeAll(id);
    } else if (kind == QLatin1String("putGroup")) {
        QJsonObject g = op;
        g.remove(QStringLiteral("op"));
        m_sessions[sessionOf(id)].groups.insert(id, g);
    } else if (kind == QLatin1String("removeGroup")) {
        m_sessions[sessionOf(id)].groups.remove(id);
    }
}

QJsonObject FakeCoreClient::scene() const
{
    QJsonObject sessions;
    for (const auto& [name, s] : m_sessions) {
        sessions.insert(name, QJsonObject{{QStringLiteral("sources"), s.sources}, {QStringLiteral("images"), s.images},
                                          {QStringLiteral("groups"), s.groups}, {QStringLiteral("layers"), s.layers}});
    }
    return QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("revision"), static_cast<double>(m_revision)}, {QStringLiteral("sessions"), sessions}};
}

bool FakeCoreClient::hasEvent(const QString& topic) const { return eventCount(topic) > 0; }

int FakeCoreClient::eventCount(const QString& topic) const
{
    int n = 0;
    for (const auto& e : events) if (e.first == topic) ++n;
    return n;
}

QJsonObject FakeCoreClient::lastEvent(const QString& topic) const
{
    for (auto it = events.rbegin(); it != events.rend(); ++it) if (it->first == topic) return it->second;
    return {};
}

bool FakeCoreClient::acked(quint64 revision) const
{
    for (const auto& a : acks) if (a.second == revision) return true;
    return false;
}

// ── protocol ─────────────────────────────────────────────────────────────

void FakeCoreClient::reply(const QString& method, CoreCallback cb, bool ok, QJsonValue value, const QString& error)
{
    calls << method;
    auto f = m_failNext.find(method);
    if (f != m_failNext.end()) {
        ok = false;
        value = QJsonValue();
        const QString err = f->second;
        m_failNext.erase(f);
        QTimer::singleShot(m_latency, this, [cb, err] { if (cb) cb(CoreReply{false, QJsonValue(), err}); });
        return;
    }
    CoreReply r{ok, value, ok ? QString() : error};
    QTimer::singleShot(m_latency, this, [cb, r] { if (cb) cb(r); });
}

void FakeCoreClient::openView(const QString& viewId, const QString& surfaceToken, const QString& capabilitiesJson, CoreCallback cb)
{
    opened.push_back({viewId, surfaceToken, QJsonDocument::fromJson(capabilitiesJson.toUtf8()).object()});
    m_viewBasemap[viewId] = m_defaultBasemap;
    reply(QStringLiteral("openView"), std::move(cb), true,
          QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("viewId"), viewId}, {QStringLiteral("revision"), static_cast<double>(m_revision)},
                      {QStringLiteral("basemap"), m_defaultBasemap},
                      {QStringLiteral("limits"), QJsonObject{{QStringLiteral("maxBatchBytes"), 4194304}, {QStringLiteral("maxImageBytes"), 1048576}}}});
}

void FakeCoreClient::closeView(const QString& viewId, CoreCallback cb)
{
    closed << viewId;
    reply(QStringLiteral("closeView"), std::move(cb), true, true);
}

void FakeCoreClient::ackRevision(const QString& viewId, quint64 revision, CoreCallback cb)
{
    acks.emplace_back(viewId, revision);
    reply(QStringLiteral("ackRevision"), std::move(cb), true, true);
}

void FakeCoreClient::reportViewState(const QString&, const QString& stateJson, CoreCallback cb)
{
    viewStates.push_back(QJsonDocument::fromJson(stateJson.toUtf8()).object());
    reply(QStringLiteral("reportViewState"), std::move(cb), true, true);
}

void FakeCoreClient::reportViewEvent(const QString&, const QString& topic, const QString& payloadJson, CoreCallback cb)
{
    events.emplace_back(topic, QJsonDocument::fromJson(payloadJson.toUtf8()).object());
    if (qEnvironmentVariableIsSet("MAPS_TEST_TRACE")) std::fprintf(stderr, "  [event] %s %s\n", qPrintable(topic), qPrintable(payloadJson.left(160)));
    reply(QStringLiteral("reportViewEvent"), std::move(cb), true, true);
}

void FakeCoreClient::getScene(CoreCallback cb) { reply(QStringLiteral("getScene"), std::move(cb), true, scene()); }

void FakeCoreClient::getChanges(quint64 sinceRevision, CoreCallback cb)
{
    if (sinceRevision > m_revision) {
        reply(QStringLiteral("getChanges"), std::move(cb), false, QJsonValue(), QStringLiteral("maps.badArgument: revision from the future"));
        return;
    }
    // Log window: everything we have; a `since` older than the log start
    // yields the full scene, as the core does.
    const quint64 logStart = m_log.empty() ? m_revision : m_log.front().first - 1;
    if (sinceRevision < logStart) {
        reply(QStringLiteral("getChanges"), std::move(cb), true,
              QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("full"), true}, {QStringLiteral("from"), static_cast<double>(sinceRevision)},
                          {QStringLiteral("to"), static_cast<double>(m_revision)}, {QStringLiteral("scene"), scene()}});
        return;
    }
    QJsonArray ops;
    for (const auto& [rev, batch] : m_log) {
        if (rev <= sinceRevision) continue;
        for (const QJsonValue& v : batch) ops.append(v);
    }
    reply(QStringLiteral("getChanges"), std::move(cb), true,
          QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("from"), static_cast<double>(sinceRevision)}, {QStringLiteral("to"), static_cast<double>(m_revision)},
                      {QStringLiteral("ops"), ops}});
}

void FakeCoreClient::getLayerOrder(const QString& viewId, CoreCallback cb)
{
    QJsonArray layers;
    for (const QString& id : m_layerOrder) {
        layers.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("before"), QString()}, {QStringLiteral("anchor"), QStringLiteral("top")}});
    }
    reply(QStringLiteral("getLayerOrder"), std::move(cb), true,
          QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("layers"), layers}, {QStringLiteral("missingAnchors"), QJsonArray{}},
                      {QStringLiteral("danglingRefs"), QJsonArray{}}, {QStringLiteral("revision"), static_cast<double>(m_revision)},
                      {QStringLiteral("basemap"), m_viewBasemap[viewId]}});
}

void FakeCoreClient::getBasemap(const QString& id, CoreCallback cb)
{
    auto it = m_basemaps.find(id);
    if (it == m_basemaps.end()) {
        reply(QStringLiteral("getBasemap"), std::move(cb), false, QJsonValue(), QStringLiteral("maps.unknownId: no basemap '%1'").arg(id));
        return;
    }
    QJsonObject out = it->second;
    out.insert(QStringLiteral("v"), 1);
    reply(QStringLiteral("getBasemap"), std::move(cb), true, out);
}

void FakeCoreClient::patchStyle(const QString& id, const QString& styleJson, CoreCallback cb)
{
    patchedStyles.emplace_back(id, styleJson);
    const QJsonDocument doc = QJsonDocument::fromJson(styleJson.toUtf8());
    if (!doc.isObject()) {
        reply(QStringLiteral("patchStyle"), std::move(cb), false, QJsonValue(), QStringLiteral("maps.badArgument: style is not a JSON object"));
        return;
    }
    // The "patch": tag the style so the round trip is observable.
    QJsonObject style = doc.object();
    style.insert(QStringLiteral("name"), QStringLiteral("patched:") + id);
    reply(QStringLiteral("patchStyle"), std::move(cb), true, style);
}

void FakeCoreClient::listBasemaps(const QString&, CoreCallback cb)
{
    QJsonArray list;
    for (const auto& [id, entry] : m_basemaps) list.append(entry.value(QStringLiteral("descriptor")));
    reply(QStringLiteral("listBasemaps"), std::move(cb), true, QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("basemaps"), list}});
}

void FakeCoreClient::setBasemap(const QString& viewId, const QString& id, CoreCallback cb)
{
    m_viewBasemap[viewId] = id;
    reply(QStringLiteral("setBasemap"), std::move(cb), true, QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("viewId"), viewId}, {QStringLiteral("id"), id}});
}

void FakeCoreClient::setGroupVisible(const QString& viewId, const QString& session, const QString& groupId, bool visible, CoreCallback cb)
{
    // Mirrors maps_core: edit the view's hiddenGroups option, then relay
    // viewOptionsChanged and groupVisibilityChanged.
    const QString gid = groupId.contains(QLatin1Char(':')) ? groupId : session + QLatin1Char(':') + groupId;
    groupCalls.push_back({viewId, session, gid, visible});
    QStringList& hidden = m_hiddenGroups[viewId];
    if (visible) hidden.removeAll(gid);
    else if (!hidden.contains(gid)) hidden << gid;
    QJsonArray arr;
    for (const QString& h : hidden) arr.append(h);
    emitCoreEvent(QStringLiteral("viewOptionsChanged"),
                  QJsonObject{{QStringLiteral("viewId"), viewId}, {QStringLiteral("options"), QJsonObject{{QStringLiteral("hiddenGroups"), arr}}}});
    emitCoreEvent(QStringLiteral("groupVisibilityChanged"),
                  QJsonObject{{QStringLiteral("viewId"), viewId}, {QStringLiteral("session"), session}, {QStringLiteral("groupId"), gid}, {QStringLiteral("visible"), visible}});
    reply(QStringLiteral("setGroupVisible"), std::move(cb), true, true);
}

void FakeCoreClient::setFeatureState(const QString& session, const QString& sourceId, const QString& featureId, const QString& stateJson, CoreCallback cb)
{
    const QJsonObject state = QJsonDocument::fromJson(stateJson.toUtf8()).object();
    featureStates.push_back({session, sourceId, featureId, state});
    QJsonObject& current = m_featureState[sourceId + QLatin1Char('#') + featureId];
    for (auto it = state.begin(); it != state.end(); ++it) current.insert(it.key(), it.value());
    emitCoreEvent(QStringLiteral("featureStateChanged"),
                  QJsonObject{{QStringLiteral("session"), session}, {QStringLiteral("sourceId"), sourceId},
                              {QStringLiteral("featureId"), featureId}, {QStringLiteral("state"), current}});
    reply(QStringLiteral("setFeatureState"), std::move(cb), true, true);
}

void FakeCoreClient::listProviders(CoreCallback cb)
{
    reply(QStringLiteral("listProviders"), std::move(cb), true, QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("providers"), providers}});
}

void FakeCoreClient::getResource(const QString& module, const QString& path, qint64 rangeStart, qint64 rangeEnd,
                                 std::function<void(bool, const QByteArray&, const QString&, const QString&)> cb)
{
    calls << QStringLiteral("getResource");
    auto it = resources.find(module + QLatin1Char('/') + path);
    if (it == resources.end()) {
        QTimer::singleShot(m_latency, this, [cb] { cb(false, {}, QStringLiteral("notFound"), QStringLiteral("no such resource")); });
        return;
    }
    QByteArray bytes = it->second;
    if (rangeStart >= 0 && rangeEnd >= rangeStart) bytes = bytes.mid(static_cast<int>(rangeStart), static_cast<int>(rangeEnd - rangeStart + 1));
    QTimer::singleShot(m_latency, this, [cb, bytes] { cb(true, bytes, QString(), QString()); });
}

}  // namespace maps::test
