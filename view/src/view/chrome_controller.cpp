#include "view/chrome_controller.h"

#include "input/gesture_controller.h"
#include "input/picker.h"
#include "view/map_item.h"
#include "view/scene_applier.h"
#include "view/view_controller.h"

#include <QMapLibre/Map>
#include <QMapLibre/Types>

#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace maps::view {

namespace {

const QStringList kItemNames{
    QStringLiteral("scaleBar"),      QStringLiteral("compass"),       QStringLiteral("attribution"),
    QStringLiteral("zoomButtons"),   QStringLiteral("locateButton"),  QStringLiteral("layersButton"),
    QStringLiteral("callouts"),      QStringLiteral("coordinateReadout"), QStringLiteral("statusChip"),
};
const QStringList kFollowModes{QStringLiteral("off"), QStringLiteral("follow"), QStringLiteral("followHeading")};

constexpr int kHoverDebounceMs = 60;

/// M6.3b `{prop}` templates: unknown props become "".
QString expandTemplate(const QString& text, const QJsonObject& properties)
{
    static const QRegularExpression re(QStringLiteral("\\{([A-Za-z0-9_.:-]+)\\}"));
    QString out;
    int last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        const QJsonValue v = properties.value(m.captured(1));
        if (v.isString()) out += v.toString();
        else if (v.isDouble()) out += QString::number(v.toDouble(), 'g', 12);
        else if (v.isBool()) out += v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        last = m.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

QJsonObject anchorJson(const QString& session, const QString& layerId, const QString& sourceId, const QString& featureId, double lon, double lat)
{
    return QJsonObject{{QStringLiteral("v"), 1},
                       {QStringLiteral("session"), session},
                       {QStringLiteral("layerId"), layerId},
                       {QStringLiteral("sourceId"), sourceId},
                       {QStringLiteral("featureId"), featureId},
                       {QStringLiteral("lonLat"), QJsonArray{lon, lat}}};
}

}  // namespace

// ── construction ─────────────────────────────────────────────────────────

ChromeController::ChromeController(ViewController* view)
    : QObject(view), m_view(view), m_theme(defaultTheme()), m_items(defaultItems())
{
    m_status = QVariantMap{{QStringLiteral("visible"), false}, {QStringLiteral("text"), QString()}, {QStringLiteral("kind"), QString()}};
    m_callout = QVariantMap{{QStringLiteral("open"), false}};
    m_pin = QVariantMap{{QStringLiteral("active"), false}};
    m_loadingTimer = new QTimer(this);
    m_loadingTimer->setSingleShot(true);
    m_loadingTimer->setInterval(kLoadingChipDelayMs);
    connect(m_loadingTimer, &QTimer::timeout, this, [this] {
        if (!m_loadingBasemap.isEmpty()) setStatus(QStringLiteral("loading"), tr("Loading map…"));
    });
    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    m_hoverTimer->setInterval(kHoverDebounceMs);
    connect(m_hoverTimer, &QTimer::timeout, this, [this] {
        if (!m_pendingHover) return;
        const QPointF px = *m_pendingHover;
        m_pendingHover.reset();
        Picker* picker = m_view ? m_view->picker() : nullptr;
        if (!picker) return;
        std::optional<CalloutAnchor> hover;
        if (auto pick = picker->pickNow(QStringLiteral("hover"), px)) {
            if (auto a = anchorFor(*pick)) {
                SceneApplier* applier = m_view->sceneApplier();
                const auto info = applier ? applier->layerInfo(a->layerId) : std::nullopt;
                if (info && info->selectable) hover = a;
            }
        }
        setHover(hover);
    });
}

ChromeController::~ChromeController() = default;

QVariantMap ChromeController::defaultTheme()
{
    return QVariantMap{
        {QStringLiteral("dark"), false},
        {QStringLiteral("accent"), QStringLiteral("#2b6cb0")},
        {QStringLiteral("foreground"), QStringLiteral("#1a1a1a")},
        {QStringLiteral("background"), QStringLiteral("#ffffff")},
        {QStringLiteral("surface"), QStringLiteral("#f5f5f5")},
        {QStringLiteral("border"), QStringLiteral("#b0b0b0")},
        {QStringLiteral("radius"), 6},
        {QStringLiteral("fontFamily"), QString()},
        {QStringLiteral("fontPixelSize"), 13},
    };
}

QVariantMap ChromeController::defaultItems()
{
    QVariantMap items;
    for (const QString& name : kItemNames) items.insert(name, true);
    items.insert(QStringLiteral("zoomButtons"), false);
    items.insert(QStringLiteral("coordinateReadout"), false);
    return items;
}

QMapLibre::Map* ChromeController::map() const
{
    return (m_view && m_view->mapItem()) ? m_view->mapItem()->map() : nullptr;
}

void ChromeController::report(const QString& topic, QJsonObject payload)
{
    if (!m_view) return;
    if (!payload.contains(QStringLiteral("v"))) payload.insert(QStringLiteral("v"), 1);
    m_view->reportEvent(topic, payload);
}

// ── options ──────────────────────────────────────────────────────────────

void ChromeController::applyOptions(const QJsonObject& options)
{
    if (options.value(QStringLiteral("theme")).isObject()) setTheme(options.value(QStringLiteral("theme")).toObject());
    if (options.value(QStringLiteral("chrome")).isObject()) setItems(options.value(QStringLiteral("chrome")).toObject());
    if (options.contains(QStringLiteral("padding"))) {
        const QJsonObject p = options.value(QStringLiteral("padding")).toObject();
        QVariantMap padding{{QStringLiteral("left"), p.value(QStringLiteral("left")).toInt()},
                            {QStringLiteral("top"), p.value(QStringLiteral("top")).toInt()},
                            {QStringLiteral("right"), p.value(QStringLiteral("right")).toInt()},
                            {QStringLiteral("bottom"), p.value(QStringLiteral("bottom")).toInt()}};
        if (padding != m_padding) {
            m_padding = padding;
            emit paddingChanged();
            updateCalloutPosition();
        }
    }
    if (options.contains(QStringLiteral("location"))) setLocation(options.value(QStringLiteral("location")));
    if (options.value(QStringLiteral("follow")).isString()) setFollowMode(options.value(QStringLiteral("follow")).toString(), QStringLiteral("option"));

    QStringList groups = m_hiddenGroups, hidden = m_hiddenDetails, shown = m_shownDetails;
    bool visibility = false;
    if (options.contains(QStringLiteral("hiddenGroups"))) {
        groups.clear();
        for (const QJsonValue& v : options.value(QStringLiteral("hiddenGroups")).toArray()) groups << v.toString();
        visibility = true;
    }
    if (options.contains(QStringLiteral("hiddenDetails"))) {
        hidden.clear();
        for (const QJsonValue& v : options.value(QStringLiteral("hiddenDetails")).toArray()) hidden << v.toString();
        visibility = true;
    }
    if (options.contains(QStringLiteral("shownDetails"))) {
        shown.clear();
        for (const QJsonValue& v : options.value(QStringLiteral("shownDetails")).toArray()) shown << v.toString();
        visibility = true;
    }
    if (visibility) setHidden(groups, hidden, shown);

    if (options.value(QStringLiteral("clearPin")).toBool(false)) clearPin();
    if (options.value(QStringLiteral("closeCallout")).toBool(false)) closeCallout(QStringLiteral("api"));
}

void ChromeController::setTheme(const QJsonObject& theme)
{
    QVariantMap merged = defaultTheme();
    for (auto it = theme.begin(); it != theme.end(); ++it) {
        if (!merged.contains(it.key())) continue;
        const QVariant v = it.value().toVariant();
        if (v.isNull() || !v.isValid()) continue;
        merged.insert(it.key(), v);
    }
    if (merged == m_theme) return;
    m_theme = merged;
    emit themeChanged();
}

void ChromeController::setItems(const QJsonObject& chrome)
{
    QVariantMap items = m_items;
    for (auto it = chrome.begin(); it != chrome.end(); ++it) {
        if (!kItemNames.contains(it.key()) || !it.value().isBool()) continue;  // unknown keys ignored (M8.17)
        items.insert(it.key(), it.value().toBool());
    }
    if (items == m_items) return;
    m_items = items;
    emit itemsChanged();
    if (!itemEnabled(QStringLiteral("callouts")) && m_callout.value(QStringLiteral("open")).toBool()) closeCallout(QStringLiteral("api"));
    if (!itemEnabled(QStringLiteral("coordinateReadout"))) updateCoordinate(std::nullopt);
    else updateCoordinate(m_pointer);
}

void ChromeController::setHidden(QStringList groups, QStringList details, QStringList shownDetails)
{
    groups.removeDuplicates();
    details.removeDuplicates();
    shownDetails.removeDuplicates();
    const bool changed = groups != m_hiddenGroups || details != m_hiddenDetails || shownDetails != m_shownDetails;
    m_hiddenGroups = groups;
    m_hiddenDetails = details;
    m_shownDetails = shownDetails;
    if (SceneApplier* applier = m_view ? m_view->sceneApplier() : nullptr) {
        applier->setHiddenGroups(m_hiddenGroups);
        applier->setHiddenDetails(m_hiddenDetails, m_shownDetails);
    }
    if (changed) {
        rebuildGroupsModel();
        emit visibilityChanged();
    }
}

// ── follow (M8.18) ───────────────────────────────────────────────────────

void ChromeController::setLocation(const QJsonValue& location)
{
    const bool had = m_location.isObject();
    const bool hadHeading = headingAvailable();
    m_location = location.isObject() ? location : QJsonValue();
    if (had != m_location.isObject() || hadHeading != headingAvailable()) emit locationChanged();
    else if (m_location.isObject()) emit locationChanged();  // position moved: the button may re-evaluate
    followLocation();
}

bool ChromeController::headingAvailable() const
{
    return m_location.isObject() && m_location.toObject().value(QStringLiteral("heading")).isDouble();
}

void ChromeController::followLocation()
{
    if (m_follow == QLatin1String("off") || !m_location.isObject()) return;  // a lost fix keeps the mode (M8.18)
    QMapLibre::Map* m = map();
    GestureController* g = m_view ? m_view->gestures() : nullptr;
    if (!m || (g && g->gestureInProgress())) return;  // MUST NOT ease during a gesture
    const QJsonObject loc = m_location.toObject();
    QMapLibre::CameraOptions camera;
    camera.center = QVariant::fromValue(QMapLibre::Coordinate(loc.value(QStringLiteral("lat")).toDouble(), loc.value(QStringLiteral("lon")).toDouble()));
    if (m_follow == QLatin1String("followHeading") && headingAvailable()) camera.bearing = loc.value(QStringLiteral("heading")).toDouble();
    m->easeTo(camera, QMapLibre::AnimationOptions(kFollowEaseMs));
    m_view->refreshViewState(false);
}

void ChromeController::setFollowMode(const QString& mode, const QString& reason)
{
    if (!kFollowModes.contains(mode) || mode == m_follow) return;
    m_follow = mode;
    emit followChanged();
    report(QStringLiteral("followChanged"), QJsonObject{{QStringLiteral("mode"), mode}, {QStringLiteral("reason"), reason}});
    followLocation();
}

void ChromeController::cycleFollow()
{
    const int i = kFollowModes.indexOf(m_follow);
    setFollowMode(kFollowModes.at((i + 1) % kFollowModes.size()), QStringLiteral("user"));
}

void ChromeController::endFollow(const QString& reason)
{
    if (m_follow == QLatin1String("off")) return;
    setFollowMode(QStringLiteral("off"), reason);
}

void ChromeController::onGestureBegan(const QString& kind)
{
    // Pan and rotate break follow; pinch / shove / fling-after-zoom do not.
    if (kind == QLatin1String("pan") || kind == QLatin1String("rotate") || kind == QLatin1String("fling")) endFollow(QStringLiteral("gesture"));
}

void ChromeController::onKeyboardMoved(const QString& kind)
{
    if (kind == QLatin1String("pan") || kind == QLatin1String("rotate")) endFollow(QStringLiteral("gesture"));
}

void ChromeController::onIntent(const QJsonObject& intent)
{
    if (intent.value(QStringLiteral("keepFollow")).toBool(false)) return;  // M7.4b
    endFollow(QStringLiteral("intent"));
}

// ── zoom buttons ─────────────────────────────────────────────────────────

bool ChromeController::canZoomIn() const
{
    GestureController* g = m_view ? m_view->gestures() : nullptr;
    return g && map() && g->canZoomIn();
}

bool ChromeController::canZoomOut() const
{
    GestureController* g = m_view ? m_view->gestures() : nullptr;
    return g && map() && g->canZoomOut();
}

void ChromeController::zoomIn()
{
    if (GestureController* g = m_view ? m_view->gestures() : nullptr) g->zoomStep(1.0, gesture::kZoomButtonMs);
}

void ChromeController::zoomOut()
{
    if (GestureController* g = m_view ? m_view->gestures() : nullptr) g->zoomStep(-1.0, gesture::kZoomButtonMs);
}

// ── camera / pointer ─────────────────────────────────────────────────────

void ChromeController::onCameraChanged()
{
    emit cameraChanged();
    updateCalloutPosition();
    if (itemEnabled(QStringLiteral("coordinateReadout")) && !m_pointer) updateCoordinate(std::nullopt);
}

void ChromeController::onPointerMoved(const QPointF& px)
{
    m_pointer = px;
    if (itemEnabled(QStringLiteral("coordinateReadout"))) updateCoordinate(px);
    m_pendingHover = px;
    if (!m_hoverTimer->isActive()) m_hoverTimer->start();
}

void ChromeController::pointerLeft()
{
    m_pointer.reset();
    m_pendingHover.reset();
    m_hoverTimer->stop();
    setHover(std::nullopt);
    if (itemEnabled(QStringLiteral("coordinateReadout"))) updateCoordinate(std::nullopt);
}

QString ChromeController::formatCoordinate(double lon, double lat)
{
    return QStringLiteral("%1, %2").arg(QString::number(lat, 'f', 5), QString::number(lon, 'f', 5));
}

void ChromeController::updateCoordinate(const std::optional<QPointF>& px)
{
    QVariantMap coord;
    if (QMapLibre::Map* m = map(); m && itemEnabled(QStringLiteral("coordinateReadout"))) {
        QPointF p;
        if (px) {
            p = *px;
        } else {
            const QSize size = m_view->viewportSize();
            const QMargins margins = m->margins();
            p = QPointF(margins.left() + (size.width() - margins.left() - margins.right()) / 2.0,
                        margins.top() + (size.height() - margins.top() - margins.bottom()) / 2.0);
        }
        const QMapLibre::Coordinate c = m->coordinateForPixel(p);
        coord = QVariantMap{{QStringLiteral("lon"), c.second}, {QStringLiteral("lat"), c.first},
                            {QStringLiteral("text"), formatCoordinate(c.second, c.first)}, {QStringLiteral("pointer"), px.has_value()}};
    }
    if (coord == m_coordinate) return;
    m_coordinate = coord;
    emit coordinateChanged();
}

void ChromeController::copyCoordinate()
{
    if (m_coordinate.isEmpty() || !m_view) return;
    m_view->requestShare(QStringLiteral("copy"), m_coordinate.value(QStringLiteral("text")).toString());
}

// ── status chip ──────────────────────────────────────────────────────────

void ChromeController::setStatus(const QString& kind, const QString& text)
{
    m_statusKind = kind;
    m_status = QVariantMap{{QStringLiteral("visible"), true}, {QStringLiteral("kind"), kind}, {QStringLiteral("text"), text}};
    emit statusChanged();
}

void ChromeController::clearStatus(const QString& kind)
{
    if (!m_status.value(QStringLiteral("visible")).toBool()) return;
    if (!kind.isEmpty() && m_statusKind != kind) return;
    m_statusKind.clear();
    m_status = QVariantMap{{QStringLiteral("visible"), false}, {QStringLiteral("text"), QString()}, {QStringLiteral("kind"), QString()}};
    emit statusChanged();
}

void ChromeController::onBasemapLoading(const QString& id)
{
    m_loadingBasemap = id;
    m_loadingTimer->start();  // > 1 s only (M8.17)
    clearStatus(QStringLiteral("error"));
    if (m_callout.value(QStringLiteral("open")).toBool() && !m_calloutAnchor.value_or(CalloutAnchor{}).builtin) closeCallout(QStringLiteral("removed"));
}

void ChromeController::onBasemapLoaded(const QString& id)
{
    if (m_loadingBasemap == id || m_loadingBasemap.isEmpty()) {
        m_loadingBasemap.clear();
        m_loadingTimer->stop();
        clearStatus(QStringLiteral("loading"));
    }
}

void ChromeController::onBasemapFailed(const QString& id, const QString& reason)
{
    m_loadingBasemap.clear();
    m_loadingTimer->stop();
    setStatus(QStringLiteral("error"), reason.isEmpty() ? tr("Map failed to load: %1").arg(id) : tr("Map failed to load: %1").arg(reason));
}

void ChromeController::onAnchorMissing(const QString& anchor)
{
    setStatus(QStringLiteral("info"), tr("Layer anchor \"%1\" is missing from this basemap").arg(anchor));
}

void ChromeController::onStyleWillLoad()
{
    m_styleReady = false;
    m_pinInstalled = false;
}

void ChromeController::onStyleLoaded()
{
    m_styleReady = true;
    m_pinInstalled = false;  // the new style has none of it
    if (m_pinLonLat) installPin();
    clearStatus(QStringLiteral("info"));
}

// ── layers panel model (M8.20) ───────────────────────────────────────────

void ChromeController::onSceneGroupsChanged(const QJsonArray& groups)
{
    m_sceneGroups = groups;
    rebuildGroupsModel();
}

void ChromeController::onBasemapDescriptor(const QJsonObject& descriptor)
{
    m_basemapDetails = descriptor.value(QStringLiteral("details")).toArray();
    if (SceneApplier* applier = m_view ? m_view->sceneApplier() : nullptr) applier->setBasemapDetails(m_basemapDetails);
    rebuildGroupsModel();
    emit basemapsChanged();
}

void ChromeController::onCatalogue(const QJsonArray& descriptors)
{
    QVariantList list;
    for (const QJsonValue& v : descriptors) {
        const QJsonObject d = v.toObject();
        list.append(QVariantMap{{QStringLiteral("id"), d.value(QStringLiteral("id")).toString()},
                                {QStringLiteral("name"), d.value(QStringLiteral("name")).toString(d.value(QStringLiteral("id")).toString())},
                                // M9.1a: the tile says `label`, falling back to `name`.
                                {QStringLiteral("label"), d.value(QStringLiteral("label")).toString(d.value(QStringLiteral("name")).toString(d.value(QStringLiteral("id")).toString()))},
                                {QStringLiteral("thumbnail"), d.value(QStringLiteral("thumbnail")).toString()},
                                {QStringLiteral("format"), d.value(QStringLiteral("format")).toString()}});
    }
    if (list == m_basemaps) return;
    m_basemaps = list;
    emit basemapsChanged();
    emit groupsChanged();  // layersAvailable
}

QString ChromeController::basemap() const { return m_view ? m_view->basemap() : QString(); }

void ChromeController::onGroupVisibilityChanged(const QString& groupId, bool visible)
{
    QStringList groups = m_hiddenGroups;
    if (visible) groups.removeAll(groupId);
    else if (!groups.contains(groupId)) groups << groupId;
    setHidden(groups, m_hiddenDetails, m_shownDetails);
}

void ChromeController::rebuildGroupsModel()
{
    QVariantList groups;
    QJsonArray sorted = m_sceneGroups;
    std::vector<QJsonObject> items;
    for (const QJsonValue& v : sorted) items.push_back(v.toObject());
    std::stable_sort(items.begin(), items.end(), [](const QJsonObject& a, const QJsonObject& b) {
        const int oa = a.value(QStringLiteral("order")).toInt(), ob = b.value(QStringLiteral("order")).toInt();
        if (oa != ob) return oa < ob;
        return a.value(QStringLiteral("id")).toString() < b.value(QStringLiteral("id")).toString();
    });
    for (const QJsonObject& g : items) {
        const QString id = g.value(QStringLiteral("id")).toString();
        QVariantMap entry = g.toVariantMap();
        entry.insert(QStringLiteral("visible"), !m_hiddenGroups.contains(id));
        if (!entry.contains(QStringLiteral("toggleable"))) entry.insert(QStringLiteral("toggleable"), true);
        if (!entry.contains(QStringLiteral("legend"))) entry.insert(QStringLiteral("legend"), QVariantList());
        groups.append(entry);
    }
    QVariantList details;
    for (const QJsonValue& v : m_basemapDetails) {
        const QJsonObject d = v.toObject();
        const QString id = d.value(QStringLiteral("id")).toString();
        QVariantMap entry = d.toVariantMap();
        bool visible = d.value(QStringLiteral("defaultVisible")).toBool(true);
        if (m_hiddenDetails.contains(id)) visible = false;
        else if (m_shownDetails.contains(id)) visible = true;
        entry.insert(QStringLiteral("visible"), visible);
        details.append(entry);
    }
    const bool changed = groups != m_groups || details != m_details;
    m_groups = groups;
    m_details = details;
    if (changed) emit groupsChanged();
}

void ChromeController::setPanelOpen(bool open)
{
    if (m_panelOpen == open) return;
    m_panelOpen = open;
    emit panelOpenChanged();
}

void ChromeController::setGroupVisible(const QString& groupId, bool visible)
{
    QString session;
    for (const QJsonValue& v : m_sceneGroups) {
        const QJsonObject g = v.toObject();
        if (g.value(QStringLiteral("id")).toString() == groupId) {
            session = g.value(QStringLiteral("session")).toString();
            break;
        }
    }
    if (session.isEmpty()) {
        const int colon = groupId.indexOf(QLatin1Char(':'));
        if (colon > 0) session = groupId.left(colon);
    }
    if (session.isEmpty()) return;
    // Apply locally at once (the core echoes viewOptionsChanged; idempotent).
    onGroupVisibilityChanged(groupId, visible);
    emit groupVisibilityRequested(session, groupId, visible);
}

void ChromeController::setDetailVisible(const QString& detailId, bool visible)
{
    QStringList hidden = m_hiddenDetails, shown = m_shownDetails;
    hidden.removeAll(detailId);
    shown.removeAll(detailId);
    bool defaultVisible = true;
    for (const QJsonValue& v : m_basemapDetails) {
        const QJsonObject d = v.toObject();
        if (d.value(QStringLiteral("id")).toString() == detailId) defaultVisible = d.value(QStringLiteral("defaultVisible")).toBool(true);
    }
    if (visible && !defaultVisible) shown << detailId;
    if (!visible && defaultVisible) hidden << detailId;
    setHidden(m_hiddenGroups, hidden, shown);
    // M8.20: the choice lives in the view options so it survives a reload.
    QJsonObject options{{QStringLiteral("v"), 1},
                        {QStringLiteral("hiddenDetails"), QJsonArray::fromStringList(m_hiddenDetails)},
                        {QStringLiteral("shownDetails"), QJsonArray::fromStringList(m_shownDetails)}};
    if (m_view) m_view->recordOptions(options);
}

void ChromeController::selectBasemap(const QString& id)
{
    if (id.isEmpty() || id == basemap()) return;
    emit basemapRequested(id);
}

// ── callout (M8.19) ──────────────────────────────────────────────────────

std::optional<ChromeController::CalloutAnchor> ChromeController::anchorFor(const QJsonObject& pick, QJsonObject* hitOut) const
{
    SceneApplier* applier = m_view ? m_view->sceneApplier() : nullptr;
    const QJsonArray lonLat = pick.value(QStringLiteral("lonLat")).toArray();
    for (const QJsonValue& v : pick.value(QStringLiteral("hits")).toArray()) {
        const QJsonObject hit = v.toObject();
        const QString layerId = hit.value(QStringLiteral("layerId")).toString();
        if (layerId == QLatin1String(kPinLayer)) {
            CalloutAnchor a;
            a.builtin = true;
            a.session = QStringLiteral("maps/builtin");
            a.layerId = layerId;
            a.sourceId = QLatin1String(kPinSource);
            a.featureId = hit.value(QStringLiteral("featureId")).toVariant().toString();
            a.lon = m_pinLonLat ? m_pinLonLat->x() : lonLat.at(0).toDouble();
            a.lat = m_pinLonLat ? m_pinLonLat->y() : lonLat.at(1).toDouble();
            if (hitOut) *hitOut = hit;
            return a;
        }
        const auto info = applier ? applier->layerInfo(layerId) : std::nullopt;
        if (!info || (info->callout.isEmpty() && !info->selectable)) continue;
        CalloutAnchor a;
        a.session = hit.value(QStringLiteral("session")).toString();
        a.layerId = layerId;
        a.sourceId = hit.value(QStringLiteral("sourceId")).toString();
        a.featureId = hit.value(QStringLiteral("featureId")).toVariant().toString();
        a.lon = lonLat.at(0).toDouble();
        a.lat = lonLat.at(1).toDouble();
        if (hitOut) *hitOut = hit;
        return a;
    }
    return std::nullopt;
}

void ChromeController::onFeaturePicked(const QJsonObject& payload)
{
    const QString kind = payload.value(QStringLiteral("kind")).toString();
    const QJsonArray hits = payload.value(QStringLiteral("hits")).toArray();
    const QJsonArray lonLat = payload.value(QStringLiteral("lonLat")).toArray();
    if (kind == QLatin1String("longPress") || kind == QLatin1String("context")) {
        // M8.21: no scene hit -> drop a pin; with one the pick stands alone.
        // Basemap features (session "") sit under almost every point and do
        // not count, or the pin would only ever land on open water.
        bool sceneHit = false;
        for (const QJsonValue& h : hits)
            if (!h.toObject().value(QStringLiteral("session")).toString().isEmpty()) { sceneHit = true; break; }
        if (!sceneHit && lonLat.size() == 2) dropPin(lonLat.at(0).toDouble(), lonLat.at(1).toDouble());
        return;
    }
    if (kind != QLatin1String("tap")) return;
    // Any tap on the map dismisses the layers sheet, as Vela's sheet and Pure
    // Maps' basemap panel do when the map is touched outside them.
    setPanelOpen(false);
    QJsonObject hit;
    const auto anchor = anchorFor(payload, &hit);
    if (!anchor) {
        mapTapped();
        return;
    }
    if (anchor->builtin) {
        if (m_pinLonLat) {
            QJsonObject content{{QStringLiteral("title"), formatCoordinate(m_pinLonLat->x(), m_pinLonLat->y())},
                                {QStringLiteral("actions"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("copy")}, {QStringLiteral("label"), tr("Copy")}},
                                                                       QJsonObject{{QStringLiteral("id"), QStringLiteral("share")}, {QStringLiteral("label"), tr("Share")}}}}};
            if (m_callout.value(QStringLiteral("open")).toBool() && m_calloutAnchor && m_calloutAnchor->builtin) return;
            openCallout(*anchor, content, QJsonObject());
        }
        return;
    }
    SceneApplier* applier = m_view ? m_view->sceneApplier() : nullptr;
    const auto info = applier ? applier->layerInfo(anchor->layerId) : std::nullopt;
    const QJsonObject properties = hit.value(QStringLiteral("properties")).toObject();
    if (info && !info->callout.isEmpty() && itemEnabled(QStringLiteral("callouts"))) {
        openCallout(*anchor, info->callout, properties);
    } else if (info && info->selectable) {
        // Selectable without a callout: selection only.
        if (m_calloutAnchor && !m_calloutAnchor->builtin) closeCallout(QStringLiteral("tap"));
        if (m_calloutAnchor) setSelected(*m_calloutAnchor, false);
        m_calloutAnchor = anchor;
        setSelected(*anchor, true);
    }
}

