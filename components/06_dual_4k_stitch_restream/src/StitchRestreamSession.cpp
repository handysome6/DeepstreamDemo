#include "StitchRestreamSession.hpp"

#include <QDebug>

#include <algorithm>
#include <memory>

namespace {

QString normalizedMountPoint(QString mountPoint) {
    if (!mountPoint.startsWith('/')) {
        mountPoint.prepend('/');
    }
    return mountPoint;
}

int gopFrames(const StitchRestreamSession::Options& opts) {
    const int fpsDen = std::max(1, opts.fpsDen);
    return std::max(1, opts.fpsNum / fpsDen);
}

QString capsString(const StitchRestreamSession::Options& opts, const char* format) {
    return QStringLiteral("video/x-raw(memory:NVMM),format=%1,width=%2,height=%3,framerate=%4/%5")
        .arg(QString::fromLatin1(format))
        .arg(opts.width)
        .arg(opts.height)
        .arg(opts.fpsNum)
        .arg(opts.fpsDen);
}

}  // namespace

StitchRestreamSession::StitchRestreamSession(Options opts, QObject* parent)
    : QObject(parent), m_opts(std::move(opts)) {}

StitchRestreamSession::~StitchRestreamSession() {
    stop();
}

QString StitchRestreamSession::rtspUrl() const {
    return QString("rtsp://%1:%2%3")
        .arg(m_opts.rtspHost)
        .arg(m_opts.rtspPort)
        .arg(normalizedMountPoint(m_opts.mountPoint));
}

GstCaps* StitchRestreamSession::sourceCaps() const {
    const QByteArray text = capsString(m_opts, "RGBA").toUtf8();
    return gst_caps_from_string(text.constData());
}

GstCaps* StitchRestreamSession::encoderCaps() const {
    const QByteArray text = capsString(m_opts, "NV12").toUtf8();
    return gst_caps_from_string(text.constData());
}

