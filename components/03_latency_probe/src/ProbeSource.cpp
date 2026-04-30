#include "ProbeSource.hpp"

#include <QDebug>

#include <glib.h>

namespace {

// CLOCK_MONOTONIC microseconds — the only clock the probe is allowed to use.
// Hardcoded as a static helper so the rule cannot be broken accidentally
// elsewhere in this component.
qint64 monotonicNowUs() {
    return qint64(g_get_monotonic_time());
}

int rtpProtocolValue(const QString& name) {
    if (name.compare("tcp", Qt::CaseInsensitive) == 0) return 4;
    if (name.compare("udp", Qt::CaseInsensitive) == 0) return 1;
    qWarning() << "Unknown rtp protocol" << name << "- falling back to TCP";
    return 4;
}

}  // namespace

GstCaps* probeReferenceCaps() {
    // gst_caps_new_empty_simple is fine to call once; we leak the singleton
    // intentionally for process lifetime. gst_buffer_get_reference_timestamp_meta
    // matches by gst_caps_is_equal, so a single live instance is enough.
    static GstCaps* caps = gst_caps_new_empty_simple("timestamp/x-probe-emit");
    return caps;
}

ProbeSource::ProbeSource(Options opts, QObject* parent)
    : QObject(parent), m_opts(std::move(opts)) {}

ProbeSource::~ProbeSource() { stop(); }

bool ProbeSource::start() {
    m_pipeline = gst_pipeline_new("probe-pipeline");
    m_convert    = gst_element_factory_make("nvvideoconvert", "convert");
    m_capsfilter = gst_element_factory_make("capsfilter",     "caps");
    m_appsink    = gst_element_factory_make("appsink",        "sink");

    if (!m_pipeline || !m_convert || !m_capsfilter || !m_appsink) {
        qWarning() << "Failed to create core elements (pipeline/convert/caps/appsink).";
        stop();
        return false;
    }

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
                     G_CALLBACK(&ProbeSource::onNewSampleCb), this);

    gst_bin_add_many(GST_BIN(m_pipeline),
                     m_convert, m_capsfilter, m_appsink, nullptr);
    if (!gst_element_link_many(m_convert, m_capsfilter, m_appsink, nullptr)) {
        qWarning() << "Failed to link nvvideoconvert -> capsfilter -> appsink";
        stop();
        return false;
    }

    bool ok = false;
    switch (m_opts.sourceMode) {
        case SourceMode::VideoTestSrc: ok = startVideoTestSrc(); break;
        case SourceMode::Rtsp:         ok = startRtsp();         break;
    }
    if (!ok) {
        stop();
        return false;
    }

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    m_busWatchId = gst_bus_add_watch(bus, &ProbeSource::onBusMessageCb, this);
    gst_object_unref(bus);

    qInfo().noquote() << QString("Starting probe pipeline (mode=%1)")
        .arg(m_opts.sourceMode == SourceMode::VideoTestSrc ? "videotestsrc" : "rtsp");

    GstStateChangeReturn st = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (st == GST_STATE_CHANGE_FAILURE) {
        qWarning() << "Pipeline failed to enter PLAYING.";
        return false;
    }
    return true;
}

bool ProbeSource::startVideoTestSrc() {
    m_source = gst_element_factory_make("videotestsrc", "src");
    if (!m_source) {
        qWarning() << "Failed to create videotestsrc.";
        return false;
    }
    g_object_set(m_source,
                 "is-live",  TRUE,
                 "pattern",  18,           // ball — moves, easy to eyeball
                 nullptr);

    // Capsfilter to pin width/height/framerate at the source.
    GstElement* srcCaps = gst_element_factory_make("capsfilter", "src-caps");
    if (!srcCaps) {
        qWarning() << "Failed to create source-side capsfilter.";
        return false;
    }
    GstCaps* c = gst_caps_new_simple("video/x-raw",
        "width",     G_TYPE_INT,        m_opts.testWidth,
        "height",    G_TYPE_INT,        m_opts.testHeight,
        "framerate", GST_TYPE_FRACTION, m_opts.testFps, 1,
        nullptr);
    g_object_set(srcCaps, "caps", c, nullptr);
    gst_caps_unref(c);

    gst_bin_add_many(GST_BIN(m_pipeline), m_source, srcCaps, nullptr);
    if (!gst_element_link_many(m_source, srcCaps, m_convert, nullptr)) {
        qWarning() << "Failed to link videotestsrc -> capsfilter -> nvvideoconvert.";
        return false;
    }

    // t0 probe sits on videotestsrc's static src pad — i.e. as close to
    // "frame emitted by source" as we can possibly get.
    GstPad* pad = gst_element_get_static_pad(m_source, "src");
    if (!pad) {
        qWarning() << "videotestsrc has no src pad?";
        return false;
    }
    installSrcProbe(pad);
    gst_object_unref(pad);
    return true;
}

