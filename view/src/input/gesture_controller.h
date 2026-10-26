// GestureController: the interaction policy of docs/architecture.md section 8
// (M8.1-M8.11, Appendix B) as one clean-room detector.
//
// Raw input comes in as touch points (pressed/moved/released, logical px) and
// as pointer-class events (drag, wheel, click, keys); the controller decides
// what each sequence means -- pan, pinch, rotate, shove, two-finger pan,
// tap/double-tap/long-press, quick zoom, two-finger tap, fling -- applies
// the Appendix B thresholds, drives a CameraTarget and reports gesture kinds
// for follow-camera policies (M8.7). Qt's handlers cannot expose individual
// touch points, so MapWindow.qml feeds them from a MultiPointTouchArea; the
// pointer stack uses DragHandler/WheelHandler/TapHandler per M8.1.
//
// Time is injectable (fake clock + tick()) so the section 16.4 matrix runs
// deterministically; in production a 16 ms timer drives inertia (M8.4).
#pragma once

#include "input/camera_target.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QTimer>

#include <deque>
#include <functional>
#include <map>
#include <optional>

namespace maps::view {

// Appendix B normative constants (dp == logical px under Qt).
namespace gesture {
constexpr double kMoveThresholdTouchDp = 4.0;
constexpr double kMoveThresholdPointerPx = 5.0;
constexpr double kRotateStartDeg = 3.0;
constexpr double kRotateStartWhilePinchingDeg = 15.0;
constexpr double kScaleSpanThresholdDp = 6.0;      // mapbox_defaultScaleSpanSinceStartThreshold
constexpr double kScaleMinSpeedDpPerFrame = 0.15;  // Vela: a slow pinch must zoom
constexpr double kShoveStartDp = 20.0;
constexpr double kShoveMaxFingerAngleDeg = 55.0;
constexpr double kTiltDegPerPx = 0.1;
constexpr double kTwoFingerPanDp = 44.0;
constexpr double kDefaultMaxPitchDeg = 60.0;
constexpr double kAbsoluteMaxPitchDeg = 70.0;
constexpr double kMinZoom = 0.0;
constexpr double kMaxZoom = 25.5;
constexpr int kDoubleTapMs = 300;
constexpr int kTapDisambiguationMs = 350;
constexpr int kLongPressMs = 500;
constexpr double kQuickZoomBase = 0.8;
constexpr double kQuickZoomPxPerStep = 60.0;
constexpr double kQuickZoomMaxLevels = 4.0;
constexpr double kFlingMinPxPerMs = 0.2;
constexpr double kFlingCapPxPerMs = 5.0;
constexpr int kPanVelocityWindowMs = 100;
constexpr int kZoomVelocityWindowMs = 150;
constexpr double kInertiaDecayPerFrame = 0.92;
constexpr int kInertiaFrameMs = 16;
constexpr double kFlingVerticalOnlyAboveDeg = 75.0;
constexpr int kVelocityAnimationCancelMs = 150;
constexpr double kScaleVelocityMaxZoomDelta = 2.5;
constexpr double kNorthLockSlackDeg = 0.05;
constexpr double kSmallTurnSnapBackDeg = 12.0;
constexpr double kKeyboardPanFraction = 0.18;
constexpr double kKeyboardRotateDeg = 15.0;
constexpr double kKeyboardTiltDeg = 10.0;
constexpr int kKeyboardZoomMs = 300;     // M8.22 (gl-js keyboard ease)
constexpr int kZoomButtonMs = 500;       // M8.17 (gl-js NavigationControl easeTo default)
constexpr double kPickBoxDp = 24.0;
}  // namespace gesture

class GestureController : public QObject {
    Q_OBJECT
    // M8.10 settings surface.
    Q_PROPERTY(bool rotateEnabled READ rotateEnabled WRITE setRotateEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool tiltEnabled READ tiltEnabled WRITE setTiltEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool zoomEnabled READ zoomEnabled WRITE setZoomEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool scrollEnabled READ scrollEnabled WRITE setScrollEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool doubleTapEnabled READ doubleTapEnabled WRITE setDoubleTapEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool quickZoomEnabled READ quickZoomEnabled WRITE setQuickZoomEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool twoFingerTapEnabled READ twoFingerTapEnabled WRITE setTwoFingerTapEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool flingEnabled READ flingEnabled WRITE setFlingEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool northLock READ northLock WRITE setNorthLock NOTIFY settingsChanged)
    Q_PROPERTY(bool rotateWhileScaling READ rotateWhileScaling WRITE setRotateWhileScaling NOTIFY settingsChanged)
    Q_PROPERTY(bool mouseAsTouchScreen READ mouseAsTouchScreen WRITE setMouseAsTouchScreen NOTIFY settingsChanged)
    Q_PROPERTY(double maxPitch READ maxPitch WRITE setMaxPitch NOTIFY settingsChanged)
    Q_PROPERTY(double minZoom READ minZoom WRITE setMinZoom NOTIFY settingsChanged)
    Q_PROPERTY(double maxZoom READ maxZoom WRITE setMaxZoom NOTIFY settingsChanged)
    Q_PROPERTY(QPointF focalPoint READ focalPoint WRITE setFocalPoint NOTIFY settingsChanged)
    Q_PROPERTY(bool gestureInProgress READ gestureInProgress NOTIFY gestureInProgressChanged)

public:
    explicit GestureController(QObject* parent = nullptr);
    ~GestureController() override;

