// Section 16.4 gesture matrix: synthetic touch sequences and pointer events
// through the GestureController against a mocked camera, with a fake clock
// so inertia and timeouts are deterministic. Every row of M8.2/M8.3 has a
// positive and a negative case; then a QTest touch/mouse pass through the
// real MapWindow.qml handler stacks proves the wiring.
#include "input/gesture_controller.h"
#include "test_support.h"
#include "view/view_window.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QtMath>

#include <cmath>

using namespace maps::view;
using namespace maps::test;

namespace {

// A camera that records every call and keeps a consistent state.
class MockCamera final : public CameraTarget {
public:
    double z = 10, b = 0, p = 0;
    QSizeF sz{800, 600};
    int moves = 0, scales = 0, bearings = 0, pitches = 0, eases = 0, cancels = 0;
    QPointF totalMove;
    double totalZoom = 0;
    QPointF lastAnchor;
    std::optional<CameraEase> lastEase;
    bool frozen = false;
    int freezeChanges = 0;

    double zoom() const override { return z; }
    double bearing() const override { return b; }
    double pitch() const override { return p; }
    QSizeF size() const override { return sz; }
    void moveBy(const QPointF& o) override { ++moves; totalMove += o; }
    void scaleBy(double f, const QPointF& a) override { ++scales; z += std::log2(f); totalZoom += std::log2(f); lastAnchor = a; }
    void setBearing(double d, const QPointF& a) override { ++bearings; b = d; lastAnchor = a; }
    void setPitch(double d) override { ++pitches; p = d; }
    void setZoom(double zoom, const QPointF& a) override { ++scales; totalZoom += zoom - z; z = zoom; lastAnchor = a; }
    void easeTo(const CameraEase& e) override
    {
        ++eases;
        lastEase = e;
        if (e.zoom) z = *e.zoom;
        if (e.bearing) b = *e.bearing;
        if (e.pitch) p = *e.pitch;
    }
    void cancelTransitions() override { ++cancels; }
    void setGestureInProgress(bool f) override { if (frozen != f) ++freezeChanges; frozen = f; }
    void reset() { moves = scales = bearings = pitches = eases = cancels = 0; totalMove = QPointF(); totalZoom = 0; lastEase.reset(); }
};

struct Rig {
    MockCamera cam;
    GestureController gc;
    qint64 t = 1000;
    QStringList began, ended;
    QList<QPointF> taps, longPresses, contexts;
    int idles = 0;

    Rig()
    {
        gc.setCamera(&cam);
        gc.setClock([this] { return t; });
        gc.setInertiaTimerEnabled(false);
        QObject::connect(&gc, &GestureController::gestureBegan, [this](const QString& k) { began << k; });
        QObject::connect(&gc, &GestureController::gestureEnded, [this](const QString& k) { ended << k; });
        QObject::connect(&gc, &GestureController::tapped, [this](const QPointF& p) { taps << p; });
        QObject::connect(&gc, &GestureController::longPressed, [this](const QPointF& p) { longPresses << p; });
        QObject::connect(&gc, &GestureController::contextRequested, [this](const QPointF& p) { contexts << p; });
        QObject::connect(&gc, &GestureController::idle, [this] { ++idles; });
    }
    void advance(int ms) { t += ms; }
    // Step the inertia clock in 16 ms frames for `ms` total.
    void run(int ms)
    {
        for (int i = 0; i < ms / 16; ++i) { advance(16); gc.tick(16); }
    }
    // Two fingers placed symmetrically around `centre` at `span` and `angleDeg`.
    void placeTwo(const QPointF& centre, double span, double angleDeg, bool press)
    {
        const QPointF d(std::cos(qDegreesToRadians(angleDeg)) * span / 2, std::sin(qDegreesToRadians(angleDeg)) * span / 2);
        if (press) { gc.touchPressed(1, centre - d); gc.touchPressed(2, centre + d); }
        else { gc.touchBatchBegin(); gc.touchMoved(1, centre - d); gc.touchMoved(2, centre + d); gc.touchBatchEnd(); }
    }
    void releaseTwo(const QPointF& centre, double span, double angleDeg)
    {
        const QPointF d(std::cos(qDegreesToRadians(angleDeg)) * span / 2, std::sin(qDegreesToRadians(angleDeg)) * span / 2);
        gc.touchReleased(1, centre - d);
        gc.touchReleased(2, centre + d);
    }
};

}  // namespace

