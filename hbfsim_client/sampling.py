#!/usr/bin/env python3
"""Stratified sampling of a long batch sequence.

A window of thousands of batches costs the engine one full pass per batch,
although consecutive batches of a steady phase take nearly the same time.
Sampling times a subset in full and gives the rest the running mean of their
stratum, so the engine's work shrinks with the sampling rate while every
batch still runs for its state: the writes and copies that change what the
engine holds are kept, only the reads of the plain tiers are left out
(``state_only``). A frontend that drives the session protocol decides the
stratum of each batch; the estimator reports per-stratum counts, means and
variances and the stratified estimate of the whole window with a confidence
interval.

The estimate is for the sequence of batches as it was produced. A frontend
that schedules the next batch from completion times runs on a clock that
mixes timed and estimated durations; what that changes in the schedule is
the frontend's to declare.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import math
import random
from typing import Any, Hashable, Mapping

from hbfsim_client.transaction_protocol import (
    Transaction,
    TransactionBatch,
    TransactionProtocolError,
)


SAMPLING_SCHEMA = {"name": "hbfsim.sampled_batches", "version": 1}
# Reads of these targets change nothing the engine keeps except its caches.
STATELESS_READ_TARGETS = frozenset({
    "HBM", "HBF_LOGICAL", "HBF_STATIC", "HBF_PHYSICAL", "EXTERNAL", "HOST_DRAM",
})
# Two-sided 95% normal quantile; the stratified estimator is approximately
# normal once each stratum holds a handful of observations.
NORMAL_95 = 1.959963984540054


@dataclass(frozen=True)
class SamplingPlan:
    """How many batches to time: every stratum's first ``pilot`` batches, then a share ``rate``.

    A timed batch that follows a skipped one finds the engine's caches as the
    skipped batches left them; ``warmup`` such batches are timed but do not
    enter the estimate.
    """

    rate: float
    pilot: int = 2
    warmup: int = 1
    seed: int = 0

    def __post_init__(self) -> None:
        if isinstance(self.rate, bool) or not isinstance(self.rate, (int, float)) \
                or not 0.0 < float(self.rate) <= 1.0:
            raise ValueError("the sampling rate must be in (0, 1]")
        for name in ("pilot", "warmup", "seed"):
            value = getattr(self, name)
            if isinstance(value, bool) or not isinstance(value, int) or (name != "seed" and value < 0):
                raise ValueError(f"sampling {name} must be a nonnegative integer")
        if self.pilot == 0 and self.rate < 1.0:
            raise ValueError("a pilot of at least one timed batch per stratum is needed to estimate the rest")

    def canonical(self) -> dict[str, Any]:
        return {"rate": float(self.rate), "pilot": self.pilot, "warmup": self.warmup, "seed": self.seed}


@dataclass(frozen=True)
class Decision:
    """What to do with one batch."""

    index: int
    stratum: Hashable
    timed: bool
    counted: bool  # a timed batch that enters the estimate (not a warm-up)
    estimate_ns: float | None  # the duration given to a skipped batch

    def canonical(self) -> dict[str, Any]:
        return {"index": self.index, "stratum": _stratum_label(self.stratum), "timed": self.timed,
                "counted": self.counted, "estimate_ns": self.estimate_ns}


@dataclass
class _Stratum:
    batches: int = 0
    timed: int = 0
    counted: int = 0
    skipped: int = 0
    sum_ns: float = 0.0
    sum_squares: float = 0.0
    timed_ns: float = 0.0
    assigned_ns: float = 0.0

    @property
    def mean_ns(self) -> float:
        return self.sum_ns / self.counted

    @property
    def variance(self) -> float | None:
        if self.counted < 2:
            return None
        return max(0.0, (self.sum_squares - self.sum_ns * self.sum_ns / self.counted) / (self.counted - 1))


def _stratum_label(stratum: Hashable) -> str:
    return stratum if isinstance(stratum, str) else "/".join(str(part) for part in stratum) \
        if isinstance(stratum, tuple) else str(stratum)


class Sampler:
    """Decides which batches to time and estimates the rest from their strata."""

    def __init__(self, plan: SamplingPlan) -> None:
        self.plan = plan
        self._random = random.Random(plan.seed)
        self._strata: dict[Hashable, _Stratum] = {}
        self._order: list[Hashable] = []
        self._decisions: list[Decision] = []
        self._pending_warmups = 0

    def decide(self, stratum: Hashable) -> Decision:
        """Time this batch, or assign it its stratum's current mean."""
        state = self._strata.get(stratum)
        if state is None:
            state = self._strata[stratum] = _Stratum()
            self._order.append(stratum)
        state.batches += 1
        timed = state.counted < self.plan.pilot or self._random.random() < self.plan.rate
        if timed:
            counted = self._pending_warmups == 0
            if not counted:
                self._pending_warmups -= 1
            state.timed += 1
            decision = Decision(len(self._decisions), stratum, True, counted, None)
        else:
            estimate = state.mean_ns
            state.skipped += 1
            state.assigned_ns += estimate
            self._pending_warmups = self.plan.warmup
            decision = Decision(len(self._decisions), stratum, False, False, estimate)
        self._decisions.append(decision)
        return decision

    def observe(self, decision: Decision, elapsed_ns: float) -> None:
        """Record a timed batch's duration."""
        if not decision.timed:
            raise ValueError("only timed batches are observed")
        if not math.isfinite(elapsed_ns) or elapsed_ns < 0:
            raise ValueError("a batch duration must be finite and nonnegative")
        state = self._strata[decision.stratum]
        state.timed_ns += elapsed_ns
        if decision.counted:
            state.counted += 1
            state.sum_ns += elapsed_ns
            state.sum_squares += elapsed_ns * elapsed_ns

    @property
    def decisions(self) -> tuple[Decision, ...]:
        return tuple(self._decisions)

    def report(self) -> dict[str, Any]:
        """Per-stratum counts and moments, and the stratified estimate of the window.

        The estimate gives every batch of a stratum the stratum's final mean;
        its variance is the finite-population stratified one, with strata of a
        single observation contributing no variance and said so.
        """
        rows = []
        estimate = variance = 0.0
        unknown_batches = 0
        for stratum in self._order:
            state = self._strata[stratum]
            if state.counted == 0:
                raise ValueError("a stratum without a counted observation cannot be reported")
            stratum_variance = state.variance
            if stratum_variance is None:
                # One observation says nothing about the spread; the skipped
                # batches of such a stratum are counted as unknown.
                unknown_batches += state.batches - state.counted
            else:
                variance += state.batches ** 2 * (1 - state.counted / state.batches) * stratum_variance / state.counted
            estimate += state.batches * state.mean_ns
            rows.append({
                "stratum": _stratum_label(stratum), "batches": state.batches, "timed": state.timed,
                "counted": state.counted, "skipped": state.skipped, "mean_ns": state.mean_ns,
                "variance_ns2": stratum_variance, "timed_ns": state.timed_ns, "assigned_ns": state.assigned_ns,
            })
        batches = sum(row["batches"] for row in rows)
        timed = sum(row["timed"] for row in rows)
        clock_ns = sum(row["timed_ns"] + row["assigned_ns"] for row in rows)
        margin = NORMAL_95 * math.sqrt(variance)
        return {
            "schema": SAMPLING_SCHEMA,
            "plan": self.plan.canonical(),
            "batches": batches,
            "timed_batches": timed,
            "timed_fraction": timed / batches if batches else None,
            "strata": rows,
            "clock_ns": clock_ns,
            "estimate_ns": estimate,
            "standard_error_ns": math.sqrt(variance),
            "confidence_interval_95_ns": [estimate - margin, estimate + margin],
            # Skipped batches whose stratum has one counted observation: their
            # spread is not in the interval.
            "unknown_variance_batches": unknown_batches,
            "variance_known": unknown_batches == 0,
            "estimator": "stratified_mean_finite_population_v1",
        }


