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

**Zero-copy claim:** at no point is the NV12 surface read on the CPU, copied
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

The default pipeline uses `videotestsrc` so you can validate the component
with no camera and no DeepStream installed beyond the libs:

```bash
./scripts/run_videotestsrc.sh
```

For a real RTSP camera (still no DeepStream pipeline yet — this is the bare
NVMM path):

```bash
./scripts/run_rtsp.sh rtsp://USER:PASS@HOST:554/Streaming/channels/101
```

You should see a window with the test pattern (or live video) animating
smoothly, and a periodic stdout line:

```
fps=30 ingest-to-paint-latency=12ms
```

## Success criteria

Tick each one on real hardware before moving status to `ready`.

- [ ] Builds clean on a fresh checkout against DeepStream 7.x or 9.x.
- [ ] `run_videotestsrc.sh` shows the moving ball pattern, no black frame,
      no stuttering, for at least 5 minutes.
- [ ] `run_rtsp.sh` shows the live camera feed.
- [ ] `nsys profile` of the running process shows **zero** `cuMemcpyDtoH` /
      `cuMemcpyHtoD` in the steady-state frame loop. (One-time setup
      transfers are fine.)
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
- Verified zero-copy via `nsys`: `__` (yes/no, attach trace path)

## Known gotchas

- **Qt must use EGL, not GLX.** On X11 set `QT_XCB_GL_INTEGRATION=xcb_egl`
  before `QApplication` is constructed (the run scripts do this). Otherwise
  Qt creates a GLX context and `NvBufSurfaceMapEglImage`'s `EGLImage` is
  not bindable in it.
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
- **Driver version.** Tested target is `RTX 5070 Ti` (Blackwell sm_120).
  Driver must support both NvBufSurface EGLImage export and desktop
  `GL_OES_EGL_image_external`. Any recent prop driver (>= 535) is expected
  to work; if the EGLImage binds but the texture renders black, suspect a
  driver/profile mismatch.

## Next

Once this is `ready`:

- **Component 02** (`nvurisrcbin_reconnect`) replaces the source with
  `nvurisrcbin` and proves robust RTSP reconnect using the same display
  path.
- **Component 06** (`multi_widget_canvas`) extends to N independent
  `EGLImage` textures composited inside one `QOpenGLWidget` — the actual
  production form.
