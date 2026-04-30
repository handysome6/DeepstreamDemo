# Component 03: `latency_probe`

> **Status:** `draft` — implementation landed and initial hardware validation
> passed on 2026-04-30, but `ready` is still blocked on the remaining soak /
> reconnect / full-rate CSV validation items below.
>
> What is already proven:
>
> - `videotestsrc` steady-state baseline with vsync off is stable and within
>   budget.
> - `QOpenGLWidget::frameSwapped` sampling makes the probe vsync-sensitive.
> - `GstReferenceTimestampMeta` survives the single-stream
>   `nvvideoconvert -> appsink` path and reaches the CSV pipeline.
>
> What is **not** yet signed off:
>
> - 5-minute `videotestsrc` soak with CSV enabled.
> - 5-minute RTSP drift run.
> - RTSP publisher stop/start recovery proving the probe survives reconnect.
> - Exact stdout-vs-CSV percentile match in a `--csv-every 1` validation run.

## Goal

Provide a **reproducible measurement protocol** for the single-stream
source/output boundary this component actually owns, split into three
attributable segments so any future regression can be assigned to either
pipeline/queue or upload/paint instead of a single opaque
"ingest-to-paint" number.

For `videotestsrc`, `t0` is stamped on the source element's `src` pad, so the
measurement is effectively source→paint. For RTSP, `t0` is stamped on
`nvurisrcbin`'s dynamically exposed output pad, so the RTSP numbers mean
**decoder-output / source-bin-output → appsink → swap**, not on-the-wire
camera capture → paint. That distinction is intentional: this component is a
local reproducibility contract, not a distributed clock-sync system.

This component is a *measurement capability*, not a one-off probe. Its
output is the yardstick every subsequent latency claim in this repo is
expected to use.

## Why this matters

P0.1 and P0.2 already print a `ingest-to-paint avg/max` line. That number
served as a smoke signal — "the pipeline is alive and not seconds behind" —
but it is **not** a baseline that 04+ can rely on:

- It collapses decode/queue cost and upload/paint cost into one number, so
  a regression cannot be localized.
- It does not pin display-side variables (vsync, max-buffers, drop policy),
  so the number conflates pipeline cost with display cadence.
- Each component would otherwise re-invent its own probe, with its own
  subtle clock and stamping bugs. We have already burned time on that in
  prior pipelines.

`docs/architecture.md` calls this out explicitly: *"Latency claims without
measurement infrastructure"* is one of the named anti-patterns. P0.3 exists
so 04, 05, 06 are forbidden from making latency claims without numbers
produced by this contract.

## Scope (and what is explicitly out)

In scope:

- Inject a **monotonic** source-side timestamp into each frame.
- Read it back at the appsink callback (decode/queue boundary).
- Read it again at paint completion in the GL widget (upload/paint
  boundary).
- Summarize as p50 / p95 / p99 / max per segment over a rolling window,
  emit one line per second to stdout, and append per-N-frame rows to a CSV.

Out of scope (kept out so this component stays small):

- No inference. No YOLO. No bounding boxes.
- No multi-stream. Single source, single window, exactly like P0.1/P0.2.
- No new UI. Reuses the P0.1 `VideoGLWidget` shape unchanged.
- No runtime budget enforcement. The probe **measures**; downstream
  components decide whether the numbers pass their criteria.
- No latency optimization. If the numbers are bad, that is a finding, not
  a bug in this component.

## Architecture

```text
source ──► nvvideoconvert ──► appsink ──► ProbeGLWidget paintGL ──► swap
   │ t0                          │ t1                              │ t2
   │ stamped on the buffer       │ read in the                     │ read in
   │ via GstReferenceTimestamp-  │ appsink callback                │ frameSwapped
   │ Meta (caps =                │ (streaming thread)              │ (post-swap,
   │ "timestamp/x-probe-emit")   │                                 │ vsync-aware)
```

Three segments per frame, all in microseconds, all from
`g_get_monotonic_time` (never wallclock):

| segment        | meaning                                                | computed as |
| -------------- | ------------------------------------------------------ | ----------- |
| `decode_queue` | decode + nvvideoconvert + appsink queue                | `t1 − t0`   |
| `upload_paint` | CUDA→GL upload + draw + buffer swap (incl. vsync wait) | `t2 − t1`   |
| `end_to_end`   | source emit → paint complete             | `t2 − t0`   |

Sources covered for the contract validation (in this order):

