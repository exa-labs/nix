#!/usr/bin/env bash
# e2e tests for eval-origins (evalFiles) in nix derivation source-origins.
#
# Creates temporary git repos with various flake patterns and validates
# that evalFiles correctly tracks eval-time file reads.

set -euo pipefail

NIX="${1:-$(dirname "$0")/../result/bin/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "FATAL: nix binary not found at $NIX" >&2
  exit 1
fi

echo "using nix: $NIX"
echo "version: $("$NIX" --version)"

PASS=0
FAIL=0
TESTS=()

assert_eq() {
  local label="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    echo "  PASS  $label"
    PASS=$((PASS + 1))
  else
    echo "  FAIL  $label"
    echo "    expected: $expected"
    echo "    actual:   $actual"
    FAIL=$((FAIL + 1))
  fi
}

assert_contains() {
  local label="$1" needle="$2" haystack="$3"
  if echo "$haystack" | grep -qF "$needle"; then
    echo "  PASS  $label"
    PASS=$((PASS + 1))
  else
    echo "  FAIL  $label"
    echo "    expected to contain: $needle"
    echo "    in: $haystack"
    FAIL=$((FAIL + 1))
  fi
}

assert_not_contains() {
  local label="$1" needle="$2" haystack="$3"
  if ! echo "$haystack" | grep -qF "$needle"; then
    echo "  PASS  $label"
    PASS=$((PASS + 1))
  else
    echo "  FAIL  $label"
    echo "    expected NOT to contain: $needle"
    echo "    in: $haystack"
    FAIL=$((FAIL + 1))
  fi
}

make_repo() {
  local dir
  dir=$(mktemp -d)
  git -C "$dir" init -q
  git -C "$dir" config user.name "test"
  git -C "$dir" config user.email "test@test"
  echo "$dir"
}

commit_all() {
  local repo="$1"
  git -C "$repo" add -A
  git -C "$repo" commit -q -m "commit" --allow-empty
}

run_source_origins() {
  local repo="$1" attr="${2:-packages.x86_64-linux.default}"
  "$NIX" derivation source-origins --extra-experimental-features "nix-command flakes" \
    "git+file://$repo#$attr" 2>/dev/null
}

get_eval_files() {
  local json="$1"
  echo "$json" | jq -r '.evalFiles[]' 2>/dev/null | sort
}

get_eval_files_count() {
  local json="$1"
  echo "$json" | jq '.evalFiles | length' 2>/dev/null
}

get_drv_eval_files() {
  local json="$1" name="$2"
  echo "$json" | jq -r --arg name "$name" '.[] | select(type == "object" and .name == $name) | .evalFiles[]' 2>/dev/null | sort
}

# ============================================================================
# Test 1: import ./config.nix with builtins.path (no self reference)
# config.nix should appear in evalFiles but NOT in inputSrcs
# ============================================================================
echo ""
echo "=== Test 1: import with builtins.path (no self) ==="

repo=$(make_repo)
mkdir -p "$repo/src"
echo '{ name = "test-import"; }' > "$repo/config.nix"
echo '#!/bin/sh' > "$repo/src/main.sh"
cat > "$repo/flake.nix" <<'EOF'
{
  description = "test: import at eval time, builtins.path for build";
  outputs = { self }: let
    config = import ./config.nix;
  in {
    packages.x86_64-linux.default = derivation {
      name = config.name;
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
      src = builtins.path { path = ./src; name = "src"; };
    };
  };
}
EOF
commit_all "$repo"

json=$(run_source_origins "$repo")
eval_files=$(get_eval_files "$json")
assert_contains "config.nix in evalFiles" "$repo/config.nix" "$eval_files"
assert_contains "flake.nix in evalFiles" "$repo/flake.nix" "$eval_files"
assert_not_contains "src/main.sh NOT in evalFiles (it's in inputSrcs)" "$repo/src/main.sh" "$eval_files"
rm -rf "$repo"

# ============================================================================
# Test 2: builtins.readFile
# ============================================================================
echo ""
echo "=== Test 2: builtins.readFile ==="

repo=$(make_repo)
echo "hello world" > "$repo/data.txt"
cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: let
    content = builtins.readFile ./data.txt;
  in {
    packages.x86_64-linux.default = derivation {
      name = "test-readFile-${builtins.substring 0 5 content}";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
    };
  };
}
EOF
commit_all "$repo"

json=$(run_source_origins "$repo")
eval_files=$(get_eval_files "$json")
assert_contains "data.txt in evalFiles (readFile)" "$repo/data.txt" "$eval_files"
rm -rf "$repo"

# ============================================================================
# Test 3: builtins.readDir
# ============================================================================
echo ""
echo "=== Test 3: builtins.readDir ==="

repo=$(make_repo)
mkdir -p "$repo/mydir"
echo "a" > "$repo/mydir/a.txt"
echo "b" > "$repo/mydir/b.txt"
cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: let
    entries = builtins.readDir ./mydir;
  in {
    packages.x86_64-linux.default = derivation {
      name = "test-readDir-${builtins.concatStringsSep "-" (builtins.attrNames entries)}";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
    };
  };
}
EOF
commit_all "$repo"

