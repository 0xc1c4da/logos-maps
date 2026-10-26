#include "view/view_controller.h"

#include "input/gesture_controller.h"
#include "input/map_camera_target.h"
#include "input/picker.h"
#include "providers/logos_file_source.h"
#include "providers/provider_registry.h"
#include "view/builtins.h"
#include "view/selection_overlay.h"
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

#include <QMapLibre/Types>

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickWindow>
#include <QRectF>
#include <QUrl>
#include <QtMath>

#include <cmath>

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

    // The engine draws into a GL texture (M11.2): the scene graph has to run
    // on the OpenGL RHI backend. Only effective before the first QQuickWindow
    // exists — which is why it comes BEFORE the first LogosViewSurface::window()
    // call below (RFC-8 §2: the runtime creates its window lazily, on that
    // call). A host that already picked another API gets the MapItem warning
    // and an empty item instead.
    if (QQuickWindow::graphicsApi() != QSGRendererInterface::OpenGL) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    }

    // RFC-8: render into the window the runtime presents to the host over
    // its SurfaceTransport; without one (dev launcher, tests, a ui-host that
    // predates the API) open our own top-level. The transport the runtime
    // reports is what capabilities.surface.transport carries.
    QQuickWindow* hostWindow = ViewWindow::hostWindow();
    if (hostWindow) m_options.surfaceTransport = ViewWindow::hostTransport();

    CapabilityInputs inputs;
    inputs.transport = m_options.surfaceTransport;
    inputs.providerSchemes = ProviderRegistry::instance().schemes();
    inputs.providerRangeRequests = ProviderRegistry::instance().rangeRequests();
    m_capabilities = maps::view::capabilities(inputs);
    emit capabilitiesChanged(compact(m_capabilities));

    m_window = std::make_unique<ViewWindow>(hostWindow);
    m_window->setTitle(QStringLiteral("Logos Maps"));
    m_window->resize(m_options.initialSize);
    // The gesture controller is what MapWindow.qml's handler stacks talk
    // to; it exists before the QML loads and gets its camera once the map
    // is created.
    m_gestures = std::make_unique<GestureController>(this);
    m_window->setGestureController(m_gestures.get());
    m_window->setContextObject(QStringLiteral("view"), this);
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
        connect(m_styles.get(), &StyleLoader::attributionChanged, this, [this](const QString& html) {
            m_attributionHtml = html;
            emit attributionChanged(html);
        });
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
        attachInput(map);
        openView();
    };
    if (m_mapItem->hasMap()) {
        attach();
    } else {
        connect(m_mapItem, &MapItem::mapCreated, this, attach, Qt::SingleShotConnection);
        if (!m_options.showWindow) m_mapItem->createMapNow(m_options.initialSize, 1.0);
    }
    if (m_options.showWindow) m_window->show();  // own window only; the host shows its own
}

