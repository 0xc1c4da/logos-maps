// Section 16.5 Picking (maps_view, offscreen).
//
// A headless ViewWindow over a FakeCoreClient with a background-only
// basemap and a synthetic scene: one GeoJSON source of three points drawn by
// three overlapping layers (two circle layers and a symbol layer). Asserts:
//   - a tap at a known pixel returns hits ordered by distance, then by the
//     session's pickPriority list (stable), one hit per layer drawing the
//     feature (M8.12-M8.14);
//   - the 24 dp box catches a feature 10 dp off-centre and misses one 14 dp
//     off; pickBoxDp widens it;
//   - a query before the style has loaded fails with maps.styleNotLoaded;
//   - the native feature-state path (P2) and the selection-overlay fallback
//     (M8.15) produce identical featurePicked payloads, with the capability
//     toggled at runtime on the SceneApplier.
#include "fake_core_client.h"
#include "test_support.h"

#include "input/picker.h"
#include "view/capabilities.h"
#include "view/map_item.h"
#include "view/scene_applier.h"
#include "view/selection_overlay.h"
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

#include <functional>

using namespace maps::test;
using namespace maps::view;

namespace {

QJsonObject backgroundStyle()
{
    return QJsonObject{
        {QStringLiteral("version"), 8},
        {QStringLiteral("name"), QStringLiteral("background")},
        {QStringLiteral("sources"), QJsonObject{}},
        {QStringLiteral("layers"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("bg")}, {QStringLiteral("type"), QStringLiteral("background")},
                                                          {QStringLiteral("paint"), QJsonObject{{QStringLiteral("background-color"), QStringLiteral("#203040")}}}}}},
    };
}

QJsonObject point(int id, const QString& name, const QMapLibre::Coordinate& c)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("Feature")}, {QStringLiteral("id"), id},
                       {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Point")}, {QStringLiteral("coordinates"), QJsonArray{c.second, c.first}}}},
                       {QStringLiteral("properties"), QJsonObject{{QStringLiteral("name"), name}}}};
}

QJsonObject layerOp(const QString& id, const QString& type, const QJsonObject& layout, const QJsonObject& paint)
{
    return QJsonObject{{QStringLiteral("op"), QStringLiteral("putLayer")}, {QStringLiteral("id"), id}, {QStringLiteral("type"), type},
                       {QStringLiteral("placement"), QJsonObject{{QStringLiteral("top"), true}}}, {QStringLiteral("source"), QStringLiteral("demo:pins")},
                       {QStringLiteral("layout"), layout}, {QStringLiteral("paint"), paint}};
}

// "<featureId>@<layerId>" for every hit, in order.
QStringList hitKeys(const QJsonObject& payload)
{
    QStringList out;
    for (const QJsonValue& h : payload.value(QStringLiteral("hits")).toArray()) {
        const QJsonObject o = h.toObject();
        out << o.value(QStringLiteral("featureId")).toVariant().toString() + QLatin1Char('@') + o.value(QStringLiteral("layerId")).toString();
    }
    return out;
}

QJsonObject stateOf(const QJsonObject& payload, const QString& featureId)
{
    for (const QJsonValue& h : payload.value(QStringLiteral("hits")).toArray()) {
        const QJsonObject o = h.toObject();
        if (o.value(QStringLiteral("featureId")).toVariant().toString() == featureId) return o.value(QStringLiteral("state")).toObject();
    }
    return QJsonObject{{QStringLiteral("_missing"), true}};
}

}  // namespace

