#include "StitchFrame.hpp"

#include <cuda_runtime.h>

#include <QDebug>

StitchedFrame* cloneStitchedFrame(const StitchedFrame& src) {
    if (!src.surface || src.surface->numFilled == 0 || !src.surface->surfaceList[0].dataPtr) {
        qWarning() << "cloneStitchedFrame: invalid source surface";
        return nullptr;
    }

    NvBufSurfaceCreateParams params{};
    params.gpuId = src.surface->gpuId;
    params.width = src.width;
    params.height = src.height;
    params.size = 0;
    params.isContiguous = false;
    params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    params.layout = NVBUF_LAYOUT_PITCH;
    params.memType = NVBUF_MEM_CUDA_DEVICE;

    NvBufSurface* out = nullptr;
    if (NvBufSurfaceCreate(&out, 1, &params) != 0 || !out || out->numFilled == 0) {
        qWarning() << "cloneStitchedFrame: NvBufSurfaceCreate failed";
        if (out) NvBufSurfaceDestroy(out);
        return nullptr;
    }

    const auto& s = src.surface->surfaceList[0];
    auto& d = out->surfaceList[0];
    const cudaError_t err = cudaMemcpy2D(d.dataPtr,
                                         d.pitch,
                                         s.dataPtr,
                                         s.pitch,
                                         src.width * 4u,
                                         src.height,
                                         cudaMemcpyDeviceToDevice);
    if (err != cudaSuccess) {
        qWarning().noquote() << QString("cloneStitchedFrame cudaMemcpy2D failed: %1")
            .arg(cudaGetErrorString(err));
        NvBufSurfaceDestroy(out);
        return nullptr;
    }

    auto* copy = new StitchedFrame;
    copy->surface = out;
    copy->width = src.width;
    copy->height = src.height;
    copy->pitch = d.pitch;
    copy->memType = int(out->memType);
    copy->colorFormat = int(d.colorFormat);
    copy->layout = int(d.layout);
    copy->pts = src.pts;
    copy->captureWallNs = src.captureWallNs;
    return copy;
}
