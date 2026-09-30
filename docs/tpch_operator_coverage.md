# TPC-H operator coverage from PostgreSQL 12 plans

The plans were collected from all 22 files in `scripts/gen_data/queries/*.explain.sql`
against `tpch_s1`.  Raw plans are saved in `results/explain_sf1/`.  This is the
required physical-operator coverage for the CSD plan executor; implementing
only filter/project/aggregate/inner hash join/sort is not sufficient.

| PostgreSQL plan node seen | CSD plan representation |
| --- | --- |
| Seq Scan, Parallel Seq Scan | `SCAN` with a vector predicate and projection pipeline |
| Index Scan, Index Only Scan, Bitmap Index/Heap Scan | `INDEX_SCAN` candidate-page source; initially a canonical full `SCAN+FILTER` fallback is required for semantic coverage |
| Hash / Hash Join / Parallel Hash Join | `HASH_JOIN` with build/probe phases and join-kind field |
| Merge Join | `MERGE_JOIN`; planner may canonicalize it to partitioned `HASH_JOIN` when no input ordering must be retained |
| Nested Loop, Left, Semi, Anti | `NL_JOIN` and `HASH_JOIN` join-kind values `INNER`, `LEFT`, `SEMI`, `ANTI` |
| Aggregate, Partial/Finalize Aggregate | local aggregate plus a final reduction stage |
| HashAggregate, Partial/Finalize HashAggregate | partitioned `HASH_AGG` plus final merge; spill partitions to Flash |
| GroupAggregate, Partial/Finalize GroupAggregate | `SORT` or ordered-stream input followed by grouped aggregate |
| Sort | external run generation and k-way merge; `TOP_N` is `SORT+LIMIT` |
| Materialize | CSD batch/Flash materialization barrier |
| Limit | `LIMIT` with early cancellation propagated upstream |
| InitPlan / SubPlan / hashed SubPlan | subplan DAG; scalar result, set-membership, `EXISTS`, `IN`, and `NOT IN` inputs to an expression or join operator |
| Gather / Gather Merge | not sent literally: the CSD scheduler partitions work over CUs and performs a final merge/reduce |

## ABI v7 engine status

The CSD engine has a semantic representation and executable implementation for
every operator and expression family used by the 22 queries. PostgreSQL emits real scan,
projection, join, aggregate, sort, limit, and expression descriptors for all
22 forced-CSD EXPLAIN cases. Q2's correlated scalar aggregate and Q16's
null-safe `NOT IN` are rewritten into aggregate/join DAG branches, leaving zero
pending expressions. On SF=1, all 22 SQL files execute end to end through CEMU
Program Execute and match the guest PostgreSQL HOST results exactly. The
comparison is automated by `tests/test_csd_tpch_execution.sh`.

| TPC-H requirement | ABI v7 lowering |
| --- | --- |
| Decimal arithmetic and ratios | fixed-point expression `ADD/SUBTRACT/MULTIPLY/DIVIDE` with scale |
| `CASE`, `OR`, `NOT`, `BETWEEN`, `IN` | postfix boolean program and `SELECT` mux; `BETWEEN`/`IN` expand to comparisons |
| Prefix/suffix/contains `LIKE` | bounded `LIKE` instruction (`%` and `_`) |
| Date ranges and `extract(year)` | planner-folded interval constants and `EXTRACT_YEAR` |
| `substring(phone,1,2)` | `SUBSTRING` with constant start/length |
| Multi-column grouping/order/join | descriptor arrays rather than one-key records |
| `count(distinct ...)` | per-aggregate `DISTINCT` flag |
| `EXISTS`/`NOT EXISTS` | hash `SEMI`/`ANTI` join with optional residual |
| `IN`/`NOT IN` subquery | semi/anti join when PostgreSQL proves null-safe; otherwise explicit software fallback |
| Scalar aggregate subquery | aggregate DAG branch followed by keyed or one-row join |
| Index/bitmap scan | counted canonicalization to heap `SCAN+FILTER` until a native index-page decoder exists |
| Merge join | counted canonicalization to multi-key hash join |
| Non-equality/correlated nested loop | functional `NL_JOIN`, explicitly counted as software fallback |

The unit suite exercises all join kinds, residual predicates, scan/index
canonicalization, merge canonicalization, subplan barriers, multi-key
aggregate/sort, all five aggregates, `DISTINCT`, top-N/limit, fixed-point
arithmetic, `CASE`, `LIKE`, substring, boolean logic and date extraction.
Unsupported PostgreSQL functions, collations, external/TOASTed values and
unbounded numeric precision still reject CSD lowering explicitly.

## Per-query lowering shape

| Queries | Dominant CSD shape |
| --- | --- |
| Q1 | scan/filter -> arithmetic project -> two-key aggregate -> two-key sort |
| Q2 | five-way equi-join plus suffix `LIKE`; correlated `MIN` branch becomes grouped aggregate + join; top-N sort |
| Q3, Q5, Q10 | filtered equi-join pipeline -> arithmetic project -> multi-key aggregate -> top-N |
| Q4 | orders range filter + lineitem residual semi-join -> group aggregate |
| Q6 | scan + boolean range filter -> multiply -> global sum |
| Q7-Q9 | multiway joins -> year/arithmetic/`CASE` project -> multi-key aggregate/sort |
| Q11 | aggregate branch joined with a scalar global aggregate used by HAVING |
| Q12 | join/filter -> two `CASE` mux outputs -> group aggregate |
| Q13 | left join with `NOT LIKE` residual -> first group aggregate -> second group aggregate |
| Q14 | join -> `CASE` arithmetic -> two sums -> ratio project |
| Q15 | reusable aggregate branch (view) -> scalar max branch -> supplier join |
| Q16 | filters + null-safe anti-join -> multi-key `COUNT DISTINCT` aggregate |
| Q17 | per-part average branch -> keyed join/filter -> global sum/divide |
| Q18 | grouped lineitem/HAVING branch -> semi-join -> final multi-key aggregate/top-N |
| Q19 | three OR branches in one boolean expression -> join -> global sum |
| Q20 | nested aggregate and prefix-filter branches -> keyed joins/semi-join |
| Q21 | two lineitem branches become residual semi/anti hash joins -> aggregate/top-N |
| Q22 | substring filter + scalar average branch + orders anti-join -> group aggregate |

The 22 plans contain 28 `Sort` nodes, 29 nested-loop variants, hash/parallel
hash joins, merge joins, semi/anti/left joins, `Materialize`, `Limit`, and
`InitPlan`/`SubPlan` nodes.  They also use expression features that belong in
the plan expression IR: arithmetic over decimal values, `CASE`, date/interval
comparison, `LIKE`, `IN`, `EXISTS`, scalar subqueries, boolean `OR`/`NOT`, and
aggregates `SUM`, `AVG`, `COUNT`, `MIN`, and `MAX`.

`Gather` is a host parallel-execution wrapper, not an SQL semantic operator;
a CSD path must replace it with controller scheduling rather than emulate its
host worker protocol.  Similarly, index-plan nodes must not be silently
dropped: the first correct implementation may request relation page ranges
and evaluate the index condition as a filter, but the plan status must record
that fallback so cost calibration can distinguish it from a true CSD index
scan.