    /// The camera to drive; may be null (events are then dropped).
    void setCamera(CameraTarget* camera);
    CameraTarget* camera() const { return m_camera; }

    /// Testing: replace the clock (ms) and stop the inertia timer so tick()
    /// is the only source of time.
    void setClock(std::function<qint64()> clock);
    void setInertiaTimerEnabled(bool enabled);

    // ── settings (M8.10) ────────────────────────────────────────────────
    bool rotateEnabled() const { return m_s.rotateEnabled; }
    void setRotateEnabled(bool v);
    bool tiltEnabled() const { return m_s.tiltEnabled; }
    void setTiltEnabled(bool v);
    bool zoomEnabled() const { return m_s.zoomEnabled; }
    void setZoomEnabled(bool v);
    bool scrollEnabled() const { return m_s.scrollEnabled; }
    void setScrollEnabled(bool v);
    bool doubleTapEnabled() const { return m_s.doubleTapEnabled; }
    void setDoubleTapEnabled(bool v);
    bool quickZoomEnabled() const { return m_s.quickZoomEnabled; }
    void setQuickZoomEnabled(bool v);
    bool twoFingerTapEnabled() const { return m_s.twoFingerTapEnabled; }
    void setTwoFingerTapEnabled(bool v);
    bool flingEnabled() const { return m_s.flingEnabled; }
    void setFlingEnabled(bool v);
    bool northLock() const { return m_s.northLock; }
    void setNorthLock(bool v);
    bool rotateWhileScaling() const { return m_s.rotateWhileScaling; }
    void setRotateWhileScaling(bool v);
    bool mouseAsTouchScreen() const { return m_s.mouseAsTouchScreen; }
    void setMouseAsTouchScreen(bool v);
    double maxPitch() const { return m_s.maxPitch; }
    void setMaxPitch(double v);
    double minZoom() const { return m_s.minZoom; }
    void setMinZoom(double v);
    double maxZoom() const { return m_s.maxZoom; }
    void setMaxZoom(double v);
    QPointF focalPoint() const { return m_s.focalPoint; }  // NaN,NaN = none
    void setFocalPoint(const QPointF& p);
    bool hasFocalPoint() const;

    /// Apply the M8.10 keys found in a setViewOptions object; returns the
    /// keys consumed. Unknown keys are left for other consumers.
    QStringList applyOptions(const QJsonObject& options);
    QJsonObject settingsJson() const;
    static const QStringList& optionKeys();

