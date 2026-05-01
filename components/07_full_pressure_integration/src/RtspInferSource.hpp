#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <atomic>
#include <string>

#include "FrameHolder.hpp"

// Always-Infer RTSP source. P0.5 supported a Mode::Raw / Mode::Infer flip;
// P0.7 puts inference under steady full-pressure load, so the runtime flip
// path is dropped here. The pipeline shape is fixed at construction:
//
//   nvurisrcbin -> nvstreammux(batch=1) -> nvinfer -> nvvideoconvert
//        -> RGBA NVMM -> nvdsosd -> appsink
class RtspInferSource : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString uri;
        // Path to the nvinfer config file. Absolute path strongly recommended
        // — nvinfer resolves relative paths from the process CWD, which
        // inside the container is /workspace.
        QString inferConfigPath;
        // nvurisrcbin properties. Same defaults as P0.2/P0.4/P0.5.
        int     latencyMs               = 0;
        bool    dropOnLatency           = true;
        int     rtspReconnectIntervalS  = 5;
        int     rtspReconnectAttempts   = 0;
        QString rtpProtocol             = "tcp";
        // Stall detector.
        int     stallTimeoutMs          = 2000;
        // nvstreammux output dimensions. P0.7 leans on the fact that the
        // appsink frame inherits the mux output dim — set this to 3840x2160
        // for a 4K display panel and let nvinfer's network-input-shape
        // (e.g. 640x640) handle the inference downsample internally.
        int     muxWidth                = 1920;
        int     muxHeight               = 1080;
    };

    explicit RtspInferSource(Options opts, QObject* parent = nullptr);
    ~RtspInferSource() override;

    bool start();
    void stop();

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

public slots:
    void pollHealth();

private:
    static GstFlowReturn onNewSampleCb(GstAppSink* sink, gpointer user_data);
    static gboolean      onBusMessageCb(GstBus* bus, GstMessage* msg, gpointer user_data);
    static void          onPadAddedCb(GstElement* src, GstPad* new_pad, gpointer user_data);

    GstFlowReturn handleNewSample(GstAppSink* sink);
    void          linkUriPadToNext(GstPad* new_pad);
    void          handleBusError(GstMessage* msg);

    bool buildInferPipeline();

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
