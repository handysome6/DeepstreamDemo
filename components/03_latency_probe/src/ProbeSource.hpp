#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

extern "C" {
#include <nvbufsurface.h>
}

#include <atomic>

// One NVMM/CUDA-device frame plus the two timestamps we have stamped on it
// so far. The widget will fill in t2_us after glFinish in paintGL.
//
// Duplicated from 02 by design — the architecture rule is "no shared code
// across components in early phases". Keeping it independent also lets us add
// the t0/t1 fields without touching 02.
struct FrameHolder {
    GstSample*    sample        = nullptr;
    GstBuffer*    buffer        = nullptr;
    GstMapInfo    mapInfo{};
    NvBufSurface* surface       = nullptr;
    uint32_t      width         = 0;
    uint32_t      height        = 0;
    uint32_t      pitch         = 0;
    int           memType       = 0;
    int           colorFormat   = 0;
    int           layout        = 0;
    guint64       pts           = 0;

    // Probe timestamps, all CLOCK_MONOTONIC microseconds.
    // t0 = stamped on the buffer at the head-of-pipeline pad probe.
    // t1 = sampled in the appsink callback (decode/queue boundary).
    // t2 = sampled by the widget after glFinish in paintGL.
    qint64        t0_us         = 0;
    qint64        t1_us         = 0;

    FrameHolder() = default;
    FrameHolder(const FrameHolder&) = delete;
    FrameHolder& operator=(const FrameHolder&) = delete;

    ~FrameHolder() {
        if (buffer) {
            gst_buffer_unmap(buffer, &mapInfo);
        }
        if (sample) {
            gst_sample_unref(sample);
        }
    }
};

Q_DECLARE_METATYPE(FrameHolder*)

// Builds one of two pipelines, selected by Options::sourceMode:
//
//   videotestsrc:
//     videotestsrc is-live=true pattern=ball !
//     video/x-raw,width=W,height=H,framerate=F/1 !
//     nvvideoconvert ! video/x-raw(memory:NVMM),format=RGBA ! appsink
//
//   rtsp:
//     nvurisrcbin uri=URI (P0.2 defaults) !
//     nvvideoconvert ! video/x-raw(memory:NVMM),format=RGBA ! appsink
//
// In both cases a buffer pad probe sits at the *head of the section we own*
// (videotestsrc src pad, or nvurisrcbin's dynamic src pad after it appears)
// and stamps each buffer with a GstReferenceTimestampMeta carrying
// CLOCK_MONOTONIC microseconds. The appsink callback reads that meta back as
// t0, samples its own monotonic clock as t1, and ships both on the holder.
class ProbeSource : public QObject {
    Q_OBJECT
public:
    enum class SourceMode { VideoTestSrc, Rtsp };

    struct Options {
        SourceMode sourceMode = SourceMode::VideoTestSrc;

        // videotestsrc-only.
        int  testWidth  = 1920;
        int  testHeight = 1080;
        int  testFps    = 30;

        // rtsp-only — same defaults as P0.2.
        QString uri = "rtsp://127.0.0.1:8554/p02cam";
        int     latencyMs              = 0;
        bool    dropOnLatency          = true;
        int     rtspReconnectIntervalS = 5;
        int     rtspReconnectAttempts  = 0;
        QString rtpProtocol            = "tcp";
    };

    explicit ProbeSource(Options opts, QObject* parent = nullptr);
    ~ProbeSource() override;

    bool start();
    void stop();

    // Counters surfaced for the per-second status line.
    quint64 totalFramesSeen()      const { return m_totalFrames.load(); }
    quint64 framesMissingT0Meta()  const { return m_missingT0.load(); }

signals:
    // Delivered to the GUI thread via Qt::QueuedConnection.
    // Receiver takes ownership of the holder.
    void newFrame(FrameHolder* holder);
    void pipelineError(QString message);

private:
    static GstFlowReturn      onNewSampleCb(GstAppSink* sink, gpointer user_data);
    static gboolean           onBusMessageCb(GstBus* bus, GstMessage* msg, gpointer user_data);
    static void               onPadAddedCb(GstElement* src, GstPad* new_pad, gpointer user_data);
    static GstPadProbeReturn  onSrcBufferProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);

    bool startVideoTestSrc();
    bool startRtsp();
    GstFlowReturn handleNewSample(GstAppSink* sink);
    void          installSrcProbe(GstPad* pad);
    void          handleBusError(GstMessage* msg);

    Options m_opts;

    GstElement* m_pipeline   = nullptr;
    GstElement* m_source     = nullptr;   // videotestsrc or nvurisrcbin
    GstElement* m_convert    = nullptr;
    GstElement* m_capsfilter = nullptr;
    GstElement* m_appsink    = nullptr;
    guint       m_busWatchId = 0;

    // Per-pipeline pad probe ID, so we can remove cleanly on stop().
    GstPad*     m_probedPad  = nullptr;
    gulong      m_probeId    = 0;

    std::atomic<quint64> m_totalFrames{0};
    std::atomic<quint64> m_missingT0{0};
};

// Returns the singleton GstCaps used as the GstReferenceTimestampMeta
// reference key. Same pointer / equal caps for both writer (pad probe) and
// reader (appsink callback). The caps name is intentionally specific so it
// cannot accidentally collide with another component's meta.
GstCaps* probeReferenceCaps();
