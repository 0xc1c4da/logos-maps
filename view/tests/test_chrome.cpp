// Section 16.8 Chrome and groups (maps_view, offscreen).
//
// A headless ViewWindow over a FakeCoreClient with a background-only
// basemap (plus a style layer for `details`) and a synthetic scene with a
// group, a callout-bearing layer and a selectable layer. Asserts:
//   - theme: neutral light defaults; the `theme` option is reflected (M8.23);
//   - chrome items: defaults (zoom buttons / readout off), `chrome` option
//     toggles, unknown keys ignored (M8.17);
//   - follow: `follow` + `location` eases the camera to the fix, a pan
//     gesture ends it with followChanged{reason: gesture}, a wheel zoom
//     does not, an intent ends it unless keepFollow, followHeading sets the
//     bearing, Escape ends it (M8.18, M7.4b, M8.22);
//   - zoom buttons: +1 / -1 about the centre, disabled at the limits (M8.17);
//   - groups: the panel model lists groups in order with their layers;
//     hiddenGroups (option / setGroupVisible / groupVisibilityChanged) hides
//     member layers in this view only, surviving a style reload (M6.3a, M8.20);
//   - details: the descriptor's details are listed and toggling hides the
//     prefixed style layers (M9.1a);
//   - callout: a tap on a callout layer opens the card with {prop} templates
//     expanded, reports calloutOpened, selected state goes through the
//     core, actions report and close, Escape closes (M8.19, M8.15a);
//   - pin: a long-press with no hit drops the pin, opens the coordinate
//     callout, reports pinDropped; Escape clears it (M8.21, M8.22);
//   - status chip: basemapFailed shows an error chip (M8.17).
#include "fake_core_client.h"
#include "test_support.h"

#include "input/gesture_controller.h"
#include "input/picker.h"
#include "view/chrome_controller.h"
#include "view/map_item.h"
#include "view/scene_applier.h"
#include "view/view_controller.h"
#include "view/view_window.h"

#include <QMapLibre/Map>

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cmath>

using namespace maps::test;
using namespace maps::view;

namespace {

QJsonObject backgroundStyle()
{
    // A background plus two "detail" style layers (never visible: empty
    // source) whose ids share the prefix the descriptor's details name.
    return QJsonObject{
        {QStringLiteral("version"), 8},
        {QStringLiteral("name"), QStringLiteral("background")},
        {QStringLiteral("sources"), QJsonObject{{QStringLiteral("empty"), QJsonObject{{QStringLiteral("type"), QStringLiteral("geojson")},
                                                                                       {QStringLiteral("data"), QJsonObject{{QStringLiteral("type"), QStringLiteral("FeatureCollection")}, {QStringLiteral("features"), QJsonArray{}}}}}}}},
        {QStringLiteral("layers"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("bg")}, {QStringLiteral("type"), QStringLiteral("background")},
                        {QStringLiteral("paint"), QJsonObject{{QStringLiteral("background-color"), QStringLiteral("#203040")}}}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("label-city")}, {QStringLiteral("type"), QStringLiteral("circle")}, {QStringLiteral("source"), QStringLiteral("empty")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("label-town")}, {QStringLiteral("type"), QStringLiteral("circle")}, {QStringLiteral("source"), QStringLiteral("empty")}},
        }},
    };
}

QJsonObject point(int id, const QString& name, const QMapLibre::Coordinate& c)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("Feature")}, {QStringLiteral("id"), id},
                       {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Point")}, {QStringLiteral("coordinates"), QJsonArray{c.second, c.first}}}},
                       {QStringLiteral("properties"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("kind"), QStringLiteral("cafe")}}}};
}

