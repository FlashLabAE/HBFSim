# HBF mapping organizations: implementation and entry points

> Status: Current
> Last reviewed: 2026-10-11

Six candidate mapping families are implemented in the native simulator, and
block mapping is further split into plain block and block-plus-log, so seven
organizations can be run. They share the same NAND, ECC, HBIO and shared-HBM
device models. The choice of algorithm, the sizes of the in-memory records and
the software timing are simulator policies, not an FTL specified by OCP.

## Choosing an organization

For everyday use pick an overlay under `configs/overlays/hbf/mapping/`
(`resident`, `page-cache`, `entry-cache`, `compressed-cache`, `block`,
`block-log-striped`, `extent-striped`), for example
`python3 -m hbfsim sweep --overlay cached-l2p-1-over-1000 --vary overlay=mapping/page-cache,mapping/block --scenarios all-hbf`.
The `.cfg` files in the table below are the test fixtures in
`tests/fixtures/mapping-organizations/`: a 32 MiB single-plane HBF that loads
`configs/systems/eight-stack-baseline.cfg`, then `base.cfg`, then the named
file, used to exercise each of the seven organizations over the engine
protocol.

| Fixture | Primary mapping / residency | Read, write and reclamation path |
| --- | --- | --- |
| `resident.cfg` | `page` + `full-resident` | Resident L2P; out-of-place updates; page-level GC; checkpoint by dirty mapping page |
| `page-cache.cfg` | `page` + `cached` + `page` | Resident translation directory; whole mapping pages under LRU; miss reads, dirty eviction and checkpoint |
| `entry-cache.cfg` | `page` + `cached` + `entry` | Bounded entry cache; backing mapping-page reads; dirty entries merged per backing page; GC and recovery |
| `block.cfg` | `hbf-mapping-organization=block` | LBN to data block with fixed in-block offsets; a single-page overwrite copies the block; a complete-block request replaces it directly; holes are explicitly programmed as padding |
| `block-log.cfg` | `hbf-mapping-organization=block-log` | At most one dedicated log block per active logical block; the number of logs is bounded globally; reads consult the log first; LRU evicts a log through a switch, partial or full merge |
| `extent.cfg` | `hbf-mapping-organization=extent` | Resident, ordered, authoritative extent index; updates split runs and adjacent contiguous runs merge; append allocation and cleaning of the segment with the fewest valid pages |
| `object-segment.cfg` | `hbf-mapping-organization=object-segment` | Object ID / logical-range directory plus an extent index; shared-segment appends; CREATE / SEAL / DELETE; data that survives a partial invalidation is moved |

The main implementation is `src/host/hbf_mapping_policy.cpp`; the first three
rows keep the mature page FTL in `src/host/hbf_controller.cpp`.
`MappingCacheLayout::Extent` is still used by the separate "compressed mapping
page cache" study, whose backing table is a page L2P; the authoritative mapping
of the new `MappingOrganization::Extent` is itself the extent index. The two
settings study different questions and are not interchangeable.

Block-log uses **BAST-style dedicated logs**; it does not claim to implement
FAST's shared-log policy. `hbf-mapping-log-blocks` bounds the active log blocks
of the whole device. A complete, in-order log takes a switch merge with no
valid-page copies; an in-order prefix takes a partial merge that fills in the
old tail; a random log takes a full merge that rebuilds the data block. Plain
block mapping replaces a block directly only when the caller commits a complete
block in one request; several independent single-page requests are never
silently combined into a block request.

Object mode lets several objects share physical segments. SEAL flushes the
object's data and forbids further overwrites, DELETE drops the object's buffers
and mapping, and a fully invalidated block is released at once; a block mixed
with other objects stays until cleaning. The object directory does not by
itself provide lifetime-isolated physical pools. A logical range with no
registered object cannot be read or written.

## Granularity, write-back and recovery

- The smallest addressable unit for page, extent and log updates is 4 KiB;
  plain-block data bases are NAND blocks with in-block offsets. The host
  accepts partial writes such as 64 B, reads the old page for a
  read-modify-write when needed, and finally commits complete NAND pages.
