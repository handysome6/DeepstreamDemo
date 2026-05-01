# 008 — P0.6 calibrated CUDA kernel needs native `sm_120` code on RTX 5090 D

## Status

Closed on 2026-05-01.

## Symptom

Identity mode worked because its fast path used `cudaMemcpy2DAsync`, but the
first real calibrated stitch kernel failed immediately at runtime:

```text
CUDA stitch failed: the provided PTX was compiled with an unsupported toolchain.
```

The effect was severe:

- calibrated mode never produced a stitched frame
- preview stayed black
- restream never published a live stitched output
- repeated pipeline errors flooded stdout even though build had succeeded

## Root cause

This component’s first custom CUDA kernel path was running on an RTX 5090 D
(`compute_cap = 12.0`). The build produced code that left the runtime depending
on PTX JIT for the calibrated kernel, and the runtime rejected that PTX with
`unsupported toolchain`.

So the stitch logic itself was not the problem. The issue was that the binary
was not emitting a native architecture target suitable for this Blackwell-class
GPU.

## Resolution

Pin the component build to a native architecture target in
`components/06_dual_4k_stitch_restream/CMakeLists.txt`:

```cmake
set(CMAKE_CUDA_ARCHITECTURES 120-real)
```

After rebuilding, the same calibrated smoke run succeeded and produced the
expected proof line:

```text
p06 stitch proof ... out=3840x4288 ... mode=CALIBRATED+BLEND overlap=32
```

and later steady-state counters such as:

```text
stitched=262 pushed=262 mode=CALIBRATED+BLEND state=OK
```

## Lessons

- A component can appear “CUDA-correct” if it only exercises memcpy-based fast
  paths; the first real kernel launch may reveal architecture-target problems.
- On very new GPUs, do not rely on whatever default CMake / nvcc arch behavior
  happens to produce. Lock the intended real architecture explicitly.
- If runtime says `unsupported toolchain`, inspect code-generation targets
  before rewriting kernel logic.