void ViewController::attachInput(QMapLibre::Map* map)
{
    m_cameraTarget = std::make_unique<MapCameraTarget>(map, m_mapItem);
    m_gestures->setCamera(m_cameraTarget.get());
    m_picker = std::make_unique<Picker>(map, this);
    m_builtins = std::make_unique<Builtins>(map, this);
    m_selection = std::make_unique<SelectionOverlay>(map, this);
    // M8.15: without native feature state the overlay mirrors selection and
    // hover; picks still report the state as if the engine held it.
    connect(m_applier.get(), &SceneApplier::featureStateFallback, this, [this](const QString& sourceId, const QString& featureId, const QJsonValue& state) {
        if (m_selection) m_selection->apply(sourceId, featureId, state);
    });
    connect(m_applier.get(), &SceneApplier::sourceUpdated, this, [this](const QString& sourceId) {
        if (m_selection) m_selection->onSourceUpdated(sourceId);
    });
    m_picker->setStateFallback(m_selection->sourceId(), [this](const QString& sourceId, const QString& featureId) {
        return m_selection ? m_selection->stateFor(sourceId, featureId) : QJsonObject();
    });

    // M8.5 / M7.5: the engine's freeze flag follows the controller; camera
    // idle is declared when every gesture and its inertia has ended.
    connect(m_gestures.get(), &GestureController::gestureInProgressChanged, this, [this](bool active) {
        if (m_gestureInProgress == active) return;
        m_gestureInProgress = active;
        refreshViewState(false);
    });
    connect(m_gestures.get(), &GestureController::idle, this, [this] { refreshViewState(true); });
    // M8.7 gesture kinds for follow-camera policies.
    connect(m_gestures.get(), &GestureController::gestureBegan, this, [this](const QString& kind) {
        reportEvent(QStringLiteral("gestureBegan"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("kind"), kind}});
    });
    connect(m_gestures.get(), &GestureController::gestureEnded, this, [this](const QString& kind) {
        reportEvent(QStringLiteral("gestureEnded"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("kind"), kind}});
    });
    // M8.12-M8.13: taps, long presses and context clicks are picks.
    connect(m_gestures.get(), &GestureController::tapped, this, [this](const QPointF& p) { pickAt(QStringLiteral("tap"), p); });
    connect(m_gestures.get(), &GestureController::longPressed, this, [this](const QPointF& p) { pickAt(QStringLiteral("longPress"), p); });
    connect(m_gestures.get(), &GestureController::contextRequested, this, [this](const QPointF& p) { pickAt(QStringLiteral("context"), p); });
    connect(m_gestures.get(), &GestureController::centrePickRequested, this, [this](bool longPress) {
        const QSize s = viewportSize();
        pickAt(longPress ? QStringLiteral("longPress") : QStringLiteral("tap"), QPointF(s.width() / 2.0, s.height() / 2.0));
    });
    connect(m_picker.get(), &Picker::picked, this, [this](const QJsonObject& payload) {
        emit featurePicked(compact(payload));
        reportEvent(QStringLiteral("featurePicked"), payload);
    });
    connect(m_picker.get(), &Picker::failed, this, &ViewController::error);

    // Built-ins ride above the scene (M6.11).
    connect(m_applier.get(), &SceneApplier::layersReconciled, this, [this](const QStringList&, const QStringList&) {
        // M8.13: the engine's style JSON is the loaded document, so runtime
        // layers are resolved from the applier's own table.
        if (m_picker && m_applier) {
            std::vector<Picker::SceneLayer> layers;
            for (const SceneApplier::LayerInfo& l : m_applier->layerInfos()) layers.push_back({l.id, l.source, l.sourceLayer, l.type});
            m_picker->setSceneLayers(std::move(layers));
        }
        if (m_builtins) m_builtins->onSceneLayersReconciled();
        if (m_selection) m_selection->onSceneLayersReconciled();
    });

    // Settings that arrived before the map existed.
    if (!m_options_.isEmpty()) applyOptions(m_options_);
}

void ViewController::pickAt(const QString& kind, const QPointF& px)
{
    if (!m_picker) {
        emit error(QStringLiteral("maps.styleNotLoaded"), QStringLiteral("pick before the view started"));
        return;
    }
    m_picker->pick(kind, px);
}

void ViewController::stop()
{
    m_stateTimer.stop();
    if (m_opened && m_core) {
        const QString id = m_viewId;
        m_core->closeView(id, [](const CoreReply&) {});
        m_opened = false;
    }
    if (m_gestures) m_gestures->setCamera(nullptr);
    m_picker.reset();
    m_builtins.reset();
    m_selection.reset();
    m_cameraTarget.reset();
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
    } else if (topic == QLatin1String("trackRequested")) {
        // M7.6: the session's tracked set is replaced wholesale; an empty
        // array stops tracking for that session.
        if (!mine) return;
        const QString session = payload.value(QStringLiteral("session")).toString();
        const QJsonArray tracked = payload.value(QStringLiteral("tracked")).toArray();
        if (tracked.isEmpty()) m_tracked.remove(session);
        else m_tracked.insert(session, tracked);
        publishTracked();
    } else if (topic == QLatin1String("snapshotRequested")) {
        if (mine) takeSnapshot(payload.value(QStringLiteral("requestId")).toString(), payload.value(QStringLiteral("options")).toObject());
    }
    // catalogueChanged: the catalogue is read on demand; nothing to do here.
}