- `hbf-write-coalescing=true` enables a bounded per-stack data-page cache;
  `hbf-write-buffer-pages` sets its capacity and
  `hbf-write-buffer-flush-threshold-pages` its eviction threshold. Capacity
  pressure evicts by LRU within that stack; an explicit checkpoint flushes the
  remaining pages.
- `hbf-write-buffer-completion-requires-flush=false` acknowledges a write once
  its data is in HBM, while threshold write-back still counts toward device
  completion time; `true` waits for the NAND program. Either way a checkpoint
  is needed before a recoverable image with the new mapping can be exported.
- With HBF device DRAM enabled (`hbf-device-dram-capacity-denominator`, see
  [host-hbf-management.md](host-hbf-management.md#hbf-device-dram)) the write
  buffer no longer occupies HBM: the HBM budget of a structural primary
  mapping holds only the index and the per-stack workspace. Buffered pages are
  staged in the device DRAM of the stack that owns their LPN and the write is
  acknowledged when the data enters that DRAM; on flush, a target block in the
  same stack is programmed straight from device DRAM without crossing HBIO,
  otherwise the page moves over HBIO to the target stack. Reads share the page
  FTL's device-DRAM read cache, indexed by physical page, and a flushed page
  stays behind as a clean cache line.
- `hbf-mapping-placement=device` moves the mapping itself into each stack's
  device DRAM (see [host-hbf-management.md](host-hbf-management.md#hbf-device-dram)):
  index and lookups use that stack's DRAM port, the budget is reserved in whole
  pages from device DRAM, nothing is held in HBM and no metadata crosses HBIO;
  copies stage through device DRAM instead of HBM. The default `host` keeps
  host execution with metadata in HBM.
- The new primary mappings use two reserved NAND checkpoint slots, writing the
  body and then the commit root, alternating between them. Metadata programs
  are charged by the actual number of fixed-width records; recovery reads the
  current slot and checks its checksum, layout, ownership and valid pages.
- The persistent image format is **7**. This is an explicit quiescent
  checkpoint/restart: there is no power-loss recovery of an unfinished
  checkpoint, no per-operation WAL and no per-write durability guarantee.
  Older formats are no longer accepted.
- The new organizations explicitly reject asynchronous CRASH injection. Data
  blocks are chosen among free blocks by lower wear; the two metadata slots are
  fixed, so the concentrated wear of repeated checkpoints shows up in P/E and
  there is no hidden metadata wear leveling.
- Releasing a block only updates the host's allocation state; the next
  page-zero program performs the actual erase. Every live-page copy really runs
  NAND → HBIO → HBM → HBIO → NAND; there is no free copyback.

The host of a structural primary mapping executes commands in arrival order:
the write side's lookup and index update, allocation, metadata HBM traffic and
payload movement all sit on one host timeline (read-side lookups are described
below), while the NAND operations it issues run on their own planes without
waiting for completion. The host continues as soon as a program is issued; the
number of in-flight program pages per channel is bounded by
`hbf-outstanding-write-pages-per-channel`, and the host waits for the oldest
page when the bound is exceeded. Later commands wait only when they truly
depend on those operations: reading a page waits for its program (the native
media-ready time); copies, merges, read-modify-writes and recovery reads need
the data, so the host waits for the read; an old version overwritten or
trimmed while its program is in flight is invalidated only when that program
completes; reclaiming a block waits for every program it received; a
checkpoint (drain) commits only after all previously issued operations
complete. Unbuffered writes, and writes that require a completed flush, are
acknowledged after the data program completes; buffered writes are
acknowledged when they enter the HBM buffer.

A read-only logical range is split into foreground page transactions that use
the same bounded `hbf-page-read-queue-depth-per-stack` credits as the page
path; a credit is held from before the lookup until the page's complete
response, charged to the stack of the actual physical page, and HBM-buffered
and unmapped pages are subject to the queue too. Plain block mapping (Block) is
the exception: the pages of one logical block within the range form one block
transaction, read in block order from the plane the block lives on, and the
whole run holds a single credit from entry to its last page, so credits bound
in-flight block reads rather than page reads; striped organizations still hold
credits per page. One lookup resolves every page of one index entry (a logical
block or an extent run) within the range, as a block-mapped or extent FTL
would; later pages of the same entry consult the log directory only when the
block carries a log. Read lookups use the same host resources as the page path:
a shared pool of mapping compute workers (the same count as the page path) and
one controller-memory issue slot per stack. The probes of one lookup are
dependent round trips, but different lookups may overlap, so the host timeline
advances only to the lookup's issue and each page issues its NAND read once its
own translation completes. This compares the index organizations themselves
rather than a single-threaded host for structural mappings against a
multi-worker host for page mappings. Independent pages whose lookups finished
may overlap; same-bank NAND sensing, the shared channel, TSV, ECC, SRAM, HBIO
and the thermal budget remain under the native scheduler. The read cache
becomes available after a complete page decode, and HBIO transfers only the
64 B units a partial read covers.

## Superblock striping

`hbf-mapping-superblock-planes=N` makes the structural primary mappings work in
superblocks: one physical block at the same offset from each of N adjacent
allocation lanes (across stacks first, then across the planes of a stack), with
page o of the superblock at page o / N of lane o mod N. Block mapping, logs,
extent segments, allocation, wear leveling by the most worn member, merges,
cleaning, checkpoint and recovery all operate on superblocks, so the programs
and reads of consecutive pages spread across N planes in parallel. N must
divide the total plane count; `hbf-mapping-superblock-planes=all` resolves,
when the system configuration is parsed, to the whole HBF's plane count
(`hbf-stacks` × `hbf-channels` × `hbf-dies-per-channel` × `hbf-planes-per-die`),
which is how the `mapping/extent-striped` and `mapping/block-log-striped`
overlays are written; the page FTL stripes by page itself and requires N=1.
With N=1 every page of a logical block is on one plane and allocation only
rotates lanes between logical blocks.

Striping trades larger update and reclamation units for parallelism: updates,
merges and cleaning of a logical block that is not appended in block order are
charged per superblock. Sequentially appended data (an extent's append segment,
a block written in in-block order) gains the parallelism while keeping its
original write amplification; a block mapping with random in-block updates
amplifies its copies by the superblock. The checkpoint header records the
superblock geometry, and images of different widths cannot restore each other.

Lookups, tree walks, allocation, GC selection and HBM traffic all have time
ledgers, but they are not a calibrated CPU instruction model. Page mapping
stripes by page; a structural mapping with N=1 allocates contiguous physical
blocks, so the physical parallelism of a single request differs. Compare index
costs with a single-bank control, or with a raw-media control that reads the
exact same PPNs; multi-stack results must report the layout and the banks hit
separately, and the page mapping's striping benefit must not be attributed to
the lookup algorithm.

## Memory and metadata cost

The memory report is an explicit simulated record format, not the process heap
occupied by C++ `std::map` / `vector`:

| Record of the new organizations | Host charge |
| --- | ---: |
| Data block / allocation state and physical valid bitmap | 32 B per physical block + 1 bit per physical page |
| Active logical block header | 32 B + in-block valid bitmap |
| Log update entry | 16 B per active offset→PPN |
| Extent tree node | 48 B per run, including the 24 B mapping record and tree links |
| Object directory and range index | 96 B per object |
| Data cache | 4 KiB + 32 B of state per page |
| Per-stack copy / encode workspace | 8 KiB + pages per (super)block × 16 B |

Extents and objects are encoded on the checkpoint as 24 B per run and 32 B per
object; in-memory tree links are never written to media. The new primary
mappings reserve a worst-case budget by default, and an explicit
`hbf-ctrl-dram-bytes` may cap it; when fragmentation grows past the budget the
experiment fails instead of silently falling back to an unbounded page table.

The cleaner does not assume the host can read reverse addresses from NAND OOB
for free. It scans the authoritative extent index to rebuild the logical
addresses in the victim, charging HBM scan traffic and compute work, counted
in `reverse_scan_records`. The simulator's generation tags only verify that
copies and recovery lost no latest content; they take no part in the primary
index lookup.

`mapping_index_bytes` compares primary mappings and cache structures directly,
`block_state_bytes` lists the shared auxiliary state separately, and
`metadata_bytes` is their sum. `controller_reserved_bytes` is the HBM
reservation actually configured. The page FTL's budget holds the mapping,
cache, scratch and copy slots; a structural primary mapping's budget holds the
index, the write buffer (4096 B per page, as for the page FTL) and the
merge/cleaning workspace. Neither counts the common block state in the mapping
budget: it is reserved and reported outside it, so organizations compared under
one budget denominator are compared on mapping-specific state. A structural
index is resident and has no miss path, so a budget sweep only yields a flat
line; compare the reservation together with the actual usage.

The new organizations use the raw media scheduler internally, and the
underlying `data_programs` includes the copies and checkpoints the host
issues. For cross-organization attribution use the unified
`hbf_mapping_snapshot()`:
`data_programs + copy_programs + padding_programs + metadata_programs == page_programs`.
The underlying physical totals are kept for traffic and P/E accounting.

Internal raw programs are not extra host requests and do not count toward
`raw_physical_program_payload_bytes`; the WAF denominator of a structural
mapping uses only the caller's `logical_write_bytes`. Copies, padding and
checkpoints enter only the physical-write numerator. Structural-mapping builds
before 2026-09-22 double-counted that raw denominator; generic WAF/host-write
reports from them must be recomputed from the raw logical/physical counts,
while the unified mapping snapshot's physical totals and the WAF computed from
logical write bytes are unaffected.

The session's end-of-run receipt captures the same snapshot in
`hbf_mapping_observations.foreground/post_drain`, before and after the
terminal drain. Serving results additionally break writes down over the
`foreground`, `terminal_drain` and `total` windows, each reconciled with its
physical total. The page FTL reports GC and static wear-leveling copies
separately; Block uses `block_replacements`, BlockLog its three merge kinds,
and Extent/ObjectSegment `cleaned_segments`. The page-specific GC fields of a
structural organization are `null`; the underlying generic `gc_runs=0` does
not mean no structural cleaning took place. Foreground components must not be
paired with post-drain physical totals.

## Running it

Compare the organizations through the reference runner (same trace, same HBM
mapping budget):

```bash
python3 -m hbfsim sweep --overlay cached-l2p-1-over-1000 \
  --vary overlay=mapping/page-cache,mapping/entry-cache,mapping/compressed-cache,mapping/block,mapping/block-log-striped,mapping/extent-striped \
  --scenarios all-hbf --demo-tokens 16 --demo-layers 8 --jobs 4 \
  --metrics makespan_us,mean_latency_us,p95_latency_us,hbf_waf,hbf_block_erases
```

When driving the engine protocol directly, `run()` in
`tests/python/mapping_organizations.py` executes, with one random seed and
access sequence, complete block writes, sequential single-page overwrites,
random overwrites, 64 B fragments, reads, checkpoint, restart, invalidation and
replacement, and writes out the configuration, image, recovery result and wear
report for each of the seven organizations, keeping the counts before
write-back and after checkpoint for every phase; `mapping_observations()`
checks the foreground / terminal-drain write breakdown.

```python
session.hbf_object_command("CREATE", "create", object_id=7, first_lpn=0, page_count=256)
# Read and write this range with ordinary HBF_LOGICAL transactions.
session.hbf_object_command("SEAL", "seal", object_id=7)
session.checkpoint_image("save", output / "state.image")
stats = session.hbf_mapping_snapshot("mapping-cost")
session.hbf_object_command("DELETE", "expire", object_id=7)
```

The C++ test `hbf_mapping_organization_test` uses an independent generation
oracle to verify sustained updates, trim, cleaning, the three log merges,
cross-stack buffering, complete-block replacement and recovery. The Python
test `test_hbf_mapping_organizations.py` covers the seven configurations,
same-trace execution, initial images, object lifecycles and
checkpoint/restart. The existing page-cache, entry-cache and compressed-cache
tests keep checking mapping misses, dirty eviction and GC.
