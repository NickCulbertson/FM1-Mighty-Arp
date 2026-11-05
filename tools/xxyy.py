#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# One of fm1-base's tools (base/tools/SCRIPT) with Mighty Arp's release identity: id_prefix + XXYY, major and minor
# as two digits each, without fm1-base's patch digits (1.3 -> FM-1_62790103: 8 digits, the Firmware Hub's longest).
# Mighty Arp's releases are X.Y only; X.Y.Z is refused. Everything else is fm1-base's tool, unchanged.
#   tools/xxyy.py build.py app [build.py's options]           (build.sh)
#   tools/xxyy.py make_site.py app PACKAGE VERSION OUT        (tools/make_pages.sh)
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "base" / "tools"
# per tool: fm1-base's line that makes or checks the XXYYZZ identity, and Mighty Arp's for XXYY
REWRITE = {
    "build.py": (
        'PRODUCT = prefix + "%02d%02d%02d" % (int(m[1]), int(m[2]), int(m[3] or 0))',
        'PRODUCT = prefix + "%02d%02d" % (int(m[1]), int(m[2]))\n'
        '        if m[3]:\n'
        '            raise SystemExit(f"--release {a.release}: Mighty Arp\'s releases are X.Y (its identity has no patch digits)")'),
    "make_site.py": (
        're.escape(conf.get("id_prefix", "FM-1_0")) + r"\\d{6}"',
        're.escape(conf.get("id_prefix", "FM-1_0")) + r"\\d{4}"'),
}

if len(sys.argv) < 2 or sys.argv[1] not in REWRITE:
    raise SystemExit(f"usage: tools/xxyy.py {{{','.join(REWRITE)}}} ARGS...")
tool = TOOLS / sys.argv[1]
old, new = REWRITE[sys.argv[1]]
src = tool.read_text()
if src.count(old) != 1:   # fm1-base changed how it makes or checks the identity: look again before running
    raise SystemExit(f"tools/xxyy.py: {tool} no longer has the identity line it rewrites")
sys.argv = [str(tool)] + sys.argv[2:]
sys.path.insert(0, str(TOOLS))
exec(compile(src.replace(old, new), str(tool), "exec"), {"__name__": "__main__", "__file__": str(tool)})
