"""Sampled batch sequences: decisions, the stratified estimate, and state-only batches."""

from pathlib import Path
import random
import statistics
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from hbfsim_client.sampling import Sampler, SamplingPlan, state_only  # noqa: E402
from hbfsim_client.transaction_protocol import Transaction, TransactionBatch, TransactionProtocolError  # noqa: E402


def _window(strata_means, count, seed):
    """A sequence of (stratum, true duration) with stratum-specific noise."""
    generator = random.Random(seed)
    rows = []
    for index in range(count):
        stratum = generator.choice(sorted(strata_means))
        mean = strata_means[stratum]
        rows.append((stratum, max(0.0, generator.gauss(mean, mean * 0.05))))
    return rows


class SamplerTests(unittest.TestCase):
    def test_pilot_then_rate_and_warmups_are_excluded(self):
        sampler = Sampler(SamplingPlan(rate=0.25, pilot=2, warmup=1, seed=3))
        rows = _window({"a": 100.0, "b": 400.0}, 400, seed=1)
        timed_after_skip = 0
        skipped_before = False
        for stratum, duration in rows:
            decision = sampler.decide(stratum)
            if decision.timed:
                sampler.observe(decision, duration)
                if skipped_before:
                    timed_after_skip += 1
                    self.assertFalse(decision.counted)
                skipped_before = False
            else:
                self.assertIsNotNone(decision.estimate_ns)
                skipped_before = True
        report = sampler.report()
        self.assertEqual(report["batches"], 400)
        for row in report["strata"]:
            self.assertGreaterEqual(row["counted"], 2)
            self.assertEqual(row["batches"], row["timed"] + row["skipped"])
        self.assertGreater(timed_after_skip, 0)
        self.assertLess(report["timed_fraction"], 0.5)

    def test_estimate_covers_the_true_total_with_its_interval(self):
        means = {"short": 50.0, "long": 900.0, "mid": 300.0}
        covered = 0
        trials = 40
        for seed in range(trials):
            rows = _window(means, 600, seed=seed)
            sampler = Sampler(SamplingPlan(rate=0.1, pilot=3, warmup=0, seed=seed))
            for stratum, duration in rows:
                decision = sampler.decide(stratum)
                if decision.timed:
                    sampler.observe(decision, duration)
            report = sampler.report()
            self.assertTrue(report["variance_known"])
            low, high = report["confidence_interval_95_ns"]
            truth = sum(duration for _, duration in rows)
            covered += low <= truth <= high
            self.assertLess(abs(report["estimate_ns"] - truth) / truth, 0.05)
        self.assertGreaterEqual(covered / trials, 0.85)

    def test_rate_one_times_everything_and_the_estimate_is_exact(self):
        sampler = Sampler(SamplingPlan(rate=1.0, pilot=0, warmup=0))
        rows = _window({"only": 10.0}, 25, seed=5)
        for stratum, duration in rows:
            decision = sampler.decide(stratum)
            self.assertTrue(decision.timed and decision.counted)
            sampler.observe(decision, duration)
        report = sampler.report()
        self.assertEqual(report["timed_batches"], 25)
        self.assertAlmostEqual(report["estimate_ns"], sum(d for _, d in rows))
        self.assertAlmostEqual(report["clock_ns"], report["estimate_ns"])
        self.assertEqual(report["confidence_interval_95_ns"], [report["estimate_ns"]] * 2)
        self.assertEqual(report["unknown_variance_batches"], 0)

    def test_a_singleton_stratum_leaves_its_skipped_batches_outside_the_interval(self):
        sampler = Sampler(SamplingPlan(rate=0.5, pilot=1, warmup=0, seed=2))
        for stratum, duration in (("a", 5.0), ("a", 7.0), ("a", 6.0), ("b", 100.0), ("b", 100.0)):
            decision = sampler.decide(stratum)
            if decision.timed:
                sampler.observe(decision, duration)
        report = sampler.report()
        singleton = [row for row in report["strata"] if row["counted"] == 1]
        self.assertEqual(report["unknown_variance_batches"],
                         sum(row["batches"] - row["counted"] for row in singleton))
        self.assertEqual(report["variance_known"], report["unknown_variance_batches"] == 0)
        self.assertEqual(len(report["confidence_interval_95_ns"]), 2)

    def test_plan_validation(self):
        with self.assertRaises(ValueError):
            SamplingPlan(rate=0.0)
        with self.assertRaises(ValueError):
            SamplingPlan(rate=0.5, pilot=0)
        with self.assertRaises(ValueError):
            SamplingPlan(rate=0.5, warmup=-1)
        sampler = Sampler(SamplingPlan(rate=0.5))
        decision = sampler.decide("s")
        with self.assertRaises(ValueError):
            sampler.observe(decision, float("nan"))


def _batch():
    transactions = (
        Transaction("start", "BARRIER", None, 0, 0, 0.0),
        Transaction("w0", "HBF_LOGICAL", "W", 0, 4096, 0.0, dependencies=("start",)),
        Transaction("r0", "HBM", "R", 8192, 4096, 0.0, dependencies=("start",)),
        Transaction("r1", "HBF_LOGICAL", "R", 16384, 4096, 0.0, dependencies=("r0",)),
        Transaction("copy", "D2D_HBF_TO_HBM", "R", 0, 4096, 0.0, dependencies=("r1", "w0"), stack=0),
        Transaction("peer", "PEER", "R", 4096, 4096, 0.0, dependencies=("r1",)),
        Transaction("done", "BARRIER", None, 0, 0, 0.0, dependencies=("copy", "r1", "peer")),
    )
    return TransactionBatch(batch_id=7, transactions=transactions, frontier=("done",), observe=("r1", "w0"))


class StateOnlyTests(unittest.TestCase):
    def test_reads_of_plain_tiers_go_and_dependencies_fold_through_them(self):
        reduced = state_only(_batch(), estimate_ns=12345.0, frontier_id="estimate")
        kept = {transaction.id: transaction for transaction in reduced.transactions}
        self.assertEqual(set(kept), {"start", "w0", "copy", "peer", "done", "estimate"})
        self.assertEqual(kept["copy"].dependencies, ("start", "w0"))
        self.assertEqual(kept["peer"].dependencies, ("start",))
        self.assertEqual(kept["done"].dependencies, ("copy", "start", "peer"))
        self.assertEqual(kept["estimate"].issue_ns, 12345.0)
        self.assertEqual(set(kept["estimate"].dependencies), {"start", "w0", "copy", "peer", "done"})
        self.assertEqual(reduced.frontier, ("estimate",))
        self.assertEqual(reduced.observe, ("w0",))
        self.assertEqual(reduced.batch_id, 7)
        self.assertEqual(reduced.receipt["sampling"]["dropped_reads"], 2)

    def test_frontier_id_must_be_new_and_estimate_finite(self):
        with self.assertRaises(TransactionProtocolError):
            state_only(_batch(), estimate_ns=1.0, frontier_id="done")
        with self.assertRaises(TransactionProtocolError):
            state_only(_batch(), estimate_ns=float("inf"), frontier_id="estimate")


if __name__ == "__main__":
    unittest.main()
