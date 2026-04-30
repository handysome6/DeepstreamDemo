# Component 01: `qt_eglimage_zerocopy`

> **Status:** `ready` — validated on dGPU as a **GPU-only display path**, not as
> a strict EGLImage zero-copy import. The directory name is kept for repo
> stability; the contract is described below.

## Goal

Prove that an `NvBufSurface`-backed `GstBuffer` (i.e.
`video/x-raw(memory:NVMM), format=RGBA`, `NVBUF_MEM_CUDA_DEVICE`) can be
displayed in a `QOpenGLWidget` **without ever leaving the GPU** — i.e. no
host-side `cudaMemcpy`, no read-back to system memory, no CPU re-upload.

The original framing was "Qt + EGLImage zero-copy". On desktop dGPU with
`NVBUF_MEM_CUDA_DEVICE` surfaces, `NvBufSurfaceMapEglImage()` is the wrong
primitive (per `nvbufsurface.h`, it requires `NVBUF_MEM_SURFACE_ARRAY`). The
validated contract is therefore:

- **Source frame:** `NVMM` + `RGBA` + `NVBUF_MEM_CUDA_DEVICE` (pitch-linear).
- **Transport into GL:** CUDA ↔ GL interop (`cudaGraphicsGLRegisterImage` →
  `cudaGraphicsMapResources` → `cudaMemcpy2DToArray(..., DeviceToDevice)` →
  `cudaGraphicsUnmapResources`).
- **GL side:** plain `GL_TEXTURE_2D` + `sampler2D` in a `QOpenGLWidget` under
  `xcb_glx`.

This is **GPU-only**, not strict zero-copy. It is the correct engineering
target for desktop RTX: it satisfies "DeepStream-originated frames reach Qt
display without host round-trip", which is the property the production app
actually depends on.

See `docs/debug/002 - dgpu-gpu-only-display-pivot.md` for the pivot rationale
and the CUDA-call trace that proves no `HtoD` / `DtoH` occurs in the steady
state.

## Why this matters

Every other DeepStream advantage (selective batched inference, GPU OSD,
multi-stream tiling, low-latency NVDEC) becomes irrelevant the moment the
display path forces a `cudaMemcpy` to system memory. Production today already
has GPU-resident decode → custom OpenGL canvas. Any DeepStream replacement has
to match that or it loses on latency before it starts.

This component is therefore the **single highest integration risk** in the
whole DeepStream port. It is now the **baseline proof** that everything from
component 02 onward is allowed to assume.

## Architecture

```text
videotestsrc (or rtspsrc + rtph264depay + h264parse + nvv4l2decoder)
    │
    ▼
nvvideoconvert
    │
    ▼   video/x-raw(memory:NVMM), format=RGBA   (NVBUF_MEM_CUDA_DEVICE, pitch-linear)
appsink (emit-signals=true, sync=false, max-buffers=1, drop=true)
    │   GST streaming thread
    │   gst_buffer_map -> NvBufSurface*
    │   surfaceList[0].dataPtr  (CUDA device pointer)
    │
    ▼   Qt::QueuedConnection
QOpenGLWidget::onNewFrame(FrameHolder*)
    │   Qt main thread / GL thread
    │   cudaGraphicsMapResources(&glTex2D)
    │   cudaGraphicsSubResourceGetMappedArray -> cudaArray
    │   cudaMemcpy2DToArray(cudaArray, src=dataPtr, kind=DeviceToDevice)
    │   cudaGraphicsUnmapResources
    │   draw fullscreen quad with sampler2D
    ▼
swap buffers (xcb_glx)
```

**GPU-only claim:** at no point is the NVMM surface read on the CPU, copied
to system memory, or re-uploaded over PCIe to the GPU. The GL texture's
backing storage is written in-place from the CUDA device pointer carried by
the `NvBufSurface`. Verified via direct CUDA-runtime call tracing (see
`docs/debug/002`).

## Environment matrix (validated)

