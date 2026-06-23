#!/usr/bin/env bash
#
# Refresh the vendored Perfetto amalgamated SDK from an official GitHub release.
#
# As of ~v54 Perfetto no longer checks the amalgamated SDK into the source tree;
# it ships it as a release artifact (perfetto-cpp-sdk-src.zip). This script
# downloads that artifact and drops perfetto.h / perfetto.cc into
# third_party/perfetto/, replacing whatever is vendored there.
#
# Usage:
#   scripts/fetch_perfetto_sdk.sh                 # latest release
#   PERFETTO_VERSION=v54.0 scripts/fetch_perfetto_sdk.sh   # a specific tag
#
# Requires: curl, unzip. No GN/clang toolchain needed (that's only for building
# the SDK from source via tools/gen_amalgamated).
#
set -euo pipefail

ASSET="${PERFETTO_ASSET:-perfetto-cpp-sdk-src.zip}"
REPO="${PERFETTO_REPO:-google/perfetto}"
DEST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/third_party/perfetto"

if [[ -n "${PERFETTO_VERSION:-}" ]]; then
  URL="https://github.com/${REPO}/releases/download/${PERFETTO_VERSION}/${ASSET}"
else
  # GitHub redirects /releases/latest/download/<asset> to the newest release.
  URL="https://github.com/${REPO}/releases/latest/download/${ASSET}"
fi

command -v curl  >/dev/null || { echo "error: curl not found"  >&2; exit 1; }
command -v unzip >/dev/null || { echo "error: unzip not found" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading ${ASSET} (${PERFETTO_VERSION:-latest}) ..."
echo "  $URL"
curl -fSL --retry 3 -o "$tmp/sdk.zip" "$URL"

echo "Extracting ..."
unzip -q "$tmp/sdk.zip" -d "$tmp/sdk"

# The asset layout has varied across releases, so locate the files rather than
# assuming a fixed path.
hdr="$(find "$tmp/sdk" -name perfetto.h  -print -quit)"
src="$(find "$tmp/sdk" -name perfetto.cc -print -quit)"
if [[ -z "$hdr" || -z "$src" ]]; then
  echo "error: perfetto.h / perfetto.cc not found inside ${ASSET}" >&2
  echo "contents were:" >&2; find "$tmp/sdk" -maxdepth 2 -type f | sed 's/^/  /' >&2
  exit 1
fi

mkdir -p "$DEST"
cp "$hdr" "$DEST/perfetto.h"
cp "$src" "$DEST/perfetto.cc"

echo "Updated:"
ls -la "$DEST/perfetto.h" "$DEST/perfetto.cc"
ver="$(grep -m1 -oE 'PERFETTO_VERSION_STRING[^"]*"[^"]+"' "$DEST/perfetto.h" 2>/dev/null || true)"
[[ -n "$ver" ]] && echo "SDK version: $ver"
echo "Done. Rebuild with: cmake --build build -j"
