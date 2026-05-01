# Component 07: `full_pressure_integration`

> **Status:** `ready` — verified on a desktop dGPU (RTX 5090 / driver
> 580.126.09 / DeepStream 9.0) against the local `cam0..cam5` mediamtx lab.
> The staged-ramp smoke (stages 1/2/3), 5-min full-pressure soak, and three
> per-stream isolation tests (1080p / stitch / YOLO source classes) were all
> exercised on 2026-05-01 with logs preserved under `logs/`. Numbers are in
> **Measurements** below.

## Goal

Run, in **one Qt application on one box**, the production-shape mix:

- **8 × 1080p RTSP** → raw widget (the P0.4 path)
- **1 × 4K RTSP**   → YOLOv10s with full-res display + nvinfer-internal
                       downsample to network input (the P0.5 path, mux output
                       bumped to 3840×2160)
- **2 × 4K RTSP**   → GPU stitch top-bottom → one widget (the P0.6 path
                       *without* the restream branch)

10 widgets in one window, 11 RTSP ingests, three independent subsystems
sharing the same dGPU.

## Why this matters

Each prior component proved its capability in isolation. Production load is
none of them alone — it is all three at once. P0.7 is the first composition
proof that exposes the **real bottleneck** under combined decode / VRAM /
CUDA / Qt-paint contention. The earlier DeepStream POC failed when a CPU
compositor silently slipped in under multi-stream load; this component is
the soonest meaningful place to detect that class of failure for the
DeepStream replacement.

## Architecture

```text
RTSP 1080p #1 ─► RtspSource ─► VideoGLWidget          (raw)
RTSP 1080p #2 ─► RtspSource ─► VideoGLWidget          (raw)
…
RTSP 1080p #8 ─► RtspSource ─► VideoGLWidget          (raw)

RTSP 4K       ─► nvurisrcbin ─► nvstreammux(3840×2160,batch=1)
                              ─► nvinfer (YOLOv10s, 640²)
                              ─► nvvideoconvert ─► RGBA NVMM
                              ─► nvdsosd ─► appsink ─► VideoGLWidget   (4K + boxes)

RTSP 4K top   ─► nvurisrcbin ─► nvvideoconvert ─► RGBA NVMM ─┐
                                                              ├─► StitchCuda ─► StitchGLWidget
RTSP 4K bot   ─► nvurisrcbin ─► nvvideoconvert ─► RGBA NVMM ─┘    (calibrated + blend, no restream)
```

All 10 widgets live inside one top-level `QWidget` + `QGridLayout` driven by
a single `QApplication` thread.

**GPU-only claim.** Every widget consumes `NvBufSurface` frames in
`NVBUF_MEM_CUDA_DEVICE` memory and uploads to its GL texture with
`cudaMemcpy2DToArray(..., cudaMemcpyDeviceToDevice)`. This is the same
contract validated by P0.1/P0.4/P0.5/P0.6. The stitch panel allocates its
output with `cudaMallocPitch` wrapped into an `NvBufSurface`, identical to
P0.6's path.

**Selectivity model.** Stage selection is a startup flag, not a runtime
toggle. The YOLO panel is always-on under `--stage plus_yolo` and `--stage
full`. The 05 mode-flip code path was deliberately stripped — full-pressure
integration is not the place to exercise per-stream raw↔infer flips.

## Build

```bash
cd components/07_full_pressure_integration
./scripts/build_in_container.sh
```

Dependency surface is the union of 04/05/06:

- DeepStream 9.0 (`nvurisrcbin`, `nvstreammux`, `nvinfer`, `nvdsosd`,
  `nvvideoconvert`)
- Qt 6 (Core, Gui, Widgets, OpenGL, OpenGLWidgets)
- GStreamer 1.x
- CUDA toolkit with CUDA-GL interop, **`CMAKE_CUDA_ARCHITECTURES 120-real`**
  for the StitchCuda kernel on RTX 5090 (debug 008)
- NVIDIA desktop driver supporting the P0.1/P0.2 path

## Run

```bash
# Stage the YOLOv10s assets once (same source as 05).
./scripts/fetch_models.sh

# Stage 1: 8 × 1080p raw only — paint pressure floor.
./scripts/run_in_container.sh --stage 1080p_only --quit-after-seconds 30

# Stage 2: + 1 × 4K YOLO panel.
./scripts/run_in_container.sh --stage plus_yolo --quit-after-seconds 30

# Stage 3: + 2 × 4K stitch panel (full pressure, default).
./scripts/run_in_container.sh --stage full --quit-after-seconds 30

# Override URIs (default duplicates the existing local lab feeds).
./scripts/run_in_container.sh \
  --uri-1080p rtsp://127.0.0.1:8554/cam0 \
  --uri-1080p rtsp://127.0.0.1:8554/cam1 \
  ...                                         # up to 8 --uri-1080p flags
  --uri-4k-yolo rtsp://127.0.0.1:8554/cam4 \
  --uri-4k-stitch-top rtsp://127.0.0.1:8554/cam4 \
  --uri-4k-stitch-bottom rtsp://127.0.0.1:8554/cam5
```

