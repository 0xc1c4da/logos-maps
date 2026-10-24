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
    void refresh(const QString& what);
};