// M7.4 / section 16.7: `snapshot(viewId, options)` on maps_core becomes a
// snapshotRequested event here and a snapshotReady event back, carrying a
// reference to the PNG rather than its bytes (`bytesRef`, a file: URL under
// the view's cache). Options: {"width","height"} (default: the viewport in
// physical pixels), {"path"} (where to write; default:
// <cache>/snapshots/<requestId>.png), {"settleMs": N} (cap on the wait for
// a fully rendered frame; default 3000, 0 = next frame as is).
//
// The image is the engine's own frame -- MapItem reads its texture back on
// the render thread -- not a grab of the host window, so it is the map and
// nothing else, at the item's pixel ratio, and it does not depend on how
// (or whether) the window is composited. The request waits for the next
// fully rendered frame so a scene still loading shows the scene, not the
// loading.
void ViewController::takeSnapshot(const QString& requestId, const QJsonObject& options)
{
    if (!m_window || !m_mapItem || !m_mapItem->map()) {
        snapshotFailed(requestId, QStringLiteral("maps.notReady"));
        return;
    }
    const int settleMs = options.contains(QStringLiteral("settleMs")) ? options.value(QStringLiteral("settleMs")).toInt() : 3000;
    if (settleMs > 0) {
        PendingSnapshot pending{requestId, options};
        pending.options.insert(QStringLiteral("settleMs"), 0);
        m_pendingSnapshots.push_back(std::move(pending));
        if (!m_snapshotTimer) {
            m_snapshotTimer = new QTimer(this);
            m_snapshotTimer->setSingleShot(true);
            connect(m_snapshotTimer, &QTimer::timeout, this, &ViewController::flushSnapshots);
        }
        m_snapshotTimer->start(settleMs);
        m_mapItem->update();  // ask for a frame; FrameRendered flushes earlier
        return;
    }
    if (!m_grabConnected) {
        m_grabConnected = true;
        connect(m_mapItem, &MapItem::frameGrabbed, this, &ViewController::onFrameGrabbed);
    }
    const quint64 token = ++m_grabToken;
    m_grabs.insert(token, PendingSnapshot{requestId, options});
    m_mapItem->grabNextFrame(token);
}

void ViewController::onFrameGrabbed(quint64 token, const QImage& grabbed)
{
    const auto it = m_grabs.find(token);
    if (it == m_grabs.end()) return;
    const PendingSnapshot snapshot = *it;
    m_grabs.erase(it);
    if (grabbed.isNull()) {
        snapshotFailed(snapshot.requestId, QStringLiteral("maps.renderFailed"));
        return;
    }
    QImage image = grabbed;
    const int w = snapshot.options.value(QStringLiteral("width")).toInt(0);
    const int h = snapshot.options.value(QStringLiteral("height")).toInt(0);
    if (w > 0 && h > 0 && QSize(w, h) != image.size())
        image = image.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QString path = snapshot.options.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        const QString dir = QFileInfo(m_engine.cachePath).absolutePath() + QStringLiteral("/snapshots");
        QDir().mkpath(dir);
        path = dir + QStringLiteral("/") + snapshot.requestId + QStringLiteral(".png");
    }
    if (!image.save(path, "PNG")) {
        snapshotFailed(snapshot.requestId, QStringLiteral("maps.writeFailed"));
        return;
    }
    reportEvent(QStringLiteral("snapshotReady"), QJsonObject{{QStringLiteral("v"), 1},
                                                              {QStringLiteral("requestId"), snapshot.requestId},
                                                              {QStringLiteral("bytesRef"), QUrl::fromLocalFile(path).toString()},
                                                              {QStringLiteral("width"), image.width()},
                                                              {QStringLiteral("height"), image.height()}});
}

