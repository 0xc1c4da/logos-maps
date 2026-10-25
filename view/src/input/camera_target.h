// CameraTarget: the camera the gesture controller drives (docs/architecture.md
// section 8). The real implementation wraps QMapLibre::Map; the section 16.4
// matrix drives a recording mock. All positions are logical pixels in the
// map item's coordinate space; angles are degrees.
#pragma once

#include <QPointF>
#include <QSizeF>

#include <optional>

namespace maps::view {

struct CameraEase {
    std::optional<double> zoom;
    std::optional<double> bearing;
    std::optional<double> pitch;
    std::optional<QPointF> anchor;  // keep this screen point fixed (zoom)
    int durationMs = 300;
};

class CameraTarget {
public:
    virtual ~CameraTarget() = default;

    virtual double zoom() const = 0;
    virtual double bearing() const = 0;  // degrees clockwise from north, (-180, 180]
    virtual double pitch() const = 0;
    virtual QSizeF size() const = 0;

    /// Immediate (gesture-driven) changes.
    virtual void moveBy(const QPointF& offsetPx) = 0;
    virtual void scaleBy(double factor, const QPointF& anchorPx) = 0;
    virtual void setBearing(double degrees, const QPointF& anchorPx) = 0;
    virtual void setPitch(double degrees) = 0;
    virtual void setZoom(double zoom, const QPointF& anchorPx) = 0;

    /// Animated change (double-tap zoom, north snap, two-finger tap).
    virtual void easeTo(const CameraEase& ease) = 0;
    virtual void cancelTransitions() = 0;

    /// M8.5: the engine defers expensive work while set.
    virtual void setGestureInProgress(bool inProgress) = 0;
};

}  // namespace maps::view