bool ProbeSource::startRtsp() {
    m_source = gst_element_factory_make("nvurisrcbin", "src");
    if (!m_source) {
        qWarning() << "Failed to create nvurisrcbin.";
        return false;
    }
    g_object_set(m_source,
                 "uri",                     m_opts.uri.toUtf8().constData(),
                 "latency",                 guint(m_opts.latencyMs),
                 "drop-on-latency",         gboolean(m_opts.dropOnLatency),
                 "rtsp-reconnect-interval", guint(m_opts.rtspReconnectIntervalS),
                 "rtsp-reconnect-attempts", gint(m_opts.rtspReconnectAttempts),
                 "select-rtp-protocol",     guint(rtpProtocolValue(m_opts.rtpProtocol)),
                 nullptr);

    gst_bin_add(GST_BIN(m_pipeline), m_source);

    // nvurisrcbin's src pad is dynamic (P0.2 spelled out why). The pad probe
    // for t0 stamping has to be installed inside pad-added.
    g_signal_connect(m_source, "pad-added",
                     G_CALLBACK(&ProbeSource::onPadAddedCb), this);
    return true;
}

void ProbeSource::installSrcProbe(GstPad* pad) {
    if (m_probedPad) {
        // Old probe still attached (e.g. nvurisrcbin re-emitted pad-added on
        // reconnect). Remove before installing the new one.
        gst_pad_remove_probe(m_probedPad, m_probeId);
        gst_object_unref(m_probedPad);
        m_probedPad = nullptr;
        m_probeId   = 0;
    }
    m_probedPad = GST_PAD(gst_object_ref(pad));
    m_probeId   = gst_pad_add_probe(pad,
                                    GST_PAD_PROBE_TYPE_BUFFER,
                                    &ProbeSource::onSrcBufferProbe,
                                    this,
                                    nullptr);
    qInfo() << "t0 stamping probe installed on" << GST_PAD_NAME(pad);
}

void ProbeSource::stop() {
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
    }
    if (m_busWatchId) {
        g_source_remove(m_busWatchId);
        m_busWatchId = 0;
    }
    if (m_probedPad) {
        gst_pad_remove_probe(m_probedPad, m_probeId);
        gst_object_unref(m_probedPad);
        m_probedPad = nullptr;
        m_probeId   = 0;
    }
    if (m_pipeline) {
        gst_object_unref(m_pipeline);
        m_pipeline   = nullptr;
        m_source     = nullptr;
        m_convert    = nullptr;
        m_capsfilter = nullptr;
        m_appsink    = nullptr;
    }
}

GstPadProbeReturn ProbeSource::onSrcBufferProbe(GstPad* /*pad*/,
                                                GstPadProbeInfo* info,
                                                gpointer /*user_data*/) {
    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;

    // Need a writable buffer to attach metas. The canonical pattern is:
    // make_writable, then write the (possibly new) pointer back into the
    // probe info struct.
    buf = gst_buffer_make_writable(buf);
    GST_PAD_PROBE_INFO_DATA(info) = buf;

    // Microseconds in, nanoseconds on the meta (GstClockTime is ns).
    const GstClockTime tsNs = GstClockTime(monotonicNowUs()) * 1000;
    gst_buffer_add_reference_timestamp_meta(buf,
                                            probeReferenceCaps(),
                                            tsNs,
                                            GST_CLOCK_TIME_NONE);
    return GST_PAD_PROBE_OK;
}

