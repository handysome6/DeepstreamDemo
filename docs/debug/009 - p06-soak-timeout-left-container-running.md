# 009 — P0.6 soak timeout left the container running

## Status

Closed on 2026-05-01.

## Symptom

A short wrapper run that was supposed to stop after `DURATION=2m` kept writing
stitched-frame logs for hours instead. The host workspace showed a large soak
log under `components/06_dual_4k_stitch_restream/logs/`, and `docker ps` still
showed the named P0.6 container alive long after the timeout window should have
ended.

## Root cause

`components/06_dual_4k_stitch_restream/scripts/soak.sh` uses:

```bash
timeout --foreground --signal=INT "$DURATION" "$HERE/run_in_container.sh" ...
```

but `run_in_container.sh` launched `docker run` in the foreground. In that
shape, the shell trap that calls `docker stop "$CONTAINER_NAME"` did not get a
reliable chance to run when the timeout interrupted the wrapper, so the Docker
CLI kept the container attached and the component continued running.

The core P0.6 pipeline was healthy; the bug was in the host-side wrapper's
process/signal ownership.

## Resolution

Change `run_in_container.sh` to launch `docker run` in the background and then
`wait` on its PID:

```bash
docker run ... &
DOCKER_RUN_PID=$!
wait "$DOCKER_RUN_PID"
```

With that structure, `EXIT` / `INT` / `TERM` traps stay owned by the shell
wrapper itself, so the cleanup path can reliably issue `docker stop` for the
named container when the soak timeout fires.

A component-local `.gitignore` was also added so `build/` and `logs/` stay out
of version control.

The wrapper was also made tolerant of `xhost` setup failure on headless shells:
`xhost +local:root` is now best-effort rather than fatal, so timeout/cleanup
behavior can still be validated from a non-GUI terminal.

A second wrapper bug surfaced during that verification: the script defaulted the
container's `DISPLAY` to `:0`, but its host-side `xhost` call did not use that
same default. The fix was to compute `DISPLAY_VALUE` once and reuse it for both
`xhost` and `docker run`, so the wrapper does not fail just because the host
shell omitted `DISPLAY`.

A direct verification then passed with:

```bash
timeout 20s ./scripts/run_in_container.sh \
  --top-uri rtsp://127.0.0.1:8554/cam4 \
  --bottom-uri rtsp://127.0.0.1:8554/cam5 \
  --calibration configs/identity_pair.json \
  --quit-after-seconds 600
```

The wrapper exited with timeout status `124`, and no `p06-dual-stitch-*`
container remained in `docker ps` afterward.

## Lessons

- When a wrapper relies on shell traps for Docker cleanup, do not leave `docker
  run` as an unmediated foreground process.
- If a soak command appears to exceed its requested duration, verify whether the
  timeout stopped the workload or only stopped supervising it.
- Keep generated soak logs ignored so a wrapper bug does not immediately pollute
  the working tree.
