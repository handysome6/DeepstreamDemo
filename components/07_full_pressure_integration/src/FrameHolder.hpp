#pragma once

#include <QObject>
#include <QtGlobal>

#include <gst/gst.h>

extern "C" {
#include <nvbufsurface.h>
}

#include <cstdint>

// One NVMM/CUDA-device frame, owned by whoever consumes the newFrame signal.
//
// In components 04/05/06 each source class redeclared this struct in its own
// header on purpose — the architecture rule forbids cross-component sharing
// in early phases. Inside this single component (07) we have three source
// classes (RtspSource, RtspInferSource, plus the RtspSource consumed by the
// stitch pair) all emitting the same holder, so we extract it once here to
// avoid multiple-definition collisions when their headers are included
// together by the integration main.cpp.
struct FrameHolder {
    GstSample*    sample        = nullptr;
    GstBuffer*    buffer        = nullptr;
    GstMapInfo    mapInfo{};
    NvBufSurface* surface       = nullptr;
    uint32_t      width         = 0;
    uint32_t      height        = 0;
    uint32_t      pitch         = 0;
    int           memType       = 0;
    int           colorFormat   = 0;
    int           layout        = 0;
    guint64       pts           = 0;
    qint64        captureWallNs = 0;

    FrameHolder() = default;
    FrameHolder(const FrameHolder&) = delete;
    FrameHolder& operator=(const FrameHolder&) = delete;

    ~FrameHolder() {
        if (buffer) {
            gst_buffer_unmap(buffer, &mapInfo);
        }
        if (sample) {
            gst_sample_unref(sample);
        }
    }
};

Q_DECLARE_METATYPE(FrameHolder*)
