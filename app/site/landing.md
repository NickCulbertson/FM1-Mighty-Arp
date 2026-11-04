# Mighty Arp

Mighty Arp is custom firmware for the M-VAVE FM-1. It's a 4-op FM and subtractive hybrid synth built around a chance
arpeggiator. You choose the notes, the arp decides when they play. Hold a chord, set CHANCE, and let it create
melodies. Plus it has a 32-step sequencer, 64 sounds, chords with strum, swing and humanize, a chorus, a ping-pong
delay and a reverb with shimmer, 16 presets of your own, and MIDI and clock in and out.

![Mighty Arp's HOME page: the arp's recent notes above, then CHANCE, RATE, SPACE and DRIFT](manual/img/home.png "Mighty Arp on the FM-1's screen")

Version {{VERSION}}. Installing is at your own risk; the installer can put M-VAVE's own firmware back.

- [Try it in the browser](emu/): the same code the FM-1 runs, with sound. No FM-1 needed.
- [Install](install/): from Chrome or Edge, with the FM-1 connected by USB.
- [Manual](manual/): playing, the pages, presets, MIDI. One page.
- [Download {{PKG}}](firmware/{{PKG}}): for the command-line installer in
  [fm1-base](https://github.com/charlesvestal/fm1-base), `python3 tools/fm1_install.py {{PKG}}`.
- [Source](https://github.com/NickCulbertson/FM1-Mighty-Arp): MPL-2.0. Built on
  [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal and felucca-bsp, from Felucca by Leo
  Kuroshita, Hügelton Instruments.

M-VAVE and FM-1 are trademarks of their owners. Mighty Arp is not affiliated with M-VAVE.

This project was developed with assistance from Claude Code.
