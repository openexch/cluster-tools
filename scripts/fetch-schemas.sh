#!/bin/bash
# Extracts the engine wire schemas at the commits dependencies.lock pins and verifies each
# against the lock's sha256. Never vendored into this repo (two copies of a wire schema
# drift); never taken from a branch tip (a moving ref is how a stale schema gets in).
#
# Usage: scripts/fetch-schemas.sh <out-dir> <assets-checkout> <match-checkout>
# Prints the cmake arguments: -DMONEY_SCHEMA=<out>/money-schema.xml -DMATCH_SCHEMA=<out>/order-schema.xml
# -DME_JOURNAL_SCHEMA=<out>/me-journal-schema.xml
#
# The ME settlement journal (schemaId 4) lives in the match repo and is taken at its own pin,
# me_journal_schema_ref (RC2 S9a contract 2.2; today the same commit as match_schema_ref).
#
# A newer schema is taken by bumping *_ref and *_sha256 in the lock, or by pointing
# -DMONEY_SCHEMA / -DMATCH_SCHEMA at the file directly: the CMake configure check validates their
# structure (schema id, version floor, required templates). The ME journal schema is pinned by exact
# sha256 always, and with -DWITH_BRIDGE=ON the money schema is too (the bridge's exact v7 core wire).
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

extract "$3" "$(lock me_journal_schema_ref)" "$(lock me_journal_schema_path)" "$(lock me_journal_schema_sha256)" "$OUT/me-journal-schema.xml"

echo "-DMONEY_SCHEMA=$OUT/money-schema.xml"
echo "-DMATCH_SCHEMA=$OUT/order-schema.xml"
echo "-DME_JOURNAL_SCHEMA=$OUT/me-journal-schema.xml"
