# SF=1 bounded Join spill-chain validation

Date: 2026-10-10

## Scope

The previous two-sided streaming Join removed complete build and probe Scan
`Batch` objects, but retained one in-memory `SpillHandle` for every nonempty
batch/partition selection. This milestone replaces those vectors with a
backwards-linked chain stored in query-local Flash. Every data record is
followed by one packed 40-byte chain record containing its handle and the
previous chain-record offset. Controller memory retains only a 16-byte
`{tail_offset, count}` descriptor for each radix partition.

The chain is traversed newest-to-oldest. Build insertion order is not visible,
and probe records already carry a global ordinal before replay, so the final
stable ordinal sort preserves SQL result order. Traversal is bounded by the
stored count and validates the magic, node id, data range, column count and
terminal offset. Adaptive build repartitioning writes replacement chains using
the same representation.

The final versioned kernel is:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_spill_chain_v1.so`

## Directed validation

The low-memory unit case starts with four partitions, then splits colliding
build keys into eight partitions. It produces seven chain records and reads
all seven during repartition/replay. Sixteen build/probe chain heads consume
256 bytes, independent of the number of records in those chains. The LEFT
join result remains exact and in original probe order. Optimized and
ASan/UBSan suites pass.

## CEMU Q3 at a 32 MiB operator arena

Forced-CSD Q3 used cache fill off, profiling on and the final chain kernel. Its
result is byte-for-byte identical to HOST.

| Counter | Streaming build v2 | Flash chain v1 |
| --- | ---: | ---: |
| Operator capacity | 33,554,432 B | 33,554,432 B |
| Operator peak | 8,832,780 B | 8,832,780 B |
| Build FIFO batches / rows | 7 / 737,165 | 7 / 737,165 |
| Probe FIFO batches / rows | 30 / 3,231,383 | 30 / 3,231,383 |
| Data spill records | 256 | 256 |
| Flash chain records / reads | n/a | 256 / 256 |
| Resident chain descriptor peak | linear handle vectors | 1,024 B |
| Spill write/read | 60,180,376 B / 60,180,376 B | 60,190,616 B / 60,190,616 B |
| Base relation Flash read | 1,519,804,416 B | 1,519,804,416 B |
| Guest-visible query time | 1,939.764 ms | 2,020.243 ms |

The exact 10,240-byte increase in each spill direction is `256 * 40` bytes of
chain metadata. Subtracting replay bytes from total Flash reads leaves the
same base-relation read volume. The timing values are independent TCG samples
and are not used as a performance claim; the result here is the removal of
record-count-dependent resident handle storage.

## Regression gates

- final chain kernel forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT`;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 parameter relocations and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  output-capacity/SubPlan/selectivity guards, Q2=HOST and Q16=HOST;
- optimized and ASan/UBSan unit suites: pass.

Raw query outputs, the Q3 profile, planner output and AUTO plans are archived
in `tpch_sf1_spill_chain_validation_raw.tar.gz` with SHA-256
`75646d7446fd7712a724a8e56121629767bd4077f2006a257896b4405f92c271`.

## Remaining boundary

Historical note: the subsequent ordinal-merge milestone removes the complete
`OrderedJoinRow` vector and keeps only logarithmically many Flash run handles.
See `tpch_sf1_ordinal_merge_validation.md`. The final-output and allocator
limitations below remain current.

The resident descriptor payload is now proportional to radix partition count,
not spill-record count. It is still stored in ordinary `std::vector` objects,
and the metric excludes allocator overhead. Partition count is explicitly
bounded at 65,536. Adaptive repartitioning appends replacement data and chain
records without reclaiming obsolete query-local scratch space. A restored
build partition, the complete Join output and its ordinal reorder vector still
use ordinary C++ containers. Join inputs that are not eligible sole
Scan/Filter/Project chains retain the complete-input path. Therefore this
milestone bounds streamed Join handle metadata but does not yet establish a
strict SF=32 end-to-end physical-memory bound.
