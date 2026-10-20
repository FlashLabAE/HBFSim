#!/usr/bin/env python3
"""Example 7 — what happens when an HBF stack hits its thermal limits?

OCP HBF v0.7.0 (section 9.2) gives a stack four thermal modes. Normal runs at
full speed. At the light-throttling temperature (LTT) the Base-die firmware
lowers power; HBFSim models that by pacing media operations to a power
budget. If the stack still reaches the severe-throttling temperature (STT),
it asserts CATTRIP and stops accepting commands until it has cooled below
LTT. Past the shutdown point the host sees a link error. HBFSim drives these
modes from a lumped RC node per stack: every read, program and erase deposits
energy, and the node cools toward the package boundary temperature.

This example issues the same burst of 4 KiB reads three times:

1. thermal model off: the NAND array sets the bandwidth;
2. light throttling: the stacks start at LTT behind a 70 C package boundary
   (`thermal-steady-state-start` + `thermal-boundary-70c`), so the firmware
   paces reads to the budget that holds the node at LTT, 5.5 W per stack;
3. severe throttling: the same boundary with a 16.5 W light budget, three
   times what the boundary can carry, the model of a clock reduction that is
   not enough. The thermal mass is scaled down a million-fold so the
   sub-second heat-up fits into a microsecond burst (the time constant
   shrinks the same way); the stacks reach STT, assert CATTRIP, halt until
   they cool below LTT, and repeat.

    python3 examples/07_thermal_throttling.py

The thermal parameters are derived from published measurements and stack
geometry (docs/reference/thermal-parameters.md), not from an HBF product;
the mechanism, not the numbers, is the point. Example 3 explains the `hbfsim.open_session` entry point used here.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # run from a clone without installing

import hbfsim  # noqa: E402
from hbfsim.runner import prepare_output_directory  # noqa: E402
from hbfsim.session import session_config  # noqa: E402
from hbfsim_client import Transaction  # noqa: E402


SYSTEM = "4hbm-4hbf"
PAGE = 4096
STEADY_START = "thermal-steady-state-start"
HOT_BOUNDARY = "thermal-boundary-70c"

# The light budget that holds a stack at LTT behind a 70 C boundary is
# (85 - 70) / 2.0 - 2 = 5.5 W. Three times that keeps heating the stack. A
# million-fold smaller thermal mass turns the 0.6 s time constant into
# microseconds (R * C shrinks the same way), so one burst shows several
# Light -> Severe -> Light cycles instead of needing seconds of traffic.
INSUFFICIENT_LIGHT_BUDGET_W = 16.5
COMPRESSED_CAPACITANCE_J_PER_C = 3.0e-7

RUNS = (
    ("thermal off", (), {"hbf-thermal-enable": "false"}),
    ("light throttling", (STEADY_START, HOT_BOUNDARY), {}),
    ("severe throttling", (STEADY_START, HOT_BOUNDARY), {
        "hbf-thermal-throttle-power-w": INSUFFICIENT_LIGHT_BUDGET_W,
        "hbf-thermal-capacitance-j-per-c": COMPRESSED_CAPACITANCE_J_PER_C,
    }),
)


def read_burst(overlays, options, pages: int, out_dir: Path) -> dict:
    """Issue `pages` independent 4 KiB reads at t=0; return bandwidth and thermal telemetry."""

    out_dir.mkdir(parents=True, exist_ok=True)
    thermal = session_config(SYSTEM, overlays, options, out_dir=out_dir).hbf_thermal_identity
    with hbfsim.open_session(SYSTEM, overlays=list(overlays), options=options,
                             out_dir=out_dir, overwrite=True,
                             initial_hbf_logical_pages=pages) as session:  # data already written
        reads = [Transaction(id=f"r{page}", target="HBF_LOGICAL", op="R",
                             addr=page * PAGE, bytes=PAGE, issue_ns=0.0)
                 for page in range(pages)]
        result = session.run(reads, completions=False)
    # The batch receipt carries this batch's HBF counters; the absolute
    # device state (temperatures, mode per stack) comes with the stop receipt.
    hbf = result.receipt["device_delta"]["hbf"]
    state = session.final_device_totals["hbf"]["state"]
    enabled = bool(state["thermal_enabled"])
    return {
        "budget W": float(thermal["pacing_power_w"]) if thermal["enabled"] else None,
        "GB/s": pages * PAGE / result.elapsed_ns,  # bytes per ns == GB/s
        "paced reads": int(hbf["thermal_throttled_media_ops"]),
        "halted reads": int(hbf["thermal_severe_halted_media_ops"]),
        "CATTRIP": int(hbf["thermal_severe_engagements"]),
        "peak C": float(state["thermal_peak_temperature_c"]) if enabled else None,
        "light us": float(hbf["thermal_throttled_span_ns"]) / 1e3,
        "severe us": float(hbf["thermal_severe_span_ns"]) / 1e3,
        "stacks": int(state["thermal_cattrip_stacks"]),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=None, help="output directory")
    parser.add_argument("--pages", type=int, default=16384, help="reads per burst")
    args = parser.parse_args()
    out = prepare_output_directory(args.out, "example-thermal")

    rows = []
    for name, overlays, options in RUNS:
        row = read_burst(overlays, options, args.pages, out / name.replace(" ", "-"))
        row["run"] = name
        rows.append(row)

    columns = ("run", "budget W", "GB/s", "paced reads", "halted reads", "CATTRIP",
               "peak C", "light us", "severe us")
    print(f"HBF read burst under the OCP thermal modes, {SYSTEM}, {args.pages} x 4 KiB reads at t=0\n")
    print("  ".join(f"{column:>18}" if column == "run" else f"{column:>12}" for column in columns))
    for row in rows:
        cells = []
        for column in columns:
            value = row[column]
            if column == "run":
                cells.append(f"{value:>18}")
            elif value is None:
                cells.append(f"{'-':>12}")
            elif isinstance(value, float):
                cells.append(f"{value:>12.1f}")
            else:
                cells.append(f"{value:>12,}")
        print("  ".join(cells))

    off, light, severe = rows
    print(f"\nLight throttling paces every read ({light['paced reads']:,} of {args.pages:,}) to the")
    print(f"{light['budget W']:.0f} W per stack a 70 C boundary can carry, {off['GB/s'] / light['GB/s']:.1f}x below the array's")
    print("bandwidth; the stacks sit at their 85 C ceiling for the whole burst and never reach STT.")
    print(f"A {severe['budget W']:.0f} W budget heats the stacks to STT ({severe['peak C']:.1f} C): CATTRIP goes high")
    print(f"{severe['CATTRIP']:,} times across {severe['stacks']} stacks, {severe['halted reads']:,} reads wait for a halt to lift, and")
    print(f"the stacks spend {severe['severe us']:.0f} us halted against {severe['light us']:.0f} us throttled. Three times the")
    print(f"budget buys {severe['GB/s'] / light['GB/s']:.1f}x the bandwidth, with the device alternating between serving and")
    print("a fault condition the host is told about: a budget the cooling cannot carry is not a")
    print("performance setting. Per-run configs (including the thermal keys) and wear maps are in")
    print(f"{out}; the summary fields are the thermal_* counters of device_delta.hbf and the")
    print("thermal_* state of final_device_totals['hbf']['state'].")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
