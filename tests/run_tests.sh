#!/bin/sh
# The app in fm1-base's host simulator (app/tests/*.txt). fm1-base's own tests: base/tests/run_tests.sh.
#   tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
B=app/build
OUT=$B/tests
H=$B/host/mightyarp_host                 # NAME_host: fm1.toml's name in lower case
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
python3 tests/make_sounds_test.py > app/tests/sounds.txt   # every sound, from app.c's list
run host-build sh build.sh --host
run basic $H app/tests/basic.txt $B/host
run release $H app/tests/release.txt $B/host
run recover $H app/tests/recover.txt $B/host
run sync $H app/tests/sync.txt $B/host
run space $H app/tests/space.txt $B/host
run modes $H app/tests/modes.txt $B/host
run overload $H app/tests/overload.txt $B/host
run fixes $H app/tests/fixes.txt $B/host
run fixes2 $H app/tests/fixes2.txt $B/host
run fixes3 $H app/tests/fixes3.txt $B/host
run filter $H app/tests/filter.txt $B/host
run chords $H app/tests/chords.txt $B/host
run presets $H app/tests/presets.txt $B/host
run userpeek $H app/tests/userpeek.txt $B/host
run seq $H app/tests/seq.txt $B/host
run sounds $H app/tests/sounds.txt $B/host
exit $FAIL
