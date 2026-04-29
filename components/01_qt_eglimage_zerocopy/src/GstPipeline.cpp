#include "GstPipeline.hpp"

#include <QDateTime>
#include <QDebug>

GstSourcePipeline::GstSourcePipeline(std::string pipelineDesc, QObject* parent)
    : QObject(parent), m_desc(std::move(pipelineDesc)) {}

GstSourcePipeline::~GstSourcePipeline() { stop(); }

bool GstSourcePipeline::start() {
    GError* err = nullptr;
    m_pipeline = gst_parse_launch(m_desc.c_str(), &err);
    if (!m_pipeline) {
        qWarning() << "gst_parse_launch failed:" << (err ? err->message : "unknown");
        if (err) g_error_free(err);
        return false;
    }

    m_appsink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
    if (!m_appsink) {
        qWarning() << "Pipeline must contain an element named 'sink' (an appsink).";
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        return false;
    }

    // Defensive: even if the pipeline string sets these, force them here.
    g_object_set(m_appsink,
                 "emit-signals", TRUE,
                 "sync",         FALSE,
                 "max-buffers",  1u,
                 "drop",         TRUE,
                 nullptr);
    g_signal_connect(m_appsink, "new-sample",
                     G_CALLBACK(&GstSourcePipeline::onNewSampleCb), this);

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    m_busWatchId = gst_bus_add_watch(bus, &GstSourcePipeline::onBusMessageCb, this);
    gst_object_unref(bus);

    GstStateChangeReturn st = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (st == GST_STATE_CHANGE_FAILURE) {
        qWarning() << "Pipeline failed to enter PLAYING.";
        return false;
    }
    return true;
}

void GstSourcePipeline::stop() {
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
    }
    if (m_busWatchId) {
        g_source_remove(m_busWatchId);
        m_busWatchId = 0;
    }
    if (m_appsink) {
        gst_object_unref(m_appsink);
        m_appsink = nullptr;
    }
    if (m_pipeline) {
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
}

GstFlowReturn GstSourcePipeline::onNewSampleCb(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<GstSourcePipeline*>(user_data);

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
            "NvBufSurface: numFilled=%1 memType=%2 colorFormat=%3 layout=%4 width=%5 height=%6 pitch=%7 plane0-offset=%8 plane-count=%9 dataPtr=%10")
            .arg(surf->numFilled)
            .arg(int(surf->memType))
            .arg(int(s0.colorFormat))
            .arg(int(s0.layout))
            .arg(s0.width)
            .arg(s0.height)
            .arg(s0.pitch)
            .arg(s0.planeParams.offset[0])
            .arg(s0.planeParams.num_planes)
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
    holder->captureWallNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;

    emit self->newFrame(holder);
    return GST_FLOW_OK;
}

gboolean GstSourcePipeline::onBusMessageCb(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
    auto* self = static_cast<GstSourcePipeline*>(user_data);
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* dbg  = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            qWarning() << "GStreamer error from"
                       << GST_OBJECT_NAME(msg->src) << ":" << err->message;
            if (dbg) qWarning() << "  debug:" << dbg;
            emit self->pipelineError(QString::fromUtf8(err->message));
            g_clear_error(&err);
            g_free(dbg);
            break;
        }
        case GST_MESSAGE_EOS:
            qInfo() << "GStreamer end-of-stream.";
            break;
        default:
            break;
    }
    return TRUE;
}
