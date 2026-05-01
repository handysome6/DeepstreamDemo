// P0.7 — full-pressure integration.
//
// Single Qt application that runs three independent subsystems concurrently
// against the local RTSP lab:
//
//   * 8 × 1080p raw widgets (RtspSource → VideoGLWidget), the 04 path
//   * 1 × 4K YOLO widget    (RtspInferSource → VideoGLWidget), the 05 path
//                           with mux output bumped to 3840x2160 so the
//                           displayed frame is full-res; nvinfer's
//                           network-input-shape (640x640) handles the
//                           inference downsample internally.
//   * 1 × 4K stitch widget  (DualRtspSourcePair → StitchGLWidget), the 06
//                           path with the restream branch removed.
//
// Goal: expose the real bottleneck under combined decode / VRAM / CUDA /
// Qt-paint contention. Not a new algorithm, not a new pipeline shape — the
// composition itself is the proof.

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWidget>

#include <gst/gst.h>

#include <algorithm>
#include <vector>

#include "DualRtspSourcePair.hpp"
#include "FrameHolder.hpp"
#include "RtspInferSource.hpp"
#include "RtspSource.hpp"
#include "StitchFrame.hpp"
#include "StitchGLWidget.hpp"
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

QString resolveInferConfig(const QString& cliValue) {
    if (cliValue.isEmpty()) return {};
    QFileInfo fi(cliValue);
    if (fi.isAbsolute()) return cliValue;
    if (fi.exists()) return fi.absoluteFilePath();
    const QString fallback = QDir::current().filePath(cliValue);
    if (QFileInfo::exists(fallback)) return fallback;
    return cliValue;
}

bool loadTransform(const QJsonObject& obj, const char* key, StitchTransform* out, QString* error) {
    if (!obj.contains(key)) return true;
    const QJsonValue value = obj.value(key);
    if (!value.isArray()) {
        if (error) *error = QString("%1 must be an array of 9 numbers").arg(QString::fromLatin1(key));
        return false;
    }
    const QJsonArray arr = value.toArray();
    if (arr.size() != 9) {
        if (error) *error = QString("%1 must contain exactly 9 numbers").arg(QString::fromLatin1(key));
        return false;
    }
    for (int i = 0; i < 9; ++i) {
        out->m[i] = float(arr.at(i).toDouble((i % 4 == 0) ? 1.0 : 0.0));
    }
    return true;
}