void ViewController::snapshotFailed(const QString& requestId, const QString& reason)
{
    reportEvent(QStringLiteral("snapshotReady"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("requestId"), requestId}, {QStringLiteral("error"), reason}});
}

void ViewController::flushSnapshots()
{
    if (m_snapshotTimer) m_snapshotTimer->stop();
    const QList<PendingSnapshot> pending = std::move(m_pendingSnapshots);
    m_pendingSnapshots.clear();
    for (const PendingSnapshot& p : pending) takeSnapshot(p.requestId, p.options);
}

void ViewController::publishTracked()
{
    if (m_tracked.isEmpty() || !m_mapItem || !m_mapItem->map()) return;
    QMapLibre::Map& map = *m_mapItem->map();
    const QSize size = viewportSize();
    const QRectF visible(0, 0, size.width(), size.height());
    for (auto it = m_tracked.begin(); it != m_tracked.end(); ++it) {
        QJsonArray items;
        for (const QJsonValue& v : it.value().toArray()) {
            const QJsonObject t = v.toObject();
            const QJsonArray lonLat = t.value(QStringLiteral("lonLat")).toArray();
            if (lonLat.size() != 2) continue;
            const QPointF px = map.pixelForCoordinate(QMapLibre::Coordinate(lonLat[1].toDouble(), lonLat[0].toDouble()));
            items.append(QJsonObject{{QStringLiteral("id"), t.value(QStringLiteral("id"))},
                                     {QStringLiteral("px"), QJsonArray{px.x(), px.y()}},
                                     {QStringLiteral("visible"), visible.contains(px)}});
        }
        reportEvent(QStringLiteral("trackedMoved"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("session"), it.key()}, {QStringLiteral("items"), items}});
    }
}

