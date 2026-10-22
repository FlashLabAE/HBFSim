# Verification coverage

> Status: Current
> Last reviewed: 2026-10-22

This page answers "does the code faithfully execute the declared model?". It
does not answer "is the declared model the future HBF silicon?". The former is
covered by the tests in this repository; the state of parameter calibration
and external hardware validation is described in the
[evidence policy](../reference/evidence-policy.md).

## Kinds of evidence

| Kind | What it checks | Mainly finds | Cannot prove |
|---|---|---|---|
| Exact conservation | Requests, pages, bytes, mappings, relocations and link traffic agree item by item | Loss, duplication, misrouting, misaccounting | That the timing model or its parameters are real |
| Hand-derived timing | Lower bounds, upper bounds or tolerance intervals derived from the explicit configuration | Hidden latency, missing gates, early completion | That the configured values were measured |
| Sharp regimes | States constructed to produce order-of-magnitude or directional differences | A mechanism not taking effect or taking the wrong branch | That the mechanism's magnitude matches hardware |
| Theoretical bounds | Mathematical bounds, monotonicity, capacity limits, time-shift invariance | Scale-dependent drift, wrong metric definitions | The complete microarchitecture |
| Cross-consistency | Two independent paths or a replay must agree | Divergence in the composition layer, serialization or reporting | That both paths are not wrong together |
| Fail-closed | Illegal ranges, capacities, NaN/Inf and causal regressions must be rejected | "Plausible-looking" invalid results | That a legal input is physically real |

These kinds of evidence complement each other, but even when all of them pass
they only establish high internal confidence of the current implementation in
the current model. The existing Ramulator2/MQSim comparisons cover two
explicitly narrow facets; broader external confidence still needs parameter
calibration and validation on data sets that took no part in calibration. The
hardware measurements behind the calibrated overlays are described in
[evidence](../../evidence/README.md).

## Module coverage

### HBM