void ChromeController::openCallout(const CalloutAnchor& anchor, const QJsonObject& content, const QJsonObject& properties)
{
    const bool wasOpen = m_callout.value(QStringLiteral("open")).toBool();
    if (m_calloutAnchor && !m_calloutAnchor->builtin && !(m_calloutAnchor->sourceId == anchor.sourceId && m_calloutAnchor->featureId == anchor.featureId))
        setSelected(*m_calloutAnchor, false);
    if (wasOpen) report(QStringLiteral("calloutClosed"), QJsonObject{{QStringLiteral("reason"), QStringLiteral("tap")}});

    m_calloutAnchor = anchor;
    m_calloutProperties = properties;
    QVariantList actions;
    for (const QJsonValue& v : content.value(QStringLiteral("actions")).toArray()) {
        if (actions.size() >= 8) break;
        const QJsonObject a = v.toObject();
        actions.append(QVariantMap{{QStringLiteral("id"), a.value(QStringLiteral("id")).toString()},
                                   {QStringLiteral("label"), expandTemplate(a.value(QStringLiteral("label")).toString(), properties)}});
    }
    m_callout = QVariantMap{
        {QStringLiteral("open"), true},
        {QStringLiteral("title"), expandTemplate(content.value(QStringLiteral("title")).toString(), properties)},
        {QStringLiteral("subtitle"), expandTemplate(content.value(QStringLiteral("subtitle")).toString(), properties)},
        {QStringLiteral("icon"), content.value(QStringLiteral("icon")).toString()},
        {QStringLiteral("actions"), actions},
        {QStringLiteral("builtin"), anchor.builtin},
        {QStringLiteral("session"), anchor.session},
        {QStringLiteral("layerId"), anchor.layerId},
        {QStringLiteral("sourceId"), anchor.sourceId},
        {QStringLiteral("featureId"), anchor.featureId},
        {QStringLiteral("lon"), anchor.lon},
        {QStringLiteral("lat"), anchor.lat},
        {QStringLiteral("px"), QVariantList{0.0, 0.0}},
        {QStringLiteral("maxWidth"), 240},
    };
    updateCalloutPosition();
    emit calloutChanged();
    if (!anchor.builtin) {
        SceneApplier* applier = m_view ? m_view->sceneApplier() : nullptr;
        const auto info = applier ? applier->layerInfo(anchor.layerId) : std::nullopt;
        if (info && info->selectable) setSelected(anchor, true);
        QJsonObject payload = anchorJson(anchor.session, anchor.layerId, anchor.sourceId, anchor.featureId, anchor.lon, anchor.lat);
        payload.insert(QStringLiteral("properties"), properties);
        report(QStringLiteral("calloutOpened"), payload);
    }
}

