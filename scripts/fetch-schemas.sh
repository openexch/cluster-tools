#!/bin/bash
# Extracts the engine wire schemas at the commits dependencies.lock pins and verifies each
# against the lock's sha256. Never vendored into this repo (two copies of a wire schema
# drift); never taken from a branch tip (a moving ref is how a stale schema gets in).
#
# Usage: scripts/fetch-schemas.sh <out-dir> <assets-checkout> <match-checkout>
# Prints the cmake arguments: -DMONEY_SCHEMA=<out>/money-schema.xml -DMATCH_SCHEMA=<out>/order-schema.xml
#
# A newer schema (money-schema v7, order-schema v12) is taken by bumping *_ref and *_sha256 in
# the lock, or by pointing -DMONEY_SCHEMA / -DMATCH_SCHEMA at the file directly: the CMake
# configure check validates structure (schema id, version floor, required templates), not the hash.
set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <out-dir> <assets-checkout> <match-checkout>" >&2
    exit 64
fi
HERE=$(cd "$(dirname "$0")/.." && pwd)
LOCK="$HERE/dependencies.lock"
OUT=$(mkdir -p "$1" && cd "$1" && pwd)

lock() {
    python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$LOCK" "$1"
}

extract() { # checkout ref path want dest
    local repo=$1 ref=$2 path=$3 want=$4 dest=$5
    git -C "$repo" cat-file -e "$ref^{commit}" 2>/dev/null || {
        echo "FATAL: $repo has no commit $ref (fetch it first)" >&2
        exit 1
    }
    git -C "$repo" show "$ref:$path" > "$dest.tmp"
    local got
    got=$(sha256sum "$dest.tmp" | cut -d' ' -f1)
    if [ "$got" != "$want" ]; then
        echo "FATAL: $repo:$ref:$path sha256 $got, lock pins $want" >&2
        rm -f "$dest.tmp"
        exit 1
    fi
    mv "$dest.tmp" "$dest"
    echo "verified $(basename "$dest") $got ($ref)" >&2
}

extract "$2" "$(lock money_schema_ref)" "$(lock money_schema_path)" "$(lock money_schema_sha256)" "$OUT/money-schema.xml"
extract "$3" "$(lock match_schema_ref)" "$(lock match_schema_path)" "$(lock match_schema_sha256)" "$OUT/order-schema.xml"

echo "-DMONEY_SCHEMA=$OUT/money-schema.xml"
echo "-DMATCH_SCHEMA=$OUT/order-schema.xml"
