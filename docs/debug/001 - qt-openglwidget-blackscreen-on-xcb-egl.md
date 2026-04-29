# 001 — `QOpenGLWidget` shows all-black under `xcb_egl` (DeepStream 9.0 container, NVIDIA 580)

> **Component:** `01_qt_eglimage_zerocopy`
> **Status:** root cause located, fix not yet committed.
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
changes. We added an `ALLOW_GLX=1` runtime switch in `main.cpp` and
`run_in_container.sh`:

```bash
ALLOW_GLX=1 ./scripts/run_in_container.sh    # forces xcb_glx
./scripts/run_in_container.sh                # default xcb_egl
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

1. **Drop the EGL force.** Remove the `qputenv("QT_XCB_GL_INTEGRATION",
   "xcb_egl")` and let Qt pick GLX (or run with `ALLOW_GLX=1`). On NVIDIA
   + libglvnd, `eglGetProcAddress("glEGLImageTargetTexture2DOES")` and
   the `EGLImageKHR` returned by `NvBufSurfaceMapEglImage` remain usable
   from a GLX context — they share libglvnd's EGL display. This is the
   working assumption for the next iteration of the component.
2. **Replace `QOpenGLWidget` with `QOpenGLWindow`** embedded via
   `QWidget::createWindowContainer`. This sidesteps QOpenGLWidget's FBO
   compose entirely, so the EGL integration can stay if zero-copy
   interop forces it later.
3. **File a Qt / NVIDIA reproducer.** Out of scope for the component,
   but worth doing once the demo itself is unblocked.

We will try (1) first. If EGLImage interop turns out to need a true EGL
context, fall back to (2).

## Diagnostic instrumentation kept in the tree

So the next person can re-run all three probes without rederiving them:

- `src/VideoGLWidget.cpp` — `paintGL` carries the UV-gradient shader, the
  purple `glClearColor`, and the 5-point `glReadPixels` probe printed once
  per second alongside the fps line.
- `src/main.cpp` — honours `ALLOW_GLX=1` to skip forcing `xcb_egl`.
- `scripts/run_in_container.sh` — passes `QT_XCB_GL_INTEGRATION=xcb_glx`
  through to the container when `ALLOW_GLX=1`.
- This document, plus `/tmp/xwd2pgm.py` (regenerate from this file's
  Probe 2 section if it has been deleted).

When the texture path comes back, **leave the FBO probe in place until
the texture render is verified on-screen** — otherwise we lose the
ability to tell "shader didn't sample" from "compositor swallowed it"
again.

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
  (`ALLOW_GLX=1`) in the tree is cheap and pays for itself the next
  time someone hits a related symptom.
