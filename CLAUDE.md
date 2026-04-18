# Depot

March data/storage library.

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
