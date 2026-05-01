#include "RtspSource.hpp"

#include <QDateTime>
#include <QDebug>

namespace {

qint64 nowWallMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

// nvurisrcbin's "select-rtp-protocol" exposes a flag-style enum (verified via
// `gst-inspect-1.0 nvurisrcbin`):
//   1 = udp, 2 = udp-mcast, 4 = tcp, 7 = udp+udpmcast+tcp (default), 10 = http,
//   20 = tls.
// We expose the two we actually care about for production cameras by name.
int rtpProtocolValue(const QString& name) {
    if (name.compare("tcp", Qt::CaseInsensitive) == 0) return 4;
    if (name.compare("udp", Qt::CaseInsensitive) == 0) return 1;
    qWarning() << "Unknown rtp protocol" << name << "- falling back to TCP";
    return 4;
}

}  // namespace

RtspSource::RtspSource(Options opts, QObject* parent)
    : QObject(parent), m_opts(std::move(opts)) {}

RtspSource::~RtspSource() { stop(); }

bool RtspSource::start() {
    m_pipeline   = gst_pipeline_new("rtsp-pipeline");
    m_uriSrcBin  = gst_element_factory_make("nvurisrcbin",   "uri-src");
    m_convert    = gst_element_factory_make("nvvideoconvert", "convert");
    m_capsfilter = gst_element_factory_make("capsfilter",     "caps");
    m_appsink    = gst_element_factory_make("appsink",        "sink");

    if (!m_pipeline || !m_uriSrcBin || !m_convert || !m_capsfilter || !m_appsink) {
        qWarning().noquote() << QString(
            "Failed to create elements: pipeline=%1 nvurisrcbin=%2 "
            "nvvideoconvert=%3 capsfilter=%4 appsink=%5. "
            "Run `gst-inspect-1.0 nvurisrcbin` to verify the DeepStream "
            "plugin is on the GST_PLUGIN_PATH at runtime.")
            .arg(m_pipeline   ? "ok" : "NULL")
            .arg(m_uriSrcBin  ? "ok" : "NULL")
            .arg(m_convert    ? "ok" : "NULL")
            .arg(m_capsfilter ? "ok" : "NULL")
            .arg(m_appsink    ? "ok" : "NULL");
        stop();
        return false;
    }

    // ---- nvurisrcbin: live RTSP source with built-in reconnect ---------------
    g_object_set(m_uriSrcBin,
                 "uri",                      m_opts.uri.toUtf8().constData(),
                 "latency",                  guint(m_opts.latencyMs),
                 "drop-on-latency",          gboolean(m_opts.dropOnLatency),
                 // Seconds. Polled internally; 0 would disable the watcher.
                 "rtsp-reconnect-interval",  guint(m_opts.rtspReconnectIntervalS),
                 // 0 means "never give up". We want this for production
                 // cameras that may be down for arbitrary periods.
                 "rtsp-reconnect-attempts",  gint(m_opts.rtspReconnectAttempts),
                 "select-rtp-protocol",      guint(rtpProtocolValue(m_opts.rtpProtocol)),
                 // type=0 (auto) is fine: nvurisrcbin already routes
                 // rtsp:// URIs through its rtsp-aware path. Setting type=2
                 // would only be needed for smart-record support, which we
                 // do not exercise here.
                 nullptr);

    // ---- caps after nvvideoconvert: NVMM RGBA, the dGPU GPU-only contract ----
    GstCaps* caps = gst_caps_from_string("video/x-raw(memory:NVMM),format=RGBA");
    g_object_set(m_capsfilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // ---- appsink: at-most-one buffer, drop on backpressure -------------------
    g_object_set(m_appsink,
                 "emit-signals", TRUE,
                 "sync",         FALSE,
                 "max-buffers",  1u,
                 "drop",         TRUE,
                 nullptr);
    g_signal_connect(m_appsink, "new-sample",
                     G_CALLBACK(&RtspSource::onNewSampleCb), this);

    gst_bin_add_many(GST_BIN(m_pipeline),
                     m_uriSrcBin, m_convert, m_capsfilter, m_appsink, nullptr);

    if (!gst_element_link_many(m_convert, m_capsfilter, m_appsink, nullptr)) {
        qWarning() << "Failed to link nvvideoconvert -> capsfilter -> appsink";
        stop();
        return false;
    }

    // nvurisrcbin's src pad is dynamic — it appears once the URI has been
    // resolved and the decoder is plugged in. Defer the link to pad-added.
    g_signal_connect(m_uriSrcBin, "pad-added",
                     G_CALLBACK(&RtspSource::onPadAddedCb), this);

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    m_busWatchId = gst_bus_add_watch(bus, &RtspSource::onBusMessageCb, this);
    gst_object_unref(bus);

    qInfo().noquote() << QString(
        "Starting RTSP pipeline: uri=%1 latency=%2ms dropOnLatency=%3 "
        "reconnectInterval=%4s reconnectAttempts=%5 rtp=%6 stallTimeout=%7ms")
        .arg(m_opts.uri)
        .arg(m_opts.latencyMs)
        .arg(m_opts.dropOnLatency ? "true" : "false")
        .arg(m_opts.rtspReconnectIntervalS)
        .arg(m_opts.rtspReconnectAttempts)
        .arg(m_opts.rtpProtocol)
        .arg(m_opts.stallTimeoutMs);

    GstStateChangeReturn st = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (st == GST_STATE_CHANGE_FAILURE) {
        qWarning() << "Pipeline failed to enter PLAYING.";
        return false;
    }
    return true;
}

void RtspSource::stop() {
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
        // The bin took ownership of the elements above; null our pointers.
        m_uriSrcBin  = nullptr;
        m_convert    = nullptr;
        m_capsfilter = nullptr;
        m_appsink    = nullptr;
    }
}

