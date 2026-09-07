R""(

# Examples

* Show the source origins for a flake's default package:

  ```console
  # nix derivation source-origins .#default
  {
    "/nix/store/...-my-project.drv": {
      "drvPath": "/nix/store/...-my-project.drv",
      "inputSrcs": {
        "/nix/store/...-source": {
          "sourceFiles": [
            "/home/user/my-project/Cargo.toml",
            "/home/user/my-project/src/main.rs"
          ],
          "sourcePath": "/home/user/my-project",
          "storePath": "/nix/store/...-source"
        }
      },
      "name": "my-project"
    }
  }
  ```

* Show source origins for a flake in a subdirectory of a Git monorepo.
  Using the `git+file://` scheme makes Nix resolve source paths
  relative to the Git repository root rather than the flake directory:

  ```console
  # nix derivation source-origins "git+file:///path/to/monorepo?dir=my/flake"
  ```

* Show only the direct `inputSrcs` of the specified derivation,
  without recursing into its `inputDrvs`:

  ```console
  # nix derivation source-origins --no-recursive .#default
  ```

# Description

This command evaluates the given [*installables*](./nix.md#installables)
and, for each resulting [store derivation], prints a JSON object mapping
every `inputSrcs` store path back to the original filesystem path from
which it was copied into the store during evaluation.

By default the command is *recursive*: it follows all transitive
`inputDrvs` and reports the `inputSrcs` of every derivation in the
build closure. Pass `--no-recursive` to restrict the output to the
direct `inputSrcs` of the specified derivations.

This is useful for build-system tooling that needs to know which
working-tree directories contributed to a derivation's build inputs, for
example to determine which parts of a monorepo are affected by a change.

# Output format

The output is a JSON object keyed by derivation store path. Each value
has the following fields:

* `drvPath`: the store path of the derivation.

* `name`: the name of the derivation.

* `inputSrcs`: a JSON object keyed by `inputSrcs` store path. Each
  value has the following fields:

  * `storePath`: the store path of the input source.

  * `sourcePath`: the filesystem path from which the store path was
    copied during this evaluation, or `null` if the store path was not
    produced by this evaluation (e.g. it was obtained from a
    substituter or produced by a previous evaluation).

    For sources that were filtered (`builtins.path`,
    `builtins.filterSource`, `lib.cleanSourceWith`, ...) this is the
    directory that was filtered, not the individual files that passed
    the filter.

  * `sourceFiles` (only present if `sourcePath` is a directory and the
    store path is available in the store): the individual files inside
    the store path, mapped back to their original locations under
    `sourcePath`. Since the store path *is* the result of any
    filtering, this lists exactly the files that ended up in the input
    source, which gives file-level precision even if `sourcePath` is a
    broad directory such as a repository root. Symlinks are listed but
    not followed.

# Notes

Because the mapping is recorded during evaluation, this command
disables the evaluation cache for the evaluation it performs.

The mapping is populated by the following mechanisms:

* Paths that are coerced to strings during evaluation (e.g. `src = ./.`)
  are recorded when they are copied to the store.

* Flake inputs obtained from the local filesystem (`path:` and
  `git+file:` inputs, including the flake being evaluated) record the
  filesystem root of the source tree when they are mounted, so that
  paths inside them resolve to the original location rather than to
  `/nix/store/...-source`.

* Sources added via `builtins.path` and `builtins.filterSource` are
  recorded when they are added to the store.

[store derivation]: @docroot@/glossary.md#gloss-store-derivation

)""
