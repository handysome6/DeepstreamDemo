#include "StitchRenderer.hpp"

#include <QDebug>

#include <algorithm>

#include "StitchCuda.hpp"

namespace {

bool isValidInput(const FrameHolder& holder) {
    return holder.surface && holder.surface->numFilled > 0 &&
           holder.memType == NVBUF_MEM_CUDA_DEVICE &&
           holder.colorFormat == NVBUF_COLOR_FORMAT_RGBA &&
           holder.layout == NVBUF_LAYOUT_PITCH &&
           holder.width > 0 && holder.height > 0;
}

}  // namespace

StitchRenderer::StitchRenderer(QObject* parent) : QObject(parent) {
    cudaStreamCreate(&m_stream);
    cudaMalloc(&m_deviceUsedBlend, sizeof(bool));
}

StitchRenderer::~StitchRenderer() {
    if (m_deviceUsedBlend) {
        cudaFree(m_deviceUsedBlend);
        m_deviceUsedBlend = nullptr;
    }
    if (m_stream) {
        cudaStreamDestroy(m_stream);
        m_stream = nullptr;
    }
}

bool StitchRenderer::setCalibration(const Calibration& calibration, QString* error) {
    if (calibration.outputWidth <= 0 || calibration.outputHeight <= 0) {
        const QString msg = QStringLiteral("invalid output size");
        if (error) *error = msg;
        m_lastError = msg;
        return false;
    }
    if (calibration.overlapPx < 0) {
        const QString msg = QStringLiteral("overlapPx must be >= 0");
        if (error) *error = msg;
        m_lastError = msg;
        return false;
    }
    const QString mode = calibration.mode.trimmed().toLower();
    if (mode != QStringLiteral("identity") && mode != QStringLiteral("calibrated")) {
        const QString msg = QStringLiteral("calibration mode must be identity or calibrated");
        if (error) *error = msg;
        m_lastError = msg;
        return false;
    }
    m_calibration = calibration;
    m_calibration.mode = mode;
    m_lastError.clear();
    return true;
}

QString StitchRenderer::currentModeTag() const {
    if (m_calibration.mode == QStringLiteral("calibrated")) {
        return m_calibration.enableBlend
            ? QStringLiteral("CALIBRATED+BLEND")
            : QStringLiteral("CALIBRATED");
    }
    return QStringLiteral("IDENTITY");
}

bool StitchRenderer::ensureScratch() {
    return m_stream && m_deviceUsedBlend;
}

StitchedFrame* StitchRenderer::stitch(const FrameHolder& top, const FrameHolder& bottom) {
    m_lastError.clear();
    m_degraded = false;

    if (!ensureScratch()) {
        m_lastError = QStringLiteral("CUDA scratch not ready");
        return nullptr;
    }
    if (!isValidInput(top) || !isValidInput(bottom)) {
        m_lastError = QStringLiteral("input frame contract mismatch");
        return nullptr;
    }
    if (top.width != bottom.width || top.height != bottom.height) {
        m_lastError = QStringLiteral("top/bottom dimensions differ");
        return nullptr;
    }

    const int overlapPx = std::min<int>(m_calibration.overlapPx, int(top.height));
    const int expectedHeight = int(top.height + bottom.height - overlapPx);
    if (m_calibration.outputWidth != int(top.width) || m_calibration.outputHeight != expectedHeight) {
        m_lastError = QStringLiteral("calibration output size does not match input pair");
        return nullptr;
    }

    uint32_t outPitch = 0;
    NvBufSurface* out = createWrappedCudaSurface(top.width, expectedHeight, top.surface->gpuId, &outPitch);
    if (!out || out->numFilled == 0) {
        m_lastError = QString("createWrappedCudaSurface failed gpuId=%1 size=%2x%3")
            .arg(top.surface->gpuId)
            .arg(top.width)
            .arg(expectedHeight);
        if (out) destroyWrappedCudaSurface(out);
        return nullptr;
    }

    cudaSetDevice(int(top.surface->gpuId));
    cudaError_t cudaErr = cudaSuccess;
    if (m_calibration.mode == QStringLiteral("calibrated")) {
        cudaErr = stitchCalibratedTopBottom(out,
                                            top.surface,
                                            bottom.surface,
                                            m_stream,
                                            m_calibration.topSourceFromOutput,
                                            m_calibration.bottomSourceFromOutput,
                                            overlapPx,
                                            m_calibration.enableBlend,
                                            m_deviceUsedBlend);
    } else {
        cudaErr = stitchIdentityTopBottom(out,
                                          top.surface,
                                          bottom.surface,
                                          m_stream,
                                          overlapPx,
                                          m_calibration.enableBlend,
                                          m_deviceUsedBlend);
    }
    if (cudaErr == cudaSuccess) {
        cudaErr = cudaStreamSynchronize(m_stream);
    }
    if (cudaErr != cudaSuccess) {
        destroyWrappedCudaSurface(out);
        m_lastError = QString("CUDA stitch failed: %1").arg(cudaGetErrorString(cudaErr));
        return nullptr;
    }

    auto* stitched = new StitchedFrame;
    stitched->surface = out;
    stitched->width = out->surfaceList[0].width;
    stitched->height = out->surfaceList[0].height;
    stitched->pitch = outPitch;
    stitched->memType = int(out->memType);
    stitched->colorFormat = int(out->surfaceList[0].colorFormat);
    stitched->layout = int(out->surfaceList[0].layout);
    stitched->pts = std::max(top.pts, bottom.pts);
    stitched->captureWallNs = std::max(top.captureWallNs, bottom.captureWallNs);

    m_lastPairDeltaMs = qAbs(top.captureWallNs - bottom.captureWallNs) / 1000000LL;
    ++m_stitchedFrameCount;

    if (!m_loggedProof) {
        m_loggedProof = true;
        qInfo().noquote() << QString(
            "p06 stitch proof top=%1x%2 pitch=%3 mem=%4 | bottom=%5x%6 pitch=%7 mem=%8 | out=%9x%10 pitch=%11 mem=%12 pairDelta=%13ms gpuOnly=true mode=%14 overlap=%15")
            .arg(top.width)
            .arg(top.height)
            .arg(top.pitch)
            .arg(top.memType)
            .arg(bottom.width)
            .arg(bottom.height)
            .arg(bottom.pitch)
            .arg(bottom.memType)
            .arg(stitched->width)
            .arg(stitched->height)
            .arg(stitched->pitch)
            .arg(stitched->memType)
            .arg(m_lastPairDeltaMs)
            .arg(currentModeTag())
            .arg(overlapPx);
    }

    return stitched;
}
