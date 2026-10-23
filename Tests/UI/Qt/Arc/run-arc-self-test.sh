#!/bin/sh
# Runs the Arc sidebar's integration tests (UI/Qt/Arc/ArcSelfTest.cpp) in a real Ladybird window, with a throwaway
# profile. Needs a desktop session (it opens a window). Usage: run-arc-self-test.sh /path/to/Ladybird
set -u
LADYBIRD="$1"
PROFILE="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-arc-self-test.XXXXXX")"
trap 'rm -rf "$PROFILE"' EXIT
ARC_SELF_TEST=1 "$LADYBIRD" --profile-path "$PROFILE" --window-width 1200 --window-height 800 about:blank
