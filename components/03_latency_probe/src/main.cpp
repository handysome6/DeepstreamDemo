#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QSurfaceFormat>
#include <QTimer>

#include <gst/gst.h>
#include <glib.h>

#include "LatencyAggregator.hpp"
#include "ProbeGLWidget.hpp"
#include "ProbeSource.hpp"

namespace {

// One-shot self-overhead measurement on startup, so the floor numbers in the
// success criteria can be interpreted with the right error bar. Reported on
// the first stdout line.
qint64 measureMonotonicCallOverheadUs() {
    constexpr int kIters = 100000;
    const qint64 t0 = qint64(g_get_monotonic_time());
    qint64 acc = 0;
    for (int i = 0; i < kIters; ++i) {
        acc ^= qint64(g_get_monotonic_time());
    }
    const qint64 t1 = qint64(g_get_monotonic_time());
    // Prevent the loop from being optimized out.
    if (acc == 0xDEADBEEF) {
        qInfo() << "(impossible coincidence; preventing dead-code elimination)";
    }
    return (t1 - t0) / kIters;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (qgetenv("ALLOW_EGL") == "1") {
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    } else {
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_glx");
    }

    gst_init(&argc, &argv);

    QApplication app(argc, argv);
    qRegisterMetaType<FrameHolder*>("FrameHolder*");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "P0.3 — source→paint latency measurement contract. Stamps t0 at the "
        "head of the pipeline via GstReferenceTimestampMeta, samples t1 at "
        "appsink, and t2 after frameSwapped. Emits per-second stdout "
        "summary plus a CSV row every N frames.");
    parser.addHelpOption();

    QCommandLineOption sourceOpt(QStringList() << "source",
        "Source mode: videotestsrc (default) or rtsp.", "mode", "videotestsrc");
    QCommandLineOption uriOpt(QStringList() << "uri",
        "RTSP URI (only used when --source=rtsp).", "uri",
        "rtsp://127.0.0.1:8554/p02cam");
    QCommandLineOption fpsOpt(QStringList() << "fps",
        "videotestsrc framerate.", "n", "30");
    QCommandLineOption widthOpt(QStringList() << "width",
        "videotestsrc width.", "px", "1920");
    QCommandLineOption heightOpt(QStringList() << "height",
        "videotestsrc height.", "px", "1080");
    QCommandLineOption vsyncOpt(QStringList() << "vsync",
        "Display vsync: on or off (default off).", "mode", "off");
    QCommandLineOption csvOpt(QStringList() << "csv",
        "CSV output path (default: logs/probe-<src>-<ts>.csv). Pass empty string to disable.",
        "path");
    QCommandLineOption windowOpt(QStringList() << "window-seconds",
        "Rolling window for stdout summaries.", "s", "5");
    QCommandLineOption csvEveryOpt(QStringList() << "csv-every",
        "Write a CSV row every N frames (0 disables).", "n", "30");
    QCommandLineOption durationOpt(QStringList() << "duration-seconds",
        "Auto-quit after N seconds (0 = run until manually closed).", "n", "0");
    QCommandLineOption rtpProtocolOpt(QStringList() << "rtp",
        "RTP transport for --source=rtsp: tcp (default) or udp.", "proto", "tcp");

    parser.addOption(sourceOpt);
    parser.addOption(uriOpt);
    parser.addOption(fpsOpt);
    parser.addOption(widthOpt);
    parser.addOption(heightOpt);
    parser.addOption(vsyncOpt);
    parser.addOption(csvOpt);
    parser.addOption(windowOpt);
    parser.addOption(csvEveryOpt);
    parser.addOption(durationOpt);
    parser.addOption(rtpProtocolOpt);
    parser.process(app);

    const QString sourceStr = parser.value(sourceOpt).toLower();
    ProbeSource::SourceMode sourceMode;
    QString sourceLabel;
    if (sourceStr == "videotestsrc") {
        sourceMode  = ProbeSource::SourceMode::VideoTestSrc;
        sourceLabel = "videotestsrc";
    } else if (sourceStr == "rtsp") {
        sourceMode  = ProbeSource::SourceMode::Rtsp;
        sourceLabel = "rtsp";
    } else {
        qFatal("Unknown --source value: %s (expected videotestsrc|rtsp)",
               qPrintable(sourceStr));
    }

