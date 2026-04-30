#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QGridLayout>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWidget>

#include <gst/gst.h>

#include <algorithm>
#include <vector>

#include "RtspSource.hpp"
#include "VideoGLWidget.hpp"

namespace {

constexpr int kHealthPollIntervalMs = 200;
constexpr int kStatusOverlayMs      = 500;
constexpr int kSummaryLogMs         = 1000;

QString formatHms(qint64 elapsedMs) {
    const qint64 s = elapsedMs / 1000;
    return QString("%1:%2:%3")
        .arg(s / 3600, 2, 10, QChar('0'))
        .arg((s / 60) % 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0'));
}

QString defaultUri() {
    const QByteArray env = qgetenv("RTSP_URL");
    return env.isEmpty()
        ? QStringLiteral("rtsp://127.0.0.1:8554/p02cam")
        : QString::fromLocal8Bit(env);
}

struct PanelState {
    int            index = 0;
    QString        label;
    QString        uri;
    RtspSource*    source = nullptr;
    VideoGLWidget* widget = nullptr;
    QString        lastError;
};

QString panelStatusLine(const PanelState& panel) {
    const qint64 age = panel.source->lastFrameAgeMs();
    QString state = panel.source->stalledNow() ? QStringLiteral("STALL") : QStringLiteral("LIVE");
    if (!panel.lastError.isEmpty()) {
        state += QStringLiteral("/ERR");
    }

    QString line = QString("%1 %2 age=%3ms rc=%4 st=%5 f=%6")
        .arg(panel.label)
        .arg(state)
        .arg(age < 0 ? QStringLiteral("--") : QString::number(age))
        .arg(panel.source->reconnectCount())
        .arg(panel.source->stallEventCount())
        .arg(panel.source->totalFramesSeen());

    if (!panel.lastError.isEmpty()) {
        line += QString(" err=%1").arg(panel.lastError);
    }
    return line;
}

QString aggregateStatusLine(const std::vector<PanelState>& panels, qint64 elapsedMs) {
    QStringList parts;
    parts.reserve(int(panels.size()) + 1);
    parts << QString("multi status uptime=%1 panels=%2")
                 .arg(formatHms(elapsedMs))
                 .arg(panels.size());
    for (const PanelState& panel : panels) {
        const qint64 age = panel.source->lastFrameAgeMs();
        QString state = panel.source->stalledNow() ? QStringLiteral("STALL") : QStringLiteral("LIVE");
        if (!panel.lastError.isEmpty()) {
            state += QStringLiteral("/ERR");
        }
        parts << QString("%1=%2 age=%3ms rc=%4 st=%5 f=%6")
                     .arg(panel.label)
                     .arg(state)
                     .arg(age < 0 ? QStringLiteral("--") : QString::number(age))
                     .arg(panel.source->reconnectCount())
                     .arg(panel.source->stallEventCount())
                     .arg(panel.source->totalFramesSeen());
    }
    return parts.join(QStringLiteral(" | "));
}

}  // namespace

int main(int argc, char* argv[]) {
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
        "P0.4 — multiple independent RTSP sources displayed in separate "
        "QOpenGLWidgets over the same dGPU GPU-only contract proven by P0.1/P0.2.");
    parser.addHelpOption();

