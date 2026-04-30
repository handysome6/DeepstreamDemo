#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QKeyEvent>
#include <QShortcut>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWidget>

#include <gst/gst.h>

#include <algorithm>
#include <vector>

#include "RtspInferSource.hpp"
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
    int              index = 0;
    QString          label;
    QString          uri;
    RtspInferSource* source = nullptr;
    VideoGLWidget*   widget = nullptr;
    QString          lastError;
};

QString panelStatusLine(const PanelState& panel) {
    const qint64 age = panel.source->lastFrameAgeMs();
    QString state = panel.source->stalledNow() ? QStringLiteral("STALL") : QStringLiteral("LIVE");
    if (!panel.lastError.isEmpty()) {
        state += QStringLiteral("/ERR");
    }
    const QString modeTag = panel.source->currentMode() == RtspInferSource::Mode::Infer
        ? QStringLiteral("YOLO") : QStringLiteral("raw ");

    QString line = QString("%1 [%2] %3 age=%4ms rc=%5 st=%6 f=%7")
        .arg(panel.label)
        .arg(modeTag)
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

    int inferCount = 0;
    for (const PanelState& p : panels) {
        if (p.source->currentMode() == RtspInferSource::Mode::Infer) ++inferCount;
    }
    parts << QString("p05 status uptime=%1 panels=%2 infer=%3/%4")
                 .arg(formatHms(elapsedMs))
                 .arg(panels.size())
                 .arg(inferCount)
                 .arg(panels.size());
    for (const PanelState& panel : panels) {
        const qint64 age = panel.source->lastFrameAgeMs();
        QString state = panel.source->stalledNow() ? QStringLiteral("STALL") : QStringLiteral("LIVE");
        if (!panel.lastError.isEmpty()) {
            state += QStringLiteral("/ERR");
        }
        const QString modeTag = panel.source->currentMode() == RtspInferSource::Mode::Infer
            ? QStringLiteral("yolo") : QStringLiteral("raw");
        parts << QString("%1=%2/%3 age=%4ms rc=%5 st=%6 f=%7")
                     .arg(panel.label)
                     .arg(modeTag)
                     .arg(state)
                     .arg(age < 0 ? QStringLiteral("--") : QString::number(age))
                     .arg(panel.source->reconnectCount())
                     .arg(panel.source->stallEventCount())
                     .arg(panel.source->totalFramesSeen());
    }
    return parts.join(QStringLiteral(" | "));
}

