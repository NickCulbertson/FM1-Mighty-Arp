# Mighty Arp licensing

Mighty Arp is free software under the **Mozilla Public License 2.0** (`MPL-2.0`, full text in `LICENSE`), file by
file. If you change one of its files and share the result, share that file's source under the MPL. It is built on
[fm1-base](https://github.com/charlesvestal/fm1-base) and [felucca-bsp](https://github.com/hugelton/felucca-bsp)
(`base/`, a git submodule), both MPL-2.0.

## Where the code comes from

| What | Origin | Licence |
| --- | --- | --- |
| The platform: `base/` (boot, USB and USB audio, the TRS jack, storage, the update path, screen, panel, the screen reader, the host simulator and the browser emulator, build and install tools) | [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal and [felucca-bsp](https://github.com/hugelton/felucca-bsp), from [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita (@kurogedelic), Hügelton Instruments, under MPL-2.0 with his permission ([felucca-bsp#4](https://github.com/hugelton/felucca-bsp/issues/4)) | MPL-2.0 |
| `app/app.c`, `app/app.h`, `app/fm1.toml`, `app/words.tsv`, `.github/workflows/release.yml`, `build.sh`: the layout and the contract with the platform | fm1-base's template, Charles Vestal | MPL-2.0 |
| `app/` (the settings and saves, the sounds, the arp, the sequencer, MIDI, the screen; `dsp/engine.c`, the FM voices, filter and effects), `app/tests/`, `tests/`, `tools/` | Mighty Arp, Nick Culbertson (with Charles Vestal's template code) | MPL-2.0 |
| The chord voicings (LUSH, OPEN, OMNI) | Nick Culbertson's own, from his Macro MIDI Pad (MacroPicoMIDI) | MPL-2.0 |
| The sounds | made by the engine: no samples | MPL-2.0 |
| The idea of a browser emulator for FM-1 firmware (fm1-base's emulator) | [Groove OS](https://www.groove-os.com/) by Peter Gombos, whose site runs its firmware in the browser; no code from it | credit only |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Fonts: Barlow Semi Condensed (The Barlow Project Authors), Terminus (Dimitar Toshkov Zhekov) | SIL OFL 1.1 | `base/assets/fonts/` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

fm1-base's own parts and their licences are in `base/LICENSING.md` and `base/CREDITS.md`.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments. "M-VAVE" and "FM-1" are trademarks of their
respective owners, used here only to say what hardware Mighty Arp runs on. Mighty Arp is independent firmware; it
is not affiliated with, endorsed by or supported by any of them.

## Radio

Mighty Arp never enables the Bluetooth / Wi-Fi radio of the hardware (`ble = false` in `app/fm1.toml`).
