#include "RtspInferSource.hpp"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>

namespace {

qint64 nowWallMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

int rtpProtocolValue(const QString& name) {
    if (name.compare("tcp", Qt::CaseInsensitive) == 0) return 4;
    if (name.compare("udp", Qt::CaseInsensitive) == 0) return 1;
    qWarning() << "Unknown rtp protocol" << name << "- falling back to TCP";
    return 4;
}

}  // namespace

RtspInferSource::RtspInferSource(Options opts, QObject* parent)
    : QObject(parent), m_opts(std::move(opts)) {}

RtspInferSource::~RtspInferSource() { stop(); }

bool RtspInferSource::start() {
    bool ok = (m_opts.mode == Mode::Infer) ? buildInferPipeline() : buildRawPipeline();
    if (!ok) {
        stop();
        return false;
    }

    // ---- bus watch (shared across both shapes) ------------------------------
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    m_busWatchId = gst_bus_add_watch(bus, &RtspInferSource::onBusMessageCb, this);
    gst_object_unref(bus);

    qInfo().noquote() << QString(
        "Starting RTSP%1 pipeline: uri=%2 latency=%3ms dropOnLatency=%4 "
        "reconnectInterval=%5s reconnectAttempts=%6 rtp=%7 stallTimeout=%8ms "
        "infer=%9")
        .arg(m_opts.mode == Mode::Infer ? "+infer" : "")
        .arg(m_opts.uri)
        .arg(m_opts.latencyMs)
        .arg(m_opts.dropOnLatency ? "true" : "false")
        .arg(m_opts.rtspReconnectIntervalS)
        .arg(m_opts.rtspReconnectAttempts)
        .arg(m_opts.rtpProtocol)
        .arg(m_opts.stallTimeoutMs)
        .arg(m_opts.mode == Mode::Infer ? m_opts.inferConfigPath : QStringLiteral("(off)"));

    GstStateChangeReturn st = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (st == GST_STATE_CHANGE_FAILURE) {
        qWarning() << "Pipeline failed to enter PLAYING.";
        return false;
    }
    return true;
}

