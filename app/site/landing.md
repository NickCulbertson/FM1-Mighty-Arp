# Mighty Arp

An FM and analog-style synth for the M-VAVE FM-1, built around a chance arpeggiator: you choose the notes, it
decides when they play. 67 sounds, chords with strum, swing and humanize, your own presets, MIDI and clock in and out.

![Mighty Arp's HOME page: the arp's recent notes above, then CHANCE, RATE, SPACE and DRIFT](manual/img/home.png "Mighty Arp on the FM-1's screen")

Version {{VERSION}}. Installing is at your own risk; the installer can put M-VAVE's own firmware back.

- [Try it in the browser](emu/): the same code the FM-1 runs, with sound. No FM-1 needed.
- [Install](install/): from Chrome or Edge, with the FM-1 connected by USB.
- [Manual](manual/): playing, the pages, presets, MIDI. One page.
- [Download {{PKG}}](firmware/{{PKG}}): for the command-line installer, `python3 tools/fm1_install.py {{PKG}}`.
- [Source](https://github.com/charlesvestal/fm1-base): MPL-2.0. Built on fm1-base and felucca-bsp, from Felucca by
  Leo Kuroshita, Hügelton Instruments.

M-VAVE and FM-1 are trademarks of their owners. Mighty Arp is not affiliated with M-VAVE.
