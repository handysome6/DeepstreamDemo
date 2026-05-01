#pragma once

#include <QObject>
#include <QString>

#include <cuda_runtime.h>

#include <memory>

#include "RtspSource.hpp"
#include "StitchCuda.hpp"
#include "StitchFrame.hpp"

class StitchRenderer : public QObject {
    Q_OBJECT
public:
    struct Calibration {
        QString mode = "identity";
        int outputWidth = 3840;
        int outputHeight = 4320;
        int overlapPx = 0;
        bool enableBlend = false;
        StitchTransform topSourceFromOutput;
        StitchTransform bottomSourceFromOutput;
    };

    explicit StitchRenderer(QObject* parent = nullptr);
    ~StitchRenderer() override;

    bool setCalibration(const Calibration& calibration, QString* error = nullptr);
    QString currentModeTag() const;
    qint64 lastPairDeltaMs() const { return m_lastPairDeltaMs; }
    quint64 stitchedFrameCount() const { return m_stitchedFrameCount; }
    bool degradedNow() const { return m_degraded; }
    QString lastError() const { return m_lastError; }

    StitchedFrame* stitch(const FrameHolder& top, const FrameHolder& bottom);

signals:
    void proofLine(QString text);

private:
    bool ensureScratch();

    Calibration m_calibration;
    cudaStream_t m_stream = nullptr;
    bool* m_deviceUsedBlend = nullptr;
    bool m_loggedProof = false;
    qint64 m_lastPairDeltaMs = -1;
    quint64 m_stitchedFrameCount = 0;
    bool m_degraded = false;
    QString m_lastError;
};
