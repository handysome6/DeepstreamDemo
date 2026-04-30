# Architecture: incremental components

## Why this shape

The previous DeepStream POC tried to demonstrate selective inference, custom
8K composition, multi-stream sync, and a working pipeline at the same time.
The result was a moving target where any single failure (e.g. CPU `compositor`
silently sneaking in instead of `nvblender`) invalidated the whole demo.

This repo flips that: every capability is built and proven in **isolation**
before anything composes. The cost of doing this is more directories. The
benefit is that we can answer "is X working?" without running the whole
system.

## Component contract

Every component under `components/` must have:

1. **`README.md`** containing, in order:
   - **Goal** — what one capability this proves, in 1–2 sentences.
   - **Why this matters** — its role in the larger DeepStream replacement.
   - **Architecture** — text diagram of what flows through it.
   - **Build** — copy-pasteable commands.
   - **Run** — copy-pasteable commands, including any required env vars.
   - **Success criteria** — checkbox list. Until every box is ticked on real
     hardware, the component is `draft`.
   - **Known gotchas** — driver/version/flag traps the next person needs.
   - **Next** — what component naturally follows.

2. **`CMakeLists.txt`** — fully self-contained. Each component must build
   with `cmake -S . -B build && cmake --build build` from inside its own
   directory. No top-level umbrella CMake.

3. **`scripts/`** — at minimum a `run_*.sh` that launches the component with
   sane defaults and the right env vars set.

4. **No shared code** with other components in early phases. If two
   components grow the same helper, it stays duplicated until a third
   component needs it; only then it moves to a `common/` directory. Up-front
   sharing produces premature coupling.

## Lifecycle: draft → ready → integrated

A component goes through three states, recorded in its `README.md` header
and in the top-level component index:

- **`draft`** — code exists, may build, may not run on real hardware yet.
- **`ready`** — every success criterion verified on real hardware. Latency
  numbers (where applicable) are recorded in the README. Safe to depend on.
- **`integrated`** — at least one downstream component reuses it. From this
  point, breaking changes need a note in both READMEs.

## Anti-patterns we are explicitly avoiding

- **Big-bang assembly.** Writing a 2000-line `pipeline_app.cpp` that wires
  everything before any one stage is proven. The ffmpeg_decode → DeepStream
  POC failed this way: a CPU `compositor` slipped in and the whole demo's
  premise (zero-copy GPU pipeline) was silently violated.
- **Shared `common/` from day one.** Forces coordinated changes across
  components that should be independent. Defer until the duplication is
  real and recurring.
- **Latency claims without measurement infrastructure.** Component 03
  (`latency_probe`) exists specifically so no other component is allowed to
  claim a latency win without numbers.
- **Hidden dependencies on the production camera.** Components must run
  against `videotestsrc` or a local `mediamtx` lab first; only the explicitly
  marked components touch real RTSP cameras.

## Integration plan (sketch, subject to revision)

The end-state we are heading toward, once components are individually proven:

```
01_qt_eglimage_zerocopy   ──┐
02_nvurisrcbin_reconnect  ──┼─► 04_multi_rtsp_widgets
03_latency_probe          ──┘      proves several independent RTSP widgets can stay live together
                                     and recover per-stream without collapsing the whole viewer

04_multi_rtsp_widgets     ──► 05_selective_yolo_infer
                             puts nvinfer + nvdsosd on a runtime-selectable subset of panels
                             without disturbing failure isolation across panels

05_selective_yolo_infer   ──► 06_multi_widget_canvas
                             collapses multiple proven per-stream textures (some inferred,
                             some raw) into one integrated canvas
```

We do not start building integration components (06+) until 01–05 are each
in `ready` state.