void ChromeController::updateCalloutPosition()
{
    if (!m_callout.value(QStringLiteral("open")).toBool() || !m_calloutAnchor) return;
    QMapLibre::Map* m = map();
    if (!m) return;
    const QPointF px = m->pixelForCoordinate(QMapLibre::Coordinate(m_calloutAnchor->lat, m_calloutAnchor->lon));
    const QVariantList current = m_callout.value(QStringLiteral("px")).toList();
    if (current.size() == 2 && std::fabs(current[0].toDouble() - px.x()) < 0.01 && std::fabs(current[1].toDouble() - px.y()) < 0.01) return;
    m_callout.insert(QStringLiteral("px"), QVariantList{px.x(), px.y()});
    emit calloutChanged();
}

void ChromeController::onCalloutContent(const QJsonObject& content)
{
    // M8.19 / M8.21: a module names the pin (or replaces the open callout's
    // content). Without an open anchor the pin is the target; with neither
    // the content has nowhere to go.
    if (m_calloutAnchor) {
        openCallout(*m_calloutAnchor, content, m_calloutProperties);
    } else if (m_pinLonLat) {
        CalloutAnchor a;
        a.builtin = true;
        a.session = QStringLiteral("maps/builtin");
        a.layerId = QLatin1String(kPinLayer);
        a.sourceId = QLatin1String(kPinSource);
        a.featureId = m_pin.value(QStringLiteral("featureId")).toString();
        a.lon = m_pinLonLat->x();
        a.lat = m_pinLonLat->y();
        openCallout(a, content, QJsonObject());
    }
}

