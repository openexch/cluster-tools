#!/bin/bash
# Fetches and VERIFIES the pinned third-party inputs named in dependencies.lock:
# the Aeron 1.53.1 source tarball, sbe-tool 1.40.1 and agrona 2.5.0. Nothing is trusted
# by name or by version string: every file is checked against the lock's sha256 before
# it is used, and a file already in the cache is re-checked, not assumed.
#
# Usage: scripts/fetch-deps.sh <cache-dir> [--it] [--harness]
#        --it also fetches the aeron-all jar the archive integration fixture runs on
#        (archive/it, RC2 S9a 9.3) and prints -DCT_IT_AERON_ALL_JAR=...
#        --harness also provides the RC2 S9h feed harness jar (assets test scope: the reference translator, serve,
#        story, t1-journal, t1-core) and prints -DCT_FEED_HARNESS_JAR=...; it is downloaded from
#        $CT_ARTIFACT_BASE_URL/<feed_harness_url>, or a local build named by CT_FEED_HARNESS_JAR is accepted when its
#        sha256 equals feed_harness_sha256 (the lock pins the jar by hash: one core, one jar, S9h 9.2)
# Prints the cmake arguments to use on stdout, e.g.
#   -DAERON_TARBALL=<cache>/aeron-1.53.1.tar.gz -DSBE_CLASSPATH=<cache>/sbe-tool-1.40.1.jar:<cache>/agrona-2.5.0.jar
# The CMake configure re-hashes every one of them (cmake/VerifiedDependencies.cmake) and
# extracts the Aeron tarball itself: this script only saves the download.
#
# Env: FETCH_OFFLINE=1 refuses to download (verify what is in the cache only).
set -euo pipefail

if [ $# -lt 1 ] || [ $# -gt 3 ]; then
    echo "usage: $0 <cache-dir> [--it] [--harness]" >&2
    exit 64
fi
WANT_IT=0
WANT_HARNESS=0
for flag in "${@:2}"; do
    case "$flag" in
        --it) WANT_IT=1 ;;
        --harness) WANT_HARNESS=1 ;;
        *) echo "usage: $0 <cache-dir> [--it] [--harness]" >&2; exit 64 ;;
    esac
done

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
if [ "$WANT_IT" = "1" ]; then
    AERON_ALL="$CACHE/aeron-all-$(lock aeron_all_version).jar"
    fetch_verified "$(lock aeron_all_url)" "$(lock aeron_all_sha256)" "$AERON_ALL"
    echo "-DCT_IT_AERON_ALL_JAR=$AERON_ALL"
fi
if [ "$WANT_HARNESS" = "1" ]; then
    HARNESS_WANT=$(lock feed_harness_sha256)
    if [ -n "${CT_FEED_HARNESS_JAR:-}" ]; then
        got=$(sha256sum "$CT_FEED_HARNESS_JAR" | cut -d' ' -f1)
        if [ "$got" != "$HARNESS_WANT" ]; then
            echo "FATAL: CT_FEED_HARNESS_JAR=$CT_FEED_HARNESS_JAR sha256 $got, the lock pins $HARNESS_WANT" >&2
            exit 1
        fi
        echo "verified local $(basename "$CT_FEED_HARNESS_JAR") $got" >&2
        echo "-DCT_FEED_HARNESS_JAR=$CT_FEED_HARNESS_JAR"
    else
        if [ -z "${CT_ARTIFACT_BASE_URL:-}" ] && [ ! -f "$CACHE/assets-feed-harness-$HARNESS_WANT.jar" ]; then
            echo "FATAL: --harness needs CT_FEED_HARNESS_JAR (a local build) or CT_ARTIFACT_BASE_URL (the artifact channel)" >&2
            exit 1
        fi
        HARNESS="$CACHE/assets-feed-harness-$HARNESS_WANT.jar"
        fetch_verified "${CT_ARTIFACT_BASE_URL:-}/$(lock feed_harness_url)" "$HARNESS_WANT" "$HARNESS"
        echo "-DCT_FEED_HARNESS_JAR=$HARNESS"
    fi
fi
