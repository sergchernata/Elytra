#!/bin/sh
# A stand-in for 1Password's `op`, for the self-test: canned logins, no vault. FAKE_OP_MODE=dismissed fails as `op`
# does when its Touch ID prompt is dismissed.
if [ "${FAKE_OP_MODE:-}" = "dismissed" ]; then
    echo "[ERROR] 2026/10/24 10:00:00 authorization prompt dismissed, please try again" >&2
    exit 1
fi
case "$*" in
"item list --categories Login --format json")
    cat <<'JSON'
[{"id":"solo","title":"Localhost","vault":{"name":"Test"},"additional_information":"solo@example.com","urls":[{"href":"http://localhost"}]},
 {"id":"first","title":"First","vault":{"name":"Test"},"additional_information":"first@example.com","urls":[{"href":"http://127.0.0.1"}]},
 {"id":"second","title":"Second","vault":{"name":"Test"},"additional_information":"second@example.com","urls":[{"href":"127.0.0.1/login"}]},
 {"id":"other","title":"Other site","vault":{"name":"Test"},"additional_information":"x","urls":[{"href":"https://example.org"}]}]
JSON
    ;;
"item get solo --reveal --format json")
    echo '{"id":"solo","fields":[{"purpose":"USERNAME","value":"solo@example.com"},{"purpose":"PASSWORD","value":"pa\"ss'"'"'1"}]}' ;;
"item get first --reveal --format json")
    echo '{"id":"first","fields":[{"purpose":"USERNAME","value":"first@example.com"},{"purpose":"PASSWORD","value":"one"}]}' ;;
"item get second --reveal --format json")
    echo '{"id":"second","fields":[{"purpose":"USERNAME","value":"second@example.com"},{"purpose":"PASSWORD","value":"two"}]}' ;;
*)
    echo "[ERROR] 2026/10/24 10:00:00 unexpected arguments: $*" >&2
    exit 1 ;;
esac
