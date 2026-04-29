# Component 02: `nvurisrcbin_reconnect`

> **Status:** `draft` — code in place, container build passes, and a local
> `rtsp://127.0.0.1:8554/cam0` smoke test reaches live video over the GPU-only
> path. The 30-min soak / 3-disconnect / real-camera criteria are still
> unverified.

## Goal

Prove that a single live RTSP source can run through the dGPU GPU-only display
contract from P0.1 **without crashing across stream loss and reconnect**:
nvurisrcbin's internal reconnect logic must keep the pipeline alive across
network drops, the appsink must not back up, and the displayed picture must
return on its own when the source returns.

## Why this matters

P0.1 only proved the *display* path: synthetic NVMM frames reaching a Qt
window over a CUDA-GL interop. It said nothing about real RTSP, and real RTSP
is the hard part of production:

- Cameras drop connections.
- Network jitter.
- Encoding parameter changes mid-stream.
- Camera reboots.
- DNS / mediamtx restarts.

If we can't survive these on a single stream, multi-stream tiling and
selective inference are pointless. P0.2 is therefore the gate between
"we have a display path" and any of the multi-source / inference work that
follows.

## Architecture

```text
nvurisrcbin uri=rtsp://...
  (rtsp-reconnect-interval=5s, rtsp-reconnect-attempts=0,
   latency=0ms, drop-on-latency=true, select-rtp-protocol=tcp)
    │   pad-added (dynamic) — linked at runtime to nvvideoconvert sink
    ▼
nvvideoconvert
    │
    ▼  video/x-raw(memory:NVMM), format=RGBA   (NVBUF_MEM_CUDA_DEVICE, pitch-linear)
appsink (emit-signals=true, sync=false, max-buffers=1, drop=true)
    │   GST streaming thread, same FrameHolder contract as P0.1
    ▼   Qt::QueuedConnection
VideoGLWidget::onNewFrame  (CUDA-GL interop, DeviceToDevice copy)
    ▼
swap buffers
```

**GPU-only claim:** the path from `nvurisrcbin` to the GL texture is
identical to P0.1's once the source pad is linked — same `NvBufSurface` →
`cudaMemcpy2DToArray(..., DeviceToDevice)`. No host round-trip is added by
swapping the source. The CPU never touches a frame byte.

**Reconnect strategy:** delegated to `nvurisrcbin`. The bin internally
restarts its inner `rtspsrc` / `nvv4l2decoder` chain on disconnect; from this
component's perspective frames simply stop and resume on the same `appsink`.
We track stream health with a heartbeat (time since last frame) rather than
parsing private bus messages — it's a more reliable proxy for what the user
actually cares about.

## Why these defaults

| property                  | value | reason                                                                                          |
| ------------------------- | ----- | ----------------------------------------------------------------------------------------------- |
| `latency`                 | 0     | P0.2 的目标就是先压到最低 live jitter buffer，验证极限设置下是否还能稳定重连。必要时再回调到 100 ms 做对照。 |
| `drop-on-latency`         | true  | 经 `gst-inspect` 与 `gst-launch` 验证，`nvurisrcbin` 直接暴露这个属性；它会传给内部 jitterbuffer，防止积压。 |
| `rtsp-reconnect-interval` | 5 s   | 最低仍算温和的探测周期；0 会彻底关闭 reconnect。 |
| `rtsp-reconnect-attempts` | 0     | 无限重试 —— 生产相机可能会离线任意时长。 |
| `select-rtp-protocol`     | TCP   | 默认 `udp+udpmcast+tcp` 会优先走 UDP；这里先固定 TCP，把“不稳定”收敛在相机/链路本身，而不是 UDP 丢包。 |
| stall-timeout             | 2 s   | 两秒无帧即视为断流；15–30 fps 摄像头在正常抖动下远达不到这个阈值。 |

## Build

```bash
cd components/02_nvurisrcbin_reconnect
./scripts/build_in_container.sh
```

First run builds the component-specific image
(`deepstream-demo/02-nvurisrcbin:latest`); subsequent runs just compile.

Dependencies (same surface as P0.1):

