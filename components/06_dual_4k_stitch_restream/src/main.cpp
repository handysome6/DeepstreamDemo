#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWidget>

#include <gst/gst.h>

#include "DualRtspSourcePair.hpp"
#include "StitchFrame.hpp"
#include "StitchRestreamSession.hpp"
#include "VideoGLWidget.hpp"

namespace {

constexpr int kHealthPollIntervalMs = 200;
constexpr int kStatusOverlayMs = 500;
constexpr int kSummaryLogMs = 1000;

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

bool loadTransform(const QJsonObject& obj, const char* key, StitchTransform* out, QString* error) {
    if (!obj.contains(key)) {
        return true;
    }
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
    if (!loadTransform(obj, "topSourceFromOutput", &out->topSourceFromOutput, error)) {
        return false;
    }
    if (!loadTransform(obj, "bottomSourceFromOutput", &out->bottomSourceFromOutput, error)) {
        return false;
    }
    return true;
}

QString statusLine(const DualRtspSourcePair& pair, const QString& lastError) {
    const qint64 topAge = pair.topLastFrameAgeMs();
    const qint64 bottomAge = pair.bottomLastFrameAgeMs();
    QString line = QString("A=%1 age=%2ms rc=%3 st=%4 | B=%5 age=%6ms rc=%7 st=%8 | delta=%9ms stitched=%10 mode=%11 %12")
        .arg(pair.topStalledNow() ? "STALL" : "LIVE")
        .arg(topAge < 0 ? QStringLiteral("--") : QString::number(topAge))
        .arg(pair.topReconnectCount())
        .arg(pair.topStallEventCount())
        .arg(pair.bottomStalledNow() ? "STALL" : "LIVE")
        .arg(bottomAge < 0 ? QStringLiteral("--") : QString::number(bottomAge))
        .arg(pair.bottomReconnectCount())
        .arg(pair.bottomStallEventCount())
        .arg(pair.lastPairDeltaMs() < 0 ? QStringLiteral("--") : QString::number(pair.lastPairDeltaMs()))
        .arg(pair.stitchedFrameCount())
        .arg(pair.modeTag())
        .arg(pair.degradedNow() ? "DEGRADED" : "OK");
    if (!lastError.isEmpty()) {
        line += QString(" err=%1").arg(lastError);
    }
    return line;
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
        "P0.6 — dual 4K top-bottom GPU stitch and RTSP restream over a pure device-memory path.");
    parser.addHelpOption();

    QCommandLineOption topUriOpt(QStringList() << "top-uri", "Top RTSP URI.", "uri");
    QCommandLineOption bottomUriOpt(QStringList() << "bottom-uri", "Bottom RTSP URI.", "uri");
    QCommandLineOption calibrationOpt(QStringList() << "calibration", "Calibration JSON path.", "path", "configs/identity_pair.json");
    QCommandLineOption latencyOpt(QStringList() << "latency", "nvurisrcbin jitter buffer in ms.", "ms", "0");
    QCommandLineOption noDropOnLatencyOpt(QStringList() << "no-drop-on-latency", "Disable drop-on-latency.");
    QCommandLineOption reconnectIvOpt(QStringList() << "reconnect-interval", "Reconnect interval in seconds.", "s", "5");
    QCommandLineOption reconnectAttemptsOpt(QStringList() << "reconnect-attempts", "Reconnect attempts (0=forever).", "n", "0");
    QCommandLineOption rtpProtocolOpt(QStringList() << "rtp", "RTP transport: tcp or udp.", "proto", "tcp");
    QCommandLineOption stallTimeoutOpt(QStringList() << "stall-timeout", "Stall timeout ms.", "ms", "2000");
    QCommandLineOption maxPairDeltaOpt(QStringList() << "max-pair-delta", "Max pair delta ms before degrading.", "ms", "250");
    QCommandLineOption rtspHostOpt(QStringList() << "restream-host", "Restream RTSP host.", "host", "127.0.0.1");
    QCommandLineOption rtspPortOpt(QStringList() << "restream-port", "Restream RTSP port.", "port", "8554");
    QCommandLineOption mountOpt(QStringList() << "restream-mount", "Restream mount point.", "path", "/p06stitch");
    QCommandLineOption noRestreamOpt(QStringList() << "no-restream", "Disable RTSP restream branch.");
    QCommandLineOption fpsOpt(QStringList() << "fps", "Output framerate numerator.", "n", "15");
    QCommandLineOption bitrateOpt(QStringList() << "bitrate", "Restream bitrate bps.", "bps", "12000000");
    QCommandLineOption quitAfterOpt(QStringList() << "quit-after-seconds", "Exit after N seconds.", "n", "0");
    QCommandLineOption titleOpt(QStringList() << "title", "Window title.", "text", "");

