# Component 04: `multi_rtsp_widgets`

> **Status:** `draft` — implementation scaffolded as the first multi-stream RTSP
> proof over the ready P0.1/P0.2 GPU-only contract. `ready` still depends on
> real hardware soak and per-stream failure-isolation validation.

## Goal

Prove that **multiple independent RTSP streams** can run at the same time over
the same dGPU GPU-only display path already validated in P0.1/P0.2, with each
stream rendered in its own `QOpenGLWidget` and each stream able to stall /
recover **without dragging the other widgets down with it**.

## Why this matters

P0.2 only proved a single `nvurisrcbin` pipeline. Production does not care
about “one stream can reconnect” in isolation; it cares that several streams
can stay alive together, and that one bad camera or one reconnect storm does
not freeze the whole viewer.

This component therefore proves the first real multi-stream shape we actually
need, while still avoiding premature integration. It is intentionally **not** a
single big canvas and **not** a YOLO/inference component. Those remain later
steps once the per-stream concurrency contract is trustworthy.

## Architecture

```text
RTSP URI #1 ─► RtspSource #1 ─► VideoGLWidget #1
RTSP URI #2 ─► RtspSource #2 ─► VideoGLWidget #2
RTSP URI #3 ─► RtspSource #3 ─► VideoGLWidget #3
RTSP URI #N ─► RtspSource #N ─► VideoGLWidget #N
                   │                     │
                   └──── per-stream health / reconnect / overlay

all widgets live inside one top-level QWidget + QGridLayout
```

Per stream, the media path remains the same as P0.2:

```text
nvurisrcbin -> nvvideoconvert -> video/x-raw(memory:NVMM),format=RGBA -> appsink
    -> Qt::QueuedConnection -> QOpenGLWidget -> CUDA-GL DeviceToDevice upload -> swap
```

**GPU-only claim:** every panel still consumes `NvBufSurface` frames in
`NVBUF_MEM_CUDA_DEVICE` memory and uploads into a GL texture with
`cudaMemcpy2DToArray(..., cudaMemcpyDeviceToDevice)`. No stream is allowed to
fall back to CPU readback just because there are multiple widgets on screen.

## Build

```bash
cd components/04_multi_rtsp_widgets
./scripts/build_in_container.sh
```

Dependency surface is intentionally the same as P0.2:

- DeepStream 9.0
- Qt 6 (Core, Gui, Widgets, OpenGL, OpenGLWidgets)
- GStreamer 1.x
- CUDA toolkit with CUDA-GL interop
- NVIDIA desktop driver that supports the P0.1/P0.2 path

## Run

Validated interface target:

```bash
# Default: 2 panels, both pointed at the local p02cam feed.
./scripts/run_in_container.sh

# Two explicit streams.
./scripts/run_in_container.sh \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --uri rtsp://127.0.0.1:8554/p02cam

# Four panels in a 2-column grid.
./scripts/run_in_container.sh \
  --cols 2 \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --uri rtsp://127.0.0.1:8554/p02cam

# UDP transport for diagnostics.
./scripts/run_in_container.sh --rtp udp --uri rtsp://127.0.0.1:8554/p02cam --uri rtsp://127.0.0.1:8554/p02cam
```

Expected runtime behavior:

- one top-level window containing one widget per `--uri`
- each panel shows its own overlay: `sN LIVE/STALL age rc st f`
- stdout prints one aggregate line per second beginning with `multi status`
- single-stream stalls/reconnects are also logged with `sN LIVE` / `sN STALLED`

### Soak test

```bash
# 30-minute two-stream soak.
DURATION=30m ./scripts/soak.sh \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --uri rtsp://127.0.0.1:8554/p02cam
```

During validation, manually stop/start the controllable publisher while the
soak is running and confirm only the intended panel stalls. The key contract is
failure isolation, not merely reconnect in the abstract.

## Success criteria

Each box must be ticked on real hardware before status moves to `ready`.

