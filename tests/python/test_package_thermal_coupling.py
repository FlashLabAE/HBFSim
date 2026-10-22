#!/usr/bin/env python3
"""Physics checks of the package thermal coupling under Peer- and Tiered-shaped traffic.

Every check drives the real engine (``build/hbfsim``) through ``hbfsim_client``
on the shipped ``4hbm-4hbf`` profile with the HBM thermal nodes and the package
network enabled by overrides. The expectations are closed forms or orderings,
not golden numbers:

* HBM energy identity: the HBM node's heat equals served bytes times the read
  and write energies per bit;
* Tiered against Peer: the same logical bytes served Tiered-style (every HBF
  byte written into HBM and read back) put more heat into the HBM stacks than
  Peer-style (HBM hits plus direct HBF reads), and through the package coupling
  raise the HBF stacks' effective boundary further;
* cascaded topology: HBF payload bytes deposit pass-through PHY energy in the
  HBM base die, none when the HBF sits beside the GPU;
* package off: a session without the package model reports the configured
  boundary and leaves the HBF trajectory of the uncoupled engine untouched.

    python3 -B tests/python/test_package_thermal_coupling.py --simulator build/hbfsim
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from hbfsim_client import ResolvedSystemConfig, SimulationSession, Transaction  # noqa: E402

SIMULATOR = ROOT / "build" / "hbfsim"
SYSTEM = ROOT / "configs/systems/4hbm-4hbf.cfg"
PAGE = 4096
MIB = 1 << 20
HBM_PJ_PER_BIT = 3.4
PASSTHROUGH_PJ_PER_BIT = 0.5

PACKAGE = {
    "hbm-thermal-enable": "true",
    "hbm-thermal-read-energy-pj-per-bit": str(HBM_PJ_PER_BIT),
    "hbm-thermal-write-energy-pj-per-bit": str(HBM_PJ_PER_BIT),
    "package-thermal-enable": "true",
    "package-thermal-update-interval-ns": "100000",
    "package-gpu-power-w": "700",
    "package-coupling-hbf-hbm-w-per-c": "0.1",
    "package-coupling-hbf-gpu-w-per-c": "0.3",
    "package-coupling-hbm-gpu-w-per-c": "0.3",
    "package-coupling-hbf-hbf-w-per-c": "0.1",
    "package-coupling-hbm-hbm-w-per-c": "0.1",
    "package-hbm-passthrough-energy-pj-per-bit": str(PASSTHROUGH_PJ_PER_BIT),
}


class Run:
    """One engine session and the receipts it leaves."""

    def __init__(self, name: str, options, batches, *, pages: int, out: Path):
        self.name = name
        out.mkdir(parents=True, exist_ok=True)
        files = [SYSTEM]
        if options:
            overrides = out / "overrides.cfg"
            overrides.write_text("".join(f"{key}={value}\n" for key, value in options.items()))
            files.append(overrides)
        self.config = ResolvedSystemConfig.load(tuple(files))
        with SimulationSession(simulator_path=SIMULATOR, system_config=self.config,
                               enable_hbm=True, enable_hbf=True,
                               initial_hbf_logical_pages=pages,
                               hbf_wear_output_prefix=out / "wear") as session:
            self.results = [session.run(batch, completions=False) for batch in batches]
        receipt = session.source_receipt()
        stop = receipt["final_measurement"]
        self.hbf = stop["device_workload_totals"]["hbf"]
        self.hbm = stop["device_workload_totals"]["hbm"]
        self.package = stop.get("package_thermal")
        self.finish_ns = float(stop["drained_frontier_ns"])


def hbf_reads(prefix: str, pages: int, chunk_pages: int, issue_ns: float = 0.0):
    chunk = chunk_pages * PAGE
    return [Transaction(id=f"{prefix}{i}", target="HBF_LOGICAL", op="R", addr=i * chunk, bytes=chunk,
                        issue_ns=issue_ns) for i in range(pages // chunk_pages)]


def hbm_ops(prefix: str, op: str, total_bytes: int, chunk_bytes: int, base: int = 0, issue_ns: float = 0.0):
    return [Transaction(id=f"{prefix}{i}", target="HBM", op=op, addr=base + i * chunk_bytes, bytes=chunk_bytes,
                        issue_ns=issue_ns) for i in range(total_bytes // chunk_bytes)]


class PackageThermalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = Path(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_hbm_heat_is_served_bytes_times_energy_per_bit(self):
        total = 64 * MIB
        batch = hbm_ops("w", "W", total, MIB) + hbm_ops("r", "R", total, MIB, issue_ns=1.0)
        run = Run("identity", PACKAGE, [batch], pages=16, out=self.out / "identity")
        served = run.hbm["read_bytes"] + run.hbm["write_bytes"]
        self.assertEqual(served, 2 * total)
        self.assertAlmostEqual(run.hbm["thermal_energy_j"], served * 8 * HBM_PJ_PER_BIT * 1e-12,
                               delta=1e-9 * served * 8 * HBM_PJ_PER_BIT * 1e-12)
        self.assertEqual(run.hbm["thermal_imported_energy_j"], 0.0)
        self.assertIsNotNone(run.package)
        self.assertEqual(run.package["topology"], "beside")
        self.assertGreater(run.package["gpu_final_temperature_c"], 45.0)

    def test_tiered_traffic_heats_hbm_and_the_hbf_boundary_more_than_peer_traffic(self):
        logical = 256 * MIB
        hbf_pages = logical // PAGE
        # Peer: a quarter of the logical bytes hit HBM, the rest is read from HBF.
        peer = hbm_ops("h", "R", logical // 4, MIB) + hbf_reads("f", hbf_pages * 3 // 4, 64)
        # Tiered: every HBF byte is also written into the HBM cache and read back from it.
        tiered = (hbm_ops("h", "R", logical // 4, MIB) + hbf_reads("f", hbf_pages * 3 // 4, 64) +
                  hbm_ops("fill", "W", logical * 3 // 4, MIB, base=logical // 4) +
                  hbm_ops("hit", "R", logical * 3 // 4, MIB, base=logical // 4, issue_ns=1.0))
        runs = {name: Run(name, PACKAGE, [batch], pages=hbf_pages, out=self.out / name)
                for name, batch in (("peer", peer), ("tiered", tiered))}
        self.assertEqual(runs["peer"].hbf["physical_read_bytes"], runs["tiered"].hbf["physical_read_bytes"])
        self.assertGreater(runs["tiered"].hbm["thermal_energy_j"], 2.0 * runs["peer"].hbm["thermal_energy_j"])
        self.assertGreater(runs["tiered"].hbm["state"]["thermal_peak_temperature_c"],
                           runs["peer"].hbm["state"]["thermal_peak_temperature_c"])
        # The hotter HBM neighbors raise the HBF stacks' effective boundary.
        self.assertGreater(runs["tiered"].hbf["state"]["thermal_effective_boundary_c"],
                           runs["peer"].hbf["state"]["thermal_effective_boundary_c"])
        for run in runs.values():
            self.assertGreater(run.hbf["state"]["thermal_effective_boundary_c"],
                               run.hbf["state"]["thermal_boundary_temperature_c"])

    def test_cascaded_topology_deposits_passthrough_energy_in_hbm(self):
        pages = 2048
        batch = hbf_reads("f", pages, 64)
        beside = Run("beside", PACKAGE, [batch], pages=pages, out=self.out / "beside")
        cascaded = Run("cascaded", dict(PACKAGE, **{"package-hbf-topology": "cascaded"}), [batch],
                       pages=pages, out=self.out / "cascaded")
        self.assertEqual(beside.hbm["thermal_imported_energy_j"], 0.0)
        payload = cascaded.hbf["physical_read_bytes"]
        self.assertEqual(payload, pages * PAGE)
        expected = payload * 8 * PASSTHROUGH_PJ_PER_BIT * 1e-12
        self.assertAlmostEqual(cascaded.hbm["thermal_imported_energy_j"], expected, delta=expected * 1e-9)
        self.assertEqual(cascaded.package["topology"], "cascaded")
        self.assertEqual(cascaded.package["passthrough_bytes"], payload)

    def test_package_off_keeps_the_uncoupled_engine(self):
        pages = 1024
        batch = hbf_reads("f", pages, 64) + hbm_ops("h", "R", 8 * MIB, MIB)
        off = Run("off", {"hbm-thermal-enable": "true"}, [batch], pages=pages, out=self.out / "off")
        self.assertIsNone(off.package)
        self.assertEqual(off.hbf["state"]["thermal_effective_boundary_c"],
                         off.hbf["state"]["thermal_boundary_temperature_c"])
        self.assertEqual(off.hbm["state"]["thermal_effective_boundary_c"], off.hbm["state"]["thermal_boundary_temperature_c"])
        plain = Run("plain", None, [batch], pages=pages, out=self.out / "plain")
        self.assertEqual(off.finish_ns, plain.finish_ns)
        self.assertEqual(off.hbf["thermal_media_energy_j"], plain.hbf["thermal_media_energy_j"])


def main() -> int:
    global SIMULATOR
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--simulator", type=Path, default=SIMULATOR)
    args, remaining = parser.parse_known_args()
    SIMULATOR = args.simulator.resolve()
    if not SIMULATOR.exists():
        print(f"simulator not built: {SIMULATOR}", file=sys.stderr)
        return 1
    return 0 if unittest.main(argv=[sys.argv[0]] + remaining, exit=False).result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
