#include "input/gesture_controller.h"

#include <QJsonArray>
#include <QLineF>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace maps::view {

using namespace gesture;

namespace {

const QString kTouchFreeze = QStringLiteral("touch");
const QString kPointerFreeze = QStringLiteral("pointer");
const QString kKineticFreeze = QStringLiteral("kinetic");

double distance(const QPointF& a, const QPointF& b) { return QLineF(a, b).length(); }

// Angle of the finger line in degrees, screen coordinates (y down), (-180, 180].
double lineAngleDeg(const QPointF& a, const QPointF& b) { return qRadiansToDegrees(std::atan2(b.y() - a.y(), b.x() - a.x())); }

// Angle of the finger line against the horizontal, folded to [0, 90].
double fingerAngleFromHorizontal(const QPointF& a, const QPointF& b)
{
    double d = std::fabs(lineAngleDeg(a, b));
    if (d > 90.0) d = 180.0 - d;
    return d;
}

}  // namespace

GestureController::GestureController(QObject* parent) : QObject(parent)
{
    m_elapsed.start();
    m_inertiaTimer.setInterval(kInertiaFrameMs);
    m_inertiaTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_inertiaTimer, &QTimer::timeout, this, [this] { tick(kInertiaFrameMs); });
    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout, this, [this] { checkTimeouts(); });
}

GestureController::~GestureController() = default;

void GestureController::setCamera(CameraTarget* camera) { m_camera = camera; }

void GestureController::setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }

void GestureController::setInertiaTimerEnabled(bool enabled)
{
    if (!enabled) {
        m_inertiaTimer.stop();
        m_inertiaTimer.setInterval(0);
        m_timeoutTimer.stop();
        m_timeoutTimer.setInterval(0);
    } else {
        m_inertiaTimer.setInterval(kInertiaFrameMs);
    }
}

qint64 GestureController::now() const { return m_clock ? m_clock() : m_elapsed.elapsed(); }

double GestureController::normalizeDeg(double d)
{
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0) d += 360.0;
    return d - 180.0;
}

// ── settings ──────────────────────────────────────────────────────────────

void GestureController::changeSetting(bool& field, bool v)
{
    if (field == v) return;
    field = v;
    emit settingsChanged();
}

void GestureController::setRotateEnabled(bool v) { changeSetting(m_s.rotateEnabled, v); }
void GestureController::setTiltEnabled(bool v)
{
    changeSetting(m_s.tiltEnabled, v);
    if (!v && m_shoving) {  // M8.11: takes effect mid-gesture
        m_shoving = false;
        endKind(QStringLiteral("shove"));
        if (m_mode == TouchMode::Shove) m_mode = TouchMode::Pinch;
    }
}
void GestureController::setZoomEnabled(bool v) { changeSetting(m_s.zoomEnabled, v); }
void GestureController::setScrollEnabled(bool v) { changeSetting(m_s.scrollEnabled, v); }
void GestureController::setDoubleTapEnabled(bool v) { changeSetting(m_s.doubleTapEnabled, v); }
void GestureController::setQuickZoomEnabled(bool v) { changeSetting(m_s.quickZoomEnabled, v); }
void GestureController::setTwoFingerTapEnabled(bool v) { changeSetting(m_s.twoFingerTapEnabled, v); }
void GestureController::setFlingEnabled(bool v) { changeSetting(m_s.flingEnabled, v); }
void GestureController::setNorthLock(bool v)
{
    if (m_s.northLock == v) return;
    m_s.northLock = v;
    if (v && m_rotating) {  // M8.11
        m_rotating = false;
        endKind(QStringLiteral("rotate"));
    }
    if (v && !gestureInProgress()) northBehaviour();
    emit settingsChanged();
}
void GestureController::setRotateWhileScaling(bool v) { changeSetting(m_s.rotateWhileScaling, v); }
void GestureController::setMouseAsTouchScreen(bool v) { changeSetting(m_s.mouseAsTouchScreen, v); }
void GestureController::setMaxPitch(double v)
{
    v = std::clamp(v, 0.0, kAbsoluteMaxPitchDeg);
    if (qFuzzyCompare(m_s.maxPitch, v)) return;
    m_s.maxPitch = v;
    if (m_camera && m_camera->pitch() > v) m_camera->setPitch(v);
    emit settingsChanged();
}
void GestureController::setMinZoom(double v)
{
    v = std::clamp(v, kMinZoom, kMaxZoom);
    if (qFuzzyCompare(m_s.minZoom, v)) return;
    m_s.minZoom = v;
    emit settingsChanged();
}
void GestureController::setMaxZoom(double v)
{
    v = std::clamp(v, kMinZoom, kMaxZoom);
    if (qFuzzyCompare(m_s.maxZoom, v)) return;
    m_s.maxZoom = v;
    emit settingsChanged();
}
void GestureController::setFocalPoint(const QPointF& p)
{
    if (m_s.focalPoint == p || (std::isnan(p.x()) && !hasFocalPoint())) return;
    m_s.focalPoint = p;
    emit settingsChanged();
}
bool GestureController::hasFocalPoint() const { return !std::isnan(m_s.focalPoint.x()) && !std::isnan(m_s.focalPoint.y()); }

const QStringList& GestureController::optionKeys()
{
    static const QStringList keys{
        QStringLiteral("rotateEnabled"), QStringLiteral("tiltEnabled"), QStringLiteral("zoomEnabled"), QStringLiteral("scrollEnabled"),
        QStringLiteral("doubleTapEnabled"), QStringLiteral("quickZoomEnabled"), QStringLiteral("twoFingerTapEnabled"),
        QStringLiteral("flingEnabled"), QStringLiteral("northLock"), QStringLiteral("rotateWhileScaling"),
        QStringLiteral("mouseAsTouchScreen"), QStringLiteral("maxPitch"), QStringLiteral("minZoom"), QStringLiteral("maxZoom"),
        QStringLiteral("focalPoint")};
    return keys;
}

