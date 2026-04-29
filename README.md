# DeepstreamDemo

Incremental component playground for proving out a DeepStream-based replacement
for the production GStreamer + nvcodec + Qt OpenGL pipeline.

## Philosophy

This repo deliberately avoids being one big application. Each subdirectory
under `components/` is an **isolated, independently buildable, independently
runnable prototype** that proves exactly **one** capability. A component only
graduates to "ready for integration" after its success criteria (in its own
`README.md`) are met on real hardware.

Integration is the disease this repo tries to cure. We will not write a
multi-thousand-line app and then debug it as a unit. We will land each piece
on its own, prove it, document its contract, and only then compose.

## Layout

```
DeepstreamDemo/
├── README.md                       this file
├── docs/
│   ├── architecture.md             the sub-project rules
│   └── component-template.md       what every new component must contain
└── components/
    ├── 01_qt_eglimage_zerocopy/    P0.1: NVMM → Qt OpenGL GPU-only display
    └── 02_nvurisrcbin_reconnect/   P0.2: single RTSP source + reconnect
```

## Component Index

| #  | Name                       | Proves                                                   | Status |
|----|----------------------------|----------------------------------------------------------|--------|
| 01 | `qt_eglimage_zerocopy`     | NVMM `GstBuffer` → CUDA-GL interop → Qt `QOpenGLWidget`  | ready  |
| 02 | `nvurisrcbin_reconnect`    | Single RTSP source with reliable reconnect over 01       | draft  |

Future components (planned, not implemented):

| 03 | `latency_probe`            | PTS-to-paint wallclock measurement utility              | tbd |
| 04 | `selective_yolo_batch`     | Selective batched inference on N of M streams           | tbd |
| 05 | `cuda_stitch_appsink_loop` | NVMM stitching via appsink → CUDA → appsrc round-trip   | tbd |
| 06 | `multi_widget_canvas`      | N independent EGLImage textures in one Qt canvas        | tbd |

## How to add a new component

1. Copy `docs/component-template.md` into `components/NN_descriptive_name/README.md`.
2. Fill in the Goal, Architecture, Success Criteria, and Known Gotchas sections.
3. Add a self-contained `CMakeLists.txt`. Do not depend on a top-level CMake
   project — each component must build standalone.
4. Land the code. Run it. Tick off the success criteria in the README before
   declaring it ready.

See `docs/architecture.md` for the discipline rules.
