# Fine frontend baseline v1

> Status: Current
> Last reviewed: 2026-10-23

Frozen on 2026-10-23 after the approved configuration-only OCP update and paired
whole-model qualification. `open_fine_session` defaults to fine-v1; ordinary
`open_session` and C++ defaults remain unchanged. Text, binary and live inputs
share this policy, and coarse inputs may explicitly select it.

| Setting | Default and basis |
|---|---|
| Same-page read sharing | Enabled |
| Admission-timed page MSHR and buffer tags | Enabled; software future history separate from hardware entries |
| Entry budget | Native auto: Q +2P, reference4608 per stack |
| Targets per page |64; OCP v0.7.0 section 5.3.1 items 1/3, six ID bits for64 commands per4KiB |
| Ingress interval |0.03125ns/stack =0.5ns/channel /16; independent channels, sections 4.3/5.1, grade 2 approximately 2GHz AXI |
| Mapping workers |64 total; nominal host policy |
| Host mapping compute |5ns; nominal internal model value, not an OCP timing |
| Ordinary-path issue |2ns; nominal base-die scheduler assumption |
| Write-buffer data |Existing shared HBM calendars, no extra stack serial issue slot; nominal host-managed architecture |

The aggregate interval retains one stack resource. It approximates per-channel
admission under channel imbalance and is not a measured tag/SRAM latency. Other
channel counts/grades require explicit overrides. Buffer sizes, media timings and
HBM bandwidth inherit the system. See configs/parameter-provenance.json for source
clauses and the distinction between standard-derived and nominal values.

Configuration order: system, frontend, user overlays, explicit options. Native
auto capacity uses final geometry. Overrides are custom-derived and recorded
with hashes, resolved values, binary/source identity and drain policy.
`sharing-only` and `page-mshr` are reference ablations; `legacy` injects nothing.

## Qualification scope

Qwen2.5-1.5B, SGLang BF16, batch 1, P128 + 1 actual decode; weight-HBF, dense
objects, cold HBF session,515 call barriers, memory-only open-loop replay.
Existing modeled40MiB GPU-cache output includes final GPU-cache drain; HBF
terminal drain is separate from serving time. The fine input retains a known
cache re-read artifact and is not hardware truth.

| Input | Prefill ms | Decode ms |
|---|---:|---:|
| Fine |3.985065|3.703345|
| Enhanced coarse |3.690610|3.682973|
| Fine/coarse |1.079785x|1.005531x|

Historical0.125/0.25ns inputs remain sensitivity results, not the frozen default.
The executable was built from clean source 19338be73d5813686a77665f249328f3fe5ec1eb
before local commit-message cleanup. Its build source SHA256 is
3783bee740c46e2a3bedcb4d2693fa7a607a378e5464e9db8164b92d2ba4dc00.
Message cleanup preserves file trees; original source identities remain in the
research capture receipts. This is software qualification, not hardware timing
or GPU-cache accuracy validation. Focused frontdoor 21 checks and parameter
provenance coverage pass; the physical implementation previously passed 141/141.

The policy remains open-loop within batches. Host-HBM write accumulation and
buffered logical completion are simulator policies, rather than a claim to
model OCP non-posted4KiB writes or partial-coverage error behavior exactly.

## Receipts and diagnostics

`frontend-receipt.json` records the profile and its hash, ordered inputs,
custom-derived overrides, final native fields, binary hash and source checkout
identity. Engines lacking requested capabilities fail rather than silently
downgrade. Serving time excludes terminal drain, reported separately on session
close; no GPU inflight limit is inferred within submitted batches.

The native `read_mshr_history_*_peak` diagnostics are cumulative high-water marks:
use the maximum across batches, never the sum. Payload bytes cover version-vector
allocated capacity only; hash nodes, vector metadata and allocator costs are
excluded. Measure process peak RSS separately for total host memory.
