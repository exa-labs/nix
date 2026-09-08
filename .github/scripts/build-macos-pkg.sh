#!/usr/bin/env bash
# Build a macOS installer package (.pkg) that installs this repo's Nix via the
# embedded-closure Rust installer (packaging/rust-installer, i.e.
# NixOS/nix-installer with this revision's Nix closure appended).
#
# The pkg payload places the self-contained `nix-installer` binary under
# /usr/local/lib/exa-nix/, and a postinstall script runs it non-interactively.
# The installer handles all macOS specifics (APFS "Nix Store" volume,
# synthetic.conf /nix mount, _nixbld users, nix-daemon LaunchDaemon) and writes
# /nix/receipt.json; the postinstall is a no-op when a receipt already exists,
# so re-pushing the pkg through MDM is safe.
#
# Usage: build-macos-pkg.sh <nix-installer-binary> <version> <output.pkg>
#
# Optional env:
#   INSTALLER_SIGNING_IDENTITY  "Developer ID Installer: ..." identity to sign
#                               the product archive with (unsigned otherwise).
#   EXTRA_TRUSTED_PUBLIC_KEYS   Space-separated Nix public keys to add to
#                               `extra-trusted-public-keys` in the installed
#                               nix.conf (e.g. the release cache signing key).
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <nix-installer-binary> <version> <output.pkg>" >&2
  exit 2
fi

installer_binary=$1
version=$2
output=$3

identifier="ai.exa.nix"
install_dir="/usr/local/lib/exa-nix"

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

mkdir -p "$workdir/root$install_dir" "$workdir/scripts"
install -m755 "$installer_binary" "$workdir/root$install_dir/nix-installer"

trusted_keys_args=""
if [[ -n "${EXTRA_TRUSTED_PUBLIC_KEYS:-}" ]]; then
  # Nix public keys are <name>:<base64>, so they never contain quotes.
  trusted_keys_args=" \\
  --extra-conf \"extra-trusted-public-keys = $EXTRA_TRUSTED_PUBLIC_KEYS\""
fi

cat > "$workdir/scripts/postinstall" <<EOF
#!/bin/bash
set -euo pipefail

# Already installed (receipt from a previous run of this installer): no-op.
# Nix upgrades and settings changes are managed by nix-darwin afterwards.
if [[ -e /nix/receipt.json ]]; then
  echo "exa-nix: /nix/receipt.json exists, skipping install" >&2
  exit 0
fi

$install_dir/nix-installer install \\
  --no-confirm \\
  --extra-conf "extra-experimental-features = nix-command flakes"$trusted_keys_args
EOF
chmod 755 "$workdir/scripts/postinstall"

pkgbuild \
  --root "$workdir/root" \
  --scripts "$workdir/scripts" \
  --identifier "$identifier" \
  --version "$version" \
  --install-location / \
  "$workdir/component.pkg"

# Wrap the component in a distribution product archive (the format MDMs
# expect), signing it when an identity is provided.
if [[ -n "${INSTALLER_SIGNING_IDENTITY:-}" ]]; then
  productbuild --package "$workdir/component.pkg" \
    --sign "$INSTALLER_SIGNING_IDENTITY" "$output"
else
  productbuild --package "$workdir/component.pkg" "$output"
fi

echo "built $output (version $version)"
