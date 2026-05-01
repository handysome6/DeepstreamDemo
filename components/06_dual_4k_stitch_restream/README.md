# Component 06: `dual_4k_stitch_restream`

> **Status:** `ready` — verified on a desktop dGPU (RTX 5090 D / driver 580.126.09)
> against two local controllable 4K RTSP feeds plus a local mediamtx restream
> target. Identity stitch, calibrated stitch + blend, live restream, and
> one-sided degrade/recover have all been exercised with logs under `logs/`.

## Goal

Prove that **exactly two 4K RTSP inputs** can be stitched in **top-bottom**
layout into one GPU-resident output frame, previewed locally, and restreamed
without silently falling back to a CPU compositor or host-memory composition.

This component intentionally excludes inference on the stitched output.

## Why this matters

The earlier DeepStream POC failed partly because a composition step quietly
slipped onto CPU. P0.6 narrows the problem to the production-critical claim:
can we keep dual-camera ingest, pairwise stitch, preview, and restream on an
explicit GPU-owned path, with the single-pair stitch proof standing on its own
before we trust the continuous live path.

Production also does not mean a naive hard seam. The required end state is:

- top-bottom output geometry
- static calibration transforms per camera
- seam-hiding overlap blend
- live restream of the stitched result

## Architecture

```text
RTSP A (4K) ─► nvurisrcbin ─► nvvideoconvert ─► RGBA NVMM ─┐
                                                            ├─► custom CUDA stitch/warp/blend ─► Qt preview (CUDA-GL)
RTSP B (4K) ─► nvurisrcbin ─► nvvideoconvert ─► RGBA NVMM ─┘                                 └─► appsrc
                                                                                                   └─► nvvideoconvert
                                                                                                   └─► NV12 NVMM
                                                                                                   └─► nvv4l2h265enc
                                                                                                   └─► h265parse
                                                                                                   └─► rtspclientsink
                                                                                                   └─► mediamtx path /p06stitch
```

The stitch core has two modes:

- **`identity`** — top frame copied to rows `[0, H-1]`, bottom frame copied to
  rows `[H, 2H-1]`
- **`calibrated`** — output-space sampling through per-camera 3×3 transforms,
  with optional overlap blending

**GPU-only claim.** Inputs are validated as `NVBUF_MEM_CUDA_DEVICE` + RGBA +
pitch-linear `NvBufSurface`. The stitch output is allocated with
`cudaMallocPitch`, wrapped back into an `NvBufSurface`, previewed through the
existing CUDA-GL path, and pushed to the restream branch via wrapped device
memory.

## Build

```bash
cd components/06_dual_4k_stitch_restream
./scripts/build_in_container.sh
```

Dependency surface:

- DeepStream 9.0
- Qt 6 (Core, Gui, Widgets, OpenGL, OpenGLWidgets)
- GStreamer 1.24
- CUDA toolkit with CUDA-GL interop
- local RTSP server for restream publish target (validated with mediamtx)

## Run

### Identity stitch smoke

```bash
./scripts/run_in_container.sh \
  --top-uri rtsp://127.0.0.1:8554/cam4 \
  --bottom-uri rtsp://127.0.0.1:8554/cam5 \
  --calibration configs/identity_pair.json
```

Expected proof lines:

```text
p06 stitch proof top=3840x2160 ... | bottom=3840x2160 ... | out=3840x4320 ... gpuOnly=true mode=IDENTITY overlap=0
restream READY url=rtsp://127.0.0.1:8554/p06stitch ... via=rtspclientsink
```

### Calibrated stitch + blend smoke

```bash
./scripts/run_in_container.sh \
  --top-uri rtsp://127.0.0.1:8554/cam4 \
  --bottom-uri rtsp://127.0.0.1:8554/cam5 \
  --calibration configs/pair_calibration_example.json
```

Expected proof lines:

```text
p06 stitch proof top=3840x2160 ... | bottom=3840x2160 ... | out=3840x4288 ... gpuOnly=true mode=CALIBRATED+BLEND overlap=32
p06 status ... stitched=... pushed=... mode=CALIBRATED+BLEND state=OK
```

### Read the restream externally

```bash
docker run --rm --net=host --entrypoint bash \
  deepstream-demo/06-dual-4k-stitch-restream:latest -lc \
  'timeout 8 gst-launch-1.0 -q \
     rtspsrc location=rtsp://127.0.0.1:8554/p06stitch protocols=tcp latency=0 \
     ! rtph265depay ! h265parse ! fakesink sync=false'
```

A successful probe times out cleanly after attaching to the live stream.

### Soak

```bash
DURATION=30m ./scripts/soak.sh \
  --top-uri rtsp://127.0.0.1:8554/cam4 \
  --bottom-uri rtsp://127.0.0.1:8554/cam5 \
  --calibration configs/pair_calibration_example.json
```

A shorter validated soak variant used during this cycle was:

```bash
DURATION=2m ./scripts/soak.sh \
  --top-uri rtsp://127.0.0.1:8554/cam4 \
  --bottom-uri rtsp://127.0.0.1:8554/cam5 \
  --calibration configs/pair_calibration_example.json
```

## Success criteria

- [x] Builds cleanly in the component container.
- [x] Two RTSP branches independently deliver valid RGBA `NVBUF_MEM_CUDA_DEVICE`
      frames.
- [x] Identity-mode first stitch proof logs `top=3840x2160`, `bottom=3840x2160`,
      `out=3840x4320`, and `gpuOnly=true`.
- [x] Local preview shows top-bottom composition correctly.
- [x] Calibrated mode exercises output-space transform sampling and overlap
      blending with `out=3840x4288` and `mode=CALIBRATED+BLEND`.
