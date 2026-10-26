// LogosctlCoreClient: CoreClient over the logosctl CLI, for the dev launcher
// (docs/architecture.md section 16.7 tooling). Each call is one
// `logosctl call maps_core <method> args...` process (piped output is one JSON
// object: {"status","module","method","result"}); events come from a long-
// running `logosctl watch maps_core` whose JSON lines are
// {"timestamp","module","event","data":{"arg0":"<json text>"}}.
//
// Not a production path: the plugin uses SdkCoreClient. getResource answers
// "unavailable" (logosctl cannot stream bytes from arbitrary modules).
#pragma once

#include "view/core_client.h"

#include <QProcess>
#include <QStringList>

#include <memory>

namespace maps::view::devtools {

class LogosctlCoreClient final : public CoreClient {
    Q_OBJECT
public:
    /// `logosctl` is the binary; `environment` is applied to every process
    /// (LOGOSCTL_CONFIG_DIR, HOME for an isolated session).
    LogosctlCoreClient(QString logosctl, QProcessEnvironment environment, QObject* parent = nullptr);
    ~LogosctlCoreClient() override;

    void startWatching();
    int inFlight() const { return m_inFlight; }

    void openView(const QString& viewId, const QString& surfaceToken, const QString& capabilitiesJson, CoreCallback cb) override;
    void closeView(const QString& viewId, CoreCallback cb) override;
    void ackRevision(const QString& viewId, quint64 revision, CoreCallback cb) override;
    void reportViewState(const QString& viewId, const QString& stateJson, CoreCallback cb) override;
    void reportViewEvent(const QString& viewId, const QString& topic, const QString& payloadJson, CoreCallback cb) override;
    void getScene(CoreCallback cb) override;
    void getChanges(quint64 sinceRevision, CoreCallback cb) override;
    void getLayerOrder(const QString& viewId, CoreCallback cb) override;
    void getBasemap(const QString& id, CoreCallback cb) override;
    void patchStyle(const QString& id, const QString& styleJson, CoreCallback cb) override;
    void listBasemaps(const QString& filterJson, CoreCallback cb) override;
    void setBasemap(const QString& viewId, const QString& id, CoreCallback cb) override;
    void setGroupVisible(const QString& viewId, const QString& session, const QString& groupId, bool visible, CoreCallback cb) override;
    void setFeatureState(const QString& session, const QString& sourceId, const QString& featureId, const QString& stateJson, CoreCallback cb) override;
    void listProviders(CoreCallback cb) override;
    void getResource(const QString& module, const QString& path, qint64 rangeStart, qint64 rangeEnd,
                     std::function<void(bool, const QByteArray&, const QString&, const QString&)> cb) override;

private:
    enum class Shape { Result, Bool, Text };
    /// Arguments are already typed for logosctl ("str:…", "json:…", "@file", bare numbers).
    void call(const QString& method, QStringList args, Shape shape, CoreCallback cb);
    static QString str(const QString& s) { return QStringLiteral("str:") + s; }
    QString viaFile(const QString& text);
    void onWatchLine(const QByteArray& line);

    QString m_logosctl;
    QProcessEnvironment m_env;
    std::unique_ptr<QProcess> m_watch;
    QByteArray m_watchBuffer;
    QStringList m_tempFiles;
    int m_inFlight = 0;
};

}  // namespace maps::view::devtools
