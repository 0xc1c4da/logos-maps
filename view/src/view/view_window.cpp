#include "view/view_window.h"

#include "input/gesture_controller.h"
#include "view/map_item.h"

#include <QDir>
#include <QFileInfo>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QUrl>
#include <QtGlobal>

#include <dlfcn.h>

namespace maps::view {

namespace {
void anchorSymbol() {}
}  // namespace

ViewWindow::ViewWindow(QWindow* parent) : QQuickView(parent)
{
    registerMapQmlTypes();
    setResizeMode(QQuickView::SizeRootObjectToView);
    setColor(Qt::black);
    engine()->addImportPath(qmlDirectory());
}

void ViewWindow::setGestureController(GestureController* controller)
{
    engine()->rootContext()->setContextProperty(QStringLiteral("gestures"), controller);
}

void ViewWindow::setContextObject(const QString& name, QObject* object)
{
    engine()->rootContext()->setContextProperty(name, object);
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

MapItem* ViewWindow::load(const QString& qmlDirIn)
{
    const QString qmlDir = qmlDirIn.isEmpty() ? qmlDirectory() : qmlDirIn;
    const QString file = qmlDir + QStringLiteral("/MapWindow.qml");
    m_fallback = false;
    m_mapItem = nullptr;
    if (QFileInfo::exists(file)) {
        setSource(QUrl::fromLocalFile(file));
        if (status() == QQuickView::Ready && rootObject()) {
            m_mapItem = rootObject()->findChild<MapItem*>(QStringLiteral("map"));
            if (!m_mapItem) m_mapItem = qobject_cast<MapItem*>(rootObject());
        } else {
            for (const QQmlError& e : errors()) qWarning("maps_view: %s", qPrintable(e.toString()));
        }
    } else {
        qWarning("maps_view: %s not found", qPrintable(file));
    }
    if (!m_mapItem) {
        m_fallback = true;
        setSource(QUrl());
        auto* root = new QQuickItem();
        root->setObjectName(QStringLiteral("fallbackRoot"));
        m_mapItem = new MapItem(root);
        m_mapItem->setObjectName(QStringLiteral("map"));
        m_mapItem->setSize(QSizeF(width(), height()));
        connect(this, &QQuickWindow::widthChanged, m_mapItem, [this](int w) { m_mapItem->setWidth(w); });
        connect(this, &QQuickWindow::heightChanged, m_mapItem, [this](int h) { m_mapItem->setHeight(h); });
        root->setParentItem(contentItem());
        root->setSize(QSizeF(width(), height()));
        connect(this, &QQuickWindow::widthChanged, root, [root](int w) { root->setWidth(w); });
        connect(this, &QQuickWindow::heightChanged, root, [root](int h) { root->setHeight(h); });
    }
    return m_mapItem;
}

}  // namespace maps::view