Expected runtime behavior:

- one window with up to 10 `VideoGLWidget`s in a 4-column grid plus the
  spanning stitch panel.
- each panel's overlay reads `rN [raw ] LIVE age=…`, `y1 [YOLO] LIVE age=…`
  or `s1 [STCH] a=LIVE b=LIVE delta=…ms stitched=N OK`.
- once a second stdout prints `p07 status uptime=… stage=… raw=L/L
  yolo=L/L stitch=L/L | …`.
- the YOLO panel renders boxes via `nvdsosd` directly on the 4K canvas.

### Soak test

```bash
DURATION=5m ./scripts/soak.sh --stage full
```

Logs land under `logs/soak-<timestamp>.log`; the wrapper prints a soak
summary counting LIVE / STALL transitions, pipeline errors, and pair
DEGRADED / RECOVERED events.

## Success criteria

- [x] Builds clean in the component container with `CMAKE_CUDA_ARCHITECTURES
      120-real` taking effect.
- [x] Stage 1 (`--stage 1080p_only`) brings up 8 raw widgets, all `LIVE`
      within ~5 s, frame counters monotonic (`logs/stage1-smoke.log`).
- [x] Stage 2 (`--stage plus_yolo`) brings up stage 1 + the 4K YOLO panel
      with visible bounding boxes from `nvdsosd`
      (`logs/stage2-smoke.log`).
- [x] Stage 3 (`--stage full`) brings up all 10 widgets, all `LIVE` within
      ~10 s; the stitch panel reports `tex=3840x4288`
      (`logs/stage3-smoke.log`).
- [x] 5-minute Stage 4 soak completed without process crash, monotonic VRAM
      growth, or pipeline errors (`logs/soak-20260501-183359.log`).
- [x] Per-stream isolation: stopping any one publisher only stalls its own
      widget; restarting it returns that widget to `LIVE` without restarting
      the application. Verified for `cam2` (1080p),
      `cam5` (stitch bottom), and `cam5` reassigned to YOLO
      (`logs/isolation-*.log`).
- [x] `nvidia-smi` shows VRAM band 2443–2444 MiB during the full-pressure
      run (1 MiB spread); VRAM fell from 2444 MiB to 306 MiB on clean exit
      (idle baseline 297 MiB) — `logs/vram-stage4.csv`.
- [x] Latency claims are expressed only relative to the P0.3 measurement
      contract; this component does not redefine a new latency metric.

## Measurements

All measurements taken on `RTX 5090 / driver 580.126.09 / DeepStream 9.0`
against the local mediamtx lab (`cam0..cam3` 1920×1080@15fps, `cam4..cam5`
3840×2160@15fps). 1080p slots default to duplicating `cam0..cam3` across
8 widgets; the YOLO panel runs on `cam4` and the stitch pair on `cam4`+
`cam5`.

### Convergence — staged ramp

| stage | mix                                | time-to-all-LIVE | log |
|-------|------------------------------------|------------------|-----|
| 1     | 8 × 1080p raw                      | ≤ 5 s            | `logs/stage1-smoke.log` |
| 2     | stage 1 + 1 × 4K YOLO              | ≤ 10 s           | `logs/stage2-smoke.log` |
| 3     | stage 2 + 2 × 4K stitch (top+bot)  | ≤ 10 s           | `logs/stage3-smoke.log` |

### Steady-state per-widget FPS (Stage 3 smoke)

| widget class | source            | render rate | ingest-to-paint avg | ingest-to-paint max |
|--------------|-------------------|-------------|---------------------|---------------------|
| 8 × raw      | `cam0..cam3` (×2) | 17–18 fps   | 3.5–7.8 ms          | 38–78 ms            |
| 1 × YOLO     | `cam4` 4K         | 17–18 fps   | 6.4 ms (`tex=3840×2160`) | 59 ms          |
| 1 × stitch   | `cam4`+`cam5` 4K  | 17–18 fps   | 6.6–7.2 ms (stitch-to-paint, `tex=3840×4288`) | 70 ms |

The YOLO panel is the headline number to compare against 05: at the
**4K mux output** required for full-res display, ingest-to-paint averages
**6.4 ms** versus 05's 3–5 ms at 1080p mux. The 4K nvdsosd canvas is the
visible cost of "display 4K + boxes" and is the explicit tradeoff this
component is measuring.

### 5-min full-pressure soak (`logs/soak-20260501-183359.log`)

Command: `DURATION=5m ./scripts/soak.sh --stage full`.

| metric                                            | value |
|---------------------------------------------------|-------|
| process crashes                                   | 0 |
| pipeline errors                                   | 0 |
| stall transitions during steady state             | 0 |
| LIVE / STALL / DEGRADED tracking                  | `raw=8/8 yolo=1/1 stitch=1/1` for the entire 5 min |
| per-raw frame count at uptime 4:55                | ~4380–4421 (≈15 fps × 5 min) |
| YOLO frame count at uptime 4:55                   | 4414 |
| stitched frame count at uptime 4:55               | 4235 |
| pairDelta band                                    | 0–61 ms (well under the 250 ms DEGRADED threshold) |