    parser.addOption(topUriOpt);
    parser.addOption(bottomUriOpt);
    parser.addOption(calibrationOpt);
    parser.addOption(latencyOpt);
    parser.addOption(noDropOnLatencyOpt);
    parser.addOption(reconnectIvOpt);
    parser.addOption(reconnectAttemptsOpt);
    parser.addOption(rtpProtocolOpt);
    parser.addOption(stallTimeoutOpt);
    parser.addOption(maxPairDeltaOpt);
    parser.addOption(rtspHostOpt);
    parser.addOption(rtspPortOpt);
    parser.addOption(mountOpt);
    parser.addOption(noRestreamOpt);
    parser.addOption(fpsOpt);
    parser.addOption(bitrateOpt);
    parser.addOption(quitAfterOpt);
    parser.addOption(titleOpt);
    parser.process(app);

    const QString defaultPairUri = defaultUri();
    const QString topUri = parser.isSet(topUriOpt) ? parser.value(topUriOpt) : defaultPairUri;
    const QString bottomUri = parser.isSet(bottomUriOpt) ? parser.value(bottomUriOpt) : defaultPairUri;

    StitchRenderer::Calibration calibration;
    QString calibrationError;
    if (!loadCalibration(parser.value(calibrationOpt), &calibration, &calibrationError)) {
        qWarning().noquote() << calibrationError;
        return 2;
    }

    RtspSource::Options topOpts;
    topOpts.uri = topUri;
    topOpts.latencyMs = parser.value(latencyOpt).toInt();
    topOpts.dropOnLatency = !parser.isSet(noDropOnLatencyOpt);
    topOpts.rtspReconnectIntervalS = parser.value(reconnectIvOpt).toInt();
    topOpts.rtspReconnectAttempts = parser.value(reconnectAttemptsOpt).toInt();
    topOpts.rtpProtocol = parser.value(rtpProtocolOpt);
    topOpts.stallTimeoutMs = parser.value(stallTimeoutOpt).toInt();

    RtspSource::Options bottomOpts = topOpts;
    bottomOpts.uri = bottomUri;

    DualRtspSourcePair::Options pairOpts;
    pairOpts.top = topOpts;
    pairOpts.bottom = bottomOpts;
    pairOpts.calibration = calibration;
    pairOpts.maxPairDeltaMs = parser.value(maxPairDeltaOpt).toLongLong();

    StitchRestreamSession::Options restreamOpts;
    restreamOpts.enabled = !parser.isSet(noRestreamOpt);
    restreamOpts.width = calibration.outputWidth;
    restreamOpts.height = calibration.outputHeight;
    restreamOpts.fpsNum = parser.value(fpsOpt).toInt();
    restreamOpts.fpsDen = 1;
    restreamOpts.bitrate = parser.value(bitrateOpt).toInt();
    restreamOpts.rtspHost = parser.value(rtspHostOpt);
    restreamOpts.rtspPort = parser.value(rtspPortOpt).toInt();
    restreamOpts.mountPoint = parser.value(mountOpt);