def state_only(batch: TransactionBatch, *, estimate_ns: float, frontier_id: str) -> TransactionBatch:
    """The batch without the reads that leave no state behind, ending at ``estimate_ns``.

    Writes, link copies, peer accesses and barriers stay; a transaction that
    depended on a dropped read now depends on what that read depended on.
    A barrier issued at ``estimate_ns`` after every kept transaction is the
    batch's only frontier, so the engine's clock advances by the estimate
    or by the kept work, whichever is later.
    """
    if not math.isfinite(estimate_ns) or estimate_ns < 0:
        raise TransactionProtocolError("a skipped batch needs a finite, nonnegative estimate")
    by_id = {transaction.id: transaction for transaction in batch.transactions}
    if frontier_id in by_id:
        raise TransactionProtocolError(f"frontier id {frontier_id} is already used in the batch")
    dropped = {
        transaction.id
        for transaction in batch.transactions
        if transaction.op == "R" and transaction.target in STATELESS_READ_TARGETS
    }
    resolved: dict[str, tuple[str, ...]] = {}

    def dependencies(identifier: str) -> tuple[str, ...]:
        """The kept transactions ``identifier`` transitively depends on, in first-seen order."""
        if identifier in resolved:
            return resolved[identifier]
        kept: list[str] = []
        for dependency in by_id[identifier].dependencies if identifier in by_id else ():
            if dependency in dropped:
                kept.extend(d for d in dependencies(dependency) if d not in kept)
            elif dependency not in kept:
                kept.append(dependency)
        resolved[identifier] = tuple(kept)
        return resolved[identifier]

    transactions = [
        Transaction(
            id=transaction.id, target=transaction.target, op=transaction.op, addr=transaction.addr,
            bytes=transaction.bytes, issue_ns=transaction.issue_ns, duration_ns=transaction.duration_ns,
            dependencies=dependencies(transaction.id), stack=transaction.stack)
        for transaction in batch.transactions
        if transaction.id not in dropped
    ]
    transactions.append(Transaction(
        id=frontier_id, target="BARRIER", op=None, addr=0, bytes=0, issue_ns=float(estimate_ns),
        duration_ns=0.0, dependencies=tuple(transaction.id for transaction in transactions)))
    return TransactionBatch(
        batch_id=batch.batch_id, transactions=tuple(transactions),
        logical_trace_sha256=batch.logical_trace_sha256, routing_sidecar_sha256=batch.routing_sidecar_sha256,
        receipt=dict(batch.receipt, sampling={"state_only": True, "dropped_reads": len(dropped),
                                               "estimate_ns": float(estimate_ns)}),
        frontier=(frontier_id,), retain=batch.retain, completions=batch.completions,
        observe=tuple(identifier for identifier in batch.observe if identifier not in dropped))