| layer            | version                                                     |
| ---------------- | ----------------------------------------------------------- |
| Host OS          | Linux 6.17, X11 (`mutter` WM)                               |
| GPU              | NVIDIA GeForce RTX 5090 D (32 GB)                           |
| NVIDIA driver    | 580.126.09                                                  |
| Container image  | `nvcr.io/nvidia/deepstream:9.0-triton-multiarch` + Qt6      |
| DeepStream       | 9.0.0                                                       |
| CUDA toolkit     | 13.1 (V13.1.115)                                            |
| Qt               | 6.4.2                                                       |
| Platform plugin  | `xcb` with `QT_XCB_GL_INTEGRATION=xcb_glx`                  |
| GL profile       | Compatibility 4.6 (NVIDIA driver default)                   |

GLX is the known-good integration on this stack. `xcb_egl` produces a
uniformly black `QOpenGLWidget` window with no GL error — root-caused in
`docs/debug/001` to Qt's compose step losing the widget's internal FBO when
EGL is forced. The run script therefore defaults to GLX; `ALLOW_EGL=1`
remains as a diagnostic switch only.

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
- EGL (linked, used at platform-init by Qt; not used as the frame transport)
- CUDA toolkit (`CUDA::cudart`, `cuda_gl_interop.h`)
- DeepStream SDK (for `libnvbufsurface.so` and `nvbufsurface.h`)
- NVIDIA driver supporting CUDA-GL interop on desktop GL

## Run

The default pipeline uses `videotestsrc pattern=ball` so you can validate the
component with no camera and no DeepStream pipeline beyond the libs:

```bash
./scripts/run_videotestsrc.sh
# or, inside the containerized dev setup used in this repo:
./scripts/run_in_container.sh          # moving ball, best for human visual check
./scripts/run_in_container.sh smpte    # full-frame bars, best for machine checks
```

For a real RTSP camera (still no DeepStream pipeline yet — this is the bare
NVMM path):

```bash
RTSP_URL=rtsp://USER:PASS@HOST:554/Streaming/channels/101 \
  ./scripts/run_in_container.sh rtsp
# or, on the host build:
./scripts/run_rtsp.sh rtsp://USER:PASS@HOST:554/Streaming/channels/101
```

You should see a window with the test pattern (or live video) animating
smoothly, and a periodic stdout line similar to:

```text
fps=31  ingest-to-paint avg=0.6 ms  max=2 ms | tex=1920x1080 | FBO grid nonBlack=24/25 bright=15
```

Interpretation:
- `pattern=ball`: best for human visual confirmation that motion is visible.
- `pattern=smpte`: best for machine checks, because a full-frame pattern makes
  the `FBO grid nonBlack=...` statistic stable and meaningful.

For diagnostics, `ALLOW_EGL=1 ./scripts/run_in_container.sh` forces the
known-bad `xcb_egl` path. Expected result: window opens, `paintGL` runs at
~30 fps, latency stays sub-ms, but the visible window is uniformly black.

## Success criteria

- [x] Builds clean on a fresh checkout against DeepStream 9.0.
- [x] `run_in_container.sh smpte` shows a stable full-frame pattern,
      `FBO grid nonBlack=24/25`, no black frames, no stuttering.
- [x] `run_in_container.sh` (ball) shows the moving ball pattern with no
      stuttering.
- [x] `run_in_container.sh rtsp` shows the live camera feed using
      `nvv4l2decoder` (the decoder actually present in the DS9.0 container;
      `nvh264dec` is not provided here and its absence is not a display-path
      defect — see `docs/debug/002`).
- [x] Steady-state CUDA call tracing shows **zero** `HtoD` / `DtoH` host
      round-trips, and repeatedly observes
      `cudaGraphicsMapResources` → `cudaGraphicsSubResourceGetMappedArray` →
      `cudaMemcpy2DToArray(..., cudaMemcpyDeviceToDevice)` →
      `cudaGraphicsUnmapResources`.
- [x] `ingest-to-paint` p50 well under the 25 ms render-budget bound (see
      Measurements below).

## Measurements

Captured on the environment matrix above, 1920×1080 @ 30 fps, `smpte`
pattern, `xcb_glx`, ~45 s steady-state run.