int main(int argc, char** argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    if (!qEnvironmentVariableIsSet("QT_QUICK_BACKEND")) qputenv("QT_QUICK_BACKEND", "rhi");
    qputenv("QT_SCALE_FACTOR", "1");  // dp == px keeps the box arithmetic literal
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    qputenv("HOME", tmp.path().toLocal8Bit());
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);

    if (!Picker::renderedQueriesAvailable()) {
        std::fprintf(stderr, "engine without P1 (queryRenderedFeatures): picking suite not applicable\n");
        return finish();
    }

    FakeCoreClient core;
    core.addBasemap(QStringLiteral("test/bg"), backgroundStyle());

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
    QList<QJsonObject> picks;
    QObject::connect(&view, &ViewController::featurePicked, [&](const QString& json) { picks << QJsonDocument::fromJson(json.toUtf8()).object(); });

    // ── before the style: maps.styleNotLoaded ────────────────────────────
    view.pickAt(QStringLiteral("tap"), QPointF(10, 10));
    CHECK(!errors.isEmpty() && errors.last().startsWith(QStringLiteral("maps.styleNotLoaded")));
    errors.clear();

    view.start();
    REQUIRE(view.window() != nullptr);
    WAIT_FOR(view.mapItem()->hasMap());
    MapItem* item = view.mapItem();
    QMapLibre::Map* map = item->map();
    REQUIRE(map != nullptr);
    {
        Picker early(map);
        QString code;
        CHECK(!early.pickNow(QStringLiteral("tap"), QPointF(10, 10), &code));
        CHECK(code == QStringLiteral("maps.styleNotLoaded"));
    }
    WAIT_FOR(view.status() == QLatin1String("open"));
    WAIT_FOR(item->styleLoaded());
    WAIT_FOR(core.hasEvent(QStringLiteral("styleReady")));
    REQUIRE(view.picker() != nullptr);
    CHECK(view.picker()->styleLoaded());
    SceneApplier* applier = view.sceneApplier();
    REQUIRE(applier != nullptr);

    // ── a scene around the viewport centre ───────────────────────────────
    // Zoom in so a few pixels are a meaningful geographic distance; the
    // features are then placed by inverting the camera at known pixels.
    QMapLibre::CameraOptions cam;
    cam.center = QVariant::fromValue(QMapLibre::Coordinate(52.52, 13.405));
    cam.zoom = 14.0;
    map->jumpTo(cam);
    pump(200);
    const QPointF centre(128, 96);
    const QMapLibre::Coordinate atCentre = map->coordinateForPixel(centre);
    const QMapLibre::Coordinate tenOff = map->coordinateForPixel(centre + QPointF(10, 0));
    const QMapLibre::Coordinate fourteenOff = map->coordinateForPixel(centre + QPointF(0, 14));

    QJsonArray ops;
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("putSource")}, {QStringLiteral("id"), QStringLiteral("demo:pins")},
                           {QStringLiteral("type"), QStringLiteral("geojson")}, {QStringLiteral("def"), QJsonObject{}}});
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("replaceGeoJson")}, {QStringLiteral("sourceId"), QStringLiteral("demo:pins")},
                           {QStringLiteral("data"), QJsonObject{{QStringLiteral("type"), QStringLiteral("FeatureCollection")},
                                                                {QStringLiteral("features"), QJsonArray{point(1, QStringLiteral("centre"), atCentre),
                                                                                                        point(2, QStringLiteral("ten"), tenOff),
                                                                                                        point(3, QStringLiteral("fourteen"), fourteenOff)}}}},
                           {QStringLiteral("digest"), QStringLiteral("p1")}});
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("putImage")}, {QStringLiteral("id"), QStringLiteral("demo:pin")},
                           {QStringLiteral("pixelRatio"), 1.0}, {QStringLiteral("sdf"), false},
                           {QStringLiteral("bytes"), QString::fromLatin1(pngTile(2, Qt::white).toBase64())}});
    // Three overlapping layers over the same features: tiny footprints so the
    // 24 dp box, not the drawn size, decides what is hit.
    ops.append(layerOp(QStringLiteral("demo:a"), QStringLiteral("circle"), QJsonObject{},
                       QJsonObject{{QStringLiteral("circle-radius"), 1}, {QStringLiteral("circle-color"), QStringLiteral("#ff0000")}}));
    ops.append(layerOp(QStringLiteral("demo:b"), QStringLiteral("circle"), QJsonObject{},
                       QJsonObject{{QStringLiteral("circle-radius"), 1}, {QStringLiteral("circle-color"), QStringLiteral("#00ff00")}}));
    ops.append(layerOp(QStringLiteral("demo:c"), QStringLiteral("symbol"),
                       QJsonObject{{QStringLiteral("icon-image"), QStringLiteral("demo:pin")}, {QStringLiteral("icon-allow-overlap"), true},
                                   {QStringLiteral("icon-ignore-placement"), true}, {QStringLiteral("icon-padding"), 0}},
                       QJsonObject{}));
    const quint64 rev = core.publish(ops);
    WAIT_FOR(applier->appliedRevision() == rev);
    CHECK(map->layerExists(QStringLiteral("demo:a")) && map->layerExists(QStringLiteral("demo:b")) && map->layerExists(QStringLiteral("demo:c")));
    WAIT_FOR(core.lastEvent(QStringLiteral("frameRendered")).value(QStringLiteral("revision")).toDouble() == static_cast<double>(rev));
    pump(300);  // symbol placement runs after the frame

    // One pick through the controller: the featurePicked signal and the core
    // event both carry the payload.
    auto pick = [&](const QString& kind, const QPointF& px) {
        const int before = picks.size();
        const int beforeEvents = core.eventCount(QStringLiteral("featurePicked"));
        view.pickAt(kind, px);
        const bool ok = waitFor([&] { return picks.size() == before + 1 && core.eventCount(QStringLiteral("featurePicked")) == beforeEvents + 1; }, 5000);
        CHECK_MSG(ok, QStringLiteral("pick did not answer"));
        return picks.isEmpty() ? QJsonObject() : picks.last();
    };
    // Symbol hits need placement, which runs after the frame: retry quietly.
    auto pickUntil = [&](const QString& kind, const QPointF& px, const std::function<bool(const QJsonObject&)>& done) {
        QJsonObject p;
        for (int i = 0; i < 25; ++i) {
            p = pick(kind, px);
            if (done(p)) break;
            pump(200);
        }
        return p;
    };

    // ── distance order, one hit per drawing layer, box edge ──────────────
    {
        // Symbol layers hit through their collision boxes; give placement a
        // moment if the first query sees only the circles.
        const QJsonObject p = pickUntil(QStringLiteral("tap"), centre, [](const QJsonObject& o) { return hitKeys(o).contains(QStringLiteral("1@demo:c")); });
        CHECK(p.value(QStringLiteral("v")).toInt() == 1);
        CHECK(p.value(QStringLiteral("kind")).toString() == QStringLiteral("tap"));
        CHECK(p.value(QStringLiteral("px")).toArray() == (QJsonArray{128.0, 96.0}));
        const QStringList keys = hitKeys(p);
        CHECK_MSG(keys == (QStringList{QStringLiteral("1@demo:a"), QStringLiteral("1@demo:b"), QStringLiteral("1@demo:c"),
                                       QStringLiteral("2@demo:a"), QStringLiteral("2@demo:b"), QStringLiteral("2@demo:c")}),
                  keys.join(QLatin1Char(',')));
        const QJsonArray hits = p.value(QStringLiteral("hits")).toArray();
        CHECK(hits.size() == 6);
        CHECK(hits[0].toObject().value(QStringLiteral("distancePx")).toDouble() < 0.5);
        CHECK(std::fabs(hits[3].toObject().value(QStringLiteral("distancePx")).toDouble() - 10.0) < 0.5);
        CHECK(hits[0].toObject().value(QStringLiteral("session")).toString() == QStringLiteral("demo"));
        CHECK(hits[0].toObject().value(QStringLiteral("sourceId")).toString() == QStringLiteral("demo:pins"));
        CHECK(hits[0].toObject().value(QStringLiteral("properties")).toObject().value(QStringLiteral("name")).toString() == QStringLiteral("centre"));
        CHECK(hits[0].toObject().value(QStringLiteral("state")).toObject().isEmpty());
        // 14 dp off is outside the 24 dp box.
        CHECK(!keys.contains(QStringLiteral("3@demo:a")));
        // lonLat is the tap's geographic position.
        const QJsonArray lonLat = p.value(QStringLiteral("lonLat")).toArray();
        CHECK(std::fabs(lonLat[0].toDouble() - atCentre.second) < 1e-6 && std::fabs(lonLat[1].toDouble() - atCentre.first) < 1e-6);
        // The event went to the core unchanged.
        CHECK(core.lastEvent(QStringLiteral("featurePicked")).value(QStringLiteral("hits")).toArray().size() == 6);
    }

    // ── pickBoxDp option widens the box; pickPriority reorders ───────────
    {
        core.emitCoreEvent(QStringLiteral("viewOptionsChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()},
                                                                           {QStringLiteral("options"), QJsonObject{{QStringLiteral("pickBoxDp"), 32}}}});
        pump(50);
        CHECK(view.picker()->boxDp() == 32.0);
        const QJsonObject p = pick(QStringLiteral("longPress"), centre);
        CHECK(p.value(QStringLiteral("kind")).toString() == QStringLiteral("longPress"));
        const QStringList keys = hitKeys(p);
        CHECK_MSG(keys.contains(QStringLiteral("3@demo:a")) && keys.indexOf(QStringLiteral("3@demo:a")) > keys.indexOf(QStringLiteral("2@demo:a")), keys.join(QLatin1Char(',')));

        core.emitCoreEvent(QStringLiteral("viewOptionsChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()},
                                                                           {QStringLiteral("options"), QJsonObject{{QStringLiteral("pickBoxDp"), 24},
                                                                                                                   {QStringLiteral("pickPriority"), QJsonArray{QStringLiteral("demo:c")}}}}});
        pump(50);
        CHECK(view.picker()->priority() == QStringList{QStringLiteral("demo:c")});
        const QStringList prio = hitKeys(pick(QStringLiteral("tap"), centre));
        // Listed layer first (distance order kept within it), the rest in distance order.
        CHECK_MSG(prio == (QStringList{QStringLiteral("1@demo:c"), QStringLiteral("2@demo:c"), QStringLiteral("1@demo:a"), QStringLiteral("1@demo:b"),
                                       QStringLiteral("2@demo:a"), QStringLiteral("2@demo:b")}),
                  prio.join(QLatin1Char(',')));
        core.emitCoreEvent(QStringLiteral("viewOptionsChanged"), QJsonObject{{QStringLiteral("viewId"), view.viewId()},
                                                                           {QStringLiteral("options"), QJsonObject{{QStringLiteral("pickPriority"), QJsonValue()}}}});
        pump(50);
    }

    // ── feature state: native path and overlay fallback agree ────────────
    {
        auto setState = [&](const QString& id, const QJsonValue& state) {
            core.emitCoreEvent(QStringLiteral("featureStateChanged"), QJsonObject{{QStringLiteral("session"), QStringLiteral("demo")}, {QStringLiteral("sourceId"), QStringLiteral("demo:pins")},
                                                                                {QStringLiteral("featureId"), id}, {QStringLiteral("state"), state}});
            pump(100);
        };
        SelectionOverlay* overlay = view.selection();
        REQUIRE(overlay != nullptr);

        QJsonObject nativePayload;
        if (hasFeatureState()) {
            CHECK(applier->nativeFeatureState());
            setState(QStringLiteral("1"), QJsonObject{{QStringLiteral("selected"), true}});
            setState(QStringLiteral("1"), QJsonObject{{QStringLiteral("hover"), true}});  // merges
            CHECK(overlay->entryCount() == 0);  // nothing mirrored on the native path
            nativePayload = pickUntil(QStringLiteral("tap"), centre, [](const QJsonObject& o) { return stateOf(o, QStringLiteral("1")).value(QStringLiteral("hover")).toBool(); });
            CHECK(stateOf(nativePayload, QStringLiteral("1")) == (QJsonObject{{QStringLiteral("selected"), true}, {QStringLiteral("hover"), true}}));
            CHECK(stateOf(nativePayload, QStringLiteral("2")).isEmpty());
            setState(QStringLiteral("1"), QJsonValue());  // clear before switching paths
            applier->setNativeFeatureState(false);
        }

        // Fallback: the overlay mirrors the feature and the pick reads state from it.
        setState(QStringLiteral("1"), QJsonObject{{QStringLiteral("selected"), true}});
        setState(QStringLiteral("1"), QJsonObject{{QStringLiteral("hover"), true}});
        CHECK(overlay->entryCount() == 1);
        WAIT_FOR(overlay->installed());
        CHECK(map->sourceExists(overlay->sourceId()));
        for (const QString& id : overlay->layerIds()) CHECK_MSG(map->layerExists(id), id);
        CHECK(overlay->stateFor(QStringLiteral("demo:pins"), QStringLiteral("1")) == (QJsonObject{{QStringLiteral("selected"), true}, {QStringLiteral("hover"), true}}));
        pump(300);
        QJsonObject fallbackPayload;
        fallbackPayload = pickUntil(QStringLiteral("tap"), centre, [](const QJsonObject& o) { return stateOf(o, QStringLiteral("1")).value(QStringLiteral("hover")).toBool(); });
        CHECK(stateOf(fallbackPayload, QStringLiteral("1")) == (QJsonObject{{QStringLiteral("selected"), true}, {QStringLiteral("hover"), true}}));
        CHECK(stateOf(fallbackPayload, QStringLiteral("2")).isEmpty());
        // The mirror itself is never a hit.
        for (const QJsonValue& h : fallbackPayload.value(QStringLiteral("hits")).toArray()) {
            CHECK(h.toObject().value(QStringLiteral("sourceId")).toString() != overlay->sourceId());
            CHECK(!h.toObject().value(QStringLiteral("layerId")).toString().startsWith(QStringLiteral("maps/builtin:")));
        }
        if (hasFeatureState()) {
            CHECK_MSG(fallbackPayload.value(QStringLiteral("hits")) == nativePayload.value(QStringLiteral("hits")),
                      QJsonDocument(fallbackPayload).toJson(QJsonDocument::Compact) + QStringLiteral("\n != \n") + QJsonDocument(nativePayload).toJson(QJsonDocument::Compact));
        }
        // Null removes: overlay empties, state disappears from picks.
        setState(QStringLiteral("1"), QJsonValue());
        CHECK(overlay->entryCount() == 0);
        const QJsonObject cleared = pick(QStringLiteral("tap"), centre);
        CHECK(stateOf(cleared, QStringLiteral("1")).isEmpty());
        if (hasFeatureState()) applier->setNativeFeatureState(true);
    }

    view.stop();
    WAIT_FOR(core.closed.contains(view.viewId()));
    CHECK_MSG(errors.isEmpty(), errors.join(QStringLiteral("\n")));
    return finish();
}