qint64 RtspSource::lastFrameAgeMs() const {
    const qint64 last = m_lastFrameWallMs.load();
    if (last == 0) return -1;
    return nowWallMs() - last;
}

void RtspSource::pollHealth() {
    const qint64 age = lastFrameAgeMs();
    const bool wasStalled = m_stalled.load();

    // Negative age means we have not yet seen any frame — leave the initial
    // "stalled" state alone but don't count it as a reconnect-from-stall yet
    // because we never went live in the first place.
    if (age < 0) {
        return;
    }

    if (wasStalled && age < m_opts.stallTimeoutMs) {
        // Transition: stalled -> live. The very first successful frame proves
        // startup, not reconnect; only later stall recoveries count.
        m_stalled.store(false);
        if (m_hadFirstLiveFrame.exchange(true)) {
            m_reconnectCount.fetch_add(1);
        }
        qInfo().noquote() << QString(
            "stream LIVE — frame just arrived (age=%1 ms, total reconnects=%2, frames=%3)")
            .arg(age).arg(m_reconnectCount.load()).arg(m_totalFrames.load());
        emit streamLive();
    } else if (!wasStalled && age >= m_opts.stallTimeoutMs) {
        // Transition: live -> stalled.
        m_stalled.store(true);
        m_stallStartCount.fetch_add(1);
        qWarning().noquote() << QString(
            "stream STALLED — no frame for %1 ms (stall events=%2)")
            .arg(age).arg(m_stallStartCount.load());
        emit streamStalled(age);
    }
}

GstFlowReturn RtspSource::onNewSampleCb(GstAppSink* sink, gpointer user_data) {
    return static_cast<RtspSource*>(user_data)->handleNewSample(sink);
}

GstFlowReturn RtspSource::handleNewSample(GstAppSink* sink) {
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

void RtspSource::onPadAddedCb(GstElement* src, GstPad* new_pad, gpointer user_data) {
    Q_UNUSED(src);
    static_cast<RtspSource*>(user_data)->linkUriPadToConvert(new_pad);
}

void RtspSource::linkUriPadToConvert(GstPad* new_pad) {
    GstPad* sinkPad = gst_element_get_static_pad(m_convert, "sink");
    if (!sinkPad) {
        qWarning() << "nvvideoconvert has no sink pad — cannot link.";
        return;
    }

    if (gst_pad_is_linked(sinkPad)) {
        // We already have a video pad linked. nvurisrcbin can in principle
        // expose audio pads too; we just ignore extras.
        gst_object_unref(sinkPad);
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(new_pad);
    if (!caps) caps = gst_pad_query_caps(new_pad, nullptr);
    const gchar* name = caps ? gst_structure_get_name(gst_caps_get_structure(caps, 0)) : "";
    const bool isVideo = name && (g_str_has_prefix(name, "video/"));
    if (caps) gst_caps_unref(caps);

    if (!isVideo) {
        qInfo() << "Ignoring non-video pad from nvurisrcbin:" << (name ? name : "(null)");
        gst_object_unref(sinkPad);
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(new_pad, sinkPad);
    gst_object_unref(sinkPad);

    if (GST_PAD_LINK_FAILED(ret)) {
        qWarning() << "Failed to link nvurisrcbin src pad to nvvideoconvert sink pad:"
                   << ret;
        emit pipelineError(QStringLiteral("nvurisrcbin -> nvvideoconvert link failed"));
    } else {
        qInfo() << "Linked nvurisrcbin src pad to nvvideoconvert sink pad.";
    }
}

gboolean RtspSource::onBusMessageCb(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
    auto* self = static_cast<RtspSource*>(user_data);

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
            // nvurisrcbin emits structured element messages on connect /
            // disconnect / reconnect. We don't currently rely on parsing them
            // for the stall counter (the heartbeat is more reliable), but we
            // log them so debugging post-mortems are easier.
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

void RtspSource::handleBusError(GstMessage* msg) {
    GError* err = nullptr;
    gchar* dbg  = nullptr;
    gst_message_parse_error(msg, &err, &dbg);
    const QString src = QString::fromUtf8(GST_OBJECT_NAME(msg->src));
    const QString message = err ? QString::fromUtf8(err->message) : QStringLiteral("unknown");

    qWarning().noquote() << QString("GST error from %1: %2").arg(src, message);
    if (dbg) qWarning().noquote() << "  debug:" << dbg;

    // nvurisrcbin emits transient errors during disconnect (the inner
    // rtspsrc fails its socket read). Those are NOT fatal — the bin handles
    // its own reconnect. We surface them as warnings, not as a fatal error
    // signal that the GUI uses to quit. Only treat as fatal if it came from
    // the top-level pipeline element itself.
    const bool fromUriBin = src.contains(QStringLiteral("uri-src"))
                         || src.contains(QStringLiteral("rtspsrc"))
                         || src.contains(QStringLiteral("nvv4l2decoder"));
    if (!fromUriBin) {
        emit pipelineError(message);
    }

    g_clear_error(&err);
    g_free(dbg);
}
