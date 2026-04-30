# 003 — P0.3 `videotestsrc` 5-minute late-run degradation

## Status

Closed on 2026-04-30.

P0.3's soak/termination bug is fixed, and the apparent late-run
`videotestsrc` degradation was explained by the probe window being
backgrounded or de-focused. Because `t2` is sampled post-`frameSwapped`, the
measurement contract intentionally includes compositor / presentation
behavior for that window. The correct contract is therefore: baseline runs are
valid when the probe window stays foreground and unobscured.

## What was observed

Validation run:

```bash
cd components/03_latency_probe
DURATION=5m ./scripts/soak.sh
```

Evidence:

- Wrapper log: `components/03_latency_probe/logs/soak-videotestsrc-5m-postfix-wrapper.log`
- Soak log: `components/03_latency_probe/logs/soak-20260430-210510.log`
- CSV: `components/03_latency_probe/logs/probe-videotestsrc-20260430-130512.csv`

Healthy-path lifecycle behavior is now correct:

- `Auto-quit fired after 300 s`
- `missing-t0: NO`
- No lingering `p03-latency-probe-*` container after exit.

The issue is purely about latency behavior over a longer run.

## What the later experiments proved

The full 5-minute soak log did **not** indicate a true pipeline-side monotonic
latency drift. Later controlled runs showed that the elevated plateaus were
primarily caused by window state:

- when the probe window stayed foreground, steady-state `upload_paint p99`
  held around ~0.8–1.0 ms and `end_to_end p99` around ~3.2–5.5 ms;
- when the window was backgrounded or covered, `upload_paint` and
  `end_to_end` percentiles rose within a few seconds;
- switching the window back to foreground dropped the percentiles again.

So the apparent “late-run degradation” was really a display-contract issue:
post-`frameSwapped` `t2` is sensitive to compositor / presentation policy for
background windows.

### Representative windows

Early steady-state:

```text
block 020-024
  dq50 ≈ 2.34–2.57 ms
  up50 ≈ 1.10–1.16 ms
  e250 ≈ 3.51–3.88 ms
```

Mid-run stable platform:

```text
block 120-124
  dq50 ≈ 2.30–2.41 ms
  up50 ≈ 1.07–1.12 ms
  e250 ≈ 3.46–3.78 ms
```

First clear elevated plateau:

```text
block 200-204
  dq50 ≈ 3.77–3.86 ms
  up50 ≈ 2.06–2.31 ms
  e250 ≈ 6.18–6.57 ms
```

Later elevated plateau near the end:

```text
block 260-264
  dq50 ≈ 3.55–3.84 ms
  up50 ≈ 1.73–1.85 ms
  e250 ≈ 5.45–6.04 ms
```

End of run still elevated:

```text
block 280-284
  dq50 ≈ 3.61–3.85 ms
  up50 ≈ 1.77–1.91 ms
  e250 ≈ 5.44–5.97 ms
```

### Aggregated checkpoints from the full log

From a scripted pass over `soak-20260430-210510.log`:

- `first10` average `end_to_end p50` ≈ **4.60 ms**
- `mid10` average `end_to_end p50` ≈ **3.95 ms**
- `last20` average `end_to_end p50` ≈ **5.56 ms**

Rolling 20-window means:

- start: `e250 ≈ 4.38 ms`
- ~25%: `e250 ≈ 3.93 ms`
- ~50%: `e250 ≈ 4.07 ms`
- ~75%: `e250 ≈ 4.16 ms`
- end: `e250 ≈ 5.56 ms`

## Why this matters

This is **not** an isolated spike-only problem. Both `decode_queue p50` and
`upload_paint p50` rise together in the elevated windows. That suggests the
issue is not just a single `swapBuffers` / compositor blip — the whole
source→appsink→paint cadence loosens in the back half of the run.

Because P0.3 is the measurement baseline for later components, it cannot be
marked `ready` until this longer-run floor is either:

1. fixed, or
2. convincingly explained and re-contracted.

## Controlled experiments that closed the issue

### 1) Wrapperless direct run

Running:

```bash
./scripts/run_in_container.sh --duration-seconds 300
```

showed that the problem was not specific to `soak.sh`, but it also did not
reproduce a persistent pipeline-side drift when the probe window remained
active.

### 2) Unattended foreground run

A 90-second run with the probe window left untouched and active held the
expected steady-state band rather than rising into the late-run plateaus.

### 3) Manual foreground/background toggling

Interactive testing showed a sharp effect: clicking the probe window dropped
`p99` within a few seconds, and moving it to the background raised it again.

### 4) `wmctrl`-controlled A/B switching

Using `wmctrl`, focus was moved back and forth between the probe window and a
background editor window while the probe was running. The resulting logs showed
that:

- foreground windows held `upload_paint p99` roughly in the ~0.8–1.0 ms band;
- backgrounded windows raised `upload_paint p95/p99` and `end_to_end p95/p99`;
- returning the window to foreground restored the lower steady-state band.

That was sufficient to close the issue without changing code: the contract had
been under-specified, not violated.

## Resolution

This thread closed via the third exit path: the measurement contract was
revised, with evidence, to reflect an unavoidable display-side condition of the
current stack.

Accepted contract statement:

- `t2` is sampled post-`frameSwapped`, so display-side metrics include
  compositor / presentation behavior for the probe window.
- Baseline validation runs are valid when that window remains foreground and
  unobscured.
- Backgrounding or covering the probe window may inflate `upload_paint` /
  `end_to_end` percentiles without indicating any source / decode / appsink
  regression.