bool loadCalibration(const QString& path, StitchRenderer::Calibration* out, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QString("failed to open calibration file: %1").arg(path);
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QString("invalid calibration json: %1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject obj = doc.object();
    out->mode = obj.value("mode").toString("identity");
    out->outputWidth = obj.value("outputWidth").toInt(3840);
    out->outputHeight = obj.value("outputHeight").toInt(4320);
    out->overlapPx = obj.value("overlapPx").toInt(0);
    out->enableBlend = obj.value("enableBlend").toBool(false);
    if (!loadTransform(obj, "topSourceFromOutput", &out->topSourceFromOutput, error)) return false;
    if (!loadTransform(obj, "bottomSourceFromOutput", &out->bottomSourceFromOutput, error)) return false;
    return true;
}

struct RawPanel {
    int             index = 0;
    QString         label;
    QString         uri;
    RtspSource*     source = nullptr;
    VideoGLWidget*  widget = nullptr;
    QString         lastError;
};

struct YoloPanel {
    QString          label;
    QString          uri;
    RtspInferSource* source = nullptr;
    VideoGLWidget*   widget = nullptr;
    QString          lastError;
};

struct StitchPanel {
    QString             label;
    QString             topUri;
    QString             bottomUri;
    DualRtspSourcePair* pair   = nullptr;
    StitchGLWidget*     widget = nullptr;
    QString             lastError;
};

QString rawPanelLine(const RawPanel& p) {
    const qint64 age = p.source->lastFrameAgeMs();
    const QString state = p.source->stalledNow() ? "STALL" : "LIVE";
    return QString("%1 [raw ] %2 age=%3ms rc=%4 st=%5 f=%6")
        .arg(p.label)
        .arg(state)
        .arg(age < 0 ? QStringLiteral("--") : QString::number(age))
        .arg(p.source->reconnectCount())
        .arg(p.source->stallEventCount())
        .arg(p.source->totalFramesSeen());
}

QString yoloPanelLine(const YoloPanel& p) {
    const qint64 age = p.source->lastFrameAgeMs();
    const QString state = p.source->stalledNow() ? "STALL" : "LIVE";
    return QString("%1 [YOLO] %2 age=%3ms rc=%4 st=%5 f=%6")
        .arg(p.label)
        .arg(state)
        .arg(age < 0 ? QStringLiteral("--") : QString::number(age))
        .arg(p.source->reconnectCount())
        .arg(p.source->stallEventCount())
        .arg(p.source->totalFramesSeen());
}

QString stitchPanelLine(const StitchPanel& p) {
    const qint64 a = p.pair->topLastFrameAgeMs();
    const qint64 b = p.pair->bottomLastFrameAgeMs();
    return QString("%1 [STCH] a=%2/%3ms b=%4/%5ms delta=%6ms stitched=%7 %8")
        .arg(p.label)
        .arg(p.pair->topStalledNow() ? "STALL" : "LIVE")
        .arg(a < 0 ? QStringLiteral("--") : QString::number(a))
        .arg(p.pair->bottomStalledNow() ? "STALL" : "LIVE")
        .arg(b < 0 ? QStringLiteral("--") : QString::number(b))
        .arg(p.pair->lastPairDeltaMs() < 0 ? QStringLiteral("--") : QString::number(p.pair->lastPairDeltaMs()))
        .arg(p.pair->stitchedFrameCount())
        .arg(p.pair->degradedNow() ? "DEGRADED" : "OK");
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
    qRegisterMetaType<StitchedFrame*>("StitchedFrame*");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "P0.7 — full-pressure integration of 04/05/06: 8x 1080p raw + 1x 4K YOLO "
        "+ 2x 4K stitch in one Qt application.");
    parser.addHelpOption();

    QCommandLineOption uri1080pOpt(QStringList() << "uri-1080p",
        "1080p RTSP URI for a raw widget. Repeat per widget (up to 8).", "uri");
    QCommandLineOption uri4kYoloOpt(QStringList() << "uri-4k-yolo",
        "4K RTSP URI for the YOLO panel.", "uri");
    QCommandLineOption uri4kTopOpt(QStringList() << "uri-4k-stitch-top",
        "4K RTSP URI for the stitch top input.", "uri");
    QCommandLineOption uri4kBottomOpt(QStringList() << "uri-4k-stitch-bottom",
        "4K RTSP URI for the stitch bottom input.", "uri");
    QCommandLineOption stageOpt(QStringList() << "stage",
        "Which subsystems to enable: '1080p_only' | 'plus_yolo' | 'full' (default).",
        "stage", "full");
    QCommandLineOption inferConfigOpt(QStringList() << "infer-config",
        "Path to nvinfer config for the YOLO panel.",
        "path", "configs/config_infer_primary_yolov10.txt");
    QCommandLineOption calibrationOpt(QStringList() << "calibration",
        "Path to stitch calibration JSON.",
        "path", "configs/pair_calibration_example.json");
    QCommandLineOption muxWHOpt(QStringList() << "mux-size",
        "nvstreammux output WxH for the YOLO panel. Defaults to 3840x2160 so "
        "the appsink frame is delivered at the source's full 4K — nvinfer "
        "downsamples internally for the network input.",
        "WxH", "3840x2160");
    QCommandLineOption colsOpt(QStringList() << "cols",
        "Grid column count.", "n", "4");
    QCommandLineOption titleOpt(QStringList() << "title",
        "Top-level window title.", "text", "");
    QCommandLineOption latencyOpt(QStringList() << "latency",
        "nvurisrcbin jitter buffer in ms.", "ms", "0");
    QCommandLineOption noDropOnLatencyOpt(QStringList() << "no-drop-on-latency",
        "Disable nvurisrcbin's drop-on-latency behavior.");
    QCommandLineOption reconnectIvOpt(QStringList() << "reconnect-interval",
        "Seconds between reconnect attempts inside nvurisrcbin.", "s", "5");
    QCommandLineOption reconnectAttemptsOpt(QStringList() << "reconnect-attempts",
        "Reconnect attempts (0 = retry forever).", "n", "0");
    QCommandLineOption rtpProtocolOpt(QStringList() << "rtp",
        "RTP transport: tcp (default) or udp.", "proto", "tcp");
    QCommandLineOption stallTimeoutOpt(QStringList() << "stall-timeout",
        "Heartbeat stall threshold in ms.", "ms", "2000");
    QCommandLineOption maxPairDeltaOpt(QStringList() << "max-pair-delta",
        "Max pair delta ms before stitch pair is marked DEGRADED.", "ms", "250");
    QCommandLineOption quitAfterOpt(QStringList() << "quit-after-seconds",
        "Exit cleanly after N seconds.", "n", "0");

    parser.addOption(uri1080pOpt);
    parser.addOption(uri4kYoloOpt);
    parser.addOption(uri4kTopOpt);
    parser.addOption(uri4kBottomOpt);
    parser.addOption(stageOpt);
    parser.addOption(inferConfigOpt);
    parser.addOption(calibrationOpt);
    parser.addOption(muxWHOpt);
    parser.addOption(colsOpt);
    parser.addOption(titleOpt);
    parser.addOption(latencyOpt);
    parser.addOption(noDropOnLatencyOpt);
    parser.addOption(reconnectIvOpt);
    parser.addOption(reconnectAttemptsOpt);
    parser.addOption(rtpProtocolOpt);
    parser.addOption(stallTimeoutOpt);
    parser.addOption(maxPairDeltaOpt);
    parser.addOption(quitAfterOpt);
    parser.process(app);

    const QString stage = parser.value(stageOpt);
    const bool wantYolo   = (stage == "plus_yolo" || stage == "full");
    const bool wantStitch = (stage == "full");

    QStringList raw1080pUris = parser.values(uri1080pOpt);
    if (raw1080pUris.size() > 8) {
        qWarning().noquote() << QString(
            "More than 8 --uri-1080p values provided (%1); truncating to 8.")
            .arg(raw1080pUris.size());
        raw1080pUris = raw1080pUris.mid(0, 8);
    }

    int muxW = 3840, muxH = 2160;
    {
        const QStringList wh = parser.value(muxWHOpt).split('x');
        if (wh.size() == 2) {
            const int w = wh[0].toInt();
            const int h = wh[1].toInt();
            if (w > 0 && h > 0) { muxW = w; muxH = h; }
        }
    }

    const QString resolvedInferCfg = resolveInferConfig(parser.value(inferConfigOpt));
    if (wantYolo && !QFileInfo::exists(resolvedInferCfg)) {
        qWarning().noquote() << QString(
            "YOLO panel requested but infer config not found at '%1'. "
            "Either pass --infer-config <path> or run scripts/fetch_models.sh.")
            .arg(resolvedInferCfg);
        return 3;
    }

    StitchRenderer::Calibration calibration;
    if (wantStitch) {
        QString calErr;
        if (!loadCalibration(parser.value(calibrationOpt), &calibration, &calErr)) {
            qWarning().noquote() << calErr;
            return 4;
        }
    }

    const int cols = std::max(1, parser.value(colsOpt).toInt());

    QWidget window;
    auto* layout = new QGridLayout(&window);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setHorizontalSpacing(8);
    layout->setVerticalSpacing(8);

    const QString title = parser.value(titleOpt);
    window.setWindowTitle(title.isEmpty()
        ? QStringLiteral("07_full_pressure_integration — stage=%1").arg(stage)
        : title);

    // ---- Build raw 1080p panels --------------------------------------------
    std::vector<RawPanel> rawPanels;
    rawPanels.reserve(raw1080pUris.size());
    for (int i = 0; i < raw1080pUris.size(); ++i) {
        RtspSource::Options opts;
        opts.uri                    = raw1080pUris.at(i);
        opts.latencyMs              = parser.value(latencyOpt).toInt();
        opts.dropOnLatency          = !parser.isSet(noDropOnLatencyOpt);
        opts.rtspReconnectIntervalS = parser.value(reconnectIvOpt).toInt();
        opts.rtspReconnectAttempts  = parser.value(reconnectAttemptsOpt).toInt();
        opts.rtpProtocol            = parser.value(rtpProtocolOpt);
        opts.stallTimeoutMs         = parser.value(stallTimeoutOpt).toInt();

        auto* widget = new VideoGLWidget(&window);
        widget->setMinimumSize(320, 180);
        auto* source = new RtspSource(opts, &window);

        RawPanel panel;
        panel.index  = i;
        panel.label  = QString("r%1").arg(i + 1);
        panel.uri    = opts.uri;
        panel.source = source;
        panel.widget = widget;
        rawPanels.push_back(panel);
    }
    // Hand out stable pointers AFTER the vector is fully populated.
    for (RawPanel& panel : rawPanels) {
        RawPanel* pp = &panel;
        QObject::connect(pp->source, &RtspSource::newFrame,
                         pp->widget, &VideoGLWidget::onNewFrame,
                         Qt::QueuedConnection);
        QObject::connect(pp->source, &RtspSource::streamLive,
                         &app, [pp]() {
                             qInfo().noquote() << QString("%1 LIVE (%2)").arg(pp->label, pp->uri);
                         });
        QObject::connect(pp->source, &RtspSource::streamStalled,
                         &app, [pp](qint64 ageMs) {
                             qWarning().noquote() << QString("%1 STALLED age=%2ms (%3)")
                                 .arg(pp->label).arg(ageMs).arg(pp->uri);
                         });
        QObject::connect(pp->source, &RtspSource::pipelineError,
                         &app, [pp](const QString& msg) {
                             pp->lastError = msg;
                             qWarning().noquote() << QString("%1 pipeline error: %2")
                                 .arg(pp->label, msg);
                         });
    }

    // ---- Build YOLO panel --------------------------------------------------
    YoloPanel yoloPanel;
    if (wantYolo) {
        const QString yoloUri = parser.isSet(uri4kYoloOpt)
            ? parser.value(uri4kYoloOpt)
            : (raw1080pUris.isEmpty() ? QStringLiteral("rtsp://127.0.0.1:8554/cam4")
                                       : raw1080pUris.first());

        RtspInferSource::Options opts;
        opts.uri                    = yoloUri;
        opts.inferConfigPath        = resolvedInferCfg;
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

        yoloPanel.label  = QStringLiteral("y1");
        yoloPanel.uri    = yoloUri;
        yoloPanel.source = source;
        yoloPanel.widget = widget;

        YoloPanel* yp = &yoloPanel;
        QObject::connect(source, &RtspInferSource::newFrame,
                         widget, &VideoGLWidget::onNewFrame,
                         Qt::QueuedConnection);
        QObject::connect(source, &RtspInferSource::streamLive,
                         &app, [yp]() {
                             qInfo().noquote() << QString("%1 LIVE (%2)").arg(yp->label, yp->uri);
                         });
        QObject::connect(source, &RtspInferSource::streamStalled,
                         &app, [yp](qint64 ageMs) {
                             qWarning().noquote() << QString("%1 STALLED age=%2ms (%3)")
                                 .arg(yp->label).arg(ageMs).arg(yp->uri);
                         });
        QObject::connect(source, &RtspInferSource::pipelineError,
                         &app, [yp](const QString& msg) {
                             yp->lastError = msg;
                             qWarning().noquote() << QString("%1 pipeline error: %2")
                                 .arg(yp->label, msg);
                         });
    }

    // ---- Build stitch panel ------------------------------------------------
    StitchPanel stitchPanel;
    if (wantStitch) {
        const QString topUri = parser.isSet(uri4kTopOpt)
            ? parser.value(uri4kTopOpt) : QStringLiteral("rtsp://127.0.0.1:8554/cam4");
        const QString bottomUri = parser.isSet(uri4kBottomOpt)
            ? parser.value(uri4kBottomOpt) : QStringLiteral("rtsp://127.0.0.1:8554/cam5");

        RtspSource::Options topOpts;
        topOpts.uri                    = topUri;
        topOpts.latencyMs              = parser.value(latencyOpt).toInt();
        topOpts.dropOnLatency          = !parser.isSet(noDropOnLatencyOpt);
        topOpts.rtspReconnectIntervalS = parser.value(reconnectIvOpt).toInt();
        topOpts.rtspReconnectAttempts  = parser.value(reconnectAttemptsOpt).toInt();
        topOpts.rtpProtocol            = parser.value(rtpProtocolOpt);
        topOpts.stallTimeoutMs         = parser.value(stallTimeoutOpt).toInt();

        RtspSource::Options bottomOpts = topOpts;
        bottomOpts.uri                 = bottomUri;

        DualRtspSourcePair::Options pairOpts;
        pairOpts.top            = topOpts;
        pairOpts.bottom         = bottomOpts;
        pairOpts.calibration    = calibration;
        pairOpts.maxPairDeltaMs = parser.value(maxPairDeltaOpt).toLongLong();

        auto* widget = new StitchGLWidget(&window);
        widget->setMinimumSize(360, 360);
        auto* pair = new DualRtspSourcePair(pairOpts, &window);

        stitchPanel.label     = QStringLiteral("s1");
        stitchPanel.topUri    = topUri;
        stitchPanel.bottomUri = bottomUri;
        stitchPanel.pair      = pair;
        stitchPanel.widget    = widget;

        StitchPanel* sp = &stitchPanel;
        QObject::connect(pair, &DualRtspSourcePair::newStitchedFrame,
                         &app, [sp](StitchedFrame* frame) {
                             // 06 fans out one stitched frame to preview +
                             // restream. P0.7 has no restream branch, so the
                             // widget is the single consumer — hand the
                             // stitched frame straight to it.
                             sp->widget->onNewFrame(frame);
                         },
                         Qt::DirectConnection);
        QObject::connect(pair, &DualRtspSourcePair::pipelineError,
                         &app, [sp](const QString& msg) {
                             sp->lastError = msg;
                             qWarning().noquote() << QString("%1 pipeline error: %2")
                                 .arg(sp->label, msg);
                         });
        QObject::connect(pair, &DualRtspSourcePair::pairDegraded,
                         &app, [sp](const QString& reason) {
                             qWarning().noquote() << QString("%1 pair DEGRADED: %2")
                                 .arg(sp->label, reason);
                         });
        QObject::connect(pair, &DualRtspSourcePair::pairRecovered,
                         &app, [sp]() {
                             qInfo().noquote() << QString("%1 pair RECOVERED").arg(sp->label);
                         });
    }

    // ---- Lay out widgets in the grid ---------------------------------------
    int slot = 0;
    for (RawPanel& panel : rawPanels) {
        layout->addWidget(panel.widget, slot / cols, slot % cols);
        ++slot;
    }
    if (yoloPanel.widget) {
        layout->addWidget(yoloPanel.widget, slot / cols, slot % cols);
        ++slot;
    }
    if (stitchPanel.widget) {
        // The stitch panel is a 2× tall composition (top-bottom). Span two
        // grid rows so it doesn't get squeezed.
        layout->addWidget(stitchPanel.widget, slot / cols, slot % cols, 2, 1);
        ++slot;
    }

    const int totalSlots = slot;
    const int rows = (totalSlots + cols - 1) / cols;
    window.resize(cols * 480, std::max(1, rows) * 320);
    window.show();

    // ---- Start sources -----------------------------------------------------
    bool startOk = true;
    for (RawPanel& panel : rawPanels) {
        if (!panel.source->start()) {
            qWarning().noquote() << QString("Failed to start %1 (%2)")
                .arg(panel.label, panel.uri);
            panel.lastError = QStringLiteral("start failed");
            startOk = false;
            break;
        }
    }
    if (startOk && yoloPanel.source) {
        if (!yoloPanel.source->start()) {
            qWarning().noquote() << QString("Failed to start %1 (%2)")
                .arg(yoloPanel.label, yoloPanel.uri);
            yoloPanel.lastError = QStringLiteral("start failed");
            startOk = false;
        }
    }
    if (startOk && stitchPanel.pair) {
        if (!stitchPanel.pair->start()) {
            qWarning().noquote() << QString("Failed to start stitch pair: %1")
                .arg(stitchPanel.pair->lastError());
            stitchPanel.lastError = stitchPanel.pair->lastError();
            startOk = false;
        }
    }

    if (!startOk) {
        for (RawPanel& panel : rawPanels) panel.source->stop();
        if (yoloPanel.source)   yoloPanel.source->stop();
        if (stitchPanel.pair)   stitchPanel.pair->stop();
        return 5;
    }

    qInfo().noquote() << QString(
        "p07 run stage=%1 raw=%2 yolo=%3 stitch=%4 mux=%5x%6 cols=%7")
        .arg(stage)
        .arg(rawPanels.size())
        .arg(yoloPanel.source ? 1 : 0)
        .arg(stitchPanel.pair ? 1 : 0)
        .arg(muxW).arg(muxH)
        .arg(cols);

    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();

    // ---- Health, status overlay, summary timers ----------------------------
    QTimer healthTimer;
    QObject::connect(&healthTimer, &QTimer::timeout, [&]() {
        for (RawPanel& panel : rawPanels) panel.source->pollHealth();
        if (yoloPanel.source)   yoloPanel.source->pollHealth();
        if (stitchPanel.pair)   stitchPanel.pair->pollHealth();
    });
    healthTimer.start(kHealthPollIntervalMs);

    QTimer statusTimer;
    QObject::connect(&statusTimer, &QTimer::timeout, [&]() {
        for (RawPanel& panel : rawPanels) {
            panel.widget->setStatusLine(rawPanelLine(panel));
            panel.widget->update();
        }
        if (yoloPanel.widget) {
            yoloPanel.widget->setStatusLine(yoloPanelLine(yoloPanel));
            yoloPanel.widget->update();
        }
        if (stitchPanel.widget) {
            stitchPanel.widget->setStatusLine(stitchPanelLine(stitchPanel));
            stitchPanel.widget->update();
        }
    });
    statusTimer.start(kStatusOverlayMs);

    QTimer summaryTimer;
    QObject::connect(&summaryTimer, &QTimer::timeout, [&]() {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startMs;
        QStringList parts;
        int rawLive = 0;
        for (const RawPanel& p : rawPanels) if (!p.source->stalledNow()) ++rawLive;
        const int yoloLive = (yoloPanel.source && !yoloPanel.source->stalledNow()) ? 1 : 0;
        const int stitchLive = (stitchPanel.pair
            && !stitchPanel.pair->topStalledNow()
            && !stitchPanel.pair->bottomStalledNow()) ? 1 : 0;
        parts << QString("p07 status uptime=%1 stage=%2 raw=%3/%4 yolo=%5/%6 stitch=%7/%8")
                     .arg(formatHms(elapsed))
                     .arg(stage)
                     .arg(rawLive).arg(rawPanels.size())
                     .arg(yoloLive).arg(yoloPanel.source ? 1 : 0)
                     .arg(stitchLive).arg(stitchPanel.pair ? 1 : 0);
        for (const RawPanel& p : rawPanels) parts << rawPanelLine(p);
        if (yoloPanel.source)   parts << yoloPanelLine(yoloPanel);
        if (stitchPanel.pair)   parts << stitchPanelLine(stitchPanel);
        qInfo().noquote() << parts.join(QStringLiteral(" | "));
    });
    summaryTimer.start(kSummaryLogMs);

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
    for (RawPanel& panel : rawPanels) panel.source->stop();
    if (yoloPanel.source)   yoloPanel.source->stop();
    if (stitchPanel.pair)   stitchPanel.pair->stop();
    return rc;
}
