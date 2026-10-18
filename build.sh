#!/bin/sh
# Builds KoboRoot.tgz with the NickelTC toolchain in Docker (works on macOS, incl. Apple Silicon).
set -e
cd "$(dirname "$0")"
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src ghcr.io/pgaskin/nickeltc:1.0 \
    sh -c 'make clean && make && make koboroot'
echo "Done: $PWD/KoboRoot.tgz"