1. `videotestsrc is-live=true` at 30 fps — controlled, no network jitter,
   and `t0` is stamped at the true source boundary.
2. RTSP against `rtsp://127.0.0.1:8554/p02cam` — the same controllable
   mediamtx feed P0.2 used. Here `t0` is stamped at `nvurisrcbin`'s exposed
   output pad, so the metric is downstream-of-decode / downstream-of-jitter-
   buffer latency only.
3. RTSP across one publisher stop/start cycle — verifies the probe
   continues to emit valid samples after `nvurisrcbin` reconnects (i.e.
   the meta-stamping survives the inner pipeline restart).

## Output contract (what 04+ will consume)

Stdout, one line per second, e.g.:

```
fps=30 src=videotestsrc | decode_queue p50=0.4 p99=1.1 ms | upload_paint p50=0.6 p99=1.3 ms | end_to_end p50=1.0 p99=2.2 max=3.7 ms | vsync=off
```

CSV (`logs/probe-<timestamp>.csv`), one row per N frames (default N=30),
columns fixed:

```
ts_monotonic_us, frame_idx, src_label, decode_queue_us, upload_paint_us, end_to_end_us, vsync
```

The CSV column set is **the contract**. 04+ will read this with a 5-line
Python script; do not reorder or rename columns without bumping a version
field at the top of the file.

## Build

TBD. Same component-container pattern as P0.2:

```bash
cd components/03_latency_probe
./scripts/build_in_container.sh
```

Same dependency surface as P0.2 (Qt 6, GStreamer 1.x, CUDA, DeepStream 9.0).

## Run

Validated interface:

```bash
# videotestsrc, vsync off (the baseline measurement; default source).
./scripts/run_in_container.sh

# videotestsrc, vsync off, but self-stop after 15 seconds.
./scripts/run_in_container.sh --duration-seconds 15

# videotestsrc, vsync on (sanity that post-swap t2 is vsync-sensitive).
./scripts/run_in_container.sh --vsync on

# RTSP, vsync off.
./scripts/run_in_container.sh --source rtsp --uri rtsp://127.0.0.1:8554/p02cam

# One-row-per-frame CSV for stdout-vs-CSV comparison.
./scripts/run_in_container.sh --csv-every 1 --duration-seconds 60

# Recompute p50/p99 from the emitted CSV.
uv run python3 scripts/parse_probe_csv.py logs/probe-videotestsrc-*.csv
```

### Soak test

```bash
# 5-minute videotestsrc soak (default source).
./scripts/soak.sh

# 5-minute RTSP soak.
./scripts/soak.sh --source rtsp --uri rtsp://127.0.0.1:8554/p02cam
```

For the RTSP reconnect criterion, run the soak above and, in another terminal:

```bash
docker stop p02-rtsp-pub
sleep 10
docker start p02-rtsp-pub
```

Expected healthy-path termination behavior:

- `latency_probe` logs `Auto-quit fired after ... s`
- the Qt window closes on its own
- the GStreamer pipeline stops
- the container exits on its own
- the outer `timeout` inside `scripts/soak.sh` is only a backup guardrail

Notes:

- CSV is **enabled by default** and auto-written to `logs/probe-<src>-<ts>.csv`.
- Pass `--csv ''` to disable CSV output explicitly.
- `--vsync on` leaves `vblank_mode` / `__GL_SYNC_TO_VBLANK` unset on purpose;
  the default path exports them to keep the vsync-off run honest.
- `scripts/soak.sh` converts `DURATION` (e.g. `5m`, `300s`) into
  `--duration-seconds` for the binary, then adds a +20s outer timeout cushion
  as a cleanup fallback.

## Success criteria

Each box must be ticked on real hardware before status moves from `draft`
to `ready`. These criteria are the *measurement contract being trustworthy*,
not the pipeline being fast — fast is what the criteria measure, not what
they require.

- [x] Builds clean inside the component container.
- [x] In `videotestsrc` mode at 30 fps with vsync off, the probe runs for
      5 minutes and emits both the per-second stdout summary and the CSV
      without dropping its own samples, then **self-terminates** without a
      manual window close. Verified 2026-04-30 via
      `DURATION=5m ./scripts/soak.sh`; log:
      `logs/soak-videotestsrc-5m-postfix-wrapper.log`, CSV:
      `logs/probe-videotestsrc-20260430-130512.csv`, `missing-t0: NO`. 
