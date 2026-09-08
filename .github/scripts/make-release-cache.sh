#!/usr/bin/env bash
# Export the closure of a store path as a flat, GitHub-release-compatible
# binary cache: <hash>.narinfo + <narhash>.nar.zst + nix-cache-info, all at
# the top level (release asset names cannot contain slashes). Consumers use
# the release download URL as a substituter:
#   extra-substituters = https://github.com/<owner>/<repo>/releases/download/<tag>
#
# When NIX_RELEASE_SIGNING_KEY is set (a `nix key generate-secret` secret
# key), all narinfos are signed with it.
#
# Usage: make-release-cache.sh <store-path-or-result-symlink> <output-dir>
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <store-path-or-result-symlink> <output-dir>" >&2
  exit 2
fi

store_path=$(readlink -f "$1")
out=$2

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

nix copy --to "file://$tmp/cache?compression=zstd" "$store_path"

if [[ -n "${NIX_RELEASE_SIGNING_KEY:-}" ]]; then
  keyfile=$tmp/key
  (umask 077 && printf '%s' "$NIX_RELEASE_SIGNING_KEY" > "$keyfile")
  nix store sign --store "file://$tmp/cache" --all --key-file "$keyfile"
  rm -f "$keyfile"
else
  echo "warning: NIX_RELEASE_SIGNING_KEY not set, cache will be unsigned" >&2
fi

# Flatten nar/ into the top level and rewrite narinfo URLs accordingly.
mkdir -p "$out"
cp "$tmp"/cache/nix-cache-info "$out"/
cp "$tmp"/cache/*.narinfo "$out"/
for nar in "$tmp"/cache/nar/*; do
  cp "$nar" "$out/$(basename "$nar")"
done
sed -i.bak 's|^URL: nar/|URL: |' "$out"/*.narinfo
rm -f "$out"/*.narinfo.bak

# Root store path, so consumers can `nix copy --from <release-url> $(curl
# .../store-paths)` to prefetch the whole closure.
echo "$store_path" > "$out/store-paths"

echo "cache for $store_path written to $out ($(find "$out" -type f | wc -l | tr -d ' ') files)"