QStringList GestureController::applyOptions(const QJsonObject& o)
{
    QStringList used;
    auto flag = [&](const char* key, void (GestureController::*set)(bool)) {
        const QString k = QLatin1String(key);
        if (o.contains(k) && o.value(k).isBool()) {
            (this->*set)(o.value(k).toBool());
            used << k;
        }
    };
    flag("rotateEnabled", &GestureController::setRotateEnabled);
    flag("tiltEnabled", &GestureController::setTiltEnabled);
    flag("zoomEnabled", &GestureController::setZoomEnabled);
    flag("scrollEnabled", &GestureController::setScrollEnabled);
    flag("doubleTapEnabled", &GestureController::setDoubleTapEnabled);
    flag("quickZoomEnabled", &GestureController::setQuickZoomEnabled);
    flag("twoFingerTapEnabled", &GestureController::setTwoFingerTapEnabled);
    flag("flingEnabled", &GestureController::setFlingEnabled);
    flag("northLock", &GestureController::setNorthLock);
    flag("rotateWhileScaling", &GestureController::setRotateWhileScaling);
    flag("mouseAsTouchScreen", &GestureController::setMouseAsTouchScreen);
    if (o.value(QStringLiteral("maxPitch")).isDouble()) { setMaxPitch(o.value(QStringLiteral("maxPitch")).toDouble()); used << QStringLiteral("maxPitch"); }
    if (o.value(QStringLiteral("minZoom")).isDouble()) { setMinZoom(o.value(QStringLiteral("minZoom")).toDouble()); used << QStringLiteral("minZoom"); }
    if (o.value(QStringLiteral("maxZoom")).isDouble()) { setMaxZoom(o.value(QStringLiteral("maxZoom")).toDouble()); used << QStringLiteral("maxZoom"); }
    if (o.contains(QStringLiteral("focalPoint"))) {
        const QJsonValue v = o.value(QStringLiteral("focalPoint"));
        if (v.isNull()) setFocalPoint(QPointF(qQNaN(), qQNaN()));
        else if (v.isArray() && v.toArray().size() == 2) setFocalPoint(QPointF(v.toArray()[0].toDouble(), v.toArray()[1].toDouble()));
        used << QStringLiteral("focalPoint");
    }
    return used;
}

QJsonObject GestureController::settingsJson() const
{
    QJsonObject o{
        {QStringLiteral("rotateEnabled"), m_s.rotateEnabled}, {QStringLiteral("tiltEnabled"), m_s.tiltEnabled},
        {QStringLiteral("zoomEnabled"), m_s.zoomEnabled}, {QStringLiteral("scrollEnabled"), m_s.scrollEnabled},
        {QStringLiteral("doubleTapEnabled"), m_s.doubleTapEnabled}, {QStringLiteral("quickZoomEnabled"), m_s.quickZoomEnabled},
        {QStringLiteral("twoFingerTapEnabled"), m_s.twoFingerTapEnabled}, {QStringLiteral("flingEnabled"), m_s.flingEnabled},
        {QStringLiteral("northLock"), m_s.northLock}, {QStringLiteral("rotateWhileScaling"), m_s.rotateWhileScaling},
        {QStringLiteral("mouseAsTouchScreen"), m_s.mouseAsTouchScreen}, {QStringLiteral("maxPitch"), m_s.maxPitch},
        {QStringLiteral("minZoom"), m_s.minZoom}, {QStringLiteral("maxZoom"), m_s.maxZoom},
    };
    o.insert(QStringLiteral("focalPoint"), hasFocalPoint() ? QJsonValue(QJsonArray{m_s.focalPoint.x(), m_s.focalPoint.y()}) : QJsonValue());
    return o;
}

// ── freeze / kinds ────────────────────────────────────────────────────────

QStringList GestureController::frozenIds() const
{
    QStringList l(m_frozen.begin(), m_frozen.end());
    l.sort();
    return l;
}

void GestureController::freeze(const QString& id)
{
    const bool was = gestureInProgress();
    m_frozen.insert(id);
    if (!was) {
        if (m_camera) m_camera->setGestureInProgress(true);
        emit gestureInProgressChanged(true);
    }
}

void GestureController::thaw(const QString& id)
{
    if (!m_frozen.remove(id)) return;
    if (m_frozen.isEmpty()) {
        if (m_camera) m_camera->setGestureInProgress(false);
        emit gestureInProgressChanged(false);
        maybeIdle();
    }
}

void GestureController::beginKind(const QString& kind)
{
    if (m_activeKinds.contains(kind)) return;
    m_activeKinds.insert(kind);
    emit gestureBegan(kind);
}

void GestureController::endKind(const QString& kind)
{
    if (!m_activeKinds.remove(kind)) return;
    emit gestureEnded(kind);
}

void GestureController::endAllKinds()
{
    const QStringList kinds(m_activeKinds.begin(), m_activeKinds.end());
    for (const QString& k : kinds) endKind(k);
}

void GestureController::maybeIdle()
{
    if (gestureInProgress()) return;
    northBehaviour();
    emit idle();
}

