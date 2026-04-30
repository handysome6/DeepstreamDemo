# Component 05: `selective_yolo_infer`

> **Status:** `draft` — implementation scaffolded as the first per-stream
> selective-inference proof over the ready P0.4 multi-widget contract. `ready`
> still depends on real hardware soak with at least one inference-active panel
> and runtime mode-flip validation.

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

## Default detector model

The default `--infer-config` points at the SDK-bundled
**ResNet18 TrafficCamNet** detector. That choice is deliberate:

- it ships inside the DeepStream container, so a fresh checkout works with no
  external downloads — matching the architecture rule that components must
  build and run on a clean clone;
- it produces real bounding boxes that `nvdsosd` can render, so the
  "inference vs raw" panel difference is visible to a human watcher;
- the YOLOv8 swap is then a *one-config-change* (`--infer-config
  configs/config_infer_primary_yolov8.txt`) once the model and parser are
  staged. See the *Switching to YOLO* section below.

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

## Switching to YOLO

The YOLO path is gated only by the model file and the parser library, both
out-of-image:

```bash
# 1. Stage the .pt checkpoint and labels (script is idempotent).
./scripts/fetch_models.sh

# 2. Export ONNX *outside* this container — the runtime image deliberately
#    does not bring in ultralytics or onnx-graphsurgeon.
yolo export model=models/yolov8n.pt format=onnx opset=12 imgsz=640

# 3. Drop a YOLO bbox parser .so under models/ (e.g. the marcoslucianops
#    DeepStream-Yolo build), edit configs/config_infer_primary_yolov8.txt to
#    point at it, and:
./scripts/run_in_container.sh \
  --infer-config configs/config_infer_primary_yolov8.txt \
  --infer 1
```

This explicit two-step flow is intentional. The architecture rule is that the
default checkout must build and run end-to-end; bringing YOLO inference along
with that would force-vendor either ultralytics or a third-party parser into
the image, both of which fail the rule.

## Success criteria

Each box must be ticked on real hardware before status moves from `draft` to
`ready`.

- [ ] Builds clean in the component container.
- [ ] Default smoke (`scripts/run_in_container.sh`) brings up 2 panels with
      panel 1 in `YOLO` mode and panel 2 in `raw`, both `LIVE` simultaneously.
- [ ] Inference panel renders visible bounding-box overlays via `nvdsosd`
      while the raw panel renders unchanged frames.
- [ ] Pressing the number key for any panel flips that panel's mode within
      ~1 s without affecting the other panel's `LIVE` state or frame counter
      cadence.
- [ ] A 30-minute soak with at least one panel in infer mode completes without
      crash and without monotonic VRAM/CPU runaway.
- [ ] During the soak, three deliberate mode flips on each panel show
      `mode -> YOLO` / `mode -> raw` in stdout and the matching overlay
      transitions on screen.
- [ ] `nvidia-smi` confirms inference panels actually run on GPU (TRT engine
      built once per process and reused; no monotonic VRAM growth across
      flips).
- [ ] Any latency claim in this component is expressed only relative to the
      P0.3 measurement contract; 05 itself does not redefine a new latency
      metric.

## Measurements

Fill in once verified on hardware.

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
- **YOLOv8 needs a custom bbox parser.** Raw YOLOv8 ONNX output does not
  match nvinfer's built-in detectors. The shipped YOLO config is a template
  with the parser entries commented; do not pass `--infer-config
  configs/config_infer_primary_yolov8.txt` until both the model and the
  parser `.so` are in place. The default TrafficCamNet config has no such
  prerequisite.
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