    const bool vsync = (parser.value(vsyncOpt).toLower() == "on");
    const int durationSeconds = parser.value(durationOpt).toInt();

    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setMajorVersion(3);
    fmt.setMinorVersion(3);
    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    // 0 = no vsync, 1 = swap-on-vblank. Mesa / NVIDIA env vars in
    // run_in_container.sh are the second line of defense if a compositor
    // tries to override.
    fmt.setSwapInterval(vsync ? 1 : 0);
    QSurfaceFormat::setDefaultFormat(fmt);

    // CSV path resolution:
    //   --csv not provided        -> auto-generate logs/probe-<src>-<ts>.csv
    //   --csv ''                  -> explicit "off"
    //   --csv /some/path.csv      -> use literally
    QString csvPath;
    if (parser.isSet(csvOpt)) {
        csvPath = parser.value(csvOpt);  // may be "" (explicit disable)
    } else {
        const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
        csvPath = QStringLiteral("logs/probe-%1-%2.csv").arg(sourceLabel, ts);
    }

    LatencyAggregator::Options aggOpts;
    aggOpts.csvPath        = csvPath;
    aggOpts.sourceLabel    = sourceLabel;
    aggOpts.vsync          = vsync;
    aggOpts.windowSeconds  = parser.value(windowOpt).toInt();
    aggOpts.csvEveryFrames = parser.value(csvEveryOpt).toInt();
    LatencyAggregator agg(aggOpts);

    qInfo().noquote() << QString(
        "Probe self-overhead: g_get_monotonic_time avg=%1 us/call "
        "(if this is > 0.1 us, the floor numbers in README success criteria "
        "need to be relaxed by ~3x this).")
        .arg(measureMonotonicCallOverheadUs());
    qInfo().noquote() << QString(
        "CSV output: %1").arg(csvPath.isEmpty() ? "(disabled)" : csvPath);
    qInfo().noquote() << QString(
        "Auto-quit: %1").arg(durationSeconds > 0
            ? QString::number(durationSeconds) + " s"
            : QStringLiteral("disabled"));

    ProbeGLWidget widget(&agg);
    widget.resize(1280, 720);
    widget.setWindowTitle(QStringLiteral("03_latency_probe — %1 (vsync=%2)")
                          .arg(sourceLabel, vsync ? "on" : "off"));
    widget.show();

    ProbeSource::Options srcOpts;
    srcOpts.sourceMode = sourceMode;
    srcOpts.testWidth  = parser.value(widthOpt).toInt();
    srcOpts.testHeight = parser.value(heightOpt).toInt();
    srcOpts.testFps    = parser.value(fpsOpt).toInt();
    srcOpts.uri        = parser.value(uriOpt);
    srcOpts.rtpProtocol = parser.value(rtpProtocolOpt);
    ProbeSource source(srcOpts);

    QObject::connect(&source, &ProbeSource::newFrame,
                     &widget, &ProbeGLWidget::onNewFrame,
                     Qt::QueuedConnection);
    QObject::connect(&source, &ProbeSource::pipelineError,
                     &app, [](const QString& msg) {
                         qWarning() << "Fatal pipeline error:" << msg;
                         QCoreApplication::quit();
                     });
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     &source, [&source]() {
                         qInfo() << "aboutToQuit -> stopping ProbeSource";
                         source.stop();
                     });

    if (!source.start()) {
        return 2;
    }

    QTimer summaryTimer;
    QObject::connect(&summaryTimer, &QTimer::timeout,
                     &agg, &LatencyAggregator::emitSummaryLine);
    summaryTimer.start(1000);

    // Status line in the window so a human can confirm liveness without
    // tailing stdout. Updated at 2 Hz.
    QTimer statusTimer;
    QObject::connect(&statusTimer, &QTimer::timeout, [&]() {
        widget.setStatusLine(QString(
            "src=%1 vsync=%2 frames=%3 missing-t0=%4 last-end-to-end=%5 ms")
            .arg(sourceLabel)
            .arg(vsync ? "on" : "off")
            .arg(source.totalFramesSeen())
            .arg(source.framesMissingT0Meta())
            .arg(agg.lastEndToEndUs() / 1000.0, 0, 'f', 2));
    });
    statusTimer.start(500);

    if (durationSeconds > 0) {
        QTimer::singleShot(durationSeconds * 1000, &app, [&app, durationSeconds]() {
            qInfo().noquote() << QString(
                "Auto-quit fired after %1 s").arg(durationSeconds);
            app.quit();
        });
    }

    const int rc = app.exec();
    source.stop();
    return rc;
}
