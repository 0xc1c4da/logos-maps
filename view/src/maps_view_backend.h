#pragma once

#include "rep_maps_view_source.h"
#include "logos_ui_plugin_context.h"

#include <QTimer>

#include <memory>

namespace maps::view {
class SdkCoreClient;
class ViewController;
}

/**
 * The Maps view backend (docs/architecture.md section 12.2).
 *
 * Runs in its own ui-host process. Unlike a QML-replica view it owns a real
 * top-level QQuickWindow (RFC-1) that the host composites through
 * Logos.Surface (RFC-2/3); the replica generated from maps_view.rep is the
 * narrow control channel the host uses: size, options, camera intents and
 * the session name in, view state / picks / attribution out.
 *
 * It owns no scene state: maps_core does. Everything the window shows is
 * pulled from the core by the ViewController, which this class wires to the
 * typed `modules().maps_core` client (SdkCoreClient).
 *
 * Derives:
 *   - MapsViewSimpleSource  -- generated from the .rep; slots, PROP setters,
 *     signals.
 *   - LogosUiPluginContext  -- onContextReady() plus modules().
 */
class MapsViewBackend : public MapsViewSimpleSource,
                        public LogosUiPluginContext
{
public:
    MapsViewBackend();
    ~MapsViewBackend() override;

    void setSize(int w, int h, double dpr) override;
    void setOptions(QString json) override;
    void camera(QString intentJson) override;
    void setSession(QString session) override;

    /// Fires when ui-host hands the plugin its LogosAPI.
    void onContextReady() override;
    LogosShutdown aboutToUnload() override;

    maps::view::ViewController* controller() const { return m_controller.get(); }

private:
    /// Calling back into a module synchronously from inside one of its event
    /// callbacks (or from onContextReady) blocks the dispatcher; the view
    /// starts on a later event-loop turn (M12.9).
    void startDeferred(int delayMs);
    void start();

    std::unique_ptr<maps::view::SdkCoreClient> m_core;
    std::unique_ptr<maps::view::ViewController> m_controller;
    QString m_pendingSession;
    QString m_pendingOptions;
    QTimer m_startTimer;
};
