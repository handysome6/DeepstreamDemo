# Component 05: `selective_yolo_infer`

> **Status:** `ready` — verified on a desktop dGPU (RTX 5090 / driver 580.126.09)
> against two local controllable RTSP feeds. The TrafficCamNet smoke path,
> the YOLOv10s production path, the runtime mode-flip state machine, and
> per-stream failure isolation have all been exercised with logs in
> `logs/`. See **Measurements** below for the captured numbers.

## Goal

Prove that **inference can be enabled per stream, independently**, on top of
the multi-widget GPU-only path proven by P0.4: each panel chooses at startup
whether to run a YOLO-style PGIE, and any panel can flip between raw rendering
and inference at runtime without disturbing its neighbours.

## Why this matters

Production never wants to pay inference compute on every stream. The DeepStream
replacement plan calls for "watch all N streams, run YOLO on the M that the
operator currently cares about, the rest just render". P0.4 proved we could
keep N independent RTSP streams alive concurrently. P0.5 proves we can put a
real PGIE on a *subset* of those streams and toggle that subset live, without
collapsing the others or breaking the GPU-only frame contract.

Two follow-on components depend on this:

- **P0.6** (`multi_widget_canvas`) needs to know that selective per-stream
  inference does not entangle multiple streams' GPU lifetimes — otherwise
  collapsing them into one canvas later is unsafe.
- **P0.7+** (selective batched inference, downstream tracker, MOT) only makes
  sense once "subset of streams under inference" works as a per-source primitive.

This component is **not**:

- big-canvas composition (that is P0.6),
- batched inference across streams (the integration step after P0.5),
- a YOLO benchmark — the architecture proof here is about *selective* and *per
  stream*, not about which model variant runs.

## Architecture

```text
[per-stream pipeline, Mode::Raw — same as P0.4]
RTSP URI ─► nvurisrcbin ─► nvvideoconvert ─► RGBA NVMM ─► appsink ─► VideoGLWidget

[per-stream pipeline, Mode::Infer]
RTSP URI ─► nvurisrcbin ─► nvstreammux(batch=1) ─► nvinfer(PGIE)
                          ─► nvvideoconvert ─► RGBA NVMM ─► nvdsosd ─► appsink ─► VideoGLWidget
```

Each `RtspInferSource` owns its own GstPipeline. Toggling mode tears the
pipeline down and rebuilds it in the new shape — the same lifecycle used for
disconnect recovery in P0.4 — which keeps the live/infer states explicitly
separate instead of trying to relink elements at runtime.

**GPU-only claim.** Both shapes terminate in the same NVMM RGBA appsink
contract (`NVBUF_MEM_CUDA_DEVICE`, pitch-linear), so the widget's CUDA-GL
upload path from P0.1 is byte-for-byte unchanged. Inference adds two GPU
elements (`nvinfer`, `nvdsosd`) but no host round-trip; bounding boxes are
attached as nvds metadata and rendered on-GPU by `nvdsosd` before the surface
hits the appsink.

**Selectivity model.** Two layers:

1. **Startup** — `--infer N` selects the 1-based panel index that should come
   up in inference mode. Repeatable. `--infer-all` is a shorthand.
2. **Runtime** — number keys `1`..`9` flip the corresponding panel's mode.
   Each flip is logged (`s2 mode -> YOLO`, `s2 mode -> raw`) and reflected in
   the per-panel overlay (`[YOLO]` vs `[raw ]`).

## Detector models

Two detector configs are wired in. The component is `selective_yolo_infer`,
so YOLO is the production target; TrafficCamNet stays in as the zero-friction
smoke-test path.

| `--infer-config`                                       | model                | needs external assets? |
|--------------------------------------------------------|----------------------|------------------------|
| `configs/config_infer_primary_trafficcamnet.txt` (default) | ResNet18 TrafficCamNet | no — staged from SDK image on first run |
| `configs/config_infer_primary_yolov10.txt`             | YOLOv10s + custom parser | yes — `scripts/fetch_models.sh` |

Both configs feed the same `nvinfer -> nvdsosd` shape; only the model file,
custom-lib path, and labels differ. `nvdsosd` draws bounding boxes from the
detector's metadata regardless of which model produced them.

### TrafficCamNet (default)

`scripts/run_in_container.sh` stages
`/opt/nvidia/deepstream/.../Primary_Detector/{onnx,labels,cal_trt.bin}` into
`/workspace/models/` on first run, then nvinfer compiles a TRT engine into
that same directory. Subsequent container starts deserialize the engine in
~1 s. No external downloads.

### YOLOv10s (production)

```bash
./scripts/fetch_models.sh                       # stages models/yolo/
./scripts/run_in_container.sh \
    --infer-config configs/config_infer_primary_yolov10.txt --infer 1
```

