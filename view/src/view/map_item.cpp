#include "view/map_item.h"

#include "input/gesture_controller.h"

#include <QMapLibreQuickPrivate/texture_node_opengl_p.hpp>

#include <QDebug>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGTexture>
#include <QtGui/qopengl.h>

#include <utility>

namespace maps::view {

namespace {
constexpr int kMinSize = 64;

// The scene-graph node is the binding's (final) TextureNodeOpenGL; the size
// it was last resized to is tracked on the item (render-thread only).
using MapNode = QMapLibre::TextureNodeOpenGL;
}  // namespace

MapItem::MapItem(QQuickItem* parent) : QQuickItem(parent), m_config(EngineConfig::processDefault())
{
    setFlag(ItemHasContents, true);
}

MapItem::~MapItem() = default;

void MapItem::setEngineConfig(const EngineConfig& config)
{
    m_config = config;
}

double MapItem::devicePixelRatio() const
{
    return window() ? window()->effectiveDevicePixelRatio() : 1.0;
}

double MapItem::pixelRatio() const
{
    return m_pixelRatioOverride > 0.0 ? m_pixelRatioOverride : devicePixelRatio();
}

void MapItem::setPixelRatio(double ratio)
{
    if (qFuzzyCompare(ratio, m_pixelRatioOverride)) return;
    m_pixelRatioOverride = ratio;
    m_viewportDirty = true;
    emit pixelRatiosChanged();
    if (m_map) {
        m_map->resize(viewportSize(), pixelRatio());
        emit viewportChanged(viewportSize().width(), viewportSize().height(), pixelRatio());
    }
    update();
}

QSize MapItem::viewportSize() const
{
    return QSize(static_cast<int>(width()), static_cast<int>(height())).expandedTo(QSize(kMinSize, kMinSize));
}

void MapItem::createMapNow(const QSize& size, qreal ratio)
{
    if (m_map) return;
    m_map = std::make_shared<QMapLibre::Map>(nullptr, m_config.toSettings(), size.expandedTo(QSize(kMinSize, kMinSize)), ratio);
    m_map->setConnectionEstablished();

    // Connect before anything can load: a style resolving from cache emits
    // DidFinishLoadingStyle immediately and would otherwise be lost.
    connect(m_map.get(), &QMapLibre::Map::needsRendering, this, [this] {
        static const bool trace = qEnvironmentVariableIsSet("MAPS_VIEW_TRACE");
        if (trace) qDebug("maps_view: needsRendering");
        update();
    });
    connect(m_map.get(), &QMapLibre::Map::mapChanged, this, &MapItem::onMapChanged);
    connect(m_map.get(), &QMapLibre::Map::mapLoadingFailed, this,
            [this](QMapLibre::Map::MapLoadingFailure failure, const QString& reason) {
                emit mapLoadingFailed(static_cast<int>(failure), reason);
            });
    connect(m_map.get(), &QMapLibre::Map::copyrightsChanged, this, [this](const QString& html) {
        if (m_copyrights == html) return;
        m_copyrights = html;
        emit copyrightsChanged(html);
    });

    emit mapCreated();
    update();
}

void MapItem::ensureMap()
{
    if (m_map || !window()) return;
    createMapNow(viewportSize(), pixelRatio());
    emit viewportChanged(viewportSize().width(), viewportSize().height(), pixelRatio());
}

void MapItem::componentComplete()
{
    QQuickItem::componentComplete();
    ensureMap();
}

void MapItem::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange) {
        if (value.window) {
            connect(value.window, &QQuickWindow::screenChanged, this, [this] {
                m_viewportDirty = true;
                emit pixelRatiosChanged();
                if (m_map) m_map->resize(viewportSize(), pixelRatio());
                update();
            });
            if (isComponentComplete()) ensureMap();
            m_viewportDirty = true;
            emit pixelRatiosChanged();
        }
    }
}

void MapItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size()) return;
    m_viewportDirty = true;
    if (m_map) {
        m_map->resize(viewportSize(), pixelRatio());
        emit viewportChanged(viewportSize().width(), viewportSize().height(), pixelRatio());
    }
    update();
}

void MapItem::releaseResources()
{
    // The node (and its FBO) is owned by the scene graph and deleted with the
    // item or on invalidation; the renderer dies with the Map. Nothing to do.
}

