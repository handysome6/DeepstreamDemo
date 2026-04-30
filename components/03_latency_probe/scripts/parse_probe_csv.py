#!/usr/bin/env python3
"""
Recompute per-segment p50/p99 from a latency_probe CSV.

Used by the P0.3 README success criterion: "CSV output is parseable by a
5-line pandas snippet, and that snippet's per-segment p50/p99 match the
stdout summary within rounding."

Usage:
    ./scripts/parse_probe_csv.py logs/probe-videotestsrc-20260430-101500.csv
"""
import sys
import pandas as pd

path = sys.argv[1] if len(sys.argv) > 1 else "logs/probe.csv"
df = pd.read_csv(path, comment="#")
for col in ("decode_queue_us", "upload_paint_us", "end_to_end_us"):
    print(f"{col:<18} p50={df[col].quantile(0.50):8.0f} us  p99={df[col].quantile(0.99):8.0f} us  max={df[col].max():8.0f} us  n={len(df)}")
