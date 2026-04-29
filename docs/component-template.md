# Component: `NN_short_name`

> **Status:** `draft` | `ready` | `integrated`

## Goal

One sentence on the single capability this component proves.

## Why this matters

Two or three sentences on how this fits into the larger DeepStream
replacement. What does production currently do here? What is the risk if
this capability does not work?

## Architecture

```text
text diagram of inputs → stages → outputs
```

State explicitly which buffers stay on GPU and which (if any) cross to CPU.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

List any non-obvious dependencies (DeepStream version, driver version, Qt
version) that the build assumes.

## Run

```bash
./scripts/run_default.sh
```

Document any environment variables (`QT_XCB_GL_INTEGRATION`, `GST_DEBUG`,
…), expected window/console output, and how to swap inputs.

## Success criteria

Each box must be ticked on real hardware before status moves from `draft`
to `ready`.

- [ ] Builds clean on a fresh checkout.
- [ ] Runs end-to-end without crash for at least 5 minutes.
- [ ] *(latency-relevant components only)* Recorded latency under the
      stated budget. Numbers go below in **Measurements**.
- [ ] *(GPU components only)* Verified with `nsys` / `nvidia-smi` that the
      claimed zero-copy / GPU-only path is real.
- [ ] Reproduces from `scripts/run_default.sh` with no manual editing.

## Measurements

Fill in once verified. Examples:

- ingest-to-paint latency, p50 / p99: `__ ms` / `__ ms`
- frames dropped per minute under steady state: `__`
- CPU usage of the demo process: `__ %`
- GPU usage (SM / VRAM): `__ %` / `__ MB`

## Known gotchas

Bullet list. Things the next person to touch this will trip over. Driver
version mismatches, plugin substitutions, profile requirements, etc.

## Next

What component naturally builds on this one once it is `ready`.