json=$(run_source_origins "$repo")
eval_files=$(get_eval_files "$json")
assert_contains "mydir in evalFiles (readDir)" "$repo/mydir" "$eval_files"
rm -rf "$repo"

# ============================================================================
# Test 4: builtins.pathExists
# ============================================================================
echo ""
echo "=== Test 4: builtins.pathExists ==="

repo=$(make_repo)
echo "optional" > "$repo/optional.nix"
cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: let
    hasOptional = builtins.pathExists ./optional.nix;
  in {
    packages.x86_64-linux.default = derivation {
      name = if hasOptional then "with-optional" else "without-optional";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
    };
  };
}
EOF
commit_all "$repo"

json=$(run_source_origins "$repo")
eval_files=$(get_eval_files "$json")
assert_contains "optional.nix in evalFiles (pathExists)" "$repo/optional.nix" "$eval_files"
rm -rf "$repo"

# ============================================================================
# Test 5: builtins.hashFile
# ============================================================================
echo ""
echo "=== Test 5: builtins.hashFile ==="

repo=$(make_repo)
echo "hash me" > "$repo/hashable.dat"
cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: let
    h = builtins.hashFile "sha256" ./hashable.dat;
  in {
    packages.x86_64-linux.default = derivation {
      name = "test-hashFile";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
      hash = h;
    };
  };
}
EOF
commit_all "$repo"

json=$(run_source_origins "$repo")
eval_files=$(get_eval_files "$json")
assert_contains "hashable.dat in evalFiles (hashFile)" "$repo/hashable.dat" "$eval_files"
rm -rf "$repo"

# ============================================================================
# Test 6: Transitive eval-time imports (import chain)
# ============================================================================
echo ""
echo "=== Test 6: Transitive imports (A imports B imports C) ==="

repo=$(make_repo)
echo '{ z = import ./c.nix; }' > "$repo/b.nix"
echo '42' > "$repo/c.nix"
cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: let
    b = import ./b.nix;
  in {
    packages.x86_64-linux.default = derivation {
      name = "test-transitive-${toString b.z}";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
    };
  };
}
EOF
commit_all "$repo"

json=$(run_source_origins "$repo")
eval_files=$(get_eval_files "$json")
assert_contains "b.nix in evalFiles (direct import)" "$repo/b.nix" "$eval_files"
assert_contains "c.nix in evalFiles (transitive import)" "$repo/c.nix" "$eval_files"
rm -rf "$repo"

# ============================================================================
# Test 7: No eval-time reads → evalFiles is empty (only inputSrcs)
# ============================================================================
echo ""
echo "=== Test 7: No eval-time reads (evalFiles empty except flake.nix) ==="

repo=$(make_repo)
mkdir -p "$repo/src"
echo "code" > "$repo/src/main.sh"
cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: {
    packages.x86_64-linux.default = derivation {
      name = "test-no-eval-reads";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
      src = builtins.path { path = ./src; name = "src"; };
    };
  };
}
EOF
commit_all "$repo"

json=$(run_source_origins "$repo")
eval_count=$(get_eval_files_count "$json")
eval_files=$(get_eval_files "$json")
# flake.nix itself is always read during eval, so it should appear
assert_contains "flake.nix in evalFiles" "$repo/flake.nix" "$eval_files"
# src/main.sh is an inputSrc (builtins.path), not an eval-time read
assert_not_contains "src/main.sh NOT in evalFiles" "$repo/src/main.sh" "$eval_files"
rm -rf "$repo"

# ============================================================================
# Test 8: Deduplication — same file imported multiple times
# ============================================================================
echo ""
echo "=== Test 8: Deduplication ==="

repo=$(make_repo)
echo '{ x = 1; }' > "$repo/shared.nix"
cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: let
    a = import ./shared.nix;
    b = import ./shared.nix;
    c = import ./shared.nix;
  in {
    packages.x86_64-linux.default = derivation {
      name = "test-dedup-${toString (a.x + b.x + c.x)}";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
    };
  };
}
EOF
commit_all "$repo"

json=$(run_source_origins "$repo")
eval_files=$(get_eval_files "$json")
# Count occurrences of shared.nix — should be exactly 1
count=$(echo "$eval_files" | grep -cF "$repo/shared.nix" || true)
assert_eq "shared.nix appears exactly once (deduplicated)" "1" "$count"
rm -rf "$repo"

# ============================================================================
# Test 9: Mixed — file is both eval-time AND in inputSrcs
# ============================================================================
echo ""
echo "=== Test 9: File in both evalFiles and inputSrcs ==="

repo=$(make_repo)
mkdir -p "$repo/src"
echo '{ name = "dual"; }' > "$repo/config.nix"
echo "code" > "$repo/src/main.sh"
cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: let
    config = import ./config.nix;
  in {
    packages.x86_64-linux.default = derivation {
      name = config.name;
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
      src = "${self}";
    };
  };
}
EOF
commit_all "$repo"

