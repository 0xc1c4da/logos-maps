// ChromeController: the map chrome of docs/architecture.md section 8.4 --
// everything a map user expects on every map regardless of the domain
// content drawn on it. Runs in the view process; MapWindow.qml reads it as
// the `chrome` context object and the QML items under src/qml/ render it.
//
// What lives here (one object so QML has one binding root):
//   - theme (M8.23): the host's theme subset, neutral light until told;
//   - items (M8.17): which chrome items are on, from the `chrome` option;
//   - follow (M8.18): off | follow | followHeading, eased on every location
//     update, ended by a pan/rotate gesture or an intent without keepFollow;
//   - zoom buttons (M8.17): +/- share GestureController::zoomStep with the
//     keyboard;
//   - groups and basemap details for the layers panel (M8.20);
//   - the callout (M8.19), the dropped pin (M8.21), the status chip and the
//     coordinate readout (M8.17);
//   - escape (M8.22): callout, then pin, then follow.
//
// It never touches the scene (groups hide layers through the SceneApplier's
// per-view visibility) and only speaks to maps_core through the CoreClient
// the ViewController owns, asynchronously (M12.9).
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <optional>

class QTimer;
namespace QMapLibre { class Map; }

namespace maps::view {

class ViewController;

class ChromeController : public QObject {
    Q_OBJECT
    // M8.23
    Q_PROPERTY(QVariantMap theme READ theme NOTIFY themeChanged)
    // M8.17 enabled flags: scaleBar, compass, attribution, zoomButtons,
    // locateButton, layersButton, callouts, coordinateReadout, statusChip.
    Q_PROPERTY(QVariantMap items READ items NOTIFY itemsChanged)
    // M8.18
    Q_PROPERTY(QString followMode READ followMode NOTIFY followChanged)
    Q_PROPERTY(bool locationAvailable READ locationAvailable NOTIFY locationChanged)
    Q_PROPERTY(bool headingAvailable READ headingAvailable NOTIFY locationChanged)
    // zoom buttons
    Q_PROPERTY(bool canZoomIn READ canZoomIn NOTIFY cameraChanged)
    Q_PROPERTY(bool canZoomOut READ canZoomOut NOTIFY cameraChanged)
    // M7.3 padding the chrome keeps clear of (host UI over the surface)
    Q_PROPERTY(QVariantMap padding READ padding NOTIFY paddingChanged)
    // status chip: {"visible", "text", "kind": loading|error|offline|info}
    Q_PROPERTY(QVariantMap status READ status NOTIFY statusChanged)
    // coordinate readout: {"lon","lat","text"} for the centre or the pointer
    Q_PROPERTY(QVariantMap coordinate READ coordinate NOTIFY coordinateChanged)
    // M8.20 layers panel model
    Q_PROPERTY(QVariantList groups READ groups NOTIFY groupsChanged)
    Q_PROPERTY(QVariantList details READ details NOTIFY groupsChanged)
    Q_PROPERTY(QVariantList basemaps READ basemaps NOTIFY basemapsChanged)
    Q_PROPERTY(QString basemap READ basemap NOTIFY basemapsChanged)
    Q_PROPERTY(bool layersAvailable READ layersAvailable NOTIFY groupsChanged)
    Q_PROPERTY(bool panelOpen READ panelOpen WRITE setPanelOpen NOTIFY panelOpenChanged)
    // M8.19 callout: {"open","title","subtitle","icon","actions":[{id,label}],
    // "px":[x,y], "builtin": bool}
    Q_PROPERTY(QVariantMap callout READ callout NOTIFY calloutChanged)
    // M8.21 pin: {"active","lon","lat","text"}
    Q_PROPERTY(QVariantMap pin READ pin NOTIFY pinChanged)
public:
    static constexpr const char* kPinSource = "maps/builtin:pin";
    static constexpr const char* kPinLayer = "maps/builtin:pin-dot";
    static constexpr int kFollowEaseMs = 300;
    static constexpr int kLoadingChipDelayMs = 1000;

    explicit ChromeController(ViewController* view);
    ~ChromeController() override;

