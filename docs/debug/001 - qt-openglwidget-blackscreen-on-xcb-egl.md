# 001 — `QOpenGLWidget` shows all-black under `xcb_egl` (DeepStream 9.0 container, NVIDIA 580)

> **Component:** `01_qt_eglimage_zerocopy`
> **Status:** root cause located; workaround committed by defaulting to GLX.
> **Hard takeaway:** when a Qt window is unexpectedly black, prove which side
> of the *FBO → window* boundary is wrong **before** touching shaders or
> textures.

## Symptom

- Window opens at the right size, no GL error, no GStreamer error, no crash.
- `paintGL()` runs at ~30 fps and `ingest-to-paint` latency stays under 1 ms,
  i.e. frames flow end-to-end.
- The window is **uniformly RGB(0,0,0)**, including in the corners — not even
  the `glClearColor` shows through.

## Environment at the time of the bug

| layer            | version                                                   |
| ---------------- | --------------------------------------------------------- |
| Host OS          | Linux 6.17, X11 (`mutter` WM)                             |
| GPU / driver     | RTX 5090, NVIDIA 580.126.09                               |
| Container image  | `nvcr.io/nvidia/deepstream:9.0-triton-multiarch` + Qt6    |
| Qt               | 6.4.2 (Compatibility 4.6 from NVIDIA driver)              |
| Platform plugin  | `xcb` with `QT_XCB_GL_INTEGRATION=xcb_egl` (forced)       |
| App              | `QApplication` + top-level `QOpenGLWidget`                |

## Hypothesis tree we walked

```
black screen, no error
├── A. paintGL never runs                  → REJECTED (fps logs prove it runs)
├── B. paintGL runs but draw is wrong
│   ├── B1. shader broken                  → REJECTED (FBO probe: gradient OK)
│   ├── B2. clear broken                   → REJECTED (clear writes the FBO)
│   └── B3. EGLImage / texture path        → IRRELEVANT (diagnostic shader
│                                            never samples a texture)
└── C. paintGL+draw correct, FBO never
     reaches the X window
   ├── C1. xcb_egl ↔ QOpenGLWidget compose → CONFIRMED
   └── C2. X server / compositor          → REJECTED (xwd shows black inside
                                            the right window id; window does
                                            exist at the expected geometry)
```

## How we proved it

We needed two **independent** probes that sit on either side of the
suspected boundary, so one cannot mask the other.

### Probe 1 — FBO read-back (inside the GL pipeline)

Right before `paintGL()` returns, sample 5 pixels of the QOpenGLWidget's
internal FBO with `glReadPixels`:

```cpp
unsigned char rgba[4];
glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
```

Knowing that a UV-gradient shader was bound (`fragColor = vec4(uv,0,1)`),
the expected pattern is uniquely identifying — no other failure mode produces
exactly `(0,254,0)` in the FBO bottom-left and `(254,1,0)` in the FBO
top-right.

### Probe 2 — `xwd` on the X11 window (outside the GL pipeline)

`xwd -id <win>` writes a raw window dump in XWD format. We parse it with a
~30-line Python script (`/tmp/xwd2pgm.py`) — XWD is a fixed-header format,
no library needed — and read the pixel value at the same five points,
in **window** coordinates this time.

If FBO probe says `gradient` and `xwd` probe says `(0,0,0)`, the FBO has
the right pixels but Qt's compose path lost them between the FBO and the
X window backing store.

### Probe 3 — flip `QT_XCB_GL_INTEGRATION` to isolate the offending layer

Same code, same shader, same container, only the platform integration
changes. We added an `ALLOW_EGL=1` runtime switch in `main.cpp` and
`run_in_container.sh`:

```bash
./scripts/run_in_container.sh               # default xcb_glx
ALLOW_EGL=1 ./scripts/run_in_container.sh   # forces xcb_egl
```

### Result table

| integration | FBO probe (GL side)                      | xwd probe (window side)                  |
| ----------- | ---------------------------------------- | ---------------------------------------- |
| `xcb_egl`   | `center=(128,127,0) BL=(0,254,0) ...` ✓ | `center=(0,0,0) BL=(0,0,0) ...` ✗       |
| `xcb_glx`   | `center=(128,127,0) BL=(0,254,0) ...` ✓ | `center=(128,128,0) BL=(0,254,0) ...` ✓ |

Identical bits in the FBO, opposite outcomes on screen. The bug is in
Qt's FBO → window compose, and only on the EGL integration.

## Root cause (as currently understood)

`QOpenGLWidget` always renders into a Qt-owned FBO and then asks the
top-level window's compositor (`QOpenGLContext` + backing store) to blit
that FBO onto the visible surface. On this driver/container/Qt
combination, the blit step under the `xcb_egl` integration completes
without any error code but produces no visible output. Switching to
`xcb_glx` makes the blit work because GLX has its own, separately
implemented compose path on the NVIDIA driver.

