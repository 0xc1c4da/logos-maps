#include "view/view_controller.h"

#include "providers/logos_file_source.h"
#include "providers/provider_registry.h"
#include "view/capabilities.h"
#include "view/map_change.h"
#include "view/map_item.h"
#include "view/scene_applier.h"
#include "view/style_loader.h"
#include "view/view_window.h"

#include <QMapLibre/Map>
#if __has_include(<QMapLibre/qmaplibre_patchset.h>)
#include <QMapLibre/qmaplibre_patchset.h>
#endif

#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickWindow>

namespace maps::view {

int ViewController::s_instances = 0;

namespace {
QString compact(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
}  // namespace

ViewController::ViewController(CoreClient* core, Options options, QObject* parent)
    : QObject(parent), m_core(core), m_options(std::move(options)), m_instance(++s_instances)
{
    m_surfaceToken = surfaceTokenFromProcess();
    m_engine = EngineConfig::defaults(m_options.moduleDir.isEmpty() ? ViewWindow::pluginDirectory() : m_options.moduleDir);
    EngineConfig::setProcessDefault(m_engine);

    // M7.5: view state is published at most once per frame.
    m_stateTimer.setSingleShot(true);
    m_stateTimer.setInterval(16);
    connect(&m_stateTimer, &QTimer::timeout, this, &ViewController::publishViewState);

    if (m_core) connect(m_core, &CoreClient::coreEvent, this, &ViewController::onCoreEvent);
}

ViewController::~ViewController()
{
    stop();
}

QString ViewController::surfaceTokenFromProcess()
{
    const QString name = QGuiApplication::desktopFileName();
    const QString prefix = QStringLiteral("logos-view-");
    if (name.startsWith(prefix)) return name.mid(prefix.size());
    const QByteArray env = qgetenv("LOGOS_VIEW_SURFACE_TOKEN");
    return QString::fromLocal8Bit(env);
}

quint64 ViewController::appliedRevision() const { return m_applier ? m_applier->appliedRevision() : 0; }

void ViewController::setStatus(const QString& status)
{
    if (m_status == status) return;
    m_status = status;
    emit statusChanged(status);
}

// ── start / stop ─────────────────────────────────────────────────────────

void ViewController::start()
{
    if (m_window) return;
    installProviders();

    CapabilityInputs inputs;
    inputs.transport = m_options.surfaceTransport;
    inputs.providerSchemes = ProviderRegistry::instance().schemes();
    inputs.providerRangeRequests = ProviderRegistry::instance().rangeRequests();
    m_capabilities = maps::view::capabilities(inputs);
    emit capabilitiesChanged(compact(m_capabilities));

    // The engine draws into a GL texture (M11.2): the scene graph has to run
    // on the OpenGL RHI backend. Only effective before the first QQuickWindow
    // exists; a host that already picked another API gets the MapItem
    // warning and an empty item instead.
    if (QQuickWindow::graphicsApi() != QSGRendererInterface::OpenGL) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    }

    m_window = std::make_unique<ViewWindow>();
    m_window->setTitle(QStringLiteral("Logos Maps"));
    m_window->resize(m_options.initialSize);
    m_mapItem = m_window->load(m_options.qmlDir);
    if (!m_mapItem) {
        setStatus(QStringLiteral("failed"));
        emit error(QStringLiteral("maps.internal"), QStringLiteral("could not create the map item"));
        return;
    }
    m_mapItem->setEngineConfig(m_engine);

    auto attach = [this] {
        QMapLibre::Map* map = m_mapItem->map();
        if (!map) return;
        m_applier = std::make_unique<SceneApplier>(map, m_core, this);
        m_styles = std::make_unique<StyleLoader>(map, m_core, this);
        connect(m_applier.get(), &SceneApplier::sceneError, this, &ViewController::error);
        connect(m_applier.get(), &SceneApplier::revisionApplied, this, [this](quint64) { refreshViewState(false); });
        connect(m_styles.get(), &StyleLoader::attributionChanged, this, &ViewController::attributionChanged);
        connect(m_styles.get(), &StyleLoader::basemapFailed, this, [this](const QString& id, const QString& reason) {
            emit error(QStringLiteral("maps.basemapFailed"), id + QStringLiteral(": ") + reason);
            // The previous style stays on screen; the view state reports what
            // is actually rendered, not what was asked for.
            if (m_basemap == id && m_styles->currentBasemap() != id) {
                m_basemap = m_styles->currentBasemap();
                refreshViewState(false);
            }
        });
        connect(m_styles.get(), &StyleLoader::styleApplied, this, [this](const QString& id) {
            m_basemap = id;
            emit basemapChanged(id);
            refreshViewState(false);
        });
        connect(m_mapItem, &MapItem::copyrightsChanged, m_styles.get(), &StyleLoader::setEngineCopyrights);
        connect(m_mapItem, &MapItem::mapChanged, this, &ViewController::onMapChange);
        connect(m_mapItem, &MapItem::mapLoadingFailed, this, [this](int, const QString& reason) {
            reportEvent(QStringLiteral("basemapFailed"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("id"), m_basemap}, {QStringLiteral("reason"), reason}});
        });
        connect(m_mapItem, &MapItem::viewportChanged, this, [this](int, int, double) { refreshViewState(false); });
        openView();
    };
    if (m_mapItem->hasMap()) {
        attach();
    } else {
        connect(m_mapItem, &MapItem::mapCreated, this, attach, Qt::SingleShotConnection);
        if (!m_options.showWindow) m_mapItem->createMapNow(m_options.initialSize, 1.0);
    }
    if (m_options.showWindow) m_window->show();
}