QString GestureController::currentTouchMode() const
{
    switch (m_mode) {
    case TouchMode::None: return QStringLiteral("none");
    case TouchMode::Pending: return QStringLiteral("pending");
    case TouchMode::Pan: return QStringLiteral("pan");
    case TouchMode::Pinch: return QStringLiteral("pinch");
    case TouchMode::Shove: return QStringLiteral("shove");
    case TouchMode::QuickZoom: return QStringLiteral("quickZoom");
    }
    return QString();
}

// ── camera helpers ────────────────────────────────────────────────────────

QPointF GestureController::anchorFor(const QPointF& centroid) const { return hasFocalPoint() ? m_s.focalPoint : centroid; }

void GestureController::applyZoomDelta(double delta, const QPointF& anchor)
{
    if (!m_camera || delta == 0.0) return;
    const double z = m_camera->zoom();
    const double target = std::clamp(z + delta, m_s.minZoom, m_s.maxZoom);
    if (qFuzzyCompare(target, z)) return;
    m_camera->scaleBy(std::pow(2.0, target - z), anchorFor(anchor));
}

bool GestureController::canZoomIn() const { return m_camera && m_s.zoomEnabled && m_camera->zoom() < m_s.maxZoom - 1e-3; }
bool GestureController::canZoomOut() const { return m_camera && m_s.zoomEnabled && m_camera->zoom() > m_s.minZoom + 1e-3; }

bool GestureController::zoomStep(double levels, int durationMs)
{
    if (!m_camera || !m_s.zoomEnabled || levels == 0.0) return false;
    stopInertia();
    const double z = m_camera->zoom();
    const double target = std::clamp(z + levels, m_s.minZoom, m_s.maxZoom);
    if (qFuzzyCompare(target, z)) return false;
    CameraEase e;
    e.zoom = target;
    // Without an anchor the engine zooms about the padded centre (M7.3); a
    // navigation focal point (M8.10) wins when set.
    if (hasFocalPoint()) e.anchor = m_s.focalPoint;
    e.durationMs = std::max(0, durationMs);
    m_camera->easeTo(e);
    emit keyboardMoved(QStringLiteral("zoom"));
    return true;
}

void GestureController::applyPitchDelta(double delta)
{
    if (!m_camera || delta == 0.0) return;
    const double p = m_camera->pitch();
    const double target = std::clamp(p + delta, 0.0, m_s.maxPitch);
    if (qFuzzyCompare(target + 1.0, p + 1.0)) return;
    m_camera->setPitch(target);
}

void GestureController::applyBearingDelta(double delta, const QPointF& anchor)
{
    if (!m_camera || delta == 0.0) return;
    m_bearingTouched = true;
    m_camera->setBearing(normalizeDeg(m_camera->bearing() + delta), anchorFor(anchor));
}

// ── touch ─────────────────────────────────────────────────────────────────

void GestureController::onFirstPress()
{
    stopInertia();
    if (m_camera) m_camera->cancelTransitions();
    freeze(kTouchFreeze);
    m_mode = TouchMode::Pending;
    m_scaling = m_rotating = m_shoving = m_twoFingerPanning = false;
    m_turnTotal = 0;
    m_bearingTouched = false;
    m_longPressFired = false;
    m_gestureStartBearing = m_camera ? m_camera->bearing() : 0.0;
    m_zoomAtGestureStart = m_camera ? m_camera->zoom() : 0.0;
    m_panSamples.clear();
    m_zoomSamples.clear();
    armTimeoutTimer(kLongPressMs);
}

void GestureController::touchPressed(int id, const QPointF& pos)
{
    const qint64 t = now();
    if (m_points.empty()) onFirstPress();
    Point pt;
    pt.start = pt.pos = pt.last = pos;
    pt.pressedAt = t;
    m_points[id] = pt;

    if (m_points.size() == 1) {
        // Second press of a double tap? (300 ms, within the move tolerance)
        if (m_s.doubleTapEnabled && m_lastTapPos && t - m_lastTapAt <= kDoubleTapMs && distance(*m_lastTapPos, pos) <= kMoveThresholdTouchDp * 4) {
            m_doubleTapSecondPress = true;
            m_pendingTap.reset();
            m_quickZoomAnchor = *m_lastTapPos;
            m_quickZoomStartY = pos.y();
        } else {
            m_doubleTapSecondPress = false;
        }
        samplePan(t, pos);
    } else if (m_points.size() == 2) {
        m_doubleTapSecondPress = false;
        m_pendingTap.reset();
        m_secondPressAt = t;
        m_twoFingerTapCandidate = m_s.twoFingerTapEnabled;
        auto it = m_points.begin();
        const QPointF a = it->second.pos;
        const QPointF b = (++it)->second.pos;
        m_startSpan = m_lastSpan = distance(a, b);
        m_startAngle = m_lastAngle = lineAngleDeg(a, b);
        m_startCentroid = m_lastCentroid = (a + b) / 2.0;
        m_scaling = m_rotating = m_shoving = m_twoFingerPanning = false;
        // A pan in progress becomes a pinch; the pan kind ends only if the
        // fingers never travel the two-finger pan distance.
        m_mode = TouchMode::Pinch;
        m_zoomSamples.clear();
        if (m_camera) sampleZoom(t, m_camera->zoom());
    } else {
        // Three or more fingers: ignore the extras.
    }
}

void GestureController::touchBatchBegin() { ++m_batchDepth; }

void GestureController::touchBatchEnd()
{
    if (m_batchDepth > 0) --m_batchDepth;
    if (m_batchDepth == 0 && m_batchDirty) processTouch();
}

