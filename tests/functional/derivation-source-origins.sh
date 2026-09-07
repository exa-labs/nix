#!/usr/bin/env bash

source common.sh

TODO_NixOS

clearStoreIfPossible

# Compare two paths after resolving symlinks (e.g. /var -> /private/var on
# macOS).
samePath() {
    [[ "$(realpath "$1")" == "$(realpath "$2")" ]]
}

# Return the store path of the (only) inputSrc of derivation $2 in the
# output $1 whose sourcePath is $3 (after symlink resolution), or fail.
findInputSrc() {
    local output="$1" drvPath="$2" sourcePath="$3"
    local storePath sp
    for storePath in $(jq -r --arg drv "$drvPath" '.[$drv].inputSrcs | keys[]' <<< "$output"); do
        sp=$(jq -r --arg drv "$drvPath" --arg sp "$storePath" '.[$drv].inputSrcs[$sp].sourcePath // empty' <<< "$output")
        if [[ -n "$sp" ]] && samePath "$sp" "$sourcePath"; then
            echo "$storePath"
            return 0
        fi
    done
    fail "no inputSrc of $drvPath has sourcePath $sourcePath"
}

# Assert that the sourceFiles of inputSrc $3 of derivation $2 in output
# $1 contain (or don't contain, if $4 is "!") a file $5.
checkSourceFile() {
    local output="$1" drvPath="$2" storePath="$3" negate="$4" file="$5"
    local f found=false
    for f in $(jq -r --arg drv "$drvPath" --arg sp "$storePath" '.[$drv].inputSrcs[$sp].sourceFiles // [] | .[]' <<< "$output"); do
        if samePath "$f" "$file"; then found=true; fi
    done
    if [[ "$negate" == "!" ]]; then
        [[ $found == false ]] || fail "sourceFiles of $storePath unexpectedly contain $file"
    else
        [[ $found == true ]] || fail "sourceFiles of $storePath don't contain $file"
    fi
}

# A source tree with a couple of files, a subdirectory, a file that
# will be filtered out, and a cyclic symlink (which must not be followed).
writeSources() {
    local dir="$1"
    mkdir -p "$dir/src/sub" "$dir/dep-src"
    echo a > "$dir/src/a.txt"
    echo b > "$dir/src/sub/b.txt"
    echo ignored > "$dir/src/ignored.tmp"
    ln -s . "$dir/src/link"
    echo dep > "$dir/dep-src/dep.txt"
}

writeExpr() {
    local dir="$1"
    cat > "$dir/default.nix" <<EOF
with import ./config.nix;

rec {
  dep = mkDerivation {
    name = "source-origins-dep";
    src = ./dep-src;
    buildCommand = "cp -r \$src \$out";
  };

  top = mkDerivation {
    name = "source-origins-top";
    src = ./src;
    filteredSrc = builtins.path {
      path = ./src;
      name = "filtered-src";
      filter = path: type: baseNameOf path != "ignored.tmp";
    };
    inherit dep;
    buildCommand = "mkdir \$out; cp -r \$src \$filteredSrc \$out/";
  };
}
EOF
}

exprDir=$TEST_ROOT/expr
rm -rf "$exprDir"
mkdir -p "$exprDir"
writeSources "$exprDir"
writeExpr "$exprDir"
cp "${config_nix}" "$exprDir/"

# --- `--file` (non-flake) evaluation ---------------------------------------

topDrv=$(nix-instantiate "$exprDir" -A top)
depDrv=$(nix-instantiate "$exprDir" -A dep)

out=$(nix derivation source-origins -f "$exprDir" top)

# The output is keyed by derivation path and, by default, recursive.
jq -e --arg drv "$topDrv" 'has($drv)' <<< "$out"
jq -e --arg drv "$depDrv" 'has($drv)' <<< "$out"
[[ $(jq -r --arg drv "$topDrv" '.[$drv].drvPath' <<< "$out") == "$topDrv" ]]
[[ $(jq -r --arg drv "$topDrv" '.[$drv].name' <<< "$out") == source-origins-top ]]

# Every inputSrc entry has a storePath equal to its key, and a sourcePath
# key (possibly null).
jq -e 'to_entries | all(.value.inputSrcs | to_entries | all(.key == .value.storePath and (.value | has("sourcePath"))))' <<< "$out"

# `src = ./src` maps back to the original directory and lists its files.
srcStorePath=$(findInputSrc "$out" "$topDrv" "$exprDir/src")
checkSourceFile "$out" "$topDrv" "$srcStorePath" "" "$exprDir/src/a.txt"
checkSourceFile "$out" "$topDrv" "$srcStorePath" "" "$exprDir/src/sub/b.txt"
checkSourceFile "$out" "$topDrv" "$srcStorePath" "" "$exprDir/src/ignored.tmp"
# The symlink is listed but not followed, so there are exactly four entries.
checkSourceFile "$out" "$topDrv" "$srcStorePath" "" "$exprDir/src/link"
[[ $(jq -r --arg drv "$topDrv" --arg sp "$srcStorePath" '.[$drv].inputSrcs[$sp].sourceFiles | length' <<< "$out") == 4 ]]

