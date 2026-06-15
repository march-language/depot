# Depot

March data/storage library.

## Project layout

`lib/` source is grouped by domain. `depot.march` (the framework entry point)
stays at the `lib/` root, and `lib/forge/` (CLI tasks referenced by `forge.toml`)
is left untouched.

```
lib/
├── depot.march      # entry point — stays at lib/ root
├── sql/             # query engine: ast, build, compile, cursor, schema
├── mutation/        # write path: mutation(+_build/_compile/_exec)
├── wire/            # postgres wire protocol: message, encode, decode, connection, pool
├── data/            # data shaping: depot_schema, depot_type, depot_embed, depot_gate
├── api/             # public Depot.* API: depot_query, depot_repo, depot_migration,
│                    #   transaction, depot_explain, depot_telemetry, depot_test, preload
└── forge/           # forge CLI tasks (cmd_depot) — referenced by forge.toml
```

> **Module resolution & lib/ subfolders.** Modules are imported by name
> (`import Ast`), and the compiler resolves `import X` to `x.march` by filename.
> `forge` puts `lib/` **and all of its subdirectories** on the module search
> path (`MARCH_LIB_PATH`), so modules can live in any subfolder without changing
> a single `import`. Keep module filenames unique across the whole `lib/` tree —
> two files with the same basename in different folders would collide.

## Build & test

```bash
forge check          # fast typecheck (use after every .march edit)
forge build          # compile
forge lint --strict  # lint
forge test           # run tests
```

**After editing any `.march` file, run `forge check` to typecheck the whole project quickly before proceeding.**

## Searching the codebase

**Use `forge search` to find modules, functions, types, and other code constructs.** This is the primary way to discover what exists in the codebase.

```
forge search "function_name"    # search for a function
forge search "ModuleName"       # search for a module
forge search "type_name"        # search for a type
```

Use `forge search -t "Type -> Type"` for Hoogle-style type search, and `forge search -d "keyword"` to search docstrings.

**`forge search` is always the preferred way to search `.march` files.** Use it instead of Grep/grep whenever the target is March code — names, types, docstrings, or constructors.