void GestureController::touchMoved(int id, const QPointF& pos)
{
    auto it = m_points.find(id);
    if (it == m_points.end()) return;
    Point& pt = it->second;
    if (!pt.dirty) {  // first change of this batch keeps the pre-batch position
        pt.last = pt.pos;
        pt.dirty = true;
    }
    pt.pos = pos;
    m_lastMoveAt = now();
    if (!pt.moved && distance(pt.start, pos) >= kMoveThresholdTouchDp) pt.moved = true;
    m_batchDirty = true;
    if (m_batchDepth == 0) processTouch();
}

void GestureController::processTouch()
{
    m_batchDirty = false;
    for (auto& [id, pt] : m_points) pt.dirty = false;
    if (m_points.empty()) return;

    if (m_pinchReleasing) {
        // M8.3 last row: the finger left behind by a pinch does not pan
        // until the next event-loop pass; its start follows it meanwhile.
        for (auto& [id, pt] : m_points) {
            pt.start = pt.pos;
            pt.moved = false;
        }
        return;
    }

    if (m_points.size() >= 2) {
        updateTwoFinger();
        return;
    }
    Point& pt = m_points.begin()->second;
    updateSinglePan(pt, pt.last);
}

void GestureController::updateSinglePan(Point& pt, const QPointF& prev)
{
    const qint64 t = now();
    if (m_doubleTapSecondPress) {
        // Quick zoom: second tap held, dragged vertically (M8.2).
        if (!m_s.quickZoomEnabled) return;
        if (m_mode != TouchMode::QuickZoom) {
            if (!pt.moved) return;
            m_mode = TouchMode::QuickZoom;
            m_pendingTap.reset();
            beginKind(QStringLiteral("pinch"));
        }
        if (!m_camera) return;
        const double dy = pt.pos.y() - m_quickZoomStartY;
        double delta = std::log2(std::pow(kQuickZoomBase, dy / kQuickZoomPxPerStep));
        delta = std::clamp(delta, -kQuickZoomMaxLevels, kQuickZoomMaxLevels);
        const double target = std::clamp(m_zoomAtGestureStart + delta, m_s.minZoom, m_s.maxZoom);
        if (!qFuzzyCompare(target, m_camera->zoom())) m_camera->scaleBy(std::pow(2.0, target - m_camera->zoom()), anchorFor(m_quickZoomAnchor));
        return;
    }
    if (!pt.moved) return;
    if (m_mode == TouchMode::Pending) {
        m_mode = TouchMode::Pan;
        m_pendingTap.reset();
        if (m_s.scrollEnabled) beginKind(QStringLiteral("pan"));
    }
    if (m_mode != TouchMode::Pan) return;
    samplePan(t, pt.pos);
    if (!m_s.scrollEnabled || !m_camera) return;
    beginKind(QStringLiteral("pan"));  // no-op when live; starts it after a pinch
    m_camera->moveBy(pt.pos - prev);
}

void GestureController::updateTwoFinger()
{
    const qint64 t = now();
    auto it = m_points.begin();
    const Point& p1 = it->second;
    const Point& p2 = (++it)->second;
    const QPointF a = p1.pos, b = p2.pos;
    const double span = distance(a, b);
    const double angle = lineAngleDeg(a, b);
    const QPointF centroid = (a + b) / 2.0;
    const double centroidTravel = distance(centroid, m_startCentroid);

    if (m_twoFingerTapCandidate) {
        // Any finger travelling (a pure rotation keeps span and centroid
        // still) disqualifies the two-finger tap.
        bool anyMoved = centroidTravel >= kMoveThresholdTouchDp || std::fabs(span - m_startSpan) >= kMoveThresholdTouchDp;
        for (const auto& [id, pt] : m_points) anyMoved = anyMoved || pt.moved;
        if (anyMoved) m_twoFingerTapCandidate = false;
    }

    // Shove (tilt): midpoint travelled 20 dp vertically with the fingers no
    // steeper than 55 degrees, before anything else began (Vela/Android:
    // tilt is exclusive once it starts).
    if (!m_shoving && !m_scaling && !m_rotating && !m_twoFingerPanning && m_s.tiltEnabled) {
        const double dy = std::fabs(centroid.y() - m_startCentroid.y());
        const double dx = std::fabs(centroid.x() - m_startCentroid.x());
        if (dy >= kShoveStartDp && dy > dx && fingerAngleFromHorizontal(a, b) <= kShoveMaxFingerAngleDeg) {
            m_shoving = true;
            m_mode = TouchMode::Shove;
            beginKind(QStringLiteral("shove"));
            m_lastCentroid = centroid;  // no jump by the threshold distance
        }
    }
    if (m_shoving) {
        applyPitchDelta(-(centroid.y() - m_lastCentroid.y()) * kTiltDegPerPx);
        m_lastCentroid = centroid;
        m_lastSpan = span;
        m_lastAngle = angle;
        return;
    }

    // Two-finger pan: decided in the move handler, after scale may have
    // begun (M8.7) -- a pinch's incidental translation is not a pan.
    if (!m_twoFingerPanning && centroidTravel >= kTwoFingerPanDp && m_s.scrollEnabled) {
        m_twoFingerPanning = true;
        beginKind(QStringLiteral("pan"));
    }

    // Scale.
    if (m_s.zoomEnabled && !m_scaling) {
        const bool pastSpan = std::fabs(span - m_startSpan) >= kScaleSpanThresholdDp;
        const bool fastEnough = std::fabs(span - m_lastSpan) >= kScaleMinSpeedDpPerFrame;
        if (pastSpan && fastEnough) {
            m_scaling = true;
            beginKind(QStringLiteral("pinch"));
            m_lastSpan = span;  // no jump by the threshold distance
        }
    }
    if (m_scaling && m_lastSpan > 0 && span > 0) {
        applyZoomDelta(std::log2(span / m_lastSpan), centroid);
        if (m_camera) sampleZoom(t, m_camera->zoom());
    }

    // Rotate: 3 degrees plain, 15 once pinching (a hand pinch twists).
    const double dAngle = normalizeDeg(angle - m_lastAngle);
    if (m_s.rotateEnabled && !m_s.northLock && !m_rotating && (m_s.rotateWhileScaling || !m_scaling)) {
        const double threshold = m_scaling ? kRotateStartWhilePinchingDeg : kRotateStartDeg;
        if (std::fabs(normalizeDeg(angle - m_startAngle)) >= threshold) {
            m_rotating = true;
            beginKind(QStringLiteral("rotate"));
            m_lastAngle = angle;  // no jump by the threshold angle
        }
    }
    if (m_rotating && !m_s.northLock && m_s.rotateEnabled) {
        const double d = normalizeDeg(angle - m_lastAngle);
        m_turnTotal += std::fabs(d);
        // The finger line turning clockwise on screen turns the map
        // clockwise: bearing (clockwise from north) decreases.
        applyBearingDelta(-d, centroid);
    }
    Q_UNUSED(dAngle);

    // Translation: once any two-finger gesture is live the map follows the
    // centroid (anchored pinch); before that nothing moves.
    if ((m_scaling || m_rotating || m_twoFingerPanning) && m_s.scrollEnabled && m_camera) {
        const QPointF d = centroid - m_lastCentroid;
        if (!qFuzzyIsNull(d.x()) || !qFuzzyIsNull(d.y())) m_camera->moveBy(d);
        samplePan(t, centroid);
    }

    m_lastSpan = span;
    m_lastAngle = angle;
    m_lastCentroid = centroid;
}