void ChromeController::calloutAction(const QString& actionId)
{
    if (!m_callout.value(QStringLiteral("open")).toBool() || !m_calloutAnchor) return;
    if (m_calloutAnchor->builtin) {
        if (actionId == QLatin1String("copy")) copyPin();
        else if (actionId == QLatin1String("share") && m_view) m_view->requestShare(QStringLiteral("share"), sharePin());
        return;  // the pin callout stays (the user may copy, then share)
    }
    const CalloutAnchor a = *m_calloutAnchor;
    QJsonObject payload = anchorJson(a.session, a.layerId, a.sourceId, a.featureId, a.lon, a.lat);
    payload.insert(QStringLiteral("actionId"), actionId);
    payload.insert(QStringLiteral("properties"), m_calloutProperties);
    report(QStringLiteral("calloutAction"), payload);
    closeCallout(QStringLiteral("action"));
}

void ChromeController::closeCallout(const QString& reason)
{
    const bool open = m_callout.value(QStringLiteral("open")).toBool();
    if (m_calloutAnchor && !m_calloutAnchor->builtin) setSelected(*m_calloutAnchor, false);
    m_calloutAnchor.reset();
    m_calloutProperties = QJsonObject();
    if (!open) return;
    m_callout = QVariantMap{{QStringLiteral("open"), false}};
    emit calloutChanged();
    report(QStringLiteral("calloutClosed"), QJsonObject{{QStringLiteral("reason"), reason}});
}