    QWidget window;
    auto* widget = new VideoGLWidget(&window);
    window.resize(960, 1080);
    const QString explicitTitle = parser.value(titleOpt);
    window.setWindowTitle(explicitTitle.isEmpty()
        ? QStringLiteral("06_dual_4k_stitch_restream")
        : explicitTitle);

    auto* pair = new DualRtspSourcePair(pairOpts, &window);
    auto* restream = new StitchRestreamSession(restreamOpts, &window);
    QString lastError;

    QObject::connect(pair, &DualRtspSourcePair::newStitchedFrame,
                     &app, [&](StitchedFrame* frame) {
                         StitchedFrame* previewFrame = cloneStitchedFrame(*frame);
                         if (previewFrame) {
                             widget->onNewFrame(previewFrame);
                         }
                         restream->pushFrame(frame);
                     },
                     Qt::DirectConnection);
    QObject::connect(pair, &DualRtspSourcePair::pipelineError,
                     &app, [&](const QString& msg) {
                         lastError = msg;
                         qWarning().noquote() << "pipeline error:" << msg;
                     });
    QObject::connect(pair, &DualRtspSourcePair::pairDegraded,
                     &app, [&](const QString& reason) {
                         qWarning().noquote() << "pair DEGRADED:" << reason;
                     });
    QObject::connect(pair, &DualRtspSourcePair::pairRecovered,
                     &app, [&]() {
                         qInfo().noquote() << "pair RECOVERED";
                     });

    window.show();

    QString restreamError;
    if (!restream->start(&restreamError)) {
        qWarning().noquote() << restreamError;
        return 3;
    }
    if (!pair->start()) {
        qWarning().noquote() << pair->lastError();
        restream->stop();
        return 4;
    }

    qInfo().noquote() << QString("p06 run top=%1 bottom=%2 calibration=%3 restream=%4")
        .arg(topUri)
        .arg(bottomUri)
        .arg(parser.value(calibrationOpt))
        .arg(restreamOpts.enabled ? restream->rtspUrl() : QStringLiteral("disabled"));

    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();

    QTimer healthTimer;
    QObject::connect(&healthTimer, &QTimer::timeout, [&]() {
        pair->pollHealth();
    });
    healthTimer.start(kHealthPollIntervalMs);

    QTimer statusTimer;
    QObject::connect(&statusTimer, &QTimer::timeout, [&]() {
        widget->setStatusLine(statusLine(*pair, lastError));
        widget->update();
    });
    statusTimer.start(kStatusOverlayMs);

    QTimer summaryTimer;
    QObject::connect(&summaryTimer, &QTimer::timeout, [&]() {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startMs;
        qInfo().noquote() << QString(
            "p06 status uptime=%1 a=%2 age=%3ms rc=%4 st=%5 | b=%6 age=%7ms rc=%8 st=%9 | pairDelta=%10ms stitched=%11 pushed=%12 mode=%13 state=%14")
            .arg(formatHms(elapsed))
            .arg(pair->topStalledNow() ? "STALL" : "LIVE")
            .arg(pair->topLastFrameAgeMs() < 0 ? QStringLiteral("--") : QString::number(pair->topLastFrameAgeMs()))
            .arg(pair->topReconnectCount())
            .arg(pair->topStallEventCount())
            .arg(pair->bottomStalledNow() ? "STALL" : "LIVE")
            .arg(pair->bottomLastFrameAgeMs() < 0 ? QStringLiteral("--") : QString::number(pair->bottomLastFrameAgeMs()))
            .arg(pair->bottomReconnectCount())
            .arg(pair->bottomStallEventCount())
            .arg(pair->lastPairDeltaMs() < 0 ? QStringLiteral("--") : QString::number(pair->lastPairDeltaMs()))
            .arg(pair->stitchedFrameCount())
            .arg(restream->pushedFrameCount())
            .arg(pair->modeTag())
            .arg(pair->degradedNow() ? "DEGRADED" : "OK");
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
    pair->stop();
    restream->stop();
    return rc;
}