- Qt 6 (Core, Gui, Widgets, OpenGL, OpenGLWidgets)
- GStreamer 1.x (`gstreamer-1.0`, `gstreamer-app-1.0`, `gstreamer-video-1.0`)
- CUDA toolkit (`CUDA::cudart`, `cuda_gl_interop.h`)
- DeepStream SDK (for `nvurisrcbin`, `nvvideoconvert`, `libnvbufsurface.so`)
- NVIDIA driver supporting CUDA-GL interop on desktop GL

## Run

The default URI points at the local mediamtx test feed
(`rtsp://127.0.0.1:8554/cam0`) used during P0.1 validation. To run against a
real production camera, override `RTSP_URL`:

```bash
# Local mediamtx (default).
./scripts/run_in_container.sh

# Production camera, TCP transport.
RTSP_URL='rtsp://user:pass@cam01.lan/Streaming/channels/101' \
  ./scripts/run_in_container.sh

# Force UDP transport for diagnostics.
./scripts/run_in_container.sh --rtp udp
```

A 720p window opens. Stdout once per second prints, e.g.:

```
fps=15 ingest-to-paint avg=0.6 ms max=1 ms | tex=1920x1080 | uptime=00:00:09 state=LIVE last-frame-age=30ms reconnects=0 stalls=0 frames=124
```

Measured on 2026-04-30 against the local mediamtx lab feed
`rtsp://127.0.0.1:8554/cam0` (1920×1080 @ 15 fps). The same status line is
also drawn into the window as a translucent top overlay so a human watcher can
confirm reconnect behavior without tailing stdout.

### Soak test

`scripts/soak.sh` wraps `run_in_container.sh` with a `timeout`-bound run and
captures the log to `logs/soak-<timestamp>.log`. To validate the
"3 disconnects, all recover" criterion, in another terminal stop and start
the publisher container three times during the 30-minute window:

```bash
# in terminal 1:
DURATION=30m ./scripts/soak.sh

# in terminal 2, every ~7 minutes:
docker stop deepstreampoc-rtsp-pub-0 && sleep 30 && docker start deepstreampoc-rtsp-pub-0
```

(Replace `deepstreampoc-rtsp-pub-0` and `cam0` with whatever publisher /
stream you set `RTSP_URL` to.)

## Success criteria

Each box must be ticked on real hardware before status moves from `draft` to
`ready`.

- [x] Builds clean in the component container.
- [x] Short smoke test reaches live video against
      `rtsp://127.0.0.1:8554/cam0` with the expected `nvurisrcbin -> nvvideoconvert -> RGBA NVMM -> appsink` path.
- [ ] Runs continuously for **30 minutes** against
      `rtsp://127.0.0.1:8554/cam0` (the local mediamtx test feed) without
      crashing or exiting.
- [ ] During the 30-min run, the publisher is stopped and started **3 times**;
      the stdout log shows `stream STALLED` followed by `stream LIVE` for each
      cycle, and the displayed window recovers visible video each time.
- [ ] Real-camera variant (`RTSP_URL=rtsp://...real camera...`) runs end-to-end
      and survives a camera reboot (camera power-cycled — should be detected
      as one stall + one recover).
- [ ] `ingest-to-paint` p50 stays within 2× of the P0.1 floor (P0.1 measured
      0.6 ms avg; budget here is ≤ 1.5 ms steady-state once the stream is
      live, since RTSP adds inevitable jitter on the source side but should
      not affect the GPU upload itself).
- [ ] `nvidia-smi --query-gpu=memory.used` of the demo process at minute 30
      is within ±50 MiB of the value at minute 5 (no monotonic VRAM growth).
- [ ] CPU usage of the demo process stays under 100% of one core in steady
      state (i.e. no accidental CPU-side decode or readback).

## Measurements

Fill in once verified.

