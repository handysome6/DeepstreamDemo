# 006 — P0.6 repeated stitch backlog exhausts device surfaces

## Status

Closed on 2026-05-01.

## Symptom

The first continuous P0.6 restream attempts looked healthy for a few seconds and
then deteriorated badly. Around the 15–20 second mark, the stitch path stopped
making forward progress and the logs filled with allocation failures like:

```text
createWrappedCudaSurface failed gpuId=0 size=3840x4320
```

At the same time:

- `stitched=` stopped increasing monotonically
- preview FPS collapsed
- restream `pushed=` fell behind or flatlined

The failure looked at first like a raw CUDA allocator problem, but the timing
pattern pointed to pressure buildup rather than a single bad allocation site.

## Root cause

Two independent ownership mistakes amplified each other:

1. `DualRtspSourcePair::maybeStitch()` emitted a successful stitched frame but
   did **not** clear `m_topFrame` and `m_bottomFrame`. That let the same paired
   inputs be stitched again on the next source callback instead of being
   consumed once.
2. `main.cpp` fanned the stitched frame out through queued handoffs to both the
   preview widget and the restream session. Under load, those queued deliveries
   could lag the producer and accumulate GPU-backed frame objects faster than
   they were retired.

So the system was not really processing “one fresh pair in, one stitched frame
out”. It was restitching stale pairs and buffering the results, which eventually
exhausted the available device-side surface allocations.

## Resolution

Three coordinated changes fixed it:

1. **Consume paired inputs exactly once.** After a successful stitch,
   `DualRtspSourcePair.cpp` now deletes `m_topFrame` and `m_bottomFrame` before
   emitting `newStitchedFrame(stitched)`.
2. **Remove unnecessary queued fanout.** `main.cpp` now hands the preview clone
   directly to `widget->onNewFrame(...)` and calls `restream->pushFrame(frame)`
   directly instead of queueing both deliveries through `QMetaObject::invokeMethod`.
3. **Bound downstream buffering.** `StitchRestreamSession` configures `appsrc`
   as a single-buffer leaky source so transient restream backpressure sheds old
   frames instead of creating an unbounded queue.

After the fix, both identity and calibrated smoke runs showed steady monotonic
progress:

```text
identity:   stitched=133 pushed=133 by uptime=00:00:11
calibrated: stitched=262 pushed=262 by uptime=00:00:19
```

No further `createWrappedCudaSurface failed` bursts appeared in the validated
runs.

## Lessons

- Multi-consumer GPU frame paths need explicit ownership rules; “just queue it
  to both consumers” is enough to hide backlog until the allocator fails.
- Pairwise stitch logic must be **consume-once**. Holding onto a last-good pair
  is acceptable for degraded-state display policy, but blindly restitching the
  same pair on every callback is not.
- If a GPU allocator appears to fail only after several seconds of apparently
  good runtime, inspect producer/consumer balance before blaming the allocator
  itself.
