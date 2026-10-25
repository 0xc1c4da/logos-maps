#include "logosctl_core_client.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QUuid>

namespace maps::view::devtools {

LogosctlCoreClient::LogosctlCoreClient(QString logosctl, QProcessEnvironment environment, QObject* parent)
    : CoreClient(parent), m_logosctl(std::move(logosctl)), m_env(std::move(environment)) {}

LogosctlCoreClient::~LogosctlCoreClient()
{
    if (m_watch) {
        m_watch->terminate();
        m_watch->waitForFinished(1000);
    }
    for (const QString& f : m_tempFiles) QFile::remove(f);
}

void LogosctlCoreClient::startWatching()
{
    if (m_watch) return;
    m_watch = std::make_unique<QProcess>(this);
    m_watch->setProcessEnvironment(m_env);
    m_watch->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_watch.get(), &QProcess::readyReadStandardOutput, this, [this] {
        m_watchBuffer += m_watch->readAllStandardOutput();
        int nl;
        while ((nl = m_watchBuffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_watchBuffer.left(nl).trimmed();
            m_watchBuffer.remove(0, nl + 1);
            if (!line.isEmpty()) onWatchLine(line);
        }
    });
    connect(m_watch.get(), &QProcess::started, this, [this] { emit connectedChanged(true); });
    connect(m_watch.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int, QProcess::ExitStatus) {
        emit connectedChanged(false);
    });
    m_watch->start(m_logosctl, {QStringLiteral("watch"), QStringLiteral("maps_core")});
}

void LogosctlCoreClient::onWatchLine(const QByteArray& line)
{
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject()) return;
    const QJsonObject ev = doc.object();
    const QString topic = ev.value(QStringLiteral("event")).toString();
    const QJsonObject data = ev.value(QStringLiteral("data")).toObject();
    const QJsonValue arg0 = data.value(QStringLiteral("arg0"));
    const QJsonValue payload = arg0.isString() ? jsonFromText(arg0.toString()) : arg0;
    emit coreEvent(topic, payload.toObject());
}

QString LogosctlCoreClient::viaFile(const QString& text)
{
    QTemporaryFile f(QDir::tempPath() + QStringLiteral("/maps_view_dev_XXXXXX.json"));
    f.setAutoRemove(false);
    if (!f.open()) return str(text);
    f.write(text.toUtf8());
    f.close();
    m_tempFiles << f.fileName();
    if (m_tempFiles.size() > 64) {
        QFile::remove(m_tempFiles.takeFirst());
    }
    return QStringLiteral("@") + f.fileName();
}

void LogosctlCoreClient::call(const QString& method, QStringList args, Shape shape, CoreCallback cb)
{
    auto* proc = new QProcess(this);
    proc->setProcessEnvironment(m_env);
    ++m_inFlight;
    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, proc, method, shape, cb](int, QProcess::ExitStatus) {
        --m_inFlight;
        const QByteArray out = proc->readAllStandardOutput();
        const QByteArray err = proc->readAllStandardError();
        proc->deleteLater();
        // The reply is the last JSON line.
        QByteArray last;
        for (const QByteArray& l : out.split('\n')) if (!l.trimmed().isEmpty()) last = l.trimmed();
        const QJsonObject envelope = QJsonDocument::fromJson(last).object();
        CoreReply reply;
        if (envelope.value(QStringLiteral("status")).toString() != QLatin1String("ok")) {
            reply.ok = false;
            reply.error = QStringLiteral("maps.transport: logosctl %1: %2").arg(method, envelope.value(QStringLiteral("message")).toString(QString::fromUtf8(err).trimmed()));
            if (cb) cb(reply);
            return;
        }
        const QJsonValue result = envelope.value(QStringLiteral("result"));
        switch (shape) {
        case Shape::Result: {
            const QJsonObject r = result.toObject();
            reply = replyFromResult(r.value(QStringLiteral("success")).toBool(), r.value(QStringLiteral("value")), r.value(QStringLiteral("error")).toString());
            break;
        }
        case Shape::Bool:
            reply = replyFromResult(result.toBool(), result, result.toBool() ? QString() : QStringLiteral("maps.rejected: the core returned false"));
            break;
        case Shape::Text:
            reply = replyFromResult(true, result.isString() ? jsonFromText(result.toString()) : result, QString());
            break;
        }
        if (cb) cb(reply);
    });
    QStringList full{QStringLiteral("call"), QStringLiteral("maps_core"), method};
    full += args;
    proc->start(m_logosctl, full);
}