- [x] Restream branch is externally reachable at `rtsp://127.0.0.1:8554/p06stitch`.
- [x] One-sided source interruption degrades the pair and recovers without
      process restart.
- [x] No inference is performed on the stitched output path.

## Measurements

All measurements below were taken on `RTX 5090 D / driver 580.126.09 /
DeepStream 9.0`, with two local 4K RTSP feeds published on `cam4` and `cam5`.

### Identity smoke

Command:

```bash
./scripts/run_in_container.sh \
  --top-uri rtsp://127.0.0.1:8554/cam4 \
  --bottom-uri rtsp://127.0.0.1:8554/cam5 \
  --calibration configs/identity_pair.json \
  --quit-after-seconds 12
```

Observed evidence:

- Proof line: `out=3840x4320 ... gpuOnly=true mode=IDENTITY overlap=0`
- By `uptime=00:00:11`, status reached `stitched=133 pushed=133 mode=IDENTITY state=OK`
- After adding a downstream queue in the restream branch, the earlier
  `please add queues` restream warning no longer appeared in this smoke run.

### Calibrated stitch + blend smoke

Command:

```bash
./scripts/run_in_container.sh \
  --top-uri rtsp://127.0.0.1:8554/cam4 \
  --bottom-uri rtsp://127.0.0.1:8554/cam5 \
  --calibration configs/pair_calibration_example.json \
  --quit-after-seconds 20
```

Observed evidence:

- Proof line: `out=3840x4288 ... gpuOnly=true mode=CALIBRATED+BLEND overlap=32`
- By `uptime=00:00:19`, status reached `stitched=262 pushed=262 mode=CALIBRATED+BLEND state=OK`
- External reader attached to `rtsp://127.0.0.1:8554/p06stitch` and stayed
  connected until the probe timeout.

### Short calibrated soak

Command:

```bash
DURATION=75s ./scripts/soak.sh \
  --top-uri rtsp://127.0.0.1:8554/cam4 \
  --bottom-uri rtsp://127.0.0.1:8554/cam5 \
  --calibration configs/pair_calibration_example.json
```

Observed evidence:

- After fixing the wrapper cleanup path, the short soak ended on schedule instead of leaving the component container alive past the timeout window.
- By `uptime=00:01:12`, status was still monotonic and balanced: `stitched=1018 pushed=1018 mode=CALIBRATED+BLEND state=OK`.
- The soak summary reported `observed stall transitions: 0` and `observed pipeline errors: 0`; the single `pair DEGRADED` / `pair RECOVERED` counted in the summary came from the normal startup phase before both sources were live.

### One-sided stall / recovery

Command shape:

```bash
./scripts/run_in_container.sh \
  --top-uri rtsp://127.0.0.1:8554/cam4 \
  --bottom-uri rtsp://127.0.0.1:8554/cam5 \
  --calibration configs/pair_calibration_example.json \
  --quit-after-seconds 26
```

During the run, the `cam5` publisher was stopped for ~7 seconds and then
recreated.

Observed evidence:

- Before interruption: `stitched=84 pushed=84 mode=CALIBRATED+BLEND state=OK`
- Stall detection: `stream STALLED — no frame for 2007 ms (stall events=1)`
- Pair degradation: `pair DEGRADED: source stale top=LIVE bottom=STALL`
- Recovery: `stream LIVE — frame just arrived (age=28 ms, total reconnects=1, frames=94)`
- Pair recovery: `pair RECOVERED`
- Post-recovery status: `b=LIVE age=22ms rc=1 st=1 ... stitched=92 pushed=92 mode=CALIBRATED+BLEND state=OK`

This confirms one-sided interruption degrades the pair and resumes continuous
stitching after the source returns, without restarting the process.

## Known gotchas

- **Restream uses RTSP RECORD, not self-hosted RTSP serving.** The validated
  path is `rtspclientsink -> mediamtx`, not `nvrtspoutsinkbin` and not the
  first `gst-rtsp-server` attempt. See `docs/debug/007 - p06-nvrtspoutsinkbin-503-switch-to-rtspclientsink.md`.
- **Blackwell-class GPUs need an explicit CUDA arch target here.** The
  calibrated kernel initially failed at runtime on RTX 5090 D until the build
  was pinned to `CMAKE_CUDA_ARCHITECTURES 120-real`. See
  `docs/debug/008 - p06-calibrated-kernel-needs-sm120-real.md`.
- **The pair consumer must be consume-once.** Reusing the same top/bottom pair
  and fanning out frames via queued UI/restream handoff caused repeated stitch
  work and eventual device-surface allocation failure. See
  `docs/debug/006 - p06-repeated-stitch-backlog-exhausts-device-surfaces.md`.
- **The local RTSP lab's 4K publishers are disposable containers.** If a
  publisher is stopped with `docker stop`, it must be recreated with
  `docker run`; `docker start` is not available because the containers run with
  `--rm`.
- **Soak cleanup depends on the wrapper-owned trap path.** The short soak now
  exits cleanly after moving `docker run` into a backgrounded child that the
  shell wrapper can `wait` on. See `docs/debug/009 - p06-soak-timeout-left-container-running.md`.
- The DeepStream image still prints harmless plugin-scan warnings for
  `libtritonserver.so` / `librivermax.so.1` even though this component does not
  use those plugins.

## Next

- The next integration step should build on this dual-camera GPU stitch contract
  rather than re-proving it.
- If production calibration becomes more complex, extend the current static 3×3
  transform representation rather than replacing the validated GPU-only stitch /
  restream path.
