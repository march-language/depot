# Depot Feature-Gap Plan

A three-phase plan for closing the gap between depot and mature functional-language DB libraries (Ecto, Diesel, Persistent, Slick).

## Working agreement

Every item below is delivered TDD:

1. **Red** — write a failing test (`describe`/`test` block under `test/`) that pins down the new behavior.
2. **Green** — minimum implementation to pass.
3. **Refactor** — prefer `match` + nested `pfn` over nested `if` (depot house style).
4. **Lint** — `forge lint --strict` clean before the item is "done". Don't let warnings pile up between items.
5. **Typecheck** — `forge check` clean.
6. **Format** — `forge format --check` clean.
7. **Commit** — one concern per commit; message explains *why*, not *what*.

### Property tests

Depot is a functional library with a lot of algebraic structure (queries compose, SQL generation has invariants, changesets merge). Use property tests — not just examples — anywhere there's a general law.

March ships a Hedgehog-style property framework in stdlib:

- `Check.all(generator, fn value -> Bool)` — main runner, 100 runs by default, integrated shrinking on failure.
- `Check.all_with(config, generator, prop)` — override `num_runs`, `seed`, `max_shrink_steps`, `max_size`.
- `Gen.int(lo, hi)`, `Gen.list(g)`, `Gen.string(...)`, `Gen.tuple2/3`, `Gen.oneof(...)`, `Gen.bind`, `Gen.map` — generator combinators.
- Failing properties are shrunk automatically and panic with the minimal counterexample + seed for replay.
- `MARCH_PROP_SEED=N forge test` replays a failure.

Reference: `stdlib/check.march`, `stdlib/gen.march`, `test/stdlib/test_properties.march`.

Property-test conventions for this plan:

- **State laws, not examples.** "reverse is involutive" beats "reverse of [1,2,3] is [3,2,1]".
- **Shrinking-friendly generators.** Prefer `Gen.int(0, 100)` over `Gen.int(-max, max)` for values where magnitude doesn't matter — smaller counterexamples read better.
- **One property per `test` block.** Shrinking reports one failing seed; multiple properties in one block make it ambiguous which law broke.
- **Seed every CI run.** Save the seed on failure — a flaky property usually means a hidden non-determinism bug, not a flaky test.

### Cross-cutting infrastructure (do before Phase 1)

- **Real-Postgres test fixture.** A helper that returns a pooled, wiped connection per test. Before transactions land (Phase 1.1), `TRUNCATE ... RESTART IDENTITY CASCADE` between tests. After 1.1 lands, rewrap as a transactional sandbox.
- **SQL snapshot assertion.** `assert_sql(query, expected_sql, expected_params)` so query-builder tests read as diffs, not opaque string compares.
- **Schema/row generators.** `Gen.schema()`, `Gen.row(schema)`, `Gen.query(schema)` — reusable across every query-layer property test. Budget a day for these; they pay back across phases 1–3.

---

## Phase 1 — Expressiveness & safety foundations

**Goal.** Make the query builder powerful enough to model real queries, and give users real transactions. These primitives are escape hatches and safety rails that everything downstream composes against.

### 1.1 PostgreSQL transactions — `Depot.Repo.transaction/2`