int main(int argc, char** argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const QPointF c(400, 300);

    // ── single-finger pan: dead zone and movement ────────────────────────
    {
        Rig r;
        r.gc.touchPressed(1, c);
        CHECK(r.gc.gestureInProgress());
        CHECK(r.cam.frozen);
        r.gc.touchMoved(1, c + QPointF(3, 0));  // inside the 4 dp dead zone
        CHECK(r.cam.moves == 0);
        CHECK(r.began.isEmpty());
        r.gc.touchMoved(1, c + QPointF(10, 0));
        CHECK(r.cam.moves == 1);
        CHECK(r.began == QStringList{QStringLiteral("pan")});
        r.gc.touchMoved(1, c + QPointF(20, 5));
        CHECK(qFuzzyCompare(r.cam.totalMove.x(), 17.0) && qFuzzyCompare(r.cam.totalMove.y(), 5.0));
        r.advance(300);  // released long after the last move: no fling
        r.gc.touchReleased(1, c + QPointF(20, 5));
        CHECK(!r.gc.gestureInProgress());
        CHECK(!r.cam.frozen);
        CHECK(r.ended == QStringList{QStringLiteral("pan")});
        CHECK(!r.gc.inertiaActive());
        CHECK(r.idles == 1);
        CHECK(r.taps.isEmpty());
    }

    // scrollEnabled=false: the finger moves, the map does not.
    {
        Rig r;
        r.gc.setScrollEnabled(false);
        r.gc.touchPressed(1, c);
        r.gc.touchMoved(1, c + QPointF(40, 0));
        CHECK(r.cam.moves == 0);
        CHECK(r.began.isEmpty());
        r.gc.touchReleased(1, c + QPointF(40, 0));
    }

    // ── fling: inertia from sampled velocity, decays to rest within 1.5 s ─
    {
        Rig r;
        r.gc.touchPressed(1, c);
        QPointF p = c;
        for (int i = 0; i < 8; ++i) { r.advance(16); p += QPointF(16, 0); r.gc.touchMoved(1, p); }  // 1 px/ms
        r.cam.reset();
        r.gc.touchReleased(1, p);
        CHECK(r.gc.inertiaActive());
        CHECK(r.gc.gestureInProgress());  // kinetic freeze: still "in progress" (M8.5)
        CHECK(r.cam.frozen);
        CHECK(r.began.contains(QStringLiteral("fling")));
        CHECK(r.idles == 0);
        r.run(1500);
        CHECK(!r.gc.inertiaActive());
        CHECK(!r.gc.gestureInProgress());
        CHECK(r.ended.contains(QStringLiteral("fling")));
        CHECK(r.cam.totalMove.x() > 100);  // travelled
        CHECK(r.idles == 1);
        // 0.92 decay per frame from 16 px/frame: sum ~ 16/(1-0.92) = 200 px.
        CHECK_MSG(r.cam.totalMove.x() > 150 && r.cam.totalMove.x() < 220, QString::number(r.cam.totalMove.x()));
    }
    // Below the minimum velocity there is no fling.
    {
        Rig r;
        r.gc.touchPressed(1, c);
        QPointF p = c;
        for (int i = 0; i < 8; ++i) { r.advance(16); p += QPointF(2, 0); r.gc.touchMoved(1, p); }  // 0.125 px/ms
        r.gc.touchReleased(1, p);
        CHECK(!r.gc.inertiaActive());
    }
    // A fling capped at 5 px/ms, and any press stops it.
    {
        Rig r;
        r.gc.touchPressed(1, c);
        QPointF p = c;
        for (int i = 0; i < 6; ++i) { r.advance(16); p += QPointF(200, 0); r.gc.touchMoved(1, p); }
        r.cam.reset();
        r.gc.touchReleased(1, p);
        CHECK(r.gc.inertiaActive());
        r.run(32);
        CHECK_MSG(r.cam.totalMove.x() <= 5 * 16 * 2 + 1, QString::number(r.cam.totalMove.x()));
        r.gc.touchPressed(1, c);
        CHECK(!r.gc.inertiaActive());
        CHECK(r.gc.gestureInProgress());
        r.gc.touchReleased(1, c);
    }
    // flingEnabled=false.
    {
        Rig r;
        r.gc.setFlingEnabled(false);
        r.gc.touchPressed(1, c);
        QPointF p = c;
        for (int i = 0; i < 8; ++i) { r.advance(16); p += QPointF(16, 0); r.gc.touchMoved(1, p); }
        r.gc.touchReleased(1, p);
        CHECK(!r.gc.inertiaActive());
    }

    // ── pinch: span threshold then zoom, anchored at the centroid ────────
    {
        Rig r;
        r.placeTwo(c, 100, 0, true);
        CHECK(r.gc.currentTouchMode() == QStringLiteral("pinch"));
        r.placeTwo(c, 104, 0, false);  // 4 dp: under the 6 dp span threshold
        CHECK(r.cam.scales == 0);
        r.placeTwo(c, 112, 0, false);
        CHECK(r.cam.scales == 0);  // crossing frame starts scaling without a jump
        CHECK(r.began == QStringList{QStringLiteral("pinch")});
        r.placeTwo(c, 140, 0, false);
        CHECK(r.cam.scales == 1);
        CHECK(qFuzzyCompare(r.cam.totalZoom, std::log2(140.0 / 112.0)));
        CHECK(r.cam.lastAnchor == c);
        CHECK(r.cam.moves == 0);  // centroid did not move
        r.advance(300);
        r.releaseTwo(c, 140, 0);
        CHECK(r.ended.contains(QStringLiteral("pinch")));
        CHECK(!r.began.contains(QStringLiteral("pan")));  // no incidental pan reported
        CHECK(!r.gc.gestureInProgress());
    }
    // A slow pinch (< 0.15 dp/frame) does not start zooming; a fast one does.
    {
        Rig r;
        r.placeTwo(c, 100, 0, true);
        double span = 100;
        for (int i = 0; i < 100; ++i) { span += 0.1; r.placeTwo(c, span, 0, false); }
        CHECK(r.cam.scales == 0);
        CHECK(!r.began.contains(QStringLiteral("pinch")));
        r.placeTwo(c, span + 1, 0, false);
        CHECK(r.began.contains(QStringLiteral("pinch")));
        r.releaseTwo(c, span + 1, 0);
    }
    // zoomEnabled=false.
    {
        Rig r;
        r.gc.setZoomEnabled(false);
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c, 160, 0, false);
        r.placeTwo(c, 200, 0, false);
        CHECK(r.cam.scales == 0);
        r.releaseTwo(c, 200, 0);
    }
    // Focal point replaces the centroid as anchor (M8.10).
    {
        Rig r;
        r.gc.setFocalPoint(QPointF(100, 100));
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c, 120, 0, false);
        r.placeTwo(c, 160, 0, false);
        CHECK(r.cam.lastAnchor == QPointF(100, 100));
        r.releaseTwo(c, 160, 0);
    }
    // Zoom clamps to [minZoom, maxZoom].
    {
        Rig r;
        r.gc.setMaxZoom(10.5);
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c, 120, 0, false);
        r.placeTwo(c, 400, 0, false);
        CHECK_MSG(std::fabs(r.cam.z - 10.5) < 1e-9, QString::number(r.cam.z));
        r.releaseTwo(c, 400, 0);
    }

    // ── rotate: 3 degrees plain, 15 once pinching, none under northLock ──
    {
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c, 100, 2, false);  // 2 degrees: no rotate
        CHECK(r.cam.bearings == 0);
        r.placeTwo(c, 100, 3.5, false);  // 3.5: rotate starts (no jump)
        CHECK(r.began == QStringList{QStringLiteral("rotate")});
        CHECK(r.cam.bearings == 0);
        r.placeTwo(c, 100, 13.5, false);
        CHECK(r.cam.bearings == 1);
        CHECK_MSG(std::fabs(r.cam.b - (-10.0)) < 1e-6, QString::number(r.cam.b));  // clockwise finger turn: bearing decreases
        r.releaseTwo(c, 100, 13.5);
        CHECK(r.ended.contains(QStringLiteral("rotate")));
    }
    {
        // Already pinching: a 10 degree twist is a hand twist, 16 is a rotate.
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c, 110, 0, false);
        r.placeTwo(c, 130, 0, false);
        CHECK(r.began == QStringList{QStringLiteral("pinch")});
        r.placeTwo(c, 150, 10, false);
        CHECK(r.cam.bearings == 0);
        CHECK(!r.began.contains(QStringLiteral("rotate")));
        r.placeTwo(c, 160, 16, false);
        CHECK(r.began.contains(QStringLiteral("rotate")));
        r.placeTwo(c, 170, 26, false);
        CHECK(r.cam.bearings >= 1);
        CHECK(r.cam.scales >= 2);  // pinch and rotate in one gesture (no exclusion)
        r.releaseTwo(c, 170, 26);
    }
    {
        Rig r;
        r.gc.setNorthLock(true);
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c, 100, 30, false);
        r.placeTwo(c, 100, 60, false);
        CHECK(r.cam.bearings == 0);
        CHECK(!r.began.contains(QStringLiteral("rotate")));
        r.releaseTwo(c, 100, 60);
    }
    {
        Rig r;
        r.gc.setRotateEnabled(false);
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c, 100, 30, false);
        CHECK(r.cam.bearings == 0);
        r.releaseTwo(c, 100, 30);
    }
    {
        // rotateWhileScaling=false: a pinch in progress blocks rotation.
        Rig r;
        r.gc.setRotateWhileScaling(false);
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c, 110, 0, false);
        r.placeTwo(c, 130, 0, false);
        r.placeTwo(c, 150, 40, false);
        CHECK(r.cam.bearings == 0);
        r.releaseTwo(c, 150, 40);
    }

    // ── shove (tilt): 20 dp vertical midpoint travel, fingers <= 55 deg ──
    {
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c + QPointF(0, -15), 100, 0, false);  // 15 dp: under threshold
        CHECK(r.cam.pitches == 0);
        r.placeTwo(c + QPointF(0, -25), 100, 0, false);  // starts (no jump)
        CHECK(r.began == QStringList{QStringLiteral("shove")});
        CHECK(r.gc.currentTouchMode() == QStringLiteral("shove"));
        r.placeTwo(c + QPointF(0, -125), 100, 0, false);  // 100 px up: +10 degrees
        CHECK(r.cam.pitches == 1);
        CHECK_MSG(std::fabs(r.cam.p - 10.0) < 1e-6, QString::number(r.cam.p));
        // Exclusive: span changes while shoving do not zoom.
        r.placeTwo(c + QPointF(0, -130), 200, 0, false);
        CHECK(r.cam.scales == 0);
        r.releaseTwo(c + QPointF(0, -130), 200, 0);
        CHECK(r.ended.contains(QStringLiteral("shove")));
    }
    {
        // Fingers steeper than 55 degrees: vertical travel is not a shove.
        Rig r;
        r.placeTwo(c, 100, 70, true);
        r.placeTwo(c + QPointF(0, -30), 100, 70, false);
        CHECK(r.cam.pitches == 0);
        CHECK(!r.began.contains(QStringLiteral("shove")));
        r.releaseTwo(c + QPointF(0, -30), 100, 70);
    }
    {
        // Pitch clamps at maxPitch (default 60) and 0.
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c + QPointF(0, -25), 100, 0, false);
        r.placeTwo(c + QPointF(0, -2000), 100, 0, false);
        CHECK_MSG(std::fabs(r.cam.p - 60.0) < 1e-6, QString::number(r.cam.p));
        r.placeTwo(c + QPointF(0, 2000), 100, 0, false);
        CHECK(std::fabs(r.cam.p) < 1e-6);
        r.releaseTwo(c + QPointF(0, 2000), 100, 0);
        // maxPitch 70 allowed, 80 clamped to 70.
        r.gc.setMaxPitch(80);
        CHECK(qFuzzyCompare(r.gc.maxPitch(), 70.0));
    }
    {
        // tiltEnabled=false, including mid-gesture (M8.11).
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c + QPointF(0, -25), 100, 0, false);
        CHECK(r.began.contains(QStringLiteral("shove")));
        r.gc.setTiltEnabled(false);
        CHECK(r.ended.contains(QStringLiteral("shove")));
        r.placeTwo(c + QPointF(0, -125), 100, 0, false);
        CHECK(r.cam.pitches == 0);
        r.releaseTwo(c + QPointF(0, -125), 100, 0);
    }

    // ── two-finger pan vs tilt: 44 dp of two-finger movement is a pan ────
    {
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c + QPointF(30, 0), 100, 0, false);  // 30 dp horizontal: nothing yet
        CHECK(r.cam.moves == 0 && r.cam.pitches == 0);
        r.placeTwo(c + QPointF(50, 0), 100, 0, false);  // 50 dp: pan
        CHECK(r.began.contains(QStringLiteral("pan")));
        CHECK(r.cam.moves == 1);
        // Now vertical travel keeps panning, never tilts.
        r.placeTwo(c + QPointF(50, -60), 100, 0, false);
        CHECK(r.cam.pitches == 0);
        CHECK(r.cam.moves == 2);
        r.releaseTwo(c + QPointF(50, -60), 100, 0);
    }
    {
        // A 15 dp horizontal shuffle then 25 dp up: shove wins (dy > dx and
        // dy >= 20 before 44 dp of travel).
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c + QPointF(15, 0), 100, 0, false);
        r.placeTwo(c + QPointF(15, -25), 100, 0, false);
        CHECK(r.began == QStringList{QStringLiteral("shove")});
        r.releaseTwo(c + QPointF(15, -25), 100, 0);
    }

    // ── pinch-release guard: the finger left behind does not pan ─────────
    {
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c, 120, 0, false);
        r.placeTwo(c, 160, 0, false);
        r.cam.reset();
        r.gc.touchReleased(2, c + QPointF(80, 0));
        // Same event batch: the remaining finger jumps as the hand opens.
        r.gc.touchMoved(1, c - QPointF(80, 0) + QPointF(30, 10));
        CHECK(r.cam.moves == 0);
        CHECK(r.gc.gestureInProgress());
        r.gc.settle();  // next event-loop pass
        r.gc.touchMoved(1, c - QPointF(80, 0) + QPointF(30, 10) + QPointF(2, 0));  // under dead zone from new start
        CHECK(r.cam.moves == 0);
        r.gc.touchMoved(1, c - QPointF(80, 0) + QPointF(30, 10) + QPointF(12, 0));
        CHECK(r.cam.moves == 1);
        CHECK(r.began.contains(QStringLiteral("pan")));
        r.gc.touchReleased(1, c);
        CHECK(!r.gc.gestureInProgress());
    }

    // ── taps, double tap, quick zoom, two-finger tap, long press ─────────
    {
        Rig r;
        r.gc.touchPressed(1, c);
        r.advance(80);
        r.gc.touchReleased(1, c);
        CHECK(r.taps.isEmpty());  // held for the double-tap window
        r.advance(200);
        r.gc.tick(0);
        CHECK(r.taps.isEmpty());
        r.advance(200);
        r.gc.tick(0);
        CHECK(r.taps.size() == 1 && r.taps[0] == c);
        CHECK(r.cam.eases == 0);
    }
    {
        // Double tap: +1 zoom eased at the point.
        Rig r;
        r.gc.touchPressed(1, c);
        r.advance(60);
        r.gc.touchReleased(1, c);
        r.advance(150);
        r.gc.touchPressed(1, c + QPointF(3, 2));
        r.advance(60);
        r.gc.touchReleased(1, c + QPointF(3, 2));
        CHECK(r.cam.eases == 1);
        CHECK(r.cam.lastEase && r.cam.lastEase->zoom && std::fabs(*r.cam.lastEase->zoom - 11.0) < 1e-9);
        CHECK(r.cam.lastEase->anchor && *r.cam.lastEase->anchor == c + QPointF(3, 2));
        CHECK(r.cam.lastEase->durationMs == 300);
        r.advance(400);
        r.gc.tick(0);
        CHECK(r.taps.isEmpty());
    }
    {
        // Two taps 400 ms apart are two taps, not a double tap.
        Rig r;
        r.gc.touchPressed(1, c);
        r.advance(60);
        r.gc.touchReleased(1, c);
        r.advance(400);
        r.gc.tick(0);
        r.gc.touchPressed(1, c);
        r.advance(60);
        r.gc.touchReleased(1, c);
        r.advance(400);
        r.gc.tick(0);
        CHECK(r.cam.eases == 0);
        CHECK(r.taps.size() == 2);
    }
    {
        // doubleTapEnabled=false: taps are immediate, no zoom.
        Rig r;
        r.gc.setDoubleTapEnabled(false);
        r.gc.touchPressed(1, c);
        r.gc.touchReleased(1, c);
        CHECK(r.taps.size() == 1);
        r.advance(100);
        r.gc.touchPressed(1, c);
        r.gc.touchReleased(1, c);
        CHECK(r.taps.size() == 2);
        CHECK(r.cam.eases == 0);
    }
    {
        // Quick zoom: double-tap, hold, drag down zooms out by pow(0.8, dy/60); cap 4 levels.
        Rig r;
        r.gc.touchPressed(1, c);
        r.advance(60);
        r.gc.touchReleased(1, c);
        r.advance(100);
        r.gc.touchPressed(1, c);
        r.gc.touchMoved(1, c + QPointF(0, 60));
        CHECK(r.began == QStringList{QStringLiteral("pinch")});
        CHECK_MSG(std::fabs(r.cam.z - (10 + std::log2(0.8))) < 1e-6, QString::number(r.cam.z));
        CHECK(r.cam.lastAnchor == c);
        r.gc.touchMoved(1, c + QPointF(0, 6000));
        CHECK_MSG(std::fabs(r.cam.z - 6.0) < 1e-6, QString::number(r.cam.z));  // capped at 4 levels
        r.gc.touchReleased(1, c + QPointF(0, 6000));
        CHECK(r.cam.eases == 0);
        CHECK(r.ended.contains(QStringLiteral("pinch")));
    }
    {
        Rig r;
        r.gc.setQuickZoomEnabled(false);
        r.gc.touchPressed(1, c);
        r.gc.touchReleased(1, c);
        r.advance(100);
        r.gc.touchPressed(1, c);
        r.gc.touchMoved(1, c + QPointF(0, 120));
        CHECK(r.cam.scales == 0 && r.cam.moves == 0);
        r.gc.touchReleased(1, c + QPointF(0, 120));
    }
    {
        // Two-finger tap: -1 zoom.
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.advance(80);
        r.releaseTwo(c, 100, 0);
        CHECK(r.cam.eases == 1);
        CHECK(r.cam.lastEase && r.cam.lastEase->zoom && std::fabs(*r.cam.lastEase->zoom - 9.0) < 1e-9);
        CHECK(r.cam.lastEase->anchor && *r.cam.lastEase->anchor == c);
    }
    {
        // Two fingers that travelled, or stayed too long, are not a tap.
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c + QPointF(10, 0), 100, 0, false);
        r.releaseTwo(c + QPointF(10, 0), 100, 0);
        CHECK(r.cam.eases == 0);
        r.placeTwo(c, 100, 0, true);
        r.advance(500);
        r.releaseTwo(c, 100, 0);
        CHECK(r.cam.eases == 0);
        r.gc.setTwoFingerTapEnabled(false);
        r.placeTwo(c, 100, 0, true);
        r.advance(50);
        r.releaseTwo(c, 100, 0);
        CHECK(r.cam.eases == 0);
    }
    {
        // Long press: 500 ms without movement.
        Rig r;
        r.gc.touchPressed(1, c);
        r.advance(400);
        r.gc.tick(0);
        CHECK(r.longPresses.isEmpty());
        r.advance(150);
        r.gc.tick(0);
        CHECK(r.longPresses.size() == 1 && r.longPresses[0] == c);
        r.gc.touchReleased(1, c);
        CHECK(r.taps.isEmpty());
        // Movement cancels the long press.
        r.gc.touchPressed(1, c);
        r.gc.touchMoved(1, c + QPointF(20, 0));
        r.advance(600);
        r.gc.tick(0);
        CHECK(r.longPresses.size() == 1);
        r.gc.touchReleased(1, c + QPointF(20, 0));
    }

    // ── north behaviour (M8.6) ───────────────────────────────────────────
    {
        // A small turn (< 12 degrees total) snaps back to north on idle.
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c, 100, 4, false);   // rotate starts
        r.placeTwo(c, 100, 10, false);  // 6 degrees applied
        CHECK(std::fabs(r.cam.b + 6.0) < 1e-6);
        r.advance(300);
        r.releaseTwo(c, 100, 10);
        CHECK(r.cam.eases == 1);
        CHECK(r.cam.lastEase && r.cam.lastEase->bearing && *r.cam.lastEase->bearing == 0.0);
    }
    {
        // A deliberate turn (>= 12 degrees) stays.
        Rig r;
        r.placeTwo(c, 100, 0, true);
        r.placeTwo(c, 100, 4, false);
        r.placeTwo(c, 100, 30, false);
        r.advance(300);
        r.releaseTwo(c, 100, 30);
        CHECK(r.cam.eases == 0);
        CHECK(std::fabs(r.cam.b + 26.0) < 1e-6);
    }
    {
        // northLock: any resting bearing beyond 0.05 degrees eases back.
        Rig r;
        r.cam.b = 0.3;
        r.gc.setNorthLock(true);
        CHECK(r.cam.eases == 1);
        r.cam.b = 0.02;
        r.cam.reset();
        r.gc.touchPressed(1, c);
        r.gc.touchReleased(1, c);
        CHECK(r.cam.eases == 0);
    }

    // ── freeze ref-counting (M8.5) ───────────────────────────────────────
    {
        Rig r;
        r.gc.freeze(QStringLiteral("host"));
        CHECK(r.cam.frozen && r.cam.freezeChanges == 1);
        r.gc.touchPressed(1, c);
        CHECK(r.cam.freezeChanges == 1);
        r.gc.touchReleased(1, c);
        CHECK(r.cam.frozen);  // host still holds it
        CHECK(r.idles == 0);
        r.gc.thaw(QStringLiteral("host"));
        CHECK(!r.cam.frozen && r.cam.freezeChanges == 2);
        CHECK(r.idles == 1);
        CHECK(r.gc.frozenIds().isEmpty());
    }

    // ── pointer class ────────────────────────────────────────────────────
    {
        Rig r;
        r.gc.pointerDragBegan(c, Qt::LeftButton, 0);
        CHECK(r.gc.gestureInProgress());
        r.gc.pointerDragMoved(c + QPointF(10, 5));
        CHECK(r.cam.moves == 1 && r.cam.totalMove == QPointF(10, 5));
        r.advance(300);
        r.gc.pointerDragEnded(c + QPointF(10, 5));
        CHECK(!r.gc.gestureInProgress());
        CHECK(r.began == QStringList{QStringLiteral("pan")} && r.ended == QStringList{QStringLiteral("pan")});

        r.cam.reset();
        r.gc.pointerDragBegan(c, Qt::MiddleButton, 0);
        r.gc.pointerDragMoved(c + QPointF(-7, 0));
        CHECK(r.cam.moves == 1);
        r.gc.pointerDragEnded(c + QPointF(-7, 0));

        // Shift+left: tilt, 0.1 degree per px, dragging up pitches.
        r.cam.reset();
        r.gc.pointerDragBegan(c, Qt::LeftButton, Qt::ShiftModifier);
        r.gc.pointerDragMoved(c + QPointF(0, -100));
        CHECK(std::fabs(r.cam.p - 10.0) < 1e-6);
        CHECK(r.began.last() == QStringLiteral("shove"));
        r.gc.pointerDragEnded(c + QPointF(0, -100));

        // Ctrl+left: rotate around the centre.
        r.cam.reset();
        r.cam.b = 0;
        r.gc.pointerDragBegan(QPointF(500, 300), Qt::LeftButton, Qt::ControlModifier);
        r.gc.pointerDragMoved(QPointF(400, 400));  // 90 degrees clockwise around (400,300)
        CHECK_MSG(std::fabs(r.cam.b + 90.0) < 1e-6, QString::number(r.cam.b));
        CHECK(r.cam.lastAnchor == QPointF(400, 300));
        r.gc.pointerDragEnded(QPointF(400, 400));
        CHECK(r.cam.eases == 0);  // 90 degrees stays

        // Shift+middle: rotate.
        r.cam.reset();
        r.gc.pointerDragBegan(QPointF(500, 300), Qt::MiddleButton, Qt::ShiftModifier);
        r.gc.pointerDragMoved(QPointF(500, 310));
        CHECK(r.cam.bearings == 1);
        r.gc.pointerDragEnded(QPointF(500, 310));

        // Right drag: vertical zoom.
        r.cam.reset();
        r.gc.pointerDragBegan(c, Qt::RightButton, 0);
        r.gc.pointerDragMoved(c + QPointF(0, -100));
        CHECK(std::fabs(r.cam.totalZoom - 1.0) < 1e-9);
        r.gc.pointerDragEnded(c + QPointF(0, -100));

        // Wheel: one notch = one level, continuous.
        r.cam.reset();
        r.gc.wheel(c, 120, 0, 0);
        CHECK(std::fabs(r.cam.totalZoom - 1.0) < 1e-9 && r.cam.lastAnchor == c);
        r.gc.wheel(c, -60, 0, 0);
        CHECK(std::fabs(r.cam.totalZoom - 0.5) < 1e-9);
        r.gc.wheel(c, 0, 240, 0);  // touchpad pixel delta
        CHECK(std::fabs(r.cam.totalZoom - 2.5) < 1e-9);

        // Clicks.
        r.gc.clicked(c, Qt::LeftButton);
        CHECK(r.taps.size() == 1);
        r.gc.clicked(c, Qt::RightButton);
        CHECK(r.contexts.size() == 1);
        r.cam.reset();
        r.gc.doubleClicked(c, Qt::LeftButton);
        CHECK(r.cam.eases == 1 && r.cam.lastEase->zoom && std::fabs(*r.cam.lastEase->zoom - r.cam.z) < 1e-9);
        r.gc.pointerLongPressed(c);
        CHECK(r.longPresses.size() == 1);
    }
    {
        // Pointer pan fling.
        Rig r;
        r.gc.pointerDragBegan(c, Qt::LeftButton, 0);
        QPointF p = c;
        for (int i = 0; i < 8; ++i) { r.advance(16); p += QPointF(16, 0); r.gc.pointerDragMoved(p); }
        r.cam.reset();
        r.gc.pointerDragEnded(p);
        CHECK(r.gc.inertiaActive());
        r.run(1500);
        CHECK(!r.gc.inertiaActive());
        CHECK(r.cam.totalMove.x() > 100);
    }

    // ── keyboard / D-pad (M8.8) ──────────────────────────────────────────
    {
        Rig r;
        CHECK(r.gc.keyPressed(Qt::Key_Left, 0));
        CHECK(r.cam.moves == 1 && std::fabs(r.cam.totalMove.x() - 800 * 0.18) < 1e-9);
        CHECK(r.gc.keyPressed(Qt::Key_Up, 0));
        CHECK(std::fabs(r.cam.totalMove.y() - 600 * 0.18) < 1e-9);
        // M8.22: +/- ease by one level (two with Shift) about the padded
        // centre (no anchor), the same path as the zoom buttons.
        const double z0 = r.cam.z;
        CHECK(r.gc.keyPressed(Qt::Key_Plus, 0));
        CHECK(r.cam.eases == 1 && r.cam.lastEase && r.cam.lastEase->zoom && std::fabs(*r.cam.lastEase->zoom - (z0 + 1.0)) < 1e-9);
        CHECK(r.cam.lastEase->durationMs == gesture::kKeyboardZoomMs && !r.cam.lastEase->anchor);
        CHECK(r.gc.keyPressed(Qt::Key_Minus, 0));
        CHECK(std::fabs(r.cam.z - z0) < 1e-9);
        CHECK(r.gc.keyPressed(Qt::Key_Plus, Qt::ShiftModifier));
        CHECK(std::fabs(r.cam.z - (z0 + 2.0)) < 1e-9);
        CHECK(r.gc.keyPressed(Qt::Key_Minus, Qt::ShiftModifier));
        CHECK(std::fabs(r.cam.z - z0) < 1e-9);
        int keyboardMoves = 0, escapes = 0;
        QObject::connect(&r.gc, &GestureController::keyboardMoved, [&](const QString&) { ++keyboardMoves; });
        QObject::connect(&r.gc, &GestureController::escapePressed, [&] { ++escapes; });
        CHECK(r.gc.keyPressed(Qt::Key_Escape, 0) && escapes == 1);
        CHECK(r.gc.zoomStep(1.0, gesture::kZoomButtonMs) && r.cam.lastEase->durationMs == gesture::kZoomButtonMs && keyboardMoves == 1);
        CHECK(r.gc.canZoomIn() && r.gc.canZoomOut());
        r.gc.setMaxZoom(r.cam.z);
        CHECK(!r.gc.canZoomIn() && !r.gc.zoomStep(1.0, gesture::kZoomButtonMs));
        r.gc.setMaxZoom(gesture::kMaxZoom);
        r.gc.zoomStep(-1.0, 0);
        CHECK(r.gc.keyPressed(Qt::Key_Right, Qt::ShiftModifier));
        CHECK(std::fabs(r.cam.b - 15.0) < 1e-9);
        CHECK(r.gc.keyPressed(Qt::Key_Up, Qt::ShiftModifier));
        CHECK(std::fabs(r.cam.p - 10.0) < 1e-9);
        CHECK(!r.gc.keyPressed(Qt::Key_A, 0));
        int centrePicks = 0, longPicks = 0;
        QObject::connect(&r.gc, &GestureController::centrePickRequested, [&](bool l) { l ? ++longPicks : ++centrePicks; });
        CHECK(r.gc.keyPressed(Qt::Key_Return, 0));
        r.advance(100);
        CHECK(r.gc.keyReleased(Qt::Key_Return, 0));
        CHECK(centrePicks == 1 && longPicks == 0);
        CHECK(r.gc.keyPressed(Qt::Key_Return, 0));
        r.advance(600);
        r.gc.tick(0);
        CHECK(longPicks == 1);
        r.gc.keyReleased(Qt::Key_Return, 0);
        CHECK(centrePicks == 1);
    }

    // ── settings surface (M8.10) through options JSON ────────────────────
    {
        Rig r;
        const QStringList used = r.gc.applyOptions(QJsonObject{{QStringLiteral("northLock"), true}, {QStringLiteral("maxPitch"), 70}, {QStringLiteral("focalPoint"), QJsonArray{10, 20}}, {QStringLiteral("other"), 1}});
        CHECK(used.size() == 3 && !used.contains(QStringLiteral("other")));
        CHECK(r.gc.northLock() && qFuzzyCompare(r.gc.maxPitch(), 70.0) && r.gc.focalPoint() == QPointF(10, 20));
        const QJsonObject s = r.gc.settingsJson();
        for (const QString& k : GestureController::optionKeys()) CHECK_MSG(s.contains(k), k);
        r.gc.applyOptions(QJsonObject{{QStringLiteral("focalPoint"), QJsonValue()}});
        CHECK(!r.gc.hasFocalPoint());
    }

    // ── through the real MapWindow.qml handler stacks ────────────────────
    {
        MockCamera cam;
        GestureController gc;
        gc.setCamera(&cam);
        ViewWindow vw;  // no host window here: its own top-level (RFC-8 fallback)
        QQuickWindow& window = *vw.window();
        vw.setGestureController(&gc);
        vw.resize(640, 480);
        vw.setContextObject(QStringLiteral("view"), nullptr);
        MapItem* item = vw.load(qEnvironmentVariable("MAPS_VIEW_QML_DIR"));
        REQUIRE(item != nullptr);
        CHECK(!vw.usedFallback());
        vw.show();
        WAIT_FOR(window.isExposed());
        QQuickItem* root = vw.rootObject();
        REQUIRE(root != nullptr);
        CHECK(root->findChild<QObject*>(QStringLiteral("touchArea")) != nullptr);
        CHECK(root->findChild<QObject*>(QStringLiteral("pointerDrag")) != nullptr);
        CHECK(root->findChild<QObject*>(QStringLiteral("scaleBar")) != nullptr);
        CHECK(root->findChild<QObject*>(QStringLiteral("compass")) != nullptr);
        CHECK(root->findChild<QObject*>(QStringLiteral("attribution")) != nullptr);

        // Touch pan through MultiPointTouchArea -> controller -> camera.
        auto* touchDevice = QTest::createTouchDevice();
        QTest::touchEvent(&window, touchDevice).press(0, QPoint(300, 200));
        QTest::touchEvent(&window, touchDevice).move(0, QPoint(330, 210));
        WAIT_FOR_MS(cam.moves >= 1, 2000);
        CHECK(cam.frozen);
        QTest::touchEvent(&window, touchDevice).release(0, QPoint(330, 210));
        WAIT_FOR_MS(!gc.gestureInProgress(), 3000);

        // Touch pinch.
        cam.reset();
        QTest::touchEvent(&window, touchDevice).press(0, QPoint(250, 200)).press(1, QPoint(350, 200));
        QTest::qWait(30);
        // Qt Quick compresses touch moves until the next frame, so each
        // step is flushed before the next one is queued.
        QTest::touchEvent(&window, touchDevice).move(0, QPoint(240, 200)).move(1, QPoint(360, 200));
        QTest::qWait(30);
        QTest::touchEvent(&window, touchDevice).move(0, QPoint(200, 200)).move(1, QPoint(400, 200));
        QTest::qWait(30);
        WAIT_FOR_MS(cam.scales >= 1, 2000);
        CHECK(cam.totalZoom > 0);
        QTest::touchEvent(&window, touchDevice).release(0, QPoint(200, 200)).release(1, QPoint(400, 200));
        WAIT_FOR_MS(!gc.gestureInProgress(), 3000);

        // Mouse drag through the DragHandler, wheel through the WheelHandler.
        cam.reset();
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, QPoint(300, 200));
        QTest::mouseMove(&window, QPoint(310, 205));
        QTest::mouseMove(&window, QPoint(340, 220));
        WAIT_FOR_MS(cam.moves >= 1, 2000);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, QPoint(340, 220));
        WAIT_FOR_MS(!gc.gestureInProgress(), 3000);
        cam.reset();
        QWheelEvent wheel(QPointF(300, 200), QPointF(300, 200), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &wheel);
        WAIT_FOR_MS(cam.scales >= 1, 2000);
        CHECK(std::fabs(cam.totalZoom - 1.0) < 1e-9);

        // Keys reach the controller through the focused root.
        cam.reset();
        QTest::keyClick(&window, Qt::Key_Plus);
        WAIT_FOR_MS(cam.eases >= 1, 2000);
    }

    return finish();
}