bool StitchRestreamSession::start(QString* error) {
    if (!m_opts.enabled) return true;
    m_lastError.clear();
    m_pushedFrameCount = 0;

    m_pipeline = gst_pipeline_new("p06-restream-pipeline");
    m_appsrc = gst_element_factory_make("appsrc", "stitch-appsrc");
    m_convert = gst_element_factory_make("nvvideoconvert", "stitch-convert");
    m_capsfilter = gst_element_factory_make("capsfilter", "stitch-caps");
    m_encoder = gst_element_factory_make("nvv4l2h265enc", "stitch-encoder");
    m_parser = gst_element_factory_make("h265parse", "stitch-parser");
    m_queue = gst_element_factory_make("queue", "stitch-queue");
    m_rtspClientSink = gst_element_factory_make("rtspclientsink", "stitch-rtsp-client");

    if (!m_pipeline || !m_appsrc || !m_convert || !m_capsfilter ||
        !m_encoder || !m_parser || !m_queue || !m_rtspClientSink) {
        m_lastError = QStringLiteral(
            "failed to create restream elements: pipeline=%1 appsrc=%2 convert=%3 caps=%4 encoder=%5 parser=%6 queue=%7 rtspclientsink=%8")
            .arg(m_pipeline ? "ok" : "NULL")
            .arg(m_appsrc ? "ok" : "NULL")
            .arg(m_convert ? "ok" : "NULL")
            .arg(m_capsfilter ? "ok" : "NULL")
            .arg(m_encoder ? "ok" : "NULL")
            .arg(m_parser ? "ok" : "NULL")
            .arg(m_queue ? "ok" : "NULL")
            .arg(m_rtspClientSink ? "ok" : "NULL");
        if (error) *error = m_lastError;
        stop();
        return false;
    }

    GstCaps* srcCaps = sourceCaps();
    if (!srcCaps) {
        m_lastError = QStringLiteral("failed to build RGBA NVMM source caps");
        if (error) *error = m_lastError;
        stop();
        return false;
    }
    g_object_set(m_appsrc,
                 "is-live", TRUE,
                 "do-timestamp", TRUE,
                 "format", GST_FORMAT_TIME,
                 "stream-type", GST_APP_STREAM_TYPE_STREAM,
                 "block", FALSE,
                 "max-buffers", guint64(1),
                 "max-bytes", guint64(0),
                 "max-time", guint64(0),
                 "leaky-type", GST_APP_LEAKY_TYPE_DOWNSTREAM,
                 "caps", srcCaps,
                 nullptr);
    gst_caps_unref(srcCaps);

    GstCaps* encCaps = encoderCaps();
    if (!encCaps) {
        m_lastError = QStringLiteral("failed to build NV12 NVMM encoder caps");
        if (error) *error = m_lastError;
        stop();
        return false;
    }
    g_object_set(m_capsfilter, "caps", encCaps, nullptr);
    gst_caps_unref(encCaps);

    const int gop = gopFrames(m_opts);
    g_object_set(m_convert,
                 "nvbuf-memory-type", 2,
                 "compute-hw", 1,
                 nullptr);
    g_object_set(m_encoder,
                 "gpu-id", 0u,
                 "bitrate", guint(m_opts.bitrate),
                 "insert-sps-pps", TRUE,
                 "iframeinterval", gop,
                 "idrinterval", gop,
                 "preset-id", 1u,
                 "tuning-info-id", 2u,
                 nullptr);
    g_object_set(m_queue,
                 "max-size-buffers", 4u,
                 "max-size-bytes", 0u,
                 "max-size-time", guint64(0),
                 "leaky", 2,
                 nullptr);

    const QByteArray location = rtspUrl().toUtf8();
    g_object_set(m_rtspClientSink,
                 "location", location.constData(),
                 "protocols", guint(4),
                 "latency", 0u,
                 "message-forward", TRUE,
                 nullptr);

    gst_bin_add_many(GST_BIN(m_pipeline),
                     m_appsrc,
                     m_convert,
                     m_capsfilter,
                     m_encoder,
                     m_parser,
                     m_queue,
                     m_rtspClientSink,
                     nullptr);

    if (!gst_element_link_many(m_appsrc,
                               m_convert,
                               m_capsfilter,
                               m_encoder,
                               m_parser,
                               m_queue,
                               m_rtspClientSink,
                               nullptr)) {
        m_lastError = QStringLiteral("failed to link restream pipeline elements");
        if (error) *error = m_lastError;
        stop();
        return false;
    }

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    m_busWatchId = gst_bus_add_watch(bus, &StitchRestreamSession::onBusMessageCb, this);
    gst_object_unref(bus);

    qInfo().noquote() << QString(
        "restream LAUNCH appsrc ! nvvideoconvert ! video/x-raw(memory:NVMM),format=NV12 ! nvv4l2h265enc ! h265parse ! rtspclientsink location=%1")
        .arg(rtspUrl());

    const GstStateChangeReturn st = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (st == GST_STATE_CHANGE_FAILURE) {
        m_lastError = QStringLiteral("restream pipeline failed to enter PLAYING");
        if (error) *error = m_lastError;
        stop();
        return false;
    }

    qInfo().noquote() << QString("restream READY url=%1 bitrate=%2 fps=%3/%4 codec=H265 via=rtspclientsink")
        .arg(rtspUrl())
        .arg(m_opts.bitrate)
        .arg(m_opts.fpsNum)
        .arg(m_opts.fpsDen);
    m_running = true;
    return true;
}

void StitchRestreamSession::stop() {
    m_running = false;

    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
    }
    if (m_busWatchId != 0) {
        g_source_remove(m_busWatchId);
        m_busWatchId = 0;
    }
    if (m_pipeline) {
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        m_appsrc = nullptr;
        m_convert = nullptr;
        m_capsfilter = nullptr;
        m_encoder = nullptr;
        m_parser = nullptr;
        m_queue = nullptr;
        m_rtspClientSink = nullptr;
    }
}

