# SF=1 Hash Join Flash-replay validation

Date: 2026-10-02 (Asia/Shanghai)

## Scope

This milestone replaces the oversized Hash Join protection-copy path with a
functional Grace/radix replay path. The join chooses a power-of-two partition
count, partitions both build and probe rows by the canonical join key, writes
the pairs into the query-local NVMe spill file, and reads one pair at a time
through the bounded FDM staging window. Each restored build partition must
obtain an operator-arena lease before its hash table is created.

A hidden probe ordinal restores the original probe order after replay and is
removed before residual-expression evaluation and result emission. NULL keys
are handled directly for LEFT and ANTI joins and discarded for INNER and SEMI
joins, matching SQL equality semantics.

## Forced replay case

The operator arena was capped at 1 MiB and this query was forced through CSD:

```sql
SELECT count(*)
FROM lineitem l
JOIN orders o ON l.l_orderkey = o.o_orderkey
WHERE o.o_orderkey < 300000;
```

The CEMU status record reported:

| Metric | Bytes / count |
| --- | ---: |
| Base-table Flash reads | 1,490,436,096 |
| Join partition writes | 241,548,724 |
| Join partition reads | 241,548,724 |
| Total Flash reads | 1,731,984,820 |
| Operator arena | 1,048,576 |
| Peak replay build lease | 750,240 |
| Successful operator leases | 3 |
| Spill records | 4 |

The returned count was `299808`, identical to HOST. The total Flash-read count
is exactly the two base relation sizes plus the replayed partition bytes.

## Regression coverage

- The final shared library was compiled inside the TCG CEMU guest and its
  guest kernel suite passed.
- The standalone low-memory pipeline forces Join partition replay and requires
  successful replay-time arena allocations plus real spill reads.
- The residual/canonicalization test now forces merge/hash Join replay at a
  64-byte budget while also exercising LEFT, SEMI, ANTI and nested-loop
  compatibility behavior.
- With the normal 4-GiB FDM configuration, all Q1 through Q22 again reported
  `EXACT` against HOST.
- AddressSanitizer and UndefinedBehaviorSanitizer passed; LeakSanitizer itself
  is unavailable in the ptrace-based test environment.

## Remaining boundary

The original decoded inputs and the first-generation radix partition buffers
are still C++ `Batch` objects, so partition creation temporarily duplicates
rows before their handles replace those buffers. A streaming partition writer
is required for a physical hard bound at SF=32. Aggregate partition replay and
arena-backed operator data structures also remain future work. Extreme skew
that cannot produce an arena-sized build partition is rejected explicitly.