void ChromeController::onLayersRemoved(const QStringList& layerIds)
{
    if (m_calloutAnchor && layerIds.contains(m_calloutAnchor->layerId)) closeCallout(QStringLiteral("removed"));
    if (m_hover && layerIds.contains(m_hover->layerId)) m_hover.reset();
}

void ChromeController::mapTapped()
{
    // A tap away from any anchor: close the callout; a tap away from the
    // pin's callout also clears the pin (M8.21).
    const bool builtin = m_calloutAnchor && m_calloutAnchor->builtin;
    if (m_calloutAnchor) closeCallout(QStringLiteral("tap"));
    if (builtin) clearPin();
}

// ── selection / hover state (M8.15a) ─────────────────────────────────────

void ChromeController::setSelected(const CalloutAnchor& anchor, bool selected)
{
    if (anchor.builtin || anchor.sourceId.isEmpty() || anchor.featureId.isEmpty()) return;
    emit featureStateRequested(anchor.session, anchor.sourceId, anchor.featureId, QJsonObject{{QStringLiteral("selected"), selected}});
}

void ChromeController::setHover(const std::optional<CalloutAnchor>& anchor)
{
    const bool same = (!anchor && !m_hover) || (anchor && m_hover && anchor->sourceId == m_hover->sourceId && anchor->featureId == m_hover->featureId);
    if (same) return;
    if (m_hover && !m_hover->builtin) emit featureStateRequested(m_hover->session, m_hover->sourceId, m_hover->featureId, QJsonObject{{QStringLiteral("hover"), false}});
    m_hover = anchor;
    if (m_hover && !m_hover->builtin) emit featureStateRequested(m_hover->session, m_hover->sourceId, m_hover->featureId, QJsonObject{{QStringLiteral("hover"), true}});
}