void GestureController::finishPinch()
{
    const qint64 t = now();
    // Scale velocity animation: scheduled on release unless the fingers
    // stayed put for 150 ms (M8.3).
    if (m_scaling && m_s.flingEnabled && m_s.zoomEnabled && t - m_lastMoveAt <= kVelocityAnimationCancelMs) {
        const double v = zoomVelocity(t);  // zoom levels per ms
        // 225 dp/s scale velocity ~ 0.0005 zoom/ms at a 200 px span.
        if (std::fabs(v) >= 0.0005) startZoomInertia(std::clamp(v, -kScaleVelocityMaxZoomDelta / 400.0, kScaleVelocityMaxZoomDelta / 400.0), m_lastCentroid);
    }
    if (m_scaling) endKind(QStringLiteral("pinch"));
    if (m_rotating) endKind(QStringLiteral("rotate"));
    if (m_shoving) endKind(QStringLiteral("shove"));
    m_scaling = m_rotating = m_shoving = false;
}

void GestureController::touchReleased(int id, const QPointF& pos)
{
    auto it = m_points.find(id);
    if (it == m_points.end()) return;
    const qint64 t = now();
    it->second.pos = pos;
    const size_t before = m_points.size();
    m_points.erase(it);

    if (before == 2) {
        // Two-finger tap: both down and up within 300 ms, no travel.
        if (m_twoFingerTapCandidate && t - m_secondPressAt <= kDoubleTapMs && m_s.twoFingerTapEnabled && m_s.zoomEnabled && m_camera) {
            CameraEase e;
            e.zoom = std::clamp(m_camera->zoom() - 1.0, m_s.minZoom, m_s.maxZoom);
            e.anchor = anchorFor(m_startCentroid);
            e.durationMs = kDoubleTapMs;
            m_camera->easeTo(e);
        }
        m_twoFingerTapCandidate = false;
        finishPinch();
        if (!m_points.empty()) {
            // One finger left: it must not start a pan until the next
            // event-loop pass (QField's pinchReleasing guard).
            Point& rest = m_points.begin()->second;
            rest.start = rest.pos;
            rest.moved = false;
            m_pinchReleasing = true;
            m_mode = TouchMode::Pan;  // continuing from a pinch is never a tap
            if (!m_settlePending) {
                m_settlePending = true;
                QTimer::singleShot(0, this, [this] { settle(); });
            }
            // A pan kind that is live (one- or two-finger) continues with
            // the remaining finger; it ends at the last release.
            m_twoFingerPanning = false;
            m_panSamples.clear();
            return;
        }
        m_twoFingerPanning = false;
    }
    if (m_points.empty()) onLastRelease(pos, t);
}

