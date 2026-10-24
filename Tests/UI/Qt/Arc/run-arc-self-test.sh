#!/bin/sh
# Runs the Arc sidebar's integration tests (UI/Qt/Arc/ArcSelfTest.cpp) in a real Ladybird window, with a throwaway
# profile. Needs a desktop session (it opens a window). Usage: run-arc-self-test.sh /path/to/Ladybird
set -u
LADYBIRD="$1"
HERE="$(cd "$(dirname "$0")" && pwd)"
PROFILE="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-arc-self-test.XXXXXX")"

# Login pages for Fill from 1Password, over http on this computer (the fill skips insecure and data: pages), on
# whichever port is free. A stand-in `op` answers with canned logins.
python3 -c '
import http.server, functools, sys
http.server.SimpleHTTPRequestHandler.log_message = lambda *args: None
handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=sys.argv[1])
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
open(sys.argv[2], "w").write(str(server.server_address[1]))
server.serve_forever()
' "$HERE/fixtures" "$PROFILE/port" &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; rm -rf "$PROFILE"' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$PROFILE/port" ] && break; sleep 0.2; done

# --allow-popups: scenarios open popups from script, with no user click behind them.
ARC_SELF_TEST=1 ARC_SELF_TEST_HTTP_PORT="$(cat "$PROFILE/port" 2>/dev/null)" ARC_OP_PATH="$HERE/fake-op.sh" \
    "$LADYBIRD" --profile-path "$PROFILE/profile" --allow-popups --window-width 1200 --window-height 800 about:blank
