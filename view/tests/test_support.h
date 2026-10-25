// Minimal check/wait helpers shared by the maps_view conformance tests.
// No framework: each test is one executable, exit code is the verdict, and
// every assertion prints its line so a failure in the nix log is locatable.
#pragma once

#include <QByteArray>
#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QString>

#include <cstdio>
#include <cstdlib>
#include <functional>

namespace maps::test {

inline int& failures()
{
    static int n = 0;
    return n;
}

inline void report(bool ok, const char* file, int line, const char* expr, const QString& detail = QString())
{
    if (ok) {
        std::fprintf(stderr, "ok    %s\n", expr);
        return;
    }
    ++failures();
    std::fprintf(stderr, "FAIL  %s:%d  %s%s%s\n", file, line, expr, detail.isEmpty() ? "" : "  -- ", qPrintable(detail));
}

#define CHECK(cond) ::maps::test::report(static_cast<bool>(cond), __FILE__, __LINE__, #cond)
#define CHECK_MSG(cond, msg) ::maps::test::report(static_cast<bool>(cond), __FILE__, __LINE__, #cond, (msg))
#define REQUIRE(cond)                                                                  \
    do {                                                                               \
        const bool _ok = static_cast<bool>(cond);                                      \
        ::maps::test::report(_ok, __FILE__, __LINE__, #cond);                          \
        if (!_ok) {                                                                    \
            std::fprintf(stderr, "ABORT: required condition failed\n");                \
            std::exit(1);                                                              \
        }                                                                              \
    } while (0)

/// Pump the event loop until `pred` holds or `ms` elapsed.
inline bool waitFor(const std::function<bool()>& pred, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
    return true;
}

#define WAIT_FOR(pred) CHECK_MSG(::maps::test::waitFor([&] { return static_cast<bool>(pred); }), QStringLiteral("timed out waiting for " #pred))
#define WAIT_FOR_MS(pred, ms) CHECK_MSG(::maps::test::waitFor([&] { return static_cast<bool>(pred); }, (ms)), QStringLiteral("timed out waiting for " #pred))

/// Pump for a fixed time (to observe that nothing happens).
inline void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

/// A solid PNG tile.
inline QByteArray pngTile(int size, const QColor& color)
{
    QImage img(size, size, QImage::Format_ARGB32);
    img.fill(color);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

inline int finish()
{
    if (failures() == 0) {
        std::fprintf(stderr, "ALL OK\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILED\n", failures());
    return 1;
}

}  // namespace maps::test
