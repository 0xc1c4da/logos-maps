#include "maps_view_backend.h"

#include "logos_sdk.h"
#include "sdk_core_client.h"
#include "view/view_controller.h"
#include "view/view_window.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonDocument>

MapsViewBackend::MapsViewBackend()
{
    m_startTimer.setSingleShot(true);
    connect(&m_startTimer, &QTimer::timeout, this, &MapsViewBackend::start);
    setStatus(QStringLiteral("starting"));
}

MapsViewBackend::~MapsViewBackend() = default;

void MapsViewBackend::onContextReady()
{
    m_core = std::make_unique<maps::view::SdkCoreClient>(modules().maps_core, modules().api, this);
    m_core->subscribe();
    // The host's RFC-3 setSession may still be on its way; give it a turn.
    startDeferred(250);
}

void MapsViewBackend::startDeferred(int delayMs)
{
    if (m_controller) return;
    m_startTimer.start(delayMs);
}

void MapsViewBackend::start()
{
    if (m_controller || !m_core) return;
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        // Pre-RFC-1 ui-host: no QGuiApplication, so no window can exist.
        setStatus(QStringLiteral("error: ui-host was not started with --gui (RFC-1); maps_view needs a QGuiApplication"));
        qWarning("maps_view: no QGuiApplication; the view cannot create its surface window");
        return;
    }
    maps::view::ViewController::Options options;
    options.moduleName = QStringLiteral("maps_view");
    if (!m_pendingSession.isEmpty()) options.session = m_pendingSession;
    options.moduleDir = maps::view::ViewWindow::pluginDirectory();
    // RFC-8: the runtime names the transport ("wayland", "channel-shm", ...).
    // A ui-host that predates the API publishes nothing; if it still handed
    // us a surface token the window it composites is a Wayland one.
    {
        const QString reported = maps::view::ViewWindow::hostTransport();
        if (reported != QLatin1String("none")) options.surfaceTransport = reported;
        else if (!maps::view::ViewController::surfaceTokenFromProcess().isEmpty()) options.surfaceTransport = QStringLiteral("wayland");
    }
    m_controller = std::make_unique<maps::view::ViewController>(m_core.get(), options, this);

    auto* c = m_controller.get();
    connect(c, &maps::view::ViewController::statusChanged, this, [this](const QString& s) {
        setStatus(s == QLatin1String("open") ? QStringLiteral("ready") : s);
    });
    connect(c, &maps::view::ViewController::viewIdChanged, this, &MapsViewBackend::setViewId);
    connect(c, &maps::view::ViewController::viewStateChanged, this, &MapsViewBackend::setViewStateJson);
    connect(c, &maps::view::ViewController::capabilitiesChanged, this, &MapsViewBackend::setCapabilitiesJson);
    connect(c, &maps::view::ViewController::cameraIdle, this, &MapsViewBackend::cameraIdle);
    connect(c, &maps::view::ViewController::featurePicked, this, &MapsViewBackend::featurePicked);
    connect(c, &maps::view::ViewController::attributionChanged, this, &MapsViewBackend::attributionChanged);
    connect(c, &maps::view::ViewController::shareRequested, this, &MapsViewBackend::shareRequested);
    connect(c, &maps::view::ViewController::error, this, [this](const QString& code, const QString& message) {
        qWarning("maps_view: %s %s", qPrintable(code), qPrintable(message));
        if (code == QLatin1String("maps.internal")) setStatus(QStringLiteral("error: ") + message);
    });
    setSurfaceToken(c->surfaceToken());
    c->start();
    if (!m_pendingOptions.isEmpty()) {
        c->setOptions(m_pendingOptions);
        m_pendingOptions.clear();
    }
}

void MapsViewBackend::setSize(int w, int h, double dpr)
{
    if (m_controller) m_controller->setSize(w, h, dpr);
}

void MapsViewBackend::setOptions(QString json)
{
    if (m_controller) {
        m_controller->setOptions(json);
        return;
    }
    // Merge until the controller exists.
    QJsonObject merged = QJsonDocument::fromJson(m_pendingOptions.toUtf8()).object();
    const QJsonObject incoming = QJsonDocument::fromJson(json.toUtf8()).object();
    for (auto it = incoming.begin(); it != incoming.end(); ++it) merged.insert(it.key(), it.value());
    m_pendingOptions = QString::fromUtf8(QJsonDocument(merged).toJson(QJsonDocument::Compact));
}

void MapsViewBackend::camera(QString intentJson)
{
    if (m_controller) m_controller->camera(intentJson);
}

void MapsViewBackend::setSession(QString session)
{
    m_pendingSession = session;
    if (m_controller) {
        m_controller->setSession(session);
    } else if (m_core && m_startTimer.isActive()) {
        // The session arrived: no need to keep waiting for it.
        m_startTimer.start(0);
    }
}

LogosShutdown MapsViewBackend::aboutToUnload()
{
    m_startTimer.stop();
    if (m_controller) m_controller->stop();
    return LogosShutdown::Synchronous;
}