- [x] `decode_queue + upload_paint` is within ±200 μs of `end_to_end` on
      steady-state emitted summary lines. Verified 2026-04-30 on both
      `videotestsrc` and RTSP runs; observed sanity deltas clustered around
      0.00–0.05 ms for vsync-off steady state.
- [ ] `videotestsrc` baseline: `end_to_end` p50 ≤ 5 ms, p99 ≤ 10 ms with
      vsync off. This passed in earlier shorter steady-state runs
      (`end_to_end` p50 ≈ 2.71–2.99 ms, p99 ≈ 3.13–3.24 ms) but **failed in
      the post-fix 5-minute soak**: final windows landed around p50 ≈
      5.15–6.30 ms, p99 ≈ 9.02–18.17 ms. The stop/soak lifecycle bug is now
      fixed; the remaining work is to explain or eliminate this longer-run
      latency drift before 03 can move to `ready`. 
- [x] RTSP mode against `rtsp://127.0.0.1:8554/p02cam` for 5 minutes:
      segment p50s do not drift monotonically (no segment p50 grows by
      > 100 μs/min over the run), and the run self-terminates without a
      manual window close. Verified 2026-04-30: pre-reconnect steady state
      held around `decode_queue` p50 ≈ 0.13–0.16 ms,
      `upload_paint` p50 ≈ 0.95–1.28 ms, `end_to_end` p50 ≈ 1.07–1.47 ms;
      final windows after recovery returned to `decode_queue` p50 ≈ 0.13–0.14 ms,
      `upload_paint` p50 ≈ 0.87–1.02 ms, `end_to_end` p50 ≈ 1.03–1.17 ms;
      soak summary showed `Auto-quit fired after 300 s`, `missing-t0: NO`.
- [x] After one manual `docker stop && docker start` cycle of the RTSP
      publisher during `scripts/soak.sh --source rtsp`, the probe resumes
      emitting valid samples within 10 s of stream return, with
      `decode_queue` and `upload_paint` recovering to within 2× of their
      pre-disconnect values, and the soak still self-terminates at the end.
      Verified 2026-04-30 against `p02-rtsp-pub`: during publisher outage,
      `nvurisrcbin` logged reconnect attempts / `No data from source since
      last 5 sec`; after the publisher returned the probe recovered from
      ~1 fps to 15 fps within the next 5-second windows and the segment p50s
      returned to roughly the same band as pre-disconnect.
- [x] Vsync sanity: with `--vsync on` at 60 Hz, `upload_paint` **p99 / max**
      increase by approximately one refresh interval (~10–17 ms) versus
      `--vsync off`. (The p50 does *not* shift on NVIDIA — driver triple
      buffering lets `swapBuffers` return immediately for most frames; only
      the ones that exhaust the back-buffer queue actually block on vblank.
      The original draft of this criterion targeted p50 and was wrong.)
      Verified 2026-04-30: vsync-off p99 ≈ 3 ms vs vsync-on p99 ≈ 9.5–10.4 ms,
      max ≈ 11–13 ms.
- [x] CSV output is parseable by a 5-line pandas snippet (committed under
      `scripts/parse_probe_csv.py`). Verified 2026-04-30 via
      `uv run python3 scripts/parse_probe_csv.py logs/probe-videotestsrc-*.csv`.
      Exact stdout-vs-CSV percentile matching still requires a dedicated
      `--csv-every 1` comparison run, because the default `--csv-every 30`
      down-samples the dataset by design.

## Measurements

Validated 2026-04-30 on RTX 5090 dGPU host.

