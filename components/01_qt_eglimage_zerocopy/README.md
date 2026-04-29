# Component 01: `qt_eglimage_zerocopy`

> **Status:** `draft` (not yet validated on hardware)

## Goal

Prove that an `NvBufSurface`-backed `GstBuffer` (i.e. `video/x-raw(memory:NVMM)`)
can be displayed in a `QOpenGLWidget` **without ever crossing the GPU/CPU
boundary**, by binding the surface's `EGLImage` directly to a GL texture.

## Why this matters

Every other DeepStream advantage (selective batched inference, GPU OSD,
multi-stream tiling, low-latency NVDEC) becomes irrelevant the moment the
display path forces a `cudaMemcpy` to system memory. Production today already
has zero-copy from NVDEC to its custom OpenGL canvas. Any DeepStream
replacement has to match that or it loses on latency before it starts.

This component is therefore the **single highest integration risk** in the
whole DeepStream port. If this works, everything else is engineering. If it
doesn't, the whole project needs a different display strategy.

## Architecture

```text
videotestsrc (or rtspsrc + nvh264dec)
    │
    ▼
nvvideoconvert
    │
    ▼   video/x-raw(memory:NVMM), format=NV12
appsink (emit-signals=true, sync=false, max-buffers=1, drop=true)
    │   GST streaming thread
    │   gst_buffer_map -> NvBufSurface*
    │   NvBufSurfaceMapEglImage(surf, 0) -> EGLImageKHR
    │
    ▼   Qt::QueuedConnection
QOpenGLWidget::onNewFrame(FrameHolder*)
    │   Qt main thread / GL thread
    │   glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex)
    │   glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, eglImage)
    │   draw fullscreen quad with samplerExternalOES
    ▼
swap buffers
```

**Zero-copy claim:** at no point is the NVMM surface read on the CPU, copied
to system memory, or re-uploaded to GL. The GL texture is a *view* over the
same CUDA-resident memory the decoder produced.

## Build

Assumes DeepStream is installed at `/opt/nvidia/deepstream/deepstream`. If
elsewhere, override with `-DDEEPSTREAM_DIR=...`.

