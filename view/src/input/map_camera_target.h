// MapCameraTarget: CameraTarget over QMapLibre::Map for the item it renders.
#pragma once

#include "input/camera_target.h"

#include <QMapLibre/Map>

#include <QMapLibre/Types>

#include <QPointer>
#include <QQuickItem>
#include <QVariant>

#include <cmath>

namespace maps::view {

class MapCameraTarget final : public CameraTarget {
public:
    MapCameraTarget(QMapLibre::Map* map, QQuickItem* item) : m_map(map), m_item(item) {}

    void setMap(QMapLibre::Map* map) { m_map = map; }

    double zoom() const override { return m_map ? m_map->zoom() : 0.0; }
    double bearing() const override { return m_map ? m_map->bearing() : 0.0; }
    double pitch() const override { return m_map ? m_map->pitch() : 0.0; }
    QSizeF size() const override { return m_item ? QSizeF(m_item->width(), m_item->height()) : QSizeF(); }

    void moveBy(const QPointF& offsetPx) override { if (m_map) m_map->moveBy(offsetPx); }
    void scaleBy(double factor, const QPointF& anchorPx) override { if (m_map) m_map->scaleBy(factor, anchorPx); }
    void setBearing(double degrees, const QPointF& anchorPx) override { if (m_map) m_map->setBearing(degrees, anchorPx); }
    void setPitch(double degrees) override { if (m_map) m_map->setPitch(degrees); }
    void setZoom(double zoom, const QPointF& anchorPx) override
    {
        if (!m_map) return;
        m_map->scaleBy(std::pow(2.0, zoom - m_map->zoom()), anchorPx);
    }

    void easeTo(const CameraEase& ease) override
    {
        if (!m_map) return;
        QMapLibre::CameraOptions camera;
        if (ease.zoom) camera.zoom = *ease.zoom;
        if (ease.bearing) camera.bearing = *ease.bearing;
        if (ease.pitch) camera.pitch = *ease.pitch;
        if (ease.anchor) camera.anchor = *ease.anchor;
        m_map->easeTo(camera, QMapLibre::AnimationOptions(ease.durationMs));
    }
    void cancelTransitions() override
    {
        // The binding has no cancel; jumping to the current camera ends any
        // transition (same device as camera_intents.cpp `cancel`).
        if (!m_map) return;
        QMapLibre::CameraOptions camera;
        camera.center = QVariant::fromValue(QMapLibre::Coordinate(m_map->latitude(), m_map->longitude()));
        camera.zoom = m_map->zoom();
        camera.bearing = m_map->bearing();
        camera.pitch = m_map->pitch();
        m_map->jumpTo(camera);
    }

    void setGestureInProgress(bool inProgress) override { if (m_map) m_map->setGestureInProgress(inProgress); }

private:
    QMapLibre::Map* m_map;
    QPointer<QQuickItem> m_item;
};

}  // namespace maps::view