`fetch_models.sh` copies a known-good `yolov10s.onnx` + custom YOLO bbox
parser `.so` from a sibling project (override with `YOLO_SRC_DIR=...`).
Engine compile is ~50 s on first run; the harness then auto-relocates the
engine into `models/yolo/model.engine` so the next start hits the cache. The
quirk is documented in
[`docs/debug/005 - p05-trt-engine-not-persisted.md`](../../docs/debug/005%20-%20p05-trt-engine-not-persisted.md).

## Build

```bash
cd components/05_selective_yolo_infer
./scripts/build_in_container.sh
```

Dependency surface (same as P0.4 plus already-present DS plugins):

- DeepStream 9.0 (`nvurisrcbin`, `nvstreammux`, `nvinfer`, `nvdsosd`,
  `nvvideoconvert`)
- Qt 6 (Core, Gui, Widgets, OpenGL, OpenGLWidgets)
- GStreamer 1.x
- CUDA toolkit with CUDA-GL interop
- NVIDIA desktop driver supporting the P0.1/P0.2 path

## Run

```bash
# Default smoke: 2 panels on the local p02cam feed, panel 1 in YOLO mode.
./scripts/run_in_container.sh

# Two explicit streams, panel 2 starts inferring.
./scripts/run_in_container.sh \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --infer 2

# Four panels, two of them inferring.
./scripts/run_in_container.sh \
  --cols 2 \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --infer 1 --infer 4

# Every panel inferring.
./scripts/run_in_container.sh --infer-all
```

Expected runtime behavior:

- one top-level window with one `VideoGLWidget` per `--uri`.
- Each panel's overlay reads `sN [YOLO] LIVE age=…` or `sN [raw ] LIVE age=…`.
- stdout once per second prints `p05 status uptime=… panels=N infer=K/N | s1=… | s2=…`.
- Pressing a number key (e.g. `2`) toggles that panel's inference mode in
  place, logged as `s2 mode -> YOLO` / `s2 mode -> raw`.
- Inference panels render bounding boxes via `nvdsosd`; raw panels render
  unchanged frames.

### Soak test

```bash
DURATION=30m ./scripts/soak.sh \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --uri rtsp://127.0.0.1:8554/p02cam \
  --infer 1
```

During validation, manually flip panels via the keyboard at least three times
across the soak and confirm stdout shows `mode -> ` events lining up with the
overlay change, with the other panel remaining `LIVE` throughout.

## Soak harness flags

The Qt binary exposes two harness flags so soak runs do not require an
interactive keyboard:

- `--auto-toggle-seconds N` — every N seconds, every panel flips its mode.
  Used to validate that the stop/rebuild path holds up under repeated mode
  flips and that no per-flip resource leaks accumulate.
- `--quit-after-seconds N` — exits the binary cleanly after N seconds. Used
  by `scripts/soak.sh` so the harness ends through `QCoreApplication::quit()`
  rather than via SIGTERM, which lets us audit the destructor path for FD /
  VRAM cleanup.

Interactive mode-flip via number keys (1..9) still works when the soak flags
are absent.

## Success criteria

- [x] Builds clean in the component container (rebuilds in seconds against
      the cached `deepstream-demo/05-selective-yolo:latest` image).
- [x] Default smoke (`scripts/run_in_container.sh`) brings up 2 panels with
      panel 1 in `YOLO` mode and panel 2 in `raw`, both `LIVE` simultaneously
      (`logs/yolo-smoke.log`).
- [x] Inference panel renders visible bounding-box overlays via `nvdsosd`
      while the raw panel renders unchanged frames (visual confirmation
      during the smoke run; YOLOv10s detector output goes through `nvdsosd`
      before reaching the appsink, so the rendered texture already carries
      drawn boxes by the time it reaches CUDA-GL).
- [x] Auto-toggle (`--auto-toggle-seconds 30`) flips panel modes throughout a
      soak. Each flip is `Switching mode: X -> Y` followed by a `stream LIVE`
      event within the 2 s stall budget; both panels' frame counters keep
      climbing monotonically (`logs/soak-5min.log`).
- [x] A 5-minute soak with one panel in infer mode and 18 mode flips spread
      across the run completes without process crash or pipeline error
      (`Switching mode count: 18 / stream LIVE: 18 / pipeline error: 0`).
- [x] During the soak, deliberate mode flips show `mode -> YOLO` /
      `mode -> raw` in stdout and the matching overlay transitions on
      screen.
- [x] `nvidia-smi` shows VRAM held in a tight 1141–1172 MiB band across
      5 minutes of toggling — no monotonic growth — and falls to 342 MiB
      after process exit, confirming clean teardown of the inference engines.
- [x] Per-stream failure isolation: with one panel inferring and one raw,
      stopping the raw panel's publisher leaves the inferring panel's frame
      counter climbing throughout the 20 s outage; restarting the publisher
      brings the raw panel back to `LIVE` with `rc=1 st=1` while the
      inferring panel registers `rc=0 st=0` (`logs/isolation-yolo.log`).
- [x] Any latency claim in this component is expressed only relative to the
      P0.3 measurement contract; 05 itself does not redefine a new latency
      metric.

## Measurements