The only current model is `channel-aggregate-v2`; DRAM row state and refresh
events are no longer verified or reported. A service group holds 4
pseudochannels by default (`service_group_channels`); group size 1 serves as
the precision-sensitivity control on the same path. The canonical ledger
resolves to service groups: every touched group records one `hbm/group{N}`
event whose service duration is the burst count of the group's busiest lane
(an isolated transfer keeps the lane bandwidth; disjoint concurrent lanes in a
group serialize) and whose `physical_bytes` are the burst-rounded bytes of all
lanes in the group. The ledger reducer recomputes read/write bytes, bus work,
completion times and the number of active groups exactly from these events;
per-lane counts (`active_pseudo_channels`, `max_pseudo_channel_accesses`) are
not in the events, so the reducer gives only bounds from group events (every
active group holds 1..width active lanes; the busiest lane lies between
ceil(group bursts / width) and the group's bursts), while the exact values come
from the independent oracle's per-burst counts compared field by field with
the production summary.

| Mechanism | Check | Entry |
|---|---|---|
| Frequency / width / BL consistency | The interface derives physical bytes and raw bandwidth; effective bandwidth decides service time | `physical_probe hbm-interface` |
| Addresses and capacity | Unaligned bursts round up, address mapping is invertible, over-capacity is rejected; an independent per-burst enumeration checks the aggregate traffic | `physical_probe hbm-boundaries`, `hbm_channel_model` |
| Channel parallelism | Multi-channel speed-up at fixed work; channels are independent | `physical_probe hbm-channels` |
| Bounded queues and fairness | Large and small requests alternate, capacity back-pressure, external bounds limit aggregation, single-parent merging agrees with per-fragment progress | `hbm_channel_model`, `simulation_session_contract` |
| Controller buffer sharing HBM | Physical capacity reservation, application-address isolation, channel contention, future reservations and gap backfill, common-frontier reclamation, HBM/HBF traffic conservation | `hbm_controller_buffer`, `hbm_channel_model` |
| Read/write turnaround | The configured direction-change cost applies to application channel service | `physical_probe hbm-turnaround` |
| Clock rounding | One-ULP boundary residuals, time-shift invariance, long runs | `hbm_clock_roundoff` |
| Synchronized ledger | Independent per-burst address counts, the busiest-lane interval reference per service group, per-group read/write direction switches; completion times, physical bytes and per-lane counts are checked; the fuzz also varies group width 1/4 | `hbm.channel-service`, `hbm.burst-boundary`, `hbm.map-exhaustive`, `hbm.dual-pch-overlap`, `hbm.rw-turnaround`, `oracle_property_fuzz` |

### HBF device contract and Host management

The current boundary is OCP v0.7.0. Protocol framing and complete electrical
conformance are outside this transaction simulator; see [model](../reference/model.md).

| Contract | Evidence entry |
|---|---|
| Grades 1/2/3, per-channel command/RX/TX work, geometry limits, aligned reads and sequential writes | `ocp_standard_contract`, `hbf_ocp_v070_grades_effective` |
| Same-Bank sensing serializes; different Banks overlap; trace on/off preserves service | `physical_probe hbf-bank-pipeline` |
| ECC initiation throughput differs from response latency; OOB stays internal | `physical_probe hbf-ecc-pipeline` |
| Two decoded pages per Bank, temporal fills, eviction, invalidation and future-gap scheduling | `hbf_read_buffer_pressure_test`, `hbf_read_buffer_handoff_test` |
| Host subpage assembly sends one complete device page; alignment covers both request ends | `physical_probe hbf-host-boundary` |
| Program/read/erase barriers; first and reused page-zero programs pay actual erase | `physical_probe hbf-program-barriers` |
| Independent interval oracle for logical reads, subpage writes, Host copies, zero-live and live-page reclamation, and hybrid routing | `canonical_oracle_cases` |
| Host mapping ledger: every lookup or update pays `mapping_control_compute_ns` (`mapping_lookup_compute` / `mapping_update_compute`) on the shared host compute pool (one worker per stack; equal starts choose the most recently freed worker, idle since no earlier than the request arrival), then one `ctrl_dram_issue_ns` issue on `host/mapping/partition{stack}` whose entry transfer shares the HBM service-group calendars; an update then publishes for `mapping_update_ns` (`mapping_publish_compute`) on the pool. User and GC updates share that stage: the reducer derives GC updates from relocated data pages, requires one publication per update access, and reconstructs `mapping_compute_work_ns` / `mapping_compute_ops` from the `mapping_compute` events. Case notes state the rule; fuzz also charges zero control work | `canonical_oracle_cases`, `oracle_property_fuzz` |
| Resident/cached Host mapping, page/entry/affine layouts, GC reserve progress and exact metadata capacity | `hbf_mapping_cache_test`, `hbf_mapping_layout_test`, `hbf_gc_cached_mapping_progress` |
| Host GC and static wear leveling use ordinary HBF transfers and finite copy slots | `hbf_gc_reserve_test`, `hbf_static_wear_leveling_test`, `verify_write_amplification` |
| Program classes and WAF conserve bytes; logical reclaim count differs from physical erase count | `verify_write_amplification`, `gc_waf_quick_accounting` |
| Channel-local, block-aligned zones; invalid-only swaps; P/E stays with physical blocks | `hbf_host_zones_test`, `test_hbf_host_zones.py` |
| Canonical v5 persistent Host/media image; fresh-process restore preserves physical wear and future work | `hbf_persistent_image_test`, `simulation_session_checkpoint_contract` |
| Exact ownership and publication accounting for compact mutable populations | `hbf_compact_mutable_test`, `simulation_session_contract` |
| OCP section 9.2 thermal modes on the RC node: light pacing at the budget, severe halt with CATTRIP until the node cools below LTT, Light-then-Normal cool-down order, TTTEMP grid and threshold order, fail-closed shutdown; R, C and energies remain assumptions | `hbf_thermal_test` |
| Thermal physics under LLM-shaped traffic through the engine: media heat equals per-operation energies, a decode loop heats along the RC closed form, light throttling caps reads at budget over energy per bit and monotonically with the package boundary, release below RTT after the RC cooling time, CATTRIP halts last the STT-to-LTT cooling time, programs heat an order of magnitude more than reads and GC adds the write amplification | `hbf_thermal_llm_physics` |
| HBM thermal node and package network: HBM heat equals served bytes times energy per bit, an HBM node relaxes on its RC curve, the coupled GPU/HBM/HBF steady state matches the linear system it represents, zero coupling reproduces the configured environment, and a cascaded HBF deposits pass-through PHY energy in its HBM base die | `package_thermal_network` |
| Package coupling through the engine under Peer- and Tiered-shaped traffic: Tiered fills heat the HBM stacks and raise the HBF effective boundary more than Peer traffic for the same logical bytes, cascaded pass-through heat equals HBF payload bytes times the link energy, and a session without the package model keeps the uncoupled engine's trajectory | `package_thermal_coupling` |

A passing test establishes its stated contract only. The independent Python
Host oracle is bounded to its declared tiny request sequences; arbitrary
concurrent GC policy behavior is covered by native causal/conservation tests,
not claimed as a separately implemented full Host controller.

### Composition layer and initial state

| Mechanism | Check | Entry |
|---|---|---|
| FLAT routing | Exact request counts below and above the boundary; page-crossing requests split per placement | `flat-address-routing`, `verify_physical` |
| HBF placement | A contiguous data stripe puts exactly one page on each stack and mapping pages stay stack-local; an adversarial `8×k` stride is still spread by group rotation; `direct-read` static pages reuse the same data placement and are a full bijection on small geometries, invertible, with out-of-range rejected | `composition-static-mapping` |
| Static block isolation | Blocks holding static pages never enter mutable FTL allocation | `composition-static-mapping` |
| `direct-read` bypasses the FTL | Eligible reads use only static physical HBF; logical reads/writes, mapping, programs and D2D are all zero, writes stay in HBM | `composition-kind-blind`, `verify_physical` |
| Initial image | A read infers an old page only when it observes bytes no earlier write covered; a full-page write followed by a read never reads the future | `composition-initial-image`, `composition-reuse-routing` |
| Lazy sequential initial image | When the ends of an HBF range fall in partial mapping pages, the compact representation and the materialized trace give identical scenario results field by field; with hbf-streaming off its buffer parameters take no HBM and do not block direct-only | `lazy_sequential_equivalence_and_option_scoping` |
| Cooperative writes | Destaging follows explicit watermarks, bounded slot pressure or the final drain; fixed page slots; same-page and overlapping subpage merging; unaligned page-crossing splits; destaged bytes equal D2D write bytes; split per stack | `composition-coop-write`, `composition-write-conservation` |
| Read-after-write consistency | Parked and foreground-write versions are served from HBM and never fall back to stale raw HBF | `composition-reuse-routing`, `composition-read-after-write` |
| hbf-streaming initial layer DMA | Every NAND block of the complete read-only weight population is physically isolated before any FTL allocation; actual weight reads use pre-resolved, cross-stack striped static extents; mutable cold/overflow KV always uses the logical FTL from its initial version, hot KV stays resident in HBM, and nothing falls back to a stale static version | `summary_hybrid_residency_observability`, `composition-static-mapping` |
| hbf-streaming read/write order | Read-before-overwrite reads the old page; a first-touch full-page overwrite skips it; same-page write-back completes by version | `summary_hybrid_residency_observability` |
| hbf-streaming double-buffer conservation | HBF (static + logical) → D2D → HBM install and HBM → D2D → HBF write-back conserve page by page and byte by byte; odd/even buffer reuse has a causal fence | `summary_hybrid_residency_observability` |
| hbf-streaming capacity and scheduling modes | An explicit residency contract checks the trace bytes/SHA-256 and rejects `max-ops`; runtime overhead/block tables and the plan's hot KV stay resident, untouched objects still count toward capacity, weights and cold/overflow KV are backed by the chosen tier; byte-exact pressure basis, per-object page rounding, double buffers, and resident/backing/unused partitions conserve and never exceed HBM; unknown objects, out-of-range or aliased addresses, KV overflow, page-granularity drift, partial contracts and insufficient buffers all fail closed; phases/layers are independent | `summary_hybrid_residency_observability`, `external_backing_layer_streaming_contract` |
| External backing without HBF | On-package LPDDR, host DRAM, CXL memory and NVMe SSD share one M2S → controller → media → S2M device path; address pages, caller ranges and transport segments are accounted in layers; read/write caller ranges pick a queue round-robin per direction, a range's segments stay on one queue and stripe by address onto that queue's channels, and one queue/channel timeline is shared between reads and writes; a direction's aggregate bandwidth is divided evenly over its queues × channels; the global end-to-end credit, earliest-gap backfill, returned completions, payload/protocol/wire, local-span utilization and the address heatmap conserve; at equal page granularity the hbf-streaming plan and HBM traffic of HBF and external backing are identical field by field | `external_backing_device_contract`, `external_backing_layer_streaming_contract` |
| Independent External implementation | Eight canonical cases cover read/write/full-duplex/controller-media queue/global outstanding/cross-channel backfill, a host-DRAM 16 KiB range split into 2 segments by 8 KiB transport commands covering 4 address pages, and two simultaneously arriving read ranges assigned to two queues with no segment crossing a queue; the independent Python scheduler and ledger reducer call no production helper and verify that shared queue/channel resources do not overlap and that a direction's bandwidth divides by queues × channels; the fuzz also varies read/write queue counts, and 32 seeds, time-shift/ID-rename metamorphic tests and two deliberate production faults are all caught by the gate | `canonical_oracle_cases`, `oracle_property_fuzz`, `verify_mutations` |
| Optional kind | A trace without semantic kinds is fully defined in every composition | `composition-kind-blind` |
| Behavior-only placement | HBF as the authoritative backing, bounded HBM page tier; first-touch bypass, promotion on reuse, true LRU, dirty write-back; the complete decision ledger matches the independent Python oracle; erasing semantics, address bijection, time shift, future suffixes and window/device timing never change the policy; ten classes of production mutation must all be killed | `behavioral_tiering_policy`, `placement_oracle_differential`, `verify_mutations` |
| Behavior workload eligibility | Occupied footprint, address-span inflation, read/write counts, reuse, exact LRU stack distance and the working-set/HBM relation are declared by the case up front and fail closed; performance plays no part in eligibility | `trace_locality_analyzer`, `workload_quality_contracts` |
| Time semantics | Completion is never earlier than arrival + service; Direct same-phase parent requests rotate by page and same-page reads/writes keep order; hbf-streaming layer prefetch obeys admission and layer dependencies; independent HBM/HBF resources are not blocked by global head-of-line | `arrival-time-causality`, `summary_hybrid_residency_observability`, `composition-reuse-routing`, `verify_physical` |
| Outstanding credit | Credits count 4 KiB physical page transactions, not parent trace records; a full window waits only for the earliest completion; equivalent 4 KiB and large-request expressions give the same physical completion span | `closed_loop_window_completion_order`, `composition-reuse-routing` |

### Metrics, configuration and reproducibility

| Contract | Check | Entry |
|---|---|---|
| Time-origin invariance | Shifting every arrival by 1 ms changes the active span, throughput and utilization/parallelism by at most 4 ULP | `time-origin-invariance` |
| Illegal configuration rejection | Negative timings, NaN and Inf all exit non-zero | `invalid-config-rejection` |
| Configuration replay | Replaying the exported resolved configuration gives an identical scientific payload | `ctest` replay tests |
| Provenance | Schema, git commit and dirty flag, build, argv/cwd, trace SHA-256 and the complete configuration are all present | `analytical_microbench`, `certificate_contracts` |
| Analytical bounds | HBM busy work is recomputed exactly from bytes/bandwidth; the 25-point media/HBIO phase boundary of the isolated HBF read pipeline cancels the start-up term with two length sets; the Direct mixed makespan equals the slower of the independent tiers with both sides' resource work unchanged; dropped points, changed ranges or serialization are rejected | `analytical_microbench`, `certificate_contracts` |
| Certificate binding | Certificate schema v8 binds `hbfsim`, the ledger probe, the physical probe, every tracked external-evidence input and the sanitizer platform/runtime policy, and requires the behavior-only placement differential and the analytical gate; a summary obtained directly is fixed at `exploratory_unattached`, and a certificate is attached only after verifying a clean commit, the input digests and the exact binary | `certificate_contracts` |
| Metric naming | User-completion throughput and drain-inclusive makespan throughput are reported separately; the primary latency is offer→completion, with the service and source distributions kept independently; wall-clock span, latency work, stage work and resource busy time are separate domains | summary schema v18, `summary_time_breakdown_contract` |
| Time-breakdown contract | offered + post-offer = user completion; user completion + drain = makespan; JSON and CSV scalars agree; trace off/full does not change the result | `summary_time_breakdown_contract` |
| Time reports | The report renderer produces long-form CSV, Markdown and self-contained HTML from the summary; the visualization facets additive wall time, latency work, overlapping stage work and resource capacity; parent/child attribution, rejection of bad contracts before writing, and atomic outputs are all gated | `time_breakdown_report_renderer`, `time_breakdown_visualization_renderer`, `component_contracts` |
| Address heatmap accounting | Five domains, fixed source order, read/write/erase directions, exact conservation of bin bytes, cross-bin access fan-out, the `UINT64_MAX` top address and the `2^64` exclusive end, fixed capacity and fail-closed overflow | `address_heatmap_accounting` |
| Address heatmap integration and rendering | Every v18 scenario embeds a heatmap; HBM/HBF/external physical bytes agree with the device statistics; the hottest bin of zero traffic is n/a; multiple scenarios require an explicit selection; the C++ JSON is strictly validated by Python and rendered as HTML with no network dependency | `summary_time_breakdown_contract`, `address_heatmap_renderer`, `address_heatmap_cpp_python_interop`, `external_backing_layer_streaming_contract` |
| hbf-streaming layer-streaming observability | Mode, explicit-contract flag, byte-exact pressure basis, page-allocated footprint/rounding, layer count, explicit layer/compute, the complete fixed/hot-KV/backing/unused HBM partition, stream/write-back page and byte conservation; the backing credit limit, actual peak concurrency and admission wait are checkable; user accesses all go to HBM; exposed/hidden prefetch and buffer-reuse waits are checkable; JSON/CSV/console agree | `summary_hybrid_residency_observability` |
| HBF wait observability | The four cumulative work classes (ingress, downstream scheduler, ECC issue, write-buffer slot) agree across JSON/CSV/console; the underlying queue pressure is triggered independently by the physical probe | `summary_time_breakdown_contract`, `physical_probe all` |
| ECC and data boundary | Internal work on raw page+OOB and the external payload are counted separately; the ECC initiation interval, response latency and host copies are modeled separately | `hbf_ecc_pipeline_contract`, `verify_physical` |
| Bank decoded cache | Two pages per Bank, eviction of a third, cross-Bank isolation, future fills invisible, page-zero erase clears the cache; a Host logical invalidation never fakes a device cache clear | `ocp_standard_test`, `oracle_property_fuzz` |
| Program completion and reclamation | No page-skipping writes; each page's own program completion constrains reads; page zero erases automatically; a Host zone reset never double-counts erases | `ocp_standard_test`, `hbf_host_zones_test` |
| Mapping / GC publication order | The ownership of pending programs, mapping publications and Host reclamation conserves after the drain; every relocated live data page publishes exactly one GC mapping update through the user L2P path, and mapping-page relocation only performs a checkpoint relocate; random cases are compared with the independent oracle and the stress test checks reclamation progress | `canonical_oracle_cases`, `oracle_property_fuzz`, `hbf_gc_cached_mapping_progress` |

## Quality gates

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target verify_physical
cmake --build build --target verify_components
cmake --build build --target verify_write_amplification
```

CI runs the corresponding gates on Linux and macOS for every push, and the
Address/UndefinedBehavior Sanitizer build on pull requests and weekly. Any
model change must update
the test expectations, the parameter evidence and this map together; updating
a result baseline alone to "accept" drift is not allowed.
