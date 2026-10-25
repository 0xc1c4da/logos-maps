#include "maps_view_backend.h"

#include <QTimer>

// Generated umbrella for metadata.json "dependencies": LogosModules behind
// modules(), with the Qt-typed maps_core client and its event accessors.
#include "logos_sdk.h"

bool MapsViewBackend::addLayer(QString layerId, QString layerJson)
{
    return modules().maps_core.addLayer(layerId, layerJson);
}

bool MapsViewBackend::removeLayer(QString layerId)
{
    return modules().maps_core.removeLayer(layerId);
}

bool MapsViewBackend::setFeatures(QString layerId, QString geojson)
{
    return modules().maps_core.setFeatures(layerId, geojson);
}

QString MapsViewBackend::getLayer(QString layerId)
{
    return modules().maps_core.getLayer(layerId);
}

QString MapsViewBackend::getFeatures(QString layerId)
{
    return modules().maps_core.getFeatures(layerId);
}

QString MapsViewBackend::listLayers()
{
    return modules().maps_core.listLayers();
}

void MapsViewBackend::onContextReady()
{
    // Typed core events -> view. QtRO pushes every PROP change and signal to
    // the QML replica; the view refetches listLayers() on layersChanged.
    // Only subscriptions here: no call into the core from this method or
    // from inside a callback (see scheduleRefresh).
    modules().maps_core.onLayerAdded([this](const QString& id) { scheduleRefresh(QStringLiteral("added %1").arg(id)); });
    modules().maps_core.onLayerChanged([this](const QString& id) { scheduleRefresh(QStringLiteral("changed %1").arg(id)); });
    modules().maps_core.onLayerRemoved([this](const QString& id) { scheduleRefresh(QStringLiteral("removed %1").arg(id)); });
    scheduleRefresh(QStringLiteral("connected to maps_core"), 1000);
}

void MapsViewBackend::scheduleRefresh(const QString& what, int delayMs)
{
    QTimer::singleShot(delayMs, this, [this, what] { refresh(what); });
}

void MapsViewBackend::refresh(const QString& what)
{
    setLayerCount(static_cast<int>(modules().maps_core.layerCount()));
    setStatus(what);
    emit layersChanged();
}