void StitchRestreamSession::destroyWrappedSurface(gpointer data) {
    auto* surface = static_cast<NvBufSurface*>(data);
    if (surface) {
        destroyWrappedCudaSurface(surface);
    }
}

gboolean StitchRestreamSession::onBusMessageCb(GstBus* bus,
                                               GstMessage* message,
                                               gpointer user_data) {
    Q_UNUSED(bus);
    auto* self = static_cast<StitchRestreamSession*>(user_data);
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR:
            self->handleBusError(message);
            break;
        case GST_MESSAGE_WARNING:
            self->handleBusWarning(message);
            break;
        case GST_MESSAGE_EOS:
            self->m_lastError = QStringLiteral("restream pipeline reached EOS");
            qWarning().noquote() << self->m_lastError;
            break;
        default:
            break;
    }
    return G_SOURCE_CONTINUE;
}

void StitchRestreamSession::handleBusError(GstMessage* message) {
    GError* err = nullptr;
    gchar* debugInfo = nullptr;
    gst_message_parse_error(message, &err, &debugInfo);

    const QString srcName = QString::fromUtf8(GST_OBJECT_NAME(message->src));
    const QString errText = err ? QString::fromUtf8(err->message) : QStringLiteral("unknown error");
    m_lastError = QStringLiteral("restream error from %1: %2").arg(srcName, errText);
    if (debugInfo && *debugInfo) {
        qWarning().noquote() << m_lastError;
        qWarning().noquote() << QStringLiteral("restream debug: %1").arg(QString::fromUtf8(debugInfo));
    } else {
        qWarning().noquote() << m_lastError;
    }

    if (err) g_error_free(err);
    g_free(debugInfo);
}

void StitchRestreamSession::handleBusWarning(GstMessage* message) {
    GError* err = nullptr;
    gchar* debugInfo = nullptr;
    gst_message_parse_warning(message, &err, &debugInfo);

    const QString srcName = QString::fromUtf8(GST_OBJECT_NAME(message->src));
    const QString errText = err ? QString::fromUtf8(err->message) : QStringLiteral("unknown warning");
    const QString warning = QStringLiteral("restream warning from %1: %2").arg(srcName, errText);
    if (debugInfo && *debugInfo) {
        qWarning().noquote() << warning;
        qWarning().noquote() << QStringLiteral("restream debug: %1").arg(QString::fromUtf8(debugInfo));
    } else {
        qWarning().noquote() << warning;
    }

    if (err) g_error_free(err);
    g_free(debugInfo);
}

void StitchRestreamSession::pushFrame(StitchedFrame* frame) {
    if (!frame) return;
    std::unique_ptr<StitchedFrame> owner(frame);
    if (!m_running || !m_appsrc || !frame->surface) {
        return;
    }

    NvBufSurface* detached = frame->surface;
    frame->surface = nullptr;

    GstBuffer* buffer = gst_buffer_new_wrapped_full(
        GstMemoryFlags(0),
        detached,
        sizeof(NvBufSurface),
        0,
        sizeof(NvBufSurface),
        detached,
        &StitchRestreamSession::destroyWrappedSurface);
    if (!buffer) {
        destroyWrappedCudaSurface(detached);
        m_lastError = QStringLiteral("gst_buffer_new_wrapped_full failed");
        qWarning().noquote() << m_lastError;
        return;
    }

    GST_BUFFER_PTS(buffer) = frame->pts;
    GST_BUFFER_DTS(buffer) = frame->pts;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale_int(GST_SECOND, m_opts.fpsDen, m_opts.fpsNum);

    const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), buffer);
    if (flow != GST_FLOW_OK) {
        m_lastError = QStringLiteral("gst_app_src_push_buffer failed: %1").arg(int(flow));
        qWarning().noquote() << m_lastError;
        return;
    }

    ++m_pushedFrameCount;
}