    // ── fed by the ViewController ──────────────────────────────────────
    /// Options merged by setViewOptions: theme, chrome, follow, location,
    /// padding, hiddenGroups, hiddenDetails, clearPin, closeCallout.
    void applyOptions(const QJsonObject& options);
    /// A camera intent is about to be applied (M7.4b).
    void onIntent(const QJsonObject& intent);
    /// GestureController signals.
    void onGestureBegan(const QString& kind);
    void onKeyboardMoved(const QString& kind);
    /// Camera / viewport changed (zoom limits, callout anchor, readout).
    void onCameraChanged();
    /// Pointer hover position in item coordinates (readout, M8.15a hover).
    void onPointerMoved(const QPointF& px);
    /// A pick result (M8.19 callout, M8.21 pin).
    void onFeaturePicked(const QJsonObject& payload);
    /// Style lifecycle (pin source / status chip).
    void onStyleWillLoad();
    void onStyleLoaded();
    void onBasemapLoading(const QString& id);
    void onBasemapLoaded(const QString& id);
    void onBasemapFailed(const QString& id, const QString& reason);
    void onAnchorMissing(const QString& anchor);
    /// Scene metadata changed (groups), or the basemap descriptor arrived.
    void onSceneGroupsChanged(const QJsonArray& groups);  // [{session,id,title,...,layers[]}]
    void onBasemapDescriptor(const QJsonObject& descriptor);
    void onCatalogue(const QJsonArray& descriptors);
    /// A group's visibility changed in another view or through the API.
    void onGroupVisibilityChanged(const QString& groupId, bool visible);
    /// setCalloutContent from a module (M8.19).
    void onCalloutContent(const QJsonObject& content);
    /// A layer or session left the scene: close a callout anchored to it.
    void onLayersRemoved(const QStringList& layerIds);
    /// Scene layers were re-inserted: raise the pin back above them (M8.21).
    void updatePinSource();

    // ── read ───────────────────────────────────────────────────────────
    QVariantMap theme() const { return m_theme; }
    QVariantMap items() const { return m_items; }
    QString followMode() const { return m_follow; }
    bool locationAvailable() const { return m_location.isObject(); }
    bool headingAvailable() const;
    bool canZoomIn() const;
    bool canZoomOut() const;
    QVariantMap padding() const { return m_padding; }
    QVariantMap status() const { return m_status; }
    QVariantMap coordinate() const { return m_coordinate; }
    QVariantList groups() const { return m_groups; }
    QVariantList details() const { return m_details; }
    QVariantList basemaps() const { return m_basemaps; }
    QString basemap() const;
    bool layersAvailable() const { return !m_groups.isEmpty() || !m_details.isEmpty() || m_basemaps.size() > 1; }
    bool panelOpen() const { return m_panelOpen; }
    QVariantMap callout() const { return m_callout; }
    QVariantMap pin() const { return m_pin; }
    QStringList hiddenGroups() const { return m_hiddenGroups; }
    QStringList hiddenDetails() const { return m_hiddenDetails; }
    bool itemEnabled(const QString& name) const { return m_items.value(name, false).toBool(); }
    /// The pin's layer is in the style (the picker must know it, M8.21).
    bool pinInstalled() const { return m_pinInstalled; }
    /// Default theme values (neutral light), for tests.
    static QVariantMap defaultTheme();
    static QVariantMap defaultItems();