- [x] Builds clean in the component container.
- [x] Two explicit `--uri` panels reach live video simultaneously over the same
      GPU-only path proven by P0.2.
- [x] Four-panel launch with `--cols 2` opens a stable grid and each panel's
      overlay updates independently.
- [ ] A 30-minute soak with at least 2 panels completes without process crash
      or manual restart.
- [x] During the soak, manually stopping one panel's publisher causes only that
      panel to enter `STALL`; the untouched panels remain `LIVE` and continue
      increasing frame counters.
- [x] After that publisher returns, the affected panel returns to `LIVE`
      without requiring the other panels to restart.
- [x] Repeating the same test on a different panel shows the same isolation and
      recovery behavior.
- [ ] `nvidia-smi` / runtime logs do not show obvious monotonic VRAM or CPU
      runaway in steady state.
- [x] Any latency claim in this component is expressed only relative to the P0.3
      measurement contract; 04 itself does not redefine a new latency metric.

## Measurements

### 2026-05-01 four-stream isolation run

Log:

```text
components/04_multi_rtsp_widgets/logs/soak-20260501-001210.log
```

Shape under test:

```bash
DURATION=150s ./scripts/soak.sh \
  --cols 2 \
  --uri rtsp://127.0.0.1:8554/cam0 \
  --uri rtsp://127.0.0.1:8554/cam1 \
  --uri rtsp://127.0.0.1:8554/cam2 \
  --uri rtsp://127.0.0.1:8554/cam3
```

Observed evidence:

- Initial four-stream convergence succeeded: `s1 LIVE`, `s2 LIVE`, `s3 LIVE`,
  `s4 LIVE`, followed by aggregate `multi status` with all four panels live.
- `cam1` interruption isolated correctly:
  - `s2 STALLED age=2007ms (rtsp://127.0.0.1:8554/cam1)`
  - while `s1`, `s3`, and `s4` remained `LIVE` and kept increasing `f=`.
- `cam3` interruption isolated correctly later in the same run:
  - `s4 STALLED age=2177ms (rtsp://127.0.0.1:8554/cam3)`
  - while `s1` and `s3` stayed `LIVE`, and `s2` remained the only previously
    stalled panel until its publisher returned.
- Recovery was observed for both interrupted panels without restarting the app:
  - `s4 LIVE (rtsp://127.0.0.1:8554/cam3)` with subsequent aggregate status
    showing `s4=LIVE age=34ms rc=1 st=1 f=877`
  - `s2 LIVE (rtsp://127.0.0.1:8554/cam1)` with subsequent aggregate status
    showing `s2=LIVE age=8ms rc=1 st=1 f=328`
- This run proves per-stream failure isolation and per-stream recovery inside a
  four-widget viewer, but it is not the final 30-minute readiness soak.

## Known gotchas

- **This is multi-widget, not big-canvas composition.** If you need one shared
  canvas or cross-stream compositing, that belongs to 06, not here.
- **Each panel is intentionally its own source/widget pair.** We are proving
  concurrency and fault isolation, not deduplicating render resources.
- **The default launcher duplicates one local RTSP URI on purpose.** That keeps
  the smoke path simple even before multiple distinct publishers exist.
- **The same P0.2 stream caveats still apply.** Dynamic pad linking,
  `nvurisrcbin` reconnect semantics, `appsink max-buffers=1 drop=true`, and the
  GLX-vs-EGL behavior all carry forward unchanged.
- **This RTSP lab's publishers are not restartable with `docker start`.** In the
  observed local lab, stopping a publisher removed its container, so recovery
  testing required recreating the publisher container with `docker run` instead
  of assuming a stopped container still exists.

## Next

- **Component 05** (`cuda_stitch_appsink_loop`) can reuse the fact that several
  independent RTSP sources now run concurrently over the GPU-only path.
- **Component 06** (`multi_widget_canvas`) is the later step that collapses
  multiple per-stream textures into one integrated canvas once this per-panel
  isolation contract is proven.
