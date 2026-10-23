"""Synthetic fine-frontend demonstration shared by the CLI and example script."""

from __future__ import annotations

import json
from pathlib import Path

from hbfsim.session import open_fine_session
from hbfsim.runner import prepare_output_directory
from hbfsim_client import Transaction

PAGE = 4096
ACCESS = 64


def run_fine_frontend(*, pages: int, frontend: str, out_dir: Path | None) -> int:
    out = prepare_output_directory(out_dir, "example-fine-frontend")
    reads = [Transaction(id=f"r{page}-{offset}", target="HBF_LOGICAL", op="R",
                         addr=page * PAGE + offset, bytes=ACCESS, issue_ns=0.0)
             for page in range(pages) for offset in range(0, PAGE, ACCESS)]
    with open_fine_session("4hbm-4hbf", frontend=frontend, out_dir=out,
                           initial_hbf_logical_pages=pages) as session:
        result = session.run(reads, completions=False)
    summary = {
        "workload": "synthetic read-only 64-byte accesses, page-major order",
        "frontend": frontend,
        "requests": len(reads),
        "logical_read_bytes": len(reads) * ACCESS,
        "serving_ns": result.elapsed_ns,
        "timing_scope": "memory-only, open-loop; excludes terminal drain",
    }
    (out / "example-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{frontend}: {len(reads):,} x {ACCESS} B reads over {pages} pages")
    print(f"Simulated serving time: {result.elapsed_ns / 1000:.3f} us (terminal drain excluded)")
    print("Synthetic API demonstration; not a hardware or model-inference benchmark.")
    print(f"Resolved frontend: {out / 'frontend-receipt.json'}")
    print(f"Session receipts and summary: {out}")
    return 0