| metric                                          | value |
| ----------------------------------------------- | ----- |
| short smoke test (`cam0`, ~9 s) reached live video | yes |
| short smoke test source rate                       | 1920×1080 @ 15 fps |
| 30-minute soak completed without crash             | _tbd_ |
| reconnects observed across 3 manual disconnects    | _tbd_ |
| ingest-to-paint avg latency (short smoke)          | 0.4–0.7 ms steady |
| ingest-to-paint max latency (short smoke)          | 1–4 ms |
| CPU usage (demo process, % of one core)            | _tbd_ |
| VRAM at minute 5 vs minute 30                      | _tbd_ |
| GPU SM utilization                                 | _tbd_ |

## Known gotchas

- **`nvurisrcbin` exposes its src pad dynamically.** It does *not* expose a
  static "src" pad you can `gst_element_link` to. The bin only emits its src
  pad after it has resolved the URI and instantiated the inner decoder, which
  happens asynchronously. We connect to `pad-added` and link there.
- **Inner errors during disconnect are normal.** When the RTSP source drops,
  the inner `rtspsrc` / `nvv4l2decoder` posts `GST_MESSAGE_ERROR` on the bus
  with messages like *"Could not read from resource"*. These are **not
  fatal** — `nvurisrcbin` catches them and triggers its own reconnect.
  `RtspSource::handleBusError` therefore filters errors that originate from
  inside the bin and only propagates errors from the top-level pipeline as
  fatal. If you see the demo quit on disconnect, this filter is the place to
  look first.
- **`type` enum has only 3 values.** Valid `type` values on this DS9.0
  build are 0 (auto), 1 (uri), 2 (rtsp-with-smart-record). Do not pass 4 — it
  silently leaves the property at its previous value but logs a GLib warning.
  We rely on `type=0` (auto) since it correctly routes `rtsp://` URIs through
  the rtsp-aware code path; setting `type=2` is only needed for smart record.
- **TCP, not the default `udp+udpmcast+tcp`.** The default
  `select-rtp-protocol=7` (the `rtp-udp-udpmcast-tcp` flag combo) prefers UDP
  and silently degrades to lossy reception under any cross-subnet routing.
  We default to TCP because production deployment is across a LAN router,
  and the silent UDP-loss failure mode is much worse than the slight latency
  cost of TCP.
- **Stall heartbeat is in the GUI thread.** `pollHealth` is invoked by a
  `QTimer` at 5 Hz on the GUI thread, not by GStreamer. It reads atomics
  written by the streaming thread. The reconnect counter is therefore
  accurate but slightly delayed (≤ 200 ms).
- **Pad-added fires only when video appears.** If the URI is wrong or the
  camera doesn't authenticate, no pad will ever be added — the binary will
  sit in `state=STALL last-frame-age=--` indefinitely, with bus errors
  visible in stdout. That's the intended behavior; it is *not* a hang in our
  code.
- **This DS9 container does not export the DeepStream plugin directory by default.**
  `libgstnvvideoconvert.so` exists at
  `/opt/nvidia/deepstream/deepstream-9.0/lib/gst-plugins/libgstnvvideoconvert.so`,
  but a plain `docker run ... gst-inspect-1.0 nvvideoconvert` reports
  *"No such element"* unless `GST_PLUGIN_PATH` includes that directory. The
  component run/build scripts now export both `GST_PLUGIN_PATH` and
  `LD_LIBRARY_PATH`; if `nvvideoconvert` suddenly disappears again, check the
  environment before assuming the image is broken.
- **`drop-on-latency` really is exposed by `nvurisrcbin` on DS9.0.**
  `gst-inspect-1.0 nvurisrcbin` shows it as a top-level boolean property, and a
  `gst-launch-1.0 nvurisrcbin ! nvvideoconvert ! fakesink -v` capture confirms
  it propagates down to the inner `GstRtpJitterBuffer`. So for P0.2 we set it
  directly on `nvurisrcbin`, plus keep `appsink max-buffers=1 drop=true` at the
  tail for display-side backpressure protection.

## Next

- **Component 03** (`latency_probe`) gives us a measurement floor that lets
  us *prove* the latency claim above rather than asserting it.
- **Component 06** (`multi_widget_canvas`) extends to N independent
  `RtspSource`s and N CUDA-bound textures composited inside one
  `QOpenGLWidget`. P0.2 is the per-source contract that 06 will lean on.
