#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QSurfaceFormat>
#include <QTimer>
#include <QString>

#include <gst/gst.h>

#include "RtspSource.hpp"
#include "VideoGLWidget.hpp"

namespace {

constexpr int kHealthPollIntervalMs = 200;   // 5 Hz heartbeat check
constexpr int kStatusOverlayMs      = 500;   // 2 Hz overlay refresh

QString formatHms(qint64 elapsedMs) {
    const qint64 s = elapsedMs / 1000;
    return QString("%1:%2:%3")
        .arg(s / 3600, 2, 10, QChar('0'))
        .arg((s / 60) % 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0'));
}

}  // namespace

int main(int argc, char* argv[]) {
    // P0.1 established that QOpenGLWidget composes correctly under xcb_glx
    // and renders a uniform-black window under xcb_egl on this stack. Default
    // to GLX; ALLOW_EGL=1 left as a diagnostic escape.
    if (qgetenv("ALLOW_EGL") == "1") {
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    } else {
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_glx");
    }

    gst_init(&argc, &argv);

    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setMajorVersion(3);
    fmt.setMinorVersion(3);
    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    fmt.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    qRegisterMetaType<FrameHolder*>("FrameHolder*");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "P0.2 — single RTSP source via nvurisrcbin with reconnect, displayed "
        "via the dGPU GPU-only Qt+CUDA-GL path proven by P0.1.");
    parser.addHelpOption();

    QCommandLineOption uriOpt(QStringList() << "u" << "uri",
        "RTSP URI (e.g. rtsp://127.0.0.1:8554/cam0).", "uri",
        "rtsp://127.0.0.1:8554/cam0");
    QCommandLineOption latencyOpt(QStringList() << "latency",
        "nvurisrcbin jitter buffer in ms.", "ms", "0");
    QCommandLineOption noDropOnLatencyOpt(QStringList() << "no-drop-on-latency",
        "Disable nvurisrcbin's drop-on-latency behavior.");
    QCommandLineOption reconnectIvOpt(QStringList() << "reconnect-interval",
        "Seconds between reconnect health-checks inside nvurisrcbin (0 = disabled).",
        "s", "5");
    QCommandLineOption reconnectAttemptsOpt(QStringList() << "reconnect-attempts",
        "Reconnect attempts inside nvurisrcbin (0 = retry forever).", "n", "0");
    QCommandLineOption rtpProtocolOpt(QStringList() << "rtp",
        "RTP transport: tcp (default) or udp.", "proto", "tcp");
    QCommandLineOption stallTimeoutOpt(QStringList() << "stall-timeout",
        "Heartbeat stall threshold in ms (defines what counts as 'stream down').",
        "ms", "2000");
    QCommandLineOption titleOpt(QStringList() << "title",
        "Window title suffix.", "text", "");

    parser.addOption(uriOpt);
    parser.addOption(latencyOpt);
    parser.addOption(noDropOnLatencyOpt);
    parser.addOption(reconnectIvOpt);
    parser.addOption(reconnectAttemptsOpt);
    parser.addOption(rtpProtocolOpt);
    parser.addOption(stallTimeoutOpt);
    parser.addOption(titleOpt);
    parser.process(app);

    RtspSource::Options opts;
    opts.uri                     = parser.value(uriOpt);
    opts.latencyMs               = parser.value(latencyOpt).toInt();
    opts.dropOnLatency           = !parser.isSet(noDropOnLatencyOpt);
    opts.rtspReconnectIntervalS  = parser.value(reconnectIvOpt).toInt();
    opts.rtspReconnectAttempts   = parser.value(reconnectAttemptsOpt).toInt();
    opts.rtpProtocol             = parser.value(rtpProtocolOpt);
    opts.stallTimeoutMs          = parser.value(stallTimeoutOpt).toInt();

    VideoGLWidget widget;
    widget.resize(1280, 720);
    widget.setWindowTitle(parser.value(titleOpt).isEmpty()
        ? QStringLiteral("02_nvurisrcbin_reconnect — ") + opts.uri
        : QStringLiteral("02_nvurisrcbin_reconnect — ") + parser.value(titleOpt));
    widget.show();

    RtspSource source(opts);
    QObject::connect(&source, &RtspSource::newFrame,
                     &widget, &VideoGLWidget::onNewFrame,
                     Qt::QueuedConnection);
    QObject::connect(&source, &RtspSource::pipelineError,
                     &app, [](const QString& msg) {
                         qWarning() << "Fatal pipeline error:" << msg;
                         QCoreApplication::quit();
                     });

    if (!source.start()) {
        return 2;
    }

    // 5 Hz heartbeat poll: detect live <-> stalled transitions.
    QTimer healthTimer;
    QObject::connect(&healthTimer, &QTimer::timeout,
                     &source, &RtspSource::pollHealth);
    healthTimer.start(kHealthPollIntervalMs);

    // 2 Hz overlay refresh: in-window status line for the human watcher.
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    QTimer statusTimer;
    QObject::connect(&statusTimer, &QTimer::timeout, [&]() {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startMs;
        const qint64 age = source.lastFrameAgeMs();
        const QString state = source.stalledNow() ? "STALL" : "LIVE";
        const QString line = QString(
            "uptime=%1 state=%2 last-frame-age=%3ms reconnects=%4 stalls=%5 frames=%6")
            .arg(formatHms(elapsed))
            .arg(state)
            .arg(age < 0 ? QStringLiteral("--") : QString::number(age))
            .arg(source.reconnectCount())
            .arg(source.stallEventCount())
            .arg(source.totalFramesSeen());
        widget.setStatusLine(line);
    });
    statusTimer.start(kStatusOverlayMs);

    const int rc = app.exec();
    source.stop();
    return rc;
}
