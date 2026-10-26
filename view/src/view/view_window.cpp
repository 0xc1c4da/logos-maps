#include "view/view_window.h"

#include "input/gesture_controller.h"
#include "view/map_item.h"

#include <logos/view_surface.h>

#include <QDir>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUrl>
#include <QtGlobal>

#include <dlfcn.h>

namespace maps::view {

namespace {
void anchorSymbol() {}
}  // namespace

ViewWindow::ViewWindow(QQuickWindow* hostWindow, QObject* parent)
    : QObject(parent)
{
    registerMapQmlTypes();
    if (hostWindow) {
        // RFC-8: the runtime's window. Geometry, visibility and lifetime are
        // the host's; we only put content in it.
        m_window = hostWindow;
        m_owned = false;
    } else {
        m_window = new QQuickWindow();
        m_owned = true;
        m_window->setColor(Qt::black);
    }
    m_engine = new QQmlEngine(this);
    m_engine->addImportPath(qmlDirectory());
    // Follow the window: the loaded root always fills it (what
    // QQuickView::SizeRootObjectToView did when ViewWindow was a view).
    connect(m_window, &QQuickWindow::widthChanged, this, &ViewWindow::fitRoot);
    connect(m_window, &QQuickWindow::heightChanged, this, &ViewWindow::fitRoot);
}

ViewWindow::~ViewWindow()
{
    // Content goes before the engine that created it; the host's window
    // outlives us untouched, our own window goes last.
    clearRoot();
    delete m_component;
    m_component = nullptr;
    delete m_engine;
    m_engine = nullptr;
    if (m_owned) delete m_window;
    m_window = nullptr;
}

QQuickWindow* ViewWindow::hostWindow()
{
    return LogosViewSurface::window();
}

QString ViewWindow::hostTransport()
{
    return LogosViewSurface::transport();
}

void ViewWindow::setGestureController(GestureController* controller)
{
    m_engine->rootContext()->setContextProperty(QStringLiteral("gestures"), controller);
}

void ViewWindow::setContextObject(const QString& name, QObject* object)
{
    m_engine->rootContext()->setContextProperty(name, object);
}

QString ViewWindow::pluginDirectory()
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&anchorSymbol), &info) && info.dli_fname) {
        return QFileInfo(QString::fromLocal8Bit(info.dli_fname)).absolutePath();
    }
    return QDir::currentPath();
}

QString ViewWindow::qmlDirectory()
{
    const QByteArray override = qgetenv("MAPS_VIEW_QML_DIR");
    if (!override.isEmpty()) return QString::fromLocal8Bit(override);
    return pluginDirectory() + QStringLiteral("/qml");
}

void ViewWindow::clearRoot()
{
    if (m_root) {
        m_root->setParentItem(nullptr);
        delete m_root;
        m_root = nullptr;
    }
    m_mapItem = nullptr;
}

void ViewWindow::fitRoot()
{
    if (!m_root || !m_window) return;
    m_root->setSize(QSizeF(m_window->width(), m_window->height()));
}

MapItem* ViewWindow::load(const QString& qmlDirIn)
{
    const QString qmlDir = qmlDirIn.isEmpty() ? qmlDirectory() : qmlDirIn;
    const QString file = qmlDir + QStringLiteral("/MapWindow.qml");
    m_fallback = false;
    m_errors.clear();
    clearRoot();
    delete m_component;
    m_component = nullptr;

    if (QFileInfo::exists(file)) {
        m_component = new QQmlComponent(m_engine, QUrl::fromLocalFile(file), QQmlComponent::PreferSynchronous);
        if (m_component->isReady()) {
            QObject* obj = m_component->create(m_engine->rootContext());
            m_root = qobject_cast<QQuickItem*>(obj);
            if (m_root) {
                m_root->setParentItem(m_window->contentItem());
                m_mapItem = m_root->findChild<MapItem*>(QStringLiteral("map"));
                if (!m_mapItem) m_mapItem = qobject_cast<MapItem*>(m_root);
            } else {
                qWarning("maps_view: %s is not an Item", qPrintable(file));
                delete obj;
            }
        } else {
            m_errors = m_component->errors();
            for (const QQmlError& e : m_errors) qWarning("maps_view: %s", qPrintable(e.toString()));
        }
    } else {
        qWarning("maps_view: %s not found", qPrintable(file));
    }
    if (!m_mapItem) {
        clearRoot();
        m_fallback = true;
        m_root = new QQuickItem();
        m_root->setObjectName(QStringLiteral("fallbackRoot"));
        m_mapItem = new MapItem(m_root);
        m_mapItem->setObjectName(QStringLiteral("map"));
        m_mapItem->setSize(QSizeF(m_window->width(), m_window->height()));
        connect(m_root, &QQuickItem::widthChanged, m_mapItem, [this] { m_mapItem->setWidth(m_root->width()); });
        connect(m_root, &QQuickItem::heightChanged, m_mapItem, [this] { m_mapItem->setHeight(m_root->height()); });
        m_root->setParentItem(m_window->contentItem());
    }
    fitRoot();
    return m_mapItem;
}

void ViewWindow::setTitle(const QString& title)
{
    if (m_owned) m_window->setTitle(title);
}

void ViewWindow::resize(int width, int height)
{
    if (m_owned) m_window->resize(width, height);
}

void ViewWindow::resize(const QSize& size)
{
    if (m_owned) m_window->resize(size);
}

void ViewWindow::show()
{
    if (m_owned) m_window->show();
}

}  // namespace maps::view