double ViewController::metersPerPixelAtPaddedCentre(const QSize& size) const
{
    // M7.3: at the padded centre, 512 px tiles (M7.2).
    if (!m_mapItem || !m_mapItem->map()) return 0.0;
    QMapLibre::Map& map = *m_mapItem->map();
    const QMargins m = map.margins();
    const QPointF centre(m.left() + (size.width() - m.left() - m.right()) / 2.0, m.top() + (size.height() - m.top() - m.bottom()) / 2.0);
    const QMapLibre::Coordinate c = map.coordinateForPixel(centre);
    constexpr double kEarthCircumferenceM = 40075016.686;
    return kEarthCircumferenceM * std::cos(qDegreesToRadians(c.first)) / (512.0 * std::pow(2.0, map.zoom()));
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
        if (m_picker) m_picker->setStyleLoaded(false);
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
        if (m_builtins) m_builtins->onStyleLoaded();
        if (m_selection) m_selection->onStyleLoaded();
        if (m_picker) m_picker->setStyleLoaded(true);
        reportEvent(QStringLiteral("styleReady"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("id"), m_basemap}});
        break;
    case PublicEvent::CameraChanged:
        refreshViewState(false);
        break;
    case PublicEvent::CameraIdle:
        // The engine reports a region change after every immediate step; a
        // gesture (or its inertia) still running means the camera is not
        // idle yet -- GestureController::idle declares that (M7.5).
        refreshViewState(!(m_gestures && m_gestures->gestureInProgress()));
        break;
    case PublicEvent::FrameRendered:
        if (m_applier) m_applier->onFrameFullyRendered();
        if (!m_pendingSnapshots.isEmpty()) flushSnapshots();
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
    QJsonObject camera = cam.toJson();
    m_metersPerPixel = metersPerPixelAtPaddedCentre(size);
    camera.insert(QStringLiteral("metersPerPixel"), m_metersPerPixel);
    QJsonObject gestureState{{QStringLiteral("active"), m_gestureInProgress}};
    if (m_gestures) {
        gestureState.insert(QStringLiteral("frozen"), QJsonArray::fromStringList(m_gestures->frozenIds()));
        gestureState.insert(QStringLiteral("inertia"), m_gestures->inertiaActive());
    }
    QJsonObject state{
        {QStringLiteral("v"), 1},
        {QStringLiteral("camera"), camera},
        {QStringLiteral("size"), QJsonArray{size.width(), size.height()}},
        {QStringLiteral("dpr"), m_mapItem->devicePixelRatio()},
        {QStringLiteral("basemap"), m_basemap},
        {QStringLiteral("gestureState"), gestureState},
        {QStringLiteral("appliedRevision"), static_cast<double>(appliedRevision())},
    };
    if (m_gestures) state.insert(QStringLiteral("gestures"), m_gestures->settingsJson());
    if (!m_options_.isEmpty()) state.insert(QStringLiteral("options"), m_options_);
    const bool cameraMoved = cam != m_lastCamera;
    m_lastCamera = cam;
    m_viewState = state;
    m_stateDirty = true;
    if (cameraMoved) emit chromeChanged();
    if (cameraMoved && !m_tracked.isEmpty() && !m_stateTimer.isActive()) publishTracked();
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
    // A host window is sized by the host through the transport (RFC-5);
    // ViewWindow::resize is a no-op there and this slot only matters for
    // the dev launcher's own window.
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
    // M8.10 gesture settings, effective at once (M8.11).
    if (m_gestures) m_gestures->applyOptions(options);
    // M8.12 pick priority and box, from the session owner.
    if (m_picker) {
        if (options.contains(QStringLiteral("pickPriority"))) {
            QStringList priority;
            for (const QJsonValue& v : options.value(QStringLiteral("pickPriority")).toArray()) priority << v.toString();
            m_picker->setPriority(priority);
        }
        if (options.value(QStringLiteral("pickBoxDp")).isDouble()) m_picker->setBoxDp(options.value(QStringLiteral("pickBoxDp")).toDouble());
    }
    // M9.9: attribution can be moved, never removed.
    if (options.value(QStringLiteral("attributionCorner")).isString()) {
        static const QStringList corners{QStringLiteral("bottom-right"), QStringLiteral("bottom-left"), QStringLiteral("top-right"), QStringLiteral("top-left")};
        const QString c = options.value(QStringLiteral("attributionCorner")).toString();
        if (corners.contains(c) && c != m_attributionCorner) {
            m_attributionCorner = c;
            emit chromeChanged();
        }
    }
    // M6.11 puck.
    if (m_builtins) {
        if (options.contains(QStringLiteral("location"))) m_builtins->setLocation(options.value(QStringLiteral("location")));
        if (options.value(QStringLiteral("puck")).isObject()) m_builtins->setPuckOptions(options.value(QStringLiteral("puck")).toObject());
    }
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
    // An external gesture (host chrome driving the camera) is one more
    // frozen id in the controller's ref-count (M8.5).
    if (m_gestures) {
        const bool was = m_gestures->frozenIds().contains(QStringLiteral("host"));
        if (inProgress == was) return;
        reportEvent(inProgress ? QStringLiteral("gestureBegan") : QStringLiteral("gestureEnded"),
                    QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("kind"), QStringLiteral("host")}});
        if (inProgress) m_gestures->freeze(QStringLiteral("host"));
        else m_gestures->thaw(QStringLiteral("host"));
        return;
    }
    if (m_gestureInProgress == inProgress) return;
    m_gestureInProgress = inProgress;
    if (m_mapItem && m_mapItem->map()) m_mapItem->map()->setGestureInProgress(inProgress);
    refreshViewState(!inProgress);
}

bool ViewController::compassVisible() const
{
    // A compass whose visibility follows bearing != 0 (M6.11); under north
    // lock the bearing is pinned so there is nothing to show.
    return std::fabs(m_lastCamera.bearing) > gesture::kNorthLockSlackDeg;
}

void ViewController::resetNorth()
{
    if (!m_mapItem || !m_mapItem->map()) return;
    QMapLibre::CameraOptions camera;
    camera.bearing = 0.0;
    m_mapItem->map()->easeTo(camera, QMapLibre::AnimationOptions(300));
    refreshViewState(false);
}

}  // namespace maps::view
