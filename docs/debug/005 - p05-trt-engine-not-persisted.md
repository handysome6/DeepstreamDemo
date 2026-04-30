# 005 — P0.5 TRT engine never persists across container instances

## Status

Closed on 2026-05-01.

## Symptom

Every fresh container startup re-built the TRT engine from ONNX, costing
roughly 20 s on a desktop dGPU even after a previous run had clearly produced
one. The engine compile log showed up cleanly the first time:

```text
[UID = 1]: deserialize engine from file :/opt/.../resnet18_..._fp16.engine failed
[UID = 1]: Trying to create engine from model files
[UID = 1]: serialize cuda engine to file: /opt/nvidia/.../resnet18_..._fp16.engine successfully
```

…and then the *next* run, into a fresh container, did exactly the same thing
again. We expected `model-engine-file` in the nvinfer config to point at a
host-persistent path under `/workspace/models/`, and to deserialize from it
on the second run. It never did.

This bit P0.5 hard because the toggle / soak harness rebuilds the pipeline
many times per minute; re-paying ~20 s of engine compile per rebuild made
the per-flip stall window unworkable.

## Root cause

`model-engine-file` in nvinfer's config is consulted only as a **read** path.
On a write, the SDK's nvinfer derives the engine path from the `onnx-file`
directory — it appends `_b<batch>_gpu<id>_<mode>.engine` to the ONNX
basename and writes there. So if the ONNX lives under
`/opt/nvidia/deepstream/deepstream-9.0/samples/models/Primary_Detector/`,
the engine ends up there too, regardless of `model-engine-file`. That
directory is part of the container's writable layer, not bind-mounted to the
host, so it is gone the moment `docker run --rm` exits.

A second wrinkle hit the YOLOv10 path: the custom
`engine-create-func-name=NvDsInferYoloCudaEngineGet` from the parser .so does
its own engine save and uses `<CWD>/model_b<batch>_gpu<id>_<mode>.engine`,
ignoring both the ONNX directory and `model-engine-file`. So under the YOLO
config, the file landed at `/workspace/model_b1_gpu0_fp16.engine` — host
persistent, but at the wrong path for the next deserialize attempt.

## Resolution

Two halves, one per detector path, both implemented in
`scripts/run_in_container.sh` so the README-stated "fresh checkout works"
contract holds:

### TrafficCamNet (default path)

- `scripts/run_in_container.sh` stages the SDK-shipped ONNX, labels, and
  int8 calibration into the bind-mounted `/workspace/models/` on first run,
  copying out of the build image. Subsequent runs see them already there
  and skip the copy.
- `configs/config_infer_primary_trafficcamnet.txt` now points
  `onnx-file=/workspace/models/resnet18_trafficcamnet_pruned.onnx` and
  `model-engine-file=/workspace/models/resnet18_trafficcamnet_pruned.onnx_b1_gpu0_fp16.engine`.
  Because both paths are inside the bind mount, nvinfer writes the engine
  into the same host-persistent directory and the next start hits the cache.

Verification:

```text
first run: serialize cuda engine to file: /workspace/models/resnet18_..._fp16.engine successfully
second run: deserialized trt engine from :/workspace/models/resnet18_..._fp16.engine
            Use deserialized engine model: /workspace/models/resnet18_..._fp16.engine
```

### YOLOv10s (production path)

- `scripts/fetch_models.sh` stages `yolov10s.onnx`, `yolov10s.onnx.data`,
  `libnvdsinfer_custom_impl_Yolo.so` and `labels.txt` into
  `/workspace/models/yolo/` from a sibling project that already has them.
- `scripts/run_in_container.sh` adds an explicit relocation step: if a fresh
  build dropped the engine at `/workspace/model_b1_gpu0_fp16.engine` (the
  custom-lib's hardcoded path), the script moves it to
  `/workspace/models/yolo/model.engine` on the next launch, where the YOLO
  config's `model-engine-file` is looking for it.

Verification:

```text
first run: serialize cuda engine to file: /workspace/model_b1_gpu0_fp16.engine successfully
[host]   : mv .../model_b1_gpu0_fp16.engine .../models/yolo/model.engine
second run: deserialized trt engine from :/workspace/models/yolo/model.engine
```

## Lessons

- `model-engine-file` is **not** the engine output path. It is the
  deserialize target. Treat it as one half of a contract whose other half is
  controlled by where the ONNX lives.
- Custom `engine-create-func-name` implementations may have their own,
  totally different filename convention. Inspect the parser `.so` before
  trusting `model-engine-file` to govern its writes.
- For DeepStream components that need engines to survive container churn,
  always co-locate the ONNX inside the bind mount; never leave it inside the
  SDK install directory.
- After 20 s × N rebuild becomes a measurable share of soak time, the
  P0.5-style mode-toggle pattern won't even reach its real validation
  questions (toggle latency, leak detection) — fix the cache first.