All on `RTX 5090 / driver 580.126.09 / DeepStream 9.0`, two `1920×1080 @ 15 fps`
local RTSP feeds, the inference panel using `configs/config_infer_primary_yolov10.txt`.

| metric                                      | value |
| ------------------------------------------- | ----- |
| smoke time-to-first-frame (engine cached)   | ~1 s |
| TRT engine compile (first run, YOLOv10s)    | ~50 s — cached at `models/yolo/model.engine` thereafter |
| TRT engine compile (first run, TrafficCamNet) | ~20 s — cached at `models/resnet18_..._fp16.engine` |
| steady-state per-panel render rate          | 17–18 fps (matches source rate) |
| ingest-to-paint avg (raw panel)             | ~5–7 ms |
| ingest-to-paint avg (YOLO panel)            | ~3–5 ms (the inference latency is absorbed inside the GST pipeline before the appsink hand-off, which is what `ingest-to-paint` measures from) |
| 5-min soak: mode flips                      | 18 across 9 toggle ticks |
| 5-min soak: pipeline errors                 | 0 |
| 5-min soak: VRAM band                       | 1141–1172 MiB (`logs/soak-5min.log` + `nvidia-smi` samples) |
| VRAM after clean exit                       | 342 MiB (idle baseline ~371 MiB) |
| failure-isolation: frames on infer panel during 20 s peer-outage | monotonic, `45 → 376 → 751 → 976` (`logs/isolation-yolo.log`) |
| failure-isolation: peer auto-recovery       | 1 stall + 1 reconnect on the raw panel; infer panel stays at `rc=0 st=0` |

## Known gotchas

- **`nvstreammux` is per-source.** Each `RtspInferSource` instantiates its
  own mux at `batch-size=1`. Sharing one mux across multiple panels is
  exactly the integration step P0.6 will tackle, and is intentionally NOT
  what this component proves. Don't refactor it in.
- **`nvstreammux` insists on a fixed output WxH.** Source resolution leaks
  through the mux only if you scale to its output dims downstream. We default
  to 1920×1080; override with `--mux-size WxH` if your camera is bigger and
  you want full-resolution inference input.
- **Mode flip is a stop/rebuild, not a relink.** A flip takes a few hundred
  ms to bring the new pipeline up; during that window the panel transiently
  shows `STALL` while `nvurisrcbin` reattaches. This is by design — the
  rebuild path is the same one we use for disconnect recovery, so a flip pays
  no new failure modes.
- **TRT engine build on first PGIE start.** The first time `nvinfer` runs in
  a fresh image it builds a TRT engine from the ONNX. That can take 30–90 s
  for ResNet18 / YOLOv8n on a typical dGPU. The widget will sit in `STALL`
  during this build; subsequent runs reuse the cached `*.engine` next to the
  ONNX. Don't mistake this for a hang.
- **YOLOv8/v10 needs a custom bbox parser.** Raw YOLO ONNX output does not
  match nvinfer's built-in detectors. The shipped YOLOv10s config (the
  production target) wires in a vendored `libnvdsinfer_custom_impl_Yolo.so`
  plus `parse-bbox-func-name=NvDsInferParseYolo` and
  `engine-create-func-name=NvDsInferYoloCudaEngineGet`. Run
  `scripts/fetch_models.sh` once before pointing `--infer-config` at the
  YOLO file. The default TrafficCamNet config has no such prerequisite.
- **YOLO custom-lib writes engine to a non-obvious path.** The YOLOv10
  parser's `NvDsInferYoloCudaEngineGet` hardcodes the engine save name and
  ignores `model-engine-file`'s directory; the engine ends up at
  `<CWD>/model_b<batch>_gpu<id>_fp<bits>.engine`. `scripts/run_in_container.sh`
  detects this and relocates the engine into `models/yolo/model.engine`
  on the next launch so the cache hits. See
  `docs/debug/005 - p05-trt-engine-not-persisted.md`.
- **DeepStream image entrypoint absorbs SIGTERM.** The base image's
  `entrypoint.sh` is a non-`exec` bash script that does not propagate
  signals, so a `timeout` wrapper used to leave orphan containers running.
  `scripts/run_in_container.sh` works around this with `--init` plus
  `--entrypoint /workspace/build/selective_yolo_infer`. See
  `docs/debug/004 - p05-deepstream-entrypoint-eats-sigterm.md`.
- **Same P0.2/P0.4 stream caveats still apply.** Dynamic pad linking,
  `nvurisrcbin` reconnect semantics, `appsink max-buffers=1 drop=true`, and
  the GLX-vs-EGL behavior all carry forward unchanged.

## Next

- **Component 06** (`multi_widget_canvas`) collapses several proven per-stream
  textures into one integrated canvas. P0.5 is its prerequisite specifically
  for the case where some of the per-stream feeds are inferred and some are
  raw, since 06 must not deduplicate the inference compute when it composites.
- A later batched-inference component will share *one* `nvstreammux` +
  `nvinfer` across selected streams and demux to per-stream widgets — that is
  the next natural step *after* both 05 and 06 are `ready`, not before.
