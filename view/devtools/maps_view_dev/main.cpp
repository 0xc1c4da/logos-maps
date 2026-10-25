// maps_view_dev: run the maps view as a desktop window against a maps_core
// loaded in a logosctl session (docs/architecture.md section 16.7 tooling).
//
//   LOGOSCTL_CONFIG_DIR=... maps_view_dev [--logosctl PATH] [--session NAME]
//                                         [--style FILE] [--qml-dir DIR]
//                                         [--size WxH] [--no-core]
//
// --style loads a style JSON directly (no core needed); --no-core runs the
// view standalone with providers only. Everything else goes through
// `logosctl call/watch maps_core`.
#include "logosctl_core_client.h"

#include "providers/memory_file_source.h"
#include "providers/provider_registry.h"
#include "view/map_item.h"
#include "view/scene_applier.h"
#include "view/style_loader.h"
#include "view/view_controller.h"
#include "view/view_window.h"

#include <QCommandLineParser>
#include <QFile>
#include <QGuiApplication>
#include <QTimer>

#include <cstdio>

using namespace maps::view;

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("maps_view_dev"));
    QCoreApplication::setOrganizationName(QStringLiteral("logos"));

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption logosctlOpt(QStringLiteral("logosctl"), QStringLiteral("logosctl binary"), QStringLiteral("path"), QStringLiteral("logosctl"));
    QCommandLineOption sessionOpt(QStringLiteral("session"), QStringLiteral("view session name"), QStringLiteral("name"), QStringLiteral("dev"));
    QCommandLineOption styleOpt(QStringLiteral("style"), QStringLiteral("style JSON file to load directly"), QStringLiteral("file"));
    QCommandLineOption qmlOpt(QStringLiteral("qml-dir"), QStringLiteral("directory holding MapWindow.qml"), QStringLiteral("dir"));
    QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("window size WxH"), QStringLiteral("wxh"), QStringLiteral("1024x768"));
    QCommandLineOption noCoreOpt(QStringLiteral("no-core"), QStringLiteral("run without maps_core"));
    QCommandLineOption quitAfterOpt(QStringLiteral("quit-after"), QStringLiteral("exit after N ms (smoke tests)"), QStringLiteral("ms"));
    parser.addOptions({logosctlOpt, sessionOpt, styleOpt, qmlOpt, sizeOpt, noCoreOpt, quitAfterOpt});
    parser.process(app);

    const QStringList wh = parser.value(sizeOpt).split(QLatin1Char('x'));
    const QSize size(wh.value(0).toInt() > 0 ? wh.value(0).toInt() : 1024, wh.value(1).toInt() > 0 ? wh.value(1).toInt() : 768);

    std::unique_ptr<devtools::LogosctlCoreClient> core;
    if (!parser.isSet(noCoreOpt)) {
        core = std::make_unique<devtools::LogosctlCoreClient>(parser.value(logosctlOpt), QProcessEnvironment::systemEnvironment());
        core->startWatching();
    }

    ViewController::Options options;
    options.moduleName = QStringLiteral("maps_view");
    options.session = parser.value(sessionOpt);
    options.surfaceTransport = QStringLiteral("none");
    options.qmlDir = parser.value(qmlOpt);
    options.moduleDir = ViewWindow::pluginDirectory();
    options.initialSize = size;
    options.registerLogosProvider = core != nullptr;

    ViewController controller(core.get(), options);
    QObject::connect(&controller, &ViewController::statusChanged, [](const QString& s) { std::fprintf(stderr, "[maps_view_dev] status %s\n", qPrintable(s)); });
    QObject::connect(&controller, &ViewController::error, [](const QString& code, const QString& msg) {
        std::fprintf(stderr, "[maps_view_dev] %s %s\n", qPrintable(code), qPrintable(msg));
    });
    QObject::connect(&controller, &ViewController::cameraIdle, [](const QString& json) { std::fprintf(stderr, "[maps_view_dev] idle %s\n", qPrintable(json)); });
    QObject::connect(&controller, &ViewController::attributionChanged, [](const QString& html) { std::fprintf(stderr, "[maps_view_dev] attribution %s\n", qPrintable(html)); });
    controller.start();

    if (parser.isSet(styleOpt)) {
        QFile f(parser.value(styleOpt));
        if (!f.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "cannot read %s\n", qPrintable(parser.value(styleOpt)));
            return 2;
        }
        const QString style = QString::fromUtf8(f.readAll());
        auto apply = [&controller, style] {
            if (controller.styleLoader()) controller.styleLoader()->applyStyleJson(style, QStringLiteral("--style"));
        };
        if (controller.styleLoader()) apply();
        else if (controller.mapItem()) QObject::connect(controller.mapItem(), &MapItem::mapCreated, &app, [apply] { QTimer::singleShot(0, apply); });
    }

    if (parser.isSet(quitAfterOpt)) QTimer::singleShot(parser.value(quitAfterOpt).toInt(), &app, &QCoreApplication::quit);
    const int rc = app.exec();
    controller.stop();
    return rc;
}
