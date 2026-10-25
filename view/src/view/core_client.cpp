#include "view/core_client.h"

#include <QJsonDocument>

namespace maps::view {

QJsonValue jsonFromText(const QString& text)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return QJsonValue();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(t.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) return QJsonValue(text);  // a plain string result
    if (doc.isObject()) return doc.object();
    if (doc.isArray()) return doc.array();
    return QJsonValue(text);
}

CoreReply replyFromResult(bool success, const QJsonValue& value, const QString& error)
{
    CoreReply r;
    r.ok = success;
    r.value = value.isString() ? jsonFromText(value.toString()) : value;
    r.error = success ? QString() : (error.isEmpty() ? QStringLiteral("maps.unknown: call failed") : error);
    return r;
}

}  // namespace maps::view
