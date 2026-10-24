#!/bin/sh
# Merges the latest upstream Ladybird into Elytra's branch, then builds and runs the tests.
#
#   Meta/elytra-sync-upstream.sh            fetch, merge, build, test
#   Meta/elytra-sync-upstream.sh --continue after resolving conflicts by hand: commit the merge, build, test
#
# Elytra's branch is published, so upstream is merged in (never rebased onto). Git's rerere is turned on, so a
# conflict resolved once is resolved the same way the next time it comes up. See SYNCING.md.
set -eu

cd "$(dirname "$0")/.."
BUILD_DIR="${BUILD_DIR:-Build/release}"
UPSTREAM="${UPSTREAM:-upstream}"
UPSTREAM_BRANCH="${UPSTREAM_BRANCH:-master}"

say() { printf '\n== %s\n' "$*"; }

# Editors and shell prompts run `git status` now and then, which holds the index lock for a moment: retry then.
git_retrying() {
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        if [ ! -e .git/index.lock ]; then
            git "$@" && return 0
            [ -e .git/index.lock ] || return 1
        fi
        sleep 1
    done
    git "$@"
}

# Remember conflict resolutions, and reuse them.
git config rerere.enabled true
git config rerere.autoUpdate true

# This checkout may be a partial clone that fetches file contents on demand. Upstream refuses objects it no longer
# advertises, which stops a merge partway; the fork has them, so it may serve them too.
if [ "$(git config --get "remote.$UPSTREAM.promisor" || true)" = "true" ]; then
    git config remote.origin.promisor true
fi

if [ "${1:-}" != "--continue" ]; then
    if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
        echo "The working tree has changes; commit or stash them first." >&2
        exit 1
    fi

    say "Fetching $UPSTREAM"
    git fetch "$UPSTREAM"
    behind=$(git rev-list --count "HEAD..$UPSTREAM/$UPSTREAM_BRANCH")
    if [ "$behind" = 0 ]; then
        echo "Already up to date with $UPSTREAM/$UPSTREAM_BRANCH."
        exit 0
    fi
    echo "$behind new upstream commits."

    say "Merging"
    if ! git_retrying merge --no-edit -m "Merge upstream Ladybird into Elytra" "$UPSTREAM/$UPSTREAM_BRANCH"; then
        conflicts=$(git diff --name-only --diff-filter=U)
        if [ -z "$conflicts" ]; then
            echo "Everything conflicting was resolved from earlier merges (rerere); check it builds."
        else
            echo
            echo "Conflicts to resolve by hand:"
            for file in $conflicts; do
                # Fork hooks are listed in SYNCING.md: re-add each in the equivalent spot in upstream's new code.
                if grep -q "\`$file" SYNCING.md 2>/dev/null || grep -q "$(basename "$file")" SYNCING.md 2>/dev/null; then
                    echo "  $file  (has fork hooks: see SYNCING.md)"
                else
                    echo "  $file"
                fi
            done
            echo
            echo "Resolve them, 'git add' each, then run: $0 --continue"
            exit 1
        fi
    fi
fi

if [ -f .git/MERGE_HEAD ]; then
    if [ -n "$(git diff --name-only --diff-filter=U)" ]; then
        echo "Conflicts remain:" >&2
        git diff --name-only --diff-filter=U >&2
        exit 1
    fi
    git_retrying commit --no-edit
fi

say "Building"
ninja -C "$BUILD_DIR"

say "Unit tests"
export QT_QPA_PLATFORM=offscreen
for test in TestArcSpaceStore TestArcWidgets TestArcEdgeCases TestArcImportParser TestArcOnePassword TestArcReleaseInfo; do
    "$BUILD_DIR/bin/$test"
done
LADYBIRD_SOURCE_DIR="$PWD" "$BUILD_DIR/bin/TestArcProfileSessions"
(cd "$BUILD_DIR" && ctest -R "PictureInPicture|UIOverlays" --output-on-failure)
unset QT_QPA_PLATFORM

say "Self-test (opens a test window)"
LADYBIRD_SOURCE_DIR="$PWD" Tests/UI/Qt/Arc/run-arc-self-test.sh "$BUILD_DIR/bin/Ladybird.app/Contents/MacOS/Ladybird"

say "Done: merged, built and tested. Push with: git push origin HEAD"