- **TDD.** Integration tests against a real Postgres. Cases: successful commit; explicit rollback via `Err(...)` return; exception-path rollback (`__try_call`); nested transactions (savepoints); pool interaction (connection must be pinned for the transaction's lifetime).
- **Property.** For any sequence of writes `ws` inside a transaction that returns `Err`, the post-rollback row set equals the pre-transaction row set.

  ```march
  Check.all(Gen.list(Gen.write_op()), fn ops ->
    let before = snapshot_tables()
    let _ = Repo.transaction(fn _ -> apply_ops(ops) ; Err("rollback") end)
    snapshot_tables() == before
  )
  ```
- **Implementation.** Extend `Pool` with `with_conn_pinned`; route `BEGIN`/`COMMIT`/`ROLLBACK` through `Connection.simple_query`; savepoints as `SAVEPOINT sp_<depth>` for nesting.

### 1.2 `where_in` / `where_not_in`

- **TDD.** Empty list → `WHERE FALSE` (match the convention already used in `Preload.build_preload_query`); single value; many values; mixed with other conditions.
- **Property.** For any row list `rows` and subset `s`,
  `execute(where_in(q, field, values_of(s)), rows)` returns exactly `s` (modulo order).
- **Property.** `where_in(q, field, [v])` ≡ `where_eq(q, field, v)` (result set equality on arbitrary rows).
- **Implementation.** Expose the existing `In` AST node already used internally by `Preload`.

### 1.3 OR conditions / condition grouping

- **TDD.** `where_or([c1, c2])`; mixing AND and OR (`(a AND b) OR c`); de Morgan's law parity between `where_or` and negated `where_and`.
- **Property.** Commutativity: `where_or([a, b])` ≡ `where_or([b, a])` on arbitrary rows.
- **Property.** Identity: `where_or([c])` ≡ `where_(c)`.
- **Property.** Empty: `where_or([])` matches no rows.
- **Property.** Distributivity: `where_(a) |> where_or([b, c])` ≡ `where_or([where_(a, b), where_(a, c)])`.
- **Implementation.** Refactor `sql_conds` from a flat list of triples to a condition tree (`And(cs) | Or(cs) | Leaf(field, op, value) | Raw(s) | In(expr, exprs)`). **This is the biggest structural change in Phase 1 — do it first so 1.4/1.5/1.6 compose against the new tree.**

### 1.4 Raw SQL fragments — `Depot.Query.fragment/2`

- **TDD.** `fragment("lower(?) = ?", [Col("email"), Lit("a@b.com")])` generates `lower(email) = $1` with params `["a@b.com"]`; placeholder count mismatch surfaces as a build-time error, not a runtime one; mixing fragments with typed conditions.
- **Property.** For any fragment, `count("?", template) == List.length(args)` always holds — enforce in the constructor, property-test it by generating random templates and argument lists.
- **Property.** Placeholder rewriting preserves parameter order: generate a fragment, serialize, check that the Nth `$N` corresponds to the Nth argument.
- **Implementation.** Expose the existing `Raw` AST node. Fragments are the pressure valve; every gap below them has a raw-SQL workaround.

### 1.5 Subqueries — `where_in_subquery`, `where_exists`, `where_not_exists`

- **TDD.** `from("users") |> where_in_subquery("id", from("admins") |> select_col("user_id"))` → `WHERE id IN (SELECT user_id FROM admins)`; correlated subqueries via `EXISTS`; parameter numbering must be contiguous across nested queries.
- **Property.** Param indices are **dense** (no gaps) and **unique** across arbitrary nesting depth. Generate `Gen.query()` with random subquery trees, assert that the param list length equals the max `$N` in the emitted SQL. **This is the highest-value property test in Phase 1 — a param-numbering bug silently binds wrong values.**
- **Property.** `where_in_subquery(q, f, from(t) |> select_col(c))` with a static child table is equivalent to `where_in(q, f, load_column(t, c))`.
- **Implementation.** The SQL builder must thread a shared param counter through nested `Query → SQL` conversions. Current builder starts at `$1` per query — that has to change.

### 1.6 `RETURNING` clause

- **TDD.** `insert` returns the real DB row including server-side defaults (sequences, triggers, `now()`); `update` returns the post-update row; `delete` returns the deleted row; bulk variants return rows in insertion order.
- **Property.** `insert(gate) ; get_by(pk)` returns the same row as `insert_returning(gate)` — the `RETURNING` path and the re-fetch path agree.
- **Implementation.** Extend `Connection.exec_prepared` to return rows for `INSERT`/`UPDATE`/`DELETE`. Prerequisite for 2.1 and 2.2.

### 1.7 Query logging / telemetry

- **TDD.** A registered callback receives `(sql, params, duration_ms, row_count, result)` for every query; multiple listeners compose; a listener raising doesn't break the query.
- **Implementation.** Single global registry (Vault table or actor). Keep the API small — resist turning it into a plugin framework.

**Phase 1 exit criteria.**

- All existing tests still pass.
- New features have unit + property coverage (except 1.1 and 1.7, which are unit-only).
- `forge lint --strict`, `forge check`, `forge format --check` all clean.
- A developer can write `(a OR b) AND c IN (SELECT ...)` inside `Repo.transaction` and have it commit atomically with `RETURNING` rows logged.

---

## Phase 2 — Writes at scale & real schema management

**Goal.** Make depot usable for the write-heavy half of an app, and make migrations actually migrate.

### 2.1 Upsert — `Repo.insert(gate, on_conflict: ...)`

- **TDD.** Conflict on PK replaces; conflict on a unique index with partial update; `on_conflict = :nothing`; `RETURNING` distinguishes inserted vs. updated rows.
- **Property.** Idempotence: for any gate, `insert(g, on_conflict=:replace)` twice ≡ `insert(g, on_conflict=:replace)` once (table state equality).

### 2.2 Bulk insert — `Repo.insert_all(schema, records)`

- **TDD.** Empty list is a no-op (no SQL issued); 1000-row insert is a single statement; `RETURNING` returns rows in insertion order.
- **Property.** `insert_all(xs) ; all()` set-equals `map(insert, xs) ; all()`.
- **Property.** Order preservation: `insert_all(xs)` with `RETURNING` returns rows where the Nth result corresponds to `xs[N]`.

### 2.3 Query-based update / delete — `Repo.update_all`, `Repo.delete_all`

- **TDD.** `from("users") |> where_lt("age", 13) |> delete_all` deletes only matching rows; returns affected count; combines with `RETURNING` for captured rows.
- **Property.** `update_all(q, ch)` affected count equals `count(q)` measured pre-update.
- **Property.** Non-matching rows are untouched: for any query `q` and change set `ch`, rows in `all() - execute(q, all())` are byte-identical before and after `update_all(q, ch)`.

### 2.4 Migration execution

- **TDD.** `Migration.run(conn, migration_mod)` executes the generated SQL; idempotent when schema already matches (via `CREATE TABLE IF NOT EXISTS` / a version check); failed migrations roll back (uses 1.1).
- **Implementation.** Wrap the SQL generators in `Depot.Migration` with real `Connection.simple_query` calls wrapped in a transaction.

### 2.5 Migration versioning

- **TDD.** A `schema_migrations` table records applied versions; `migrate(conn)` runs only pending migrations in order; `rollback(conn, n)` runs `down` for the last `n`; divergence detected (version in DB not in code).
- **Property.** Round-trip: `migrate ; rollback_all ; migrate` reaches the same schema state as a single `migrate` (compare `pg_catalog` snapshots).

### 2.6 Joins — `join`, `left_join`, `right_join`, `inner_join`

- **TDD.** Two-table join returns merged rows; `on` clause uses the condition tree from 1.3; nullable columns from `LEFT JOIN` decode to `None`; chains of three+ joins.
- **Property.** `count(inner_join(a, b, on))` ≤ `count(a) * count(b)`.
- **Property.** `count(left_join(a, b, on))` ≥ `count(a)`.
- **Property.** `inner_join(a, b, on)` row set ⊆ `left_join(a, b, on)` row set.

### 2.7 Aggregates & GROUP BY

- **TDD.** `group_by`, `having`, `sum`, `avg`, `min`, `max`, `count(col)`; interaction with `order_by` / `limit`.
- **Property.** Sum-of-sums: `group_by(q, field) |> sum(other)` equals summing `sum(other)` over each group bucket computed in March.
- **Property.** `count(*)` of a grouped query equals the number of distinct group-key values.

### 2.8 Preload ergonomics — `Repo.preload(rows, assoc_name)`

- **TDD.** Replaces the manual grouping currently required by `Preload.preload/5`; reads association metadata from `Depot.Schema`; nested preloads (`preload(rows, {posts: [:comments]})`).
- **Property.** `preload(rows, :assoc)` produces the same associations as the manual IN + group-by-FK dance the caller would do by hand.

**Phase 2 exit criteria.**

- Can model a realistic blog/CRM schema end-to-end.
- Migrations apply cleanly against a fresh Postgres.
- Joins, aggregates, bulk writes work and are property-tested.
- Preloads don't N+1 and return fully assembled parent+child structures.
- Lint / typecheck / format clean.

---

## Phase 3 — Advanced patterns & polish

**Goal.** Reach parity with what production Ecto apps reach for once the basics feel good.

### 3.1 `SELECT DISTINCT` / `distinct_on`

- Unit tests only — a SQL modifier. Verify interaction with `order_by` for `DISTINCT ON`.

### 3.2 `BETWEEN` — `where_between(field, lo, hi)`

- **TDD** + **property.** `where_between(q, f, lo, hi)` ≡ `where_gte(q, f, lo) |> where_lte(q, f, hi)` for any `lo ≤ hi`.

### 3.3 Soft deletes

- **TDD.** Schema opt-in (`soft_delete: true` adds `deleted_at`); `Repo.delete` sets the timestamp; `Query.from` auto-filters by default; `with_deleted()` escape hatch.
- **Property.** `delete(x) ; all()` excludes `x`.
- **Property.** `delete(x) ; all_with_deleted()` includes `x`.
- **Property.** `delete(x) ; undelete(x) ; all()` includes `x` (if `undelete` is provided).

### 3.4 Optimistic locking

- **TDD.** `lock_version` field increments on update; concurrent update against a stale version returns `Err(StaleEntry)`.
- **Property.** Two racing `update` calls against the same version — exactly one succeeds, the other returns `StaleEntry`. (Use actor-based concurrency to simulate the race deterministically under `Check.all`.)

### 3.5 Custom types

- **TDD.** A user-defined `Email` type with custom cast/load/dump; enum types mapping March atoms to DB strings; round-trip through insert + fetch.
- **Property.** For any custom type `T` and value `v : T`, `load(dump(v)) == v` (round-trip identity).

### 3.6 Embedded schemas

- **TDD.** Nested changeset validation; JSONB serialization on save; partial updates to embedded fields via `put_embed`.
- **Property.** `apply_changes(cast(base, params))` with embedded changes merges to the same record as manually applying each embedded update in sequence.

### 3.7 `EXPLAIN` — `Repo.explain(query, opts)`

- Unit tests only — pass-through to `EXPLAIN`/`EXPLAIN ANALYZE`. Parse the plan into a structured value so callers can assert on it.

**Phase 3 exit criteria.**

- Depot is a viable Ecto replacement for a Phoenix-sized app.
- No reaching for fragments to work around missing features in common CRUD paths.
- Lint / typecheck / format clean.

---

## Sequencing notes

- **1.3 (OR / condition tree) blocks 1.4, 1.5, 2.6, 2.7.** The condition-tree refactor has to land first in Phase 1 — don't save it for last.
- **1.6 (`RETURNING`) blocks 2.1 (upsert) and 2.2 (bulk insert).** Don't leave it for the end of Phase 1.
- **1.1 (transactions) enables 2.4/2.5 (migration rollback on failure).** Keep this order.
- **1.2 (`where_in`) unblocks a cleaner 2.8 (preload)** — the current `Preload` module hand-rolls its own IN construction; consolidate once `where_in` is public.

## Non-goals

Calling these out so they don't creep in:

- ORM-style lazy loading / change tracking on records. Depot is query-builder-first; parents and children stay explicit.
- Automatic migration generation from schema diffs. Migrations stay hand-written.
- Cross-database abstraction layer. PostgreSQL first; if SQLite/MySQL matter later, they get their own `Connection` module, not a pluggable dialect system.
- DSL sugar that hides the generated SQL. `to_sql(q)` must always render something a human can paste into `psql`.

## Pre-existing issues to track

From `memory/project_depot_status.md`: encode/decode has pre-existing parse errors that block `forge test` on some branches. Confirm they don't block new Phase 1 tests before starting; fix as a pre-Phase-1 task if they do.