void LogosctlCoreClient::openView(const QString& viewId, const QString& surfaceToken, const QString& capabilitiesJson, CoreCallback cb)
{
    call(QStringLiteral("openView"), {str(viewId), str(surfaceToken), viaFile(capabilitiesJson)}, Shape::Result, std::move(cb));
}
void LogosctlCoreClient::closeView(const QString& viewId, CoreCallback cb)
{
    call(QStringLiteral("closeView"), {str(viewId)}, Shape::Bool, std::move(cb));
}
void LogosctlCoreClient::ackRevision(const QString& viewId, quint64 revision, CoreCallback cb)
{
    call(QStringLiteral("ackRevision"), {str(viewId), QString::number(revision)}, Shape::Bool, std::move(cb));
}
void LogosctlCoreClient::reportViewState(const QString& viewId, const QString& stateJson, CoreCallback cb)
{
    call(QStringLiteral("reportViewState"), {str(viewId), str(stateJson)}, Shape::Bool, std::move(cb));
}
void LogosctlCoreClient::reportViewEvent(const QString& viewId, const QString& topic, const QString& payloadJson, CoreCallback cb)
{
    call(QStringLiteral("reportViewEvent"), {str(viewId), str(topic), str(payloadJson)}, Shape::Bool, std::move(cb));
}
void LogosctlCoreClient::getScene(CoreCallback cb)
{
    call(QStringLiteral("getScene"), {}, Shape::Result, std::move(cb));
}
void LogosctlCoreClient::getChanges(quint64 sinceRevision, CoreCallback cb)
{
    call(QStringLiteral("getChanges"), {QString::number(sinceRevision)}, Shape::Result, std::move(cb));
}
void LogosctlCoreClient::getLayerOrder(const QString& viewId, CoreCallback cb)
{
    call(QStringLiteral("getLayerOrder"), {str(viewId)}, Shape::Result, std::move(cb));
}
void LogosctlCoreClient::getBasemap(const QString& id, CoreCallback cb)
{
    call(QStringLiteral("getBasemap"), {str(id)}, Shape::Result, std::move(cb));
}
void LogosctlCoreClient::patchStyle(const QString& id, const QString& styleJson, CoreCallback cb)
{
    call(QStringLiteral("patchStyle"), {str(id), viaFile(styleJson)}, Shape::Result, std::move(cb));
}
void LogosctlCoreClient::listBasemaps(const QString& filterJson, CoreCallback cb)
{
    call(QStringLiteral("listBasemaps"), {str(filterJson.isEmpty() ? QStringLiteral("{}") : filterJson)}, Shape::Text, std::move(cb));
}
void LogosctlCoreClient::setBasemap(const QString& viewId, const QString& id, CoreCallback cb)
{
    call(QStringLiteral("setBasemap"), {str(viewId), str(id)}, Shape::Result, std::move(cb));
}
void LogosctlCoreClient::listProviders(CoreCallback cb)
{
    call(QStringLiteral("listProviders"), {}, Shape::Text, std::move(cb));
}
void LogosctlCoreClient::getResource(const QString&, const QString&, qint64, qint64,
                                     std::function<void(bool, const QByteArray&, const QString&, const QString&)> cb)
{
    cb(false, {}, QStringLiteral("unavailable"), QStringLiteral("logosctl client cannot fetch module resources"));
}

}  // namespace maps::view::devtools