# `builtins.path { filter = ...; }` maps back to the filtered directory,
# and sourceFiles only lists the files that passed the filter.
filteredStorePath=$(jq -r --arg drv "$topDrv" --arg name "filtered-src" \
    '.[$drv].inputSrcs | keys[] | select(endswith("-" + $name))' <<< "$out")
[[ -n "$filteredStorePath" ]]
[[ "$filteredStorePath" != "$srcStorePath" ]]
samePath "$(jq -r --arg drv "$topDrv" --arg sp "$filteredStorePath" '.[$drv].inputSrcs[$sp].sourcePath' <<< "$out")" "$exprDir/src"
checkSourceFile "$out" "$topDrv" "$filteredStorePath" "" "$exprDir/src/a.txt"
checkSourceFile "$out" "$topDrv" "$filteredStorePath" "" "$exprDir/src/sub/b.txt"
checkSourceFile "$out" "$topDrv" "$filteredStorePath" "!" "$exprDir/src/ignored.tmp"

# The builder script created with `builtins.toFile` was not copied from
# the filesystem, so it has no source path.
jq -e --arg drv "$topDrv" '[.[$drv].inputSrcs[] | select(.sourcePath == null)] | length == 1' <<< "$out"
jq -e --arg drv "$topDrv" '[.[$drv].inputSrcs[] | select(.sourcePath == null) | has("sourceFiles")] | all(. == false)' <<< "$out"

# The transitive dependency's sources are reported too.
findInputSrc "$out" "$depDrv" "$exprDir/dep-src" > /dev/null

# --no-recursive only reports the requested derivation.
out=$(nix derivation source-origins --no-recursive -f "$exprDir" top)
jq -e --arg drv "$topDrv" 'has($drv)' <<< "$out"
jq -e --arg drv "$depDrv" 'has($drv) | not' <<< "$out"
[[ $(jq 'length' <<< "$out") == 1 ]]

# Multiple installables are all reported.
out=$(nix derivation source-origins --no-recursive -f "$exprDir" top dep)
[[ $(jq 'length' <<< "$out") == 2 ]]

# --- `path:` flake ----------------------------------------------------------

flakeDir=$TEST_ROOT/flake
rm -rf "$flakeDir"
mkdir -p "$flakeDir"
writeSources "$flakeDir"
writeExpr "$flakeDir"
cp "${config_nix}" "$flakeDir/"
cat > "$flakeDir/flake.nix" <<EOF
{
  outputs = { self }: {
    packages.$system = import ./default.nix;
  };
}
EOF

out=$(nix derivation source-origins "path:$flakeDir#top")
topDrv=$(jq -r 'to_entries[] | select(.value.name == "source-origins-top") | .key' <<< "$out")
depDrv=$(jq -r 'to_entries[] | select(.value.name == "source-origins-dep") | .key' <<< "$out")
[[ -n "$topDrv" && -n "$depDrv" ]]

# Paths inside the flake resolve to the original flake directory, not to
# the /nix/store/...-source copy.
srcStorePath=$(findInputSrc "$out" "$topDrv" "$flakeDir/src")
checkSourceFile "$out" "$topDrv" "$srcStorePath" "" "$flakeDir/src/a.txt"
checkSourceFile "$out" "$topDrv" "$srcStorePath" "" "$flakeDir/src/sub/b.txt"
findInputSrc "$out" "$depDrv" "$flakeDir/dep-src" > /dev/null

filteredStorePath=$(jq -r --arg drv "$topDrv" --arg name "filtered-src" \
    '.[$drv].inputSrcs | keys[] | select(endswith("-" + $name))' <<< "$out")
[[ -n "$filteredStorePath" ]]
samePath "$(jq -r --arg drv "$topDrv" --arg sp "$filteredStorePath" '.[$drv].inputSrcs[$sp].sourcePath' <<< "$out")" "$flakeDir/src"
checkSourceFile "$out" "$topDrv" "$filteredStorePath" "" "$flakeDir/src/a.txt"
checkSourceFile "$out" "$topDrv" "$filteredStorePath" "!" "$flakeDir/src/ignored.tmp"

# Running again (with the evaluation cache now populated for the flake)
# must give the same result, since the command bypasses the cache.
out2=$(nix derivation source-origins "path:$flakeDir#top")
[[ "$out" == "$out2" ]]

# --- `git+file:` flake ------------------------------------------------------

if [[ $(type -p git) ]]; then
    repoDir=$TEST_ROOT/repo
    createGitRepo "$repoDir"
    mkdir -p "$repoDir/sub"
    writeSources "$repoDir/sub"
    writeExpr "$repoDir/sub"
    cp "${config_nix}" "$repoDir/sub/"
    cp "$flakeDir/flake.nix" "$repoDir/sub/"
    git -C "$repoDir" add .
    git -C "$repoDir" commit -q -m 'Initial'

    out=$(nix derivation source-origins "git+file://$repoDir?dir=sub#top")
    topDrv=$(jq -r 'to_entries[] | select(.value.name == "source-origins-top") | .key' <<< "$out")
    [[ -n "$topDrv" ]]

    # Paths resolve relative to the Git repository root.
    srcStorePath=$(findInputSrc "$out" "$topDrv" "$repoDir/sub/src")
    checkSourceFile "$out" "$topDrv" "$srcStorePath" "" "$repoDir/sub/src/a.txt"
fi
