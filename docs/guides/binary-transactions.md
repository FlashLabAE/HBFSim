# Binary transaction encoding v1

> Status: Experimental
> Last reviewed: 2026-10-17

The simulation session ready receipt advertises `binary_transactions_v1: true`.
This optional local-file encoding is additive to the existing text protocol.
Both decode into `SimulationTransaction` and call the same
`SimulationSession::run_batch`, with the same graph checks and result writers.
No HBM/HBF/OCP timing model, queue setting or request size changes are included.

## Session envelope

Use the normal BEGIN fields and frontier/retain/completions options, replacing
`BEGIN` with `BEGIN_BINARY`. The body contains exactly one line:

```text
FILE <hex-encoded-absolute-path> <byte-offset> <byte-length>
END <batch-id>
```

The transaction SHA256 covers exactly the specified raw byte slice, rather than
text transaction lines. Logical SHA256 remains the source input identity.
The file must be a regular non-symlink file accessible to the simulator. Bounds,
checksum and decoding failures return the normal batch error; no batch execution
occurs and the session can accept a subsequent valid batch. Envelope framing
errors retain the existing protocol's terminal-error behavior.

## Byte layout

All integers and IEEE754 doubles are little-endian. Start with the eight bytes
`HBFTX01\n`, followed by a u64 record count. Each record has this fixed header:

| Field | Type |
| --- | --- |
| ID byte length | u16 |
| Target code / operation code | u8 / u8 |
| Stack | u32 |
| Address / bytes | u64 / u64 |
| Issue time / duration in ns | f64 / f64 |
| Dependency count / reserved zero | u32 / u32 |

Append the ID bytes, then each dependency as u16 byte length and ID bytes.
IDs use the same safe ASCII alphabet as text, with lengths 1–4096. The target
codes 0–10 are HBM, HBF_LOGICAL, HBF_STATIC, HBF_PHYSICAL, D2D_HBF_TO_HBM,
D2D_HBM_TO_HBF, EXTERNAL, HOST_DRAM, DIRECT_HBF_TO_EXTERNAL,
DIRECT_EXTERNAL_TO_HBF, BARRIER. Operations are read=0, write=1, barrier=2.
Finite nonnegative times, reserved fields, lengths, truncation and trailing bytes
are checked before execution. Existing native executor rules enforce target,
barrier, address, stack, dependency and enabled-device semantics.

Each byte slice is 16 bytes to 512 MiB, with at most 2,000,000 records. Dependency
counts must fit the remaining payload before allocation. Native decoding still
materializes the ordinary transaction vector/DAG; this is not compressed or
analytically approximated simulation.

The initial ServeLoop sender (`hbfsim_client.binary_transactions`) qualifies
fixed HBM/HBF requests and barriers, without completion export. Other target
codes in the format are reserved for future client qualification; their presence
in the decoder does not establish workload or device validation. The stock
HBFSim Python client's text API is unchanged.

## Compact serial-phase encoding

A backend advertising `binary_serial_phase_v1: true` also accepts `HBFPH01\n`
inside the same BEGIN_BINARY/FILE/END envelope. This is a lossless shorthand for
an explicitly defined scheduling contract, not workload extrapolation or request
coalescing. The ordinary text and `HBFTX01` formats remain supported.

The header is little-endian `<8sQQQ>`: magic, memory-request count, phase index,
first global request index. It is followed by exactly count `<BBHQQ>` records:
target code (0–3), read/write code (0/1), reserved zero, address and byte size.
Count is at most1,999,999; with the generated barrier this retains the existing
2,000,000-transaction cap. There are no implicit extra phase boundaries.

The native decoder reconstructs, in record order:

- Request ID `replay/<phase>/op/request/<first+i>`, zero issue/duration, stack0.
- For phase>0, each request depends on `replay/<phase-1>/complete`.
- One `replay/<phase>/complete` barrier after the memory records, depending on
  every request followed by the previous completion if present. Empty phases
  retain that previous-completion dependency.