// ── dropped pin (M8.21) ──────────────────────────────────────────────────

void ChromeController::dropPin(double lon, double lat)
{
    if (m_calloutAnchor) closeCallout(QStringLiteral("tap"));
    m_pinLonLat = QPointF(lon, lat);
    const QString featureId = QStringLiteral("pin:%1,%2").arg(QString::number(lat, 'f', 5), QString::number(lon, 'f', 5));
    m_pin = QVariantMap{{QStringLiteral("active"), true}, {QStringLiteral("lon"), lon}, {QStringLiteral("lat"), lat},
                        {QStringLiteral("featureId"), featureId}, {QStringLiteral("text"), formatCoordinate(lon, lat)}};
    installPin();
    emit pinChanged();  // the ViewController re-syncs the picker's runtime layers
    report(QStringLiteral("pinDropped"), QJsonObject{{QStringLiteral("lonLat"), QJsonArray{lon, lat}}});

    CalloutAnchor a;
    a.builtin = true;
    a.session = QStringLiteral("maps/builtin");
    a.layerId = QLatin1String(kPinLayer);
    a.sourceId = QLatin1String(kPinSource);
    a.featureId = featureId;
    a.lon = lon;
    a.lat = lat;
    openCallout(a,
                QJsonObject{{QStringLiteral("title"), formatCoordinate(lon, lat)},
                            {QStringLiteral("actions"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("copy")}, {QStringLiteral("label"), tr("Copy")}},
                                                                   QJsonObject{{QStringLiteral("id"), QStringLiteral("share")}, {QStringLiteral("label"), tr("Share")}}}}},
                QJsonObject());
}

