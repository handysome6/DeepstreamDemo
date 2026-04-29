#!/usr/bin/env bash
# Runs the host-built binary directly (no container) against an RTSP URI.
# Use scripts/run_in_container.sh on systems where Qt6 / DeepStream is only
# available inside the container.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BIN="$ROOT/build/nvurisrcbin_reconnect"

if [[ ! -x "$BIN" ]]; then
    echo "Binary not found at $BIN. Build first." >&2
    exit 1
fi

URI="${1:-${RTSP_URL:-rtsp://127.0.0.1:8554/cam0}}"
shift || true

export QT_XCB_GL_INTEGRATION=xcb_glx
exec "$BIN" --uri "$URI" "$@"
