#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

extern "C" {
#include <nvbufsurface.h>
}

#include <atomic>
#include <string>

// One NVMM/CUDA-device frame, owned by whoever consumes the newFrame signal.
// Same shape as the P0.1/P0.4 holder. Duplicated on purpose — the architecture
// rule is "no shared code between components in early phases".
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
    qint64        captureWallNs = 0;

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

// Two pipeline shapes per source. The "selective" part of P0.5 lives in
// being able to choose at startup, and to flip at runtime, which streams
// pay the inference compute cost.
//
// Mode::Raw (same as P0.4):
//   nvurisrcbin -> nvvideoconvert -> RGBA NVMM -> appsink
//
// Mode::Infer (this component's payload):
//   nvurisrcbin -> nvstreammux(batch=1) -> nvinfer -> nvvideoconvert
//        -> RGBA NVMM -> nvdsosd -> appsink
//
// Toggling is implemented by tearing the pipeline down and rebuilding it in
// the requested shape. We deliberately avoid runtime element relinking — the
// stop/rebuild path is the same one nvurisrcbin already uses to recover from
// disconnects, so we pay no new failure modes for it.
class RtspInferSource : public QObject {
    Q_OBJECT
public:
    enum class Mode { Raw, Infer };

    struct Options {
        QString uri;
        // Initial mode the source comes up in.
        Mode    mode                    = Mode::Raw;
        // Path to the nvinfer config file used in Mode::Infer. Absolute path
        // strongly recommended — nvinfer resolves relative paths from the
        // process CWD, which inside the container is /workspace.
        QString inferConfigPath;
        // nvurisrcbin properties. Same defaults as P0.2/P0.4.
        int     latencyMs               = 0;
        bool    dropOnLatency           = true;
        int     rtspReconnectIntervalS  = 5;
        int     rtspReconnectAttempts   = 0;
        QString rtpProtocol             = "tcp";
        // Stall detector.
        int     stallTimeoutMs          = 2000;
        // nvstreammux output dimensions. The mux insists on a fixed output
        // resolution; downstream nvvideoconvert handles the rescale to the
        // widget. Keep this generous enough to preserve the source resolution
        // for typical 1080p / 4K cameras.
        int     muxWidth                = 1920;
        int     muxHeight               = 1080;
    };

    explicit RtspInferSource(Options opts, QObject* parent = nullptr);
    ~RtspInferSource() override;

    bool start();
    void stop();

    // Runtime toggle. Internally calls stop() then start() with the new mode.
    // Safe to call from the GUI thread.
    bool setMode(Mode newMode);
    Mode currentMode() const { return m_opts.mode; }

    qint64  lastFrameAgeMs() const;
    int     reconnectCount() const { return m_reconnectCount.load(); }
    int     stallEventCount() const { return m_stallStartCount.load(); }
    bool    stalledNow() const { return m_stalled.load(); }
    quint64 totalFramesSeen() const { return m_totalFrames.load(); }

signals:
    void newFrame(FrameHolder* holder);
    void streamLive();
    void streamStalled(qint64 stalledForMs);
    void pipelineError(QString message);
    void modeChanged(int mode);

public slots:
    void pollHealth();

private:
    static GstFlowReturn onNewSampleCb(GstAppSink* sink, gpointer user_data);
    static gboolean      onBusMessageCb(GstBus* bus, GstMessage* msg, gpointer user_data);
    static void          onPadAddedCb(GstElement* src, GstPad* new_pad, gpointer user_data);

    GstFlowReturn handleNewSample(GstAppSink* sink);
    void          linkUriPadToNext(GstPad* new_pad);
    void          handleBusError(GstMessage* msg);

    bool buildRawPipeline();
    bool buildInferPipeline();
    void resetCounters();

    Options m_opts;

    GstElement* m_pipeline    = nullptr;
    GstElement* m_uriSrcBin   = nullptr;
    GstElement* m_streammux   = nullptr;       // Infer mode only
    GstElement* m_nvinfer     = nullptr;       // Infer mode only
    GstElement* m_convert     = nullptr;
    GstElement* m_capsfilter  = nullptr;
    GstElement* m_osd         = nullptr;       // Infer mode only
    GstElement* m_appsink     = nullptr;
    guint       m_busWatchId  = 0;

    std::atomic<qint64>  m_lastFrameWallMs{0};
    std::atomic<bool>    m_stalled{true};
    std::atomic<bool>    m_hadFirstLiveFrame{false};
    std::atomic<int>     m_reconnectCount{0};
    std::atomic<int>     m_stallStartCount{0};
    std::atomic<quint64> m_totalFrames{0};
};
