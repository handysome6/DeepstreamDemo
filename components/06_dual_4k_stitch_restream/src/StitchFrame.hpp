#pragma once

#include <QtGlobal>

#include <gst/gst.h>

extern "C" {
#include <nvbufsurface.h>
}

void destroyWrappedCudaSurface(NvBufSurface* surface);
NvBufSurface* createWrappedCudaSurface(uint32_t width,
                                       uint32_t height,
                                       uint32_t gpuId,
                                       uint32_t* pitchBytes = nullptr);

struct StitchedFrame {
    NvBufSurface* surface       = nullptr;
    uint32_t      width         = 0;
    uint32_t      height        = 0;
    uint32_t      pitch         = 0;
    int           memType       = 0;
    int           colorFormat   = 0;
    int           layout        = 0;
    guint64       pts           = GST_CLOCK_TIME_NONE;
    qint64        captureWallNs = 0;

    StitchedFrame() = default;
    StitchedFrame(const StitchedFrame&) = delete;
    StitchedFrame& operator=(const StitchedFrame&) = delete;

    ~StitchedFrame() {
        if (surface) {
            destroyWrappedCudaSurface(surface);
            surface = nullptr;
        }
    }
};

StitchedFrame* cloneStitchedFrame(const StitchedFrame& src);

#ifndef __CUDACC__
Q_DECLARE_METATYPE(StitchedFrame*)
#endif