void ViewController::stop()
{
    m_stateTimer.stop();
    if (m_opened && m_core) {
        const QString id = m_viewId;
        m_core->closeView(id, [](const CoreReply&) {});
        m_opened = false;
    }
    m_applier.reset();
    m_styles.reset();
    m_mapItem = nullptr;
    m_window.reset();
    if (m_logosProvider) {
        ProviderRegistry::instance().remove(QStringLiteral("logos"));
        m_logosProvider.reset();
    }
    setStatus(QStringLiteral("stopped"));
}

void ViewController::installProviders()
{
    if (!m_options.registerLogosProvider || !m_core || m_logosProvider) return;
    CoreClient* core = m_core;
    m_logosProvider = std::make_shared<LogosFileSource>(
        [core](const QString& module, const QString& path, qint64 start, qint64 end, FetchResultCallback cb) {
            core->getResource(module, path, start, end, [cb](bool ok, const QByteArray& bytes, const QString& code, const QString& message) {
                FetchResult r;
                r.ok = ok;
                r.bytes = bytes;
                r.errorCode = code;
                r.errorMessage = message;
                cb(r);
            });
        });
    const QString glyphs = (m_options.moduleDir.isEmpty() ? ViewWindow::pluginDirectory() : m_options.moduleDir) + QStringLiteral("/assets/glyphs");
    if (QFileInfo(glyphs).isDir()) m_logosProvider->addLocalPrefix(QStringLiteral("maps"), QStringLiteral("glyphs/"), glyphs);
    ProviderRegistry::instance().add(m_logosProvider);
    m_core->listProviders([this](const CoreReply& reply) {
        if (reply.ok && m_logosProvider) m_logosProvider->setProviders(reply.object().value(QStringLiteral("providers")).toArray());
    });
}

// ── view-side protocol ───────────────────────────────────────────────────

void ViewController::openView()
{
    if (!m_core) {
        setStatus(QStringLiteral("standalone"));
        return;
    }
    m_viewId = QStringLiteral("%1/%2/%3").arg(m_options.moduleName, m_options.session).arg(m_instance);
    emit viewIdChanged(m_viewId);
    m_applier->setViewId(m_viewId);
    m_styles->setViewId(m_viewId);
    setStatus(QStringLiteral("opening"));
    m_core->openView(m_viewId, m_surfaceToken, compact(m_capabilities), [this](const CoreReply& reply) {
        if (!reply.ok) {
            setStatus(QStringLiteral("failed"));
            emit error(reply.code(), reply.error);
            return;
        }
        m_opened = true;
        const QJsonObject out = reply.object();
        m_limits = out.value(QStringLiteral("limits")).toObject();
        const QString basemap = out.value(QStringLiteral("basemap")).toString();
        setStatus(QStringLiteral("open"));
        if (basemap.isEmpty()) {
            chooseDefaultBasemap();
        } else {
            loadBasemap(basemap);
        }
        // Scene content published before we opened: the style load that
        // follows replays anyway, but a view without a style still needs it.
        m_applier->pullChanges();
        refreshViewState(true);
    });
}

void ViewController::chooseDefaultBasemap()
{
    m_core->listBasemaps(QStringLiteral("{\"v\":1}"), [this](const CoreReply& reply) {
        if (!reply.ok) {
            emit error(reply.code(), reply.error);
            return;
        }
        QJsonArray list = reply.value.isArray() ? reply.value.toArray() : reply.object().value(QStringLiteral("basemaps")).toArray();
        if (list.isEmpty()) {
            emit error(QStringLiteral("maps.unknownId"), QStringLiteral("no basemaps in the catalogue"));
            return;
        }
        const QString id = list.first().toObject().value(QStringLiteral("id")).toString();
        m_core->setBasemap(m_viewId, id, [](const CoreReply&) {});
        loadBasemap(id);
    });
}

void ViewController::loadBasemap(const QString& id)
{
    if (!m_styles) return;
    m_basemap = id;
    m_styles->load(id);
}

