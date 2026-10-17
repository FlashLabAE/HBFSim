#!/usr/bin/env python3
"""Physics checks of the HBF thermal model under LLM-serving traffic.

Every check drives the real engine (``build/hbfsim``) through
``hbfsim_client`` on the shipped ``4hbm-4hbf`` profile with traffic shaped
like LLM serving: decode steps that stream weight slices and KV pages,
prefill chunks and KV write-back that program pages, and long sessions that
churn KV until garbage collection runs. The expectations are closed forms,
not golden numbers:

* energy identity: media heat equals reads, programs and erases times their
  per-operation energies (first law, per NAND operation);
* RC transient: a decode loop from idle heats as T_eq - (T_eq - T0) e^{-t/tau}
  with T_eq = boundary + (static + dynamic) * R and tau = R * C;
* light throttling: a stack that reached LTT serves at most budget / energy
  per bit, monotonically less as the package boundary rises (OCP 9.2);
* hysteresis: light throttling releases below RTT, not at LTT, after exactly
  the RC cooling time (OCP Figure 36);
* severe throttling: every CATTRIP halt lasts the RC cooling time from STT to
  LTT and the stack serves nothing meanwhile (OCP 9.2);
* NAND asymmetry: programming heats an order of magnitude more per byte than
  reading, and garbage collection multiplies that by the write amplification
  (standard SSD/NAND behavior).

Thermal time constants are seconds in the shipped profiles; some checks
compress the thermal capacitance so the transient fits a short run, which
scales tau the same way and leaves every equilibrium untouched.

    python3 -B tests/python/test_thermal_llm_physics.py --simulator build/hbfsim
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import random
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from hbfsim_client import ResolvedSystemConfig, SimulationSession, Transaction  # noqa: E402

SIMULATOR = ROOT / "build" / "hbfsim"
SYSTEM = ROOT / "configs/systems/4hbm-4hbf.cfg"
OVERLAYS = ROOT / "configs/overlays/hbf"
PAGE = 4096
MIB = 1 << 20


class Run:
    """One engine session (one or more batches) and the telemetry it leaves."""

    def __init__(self, name: str, overlays, options, batches, *, pages: int, out: Path):
        self.name = name
        out.mkdir(parents=True, exist_ok=True)
        files = [SYSTEM] + [OVERLAYS / f"{overlay}.cfg" for overlay in overlays]
        if options:
            overrides = out / "overrides.cfg"
            overrides.write_text("".join(f"{key}={value}\n" for key, value in options.items()))
            files.append(overrides)
        self.config = ResolvedSystemConfig.load(tuple(files))
        self.thermal = self.config.hbf_thermal_identity
        with SimulationSession(simulator_path=SIMULATOR, system_config=self.config,
                               enable_hbm=True, enable_hbf=True,
                               initial_hbf_logical_pages=pages,
                               hbf_wear_output_prefix=out / "wear") as session:
            self.results = [session.run(batch, completions=False) for batch in batches]
        stop = session.source_receipt()["final_measurement"]
        # Cumulative accounting at the stop receipt already includes the
        # end-of-session drain; the drain's own delta is kept for reporting.
        self.totals = stop["device_workload_totals"]["hbf"]
        self.drain = stop["end_of_session_drain"]["device_delta"]["hbf"]
        self.state = self.totals["state"]
        self.finish_ns = float(stop["drained_frontier_ns"])

    @property
    def result(self):
        return self.results[-1]

    def counter(self, key: str) -> float:
        return float(self.totals.get(key, 0.0))

    @property
    def stacks(self) -> int:
        return int(self.config.values["hbf-stacks"])

    def expected_energy_j(self) -> float:
        bits = PAGE * 8
        return (self.counter("page_reads") * bits * self.thermal["read_energy_pj_per_bit"] * 1e-12
                + self.counter("page_programs") * bits * self.thermal["program_energy_pj_per_bit"] * 1e-12
                + self.counter("block_erases") * self.thermal["erase_energy_uj_per_block"] * 1e-6)


def read_burst(total_bytes: int, chunk_bytes: int = MIB, at_ns: float = 0.0, base: int = 0, prefix: str = "r"):
    """A decode-like read stream issued at once, in request-sized chunks."""
    return [Transaction(id=f"{prefix}{i}", target="HBF_LOGICAL", op="R", addr=base + i * chunk_bytes,
                        bytes=chunk_bytes, issue_ns=at_ns)
            for i in range(total_bytes // chunk_bytes)]


def write_pages(page_numbers, at_ns: float = 0.0, prefix: str = "w"):
    """Prefill / KV write-back: one page program per transaction."""
    return [Transaction(id=f"{prefix}{i}", target="HBF_LOGICAL", op="W", addr=page * PAGE,
                        bytes=PAGE, issue_ns=at_ns)
            for i, page in enumerate(page_numbers)]


class ThermalPhysicsTests(unittest.TestCase):
    out: Path

    @classmethod
    def setUpClass(cls):
        cls.base = ResolvedSystemConfig.load((SYSTEM,))
        cls.thermal = cls.base.hbf_thermal_identity
        values = cls.base.values
        banks = (int(values["hbf-channels"]) * int(values["hbf-dies-per-channel"])
                 * int(values["hbf-planes-per-die"]))
        cls.banks_per_stack = banks
        cls.array_GBps_per_stack = banks * PAGE / float(values["hbf-read-ns"])  # bytes/ns = GB/s
        cls.read_j_per_page = PAGE * 8 * cls.thermal["read_energy_pj_per_bit"] * 1e-12
        cls.program_j_per_page = PAGE * 8 * cls.thermal["program_energy_pj_per_bit"] * 1e-12
        cls.resistance = cls.thermal["resistance_c_per_w"]
        cls.static_w = cls.thermal["static_power_w"]
        cls.ltt = cls.thermal["throttle_c"]
        cls.rtt = cls.thermal["release_c"]
        cls.stt = cls.thermal["severe_c"]
        cls.reports: list[str] = []

    @classmethod
    def tearDownClass(cls):
        print("\n" + "\n".join(cls.reports))

    def report(self, line: str) -> None:
        self.reports.append(line)

    def budget_w(self, boundary_c: float) -> float:
        return (self.ltt - boundary_c) / self.resistance - self.static_w

    def sustainable_GBps(self, boundary_c: float) -> float:
        return min(self.budget_w(boundary_c) / self.read_j_per_page * PAGE / 1e9, self.array_GBps_per_stack)

    # 1. First law per NAND operation ------------------------------------
    def test_decode_and_prefill_energy_is_reads_programs_and_erases(self):
        weights, kv, prefill = 32 * MIB, 8 * MIB, 4 * MIB
        batch = read_burst(weights, prefix="w") + read_burst(kv, base=weights, prefix="k")
        batch += write_pages(range((weights + kv) // PAGE, (weights + kv + prefill) // PAGE), prefix="p")
        run = Run("energy-identity", (), {}, [batch], pages=(weights + kv + prefill) // PAGE,
                  out=self.out / "energy-identity")
        model_j = run.counter("thermal_media_energy_j")
        expected_j = run.expected_energy_j()
        self.assertGreaterEqual(run.counter("data_programs"), prefill // PAGE,
                                "prefill pages must reach the NAND by the end of the session")
        self.assertAlmostEqual(model_j, expected_j, delta=1e-9 * expected_j,
                               msg="media heat must equal the per-operation energies of the work done")
        self.report(f"[energy identity] {run.counter('page_reads'):.0f} page reads, "
                    f"{run.counter('page_programs'):.0f} programs ({run.counter('data_programs'):.0f} data, "
                    f"{run.counter('mapping_page_programs'):.0f} mapping), {run.counter('block_erases'):.0f} erases: "
                    f"model {model_j * 1e3:.4f} mJ = expected {expected_j * 1e3:.4f} mJ; "
                    f"{run.drain.get('page_programs', 0)} programs happened in the end-of-session drain")

    # 2. RC transient of a decode loop from idle --------------------------
    def test_decode_loop_heats_along_the_rc_curve(self):
        tau_s = 1e-3
        capacitance = tau_s / self.resistance
        interval_ns = 2500.0
        slice_bytes = 2 * MIB  # 128 pages per stack per step: about 10 W dynamic per stack
        weights = 64 * MIB     # rotate slices over 64 MiB so no decoded-page buffer serves a repeat
        slices = weights // slice_bytes
        idle_c = self.thermal["idle_temperature_c"]
        samples = []
        for multiple in (1, 2, 4):
            steps = int(round(multiple * tau_s * 1e9 / interval_ns))
            batch = [Transaction(id=f"s{step}", target="HBF_LOGICAL", op="R",
                                 addr=(step % slices) * slice_bytes, bytes=slice_bytes,
                                 issue_ns=step * interval_ns)
                     for step in range(steps)]
            run = Run(f"rc-{multiple}tau", (), {"hbf-thermal-capacitance-j-per-c": capacitance},
                      [batch], pages=weights // PAGE, out=self.out / f"rc-{multiple}tau")
            self.assertEqual(run.counter("page_reads"), steps * slice_bytes // PAGE,
                             "every slice must be sensed from the array")
            self.assertEqual(run.counter("thermal_throttled_media_ops"), 0,
                             "the loop must stay below LTT so the check sees the bare RC node")
            t_end_s = run.finish_ns / 1e9
            dynamic_w = run.counter("thermal_media_energy_j") / run.stacks / t_end_s
            t_eq = self.thermal["boundary_temperature_c"] + (self.static_w + dynamic_w) * self.resistance
            expected = t_eq - (t_eq - idle_c) * math.exp(-t_end_s / tau_s)
            final = float(run.state["thermal_final_temperature_c"])
            self.assertAlmostEqual(final, expected, delta=0.5,
                                   msg=f"{multiple} tau: RC closed form {expected:.2f} C, model {final:.2f} C")
            self.assertLessEqual(float(run.state["thermal_peak_temperature_c"]), final + 0.2,
                                 "heating toward equilibrium is monotone")
            samples.append((multiple, dynamic_w, t_eq, expected, final))
        # The normalized approach to equilibrium must follow e^{-t/tau}.
        for multiple, dynamic_w, t_eq, expected, final in samples:
            remaining = (t_eq - final) / (t_eq - idle_c)
            self.assertAlmostEqual(remaining, math.exp(-multiple), delta=0.03,
                                   msg=f"{multiple} tau: remaining excursion {remaining:.3f} vs e^-{multiple}")
            self.report(f"[rc transient] {multiple} tau at {dynamic_w:.2f} W/stack dynamic -> T_eq {t_eq:.2f} C: "
                        f"closed form {expected:.2f} C, model {final:.2f} C "
                        f"(remaining {remaining:.3f} vs e^-{multiple} = {math.exp(-multiple):.3f})")

    # 3. Light throttling: budget over energy per bit, worse at hotter boundaries
    def test_light_throttling_caps_decode_bandwidth_by_package_boundary(self):
        burst = read_burst(64 * MIB)
        off = Run("thermal-off", (), {"hbf-thermal-enable": "false"}, [burst],
                  pages=64 * MIB // PAGE, out=self.out / "cap-off")
        off_GBps = 64 * MIB / off.result.elapsed_ns / off.stacks
        self.assertAlmostEqual(off_GBps, self.array_GBps_per_stack, delta=0.08 * self.array_GBps_per_stack,
                               msg="without the thermal model the array sets the read bandwidth")
        previous = math.inf
        lines = [f"[light cap] thermal off {off_GBps:.1f} GB/s per stack (array {self.array_GBps_per_stack:.1f})"]
        for overlay, boundary in (("thermal-boundary-55c", 55.0), ("thermal-boundary-65c", 65.0),
                                  ("thermal-boundary-70c", 70.0)):
            run = Run(f"cap-{boundary:.0f}", ("thermal-steady-state-start", overlay), {}, [burst],
                      pages=64 * MIB // PAGE, out=self.out / f"cap-{boundary:.0f}")
            self.assertAlmostEqual(run.thermal["boundary_temperature_c"], boundary)
            achieved = 64 * MIB / run.result.elapsed_ns / run.stacks
            expected = self.sustainable_GBps(boundary)
            self.assertAlmostEqual(achieved, expected, delta=0.07 * expected,
                                   msg=f"{boundary:.0f} C boundary: budget {self.budget_w(boundary):.1f} W "
                                       f"should cap reads near {expected:.1f} GB/s per stack")
            self.assertLess(achieved, previous, "a hotter package must serve less")
            self.assertEqual(run.counter("thermal_throttled_media_ops"), 64 * MIB // PAGE,
                             "every page of a burst at the light ceiling is paced")
            self.assertEqual(int(run.state["thermal_throttled_stacks"]), run.stacks,
                             "at the derived budget the stacks stay at LTT: light throttling never releases")
            self.assertEqual(run.counter("thermal_severe_engagements"), 0,
                             "the derived budget can never reach STT")
            previous = achieved
            lines.append(f"[light cap] boundary {boundary:.0f} C, budget {self.budget_w(boundary):.1f} W: "
                         f"{achieved:.1f} GB/s per stack, closed form {expected:.1f}, "
                         f"decode step {off_GBps / achieved:.2f}x slower")
        self.reports.extend(lines)

    # 4. Hysteresis: release below RTT after the RC cooling time ------------
    def test_light_throttling_releases_below_rtt_after_an_idle_gap(self):
        tau_s = 1e-3
        boundary = 70.0
        idle_c = boundary + self.static_w * self.resistance
        burst = read_burst(16 * MIB)
        pause_ns = 2e7  # 20 ms >> tau ln 2
        late = [Transaction(id="late", target="HBF_LOGICAL", op="R", addr=0, bytes=MIB, issue_ns=pause_ns)]
        run = Run("hysteresis", ("thermal-steady-state-start", "thermal-boundary-70c"),
                  {"hbf-thermal-capacitance-j-per-c": tau_s / self.resistance}, [burst, late],
                  pages=16 * MIB // PAGE, out=self.out / "hysteresis")
        burst_pages = 16 * MIB // PAGE
        self.assertEqual(run.counter("thermal_throttled_media_ops"), burst_pages,
                         "the request after the idle gap meets released stacks and is not paced")
        self.assertEqual(int(run.state["thermal_throttled_stacks"]), 0)
        self.assertEqual(run.counter("thermal_throttle_engagements"), 0,
                         "one 1 MiB request cannot heat a released stack back to LTT")
        release_s = tau_s * math.log((self.ltt - idle_c) / (self.rtt - idle_c))
        burst_s = run.results[0].finish_ns / 1e9
        light_s = run.counter("thermal_throttled_span_ns") / 1e9 / run.stacks
        # Each stack stays throttled through the burst and then for exactly
        # the RC cooling time from LTT to RTT.
        self.assertAlmostEqual(light_s, burst_s + release_s, delta=0.02 * (burst_s + release_s),
                               msg=f"light span per stack {light_s * 1e3:.3f} ms should be burst "
                                   f"{burst_s * 1e3:.3f} ms + release {release_s * 1e3:.3f} ms")
        self.report(f"[hysteresis] burst {burst_s * 1e3:.3f} ms + RC release LTT->RTT {release_s * 1e3:.3f} ms "
                    f"= {light_s * 1e3:.3f} ms per stack in light throttling; "
                    f"the request after a 20 ms pause ran unthrottled")

    # 5. Severe throttling: halts last the RC cooling time from STT to LTT ---
    def test_cattrip_halts_last_the_rc_cooling_time(self):
        tau_s = 2e-5
        boundary = 70.0
        budget_w = 12.0
        idle_c = boundary + self.static_w * self.resistance
        # Single-page requests keep the per-stack deposit granularity at one
        # page, so STT is overshot by microkelvins and every halt is the RC
        # cooling time from STT to LTT.
        run = Run("severe", ("thermal-steady-state-start", "thermal-boundary-70c"),
                  {"hbf-thermal-throttle-power-w": budget_w,
                   "hbf-thermal-capacitance-j-per-c": tau_s / self.resistance},
                  [read_burst(64 * MIB, chunk_bytes=PAGE)], pages=64 * MIB // PAGE, out=self.out / "severe")
        halts = run.counter("thermal_severe_engagements")
        self.assertGreater(halts, 0, "a 12 W budget behind a 70 C boundary must reach STT")
        self.assertEqual(int(run.state["thermal_cattrip_stacks"]), run.stacks, "TMON.CES is set on every stack")
        halt_s = run.counter("thermal_severe_span_ns") / 1e9 / halts
        expected_s = tau_s * math.log((self.stt - idle_c) / (self.ltt - idle_c))
        self.assertAlmostEqual(halt_s, expected_s, delta=0.005 * expected_s,
                               msg="each halt must last the RC cooling time from STT to LTT")
        peak = float(run.state["thermal_peak_temperature_c"])
        self.assertGreaterEqual(peak, self.stt)
        self.assertLess(peak, self.stt + self.read_j_per_page / (tau_s / self.resistance) + 1e-6,
                        "STT is overshot by at most one page deposit")
        # Nothing is served during a halt: per stack, the burst takes at least
        # the halted time plus the paced time at the 12 W budget.
        paced_s = run.counter("thermal_throttled_media_ops") * self.read_j_per_page / budget_w / run.stacks
        halted_s = run.counter("thermal_severe_span_ns") / 1e9 / run.stacks
        self.assertGreaterEqual(run.finish_ns / 1e9, 0.97 * (paced_s + halted_s))
        self.report(f"[CATTRIP] {halts:.0f} halts across {run.stacks} stacks, each "
                    f"{halt_s * 1e6:.3f} us vs RC {expected_s * 1e6:.3f} us; peak {peak:.4f} C; "
                    f"per stack {halted_s * 1e6:.0f} us halted + {paced_s * 1e6:.0f} us paced "
                    f"of {run.finish_ns / 1e3:.0f} us total")

    # 6. NAND asymmetry: writes heat more, GC heats more still ---------------
    def test_prefill_writes_heat_an_order_of_magnitude_more_than_decode_reads(self):
        size = 16 * MIB
        reads = Run("decode-reads", (), {}, [read_burst(size)], pages=size // PAGE,
                    out=self.out / "nand-reads")
        writes = Run("prefill-writes", (), {}, [write_pages(range(size // PAGE))], pages=size // PAGE,
                     out=self.out / "nand-writes")
        read_j = reads.counter("thermal_media_energy_j")
        write_j = writes.counter("thermal_media_energy_j")
        self.assertEqual(reads.counter("page_reads"), size // PAGE)
        self.assertEqual(writes.counter("data_programs"), size // PAGE,
                         "full-page sequential writes program each page once (WAF 1)")
        ratio = write_j / read_j
        self.assertGreaterEqual(ratio, 8.0, "programming NAND must cost far more energy per byte than sensing it")
        self.assertAlmostEqual(write_j, writes.expected_energy_j(), delta=1e-9 * write_j)
        self.report(f"[NAND asymmetry] {size // MIB} MiB: decode reads {read_j * 1e3:.3f} mJ, "
                    f"prefill writes {write_j * 1e3:.3f} mJ ({writes.counter('data_programs'):.0f} data + "
                    f"{writes.counter('mapping_page_programs'):.0f} mapping programs, "
                    f"{writes.counter('block_erases'):.0f} erases on pre-erased free blocks): {ratio:.1f}x")

    def test_kv_churn_with_garbage_collection_heats_more_per_logical_byte(self):
        # A KV pool of 720 pages leaves the scaled device little
        # over-provisioning; every later round rewrites a different random half
        # of the pool, so dead pages scatter over all blocks and the greedy
        # collector runs out of entirely dead victims: it must relocate live
        # KV pages to reclaim space, whatever the allocator's striping.
        region, rounds = 720, 4
        batches = [write_pages(range(region), prefix="g0-")]
        batches += [write_pages(sorted(random.Random(round_index).sample(range(region), region // 2)),
                                at_ns=round_index * 1e6, prefix=f"g{round_index}-")
                    for round_index in range(1, rounds)]
        run = Run("kv-churn", ("endurance-regime-scaled",), {}, batches, pages=region,
                  out=self.out / "kv-churn")
        self.assertGreater(run.counter("block_erases"), 0, "KV churn on a full device must erase blocks")
        self.assertGreater(run.counter("gc_relocations"), 0, "GC must relocate the live cold KV pages")
        logical_bytes = run.counter("logical_write_bytes")
        physical_bytes = run.counter("physical_write_bytes")
        waf = physical_bytes / logical_bytes
        self.assertGreater(waf, 1.0)
        heat_per_logical_page = run.counter("thermal_media_energy_j") / (logical_bytes / PAGE)
        self.assertGreater(heat_per_logical_page, waf * self.program_j_per_page,
                           "heat per logical page must carry the amplified programs plus the erases")
        self.assertAlmostEqual(run.counter("thermal_media_energy_j"), run.expected_energy_j(),
                               delta=1e-9 * run.expected_energy_j())
        program_j = run.counter("page_programs") * self.program_j_per_page
        erase_j = run.counter("block_erases") * self.thermal["erase_energy_uj_per_block"] * 1e-6
        pages_per_block = int(run.config.values["hbf-pages-per-block"])
        self.report(f"[GC heat] {rounds} rounds over {region} KV pages: WAF {waf:.2f}, "
                    f"{run.counter('gc_relocations'):.0f} relocations, {run.counter('block_erases'):.0f} erases: "
                    f"{heat_per_logical_page / self.program_j_per_page:.2f}x the heat of the logical programs alone "
                    f"(programs {program_j * 1e3:.2f} mJ, erases {erase_j * 1e3:.2f} mJ; erase heat per page scales "
                    f"with 1/{pages_per_block} pages per block in this scaled overlay, 1/256 in the shipped geometry)")

    # 7. Thresholds as the industry sets them ---------------------------------
    def test_thresholds_follow_the_register_and_controller_conventions(self):
        t = self.thermal
        self.assertLess(t["release_c"], t["throttle_c"])
        self.assertLess(t["throttle_c"], t["severe_c"])
        self.assertLess(t["severe_c"], t["shutdown_c"])
        self.assertGreaterEqual(t["throttle_c"] - t["release_c"], 2.0, "controllers keep a hysteresis band")
        for key in ("release_c", "throttle_c", "severe_c"):
            steps = (t[key] - 60.0) * 2.0
            self.assertEqual(steps, round(steps), f"{key} must sit on the TTTEMP 0.5 C grid")
            self.assertTrue(60.0 <= t[key] <= 123.5)
        self.assertLess(t["idle_temperature_c"], t["release_c"], "an idle stack must be able to leave throttling")
        self.assertGreaterEqual(self.sustainable_GBps(t["boundary_temperature_c"]), self.array_GBps_per_stack,
                                "at the warm-water inlet boundary the shipped stack is array-bound, not thermally bound")
        self.assertLess(self.sustainable_GBps(70.0), 0.5 * self.array_GBps_per_stack,
                        "behind a full-load package neighbor it is thermally bound")
        self.report(f"[thresholds] RTT {t['release_c']} < LTT {t['throttle_c']} < STT {t['severe_c']} "
                    f"< shutdown {t['shutdown_c']} C; idle {t['idle_temperature_c']:.1f} C; "
                    f"sustainable {self.sustainable_GBps(45.0):.0f} GB/s per stack at 45 C vs "
                    f"{self.sustainable_GBps(70.0):.0f} at 70 C (array {self.array_GBps_per_stack:.0f}, "
                    f"{self.banks_per_stack} banks)")


def main() -> int:
    global SIMULATOR
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--simulator", type=Path, default=SIMULATOR, help="path to build/hbfsim")
    parser.add_argument("--out", type=Path, default=None, help="keep run directories here")
    args, remaining = parser.parse_known_args()
    SIMULATOR = args.simulator.resolve()
    if args.out is not None:
        ThermalPhysicsTests.out = args.out
        return 0 if unittest.main(argv=[sys.argv[0]] + remaining, exit=False).result.wasSuccessful() else 1
    with tempfile.TemporaryDirectory(prefix="hbfsim-thermal-physics-") as temporary:
        ThermalPhysicsTests.out = Path(temporary)
        return 0 if unittest.main(argv=[sys.argv[0]] + remaining, exit=False).result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