bool RtspInferSource::buildRawPipeline() {
    m_pipeline   = gst_pipeline_new("rtsp-pipeline-raw");
    m_uriSrcBin  = gst_element_factory_make("nvurisrcbin",   "uri-src");
    m_convert    = gst_element_factory_make("nvvideoconvert", "convert");
    m_capsfilter = gst_element_factory_make("capsfilter",     "caps");
    m_appsink    = gst_element_factory_make("appsink",        "sink");

    if (!m_pipeline || !m_uriSrcBin || !m_convert || !m_capsfilter || !m_appsink) {
        qWarning() << "Failed to create elements for raw pipeline.";
        return false;
    }

    g_object_set(m_uriSrcBin,
                 "uri",                      m_opts.uri.toUtf8().constData(),
                 "latency",                  guint(m_opts.latencyMs),
                 "drop-on-latency",          gboolean(m_opts.dropOnLatency),
                 "rtsp-reconnect-interval",  guint(m_opts.rtspReconnectIntervalS),
                 "rtsp-reconnect-attempts",  gint(m_opts.rtspReconnectAttempts),
                 "select-rtp-protocol",      guint(rtpProtocolValue(m_opts.rtpProtocol)),
                 nullptr);

    GstCaps* caps = gst_caps_from_string("video/x-raw(memory:NVMM),format=RGBA");
    g_object_set(m_capsfilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    g_object_set(m_appsink,
                 "emit-signals", TRUE,
                 "sync",         FALSE,
                 "max-buffers",  1u,
                 "drop",         TRUE,
                 nullptr);
    g_signal_connect(m_appsink, "new-sample",
                     G_CALLBACK(&RtspInferSource::onNewSampleCb), this);

    gst_bin_add_many(GST_BIN(m_pipeline),
                     m_uriSrcBin, m_convert, m_capsfilter, m_appsink, nullptr);

    if (!gst_element_link_many(m_convert, m_capsfilter, m_appsink, nullptr)) {
        qWarning() << "Failed to link nvvideoconvert -> capsfilter -> appsink";
        return false;
    }

    g_signal_connect(m_uriSrcBin, "pad-added",
                     G_CALLBACK(&RtspInferSource::onPadAddedCb), this);
    return true;
}

bool RtspInferSource::buildInferPipeline() {
    if (m_opts.inferConfigPath.isEmpty() ||
        !QFileInfo::exists(m_opts.inferConfigPath)) {
        qWarning().noquote() << QString(
            "Inference config not found: '%1'. Either pass --infer-config or "
            "drop back to Mode::Raw.").arg(m_opts.inferConfigPath);
        return false;
    }

    m_pipeline   = gst_pipeline_new("rtsp-pipeline-infer");
    m_uriSrcBin  = gst_element_factory_make("nvurisrcbin",   "uri-src");
    m_streammux  = gst_element_factory_make("nvstreammux",   "mux");
    m_nvinfer    = gst_element_factory_make("nvinfer",       "pgie");
    m_convert    = gst_element_factory_make("nvvideoconvert", "convert");
    m_capsfilter = gst_element_factory_make("capsfilter",     "caps");
    m_osd        = gst_element_factory_make("nvdsosd",       "osd");
    m_appsink    = gst_element_factory_make("appsink",        "sink");

    if (!m_pipeline || !m_uriSrcBin || !m_streammux || !m_nvinfer ||
        !m_convert || !m_capsfilter || !m_osd || !m_appsink) {
        qWarning().noquote() << QString(
            "Failed to create elements for infer pipeline: "
            "pipeline=%1 uri=%2 mux=%3 pgie=%4 convert=%5 caps=%6 osd=%7 sink=%8")
            .arg(m_pipeline   ? "ok" : "NULL")
            .arg(m_uriSrcBin  ? "ok" : "NULL")
            .arg(m_streammux  ? "ok" : "NULL")
            .arg(m_nvinfer    ? "ok" : "NULL")
            .arg(m_convert    ? "ok" : "NULL")
            .arg(m_capsfilter ? "ok" : "NULL")
            .arg(m_osd        ? "ok" : "NULL")
            .arg(m_appsink    ? "ok" : "NULL");
        return false;
    }

    g_object_set(m_uriSrcBin,
                 "uri",                      m_opts.uri.toUtf8().constData(),
                 "latency",                  guint(m_opts.latencyMs),
                 "drop-on-latency",          gboolean(m_opts.dropOnLatency),
                 "rtsp-reconnect-interval",  guint(m_opts.rtspReconnectIntervalS),
                 "rtsp-reconnect-attempts",  gint(m_opts.rtspReconnectAttempts),
                 "select-rtp-protocol",      guint(rtpProtocolValue(m_opts.rtpProtocol)),
                 nullptr);

    // nvstreammux: batch-size=1 because each RtspInferSource owns its own
    // pipeline. Multi-stream batching across panels is exactly the integration
    // step P0.6 will tackle, and on purpose is not what 05 proves.
    g_object_set(m_streammux,
                 "batch-size",          guint(1),
                 "width",               guint(m_opts.muxWidth),
                 "height",              guint(m_opts.muxHeight),
                 "live-source",         gboolean(TRUE),
                 // 40 ms ~ one 25fps frame; this is the gate that lets the mux
                 // emit a batched buffer if the producer slows down. Anything
                 // longer hurts steady-state latency for a single-source mux.
                 "batched-push-timeout", gint(40000),
                 nullptr);

    g_object_set(m_nvinfer,
                 "config-file-path", m_opts.inferConfigPath.toUtf8().constData(),
                 // Single-source pipeline: nvinfer must agree with the mux.
                 "batch-size",       guint(1),
                 nullptr);

    GstCaps* caps = gst_caps_from_string("video/x-raw(memory:NVMM),format=RGBA");
    g_object_set(m_capsfilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    g_object_set(m_appsink,
                 "emit-signals", TRUE,
                 "sync",         FALSE,
                 "max-buffers",  1u,
                 "drop",         TRUE,
                 nullptr);
    g_signal_connect(m_appsink, "new-sample",
                     G_CALLBACK(&RtspInferSource::onNewSampleCb), this);

    gst_bin_add_many(GST_BIN(m_pipeline),
                     m_uriSrcBin, m_streammux, m_nvinfer,
                     m_convert, m_capsfilter, m_osd, m_appsink, nullptr);

    // mux -> infer -> convert -> caps -> osd -> appsink. nvurisrcbin attaches
    // to the mux's sink_0 inside the pad-added callback below.
    if (!gst_element_link_many(m_streammux, m_nvinfer, m_convert,
                               m_capsfilter, m_osd, m_appsink, nullptr)) {
        qWarning() << "Failed to link mux -> nvinfer -> convert -> caps -> osd -> appsink";
        return false;
    }

    g_signal_connect(m_uriSrcBin, "pad-added",
                     G_CALLBACK(&RtspInferSource::onPadAddedCb), this);
    return true;
}

void RtspInferSource::stop() {
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
    }
    if (m_busWatchId) {
        g_source_remove(m_busWatchId);
        m_busWatchId = 0;
    }
    if (m_pipeline) {
        gst_object_unref(m_pipeline);
        m_pipeline   = nullptr;
        m_uriSrcBin  = nullptr;
        m_streammux  = nullptr;
        m_nvinfer    = nullptr;
        m_convert    = nullptr;
        m_capsfilter = nullptr;
        m_osd        = nullptr;
        m_appsink    = nullptr;
    }
}

bool RtspInferSource::setMode(Mode newMode) {
    if (newMode == m_opts.mode && m_pipeline != nullptr) {
        return true;
    }
    qInfo().noquote() << QString("Switching mode: %1 -> %2 (uri=%3)")
        .arg(m_opts.mode == Mode::Infer ? "infer" : "raw")
        .arg(newMode      == Mode::Infer ? "infer" : "raw")
        .arg(m_opts.uri);
    stop();
    resetCounters();
    m_opts.mode = newMode;
    const bool ok = start();
    if (ok) {
        emit modeChanged(int(newMode));
    }
    return ok;
}

void RtspInferSource::resetCounters() {
    m_lastFrameWallMs.store(0);
    m_stalled.store(true);
    m_hadFirstLiveFrame.store(false);
    // Note: reconnect / stall / total counters are NOT reset here. They are
    // lifetime counters and a mode flip is conceptually not a fresh source.
}

qint64 RtspInferSource::lastFrameAgeMs() const {
    const qint64 last = m_lastFrameWallMs.load();
    if (last == 0) return -1;
    return nowWallMs() - last;
}

void RtspInferSource::pollHealth() {
    const qint64 age = lastFrameAgeMs();
    const bool wasStalled = m_stalled.load();

    if (age < 0) return;

    if (wasStalled && age < m_opts.stallTimeoutMs) {
        m_stalled.store(false);
        if (m_hadFirstLiveFrame.exchange(true)) {
            m_reconnectCount.fetch_add(1);
        }
        qInfo().noquote() << QString(
            "stream LIVE — frame just arrived (age=%1 ms, total reconnects=%2, frames=%3)")
            .arg(age).arg(m_reconnectCount.load()).arg(m_totalFrames.load());
        emit streamLive();
    } else if (!wasStalled && age >= m_opts.stallTimeoutMs) {
        m_stalled.store(true);
        m_stallStartCount.fetch_add(1);
        qWarning().noquote() << QString(
            "stream STALLED — no frame for %1 ms (stall events=%2)")
            .arg(age).arg(m_stallStartCount.load());
        emit streamStalled(age);
    }
}

GstFlowReturn RtspInferSource::onNewSampleCb(GstAppSink* sink, gpointer user_data) {
    return static_cast<RtspInferSource*>(user_data)->handleNewSample(sink);
}

GstFlowReturn RtspInferSource::handleNewSample(GstAppSink* sink) {
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo mapInfo{};
    if (!gst_buffer_map(buf, &mapInfo, GST_MAP_READ)) {
        qWarning() << "gst_buffer_map failed; is this an NVMM buffer?";
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    auto* surf = reinterpret_cast<NvBufSurface*>(mapInfo.data);
    if (!surf || surf->numFilled == 0) {
        qWarning() << "Sample is not backed by a populated NvBufSurface.";
        gst_buffer_unmap(buf, &mapInfo);
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    const auto& s0 = surf->surfaceList[0];

    static bool s_loggedSurfaceInfo = false;
    if (!s_loggedSurfaceInfo) {
        qInfo().noquote() << QString(
            "First NvBufSurface: numFilled=%1 memType=%2 colorFormat=%3 layout=%4 "
            "size=%5x%6 pitch=%7 dataPtr=0x%8")
            .arg(surf->numFilled)
            .arg(int(surf->memType))
            .arg(int(s0.colorFormat))
            .arg(int(s0.layout))
            .arg(s0.width)
            .arg(s0.height)
            .arg(s0.pitch)
            .arg(quintptr(s0.dataPtr), 0, 16);
        s_loggedSurfaceInfo = true;
    }

    if (surf->memType != NVBUF_MEM_CUDA_DEVICE ||
        s0.colorFormat != NVBUF_COLOR_FORMAT_RGBA ||
        s0.layout != NVBUF_LAYOUT_PITCH ||
        s0.dataPtr == nullptr) {
        qWarning().noquote() << QString(
            "Unsupported dGPU frame contract: memType=%1 colorFormat=%2 layout=%3 dataPtr=%4")
            .arg(int(surf->memType))
            .arg(int(s0.colorFormat))
            .arg(int(s0.layout))
            .arg(quintptr(s0.dataPtr), 0, 16);
        gst_buffer_unmap(buf, &mapInfo);
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    auto* holder = new FrameHolder;
    holder->sample        = sample;
    holder->buffer        = buf;
    holder->mapInfo       = mapInfo;
    holder->surface       = surf;
    holder->width         = s0.width;
    holder->height        = s0.height;
    holder->pitch         = s0.pitch;
    holder->memType       = int(surf->memType);
    holder->colorFormat   = int(s0.colorFormat);
    holder->layout        = int(s0.layout);
    holder->pts           = GST_BUFFER_PTS(buf);
    holder->captureWallNs = nowWallMs() * 1000000LL;

    m_lastFrameWallMs.store(nowWallMs());
    m_totalFrames.fetch_add(1);

    emit newFrame(holder);
    return GST_FLOW_OK;
}

void RtspInferSource::onPadAddedCb(GstElement* src, GstPad* new_pad, gpointer user_data) {
    Q_UNUSED(src);
    static_cast<RtspInferSource*>(user_data)->linkUriPadToNext(new_pad);
}

void RtspInferSource::linkUriPadToNext(GstPad* new_pad) {
    // Pad caps gate: ignore audio / non-video pads coming out of nvurisrcbin.
    GstCaps* caps = gst_pad_get_current_caps(new_pad);
    if (!caps) caps = gst_pad_query_caps(new_pad, nullptr);
    const gchar* name = caps ? gst_structure_get_name(gst_caps_get_structure(caps, 0)) : "";
    const bool isVideo = name && (g_str_has_prefix(name, "video/"));
    if (caps) gst_caps_unref(caps);

    if (!isVideo) {
        qInfo() << "Ignoring non-video pad from nvurisrcbin:" << (name ? name : "(null)");
        return;
    }

    GstPad* sinkPad = nullptr;
    if (m_opts.mode == Mode::Infer) {
        // nvstreammux's sink pads are request pads named sink_%u. Each source
        // grabs sink_0 on its own private mux instance.
        sinkPad = gst_element_request_pad_simple(m_streammux, "sink_0");
        if (!sinkPad) {
            qWarning() << "nvstreammux did not provide sink_0; cannot link.";
            emit pipelineError(QStringLiteral("nvstreammux sink_0 request failed"));
            return;
        }
    } else {
        sinkPad = gst_element_get_static_pad(m_convert, "sink");
        if (!sinkPad) {
            qWarning() << "nvvideoconvert has no sink pad — cannot link.";
            return;
        }
        if (gst_pad_is_linked(sinkPad)) {
            gst_object_unref(sinkPad);
            return;
        }
    }

    GstPadLinkReturn ret = gst_pad_link(new_pad, sinkPad);
    gst_object_unref(sinkPad);

    if (GST_PAD_LINK_FAILED(ret)) {
        qWarning() << "Failed to link nvurisrcbin src pad to downstream sink pad:" << ret;
        emit pipelineError(QStringLiteral("nvurisrcbin -> downstream link failed"));
    } else {
        qInfo() << "Linked nvurisrcbin src pad to downstream sink pad (mode="
                << (m_opts.mode == Mode::Infer ? "infer" : "raw") << ").";
    }
}

gboolean RtspInferSource::onBusMessageCb(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
    auto* self = static_cast<RtspInferSource*>(user_data);

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR:
            self->handleBusError(msg);
            break;
        case GST_MESSAGE_WARNING: {
            GError* err = nullptr;
            gchar* dbg  = nullptr;
            gst_message_parse_warning(msg, &err, &dbg);
            qWarning() << "GST warning from"
                       << GST_OBJECT_NAME(msg->src) << ":" << err->message;
            if (dbg) qWarning() << "  debug:" << dbg;
            g_clear_error(&err);
            g_free(dbg);
            break;
        }
        case GST_MESSAGE_EOS:
            qInfo() << "GST end-of-stream — nvurisrcbin reconnect logic should "
                       "handle this without us tearing down the pipeline.";
            break;
        case GST_MESSAGE_ELEMENT: {
            const GstStructure* s = gst_message_get_structure(msg);
            if (s) {
                const gchar* sname = gst_structure_get_name(s);
                if (sname && (g_str_has_prefix(sname, "GstNvUriSrcBin") ||
                              g_str_has_prefix(sname, "stream-eos") ||
                              g_str_has_prefix(sname, "source-bin-resync"))) {
                    gchar* str = gst_structure_to_string(s);
                    qInfo().noquote() << "nvurisrcbin element msg:" << str;
                    g_free(str);
                }
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}

void RtspInferSource::handleBusError(GstMessage* msg) {
    GError* err = nullptr;
    gchar* dbg  = nullptr;
    gst_message_parse_error(msg, &err, &dbg);
    const QString src = QString::fromUtf8(GST_OBJECT_NAME(msg->src));
    const QString message = err ? QString::fromUtf8(err->message) : QStringLiteral("unknown");

    qWarning().noquote() << QString("GST error from %1: %2").arg(src, message);
    if (dbg) qWarning().noquote() << "  debug:" << dbg;

    // Same filter as P0.2/P0.4: only top-level pipeline errors are fatal;
    // inner rtspsrc / decoder / nvurisrcbin errors are part of the bin's own
    // disconnect/reconnect behavior.
    const bool fromUriBin = src.contains(QStringLiteral("uri-src"))
                         || src.contains(QStringLiteral("rtspsrc"))
                         || src.contains(QStringLiteral("nvv4l2decoder"));
    if (!fromUriBin) {
        emit pipelineError(message);
    }

    g_clear_error(&err);
    g_free(dbg);
}