void MapItem::onMapChanged(QMapLibre::Map::MapChange change)
{
    switch (change) {
    case QMapLibre::Map::MapChangeWillStartLoadingMap:
        if (m_styleLoaded) {
            m_styleLoaded = false;
            emit styleLoadedChanged();
        }
        break;
    case QMapLibre::Map::MapChangeDidFinishLoadingStyle:
        if (!m_styleLoaded) {
            m_styleLoaded = true;
            emit styleLoadedChanged();
        }
        break;
    default:
        break;
    }
    emit mapChanged(static_cast<int>(change));
}

QSGNode* MapItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    m_paintNodeUpdates.fetch_add(1);
    static const bool trace = qEnvironmentVariableIsSet("MAPS_VIEW_TRACE");
    if (trace) qDebug("maps_view: updatePaintNode");
    if (!m_map) {
        delete oldNode;
        return nullptr;
    }

    auto* node = static_cast<MapNode*>(oldNode);
    const QSize size = viewportSize();
    const qreal ratio = pixelRatio();

    if (!node) {
        if (QOpenGLContext::currentContext() == nullptr) {
            // Software or non-GL scene graph: no engine output is possible.
            // Reported as a capability (surface.transport stays "none"); the
            // item draws nothing rather than crashing.
            qWarning("MapItem: no OpenGL context on the render thread; the Qt Quick backend must be OpenGL");
            return nullptr;
        }
        node = new MapNode(m_map, size, ratio);
        m_rendererCreations.fetch_add(1);
        m_viewportDirty = true;
    }

    if (m_viewportDirty || m_nodeSize != size || !qFuzzyCompare(m_nodeRatio, ratio)) {
        node->resize(size, ratio, window());
        m_nodeSize = size;
        m_nodeRatio = ratio;
        m_viewportDirty = false;
    }

    bool grabWanted = false;
    {
        const QMutexLocker lock(&m_grabMutex);
        grabWanted = !m_pendingGrabs.isEmpty();
    }
    node->render(window());
    if (QSGTexture* texture = node->texture()) m_lastTextureSize = texture->textureSize();
    if (grabWanted) readBackFrame(node->texture(), ratio);
    return node;
}

void MapItem::grabNextFrame(quint64 token)
{
    {
        const QMutexLocker lock(&m_grabMutex);
        m_pendingGrabs.append(token);
    }
    if (!m_map || !window()) {
        for (quint64 t : takePendingGrabs()) emit frameGrabbed(t, QImage());
        return;
    }
    update();
}

QList<quint64> MapItem::takePendingGrabs()
{
    const QMutexLocker lock(&m_grabMutex);
    return std::exchange(m_pendingGrabs, {});
}

// Render thread, right after the engine drew the frame into its texture:
// attach that texture to a scratch FBO and read it back. The engine's
// framebuffer is cleared opaque, so the image is flattened to RGB32.
void MapItem::readBackFrame(QSGTexture* texture, qreal ratio)
{
    static const bool trace = qEnvironmentVariableIsSet("MAPS_VIEW_TRACE");
    const QList<quint64> tokens = takePendingGrabs();
    if (tokens.isEmpty()) return;
    QImage image;
    QOpenGLContext* context = QOpenGLContext::currentContext();
    auto* native = texture ? texture->nativeInterface<QNativeInterface::QSGOpenGLTexture>() : nullptr;
    if (context && native && native->nativeTexture() != 0 && !texture->textureSize().isEmpty()) {
        QOpenGLFunctions* gl = context->functions();
        const QSize size = texture->textureSize();
        GLint previous = 0;
        gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
        GLuint fbo = 0;
        gl->glGenFramebuffers(1, &fbo);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, native->nativeTexture(), 0);
        if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            QImage raw(size, QImage::Format_RGBA8888_Premultiplied);
            gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
            gl->glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, raw.bits());
            if (trace) {
                qDebug("maps_view: readBackFrame texture %u %dx%d centre #%08x", native->nativeTexture(), size.width(), size.height(),
                       raw.pixel(size.width() / 2, size.height() / 2));
            }
            image = raw.flipped(Qt::Vertical).convertToFormat(QImage::Format_RGB32);
            image.setDevicePixelRatio(ratio);
        }
        gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
        gl->glDeleteFramebuffers(1, &fbo);
    }
    for (quint64 t : tokens) emit frameGrabbed(t, image);
}

void registerMapQmlTypes()
{
    static bool done = false;
    if (done) return;
    done = true;
    qmlRegisterType<MapItem>("Maps.View", 1, 0, "MapItem");
    qmlRegisterUncreatableType<GestureController>("Maps.View", 1, 0, "GestureController", QStringLiteral("owned by the view"));
}

}  // namespace maps::view