void ChromeController::installPin()
{
    QMapLibre::Map* m = map();
    if (!m || !m_styleReady || !m_pinLonLat) return;
    const QString source = QLatin1String(kPinSource);
    const QString layer = QLatin1String(kPinLayer);
    const QByteArray data = QJsonDocument(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("FeatureCollection")},
        {QStringLiteral("features"), QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("Feature")},
            {QStringLiteral("id"), m_pin.value(QStringLiteral("featureId")).toString()},
            {QStringLiteral("properties"), QJsonObject{{QStringLiteral("id"), m_pin.value(QStringLiteral("featureId")).toString()}}},
            {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Point")},
                                                     {QStringLiteral("coordinates"), QJsonArray{m_pinLonLat->x(), m_pinLonLat->y()}}}}}}}}).toJson(QJsonDocument::Compact);
    if (!m->sourceExists(source)) m->addSource(source, {{QStringLiteral("type"), QStringLiteral("geojson")}, {QStringLiteral("data"), data}});
    else m->updateSource(source, {{QStringLiteral("data"), data}});
    if (m->layerExists(layer)) m->removeLayer(layer);  // re-adding raises it above the scene
    {
        const QString accent = m_theme.value(QStringLiteral("accent")).toString();
        m->addLayer(layer, {{QStringLiteral("type"), QStringLiteral("circle")},
                            {QStringLiteral("source"), source},
                            {QStringLiteral("paint"), QVariantMap{{QStringLiteral("circle-radius"), 7},
                                                                  {QStringLiteral("circle-color"), accent},
                                                                  {QStringLiteral("circle-stroke-color"), QStringLiteral("#ffffff")},
                                                                  {QStringLiteral("circle-stroke-width"), 2},
                                                                  {QStringLiteral("circle-pitch-alignment"), QStringLiteral("map")}}}});
    }
    m_pinInstalled = true;
}