void GestureController::onLastRelease(const QPointF& pos, qint64 t)
{
    const TouchMode mode = m_mode;
    m_mode = TouchMode::None;
    m_pinchReleasing = false;
    m_timeoutTimer.stop();

    if (mode == TouchMode::Pending && !m_longPressFired) {
        if (m_doubleTapSecondPress) {
            // Double tap: +1 zoom at the point, 300 ms.
            m_doubleTapSecondPress = false;
            m_lastTapPos.reset();
            if (m_s.doubleTapEnabled && m_s.zoomEnabled && m_camera) {
                CameraEase e;
                e.zoom = std::clamp(m_camera->zoom() + 1.0, m_s.minZoom, m_s.maxZoom);
                e.anchor = anchorFor(pos);
                e.durationMs = kDoubleTapMs;
                m_camera->easeTo(e);
            }
        } else if (m_s.doubleTapEnabled) {
            // Hold the tap until the double-tap window closes (350 ms).
            m_pendingTap = pos;
            m_pendingTapAt = t;
            m_lastTapPos = pos;
            m_lastTapAt = t;
            armTimeoutTimer(kTapDisambiguationMs);
        } else {
            emit tapped(pos);
        }
    } else if (mode == TouchMode::QuickZoom) {
        m_doubleTapSecondPress = false;
        m_lastTapPos.reset();
        endKind(QStringLiteral("pinch"));
    } else if (mode == TouchMode::Pan) {
        m_doubleTapSecondPress = false;
        m_lastTapPos.reset();
        if (m_s.flingEnabled && m_s.scrollEnabled) {
            QPointF v = panVelocity(t);
            if (m_camera && m_camera->pitch() > 0 && std::fabs(qRadiansToDegrees(std::atan2(std::fabs(v.y()), std::fabs(v.x())))) > kFlingVerticalOnlyAboveDeg) {
                v.setX(0);  // steep flings on a pitched map move along the view axis only
            }
            const double speed = std::hypot(v.x(), v.y());
            if (speed >= kFlingMinPxPerMs) {
                if (speed > kFlingCapPxPerMs) v *= kFlingCapPxPerMs / speed;
                startPanInertia(v);
            }
        }
        endKind(QStringLiteral("pan"));
    } else {
        m_doubleTapSecondPress = false;
        m_lastTapPos.reset();
    }
    endAllKinds();
    thaw(kTouchFreeze);
}

void GestureController::touchCancelled()
{
    m_points.clear();
    m_mode = TouchMode::None;
    m_pinchReleasing = false;
    m_doubleTapSecondPress = false;
    m_pendingTap.reset();
    m_timeoutTimer.stop();
    m_scaling = m_rotating = m_shoving = m_twoFingerPanning = false;
    endAllKinds();
    thaw(kTouchFreeze);
}

void GestureController::settle()
{
    m_settlePending = false;
    m_pinchReleasing = false;
}

// ── timeouts ──────────────────────────────────────────────────────────────

void GestureController::armTimeoutTimer(int ms)
{
    if (m_inertiaTimer.interval() == 0) return;  // test mode: tick() checks
    m_timeoutTimer.start(ms);
}

void GestureController::checkTimeouts()
{
    const qint64 t = now();
    // Long press: one finger, no movement, 500 ms.
    if (m_points.size() == 1 && m_mode == TouchMode::Pending && !m_longPressFired && !m_doubleTapSecondPress) {
        const Point& pt = m_points.begin()->second;
        if (!pt.moved && t - pt.pressedAt >= kLongPressMs) {
            m_longPressFired = true;
            m_pendingTap.reset();
            m_lastTapPos.reset();
            emit longPressed(pt.pos);
        } else if (!pt.moved) {
            armTimeoutTimer(static_cast<int>(kLongPressMs - (t - pt.pressedAt)));
        }
    }
    // Tap disambiguation: no second press within 350 ms.
    if (m_pendingTap && m_points.empty()) {
        if (t - m_pendingTapAt >= kTapDisambiguationMs) {
            const QPointF p = *m_pendingTap;
            m_pendingTap.reset();
            emit tapped(p);
        } else {
            armTimeoutTimer(static_cast<int>(kTapDisambiguationMs - (t - m_pendingTapAt)));
        }
    }
    // Keyboard OK held: long press at centre.
    if (m_okPressedAt && t - m_okPressedAt >= kLongPressMs) {
        m_okPressedAt = 0;
        emit centrePickRequested(true);
    }
}

// ── velocity sampling and inertia (M8.4) ─────────────────────────────────

void GestureController::samplePan(qint64 t, const QPointF& p)
{
    m_panSamples.push_back({t, p});
    while (!m_panSamples.empty() && t - m_panSamples.front().t > kPanVelocityWindowMs * 2) m_panSamples.pop_front();
}

void GestureController::sampleZoom(qint64 t, double zoom)
{
    m_zoomSamples.push_back({t, QPointF(zoom, 0)});
    while (!m_zoomSamples.empty() && t - m_zoomSamples.front().t > kZoomVelocityWindowMs * 2) m_zoomSamples.pop_front();
}

QPointF GestureController::panVelocity(qint64 t) const
{
    // Oldest sample inside the window vs the newest; >= 2 samples.
    const Sample* first = nullptr;
    for (const Sample& s : m_panSamples) {
        if (t - s.t <= kPanVelocityWindowMs) { first = &s; break; }
    }
    if (!first || m_panSamples.size() < 2) return QPointF();
    const Sample& last = m_panSamples.back();
    const qint64 dt = last.t - first->t;
    if (dt <= 0 || &last == first) return QPointF();
    // Released long after the last move: no fling.
    if (t - last.t > kPanVelocityWindowMs) return QPointF();
    return (last.p - first->p) / static_cast<double>(dt);
}

double GestureController::zoomVelocity(qint64 t) const
{
    const Sample* first = nullptr;
    for (const Sample& s : m_zoomSamples) {
        if (t - s.t <= kZoomVelocityWindowMs) { first = &s; break; }
    }
    if (!first || m_zoomSamples.size() < 2) return 0.0;
    const Sample& last = m_zoomSamples.back();
    const qint64 dt = last.t - first->t;
    if (dt <= 0 || &last == first) return 0.0;
    return (last.p.x() - first->p.x()) / static_cast<double>(dt);
}

void GestureController::startPanInertia(const QPointF& velocity)
{
    m_pan.active = true;
    m_pan.velocity = velocity;
    beginKind(QStringLiteral("fling"));
    freeze(kKineticFreeze);
    if (m_inertiaTimer.interval() > 0) m_inertiaTimer.start();
}

