#!/usr/bin/env python3
"""Example 8 — replay synthetic 64-byte reads with an explicit frontend profile.

    python3 examples/08_fine_trace_frontend.py
    python3 examples/08_fine_trace_frontend.py --frontend sharing-only

This is a small interface demonstration, not a captured model workload or a
hardware timing validation. All reads arrive at t=0 in one open-loop batch.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from hbfsim import open_fine_session  # noqa: E402
from hbfsim.runner import prepare_output_directory  # noqa: E402
from hbfsim_client import Transaction  # noqa: E402


PAGE = 4096
ACCESS = 64


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=None, help="fresh output directory")
    parser.add_argument("--frontend", default="fine-v1",
                        choices=("fine-v1", "page-mshr", "sharing-only", "legacy"))
    parser.add_argument("--pages", type=int, default=64, help="read-only 4 KiB pages")
    args = parser.parse_args()
    if not 1 <= args.pages <= 4096:
        parser.error("--pages must be between 1 and 4096 for this small example")
    out = prepare_output_directory(args.out, "example-fine-frontend")
    reads = [Transaction(id=f"r{page}-{offset}", target="HBF_LOGICAL", op="R",
                         addr=page * PAGE + offset, bytes=ACCESS, issue_ns=0.0)
             for page in range(args.pages) for offset in range(0, PAGE, ACCESS)]
    with open_fine_session("4hbm-4hbf", frontend=args.frontend, out_dir=out,
                           initial_hbf_logical_pages=args.pages) as session:
        result = session.run(reads, completions=False)
    summary = {
        "workload": "synthetic read-only 64-byte accesses, page-major order",
        "frontend": args.frontend,
        "requests": len(reads),
        "logical_read_bytes": len(reads) * ACCESS,
        "serving_ns": result.elapsed_ns,
        "timing_scope": "memory-only, open-loop; excludes terminal drain",
    }
    (out / "example-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{args.frontend}: {len(reads):,} x {ACCESS} B reads over {args.pages} pages")
    print(f"Simulated serving time: {result.elapsed_ns / 1000:.3f} us (terminal drain excluded)")
    print("Synthetic API demonstration; not a hardware or model-inference benchmark.")
    print(f"Resolved frontend: {out / 'frontend-receipt.json'}")
    print(f"Session receipts and summary: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
