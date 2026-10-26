// Section 16.1 Engine contract (maps_view, offscreen).
//
// A headless ViewWindow (QT_QPA_PLATFORM=offscreen, QT_SCALE_FACTOR=2) over
// a FakeCoreClient whose single basemap is a memory:// raster style. Asserts:
//   - needsRendering -> updatePaintNode (one per frame, none when idle);
//   - resize changes the backing texture by the device pixel ratio;
//   - every MapChange code reaches the M11.3 mapping with the expected event;
//   - styleJson() round-trips the patched style;
//   - SceneApplier against the real Map: ordered apply, getLayerOrder-driven
//     insertion, ack, frameRendered, replay after a style reload (M6.8),
//     feature state (P2), image metadata (P4).
#include "fake_core_client.h"
#include "test_support.h"

#include "providers/memory_file_source.h"
#include "providers/provider_registry.h"
#include "view/camera_intents.h"
#include "view/capabilities.h"
#include "view/map_change.h"
#include "view/map_item.h"
#include "view/scene_applier.h"
#include "view/style_loader.h"
#include "view/view_controller.h"
#include "view/view_window.h"

#include <QMapLibre/Map>
#if __has_include(<QMapLibre/qmaplibre_patchset.h>)
#include <QMapLibre/qmaplibre_patchset.h>
#endif

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <memory>

using namespace maps::test;
using namespace maps::view;

namespace {

QJsonObject rasterStyle(const QString& tilesUrl)
{
    return QJsonObject{
        {QStringLiteral("version"), 8},
        {QStringLiteral("name"), QStringLiteral("memory raster")},
        {QStringLiteral("sources"), QJsonObject{{QStringLiteral("basemap"), QJsonObject{{QStringLiteral("type"), QStringLiteral("raster")},
                                                                                       {QStringLiteral("tiles"), QJsonArray{tilesUrl}},
                                                                                       {QStringLiteral("tileSize"), 256},
                                                                                       {QStringLiteral("minzoom"), 0},
                                                                                       {QStringLiteral("maxzoom"), 2}}}}},
        {QStringLiteral("layers"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("bg")}, {QStringLiteral("type"), QStringLiteral("background")},
                                                          {QStringLiteral("paint"), QJsonObject{{QStringLiteral("background-color"), QStringLiteral("#203040")}}}},
                                              QJsonObject{{QStringLiteral("id"), QStringLiteral("basemap")}, {QStringLiteral("type"), QStringLiteral("raster")},
                                                          {QStringLiteral("source"), QStringLiteral("basemap")}}}},
    };
}

QString json(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

QJsonObject berlinPoint()
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("FeatureCollection")},
                       {QStringLiteral("features"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("Feature")}, {QStringLiteral("id"), 7},
                                                                           {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Point")},
                                                                                                                    {QStringLiteral("coordinates"), QJsonArray{13.405, 52.52}}}},
                                                                           {QStringLiteral("properties"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Berlin")}}}}}}};
}

}  // namespace