void ViewController::onCoreEvent(const QString& topic, const QJsonObject& payload)
{
    const QString forView = payload.value(QStringLiteral("viewId")).toString();
    const bool mine = forView.isEmpty() || forView == m_viewId;
    if (topic == QLatin1String("revisionPublished")) {
        if (m_applier) m_applier->onRevisionPublished(payload);
    } else if (topic == QLatin1String("basemapChanged")) {
        if (mine) loadBasemap(payload.value(QStringLiteral("id")).toString());
    } else if (topic == QLatin1String("cameraIntent")) {
        if (mine && m_mapItem && m_mapItem->map()) {
            const IntentResult r = applyCameraIntent(*m_mapItem->map(), payload.value(QStringLiteral("intent")).toObject(), viewportSize(), m_gestureInProgress);
            if (!r.accepted) emit error(r.error.section(QLatin1Char(':'), 0, 0), r.error);
            refreshViewState(!r.animated);
        }
    } else if (topic == QLatin1String("viewOptionsChanged")) {
        if (mine) applyOptions(payload.value(QStringLiteral("options")).toObject());
    } else if (topic == QLatin1String("featureStateChanged")) {
        if (m_applier) m_applier->onFeatureStateChanged(payload);
    } else if (topic == QLatin1String("cacheClearRequested")) {
        // The cache database is owned by the engine for the map's lifetime;
        // clearing means recreating it, which the next start does when the
        // file is gone. Report what we know.
        reportEvent(QStringLiteral("cacheStats"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("path"), m_engine.cachePath}, {QStringLiteral("cleared"), false}});
    }
    // trackRequested / snapshotRequested / catalogueChanged: M5 / later.
}

// ── map changes ──────────────────────────────────────────────────────────

void ViewController::onMapChange(int change)
{
    const auto mc = static_cast<QMapLibre::Map::MapChange>(change);
    const MapChangeMapping m = mapChangeMapping(mc);
    static const bool trace = qEnvironmentVariableIsSet("MAPS_VIEW_TRACE");
    if (trace) qDebug("maps_view: MapChange %s", qPrintable(mapChangeName(mc)));
    if (m.opensBlockingWindow && m_applier) m_applier->onStyleWillLoad();
    switch (m.event) {
    case PublicEvent::BasemapLoading:
        reportEvent(QStringLiteral("basemapLoading"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("id"), m_basemap}});
        break;
    case PublicEvent::BasemapLoaded:
        reportEvent(QStringLiteral("basemapLoaded"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("id"), m_basemap}});
        break;
    case PublicEvent::BasemapFailed:
        reportEvent(QStringLiteral("basemapFailed"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("id"), m_basemap}});
        break;
    case PublicEvent::StyleReady:
#ifdef MAPS_ENGINE_HAS_P10_PLACEMENT_TRANSITIONS
        // Every style load resets the transition options; without this the
        // orchestrator's stale-placement repaint keeps the map rendering
        // forever (engine/patches/README.md, P10). Idle must mean zero frames.
        if (m_mapItem && m_mapItem->map()) m_mapItem->map()->setPlacementTransitionsEnabled(false);
#endif
        if (m_applier && m.triggersReplay) m_applier->onStyleLoaded();
        reportEvent(QStringLiteral("styleReady"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("id"), m_basemap}});
        break;
    case PublicEvent::CameraChanged:
        refreshViewState(false);
        break;
    case PublicEvent::CameraIdle:
        refreshViewState(true);
        break;
    case PublicEvent::FrameRendered:
        if (m_applier) m_applier->onFrameFullyRendered();
        break;
    default:
        break;
    }
}

QSize ViewController::viewportSize() const
{
    if (m_mapItem) return QSize(static_cast<int>(m_mapItem->width()), static_cast<int>(m_mapItem->height()));
    return m_options.initialSize;
}

