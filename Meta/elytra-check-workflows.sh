#!/bin/sh
# Checks that every script Elytra's own workflows (.github/workflows/arc-*.yml) run from Meta/ still exists: upstream
# renames them now and then, and a release would otherwise fail only after an hour of building.
cd "$(dirname "$0")/.." || exit 1
missing=0
for path in $(grep -ohE 'Meta/[A-Za-z0-9_./-]+\.(py|sh)' .github/workflows/arc-*.yml | sort -u); do
    if [ ! -f "$path" ]; then
        echo "A workflow runs $path, which no longer exists" >&2
        missing=1
    fi
done
exit $missing
