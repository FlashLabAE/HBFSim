# Peer HBM/HBF residency and background writeback

> Status: Current
> Last reviewed: 2026-10-11

The native `host::PeerMemoryController` owns Peer placement, access recency,
capacity pressure, and background migration. Workloads declare their data
range, immutable-weight boundary, initialized byte ranges, application HBM
budget, and placement priority. They submit logical `PEER` reads/writes and
last-reference releases. They do not choose victims or announce idle compute
windows. E3's two HBF Peer variants use this path.

Weights-first fills HBM with weights and then produced KV. KV-first fills it
with produced KV and uses all remaining space for weights. New KV reclaims
opportunistic weight homes in KV-first. An unused logical KV arena does not
reserve physical slots. Both policies keep exclusive homes; transient copies
reserve their destination and pin their source until completion. HBF data can
move into free HBM when accessed. With weights-first and no HBM space left
after weights, new KV goes directly to HBF.

The HBF system configuration controls background behavior:

| Key | Default | Meaning |
| --- | ---: | --- |
| `hbf-peer-writeback` | `true` | Enable idle-time background migration for configured Peer memory |
| `hbf-peer-high-watermark` | `0.90` | Start draining when resident KV reaches this fraction of its HBM budget |
| `hbf-peer-low-watermark` | `0.80` | Stop draining once occupancy falls to this fraction |
| `hbf-peer-cold-ns` | `1000000` | Minimum time since the last access before a KV unit is eligible |

These defaults are simulation policy choices, not HBF hardware measurements.
Weights-first measures pressure against the capacity left by its resident
weights. KV-first measures it against all application HBM; lower-priority
weights are reclaimed before KV capacity migration becomes necessary.
Recently accessed units and units with outstanding consumers are protected
from background selection. Mandatory allocation under full capacity can wait
for a migration even when no cold victim or idle opportunity is available.
Disabling background migration retains this capacity fallback.

The session gives ready foreground demand priority, admits background work
only after the native HBF controller's current work has finished, and checks
HBF occupancy again before each background destination write. This first
implementation uses whole-HBF idleness, including native background programs,
rather than trying to exploit a free plane inside an otherwise busy device.
An already-started NAND operation is not preempted when new demand arrives;
its actual contention remains in the native resource model.

Copies charge HBM access, the actual per-stack D2D links, HBF writes, and FTL
work. Background IO can remain in flight across batch boundaries without
an unconditional foreground wait; physical contention can still delay demand.
A later read, write, slot reuse, or
release waits for its own outstanding ownership transition. Range retirement
waits for prior publications of those LPNs and preserves unrelated work.
Dead HBM data can be discarded; live data is never discarded to make room.
Closing a session completes already-started transfers and the ordinary HBF
drain; it does not force all surviving HBM data to flash.

`peer_memory` in native batch and final receipts reports residency, reserved slots,
live/released bytes, demand routing by role, background/forced migration,
promotions, and the last background victim and start time. Native device
receipts retain the physical IO and contention. Initial logical ranges and
policy configuration are recorded in the Python session's source receipt.

The generic protocol has `PEER_CONFIG` (layout and initialized ranges),
`PEER_RELEASE` (page-aligned logical ranges), and the `PEER` transaction
target. `SimulationSession.configure_peer_memory()` and
`release_peer_memory()` expose these operations without a serving frontend.
The module uses the ordinary demand frontier; background dependencies are
owned internally. A caller-provided detached frontier is therefore rejected
for configured Peer sessions. HBF image checkpoints contain the flash tier;
they do not persist or restore the Peer layout and HBM-resident data.

Tiered is a separate inclusive-cache experiment. Its explicit compute-window
writeback is unchanged. External CPU/NVMe/CXL comparisons keep their fixed
homes and staging-ring implementation because those devices do not contain
this HBF Peer module. Explicit layer streaming also retains its declared
buffers. Historical fixed-Peer results describe their frozen implementation
and must not be relabeled as runs of this controller.

Focused native coverage: `tests/python/test_peer_memory.py`, plus the E3
serving/lifecycle integration in `tests/python/test_paper_e3_native.py`.
