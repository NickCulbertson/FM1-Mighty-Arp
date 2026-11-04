# Mighty Arp: the app

Mighty Arp's own code, on fm1-base's template layout. The repository's [README](../README.md) is the overview,
[docs/MANUAL.md](docs/MANUAL.md) the player's manual, [../SPEC.md](../SPEC.md) the design.

| File | What |
| --- | --- |
| `app.c` | The app: the settings and saves, the 64 sounds, the arp, the sequencer and the clock, MIDI, the screen |
| `app.h` | The app's settings (name, block size, the store's objects) and its half of the contract with fm1-base |
| `dsp/engine.c`, `dsp/engine.h` | The sound: the 4-operator FM voices, the filter, chorus, delay and reverb (built at -O2) |
| `splash.h` | The boot screen, made by `../tools/make_splash.py` |
| `fm1.toml` | Build and site settings: name, package identity, USB names, tagline, source, licence |
| `words.tsv` | The screen reader's words (fm1-reader) |
| `tests/*.txt` | Host simulator checks, run by `../tests/run_tests.sh` (`sounds.txt` is generated from the sound list) |
| `docs/` | The manual and its screenshots |
| `site/landing.md` | The site's front page |

## Build, test, install

From the repository's root:

    ./build.sh --host && tests/run_tests.sh      # the host simulator and every check
    ./build.sh --web                             # the browser emulator: app/build/emu/ (serve it over http)
    ./build.sh                                   # the device package: app/build/mightyarp.fwsc
    python3 base/tools/fm1_install.py app/build/mightyarp.fwsc

## Release and site

A release is a `vX.Y` tag pushed to GitHub: `.github/workflows/release.yml` builds it on the shared FM-1 builder
([`zvenson/fm1-build` at `v1`](https://github.com/zvenson/fm1-build/tree/v1)), runs the tests (a failing one stops
it), signs build provenance for `mightyarp-X.Y.fwsc` and `mightyarp-X.Y-app.bin`, and publishes it. A tag with a
suffix (`v1.0-beta1`) is a pre-release; a published release is never rebuilt. For the site, give the attested
package to `publish_site.sh`:

    gh release download v1.0 --pattern '*.fwsc' --dir app/build
    base/tools/publish_site.sh app 1.0 app/build/mightyarp-1.0.fwsc
