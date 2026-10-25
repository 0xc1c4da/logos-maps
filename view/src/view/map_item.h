// MapItem: the QQuickItem over QMapLibre::Map (docs/architecture.md M5.3-M5.6).
//
// The Map is created on the GUI thread as soon as the item has a window, and
// rendered from the scene-graph render thread inside updatePaintNode through
// the binding's exported TextureNodeOpenGL (section 14.1 P7): an FBO of
// item size x pixelRatio presented as a QSGTexture. needsRendering is wired
// to update(), so an idle map renders zero frames (section 15.2).
//
// Everything else -- scene application, style loading, camera, gestures --
// lives in collaborators that take map() on the GUI thread. The item itself
// has no opinion on content.
#pragma once

#include "view/engine_config.h"

#include <QMapLibre/Map>

#include <QImage>
#include <QList>
#include <QMutex>
#include <QQuickItem>
#include <QSize>

#include <atomic>
#include <memory>

class QSGTexture;

namespace maps::view {

class MapItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool hasMap READ hasMap NOTIFY mapCreated)
    Q_PROPERTY(bool styleLoaded READ styleLoaded NOTIFY styleLoadedChanged)
    Q_PROPERTY(double devicePixelRatio READ devicePixelRatio NOTIFY pixelRatiosChanged)
    Q_PROPERTY(double pixelRatio READ pixelRatio WRITE setPixelRatio NOTIFY pixelRatiosChanged)
    Q_PROPERTY(double mapToQtPixelRatio READ mapToQtPixelRatio NOTIFY pixelRatiosChanged)
    Q_PROPERTY(QString copyrights READ copyrights NOTIFY copyrightsChanged)

public:
    explicit MapItem(QQuickItem* parent = nullptr);
    ~MapItem() override;

    /// Must be set before the map is created (before the item gets a window).
    void setEngineConfig(const EngineConfig& config);
    const EngineConfig& engineConfig() const { return m_config; }

    /// The engine, GUI thread only. Null until mapCreated().
    QMapLibre::Map* map() const { return m_map.get(); }
    std::shared_ptr<QMapLibre::Map> sharedMap() const { return m_map; }
    bool hasMap() const { return m_map != nullptr; }
    bool styleLoaded() const { return m_styleLoaded; }

    // M5.5 pixel model.
    double devicePixelRatio() const;
    double pixelRatio() const;             // map render scale; defaults to devicePixelRatio
    void setPixelRatio(double ratio);      // 0 = follow devicePixelRatio
    double mapToQtPixelRatio() const { return pixelRatio(); }

    QString copyrights() const { return m_copyrights; }

    /// Create the map now even without a window (headless tests); the item
    /// still needs a window to render.
    void createMapNow(const QSize& size, qreal pixelRatio);

    // Test instrumentation.
    int paintNodeUpdates() const { return m_paintNodeUpdates.load(); }
    QSize lastTextureSize() const { return m_lastTextureSize; }
    int rendererCreations() const { return m_rendererCreations.load(); }

    /// Snapshot rendering (M7.4): read the engine's texture back after the
    /// next frame. The result arrives on the GUI thread as frameGrabbed(),
    /// an opaque image at physical size with the item's pixel ratio set,
    /// or a null image when the item could not render (no GL, no map).
    void grabNextFrame(quint64 token);

signals:
    void frameGrabbed(quint64 token, const QImage& image);
    void mapCreated();
    void mapChanged(int change);          // raw QMapLibre::Map::MapChange
    void styleLoadedChanged();
    void mapLoadingFailed(int failure, const QString& reason);
    void copyrightsChanged(const QString& html);
    void pixelRatiosChanged();
    void viewportChanged(int width, int height, double pixelRatio);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void componentComplete() override;
    void releaseResources() override;

private:
    void ensureMap();
    void onMapChanged(QMapLibre::Map::MapChange change);
    QSize viewportSize() const;
    void readBackFrame(QSGTexture* texture, qreal ratio);  // render thread
    QList<quint64> takePendingGrabs();

    QMutex m_grabMutex;
    QList<quint64> m_pendingGrabs;  // guarded by m_grabMutex

    EngineConfig m_config;
    std::shared_ptr<QMapLibre::Map> m_map;
    double m_pixelRatioOverride = 0.0;
    bool m_styleLoaded = false;
    bool m_viewportDirty = true;
    QString m_copyrights;
    std::atomic<int> m_paintNodeUpdates{0};
    std::atomic<int> m_rendererCreations{0};
    QSize m_lastTextureSize;
    QSize m_nodeSize;        // render thread only
    qreal m_nodeRatio = 0.0; // render thread only
};

/// Registers MapItem as QML type Maps.View 1.0 MapItem (idempotent).
void registerMapQmlTypes();

}  // namespace maps::view