### VRAM band (`logs/vram-stage4.csv`, 5 s sampling)

| sample point             | VRAM     |
|--------------------------|----------|
| idle baseline (pre-launch) | 297 MiB |
| ramp-up (after 5 s)        | 939 MiB |
| steady-state band, 5 min   | **2443–2444 MiB** (1 MiB spread) |
| post-exit                  | 306 MiB |

No monotonic growth and a clean ~9 MiB residual over baseline after exit.
2.4 GiB is the headline integration cost of 11 RTSP ingests + the YOLO
engine + the StitchCuda output buffer — all on a 32 GiB RTX 5090, well
inside the budget.

### Per-stream isolation

Each test runs `--stage full` (or `plus_yolo`) and stops the targeted
publisher container at uptime 15 s, recreating it 12 s later. The Qt app
runs throughout.

#### 1080p (`logs/isolation-cam2-1080p.log`)

Stopped `deepstreampoc-rtsp-pub-2` (publishes `cam2`). Runtime panel-to-
URI map for this run was `r2,r6 → cam2` (the `--uri-1080p` defaults are
prepended to the args list in run_in_container.sh, so the parser order
is reversed; `r1,r5 → cam3`, `r3,r7 → cam1`, `r4,r8 → cam0`).

Observed:
- `r2 STALLED age=2196ms (rtsp://127.0.0.1:8554/cam2)`
- `r6 STALLED age=2196ms (rtsp://127.0.0.1:8554/cam2)`
- `r1 r3 r4 r5 r7 r8 y1 s1` all stayed `LIVE` with `rc=0 st=0`
- After recreate: `r6 LIVE`, then `r2 LIVE`, both with `rc=1 st=1`

#### Stitch bottom (`logs/isolation-cam5-stitch.log`)

Stopped `deepstreampoc-rtsp-pub-5` (publishes `cam5`, used by stitch
bottom only in this stage). Observed:
- `s1 pair DEGRADED: source stale top=LIVE bottom=STALL`
- `raw=8/8 yolo=1/1 stitch=0/1` throughout the outage
- `delta=3ms` held (confirming the `top` half was still flowing)
- After recreate: `stream LIVE — frame just arrived (... reconnects=1)`,
  `s1 pair RECOVERED`, stitched count resumed climbing
  (128 → 205 → 278 → 349 over the next 18 s).

#### YOLO (`logs/isolation-cam5-yolo.log`)

Reassigned the YOLO panel to `cam5` (`--uri-4k-yolo
rtsp://127.0.0.1:8554/cam5`) under `--stage plus_yolo` so cam5 is the
YOLO source only. Stopped `deepstreampoc-rtsp-pub-5`. Observed:
- `y1 STALLED age=2130ms (rtsp://127.0.0.1:8554/cam5)`
- `raw=8/8 yolo=0/1 stitch=0/0` throughout the outage
- All 8 raw widget frame counters kept climbing
- After recreate: `y1 LIVE` with `rc=1 st=1`, frames climbing to 361 over
  the next 18 s.

All three isolation tests demonstrate the per-source failure-isolation
contract: only the affected widget(s) transition to `STALL`/`DEGRADED`,
all others remain `LIVE`, and recovery is automatic on publisher
restart without any application-level intervention.

## Known gotchas

- **The local RTSP lab is external to the repo.** P0.7's defaults duplicate
  the existing `cam0..cam5` + `p02cam` publishers across the 8 raw slots, the
  same way 04 does. Distinct cameras would stress nvdec harder; the recorded
  measurements will note "shared publishers" so the numbers are not over-
  claimed.
- **YOLO mux at 4K is not free.** `nvdsosd` draws boxes on the 3840×2160
  canvas every frame, which is more GPU work than 05 measured (1080p mux).
  This cost is part of what P0.7 is measuring, not avoiding. To compare
  apples-to-apples with 05, override with `--mux-size 1920x1080`.
- **Stitch panel is consume-once.** Lifted from 06's debug-006 finding —
  P0.7 still hands stitched frames to a single consumer (the widget); the
  restream consumer is intentionally absent.
- **Engine cache on first run.** YOLOv10s engine compile is ~50 s; first
  Stage-2 launch will sit in `STALL` during that build. Subsequent runs
  reuse `models/yolo/model.engine`.
- **Same SIGTERM and soak cleanup workarounds carry over** from debug 004
  (`--init` + non-bash entrypoint) and debug 009 (wrapper-owned trap path).
- **No mode flip.** 05's number-key toggles and `--auto-toggle-seconds` are
  intentionally absent from this component; the YOLO panel is always-on.
- **No restream.** Production restream is 06's contract; not P0.7's.

## Next

- Once 07 is `ready`, the natural successor is **batched inference**: share
  one `nvstreammux` + one `nvinfer` across the YOLO-targeted streams (one
  per source today) and demux back to per-stream widgets. That is *only*
  meaningful with both 05 and 07 already proven.
- A 30-minute full-pressure soak is the next graduation beyond the 5-min
  bar adopted here, mirroring 04's still-pending 30-min soak.
