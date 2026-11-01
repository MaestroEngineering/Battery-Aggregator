#!/usr/bin/env bash
# Build a release image named from VERSION:  release/bms-agg-<VERSION>.bin
#
#   scripts/release.sh                  normal firmware
#   scripts/release.sh --fail-selftest  TEST image whose self-test always fails
#                                       (release/bms-agg-<VERSION>-selftest-fail.bin)
#
# Remember: bump VERSION by hand before each OTA test build, otherwise the new
# image reports the same version and you cannot tell it was installed.
set -euo pipefail
cd "$(dirname "$0")/.."

ENV=bms-agg
SUFFIX=""
case "${1:-}" in
  "") ;;
  --fail-selftest) ENV=bms-agg-selftest-fail; SUFFIX="-selftest-fail" ;;
  *) echo "usage: $0 [--fail-selftest]" >&2; exit 2 ;;
esac

VER="$(tr -d ' \t\r\n' < VERSION)"
if ! [[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.-]+)?$ ]]; then
  echo "VERSION must look like 0.1.0, got '$VER'" >&2
  exit 1
fi

PIO="${PIO:-$(command -v pio || command -v platformio || echo "$HOME/.platformio/penv/bin/pio")}"
"$PIO" run -e "$ENV"

SRC=".pio/build/$ENV/firmware.bin"
OUT="release/bms-agg-$VER$SUFFIX.bin"
mkdir -p release
cp "$SRC" "$OUT"

# Sanity check: the app descriptor inside the image must carry VERSION.
python3 - "$OUT" "$VER" <<'PY'
import sys
data = open(sys.argv[1], "rb").read(32 + 256)
assert data[0] == 0xE9, "bad image magic"
assert int.from_bytes(data[32:36], "little") == 0xABCD5432, "no app descriptor"
ver = data[48:80].split(b"\0")[0].decode()
proj = data[80:112].split(b"\0")[0].decode()
date = data[128:144].split(b"\0")[0].decode(); tim = data[112:128].split(b"\0")[0].decode()
if ver != sys.argv[2]:
    sys.exit(f"image version {ver!r} != VERSION {sys.argv[2]!r} (stale build?)")
print(f"image: project={proj} version={ver} built={date} {tim}")
PY

( cd release && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256" )
echo "release image: $OUT ($(stat -c %s "$OUT") bytes)"
