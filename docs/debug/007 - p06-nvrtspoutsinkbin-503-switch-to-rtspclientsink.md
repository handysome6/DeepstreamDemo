# 007 — P0.6 `nvrtspoutsinkbin` returns 503; switch restream publish to `rtspclientsink`

## Status

Closed on 2026-05-01.

## Symptom

The first P0.6 restream implementation used a local `appsrc -> nvvideoconvert
-> nvrtspoutsinkbin` branch and reported a plausible ready line:

```text
restream READY url=rtsp://127.0.0.1:8556/p06stitch ...
```

But external readers consistently failed:

```text
Service Unavailable (503)
```

That initially looked like a bad handoff of wrapped `NvBufSurface` buffers into
`appsrc`, which would have been a serious GPU-memory integration bug.

## Root cause

A minimal control experiment showed the problem was **not** specific to the
stitched output at all. Even a stripped-down test publisher that bypassed the
P0.6 stitch path still produced `503 Service Unavailable` when exposed through
`nvrtspoutsinkbin` in this environment.

That meant the failure was below the stitch layer: the self-hosted publish route
was not trustworthy enough to serve as the readiness path for P0.6.

A follow-on `gst-rtsp-server` attempt also failed to become the final solution:
its media graph exposed encoder/parser/payloader elements, but the expected
`appsrc` could not be found in the configured RTSP media tree, so the session
never became a working publish target.

## Resolution

The validated restream path was changed to:

```text
appsrc -> nvvideoconvert -> NV12 NVMM -> nvv4l2h265enc -> h265parse -> rtspclientsink
                                                   └─► local mediamtx path /p06stitch
```

This turned the component from “host its own RTSP endpoint” into “publish by
RTSP RECORD to an already-running local RTSP server”, which matched the local
lab architecture better and was directly verifiable.

Successful proof points:

```text
restream READY url=rtsp://127.0.0.1:8554/p06stitch ... via=rtspclientsink
```

mediamtx confirmed publication:

```text
[path p06stitch] stream is available and online, 1 track (H265)
[RTSP] [session ...] is publishing to path 'p06stitch'
```

An external reader then attached successfully over TCP and stayed connected
until probe timeout.

## Lessons

- When a publish path fails, isolate it with a minimal pipeline before blaming
  the application’s frame ownership or CUDA memory contract.
- `nvrtspoutsinkbin` being present in the image is not the same thing as it
  being the best production-facing readiness path for a local lab.
- For this repo’s current lab shape, `rtspclientsink -> mediamtx` is the more
  reliable restream contract because mediamtx already provides the observable
  external endpoint and diagnostics.
