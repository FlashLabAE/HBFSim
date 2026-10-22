# Design-space recipes

> Status: Current
> Last reviewed: 2026-10-22

Each recipe below is a common HBF design question with commands that were run
against this revision. Result tables are examples from the shipped profiles;
the point of a recipe is the method, and your numbers will move with the
workload and parameters. Commands run from the repository root after
`python3 -m hbfsim build`.

## Before you start: match the workload to the question

The built-in smoke trace (`--demo-tokens 4 --demo-layers 2`, 1,920 accesses;
`16`/`8` gives about 55,000) finishes in a second but touches a few megabytes.
It exposes **latency, FTL and write-path** effects well. It does **not**
stress capacity, link bandwidth or thermal limits:

| Question depends on… | Use |
| --- | --- |
| latency, mapping, write buffering, WAF | the smoke trace, possibly larger (`--demo-tokens 16 --demo-layers 8`) |
| sustained bandwidth | a burst through the engine ([example 3](../../examples/03_read_bandwidth_ceiling.py)) or a long trace |
| GC, wear and lifetime | a write-heavy trace on a small HBF (recipe below) |
| capacity and HBM:HBF ratio | real model footprints through [ServeLoop](serveloop.md) |
| thermal throttling | a steady-state start (`thermal-steady-state-start`) or sustained load over seconds; see [Thermal limits](#thermal-limits) |

Two habits keep sweeps honest: compare points that replay the **same trace**
(`sweep` does this for you), and read every **warning** — an open-loop arrival
rate above a device's peak turns latency into queueing time. Add
`--set max-outstanding-requests=64` for closed-loop admission when latency is
the claim.

## How far is HBF from HBM, and which placement closes the gap?

```bash
python3 -m hbfsim quickstart
```

On the smoke trace, `all-hbf` is three orders of magnitude slower than
`all-hbm`; `flat`, `direct-read` and `hbf-streaming` land in between depending
on which data they keep in flash. Change the placement boundary with
`--set flat-hbm-bytes=...` or the layer buffer with
`--set layer-buffer-bytes=...` and rerun. [Example 1](../../examples/01_first_comparison.py)
shows how to read individual numbers out of the result.

## Interface speed grade or NAND sensing: what limits read bandwidth?

```bash
python3 examples/03_read_bandwidth_ceiling.py
```

```text
     grade      tR=4us      tR=2us      tR=1us
    grade1         488         914        1300
    grade2         917        1628        2347
    grade3         918        1631        2355
```

Grade 1 → 2 doubles bandwidth, but grade 3 adds nothing at any tR: with 256
banks per stack and 4 KiB pages, array parallelism and internal paths cap each
stack before the 192 GB/s-per-channel interface does. The runner says so
directly — `run --overlay ocp-v070-grade3` warns that the ECC, raw channel and
TSV ceilings are below the interface bandwidth. Try raising
`hbf-ecc-decode-raw-bw`, `hbf-channel-bw` or `hbf-tsv-bw` together with a
shorter `hbf-read-ns` to find the next bottleneck.

## What does the FTL mapping strategy cost?

```bash
python3 -m hbfsim sweep --overlay cached-l2p-1-over-1000 \
  --vary overlay=mapping/page-cache,mapping/entry-cache,mapping/compressed-cache,mapping/block \
  --scenarios all-hbf --demo-tokens 16 --demo-layers 8 --jobs 4 \
  --metrics makespan_us,mean_latency_us,p95_latency_us,hbf_waf,hbf_block_erases
```

```text
overlay                   scenario  makespan (us)  mean lat (us)  p95 lat (us)  HBF WAF  erases
mapping/page-cache        all-hbf           5,441          63.16         594.4    1.792     172
mapping/entry-cache       all-hbf           9,198          24.32         45.81    1.792     172
mapping/compressed-cache  all-hbf           5,700          61.98           492    1.792     172
mapping/block             all-hbf          71,816          521.2         2,947    14.48      13
```

`cached-l2p-1-over-1000` gives the mapping cache an HBM budget of 1/1000 of HBF
capacity; the swept overlay then selects what the cache holds. Entry caching
trades makespan for much better request latency here; block mapping needs a
tiny table but rewrites whole blocks on small updates (8× the WAF). The fully
resident page table (`mapping/resident`) does not fit this budget, and the
sweep reports that point as rejected. Vary the budget with
`--set hbf-ctrl-dram-capacity-denominator=...`. The
[mapping organizations reference](../reference/hbf-mapping-organizations.md)
describes every strategy; [example 2](../../examples/02_ftl_mapping_tradeoffs.py)
is the same study as a script.

## Does NAND program time matter?

```bash
python3 -m hbfsim sweep --vary hbf-program-ns=37500,75000,150000 \
  --scenarios all-hbf --demo-tokens 16 --demo-layers 8 --jobs 3 \
  --metrics makespan_us,mean_latency_us,p95_latency_us
```

```text
hbf-program-ns  scenario  makespan (us)  mean lat (us)  p95 lat (us)
37500           all-hbf           4,849          25.12         81.03
75000           all-hbf           4,924          25.12         81.03
150000          all-hbf           5,074          25.12         81.03
```

Request latency does not change: the host write buffer acknowledges writes and
programs flash in the background, so tPROG only lengthens the drain at the end
(makespan). Program time reaches the critical path once writes outrun the
buffer — shrink `hbf-write-buffer-pages` or use a write-heavy trace to find
that point.

## Wear leveling versus write amplification

GC and wear need writes that fill the device. Generate a hotspot write stream
and replay it on a scaled-down HBF (`endurance-regime-scaled`: 4 MiB raw, so
GC runs within seconds):

```bash
mkdir -p out
python3 workloads/synthetic/generate.py --output out/hot-writes.trace \
  --pattern hotspot --pages 700 --ops 20000 --read-percent 0 \
  --hot-page-percent 10 --hot-access-percent 90 --seed 1
python3 -m hbfsim sweep --system 4hbm-4hbf --policy none \
  --overlay endurance-regime-scaled --trace out/hot-writes.trace \
  --vary hbf-gc-wear-leveling-weight=0,0.5 --scenarios all-hbf --jobs 2 \
  --metrics makespan_us,hbf_waf,hbf_block_erases,hbf_max_block_erases,hbf_gc_relocations
```

```text
hbf-gc-wear-leveling-weight  scenario  makespan (us)  HBF WAF  erases  max P/E  GC copies
0                            all-hbf       1.876e+06    1.005   2,520      118        106
0.5                          all-hbf       2.401e+06    1.137   2,847       44      2,732
```

Without wear leveling, GC keeps recycling the hot blocks: the most-worn block
reaches 118 erases while writes stay nearly unamplified. Weight 0.5 moves cold
data out of fresh blocks, cutting the worst block to 44 erases — about 2.7×
more lifetime — for 13% write amplification and a 28% longer run. Each run
directory's `summary-hbf-wear/all-hbf.html` maps the per-block wear. Lifetime
projection and zone management are covered by the
[host HBF management reference](../reference/host-hbf-management.md).

## Is HBF a better capacity tier than DRAM, CXL or SSDs?

```bash
python3 examples/06_capacity_tier_media.py
```

The backing overlays (`host-dram`, `cxl-memory`, `on-package-lpddr`,
`nvme-ssd`, `cxl-ssd`, plus `cxl-ssd-cached` for a device DRAM cache) swap the
device behind HBM, and `external-streaming` streams layers from it exactly as
`hbf-streaming` does from HBF. Most envelopes are exploratory; the
`backing/calibrated/` overlays carry measured A100/H200 host timings. Heed the
open-loop warnings, or add `--set max-outstanding-requests=64`.

## How much HBM versus how much HBF?

The topology profiles `2hbm-6hbf`, `4hbm-4hbf`, `6hbm-2hbf` and
`eight-stack-baseline` keep eight stack slots and vary the split (the
`miniquick/` variants are capacity-scaled). A ratio question is a capacity
question, so it needs a workload whose footprint approaches HBM: model
weights plus KV cache for a real model and batch. Run it through
[ServeLoop](serveloop.md) with `--system configs/systems/<topology>.cfg`. On the
smoke trace every topology fits in HBM and the results barely differ.

## Thermal limits

HBF follows the four thermal modes of OCP HBF v0.7.0 section 9.2. Each stack
has a lumped RC node for its junction temperature; reads, programs and erases
deposit energy and the node cools toward the package boundary. At LTT
(`hbf-thermal-throttle-c`, 85 C in the shipped profiles) the firmware enters
**light throttling** and paces media operations to a power budget
(`hbf-thermal-throttle-power-w`; `0` derives the budget that holds the node
at LTT, 18 W per stack at the 45 C boundary, 5.5 W at 70 C). If the node still
reaches STT (`hbf-thermal-severe-c`, 95 C) the stack enters **severe
throttling**: it asserts CATTRIP and accepts no command until it has cooled
below LTT. At `hbf-thermal-shutdown-c` (105 C) the run fails closed. The
[model reference](../reference/model.md#thermal-reliability-and-evidence-limits)
has the entry and exit rules.

The thermal time constant is 0.6 s in the shipped profiles (R × C), so a
millisecond trace never heats a stack from cold while a few seconds of
traffic bring it to its steady state. Two overlays make the modes
observable: `thermal-steady-state-start` boots every stack at LTT with
pacing active (the steady state of a stack that has been serving for a
while), and `thermal-boundary-55c`/`65c`/`70c` raise the package boundary,
which shrinks the sustainable budget. `package-coupling-liquid-cooled`
replaces the fixed boundary with a package network: a 700 W GPU and the HBM
stacks (each HBM stack is its own thermal node fed by the bytes it serves)
heat the HBF stacks through lid and interposer conductances, so the boundary
becomes a result of the workload; `package-cascaded-hbf` on top of it hangs
every HBF stack behind an HBM base die, whose PHY then carries the HBF bytes.
Under that network Tiered placements, which write every HBF byte into HBM and
read it back, heat the HBM stacks and through them the HBF boundary more than
Peer placements that read HBF directly. Example 7 issues one read burst under
the thermal model off, light throttling at a 70 C boundary, and a 16.5 W
budget that boundary cannot carry (with a compressed thermal mass so the
heat-up fits the burst):

```bash
python3 examples/07_thermal_throttling.py
```

```text
               run      budget W          GB/s   paced reads  halted reads       CATTRIP        peak C      light us     severe us
       thermal off             -        1012.1             0             0             0             -           0.0           0.0
  light throttling           5.5         336.1        16,384             0             0          85.0         781.1           0.0
 severe throttling          16.5         453.2        16,384        16,284           652          95.1         318.8         254.5
```

Light throttling caps the burst at the budget divided by the energy per bit
(5.5 W ÷ 64 pJ/B ≈ 86 GB/s per stack, 3.0× below the array). The 16.5 W
budget runs faster but the stacks heat to STT every few microseconds of
compressed time, CATTRIP goes high 652 times, and the burst spends 255 µs
halted against 319 µs throttled: a budget the cooling cannot carry is a
fault condition, not a performance setting. The same telemetry is in every summary and session receipt:

```bash
python3 -m hbfsim run --system 2hbm-6hbf --overlay thermal-steady-state-start \
  --scenarios all-hbf \
  --metrics makespan_us,hbf_peak_temperature_c,hbf_thermal_paced_ops,hbf_thermal_halted_ops,hbf_cattrip_assertions
```

`hbf_stats.thermal_*` in `summary.json` carries paced and halted operations,
light and severe entries, the time in each mode, media energy and the peak
and final temperatures; a session's `device_delta.hbf` reports the same
counters per batch and `final_device_totals["hbf"]["state"]` the end state.
The energy and RC values are derived from published measurements and stack
geometry, the boundary and thresholds are study choices, and none is an HBF
product measurement: [HBF thermal parameters](../reference/thermal-parameters.md)
gives each derivation and its range, so compare mechanisms and
sensitivities, not absolute temperatures.

## Your own idea

A policy that decides what to keep in HBM is a Python function over
transactions: [example 5](../../examples/05_custom_policy.py) prototypes a
promotion cache in about sixty lines. [Extending HBFSim](extending.md)
explains the next steps, and the "Share a study, result, or question" issue
template is the place to publish what you find.
