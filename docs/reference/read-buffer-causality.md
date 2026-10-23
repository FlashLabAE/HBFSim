# HBF decoded bank-cache causality

> Status: Current
> Last reviewed: 2026-10-23

OCP HBF v0.7.0 pages 56–57 require at least two decoded pages per bank. The
current device implements two, with LRU replacement within that bank. There is
no pooled per-stack cache size, independent-subarray mode or batch-sense switch.

`physical::hbf::HbfDevice` owns the cache. Each bank stores its resident LRU at
the causal frontier and ordered future fill, touch and invalidation events.
Lookup replays events only through the request's readiness time, so a future
decode cannot appear as an earlier hit. A future response that has already been
returned by the simulator reserves its data: an earlier backfilled fill is
bypassed if installing it would invalidate that response. Logical residency is
at most two pages at every time; pending event records are not extra cache slots.

Invalidation preserves prior event history until the frontier advances. Removing
an earlier fill outright could resurrect an LRU victim, so invalidations are
ordered after fenced consumers. Device reads also check the programmed-page
prefix and program completion; the cache cannot make erased media readable.

Reads that miss before decode completion issue real reads rather than claiming
an already-completed fill by default. With `hbf-read-buffer-coalescing=true`
(opt-in, default false) such a miss instead joins the earliest in-flight fill of
the same physical page, comparable to a miss-status holding register: the
controller previews its command and SRAM transfer, checks that the page is still
resident when it would consume it, and otherwise takes the ordinary read path.
Only the array read is shared; mapping, command and SRAM transfer are still
charged per request. `read_buffer_coalesced_reads`/`_bytes` count joined reads.
Without the option the implementation does not promise minimum reads through
in-flight coalescing. Host write buffering and mapping caches are
separate Host DRAM policies and cannot expand decoded device residency.

`hbf-read-mshr-entries-per-stack=N` (research option, default 0 = off;
requires read sharing) adds a page-level read MSHR at each stack's ingress.
A logical single-page read that went to the array allocates an entry until its
page read completes; when no entry is free it is not tracked
(`read_mshr_full_rejects`). A later read of the same logical page that reaches
ingress while the entry is live pays only `hbf-read-mshr-check-ns` (default
0.125) on the ingress port instead of the scheduler issue, skips the L2P lookup
and the page-read credit, and then joins the pending fill as above, so SRAM
transfer and response stay per request. At most
`hbf-read-mshr-targets-per-entry` (default 64) followers attach; further reads
take the ordinary path. Entries retire by the time a request reaches ingress,
not by host arrival, so a burst queued at ingress observes fills that completed
while it waited. Any write, erase or page invalidation retires every entry.
Multi-page reads, physical/static requests and structural mapping policies do
not use it, and followers do not consume thermal pacing budget.
`read_mshr_allocations`, `_joined_reads`, `_joined_bytes` and
`_join_fallbacks` count its use. All three values are exploratory assumptions.

`hbf-ingress-buffer-tags=true` (research option, default off; requires the read
MSHR) extends the same ingress check to pages the stack already buffers under a
logical tag. A logical single-page read is served after the check from the
write buffer when its dirty ranges cover the request, or from the decoded read
buffer when the page's MSHR entry is live and the page is still resident; a
logical single-page write to a page that already has a coalescing write-buffer
entry is staged after the check. None of them pays the scheduler issue, the L2P
lookup or the page-read credit; SRAM, write-buffer DRAM and response stay per
request. Completed entries remain usable only while their page is resident at
modeled ingress time. A read-only residency probe rejects a stale tag before
charging a fast-path check; capacity pressure also reclaims expired tags and
obsolete heap records before evicting a live entry. `read_mshr_stale_cleanups`
counts invalid metadata removed, separately from `read_mshr_evictions` (capacity
evictions). Future fills are retained and the probe never touches the LRU or
cancels an already reserved consumer. A logical write retires only its own
pages' entries; relocation, erase and restore still retire all of them.

This is lazy causal revalidation, not a host-order eviction callback. The
original fixed-size research table remains the default.

`hbf-read-mshr-admission-timed=true` is an opt-in candidate-baseline policy.
Read entries become valid at the modeled instant returned by page-credit
admission, not when the host has finished scheduling their future read. A
same-page follower arriving earlier queues for that version to activate, then
pays its ingress check and joins the pending fill, without another page credit
or mapping lookup. A full target list retains the ordinary fallback policy. Per-LPN versions ordered by
activation time retain the history needed for gap-backfilled requests. A probe
selects only a version active at its own simulated instant. Writes, relocation,
erase and restore retain the existing invalidation rules.

This mode requires sharing and at least `Q + P * 2` entries per stack: Q is the
page-read credit depth and P is the number of banks per stack. Every tracked
in-flight interval starts at credit admission and ends at that read's completion,
so it is covered by the finite Q-credit schedule. Completed valid tags can name
only the bank caches' at most P * 2 resident pages. The sets are counted without
double-counting an LPN. No hardware capacity eviction is needed with this budget.
Smaller fixed-size experiments remain available with admission timing disabled;
an undersized timed configuration is rejected, not silently widened.

`hbf-read-mshr-entries-per-stack=auto` derives this conservative budget in the
native resolver from the final geometry and Q; a later explicit numeric value
wins. This is a controller design policy, not a measured SRAM size. The software
record count may exceed this hardware budget during an open-loop future burst.
`read_mshr_occupancy(stack, time)` reconstructs in-flight/resident occupancy for
diagnostics separately from future/history records; it does not run on the hot
path or model a physical unbounded request queue. Reservations waiting before
admission remain part of the existing open-loop source/scheduling assumption.

`read_mshr_allocations` counts created reservation records, not simultaneous
hardware occupancy. `read_mshr_deferred_allocations` counts records activated
later than their original ingress start; `read_mshr_before_admission_reads`
counts followers queued for future activation. The new policy is opt-in; it does not freeze or enable fine-v1 by itself.
`ingress_tag_read_buffer_hits`,
`ingress_tag_write_buffer_hits` and `ingress_tag_writes` count its use.

Regression coverage includes a 20,000-event independent serial LRU oracle,
future-fill visibility, third-page eviction, cross-bank isolation, invalidation
without resurrection and preservation of future consumers. See
`tests/cpp/hbf_read_buffer_pressure_test.cpp`,
`tests/cpp/hbf_read_buffer_handoff_test.cpp` and
`tests/cpp/ocp_standard_test.cpp`.

Very large same-time open-loop batches retain their future events until a causal
watermark advances. This diagnostic/state cost is proportional to outstanding
work and must be measured separately from simulated hardware occupancy.
