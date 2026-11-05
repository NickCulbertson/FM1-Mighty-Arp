#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# fm1-base's build (base/tools/build.py) with Mighty Arp's release identity: id_prefix + XXYY, major and minor as
# two digits each, without fm1-base's patch digits (1.3 -> FM-1_62790103: 8 digits, the Firmware Hub's longest).
# Mighty Arp's releases are X.Y only; X.Y.Z is refused. Everything else is fm1-base's build, unchanged.
#   tools/build.py app [build.py's options]   (build.sh runs it)
import sys
from pathlib import Path

BUILD = Path(__file__).resolve().parents[1] / "base" / "tools" / "build.py"
OLD = 'PRODUCT = prefix + "%02d%02d%02d" % (int(m[1]), int(m[2]), int(m[3] or 0))'
NEW = ('PRODUCT = prefix + "%02d%02d" % (int(m[1]), int(m[2]))\n'
       '        if m[3]:\n'
       '            raise SystemExit(f"--release {a.release}: Mighty Arp\'s releases are X.Y (its identity has no patch digits)")')

src = BUILD.read_text()
if src.count(OLD) != 1:   # fm1-base changed how it makes the identity: look again before building
    raise SystemExit(f"tools/build.py: {BUILD} no longer has the identity line it rewrites")
sys.argv[0] = str(BUILD)
exec(compile(src.replace(OLD, NEW), str(BUILD), "exec"), {"__name__": "__main__", "__file__": str(BUILD)})
