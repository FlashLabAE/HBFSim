# Session transaction state and memory

> Status: Experimental
> Last reviewed: 2026-10-17

The common session executor supports arbitrary transaction dependencies and
cross-batch references. A batch is still decoded and validated completely before
device execution. Device queue depth is not a cap on software transaction storage.

## Storage optimizations

Validation's ID index is released once frontier, observation and retention
indices have been resolved. Runtime scheduling uses existing numeric node indices.
Observation slots are allocated only when the caller requests observations.
Duplicate local dependency checks use reusable node marks instead of a temporary
hash entry for every edge of a large barrier. Prior-batch dependency checks and
cycle validation remain unchanged in meaning.

HBM completion-ticket lookup starts small and grows with actual outstanding
entries. It no longer reserves buckets for every transaction in the batch.
This changes allocation only: HBM enqueue still services full finite queues and
the scheduler collects completions at the same points.

Completed batches with consecutive canonical decimal IDs share a prefix and a
vector of exact finish times. The final ID is stored separately. Other ID layouts
fall back to the ordinary exact map. This is lossless storage: every original ID
remains queryable for the same dependency window; explicitly retained interior
IDs survive normal history expiry. Leading zeros, signs, suffix text or out-of-
range values cannot alias a canonical dense ID. An explicitly present noncanonical
ID remains queryable via its ordinary map entry.

No requests or edges are removed, no queue parameters or issue priorities change,
and no extra completion barriers are introduced. Text, explicit binary and compact
phase input all use these changes in the same executor.

## Evidence and limits

The isolated all-HBM synthetic allocation fixture measured approximately503→388
MiB native peak for1M memory operations,997→766MiB near2M. This is one sample per
point and is not model inference timing. The frozen independent1.5B layer pair's
native peak fell from roughly494–501MiB to353MiB, with exact phase receipts and
terminal measurements unchanged. Coarse controls retain12,582/12,633ns.
Workspace evidence: `experiments/native_state_storage_20261007_01/RESULT.md`.

These reduce storage constants, not asymptotic memory. The full decoded vector,
DAG and ready queue remain; this is not bounded-window input or end-to-end streaming.
The binary2M total-transaction and512MiB payload caps stay unchanged. Complete
model12.5M-request kernels have not been replayed by this implementation.
