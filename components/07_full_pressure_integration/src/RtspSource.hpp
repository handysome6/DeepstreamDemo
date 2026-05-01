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

// Build:
//   nvurisrcbin uri=<URI> ! nvvideoconvert ! video/x-raw(memory:NVMM),format=RGBA ! appsink
// programmatically, because nvurisrcbin exposes its src pad dynamically (it
// has to wait until it has actually opened the URI and figured out the
// codec). Linking has to happen in a pad-added callback.
//
// This class also exists to track health that the user-visible success
// criteria depend on: time-since-last-frame (stall detection) and number of
// observed reconnects.
class RtspSource : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString uri;
        // nvurisrcbin properties. Defaults follow the P0.2 target: minimize
        // latency for live RTSP, drop late packets rather than letting the
        // jitterbuffer grow, and never give up on reconnect.
        int     latencyMs               = 0;
        bool    dropOnLatency           = true;
        int     rtspReconnectIntervalS  = 5;       // 0 disables; we never want that here
        int     rtspReconnectAttempts   = 0;       // 0 = retry forever
        QString rtpProtocol             = "tcp";   // "tcp" or "udp"
        // Stall detector: if no frame arrives in this many ms we declare the
        // stream stalled. Recovery from stall after the first successful live
        // frame counts as one reconnect event.
        int     stallTimeoutMs          = 2000;
    };

    explicit RtspSource(Options opts, QObject* parent = nullptr);
    ~RtspSource() override;

    bool start();
    void stop();

    // Snapshots of internal counters, safe to call from the GUI thread.
    qint64 lastFrameAgeMs() const;
    int    reconnectCount() const { return m_reconnectCount.load(); }
    int    stallEventCount() const { return m_stallStartCount.load(); }
    bool   stalledNow() const { return m_stalled.load(); }
    quint64 totalFramesSeen() const { return m_totalFrames.load(); }

signals:
    // Delivered to the GUI thread via Qt::QueuedConnection.
    // Receiver takes ownership of the holder.
    void newFrame(FrameHolder* holder);

    // Status transitions, useful for UI overlays / stdout summaries.
    void streamLive();
    void streamStalled(qint64 stalledForMs);
    void pipelineError(QString message);

public slots:
    // Called periodically from a QTimer in the GUI thread to flip
    // live <-> stalled state when frames stop arriving.
    void pollHealth();

private:
    static GstFlowReturn onNewSampleCb(GstAppSink* sink, gpointer user_data);
    static gboolean      onBusMessageCb(GstBus* bus, GstMessage* msg, gpointer user_data);
    static void          onPadAddedCb(GstElement* src, GstPad* new_pad, gpointer user_data);

    GstFlowReturn handleNewSample(GstAppSink* sink);
    void          linkUriPadToConvert(GstPad* new_pad);
    void          handleBusError(GstMessage* msg);

    Options     m_opts;

    GstElement* m_pipeline   = nullptr;
    GstElement* m_uriSrcBin  = nullptr;
    GstElement* m_convert    = nullptr;
    GstElement* m_capsfilter = nullptr;
    GstElement* m_appsink    = nullptr;
    guint       m_busWatchId = 0;

    // Health-tracking, written from the streaming thread, read from the GUI
    // thread. std::atomic so we don't need a mutex for the simple counters.
    std::atomic<qint64>  m_lastFrameWallMs{0};
    std::atomic<bool>    m_stalled{true};   // start in "stalled" until first frame arrives
    std::atomic<bool>    m_hadFirstLiveFrame{false};
    std::atomic<int>     m_reconnectCount{0};
    std::atomic<int>     m_stallStartCount{0};
    std::atomic<quint64> m_totalFrames{0};
};