void ViewController::refreshViewState(bool idle)
{
    if (!m_mapItem || !m_mapItem->map()) return;
    QMapLibre::Map& map = *m_mapItem->map();
    const CameraState cam = CameraState::fromMap(map);
    const QSize size = viewportSize();
    QJsonObject state{
        {QStringLiteral("v"), 1},
        {QStringLiteral("camera"), cam.toJson()},
        {QStringLiteral("size"), QJsonArray{size.width(), size.height()}},
        {QStringLiteral("dpr"), m_mapItem->devicePixelRatio()},
        {QStringLiteral("basemap"), m_basemap},
        {QStringLiteral("gestureState"), QJsonObject{{QStringLiteral("active"), m_gestureInProgress}}},
        {QStringLiteral("appliedRevision"), static_cast<double>(appliedRevision())},
    };
    if (!m_options_.isEmpty()) state.insert(QStringLiteral("options"), m_options_);
    const bool cameraMoved = cam != m_lastCamera;
    m_lastCamera = cam;
    m_viewState = state;
    m_stateDirty = true;
    if (idle) {
        QJsonObject idlePayload = cam.toJson();
        idlePayload.insert(QStringLiteral("v"), 1);
        idlePayload.insert(QStringLiteral("bounds"), CameraState::boundsJson(map, size).value(QStringLiteral("bounds")));
        const QString json = compact(idlePayload);
        emit cameraIdle(json);
        reportEvent(QStringLiteral("cameraIdle"), idlePayload);
        publishViewState();
    } else {
        if (cameraMoved) {
            QJsonObject changed = cam.toJson();
            changed.insert(QStringLiteral("v"), 1);
            changed.insert(QStringLiteral("reason"), m_gestureInProgress ? QStringLiteral("gesture") : QStringLiteral("intent"));
            if (!m_stateTimer.isActive()) reportEvent(QStringLiteral("cameraChanged"), changed);
        }
        if (!m_stateTimer.isActive()) m_stateTimer.start();
    }
}

void ViewController::publishViewState()
{
    if (!m_stateDirty) return;
    m_stateDirty = false;
    const QString json = compact(m_viewState);
    emit viewStateChanged(json);
    if (m_opened && m_core) m_core->reportViewState(m_viewId, json, [](const CoreReply&) {});
}

void ViewController::reportEvent(const QString& topic, const QJsonObject& payload)
{
    if (!m_opened || !m_core) return;
    m_core->reportViewEvent(m_viewId, topic, compact(payload), [this](const CoreReply& reply) {
        if (!reply.ok && !reply.error.isEmpty()) emit error(reply.code(), reply.error);
    });
}

// ── host slots ───────────────────────────────────────────────────────────

void ViewController::setSize(int width, int height, double devicePixelRatio)
{
    Q_UNUSED(devicePixelRatio);  // the window's screen decides; recorded in view state
    if (!m_window || width <= 0 || height <= 0) return;
    m_window->resize(width, height);
}

void ViewController::setOptions(const QString& optionsJson)
{
    const QJsonDocument doc = QJsonDocument::fromJson(optionsJson.toUtf8());
    if (!doc.isObject()) {
        emit error(QStringLiteral("maps.badArgument"), QStringLiteral("options must be a JSON object"));
        return;
    }
    applyOptions(doc.object());
}

void ViewController::applyOptions(const QJsonObject& options)
{
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (it.key() == QLatin1String("v")) continue;
        m_options_.insert(it.key(), it.value());
    }
    m_engine.applyOptions(options);
    if (m_mapItem && m_mapItem->map() && options.contains(QStringLiteral("padding"))) {
        const QJsonObject p = options.value(QStringLiteral("padding")).toObject();
        m_mapItem->map()->setMargins(QMargins(p.value(QStringLiteral("left")).toInt(), p.value(QStringLiteral("top")).toInt(),
                                              p.value(QStringLiteral("right")).toInt(), p.value(QStringLiteral("bottom")).toInt()));
    }
    if (m_mapItem && options.contains(QStringLiteral("pixelRatio"))) m_mapItem->setPixelRatio(options.value(QStringLiteral("pixelRatio")).toDouble());
    refreshViewState(false);
}

void ViewController::camera(const QString& intentJson)
{
    if (!m_mapItem || !m_mapItem->map()) return;
    const QJsonDocument doc = QJsonDocument::fromJson(intentJson.toUtf8());
    if (!doc.isObject()) {
        emit error(QStringLiteral("maps.badArgument"), QStringLiteral("intent must be a JSON object"));
        return;
    }
    const IntentResult r = applyCameraIntent(*m_mapItem->map(), doc.object(), viewportSize(), m_gestureInProgress);
    if (!r.accepted) {
        emit error(r.error.section(QLatin1Char(':'), 0, 0), r.error);
        return;
    }
    refreshViewState(!r.animated);
}

void ViewController::setSession(const QString& session)
{
    if (session.isEmpty() || session == m_options.session) return;
    m_options.session = session;
    if (!m_window) return;  // start() will use it
    // Re-register under the new id: the core treats a re-open as a restart.
    if (m_opened && m_core) m_core->closeView(m_viewId, [](const CoreReply&) {});
    m_opened = false;
    if (m_applier) openView();
}

void ViewController::setGestureInProgress(bool inProgress)
{
    if (m_gestureInProgress == inProgress) return;
    m_gestureInProgress = inProgress;
    if (m_mapItem && m_mapItem->map()) m_mapItem->map()->setGestureInProgress(inProgress);
    reportEvent(inProgress ? QStringLiteral("gestureBegan") : QStringLiteral("gestureEnded"), QJsonObject{{QStringLiteral("v"), 1}});
    refreshViewState(!inProgress);
}

}  // namespace maps::view
