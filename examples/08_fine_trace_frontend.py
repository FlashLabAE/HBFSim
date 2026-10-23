#!/usr/bin/env python3
"""Example 8 — synthetic 64-byte reads with an explicit frontend profile.

    python3 -m hbfsim example fine-frontend
    python3 examples/08_fine_trace_frontend.py --frontend sharing-only

This synthetic, open-loop demonstration is not hardware timing validation.
"""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from hbfsim.cli import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main(["example", "fine-frontend", *sys.argv[1:]]))
