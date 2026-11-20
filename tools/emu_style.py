#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Mighty Arp's browser page (app/build/emu/index.html) in its front page's look (app/site/index.html): black, the
cyan and pink title (a link back to the front page), a pink Switch on, and a line under the panel on playing it from
the computer's keyboard. As Mighty Arcade's emulator page. build.sh runs it after ./build.sh --web.
  tools/emu_style.py app/build/emu/index.html"""
import sys

p = sys.argv[1]
s = open(p, encoding="utf-8").read()
if 'id="QG_STYLE"' in s:
    sys.exit(0)

STYLE = """<style id="QG_STYLE">
html:root:root { --ground: #000; --ink: #f1ecff; --muted: #a99bc9; --rule: #32204f; --accent: #ff2e97; --teal: #6ee1ff;
                 --card: #100a1c; --bad: #ff6b8b; --focus: #ffe200; color-scheme: dark; }
body { font-family: "Helvetica Neue", Helvetica, Arial, sans-serif; }
header { align-items: center; padding-bottom: .8rem; border-bottom: 1px solid var(--rule); margin-bottom: 1.2rem; }
h1 { font: 700 2.4rem/1 "Helvetica Neue", Helvetica, Arial, sans-serif; letter-spacing: .04em; text-transform: uppercase; }
h1 a { text-decoration: none; }
h1 .m { color: #6ee1ff; }
h1 .a { color: #ff2e97; }
p.qg-hint { margin: .9rem 0 0; color: var(--ink); }
p.qg-hint kbd { color: #6ee1ff; border-color: var(--rule); background: var(--card); }
.start button { background: #ff2e97; color: #000; font-family: "Helvetica Neue", Helvetica, Arial, sans-serif;
                text-transform: uppercase; letter-spacing: .05em; font-weight: 700; }
.start button:hover, .start button:focus-visible { background: #ffe200; }
.held .btnface, .held .keyface { stroke: #ff2e97; }
.latched .btnface, .latched .keyface { stroke: #6ee1ff; }
.bar button, .speak button { border-color: var(--rule); }
.bar button:hover, .speak button:hover { border-color: #ffe200; }
.speak summary h2 { color: #6ee1ff; text-transform: uppercase; letter-spacing: .06em; font-family: "Helvetica Neue", Helvetica, Arial, sans-serif; font-size: 1rem; }
</style>
"""
HINT = ('<p class="qg-hint">Play it from your computer keyboard: <kbd>Q</kbd> to <kbd>I</kbd> and <kbd>A</kbd> to '
        '<kbd>K</kbd> are the white keys, the number row the black keys. <kbd>P</kbd> plays, <kbd>O</kbd> holds, '
        '<kbd>Z</kbd> and <kbd>X</kbd> shift the octave, <kbd>Esc</kbd> is HOME. If the keys don\'t respond, click the '
        'page first.</p>\n')

for old, new in (("</head>", STYLE + "</head>"),
                 ("<h1>Mighty Arp</h1>", '<h1><a href="../"><span class="m">Mighty</span> <span class="a">Arp</span></a></h1>'),
                 ('<div class="bar">', HINT + '<div class="bar">')):
    if s.count(old) != 1:   # fm1-base's page changed: look again before restyling it
        raise SystemExit(f"tools/emu_style.py: {p} has no single {old!r}")
    s = s.replace(old, new)
open(p, "w", encoding="utf-8").write(s)
print("emu: Mighty Arp's look")
