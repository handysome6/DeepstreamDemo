#include "StitchCuda.hpp"

#include "StitchFrame.hpp"

#include <nvbufsurface.h>

namespace {

__device__ float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

__device__ float2 applyTransform(const StitchTransform& tx, float x, float y) {
    const float sx = tx.m[0] * x + tx.m[1] * y + tx.m[2];
    const float sy = tx.m[3] * x + tx.m[4] * y + tx.m[5];
    const float sw = tx.m[6] * x + tx.m[7] * y + tx.m[8];
    if (fabsf(sw) < 1e-6f) {
        return make_float2(-1.0f, -1.0f);
    }
    return make_float2(sx / sw, sy / sw);
}

__device__ uchar4 sampleNearest(const uchar4* src,
                                size_t pitchPixels,
                                int width,
                                int height,
                                float sx,
                                float sy,
                                bool* valid) {
    const int ix = __float2int_rn(sx);
    const int iy = __float2int_rn(sy);
    if (ix < 0 || iy < 0 || ix >= width || iy >= height) {
        *valid = false;
        return uchar4{0, 0, 0, 255};
    }
    *valid = true;
    return src[iy * pitchPixels + ix];
}

__device__ uchar4 blendPixels(const uchar4& a, const uchar4& b, float alpha) {
    alpha = clampf(alpha, 0.0f, 1.0f);
    uchar4 out{};
    out.x = static_cast<unsigned char>((1.0f - alpha) * a.x + alpha * b.x);
    out.y = static_cast<unsigned char>((1.0f - alpha) * a.y + alpha * b.y);
    out.z = static_cast<unsigned char>((1.0f - alpha) * a.z + alpha * b.z);
    out.w = static_cast<unsigned char>((1.0f - alpha) * a.w + alpha * b.w);
    return out;
}

__global__ void stitchKernel(uchar4* dst,
                             size_t dstPitchPixels,
                             const uchar4* top,
                             size_t topPitchPixels,
                             const uchar4* bottom,
                             size_t bottomPitchPixels,
                             int width,
                             int topHeight,
                             int bottomHeight,
                             int overlapPx,
                             bool enableBlend,
                             bool* usedBlend) {
    const int x = int(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = int(blockIdx.y * blockDim.y + threadIdx.y);
    const int outHeight = topHeight + bottomHeight - overlapPx;
    if (x >= width || y >= outHeight) return;

    uchar4 out{};
    if (y < topHeight - overlapPx) {
        out = top[y * topPitchPixels + x];
    } else if (y >= topHeight) {
        const int by = y - (topHeight - overlapPx);
        out = bottom[by * bottomPitchPixels + x];
    } else {
        const int seamY = y - (topHeight - overlapPx);
        const int blendHeight = overlapPx > 0 ? overlapPx : 1;
        const uchar4 a = top[y * topPitchPixels + x];
        const uchar4 b = bottom[seamY * bottomPitchPixels + x];
        if (enableBlend && overlapPx > 0) {
            const float alpha = float(seamY) / float(blendHeight);
            out = blendPixels(a, b, alpha);
            if (usedBlend) *usedBlend = true;
        } else {
            out = a;
        }
    }

    dst[y * dstPitchPixels + x] = out;
}

__global__ void stitchCalibratedKernel(uchar4* dst,
                                       size_t dstPitchPixels,
                                       const uchar4* top,
                                       size_t topPitchPixels,
                                       const uchar4* bottom,
                                       size_t bottomPitchPixels,
                                       int outWidth,
                                       int outHeight,
                                       int topWidth,
                                       int topHeight,
                                       int bottomWidth,
                                       int bottomHeight,
                                       StitchTransform topTx,
                                       StitchTransform bottomTx,
                                       int overlapPx,
                                       bool enableBlend,
                                       bool* usedBlend) {
    const int x = int(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = int(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= outWidth || y >= outHeight) return;

    const float2 topUv = applyTransform(topTx, float(x), float(y));
    const float2 bottomUv = applyTransform(bottomTx, float(x), float(y));

    bool topValid = false;
    bool bottomValid = false;
    const uchar4 topPx = sampleNearest(top, topPitchPixels, topWidth, topHeight, topUv.x, topUv.y, &topValid);
    const uchar4 bottomPx = sampleNearest(bottom, bottomPitchPixels, bottomWidth, bottomHeight, bottomUv.x, bottomUv.y, &bottomValid);

    uchar4 out{0, 0, 0, 255};
    if (topValid && bottomValid) {
        if (enableBlend && overlapPx > 0) {
            const float seamStart = float(topHeight - overlapPx);
            const float alpha = (float(y) - seamStart) / float(overlapPx > 0 ? overlapPx : 1);
            out = blendPixels(topPx, bottomPx, alpha);
            if (usedBlend) *usedBlend = true;
        } else {
            out = topPx;
        }
    } else if (topValid) {
        out = topPx;
    } else if (bottomValid) {
        out = bottomPx;
    }

    dst[y * dstPitchPixels + x] = out;
}

}  // namespace

void destroyWrappedCudaSurface(NvBufSurface* surface) {
    if (!surface) return;
    if (surface->surfaceList) {
        if (surface->surfaceList[0].dataPtr) {
            cudaFree(surface->surfaceList[0].dataPtr);
            surface->surfaceList[0].dataPtr = nullptr;
        }
        free(surface->surfaceList);
        surface->surfaceList = nullptr;
    }
    free(surface);
}

NvBufSurface* createWrappedCudaSurface(uint32_t width,
                                       uint32_t height,
                                       uint32_t gpuId,
                                       uint32_t* pitchBytes) {
    cudaSetDevice(int(gpuId));

    void* data = nullptr;
    size_t pitch = 0;
    if (cudaMallocPitch(&data, &pitch, size_t(width) * 4u, height) != cudaSuccess) {
        return nullptr;
    }

    auto* surface = static_cast<NvBufSurface*>(calloc(1, sizeof(NvBufSurface)));
    auto* params = static_cast<NvBufSurfaceParams*>(calloc(1, sizeof(NvBufSurfaceParams)));
    if (!surface || !params) {
        if (data) cudaFree(data);
        free(surface);
        free(params);
        return nullptr;
    }

    surface->gpuId = gpuId;
    surface->batchSize = 1;
    surface->numFilled = 1;
    surface->isContiguous = true;
    surface->memType = NVBUF_MEM_CUDA_DEVICE;
    surface->surfaceList = params;
    surface->isImportedBuf = false;

    params[0].width = width;
    params[0].height = height;
    params[0].pitch = uint32_t(pitch);
    params[0].colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    params[0].layout = NVBUF_LAYOUT_PITCH;
    params[0].bufferDesc = 0;
    params[0].dataSize = uint32_t(pitch * height);
    params[0].dataPtr = data;
    params[0].planeParams.num_planes = 1;
    params[0].planeParams.width[0] = width;
    params[0].planeParams.height[0] = height;
    params[0].planeParams.pitch[0] = uint32_t(pitch);
    params[0].planeParams.offset[0] = 0;
    params[0].planeParams.bytesPerPix[0] = 4;
    params[0].planeParams.psize[0] = uint32_t(pitch * height);

    if (pitchBytes) *pitchBytes = uint32_t(pitch);
    return surface;
}

StitchedFrame* cloneStitchedFrame(const StitchedFrame& src) {
    if (!src.surface || src.surface->numFilled == 0 || !src.surface->surfaceList[0].dataPtr) {
        return nullptr;
    }

    uint32_t pitch = 0;
    NvBufSurface* out = createWrappedCudaSurface(src.width, src.height, src.surface->gpuId, &pitch);
    if (!out) {
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
        destroyWrappedCudaSurface(out);
        return nullptr;
    }

    auto* copy = new StitchedFrame;
    copy->surface = out;
    copy->width = src.width;
    copy->height = src.height;
    copy->pitch = pitch;
    copy->memType = int(out->memType);
    copy->colorFormat = int(d.colorFormat);
    copy->layout = int(d.layout);
    copy->pts = src.pts;
    copy->captureWallNs = src.captureWallNs;
    return copy;
}

cudaError_t stitchIdentityTopBottom(NvBufSurface* dst,
                                    const NvBufSurface* top,
                                    const NvBufSurface* bottom,
                                    cudaStream_t stream,
                                    int overlapPx,
                                    bool enableBlend,
                                    bool* usedBlend) {
    if (!dst || !top || !bottom) return cudaErrorInvalidValue;
    if (dst->numFilled == 0 || top->numFilled == 0 || bottom->numFilled == 0) {
        return cudaErrorInvalidValue;
    }

    auto& d = dst->surfaceList[0];
    const auto& a = top->surfaceList[0];
    const auto& b = bottom->surfaceList[0];
    if (!d.dataPtr || !a.dataPtr || !b.dataPtr) return cudaErrorInvalidDevicePointer;

    cudaError_t err = cudaSuccess;
    if (usedBlend) {
        err = cudaMemsetAsync(usedBlend, 0, sizeof(bool), stream);
        if (err != cudaSuccess) return err;
    }

    if (!enableBlend && overlapPx == 0) {
        err = cudaMemcpy2DAsync(d.dataPtr,
                                d.pitch,
                                a.dataPtr,
                                a.pitch,
                                size_t(a.width) * 4u,
                                a.height,
                                cudaMemcpyDeviceToDevice,
                                stream);
        if (err != cudaSuccess) return err;

        auto* dstBottom = static_cast<unsigned char*>(d.dataPtr) + size_t(a.height) * d.pitch;
        err = cudaMemcpy2DAsync(dstBottom,
                                d.pitch,
                                b.dataPtr,
                                b.pitch,
                                size_t(b.width) * 4u,
                                b.height,
                                cudaMemcpyDeviceToDevice,
                                stream);
        return err;
    }

    const dim3 block(16, 16);
    const int outHeight = a.height + b.height - overlapPx;
    const dim3 grid((a.width + block.x - 1) / block.x,
                    (outHeight + block.y - 1) / block.y);

    stitchKernel<<<grid, block, 0, stream>>>(
        reinterpret_cast<uchar4*>(d.dataPtr),
        d.pitch / sizeof(uchar4),
        reinterpret_cast<const uchar4*>(a.dataPtr),
        a.pitch / sizeof(uchar4),
        reinterpret_cast<const uchar4*>(b.dataPtr),
        b.pitch / sizeof(uchar4),
        a.width,
        a.height,
        b.height,
        overlapPx,
        enableBlend,
        usedBlend);

    return cudaGetLastError();
}

cudaError_t stitchCalibratedTopBottom(NvBufSurface* dst,
                                      const NvBufSurface* top,
                                      const NvBufSurface* bottom,
                                      cudaStream_t stream,
                                      const StitchTransform& topSourceFromOutput,
                                      const StitchTransform& bottomSourceFromOutput,
                                      int overlapPx,
                                      bool enableBlend,
                                      bool* usedBlend) {
    if (!dst || !top || !bottom) return cudaErrorInvalidValue;
    if (dst->numFilled == 0 || top->numFilled == 0 || bottom->numFilled == 0) {
        return cudaErrorInvalidValue;
    }

    auto& d = dst->surfaceList[0];
    const auto& a = top->surfaceList[0];
    const auto& b = bottom->surfaceList[0];
    if (!d.dataPtr || !a.dataPtr || !b.dataPtr) return cudaErrorInvalidDevicePointer;

    cudaError_t err = cudaSuccess;
    if (usedBlend) {
        err = cudaMemsetAsync(usedBlend, 0, sizeof(bool), stream);
        if (err != cudaSuccess) return err;
    }

    const dim3 block(16, 16);
    const dim3 grid((d.width + block.x - 1) / block.x,
                    (d.height + block.y - 1) / block.y);

    stitchCalibratedKernel<<<grid, block, 0, stream>>>(
        reinterpret_cast<uchar4*>(d.dataPtr),
        d.pitch / sizeof(uchar4),
        reinterpret_cast<const uchar4*>(a.dataPtr),
        a.pitch / sizeof(uchar4),
        reinterpret_cast<const uchar4*>(b.dataPtr),
        b.pitch / sizeof(uchar4),
        d.width,
        d.height,
        a.width,
        a.height,
        b.width,
        b.height,
        topSourceFromOutput,
        bottomSourceFromOutput,
        overlapPx,
        enableBlend,
        usedBlend);

    return cudaGetLastError();
}