json=$(run_source_origins "$repo")
eval_files=$(get_eval_files "$json")
# config.nix is imported at eval time → should be in evalFiles
assert_contains "config.nix in evalFiles (even with self ref)" "$repo/config.nix" "$eval_files"
rm -rf "$repo"

# ============================================================================
# Test 10: Multiple builtins in one flake
# ============================================================================
echo ""
echo "=== Test 10: Multiple eval builtins combined ==="

repo=$(make_repo)
echo '{ x = 1; }' > "$repo/config.nix"
echo "data content" > "$repo/data.txt"
mkdir -p "$repo/entries"
echo "e1" > "$repo/entries/a.txt"
cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: let
    config = import ./config.nix;
    content = builtins.readFile ./data.txt;
    entries = builtins.readDir ./entries;
    exists = builtins.pathExists ./maybe.nix;
  in {
    packages.x86_64-linux.default = derivation {
      name = "test-multi-${toString config.x}-${builtins.substring 0 4 content}-${builtins.concatStringsSep "-" (builtins.attrNames entries)}-${if exists then "y" else "n"}";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
    };
  };
}
EOF
commit_all "$repo"

json=$(run_source_origins "$repo")
eval_files=$(get_eval_files "$json")
assert_contains "config.nix in evalFiles (import)" "$repo/config.nix" "$eval_files"
assert_contains "data.txt in evalFiles (readFile)" "$repo/data.txt" "$eval_files"
assert_contains "entries in evalFiles (readDir)" "$repo/entries" "$eval_files"
assert_contains "maybe.nix in evalFiles (pathExists)" "$repo/maybe.nix" "$eval_files"
rm -rf "$repo"

# ============================================================================
# Test 11: Nested flake — eval reads scoped to nested dir
# ============================================================================
echo ""
echo "=== Test 11: Nested flake eval reads ==="

repo=$(make_repo)
mkdir -p "$repo/nested/src"
echo '{ name = "nested-pkg"; }' > "$repo/nested/config.nix"
echo "code" > "$repo/nested/src/main.sh"
cat > "$repo/nested/flake.nix" <<'EOF'
{
  description = "nested flake with eval-time import";
  outputs = { self }: let
    config = import ./config.nix;
  in {
    packages.x86_64-linux.default = derivation {
      name = config.name;
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
      src = builtins.path { path = ./src; name = "src"; };
    };
  };
}
EOF
commit_all "$repo"

json=$(run_source_origins "$repo" "packages.x86_64-linux.default" || echo "{}")
# Run against the nested flake dir
json=$("$NIX" derivation source-origins --extra-experimental-features "nix-command flakes" \
  "git+file://$repo?dir=nested#packages.x86_64-linux.default" 2>/dev/null)
eval_files=$(get_eval_files "$json")
assert_contains "nested/config.nix in evalFiles" "$repo/nested/config.nix" "$eval_files"
assert_not_contains "src/main.sh NOT in evalFiles" "$repo/nested/src/main.sh" "$eval_files"
rm -rf "$repo"

# ============================================================================
# Test 12: Per-installable evalFiles attribution
# ============================================================================
echo ""
echo "=== Test 12: Per-installable evalFiles attribution ==="

repo=$(make_repo)
mkdir -p "$repo/a" "$repo/b"
echo "aaa" > "$repo/a/a.txt"
echo "bbb" > "$repo/b/b.txt"
cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: {
    packages.x86_64-linux.a = let
      contents = builtins.readFile ./a/a.txt;
    in derivation {
      name = "pkg-a";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
      marker = contents;
    };

    packages.x86_64-linux.b = let
      contents = builtins.readFile ./b/b.txt;
    in derivation {
      name = "pkg-b";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "mkdir -p $out" ];
      marker = contents;
    };
  };
}
EOF
commit_all "$repo"

json=$("$NIX" derivation source-origins --extra-experimental-features "nix-command flakes" \
  "git+file://$repo#packages.x86_64-linux.a" \
  "git+file://$repo#packages.x86_64-linux.b" 2>/dev/null)
a_eval_files=$(get_drv_eval_files "$json" "pkg-a")
b_eval_files=$(get_drv_eval_files "$json" "pkg-b")
global_eval_files=$(get_eval_files "$json")
assert_contains "global evalFiles include a/a.txt" "$repo/a/a.txt" "$global_eval_files"
assert_contains "global evalFiles include b/b.txt" "$repo/b/b.txt" "$global_eval_files"
assert_contains "pkg-a evalFiles include a/a.txt" "$repo/a/a.txt" "$a_eval_files"
assert_not_contains "pkg-a evalFiles exclude b/b.txt" "$repo/b/b.txt" "$a_eval_files"
assert_contains "pkg-b evalFiles include b/b.txt" "$repo/b/b.txt" "$b_eval_files"
assert_not_contains "pkg-b evalFiles exclude a/a.txt" "$repo/a/a.txt" "$b_eval_files"
rm -rf "$repo"

# ============================================================================
# Summary
# ============================================================================
echo ""
echo "========================================"
echo "  $PASS passed, $FAIL failed out of $((PASS + FAIL)) tests"
echo "========================================"

if [[ $FAIL -gt 0 ]]; then
  exit 1
fi