int main(int argc, char** argv)
{
    // Headless by default; an explicit QT_QPA_PLATFORM (e.g. xcb against a
    // real display when debugging locally) wins. The scene graph must be
    // OpenGL: the engine renders into a GL texture (M11.2).
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    // The offscreen plugin does not advertise RhiBasedRendering, which makes
    // Qt Quick pick the software adaptation even when GLX works
    // (qsgcontextplugin.cpp); "rhi" forces the RHI path. Needs a GLX-capable
    // DISPLAY (Xvfb in the nix check).
    if (!qEnvironmentVariableIsSet("QT_QUICK_BACKEND")) qputenv("QT_QUICK_BACKEND", "rhi");
    qputenv("QT_SCALE_FACTOR", "2");
    // A fresh engine cache per run: the suite asserts that tiles reach the
    // provider, which a warm cache (correctly) prevents.
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    // (QStandardPaths test mode resolves CacheLocation under $HOME/.qttest.)
    qputenv("HOME", tmp.path().toLocal8Bit());
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);

    // ── memory:// tile store: z0-2 solid tiles ───────────────────────────
    auto memory = std::make_shared<MemoryFileSource>();
    for (int z = 0; z <= 2; ++z) {
        for (int x = 0; x < (1 << z); ++x) {
            for (int y = 0; y < (1 << z); ++y) {
                memory->put(QStringLiteral("memory://tiles/%1/%2/%3.png").arg(z).arg(x).arg(y), pngTile(256, QColor(40 * z + 60, 90, 120)));
            }
        }
    }
    const QJsonObject style = rasterStyle(QStringLiteral("memory://tiles/{z}/{x}/{y}.png"));
    memory->put(QStringLiteral("memory://style.json"), QJsonDocument(style).toJson(QJsonDocument::Compact));
    ProviderRegistry::instance().add(memory);
    CHECK(ProviderRegistry::instance().schemes().contains(QStringLiteral("memory")));

    // ── MapChange mapping table (M11.3) ──────────────────────────────────
    {
        using MC = QMapLibre::Map::MapChange;
        CHECK(kMapChangeCount == 16);
        for (int i = 0; i < kMapChangeCount; ++i) {
            const MapChangeMapping m = mapChangeMapping(static_cast<MC>(i));
            // Every code the engine can emit is classified; Internal is a
            // deliberate "no public event", None would be an unmapped code.
            CHECK_MSG(m.event != PublicEvent::None, mapChangeName(static_cast<MC>(i)));
            CHECK_MSG((m.event == PublicEvent::Internal || m.event == PublicEvent::Ignored) == (m.topic == nullptr),
                      mapChangeName(static_cast<MC>(i)));
        }
        CHECK(mapChangeMapping(MC::MapChangeWillStartRenderingFrame).event == PublicEvent::Internal);
        CHECK(mapChangeMapping(MC::MapChangeRegionIsChanging).event == PublicEvent::CameraChanged);
        CHECK(mapChangeMapping(MC::MapChangeRegionDidChange).event == PublicEvent::CameraIdle);
        CHECK(mapChangeMapping(MC::MapChangeRegionDidChangeAnimated).event == PublicEvent::CameraIdle);
        CHECK(mapChangeMapping(MC::MapChangeWillStartLoadingMap).event == PublicEvent::BasemapLoading);
        CHECK(mapChangeMapping(MC::MapChangeWillStartLoadingMap).opensBlockingWindow);
        CHECK(mapChangeMapping(MC::MapChangeDidFinishLoadingMap).event == PublicEvent::BasemapLoaded);
        CHECK(mapChangeMapping(MC::MapChangeDidFailLoadingMap).event == PublicEvent::BasemapFailed);
        CHECK(mapChangeMapping(MC::MapChangeDidFinishLoadingStyle).event == PublicEvent::StyleReady);
        CHECK(mapChangeMapping(MC::MapChangeDidFinishLoadingStyle).triggersReplay);
        CHECK(mapChangeMapping(MC::MapChangeDidFinishRenderingFrameFullyRendered).event == PublicEvent::FrameRendered);
        CHECK(mapChangeMapping(MC::MapChangeSourceDidChange).event == PublicEvent::Ignored);
        CHECK(QString::fromLatin1(mapChangeMapping(MC::MapChangeRegionDidChange).topic) == QLatin1String("cameraIdle"));
    }

    // ── capabilities report ──────────────────────────────────────────────
    {
        CapabilityInputs in;
        in.providerSchemes = ProviderRegistry::instance().schemes();
        const QJsonObject caps = capabilities(in);
        CHECK(caps.value(QStringLiteral("v")).toInt() == 1);
        CHECK(caps.value(QStringLiteral("patchSet")).toArray().contains(QStringLiteral("P7")));
        CHECK(caps.value(QStringLiteral("provider")).toObject().value(QStringLiteral("schemes")).toArray().contains(QStringLiteral("memory")));
        CHECK(caps.value(QStringLiteral("scene")).toObject().value(QStringLiteral("featureState")).toBool() == hasFeatureState());
    }

    // ── the view over a fake core ────────────────────────────────────────
    FakeCoreClient core;
    core.addBasemap(QStringLiteral("test/raster"), style);

    ViewController::Options options;
    options.session = QStringLiteral("t");
    options.qmlDir = QString::fromLocal8Bit(qgetenv("MAPS_VIEW_QML_DIR"));
    options.moduleDir = tmp.path();
    options.initialSize = QSize(256, 192);
    options.registerLogosProvider = false;
    ViewController view(&core, options);
    QStringList errors;
    QObject::connect(&view, &ViewController::error, [&](const QString& code, const QString& message) {
        errors << code + QStringLiteral(" ") + message;
        std::fprintf(stderr, "view error: %s %s\n", qPrintable(code), qPrintable(message));
    });
    view.start();
    REQUIRE(view.window() != nullptr);
    REQUIRE(view.mapItem() != nullptr);
    CHECK_MSG(!view.window()->usedFallback(), QStringLiteral("MapWindow.qml should have been found at ") + options.qmlDir);
    WAIT_FOR(view.mapItem()->hasMap());
    MapItem* item = view.mapItem();
    QMapLibre::Map* map = item->map();
    REQUIRE(map != nullptr);

    WAIT_FOR(view.status() == QLatin1String("open"));
    REQUIRE(core.opened.size() == 1);
    CHECK(core.opened[0].viewId == QStringLiteral("maps_view/t/1"));
    CHECK(core.opened[0].capabilities.value(QStringLiteral("v")).toInt() == 1);

    // Basemap: fetched from the fake, applied, loaded, rendered.
    WAIT_FOR(item->styleLoaded());
    WAIT_FOR(core.hasEvent(QStringLiteral("basemapLoaded")));
    CHECK(core.hasEvent(QStringLiteral("basemapLoading")));
    CHECK(core.hasEvent(QStringLiteral("styleReady")));
    CHECK(view.basemap() == QStringLiteral("test/raster"));
    WAIT_FOR(item->paintNodeUpdates() >= 1);
    WAIT_FOR(item->rendererCreations() == 1);
    WAIT_FOR(!memory->requested().isEmpty());
    CHECK_MSG(memory->requested().contains(QStringLiteral("memory://tiles/0/0/0.png")), memory->requested().join(QLatin1Char(',')));

    // styleJson() round trip: the raster descriptor style went through
    // unchanged (no patch step for synthesised raster styles).
    {
        const QJsonObject back = QJsonDocument::fromJson(map->styleJson().toUtf8()).object();
        CHECK(back.value(QStringLiteral("sources")).toObject().contains(QStringLiteral("basemap")));
        QStringList ids;
        for (const QJsonValue& l : back.value(QStringLiteral("layers")).toArray()) ids << l.toObject().value(QStringLiteral("id")).toString();
        CHECK_MSG((ids == QStringList{QStringLiteral("bg"), QStringLiteral("basemap")}), ids.join(QLatin1Char(',')));
    }

    // Pixel model (M5.5): the texture is item size x DPR (QT_SCALE_FACTOR=2).
    {
        WAIT_FOR(item->lastTextureSize().isValid());
        CHECK(item->devicePixelRatio() == 2.0);
        const QSize expected(static_cast<int>(item->width() * 2), static_cast<int>(item->height() * 2));
        WAIT_FOR(item->lastTextureSize() == expected);
        CHECK_MSG(item->lastTextureSize() == expected, QStringLiteral("%1x%2").arg(item->lastTextureSize().width()).arg(item->lastTextureSize().height()));

        view.setSize(320, 240, 2.0);
        WAIT_FOR(item->width() == 320 && item->height() == 240);
        WAIT_FOR(item->lastTextureSize() == QSize(640, 480));
        CHECK(item->rendererCreations() == 1);  // resize does not recreate the renderer
    }

    // Idle map renders zero frames; a repaint request renders one.
    {
        // Let the camera/idle events settle first.
        pump(400);
        WAIT_FOR_MS(core.hasEvent(QStringLiteral("cameraIdle")), 3000);
        const int before = item->paintNodeUpdates();
        pump(500);
        CHECK_MSG(item->paintNodeUpdates() == before, QStringLiteral("idle frames: %1").arg(item->paintNodeUpdates() - before));
        map->triggerRepaint();
        WAIT_FOR(item->paintNodeUpdates() > before);
        pump(300);
        CHECK_MSG(item->paintNodeUpdates() - before <= 3, QStringLiteral("frames after one repaint: %1").arg(item->paintNodeUpdates() - before));
    }

    // ── SceneApplier: publish, ack, frameRendered, order ─────────────────
    SceneApplier* applier = view.sceneApplier();
    REQUIRE(applier != nullptr);
    CHECK(applier->appliedRevision() == 0);
    {
        QJsonArray ops;
        ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("putSource")}, {QStringLiteral("id"), QStringLiteral("demo:pins")},
                               {QStringLiteral("type"), QStringLiteral("geojson")}, {QStringLiteral("def"), QJsonObject{}}});
        ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("replaceGeoJson")}, {QStringLiteral("sourceId"), QStringLiteral("demo:pins")},
                               {QStringLiteral("data"), berlinPoint()}, {QStringLiteral("digest"), QStringLiteral("x")}});
        ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("putImage")}, {QStringLiteral("id"), QStringLiteral("demo:pin")},
                               {QStringLiteral("pixelRatio"), 2.0}, {QStringLiteral("sdf"), true},
                               {QStringLiteral("bytes"), QString::fromLatin1(pngTile(32, Qt::white).toBase64())}});
        ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("putLayer")}, {QStringLiteral("id"), QStringLiteral("demo:dots")},
                               {QStringLiteral("type"), QStringLiteral("circle")}, {QStringLiteral("placement"), QJsonObject{{QStringLiteral("top"), true}}},
                               {QStringLiteral("source"), QStringLiteral("demo:pins")}, {QStringLiteral("layout"), QJsonObject{}},
                               {QStringLiteral("paint"), QJsonObject{{QStringLiteral("circle-radius"), 12}, {QStringLiteral("circle-color"), QStringLiteral("#ff0000")}}}});
        ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("putLayer")}, {QStringLiteral("id"), QStringLiteral("demo:icons")},
                               {QStringLiteral("type"), QStringLiteral("symbol")}, {QStringLiteral("placement"), QJsonObject{{QStringLiteral("top"), true}}},
                               {QStringLiteral("source"), QStringLiteral("demo:pins")},
                               {QStringLiteral("layout"), QJsonObject{{QStringLiteral("icon-image"), QStringLiteral("demo:pin")}}}, {QStringLiteral("paint"), QJsonObject{}}});
        const quint64 rev = core.publish(ops);
        CHECK(rev == 1);
        WAIT_FOR(applier->appliedRevision() == 1);
        CHECK(map->sourceExists(QStringLiteral("demo:pins")));
        CHECK(map->layerExists(QStringLiteral("demo:dots")));
        CHECK(map->layerExists(QStringLiteral("demo:icons")));
        CHECK(applier->layerOrder() == (QStringList{QStringLiteral("demo:dots"), QStringLiteral("demo:icons")}));
        // Core order drives engine order: scene layers sit above the basemap.
        const QStringList engineOrder = map->layerIds();
        CHECK_MSG(engineOrder.indexOf(QStringLiteral("basemap")) < engineOrder.indexOf(QStringLiteral("demo:dots")), engineOrder.join(QLatin1Char(',')));
        CHECK_MSG(engineOrder.indexOf(QStringLiteral("demo:dots")) < engineOrder.indexOf(QStringLiteral("demo:icons")), engineOrder.join(QLatin1Char(',')));
        WAIT_FOR(core.acked(1));
        // frameRendered carries the revision the frame reflects (the first
        // full frame after the style load reported revision 0).
        WAIT_FOR(core.lastEvent(QStringLiteral("frameRendered")).value(QStringLiteral("revision")).toDouble() == 1);
        CHECK(core.eventCount(QStringLiteral("frameRendered")) >= 2);
    }

    // Reorder: the core says icons below dots; the engine follows.
    {
        core.setLayerOrder({QStringLiteral("demo:icons"), QStringLiteral("demo:dots")});
        QJsonArray ops;
        ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("putLayer")}, {QStringLiteral("id"), QStringLiteral("demo:dots")},
                               {QStringLiteral("type"), QStringLiteral("circle")}, {QStringLiteral("placement"), QJsonObject{{QStringLiteral("top"), true}}},
                               {QStringLiteral("source"), QStringLiteral("demo:pins")}, {QStringLiteral("layout"), QJsonObject{}},
                               {QStringLiteral("paint"), QJsonObject{{QStringLiteral("circle-radius"), 8}, {QStringLiteral("circle-color"), QStringLiteral("#00ff00")}}}});
        core.publish(ops);
        WAIT_FOR(applier->appliedRevision() == 2);
        const QStringList engineOrder = map->layerIds();
        CHECK_MSG(engineOrder.indexOf(QStringLiteral("demo:icons")) < engineOrder.indexOf(QStringLiteral("demo:dots")), engineOrder.join(QLatin1Char(',')));
        WAIT_FOR(core.acked(2));
    }

    // Removal ops.
    {
        QJsonArray ops;
        ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("removeLayer")}, {QStringLiteral("id"), QStringLiteral("demo:icons")}});
        ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("removeImage")}, {QStringLiteral("id"), QStringLiteral("demo:pin")}});
        core.publish(ops);
        WAIT_FOR(applier->appliedRevision() == 3);
        CHECK(!map->layerExists(QStringLiteral("demo:icons")));
        CHECK(map->layerExists(QStringLiteral("demo:dots")));
        CHECK(applier->layerOrder() == QStringList{QStringLiteral("demo:dots")});
    }

    // ── Style reload replays the scene (M6.8) ────────────────────────────
    {
        QJsonObject style2 = style;
        style2.insert(QStringLiteral("name"), QStringLiteral("second"));
        core.addBasemap(QStringLiteral("test/raster2"), style2);
        const int sceneCalls = core.calls.count(QStringLiteral("getScene"));
        const int basemapLoaded = core.eventCount(QStringLiteral("basemapLoaded"));
        core.emitCoreEvent(QStringLiteral("basemapChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()}, {QStringLiteral("id"), QStringLiteral("test/raster2")}});
        WAIT_FOR(view.basemap() == QStringLiteral("test/raster2"));
        WAIT_FOR(core.eventCount(QStringLiteral("basemapLoaded")) > basemapLoaded);
        WAIT_FOR(core.calls.count(QStringLiteral("getScene")) > sceneCalls);
        WAIT_FOR(map->layerExists(QStringLiteral("demo:dots")));
        CHECK(map->sourceExists(QStringLiteral("demo:pins")));
        CHECK(!map->layerExists(QStringLiteral("demo:icons")));
        CHECK(QJsonDocument::fromJson(map->styleJson().toUtf8()).object().value(QStringLiteral("name")).toString() == QStringLiteral("second"));
        CHECK(!applier->blocked());
        // A revision published while the style was loading is not lost.
        core.publish(QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("removeLayer")}, {QStringLiteral("id"), QStringLiteral("demo:dots")}}});
        WAIT_FOR(applier->appliedRevision() == 4);
        CHECK(!map->layerExists(QStringLiteral("demo:dots")));
    }

    // ── mapbox-gl basemap: fetched through providers, patched by the core ─
    {
        core.addBasemap(QStringLiteral("test/vector"), QJsonObject{}, QStringLiteral("memory://style.json"));
        const int patched = static_cast<int>(core.patchedStyles.size());
        core.emitCoreEvent(QStringLiteral("basemapChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()}, {QStringLiteral("id"), QStringLiteral("test/vector")}});
        WAIT_FOR(static_cast<int>(core.patchedStyles.size()) > patched);
        WAIT_FOR(QJsonDocument::fromJson(map->styleJson().toUtf8()).object().value(QStringLiteral("name")).toString() == QStringLiteral("patched:test/vector"));
        CHECK(view.styleLoader()->lastStyleJson().contains(QStringLiteral("patched:test/vector")));
        CHECK(memory->requested().contains(QStringLiteral("memory://style.json")));
    }

    // A missing style leaves the previous one in place and reports failure.
    {
        core.addBasemap(QStringLiteral("test/missing"), QJsonObject{}, QStringLiteral("memory://nope.json"));
        const int failed = core.eventCount(QStringLiteral("basemapFailed"));
        core.emitCoreEvent(QStringLiteral("basemapChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()}, {QStringLiteral("id"), QStringLiteral("test/missing")}});
        WAIT_FOR(core.eventCount(QStringLiteral("basemapFailed")) > failed);
        CHECK(QJsonDocument::fromJson(map->styleJson().toUtf8()).object().value(QStringLiteral("name")).toString() == QStringLiteral("patched:test/vector"));
    }

    // ── camera intents and view state ────────────────────────────────────
    {
        view.camera(json(QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("type"), QStringLiteral("jumpTo")},
                                     {QStringLiteral("center"), QJsonArray{13.405, 52.52}}, {QStringLiteral("zoom"), 4}}));
        WAIT_FOR(qAbs(view.viewState().value(QStringLiteral("camera")).toObject().value(QStringLiteral("zoom")).toDouble() - 4.0) < 1e-6);
        const QJsonArray center = view.viewState().value(QStringLiteral("camera")).toObject().value(QStringLiteral("center")).toArray();
        CHECK(qAbs(center[0].toDouble() - 13.405) < 1e-6);
        CHECK(qAbs(center[1].toDouble() - 52.52) < 1e-6);
        WAIT_FOR(!core.viewStates.empty() && qAbs(core.viewStates.back().value(QStringLiteral("camera")).toObject().value(QStringLiteral("zoom")).toDouble() - 4.0) < 1e-6);
        CHECK(core.viewStates.back().value(QStringLiteral("size")).toArray() == (QJsonArray{320, 240}));
        CHECK(core.viewStates.back().value(QStringLiteral("basemap")).toString() == QStringLiteral("test/vector"));

        // fitBounds lands inside the bounds and the padding is honoured.
        view.setOptions(json(QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("padding"), QJsonObject{{QStringLiteral("left"), 40}}}}));
        CHECK(map->margins().left() == 40);
        view.camera(json(QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("type"), QStringLiteral("fitBounds")},
                                     {QStringLiteral("bounds"), QJsonArray{QJsonArray{10.0, 50.0}, QJsonArray{15.0, 54.0}}}}));
        WAIT_FOR(view.viewState().value(QStringLiteral("camera")).toObject().value(QStringLiteral("zoom")).toDouble() != 4.0);
        const QJsonArray c2 = view.viewState().value(QStringLiteral("camera")).toObject().value(QStringLiteral("center")).toArray();
        CHECK(c2[0].toDouble() > 10.0 && c2[0].toDouble() < 15.0);
        CHECK(c2[1].toDouble() > 50.0 && c2[1].toDouble() < 54.0);

        // A gesture in progress rejects intents unless forced (M7.4).
        view.setGestureInProgress(true);
        const int errs = errors.size();
        view.camera(json(QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("type"), QStringLiteral("jumpTo")}, {QStringLiteral("zoom"), 1}}));
        CHECK(errors.size() == errs + 1 && errors.last().startsWith(QStringLiteral("maps.gestureInProgress")));
        view.camera(json(QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("type"), QStringLiteral("jumpTo")}, {QStringLiteral("zoom"), 1}, {QStringLiteral("force"), true}}));
        CHECK(qAbs(map->zoom() - 1.0) < 1e-6);
        view.setGestureInProgress(false);
        CHECK(core.hasEvent(QStringLiteral("gestureBegan")) && core.hasEvent(QStringLiteral("gestureEnded")));

        // Bad intents are reported, not applied.
        view.camera(QStringLiteral("not json"));
        CHECK(errors.last().startsWith(QStringLiteral("maps.badArgument")));
    }

    // ── feature state (P2 or overlay fallback) ───────────────────────────
    {
        core.publish(QJsonArray{
            QJsonObject{{QStringLiteral("op"), QStringLiteral("putLayer")}, {QStringLiteral("id"), QStringLiteral("demo:dots")},
                        {QStringLiteral("type"), QStringLiteral("circle")}, {QStringLiteral("placement"), QJsonObject{{QStringLiteral("top"), true}}},
                        {QStringLiteral("source"), QStringLiteral("demo:pins")}, {QStringLiteral("layout"), QJsonObject{}},
                        {QStringLiteral("paint"), QJsonObject{{QStringLiteral("circle-radius"), 8}}}}});
        WAIT_FOR(applier->appliedRevision() == 5);
        int fallbacks = 0;
        QObject::connect(applier, &SceneApplier::featureStateFallback, [&](const QString&, const QString&, const QJsonValue&) { ++fallbacks; });
        core.emitCoreEvent(QStringLiteral("featureStateChanged"), QJsonObject{{QStringLiteral("session"), QStringLiteral("demo")}, {QStringLiteral("sourceId"), QStringLiteral("demo:pins")},
                                                                            {QStringLiteral("featureId"), QStringLiteral("7")}, {QStringLiteral("state"), QJsonObject{{QStringLiteral("hot"), true}}}});
        pump(200);
        if (hasFeatureState()) {
#ifdef MAPS_ENGINE_HAS_P2_FEATURE_STATE
            WAIT_FOR_MS(map->getFeatureState(QStringLiteral("demo:pins"), QString(), QStringLiteral("7")).value(QStringLiteral("hot")).toBool(), 3000);
#endif
            CHECK(fallbacks == 0);
        } else {
            CHECK(fallbacks == 1);
        }
    }

    // ── teardown ─────────────────────────────────────────────────────────
    view.stop();
    WAIT_FOR(core.closed.contains(QStringLiteral("maps_view/t/1")));
    CHECK(view.status() == QLatin1String("stopped"));
    CHECK_MSG(errors.size() <= 3, errors.join(QStringLiteral("\n")));  // the deliberate rejections above

    ProviderRegistry::instance().clear();
    return finish();
}