    // ── state ───────────────────────────────────────────────────────────
    bool gestureInProgress() const { return !m_frozen.isEmpty(); }
    QStringList frozenIds() const;
    bool inertiaActive() const { return m_pan.active || m_zoomInertia.active; }
    QString currentTouchMode() const;  // none|pending|pan|pinch|shove|quickZoom (diagnostics)

    // ── touch input (logical px, item coordinates) ──────────────────────
    /// Points that change together (one touch event) must be delivered
    /// inside a batch so the detector sees the new span/angle/centroid at
    /// once rather than one finger at a time.
    Q_INVOKABLE void touchBatchBegin();
    Q_INVOKABLE void touchBatchEnd();
    Q_INVOKABLE void touchPressed(int id, const QPointF& pos);
    Q_INVOKABLE void touchMoved(int id, const QPointF& pos);
    Q_INVOKABLE void touchReleased(int id, const QPointF& pos);
    Q_INVOKABLE void touchCancelled();

    // ── pointer input (mouse / touchpad / stylus) ───────────────────────
    /// button: Qt::MouseButton, modifiers: Qt::KeyboardModifiers (as int for QML).
    Q_INVOKABLE void pointerDragBegan(const QPointF& pos, int button, int modifiers);
    Q_INVOKABLE void pointerDragMoved(const QPointF& pos);
    Q_INVOKABLE void pointerDragEnded(const QPointF& pos);
    Q_INVOKABLE void wheel(const QPointF& pos, double angleDeltaY, double pixelDeltaY, int modifiers);
    Q_INVOKABLE void clicked(const QPointF& pos, int button);
    Q_INVOKABLE void doubleClicked(const QPointF& pos, int button);
    Q_INVOKABLE void pointerLongPressed(const QPointF& pos);
    /// M8.8 keyboard / D-pad. Returns true when consumed.
    Q_INVOKABLE bool keyPressed(int key, int modifiers);
    Q_INVOKABLE bool keyReleased(int key, int modifiers);
    /// M8.17 / M8.22: zoom by `levels` about the padded centre (or the focal
    /// point) with an ease -- the one path the zoom buttons and +/- share.
    /// Returns false when the zoom is disabled or already at the limit.
    Q_INVOKABLE bool zoomStep(double levels, int durationMs);
    bool canZoomIn() const;
    bool canZoomOut() const;

    // ── time ────────────────────────────────────────────────────────────
    /// Advance inertia and timeouts by `ms` (the 16 ms timer calls tick(16)).
    Q_INVOKABLE void tick(int ms);
    /// Run the pending event-loop-pass work now (pinch-release guard).
    Q_INVOKABLE void settle();
    /// Stop all inertia immediately (any press does this too, M8.4).
    Q_INVOKABLE void stopInertia();

    // ── M8.5 freeze ref-counting (public so the view can freeze for intents) ─
    void freeze(const QString& id);
    void thaw(const QString& id);

signals:
    void settingsChanged();
    void gestureInProgressChanged(bool inProgress);
    void gestureBegan(const QString& kind);  // pan|pinch|rotate|shove|fling
    void gestureEnded(const QString& kind);
    void tapped(const QPointF& pos);
    void longPressed(const QPointF& pos);
    void contextRequested(const QPointF& pos);
    /// All frozen ids released and inertia at rest (north behaviour ran).
    void idle();
    /// D-pad / keyboard OK: pick at the view centre (long = long press).
    void centrePickRequested(bool longPress);
    /// A keyboard step moved the camera: kind is pan|rotate|tilt|zoom. The
    /// follow policy (M8.18) treats pan/rotate like the touch gestures.
    void keyboardMoved(const QString& kind);
    /// Escape (M8.22): the chrome decides what it closes.
    void escapePressed();

private:
    struct Settings {
        bool rotateEnabled = true, tiltEnabled = true, zoomEnabled = true, scrollEnabled = true;
        bool doubleTapEnabled = true, quickZoomEnabled = true, twoFingerTapEnabled = true, flingEnabled = true;
        bool northLock = false, rotateWhileScaling = true, mouseAsTouchScreen = false;
        double maxPitch = gesture::kDefaultMaxPitchDeg;
        double minZoom = gesture::kMinZoom, maxZoom = gesture::kMaxZoom;
        QPointF focalPoint{qQNaN(), qQNaN()};
    };
    struct Point {
        QPointF start, pos, last;
        qint64 pressedAt = 0;
        bool moved = false;
        bool dirty = false;  // changed in the current batch
    };
    struct Sample {
        qint64 t;
        QPointF p;   // position (pan) or (value, 0) for scalar series
    };
    struct Inertia {
        bool active = false;
        QPointF velocity;   // px per ms (pan)
        double zoomPerMs = 0;
        QPointF anchor;
    };
    enum class TouchMode { None, Pending, Pan, Pinch, Shove, QuickZoom };
    enum class PointerMode { None, Pan, Tilt, Rotate, Zoom };