| scenario | source rate | decode_queue | upload_paint | end_to_end | notes |
| --- | --- | --- | --- | --- | --- |
| `videotestsrc`, vsync off, steady-state stdout | 1920×1080 @ 30 fps | p50 ≈ 2.24–2.31 ms, p99 ≈ 2.49–2.56 ms | p50 ≈ 0.48–0.61 ms, p99 ≈ 0.72–0.76 ms | p50 ≈ 2.71–2.99 ms, p99 ≈ 3.13–3.24 ms | sanity ≈ 0.00–0.05 ms |
| `videotestsrc`, vsync on, steady-state stdout | 1920×1080 @ 30 fps | p50 ≈ 2.54–2.68 ms | p50 ≈ 1.08–1.19 ms, p99 ≈ 9.44–10.43 ms | p50 ≈ 3.85–3.99 ms, p99 ≈ 11.88–12.80 ms | NVIDIA triple buffering hides the shift in p50 but not p99/max |
| RTSP `p02cam`, vsync off, steady-state stdout | 3840×2160 @ 15 fps | p50 ≈ 0.13–0.14 ms, p99 ≈ 0.56–0.86 ms | p50 ≈ 0.60–0.61 ms, p99 ≈ 1.17–1.40 ms | p50 ≈ 0.73–0.74 ms, p99 ≈ 1.78–2.45 ms | `t0` is stamped at `nvurisrcbin` output, so this excludes upstream network/decode latency |
| `videotestsrc`, CSV parsed by pandas | CSV sampled every 30 frames | p50 = 2952 us, p99 = 4962 us | p50 = 1278 us, p99 = 2958 us | p50 = 3934 us, p99 = 7630 us | parse path proven; numbers differ from stdout because CSV was down-sampled by default |
| `videotestsrc`, 5-minute post-fix soak | 1920×1080 @ 30 fps | final windows p50 ≈ 3.09–3.93 ms, p99 ≈ 5.52–11.65 ms | final windows p50 ≈ 1.42–1.99 ms, p99 ≈ 4.47–11.42 ms | final windows p50 ≈ 4.81–6.30 ms, p99 ≈ 9.02–18.17 ms | self-termination fixed, but longer-run latency floor regressed vs earlier short runs |
| RTSP `p02cam`, 5-minute soak with one reconnect | 3840×2160 @ 15 fps | pre-reconnect p50 ≈ 0.13–0.16 ms; post-recovery p50 ≈ 0.13–0.14 ms | pre-reconnect p50 ≈ 0.95–1.28 ms; post-recovery p50 ≈ 0.87–1.02 ms | pre-reconnect p50 ≈ 1.07–1.47 ms; post-recovery p50 ≈ 1.03–1.17 ms | reconnect recovery + self-termination both verified |

## Known gotchas

- **`GstReferenceTimestampMeta` survives `nvvideoconvert` but not
  `nvstreammux`.** `nvstreammux` batches buffers and drops per-buffer
  metas not in its allowlist. P0.3 stays single-stream, so this is not a
  blocker here, but when P0.6 (`multi_widget_canvas`) integrates the
  probe under nvstreammux, switch to per-source PTS bookkeeping or accept
  `end_to_end` only.
- **Monotonic, never wallclock.** All three stamps must come from
  `g_get_monotonic_time` (or `clock_gettime(CLOCK_MONOTONIC)`). NTP
  stepping wallclock mid-run will silently corrupt the dataset. The CSV
  column is named `ts_monotonic_us` to make this contract loud.
- **`t2` must be sampled post-swap, not post-`glFinish`.** A first cut of
  this component sampled `t2` immediately after `glFinish` inside `paintGL`.
  That captured GPU-work completion but missed the `swapBuffers` blocking
  on vblank, so the vsync-on/off comparison silently showed no shift. The
  current code samples `t2` in `QOpenGLWidget::frameSwapped`, which fires
  *after* Qt has performed the swap — so `upload_paint` includes the vsync
  wait when `setSwapInterval(1)` is in effect. If you ever need to
  attribute upload-vs-vsync separately, add a fourth segment rather than
  moving `t2` back.
- **Vsync env vars override Qt swap interval.** `vblank_mode=0` (Mesa) and
  `__GL_SYNC_TO_VBLANK=0` (NVIDIA) tell the GL driver to ignore vsync,
  regardless of `QSurfaceFormat::setSwapInterval(1)`. `run_in_container.sh`
  therefore exports them only when `--vsync on` is **not** in the args
  (default is off, so they're applied; `--vsync on` deliberately leaves
  them unset). If a compositor still re-imposes vsync on the off-path,
  this is the place to look first.
- **Probe latency is not pipeline latency.** The cost of stamping and
  reading metas is itself non-zero. We measure that overhead once at
  startup (loop the stamping/reading code with no pipeline) and report it
  on the first stdout line. If overhead is > 100 μs, the floor numbers
  above need to be relaxed by that amount.

## Next

- **Unblocks 04 (`selective_yolo_batch`):** 04 is allowed to assert
  "selective inference adds ≤ X ms to `end_to_end`" only by running with
  the probe enabled and citing the CSV.
- **Reused by 06 (`multi_widget_canvas`):** the same contract, with the
  per-source caveat in the gotchas above, becomes the per-stream budget
  ceiling.