QString resolveInferConfig(const QString& cliValue) {
    if (cliValue.isEmpty()) return {};
    QFileInfo fi(cliValue);
    if (fi.isAbsolute()) return cliValue;
    // Resolve relative paths against the binary's CWD first, then against the
    // component root. Inside the run_in_container.sh harness, CWD is
    // /workspace, so the configs/ directory lives at configs/<file>.
    if (fi.exists()) return fi.absoluteFilePath();
    const QString fallback = QDir::current().filePath(cliValue);
    if (QFileInfo::exists(fallback)) return fallback;
    return cliValue;
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
        "P0.5 — selective YOLO inference per stream over the multi-widget GPU-only "
        "contract proven by P0.1/P0.2/P0.4. Each panel can independently flip between "
        "raw rendering and inference; the inference path adds nvstreammux -> nvinfer "
        "-> nvdsosd to the same NVMM RGBA appsink contract.");
    parser.addHelpOption();

    QCommandLineOption uriOpt(QStringList() << "u" << "uri",
        "RTSP URI. Repeat this option once per panel.", "uri");
    QCommandLineOption inferOpt(QStringList() << "i" << "infer",
        "1-based panel index that should start with inference enabled. Repeatable.",
        "panel");
    QCommandLineOption inferAllOpt(QStringList() << "infer-all",
        "Start every panel in infer mode.");
    QCommandLineOption inferConfigOpt(QStringList() << "infer-config",
        "Path to the nvinfer config file. Required when any panel runs infer mode.",
        "path", "configs/config_infer_primary_trafficcamnet.txt");
    QCommandLineOption muxWHOpt(QStringList() << "mux-size",
        "nvstreammux output WxH (defaults to 1920x1080).", "WxH", "1920x1080");
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
    QCommandLineOption autoToggleOpt(QStringList() << "auto-toggle-seconds",
        "If set and >0, every N seconds flip every panel's mode. Used to exercise "
        "the selective-inference state machine without an interactive keyboard.",
        "n", "0");
    QCommandLineOption quitAfterOpt(QStringList() << "quit-after-seconds",
        "If set and >0, exit cleanly after N seconds. Used by soak harnesses to "
        "guarantee a graceful teardown for memory / FD audits.",
        "n", "0");

    parser.addOption(uriOpt);
    parser.addOption(inferOpt);
    parser.addOption(inferAllOpt);
    parser.addOption(inferConfigOpt);
    parser.addOption(muxWHOpt);
    parser.addOption(latencyOpt);
    parser.addOption(noDropOnLatencyOpt);
    parser.addOption(reconnectIvOpt);
    parser.addOption(reconnectAttemptsOpt);
    parser.addOption(rtpProtocolOpt);
    parser.addOption(stallTimeoutOpt);
    parser.addOption(colsOpt);
    parser.addOption(titleOpt);
    parser.addOption(autoToggleOpt);
    parser.addOption(quitAfterOpt);
    parser.process(app);

    QStringList uris = parser.values(uriOpt);
    if (uris.isEmpty()) {
        const QString uri = defaultUri();
        uris << uri << uri;
    }

    // Parse the panel-index list that should start in infer mode.
    std::vector<bool> startInfer(uris.size(), false);
    if (parser.isSet(inferAllOpt)) {
        std::fill(startInfer.begin(), startInfer.end(), true);
    } else {
        for (const QString& s : parser.values(inferOpt)) {
            bool ok = false;
            const int idx = s.toInt(&ok);
            if (ok && idx >= 1 && idx <= int(uris.size())) {
                startInfer[idx - 1] = true;
            } else {
                qWarning().noquote() << "Ignoring out-of-range --infer panel index:" << s;
            }
        }
    }

    const QString resolvedConfig = resolveInferConfig(parser.value(inferConfigOpt));
    const bool anyInfer = std::any_of(startInfer.begin(), startInfer.end(),
                                      [](bool v) { return v; });
    if (anyInfer && !QFileInfo::exists(resolvedConfig)) {
        qWarning().noquote() << QString(
            "Inference requested but config not found at '%1'. "
            "Either pass --infer-config <path> or run scripts/fetch_models.sh.")
            .arg(resolvedConfig);
        return 3;
    }

    int muxW = 1920, muxH = 1080;
    {
        const QStringList wh = parser.value(muxWHOpt).split('x');
        if (wh.size() == 2) {
            const int w = wh[0].toInt();
            const int h = wh[1].toInt();
            if (w > 0 && h > 0) { muxW = w; muxH = h; }
        }
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
        ? QStringLiteral("05_selective_yolo_infer — %1 streams").arg(uris.size())
        : explicitTitle);
    window.resize(cols * 640, rows * 360);

    std::vector<PanelState> panels;
    panels.reserve(uris.size());

    for (int i = 0; i < uris.size(); ++i) {
        RtspInferSource::Options opts;
        opts.uri                    = uris.at(i);
        opts.mode                   = startInfer[i] ? RtspInferSource::Mode::Infer
                                                    : RtspInferSource::Mode::Raw;
        opts.inferConfigPath        = resolvedConfig;
        opts.latencyMs              = parser.value(latencyOpt).toInt();
        opts.dropOnLatency          = !parser.isSet(noDropOnLatencyOpt);
        opts.rtspReconnectIntervalS = parser.value(reconnectIvOpt).toInt();
        opts.rtspReconnectAttempts  = parser.value(reconnectAttemptsOpt).toInt();
        opts.rtpProtocol            = parser.value(rtpProtocolOpt);
        opts.stallTimeoutMs         = parser.value(stallTimeoutOpt).toInt();
        opts.muxWidth               = muxW;
        opts.muxHeight              = muxH;

        auto* widget = new VideoGLWidget(&window);
        widget->setMinimumSize(320, 180);
        auto* source = new RtspInferSource(opts, &window);

        PanelState panel;
        panel.index = i;
        panel.label = QString("s%1").arg(i + 1);
        panel.uri = opts.uri;
        panel.source = source;
        panel.widget = widget;
        panels.push_back(panel);
        PanelState* panelPtr = &panels.back();

        QObject::connect(source, &RtspInferSource::newFrame,
                         widget, &VideoGLWidget::onNewFrame,
                         Qt::QueuedConnection);
        QObject::connect(source, &RtspInferSource::streamLive,
                         &app, [panelPtr]() {
                             qInfo().noquote() << QString("%1 LIVE (%2)")
                                 .arg(panelPtr->label, panelPtr->uri);
                         });
        QObject::connect(source, &RtspInferSource::streamStalled,
                         &app, [panelPtr](qint64 ageMs) {
                             qWarning().noquote() << QString("%1 STALLED age=%2ms (%3)")
                                 .arg(panelPtr->label)
                                 .arg(ageMs)
                                 .arg(panelPtr->uri);
                         });
        QObject::connect(source, &RtspInferSource::pipelineError,
                         &app, [panelPtr](const QString& msg) {
                             panelPtr->lastError = msg;
                             qWarning().noquote() << QString("%1 pipeline error: %2")
                                 .arg(panelPtr->label, msg);
                         });
        QObject::connect(source, &RtspInferSource::modeChanged,
                         &app, [panelPtr](int mode) {
                             panelPtr->lastError.clear();
                             qInfo().noquote() << QString("%1 mode -> %2")
                                 .arg(panelPtr->label,
                                      mode == int(RtspInferSource::Mode::Infer)
                                          ? "YOLO" : "raw");
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

    // Per-panel toggle shortcuts: keys 1..9 flip the corresponding panel's
    // mode. Keep this off the file scope so it can capture `panels`.
    for (int i = 0; i < int(panels.size()) && i < 9; ++i) {
        const QString seq = QString::number(i + 1);
        auto* sc = new QShortcut(QKeySequence(seq), &window);
        const int idx = i;
        QObject::connect(sc, &QShortcut::activated, &window, [&, idx]() {
            if (idx < 0 || idx >= int(panels.size())) return;
            PanelState& panel = panels[idx];
            const auto cur = panel.source->currentMode();
            const auto next = (cur == RtspInferSource::Mode::Infer)
                ? RtspInferSource::Mode::Raw : RtspInferSource::Mode::Infer;
            if (!panel.source->setMode(next)) {
                qWarning().noquote() << QString("%1 mode toggle failed").arg(panel.label);
            }
        });
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

    const int autoToggleS = parser.value(autoToggleOpt).toInt();
    QTimer autoToggleTimer;
    if (autoToggleS > 0) {
        QObject::connect(&autoToggleTimer, &QTimer::timeout, [&]() {
            for (PanelState& panel : panels) {
                const auto cur = panel.source->currentMode();
                const auto next = (cur == RtspInferSource::Mode::Infer)
                    ? RtspInferSource::Mode::Raw : RtspInferSource::Mode::Infer;
                if (!panel.source->setMode(next)) {
                    qWarning().noquote() << QString("%1 auto-toggle failed").arg(panel.label);
                }
            }
        });
        autoToggleTimer.start(autoToggleS * 1000);
        qInfo().noquote() << QString("Auto-toggle enabled: every %1 s every panel flips mode")
            .arg(autoToggleS);
    }

    const int quitAfterS = parser.value(quitAfterOpt).toInt();
    QTimer quitTimer;
    if (quitAfterS > 0) {
        quitTimer.setSingleShot(true);
        QObject::connect(&quitTimer, &QTimer::timeout, &app, [quitAfterS]() {
            qInfo().noquote() << QString("--quit-after-seconds=%1 fired; exiting cleanly")
                .arg(quitAfterS);
            QCoreApplication::quit();
        });
        quitTimer.start(quitAfterS * 1000);
    }

    const int rc = app.exec();
    for (PanelState& panel : panels) {
        panel.source->stop();
    }
    return rc;
}