    qint64 now() const;
    void beginKind(const QString& kind);
    void endKind(const QString& kind);
    void endAllKinds();
    void onFirstPress();
    void onLastRelease(const QPointF& pos, qint64 t);
    void processTouch();
    void updateSinglePan(Point& pt, const QPointF& prev);
    void updateTwoFinger();
    void finishPinch();
    void startPanInertia(const QPointF& velocity);
    void startZoomInertia(double zoomPerMs, const QPointF& anchor);
    QPointF panVelocity(qint64 t) const;
    double zoomVelocity(qint64 t) const;
    void samplePan(qint64 t, const QPointF& p);
    void sampleZoom(qint64 t, double zoom);
    void checkTimeouts();
    void applyZoomDelta(double delta, const QPointF& anchor);
    void applyPitchDelta(double delta);
    void applyBearingDelta(double delta, const QPointF& anchor);
    QPointF anchorFor(const QPointF& centroid) const;
    void northBehaviour();
    void maybeIdle();
    void armTimeoutTimer(int ms);
    static double normalizeDeg(double d);
    void changeSetting(bool& field, bool v);

    CameraTarget* m_camera = nullptr;
    Settings m_s;
    std::function<qint64()> m_clock;
    QElapsedTimer m_elapsed;
    QTimer m_inertiaTimer;
    QTimer m_timeoutTimer;
    QSet<QString> m_frozen;
    QSet<QString> m_activeKinds;

    // touch
    std::map<int, Point> m_points;
    int m_batchDepth = 0;
    bool m_batchDirty = false;
    TouchMode m_mode = TouchMode::None;
    bool m_pinchReleasing = false;
    bool m_settlePending = false;
    bool m_scaling = false, m_rotating = false, m_shoving = false, m_twoFingerPanning = false;
    double m_startSpan = 0, m_lastSpan = 0, m_startAngle = 0, m_lastAngle = 0;
    QPointF m_startCentroid, m_lastCentroid;
    double m_gestureStartBearing = 0;
    double m_turnTotal = 0;
    double m_zoomAtGestureStart = 0;
    bool m_bearingTouched = false;
    bool m_twoFingerTapCandidate = false;
    qint64 m_secondPressAt = 0;
    qint64 m_lastMoveAt = 0;
    std::optional<QPointF> m_pendingTap;  // waiting for the 350 ms disambiguation
    qint64 m_pendingTapAt = 0;
    std::optional<QPointF> m_lastTapPos;  // for double-tap detection
    qint64 m_lastTapAt = 0;
    bool m_doubleTapSecondPress = false;  // second press of a double tap in progress
    QPointF m_quickZoomAnchor;
    double m_quickZoomStartY = 0;
    bool m_longPressFired = false;
    std::deque<Sample> m_panSamples;
    std::deque<Sample> m_zoomSamples;
    Inertia m_pan;
    Inertia m_zoomInertia;

    // pointer
    PointerMode m_pointerMode = PointerMode::None;
    QPointF m_pointerLast;
    bool m_pointerPanning = false;

    // keyboard
    qint64 m_okPressedAt = 0;
};

}  // namespace maps::view