QJsonObject layerOp(const QString& id, const QString& source, const QJsonObject& extra = QJsonObject())
{
    QJsonObject op{{QStringLiteral("op"), QStringLiteral("putLayer")}, {QStringLiteral("id"), id}, {QStringLiteral("type"), QStringLiteral("circle")},
                   {QStringLiteral("placement"), QJsonObject{{QStringLiteral("top"), true}}}, {QStringLiteral("source"), source},
                   {QStringLiteral("layout"), QJsonObject{}},
                   {QStringLiteral("paint"), QJsonObject{{QStringLiteral("circle-radius"), 3}, {QStringLiteral("circle-color"), QStringLiteral("#ff0000")}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it) op.insert(it.key(), it.value());
    return op;
}

QStringList layersHit(const QJsonObject& pick)
{
    QStringList out;
    for (const QJsonValue& h : pick.value(QStringLiteral("hits")).toArray()) out << h.toObject().value(QStringLiteral("layerId")).toString();
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    if (!qEnvironmentVariableIsSet("QT_QUICK_BACKEND")) qputenv("QT_QUICK_BACKEND", "rhi");
    qputenv("QT_SCALE_FACTOR", "1");
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    qputenv("HOME", tmp.path().toLocal8Bit());
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);

    FakeCoreClient core;
    core.addBasemap(QStringLiteral("test/bg"), backgroundStyle());
    core.setDescriptorExtras(QStringLiteral("test/bg"),
                             QJsonObject{{QStringLiteral("thumbnail"), QStringLiteral("memory://thumb.png")},
                                         {QStringLiteral("details"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("labels")},
                                                                                            {QStringLiteral("title"), QStringLiteral("Labels")},
                                                                                            {QStringLiteral("layerPrefixes"), QJsonArray{QStringLiteral("label-")}},
                                                                                            {QStringLiteral("defaultVisible"), true}}}}});
    core.addBasemap(QStringLiteral("test/alt"), backgroundStyle());

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
    ChromeController* chrome = view.chrome();
    REQUIRE(chrome != nullptr);

    // ── theme and items before the window exists (M8.23, M8.17) ─────────
    CHECK(chrome->theme() == ChromeController::defaultTheme());
    CHECK(chrome->theme().value(QStringLiteral("dark")).toBool() == false);
    CHECK(chrome->items() == ChromeController::defaultItems());
    CHECK(chrome->itemEnabled(QStringLiteral("scaleBar")) && chrome->itemEnabled(QStringLiteral("compass")) && chrome->itemEnabled(QStringLiteral("attribution")));
    CHECK(!chrome->itemEnabled(QStringLiteral("zoomButtons")) && !chrome->itemEnabled(QStringLiteral("coordinateReadout")));
    view.setOptions(QStringLiteral(R"({"v":1,"theme":{"dark":true,"accent":"#ff8800","foreground":"#eeeeee","background":"#101010","fontPixelSize":15},
                                       "chrome":{"zoomButtons":true,"coordinateReadout":true,"bogus":true,"compass":"no"}})"));
    CHECK(chrome->theme().value(QStringLiteral("dark")).toBool() == true);
    CHECK(chrome->theme().value(QStringLiteral("accent")).toString() == QStringLiteral("#ff8800"));
    CHECK(chrome->theme().value(QStringLiteral("fontPixelSize")).toInt() == 15);
    CHECK(chrome->theme().value(QStringLiteral("surface")).toString() == ChromeController::defaultTheme().value(QStringLiteral("surface")).toString());  // unspecified keys keep defaults
    CHECK(chrome->itemEnabled(QStringLiteral("zoomButtons")) && chrome->itemEnabled(QStringLiteral("coordinateReadout")));
    CHECK(chrome->itemEnabled(QStringLiteral("compass")));  // non-boolean ignored
    CHECK(!chrome->items().contains(QStringLiteral("bogus")));

    view.start();
    REQUIRE(view.window() != nullptr);
    WAIT_FOR(view.mapItem()->hasMap());
    MapItem* item = view.mapItem();
    QMapLibre::Map* map = item->map();
    REQUIRE(map != nullptr);
    WAIT_FOR(view.status() == QLatin1String("open"));
    WAIT_FOR(item->styleLoaded());
    WAIT_FOR(core.hasEvent(QStringLiteral("styleReady")));
    REQUIRE(view.picker() != nullptr);
    REQUIRE(view.gestures() != nullptr);
    SceneApplier* applier = view.sceneApplier();
    REQUIRE(applier != nullptr);
    GestureController* gestures = view.gestures();

    // The window's chrome items exist and read the context object.
    QQuickItem* rootItem = view.window()->rootObject();
    REQUIRE(rootItem != nullptr);
    CHECK(rootItem->findChild<QObject*>(QStringLiteral("zoomButtons")) != nullptr);
    CHECK(rootItem->findChild<QObject*>(QStringLiteral("locateButton")) != nullptr);
    CHECK(rootItem->findChild<QObject*>(QStringLiteral("layersButton")) != nullptr);
    CHECK(rootItem->findChild<QObject*>(QStringLiteral("callout")) != nullptr);
    CHECK(rootItem->findChild<QObject*>(QStringLiteral("layersPanel")) != nullptr);
    CHECK(rootItem->findChild<QObject*>(QStringLiteral("statusChip")) != nullptr);
    CHECK(rootItem->findChild<QObject*>(QStringLiteral("coordinateReadout")) != nullptr);

    // The view advertises the chrome (section 11).
    {
        const QJsonObject chromeCaps = view.capabilities().value(QStringLiteral("chrome")).toObject();
        CHECK(chromeCaps.value(QStringLiteral("follow")).toBool() && chromeCaps.value(QStringLiteral("pin")).toBool() && chromeCaps.value(QStringLiteral("theme")).toBool());
        CHECK(chromeCaps.value(QStringLiteral("items")).toArray().contains(QStringLiteral("zoomButtons")));
        CHECK(view.capabilities().value(QStringLiteral("scene")).toObject().value(QStringLiteral("groups")).toBool());
        CHECK(view.capabilities().value(QStringLiteral("scene")).toObject().value(QStringLiteral("callouts")).toBool());
    }

    // ── camera at a known place ──────────────────────────────────────────
    QMapLibre::CameraOptions cam;
    cam.center = QVariant::fromValue(QMapLibre::Coordinate(52.52, 13.405));
    cam.zoom = 14.0;
    cam.bearing = 0.0;
    map->jumpTo(cam);
    pump(200);
    const QPointF centre(128, 96);
    auto followEvents = [&] { return core.eventCount(QStringLiteral("followChanged")); };

    // ── follow (M8.18) ───────────────────────────────────────────────────
    {
        CHECK(chrome->followMode() == QStringLiteral("off"));
        CHECK(!chrome->locationAvailable());
        view.setOptions(QStringLiteral(R"({"v":1,"location":{"lon":13.5,"lat":52.6,"accuracy":10,"heading":90}})"));
        CHECK(chrome->locationAvailable() && chrome->headingAvailable());
        // No follow yet: the camera stays.
        pump(50);
        CHECK(std::fabs(map->longitude() - 13.405) < 1e-6);

        const int before = followEvents();
        chrome->cycleFollow();
        CHECK(chrome->followMode() == QStringLiteral("follow"));
        WAIT_FOR(followEvents() == before + 1);
        CHECK(core.lastEvent(QStringLiteral("followChanged")).value(QStringLiteral("mode")).toString() == QStringLiteral("follow"));
        CHECK(core.lastEvent(QStringLiteral("followChanged")).value(QStringLiteral("reason")).toString() == QStringLiteral("user"));
        // Eases to the fix, keeping zoom and bearing.
        WAIT_FOR_MS(std::fabs(map->longitude() - 13.5) < 1e-4 && std::fabs(map->latitude() - 52.6) < 1e-4, 3000);
        CHECK(std::fabs(map->zoom() - 14.0) < 1e-6);
        CHECK(std::fabs(map->bearing()) < 1e-6);

        // Next fix: eased again.
        view.setOptions(QStringLiteral(R"({"v":1,"location":{"lon":13.51,"lat":52.61,"accuracy":10,"heading":90}})"));
        WAIT_FOR_MS(std::fabs(map->longitude() - 13.51) < 1e-4, 3000);

        // followHeading sets the bearing to the heading.
        chrome->cycleFollow();
        CHECK(chrome->followMode() == QStringLiteral("followHeading"));
        WAIT_FOR_MS(std::fabs(map->bearing() - 90.0) < 0.5, 3000);

        // A zoom-only gesture (wheel) keeps follow.
        gestures->wheel(centre, 120, 0, 0);
        pump(100);
        CHECK(chrome->followMode() == QStringLiteral("followHeading"));

        // A pan ends it, reason gesture.
        const int beforePan = followEvents();
        gestures->pointerDragBegan(centre, Qt::LeftButton, 0);
        gestures->pointerDragMoved(centre + QPointF(20, 10));
        gestures->pointerDragEnded(centre + QPointF(20, 10));
        CHECK(chrome->followMode() == QStringLiteral("off"));
        WAIT_FOR(followEvents() == beforePan + 1);
        CHECK(core.lastEvent(QStringLiteral("followChanged")).value(QStringLiteral("reason")).toString() == QStringLiteral("gesture"));
        gestures->settle();

        // The follow option drives it (reason option); a fix lost keeps the mode.
        view.setOptions(QStringLiteral(R"({"v":1,"follow":"follow"})"));
        CHECK(chrome->followMode() == QStringLiteral("follow"));
        WAIT_FOR(core.lastEvent(QStringLiteral("followChanged")).value(QStringLiteral("reason")).toString() == QStringLiteral("option"));
        view.setOptions(QStringLiteral(R"({"v":1,"location":null})"));
        CHECK(!chrome->locationAvailable());
        CHECK(chrome->followMode() == QStringLiteral("follow"));

        // An intent ends follow unless keepFollow (M7.4b).
        view.camera(QStringLiteral(R"({"v":1,"type":"jumpTo","center":[13.4,52.5],"keepFollow":true})"));
        CHECK(chrome->followMode() == QStringLiteral("follow"));
        const int beforeIntent = followEvents();
        view.camera(QStringLiteral(R"({"v":1,"type":"jumpTo","center":[13.41,52.51]})"));
        CHECK(chrome->followMode() == QStringLiteral("off"));
        WAIT_FOR(followEvents() == beforeIntent + 1);
        CHECK(core.lastEvent(QStringLiteral("followChanged")).value(QStringLiteral("reason")).toString() == QStringLiteral("intent"));

        // Also from the core's cameraIntent event.
        chrome->setFollowMode(QStringLiteral("follow"));
        core.emitCoreEvent(QStringLiteral("cameraIntent"), QJsonObject{{QStringLiteral("viewId"), view.viewId()},
                                                                       {QStringLiteral("intent"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("type"), QStringLiteral("jumpTo")}, {QStringLiteral("center"), QJsonArray{13.405, 52.52}}}}});
        WAIT_FOR(chrome->followMode() == QStringLiteral("off"));

        // Escape ends follow when nothing else is open (M8.22).
        chrome->setFollowMode(QStringLiteral("follow"));
        CHECK(chrome->escape() == QStringLiteral("follow"));
        CHECK(chrome->followMode() == QStringLiteral("off"));
        CHECK(chrome->escape().isEmpty());
    }

    // ── zoom buttons (M8.17) ─────────────────────────────────────────────
    {
        map->jumpTo(cam);
        pump(100);
        const double z0 = map->zoom();
        CHECK(chrome->canZoomIn() && chrome->canZoomOut());
        chrome->zoomIn();
        WAIT_FOR_MS(std::fabs(map->zoom() - (z0 + 1.0)) < 1e-3, 3000);
        chrome->zoomOut();
        WAIT_FOR_MS(std::fabs(map->zoom() - z0) < 1e-3, 3000);
        // Disabled at the limit.
        gestures->setMaxZoom(z0 + 0.5);
        gestures->setMinZoom(z0 - 0.5);
        CHECK(chrome->canZoomIn());  // a step is still possible (clamped)
        chrome->zoomIn();
        WAIT_FOR_MS(std::fabs(map->zoom() - (z0 + 0.5)) < 1e-3, 3000);
        CHECK(!chrome->canZoomIn());
        CHECK(chrome->canZoomOut());
        gestures->setMaxZoom(gesture::kMaxZoom);
        gestures->setMinZoom(0.0);
        map->jumpTo(cam);
        pump(100);
    }

    // ── a scene with a group, a callout layer and a selectable layer ─────
    const QMapLibre::Coordinate atCentre = map->coordinateForPixel(centre);
    const QMapLibre::Coordinate farOff = map->coordinateForPixel(centre + QPointF(60, 40));
    QJsonArray ops;
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("putSource")}, {QStringLiteral("id"), QStringLiteral("demo:pois")},
                           {QStringLiteral("type"), QStringLiteral("geojson")}, {QStringLiteral("def"), QJsonObject{}}});
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("replaceGeoJson")}, {QStringLiteral("sourceId"), QStringLiteral("demo:pois")},
                           {QStringLiteral("data"), QJsonObject{{QStringLiteral("type"), QStringLiteral("FeatureCollection")},
                                                                {QStringLiteral("features"), QJsonArray{point(1, QStringLiteral("Café Central"), atCentre), point(2, QStringLiteral("Far"), farOff)}}}},
                           {QStringLiteral("digest"), QStringLiteral("p1")}});
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("putGroup")}, {QStringLiteral("id"), QStringLiteral("demo:food")},
                           {QStringLiteral("title"), QStringLiteral("Food")}, {QStringLiteral("order"), 2},
                           {QStringLiteral("defaultVisible"), true}, {QStringLiteral("toggleable"), true},
                           {QStringLiteral("legend"), QJsonArray{QJsonObject{{QStringLiteral("label"), QStringLiteral("Cafe")}, {QStringLiteral("color"), QStringLiteral("#ff0000")}}}}});
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("putGroup")}, {QStringLiteral("id"), QStringLiteral("demo:admin")},
                           {QStringLiteral("title"), QStringLiteral("Admin")}, {QStringLiteral("order"), 1},
                           {QStringLiteral("defaultVisible"), true}, {QStringLiteral("toggleable"), false}});
    ops.append(layerOp(QStringLiteral("demo:cafes"), QStringLiteral("demo:pois"),
                       QJsonObject{{QStringLiteral("group"), QStringLiteral("demo:food")},
                                   {QStringLiteral("selectable"), true},
                                   {QStringLiteral("callout"), QJsonObject{{QStringLiteral("title"), QStringLiteral("{name}")},
                                                                           {QStringLiteral("subtitle"), QStringLiteral("A {kind} nearby")},
                                                                           {QStringLiteral("actions"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("open")}, {QStringLiteral("label"), QStringLiteral("Open {name}")}},
                                                                                                                  QJsonObject{{QStringLiteral("id"), QStringLiteral("route")}, {QStringLiteral("label"), QStringLiteral("Route")}}}}}}}));
    ops.append(layerOp(QStringLiteral("demo:plain"), QStringLiteral("demo:pois")));
    const quint64 rev = core.publish(ops);
    WAIT_FOR(applier->appliedRevision() == rev);
    CHECK(map->layerExists(QStringLiteral("demo:cafes")) && map->layerExists(QStringLiteral("demo:plain")));
    WAIT_FOR(core.lastEvent(QStringLiteral("frameRendered")).value(QStringLiteral("revision")).toDouble() == static_cast<double>(rev));
    pump(200);

    // ── groups model (M8.20) ─────────────────────────────────────────────
    {
        WAIT_FOR(chrome->groups().size() == 2);
        const QVariantList groups = chrome->groups();
        CHECK(groups[0].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("demo:admin"));  // order 1 first
        CHECK(groups[1].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("demo:food"));
        CHECK(groups[1].toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Food"));
        CHECK(groups[1].toMap().value(QStringLiteral("visible")).toBool());
        CHECK(groups[1].toMap().value(QStringLiteral("layers")).toStringList() == QStringList{QStringLiteral("demo:cafes")});
        CHECK(groups[1].toMap().value(QStringLiteral("legend")).toList().size() == 1);
        CHECK(groups[0].toMap().value(QStringLiteral("toggleable")).toBool() == false);
        CHECK(groups[1].toMap().value(QStringLiteral("session")).toString() == QStringLiteral("demo"));
        CHECK(chrome->layersAvailable());
        const auto info = applier->layerInfo(QStringLiteral("demo:cafes"));
        REQUIRE(info.has_value());
        CHECK(info->group == QStringLiteral("demo:food") && info->selectable && !info->callout.isEmpty());
    }

    auto pickNow = [&](const QString& kind, const QPointF& px) {
        QString code;
        auto p = view.picker()->pickNow(kind, px, &code);
        CHECK_MSG(p.has_value(), code);
        return p.value_or(QJsonObject());
    };
    // Both layers draw feature 1 at the centre.
    WAIT_FOR_MS(layersHit(pickNow(QStringLiteral("tap"), centre)).contains(QStringLiteral("demo:cafes")), 5000);
    CHECK(layersHit(pickNow(QStringLiteral("tap"), centre)).contains(QStringLiteral("demo:plain")));

    // ── hiding a group hides its layers in this view (M6.3a) ────────────
    {
        chrome->setGroupVisible(QStringLiteral("demo:food"), false);
        CHECK(!applier->layerVisible(QStringLiteral("demo:cafes")));
        CHECK(applier->layerVisible(QStringLiteral("demo:plain")));
        CHECK(chrome->hiddenGroups() == QStringList{QStringLiteral("demo:food")});
        // The request went to the core with the owning session.
        WAIT_FOR(!core.groupCalls.empty());
        CHECK(core.groupCalls.back().session == QStringLiteral("demo") && core.groupCalls.back().groupId == QStringLiteral("demo:food") && !core.groupCalls.back().visible);
        CHECK(core.groupCalls.back().viewId == view.viewId());
        pump(300);
        QStringList hit = layersHit(pickNow(QStringLiteral("tap"), centre));
        CHECK_MSG(!hit.contains(QStringLiteral("demo:cafes")) && hit.contains(QStringLiteral("demo:plain")), hit.join(QLatin1Char(',')));
        CHECK(!chrome->groups()[1].toMap().value(QStringLiteral("visible")).toBool());
        // The published view state carries the option (M7.5 / M8.20).
        WAIT_FOR(core.viewStates.size() > 0 && core.viewStates.back().value(QStringLiteral("options")).toObject().value(QStringLiteral("hiddenGroups")).toArray().contains(QStringLiteral("demo:food")));

        // Survives a style reload: switch basemap and back.
        core.emitCoreEvent(QStringLiteral("basemapChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()}, {QStringLiteral("id"), QStringLiteral("test/alt")}});
        WAIT_FOR(view.basemap() == QStringLiteral("test/alt") && item->styleLoaded());
        WAIT_FOR(applier->appliedRevision() == rev && map->layerExists(QStringLiteral("demo:cafes")));
        pump(400);
        hit = layersHit(pickNow(QStringLiteral("tap"), centre));
        CHECK_MSG(!hit.contains(QStringLiteral("demo:cafes")) && hit.contains(QStringLiteral("demo:plain")), hit.join(QLatin1Char(',')));

        // Another view (or the API) shows it again: groupVisibilityChanged.
        core.emitCoreEvent(QStringLiteral("groupVisibilityChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()}, {QStringLiteral("session"), QStringLiteral("demo")},
                                                                                {QStringLiteral("groupId"), QStringLiteral("demo:food")}, {QStringLiteral("visible"), true}});
        WAIT_FOR(chrome->hiddenGroups().isEmpty());
        CHECK(applier->layerVisible(QStringLiteral("demo:cafes")));
        pump(300);
        WAIT_FOR_MS(layersHit(pickNow(QStringLiteral("tap"), centre)).contains(QStringLiteral("demo:cafes")), 5000);

        // The hiddenGroups option from the core (another view's toggle is
        // scoped to that view; ours arrives as viewOptionsChanged).
        core.emitCoreEvent(QStringLiteral("viewOptionsChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()},
                                                                             {QStringLiteral("options"), QJsonObject{{QStringLiteral("hiddenGroups"), QJsonArray{QStringLiteral("demo:food")}}}}});
        WAIT_FOR(chrome->hiddenGroups() == QStringList{QStringLiteral("demo:food")});
        core.emitCoreEvent(QStringLiteral("viewOptionsChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()},
                                                                             {QStringLiteral("options"), QJsonObject{{QStringLiteral("hiddenGroups"), QJsonArray{}}}}});
        WAIT_FOR(chrome->hiddenGroups().isEmpty());
        pump(200);
    }

    // ── basemap details (M9.1a) and catalogue (M8.20 c) ─────────────────
    {
        // Back to the basemap that declares details.
        core.emitCoreEvent(QStringLiteral("basemapChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()}, {QStringLiteral("id"), QStringLiteral("test/bg")}});
        WAIT_FOR(view.basemap() == QStringLiteral("test/bg") && item->styleLoaded());
        WAIT_FOR(chrome->details().size() == 1);
        CHECK(chrome->details()[0].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("labels"));
        CHECK(chrome->details()[0].toMap().value(QStringLiteral("visible")).toBool());
        WAIT_FOR(chrome->basemaps().size() == 2);
        CHECK(chrome->basemap() == QStringLiteral("test/bg"));
        CHECK(chrome->basemaps()[0].toMap().value(QStringLiteral("thumbnail")).toString() == QStringLiteral("memory://thumb.png") ||
              chrome->basemaps()[1].toMap().value(QStringLiteral("thumbnail")).toString() == QStringLiteral("memory://thumb.png"));

        chrome->setDetailVisible(QStringLiteral("labels"), false);
        CHECK(!chrome->details()[0].toMap().value(QStringLiteral("visible")).toBool());
        CHECK(applier->hiddenDetails() == QStringList{QStringLiteral("labels")});
        WAIT_FOR(core.viewStates.size() > 0 && core.viewStates.back().value(QStringLiteral("options")).toObject().value(QStringLiteral("hiddenDetails")).toArray().contains(QStringLiteral("labels")));
        chrome->setDetailVisible(QStringLiteral("labels"), true);
        CHECK(applier->hiddenDetails().isEmpty());

        // Picking a basemap in the panel goes through the core.
        const int before = core.calls.count(QStringLiteral("setBasemap"));
        chrome->selectBasemap(QStringLiteral("test/alt"));
        WAIT_FOR(core.calls.count(QStringLiteral("setBasemap")) == before + 1);
        chrome->selectBasemap(QStringLiteral("test/alt"));  // the fake does not switch; same id again is a no-op on our side only when current
        pump(100);
        // Panel open/close and Escape.
        chrome->setPanelOpen(true);
        CHECK(chrome->panelOpen());
        CHECK(chrome->escape() == QStringLiteral("panel"));
        CHECK(!chrome->panelOpen());
        // A tap on the map (no hit) dismisses the sheet too.
        chrome->setPanelOpen(true);
        chrome->onFeaturePicked(QJsonObject{{QStringLiteral("kind"), QStringLiteral("tap")}, {QStringLiteral("hits"), QJsonArray()}});
        CHECK(!chrome->panelOpen());
    }

    // ── callout (M8.19) and selected state (M8.15a) ──────────────────────
    {
        pump(200);
        WAIT_FOR_MS(layersHit(pickNow(QStringLiteral("tap"), centre)).contains(QStringLiteral("demo:cafes")), 5000);
        const int opened = core.eventCount(QStringLiteral("calloutOpened"));
        const size_t states = core.featureStates.size();
        view.pickAt(QStringLiteral("tap"), centre);
        WAIT_FOR(core.eventCount(QStringLiteral("calloutOpened")) == opened + 1);
        const QVariantMap callout = chrome->callout();
        CHECK(callout.value(QStringLiteral("open")).toBool());
        CHECK(callout.value(QStringLiteral("title")).toString() == QStringLiteral("Café Central"));
        CHECK(callout.value(QStringLiteral("subtitle")).toString() == QStringLiteral("A cafe nearby"));
        CHECK(callout.value(QStringLiteral("actions")).toList().size() == 2);
        CHECK(callout.value(QStringLiteral("actions")).toList()[0].toMap().value(QStringLiteral("label")).toString() == QStringLiteral("Open Café Central"));
        CHECK(!callout.value(QStringLiteral("builtin")).toBool());
        const QVariantList px = callout.value(QStringLiteral("px")).toList();
        CHECK(px.size() == 2 && std::fabs(px[0].toDouble() - centre.x()) < 2.0 && std::fabs(px[1].toDouble() - centre.y()) < 2.0);
        const QJsonObject ev = core.lastEvent(QStringLiteral("calloutOpened"));
        CHECK(ev.value(QStringLiteral("layerId")).toString() == QStringLiteral("demo:cafes"));
        CHECK(ev.value(QStringLiteral("sourceId")).toString() == QStringLiteral("demo:pois"));
        CHECK(ev.value(QStringLiteral("featureId")).toVariant().toString() == QStringLiteral("1"));
        CHECK(ev.value(QStringLiteral("session")).toString() == QStringLiteral("demo"));
        CHECK(ev.value(QStringLiteral("properties")).toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Café Central"));
        // selected went through the core.
        WAIT_FOR(core.featureStates.size() > states);
        CHECK(core.featureStates.back().sourceId == QStringLiteral("demo:pois") && core.featureStates.back().featureId == QStringLiteral("1"));
        CHECK(core.featureStates.back().state.value(QStringLiteral("selected")).toBool() == true);

        // The anchor tracks the camera.
        map->moveBy(QPointF(10, 0));
        pump(100);
        view.pickAt(QStringLiteral("tap"), centre + QPointF(100, 80));  // nothing there: closes instead, so first check the position
        const QVariantList px2 = chrome->callout().value(QStringLiteral("px")).toList();
        CHECK(px2.size() == 2);
        pump(200);
        // The tap away closed it (reason tap) and cleared selected.
        WAIT_FOR(core.lastEvent(QStringLiteral("calloutClosed")).value(QStringLiteral("reason")).toString() == QStringLiteral("tap"));
        CHECK(!chrome->callout().value(QStringLiteral("open")).toBool());
        WAIT_FOR(core.featureStates.back().state.value(QStringLiteral("selected")).toBool() == false);
        map->moveBy(QPointF(-10, 0));
        pump(100);

        // Re-open, run an action: calloutAction then calloutClosed{action}.
        view.pickAt(QStringLiteral("tap"), centre);
        WAIT_FOR(chrome->callout().value(QStringLiteral("open")).toBool());
        const int actions = core.eventCount(QStringLiteral("calloutAction"));
        chrome->calloutAction(QStringLiteral("route"));
        WAIT_FOR(core.eventCount(QStringLiteral("calloutAction")) == actions + 1);
        CHECK(core.lastEvent(QStringLiteral("calloutAction")).value(QStringLiteral("actionId")).toString() == QStringLiteral("route"));
        CHECK(core.lastEvent(QStringLiteral("calloutAction")).value(QStringLiteral("featureId")).toVariant().toString() == QStringLiteral("1"));
        WAIT_FOR(core.lastEvent(QStringLiteral("calloutClosed")).value(QStringLiteral("reason")).toString() == QStringLiteral("action"));
        CHECK(!chrome->callout().value(QStringLiteral("open")).toBool());

        // Re-open, Escape closes (M8.22).
        view.pickAt(QStringLiteral("tap"), centre);
        WAIT_FOR(chrome->callout().value(QStringLiteral("open")).toBool());
        CHECK(gestures->keyPressed(Qt::Key_Escape, 0));
        CHECK(!chrome->callout().value(QStringLiteral("open")).toBool());
        WAIT_FOR(core.lastEvent(QStringLiteral("calloutClosed")).value(QStringLiteral("reason")).toString() == QStringLiteral("escape"));

        // setCalloutContent from a module replaces the content (M8.19).
        view.pickAt(QStringLiteral("tap"), centre);
        WAIT_FOR(chrome->callout().value(QStringLiteral("open")).toBool());
        core.emitCoreEvent(QStringLiteral("calloutContentChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()},
                                                                                {QStringLiteral("content"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Renamed")}}}});
        WAIT_FOR(chrome->callout().value(QStringLiteral("title")).toString() == QStringLiteral("Renamed"));

        // The feature's layer leaving the scene closes it (reason removed).
        const quint64 rev2 = core.publish(QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("removeLayer")}, {QStringLiteral("id"), QStringLiteral("demo:cafes")}}});
        WAIT_FOR(applier->appliedRevision() == rev2);
        CHECK(!chrome->callout().value(QStringLiteral("open")).toBool());
        WAIT_FOR(core.lastEvent(QStringLiteral("calloutClosed")).value(QStringLiteral("reason")).toString() == QStringLiteral("removed"));
        // Group membership updated: Food has no layers now.
        WAIT_FOR(chrome->groups()[1].toMap().value(QStringLiteral("layers")).toStringList().isEmpty());

        // Callouts switched off: a tap on a callout layer opens nothing.
        const quint64 rev3 = core.publish(QJsonArray{ops[4]});  // put the layer back
        WAIT_FOR(applier->appliedRevision() == rev3);
        pump(300);
        view.setOptions(QStringLiteral(R"({"v":1,"chrome":{"callouts":false}})"));
        const int openedBefore = core.eventCount(QStringLiteral("calloutOpened"));
        view.pickAt(QStringLiteral("tap"), centre);
        pump(300);
        CHECK(core.eventCount(QStringLiteral("calloutOpened")) == openedBefore);
        CHECK(!chrome->callout().value(QStringLiteral("open")).toBool());
        view.setOptions(QStringLiteral(R"({"v":1,"chrome":{"callouts":true}})"));
        chrome->mapTapped();  // clear the selection-only state
    }

    // ── dropped pin (M8.21) ──────────────────────────────────────────────
    {
        const QPointF empty(30, 30);
        CHECK(layersHit(pickNow(QStringLiteral("longPress"), empty)).isEmpty());
        const int dropped = core.eventCount(QStringLiteral("pinDropped"));
        view.pickAt(QStringLiteral("longPress"), empty);
        WAIT_FOR(core.eventCount(QStringLiteral("pinDropped")) == dropped + 1);
        CHECK(chrome->pin().value(QStringLiteral("active")).toBool());
        const QMapLibre::Coordinate at = map->coordinateForPixel(empty);
        CHECK(std::fabs(chrome->pin().value(QStringLiteral("lon")).toDouble() - at.second) < 1e-6);
        CHECK(chrome->pin().value(QStringLiteral("featureId")).toString().startsWith(QStringLiteral("pin:")));
        CHECK(map->layerExists(QLatin1String(ChromeController::kPinLayer)));
        CHECK(chrome->pinInstalled());
        // Its callout: coordinate to 5 decimals, copy + share.
        CHECK(chrome->callout().value(QStringLiteral("open")).toBool());
        CHECK(chrome->callout().value(QStringLiteral("builtin")).toBool());
        CHECK(chrome->callout().value(QStringLiteral("title")).toString() == ChromeController::formatCoordinate(at.second, at.first));
        CHECK(chrome->callout().value(QStringLiteral("actions")).toList().size() == 2);
        CHECK(chrome->sharePin().startsWith(QStringLiteral("geo:")));
        // Opening the pin's callout reports nothing as calloutOpened (built-in).
        const QJsonArray lonLat = core.lastEvent(QStringLiteral("pinDropped")).value(QStringLiteral("lonLat")).toArray();
        CHECK(lonLat.size() == 2);

        // A long-press with a hit drops nothing.
        pump(200);
        const int dropped2 = core.eventCount(QStringLiteral("pinDropped"));
        view.pickAt(QStringLiteral("longPress"), centre);
        WAIT_FOR(core.eventCount(QStringLiteral("featurePicked")) > 0);
        pump(200);
        CHECK(core.eventCount(QStringLiteral("pinDropped")) == dropped2);

        // A module names it through setCalloutContent.
        core.emitCoreEvent(QStringLiteral("calloutContentChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()},
                                                                                {QStringLiteral("content"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Somewhere St.")}}}});
        WAIT_FOR(chrome->callout().value(QStringLiteral("title")).toString() == QStringLiteral("Somewhere St."));
        CHECK(chrome->callout().value(QStringLiteral("builtin")).toBool());

        // The pin survives a style reload.
        core.emitCoreEvent(QStringLiteral("basemapChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()}, {QStringLiteral("id"), QStringLiteral("test/alt")}});
        WAIT_FOR(view.basemap() == QStringLiteral("test/alt") && item->styleLoaded());
        WAIT_FOR(map->layerExists(QLatin1String(ChromeController::kPinLayer)));
        CHECK(chrome->pin().value(QStringLiteral("active")).toBool());

        // Escape: the pin's callout and the pin go together (pinCleared).
        const int cleared = core.eventCount(QStringLiteral("pinCleared"));
        CHECK(chrome->escape() == QStringLiteral("pin"));
        CHECK(!chrome->pin().value(QStringLiteral("active")).toBool());
        CHECK(!chrome->callout().value(QStringLiteral("open")).toBool());
        WAIT_FOR(core.eventCount(QStringLiteral("pinCleared")) == cleared + 1);
        CHECK(!map->layerExists(QLatin1String(ChromeController::kPinLayer)));

        // The clearPin option.
        chrome->dropPinAt(13.0, 52.0);
        CHECK(chrome->pin().value(QStringLiteral("active")).toBool());
        view.setOptions(QStringLiteral(R"({"v":1,"clearPin":true})"));
        CHECK(!chrome->pin().value(QStringLiteral("active")).toBool());
    }

    // ── coordinate readout (M8.17) ───────────────────────────────────────
    {
        chrome->pointerMoved(centre.x(), centre.y());
        const QVariantMap c = chrome->coordinate();
        CHECK(!c.value(QStringLiteral("text")).toString().isEmpty());
        CHECK(c.value(QStringLiteral("pointer")).toBool());
        const QMapLibre::Coordinate at = map->coordinateForPixel(centre);
        CHECK(c.value(QStringLiteral("text")).toString() == ChromeController::formatCoordinate(at.second, at.first));
        chrome->pointerLeft();
        CHECK(!chrome->coordinate().value(QStringLiteral("pointer")).toBool());  // falls back to the centre
        CHECK(!chrome->coordinate().value(QStringLiteral("text")).toString().isEmpty());
        view.setOptions(QStringLiteral(R"({"v":1,"chrome":{"coordinateReadout":false}})"));
        CHECK(chrome->coordinate().isEmpty());
    }

    // ── status chip (M8.17) ──────────────────────────────────────────────
    {
        CHECK(!chrome->status().value(QStringLiteral("visible")).toBool());
        core.addBasemap(QStringLiteral("test/missing"), QJsonObject{}, QStringLiteral("memory://nope.json"));
        const int failed = core.eventCount(QStringLiteral("basemapFailed"));
        core.emitCoreEvent(QStringLiteral("basemapChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()}, {QStringLiteral("id"), QStringLiteral("test/missing")}});
        WAIT_FOR(core.eventCount(QStringLiteral("basemapFailed")) > failed || chrome->status().value(QStringLiteral("visible")).toBool());
        WAIT_FOR(chrome->status().value(QStringLiteral("visible")).toBool());
        CHECK(chrome->status().value(QStringLiteral("kind")).toString() == QStringLiteral("error"));
        // Recovery clears it.
        core.emitCoreEvent(QStringLiteral("basemapChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()}, {QStringLiteral("id"), QStringLiteral("test/bg")}});
        WAIT_FOR(view.basemap() == QStringLiteral("test/bg") && item->styleLoaded());
        WAIT_FOR(!chrome->status().value(QStringLiteral("visible")).toBool());
    }

    // ── padding reaches the chrome (M7.3) ────────────────────────────────
    view.setOptions(QStringLiteral(R"({"v":1,"padding":{"left":10,"top":20,"right":30,"bottom":40}})"));
    CHECK(chrome->padding().value(QStringLiteral("top")).toInt() == 20 && chrome->padding().value(QStringLiteral("bottom")).toInt() == 40);

    // The one error we provoked is the missing basemap for the status chip.
    QStringList unexpected;
    for (const QString& e : errors)
        if (!e.startsWith(QStringLiteral("maps.basemapFailed test/missing"))) unexpected << e;
    CHECK_MSG(unexpected.isEmpty(), unexpected.join(QStringLiteral(" | ")));
    view.stop();
    return finish();
}