    QCommandLineOption uriOpt(QStringList() << "u" << "uri",
        "RTSP URI. Repeat this option once per panel.", "uri");
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
        "Heartbeat stall threshold in ms.", "ms", "2000");
    QCommandLineOption colsOpt(QStringList() << "cols",
        "Grid column count.", "n", "2");
    QCommandLineOption titleOpt(QStringList() << "title",
        "Top-level window title.", "text", "");

    parser.addOption(uriOpt);
    parser.addOption(latencyOpt);
    parser.addOption(noDropOnLatencyOpt);
    parser.addOption(reconnectIvOpt);
    parser.addOption(reconnectAttemptsOpt);
    parser.addOption(rtpProtocolOpt);
    parser.addOption(stallTimeoutOpt);
    parser.addOption(colsOpt);
    parser.addOption(titleOpt);
    parser.process(app);

    QStringList uris = parser.values(uriOpt);
    if (uris.isEmpty()) {
        const QString uri = defaultUri();
        uris << uri << uri;
    }

    const int cols = std::max(1, parser.value(colsOpt).toInt());
    const int rows = (uris.size() + cols - 1) / cols;

    QWidget window;
    auto* layout = new QGridLayout(&window);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setHorizontalSpacing(8);
    layout->setVerticalSpacing(8);

    const QString explicitTitle = parser.value(titleOpt);
    window.setWindowTitle(explicitTitle.isEmpty()
        ? QStringLiteral("04_multi_rtsp_widgets — %1 streams").arg(uris.size())
        : explicitTitle);
    window.resize(cols * 640, rows * 360);

    std::vector<PanelState> panels;
    panels.reserve(uris.size());

    for (int i = 0; i < uris.size(); ++i) {
        RtspSource::Options opts;
        opts.uri                    = uris.at(i);
        opts.latencyMs              = parser.value(latencyOpt).toInt();
        opts.dropOnLatency          = !parser.isSet(noDropOnLatencyOpt);
        opts.rtspReconnectIntervalS = parser.value(reconnectIvOpt).toInt();
        opts.rtspReconnectAttempts  = parser.value(reconnectAttemptsOpt).toInt();
        opts.rtpProtocol            = parser.value(rtpProtocolOpt);
        opts.stallTimeoutMs         = parser.value(stallTimeoutOpt).toInt();

        auto* widget = new VideoGLWidget(&window);
        widget->setMinimumSize(320, 180);
        auto* source = new RtspSource(opts, &window);

        PanelState panel;
        panel.index = i;
        panel.label = QString("s%1").arg(i + 1);
        panel.uri = opts.uri;
        panel.source = source;
        panel.widget = widget;
        panels.push_back(panel);
        PanelState* panelPtr = &panels.back();

        QObject::connect(source, &RtspSource::newFrame,
                         widget, &VideoGLWidget::onNewFrame,
                         Qt::QueuedConnection);
        QObject::connect(source, &RtspSource::streamLive,
                         &app, [panelPtr]() {
                             qInfo().noquote() << QString("%1 LIVE (%2)")
                                 .arg(panelPtr->label, panelPtr->uri);
                         });
        QObject::connect(source, &RtspSource::streamStalled,
                         &app, [panelPtr](qint64 ageMs) {
                             qWarning().noquote() << QString("%1 STALLED age=%2ms (%3)")
                                 .arg(panelPtr->label)
                                 .arg(ageMs)
                                 .arg(panelPtr->uri);
                         });
        QObject::connect(source, &RtspSource::pipelineError,
                         &app, [panelPtr](const QString& msg) {
                             panelPtr->lastError = msg;
                             qWarning().noquote() << QString("%1 pipeline error: %2")
                                 .arg(panelPtr->label, msg);
                         });

        layout->addWidget(widget, i / cols, i % cols);
    }

    window.show();

    bool allStarted = true;
    for (PanelState& panel : panels) {
        if (!panel.source->start()) {
            qWarning().noquote() << QString("Failed to start %1 (%2)")
                .arg(panel.label, panel.uri);
            panel.lastError = QStringLiteral("start failed");
            allStarted = false;
            break;
        }
    }
    if (!allStarted) {
        for (PanelState& panel : panels) {
            panel.source->stop();
        }
        return 2;
    }

    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();

    QTimer healthTimer;
    QObject::connect(&healthTimer, &QTimer::timeout, [&]() {
        for (PanelState& panel : panels) {
            panel.source->pollHealth();
        }
    });
    healthTimer.start(kHealthPollIntervalMs);

    QTimer statusTimer;
    QObject::connect(&statusTimer, &QTimer::timeout, [&]() {
        for (PanelState& panel : panels) {
            panel.widget->setStatusLine(panelStatusLine(panel));
            panel.widget->update();
        }
    });
    statusTimer.start(kStatusOverlayMs);

    QTimer summaryTimer;
    QObject::connect(&summaryTimer, &QTimer::timeout, [&]() {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startMs;
        qInfo().noquote() << aggregateStatusLine(panels, elapsed);
    });
    summaryTimer.start(kSummaryLogMs);

    const int rc = app.exec();
    for (PanelState& panel : panels) {
        panel.source->stop();
    }
    return rc;
}