void GestureController::startZoomInertia(double zoomPerMs, const QPointF& anchor)
{
    m_zoomInertia.active = true;
    m_zoomInertia.zoomPerMs = zoomPerMs;
    m_zoomInertia.anchor = anchor;
    freeze(kKineticFreeze);
    if (m_inertiaTimer.interval() > 0) m_inertiaTimer.start();
}

void GestureController::stopInertia()
{
    const bool was = m_pan.active || m_zoomInertia.active;
    m_pan.active = false;
    m_zoomInertia.active = false;
    m_inertiaTimer.stop();
    if (was) {
        endKind(QStringLiteral("fling"));
        thaw(kKineticFreeze);
    }
}

void GestureController::tick(int ms)
{
    // Decay is specified per 16 ms frame; scale it to the step taken.
    const double frames = static_cast<double>(ms) / kInertiaFrameMs;
    const double decay = std::pow(kInertiaDecayPerFrame, frames);
    bool anyActive = false;
    if (m_pan.active && m_camera) {
        const QPointF step = m_pan.velocity * ms;
        if (QPointF::dotProduct(step, step) < 1.0 || !m_s.scrollEnabled) {
            m_pan.active = false;
        } else {
            m_camera->moveBy(step);
            m_pan.velocity *= decay;
            anyActive = true;
        }
    }
    if (m_zoomInertia.active && m_camera) {
        const double delta = m_zoomInertia.zoomPerMs * ms;
        const double perFrameFactor = std::pow(2.0, m_zoomInertia.zoomPerMs * kInertiaFrameMs);
        if (std::fabs(perFrameFactor - 1.0) < 0.001 || !m_s.zoomEnabled) {
            m_zoomInertia.active = false;
        } else {
            applyZoomDelta(delta, m_zoomInertia.anchor);
            m_zoomInertia.zoomPerMs *= decay;
            anyActive = true;
        }
    }
    if (!anyActive && m_frozen.contains(kKineticFreeze)) {
        m_inertiaTimer.stop();
        endKind(QStringLiteral("fling"));
        thaw(kKineticFreeze);
    }
    if (m_inertiaTimer.interval() == 0) checkTimeouts();
}

// ── north behaviour (M8.6) ───────────────────────────────────────────────

void GestureController::northBehaviour()
{
    if (!m_camera) return;
    const double bearing = normalizeDeg(m_camera->bearing());
    if (std::fabs(bearing) <= kNorthLockSlackDeg) {
        m_bearingTouched = false;
        return;
    }
    bool snap = false;
    if (m_s.northLock) {
        snap = true;
    } else if (m_bearingTouched && m_turnTotal < kSmallTurnSnapBackDeg && std::fabs(bearing) < kSmallTurnSnapBackDeg) {
        // A sloppy pinch left the map a few degrees off north.
        snap = true;
    }
    m_bearingTouched = false;
    m_turnTotal = 0;
    if (!snap) return;
    CameraEase e;
    e.bearing = 0.0;
    e.durationMs = kDoubleTapMs;
    m_camera->easeTo(e);
}

// ── pointer class (M8.2 pointer column) ───────────────────────────────────

void GestureController::pointerDragBegan(const QPointF& pos, int button, int modifiers)
{
    stopInertia();
    if (m_camera) m_camera->cancelTransitions();
    const auto mods = static_cast<Qt::KeyboardModifiers>(modifiers);
    const auto btn = static_cast<Qt::MouseButton>(button);
    m_pointerMode = PointerMode::None;
    if (btn == Qt::LeftButton) {
        if (mods & Qt::ShiftModifier) m_pointerMode = PointerMode::Tilt;
        else if (mods & Qt::ControlModifier) m_pointerMode = PointerMode::Rotate;
        else m_pointerMode = PointerMode::Pan;
    } else if (btn == Qt::MiddleButton) {
        m_pointerMode = (mods & Qt::ShiftModifier) ? PointerMode::Rotate : PointerMode::Pan;
    } else if (btn == Qt::RightButton) {
        m_pointerMode = PointerMode::Zoom;
    }
    if (m_pointerMode == PointerMode::None) return;
    m_pointerLast = pos;
    m_panSamples.clear();
    m_turnTotal = 0;
    m_bearingTouched = false;
    freeze(kPointerFreeze);
    switch (m_pointerMode) {
    case PointerMode::Pan: if (m_s.scrollEnabled) beginKind(QStringLiteral("pan")); samplePan(now(), pos); break;
    case PointerMode::Tilt: if (m_s.tiltEnabled) beginKind(QStringLiteral("shove")); break;
    case PointerMode::Rotate: if (m_s.rotateEnabled && !m_s.northLock) beginKind(QStringLiteral("rotate")); break;
    case PointerMode::Zoom: if (m_s.zoomEnabled) beginKind(QStringLiteral("pinch")); break;
    case PointerMode::None: break;
    }
}

void GestureController::pointerDragMoved(const QPointF& pos)
{
    if (m_pointerMode == PointerMode::None || !m_camera) return;
    const QPointF d = pos - m_pointerLast;
    m_pointerLast = pos;
    const QPointF centre(m_camera->size().width() / 2.0, m_camera->size().height() / 2.0);
    switch (m_pointerMode) {
    case PointerMode::Pan:
        samplePan(now(), pos);
        if (m_s.scrollEnabled) m_camera->moveBy(d);
        break;
    case PointerMode::Tilt:
        if (m_s.tiltEnabled) applyPitchDelta(-d.y() * kTiltDegPerPx);
        break;
    case PointerMode::Rotate:
        if (m_s.rotateEnabled && !m_s.northLock) {
            // Rotate around the centre: angle swept by the pointer.
            const double a0 = lineAngleDeg(centre, pos - d);
            const double a1 = lineAngleDeg(centre, pos);
            const double delta = normalizeDeg(a1 - a0);
            m_turnTotal += std::fabs(delta);
            applyBearingDelta(-delta, centre);
        }
        break;
    case PointerMode::Zoom:
        // Right-drag: vertical distance zooms, 100 px per level.
        if (m_s.zoomEnabled) applyZoomDelta(-d.y() / 100.0, centre);
        break;
    case PointerMode::None: break;
    }
}