| metric                         | value                                  |
| ------------------------------ | -------------------------------------- |
| fps                            | 30 – 31 (target met)                   |
| ingest-to-paint avg latency    | 0.6 ms (range 0.4 – 1.2 ms steady)     |
| ingest-to-paint max latency    | 1 – 4 ms typical; occasional 14 ms spike (host compositor / vsync) |
| CPU usage (demo process)       | ~50 % of one core                      |
| GPU SM utilization             | ~7 – 8 %                               |
| VRAM (demo process)            | 573 MiB                                |
| GPU-only verified              | yes — `cudaMemcpy2DToArray(..., DeviceToDevice)` only; no `HtoD` / `DtoH` observed |

These numbers are the **floor** that any future component must not regress
past on this stack.

## Known gotchas

- **This stack defaults to GLX, not EGL.** On the tested DS9.0 + NVIDIA 580 +
  Qt 6.4.2 + X11 stack, `QOpenGLWidget` renders a correct internal FBO under
  both integrations, but only `xcb_glx` successfully composes that FBO to the
  visible window. `xcb_egl` produces a pure-black window with no GL error.
  The run script therefore defaults to `QT_XCB_GL_INTEGRATION=xcb_glx`. Set
  `ALLOW_EGL=1` only for diagnostics or if we later move to a
  `QOpenGLWindow`-based path.
- **Frame contract is CUDA device, not SURFACE_ARRAY.** On desktop dGPU the
  `nvvideoconvert` output is `NVBUF_MEM_CUDA_DEVICE` + `RGBA` + pitch-linear.
  `NvBufSurfaceMapEglImage` and `NvBufSurfaceMapCudaBuffer` both require
  `NVBUF_MEM_SURFACE_ARRAY` per `nvbufsurface.h` and will not work here.
  Consume `surfaceList[0].dataPtr` directly via CUDA-GL interop instead.
- **`appsink max-buffers=1 drop=true`.** Without this, slow rendering grows
  the queue and adds 100 – 500 ms of latency invisibly.
- **Holder lifetime.** The CUDA device pointer is only valid while the
  underlying `GstSample` and the `NvBufSurface` mapping exist. The
  `FrameHolder` destructor releases them in the correct order.
- **Single-threaded GL.** `paintGL` and the `cudaGraphics*` calls must happen
  on the Qt GL thread. The `new-sample` callback runs on a GStreamer
  streaming thread; it `emit`s the new-frame signal which is delivered as a
  `Qt::QueuedConnection`.
- **EGL vs GLX trade-off.** GLX is the tactical default because it presents
  correctly through `QOpenGLWidget` on the tested stack. EGL remains the more
  future-facing choice for Wayland, but on this environment it requires
  moving away from `QOpenGLWidget` to avoid the black compose path. If a
  future platform forces EGL, the first fallback should be `QOpenGLWindow`
  via `createWindowContainer`, not forcing `xcb_egl` back into the current
  widget design.
- **RTSP decoder naming matters on this stack.** `nvh264dec` is not a valid
  element in the tested DeepStream container, so RTSP failure under that name
  is not evidence of a bad display path. The container provides
  `nvv4l2decoder`, and that is the decoder that matches the downstream
  `nvvideoconvert -> video/x-raw(memory:NVMM) -> NvBufSurface` contract.
- **Driver version.** Tested target is desktop NVIDIA 580.126.09. Driver must
  support CUDA-GL interop with `cudaGraphicsGLRegisterImage`. If the window
  presents but the texture itself is black, suspect a CUDA-array binding
  issue rather than the Qt compose path.

## Next

- **Component 02** (`nvurisrcbin_reconnect`) replaces the source with
  `nvurisrcbin` and proves robust RTSP reconnect over the same GPU-only
  display contract.
- **Component 04** (`multi_rtsp_widgets`) is the first multi-stream extension:
  N independent `RtspSource` + `QOpenGLWidget` pairs running concurrently over
  the same GPU-only path.
- **Component 06** (`multi_widget_canvas`) is the later integration step that
  collapses multiple proven per-stream textures into one shared canvas.
