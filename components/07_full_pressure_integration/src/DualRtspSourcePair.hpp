#pragma once

#include <QObject>
#include <QString>

#include <memory>

#include "RtspSource.hpp"
#include "StitchFrame.hpp"
#include "StitchRenderer.hpp"

class DualRtspSourcePair : public QObject {
    Q_OBJECT
public:
    struct Options {
        RtspSource::Options top;
        RtspSource::Options bottom;
        StitchRenderer::Calibration calibration;
        qint64 maxPairDeltaMs = 250;
    };

    explicit DualRtspSourcePair(Options opts, QObject* parent = nullptr);
    ~DualRtspSourcePair() override;

    bool start();
    void stop();
    void pollHealth();

    qint64 topLastFrameAgeMs() const;
    qint64 bottomLastFrameAgeMs() const;
    int topReconnectCount() const;
    int bottomReconnectCount() const;
    int topStallEventCount() const;
    int bottomStallEventCount() const;
    bool topStalledNow() const;
    bool bottomStalledNow() const;
    qint64 lastPairDeltaMs() const;
    quint64 stitchedFrameCount() const;
    bool degradedNow() const;
    QString modeTag() const;
    QString lastError() const;

signals:
    void newStitchedFrame(StitchedFrame* frame);
    void topLive();
    void bottomLive();
    void topStalled(qint64 ageMs);
    void bottomStalled(qint64 ageMs);
    void pairDegraded(QString reason);
    void pairRecovered();
    void pipelineError(QString message);

private slots:
    void onTopFrame(FrameHolder* frame);
    void onBottomFrame(FrameHolder* frame);

private:
    void maybeStitch();

    Options m_opts;
    RtspSource* m_top = nullptr;
    RtspSource* m_bottom = nullptr;
    StitchRenderer m_renderer;
    FrameHolder* m_topFrame = nullptr;
    FrameHolder* m_bottomFrame = nullptr;
    bool m_running = false;
    bool m_degraded = false;
    QString m_lastError;
};
