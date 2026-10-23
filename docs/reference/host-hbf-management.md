# Host-managed HBF

> Status: Current
> Last reviewed: 2026-10-23

The current implementation separates the OCP physical device from host mapping,
allocation, collection and wear policies. See [the model](model.md) for the full
contract and [parameter provenance](../../configs/parameter-provenance.json)
for facts, assumptions and unimplemented protocol requirements.

The seven executable mapping variants, including block/log merges, primary
extent mapping and object segment lifecycles, are described in
[mapping implementations and commands](hbf-mapping-organizations.md).

OCP HBF v0.7.0 section 11.4 says the device performs neither GC nor copyback.
The host may retain mapping and perform live-data relocation, reload data from
another source, or use invalid-zone remapping for wear distribution. It does not
require every application to use a page FTL.

`host::HbfController` owns the selected page/extent mapping representation,
write buffer, finite controller-storage budget, GC reserve, victim selection,
persistent state and data liveness. `physical::hbf::HbfDevice` owns the base-die
channel-local zone permutation and physical PEC registers, along with the
grade-constrained channel transport, NAND program ordering and two
cached decoded pages per bank. GC reads and writes use this ordinary device
path. A copy slot stays occupied until the destination payload is consumed.
Controller storage is reserved in the system HBM device and deducted from
application capacity. Mapping, write-buffer and GC traffic share its data
channels with application accesses. Controller access latency/issue remain
explicit assumptions; buffer row commands are not expanded. There is no internal
copyback or separate CPU DRAM data port. See the channel-level boundary in
[model.md](model.md#host-mapping-gc-and-wear).

`hbf-write-buffer-shared-hbm=true` selects the candidate host write-buffer
policy without an additional per-HBF-stack serial data-issue port. It reuses
`host_memory_transfer` and the already attached HBM service groups for reads,
writes, overlays and destage assembly. The configured controller access latency
remains a response floor; HBM burst rounding, finite bandwidth and contention
with metadata and application data remain charged once. Buffer placement keeps
the existing capacity-bounded per-HBF-stack partitions and cursor interleave;
it does not imply HBF stack i maps to HBM stack i, nor physical row locality.
The default is false, preserving the old issue calendar. Device-local DRAM
buffering is incompatible with this host-only policy and is rejected. This is
an uncalibrated controller design choice. `write_buffer_dram_issue_busy_ns` and
its wait counters describe only the legacy issue port (zero in shared mode);
actual shared-memory work/wait remains in `host_hbm_*` and the common HBM stats.

GC keeps its existing invalid-page, relocation-capacity and wear scoring.
Equal-score victims use a per-stack cyclic plane order, starting at an
independent GC cursor; block order breaks ties within a plane. The cursor
advances only when a GC relocation actually starts. Selection previews,
foreground allocation and static wear-leveling selection do not advance it.
This avoids repeatedly replenishing just the lowest-numbered plane under
uniform FIFO invalidation. The cursor is part of the existing persistent
image state. This is a host collection-policy change, so it can change
simulated throughput and wear, unlike an equivalent simulator execution
optimization. The native comparison is described in the
serving study.

## HBF device DRAM

`hbf-device-dram-capacity-denominator=N` (default 0, disabled) gives every HBF
stack its own on-device DRAM of `floor(raw flash pages per stack / N)` 4 KiB
pages, like the DRAM of a NAND SSD. It is not part of HBM and not part of the
`hbf-ctrl-dram-*` controller reservation. Two things share all of its pages,
with dirty pages taking precedence: a page is programmed once it is full,
clean read-cache lines use whatever the dirty pages leave and are dropped
first when a page needs room, and only a DRAM holding nothing but dirty pages
writes out its least recently written page early (`device_dram_dirty_evictions`
counts these; `device_dram_peak_dirty_pages` is the most pages one stack held
dirty at once). `hbf-write-buffer-pages` and
`hbf-write-buffer-flush-threshold-pages` size only an HBM-resident buffer and
must be 0 with device DRAM.

`hbf-mapping-placement=device` (default `host`) moves the mapping itself into
this DRAM: each HBF stack runs the configured mapping (page-level table or
cache, block, block-log or extent index) and keeps its records in its own
device DRAM, reserving `hbf-ctrl-dram-bytes / stacks` (or the
`hbf-ctrl-dram-capacity-denominator` share) of it in whole pages; the write
buffer and the read cache keep the rest, no HBM is reserved
(`host_hbm_reserved_bytes` is 0, `local_mapping_reserved_bytes` is the
device reservation), the host sends logical requests and no metadata crosses
HBIO. Mapping accesses use the device DRAM port (`hbf-device-dram-hit-latency-ns`,
`hbf-device-dram-bw`) instead of HBM. A device-local mapping runs without
automatic GC (`hbf-auto-gc-enabled=false`, default `true`; structural
organizations reclaim themselves and clear it on their own): the page FTL's
reclaim path is not modeled inside the device.

- The write buffer. With write coalescing, a staged write sends its command
  and payload over HBIO once and lands in device DRAM; it completes there
  unless `hbf-write-buffer-completion-requires-flush=true`. A page appended
  in parts (a KV row of half a page per token) is programmed once, when its
  last bytes arrive, with no read-modify-write. Buffered reads, overlays
  and partial-page merges use the device DRAM port. A flush is charged the
  same way in the page FTL and in the structural organizations: a partial
  page first brings its old image into DRAM (a read of a cached copy, or a
  NAND read decoded straight into DRAM; that superseded version is not kept
  as a cache line) and writes the dirty bytes over it, then every flush
  reads the assembled page from DRAM once and programs NAND without a second
  HBIO payload crossing. (The page FTL acknowledges on entry and merges at
  flush time, one extra copy of the dirty bytes; the structural policy merges
  on the page's chain before staging.) The HBM reservation therefore no
  longer contains a write buffer for any mapping organization (the structural
  budget is its index plus per-stack workspace).
- A clean read cache of physical NAND pages, used by the page FTL and the
  structural organizations alike. The device answers a logical or static
  page read from the closest copy: the NAND bank's decoded page buffer, then
  the device DRAM (request command, DRAM read of the requested bytes, HBIO
  egress), then the array. A page decoded from NAND is cached by a background
  fill that is admitted only when the stack's port and its fill engine are
  idle for the whole page write starting at the page's data-ready time and
  dirty pages do not fill the DRAM; otherwise the page is not cached and
  `device_dram_fill_bypasses` counts it. Fills never queue, are never charged
  to the request that read the page, and host-facing traffic (hits,
  write-buffer staging, merges and flush reads) never waits behind them. A
  flushed write-buffer page stays
  cached (write-allocate) from the moment its write-buffer slot is released,
  so a page never holds a slot and a cache line at once. Lines are purged
  when their page is invalidated (overwrite publication, TRIM, relocation) or
  its block is scheduled for erase. `hbf-device-dram-policy` selects `lru`
  or `fifo`. Physical-address probes bypass the cache.

Each stack has one DRAM port: an access costs `hbf-device-dram-hit-latency-ns`
(default 90) followed by bytes / `hbf-device-dram-bw` (GB/s, default 204.8) on
the port calendar. These defaults reuse the CXL-SSD device-DRAM envelope of
`configs/overlays/backing/cxl-ssd-cached.cfg`; they are not HBF measurements.
Counters `device_dram_read_hits`, `device_dram_read_hit_bytes` (bytes served
to reads; a buffered-overlay read counts only the bytes the write buffer did
not supply), `device_dram_read_misses`, `device_dram_fills`,
`device_dram_fill_bypasses`, `device_dram_evictions`, `device_dram_read_bytes`
and `device_dram_write_bytes` appear in the summary, in every session batch's
HBF delta and in the mapping snapshot. With the denominator at 0 every path is
the HBM write-buffer model above.

The class name `HbfController` does not place software on the HBF base die.
OCP section 4.6 assigns global-to-channel address translation to host software
and channel-local bank/die/block interpretation to the base die. It does not
specify the page-L2P cache implemented by this simulator.

Mapping control, metadata bookkeeping and codec work now use explicit compute
workers, separate from the pipelined metadata-memory issue ports. The default
host pool has one worker per HBF stack in total, shared across stacks;
`hbf-mapping-compute-workers` sets another total (0 keeps the default). Service
times are assumptions, not timings of the simulator's C++ code or calibrated
CPU instructions. `mapping_compute_work_ns` counts work; the queue counter
sums waits of internal operations and is not a foreground latency decomposition.
Equal-start worker choices prefer the most recently freed worker so a short
continuation does not unnecessarily fragment another worker's idle interval.

The research probe can add a bounded, tagged SRAM read cache over host
`FullResident` mapping. It keeps the complete HBM reservation: misses fetch an
HBM entry, never a NAND translation page. Each 32-byte SRAM record includes
the key, value and replacement metadata; capacity and port work are accounted
separately from HBM. Concurrent misses coalesce, and pending fills finish before
their slot is replaced. All mapping updates, including trim and GC, invalidate
the cached entry and still reach HBM. SRAM contains no dirty state and starts
cold after restart. The hierarchy comparison
documents its explicit service assumptions and the HBF-backed alternatives.

The native mapping-placement study
also provides a mapping-only `DeviceLocal` architectural control: the same
mapping policy and total memory budget use private per-stack memory, metadata
payload stays off HBIO, and compute workers are partitioned across stacks.
A controller-DRAM comparison
uses the native HBF/1000 capacity derivation and the same budget in host HBM.
The local path defaults to an assumed 51.2 GB/s aggregate DRAM interface with
64-byte transactions; its response/issue times use `ctrl_dram_*`. The study
uses 100 ns on both placements and scans local latency and bandwidth. These
are explicit channel-level assumptions, not a calibrated DDR bank/row model.
The old 5-ns private-memory study is retained only as historical sensitivity.
A separate host-affinity control isolates resource pooling. These controls are
configured by the native research probe; they are not general session flags or
an OCP device-side FTL implementation. Device-local mode rejects physical-address
requests, automatic GC and write coalescing. Both controls retain base-die/media
timing. Application compute contention, multi-agent cache coherence and software scheduling
instructions are outside this model.

A reclaimed block becomes reusable by the host. Physical erase occurs on its
next page-zero write, including the first write to an initially erased block.
Count physical erases and P/E cycles separately from GC runs and zone resets.
Full-page writes acknowledge after program completion; any earlier buffered
acknowledgement belongs to the write buffer (HBM controller storage, or HBF
device DRAM when it is enabled).

The raw channel frontend accepts aligned reads within a page and whole-page
programs in NAND order. Host zones are equal-sized and NAND-block aligned.
`INVALIDATE` discards the complete zone, `RESET` returns its write position,
and `REMAP` swaps two invalid zones in the same channel after I/O completion.
RESET never selects a destination or executes an erase. `recycle_zone()` is
an explicitly selected host policy: select the invalid zone with the lowest
PEC sum (logical-zone order breaks ties), issue REMAP if the configured average
PEC gap is met, then RESET. The device implements opcode 0x8 / type 0x00 with
BUCC.WLS=10b. It does not implement optional product-specific base-die WL.
No remap copies pages or moves physical wear. `zone_state()` reads the mapped
zone's physical PEC and host liveness at a completed I/O frontier.

The FTL frontend also supports logical page deallocation. Python callers use
`SimulationSession.invalidate_hbf_pages(id, first_lpn=..., page_count=...)`,
which emits `HBF_INVALIDATE id first_lpn page_count`. This explicit host barrier
finishes already-issued I/O, discards the range's unissued write-buffer data,
and invalidates its current materialized or compact L2P pages. It does not
flush other buffered data. Physical invalid pages remain allocated until the
ordinary GC path reclaims them; invalidation itself is not a media erase.

Each live L2P removal pays the configured mapping/controller HBM access and
update cost. Updates are serial within each stack and independent across
stacks. Cached mapping misses, dirty evictions and any resulting GC use their
normal physical paths. Dirty tombstones persist through ordinary mapping
eviction or checkpoint. Discarding a buffer descriptor has no additional
software latency parameter in this model. The receipt separately records
invalidated versus already-unmapped pages, discarded buffer pages/bytes,
completion time and the complete device traffic delta. It is outside the
read/write transaction census and advances subsequent batch origins. Repeating
a free is idempotent; read-only initial images and raw-physical mode reject it.

Persistent images use **v6**, with geometry, block ownership, host mapping,
physical P/E counts and the sparse zone permutation. Restore rejects older
formats and starts a new workload wear interval while retaining inherited wear.
Full-zone initial raw data are stored compactly per block, remain readable,
and consume capacity with no warmup writes or P/E. Invalidation discards this
initial data through the same zone lifecycle as subsequently written data.
Snapshots are simulator checkpoints, not a promise of unlimited physical data
retention.

The OCP zone comparison uses this channel
frontend. Its wear-only executor, `HbfZoneEndurance`, shares the device map/PEC
implementation and collapses each complete-zone rewrite into exactly one
autoerase and `pages_per_block` programs per block. Timed-versus-arithmetic
comparisons cover every physical P/E count, remap destination and write counter.
This executor only accepts whole-zone expiry/rewrite; it does not predict timing,
partial invalidation, live-copy GC, metadata persistence, refresh or failures.
The page FTL remains useful for arbitrary logical overwrites and mixed-live-page
GC studies. Its full-resident L2P and mapping-page cache results are separate
from Figure 46 evidence.

A checkpoint admits dirty mapping pages in bounded batches, each containing at
most `stacks × pages_per_block` translation pages. It publishes each completed
batch and finishes its active relocations before admitting the next batch.
This releases superseded mapping pages under capacity pressure; GC-generated
mapping changes are included in subsequent persistence work. The mapping
snapshot is refreshed after GC that makes room for its write. All resulting
mapping programs, relocation copies and physical autoerases remain charged.

`WEAR_SNAPSHOT` returns `hbfsim.hbf_wear_snapshot` v2: complete native workload
counters, valid/invalid/free and mapping state, pending/dirty state, and the
physical block erase-count vector including zeros. Observation materializes
only callbacks already completed at the session frontier. It issues no I/O,
does not advance time, and does not flush dirty data or mapping pages. This
allows continuous-window measurements without checkpoint-induced writeback.
Window counters use successive snapshot differences; delayed resource busy-time
summaries are authoritative at these boundaries rather than in lightweight
per-batch deltas.

The public C++ APIs are in `src/host/hbf_controller.hpp`; image I/O is in
`src/host/hbf_persistent_image.hpp`; program-order/channel/cache contracts are
in `src/physical/hbf/hbf_device.hpp`. The Python `SimulationSession` exposes
logical/static/physical access, logical invalidation, zone commands, checkpoints
and wear snapshots.
