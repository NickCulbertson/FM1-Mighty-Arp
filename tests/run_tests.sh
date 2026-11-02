#!/bin/sh
# The app in fm1-base's host simulator (app/tests/basic.txt). fm1-base's own tests: base/tests/run_tests.sh.
#   tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
B=app/build
OUT=$B/tests
FAIL=0
mkdir -p "$OUT"
run() {
    name="$1"; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        echo "  ok   $name"
    else
        echo "  FAIL $name (see $OUT/$name.log)"
        tail -15 "$OUT/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}
run host-build sh build.sh --host
run basic $B/host/drift_host app/tests/basic.txt $B/host   # NAME_host: fm1.toml's name in lower case
run release $B/host/drift_host app/tests/release.txt $B/host
exit $FAIL
