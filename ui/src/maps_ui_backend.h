#pragma once

#include "rep_maps_ui_source.h"
#include "logos_ui_plugin_context.h"

/**
 * The Maps view backend. Runs in its own process (ui-host); the QML view in
 * qml/Main.qml talks to it through the Qt Remote Objects replica generated
 * from maps_ui.rep.
 *
 * It owns no map state: maps_core does (so other modules can add layers
 * programmatically and this view reflects them). The backend forwards each
 * slot to the typed `modules().maps_core` client and relays the core's
 * layerAdded / layerChanged / layerRemoved events to the view as the
 * `layersChanged` signal plus the `layerCount` PROP.
 *
 * Derives:
 *   - MapsUiSimpleSource   -- generated from the .rep; implement its slots,
 *     feed its PROPs (setLayerCount(), setStatus()), emit its signals.
 *   - LogosUiPluginContext -- onContextReady() plus modules(): Qt-typed
 *     callers and event subscriptions for metadata.json "dependencies".
 */
class MapsUiBackend : public MapsUiSimpleSource,
                      public LogosUiPluginContext
{
public:
    bool addLayer(QString layerId, QString layerJson) override;
    bool removeLayer(QString layerId) override;
    bool setFeatures(QString layerId, QString geojson) override;
    QString getLayer(QString layerId) override;
    QString getFeatures(QString layerId) override;
    QString listLayers() override;

    /// Fires when ui-host hands the plugin its LogosAPI: the typed
    /// dependency surface is live, so subscriptions are armed here.
    void onContextReady() override;

private:
    /// Re-read the core's state and tell the view, on a later event-loop
    /// turn. Calling back into a module synchronously from inside one of its
    /// event callbacks (or from onContextReady) blocks the dispatcher until
    /// the 30 s call timeout and then yields a default, so the call is always
    /// deferred: 0 ms from an event callback, ~1 s from onContextReady.
    void scheduleRefresh(const QString& what, int delayMs = 0);
    void refresh(const QString& what);
};
