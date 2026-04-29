#pragma once

#include <QObject>
#include <QtGlobal>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

extern "C" {
#include <nvbufsurface.h>
}

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <string>

// Holds everything needed to keep an EGLImage alive while Qt renders it.
// Destruction order is important: unmap EGL first, then unmap the GstBuffer,
// then unref the GstSample.
struct FrameHolder {
    GstSample*    sample        = nullptr;
    GstBuffer*    buffer        = nullptr;
    GstMapInfo    mapInfo{};
    NvBufSurface* surface       = nullptr;
    EGLImageKHR   eglImage      = EGL_NO_IMAGE_KHR;
    guint64       pts           = 0;
    qint64        captureWallNs = 0;

    FrameHolder() = default;
    FrameHolder(const FrameHolder&) = delete;
    FrameHolder& operator=(const FrameHolder&) = delete;

    ~FrameHolder() {
        if (surface) {
            NvBufSurfaceUnMapEglImage(surface, 0);
        }
        if (buffer) {
            gst_buffer_unmap(buffer, &mapInfo);
        }
        if (sample) {
            gst_sample_unref(sample);
        }
    }
};

Q_DECLARE_METATYPE(FrameHolder*)

// Note: named "GstSourcePipeline" rather than "GstPipeline" because
// GStreamer already exports `GstPipeline` as a struct typedef in
// <gst/gstpipeline.h>; reusing the name causes a typedef/class conflict.
class GstSourcePipeline : public QObject {
    Q_OBJECT
public:
    explicit GstSourcePipeline(std::string pipelineDesc, QObject* parent = nullptr);
    ~GstSourcePipeline() override;

    bool start();
    void stop();

signals:
    // Delivered to the GUI thread via Qt::QueuedConnection.
    // Receiver takes ownership of the holder.
    void newFrame(FrameHolder* holder);
    void pipelineError(QString message);

private:
    static GstFlowReturn onNewSampleCb(GstAppSink* sink, gpointer user_data);
    static gboolean      onBusMessageCb(GstBus* bus, GstMessage* msg, gpointer user_data);

    std::string m_desc;
    GstElement* m_pipeline    = nullptr;
    GstElement* m_appsink     = nullptr;
    guint       m_busWatchId  = 0;
};