This is **not** unique to our code — any `QOpenGLWidget` user on this
exact stack would see it. We have not isolated this further to a specific
Qt commit, NVIDIA driver bug, or container EGL config; recording the
behaviour is enough to unblock the component.

## Things that were *not* the bug, despite looking suspicious

- `samplerExternalOES` shader with `GL_OES_EGL_image_external`. Removed
  during diagnostics; black persisted.
- `nvvideoconvert` output format `NV12` vs `RGBA`. Tried both; black
  persisted.
- `QSurfaceFormat::CompatibilityProfile` 3.3. Driver gave us 4.6
  compatibility regardless; black persisted.
- VAO / Core-profile compliance. Switched the diagnostic shader to GLSL
  330 core with an explicit VAO; black persisted.
- Window not exposed / wrong geometry. `xwininfo -root -tree` confirmed
  a 1280×720 window at sane host coords every run.

Recording these explicitly because each looked like a candidate for hours
and burning the same time again would be a waste.

## Fix options (ordered by cost)

1. **Default to GLX.** Keep `QOpenGLWidget`, but set
   `QT_XCB_GL_INTEGRATION=xcb_glx` by default and reserve `ALLOW_EGL=1`
   for diagnostics. On NVIDIA + libglvnd, `eglGetProcAddress`
   (`glEGLImageTargetTexture2DOES`) and the `EGLImageKHR` returned by
   `NvBufSurfaceMapEglImage` remain usable from a GLX context, so this is
   the cheapest way to unblock the component.
2. **Replace `QOpenGLWidget` with `QOpenGLWindow`** embedded via
   `QWidget::createWindowContainer`. This sidesteps QOpenGLWidget's FBO
   compose entirely, so the EGL integration can stay if zero-copy
   interop forces it later.
3. **File a Qt / NVIDIA reproducer.** Out of scope for the component,
   but worth doing once the demo itself is unblocked.

We implemented (1) as the current default. If EGLImage interop ever turns
out to need a true EGL context on a future platform, fall back to (2).

## Diagnostic instrumentation kept in the tree

So the next person can re-run the important probes without rederiving them:

- `src/VideoGLWidget.cpp` — the real `samplerExternalOES` texture shader is
  back, and the 5-point `glReadPixels` probe still prints once per second
  alongside the fps line so we can tell whether the internal FBO is alive.
- `src/main.cpp` — defaults to `xcb_glx` and honours `ALLOW_EGL=1` to switch
  back to the known-bad `xcb_egl` path for regression checks.
- `scripts/run_in_container.sh` — mirrors the same default / override logic
  into the container environment.
- This document, plus `/tmp/xwd2pgm.py` (regenerate from this file's
  Probe 2 section if it has been deleted).

The purpose of keeping the FBO probe is to separate two future failure modes:
`EGLImage` sampled wrong vs. widget composed wrong. Those are different bugs
and should stay distinguishable.

## Lessons

- "No errors + black screen" is almost always a presentation bug, not a
  draw bug. Reach for `glReadPixels` on the FBO **first** so we can
  decide on which side of the boundary to keep digging.
- Two probes on opposite sides of a suspected boundary beat any number
  of probes on the same side. The FBO probe alone would still leave us
  guessing whether the user just couldn't see the window; the `xwd`
  probe alone would still leave us guessing whether the shader was
  broken.
- When Qt's platform integration is implicated, leaving a runtime switch
  (`ALLOW_EGL=1`) in the tree is cheap and pays for itself the next
  time someone hits a related symptom.

## Current trade-off

We are **defaulting to GLX** today because it is the lowest-cost path that
both presents correctly on screen and still lets desktop NVIDIA bind the
`NvBufSurface`-exported `EGLImage` into `GL_TEXTURE_EXTERNAL_OES`.

We are **not defaulting to EGL** today because, in the current container /
driver / Qt combination, `QOpenGLWidget` under `xcb_egl` loses the FBO during
Qt's internal compose step and produces a black window even though the FBO is
correct.

This is a tactical choice, not a statement that GLX is architecturally better:

- **Why GLX now:** it works with the current widget design, unblocks the demo,
  and matches the immediate project constraint (X11 desktop on NVIDIA).
- **Why not EGL now:** `QOpenGLWidget` + `xcb_egl` is currently broken here.
- **Why EGL later might still matter:** Wayland and a cleaner all-EGL display
  stack will eventually push us back toward EGL.
- **What changes when that day comes:** do not force `xcb_egl` back into the
  current `QOpenGLWidget` design; migrate the display surface to
  `QOpenGLWindow` first, then retest.