The reconstructed vector is passed to the unchanged `run_batch`; all target,
capacity, dependency, retention and accounting checks still apply. There is no
optimized alternative scheduler. IDs/edges are omitted from the file only
because this dialect defines their exact values. Arbitrary timing, local edges,
other target codes and additional barriers require explicit transactions.

Native software tests compare the exported per-transaction IDs/completion digest
against text, as well as normal batch and terminal receipts. Public ServeLoop
phase sending still disables per-transaction completion export.

## Serial request stream node (opt-in)

`serial_request_stream_v1: true` advertises `BEGIN_STREAM`, with the same BEGIN
header fields and exactly one body line:

```text
FILE <hex-encoded-absolute-path> <byte-offset> <byte-length> <window-records>
END <batch-id>
```

The file uses the existing `HBFPH01` header and 20-byte records, restricted to
HBM (0) and HBF_LOGICAL (1), zero issue/duration and record order. The batch ID
must equal the decimal phase index. Frontier and retain must both contain only
`replay/<phase>/complete`; observations and per-request completion export are
unsupported. For phase>0 the preceding phase's completion must equal the current
completed frontier. Only this macro completion becomes a cross-batch DAG ID;
internal request IDs cannot be retained or referenced by later transactions.
A following ordinary DAG can depend on the macro completion. This initial
contract runs serial kernels; it does not schedule concurrent arbitrary stream
nodes in an upper DAG.

Native copies the specified slice into a private temporary disk snapshot and
checks its SHA256. It preflights every record and address before modifying any
device, rewinds the immutable snapshot, then executes with a bounded read buffer.
The C++ callback API requires the same immutable, rewindable-source contract.
A live generator is not supported. Snapshot creation requires writable temporary
disk space and adds I/O, but avoids loading the entire kernel into RAM and avoids
executing a source that changes after validation.

Window size is 1–65,536 records (default sender: 1,024). Buffer boundaries never
advance modeled time, drain devices or insert barriers. Requests use shared
session device issue/accounting helpers; the final macro waits for every HBM/HBF
request. HBM/HBF/OCP device models are unchanged. Receipts keep the equivalent
serial DAG's transaction/edge census even though those graph edges are not
materialized. Input is limited to 25,000,000 memory records and 512 MiB per slice.
The ordinary DAG's 2,000,000-transaction limit is unchanged.

This removes full-kernel transaction/vector/DAG/history storage for the stream
path. Device in-flight state remains; HBF resource/calendar/history memory is not
certified constant by all-HBM tests. Mixed HBM/HBF software controls establish
receipt equivalence, not hardware timing accuracy. Workspace evidence is in
`experiments/kernel_stream_node_20261007_01/RESULT.md`.

## Live kernel transport (opt-in)

Ready capability `live_request_stream_v1: true` enables:

```text
BEGIN_LIVE <decimal-phase> <first-global-request-index> <logical-plan-sha256>
CHUNK <record-count>
<exactly record-count * 20 raw bytes, followed by one newline>
...
END_LIVE <total-record-count> <sha256-of-concatenated-raw-records>
```

Each chunk holds 1–65,536 existing `<BBHQQ>` HBM/HBF_LOGICAL records; one
kernel holds at most25M records. Targets, bounds and zero-time serial semantics
match the immutable stream path. Chunk boundaries never advance simulated time
or add barriers. The shared `run_live_request_stream` execution loop handles
both live and prevalidated immutable input. Only the kernel completion is exposed
as a DAG dependency, with the same implicit serial request/edge census.

The logical SHA identifies the validated plan (the final source-stream SHA is
not known yet). Transaction SHA covers20B records only, without the immutable
phase header. The receipt is emitted only after validating the END count/hash
and finishing all requests. Source post-cache identity is separately verified
by the frontend when the producer has successfully completed. Scientific
receipt fields are unchanged; these two identity fields differ by design.

A live producer can fail after device mutation. Any malformed/truncated record,
late count/hash mismatch or producer failure discards the entire live session;
there is no rollback or recoverable batch-error promise for this dialect. A
frontend must not publish a successful final result until producer and consumer
both complete and the full source digest/census agree. Prior per-kernel receipts
in a partial log are not successful whole-run evidence. Existing text/binary
file inputs retain their prior validation/error contracts.
