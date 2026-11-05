#!/bin/sh
# Mighty Arp's site (base/tools/make_site.py: the front page, the emulator, the installer, the manual, the screen
# reader's words, the firmware) for release VERSION, from its package, committed to the local gh-pages branch. Push
# that branch yourself (GitHub Desktop: switch to gh-pages, Push origin, back to main); GitHub Pages serves it at
# https://nickculbertson.github.io/FM1-Mighty-Arp/.
#   tools/make_pages.sh VERSION [PACKAGE]
# PACKAGE: the release's own (gh release download vX.Y --pattern '*.fwsc' --dir app/build), else
# app/build/mightyarp-VERSION.fwsc.
set -e
cd "$(dirname "$0")/.."
V="${1:?usage: tools/make_pages.sh VERSION [PACKAGE]}"
PKG="${2:-app/build/mightyarp-$V.fwsc}"
[ -f "$PKG" ] || { echo "make_pages.sh: no package $PKG"; exit 1; }
./build.sh --web --release "$V"
PY="$(python3 -c 'import sys; print(sys.executable)')"
DYLD_FALLBACK_LIBRARY_PATH="${FRIBIDI_LIB:-$HOME/.local/fribidi-arm64/lib}" "$PY" tools/xxyy.py make_site.py app "$PKG" "$V" app/build/site
# the front page: Mighty Arp's own (app/site/index.html), its version and package filled in, over
# the one make_site.py makes from app/site/landing.md (which it needs)
sed -e "s/{{VERSION}}/$V/g" -e "s/{{PKG}}/$(basename "$PKG")/g" app/site/index.html > app/build/site/index.html
# the pages show the name with its space (fm1.toml's has none: it names the build's files)
find app/build/site -name '*.html' -exec sed -i '' 's/MightyArp/Mighty Arp/g' {} +
# with gh-pages checked out (GitHub Desktop, to push it), main's build folders are there too: never part of the site
printf 'app/\nbase/\ntests/\ntools/\n__pycache__/\n.DS_Store\n' > app/build/site/.gitignore
# onto gh-pages without checking it out: the site alone, after the branch's last site (a plain push, not a force)
IDX="$(mktemp)"
rm -f "$IDX"
GIT_INDEX_FILE="$IDX" git --work-tree=app/build/site add -A .
TREE="$(GIT_INDEX_FILE="$IDX" git write-tree)"
rm -f "$IDX"
PARENT="$(git rev-parse -q --verify refs/heads/gh-pages || true)"
COMMIT="$(git commit-tree "$TREE" ${PARENT:+-p "$PARENT"} -m "Site for Mighty Arp $V (from $(git rev-parse --short HEAD))")"
git update-ref refs/heads/gh-pages "$COMMIT"
echo "gh-pages: the site for Mighty Arp $V ($(git rev-parse --short gh-pages)); push it from GitHub Desktop"
