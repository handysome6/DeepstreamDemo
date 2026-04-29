# 002 — dGPU display path pivot: from `EGLImage` import to GPU-only CUDA→GL texture

> **Component:** `01_qt_eglimage_zerocopy`
> **Status:** in progress; window is visibly rendering the synthetic test source,
> but automated black-screen probes need to be reworked for sparse-content frames.
> **Hard takeaway:** once the source changes from a full-frame diagnostic pattern
> to a sparse pattern like `videotestsrc pattern=ball`, corner/center sampling is
> no longer a reliable oracle for “is the image black”.

## Why this document exists

`docs/debug/001 - qt-openglwidget-blackscreen-on-xcb-egl.md` closed the first
layer of failure: `QOpenGLWidget + xcb_egl` on this stack silently lost the
widget's internal FBO during Qt's compose step.

However, once we moved back from the synthetic UV-gradient shader to the real
texture path, we hit a deeper issue: on desktop dGPU, the current
`NvBufSurfaceMapEglImage()` route is the wrong primitive for the surfaces we
actually receive from GStreamer / DeepStream.

This note records the pivot from “strict EGLImage import” to a more realistic
RTX/dGPU goal: **GPU-only display** rather than **strict zero-copy display**.

## What changed in the component

The component was refactored from:

```text
NvBufSurfaceMapEglImage
  -> GL_TEXTURE_EXTERNAL_OES
  -> samplerExternalOES
  -> QOpenGLWidget
```

to:

```text
NvBufSurface (CUDA_DEVICE, RGBA, pitch-linear)
  -> CUDA / GL interop
  -> GL_TEXTURE_2D
  -> sampler2D
  -> QOpenGLWidget (xcb_glx)
```

That means:

- `EGLImageKHR` is no longer the frame contract carried into the widget.
- The widget now owns a regular `GL_TEXTURE_2D`.
- The frame upload path is now GPU→GPU (`cudaMemcpy2DToArray`) instead of the
  old `glEGLImageTargetTexture2DOES` import attempt.
- This path is **not strict zero-copy**, but it is intended to remain
  **GPU-only** (no host round-trip).

## The evidence that forced the pivot

### Surface metadata observed at runtime

The current synthetic pipeline (`videotestsrc ! nvvideoconvert !
video/x-raw(memory:NVMM),format=RGBA ! appsink`) reports:

```text
NvBufSurface: numFilled=1 memType=2 colorFormat=19 layout=0 width=1920 height=1080 pitch=7680 ...
```

Decoded:

- `memType=2` → `NVBUF_MEM_CUDA_DEVICE`
- `colorFormat=19` → `NVBUF_COLOR_FORMAT_RGBA`
- `layout=0` → `NVBUF_LAYOUT_PITCH`

### DeepStream header contract

`nvbufsurface.h` explicitly states:

- `NvBufSurfaceMapEglImage()` supports **only** `NVBUF_MEM_SURFACE_ARRAY`
- `NvBufSurfaceMapCudaBuffer()` also supports **only** `NVBUF_MEM_SURFACE_ARRAY`
- `NvBufSurfaceMapNvmmBuffer()` is the API that supports
  `NVBUF_MEM_CUDA_DEVICE`, `NVBUF_MEM_CUDA_PINNED`, and `NVBUF_MEM_CUDA_ARRAY`

So the earlier assumption — that desktop dGPU `NvBufSurface` frames could be
consumed the same way as a Jetson-style `SURFACE_ARRAY -> EGLImage` flow — was
wrong for the current environment.

## Current runtime behavior

### What is definitely working now

- The application builds with `CUDAToolkit` and `cuda_gl_interop.h`
- The widget opens
- The widget is *visibly* rendering the synthetic source: a white ball bouncing
  around inside the window
- The GL texture registers successfully with CUDA
- The pipeline remains low-latency and stable at ~30 fps in the short run

### Why the old black-screen probe now lies

The previous debug tooling sampled 5 fixed points from the FBO and from the X11
window:

- center
- bottom-left
- bottom-right
- top-left
- top-right

That worked perfectly for the UV-gradient diagnostic because the image filled
all pixels with known values.

It does **not** work for `videotestsrc pattern=ball`, because most of the frame
is black background. The ball occupies only a small moving region, so sampling
5 sparse points can legitimately return `(0,0,0)` even while the image is
correct and visibly animating.

This produced a false conclusion: logs appeared to indicate “still black”, while
human observation showed the bouncing ball correctly on screen.

## Current limitations / unresolved questions

1. **Automated probe mismatch**
   The corner/center probe is no longer a trustworthy signal for sparse test
   patterns. It must be replaced by either:
   - a full-frame checksum / histogram-style probe, or
   - a denser sampling grid, or
   - a synthetic full-frame pattern such as `videotestsrc pattern=smpte`
     for machine-checkable runs.

2. **`dataPtr` semantics are now validated for the current path**
   Direct call tracing confirmed that `surfaceList[0].dataPtr` is being consumed
   as the source of a steady-state `cudaMemcpy2DToArray(..., cudaMemcpyDeviceToDevice)`
   upload into the GL texture backing storage.

3. **GPU-only proof is complete for the tested synthetic path**
   We first tried to close this with `nsys`, but on this stack the exported
   report only surfaced CUDA device-query calls and did not reliably expose the
   steady-state interop / memcpy activity. We therefore used a temporary
   `LD_PRELOAD` shim to trace the actual CUDA runtime calls made by the process.

## Final verification result

The direct CUDA call trace on the running demo showed this repeated steady-state
sequence:

```text
cudaGraphicsMapResources
cudaGraphicsSubResourceGetMappedArray
cudaMemcpy2DToArray(kind=DtoD)
cudaGraphicsUnmapResources
```

It also showed the expected one-time setup call:

```text
cudaGraphicsGLRegisterImage
```

Most importantly:

- repeated `cudaMemcpy2DToArray(kind=DtoD)` was observed during rendering
- zero `HtoD` copies were observed
- zero `DtoH` copies were observed

So for the tested `videotestsrc pattern=smpte -> nvvideoconvert -> NVBUF_MEM_CUDA_DEVICE RGBA`
path, the current widget implementation is confirmed to be **GPU-only** rather
than host-staged.

## Why this pivot is acceptable

The original P0.1 claim was “Qt + EGLImage zero-copy display.”

That claim is too narrow for the actual desktop RTX target. What we really need
for the production-facing DeepStream demo is:

> Can DeepStream-originated frames reach a Qt display path on desktop dGPU
> without falling back to CPU readback/upload?

A GPU→GPU copy into a GL texture still satisfies that engineering goal, even
though it is no longer “strict zero-copy”.

## Next steps after this document

1. Fix the automated validation strategy:
   - keep `pattern=ball` for human visual testing
   - add `pattern=smpte` as a separate machine-checkable mode
   - replace the old 5-point probe with a coarse grid `nonBlack / bright`
     statistic so sparse-content frames no longer look falsely black
2. Re-test RTSP on the new path
3. Update the component README to stop promising strict `EGLImage` zero-copy on
   desktop dGPU and instead document the actual GPU-only contract
4. If future profiling needs a vendor tool artifact, treat `nsys` as supporting
   evidence only; the decisive proof on this stack was direct CUDA call tracing

## Lesson

Debug probes are only valid relative to the image family they were designed
for. A probe that is perfect for a full-frame gradient can be actively
misleading for sparse-content animation.
