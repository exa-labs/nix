---
synopsis: "New command `nix derivation source-origins`"
---

The new command [`nix derivation source-origins`](@docroot@/command-ref/new-cli/nix3-derivation-source-origins.md) evaluates the given installables and prints, for every derivation in their build closure, a JSON mapping from each `inputSrcs` store path back to the filesystem path it was copied from during evaluation.
This covers plain path references (`src = ./.`), paths inside local flakes (`path:` and `git+file:` inputs, resolved to the original directory rather than `/nix/store/...-source`), and filtered sources created with `builtins.path`, `builtins.filterSource` or `lib.cleanSourceWith`.
For directory sources, the individual files in the store path are listed as `sourceFiles`, which gives file-level precision even for filtered sources with a broad root.

This is useful for tooling that needs to know which parts of a working tree (e.g. of a monorepo) contribute to a given build.
