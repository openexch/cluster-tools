#!/bin/bash
# Fetches and VERIFIES the pinned third-party inputs named in dependencies.lock:
# the Aeron 1.53.1 source tarball, sbe-tool 1.40.1 and agrona 2.5.0. Nothing is trusted
# by name or by version string: every file is checked against the lock's sha256 before
# it is used, and a file already in the cache is re-checked, not assumed.
#
# Usage: scripts/fetch-deps.sh <cache-dir>
# Prints the cmake arguments to use on stdout, e.g.
#   -DAERON_TARBALL=<cache>/aeron-1.53.1.tar.gz -DSBE_CLASSPATH=<cache>/sbe-tool-1.40.1.jar:<cache>/agrona-2.5.0.jar
# The CMake configure re-hashes every one of them (cmake/VerifiedDependencies.cmake) and
# extracts the Aeron tarball itself: this script only saves the download.
#
# Env: FETCH_OFFLINE=1 refuses to download (verify what is in the cache only).
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <cache-dir>" >&2
    exit 64
fi

HERE=$(cd "$(dirname "$0")/.." && pwd)
LOCK="$HERE/dependencies.lock"
CACHE=$(mkdir -p "$1" && cd "$1" && pwd)

lock() {
    python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$LOCK" "$1"
}

fetch_verified() { # url expected-sha256 destination
    local url=$1 want=$2 dest=$3
    if [ ! -f "$dest" ]; then
        if [ "${FETCH_OFFLINE:-0}" = "1" ]; then
            echo "FATAL: $dest missing and FETCH_OFFLINE=1" >&2
            exit 1
        fi
        curl -sfL --retry 3 "$url" -o "$dest.tmp"
        mv "$dest.tmp" "$dest"
    fi
    local got
    got=$(sha256sum "$dest" | cut -d' ' -f1)
    if [ "$got" != "$want" ]; then
        echo "FATAL: sha256 mismatch for $dest" >&2
        echo "  want $want" >&2
        echo "  got  $got" >&2
        exit 1
    fi
    echo "verified $(basename "$dest") $got" >&2
}

AERON_VERSION=$(lock aeron_version)
SBE_VERSION=$(lock sbe_version)
AGRONA_VERSION=$(lock agrona_version)

TARBALL="$CACHE/aeron-$AERON_VERSION.tar.gz"
fetch_verified "$(lock aeron_source_url)" "$(lock aeron_source_sha256)" "$TARBALL"
fetch_verified "$(lock sbe_url)" "$(lock sbe_sha256)" "$CACHE/sbe-tool-$SBE_VERSION.jar"
fetch_verified "$(lock agrona_url)" "$(lock agrona_sha256)" "$CACHE/agrona-$AGRONA_VERSION.jar"

echo "-DAERON_TARBALL=$TARBALL"
echo "-DSBE_CLASSPATH=$CACHE/sbe-tool-$SBE_VERSION.jar:$CACHE/agrona-$AGRONA_VERSION.jar"