void GestureController::pointerDragEnded(const QPointF& pos)
{
    if (m_pointerMode == PointerMode::None) return;
    const qint64 t = now();
    if (m_pointerMode == PointerMode::Pan && m_s.flingEnabled && m_s.scrollEnabled) {
        samplePan(t, pos);
        QPointF v = panVelocity(t);
        const double speed = std::hypot(v.x(), v.y());
        if (speed >= kFlingMinPxPerMs) {
            if (speed > kFlingCapPxPerMs) v *= kFlingCapPxPerMs / speed;
            startPanInertia(v);
        }
    }
    m_pointerMode = PointerMode::None;
    endKind(QStringLiteral("pan"));
    endKind(QStringLiteral("shove"));
    endKind(QStringLiteral("rotate"));
    endKind(QStringLiteral("pinch"));
    thaw(kPointerFreeze);
}

void GestureController::wheel(const QPointF& pos, double angleDeltaY, double pixelDeltaY, int modifiers)
{
    if (!m_camera || !m_s.zoomEnabled) return;
    stopInertia();
    const auto mods = static_cast<Qt::KeyboardModifiers>(modifiers);
    // Continuous: one notch (120) is one zoom level; touchpads deliver
    // pixel deltas, 1 level per 120 px of scroll.
    double delta = angleDeltaY != 0.0 ? angleDeltaY / 120.0 : pixelDeltaY / 120.0;
    if (mods & Qt::ShiftModifier) delta *= 0.25;  // fine zoom
    applyZoomDelta(delta, pos);
}

void GestureController::clicked(const QPointF& pos, int button)
{
    const auto btn = static_cast<Qt::MouseButton>(button);
    if (btn == Qt::RightButton) emit contextRequested(pos);
    else if (btn == Qt::LeftButton) emit tapped(pos);
}

void GestureController::doubleClicked(const QPointF& pos, int button)
{
    if (static_cast<Qt::MouseButton>(button) != Qt::LeftButton || !m_camera || !m_s.doubleTapEnabled || !m_s.zoomEnabled) return;
    CameraEase e;
    e.zoom = std::clamp(m_camera->zoom() + 1.0, m_s.minZoom, m_s.maxZoom);
    e.anchor = anchorFor(pos);
    e.durationMs = kDoubleTapMs;
    m_camera->easeTo(e);
}

void GestureController::pointerLongPressed(const QPointF& pos) { emit longPressed(pos); }

// ── keyboard / D-pad (M8.8) ───────────────────────────────────────────────

bool GestureController::keyPressed(int key, int modifiers)
{
    if (!m_camera) return false;
    const auto mods = static_cast<Qt::KeyboardModifiers>(modifiers);
    const QSizeF size = m_camera->size();
    const QPointF centre(size.width() / 2.0, size.height() / 2.0);
    const double panX = size.width() * kKeyboardPanFraction;
    const double panY = size.height() * kKeyboardPanFraction;
    const bool shift = mods & Qt::ShiftModifier;
    auto rotate = [&](double deg) {
        if (!m_s.rotateEnabled || m_s.northLock) return;
        applyBearingDelta(deg, centre);
        emit keyboardMoved(QStringLiteral("rotate"));
    };
    auto tilt = [&](double deg) {
        if (!m_s.tiltEnabled) return;
        applyPitchDelta(deg);
        emit keyboardMoved(QStringLiteral("tilt"));
    };
    auto pan = [&](const QPointF& by) {
        if (!m_s.scrollEnabled) return;
        m_camera->moveBy(by);
        emit keyboardMoved(QStringLiteral("pan"));
    };
    switch (key) {
    case Qt::Key_Left:
        if (shift) rotate(-kKeyboardRotateDeg); else pan(QPointF(panX, 0));
        return true;
    case Qt::Key_Right:
        if (shift) rotate(kKeyboardRotateDeg); else pan(QPointF(-panX, 0));
        return true;
    case Qt::Key_Up:
        if (shift) tilt(kKeyboardTiltDeg); else pan(QPointF(0, panY));
        return true;
    case Qt::Key_Down:
        if (shift) tilt(-kKeyboardTiltDeg); else pan(QPointF(0, -panY));
        return true;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        // M8.22: Shift doubles the step (gl-js keyboard handler); eased.
        zoomStep(shift ? 2.0 : 1.0, kKeyboardZoomMs);
        return true;
    case Qt::Key_Minus:
        zoomStep(shift ? -2.0 : -1.0, kKeyboardZoomMs);
        return true;
    case Qt::Key_Escape:
        emit escapePressed();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Select:
    case Qt::Key_Space:
        if (!m_okPressedAt) {
            m_okPressedAt = now();
            armTimeoutTimer(kLongPressMs);
        }
        return true;
    default:
        return false;
    }
}

bool GestureController::keyReleased(int key, int modifiers)
{
    Q_UNUSED(modifiers);
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Select:
    case Qt::Key_Space:
        if (m_okPressedAt) {
            m_okPressedAt = 0;
            emit centrePickRequested(false);
        }
        return true;
    default:
        return false;
    }
}

}  // namespace maps::view