void ProbeSource::onPadAddedCb(GstElement* /*src*/,
                               GstPad* new_pad,
                               gpointer user_data) {
    auto* self = static_cast<ProbeSource*>(user_data);

    GstCaps* caps = gst_pad_get_current_caps(new_pad);
    if (!caps) caps = gst_pad_query_caps(new_pad, nullptr);
    const gchar* name = caps ? gst_structure_get_name(gst_caps_get_structure(caps, 0)) : "";
    const bool isVideo = name && (g_str_has_prefix(name, "video/"));
    if (caps) gst_caps_unref(caps);
    if (!isVideo) {
        qInfo() << "Ignoring non-video pad from nvurisrcbin:" << (name ? name : "(null)");
        return;
    }

    GstPad* sinkPad = gst_element_get_static_pad(self->m_convert, "sink");
    if (!sinkPad) {
        qWarning() << "nvvideoconvert has no sink pad — cannot link.";
        return;
    }
    if (gst_pad_is_linked(sinkPad)) {
        gst_object_unref(sinkPad);
        return;
    }
    GstPadLinkReturn ret = gst_pad_link(new_pad, sinkPad);
    gst_object_unref(sinkPad);
    if (GST_PAD_LINK_FAILED(ret)) {
        qWarning() << "Failed to link nvurisrcbin src pad to nvvideoconvert sink pad:" << ret;
        emit self->pipelineError(QStringLiteral("nvurisrcbin -> nvvideoconvert link failed"));
        return;
    }

    // After link succeeds, install the t0 probe on the bin's src pad. Order
    // matters: if we attached before linking, the buffer would still flow but
    // the downstream side would not yet exist.
    self->installSrcProbe(new_pad);
    qInfo() << "Linked nvurisrcbin src pad to nvvideoconvert sink pad.";
}

GstFlowReturn ProbeSource::onNewSampleCb(GstAppSink* sink, gpointer user_data) {
    return static_cast<ProbeSource*>(user_data)->handleNewSample(sink);
}

GstFlowReturn ProbeSource::handleNewSample(GstAppSink* sink) {
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
    if (surf->memType != NVBUF_MEM_CUDA_DEVICE ||
        s0.colorFormat != NVBUF_COLOR_FORMAT_RGBA ||
        s0.layout != NVBUF_LAYOUT_PITCH ||
        s0.dataPtr == nullptr) {
        qWarning() << "Unsupported dGPU frame contract on appsink output.";
        gst_buffer_unmap(buf, &mapInfo);
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    // t0: read the meta we stamped at the source pad probe. Skip frames
    // missing it loudly — that is a contract violation, not noise.
    GstReferenceTimestampMeta* meta =
        gst_buffer_get_reference_timestamp_meta(buf, probeReferenceCaps());
    qint64 t0_us = 0;
    if (meta) {
        t0_us = qint64(meta->timestamp / 1000);  // ns -> us
    } else {
        const quint64 misses = m_missingT0.fetch_add(1) + 1;
        if (misses == 1 || misses % 1000 == 0) {
            qWarning() << "Frame arrived without t0 meta (count=" << misses
                       << ") — meta dropped somewhere upstream.";
        }
        gst_buffer_unmap(buf, &mapInfo);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    auto* holder = new FrameHolder;
    holder->sample      = sample;
    holder->buffer      = buf;
    holder->mapInfo     = mapInfo;
    holder->surface     = surf;
    holder->width       = s0.width;
    holder->height      = s0.height;
    holder->pitch       = s0.pitch;
    holder->memType     = int(surf->memType);
    holder->colorFormat = int(s0.colorFormat);
    holder->layout      = int(s0.layout);
    holder->pts         = GST_BUFFER_PTS(buf);
    holder->t0_us       = t0_us;
    holder->t1_us       = monotonicNowUs();

    m_totalFrames.fetch_add(1);
    emit newFrame(holder);
    return GST_FLOW_OK;
}

gboolean ProbeSource::onBusMessageCb(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
    auto* self = static_cast<ProbeSource*>(user_data);

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
            qInfo() << "GST end-of-stream.";
            break;
        default:
            break;
    }
    return TRUE;
}

void ProbeSource::handleBusError(GstMessage* msg) {
    GError* err = nullptr;
    gchar* dbg  = nullptr;
    gst_message_parse_error(msg, &err, &dbg);
    const QString src = QString::fromUtf8(GST_OBJECT_NAME(msg->src));
    const QString message = err ? QString::fromUtf8(err->message) : QStringLiteral("unknown");

    qWarning().noquote() << QString("GST error from %1: %2").arg(src, message);
    if (dbg) qWarning().noquote() << "  debug:" << dbg;

    // Same filter as P0.2: errors from inside nvurisrcbin are transient
    // (reconnect handles them); only top-level errors are fatal here.
    const bool fromUriBin = src.contains(QStringLiteral("src"))
                         || src.contains(QStringLiteral("rtspsrc"))
                         || src.contains(QStringLiteral("nvv4l2decoder"));
    if (!fromUriBin) {
        emit pipelineError(message);
    }
    g_clear_error(&err);
    g_free(dbg);
}