```bash
cd components/01_qt_eglimage_zerocopy
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Dependencies:

- Qt 6 (Core, Gui, Widgets, OpenGL, OpenGLWidgets)
- GStreamer 1.x (`gstreamer-1.0`, `gstreamer-app-1.0`, `gstreamer-video-1.0`)
- EGL
- DeepStream SDK (for `libnvbufsurface.so` and `nvbufsurface.h`)
- NVIDIA driver supporting `GL_OES_EGL_image_external` on desktop GL
  (any recent prop driver does)

## Run

The default pipeline uses `videotestsrc pattern=ball` so you can validate the
component with no camera and no DeepStream installed beyond the libs:

```bash
./scripts/run_videotestsrc.sh
# or, inside the containerized dev setup used in this repo:
./scripts/run_in_container.sh          # moving ball, best for human visual check
./scripts/run_in_container.sh smpte    # full-frame bars, best for machine checks
```

For a real RTSP camera (still no DeepStream pipeline yet — this is the bare
NVMM path):

```bash
./scripts/run_rtsp.sh rtsp://USER:PASS@HOST:554/Streaming/channels/101
```

You should see a window with the test pattern (or live video) animating
smoothly, and a periodic stdout line similar to:

```text
fps=30  ingest-to-paint avg=0.3 ms  max=1 ms | tex=1920x1080 | FBO grid nonBlack=24/25 bright=15
```

Interpretation:
- `pattern=ball`: best for human visual confirmation that motion is visible
- `pattern=smpte`: best for machine checks, because a full-frame pattern makes
  the `FBO grid nonBlack=...` statistic stable and meaningful

For diagnostics, `ALLOW_EGL=1 ./scripts/run_in_container.sh` forces the old
`xcb_egl` path that is currently known-bad on this stack.

## Success criteria

Tick each one on real hardware before moving status to `ready`.

- [ ] Builds clean on a fresh checkout against DeepStream 7.x or 9.x.
- [ ] `run_videotestsrc.sh` shows the moving ball pattern, no black frame,
      no stuttering, for at least 5 minutes.
- [ ] `run_rtsp.sh` shows the live camera feed.
- [x] Steady-state CUDA call tracing shows **zero** `HtoD` / `DtoH` host
      round-trip in the frame loop, while repeatedly observing
      `cudaMemcpy2DToArray(..., cudaMemcpyDeviceToDevice)` plus CUDA-GL interop
      calls.
- [ ] `nvidia-smi dmon` shows GPU memory traffic without sustained PCIe
      readback traffic from device to host.
- [ ] Resizing the window does not crash and does not leak GPU memory
      (`nvidia-smi` VRAM stays flat over 60 seconds).
- [ ] Closing the window cleanly stops the GStreamer pipeline (no zombie
      threads in `htop`, no error spam).
- [ ] `ingest-to-paint-latency` p50 ≤ 25 ms with `videotestsrc`. (This is the
      pure render path latency; it bounds what the full pipeline can hope
      to achieve.)

## Measurements

Fill in once verified.

- ingest-to-paint latency p50 / p99: `__ ms` / `__ ms`
- CPU usage of demo process at 1080p30: `__ %`
- GPU SM / VRAM: `__ %` / `__ MB`
- Verified GPU-only via direct CUDA call tracing: `yes` (`cudaGraphicsMapResources` → `cudaGraphicsSubResourceGetMappedArray` → `cudaMemcpy2DToArray(..., cudaMemcpyDeviceToDevice)` → `cudaGraphicsUnmapResources`; no `HtoD` / `DtoH` observed)

## Known gotchas

- **This stack defaults to GLX, not EGL.** On the tested DS9.0 + NVIDIA 580 +
  Qt6 + X11 stack, `QOpenGLWidget` renders a correct internal FBO under both
  integrations, but only `xcb_glx` successfully composes that FBO to the
  visible window. `xcb_egl` produces a pure-black window with no GL error.
  The run script therefore defaults to `QT_XCB_GL_INTEGRATION=xcb_glx`.
  Set `ALLOW_EGL=1` only for diagnostics or if we later move to a
  `QOpenGLWindow`-based path.
- **Compatibility profile, not Core.** `samplerExternalOES` and the
  `GL_OES_EGL_image_external` extension only work in Compat profile on
  desktop NVIDIA GL. The shaders use `#version 120 + #extension`.
- **`appsink max-buffers=1 drop=true`.** Without this, slow rendering grows
  the queue and adds 100–500 ms of latency invisibly.
- **Holder lifetime.** The `EGLImage` is only valid while the underlying
  `GstSample` and the `NvBufSurface` mapping exist. The `FrameHolder`
  destructor calls `NvBufSurfaceUnMapEglImage`, then `gst_buffer_unmap`,
  then `gst_sample_unref`, in that order.
- **Single-threaded GL.** `paintGL` and `glEGLImageTargetTexture2DOES` must
  happen on the Qt GL thread. The `new-sample` callback runs on a
  GStreamer streaming thread; it `emit`s the new-frame signal which is
  delivered as a `Qt::QueuedConnection`.
- **EGL vs GLX trade-off.** GLX is the tactical default because it presents
  correctly through `QOpenGLWidget` on the tested stack. EGL remains the more
  future-facing choice for Wayland and for a purer EGLImage story, but on this
  environment it requires moving away from `QOpenGLWidget` to avoid the black
  compose path. If a future platform forces EGL, the first fallback should be
  `QOpenGLWindow` via `createWindowContainer`, not forcing `xcb_egl` back into
  the current widget design.
- **Driver version.** Tested target is desktop NVIDIA 580.126.09. Driver must
  support both NvBufSurface EGLImage export and desktop
  `GL_OES_EGL_image_external`. If the window presents but the texture itself is
  black, suspect an EGLImage/GL interop issue rather than the Qt compose path.

## Next

Once this is `ready`:

- **Component 02** (`nvurisrcbin_reconnect`) replaces the source with
  `nvurisrcbin` and proves robust RTSP reconnect using the same display
  path.
- **Component 06** (`multi_widget_canvas`) extends to N independent
  `EGLImage` textures composited inside one `QOpenGLWidget` — the actual
  production form.