    // ── QML actions ────────────────────────────────────────────────────
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    /// M8.18: locate tap cycles off -> follow -> followHeading -> off.
    Q_INVOKABLE void cycleFollow();
    Q_INVOKABLE void setFollowMode(const QString& mode, const QString& reason = QStringLiteral("user"));
    Q_INVOKABLE void setPanelOpen(bool open);
    Q_INVOKABLE void setGroupVisible(const QString& groupId, bool visible);
    Q_INVOKABLE void setDetailVisible(const QString& detailId, bool visible);
    Q_INVOKABLE void selectBasemap(const QString& id);
    Q_INVOKABLE void calloutAction(const QString& actionId);
    Q_INVOKABLE void closeCallout(const QString& reason = QStringLiteral("tap"));
    Q_INVOKABLE void copyCoordinate();
    Q_INVOKABLE void copyPin();
    Q_INVOKABLE QString sharePin();
    Q_INVOKABLE void clearPin();
    /// M8.22: callout, then pin, then follow; returns what it closed ("" = nothing).
    Q_INVOKABLE QString escape();
    /// A tap on the map away from any callout anchor (QML forwards it).
    Q_INVOKABLE void mapTapped();
    /// Pointer hover in item coordinates (readout, M8.15a hover) / left the map.
    Q_INVOKABLE void pointerMoved(double x, double y) { onPointerMoved(QPointF(x, y)); }
    Q_INVOKABLE void pointerLeft();
    /// M8.21: drop the pin programmatically (tests, host).
    Q_INVOKABLE void dropPinAt(double lon, double lat) { dropPin(lon, lat); }
    /// Format a coordinate the way the readout / pin sheet do (M8.17).
    static QString formatCoordinate(double lon, double lat);

signals:
    void themeChanged();
    void itemsChanged();
    void followChanged();
    void locationChanged();
    void cameraChanged();
    void paddingChanged();
    void statusChanged();
    void coordinateChanged();
    void groupsChanged();
    void basemapsChanged();
    void panelOpenChanged();
    void calloutChanged();
    void pinChanged();
    /// A group was toggled by the user; the ViewController relays it to core.
    void groupVisibilityRequested(const QString& session, const QString& groupId, bool visible);
    /// Basemap chosen in the panel.
    void basemapRequested(const QString& id);
    /// Feature state the view itself owns (M8.15a): hover / selected.
    void featureStateRequested(const QString& session, const QString& sourceId, const QString& featureId, const QJsonObject& state);
    /// Hidden groups/details changed: the applier re-evaluates visibility.
    void visibilityChanged();

private:
    struct CalloutAnchor {
        QString session, layerId, sourceId, featureId;
        double lon = 0, lat = 0;
        bool builtin = false;  // the pin's own callout
    };
    void setTheme(const QJsonObject& theme);
    void setItems(const QJsonObject& chrome);
    void setHidden(QStringList groups, QStringList details, QStringList shownDetails);
    /// The first hit (M8.12 order) on a callout / selectable layer or the
    /// pin, as an anchor; `hit` receives that hit's JSON.
    std::optional<CalloutAnchor> anchorFor(const QJsonObject& pick, QJsonObject* hit = nullptr) const;
    void setLocation(const QJsonValue& location);
    void followLocation();
    void endFollow(const QString& reason);
    void setStatus(const QString& kind, const QString& text);
    void clearStatus(const QString& kind);
    void openCallout(const CalloutAnchor& anchor, const QJsonObject& content, const QJsonObject& properties);
    void updateCalloutPosition();
    void dropPin(double lon, double lat);
    void installPin();
    void setSelected(const CalloutAnchor& anchor, bool selected);
    void setHover(const std::optional<CalloutAnchor>& anchor);
    void rebuildGroupsModel();
    void updateCoordinate(const std::optional<QPointF>& px);
    QMapLibre::Map* map() const;
    void report(const QString& topic, QJsonObject payload);

    ViewController* m_view;
    QVariantMap m_theme;
    QVariantMap m_items;
    QVariantMap m_padding;
    QString m_follow = QStringLiteral("off");
    QJsonValue m_location;
    QVariantMap m_status;
    QString m_statusKind;
    QVariantMap m_coordinate;
    std::optional<QPointF> m_pointer;
    QJsonArray m_sceneGroups;   // raw [{session,id,...,layers[]}]
    QJsonArray m_basemapDetails;  // descriptor.details
    QVariantList m_groups, m_details, m_basemaps;
    QStringList m_hiddenGroups, m_hiddenDetails, m_shownDetails;
    QJsonObject m_calloutProperties;  // of the open callout's feature ({prop} templates)
    bool m_panelOpen = false;
    QVariantMap m_callout;
    std::optional<CalloutAnchor> m_calloutAnchor;
    std::optional<CalloutAnchor> m_hover;
    QVariantMap m_pin;
    std::optional<QPointF> m_pinLonLat;  // x = lon, y = lat
    bool m_pinInstalled = false;
    bool m_styleReady = false;
    QString m_loadingBasemap;
    QTimer* m_loadingTimer = nullptr;
    QTimer* m_hoverTimer = nullptr;
    std::optional<QPointF> m_pendingHover;
};

}  // namespace maps::view
