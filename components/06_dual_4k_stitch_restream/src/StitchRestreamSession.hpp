#pragma once

#include <QObject>
#include <QString>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "StitchFrame.hpp"

class StitchRestreamSession : public QObject {
    Q_OBJECT
public:
    struct Options {
        bool enabled = true;
        int width = 3840;
        int height = 4320;
        int fpsNum = 15;
        int fpsDen = 1;
        int bitrate = 12000000;
        QString rtspHost = "127.0.0.1";
        int rtspPort = 8554;
        QString mountPoint = "/p06stitch";
    };

    explicit StitchRestreamSession(Options opts, QObject* parent = nullptr);
    ~StitchRestreamSession() override;

    bool start(QString* error = nullptr);
    void stop();
    bool running() const { return m_running; }
    QString rtspUrl() const;
    quint64 pushedFrameCount() const { return m_pushedFrameCount; }
    QString lastError() const { return m_lastError; }

public slots:
    void pushFrame(StitchedFrame* frame);

private:
    static void destroyWrappedSurface(gpointer data);
    static gboolean onBusMessageCb(GstBus* bus, GstMessage* message, gpointer user_data);

    void handleBusError(GstMessage* message);
    void handleBusWarning(GstMessage* message);
    GstCaps* sourceCaps() const;
    GstCaps* encoderCaps() const;

    Options m_opts;
    GstElement* m_pipeline = nullptr;
    GstElement* m_appsrc = nullptr;
    GstElement* m_convert = nullptr;
    GstElement* m_capsfilter = nullptr;
    GstElement* m_encoder = nullptr;
    GstElement* m_parser = nullptr;
    GstElement* m_queue = nullptr;
    GstElement* m_rtspClientSink = nullptr;
    guint m_busWatchId = 0;
    bool m_running = false;
    quint64 m_pushedFrameCount = 0;
    QString m_lastError;
};
