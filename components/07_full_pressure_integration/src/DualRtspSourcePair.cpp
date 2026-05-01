#include "DualRtspSourcePair.hpp"

#include <QDebug>

#include <algorithm>

DualRtspSourcePair::DualRtspSourcePair(Options opts, QObject* parent)
    : QObject(parent), m_opts(std::move(opts)), m_renderer(this) {
    m_top = new RtspSource(m_opts.top, this);
    m_bottom = new RtspSource(m_opts.bottom, this);
    m_renderer.setCalibration(m_opts.calibration);

    connect(m_top, &RtspSource::newFrame,
            this, &DualRtspSourcePair::onTopFrame,
            Qt::QueuedConnection);
    connect(m_bottom, &RtspSource::newFrame,
            this, &DualRtspSourcePair::onBottomFrame,
            Qt::QueuedConnection);

    connect(m_top, &RtspSource::streamLive, this, &DualRtspSourcePair::topLive);
    connect(m_bottom, &RtspSource::streamLive, this, &DualRtspSourcePair::bottomLive);
    connect(m_top, &RtspSource::streamStalled, this, &DualRtspSourcePair::topStalled);
    connect(m_bottom, &RtspSource::streamStalled, this, &DualRtspSourcePair::bottomStalled);

    connect(m_top, &RtspSource::pipelineError, this, [this](const QString& msg) {
        m_lastError = QStringLiteral("top: ") + msg;
        emit pipelineError(m_lastError);
    });
    connect(m_bottom, &RtspSource::pipelineError, this, [this](const QString& msg) {
        m_lastError = QStringLiteral("bottom: ") + msg;
        emit pipelineError(m_lastError);
    });
}

DualRtspSourcePair::~DualRtspSourcePair() {
    stop();
}

bool DualRtspSourcePair::start() {
    m_lastError.clear();
    if (!m_renderer.setCalibration(m_opts.calibration, &m_lastError)) {
        return false;
    }
    if (!m_top->start()) {
        m_lastError = QStringLiteral("top start failed");
        return false;
    }
    if (!m_bottom->start()) {
        m_top->stop();
        m_lastError = QStringLiteral("bottom start failed");
        return false;
    }
    m_running = true;
    return true;
}

void DualRtspSourcePair::stop() {
    m_running = false;
    if (m_top) m_top->stop();
    if (m_bottom) m_bottom->stop();
    delete m_topFrame;
    delete m_bottomFrame;
    m_topFrame = nullptr;
    m_bottomFrame = nullptr;
}

void DualRtspSourcePair::pollHealth() {
    if (!m_running) return;
    m_top->pollHealth();
    m_bottom->pollHealth();
    if (m_top->stalledNow() || m_bottom->stalledNow()) {
        if (!m_degraded) {
            m_degraded = true;
            const QString reason = QString("source stale top=%1 bottom=%2")
                .arg(m_top->stalledNow() ? "STALL" : "LIVE")
                .arg(m_bottom->stalledNow() ? "STALL" : "LIVE");
            emit pairDegraded(reason);
        }
    } else if (m_degraded) {
        m_degraded = false;
        emit pairRecovered();
    }
}

qint64 DualRtspSourcePair::topLastFrameAgeMs() const { return m_top->lastFrameAgeMs(); }
qint64 DualRtspSourcePair::bottomLastFrameAgeMs() const { return m_bottom->lastFrameAgeMs(); }
int DualRtspSourcePair::topReconnectCount() const { return m_top->reconnectCount(); }
int DualRtspSourcePair::bottomReconnectCount() const { return m_bottom->reconnectCount(); }
int DualRtspSourcePair::topStallEventCount() const { return m_top->stallEventCount(); }
int DualRtspSourcePair::bottomStallEventCount() const { return m_bottom->stallEventCount(); }
bool DualRtspSourcePair::topStalledNow() const { return m_top->stalledNow(); }
bool DualRtspSourcePair::bottomStalledNow() const { return m_bottom->stalledNow(); }
qint64 DualRtspSourcePair::lastPairDeltaMs() const { return m_renderer.lastPairDeltaMs(); }
quint64 DualRtspSourcePair::stitchedFrameCount() const { return m_renderer.stitchedFrameCount(); }
bool DualRtspSourcePair::degradedNow() const { return m_degraded; }
QString DualRtspSourcePair::modeTag() const { return m_renderer.currentModeTag(); }
QString DualRtspSourcePair::lastError() const {
    return !m_lastError.isEmpty() ? m_lastError : m_renderer.lastError();
}

void DualRtspSourcePair::onTopFrame(FrameHolder* frame) {
    delete m_topFrame;
    m_topFrame = frame;
    maybeStitch();
}

void DualRtspSourcePair::onBottomFrame(FrameHolder* frame) {
    delete m_bottomFrame;
    m_bottomFrame = frame;
    maybeStitch();
}

void DualRtspSourcePair::maybeStitch() {
    if (!m_topFrame || !m_bottomFrame) return;

    const qint64 pairDeltaMs = qAbs(m_topFrame->captureWallNs - m_bottomFrame->captureWallNs) / 1000000LL;
    if (pairDeltaMs > m_opts.maxPairDeltaMs) {
        if (!m_degraded) {
            m_degraded = true;
            emit pairDegraded(QString("pair delta too large: %1ms").arg(pairDeltaMs));
        }
        return;
    }

    StitchedFrame* stitched = m_renderer.stitch(*m_topFrame, *m_bottomFrame);
    if (!stitched) {
        m_lastError = m_renderer.lastError();
        emit pipelineError(m_lastError);
        return;
    }

    if (m_degraded && !m_top->stalledNow() && !m_bottom->stalledNow()) {
        m_degraded = false;
        emit pairRecovered();
    }

    delete m_topFrame;
    delete m_bottomFrame;
    m_topFrame = nullptr;
    m_bottomFrame = nullptr;

    emit newStitchedFrame(stitched);
}
