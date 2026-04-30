# 004 — P0.5 DeepStream image entrypoint absorbs SIGTERM

## Status

Closed on 2026-05-01.

## Symptom

During the first round of P0.5 smoke testing, every harness wrapped in
`timeout 60 ./scripts/run_in_container.sh ...` left orphan containers and
host-side processes alive long after the timeout exit:

```text
$ timeout 60 ./scripts/run_in_container.sh ...
<exit 124>
$ ps aux | grep selective_yolo_infer | grep -v grep | wc -l
3        # three previous runs are still inside their containers
$ docker ps --filter ancestor=deepstream-demo/05-selective-yolo:latest
CONTAINER ID   ...   Up 4 minutes
CONTAINER ID   ...   Up 3 minutes
CONTAINER ID   ...   Up 2 minutes
```

Each "dead" run was actually still consuming GPU and continuing to log to its
log file even though the host script had returned 124. The next test then
collided with these zombies on the same RTSP source.

## Root cause

The DeepStream 9.0 image (`nvcr.io/nvidia/deepstream:9.0-triton-multiarch`)
ships an `ENTRYPOINT ["/opt/nvidia/deepstream/deepstream-9.0/entrypoint.sh"]`
that prints a license banner and then runs the user command **as a child of
bash**, not via `exec`. Verified with `ps`:

```text
PID    CMD
137189 /bin/bash /opt/.../entrypoint.sh /workspace/build/selective_yolo_infer ...
137268     /workspace/build/selective_yolo_infer ...
```

`timeout` sends SIGTERM to `docker run`, which delivers it to PID 1 inside the
container — which is the bash entrypoint script. Bash, by default, does not
forward SIGTERM to background jobs; only its own `wait` is interrupted. The
Qt binary therefore never sees the signal, keeps running, and the container
stays up after `timeout` gives up. P0.4's harness has the same shape but was
not exercised under `timeout` long enough to surface the bug — it ran almost
exclusively under interactive sessions where Ctrl-C reaches the foreground
process group directly.

## Resolution

Change `scripts/run_in_container.sh` so that:

1. `--init` makes tini PID 1 inside the container, which propagates signals
   to its children correctly.
2. `--entrypoint /workspace/build/selective_yolo_infer` skips the DeepStream
   bash entrypoint entirely — the binary is now PID 2 directly under tini, so
   SIGTERM lands on it.
3. The container is launched with a stable `--name`, and the host script
   installs an `EXIT INT TERM` trap that runs `docker stop "$NAME"` as a
   belt-and-suspenders cleanup if the host process is killed before the
   container exits.

After the fix, a `timeout 60 ./scripts/run_in_container.sh --quit-after-seconds 30 ...`
exits with status 0 (the binary's own `--quit-after-seconds` timer fires
cleanly) and `docker ps` shows no leftover containers. Repeated runs no
longer collide with zombies.

## Lessons

- "DeepStream image ships an entrypoint" looks harmless until you wrap it in
  any signal-driven harness (`timeout`, CI, supervisor restart). Whenever the
  base image's entrypoint is a non-`exec` shell, prefer `--entrypoint` to the
  binary directly, plus `--init`.
- The bug is invisible if you only validate interactively. Soak harnesses are
  the natural place to catch it because they always end via `timeout` or
  Ctrl-C.
- Future components inheriting the same Dockerfile should adopt the same
  `--init` + `--entrypoint <bin>` pattern. P0.4's harness should be revisited
  if it is ever exercised under `timeout`.