void ChromeController::updatePinSource()
{
    if (m_pinInstalled) installPin();
}

void ChromeController::clearPin()
{
    if (!m_pinLonLat) return;
    if (m_calloutAnchor && m_calloutAnchor->builtin) closeCallout(QStringLiteral("tap"));
    m_pinLonLat.reset();
    m_pin = QVariantMap{{QStringLiteral("active"), false}};
    if (QMapLibre::Map* m = map(); m && m_pinInstalled) {
        if (m->layerExists(QLatin1String(kPinLayer))) m->removeLayer(QLatin1String(kPinLayer));
        if (m->sourceExists(QLatin1String(kPinSource))) m->removeSource(QLatin1String(kPinSource));
    }
    m_pinInstalled = false;
    emit pinChanged();
    report(QStringLiteral("pinCleared"), QJsonObject());
}

void ChromeController::copyPin()
{
    if (!m_pinLonLat || !m_view) return;
    m_view->requestShare(QStringLiteral("copy"), formatCoordinate(m_pinLonLat->x(), m_pinLonLat->y()));
}

QString ChromeController::sharePin()
{
    if (!m_pinLonLat) return QString();
    return QStringLiteral("geo:%1,%2").arg(QString::number(m_pinLonLat->y(), 'f', 6), QString::number(m_pinLonLat->x(), 'f', 6));
}

// ── escape chain (M8.22) ─────────────────────────────────────────────────

QString ChromeController::escape()
{
    if (m_callout.value(QStringLiteral("open")).toBool()) {
        const bool builtin = m_calloutAnchor && m_calloutAnchor->builtin;
        closeCallout(QStringLiteral("escape"));
        if (builtin) {  // the pin's callout and the pin go together
            clearPin();
            return QStringLiteral("pin");
        }
        return QStringLiteral("callout");
    }
    if (m_pinLonLat) {
        clearPin();
        return QStringLiteral("pin");
    }
    if (m_panelOpen) {
        setPanelOpen(false);
        return QStringLiteral("panel");
    }
    if (m_follow != QLatin1String("off")) {
        endFollow(QStringLiteral("user"));
        return QStringLiteral("follow");
    }
    return QString();
}

}  // namespace maps::view
