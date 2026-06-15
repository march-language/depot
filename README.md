# Depot

A composable ORM toolkit for [March](https://github.com/march-language/march).

Depot covers the full lifecycle of working with relational data: defining
schemas, building queries, validating input, running migrations, executing
against PostgreSQL, and inspecting query plans. It ships its own PostgreSQL
wire-protocol client, so it has no external driver dependency.

```march
let adults =
  Depot.Query.from(user_schema())
  |> Depot.Query.where_gt("age", "18")
  |> Depot.Query.order_asc("name")
  |> Depot.Query.limit(20)
  |> Depot.Query.exec_sql(conn)
```

## Highlights

- **Composable queries** — `Depot.Query` builds a pure value describing a query;
  execution (in-memory or SQL) is a separate step.
- **Changeset-style validation** — `Depot.Gate` casts and validates input and
  collects *all* errors (non-short-circuiting) so users see a complete list.
- **Schema-driven** — define tables, field types, and associations once.
- **Migrations** — generate and run versioned `up`/`down` migrations via the
  `forge depot` CLI.
- **Built-in PostgreSQL client** — connection, SCRAM/MD5 auth, prepared
  statements, and a connection pool, implemented over the wire protocol.
- **Transactions** — `Depot.Transaction` with automatic savepoint nesting.
- **Test-friendly** — `Depot.Repo` is an in-memory store (backed by Vault), so
  unit tests need no database.
- **Introspection** — `Depot.Explain` for `EXPLAIN` / `EXPLAIN ANALYZE`, and
  `Depot.Telemetry` for query events.

## Installation

Depot is a March library. Add it to your project's `forge.toml`:

```toml
[deps]
depot = { git = "https://github.com/march-language/depot.git" }
```

Or pin a local checkout during development:

```toml
[deps]
depot = { path = "../depot" }
```

Then import the modules you need:

```march
import Depot.Schema
import Depot.Query
import Depot.Gate
import Connection
```

## Quick start

### 1. Define a schema

```march
fn user_schema() do
  Depot.Schema.define("users", {
    fields = {
      name  = "String",
      email = "String",
      age   = ("Int", { default = 0 })
    }
  })
end
```

### 2. Validate input with Gate

`Depot.Gate` casts raw params and runs validations, accumulating every error:

```march
fn user_gate(params) do
  Depot.Gate.cast(params, ["name", "email", "age"])
  |> Depot.Gate.validate_required(["name", "email"])
  |> Depot.Gate.validate_format("email", "@")
  |> Depot.Gate.validate_number("age", [NumMin(13)])
end
```

### 3. Persist

Against the in-memory repo (great for tests):

```march
match Depot.Repo.insert(user_schema(), user_gate(params)) do
  Ok(record) -> record
  Err(gate)  -> Depot.Gate.errors(gate)
end
```

### 4. Query

```march
let rows =
  Depot.Query.from(user_schema())
  |> Depot.Query.where_gt("age", "18")
  |> Depot.Query.order_asc("name")
  |> Depot.Query.limit(20)
  |> Depot.Query.exec_sql(conn)
```

The same query value runs in memory via `Depot.Query.execute(q, rows)` — no
database required.

## Connecting to PostgreSQL

`Connection` provides a wire-protocol client (no external driver needed):

```march
import Connection

match connect(default_config("postgres", "my_db")) do
  Err(e)    -> handle(e)
  Ok(conn)  ->
    let _ = simple_query(conn, "CREATE TABLE IF NOT EXISTS users (id INT)")
    let result = exec_prepared(conn, "SELECT id FROM users WHERE id = $1", [ParamText("1")])
    close(conn)
end
```

`default_config(user, database)` defaults to `127.0.0.1:5432` with no password
(set `password` for SCRAM-SHA-256 / MD5 auth). For concurrent workloads, use the
connection `Pool`. Wrap multi-statement work in `Depot.Transaction.run` for
automatic `BEGIN`/`COMMIT` with savepoint nesting.

## Core modules

| Module              | Responsibility                                              |
|---------------------|-------------------------------------------------------------|
| `Depot.Schema`      | Define table schemas — field names, types, associations     |
| `Depot.Query`       | Composable query builder (in-memory `execute` / `exec_sql`) |
| `Depot.Repo`        | In-memory repository for testing (insert/get/all/delete)    |
| `Depot.Gate`        | Changeset-style casting + validation                        |
| `Depot.Migration`   | DDL generation with `up`/`down` versioning                  |
| `Depot.Transaction` | Transactions with automatic savepoint nesting               |
| `Depot.Type`        | Custom type adapters (Email, Enum) for field coercion       |
| `Depot.Embed`       | Embedded schemas — nested records inside a row              |
| `Depot.Explain`     | `EXPLAIN` / `EXPLAIN ANALYZE` query-plan inspection         |
| `Depot.Telemetry`   | Query-event instrumentation                                 |
| `Connection` / `Pool` | PostgreSQL wire-protocol client and connection pool       |

Use `forge search "<name>"` to explore functions, types, and docstrings.

## Migrations & CLI

Depot's `forge depot` tasks manage versioned migrations. Migration files live in
`priv/depot/migrations/` and applied versions are tracked in
`.march/depot/migrations.log`.

| Command                          | Description                                  |
|----------------------------------|----------------------------------------------|
| `forge depot gen.migration <name>` | Scaffold a new timestamped migration file  |
| `forge depot migrate`            | Run all pending migrations (`up`)            |
| `forge depot rollback [n]`       | Roll back the last `n` migrations (default 1) |
| `forge depot migrations`         | List migrations and their applied status     |
| `forge depot reset`              | Roll back every applied migration            |

A generated migration is a module with `up`, `down`, and `version` functions:

```march
-- priv/depot/migrations/<timestamp>_create_users.march
mod Migrations.CreateUsers do

fn up() do
  Depot.Migration.create_table("users", {
    id    = ("UUID", { primary_key = true }),
    name  = ("String", { null = false }),
    email = ("String", { null = false })
  })
end

fn down() do
  Depot.Migration.drop_table("users")
end

fn version() do <timestamp> end

end
```

## Project layout

```
lib/
├── depot.march      # entry point / facade
├── sql/             # query engine: ast, build, compile, cursor, schema
├── mutation/        # write path: mutation(+_build/_compile/_exec)
├── wire/            # postgres wire protocol: message, encode, decode, connection, pool
├── data/            # data shaping: depot_schema, depot_type, depot_embed, depot_gate
├── api/             # public Depot.* API: query, repo, migration, transaction, …
└── forge/           # forge CLI tasks (cmd_depot)
```

## Build & test

```bash
forge check          # fast typecheck
forge build          # compile
forge lint --strict  # lint
forge test           # run the test suite
```

The full suite runs through the interpreter:

```bash
MARCH_TEST_INTERPRETER=1 forge test
```

Live PostgreSQL tests (`test/*_live.march`) require a local server with a
`depot_test` database that trusts local connections; they error at
`Connection.connect` when the fixture is missing.
