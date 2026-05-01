#pragma once

#include <cuda_runtime.h>

struct NvBufSurface;

struct StitchTransform {
    float m[9] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
    };
};

cudaError_t stitchIdentityTopBottom(NvBufSurface* dst,
                                    const NvBufSurface* top,
                                    const NvBufSurface* bottom,
                                    cudaStream_t stream,
                                    int overlapPx,
                                    bool enableBlend,
                                    bool* usedBlend);

cudaError_t stitchCalibratedTopBottom(NvBufSurface* dst,
                                      const NvBufSurface* top,
                                      const NvBufSurface* bottom,
                                      cudaStream_t stream,
                                      const StitchTransform& topSourceFromOutput,
                                      const StitchTransform& bottomSourceFromOutput,
                                      int overlapPx,
                                      bool enableBlend,
                                      bool* usedBlend);
